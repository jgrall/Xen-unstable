/*
 * vmx.c: handling VMX architecture-related VM exits
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
#include <xen/lib.h>
#include <xen/trace.h>
#include <xen/sched.h>
#include <asm/current.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/shadow.h>
#include <asm/regs.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/types.h>
#include <asm/msr.h>
#include <asm/spinlock.h>
#include <asm/vmx.h>
#include <asm/vmx_vmcs.h>
#include <asm/vmx_intercept.h>
#include <asm/shadow.h>
#include <public/io/ioreq.h>

#ifdef CONFIG_VMX

int vmcs_size;
unsigned int opt_vmx_debug_level = 0;

extern long evtchn_send(int lport);
extern long do_block(void);
void do_nmi(struct xen_regs *, unsigned long);

int start_vmx()
{
    struct vmcs_struct *vmcs;
    unsigned long ecx;
    u64 phys_vmcs;      /* debugging */

    vmcs_size = VMCS_SIZE;
    /*
     * Xen does not fill x86_capability words except 0.
     */
    ecx = cpuid_ecx(1);
    boot_cpu_data.x86_capability[4] = ecx;

    if (!(test_bit(X86_FEATURE_VMXE, &boot_cpu_data.x86_capability)))
        return 0;

    set_in_cr4(X86_CR4_VMXE);   /* Enable VMXE */

    if (!(vmcs = alloc_vmcs())) {
        printk("Failed to allocate VMCS\n");    
        return 0;
    }

    phys_vmcs = (u64) virt_to_phys(vmcs);

    if (!(__vmxon(phys_vmcs))) {
        printk("VMXON is done\n");
    }

    return 1;
}

void stop_vmx()
{
    if (read_cr4() & X86_CR4_VMXE)
        __vmxoff();
}

/*
 * Not all cases recevie valid value in the VM-exit instruction length field.
 */
#define __get_instruction_length(len) \
    __vmread(INSTRUCTION_LEN, &(len)); \
     if ((len) < 1 || (len) > 15) \
        __vmx_bug(&regs);

static void inline __update_guest_eip(unsigned long inst_len) 
{
    unsigned long current_eip;

    __vmread(GUEST_EIP, &current_eip);
    __vmwrite(GUEST_EIP, current_eip + inst_len);
}


#include <asm/domain_page.h>

static int vmx_do_page_fault(unsigned long va, struct xen_regs *regs) 
{
    unsigned long eip;
    unsigned long gpte, gpa;
    int result;

#if VMX_DEBUG
    {
        __vmread(GUEST_EIP, &eip);
        VMX_DBG_LOG(DBG_LEVEL_VMMU, 
                "vmx_do_page_fault = 0x%lx, eip = %lx, error_code = %lx",
                va, eip, regs->error_code);
    }
#endif

    /*
     * If vpagetable is zero, then we are still emulating 1:1 page tables,
     * and we should have never gotten here.
     */
    if ( !current->arch.guest_vtable )
    {
        printk("vmx_do_page_fault while still running on 1:1 page table\n");
        return 0;
    }

    gpte = gva_to_gpte(va);
    if (!(gpte & _PAGE_PRESENT) )
            return 0;
    gpa = (gpte & PAGE_MASK) + (va & ~PAGE_MASK);

    /* Use 1:1 page table to identify MMIO address space */
    if (mmio_space(gpa))
        handle_mmio(va, gpa);

    result = shadow_fault(va, regs);

#if 0
    if ( !result )
    {
        __vmread(GUEST_EIP, &eip);
        printk("vmx pgfault to guest va=%p eip=%p\n", va, eip);
    }
#endif

    return result;
}

