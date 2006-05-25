/*
 * Hypercall implementations
 * 
 * Copyright (C) 2005 Hewlett-Packard Co.
 *	Dan Magenheimer (dan.magenheimer@hp.com)
 *
 */

#include <xen/config.h>
#include <xen/sched.h>
#include <xen/hypercall.h>
#include <xen/multicall.h>
#include <xen/guest_access.h>

#include <linux/efi.h>	/* FOR EFI_UNIMPLEMENTED */
#include <asm/sal.h>	/* FOR struct ia64_sal_retval */
#include <asm/fpswa.h>	/* FOR struct fpswa_ret_t */

#include <asm/vcpu.h>
#include <asm/dom_fw.h>
#include <public/dom0_ops.h>
#include <public/event_channel.h>
#include <public/memory.h>
#include <public/sched.h>
#include <xen/irq.h>
#include <asm/hw_irq.h>
#include <public/physdev.h>
#include <xen/domain.h>
#include <public/callback.h>
#include <xen/event.h>

static long do_physdev_op_compat(XEN_GUEST_HANDLE(physdev_op_t) uop);
static long do_physdev_op(int cmd, XEN_GUEST_HANDLE(void) arg);
static long do_callback_op(int cmd, XEN_GUEST_HANDLE(void) arg);
/* FIXME: where these declarations should be there ? */
extern int dump_privop_counts_to_user(char *, int);
extern int zero_privop_counts_to_user(char *, int);

unsigned long idle_when_pending = 0;
unsigned long pal_halt_light_count = 0;

hypercall_t ia64_hypercall_table[] =
	{
	(hypercall_t)do_ni_hypercall,		/* do_set_trap_table */		/*  0 */
	(hypercall_t)do_ni_hypercall,		/* do_mmu_update */
	(hypercall_t)do_ni_hypercall,		/* do_set_gdt */
	(hypercall_t)do_ni_hypercall,		/* do_stack_switch */
	(hypercall_t)do_ni_hypercall,		/* do_set_callbacks */
	(hypercall_t)do_ni_hypercall,		/* do_fpu_taskswitch */		/*  5 */
	(hypercall_t)do_sched_op_compat,
	(hypercall_t)do_dom0_op,
	(hypercall_t)do_ni_hypercall,		/* do_set_debugreg */
	(hypercall_t)do_ni_hypercall,		/* do_get_debugreg */
	(hypercall_t)do_ni_hypercall,		/* do_update_descriptor */	/* 10 */
	(hypercall_t)do_ni_hypercall,		/* do_ni_hypercall */
	(hypercall_t)do_memory_op,
	(hypercall_t)do_multicall,
	(hypercall_t)do_ni_hypercall,		/* do_update_va_mapping */
	(hypercall_t)do_ni_hypercall,		/* do_set_timer_op */		/* 15 */
	(hypercall_t)do_event_channel_op_compat,
	(hypercall_t)do_xen_version,
	(hypercall_t)do_console_io,
	(hypercall_t)do_physdev_op_compat,
	(hypercall_t)do_grant_table_op,						/* 20 */
	(hypercall_t)do_ni_hypercall,		/* do_vm_assist */
	(hypercall_t)do_ni_hypercall,		/* do_update_va_mapping_otherdomain */
	(hypercall_t)do_ni_hypercall,		/* (x86 only) */
	(hypercall_t)do_ni_hypercall,		/* do_vcpu_op */
	(hypercall_t)do_ni_hypercall,		/* (x86_64 only) */		/* 25 */
	(hypercall_t)do_ni_hypercall,		/* do_mmuext_op */
	(hypercall_t)do_ni_hypercall,		/* do_acm_op */
	(hypercall_t)do_ni_hypercall,		/* do_nmi_op */
	(hypercall_t)do_sched_op,
	(hypercall_t)do_callback_op,		/*  */			/* 30 */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_event_channel_op,
	(hypercall_t)do_physdev_op,
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */                  /* 35 */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */                  /* 40 */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */                  /* 45 */
	(hypercall_t)do_ni_hypercall,		/*  */
	(hypercall_t)do_ni_hypercall,		/*  */
#ifdef CONFIG_XEN_IA64_DOM0_VP
	(hypercall_t)do_dom0vp_op,			/* dom0vp_op */
#else
	(hypercall_t)do_ni_hypercall,		/* arch_0 */
#endif
	(hypercall_t)do_ni_hypercall,		/* arch_1 */
	(hypercall_t)do_ni_hypercall,		/* arch_2 */            /* 50 */
	(hypercall_t)do_ni_hypercall,		/* arch_3 */
	(hypercall_t)do_ni_hypercall,		/* arch_4 */
	(hypercall_t)do_ni_hypercall,		/* arch_5 */
	(hypercall_t)do_ni_hypercall,		/* arch_6 */
	(hypercall_t)do_ni_hypercall		/* arch_7 */            /* 55 */
	};

