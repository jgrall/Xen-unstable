#ifndef __ASM_IA64_MM_H__
#define __ASM_IA64_MM_H__

#include <xen/config.h>
#ifdef LINUX_2_6
#include <xen/gfp.h>
#endif
#include <xen/list.h>
#include <xen/spinlock.h>
#include <xen/perfc.h>
#include <xen/sched.h>

#include <linux/rbtree.h>

#include <asm/processor.h>
#include <asm/atomic.h>
#include <asm/flushtlb.h>
#include <asm/io.h>

#include <public/xen.h>

/*
 * The following is for page_alloc.c.
 */

typedef unsigned long page_flags_t;

/*
 * Per-page-frame information.
 * 
 * Every architecture must ensure the following:
 *  1. 'struct pfn_info' contains a 'struct list_head list'.
 *  2. Provide a PFN_ORDER() macro for accessing the order of a free page.
 */
#define PFN_ORDER(_pfn)	((_pfn)->u.free.order)

#define PRtype_info "08x"

struct page
{
    /* Each frame can be threaded onto a doubly-linked list. */
    struct list_head list;

    /* Timestamp from 'TLB clock', used to reduce need for safety flushes. */
    u32 tlbflush_timestamp;

    /* Reference count and various PGC_xxx flags and fields. */
    u32 count_info;

    /* Context-dependent fields follow... */
    union {

        /* Page is in use by a domain. */
        struct {
            /* Owner of this page. */
            u32	_domain;
            /* Type reference count and various PGT_xxx flags and fields. */
            u32 type_info;
        } inuse;

        /* Page is on a free list. */
        struct {
            /* Mask of possibly-tainted TLBs. */
            cpumask_t cpumask;
            /* Order-size of the free chunk this page is the head of. */
            u8 order;
        } free;

    } u;
// following added for Linux compiling
    page_flags_t flags;
    atomic_t _count;
    struct list_head lru;	// is this the same as above "list"?
};

#define set_page_count(p,v) 	atomic_set(&(p)->_count, v - 1)

/* Still small set of flags defined by far on IA-64 */
/* The following page types are MUTUALLY EXCLUSIVE. */
#define PGT_none            (0<<29) /* no special uses of this page */
#define PGT_l1_page_table   (1<<29) /* using this page as an L1 page table? */
#define PGT_l2_page_table   (2<<29) /* using this page as an L2 page table? */
#define PGT_l3_page_table   (3<<29) /* using this page as an L3 page table? */
#define PGT_l4_page_table   (4<<29) /* using this page as an L4 page table? */
#define PGT_writeable_page  (5<<29) /* has writable mappings of this page? */
#define PGT_type_mask       (5<<29) /* Bits 29-31. */

 /* Has this page been validated for use as its current type? */
#define _PGT_validated      28
#define PGT_validated       (1<<_PGT_validated)
/* Owning guest has pinned this page to its current type? */
#define _PGT_pinned         27
#define PGT_pinned          (1U<<_PGT_pinned)

/* 27-bit count of uses of this frame as its current type. */
#define PGT_count_mask      ((1U<<27)-1)

/* Cleared when the owning guest 'frees' this page. */
#define _PGC_allocated      31
#define PGC_allocated       (1U<<_PGC_allocated)
/* Set when the page is used as a page table */
#define _PGC_page_table     30
#define PGC_page_table      (1U<<_PGC_page_table)
/* 30-bit count of references to this frame. */
#define PGC_count_mask      ((1U<<30)-1)

#define IS_XEN_HEAP_FRAME(_pfn) ((page_to_phys(_pfn) < xenheap_phys_end) \
				 && (page_to_phys(_pfn) >= xen_pstart))

static inline struct domain *unpickle_domptr(u32 _d)
{ return (_d == 0) ? NULL : __va(_d); }
static inline u32 pickle_domptr(struct domain *_d)
{ return (_d == NULL) ? 0 : (u32)__pa(_d); }

#define page_get_owner(_p)	(unpickle_domptr((_p)->u.inuse._domain))
#define page_set_owner(_p, _d)	((_p)->u.inuse._domain = pickle_domptr(_d))

