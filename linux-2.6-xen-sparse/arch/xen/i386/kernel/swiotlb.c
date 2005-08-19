/*
 * Dynamic DMA mapping support.
 *
 * This implementation is a fallback for platforms that do not support
 * I/O TLBs (aka DMA address translation hardware).
 * Copyright (C) 2000 Asit Mallick <Asit.K.Mallick@intel.com>
 * Copyright (C) 2000 Goutham Rao <goutham.rao@intel.com>
 * Copyright (C) 2000, 2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 * Copyright (C) 2005 Keir Fraser <keir@xensource.com>
 */

#include <linux/cache.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/highmem.h>
#include <asm/io.h>
#include <asm/pci.h>
#include <asm/dma.h>

#define OFFSET(val,align) ((unsigned long)((val) & ( (align) - 1)))

#define SG_ENT_PHYS_ADDRESS(sg)	(page_to_phys((sg)->page) + (sg)->offset)

/*
 * Maximum allowable number of contiguous slabs to map,
 * must be a power of 2.  What is the appropriate value ?
 * The complexity of {map,unmap}_single is linearly dependent on this value.
 */
#define IO_TLB_SEGSIZE	128

/*
 * log of the size of each IO TLB slab.  The number of slabs is command line
 * controllable.
 */
#define IO_TLB_SHIFT 11

int swiotlb_force;

/*
 * Used to do a quick range check in swiotlb_unmap_single and
 * swiotlb_sync_single_*, to see if the memory was in fact allocated by this
 * API.
 */
static char *iotlb_virt_start, *iotlb_virt_end;
static dma_addr_t iotlb_bus_start, iotlb_bus_end;

/*
 * The number of IO TLB blocks (in groups of 64) betweeen iotlb_virt_start and
 * iotlb_virt_end.  This is command line adjustable via setup_io_tlb_npages.
 */
static unsigned long iotlb_nslabs;

/*
 * When the IOMMU overflows we return a fallback buffer. This sets the size.
 */
static unsigned long io_tlb_overflow = 32*1024;

void *io_tlb_overflow_buffer;

/*
 * This is a free list describing the number of free entries available from
 * each index
 */
static unsigned int *io_tlb_list;
static unsigned int io_tlb_index;

/*
 * We need to save away the original address corresponding to a mapped entry
 * for the sync operations.
 */
static struct phys_addr {
	struct page *page;
	unsigned int offset;
} *io_tlb_orig_addr;

/*
 * Protect the above data structures in the map and unmap calls
 */
static DEFINE_SPINLOCK(io_tlb_lock);

static int __init
setup_io_tlb_npages(char *str)
{
	/* Unlike ia64, the size is aperture in megabytes, not 'slabs'! */
	if (isdigit(*str)) {
		iotlb_nslabs = simple_strtoul(str, &str, 0) <<
			(20 - IO_TLB_SHIFT);
		iotlb_nslabs = ALIGN(iotlb_nslabs, IO_TLB_SEGSIZE);
	}
	if (*str == ',')
		++str;
	/*
         * NB. 'force' enables the swiotlb, but doesn't force its use for
         * every DMA like it does on native Linux.
         */
	if (!strcmp(str, "force"))
		swiotlb_force = 1;
	return 1;
}
__setup("swiotlb=", setup_io_tlb_npages);
/* make io_tlb_overflow tunable too? */

/*
 * Statically reserve bounce buffer space and initialize bounce buffer data
 * structures for the software IO TLB used to implement the PCI DMA API.
 */
