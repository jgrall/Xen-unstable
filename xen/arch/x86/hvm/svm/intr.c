/*
 * intr.c: Interrupt handling for SVM.
 * Copyright (c) 2005, AMD Inc. 
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
#include <xen/trace.h>
#include <xen/errno.h>
#include <xen/shadow.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/svm/svm.h>
#include <asm/hvm/svm/intr.h>
#include <xen/event.h>
#include <xen/kernel.h>
#include <public/hvm/ioreq.h>
#include <xen/domain_page.h>

/*
 * Most of this code is copied from vmx_io.c and modified 
 * to be suitable for SVM.
 */

static inline int svm_inject_extint(struct vcpu *v, int trap)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    vintr_t intr;

    ASSERT(vmcb);

    /* Save all fields */
    intr = vmcb->vintr;
    /* Update only relevant fields */    
    intr.fields.irq = 1;
    intr.fields.intr_masking = 1;
    intr.fields.vector = trap;
    intr.fields.prio = 0xF;
    intr.fields.ign_tpr = 1;
    vmcb->vintr = intr;

    return 0;
}
    
asmlinkage void svm_intr_assist(void) 
{
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    struct hvm_domain *plat=&v->domain->arch.hvm_domain;
    struct periodic_time *pt = &plat->pl_time.periodic_tm;
    struct hvm_virpic *pic= &plat->vpic;
    int callback_irq;
    int intr_type = APIC_DM_EXTINT;
    int intr_vector = -1;
    int re_injecting = 0;

    ASSERT(vmcb);

    /* Check if an Injection is active */
    /* Previous Interrupt delivery caused this Intercept? */
    if (vmcb->exitintinfo.fields.v && (vmcb->exitintinfo.fields.type == 0)) {
        v->arch.hvm_svm.saved_irq_vector = vmcb->exitintinfo.fields.vector;
        vmcb->exitintinfo.bytes = 0;
        re_injecting = 1;
    }

    /*
     * If event requires injecting then do not inject int.
     */
    if (unlikely(v->arch.hvm_svm.inject_event)) {
        v->arch.hvm_svm.inject_event = 0;
        return;
    }

    /*
     * create a 'fake' virtual interrupt on to intercept as soon
     * as the guest _can_ take interrupts
     */
    if (irq_masked(vmcb->rflags) || vmcb->interrupt_shadow) {
        vmcb->general1_intercepts |= GENERAL1_INTERCEPT_VINTR;
        svm_inject_extint(v, 0x0); /* actual vector doesn't really matter */
        return;
    }

    /* Previous interrupt still pending? */
    if (vmcb->vintr.fields.irq) {
//        printk("Re-injecting IRQ from Vintr\n");
        intr_vector = vmcb->vintr.fields.vector;
        vmcb->vintr.bytes = 0;
        re_injecting = 1;
    }
    /* Pending IRQ saved at last VMExit? */
    else if ( v->arch.hvm_svm.saved_irq_vector >= 0) {
//        printk("Re-Injecting saved IRQ\n");
        intr_vector = v->arch.hvm_svm.saved_irq_vector;
        v->arch.hvm_svm.saved_irq_vector = -1;
        re_injecting = 1;
    }
    /* Now let's check for newer interrrupts  */
    else {

      if ( v->vcpu_id == 0 )
         hvm_pic_assist(v);


      if ( (v->vcpu_id == 0) && pt->enabled && pt->pending_intr_nr ) {
          pic_set_irq(pic, pt->irq, 0);
          pic_set_irq(pic, pt->irq, 1);
      }

      if (v->vcpu_id == 0) {
          callback_irq =
              v->domain->arch.hvm_domain.params[HVM_PARAM_CALLBACK_IRQ];
          if ( callback_irq != 0)
              pic_set_xen_irq(pic, callback_irq, local_events_need_delivery());
      }

      if ( cpu_has_pending_irq(v) )
          intr_vector = cpu_get_interrupt(v, &intr_type);

    }

    /* have we got an interrupt to inject? */
    if (intr_vector >= 0) {
        switch (intr_type) {
        case APIC_DM_EXTINT:
        case APIC_DM_FIXED:
        case APIC_DM_LOWEST:
            /* Re-injecting a PIT interruptt? */
            if (re_injecting && pt->enabled && 
                is_periodic_irq(v, intr_vector, intr_type)) {
                    ++pt->pending_intr_nr;
            }
            /* let's inject this interrupt */
            TRACE_3D(TRC_VMX_INTR, v->domain->domain_id, intr_vector, 0);
            svm_inject_extint(v, intr_vector);
            break;
        case APIC_DM_SMI:
        case APIC_DM_NMI:
        case APIC_DM_INIT:
        case APIC_DM_STARTUP:
        default:
            printk("Unsupported interrupt type: %d\n", intr_type);
            BUG();
            break;
        }
        hvm_interrupt_post(v, intr_vector, intr_type);
    }
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
