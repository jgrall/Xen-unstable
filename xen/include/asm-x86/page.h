
#ifndef __X86_PAGE_H__
#define __X86_PAGE_H__

#ifndef __ASSEMBLY__
#define PAGE_SIZE           (1UL << PAGE_SHIFT)
#else
#define PAGE_SIZE           (1 << PAGE_SHIFT)
#endif
#define PAGE_MASK           (~(PAGE_SIZE-1))

#if defined(__i386__)
#include <asm/x86_32/page.h>
#elif defined(__x86_64__)
#include <asm/x86_64/page.h>
#endif

/* Convert a pointer to a page-table entry into pagetable slot index. */
#define pgentry_ptr_to_slot(_p) \
    (((unsigned long)(_p) & ~PAGE_MASK) / sizeof(*(_p)))

/* Page-table type. */
#ifndef __ASSEMBLY__
typedef struct { unsigned long pt_lo; } pagetable_t;
#define pagetable_val(_x)   ((_x).pt_lo)
#define pagetable_get_pfn(_x) ((_x).pt_lo >> PAGE_SHIFT)
#define mk_pagetable(_x)    ( (pagetable_t) { (_x) } )
#endif

#define clear_page(_p)      memset((void *)(_p), 0, PAGE_SIZE)
#define copy_page(_t,_f)    memcpy((void *)(_t), (void *)(_f), PAGE_SIZE)

#define PAGE_OFFSET         ((unsigned long)__PAGE_OFFSET)
#define __pa(x)             ((unsigned long)(x)-PAGE_OFFSET)
#define __va(x)             ((void *)((unsigned long)(x)+PAGE_OFFSET))
#define pfn_to_page(_pfn)   (frame_table + (_pfn))
#define phys_to_page(kaddr) (frame_table + ((kaddr) >> PAGE_SHIFT))
#define virt_to_page(kaddr) (frame_table + (__pa(kaddr) >> PAGE_SHIFT))
#define pfn_valid(_pfn)     ((_pfn) < max_page)

#define l1e_get_page(_x)    (pfn_to_page(l1e_get_pfn(_x)))
#define l2e_get_page(_x)    (pfn_to_page(l2e_get_pfn(_x)))
#define l3e_get_page(_x)    (pfn_to_page(l3e_get_pfn(_x)))
#define l4e_get_page(_x)    (pfn_to_page(l4e_get_pfn(_x)))

#define l1e_create_page(_x,_y) (l1e_create_pfn(page_to_pfn(_x),(_y)))
#define l2e_create_page(_x,_y) (l2e_create_pfn(page_to_pfn(_x),(_y)))
#define l3e_create_page(_x,_y) (l3e_create_pfn(page_to_pfn(_x),(_y)))
#define l4e_create_page(_x,_y) (l4e_create_pfn(page_to_pfn(_x),(_y)))

/* High table entries are reserved by the hypervisor. */
#define DOMAIN_ENTRIES_PER_L2_PAGETABLE     \
  (HYPERVISOR_VIRT_START >> L2_PAGETABLE_SHIFT)
#define HYPERVISOR_ENTRIES_PER_L2_PAGETABLE \
  (L2_PAGETABLE_ENTRIES - DOMAIN_ENTRIES_PER_L2_PAGETABLE)

#define linear_l1_table                                                 \
    ((l1_pgentry_t *)(LINEAR_PT_VIRT_START))
#define __linear_l2_table                                                 \
    ((l2_pgentry_t *)(LINEAR_PT_VIRT_START +                            \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<0))))
#define __linear_l3_table                                                 \
    ((l3_pgentry_t *)(LINEAR_PT_VIRT_START +                            \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<0)) +   \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<1))))
#define __linear_l4_table                                                 \
    ((l4_pgentry_t *)(LINEAR_PT_VIRT_START +                            \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<0)) +   \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<1)) +   \
                     (LINEAR_PT_VIRT_START >> (PAGETABLE_ORDER<<2))))

#define linear_pg_table linear_l1_table
#define linear_l2_table(_ed) ((_ed)->arch.guest_vtable)
#define linear_l3_table(_ed) ((_ed)->arch.guest_vl3table)
#define linear_l4_table(_ed) ((_ed)->arch.guest_vl4table)

#define va_to_l1mfn(_ed, _va) \
    (l2e_get_pfn(linear_l2_table(_ed)[_va>>L2_PAGETABLE_SHIFT]))

#ifndef __ASSEMBLY__
extern root_pgentry_t idle_pg_table[ROOT_PAGETABLE_ENTRIES];
extern void paging_init(void);
#endif

#define __pge_off()                                                     \
    do {                                                                \
        __asm__ __volatile__(                                           \
            "mov %0, %%cr4;  # turn off PGE     "                       \
            : : "r" (mmu_cr4_features & ~X86_CR4_PGE) );                \
        } while ( 0 )

#define __pge_on()                                                      \
    do {                                                                \
        __asm__ __volatile__(                                           \
            "mov %0, %%cr4;  # turn off PGE     "                       \
            : : "r" (mmu_cr4_features) );                               \
    } while ( 0 )

#define _PAGE_PRESENT  0x001UL
#define _PAGE_RW       0x002UL
#define _PAGE_USER     0x004UL
#define _PAGE_PWT      0x008UL
#define _PAGE_PCD      0x010UL
#define _PAGE_ACCESSED 0x020UL
#define _PAGE_DIRTY    0x040UL
#define _PAGE_PAT      0x080UL
#define _PAGE_PSE      0x080UL
#define _PAGE_GLOBAL   0x100UL
#define _PAGE_AVAIL    0xe00UL

#define __PAGE_HYPERVISOR \
    (_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED)
#define __PAGE_HYPERVISOR_NOCACHE \
    (_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_PCD | _PAGE_ACCESSED)

#define MAKE_GLOBAL(_x) ((_x) | _PAGE_GLOBAL)

#define PAGE_HYPERVISOR MAKE_GLOBAL(__PAGE_HYPERVISOR)
#define PAGE_HYPERVISOR_NOCACHE MAKE_GLOBAL(__PAGE_HYPERVISOR_NOCACHE)

#ifndef __ASSEMBLY__

static __inline__ int get_order(unsigned long size)
{
    int order;
    
    size = (size-1) >> (PAGE_SHIFT-1);
    order = -1;
    do {
        size >>= 1;
        order++;
    } while (size);
    return order;
}

/* Allocator functions for Xen pagetables. */
struct pfn_info *alloc_xen_pagetable(void);
void free_xen_pagetable(struct pfn_info *pg);
l2_pgentry_t *virt_to_xen_l2e(unsigned long v);

/* Map physical page range in Xen virtual address space. */
#define MAP_SMALL_PAGES (1UL<<16) /* don't use superpages for the mapping */
int
map_pages_to_xen(
    unsigned long virt,
    unsigned long pfn,
    unsigned long nr_pfns,
    unsigned long flags);

#endif /* !__ASSEMBLY__ */

#endif /* __I386_PAGE_H__ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
