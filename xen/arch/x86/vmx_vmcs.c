/*
 * vmx_vmcs.c: VMCS management
 * Copyright (c) 2004, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/domain_page.h>
#include <asm/current.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/vmx.h>
#include <asm/flushtlb.h>
#include <xen/event.h>
#include <xen/kernel.h>
#include <public/io/ioreq.h>
#if CONFIG_PAGING_LEVELS >= 4
#include <asm/shadow_64.h>
#endif
#ifdef CONFIG_VMX

struct vmcs_struct *alloc_vmcs(void) 
{
    struct vmcs_struct *vmcs;
    u32 vmx_msr_low, vmx_msr_high;

    rdmsr(MSR_IA32_VMX_BASIC_MSR, vmx_msr_low, vmx_msr_high);
    vmcs_size = vmx_msr_high & 0x1fff;
    vmcs = alloc_xenheap_pages(get_order(vmcs_size)); 
    memset((char *)vmcs, 0, vmcs_size); /* don't remove this */

    vmcs->vmcs_revision_id = vmx_msr_low;
    return vmcs;
} 

void free_vmcs(struct vmcs_struct *vmcs)
{
    int order;

    order = get_order(vmcs_size);
    free_xenheap_pages(vmcs, order);
}

static inline int construct_vmcs_controls(void)
{
    int error = 0;

    error |= __vmwrite(PIN_BASED_VM_EXEC_CONTROL, 
                       MONITOR_PIN_BASED_EXEC_CONTROLS);

    error |= __vmwrite(CPU_BASED_VM_EXEC_CONTROL, 
                       MONITOR_CPU_BASED_EXEC_CONTROLS);

    error |= __vmwrite(VM_EXIT_CONTROLS, MONITOR_VM_EXIT_CONTROLS);

    error |= __vmwrite(VM_ENTRY_CONTROLS, MONITOR_VM_ENTRY_CONTROLS);

    return error;
}

#define GUEST_SEGMENT_LIMIT     0xffffffff      
#define HOST_SEGMENT_LIMIT      0xffffffff      

struct host_execution_env {
    /* selectors */
    unsigned short ldtr_selector;
    unsigned short tr_selector;
    unsigned short ds_selector;
    unsigned short cs_selector;
    /* limits */
    unsigned short gdtr_limit;
    unsigned short ldtr_limit;
    unsigned short idtr_limit;
    unsigned short tr_limit;
    /* base */
    unsigned long gdtr_base;
    unsigned long ldtr_base;
    unsigned long idtr_base;
    unsigned long tr_base;
    unsigned long ds_base;
    unsigned long cs_base;
#ifdef __x86_64__ 
    unsigned long fs_base; 
    unsigned long gs_base; 
#endif 

    /* control registers */
    unsigned long cr3;
    unsigned long cr0;
    unsigned long cr4;
    unsigned long dr7;
};

#define round_pgdown(_p) ((_p)&PAGE_MASK) /* coped from domain.c */

int vmx_setup_platform(struct vcpu *d, struct cpu_user_regs *regs)
{
    int i;
    unsigned int n;
    unsigned long *p, mpfn, offset, addr;
    struct e820entry *e820p;
    unsigned long gpfn = 0;

    local_flush_tlb_pge();
    regs->ebx = 0;   /* Linux expects ebx to be 0 for boot proc */

    n = regs->ecx;
    if (n > 32) {
        VMX_DBG_LOG(DBG_LEVEL_1, "Too many e820 entries: %d", n);
        return -1;
    }

    addr = regs->edi;
    offset = (addr & ~PAGE_MASK);
    addr = round_pgdown(addr);

    mpfn = phys_to_machine_mapping(addr >> PAGE_SHIFT);
    p = map_domain_page(mpfn);

    e820p = (struct e820entry *) ((unsigned long) p + offset); 

#ifndef NDEBUG
    print_e820_memory_map(e820p, n);
#endif

    for ( i = 0; i < n; i++ )
    {
        if ( e820p[i].type == E820_SHARED_PAGE )
        {
            gpfn = (e820p[i].addr >> PAGE_SHIFT);
            break;
        }
    }

    if ( gpfn == 0 )
    {
        unmap_domain_page(p);        
        return -1;
    }   

    unmap_domain_page(p);        

    /* Initialise shared page */
    mpfn = phys_to_machine_mapping(gpfn);
    p = map_domain_page(mpfn);
    d->domain->arch.vmx_platform.shared_page_va = (unsigned long)p;

   VMX_DBG_LOG(DBG_LEVEL_1, "eport: %x\n", iopacket_port(d->domain));

   clear_bit(iopacket_port(d->domain), 
             &d->domain->shared_info->evtchn_mask[0]);

    return 0;
}