static void vmx_do_general_protection_fault(struct xen_regs *regs) 
{
    unsigned long eip, error_code;
    unsigned long intr_fields;

    __vmread(GUEST_EIP, &eip);
    __vmread(VM_EXIT_INTR_ERROR_CODE, &error_code);

    VMX_DBG_LOG(DBG_LEVEL_1,
            "vmx_general_protection_fault: eip = %lx, erro_code = %lx",
            eip, error_code);

    VMX_DBG_LOG(DBG_LEVEL_1,
            "eax=%lx, ebx=%lx, ecx=%lx, edx=%lx, esi=%lx, edi=%lx",
            regs->eax, regs->ebx, regs->ecx, regs->edx, regs->esi, regs->edi);

    /* Reflect it back into the guest */
    intr_fields = (INTR_INFO_VALID_MASK | 
		   INTR_TYPE_EXCEPTION |
		   INTR_INFO_DELIEVER_CODE_MASK |
		   TRAP_gp_fault);
    __vmwrite(VM_ENTRY_INTR_INFO_FIELD, intr_fields);
    __vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, error_code);
}

static void vmx_vmexit_do_cpuid(unsigned long input, struct xen_regs *regs) 
{
    int eax, ebx, ecx, edx;
    unsigned long eip;

    __vmread(GUEST_EIP, &eip);

    VMX_DBG_LOG(DBG_LEVEL_1, 
                "do_cpuid: (eax) %lx, (ebx) %lx, (ecx) %lx, (edx) %lx,"
                " (esi) %lx, (edi) %lx",
                regs->eax, regs->ebx, regs->ecx, regs->edx,
                regs->esi, regs->edi);

    cpuid(input, &eax, &ebx, &ecx, &edx);

    if (input == 1) {
        clear_bit(X86_FEATURE_PSE, &edx);
        clear_bit(X86_FEATURE_PAE, &edx);
        clear_bit(X86_FEATURE_PSE36, &edx);
    }

    regs->eax = (unsigned long) eax;
    regs->ebx = (unsigned long) ebx;
    regs->ecx = (unsigned long) ecx;
    regs->edx = (unsigned long) edx;

    VMX_DBG_LOG(DBG_LEVEL_1, 
            "vmx_vmexit_do_cpuid: eip: %lx, input: %lx, out:eax=%x, ebx=%x, ecx=%x, edx=%x",
            eip, input, eax, ebx, ecx, edx);

}

#define CASE_GET_REG_P(REG, reg)    \
    case REG_ ## REG: reg_p = (unsigned long *)&(regs->reg); break

static void vmx_dr_access (unsigned long exit_qualification, struct xen_regs *regs)
{
    unsigned int reg;
    unsigned long *reg_p = 0;
    struct exec_domain *ed = current;
    unsigned long eip;

    __vmread(GUEST_EIP, &eip);

    reg = exit_qualification & DEBUG_REG_ACCESS_NUM;

    VMX_DBG_LOG(DBG_LEVEL_1, 
                "vmx_dr_access : eip=%lx, reg=%d, exit_qualification = %lx",
                eip, reg, exit_qualification);

    switch(exit_qualification & DEBUG_REG_ACCESS_REG) {
        CASE_GET_REG_P(EAX, eax);
        CASE_GET_REG_P(ECX, ecx);
        CASE_GET_REG_P(EDX, edx);
        CASE_GET_REG_P(EBX, ebx);
        CASE_GET_REG_P(EBP, ebp);
        CASE_GET_REG_P(ESI, esi);
        CASE_GET_REG_P(EDI, edi);
    case REG_ESP:
        break;  
    default:
        __vmx_bug(regs);
    }
        
    switch (exit_qualification & DEBUG_REG_ACCESS_TYPE) {
    case TYPE_MOV_TO_DR: 
        /* don't need to check the range */
        if (reg != REG_ESP)
            ed->arch.debugreg[reg] = *reg_p; 
        else {
            unsigned long value;
            __vmread(GUEST_ESP, &value);
            ed->arch.debugreg[reg] = value;
        }
        break;
    case TYPE_MOV_FROM_DR:
        if (reg != REG_ESP)
            *reg_p = ed->arch.debugreg[reg];
        else {
            __vmwrite(GUEST_ESP, ed->arch.debugreg[reg]);
        }
        break;
    }
}

/*
 * Invalidate the TLB for va. Invalidate the shadow page corresponding
 * the address va.
 */
