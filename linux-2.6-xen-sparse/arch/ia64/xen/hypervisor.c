/******************************************************************************
 * include/asm-ia64/shadow.h
 *
 * Copyright (c) 2006 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

//#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/bootmem.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <asm/hypervisor.h>
#include <asm/hypercall.h>
#include <xen/interface/memory.h>
#include <xen/balloon.h>

//XXX xen/ia64 copy_from_guest() is broken.
//    This is a temporal work around until it is fixed.
//    used by balloon.c netfront.c

// get_xen_guest_handle is defined only when __XEN_TOOLS__ is defined
// if the definition in arch-ia64.h is changed, this must be updated.
#define get_xen_guest_handle(val, hnd)  do { val = (hnd).p; } while (0)

int
ia64_xenmem_reservation_op(unsigned long op,
			   struct xen_memory_reservation* reservation__)
{
	struct xen_memory_reservation reservation = *reservation__;
	unsigned long* frame_list;
	unsigned long nr_extents = reservation__->nr_extents;
	int ret = 0;
	get_xen_guest_handle(frame_list, reservation__->extent_start);

	BUG_ON(op != XENMEM_increase_reservation &&
	       op != XENMEM_decrease_reservation &&
	       op != XENMEM_populate_physmap);

	while (nr_extents > 0) {
		int tmp_ret;
		volatile unsigned long dummy;

		set_xen_guest_handle(reservation.extent_start, frame_list);
		reservation.nr_extents = nr_extents;

		dummy = frame_list[0];// re-install tlb entry before hypercall
		tmp_ret = ____HYPERVISOR_memory_op(op, &reservation);
		if (tmp_ret < 0) {
			if (ret == 0) {
				ret = tmp_ret;
			}
			break;
		}
		frame_list += tmp_ret;
		nr_extents -= tmp_ret;
		ret += tmp_ret;
	}
	return ret;
}

//XXX same as i386, x86_64 contiguous_bitmap_set(), contiguous_bitmap_clear()
// move those to lib/contiguous_bitmap?
//XXX discontigmem/sparsemem

/*
 * Bitmap is indexed by page number. If bit is set, the page is part of a
 * xen_create_contiguous_region() area of memory.
 */
unsigned long *contiguous_bitmap;

void
contiguous_bitmap_init(unsigned long end_pfn)
{
	unsigned long size = (end_pfn + 2 * BITS_PER_LONG) >> 3;
	contiguous_bitmap = alloc_bootmem_low_pages(size);
	BUG_ON(!contiguous_bitmap);
	memset(contiguous_bitmap, 0, size);
}

#if 0
int
contiguous_bitmap_test(void* p)
{
	return test_bit(__pa(p) >> PAGE_SHIFT, contiguous_bitmap);
}
#endif

static void contiguous_bitmap_set(
	unsigned long first_page, unsigned long nr_pages)
{
	unsigned long start_off, end_off, curr_idx, end_idx;

	curr_idx  = first_page / BITS_PER_LONG;
	start_off = first_page & (BITS_PER_LONG-1);
	end_idx   = (first_page + nr_pages) / BITS_PER_LONG;
	end_off   = (first_page + nr_pages) & (BITS_PER_LONG-1);

	if (curr_idx == end_idx) {
		contiguous_bitmap[curr_idx] |=
			((1UL<<end_off)-1) & -(1UL<<start_off);
	} else {
		contiguous_bitmap[curr_idx] |= -(1UL<<start_off);
		while ( ++curr_idx < end_idx )
			contiguous_bitmap[curr_idx] = ~0UL;
		contiguous_bitmap[curr_idx] |= (1UL<<end_off)-1;
	}
}