void vmx_do_launch(struct vcpu *v) 
{
/* Update CR3, GDT, LDT, TR */
    unsigned int tr, cpu, error = 0;
    struct host_execution_env host_env;
    struct Xgt_desc_struct desc;
    unsigned long pfn = 0;
    struct pfn_info *page;
    struct cpu_user_regs *regs = guest_cpu_user_regs();

    vmx_stts();

    cpu = smp_processor_id();

    page = (struct pfn_info *) alloc_domheap_page(NULL);
    pfn = (unsigned long) (page - frame_table);

    vmx_setup_platform(v, regs);

    __asm__ __volatile__ ("sidt  (%0) \n" :: "a"(&desc) : "memory");
    host_env.idtr_limit = desc.size;
    host_env.idtr_base = desc.address;
    error |= __vmwrite(HOST_IDTR_BASE, host_env.idtr_base);
 
    __asm__ __volatile__ ("sgdt  (%0) \n" :: "a"(&desc) : "memory");
    host_env.gdtr_limit = desc.size;
    host_env.gdtr_base = desc.address;
    error |= __vmwrite(HOST_GDTR_BASE, host_env.gdtr_base);

    error |= __vmwrite(GUEST_LDTR_SELECTOR, 0);
    error |= __vmwrite(GUEST_LDTR_BASE, 0);
    error |= __vmwrite(GUEST_LDTR_LIMIT, 0);
        
    __asm__ __volatile__ ("str  (%0) \n" :: "a"(&tr) : "memory");
    host_env.tr_selector = tr;
    host_env.tr_limit = sizeof(struct tss_struct);
    host_env.tr_base = (unsigned long) &init_tss[cpu];

    error |= __vmwrite(HOST_TR_SELECTOR, host_env.tr_selector);
    error |= __vmwrite(HOST_TR_BASE, host_env.tr_base);
    error |= __vmwrite(GUEST_TR_BASE, 0);
    error |= __vmwrite(GUEST_TR_LIMIT, 0xff);

    __vmwrite(GUEST_CR3, pagetable_get_paddr(v->arch.guest_table));
    __vmwrite(HOST_CR3, pagetable_get_paddr(v->arch.monitor_table));
    __vmwrite(HOST_RSP, (unsigned long)get_stack_bottom());

    v->arch.schedule_tail = arch_vmx_do_resume;
}

/*
 * Initially set the same environement as host.
 */
static inline int 
construct_init_vmcs_guest(struct cpu_user_regs *regs, 
                          struct vcpu_guest_context *ctxt,
                          struct host_execution_env *host_env)
{
    int error = 0;
    union vmcs_arbytes arbytes;
    unsigned long dr7;
    unsigned long eflags, shadow_cr;

    /* MSR */
    error |= __vmwrite(VM_EXIT_MSR_LOAD_ADDR, 0);
    error |= __vmwrite(VM_EXIT_MSR_STORE_ADDR, 0);

    error |= __vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
    error |= __vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
    error |= __vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
    /* interrupt */
    error |= __vmwrite(VM_ENTRY_INTR_INFO_FIELD, 0);
    /* mask */
    error |= __vmwrite(CR0_GUEST_HOST_MASK, -1UL);
    error |= __vmwrite(CR4_GUEST_HOST_MASK, -1UL);

