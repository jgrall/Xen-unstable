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

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <asm-xen/xen-public/xen.h>
#include <asm-xen/xen-public/dom0_ops.h>
#include <asm-xen/xen-public/io/domain_controller.h>
#include <asm/ptrace.h>
#include <asm/page.h>
#if defined(__i386__)
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#  ifdef CONFIG_X86_PAE
#   include <asm-generic/pgtable-nopud.h>
#  else
#   include <asm-generic/pgtable-nopmd.h>
#  endif
# else
#  define pud_t pgd_t
# endif
#endif

/* arch/xen/i386/kernel/setup.c */
union xen_start_info_union
{
    start_info_t xen_start_info;
    char padding[2048];
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

/* arch/xen/i386/kernel/head.S */
void lgdt_finish(void);

/* arch/xen/i386/mm/hypervisor.c */
/*
 * NB. ptr values should be PHYSICAL, not MACHINE. 'vals' should be already
 * be MACHINE addresses.
 */

void xen_pt_switch(unsigned long ptr);
void xen_new_user_pt(unsigned long ptr); /* x86_64 only */
void xen_load_gs(unsigned int selector); /* x86_64 only */
void xen_tlb_flush(void);
void xen_invlpg(unsigned long ptr);

#ifndef CONFIG_XEN_SHADOW_MODE
void xen_l1_entry_update(pte_t *ptr, pte_t val);
void xen_l2_entry_update(pmd_t *ptr, pmd_t val);
void xen_l3_entry_update(pud_t *ptr, pud_t val); /* x86_64/PAE */
void xen_l4_entry_update(pgd_t *ptr, pgd_t val); /* x86_64 only */
void xen_pgd_pin(unsigned long ptr);
void xen_pgd_unpin(unsigned long ptr);
void xen_pud_pin(unsigned long ptr); /* x86_64 only */
void xen_pud_unpin(unsigned long ptr); /* x86_64 only */
void xen_pmd_pin(unsigned long ptr); /* x86_64 only */
void xen_pmd_unpin(unsigned long ptr); /* x86_64 only */
void xen_pte_pin(unsigned long ptr);
void xen_pte_unpin(unsigned long ptr);
#else
#define xen_l1_entry_update(_p, _v) set_pte((_p), (_v))
#define xen_l2_entry_update(_p, _v) set_pgd((_p), (_v))
#define xen_pgd_pin(_p)   ((void)0)
#define xen_pgd_unpin(_p) ((void)0)
#define xen_pte_pin(_p)   ((void)0)
#define xen_pte_unpin(_p) ((void)0)
#endif

void xen_set_ldt(unsigned long ptr, unsigned long bytes);
void xen_machphys_update(unsigned long mfn, unsigned long pfn);

#ifdef CONFIG_SMP
#include <linux/cpumask.h>
void xen_tlb_flush_all(void);
void xen_invlpg_all(unsigned long ptr);
void xen_tlb_flush_mask(cpumask_t *mask);
void xen_invlpg_mask(cpumask_t *mask, unsigned long ptr);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
/* 
** XXX SMH: 2.4 doesn't have percpu.h (or support SMP guests) so just 
** include sufficient #defines to allow the below to build. 
*/
#define DEFINE_PER_CPU(type, name) \
    __typeof__(type) per_cpu__##name

#define per_cpu(var, cpu)           (*((void)cpu, &per_cpu__##var))
#define __get_cpu_var(var)          per_cpu__##var
#define DECLARE_PER_CPU(type, name) extern __typeof__(type) per_cpu__##name

#define EXPORT_PER_CPU_SYMBOL(var) EXPORT_SYMBOL(per_cpu__##var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var) EXPORT_SYMBOL_GPL(per_cpu__##var)
#endif /* linux < 2.6.0 */

void xen_create_contiguous_region(unsigned long vstart, unsigned int order);
void xen_destroy_contiguous_region(unsigned long vstart, unsigned int order);

/* Allocate a contiguous empty region of low memory. Return virtual start. */
unsigned long allocate_empty_lowmem_region(unsigned long pages);

#include <asm/hypercall.h>

#if defined(CONFIG_X86_64)
#define MULTI_UVMFLAGS_INDEX 2
#define MULTI_UVMDOMID_INDEX 3
#else
#define MULTI_UVMFLAGS_INDEX 3
#define MULTI_UVMDOMID_INDEX 4
#endif

static inline void
MULTI_update_va_mapping(
    multicall_entry_t *mcl, unsigned long va,
    pte_t new_val, unsigned long flags)
{
    mcl->op = __HYPERVISOR_update_va_mapping;
    mcl->args[0] = va;
#if defined(CONFIG_X86_64)
    mcl->args[1] = new_val.pte;
    mcl->args[2] = flags;
#elif defined(CONFIG_X86_PAE)
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = new_val.pte_high;
    mcl->args[3] = flags;
#else
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = 0;
    mcl->args[3] = flags;
#endif
}

static inline void
MULTI_update_va_mapping_otherdomain(
    multicall_entry_t *mcl, unsigned long va,
    pte_t new_val, unsigned long flags, domid_t domid)
{
    mcl->op = __HYPERVISOR_update_va_mapping_otherdomain;
    mcl->args[0] = va;
#if defined(CONFIG_X86_64)
    mcl->args[1] = new_val.pte;
    mcl->args[2] = flags;
    mcl->args[3] = domid;
#elif defined(CONFIG_X86_PAE)
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = new_val.pte_high;
    mcl->args[3] = flags;
    mcl->args[4] = domid;
#else
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = 0;
    mcl->args[3] = flags;
    mcl->args[4] = domid;
#endif
}

#endif /* __HYPERVISOR_H__ */