/* Dummy now */
#define SHARE_PFN_WITH_DOMAIN(_pfn, _dom) do { } while (0)

extern struct pfn_info *frame_table;
extern unsigned long frame_table_size;
extern struct list_head free_list;
extern spinlock_t free_list_lock;
extern unsigned int free_pfns;
extern unsigned long max_page;

#ifdef CONFIG_VIRTUAL_MEM_MAP
void __init init_frametable(void *frametable_vstart, unsigned long nr_pages);
#else
extern void __init init_frametable(void);
#endif
void add_to_domain_alloc_list(unsigned long ps, unsigned long pe);

static inline void put_page(struct pfn_info *page)
{
#ifdef CONFIG_VTI	// doesn't work with non-VTI in grant tables yet
    u32 nx, x, y = page->count_info;

    do {
	x = y;
	nx = x - 1;
    }
    while (unlikely((y = cmpxchg(&page->count_info, x, nx)) != x));

    if (unlikely((nx & PGC_count_mask) == 0))
	free_domheap_page(page);
#endif
}

/* count_info and ownership are checked atomically. */
static inline int get_page(struct pfn_info *page,
                           struct domain *domain)
{
#ifdef CONFIG_VTI
    u64 x, nx, y = *((u64*)&page->count_info);
    u32 _domain = pickle_domptr(domain);

    do {
	x = y;
	nx = x + 1;
	if (unlikely((x & PGC_count_mask) == 0) ||	/* Not allocated? */
	    unlikely((nx & PGC_count_mask) == 0) ||	/* Count overflow? */
	    unlikely((x >> 32) != _domain)) {		/* Wrong owner? */
	    DPRINTK("Error pfn %lx: rd=%p, od=%p, caf=%08x, taf=%08x\n",
		page_to_pfn(page), domain, unpickle_domptr(domain),
		x, page->u.inuse.type_info);
	    return 0;
	}
    }
    while(unlikely(y = cmpxchg(&page->count_info, x, nx)) != x);
#endif
    return 1;
}

/* No type info now */
#define put_page_type(page)
#define get_page_type(page, type) 1
static inline void put_page_and_type(struct pfn_info *page)
{
    put_page_type(page);
    put_page(page);
}


static inline int get_page_and_type(struct pfn_info *page,
                                    struct domain *domain,
                                    u32 type)
{
    int rc = get_page(page, domain);

    if ( likely(rc) && unlikely(!get_page_type(page, type)) )
    {
        put_page(page);
        rc = 0;
    }

    return rc;
}

#define	set_machinetophys(_mfn, _pfn) do { } while(0);

#ifdef MEMORY_GUARD
void *memguard_init(void *heap_start);
void memguard_guard_stack(void *p);
void memguard_guard_range(void *p, unsigned long l);
void memguard_unguard_range(void *p, unsigned long l);
#else
#define memguard_init(_s)              (_s)
#define memguard_guard_stack(_p)       ((void)0)
#define memguard_guard_range(_p,_l)    ((void)0)
#define memguard_unguard_range(_p,_l)  ((void)0)
#endif

// prototype of misc memory stuff
unsigned long __get_free_pages(unsigned int mask, unsigned int order);
void __free_pages(struct page *page, unsigned int order);
void *pgtable_quicklist_alloc(void);
void pgtable_quicklist_free(void *pgtable_entry);

// FOLLOWING FROM linux-2.6.7/include/mm.h

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
struct vm_area_struct {
	struct mm_struct * vm_mm;	/* The address space we belong to. */
	unsigned long vm_start;		/* Our start address within vm_mm. */
	unsigned long vm_end;		/* The first byte after our end address
					   within vm_mm. */

	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next;

	pgprot_t vm_page_prot;		/* Access permissions of this VMA. */
	unsigned long vm_flags;		/* Flags, listed below. */

#ifndef XEN
	struct rb_node vm_rb;

// XEN doesn't need all the backing store stuff
	/*
	 * For areas with an address space and backing store,
	 * linkage into the address_space->i_mmap prio tree, or
	 * linkage to the list of like vmas hanging off its node, or
	 * linkage of vma in the address_space->i_mmap_nonlinear list.
	 */
	union {
		struct {
			struct list_head list;
			void *parent;	/* aligns with prio_tree_node parent */
			struct vm_area_struct *head;
		} vm_set;

