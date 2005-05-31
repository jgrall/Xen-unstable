#ifndef __X86_32_PAGE_2L_H__
#define __X86_32_PAGE_2L_H__

#define L1_PAGETABLE_SHIFT      12
#define L2_PAGETABLE_SHIFT      22
#define PAGE_SHIFT              L1_PAGETABLE_SHIFT
#define ROOT_PAGETABLE_SHIFT    L2_PAGETABLE_SHIFT

#define PAGETABLE_ORDER         10
#define L1_PAGETABLE_ENTRIES    (1<<PAGETABLE_ORDER)
#define L2_PAGETABLE_ENTRIES    (1<<PAGETABLE_ORDER)
#define ROOT_PAGETABLE_ENTRIES  L2_PAGETABLE_ENTRIES

#define PADDR_BITS              32
#define PADDR_MASK              (~0UL)

#ifndef __ASSEMBLY__

#include <asm/types.h>

/* read access (should only be used for debug printk's) */
typedef u32 intpte_t;
#define PRIpte "08x"

typedef struct { intpte_t l1; } l1_pgentry_t;
typedef struct { intpte_t l2; } l2_pgentry_t;
typedef l2_pgentry_t root_pgentry_t;

#endif /* !__ASSEMBLY__ */

/* root table */
#define root_get_pfn              l2e_get_pfn
#define root_get_flags            l2e_get_flags
#define root_get_value            l2e_get_value
#define root_empty                l2e_empty
#define root_create_phys          l2e_create_phys
#define PGT_root_page_table       PGT_l2_page_table

/* misc */
#define is_guest_l1_slot(_s)    (1)
#define is_guest_l2_slot(_t,_s) ((_s) < L2_PAGETABLE_FIRST_XEN_SLOT)

/*
 * PTE pfn and flags:
 *  20-bit pfn   = (pte[31:12])
 *  12-bit flags = (pte[11:0])
 */

/* Extract flags into 12-bit integer, or turn 12-bit flags into a pte mask. */
#define get_pte_flags(x) ((int)(x) & 0xFFF)
#define put_pte_flags(x) ((intpte_t)(x))

#define L1_DISALLOW_MASK (0xFFFFF180U) /* PAT/GLOBAL */
#define L2_DISALLOW_MASK (0xFFFFF180U) /* PSE/GLOBAL */

#endif /* __X86_32_PAGE_2L_H__ */