static void contiguous_bitmap_clear(
	unsigned long first_page, unsigned long nr_pages)
{
	unsigned long start_off, end_off, curr_idx, end_idx;

	curr_idx  = first_page / BITS_PER_LONG;
	start_off = first_page & (BITS_PER_LONG-1);
	end_idx   = (first_page + nr_pages) / BITS_PER_LONG;
	end_off   = (first_page + nr_pages) & (BITS_PER_LONG-1);

	if (curr_idx == end_idx) {
		contiguous_bitmap[curr_idx] &=
			-(1UL<<end_off) | ((1UL<<start_off)-1);
	} else {
		contiguous_bitmap[curr_idx] &= (1UL<<start_off)-1;
		while ( ++curr_idx != end_idx )
			contiguous_bitmap[curr_idx] = 0;
		contiguous_bitmap[curr_idx] &= -(1UL<<end_off);
	}
}

static unsigned long
HYPERVISOR_populate_physmap(unsigned long gpfn, unsigned int extent_order,
			    unsigned int address_bits)
{
	unsigned long ret;
        struct xen_memory_reservation reservation = {
		.nr_extents   = 1,
                .address_bits = address_bits,
                .extent_order = extent_order,
                .domid        = DOMID_SELF
        };
	set_xen_guest_handle(reservation.extent_start, &gpfn);
	ret = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	BUG_ON(ret != 1);
	return 0;
}

static unsigned long
HYPERVISOR_remove_physmap(unsigned long gpfn, unsigned int extent_order)
{
	unsigned long ret;
	struct xen_memory_reservation reservation = {
		.nr_extents   = 1,
		.address_bits = 0,
		.extent_order = extent_order,
		.domid        = DOMID_SELF
	};
	set_xen_guest_handle(reservation.extent_start, &gpfn);
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
	BUG_ON(ret != 1);
	return 0;
}

/* Ensure multi-page extents are contiguous in machine memory. */
int
__xen_create_contiguous_region(unsigned long vstart,
			       unsigned int order, unsigned int address_bits)
{
	unsigned long error = 0;
	unsigned long gphys = __pa(vstart);
	unsigned long start_gpfn = gphys >> PAGE_SHIFT;
	unsigned long num_gpfn = 1 << order;
	unsigned long i;
	unsigned long flags;

	scrub_pages(vstart, num_gpfn);

	balloon_lock(flags);

	error = HYPERVISOR_remove_physmap(start_gpfn, order);
	if (error) {
		goto fail;
	}

	error = HYPERVISOR_populate_physmap(start_gpfn, order, address_bits);
	if (error) {
		goto fail;
	}
	contiguous_bitmap_set(start_gpfn, num_gpfn);
#if 0
	{
	unsigned long mfn;
	unsigned long mfn_prev = ~0UL;
	for (i = 0; i < num_gpfn; i++) {
		mfn = pfn_to_mfn_for_dma(start_gpfn + i);
		if (mfn_prev != ~0UL && mfn != mfn_prev + 1) {
			xprintk("\n");
			xprintk("%s:%d order %d "
				"start 0x%lx bus 0x%lx machine 0x%lx\n",
				__func__, __LINE__, order,
				vstart, virt_to_bus((void*)vstart),
				phys_to_machine_for_dma(gphys));
			xprintk("mfn: ");
			for (i = 0; i < num_gpfn; i++) {
				mfn = pfn_to_mfn_for_dma(start_gpfn + i);
				xprintk("0x%lx ", mfn);
			}
			xprintk("\n");
			goto out;
		}
		mfn_prev = mfn;
	}
	}
#endif
out:
	balloon_unlock(flags);
	return error;

fail:
	for (i = 0; i < num_gpfn; i++) {
		error = HYPERVISOR_populate_physmap(start_gpfn + i, 0, 0);
		if (error) {
			BUG();//XXX
		}
	}
	goto out;
}

