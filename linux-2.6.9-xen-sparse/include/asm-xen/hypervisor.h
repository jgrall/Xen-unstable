/******************************************************************************
 * hypervisor.h
 * 
 * Linux-specific hypervisor handling.
 * 
 * Copyright (c) 2002-2004, K A Fraser
 * 
 * This file may be distributed separately from the Linux kernel, or
 * incorporated into other software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef __HYPERVISOR_H__
#define __HYPERVISOR_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <asm-xen/xen-public/xen.h>
#include <asm-xen/xen-public/dom0_ops.h>
#include <asm-xen/xen-public/io/domain_controller.h>
#include <asm/ptrace.h>
#include <asm/page.h>

/* arch/xen/i386/kernel/setup.c */
union xen_start_info_union
{
    start_info_t xen_start_info;
    char padding[512];
};
extern union xen_start_info_union xen_start_info_union;
#define xen_start_info (xen_start_info_union.xen_start_info)

/* arch/xen/kernel/evtchn.c */
/* Force a proper event-channel callback from Xen. */
void force_evtchn_callback(void);

/* arch/xen/kernel/process.c */
void xen_cpu_idle (void);

/* arch/xen/i386/kernel/hypervisor.c */
void do_hypervisor_callback(struct pt_regs *regs);

/* arch/xen/i386/mm/init.c */
/* NOTE: caller must call flush_page_update_queue() */
#define PROT_ON  1
#define PROT_OFF 0
void /* __init */ protect_page(pgd_t *dpgd, void *page, int mode);
void /* __init */ protect_pagetable(pgd_t *dpgd, pgd_t *spgd, int mode);

/* arch/xen/i386/kernel/head.S */
void lgdt_finish(void);

/* arch/xen/i386/mm/hypervisor.c */
/*
 * NB. ptr values should be PHYSICAL, not MACHINE. 'vals' should be already
 * be MACHINE addresses.
 */

void queue_l1_entry_update(pte_t *ptr, unsigned long val);
void queue_l2_entry_update(pmd_t *ptr, unsigned long val);
void queue_pt_switch(unsigned long ptr);
void queue_tlb_flush(void);
void queue_invlpg(unsigned long ptr);
void queue_pgd_pin(unsigned long ptr);
void queue_pgd_unpin(unsigned long ptr);
void queue_pte_pin(unsigned long ptr);
void queue_pte_unpin(unsigned long ptr);
void queue_set_ldt(unsigned long ptr, unsigned long bytes);
void queue_machphys_update(unsigned long mfn, unsigned long pfn);
void xen_l1_entry_update(pte_t *ptr, unsigned long val);
void xen_l2_entry_update(pmd_t *ptr, unsigned long val);
void xen_pt_switch(unsigned long ptr);
void xen_tlb_flush(void);
void xen_invlpg(unsigned long ptr);
void xen_pgd_pin(unsigned long ptr);
void xen_pgd_unpin(unsigned long ptr);
void xen_pte_pin(unsigned long ptr);
void xen_pte_unpin(unsigned long ptr);
void xen_set_ldt(unsigned long ptr, unsigned long bytes);
void xen_machphys_update(unsigned long mfn, unsigned long pfn);
#define MMU_UPDATE_DEBUG 0

#if MMU_UPDATE_DEBUG > 0
typedef struct {
    void *ptr;
    unsigned long val, pteval;
    void *ptep;
    int line; char *file;
} page_update_debug_t;
extern page_update_debug_t update_debug_queue[];
#define queue_l1_entry_update(_p,_v) ({                           \
 update_debug_queue[mmu_update_queue_idx].ptr  = (_p);             \
 update_debug_queue[mmu_update_queue_idx].val  = (_v);             \
 update_debug_queue[mmu_update_queue_idx].line = __LINE__;         \
 update_debug_queue[mmu_update_queue_idx].file = __FILE__;         \
 queue_l1_entry_update((_p),(_v));                                \
})
#define queue_l2_entry_update(_p,_v) ({                           \
 update_debug_queue[mmu_update_queue_idx].ptr  = (_p);             \
 update_debug_queue[mmu_update_queue_idx].val  = (_v);             \
 update_debug_queue[mmu_update_queue_idx].line = __LINE__;         \
 update_debug_queue[mmu_update_queue_idx].file = __FILE__;         \
 queue_l2_entry_update((_p),(_v));                                \
})
#endif