uint32_t nr_hypercalls =
	sizeof(ia64_hypercall_table) / sizeof(hypercall_t);

static IA64FAULT
xen_hypercall (struct pt_regs *regs)
{
	uint32_t cmd = (uint32_t)regs->r2;

	if (cmd < nr_hypercalls)
		regs->r8 = (*ia64_hypercall_table[cmd])(
			regs->r14,
			regs->r15,
			regs->r16,
			regs->r17,
			regs->r18,
			regs->r19);
	else
		regs->r8 = -ENOSYS;

	return IA64_NO_FAULT;
}


static void
fw_hypercall_ipi (struct pt_regs *regs)
{
	int cpu = regs->r14;
	int vector = regs->r15;
	struct vcpu *targ;
		    
	if (0 && vector == 254)
		printf ("send_ipi from %d to %d vector=%d\n",
			current->vcpu_id, cpu, vector);

	if (cpu > MAX_VIRT_CPUS)
		return;

	targ = current->domain->vcpu[cpu];
	if (targ == NULL)
		return;

	if (vector == XEN_SAL_BOOT_RENDEZ_VEC
	    && !test_bit(_VCPUF_initialised, &targ->vcpu_flags)) {
		struct pt_regs *targ_regs = vcpu_regs (targ);
		struct vcpu_guest_context c;
		
		printf ("arch_boot_vcpu: %p %p\n",
			(void *)targ_regs->cr_iip,
			(void *)targ_regs->r1);
		memset (&c, 0, sizeof (c));
		/* Copy regs.  */
		c.regs.cr_iip = targ_regs->cr_iip;
		c.regs.r1 = targ_regs->r1;
		
		if (arch_set_info_guest (targ, &c) != 0) {
			printf ("arch_boot_vcpu: failure\n");
			return;
		}
		if (test_and_clear_bit(_VCPUF_down,
				       &targ->vcpu_flags)) {
			vcpu_wake(targ);
			printf ("arch_boot_vcpu: vcpu %d awaken %016lx!\n",
				targ->vcpu_id, targ_regs->cr_iip);
		}
		else
			printf ("arch_boot_vcpu: huu, already awaken!");
	}
	else {
		int running = test_bit(_VCPUF_running,
				       &targ->vcpu_flags);
		
		vcpu_pend_interrupt(targ, vector);
		vcpu_unblock(targ);
		if (running)
			smp_send_event_check_cpu(targ->processor);
	}
	return;
}

static fpswa_ret_t
fw_hypercall_fpswa (struct vcpu *v)
{
	return PSCBX(v, fpswa_ret);
}

