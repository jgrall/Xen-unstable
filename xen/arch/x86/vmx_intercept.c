/*
 * vmx_intercept.c: Handle performance critical I/O packets in hypervisor space
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
#include <xen/types.h>
#include <asm/vmx.h>
#include <asm/vmx_platform.h>
#include <asm/vmx_virpit.h>
#include <asm/vmx_intercept.h>
#include <public/io/ioreq.h>

#include <xen/lib.h>
#include <xen/sched.h>
#include <asm/current.h>

#ifdef CONFIG_VMX

/* for intercepting io request after vm_exit, return value: 0--not handle; 1--handled */
int vmx_io_intercept(ioreq_t *p)
{
    struct vcpu *d = current;
    struct vmx_handler_t *handler = &(d->domain->arch.vmx_platform.vmx_handler);
    int i;
    unsigned long addr, offset;
    for (i = 0; i < handler->num_slot; i++) {
        addr   = handler->hdl_list[i].addr;
        offset = handler->hdl_list[i].offset;
        if (p->addr >= addr &&
	    p->addr <  addr + offset)
	    return handler->hdl_list[i].action(p);
    }
    return 0;
}

int register_io_handler(unsigned long addr, unsigned long offset, intercept_action_t action)
{
    struct vcpu *d = current;
    struct vmx_handler_t *handler = &(d->domain->arch.vmx_platform.vmx_handler);
    int num = handler->num_slot;

    if (num >= MAX_IO_HANDLER) {
        printk("no extra space, register io interceptor failed!\n");
        domain_crash_synchronous();
    }

    handler->hdl_list[num].addr = addr;
    handler->hdl_list[num].offset = offset;
    handler->hdl_list[num].action = action;
    handler->num_slot++;
    return 1;

}

static void pit_cal_count(struct vmx_virpit_t *vpit)
{
    unsigned int usec_delta = (unsigned int)((NOW() - vpit->inject_point) / 1000);
    if (usec_delta > vpit->period * 1000)
        VMX_DBG_LOG(DBG_LEVEL_1, "VMX_PIT:long time has passed from last injection!");
    vpit->count = vpit->init_val - ((usec_delta * PIT_FREQ / 1000000) % vpit->init_val );
}

static void pit_latch_io(struct vmx_virpit_t *vpit)
{
    pit_cal_count(vpit);

    switch(vpit->read_state) {
    case MSByte:
        vpit->count_MSB_latched=1;
        break;
    case LSByte:
        vpit->count_LSB_latched=1;
        break;
    case LSByte_multiple:
        vpit->count_LSB_latched=1;
        vpit->count_MSB_latched=1;
        break;
    case MSByte_multiple:
        VMX_DBG_LOG(DBG_LEVEL_1, "VMX_PIT:latch PIT counter before MSB_multiple!");
        vpit->read_state=LSByte_multiple;
        vpit->count_LSB_latched=1;
        vpit->count_MSB_latched=1;
        break;
    default:
        BUG();
    }
}

static int pit_read_io(struct vmx_virpit_t *vpit)
{
    if(vpit->count_LSB_latched) {
        /* Read Least Significant Byte */
        if(vpit->read_state==LSByte_multiple) {
            vpit->read_state=MSByte_multiple;
        }
        vpit->count_LSB_latched=0;
        return (vpit->count & 0xFF);
    } else if(vpit->count_MSB_latched) {
        /* Read Most Significant Byte */
        if(vpit->read_state==MSByte_multiple) {
            vpit->read_state=LSByte_multiple;
        }
        vpit->count_MSB_latched=0;
        return ((vpit->count>>8) & 0xFF);
    } else {
        /* Unlatched Count Read */
        VMX_DBG_LOG(DBG_LEVEL_1, "VMX_PIT: unlatched read");
        pit_cal_count(vpit);
        if(!(vpit->read_state & 0x1)) {
            /* Read Least Significant Byte */
            if(vpit->read_state==LSByte_multiple) {
                vpit->read_state=MSByte_multiple;
            }
            return (vpit->count & 0xFF);
        } else {
            /* Read Most Significant Byte */
            if(vpit->read_state==MSByte_multiple) {
                vpit->read_state=LSByte_multiple;
            }
            return ((vpit->count>>8) & 0xFF);
        }
    }
}