#if MMU_UPDATE_DEBUG > 1
#if MMU_UPDATE_DEBUG > 2
#undef queue_l1_entry_update
#define queue_l1_entry_update(_p,_v) ({                           \
 update_debug_queue[mmu_update_queue_idx].ptr  = (_p);             \
 update_debug_queue[mmu_update_queue_idx].val  = (_v);             \
 update_debug_queue[mmu_update_queue_idx].line = __LINE__;         \
 update_debug_queue[mmu_update_queue_idx].file = __FILE__;         \
 printk("L1 %s %d: %p/%08lx (%08lx -> %08lx)\n", __FILE__, __LINE__,  \
        (_p), virt_to_machine(_p), pte_val(*(_p)),                 \
        (unsigned long)(_v));                                     \
 queue_l1_entry_update((_p),(_v));                                \
})
#endif
#undef queue_l2_entry_update
#define queue_l2_entry_update(_p,_v) ({                           \
 update_debug_queue[mmu_update_queue_idx].ptr  = (_p);             \
 update_debug_queue[mmu_update_queue_idx].val  = (_v);             \
 update_debug_queue[mmu_update_queue_idx].line = __LINE__;         \
 update_debug_queue[mmu_update_queue_idx].file = __FILE__;         \
 printk("L2 %s %d: %p/%08lx (%08lx -> %08lx)\n", __FILE__, __LINE__,  \
        (_p), virt_to_machine(_p), pmd_val(*_p),                  \
        (unsigned long)(_v));                                     \
 queue_l2_entry_update((_p),(_v));                                \
})
#define queue_pt_switch(_p) ({                                    \
 printk("PTSWITCH %s %d: %08lx\n", __FILE__, __LINE__, (_p));     \
 queue_pt_switch(_p);                                             \
})   
#define queue_tlb_flush() ({                                      \
 printk("TLB FLUSH %s %d\n", __FILE__, __LINE__);                 \
 queue_tlb_flush();                                               \
})   
#define queue_invlpg(_p) ({                                       \
 printk("INVLPG %s %d: %08lx\n", __FILE__, __LINE__, (_p));       \
 queue_invlpg(_p);                                                \
})   
#define queue_pgd_pin(_p) ({                                      \
 printk("PGD PIN %s %d: %08lx/%08lx\n", __FILE__, __LINE__, (_p), \
	phys_to_machine(_p));                                     \
 queue_pgd_pin(_p);                                               \
})   
#define queue_pgd_unpin(_p) ({                                    \
 printk("PGD UNPIN %s %d: %08lx\n", __FILE__, __LINE__, (_p));    \
 queue_pgd_unpin(_p);                                             \
})   
#define queue_pte_pin(_p) ({                                      \
 printk("PTE PIN %s %d: %08lx\n", __FILE__, __LINE__, (_p));      \
 queue_pte_pin(_p);                                               \
})   
#define queue_pte_unpin(_p) ({                                    \
 printk("PTE UNPIN %s %d: %08lx\n", __FILE__, __LINE__, (_p));    \
 queue_pte_unpin(_p);                                             \
})   
#define queue_set_ldt(_p,_l) ({                                        \
 printk("SETL LDT %s %d: %08lx %d\n", __FILE__, __LINE__, (_p), (_l)); \
 queue_set_ldt((_p), (_l));                                            \
})   
#endif

void _flush_page_update_queue(void);
#define flush_page_update_queue() do {				\
    DECLARE_PER_CPU(unsigned int, mmu_update_queue_idx);	\
    if (per_cpu(mmu_update_queue_idx, smp_processor_id()))	\
	_flush_page_update_queue();				\
} while (0)
#define xen_flush_page_update_queue() (_flush_page_update_queue())
#define XEN_flush_page_update_queue() (_flush_page_update_queue())
void MULTICALL_flush_page_update_queue(void);