void
__xen_destroy_contiguous_region(unsigned long vstart, unsigned int order)
{
	unsigned long flags;
	unsigned long error = 0;
	unsigned long start_gpfn = __pa(vstart) >> PAGE_SHIFT;
	unsigned long num_gpfn = 1UL << order;
	unsigned long* gpfns;
	struct xen_memory_reservation reservation;
	unsigned long i;

	gpfns = kmalloc(sizeof(gpfns[0]) * num_gpfn,
			GFP_KERNEL | __GFP_NOFAIL);
	for (i = 0; i < num_gpfn; i++) {
		gpfns[i] = start_gpfn + i;
	}

	scrub_pages(vstart, num_gpfn);

	balloon_lock(flags);

	contiguous_bitmap_clear(start_gpfn, num_gpfn);
	error = HYPERVISOR_remove_physmap(start_gpfn, order);
	if (error) {
		goto fail;
	}

	set_xen_guest_handle(reservation.extent_start, gpfns);
	reservation.nr_extents   = num_gpfn;
	reservation.address_bits = 0;
	reservation.extent_order = 0;
	reservation.domid        = DOMID_SELF;
	error = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (error != num_gpfn) {
		error = -EFAULT;//XXX
		goto fail;
	}
	error = 0;
out:
	balloon_unlock(flags);
	kfree(gpfns);
	if (error) {
		// error can't be returned.
		BUG();//XXX
	}
	return;

fail:
	for (i = 0; i < num_gpfn; i++) {
		int tmp_error;// don't overwrite error.
		tmp_error = HYPERVISOR_populate_physmap(start_gpfn + i, 0, 0);
		if (tmp_error) {
			BUG();//XXX
		}
	}
	goto out;
}


///////////////////////////////////////////////////////////////////////////
// grant table hack
// cmd: GNTTABOP_xxx

#include <linux/mm.h>
#include <xen/interface/xen.h>
#include <xen/gnttab.h>

static void
gnttab_map_grant_ref_pre(struct gnttab_map_grant_ref *uop)
{
	uint32_t flags;

	flags = uop->flags;
	if (flags & GNTMAP_readonly) {
#if 0
		xprintd("GNTMAP_readonly is not supported yet\n");
#endif
		flags &= ~GNTMAP_readonly;
	}

	if (flags & GNTMAP_host_map) {
		if (flags & GNTMAP_application_map) {
			xprintd("GNTMAP_application_map is not supported yet: flags 0x%x\n", flags);
			BUG();
		}
		if (flags & GNTMAP_contains_pte) {
			xprintd("GNTMAP_contains_pte is not supported yet flags 0x%x\n", flags);
			BUG();
		}
	} else if (flags & GNTMAP_device_map) {
		xprintd("GNTMAP_device_map is not supported yet 0x%x\n", flags);
		BUG();//XXX not yet. actually this flag is not used.
	} else {
		BUG();
	}
}

int
HYPERVISOR_grant_table_op(unsigned int cmd, void *uop, unsigned int count)
{
	if (cmd == GNTTABOP_map_grant_ref) {
		unsigned int i;
		for (i = 0; i < count; i++) {
			gnttab_map_grant_ref_pre(
				(struct gnttab_map_grant_ref*)uop + i);
		}
	}

	return ____HYPERVISOR_grant_table_op(cmd, uop, count);
}


///////////////////////////////////////////////////////////////////////////
// PageForeign(), SetPageForeign(), ClearPageForeign()

struct address_space xen_ia64_foreign_dummy_mapping;

///////////////////////////////////////////////////////////////////////////
// foreign mapping

struct xen_ia64_privcmd_entry {
	atomic_t	map_count;
	struct page*	page;
};

static void
xen_ia64_privcmd_init_entry(struct xen_ia64_privcmd_entry* entry)
{
	atomic_set(&entry->map_count, 0);
	entry->page = NULL;
}