static IA64FAULT
fw_hypercall (struct pt_regs *regs)
{
	struct vcpu *v = current;
	struct sal_ret_values x;
	efi_status_t efi_ret_value;
	fpswa_ret_t fpswa_ret;
	IA64FAULT fault; 
	unsigned long index = regs->r2 & FW_HYPERCALL_NUM_MASK_HIGH;

	switch (index) {
	    case FW_HYPERCALL_PAL_CALL:
		//printf("*** PAL hypercall: index=%d\n",regs->r28);
		//FIXME: This should call a C routine
#if 0
		// This is very conservative, but avoids a possible
		// (and deadly) freeze in paravirtualized domains due
		// to a yet-to-be-found bug where pending_interruption
		// is zero when it shouldn't be. Since PAL is called
		// in the idle loop, this should resolve it
		VCPU(v,pending_interruption) = 1;
#endif
		if (regs->r28 == PAL_HALT_LIGHT) {
			if (vcpu_deliverable_interrupts(v) ||
				event_pending(v)) {
					idle_when_pending++;
				vcpu_pend_unspecified_interrupt(v);
//printf("idle w/int#%d pending!\n",pi);
//this shouldn't happen, but it apparently does quite a bit!  so don't
//allow it to happen... i.e. if a domain has an interrupt pending and
//it tries to halt itself because it thinks it is idle, just return here
//as deliver_pending_interrupt is called on the way out and will deliver it
			}
			else {
				pal_halt_light_count++;
				do_sched_op_compat(SCHEDOP_yield, 0);
			}
			regs->r8 = 0;
			regs->r9 = 0;
			regs->r10 = 0;
			regs->r11 = 0;
		}
		else {
			struct ia64_pal_retval y;

			if (regs->r28 >= PAL_COPY_PAL)
				y = xen_pal_emulator
					(regs->r28, vcpu_get_gr (v, 33),
					 vcpu_get_gr (v, 34),
					 vcpu_get_gr (v, 35));
			else
				y = xen_pal_emulator(regs->r28,regs->r29,
						     regs->r30,regs->r31);
			regs->r8 = y.status; regs->r9 = y.v0;
			regs->r10 = y.v1; regs->r11 = y.v2;
		}
		break;
	    case FW_HYPERCALL_SAL_CALL:
		x = sal_emulator(vcpu_get_gr(v,32),vcpu_get_gr(v,33),
			vcpu_get_gr(v,34),vcpu_get_gr(v,35),
			vcpu_get_gr(v,36),vcpu_get_gr(v,37),
			vcpu_get_gr(v,38),vcpu_get_gr(v,39));
		regs->r8 = x.r8; regs->r9 = x.r9;
		regs->r10 = x.r10; regs->r11 = x.r11;
		break;
	    case FW_HYPERCALL_EFI_CALL:
		efi_ret_value = efi_emulator (regs, &fault);
		if (fault != IA64_NO_FAULT) return fault;
		regs->r8 = efi_ret_value;
		break;
	    case FW_HYPERCALL_IPI:
		fw_hypercall_ipi (regs);
		break;
	    case FW_HYPERCALL_FPSWA:
		fpswa_ret = fw_hypercall_fpswa (v);
		regs->r8  = fpswa_ret.status;
		regs->r9  = fpswa_ret.err0;
		regs->r10 = fpswa_ret.err1;
		regs->r11 = fpswa_ret.err2;
		break;
	    default:
		printf("unknown ia64 fw hypercall %lx\n", regs->r2);
		regs->r8 = do_ni_hypercall();
	}
	return IA64_NO_FAULT;
}

/* opt_unsafe_hypercall: If true, unsafe debugging hypercalls are allowed.
   These can create security hole.  */
static int opt_unsafe_hypercall = 0;
boolean_param("unsafe_hypercall", opt_unsafe_hypercall);

IA64FAULT
ia64_hypercall (struct pt_regs *regs)
{
	struct vcpu *v = current;
	unsigned long index = regs->r2;
	int privlvl = (regs->cr_ipsr & IA64_PSR_CPL) >> IA64_PSR_CPL0_BIT;

	if (index >= FW_HYPERCALL_FIRST_USER) {
	    /* Note: user hypercalls are not safe, since Xen doesn't
	       check memory access privilege: Xen does not deny reading
	       or writing to kernel memory.  */
	    if (!opt_unsafe_hypercall) {
		printf("user xen/ia64 hypercalls disabled\n");
		regs->r8 = -1;
	    }
	    else switch (index) {
		case 0xffff:
			regs->r8 = dump_privop_counts_to_user(
				(char *) vcpu_get_gr(v,32),
				(int) vcpu_get_gr(v,33));
			break;
		case 0xfffe:
			regs->r8 = zero_privop_counts_to_user(
				(char *) vcpu_get_gr(v,32),
				(int) vcpu_get_gr(v,33));
			break;
		default:
			printf("unknown user xen/ia64 hypercall %lx\n", index);
			regs->r8 = do_ni_hypercall();
	    }
	    return IA64_NO_FAULT;
	}

	/* Hypercalls are only allowed by kernel.
	   Kernel checks memory accesses.  */
	if (privlvl != 2) {
	    /* FIXME: Return a better error value ?
	       Reflection ? Illegal operation ?  */
	    regs->r8 = -1;
	    return IA64_NO_FAULT;
	}

	if (index >= FW_HYPERCALL_FIRST_ARCH)
	    return fw_hypercall (regs);
	else
	    return xen_hypercall (regs);
}

/* Need make this function common */
extern int
iosapic_guest_read(
    unsigned long physbase, unsigned int reg, u32 *pval);
extern int
iosapic_guest_write(
    unsigned long physbase, unsigned int reg, u32 pval);

