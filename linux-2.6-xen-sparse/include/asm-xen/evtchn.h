/******************************************************************************
 * evtchn.h
 * 
 * Communication via Xen event channels.
 * Also definitions for the device that demuxes notifications to userspace.
 * 
 * Copyright (c) 2004-2005, K A Fraser
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

#ifndef __ASM_EVTCHN_H__
#define __ASM_EVTCHN_H__

#include <linux/config.h>
#include <linux/interrupt.h>
#include <asm/hypervisor.h>
#include <asm/ptrace.h>
#include <asm/synch_bitops.h>
#include <asm-xen/xen-public/event_channel.h>
#include <linux/smp.h>

/*
 * LOW-LEVEL DEFINITIONS
 */

/* Dynamically bind a VIRQ source to Linux IRQ space. */
extern int  bind_virq_to_irq(int virq);
extern void unbind_virq_from_irq(int virq);

/* Dynamically bind an IPI source to Linux IRQ space. */
extern int  bind_ipi_to_irq(int ipi);
extern void unbind_ipi_from_irq(int ipi);

/*
 * Dynamically bind an event-channel port to Linux IRQ space.
 * BIND:   Returns IRQ or error.
 * UNBIND: Takes IRQ to unbind from; automatically closes the event channel.
 */
extern int  bind_evtchn_to_irq(unsigned int evtchn);
extern void unbind_evtchn_from_irq(unsigned int irq);

/*
 * Dynamically bind an event-channel port to an IRQ-like callback handler.
 * On some platforms this may not be implemented via the Linux IRQ subsystem.
 * The IRQ argument passed to the callback handler is the same as returned
 * from the bind call. It may not correspond to a Linux IRQ number.
 * BIND:   Returns IRQ or error.
 * UNBIND: Takes IRQ to unbind from; automatically closes the event channel.
 */
extern int  bind_evtchn_to_irqhandler(
	unsigned int evtchn,
	irqreturn_t (*handler)(int, void *, struct pt_regs *),
	unsigned long irqflags,
	const char *devname,
	void *dev_id);
extern void unbind_evtchn_from_irqhandler(unsigned int irq, void *dev_id);

/*
 * Unlike notify_remote_via_evtchn(), this is safe to use across
 * save/restore. Notifications on a broken connection are silently dropped.
 */
void notify_remote_via_irq(int irq);

extern void irq_resume(void);

/* Entry point for notifications into Linux subsystems. */
asmlinkage void evtchn_do_upcall(struct pt_regs *regs);

/* Entry point for notifications into the userland character device. */
void evtchn_device_upcall(int port);

static inline void mask_evtchn(int port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	synch_set_bit(port, &s->evtchn_mask[0]);
}

static inline void unmask_evtchn(int port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	vcpu_info_t *vcpu_info = &s->vcpu_data[smp_processor_id()];

	synch_clear_bit(port, &s->evtchn_mask[0]);

	/*
	 * The following is basically the equivalent of 'hw_resend_irq'. Just
	 * like a real IO-APIC we 'lose the interrupt edge' if the channel is
	 * masked.
	 */
	if (synch_test_bit         (port,    &s->evtchn_pending[0]) && 
	    !synch_test_and_set_bit(port>>5, &vcpu_info->evtchn_pending_sel)) {
		vcpu_info->evtchn_upcall_pending = 1;
		if (!vcpu_info->evtchn_upcall_mask)
			force_evtchn_callback();
	}
}

static inline void clear_evtchn(int port)
{
	shared_info_t *s = HYPERVISOR_shared_info;
	synch_clear_bit(port, &s->evtchn_pending[0]);
}

static inline void notify_remote_via_evtchn(int port)
{
	evtchn_op_t op = {
		.cmd = EVTCHNOP_send,
		.u.send.local_port = port };
	(void)HYPERVISOR_event_channel_op(&op);
}

#endif /* __ASM_EVTCHN_H__ */

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