/* vmx_io_assist light-weight version, specific to PIT DM */ 
static void resume_pit_io(ioreq_t *p)
{
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    unsigned long old_eax = regs->eax;
    p->state = STATE_INVALID;

    switch(p->size) {
    case 1:
        regs->eax = (old_eax & 0xffffff00) | (p->u.data & 0xff);
        break;
    case 2:
        regs->eax = (old_eax & 0xffff0000) | (p->u.data & 0xffff);
        break;
    case 4:
        regs->eax = (p->u.data & 0xffffffff);
        break;
    default:
        BUG();
    }
}

/* the intercept action for PIT DM retval:0--not handled; 1--handled */
int intercept_pit_io(ioreq_t *p)
{
    struct vcpu *d = current;
    struct vmx_virpit_t *vpit = &(d->domain->arch.vmx_platform.vmx_pit);

    if (p->size != 1 ||
        p->pdata_valid ||
        p->port_mm)
        return 0;
    
    if (p->addr == 0x43 &&
	p->dir == 0 &&				/* write */
        ((p->u.data >> 4) & 0x3) == 0 &&	/* latch command */
        ((p->u.data >> 6) & 0x3) == (vpit->channel)) {/* right channel */
        pit_latch_io(vpit);
	return 1;
    }

    if (p->addr == (0x40 + vpit->channel) &&
	p->dir == 1) {	/* read */
        p->u.data = pit_read_io(vpit);
        resume_pit_io(p);
	return 1;
    }

    return 0;
}

/* hooks function for the PIT initialization response iopacket */
static void pit_timer_fn(void *data)
{
    struct vmx_virpit_t *vpit = data;

    /* Set the pending intr bit, and send evtchn notification to myself. */
    if (test_and_set_bit(vpit->vector, vpit->intr_bitmap))
        vpit->pending_intr_nr++; /* already set, then count the pending intr */

    set_ac_timer(&vpit->pit_timer, NOW() + MILLISECS(vpit->period));
}


/* Only some PIT operations such as load init counter need a hypervisor hook.
 * leave all other operations in user space DM
 */
void vmx_hooks_assist(struct vcpu *d)
{
    vcpu_iodata_t * vio = get_vio(d->domain, d->vcpu_id);
    ioreq_t *p = &vio->vp_ioreq;
    unsigned long *intr = &(vio->vp_intr[0]);
    struct vmx_virpit_t *vpit = &(d->domain->arch.vmx_platform.vmx_pit);
    int rw_mode;

    /* load init count*/
    if (p->state == STATE_IORESP_HOOK) { 
        /* set up actimer, handle re-init */
        if ( active_ac_timer(&(vpit->pit_timer)) ) {
            VMX_DBG_LOG(DBG_LEVEL_1, "VMX_PIT: guest reset PIT with channel %lx!\n", (unsigned long) ((p->u.data >> 24) & 0x3) );
            rem_ac_timer(&(vpit->pit_timer));
        }
        else
            init_ac_timer(&vpit->pit_timer, pit_timer_fn, vpit, 0);

        /* init count for this channel */
        vpit->init_val = (p->u.data & 0xFFFF) ; 
        /* frequency(ms) of pit */
        vpit->period = DIV_ROUND(((vpit->init_val) * 1000), PIT_FREQ); 
        if (vpit->period < 1) {
            printk("VMX_PIT: guest programmed too small an init_val: %x\n",
                   vpit->init_val);
            vpit->period = 1;
        }
        vpit->vector = ((p->u.data >> 16) & 0xFF);
        vpit->channel = ((p->u.data >> 24) & 0x3);
        vpit->first_injected = 0;

	vpit->count_LSB_latched = 0;
	vpit->count_MSB_latched = 0;

        rw_mode = ((p->u.data >> 26) & 0x3);
        switch(rw_mode) {
        case 0x1:
            vpit->read_state=LSByte;
            break;
        case 0x2:
            vpit->read_state=MSByte;
            break;
        case 0x3:
            vpit->read_state=LSByte_multiple;
            break;
        default:
            printk("VMX_PIT:wrong PIT rw_mode!\n");
            break;
        }

        vpit->intr_bitmap = intr;

	set_ac_timer(&vpit->pit_timer, NOW() + MILLISECS(vpit->period));

        /*restore the state*/
        p->state = STATE_IORESP_READY;

	/* register handler to intercept the PIT io when vm_exit */
	register_io_handler(0x40, 4, intercept_pit_io); 
    }

}

#endif /* CONFIG_VMX */
