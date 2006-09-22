/*
 * io.c: handling I/O, interrupts related VMX entry/exit
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
#include <xen/trace.h>
#include <xen/event.h>

#include <asm/current.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/vmx/vmcs.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vlapic.h>
#include <public/hvm/ioreq.h>


static inline void
enable_irq_window(struct vcpu *v)
{
    u32  *cpu_exec_control = &v->arch.hvm_vcpu.u.vmx.exec_control;
    
    if (!(*cpu_exec_control & CPU_BASED_VIRTUAL_INTR_PENDING)) {
        *cpu_exec_control |= CPU_BASED_VIRTUAL_INTR_PENDING;
        __vmwrite(CPU_BASED_VM_EXEC_CONTROL, *cpu_exec_control);
    }
}

static inline void
disable_irq_window(struct vcpu *v)
{
    u32  *cpu_exec_control = &v->arch.hvm_vcpu.u.vmx.exec_control;
    
    if ( *cpu_exec_control & CPU_BASED_VIRTUAL_INTR_PENDING ) {
        *cpu_exec_control &= ~CPU_BASED_VIRTUAL_INTR_PENDING;
        __vmwrite(CPU_BASED_VM_EXEC_CONTROL, *cpu_exec_control);
    }
}

static inline int is_interruptibility_state(void)
{
    int  interruptibility;
    __vmread(GUEST_INTERRUPTIBILITY_INFO, &interruptibility);
    return interruptibility;
}


asmlinkage void vmx_intr_assist(void)
{
    int intr_type = 0;
    int highest_vector;
    unsigned long eflags;
    struct vcpu *v = current;
    struct hvm_domain *plat=&v->domain->arch.hvm_domain;
    struct periodic_time *pt = &plat->pl_time.periodic_tm;
    struct hvm_virpic *pic= &plat->vpic;
    int callback_irq;
    unsigned int idtv_info_field;
    unsigned long inst_len;
    int    has_ext_irq;

    if ( v->vcpu_id == 0 )
        hvm_pic_assist(v);

    if ( (v->vcpu_id == 0) && pt->enabled && pt->pending_intr_nr ) {
        pic_set_irq(pic, pt->irq, 0);
        pic_set_irq(pic, pt->irq, 1);
    }

    callback_irq = v->domain->arch.hvm_domain.params[HVM_PARAM_CALLBACK_IRQ];
    if ( callback_irq != 0 &&
         local_events_need_delivery() ) {
        /*inject para-device call back irq*/
        v->vcpu_info->evtchn_upcall_mask = 1;
        pic_set_irq(pic, callback_irq, 0);
        pic_set_irq(pic, callback_irq, 1);
    }

    has_ext_irq = cpu_has_pending_irq(v);

    if (unlikely(v->arch.hvm_vmx.vector_injected)) {
        v->arch.hvm_vmx.vector_injected=0;
        if (unlikely(has_ext_irq)) enable_irq_window(v);
        return;
    }

    /* This could be moved earlier in the VMX resume sequence. */
    __vmread(IDT_VECTORING_INFO_FIELD, &idtv_info_field);
    if (unlikely(idtv_info_field & INTR_INFO_VALID_MASK)) {
        __vmwrite(VM_ENTRY_INTR_INFO_FIELD, idtv_info_field);

        /*
         * Safe: the length will only be interpreted for software exceptions
         * and interrupts. If we get here then delivery of some event caused a
         * fault, and this always results in defined VM_EXIT_INSTRUCTION_LEN.
         */
        __vmread(VM_EXIT_INSTRUCTION_LEN, &inst_len); /* Safe */
        __vmwrite(VM_ENTRY_INSTRUCTION_LEN, inst_len);

        if (unlikely(idtv_info_field & 0x800)) { /* valid error code */
            unsigned long error_code;
            __vmread(IDT_VECTORING_ERROR_CODE, &error_code);
            __vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, error_code);
        }
        if (unlikely(has_ext_irq))
            enable_irq_window(v);

        HVM_DBG_LOG(DBG_LEVEL_1, "idtv_info_field=%x", idtv_info_field);

        return;
    }

    if (likely(!has_ext_irq)) return;

    if (unlikely(is_interruptibility_state())) {    
        /* pre-cleared for emulated instruction */
        enable_irq_window(v);
        HVM_DBG_LOG(DBG_LEVEL_1, "interruptibility");
        return;
    }

    __vmread(GUEST_RFLAGS, &eflags);
    if (irq_masked(eflags)) {
        enable_irq_window(v);
        return;
    }

    highest_vector = cpu_get_interrupt(v, &intr_type);
    switch (intr_type) {
    case APIC_DM_EXTINT:
    case APIC_DM_FIXED:
    case APIC_DM_LOWEST:
        vmx_inject_extint(v, highest_vector, VMX_DELIVER_NO_ERROR_CODE);
        TRACE_3D(TRC_VMX_INT, v->domain->domain_id, highest_vector, 0);
        break;

    case APIC_DM_SMI:
    case APIC_DM_NMI:
    case APIC_DM_INIT:
    case APIC_DM_STARTUP:
    default:
        printk("Unsupported interrupt type\n");
        BUG();
        break;
    }
    
    hvm_interrupt_post(v, highest_vector, intr_type);
    return;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
