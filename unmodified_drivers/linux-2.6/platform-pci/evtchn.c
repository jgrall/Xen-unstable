/******************************************************************************
 * evtchn.c
 *
 * A simplified event channel for para-drivers in unmodified linux
 *
 * Copyright (c) 2002-2005, K A Fraser
 * Copyright (c) 2005, <xiaofeng.ling@intel.com>
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

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <xen/evtchn.h>
#include <xen/interface/hvm/ioreq.h>
#include <xen/features.h>
#include "platform-pci.h"

void *shared_info_area;

#define MAX_EVTCHN 256
static struct
{
	irqreturn_t(*handler) (int, void *, struct pt_regs *);
	void *dev_id;
} evtchns[MAX_EVTCHN];

void mask_evtchn(int port)
{
	shared_info_t *s = shared_info_area;
	synch_set_bit(port, &s->evtchn_mask[0]);
}
EXPORT_SYMBOL(mask_evtchn);

void unmask_evtchn(int port)
{
	unsigned int cpu;
	shared_info_t *s = shared_info_area;
	vcpu_info_t *vcpu_info;

	preempt_disable();
	cpu = smp_processor_id();
	vcpu_info = &s->vcpu_info[cpu];

	/* Slow path (hypercall) if this is a non-local port.  We only
	   ever bind event channels to vcpu 0 in HVM guests. */
	if (unlikely(cpu != 0)) {
		evtchn_unmask_t op = { .port = port };
		(void)HYPERVISOR_event_channel_op(EVTCHNOP_unmask,
						  &op);
		preempt_enable();
		return;
	}

	synch_clear_bit(port, &s->evtchn_mask[0]);

	/*
	 * The following is basically the equivalent of
	 * 'hw_resend_irq'. Just like a real IO-APIC we 'lose the
	 * interrupt edge' if the channel is masked.
	 */
	if (synch_test_bit(port, &s->evtchn_pending[0]) &&
	    !synch_test_and_set_bit(port / BITS_PER_LONG,
				    &vcpu_info->evtchn_pending_sel)) {
		vcpu_info->evtchn_upcall_pending = 1;
		if (!vcpu_info->evtchn_upcall_mask)
			force_evtchn_callback();
	}
	preempt_enable();
}
EXPORT_SYMBOL(unmask_evtchn);

int
bind_evtchn_to_irqhandler(unsigned int evtchn,
			  irqreturn_t(*handler) (int, void *,
						 struct pt_regs *),
			  unsigned long irqflags, const char *devname,
			  void *dev_id)
{
	if (evtchn >= MAX_EVTCHN)
		return -EINVAL;
	evtchns[evtchn].handler = handler;
	evtchns[evtchn].dev_id = dev_id;
	unmask_evtchn(evtchn);
	return evtchn;
}

EXPORT_SYMBOL(bind_evtchn_to_irqhandler);

void unbind_from_irqhandler(unsigned int evtchn, void *dev_id)
{
	if (evtchn >= MAX_EVTCHN)
		return;

	mask_evtchn(evtchn);
	evtchns[evtchn].handler = NULL;
}

EXPORT_SYMBOL(unbind_from_irqhandler);

void notify_remote_via_irq(int irq)
{
	int evtchn = irq;
	notify_remote_via_evtchn(evtchn);
}

EXPORT_SYMBOL(notify_remote_via_irq);

irqreturn_t evtchn_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned int l1i, l2i, port;
	int cpu = smp_processor_id();
	irqreturn_t(*handler) (int, void *, struct pt_regs *);
	shared_info_t *s = shared_info_area;
	vcpu_info_t *v = &s->vcpu_info[cpu];
	unsigned long l1, l2;

	v->evtchn_upcall_pending = 0;
	/* NB. No need for a barrier here -- XCHG is a barrier
	 * on x86. */
	l1 = xchg(&v->evtchn_pending_sel, 0);
	while (l1 != 0)
	{
		l1i = __ffs(l1);
		l1 &= ~(1 << l1i);

		l2 = s->evtchn_pending[l1i] & ~s->evtchn_mask[l1i];
		while (l2 != 0)
		{
			l2i = __ffs(l2);

			port = (l1i * BITS_PER_LONG) + l2i;
			synch_clear_bit(port, &s->evtchn_pending[0]);
			if ((handler = evtchns[port].handler) != NULL)
			{
				handler(port, evtchns[port].dev_id,
					regs);
			}
			else
			{
				printk(KERN_WARNING "unexpected event channel upcall on port %d!\n", port);
			}
			l2 = s->evtchn_pending[l1i] & ~s->evtchn_mask[l1i];
		}
	}
	return IRQ_HANDLED;
}

void force_evtchn_callback(void)
{
	evtchn_interrupt(0, NULL, NULL);
}
EXPORT_SYMBOL(force_evtchn_callback);