    error |= __vmwrite(PAGE_FAULT_ERROR_CODE_MASK, 0);
    error |= __vmwrite(PAGE_FAULT_ERROR_CODE_MATCH, 0);

    /* TSC */
    error |= __vmwrite(TSC_OFFSET, 0);
    error |= __vmwrite(CR3_TARGET_COUNT, 0);

    /* Guest Selectors */
    error |= __vmwrite(GUEST_CS_SELECTOR, regs->cs);
    error |= __vmwrite(GUEST_ES_SELECTOR, regs->es);
    error |= __vmwrite(GUEST_SS_SELECTOR, regs->ss);
    error |= __vmwrite(GUEST_DS_SELECTOR, regs->ds);
    error |= __vmwrite(GUEST_FS_SELECTOR, regs->fs);
    error |= __vmwrite(GUEST_GS_SELECTOR, regs->gs);

    /* Guest segment Limits */
    error |= __vmwrite(GUEST_CS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_ES_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_SS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_DS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_FS_LIMIT, GUEST_SEGMENT_LIMIT);
    error |= __vmwrite(GUEST_GS_LIMIT, GUEST_SEGMENT_LIMIT);

    error |= __vmwrite(GUEST_IDTR_LIMIT, host_env->idtr_limit);

    /* AR bytes */
    arbytes.bytes = 0;
    arbytes.fields.seg_type = 0x3;          /* type = 3 */
    arbytes.fields.s = 1;                   /* code or data, i.e. not system */
    arbytes.fields.dpl = 0;                 /* DPL = 3 */
    arbytes.fields.p = 1;                   /* segment present */
    arbytes.fields.default_ops_size = 1;    /* 32-bit */
    arbytes.fields.g = 1;   
    arbytes.fields.null_bit = 0;            /* not null */

    error |= __vmwrite(GUEST_ES_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_SS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_DS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_FS_AR_BYTES, arbytes.bytes);
    error |= __vmwrite(GUEST_GS_AR_BYTES, arbytes.bytes);

    arbytes.fields.seg_type = 0xb;          /* type = 0xb */
    error |= __vmwrite(GUEST_CS_AR_BYTES, arbytes.bytes);

    error |= __vmwrite(GUEST_GDTR_BASE, regs->edx);
    regs->edx = 0;
    error |= __vmwrite(GUEST_GDTR_LIMIT, regs->eax);
    regs->eax = 0;

    arbytes.fields.s = 0;                   /* not code or data segement */
    arbytes.fields.seg_type = 0x2;          /* LTD */
    arbytes.fields.default_ops_size = 0;    /* 16-bit */
    arbytes.fields.g = 0;   
    error |= __vmwrite(GUEST_LDTR_AR_BYTES, arbytes.bytes);

    arbytes.fields.seg_type = 0xb;          /* 32-bit TSS (busy) */
    error |= __vmwrite(GUEST_TR_AR_BYTES, arbytes.bytes);

    error |= __vmwrite(GUEST_CR0, host_env->cr0); /* same CR0 */

    /* Initally PG, PE are not set*/
    shadow_cr = host_env->cr0;
    shadow_cr &= ~X86_CR0_PG;
    error |= __vmwrite(CR0_READ_SHADOW, shadow_cr);
    /* CR3 is set in vmx_final_setup_guest */
#ifdef __x86_64__
    error |= __vmwrite(GUEST_CR4, host_env->cr4 & ~X86_CR4_PSE);
#else
    error |= __vmwrite(GUEST_CR4, host_env->cr4);
#endif
    shadow_cr = host_env->cr4;

#ifdef __x86_64__
    shadow_cr &= ~(X86_CR4_PGE | X86_CR4_VMXE | X86_CR4_PAE);
#else
    shadow_cr &= ~(X86_CR4_PGE | X86_CR4_VMXE);
#endif
    error |= __vmwrite(CR4_READ_SHADOW, shadow_cr);

