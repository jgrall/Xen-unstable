/******************************************************************************
 * xenbus_comms.c
 *
 * Low level code to talks to Xen Store: ringbuffer and event channel.
 *
 * Copyright (C) 2005 Rusty Russell, IBM Corporation
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

#include <asm/hypervisor.h>
#include <asm-xen/evtchn.h>
#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <asm-xen/xenbus.h>
#include "xenbus_comms.h"

static int xenbus_irq      = 0;

extern void xenbus_probe(void *); 
extern int xenstored_ready; 
static DECLARE_WORK(probe_work, xenbus_probe, NULL);

DECLARE_WAIT_QUEUE_HEAD(xb_waitq);

static inline struct xenstore_domain_interface *xenstore_domain_interface(void)
{
	return mfn_to_virt(xen_start_info->store_mfn);
}

static irqreturn_t wake_waiting(int irq, void *unused, struct pt_regs *regs)
{
	if(unlikely(xenstored_ready == 0)) {
		xenstored_ready = 1; 
		schedule_work(&probe_work); 
	} 

	wake_up(&xb_waitq);
	return IRQ_HANDLED;
}

static int check_indexes(XENSTORE_RING_IDX cons, XENSTORE_RING_IDX prod)
{
	return ((prod - cons) <= XENSTORE_RING_SIZE);
}

static void *get_output_chunk(XENSTORE_RING_IDX cons,
			      XENSTORE_RING_IDX prod,
			      char *buf, uint32_t *len)
{
	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(prod);
	if ((XENSTORE_RING_SIZE - (prod - cons)) < *len)
		*len = XENSTORE_RING_SIZE - (prod - cons);
	return buf + MASK_XENSTORE_IDX(prod);
}

static const void *get_input_chunk(XENSTORE_RING_IDX cons,
				   XENSTORE_RING_IDX prod,
				   const char *buf, uint32_t *len)
{
	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(cons);
	if ((prod - cons) < *len)
		*len = prod - cons;
	return buf + MASK_XENSTORE_IDX(cons);
}

int xb_write(const void *data, unsigned len)
{
	struct xenstore_domain_interface *intf = xenstore_domain_interface();
	XENSTORE_RING_IDX cons, prod;

	while (len != 0) {
		void *dst;
		unsigned int avail;

		wait_event_interruptible(xb_waitq,
					 (intf->req_prod - intf->req_cons) !=
					 XENSTORE_RING_SIZE);

		/* Read indexes, then verify. */
		cons = intf->req_cons;
		prod = intf->req_prod;
		mb();
		if (!check_indexes(cons, prod))
			return -EIO;

		dst = get_output_chunk(cons, prod, intf->req, &avail);
		if (avail == 0)
			continue;
		if (avail > len)
			avail = len;

		memcpy(dst, data, avail);
		data += avail;
		len -= avail;

		/* Other side must not see new header until data is there. */
		wmb();
		intf->req_prod += avail;

		/* This implies mb() before other side sees interrupt. */
		notify_remote_via_evtchn(xen_start_info->store_evtchn);
	}

	return 0;
}

int xb_read(void *data, unsigned len)
{
	struct xenstore_domain_interface *intf = xenstore_domain_interface();
	XENSTORE_RING_IDX cons, prod;

	while (len != 0) {
		unsigned int avail;
		const char *src;

		wait_event_interruptible(xb_waitq,
					 intf->rsp_cons != intf->rsp_prod);

		/* Read indexes, then verify. */
		cons = intf->rsp_cons;
		prod = intf->rsp_prod;
		mb();
		if (!check_indexes(cons, prod))
			return -EIO;

		src = get_input_chunk(cons, prod, intf->rsp, &avail);
		if (avail == 0)
			continue;
		if (avail > len)
			avail = len;

		/* We must read header before we read data. */
		rmb();

		memcpy(data, src, avail);
		data += avail;
		len -= avail;

		/* Other side must not see free space until we've copied out */
		mb();
		intf->rsp_cons += avail;

		pr_debug("Finished read of %i bytes (%i to go)\n", avail, len);

		/* Implies mb(): they will see new header. */
		notify_remote_via_evtchn(xen_start_info->store_evtchn);
	}

	return 0;
}

/* Set up interrupt handler off store event channel. */
int xb_init_comms(void)
{
	int err;

	if (xenbus_irq)
		unbind_from_irqhandler(xenbus_irq, &xb_waitq);

	err = bind_evtchn_to_irqhandler(
		xen_start_info->store_evtchn, wake_waiting,
		0, "xenbus", &xb_waitq);
	if (err <= 0) {
		printk(KERN_ERR "XENBUS request irq failed %i\n", err);
		return err;
	}

	xenbus_irq = err;

	/* FIXME zero out page -- domain builder should probably do this*/
	memset(mfn_to_virt(xen_start_info->store_mfn), 0, PAGE_SIZE);

	return 0;
}

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