		struct prio_tree_node prio_tree_node;
	} shared;

	/*
	 * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
	 * list, after a COW of one of the file pages.  A MAP_SHARED vma
	 * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
	 * or brk vma (with NULL file) can only be in an anon_vma list.
	 */
	struct list_head anon_vma_node;	/* Serialized by anon_vma->lock */
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */

	/* Function pointers to deal with this struct. */
	struct vm_operations_struct * vm_ops;

	/* Information about our backing store: */
	unsigned long vm_pgoff;		/* Offset (within vm_file) in PAGE_SIZE
					   units, *not* PAGE_CACHE_SIZE */
	struct file * vm_file;		/* File we map to (can be NULL). */
	void * vm_private_data;		/* was vm_pte (shared mem) */

#ifdef CONFIG_NUMA
	struct mempolicy *vm_policy;	/* NUMA policy for the VMA */
#endif
#endif
};
/*
 * vm_flags..
 */
#define VM_READ		0x00000001	/* currently active flags */
#define VM_WRITE	0x00000002
#define VM_EXEC		0x00000004
#define VM_SHARED	0x00000008

#define VM_MAYREAD	0x00000010	/* limits for mprotect() etc */
#define VM_MAYWRITE	0x00000020
#define VM_MAYEXEC	0x00000040
#define VM_MAYSHARE	0x00000080

#define VM_GROWSDOWN	0x00000100	/* general info on the segment */
#define VM_GROWSUP	0x00000200
#define VM_SHM		0x00000400	/* shared memory area, don't swap out */
#define VM_DENYWRITE	0x00000800	/* ETXTBSY on write attempts.. */

#define VM_EXECUTABLE	0x00001000
#define VM_LOCKED	0x00002000
#define VM_IO           0x00004000	/* Memory mapped I/O or similar */

					/* Used by sys_madvise() */
#define VM_SEQ_READ	0x00008000	/* App will access data sequentially */
#define VM_RAND_READ	0x00010000	/* App will not benefit from clustered reads */

#define VM_DONTCOPY	0x00020000      /* Do not copy this vma on fork */
#define VM_DONTEXPAND	0x00040000	/* Cannot expand with mremap() */
#define VM_RESERVED	0x00080000	/* Don't unmap it from swap_out */
#define VM_ACCOUNT	0x00100000	/* Is a VM accounted object */
#define VM_HUGETLB	0x00400000	/* Huge TLB Page VM */
#define VM_NONLINEAR	0x00800000	/* Is non-linear (remap_file_pages) */

#ifndef VM_STACK_DEFAULT_FLAGS		/* arch can override this */
#define VM_STACK_DEFAULT_FLAGS VM_DATA_DEFAULT_FLAGS
#endif

#ifdef CONFIG_STACK_GROWSUP
#define VM_STACK_FLAGS	(VM_GROWSUP | VM_STACK_DEFAULT_FLAGS | VM_ACCOUNT)
#else
#define VM_STACK_FLAGS	(VM_GROWSDOWN | VM_STACK_DEFAULT_FLAGS | VM_ACCOUNT)
#endif

#if 0	/* removed when rebasing to 2.6.13 */
/*
 * The zone field is never updated after free_area_init_core()
 * sets it, so none of the operations on it need to be atomic.
 * We'll have up to (MAX_NUMNODES * MAX_NR_ZONES) zones total,
 * so we use (MAX_NODES_SHIFT + MAX_ZONES_SHIFT) here to get enough bits.
 */
#define NODEZONE_SHIFT (sizeof(page_flags_t)*8 - MAX_NODES_SHIFT - MAX_ZONES_SHIFT)
#define NODEZONE(node, zone)	((node << ZONES_SHIFT) | zone)