    error |= __vmwrite(GUEST_ES_BASE, host_env->ds_base);
    error |= __vmwrite(GUEST_CS_BASE, host_env->cs_base);
    error |= __vmwrite(GUEST_SS_BASE, host_env->ds_base);
    error |= __vmwrite(GUEST_DS_BASE, host_env->ds_base);
    error |= __vmwrite(GUEST_FS_BASE, host_env->ds_base);
    error |= __vmwrite(GUEST_GS_BASE, host_env->ds_base);
    error |= __vmwrite(GUEST_IDTR_BASE, host_env->idtr_base);

    error |= __vmwrite(GUEST_RSP, regs->esp);
    error |= __vmwrite(GUEST_RIP, regs->eip);

    eflags = regs->eflags & ~VMCS_EFLAGS_RESERVED_0; /* clear 0s */
    eflags |= VMCS_EFLAGS_RESERVED_1; /* set 1s */

    error |= __vmwrite(GUEST_RFLAGS, eflags);

    error |= __vmwrite(GUEST_INTERRUPTIBILITY_INFO, 0);
    __asm__ __volatile__ ("mov %%dr7, %0\n" : "=r" (dr7));
    error |= __vmwrite(GUEST_DR7, dr7);
    error |= __vmwrite(VMCS_LINK_POINTER, 0xffffffff);
    error |= __vmwrite(VMCS_LINK_POINTER_HIGH, 0xffffffff);

    return error;
}

static inline int construct_vmcs_host(struct host_execution_env *host_env)
{
    int error = 0;
    unsigned long crn;

    /* Host Selectors */
    host_env->ds_selector = __HYPERVISOR_DS;
    error |= __vmwrite(HOST_ES_SELECTOR, host_env->ds_selector);
    error |= __vmwrite(HOST_SS_SELECTOR, host_env->ds_selector);
    error |= __vmwrite(HOST_DS_SELECTOR, host_env->ds_selector);
#if defined (__i386__)
    error |= __vmwrite(HOST_FS_SELECTOR, host_env->ds_selector);
    error |= __vmwrite(HOST_GS_SELECTOR, host_env->ds_selector);
    error |= __vmwrite(HOST_FS_BASE, host_env->ds_base); 
    error |= __vmwrite(HOST_GS_BASE, host_env->ds_base); 

#else
    rdmsrl(MSR_FS_BASE, host_env->fs_base); 
    rdmsrl(MSR_GS_BASE, host_env->gs_base); 
    error |= __vmwrite(HOST_FS_BASE, host_env->fs_base); 
    error |= __vmwrite(HOST_GS_BASE, host_env->gs_base); 

#endif
    host_env->cs_selector = __HYPERVISOR_CS;
    error |= __vmwrite(HOST_CS_SELECTOR, host_env->cs_selector);

    host_env->ds_base = 0;
    host_env->cs_base = 0;

    __asm__ __volatile__ ("mov %%cr0,%0" : "=r" (crn) : );
    host_env->cr0 = crn;
    error |= __vmwrite(HOST_CR0, crn); /* same CR0 */

    /* CR3 is set in vmx_final_setup_hostos */
    __asm__ __volatile__ ("mov %%cr4,%0" : "=r" (crn) : ); 
    host_env->cr4 = crn;
    error |= __vmwrite(HOST_CR4, crn);

    error |= __vmwrite(HOST_RIP, (unsigned long) vmx_asm_vmexit_handler);
#ifdef __x86_64__ 
    /* TBD: support cr8 for 64-bit guest */ 
    __vmwrite(VIRTUAL_APIC_PAGE_ADDR, 0); 
    __vmwrite(TPR_THRESHOLD, 0); 
    __vmwrite(SECONDARY_VM_EXEC_CONTROL, 0); 
#endif 

    return error;
}

/*
 * Need to extend to support full virtualization.
 * The variable use_host_env indicates if the new VMCS needs to use
 * the same setups as the host has (xenolinux).
 */