void
swiotlb_init_with_default_size (size_t default_size)
{
	unsigned long i, bytes;

	if (!iotlb_nslabs) {
		iotlb_nslabs = (default_size >> IO_TLB_SHIFT);
		iotlb_nslabs = ALIGN(iotlb_nslabs, IO_TLB_SEGSIZE);
	}

	bytes = iotlb_nslabs * (1UL << IO_TLB_SHIFT);

	/*
	 * Get IO TLB memory from the low pages
	 */
	iotlb_virt_start = alloc_bootmem_low_pages(bytes);
	if (!iotlb_virt_start)
		panic("Cannot allocate SWIOTLB buffer!\n"
		      "Use dom0_mem Xen boot parameter to reserve\n"
		      "some DMA memory (e.g., dom0_mem=-128M).\n");

	for (i = 0; i < iotlb_nslabs; i += IO_TLB_SEGSIZE)
		xen_create_contiguous_region(
			(unsigned long)iotlb_virt_start + (i << IO_TLB_SHIFT),
			get_order(IO_TLB_SEGSIZE << IO_TLB_SHIFT));

	iotlb_virt_end = iotlb_virt_start + bytes;

	/*
	 * Allocate and initialize the free list array.  This array is used
	 * to find contiguous free memory regions of size up to IO_TLB_SEGSIZE
	 * between iotlb_virt_start and iotlb_virt_end.
	 */
	io_tlb_list = alloc_bootmem(iotlb_nslabs * sizeof(int));
	for (i = 0; i < iotlb_nslabs; i++)
 		io_tlb_list[i] = IO_TLB_SEGSIZE - OFFSET(i, IO_TLB_SEGSIZE);
	io_tlb_index = 0;
	io_tlb_orig_addr = alloc_bootmem(
		iotlb_nslabs * sizeof(*io_tlb_orig_addr));

	/*
	 * Get the overflow emergency buffer
	 */
	io_tlb_overflow_buffer = alloc_bootmem_low(io_tlb_overflow);
	iotlb_bus_start = virt_to_bus(iotlb_virt_start);
	iotlb_bus_end   = iotlb_bus_start + bytes;
	printk(KERN_INFO "Software IO TLB enabled: \n"
	       " Aperture:     %lu megabytes\n"
	       " Bus range:    0x%016lx - 0x%016lx\n"
	       " Kernel range: 0x%016lx - 0x%016lx\n",
	       bytes >> 20,
	       (unsigned long)iotlb_bus_start, (unsigned long)iotlb_bus_end,
	       (unsigned long)iotlb_virt_start, (unsigned long)iotlb_virt_end);
}

void
swiotlb_init(void)
{
	/* The user can forcibly enable swiotlb. */
	if (swiotlb_force)
		swiotlb = 1;

	/*
         * Otherwise, enable for domain 0 if the machine has 'lots of memory',
         * which we take to mean more than 2GB.
         */
	if (xen_start_info.flags & SIF_INITDOMAIN) {
		dom0_op_t op;
		op.cmd = DOM0_PHYSINFO;
		if ((HYPERVISOR_dom0_op(&op) == 0) &&
		    (op.u.physinfo.total_pages > 0x7ffff))
			swiotlb = 1;
	}

	if (swiotlb)
		swiotlb_init_with_default_size(64 * (1<<20));
}

static void
__sync_single(struct phys_addr buffer, char *dma_addr, size_t size, int dir)
{
	if (PageHighMem(buffer.page)) {
		size_t len, bytes;
		char *dev, *host, *kmp;
		len = size;
		while (len != 0) {
			if (((bytes = len) + buffer.offset) > PAGE_SIZE)
				bytes = PAGE_SIZE - buffer.offset;
			kmp  = kmap_atomic(buffer.page, KM_SWIOTLB);
			dev  = dma_addr + size - len;
			host = kmp + buffer.offset;
			memcpy((dir == DMA_FROM_DEVICE) ? host : dev,
			       (dir == DMA_FROM_DEVICE) ? dev : host,
			       bytes);
			kunmap_atomic(kmp, KM_SWIOTLB);
			len -= bytes;
			buffer.page++;
			buffer.offset = 0;
		}
	} else {
		char *host = (char *)phys_to_virt(
			page_to_pseudophys(buffer.page)) + buffer.offset;
		if (dir == DMA_FROM_DEVICE)
			memcpy(host, dma_addr, size);
		else if (dir == DMA_TO_DEVICE)
			memcpy(dma_addr, host, size);
	}
}

/*
 * Allocates bounce buffer and returns its kernel virtual address.
 */
