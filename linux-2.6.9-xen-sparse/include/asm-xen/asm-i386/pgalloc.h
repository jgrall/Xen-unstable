#ifndef _I386_PGALLOC_H
#define _I386_PGALLOC_H

#include <linux/config.h>
#include <asm/processor.h>
#include <asm/fixmap.h>
#include <linux/threads.h>
#include <linux/mm.h>		/* for struct page */
#include <asm/io.h>		/* for phys_to_virt and page_to_pseudophys */

#define pmd_populate_kernel(mm, pmd, pte) \
		set_pmd(pmd, __pmd(_PAGE_TABLE + __pa(pte)))

static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd, struct page *pte)
{
	set_pmd(pmd, __pmd(_PAGE_TABLE +
		((unsigned long long)page_to_pfn(pte) <<
			(unsigned long long) PAGE_SHIFT)));
	flush_page_update_queue();
	/* XXXcl queue */
}
/*
 * Allocate and free page tables.
 */

extern pgd_t *pgd_alloc(struct mm_struct *);
extern void pgd_free(pgd_t *pgd);

extern pte_t *pte_alloc_one_kernel(struct mm_struct *, unsigned long);
extern struct page *pte_alloc_one(struct mm_struct *, unsigned long);

static inline void pte_free_kernel(pte_t *pte)
{
	free_page((unsigned long)pte);
	__make_page_writable(pte);
	flush_page_update_queue();
}

extern void pte_free(struct page *pte);

#define __pte_free_tlb(tlb,pte)	tlb_remove_page((tlb),(pte))

/*
 * allocating and freeing a pmd is trivial: the 1-entry pmd is
 * inside the pgd, so has no extra memory associated with it.
 * (In the PAE case we free the pmds as part of the pgd.)
 */

#define pmd_alloc_one(mm, addr)		({ BUG(); ((pmd_t *)2); })
#define pmd_free(x)			do { } while (0)
#define __pmd_free_tlb(tlb,x)		do { } while (0)
#define pgd_populate(mm, pmd, pte)	BUG()

#define check_pgt_cache()	do { } while (0)

int direct_remap_area_pages(struct mm_struct *mm,
                            unsigned long address, 
                            unsigned long machine_addr,
                            unsigned long size, 
                            pgprot_t prot,
                            domid_t  domid);
int __direct_remap_area_pages(struct mm_struct *mm,
			      unsigned long address, 
			      unsigned long size, 
			      mmu_update_t *v);

#endif /* _I386_PGALLOC_H */