#ifdef CONFIG_XEN_PHYSDEV_ACCESS
/* Allocate a contiguous empty region of low memory. Return virtual start. */
unsigned long allocate_empty_lowmem_region(unsigned long pages);
/* Deallocate a contiguous region of low memory. Return it to the allocator. */
void deallocate_lowmem_region(unsigned long vstart, unsigned long pages);
#endif

/*
 * Assembler stubs for hyper-calls.
 */

static inline int
HYPERVISOR_set_trap_table(
    trap_info_t *table)
{
    int ret;
    unsigned long ignore;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ignore)
	: "0" (__HYPERVISOR_set_trap_table), "1" (table)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_mmu_update(
    mmu_update_t *req, int count, int *success_count)
{
    int ret;
    unsigned long ign1, ign2, ign3;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3)
	: "0" (__HYPERVISOR_mmu_update), "1" (req), "2" (count),
	  "3" (success_count)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_set_gdt(
    unsigned long *frame_list, int entries)
{
    int ret;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_set_gdt), "1" (frame_list), "2" (entries)
	: "memory" );


    return ret;
}

static inline int
HYPERVISOR_stack_switch(
    unsigned long ss, unsigned long esp)
{
    int ret;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_stack_switch), "1" (ss), "2" (esp)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_set_callbacks(
    unsigned long event_selector, unsigned long event_address,
    unsigned long failsafe_selector, unsigned long failsafe_address)
{
    int ret;
    unsigned long ign1, ign2, ign3, ign4;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3), "=S" (ign4)
	: "0" (__HYPERVISOR_set_callbacks), "1" (event_selector),
	  "2" (event_address), "3" (failsafe_selector), "4" (failsafe_address)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_fpu_taskswitch(
    void)
{
    int ret;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret) : "0" (__HYPERVISOR_fpu_taskswitch) : "memory" );

    return ret;
}

static inline int
HYPERVISOR_yield(
    void)
{
    int ret;
    unsigned long ign;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign)
	: "0" (__HYPERVISOR_sched_op), "1" (SCHEDOP_yield)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_block(
    void)
{
    int ret;
    unsigned long ign1;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1)
	: "0" (__HYPERVISOR_sched_op), "1" (SCHEDOP_block)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_shutdown(
    void)
{
    int ret;
    unsigned long ign1;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1)
	: "0" (__HYPERVISOR_sched_op),
	  "1" (SCHEDOP_shutdown | (SHUTDOWN_poweroff << SCHEDOP_reasonshift))
        : "memory" );

    return ret;
}

static inline int
HYPERVISOR_reboot(
    void)
{
    int ret;
    unsigned long ign1;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1)
	: "0" (__HYPERVISOR_sched_op),
	  "1" (SCHEDOP_shutdown | (SHUTDOWN_reboot << SCHEDOP_reasonshift))
        : "memory" );

    return ret;
}

static inline int
HYPERVISOR_suspend(
    unsigned long srec)
{
    int ret;
    unsigned long ign1, ign2;

    /* NB. On suspend, control software expects a suspend record in %esi. */
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=S" (ign2)
	: "0" (__HYPERVISOR_sched_op),
        "b" (SCHEDOP_shutdown | (SHUTDOWN_suspend << SCHEDOP_reasonshift)), 
        "S" (srec) : "memory");

    return ret;
}

static inline long
HYPERVISOR_set_timer_op(
    u64 timeout)
{
    int ret;
    unsigned long timeout_hi = (unsigned long)(timeout>>32);
    unsigned long timeout_lo = (unsigned long)timeout;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_set_timer_op), "b" (timeout_hi), "c" (timeout_lo)
	: "memory");

    return ret;
}

static inline int
HYPERVISOR_dom0_op(
    dom0_op_t *dom0_op)
{
    int ret;
    unsigned long ign1;

    dom0_op->interface_version = DOM0_INTERFACE_VERSION;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1)
	: "0" (__HYPERVISOR_dom0_op), "1" (dom0_op)
	: "memory");

    return ret;
}

static inline int
HYPERVISOR_set_debugreg(
    int reg, unsigned long value)
{
    int ret;
    unsigned long ign1, ign2;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_set_debugreg), "1" (reg), "2" (value)
	: "memory" );

    return ret;
}