static void *
map_single(struct device *hwdev, struct phys_addr buffer, size_t size, int dir)
{
	unsigned long flags;
	char *dma_addr;
	unsigned int nslots, stride, index, wrap;
	int i;

	/*
	 * For mappings greater than a page, we limit the stride (and
	 * hence alignment) to a page size.
	 */
	nslots = ALIGN(size, 1 << IO_TLB_SHIFT) >> IO_TLB_SHIFT;
	if (size > PAGE_SIZE)
		stride = (1 << (PAGE_SHIFT - IO_TLB_SHIFT));
	else
		stride = 1;

	BUG_ON(!nslots);

	/*
	 * Find suitable number of IO TLB entries size that will fit this
	 * request and allocate a buffer from that IO TLB pool.
	 */
	spin_lock_irqsave(&io_tlb_lock, flags);
	{
		wrap = index = ALIGN(io_tlb_index, stride);

		if (index >= iotlb_nslabs)
			wrap = index = 0;

		do {
			/*
			 * If we find a slot that indicates we have 'nslots'
			 * number of contiguous buffers, we allocate the
			 * buffers from that slot and mark the entries as '0'
			 * indicating unavailable.
			 */
			if (io_tlb_list[index] >= nslots) {
				int count = 0;

				for (i = index; i < (int)(index + nslots); i++)
					io_tlb_list[i] = 0;
				for (i = index - 1;
				     (OFFSET(i, IO_TLB_SEGSIZE) !=
				      IO_TLB_SEGSIZE -1) && io_tlb_list[i];
				     i--)
					io_tlb_list[i] = ++count;
				dma_addr = iotlb_virt_start +
					(index << IO_TLB_SHIFT);

				/*
				 * Update the indices to avoid searching in
				 * the next round.
				 */
				io_tlb_index = 
					((index + nslots) < iotlb_nslabs
					 ? (index + nslots) : 0);

				goto found;
			}
			index += stride;
			if (index >= iotlb_nslabs)
				index = 0;
		} while (index != wrap);

		spin_unlock_irqrestore(&io_tlb_lock, flags);
		return NULL;
	}
  found:
	spin_unlock_irqrestore(&io_tlb_lock, flags);

	/*
	 * Save away the mapping from the original address to the DMA address.
	 * This is needed when we sync the memory.  Then we sync the buffer if
	 * needed.
	 */
	io_tlb_orig_addr[index] = buffer;
	if ((dir == DMA_TO_DEVICE) || (dir == DMA_BIDIRECTIONAL))
		__sync_single(buffer, dma_addr, size, DMA_TO_DEVICE);

	return dma_addr;
}

/*
 * dma_addr is the kernel virtual address of the bounce buffer to unmap.
 */
static void
unmap_single(struct device *hwdev, char *dma_addr, size_t size, int dir)
{
	unsigned long flags;
	int i, count, nslots = ALIGN(size, 1 << IO_TLB_SHIFT) >> IO_TLB_SHIFT;
	int index = (dma_addr - iotlb_virt_start) >> IO_TLB_SHIFT;
	struct phys_addr buffer = io_tlb_orig_addr[index];

	/*
	 * First, sync the memory before unmapping the entry
	 */
	if ((dir == DMA_FROM_DEVICE) || (dir == DMA_BIDIRECTIONAL))
		__sync_single(buffer, dma_addr, size, DMA_FROM_DEVICE);

	/*
	 * Return the buffer to the free list by setting the corresponding
	 * entries to indicate the number of contigous entries available.
	 * While returning the entries to the free list, we merge the entries
	 * with slots below and above the pool being returned.
	 */
	spin_lock_irqsave(&io_tlb_lock, flags);
	{
		count = ((index + nslots) < ALIGN(index + 1, IO_TLB_SEGSIZE) ?
			 io_tlb_list[index + nslots] : 0);
		/*
		 * Step 1: return the slots to the free list, merging the
		 * slots with superceeding slots
		 */
		for (i = index + nslots - 1; i >= index; i--)
			io_tlb_list[i] = ++count;
		/*
		 * Step 2: merge the returned slots with the preceding slots,
		 * if available (non zero)
		 */
		for (i = index - 1;
		     (OFFSET(i, IO_TLB_SEGSIZE) !=
		      IO_TLB_SEGSIZE -1) && io_tlb_list[i];
		     i--)
			io_tlb_list[i] = ++count;
	}
	spin_unlock_irqrestore(&io_tlb_lock, flags);
}

static void
sync_single(struct device *hwdev, char *dma_addr, size_t size, int dir)
{
	int index = (dma_addr - iotlb_virt_start) >> IO_TLB_SHIFT;
	struct phys_addr buffer = io_tlb_orig_addr[index];
	BUG_ON((dir != DMA_FROM_DEVICE) && (dir != DMA_TO_DEVICE));
	__sync_single(buffer, dma_addr, size, dir);
}

static void
swiotlb_full(struct device *dev, size_t size, int dir, int do_panic)
{
	/*
	 * Ran out of IOMMU space for this operation. This is very bad.
	 * Unfortunately the drivers cannot handle this operation properly.
	 * unless they check for pci_dma_mapping_error (most don't)
	 * When the mapping is small enough return a static buffer to limit
	 * the damage, or panic when the transfer is too big.
	 */
	printk(KERN_ERR "PCI-DMA: Out of SW-IOMMU space for %lu bytes at "
	       "device %s\n", (unsigned long)size, dev ? dev->bus_id : "?");

	if (size > io_tlb_overflow && do_panic) {
		if (dir == PCI_DMA_FROMDEVICE || dir == PCI_DMA_BIDIRECTIONAL)
			panic("PCI-DMA: Memory would be corrupted\n");
		if (dir == PCI_DMA_TODEVICE || dir == PCI_DMA_BIDIRECTIONAL)
			panic("PCI-DMA: Random memory would be DMAed\n");
	}
}