int construct_vmcs(struct arch_vmx_struct *arch_vmx,
                   struct cpu_user_regs *regs,
                   struct vcpu_guest_context *ctxt,
                   int use_host_env)
{
    int error;
    u64 vmcs_phys_ptr;

    struct host_execution_env host_env;

    if (use_host_env != VMCS_USE_HOST_ENV)
        return -EINVAL;

    memset(&host_env, 0, sizeof(struct host_execution_env));

    vmcs_phys_ptr = (u64) virt_to_phys(arch_vmx->vmcs);

    if ((error = __vmpclear (vmcs_phys_ptr))) {
        printk("construct_vmcs: VMCLEAR failed\n");
        return -EINVAL;         
    }
    if ((error = load_vmcs(arch_vmx, vmcs_phys_ptr))) {
        printk("construct_vmcs: load_vmcs failed: VMCS = %lx\n",
               (unsigned long) vmcs_phys_ptr);
        return -EINVAL; 
    }
    if ((error = construct_vmcs_controls())) {
        printk("construct_vmcs: construct_vmcs_controls failed\n");
        return -EINVAL;         
    }
    /* host selectors */
    if ((error = construct_vmcs_host(&host_env))) {
        printk("construct_vmcs: construct_vmcs_host failed\n");
        return -EINVAL;         
    }
    /* guest selectors */
    if ((error = construct_init_vmcs_guest(regs, ctxt, &host_env))) {
        printk("construct_vmcs: construct_vmcs_guest failed\n");
        return -EINVAL;         
    }       

    if ((error |= __vmwrite(EXCEPTION_BITMAP, 
                            MONITOR_DEFAULT_EXCEPTION_BITMAP))) {
        printk("construct_vmcs: setting Exception bitmap failed\n");
        return -EINVAL;         
    }

    if (regs->eflags & EF_TF)
        __vm_set_bit(EXCEPTION_BITMAP, EXCEPTION_BITMAP_DB);
    else 
        __vm_clear_bit(EXCEPTION_BITMAP, EXCEPTION_BITMAP_DB);

    return 0;
}

/*
 * modify guest eflags and execption bitmap for gdb
 */
int modify_vmcs(struct arch_vmx_struct *arch_vmx,
                struct cpu_user_regs *regs)
{
    int error;
    u64 vmcs_phys_ptr, old, old_phys_ptr;
    vmcs_phys_ptr = (u64) virt_to_phys(arch_vmx->vmcs);

    old_phys_ptr = virt_to_phys(&old);
    __vmptrst(old_phys_ptr);
    if ((error = load_vmcs(arch_vmx, vmcs_phys_ptr))) {
        printk("modify_vmcs: load_vmcs failed: VMCS = %lx\n",
                (unsigned long) vmcs_phys_ptr);
        return -EINVAL; 
    }
    load_cpu_user_regs(regs);

    __vmptrld(old_phys_ptr);

    return 0;
}

int load_vmcs(struct arch_vmx_struct *arch_vmx, u64 phys_ptr) 
{
    int error;

    if ((error = __vmptrld(phys_ptr))) {
        clear_bit(ARCH_VMX_VMCS_LOADED, &arch_vmx->flags); 
        return error;
    }
    set_bit(ARCH_VMX_VMCS_LOADED, &arch_vmx->flags); 
    return 0;
}

int store_vmcs(struct arch_vmx_struct *arch_vmx, u64 phys_ptr) 
{
    /* take the current VMCS */
    __vmptrst(phys_ptr);
    clear_bit(ARCH_VMX_VMCS_LOADED, &arch_vmx->flags); 
    return 0;
}

void vm_launch_fail(unsigned long eflags)
{
    __vmx_bug(guest_cpu_user_regs());
}

void vm_resume_fail(unsigned long eflags)
{
    __vmx_bug(guest_cpu_user_regs());
}

#endif /* CONFIG_VMX */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