static inline unsigned long
HYPERVISOR_get_debugreg(
    int reg)
{
    unsigned long ret;
    unsigned long ign;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign)
	: "0" (__HYPERVISOR_get_debugreg), "1" (reg)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_update_descriptor(
    unsigned long ma, unsigned long word1, unsigned long word2)
{
    int ret;
    unsigned long ign1, ign2, ign3;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3)
	: "0" (__HYPERVISOR_update_descriptor), "1" (ma), "2" (word1),
	  "3" (word2)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_set_fast_trap(
    int idx)
{
    int ret;
    unsigned long ign;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign)
	: "0" (__HYPERVISOR_set_fast_trap), "1" (idx)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_dom_mem_op(
    unsigned int op, unsigned long *extent_list,
    unsigned long nr_extents, unsigned int extent_order)
{
    int ret;
    unsigned long ign1, ign2, ign3, ign4, ign5;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3), "=S" (ign4),
	  "=D" (ign5)
	: "0" (__HYPERVISOR_dom_mem_op), "1" (op), "2" (extent_list),
	  "3" (nr_extents), "4" (extent_order), "5" (DOMID_SELF)
        : "memory" );

    return ret;
}

static inline int
HYPERVISOR_multicall(
    void *call_list, int nr_calls)
{
    int ret;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_multicall), "1" (call_list), "2" (nr_calls)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_update_va_mapping(
    unsigned long page_nr, pte_t new_val, unsigned long flags)
{
    int ret;
    unsigned long ign1, ign2, ign3;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3)
	: "0" (__HYPERVISOR_update_va_mapping), 
          "1" (page_nr), "2" ((new_val).pte_low), "3" (flags)
	: "memory" );

    if ( unlikely(ret < 0) )
    {
        printk(KERN_ALERT "Failed update VA mapping: %08lx, %08lx, %08lx\n",
               page_nr, (new_val).pte_low, flags);
        BUG();
    }

    return ret;
}

static inline int
HYPERVISOR_event_channel_op(
    void *op)
{
    int ret;
    unsigned long ignore;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ignore)
	: "0" (__HYPERVISOR_event_channel_op), "1" (op)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_xen_version(
    int cmd)
{
    int ret;
    unsigned long ignore;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ignore)
	: "0" (__HYPERVISOR_xen_version), "1" (cmd)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_console_io(
    int cmd, int count, char *str)
{
    int ret;
    unsigned long ign1, ign2, ign3;
    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3)
	: "0" (__HYPERVISOR_console_io), "1" (cmd), "2" (count), "3" (str)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_physdev_op(
    void *physdev_op)
{
    int ret;
    unsigned long ign;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign)
	: "0" (__HYPERVISOR_physdev_op), "1" (physdev_op)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_grant_table_op(
    unsigned int cmd, void *uop, unsigned int count)
{
    int ret;
    unsigned long ign1, ign2, ign3;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3)
	: "0" (__HYPERVISOR_grant_table_op), "1" (cmd), "2" (count), "3" (uop)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_update_va_mapping_otherdomain(
    unsigned long page_nr, pte_t new_val, unsigned long flags, domid_t domid)
{
    int ret;
    unsigned long ign1, ign2, ign3, ign4;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2), "=d" (ign3), "=S" (ign4)
	: "0" (__HYPERVISOR_update_va_mapping_otherdomain),
          "1" (page_nr), "2" ((new_val).pte_low), "3" (flags), "4" (domid) :
        "memory" );
    
    return ret;
}

static inline int
HYPERVISOR_vm_assist(
    unsigned int cmd, unsigned int type)
{
    int ret;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_vm_assist), "1" (cmd), "2" (type)
	: "memory" );

    return ret;
}

static inline int
HYPERVISOR_boot_vcpu(
    unsigned long vcpu, full_execution_context_t *ctxt)
{
    int ret;
    unsigned long ign1, ign2;

    __asm__ __volatile__ (
        TRAP_INSTR
        : "=a" (ret), "=b" (ign1), "=c" (ign2)
	: "0" (__HYPERVISOR_boot_vcpu), "1" (vcpu), "2" (ctxt)
	: "memory");

    return ret;
}

#endif /* __HYPERVISOR_H__ */