/*
 * Map a single buffer of the indicated size for DMA in streaming mode.  The
 * PCI address to use is returned.
 *
 * Once the device is given the dma address, the device owns this memory until
 * either swiotlb_unmap_single or swiotlb_dma_sync_single is performed.
 */
dma_addr_t
swiotlb_map_single(struct device *hwdev, void *ptr, size_t size, int dir)
{
	dma_addr_t dev_addr = virt_to_bus(ptr);
	void *map;
	struct phys_addr buffer;

	BUG_ON(dir == DMA_NONE);

	/*
	 * If the pointer passed in happens to be in the device's DMA window,
	 * we can safely return the device addr and not worry about bounce
	 * buffering it.
	 */
	if (!range_straddles_page_boundary(ptr, size) &&
	    !address_needs_mapping(hwdev, dev_addr))
		return dev_addr;

	/*
	 * Oh well, have to allocate and map a bounce buffer.
	 */
	buffer.page   = virt_to_page(ptr);
	buffer.offset = (unsigned long)ptr & ~PAGE_MASK;
	map = map_single(hwdev, buffer, size, dir);
	if (!map) {
		swiotlb_full(hwdev, size, dir, 1);
		map = io_tlb_overflow_buffer;
	}

	dev_addr = virt_to_bus(map);

	/*
	 * Ensure that the address returned is DMA'ble
	 */
	if (address_needs_mapping(hwdev, dev_addr))
		panic("map_single: bounce buffer is not DMA'ble");

	return dev_addr;
}

/*
 * Unmap a single streaming mode DMA translation.  The dma_addr and size must
 * match what was provided for in a previous swiotlb_map_single call.  All
 * other usages are undefined.
 *
 * After this call, reads by the cpu to the buffer are guaranteed to see
 * whatever the device wrote there.
 */
void
swiotlb_unmap_single(struct device *hwdev, dma_addr_t dev_addr, size_t size,
		     int dir)
{
	BUG_ON(dir == DMA_NONE);
	if ((dev_addr >= iotlb_bus_start) && (dev_addr < iotlb_bus_end))
		unmap_single(hwdev, bus_to_virt(dev_addr), size, dir);
}

/*
 * Make physical memory consistent for a single streaming mode DMA translation
 * after a transfer.
 *
 * If you perform a swiotlb_map_single() but wish to interrogate the buffer
 * using the cpu, yet do not wish to teardown the PCI dma mapping, you must
 * call this function before doing so.  At the next point you give the PCI dma
 * address back to the card, you must first perform a
 * swiotlb_dma_sync_for_device, and then the device again owns the buffer
 */
void
swiotlb_sync_single_for_cpu(struct device *hwdev, dma_addr_t dev_addr,
			    size_t size, int dir)
{
	BUG_ON(dir == DMA_NONE);
	if ((dev_addr >= iotlb_bus_start) && (dev_addr < iotlb_bus_end))
		sync_single(hwdev, bus_to_virt(dev_addr), size, dir);
}

void
swiotlb_sync_single_for_device(struct device *hwdev, dma_addr_t dev_addr,
			       size_t size, int dir)
{
	BUG_ON(dir == DMA_NONE);
	if ((dev_addr >= iotlb_bus_start) && (dev_addr < iotlb_bus_end))
		sync_single(hwdev, bus_to_virt(dev_addr), size, dir);
}

/*
 * Map a set of buffers described by scatterlist in streaming mode for DMA.
 * This is the scatter-gather version of the above swiotlb_map_single
 * interface.  Here the scatter gather list elements are each tagged with the
 * appropriate dma address and length.  They are obtained via
 * sg_dma_{address,length}(SG).
 *
 * NOTE: An implementation may be able to use a smaller number of
 *       DMA address/length pairs than there are SG table elements.
 *       (for example via virtual mapping capabilities)
 *       The routine returns the number of addr/length pairs actually
 *       used, at most nents.
 *
 * Device ownership issues as mentioned above for swiotlb_map_single are the
 * same here.
 */