static void vmx_vmexit_do_invlpg(unsigned long va) 
{
    unsigned long eip;
    struct exec_domain *ed = current;
    unsigned int index;

    __vmread(GUEST_EIP, &eip);

    VMX_DBG_LOG(DBG_LEVEL_VMMU, "vmx_vmexit_do_invlpg:eip=%p, va=%p",
            eip, va);

    /*
     * We do the safest things first, then try to update the shadow
     * copying from guest
     */
    shadow_invlpg(ed, va);
    index = l2_table_offset(va);
    ed->arch.hl2_vtable[index] = 
        mk_l2_pgentry(0); /* invalidate pgd cache */
}

static void vmx_io_instruction(struct xen_regs *regs, 
                   unsigned long exit_qualification, unsigned long inst_len) 
{
    struct exec_domain *d = current;
    vcpu_iodata_t *vio;
    ioreq_t *p;
    unsigned long addr;
    unsigned long eip;

    __vmread(GUEST_EIP, &eip);

    VMX_DBG_LOG(DBG_LEVEL_1, 
            "vmx_io_instruction: eip=%p, exit_qualification = %lx",
            eip, exit_qualification);

    if (test_bit(6, &exit_qualification))
        addr = (exit_qualification >> 16) & (0xffff);
    else
        addr = regs->edx & 0xffff;

    if (addr == 0x80) {
        __update_guest_eip(inst_len);
        return;
    }

    vio = (vcpu_iodata_t *) d->arch.arch_vmx.vmx_platform.shared_page_va;
    if (vio == 0) {
        VMX_DBG_LOG(DBG_LEVEL_1, "bad shared page: %lx", (unsigned long) vio);
        domain_crash(); 
    }
    p = &vio->vp_ioreq;
    p->dir = test_bit(3, &exit_qualification);  

    p->pdata_valid = 0;
    p->count = 1;
    p->size = (exit_qualification & 7) + 1;

    if (test_bit(4, &exit_qualification)) {
        unsigned long eflags;

        __vmread(GUEST_EFLAGS, &eflags);
        p->df = (eflags & X86_EFLAGS_DF) ? 1 : 0;
        p->pdata_valid = 1;
        p->u.pdata = (void *) ((p->dir == IOREQ_WRITE) ?
            regs->esi
            : regs->edi);
        p->u.pdata = (void *) gva_to_gpa(p->u.data);
        if (test_bit(5, &exit_qualification))
            p->count = regs->ecx;
        if ((p->u.data & PAGE_MASK) != 
            ((p->u.data + p->count * p->size - 1) & PAGE_MASK)) {
            printk("stringio crosses page boundary!\n");
            if (p->u.data & (p->size - 1)) {
                printk("Not aligned I/O!\n");
                domain_crash();     
            }
            p->count = (PAGE_SIZE - (p->u.data & ~PAGE_MASK)) / p->size;
        } else {
            __update_guest_eip(inst_len);
        }
    } else if (p->dir == IOREQ_WRITE) {
        p->u.data = regs->eax;
        __update_guest_eip(inst_len);
    } else
        __update_guest_eip(inst_len);

    p->addr = addr;
    p->port_mm = 0;

    /* Check if the packet needs to be intercepted */
    if (vmx_io_intercept(p)) {
	/* no blocking & no evtchn notification */
        return;
    } 

    set_bit(ARCH_VMX_IO_WAIT, &d->arch.arch_vmx.flags);
    p->state = STATE_IOREQ_READY;
    evtchn_send(IOPACKET_PORT);
    do_block();
}

#define CASE_GET_REG(REG, reg)  \
    case REG_ ## REG: value = regs->reg; break

/*
 * Write to control registers
 */
