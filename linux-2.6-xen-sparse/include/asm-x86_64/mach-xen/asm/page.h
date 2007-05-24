#ifndef _X86_64_PAGE_H
#define _X86_64_PAGE_H

/* #include <linux/string.h> */
#ifndef __ASSEMBLY__
#include <linux/kernel.h>
#include <linux/types.h>
#include <asm/bug.h>
#endif
#include <xen/interface/xen.h> 

/*
 * Need to repeat this here in order to not include pgtable.h (which in turn
 * depends on definitions made here), but to be able to use the symbolic
 * below. The preprocessor will warn if the two definitions aren't identical.
 */
#define _PAGE_PRESENT	0x001

#define arch_free_page(_page,_order)		\
({	int foreign = PageForeign(_page);	\
	if (foreign)				\
		PageForeignDestructor(_page);	\
	foreign;				\
})
#define HAVE_ARCH_FREE_PAGE

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	12
#ifdef __ASSEMBLY__
#define PAGE_SIZE	(0x1 << PAGE_SHIFT)
#else
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#endif
#define PAGE_MASK	(~(PAGE_SIZE-1))

/* See Documentation/x86_64/mm.txt for a description of the memory map. */
#define __PHYSICAL_MASK_SHIFT	46
#define __PHYSICAL_MASK		((1UL << __PHYSICAL_MASK_SHIFT) - 1)
#define __VIRTUAL_MASK_SHIFT	48
#define __VIRTUAL_MASK		((1UL << __VIRTUAL_MASK_SHIFT) - 1)

#define PHYSICAL_PAGE_MASK	(~(PAGE_SIZE-1) & __PHYSICAL_MASK)

#define THREAD_ORDER 1 
#define THREAD_SIZE  (PAGE_SIZE << THREAD_ORDER)
#define CURRENT_MASK (~(THREAD_SIZE-1))

#define EXCEPTION_STACK_ORDER 0
#define EXCEPTION_STKSZ (PAGE_SIZE << EXCEPTION_STACK_ORDER)

#define DEBUG_STACK_ORDER (EXCEPTION_STACK_ORDER + 1)
#define DEBUG_STKSZ (PAGE_SIZE << DEBUG_STACK_ORDER)

#define IRQSTACK_ORDER 2
#define IRQSTACKSIZE (PAGE_SIZE << IRQSTACK_ORDER)

#define STACKFAULT_STACK 1
#define DOUBLEFAULT_STACK 2
#define NMI_STACK 3
#define DEBUG_STACK 4
#define MCE_STACK 5
#define N_EXCEPTION_STACKS 5  /* hw limit: 7 */

#define LARGE_PAGE_MASK (~(LARGE_PAGE_SIZE-1))
#define LARGE_PAGE_SIZE (1UL << PMD_SHIFT)

#define HPAGE_SHIFT PMD_SHIFT
#define HPAGE_SIZE	((1UL) << HPAGE_SHIFT)
#define HPAGE_MASK	(~(HPAGE_SIZE - 1))
#define HUGETLB_PAGE_ORDER	(HPAGE_SHIFT - PAGE_SHIFT)

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

extern unsigned long end_pfn;

#include <asm/maddr.h>

void clear_page(void *);
void copy_page(void *, void *);

#define clear_user_page(page, vaddr, pg)	clear_page(page)
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

#define alloc_zeroed_user_highpage(vma, vaddr) alloc_page_vma(GFP_HIGHUSER | __GFP_ZERO, vma, vaddr)
#define __HAVE_ARCH_ALLOC_ZEROED_USER_HIGHPAGE

/*
 * These are used to make use of C type-checking..
 */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pud; } pud_t;
typedef struct { unsigned long pgd; } pgd_t;
#define PTE_MASK	PHYSICAL_PAGE_MASK

typedef struct { unsigned long pgprot; } pgprot_t;

#define __pte_val(x) ((x).pte)
#define pte_val(x) ((__pte_val(x) & _PAGE_PRESENT) ? \
                    pte_machine_to_phys(__pte_val(x)) : \
                    __pte_val(x))