static long do_physdev_op(int cmd, XEN_GUEST_HANDLE(void) arg)
{
    int irq;
    long ret;

    switch ( cmd )
    {
    case PHYSDEVOP_eoi: {
        struct physdev_eoi eoi;
        ret = -EFAULT;
        if ( copy_from_guest(&eoi, arg, 1) != 0 )
            break;
        ret = pirq_guest_eoi(current->domain, eoi.irq);
        break;
    }

    /* Legacy since 0x00030202. */
    case PHYSDEVOP_IRQ_UNMASK_NOTIFY: {
        ret = pirq_guest_unmask(current->domain);
        break;
    }

    case PHYSDEVOP_irq_status_query: {
        struct physdev_irq_status_query irq_status_query;
        ret = -EFAULT;
        if ( copy_from_guest(&irq_status_query, arg, 1) != 0 )
            break;
        irq = irq_status_query.irq;
        ret = -EINVAL;
        if ( (irq < 0) || (irq >= NR_IRQS) )
            break;
        irq_status_query.flags = 0;
        /* Edge-triggered interrupts don't need an explicit unmask downcall. */
        if ( !strstr(irq_desc[irq_to_vector(irq)].handler->typename, "edge") )
            irq_status_query.flags |= XENIRQSTAT_needs_eoi;
        ret = copy_to_guest(arg, &irq_status_query, 1) ? -EFAULT : 0;
        break;
    }

    case PHYSDEVOP_apic_read: {
        struct physdev_apic apic;
        ret = -EFAULT;
        if ( copy_from_guest(&apic, arg, 1) != 0 )
            break;
        ret = -EPERM;
        if ( !IS_PRIV(current->domain) )
            break;
        ret = iosapic_guest_read(apic.apic_physbase, apic.reg, &apic.value);
        if ( copy_to_guest(arg, &apic, 1) != 0 )
            ret = -EFAULT;
        break;
    }

    case PHYSDEVOP_apic_write: {
        struct physdev_apic apic;
        ret = -EFAULT;
        if ( copy_from_guest(&apic, arg, 1) != 0 )
            break;
        ret = -EPERM;
        if ( !IS_PRIV(current->domain) )
            break;
        ret = iosapic_guest_write(apic.apic_physbase, apic.reg, apic.value);
        break;
    }

    case PHYSDEVOP_alloc_irq_vector: {
        struct physdev_irq irq_op;

        ret = -EFAULT;
        if ( copy_from_guest(&irq_op, arg, 1) != 0 )
            break;

        ret = -EPERM;
        if ( !IS_PRIV(current->domain) )
            break;

        ret = -EINVAL;
        if ( (irq = irq_op.irq) >= NR_IRQS )
            break;
        
        irq_op.vector = assign_irq_vector(irq);
        ret = copy_to_guest(arg, &irq_op, 1) ? -EFAULT : 0;
        break;
    }

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

/* Legacy hypercall (as of 0x00030202). */
static long do_physdev_op_compat(XEN_GUEST_HANDLE(physdev_op_t) uop)
{
    struct physdev_op op;

    if ( unlikely(copy_from_guest(&op, uop, 1) != 0) )
        return -EFAULT;

    return do_physdev_op(op.cmd, guest_handle_from_ptr(&uop.p->u, void));
}

/* Legacy hypercall (as of 0x00030202). */
long do_event_channel_op_compat(XEN_GUEST_HANDLE(evtchn_op_t) uop)
{
    struct evtchn_op op;

    if ( unlikely(copy_from_guest(&op, uop, 1) != 0) )
        return -EFAULT;

    return do_event_channel_op(op.cmd, guest_handle_from_ptr(&uop.p->u, void));
}

static long register_guest_callback(struct callback_register *reg)
{
    long ret = 0;
    struct vcpu *v = current;

    if (IS_VMM_ADDRESS(reg->address))
        return -EINVAL;

    switch ( reg->type )
    {
    case CALLBACKTYPE_event:
        v->arch.event_callback_ip    = reg->address;
        break;

    case CALLBACKTYPE_failsafe:
        v->arch.failsafe_callback_ip = reg->address;
        break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static long unregister_guest_callback(struct callback_unregister *unreg)
{
    return -EINVAL ;
}

/* First time to add callback to xen/ia64, so let's just stick to
 * the newer callback interface.
 */
static long do_callback_op(int cmd, XEN_GUEST_HANDLE(void) arg)
{
    long ret;

    switch ( cmd )
    {
    case CALLBACKOP_register:
    {
        struct callback_register reg;

        ret = -EFAULT;
        if ( copy_from_guest(&reg, arg, 1) )
            break;

        ret = register_guest_callback(&reg);
    }
    break;

    case CALLBACKOP_unregister:
    {
        struct callback_unregister unreg;

        ret = -EFAULT;
        if ( copy_from_guest(&unreg, arg, 1) )
            break;

        ret = unregister_guest_callback(&unreg);
    }
    break;

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}