static void mov_to_cr(int gp, int cr, struct xen_regs *regs)
{
    unsigned long value;
    unsigned long old_cr;
    struct exec_domain *d = current;

    switch (gp) {
        CASE_GET_REG(EAX, eax);
        CASE_GET_REG(ECX, ecx);
        CASE_GET_REG(EDX, edx);
        CASE_GET_REG(EBX, ebx);
        CASE_GET_REG(EBP, ebp);
        CASE_GET_REG(ESI, esi);
        CASE_GET_REG(EDI, edi);
    case REG_ESP:
        __vmread(GUEST_ESP, &value);
        break;
    default:
        printk("invalid gp: %d\n", gp);
        __vmx_bug(regs);
    }
    
    VMX_DBG_LOG(DBG_LEVEL_1, "mov_to_cr: CR%d, value = %lx,", cr, value);
    VMX_DBG_LOG(DBG_LEVEL_1, "current = %lx,", (unsigned long) current);

    switch(cr) {
    case 0: 
    {
        unsigned long old_base_mfn = 0, mfn;

        /* 
         * CR0:
         * We don't want to lose PE and PG.
         */
        __vmwrite(GUEST_CR0, (value | X86_CR0_PE | X86_CR0_PG));
        __vmwrite(CR0_READ_SHADOW, value);

        if (value & (X86_CR0_PE | X86_CR0_PG) &&
            !test_bit(VMX_CPU_STATE_PG_ENABLED, &d->arch.arch_vmx.cpu_state)) {
            /*
             * Enable paging
             */
            set_bit(VMX_CPU_STATE_PG_ENABLED, &d->arch.arch_vmx.cpu_state);
            /*
             * The guest CR3 must be pointing to the guest physical.
             */
            if (!(mfn = phys_to_machine_mapping(
                      d->arch.arch_vmx.cpu_cr3 >> PAGE_SHIFT))) 
            {
                VMX_DBG_LOG(DBG_LEVEL_VMMU, "Invalid CR3 value = %lx", 
                        d->arch.arch_vmx.cpu_cr3);
                domain_crash(); /* need to take a clean path */
            }
            old_base_mfn = pagetable_val(d->arch.guest_table) >> PAGE_SHIFT;

            /* We know that none of the previous 1:1 shadow pages are
             * going to be used again, so might as well flush them.
             * XXXX wait until the last VCPU boots before doing the flush !!
             */
            shadow_lock(d->domain);
            free_shadow_state(d->domain); // XXX SMP
            shadow_unlock(d->domain);

            /*
             * Now arch.guest_table points to machine physical.
             */
            d->arch.guest_table = mk_pagetable(mfn << PAGE_SHIFT);
            update_pagetables(d);

            VMX_DBG_LOG(DBG_LEVEL_VMMU, "New arch.guest_table = %lx", 
                    (unsigned long) (mfn << PAGE_SHIFT));

            __vmwrite(GUEST_CR3, pagetable_val(d->arch.shadow_table));
            /* 
             * arch->shadow_table should hold the next CR3 for shadow
             */
            VMX_DBG_LOG(DBG_LEVEL_VMMU, "Update CR3 value = %lx, mfn = %lx", 
                    d->arch.arch_vmx.cpu_cr3, mfn);
            /* undo the get_page done in the para virt case */
            put_page_and_type(&frame_table[old_base_mfn]);

        }
        break;
    }
    case 3: 
    {
        unsigned long mfn;

        /*
         * If paging is not enabled yet, simply copy the value to CR3.
         */
        if (!test_bit(VMX_CPU_STATE_PG_ENABLED, &d->arch.arch_vmx.cpu_state)) {
            d->arch.arch_vmx.cpu_cr3 = value;
            break;
        }
        
        hl2_table_invalidate(d);
        /*
         * We make a new one if the shadow does not exist.
         */
        if (value == d->arch.arch_vmx.cpu_cr3) {
            /* 
             * This is simple TLB flush, implying the guest has 
             * removed some translation or changed page attributes.
             * We simply invalidate the shadow.
             */
            mfn = phys_to_machine_mapping(value >> PAGE_SHIFT);
            if ((mfn << PAGE_SHIFT) != pagetable_val(d->arch.guest_table))
                __vmx_bug(regs);
            vmx_shadow_clear_state(d->domain);
            shadow_invalidate(d);
        } else {
            /*
             * If different, make a shadow. Check if the PDBR is valid
             * first.
             */
            VMX_DBG_LOG(DBG_LEVEL_VMMU, "CR3 value = %lx", value);
            if ((value >> PAGE_SHIFT) > d->domain->max_pages)
            {
                VMX_DBG_LOG(DBG_LEVEL_VMMU, 
                        "Invalid CR3 value=%lx", value);
                domain_crash(); /* need to take a clean path */
            }
            mfn = phys_to_machine_mapping(value >> PAGE_SHIFT);
            vmx_shadow_clear_state(d->domain);
            d->arch.guest_table  = mk_pagetable(mfn << PAGE_SHIFT);
            update_pagetables(d); 
            /* 
             * arch.shadow_table should now hold the next CR3 for shadow
             */
            d->arch.arch_vmx.cpu_cr3 = value;
            VMX_DBG_LOG(DBG_LEVEL_VMMU, "Update CR3 value = %lx",
                    value);
            __vmwrite(GUEST_CR3, pagetable_val(d->arch.shadow_table));
        }
        break;
    }
    case 4:         
        /* CR4 */
        if (value & X86_CR4_PAE)
            __vmx_bug(regs);    /* not implemented */
        __vmread(CR4_READ_SHADOW, &old_cr);
        
        __vmwrite(GUEST_CR4, (value | X86_CR4_VMXE));
        __vmwrite(CR4_READ_SHADOW, value);

        /*
         * Writing to CR4 to modify the PSE, PGE, or PAE flag invalidates
         * all TLB entries except global entries.
         */
        if ((old_cr ^ value) & (X86_CR4_PSE | X86_CR4_PGE | X86_CR4_PAE)) {
            vmx_shadow_clear_state(d->domain);
            shadow_invalidate(d);
            hl2_table_invalidate(d);
        }
        break;
    default:
        printk("invalid cr: %d\n", gp);
        __vmx_bug(regs);
    }
}   

