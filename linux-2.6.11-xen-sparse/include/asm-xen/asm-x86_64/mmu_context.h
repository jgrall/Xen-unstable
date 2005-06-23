#ifndef __X86_64_MMU_CONTEXT_H
#define __X86_64_MMU_CONTEXT_H

#include <linux/config.h>
#include <asm/desc.h>
#include <asm/atomic.h>
#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/pda.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

/*
 * possibly do the LDT unload here?
 */
int init_new_context(struct task_struct *tsk, struct mm_struct *mm);
void destroy_context(struct mm_struct *mm);

#ifdef CONFIG_SMP

static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	if (read_pda(mmu_state) == TLBSTATE_OK) 
		write_pda(mmu_state, TLBSTATE_LAZY);
}
#else
static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
}
#endif

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next, 
			     struct task_struct *tsk)
{
	unsigned cpu = smp_processor_id();
	struct mmuext_op _op[3], *op = _op;

	if (likely(prev != next)) {
		/* stop flush ipis for the previous mm */
		clear_bit(cpu, &prev->cpu_vm_mask);
#if 0  /* XEN: no lazy tlb */
		write_pda(mmu_state, TLBSTATE_OK);
		write_pda(active_mm, next);
#endif
		set_bit(cpu, &next->cpu_vm_mask);

		/* load_cr3(next->pgd) */
		per_cpu(cur_pgd, smp_processor_id()) = next->pgd;
		op->cmd = MMUEXT_NEW_BASEPTR;
		op->mfn = pfn_to_mfn(__pa(next->pgd) >> PAGE_SHIFT);
		op++;

		/* xen_new_user_pt(__pa(__user_pgd(next->pgd))) */
		op->cmd = MMUEXT_NEW_USER_BASEPTR;
		op->mfn = pfn_to_mfn(__pa(__user_pgd(next->pgd)) >> PAGE_SHIFT);
		op++;
		
		if (unlikely(next->context.ldt != prev->context.ldt)) {
			/* load_LDT_nolock(&next->context, cpu) */
			op->cmd = MMUEXT_SET_LDT;
			op->linear_addr = (unsigned long)next->context.ldt;
			op->nr_ents     = next->context.size;
			op++;
		}

		BUG_ON(HYPERVISOR_mmuext_op(_op, op-_op, NULL, DOMID_SELF));
	}

#if 0 /* XEN: no lazy tlb */
	else {
		write_pda(mmu_state, TLBSTATE_OK);
		if (read_pda(active_mm) != next)
			out_of_line_bug();
		if(!test_and_set_bit(cpu, &next->cpu_vm_mask)) {
			/* We were in lazy tlb mode and leave_mm disabled 
			 * tlb flush IPI delivery. We must reload CR3
			 * to make sure to use no freed page tables.
			 */
                        load_cr3(next->pgd);
                        xen_new_user_pt(__pa(__user_pgd(next->pgd)));		
			load_LDT_nolock(&next->context, cpu);
		}
	}
#endif
}

#define deactivate_mm(tsk,mm)	do { \
	load_gs_index(0); \
	asm volatile("movl %0,%%fs"::"r"(0));  \
} while(0)

#define activate_mm(prev, next) do {		\
	switch_mm((prev),(next),NULL);		\
} while (0)

#endif