int
swiotlb_map_sg(struct device *hwdev, struct scatterlist *sg, int nelems,
	       int dir)
{
	struct phys_addr buffer;
	dma_addr_t dev_addr;
	char *map;
	int i;

	BUG_ON(dir == DMA_NONE);

	for (i = 0; i < nelems; i++, sg++) {
		dev_addr = SG_ENT_PHYS_ADDRESS(sg);
		if (address_needs_mapping(hwdev, dev_addr)) {
			buffer.page   = sg->page;
			buffer.offset = sg->offset;
			map = map_single(hwdev, buffer, sg->length, dir);
			if (!map) {
				/* Don't panic here, we expect map_sg users
				   to do proper error handling. */
				swiotlb_full(hwdev, sg->length, dir, 0);
				swiotlb_unmap_sg(hwdev, sg - i, i, dir);
				sg[0].dma_length = 0;
				return 0;
			}
			sg->dma_address = (dma_addr_t)virt_to_bus(map);
		} else
			sg->dma_address = dev_addr;
		sg->dma_length = sg->length;
	}
	return nelems;
}

/*
 * Unmap a set of streaming mode DMA translations.  Again, cpu read rules
 * concerning calls here are the same as for swiotlb_unmap_single() above.
 */
void
swiotlb_unmap_sg(struct device *hwdev, struct scatterlist *sg, int nelems,
		 int dir)
{
	int i;

	BUG_ON(dir == DMA_NONE);

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			unmap_single(hwdev, 
				     (void *)bus_to_virt(sg->dma_address),
				     sg->dma_length, dir);
}

/*
 * Make physical memory consistent for a set of streaming mode DMA translations
 * after a transfer.
 *
 * The same as swiotlb_sync_single_* but for a scatter-gather list, same rules
 * and usage.
 */
void
swiotlb_sync_sg_for_cpu(struct device *hwdev, struct scatterlist *sg,
			int nelems, int dir)
{
	int i;

	BUG_ON(dir == DMA_NONE);

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			sync_single(hwdev,
				    (void *)bus_to_virt(sg->dma_address),
				    sg->dma_length, dir);
}

void
swiotlb_sync_sg_for_device(struct device *hwdev, struct scatterlist *sg,
			   int nelems, int dir)
{
	int i;

	BUG_ON(dir == DMA_NONE);

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			sync_single(hwdev,
				    (void *)bus_to_virt(sg->dma_address),
				    sg->dma_length, dir);
}

dma_addr_t
swiotlb_map_page(struct device *hwdev, struct page *page,
		 unsigned long offset, size_t size,
		 enum dma_data_direction direction)
{
	struct phys_addr buffer;
	dma_addr_t dev_addr;
	char *map;

	dev_addr = page_to_phys(page) + offset;
	if (address_needs_mapping(hwdev, dev_addr)) {
		buffer.page   = page;
		buffer.offset = offset;
		map = map_single(hwdev, buffer, size, direction);
		if (!map) {
			swiotlb_full(hwdev, size, direction, 1);
			map = io_tlb_overflow_buffer;
		}
		dev_addr = (dma_addr_t)virt_to_bus(map);
	}

	return dev_addr;
}

void
swiotlb_unmap_page(struct device *hwdev, dma_addr_t dma_address,
		   size_t size, enum dma_data_direction direction)
{
	BUG_ON(direction == DMA_NONE);
	if ((dma_address >= iotlb_bus_start) && (dma_address < iotlb_bus_end))
		unmap_single(hwdev, bus_to_virt(dma_address), size, direction);
}

int
swiotlb_dma_mapping_error(dma_addr_t dma_addr)
{
	return (dma_addr == virt_to_bus(io_tlb_overflow_buffer));
}

/*
 * Return whether the given PCI device DMA address mask can be supported
 * properly.  For example, if your device can only drive the low 24-bits
 * during PCI bus mastering, then you would pass 0x00ffffff as the mask to
 * this function.
 */
int
swiotlb_dma_supported (struct device *hwdev, u64 mask)
{
	return (mask >= 0xffffffffUL);
}

EXPORT_SYMBOL(swiotlb_init);
EXPORT_SYMBOL(swiotlb_map_single);
EXPORT_SYMBOL(swiotlb_unmap_single);
EXPORT_SYMBOL(swiotlb_map_sg);
EXPORT_SYMBOL(swiotlb_unmap_sg);
EXPORT_SYMBOL(swiotlb_sync_single_for_cpu);
EXPORT_SYMBOL(swiotlb_sync_single_for_device);
EXPORT_SYMBOL(swiotlb_sync_sg_for_cpu);
EXPORT_SYMBOL(swiotlb_sync_sg_for_device);
EXPORT_SYMBOL(swiotlb_map_page);
EXPORT_SYMBOL(swiotlb_unmap_page);
EXPORT_SYMBOL(swiotlb_dma_mapping_error);
EXPORT_SYMBOL(swiotlb_dma_supported);

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