#define CASE_SET_REG(REG, reg)      \
    case REG_ ## REG:       \
    regs->reg = value;      \
    break

/*
 * Read from control registers. CR0 and CR4 are read from the shadow.
 */
static void mov_from_cr(int cr, int gp, struct xen_regs *regs)
{
    unsigned long value;
    struct exec_domain *d = current;

    if (cr != 3)
        __vmx_bug(regs);

    value = (unsigned long) d->arch.arch_vmx.cpu_cr3;
    ASSERT(value);

    switch (gp) {
        CASE_SET_REG(EAX, eax);
        CASE_SET_REG(ECX, ecx);
        CASE_SET_REG(EDX, edx);
        CASE_SET_REG(EBX, ebx);
        CASE_SET_REG(EBP, ebp);
        CASE_SET_REG(ESI, esi);
        CASE_SET_REG(EDI, edi);
    case REG_ESP:
        __vmwrite(GUEST_ESP, value);
        regs->esp = value;
        break;
    default:
        printk("invalid gp: %d\n", gp);
        __vmx_bug(regs);
    }

    VMX_DBG_LOG(DBG_LEVEL_VMMU, "mov_from_cr: CR%d, value = %lx,", cr, value);
}

static void vmx_cr_access (unsigned long exit_qualification, struct xen_regs *regs)
{
    unsigned int gp, cr;
    unsigned long value;

    switch (exit_qualification & CONTROL_REG_ACCESS_TYPE) {
    case TYPE_MOV_TO_CR:
        gp = exit_qualification & CONTROL_REG_ACCESS_REG;
        cr = exit_qualification & CONTROL_REG_ACCESS_NUM;
        mov_to_cr(gp, cr, regs);
        break;
    case TYPE_MOV_FROM_CR:
        gp = exit_qualification & CONTROL_REG_ACCESS_REG;
        cr = exit_qualification & CONTROL_REG_ACCESS_NUM;
        mov_from_cr(cr, gp, regs);
        break;
    case TYPE_CLTS:
        __vmread(GUEST_CR0, &value);
        value &= ~X86_CR0_TS; /* clear TS */
        __vmwrite(GUEST_CR0, value);

        __vmread(CR0_READ_SHADOW, &value);
        value &= ~X86_CR0_TS; /* clear TS */
        __vmwrite(CR0_READ_SHADOW, value);
        break;
    default:
        __vmx_bug(regs);
        break;
    }
}

static inline void vmx_do_msr_read(struct xen_regs *regs)
{
    VMX_DBG_LOG(DBG_LEVEL_1, "vmx_do_msr_read: ecx=%lx, eax=%lx, edx=%lx",
            regs->ecx, regs->eax, regs->edx);

    rdmsr(regs->ecx, regs->eax, regs->edx);

    VMX_DBG_LOG(DBG_LEVEL_1, "vmx_do_msr_read returns: "
                "ecx=%lx, eax=%lx, edx=%lx",
                regs->ecx, regs->eax, regs->edx);
}

