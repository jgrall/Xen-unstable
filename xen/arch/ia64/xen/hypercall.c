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

#include <asm/vcpu.h>
#include <asm/dom_fw.h>
#include <public/dom0_ops.h>
#include <public/event_channel.h>
#include <public/memory.h>
#include <public/sched.h>
#include <xen/irq.h>
#include <asm/hw_irq.h>
#include <public/physdev.h>

extern unsigned long translate_domain_mpaddr(unsigned long);
static long do_physdev_op(GUEST_HANDLE(physdev_op_t) uop);
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
	(hypercall_t)do_event_channel_op,
	(hypercall_t)do_xen_version,
	(hypercall_t)do_console_io,
	(hypercall_t)do_physdev_op,          	/* do_physdev_op */
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
	(hypercall_t)do_ni_hypercall,		/*  */				/* 30 */
	(hypercall_t)do_ni_hypercall		/*  */
	};

static int
xen_hypercall (struct pt_regs *regs)
{
	switch (regs->r2) {
	    case __HYPERVISOR_sched_op_compat:
		regs->r8 = do_sched_op_compat((int) regs->r14,
		                              (unsigned long) regs->r15);
		break;

	    case __HYPERVISOR_dom0_op:
		regs->r8 = do_dom0_op(guest_handle_from_ptr(regs->r14,
							    dom0_op_t));
		break;

	    case __HYPERVISOR_memory_op:
		regs->r8 = do_memory_op(regs->r14,
			guest_handle_from_ptr(regs->r15, void));
		break;

	    case __HYPERVISOR_event_channel_op:
		regs->r8 = do_event_channel_op(guest_handle_from_ptr(regs->r14, evtchn_op_t));
		break;

	    case __HYPERVISOR_physdev_op:
		regs->r8 = do_physdev_op(guest_handle_from_ptr(regs->r14,
			physdev_op_t));
		break;

	    case __HYPERVISOR_grant_table_op:
		regs->r8 = do_grant_table_op((unsigned int) regs->r14,
			guest_handle_from_ptr(regs->r15, void),
			(unsigned int) regs->r16);
		break;

	    case __HYPERVISOR_console_io:
		regs->r8 = do_console_io((int) regs->r14, (int) regs->r15,
			guest_handle_from_ptr(regs->r16, char));
		break;

	    case __HYPERVISOR_xen_version:
		regs->r8 = do_xen_version((int) regs->r14,
			guest_handle_from_ptr(regs->r15, void));
		break;

	    case __HYPERVISOR_multicall:
		regs->r8 = do_multicall(guest_handle_from_ptr(regs->r14,
			multicall_entry_t), (unsigned int) regs->r15);
		break;

	    case __HYPERVISOR_sched_op:
		regs->r8 = do_sched_op((int) regs->r14,
		                       guest_handle_from_ptr(regs->r15, void));
		break;

	    default:
		printf("unknown xen hypercall %lx\n", regs->r2);
		regs->r8 = do_ni_hypercall();
	}
	return 1;
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
		
		/* Copy from vcpu 0.  */
		c.vcpu.evtchn_vector =
			current->domain->vcpu[0]->vcpu_info->arch.evtchn_vector;
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

static int
fw_hypercall (struct pt_regs *regs)
{
	struct vcpu *v = current;
	struct sal_ret_values x;
	unsigned long *tv, *tc;

	switch (regs->r2) {
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
			int pi;
#define SPURIOUS_VECTOR 15
			pi = vcpu_check_pending_interrupts(v);
			if (pi != SPURIOUS_VECTOR) {
				if (!VCPU(v,pending_interruption))
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
	    case FW_HYPERCALL_EFI_RESET_SYSTEM:
		printf("efi.reset_system called ");
		if (current->domain == dom0) {
			printf("(by dom0)\n ");
			(*efi.reset_system)(EFI_RESET_WARM,0,0,NULL);
		}
		else
			domain_shutdown (current->domain, SHUTDOWN_reboot);
		regs->r8 = EFI_UNSUPPORTED;
		break;
	    case FW_HYPERCALL_EFI_GET_TIME:
		tv = (unsigned long *) vcpu_get_gr(v,32);
		tc = (unsigned long *) vcpu_get_gr(v,33);
		//printf("efi_get_time(%p,%p) called...",tv,tc);
		tv = (unsigned long *) __va(translate_domain_mpaddr((unsigned long) tv));
		if (tc) tc = (unsigned long *) __va(translate_domain_mpaddr((unsigned long) tc));
		regs->r8 = (*efi.get_time)((efi_time_t *) tv, (efi_time_cap_t *) tc);
		//printf("and returns %lx\n",regs->r8);
		break;
	    case FW_HYPERCALL_EFI_SET_TIME:
	    case FW_HYPERCALL_EFI_GET_WAKEUP_TIME:
	    case FW_HYPERCALL_EFI_SET_WAKEUP_TIME:
		// FIXME: need fixes in efi.h from 2.6.9
	    case FW_HYPERCALL_EFI_SET_VIRTUAL_ADDRESS_MAP:
		// FIXME: WARNING!! IF THIS EVER GETS IMPLEMENTED
		// SOME OF THE OTHER EFI EMULATIONS WILL CHANGE AS 
		// POINTER ARGUMENTS WILL BE VIRTUAL!!
	    case FW_HYPERCALL_EFI_GET_VARIABLE:
		// FIXME: need fixes in efi.h from 2.6.9
	    case FW_HYPERCALL_EFI_GET_NEXT_VARIABLE:
	    case FW_HYPERCALL_EFI_SET_VARIABLE:
	    case FW_HYPERCALL_EFI_GET_NEXT_HIGH_MONO_COUNT:
		// FIXME: need fixes in efi.h from 2.6.9
		regs->r8 = EFI_UNSUPPORTED;
		break;
	    case FW_HYPERCALL_IPI:
		fw_hypercall_ipi (regs);
		break;
	    default:
		printf("unknown ia64 fw hypercall %lx\n", regs->r2);
		regs->r8 = do_ni_hypercall();
	}
	return 1;
}

/* opt_unsafe_hypercall: If true, unsafe debugging hypercalls are allowed.
   These can create security hole.  */
static int opt_unsafe_hypercall = 0;
boolean_param("unsafe_hypercall", opt_unsafe_hypercall);

int
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
	    return 1;
	}

	/* Hypercalls are only allowed by kernel.
	   Kernel checks memory accesses.  */
	if (privlvl != 2) {
	    /* FIXME: Return a better error value ?
	       Reflection ? Illegal operation ?  */
	    regs->r8 = -1;
	    return 1;
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

static long do_physdev_op(GUEST_HANDLE(physdev_op_t) uop)
{
    struct physdev_op op;
    long ret;
    int  irq;

    if ( unlikely(copy_from_guest(&op, uop, 1) != 0) )
        return -EFAULT;

    switch ( op.cmd )
    {
    case PHYSDEVOP_IRQ_UNMASK_NOTIFY:
        ret = pirq_guest_unmask(current->domain);
        break;

    case PHYSDEVOP_IRQ_STATUS_QUERY:
        irq = op.u.irq_status_query.irq;
        ret = -EINVAL;
        if ( (irq < 0) || (irq >= NR_IRQS) )
            break;
        op.u.irq_status_query.flags = 0;
        /* Edge-triggered interrupts don't need an explicit unmask downcall. */
        if ( !strstr(irq_desc[irq_to_vector(irq)].handler->typename, "edge") )
            op.u.irq_status_query.flags |= PHYSDEVOP_IRQ_NEEDS_UNMASK_NOTIFY;
        ret = 0;
        break;

    case PHYSDEVOP_APIC_READ:
        ret = -EPERM;
        if ( !IS_PRIV(current->domain) )
            break;
        ret = iosapic_guest_read(
            op.u.apic_op.apic_physbase,
            op.u.apic_op.reg,
            &op.u.apic_op.value);
        break;

    case PHYSDEVOP_APIC_WRITE:
        ret = -EPERM;
        if ( !IS_PRIV(current->domain) )
            break;
        ret = iosapic_guest_write(
            op.u.apic_op.apic_physbase,
            op.u.apic_op.reg,
            op.u.apic_op.value);
        break;

    case PHYSDEVOP_ASSIGN_VECTOR:
        if ( !IS_PRIV(current->domain) )
            return -EPERM;

        if ( (irq = op.u.irq_op.irq) >= NR_IRQS )
            return -EINVAL;
        
        op.u.irq_op.vector = assign_irq_vector(irq);
        ret = 0;
        break;

    default:
        ret = -EINVAL;
        break;
    }

    if ( copy_to_guest(uop, &op, 1) )
        ret = -EFAULT;

    return ret;
}
