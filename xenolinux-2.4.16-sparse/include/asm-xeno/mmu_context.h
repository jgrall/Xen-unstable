#ifndef __I386_MMU_CONTEXT_H
#define __I386_MMU_CONTEXT_H

#include <linux/config.h>
#include <asm/desc.h>
#include <asm/atomic.h>
#include <asm/pgalloc.h>

/*
 * possibly do the LDT unload here?
 */
#define destroy_context(mm)		do { } while(0)
#define init_new_context(tsk,mm)	0

#ifdef CONFIG_SMP

static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk, unsigned cpu)
{
	if(cpu_tlbstate[cpu].state == TLBSTATE_OK)
		cpu_tlbstate[cpu].state = TLBSTATE_LAZY;	
}
#else
static inline void enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk, unsigned cpu)
{
}
#endif

extern pgd_t *cur_pgd;

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next, struct task_struct *tsk, unsigned cpu)
{
	if (prev != next) {
		/* stop flush ipis for the previous mm */
		clear_bit(cpu, &prev->cpu_vm_mask);
		/*
		 * Re-load LDT if necessary
		 */
		if (prev->context.segments != next->context.segments)
			load_LDT(next);
#ifdef CONFIG_SMP
		cpu_tlbstate[cpu].state = TLBSTATE_OK;
		cpu_tlbstate[cpu].active_mm = next;
#endif
		set_bit(cpu, &next->cpu_vm_mask);
		set_bit(cpu, &next->context.cpuvalid);
		/* Re-load page tables */
                cur_pgd = next->pgd;
                queue_pt_switch(__pa(cur_pgd));
                XENO_flush_page_update_queue();
	}
#ifdef CONFIG_SMP
	else {
		cpu_tlbstate[cpu].state = TLBSTATE_OK;
		if(cpu_tlbstate[cpu].active_mm != next)
			BUG();
		if(!test_and_set_bit(cpu, &next->cpu_vm_mask)) {
			/* We were in lazy tlb mode and leave_mm disabled 
			 * tlb flush IPI delivery. We must flush our tlb.
			 */
			local_flush_tlb();
		}
		if (!test_and_set_bit(cpu, &next->context.cpuvalid))
			load_LDT(next);
	}
#endif
}

#define activate_mm(prev, next) \
	switch_mm((prev),(next),NULL,smp_processor_id())

#endif