/*
 * Need to use this exit to rescheule
 */
static inline void vmx_vmexit_do_hlt()
{
#if VMX_DEBUG
    unsigned long eip;
    __vmread(GUEST_EIP, &eip);
#endif
    VMX_DBG_LOG(DBG_LEVEL_1, "vmx_vmexit_do_hlt:eip=%p", eip);
    __enter_scheduler();
}

static inline void vmx_vmexit_do_mwait()
{
#if VMX_DEBUG
    unsigned long eip;
    __vmread(GUEST_EIP, &eip);
#endif
    VMX_DBG_LOG(DBG_LEVEL_1, "vmx_vmexit_do_mwait:eip=%p", eip);
    __enter_scheduler();
}

#define BUF_SIZ     256
#define MAX_LINE    80
char print_buf[BUF_SIZ];
static int index;

static void vmx_print_line(const char c, struct exec_domain *d) 
{

    if (index == MAX_LINE || c == '\n') {
        if (index == MAX_LINE) {
            print_buf[index++] = c;
        }
        print_buf[index] = '\0';
        printk("(GUEST: %u) %s\n", d->domain->id, (char *) &print_buf);
        index = 0;
    }
    else
        print_buf[index++] = c;
}

void save_vmx_execution_context(execution_context_t *ctxt)
{
    __vmread(GUEST_SS_SELECTOR, &ctxt->ss);
    __vmread(GUEST_ESP, &ctxt->esp);
    __vmread(GUEST_EFLAGS, &ctxt->eflags);
    __vmread(GUEST_CS_SELECTOR, &ctxt->cs);
    __vmread(GUEST_EIP, &ctxt->eip);

    __vmread(GUEST_GS_SELECTOR, &ctxt->gs);
    __vmread(GUEST_FS_SELECTOR, &ctxt->fs);
    __vmread(GUEST_ES_SELECTOR, &ctxt->es);
    __vmread(GUEST_DS_SELECTOR, &ctxt->ds);
}

#ifdef XEN_DEBUGGER
void save_xen_regs(struct xen_regs *regs)
{
    __vmread(GUEST_SS_SELECTOR, &regs->xss);
    __vmread(GUEST_ESP, &regs->esp);
    __vmread(GUEST_EFLAGS, &regs->eflags);
    __vmread(GUEST_CS_SELECTOR, &regs->xcs);
    __vmread(GUEST_EIP, &regs->eip);

    __vmread(GUEST_GS_SELECTOR, &regs->xgs);
    __vmread(GUEST_FS_SELECTOR, &regs->xfs);
    __vmread(GUEST_ES_SELECTOR, &regs->xes);
    __vmread(GUEST_DS_SELECTOR, &regs->xds);
}

void restore_xen_regs(struct xen_regs *regs)
{
    __vmwrite(GUEST_SS_SELECTOR, regs->xss);
    __vmwrite(GUEST_ESP, regs->esp);
    __vmwrite(GUEST_EFLAGS, regs->eflags);
    __vmwrite(GUEST_CS_SELECTOR, regs->xcs);
    __vmwrite(GUEST_EIP, regs->eip);

    __vmwrite(GUEST_GS_SELECTOR, regs->xgs);
    __vmwrite(GUEST_FS_SELECTOR, regs->xfs);
    __vmwrite(GUEST_ES_SELECTOR, regs->xes);
    __vmwrite(GUEST_DS_SELECTOR, regs->xds);
}
#endif