#define __pmd_val(x) ((x).pmd)
static inline unsigned long pmd_val(pmd_t x)
{
	unsigned long ret = __pmd_val(x);
#if CONFIG_XEN_COMPAT <= 0x030002
	if (ret) ret = pte_machine_to_phys(ret) | _PAGE_PRESENT;
#else
	if (ret & _PAGE_PRESENT) ret = pte_machine_to_phys(ret);
#endif
	return ret;
}

#define __pud_val(x) ((x).pud)
static inline unsigned long pud_val(pud_t x)
{
	unsigned long ret = __pud_val(x);
	if (ret & _PAGE_PRESENT) ret = pte_machine_to_phys(ret);
	return ret;
}

#define __pgd_val(x) ((x).pgd)
static inline unsigned long pgd_val(pgd_t x)
{
	unsigned long ret = __pgd_val(x);
	if (ret & _PAGE_PRESENT) ret = pte_machine_to_phys(ret);
	return ret;
}

#define pgprot_val(x)	((x).pgprot)

static inline pte_t __pte(unsigned long x)
{
	if (x & _PAGE_PRESENT) x = pte_phys_to_machine(x);
	return ((pte_t) { (x) });
}

static inline pmd_t __pmd(unsigned long x)
{
	if (x & _PAGE_PRESENT) x = pte_phys_to_machine(x);
	return ((pmd_t) { (x) });
}

static inline pud_t __pud(unsigned long x)
{
	if (x & _PAGE_PRESENT) x = pte_phys_to_machine(x);
	return ((pud_t) { (x) });
}

static inline pgd_t __pgd(unsigned long x)
{
	if (x & _PAGE_PRESENT) x = pte_phys_to_machine(x);
	return ((pgd_t) { (x) });
}

#define __pgprot(x)	((pgprot_t) { (x) } )

#define __PHYSICAL_START	((unsigned long)CONFIG_PHYSICAL_START)
#define __START_KERNEL		(__START_KERNEL_map + __PHYSICAL_START)
#define __START_KERNEL_map	0xffffffff80000000UL
#define __PAGE_OFFSET           0xffff880000000000UL	

#else
#define __PHYSICAL_START	CONFIG_PHYSICAL_START
#define __START_KERNEL		(__START_KERNEL_map + __PHYSICAL_START)
#define __START_KERNEL_map	0xffffffff80000000
#define __PAGE_OFFSET           0xffff880000000000
#endif /* !__ASSEMBLY__ */

#if CONFIG_XEN_COMPAT <= 0x030002
#undef LOAD_OFFSET
#define LOAD_OFFSET		0
#endif

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

#define KERNEL_TEXT_SIZE  (40UL*1024*1024)
#define KERNEL_TEXT_START 0xffffffff80000000UL 

#define PAGE_OFFSET		((unsigned long)__PAGE_OFFSET)

/* Note: __pa(&symbol_visible_to_c) should be always replaced with __pa_symbol.
   Otherwise you risk miscompilation. */ 
#define __pa(x)			(((unsigned long)(x)>=__START_KERNEL_map)?(unsigned long)(x) - (unsigned long)__START_KERNEL_map:(unsigned long)(x) - PAGE_OFFSET)
/* __pa_symbol should be used for C visible symbols.
   This seems to be the official gcc blessed way to do such arithmetic. */ 
#define __pa_symbol(x)		\
	({unsigned long v;  \
	  asm("" : "=r" (v) : "0" (x)); \
	  __pa(v); })

#define __va(x)			((void *)((unsigned long)(x)+PAGE_OFFSET))
#define __boot_va(x)		__va(x)
#define __boot_pa(x)		__pa(x)
#ifdef CONFIG_FLATMEM
#define pfn_valid(pfn)		((pfn) < end_pfn)
#endif

#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_kaddr(pfn)      __va((pfn) << PAGE_SHIFT)

#define VM_DATA_DEFAULT_FLAGS \
	(((current->personality & READ_IMPLIES_EXEC) ? VM_EXEC : 0 ) | \
	 VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#define __HAVE_ARCH_GATE_AREA 1	

#include <asm-generic/memory_model.h>
#include <asm-generic/page.h>

#endif /* __KERNEL__ */

#endif /* _X86_64_PAGE_H */