//TODO alloc_page() to allocate pseudo physical address space is 
//     waste of memory.
//     When vti domain is created, qemu maps all of vti domain pages which 
//     reaches to several hundred megabytes at least.
//     remove alloc_page().
static int
xen_ia64_privcmd_entry_mmap(struct vm_area_struct* vma,
			    unsigned long addr,
			    struct xen_ia64_privcmd_entry* entry,
			    unsigned long mfn,
			    pgprot_t prot,
			    domid_t domid)
{
	int error = 0;
	struct page* page;
	unsigned long gpfn;

	BUG_ON((addr & ~PAGE_MASK) != 0);
	BUG_ON(mfn == INVALID_MFN);

	if (entry->page != NULL) {
		error = -EBUSY;
		goto out;
	}
	page = alloc_page(GFP_KERNEL);
	if (page == NULL) {
		error = -ENOMEM;
		goto out;
	}
	gpfn = page_to_pfn(page);

	error = HYPERVISOR_add_physmap(gpfn, mfn, 0/* prot:XXX */,
				       domid);
	if (error != 0) {
		goto out;
	}

	prot = vma->vm_page_prot;
	error = remap_pfn_range(vma, addr, gpfn, 1 << PAGE_SHIFT, prot);
	if (error != 0) {
		(void)HYPERVISOR_zap_physmap(gpfn, 0);
		error = HYPERVISOR_populate_physmap(gpfn, 0, 0);
		if (error) {
			BUG();//XXX
		}
		__free_page(page);
	} else {
		atomic_inc(&entry->map_count);
		entry->page = page;
	}

out:
	return error;
}

static void
xen_ia64_privcmd_entry_munmap(struct xen_ia64_privcmd_entry* entry)
{
	struct page* page = entry->page;
	unsigned long gpfn = page_to_pfn(page);
	int error;

	error = HYPERVISOR_zap_physmap(gpfn, 0);
	if (error) {
		BUG();//XXX
	}

	error = HYPERVISOR_populate_physmap(gpfn, 0, 0);
	if (error) {
		BUG();//XXX
	}

	entry->page = NULL;
	__free_page(page);
}

static int
xen_ia64_privcmd_entry_open(struct xen_ia64_privcmd_entry* entry)
{
	if (entry->page != NULL) {
		atomic_inc(&entry->map_count);
	} else {
		BUG_ON(atomic_read(&entry->map_count) != 0);
	}
}

static int
xen_ia64_privcmd_entry_close(struct xen_ia64_privcmd_entry* entry)
{
	if (entry->page != NULL && atomic_dec_and_test(&entry->map_count)) {
		xen_ia64_privcmd_entry_munmap(entry);
	}
}

struct xen_ia64_privcmd_range {
	atomic_t			ref_count;
	unsigned long			pgoff; // in PAGE_SIZE

	unsigned long			num_entries;
	struct xen_ia64_privcmd_entry	entries[0];
};

struct xen_ia64_privcmd_vma {
	struct xen_ia64_privcmd_range*	range;

	unsigned long			num_entries;
	struct xen_ia64_privcmd_entry*	entries;
};

static void xen_ia64_privcmd_vma_open(struct vm_area_struct* vma);
static void xen_ia64_privcmd_vma_close(struct vm_area_struct* vma);

struct vm_operations_struct xen_ia64_privcmd_vm_ops = {
	.open = &xen_ia64_privcmd_vma_open,
	.close = &xen_ia64_privcmd_vma_close,
};

static void
__xen_ia64_privcmd_vma_open(struct vm_area_struct* vma,
			    struct xen_ia64_privcmd_vma* privcmd_vma,
			    struct xen_ia64_privcmd_range* privcmd_range)
{
	unsigned long entry_offset = vma->vm_pgoff - privcmd_range->pgoff;
	unsigned long num_entries = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long i;

	BUG_ON(entry_offset < 0);
	BUG_ON(entry_offset + num_entries > privcmd_range->num_entries);

	privcmd_vma->range = privcmd_range;
	privcmd_vma->num_entries = num_entries;
	privcmd_vma->entries = &privcmd_range->entries[entry_offset];
	vma->vm_private_data = privcmd_vma;
	for (i = 0; i < privcmd_vma->num_entries; i++) {
		xen_ia64_privcmd_entry_open(&privcmd_vma->entries[i]);
	}

	vma->vm_private_data = privcmd_vma;
	vma->vm_ops = &xen_ia64_privcmd_vm_ops;
}