asmlinkage void vmx_vmexit_handler(struct xen_regs regs)
{
    unsigned int exit_reason, idtv_info_field;
    unsigned long exit_qualification, eip, inst_len = 0;
    struct exec_domain *ed = current;
    int error;

    if ((error = __vmread(VM_EXIT_REASON, &exit_reason)))
        __vmx_bug(&regs);
    
    perfc_incra(vmexits, exit_reason);

    __vmread(IDT_VECTORING_INFO_FIELD, &idtv_info_field);
    if (idtv_info_field & INTR_INFO_VALID_MASK) {
        __vmwrite(VM_ENTRY_INTR_INFO_FIELD, idtv_info_field);
        if ((idtv_info_field & 0xff) == 14) {
            unsigned long error_code;

            __vmread(VM_EXIT_INTR_ERROR_CODE, &error_code);
            printk("#PG error code: %lx\n", error_code);
        }
        VMX_DBG_LOG(DBG_LEVEL_1, "idtv_info_field=%x",
                idtv_info_field);
    }

    /* don't bother H/W interrutps */
    if (exit_reason != EXIT_REASON_EXTERNAL_INTERRUPT &&
        exit_reason != EXIT_REASON_VMCALL &&
        exit_reason != EXIT_REASON_IO_INSTRUCTION)
        VMX_DBG_LOG(DBG_LEVEL_0, "exit reason = %x", exit_reason);

    if (exit_reason & VMX_EXIT_REASONS_FAILED_VMENTRY) {
        domain_crash();         
        return;
    }

    __vmread(GUEST_EIP, &eip);
    TRACE_3D(TRC_VMX_VMEXIT, ed->domain->id, eip, exit_reason);

    switch (exit_reason) {
    case EXIT_REASON_EXCEPTION_NMI:
    {
        /*
         * We don't set the software-interrupt exiting (INT n). 
         * (1) We can get an exception (e.g. #PG) in the guest, or
         * (2) NMI
         */
        int error;
        unsigned int vector;
        unsigned long va;

        if ((error = __vmread(VM_EXIT_INTR_INFO, &vector))
            && !(vector & INTR_INFO_VALID_MASK))
            __vmx_bug(&regs);
        vector &= 0xff;

        perfc_incra(cause_vector, vector);

        TRACE_3D(TRC_VMX_VECTOR, ed->domain->id, eip, vector);
        switch (vector) {
#ifdef XEN_DEBUGGER
        case TRAP_debug:
        {
            save_xen_regs(&regs);
            pdb_handle_exception(1, &regs, 1);
            restore_xen_regs(&regs);
            break;
        }
        case TRAP_int3:
        {
            save_xen_regs(&regs);
            pdb_handle_exception(3, &regs, 1);
            restore_xen_regs(&regs);
            break;
        }
#endif
        case TRAP_gp_fault:
        {
            vmx_do_general_protection_fault(&regs);
            break;  
        }
        case TRAP_page_fault:
        {
            __vmread(EXIT_QUALIFICATION, &va);
            __vmread(VM_EXIT_INTR_ERROR_CODE, &regs.error_code);
            VMX_DBG_LOG(DBG_LEVEL_VMMU, 
                    "eax=%lx, ebx=%lx, ecx=%lx, edx=%lx, esi=%lx, edi=%lx",
                        regs.eax, regs.ebx, regs.ecx, regs.edx, regs.esi,
                        regs.edi);
            ed->arch.arch_vmx.vmx_platform.mpci.inst_decoder_regs = &regs;

            if (!(error = vmx_do_page_fault(va, &regs))) {
                /*
                 * Inject #PG using Interruption-Information Fields
                 */
                unsigned long intr_fields;

                intr_fields = (INTR_INFO_VALID_MASK | 
                           INTR_TYPE_EXCEPTION |
                           INTR_INFO_DELIEVER_CODE_MASK |
                           TRAP_page_fault);
                __vmwrite(VM_ENTRY_INTR_INFO_FIELD, intr_fields);
                __vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, regs.error_code);
                ed->arch.arch_vmx.cpu_cr2 = va;
                TRACE_3D(TRC_VMX_INT, ed->domain->id, TRAP_page_fault, va);
            }
            break;
        }
        case TRAP_nmi:
            do_nmi(&regs, 0);
            break;
        default:
            printk("unexpected VMexit for exception vector 0x%x\n", vector);
            //__vmx_bug(&regs);
            break;
        }
        break;
    }
    case EXIT_REASON_EXTERNAL_INTERRUPT: 
    {
        extern int vector_irq[];
        extern asmlinkage void do_IRQ(struct xen_regs *);
        extern void smp_apic_timer_interrupt(struct xen_regs *);
        extern void timer_interrupt(int, void *, struct xen_regs *);
        unsigned int    vector;

        if ((error = __vmread(VM_EXIT_INTR_INFO, &vector))
            && !(vector & INTR_INFO_VALID_MASK))
            __vmx_bug(&regs);

        vector &= 0xff;
        local_irq_disable();

        if (vector == LOCAL_TIMER_VECTOR) {
            smp_apic_timer_interrupt(&regs);
        } else {
            regs.entry_vector = (vector == FIRST_DEVICE_VECTOR?
                     0 : vector_irq[vector]);
            do_IRQ(&regs);
        }
        break;
    }
    case EXIT_REASON_PENDING_INTERRUPT:
        __vmwrite(CPU_BASED_VM_EXEC_CONTROL, 
              MONITOR_CPU_BASED_EXEC_CONTROLS);
        vmx_intr_assist(ed);
        break;
    case EXIT_REASON_TASK_SWITCH:
        __vmx_bug(&regs);
        break;
    case EXIT_REASON_CPUID:
        __get_instruction_length(inst_len);
        vmx_vmexit_do_cpuid(regs.eax, &regs);
        __update_guest_eip(inst_len);
        break;
    case EXIT_REASON_HLT:
        __get_instruction_length(inst_len);
        __update_guest_eip(inst_len);
        vmx_vmexit_do_hlt();
        break;
    case EXIT_REASON_INVLPG:
    {
        unsigned long   va;

        __vmread(EXIT_QUALIFICATION, &va);
        vmx_vmexit_do_invlpg(va);
        __get_instruction_length(inst_len);
        __update_guest_eip(inst_len);
        break;
    }
    case EXIT_REASON_VMCALL:
        __get_instruction_length(inst_len);
        __vmread(GUEST_EIP, &eip);
        __vmread(EXIT_QUALIFICATION, &exit_qualification);

        vmx_print_line(regs.eax, ed); /* provides the current domain */
        __update_guest_eip(inst_len);
        break;
    case EXIT_REASON_CR_ACCESS:
    {
        __vmread(GUEST_EIP, &eip);
        __get_instruction_length(inst_len);
        __vmread(EXIT_QUALIFICATION, &exit_qualification);

        VMX_DBG_LOG(DBG_LEVEL_1, "eip = %lx, inst_len =%lx, exit_qualification = %lx", 
                eip, inst_len, exit_qualification);
        vmx_cr_access(exit_qualification, &regs);
        __update_guest_eip(inst_len);
        break;
    }
    case EXIT_REASON_DR_ACCESS:
        __vmread(EXIT_QUALIFICATION, &exit_qualification);  
        vmx_dr_access(exit_qualification, &regs);
        __get_instruction_length(inst_len);
        __update_guest_eip(inst_len);
        break;
    case EXIT_REASON_IO_INSTRUCTION:
        __vmread(EXIT_QUALIFICATION, &exit_qualification);
        __get_instruction_length(inst_len);
        vmx_io_instruction(&regs, exit_qualification, inst_len);
        break;
    case EXIT_REASON_MSR_READ:
        __get_instruction_length(inst_len);
        vmx_do_msr_read(&regs);
        __update_guest_eip(inst_len);
        break;
    case EXIT_REASON_MSR_WRITE:
        __vmread(GUEST_EIP, &eip);
        VMX_DBG_LOG(DBG_LEVEL_1, "MSR_WRITE: eip=%p, eax=%p, edx=%p",
                eip, regs.eax, regs.edx);
        /* just ignore this point */
        __get_instruction_length(inst_len);
        __update_guest_eip(inst_len);
        break;
    case EXIT_REASON_MWAIT_INSTRUCTION:
        __get_instruction_length(inst_len);
        __update_guest_eip(inst_len);
        vmx_vmexit_do_mwait();
        break;
    default:
        __vmx_bug(&regs);       /* should not happen */
    }

    vmx_intr_assist(ed);
    return;
}

asmlinkage void load_cr2(void)
{
    struct exec_domain *d = current;

    local_irq_disable();        
    asm volatile("movl %0,%%cr2": :"r" (d->arch.arch_vmx.cpu_cr2));
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