static inline unsigned long page_zonenum(struct page *page)
{
	return (page->flags >> NODEZONE_SHIFT) & (~(~0UL << ZONES_SHIFT));
}
static inline unsigned long page_to_nid(struct page *page)
{
	return (page->flags >> (NODEZONE_SHIFT + ZONES_SHIFT));
}

struct zone;
extern struct zone *zone_table[];

static inline struct zone *page_zone(struct page *page)
{
	return zone_table[page->flags >> NODEZONE_SHIFT];
}

static inline void set_page_zone(struct page *page, unsigned long nodezone_num)
{
	page->flags &= ~(~0UL << NODEZONE_SHIFT);
	page->flags |= nodezone_num << NODEZONE_SHIFT;
}
#endif

#ifndef CONFIG_DISCONTIGMEM          /* Don't use mapnrs, do it properly */
extern unsigned long max_mapnr;
#endif

static inline void *lowmem_page_address(struct page *page)
{
	return __va(page_to_pfn(page) << PAGE_SHIFT);
}

#if defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL)
#define HASHED_PAGE_VIRTUAL
#endif

#if defined(WANT_PAGE_VIRTUAL)
#define page_address(page) ((page)->virtual)
#define set_page_address(page, address)			\
	do {						\
		(page)->virtual = (address);		\
	} while(0)
#define page_address_init()  do { } while(0)
#endif

#if defined(HASHED_PAGE_VIRTUAL)
void *page_address(struct page *page);
void set_page_address(struct page *page, void *virtual);
void page_address_init(void);
#endif

#if !defined(HASHED_PAGE_VIRTUAL) && !defined(WANT_PAGE_VIRTUAL)
#define page_address(page) lowmem_page_address(page)
#define set_page_address(page, address)  do { } while(0)
#define page_address_init()  do { } while(0)
#endif


#ifndef CONFIG_DEBUG_PAGEALLOC
static inline void
kernel_map_pages(struct page *page, int numpages, int enable)
{
}
#endif

extern unsigned long num_physpages;
extern unsigned long totalram_pages;
extern int nr_swap_pages;

#ifdef CONFIG_VTI
extern unsigned long *mpt_table;
#undef machine_to_phys_mapping
#define machine_to_phys_mapping	mpt_table

#define INVALID_M2P_ENTRY        (~0U)
#define VALID_M2P(_e)            (!((_e) & (1U<<63)))
#define IS_INVALID_M2P_ENTRY(_e) (!VALID_M2P(_e))
/* If pmt table is provided by control pannel later, we need __get_user
* here. However if it's allocated by HV, we should access it directly
*/
#define phys_to_machine_mapping(d, gpfn)			\
    ((d) == dom0 ? gpfn : 					\
	(gpfn <= d->arch.max_pfn ? (d)->arch.pmt[(gpfn)] :	\
		INVALID_MFN))

#define __mfn_to_gpfn(_d, mfn)			\
    machine_to_phys_mapping[(mfn)]

#define __gpfn_to_mfn(_d, gpfn)			\
    phys_to_machine_mapping((_d), (gpfn))

#define __gpfn_invalid(_d, gpfn)			\
	(__gpfn_to_mfn((_d), (gpfn)) & GPFN_INV_MASK)

#define __gpfn_valid(_d, gpfn)	!__gpfn_invalid(_d, gpfn)

/* Return I/O type if trye */
#define __gpfn_is_io(_d, gpfn)				\
	(__gpfn_valid(_d, gpfn) ? 			\
	(__gpfn_to_mfn((_d), (gpfn)) & GPFN_IO_MASK) : 0)

#define __gpfn_is_mem(_d, gpfn)				\
	(__gpfn_valid(_d, gpfn) ?			\
	((__gpfn_to_mfn((_d), (gpfn)) & GPFN_IO_MASK) == GPFN_MEM) : 0)


#define __gpa_to_mpa(_d, gpa)   \
    ((__gpfn_to_mfn((_d),(gpa)>>PAGE_SHIFT)<<PAGE_SHIFT)|((gpa)&~PAGE_MASK))
#endif // CONFIG_VTI

#endif /* __ASM_IA64_MM_H__ */