static void
xen_ia64_privcmd_vma_open(struct vm_area_struct* vma)
{
	struct xen_ia64_privcmd_vma* privcmd_vma = (struct xen_ia64_privcmd_vma*)vma->vm_private_data;
	struct xen_ia64_privcmd_range* privcmd_range = privcmd_vma->range;

	atomic_inc(&privcmd_range->ref_count);
	// vm_op->open() can't fail.
	privcmd_vma = kmalloc(sizeof(*privcmd_vma), GFP_KERNEL | __GFP_NOFAIL);

	__xen_ia64_privcmd_vma_open(vma, privcmd_vma, privcmd_range);
}

static void
xen_ia64_privcmd_vma_close(struct vm_area_struct* vma)
{
	struct xen_ia64_privcmd_vma* privcmd_vma =
		(struct xen_ia64_privcmd_vma*)vma->vm_private_data;
	struct xen_ia64_privcmd_range* privcmd_range = privcmd_vma->range;
	unsigned long i;

	for (i = 0; i < privcmd_vma->num_entries; i++) {
		xen_ia64_privcmd_entry_close(&privcmd_vma->entries[i]);
	}
	vma->vm_private_data = NULL;
	kfree(privcmd_vma);

	if (atomic_dec_and_test(&privcmd_range->ref_count)) {
#if 1
		for (i = 0; i < privcmd_range->num_entries; i++) {
			struct xen_ia64_privcmd_entry* entry =
				&privcmd_range->entries[i];
			BUG_ON(atomic_read(&entry->map_count) != 0);
			BUG_ON(entry->page != NULL);
		}
#endif
		vfree(privcmd_range);
	}
}

int
privcmd_mmap(struct file * file, struct vm_area_struct * vma)
{
	unsigned long num_entries = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	struct xen_ia64_privcmd_range* privcmd_range;
	struct xen_ia64_privcmd_vma* privcmd_vma;
	unsigned long i;
	BUG_ON(!running_on_xen);

	BUG_ON(file->private_data != NULL);
	privcmd_range =
		vmalloc(sizeof(*privcmd_range) +
			sizeof(privcmd_range->entries[0]) * num_entries);
	if (privcmd_range == NULL) {
		goto out_enomem0;
	}
	privcmd_vma = kmalloc(sizeof(*privcmd_vma), GFP_KERNEL);
	if (privcmd_vma == NULL) {
		goto out_enomem1;
	}

	/* DONTCOPY is essential for Xen as copy_page_range is broken. */
	vma->vm_flags |= VM_RESERVED | VM_IO | VM_DONTCOPY | VM_PFNMAP;

	atomic_set(&privcmd_range->ref_count, 1);
	privcmd_range->pgoff = vma->vm_pgoff;
	privcmd_range->num_entries = num_entries;
	for (i = 0; i < privcmd_range->num_entries; i++) {
		xen_ia64_privcmd_init_entry(&privcmd_range->entries[i]);
	}

	__xen_ia64_privcmd_vma_open(vma, privcmd_vma, privcmd_range);
	return 0;

out_enomem1:
	kfree(privcmd_vma);
out_enomem0:
	vfree(privcmd_range);
	return -ENOMEM;
}

int
direct_remap_pfn_range(struct vm_area_struct *vma,
		       unsigned long address,	// process virtual address
		       unsigned long mfn,	// mfn, mfn + 1, ... mfn + size/PAGE_SIZE
		       unsigned long size,
		       pgprot_t prot,
		       domid_t  domid)		// target domain
{
	struct xen_ia64_privcmd_vma* privcmd_vma =
		(struct xen_ia64_privcmd_vma*)vma->vm_private_data;
	unsigned long i;
	unsigned long offset;
	int error = 0;
	BUG_ON(!running_on_xen);

#if 0
	if (prot != vm->vm_page_prot) {
		return -EINVAL;
	}
#endif

	i = (address - vma->vm_start) >> PAGE_SHIFT;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct xen_ia64_privcmd_entry* entry =
			&privcmd_vma->entries[i];
		error = xen_ia64_privcmd_entry_mmap(vma, (address + offset) & PAGE_MASK, entry, mfn, prot, domid);
		if (error != 0) {
			break;
		}

		i++;
		mfn++;
        }

	return error;
}

