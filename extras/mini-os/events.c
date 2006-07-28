/* -*-  Mode:C; c-basic-offset:4; tab-width:4 -*-
 ****************************************************************************
 * (C) 2003 - Rolf Neugebauer - Intel Research Cambridge
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: events.c
 *      Author: Rolf Neugebauer (neugebar@dcs.gla.ac.uk)
 *     Changes: Grzegorz Milos (gm281@cam.ac.uk)
 *              
 *        Date: Jul 2003, changes Jun 2005
 * 
 * Environment: Xen Minimal OS
 * Description: Deals with events recieved on event channels
 *
 ****************************************************************************
 */

#include <os.h>
#include <mm.h>
#include <hypervisor.h>
#include <events.h>
#include <lib.h>

#define NR_EVS 1024

/* this represents a event handler. Chaining or sharing is not allowed */
typedef struct _ev_action_t {
	void (*handler)(int, struct pt_regs *, void *);
	void *data;
    u32 count;
} ev_action_t;


static ev_action_t ev_actions[NR_EVS];
void default_handler(int port, struct pt_regs *regs, void *data);


/*
 * Demux events to different handlers.
 */
int do_event(u32 port, struct pt_regs *regs)
{
    ev_action_t  *action;
    if (port >= NR_EVS) {
        printk("Port number too large: %d\n", port);
		goto out;
    }

    action = &ev_actions[port];
    action->count++;

    /* call the handler */
	action->handler(port, regs, action->data);

 out:
	clear_evtchn(port);

    return 1;

}

int bind_evtchn( u32 port, void (*handler)(int, struct pt_regs *, void *),
				 void *data )
{
 	if(ev_actions[port].handler != default_handler)
        printk("WARN: Handler for port %d already registered, replacing\n",
				port);

	ev_actions[port].data = data;
	wmb();
	ev_actions[port].handler = handler;

	/* Finally unmask the port */
	unmask_evtchn(port);

	return port;
}

void unbind_evtchn( u32 port )
{
	if (ev_actions[port].handler == default_handler)
		printk("WARN: No handler for port %d when unbinding\n", port);
	ev_actions[port].handler = default_handler;
	wmb();
	ev_actions[port].data = NULL;
}

int bind_virq( u32 virq, void (*handler)(int, struct pt_regs *, void *data),
			   void *data)
{
	evtchn_op_t op;

	/* Try to bind the virq to a port */
	op.cmd = EVTCHNOP_bind_virq;
	op.u.bind_virq.virq = virq;
	op.u.bind_virq.vcpu = smp_processor_id();

	if ( HYPERVISOR_event_channel_op(&op) != 0 )
	{
		printk("Failed to bind virtual IRQ %d\n", virq);
		return 1;
    }
    bind_evtchn(op.u.bind_virq.port, handler, data);
	return 0;
}

#if defined(__x86_64__)
/* Allocate 4 pages for the irqstack */
#define STACK_PAGES 4
char irqstack[1024 * 4 * STACK_PAGES];

static struct pda
{
    int irqcount;       /* offset 0 (used in x86_64.S) */
    char *irqstackptr;  /*        8 */
} cpu0_pda;
#endif

/*
 * Initially all events are without a handler and disabled
 */
void init_events(void)
{
    int i;
#if defined(__x86_64__)
    asm volatile("movl %0,%%fs ; movl %0,%%gs" :: "r" (0));
    wrmsrl(0xc0000101, &cpu0_pda); /* 0xc0000101 is MSR_GS_BASE */
    cpu0_pda.irqcount = -1;
    cpu0_pda.irqstackptr = irqstack + 1024 * 4 * STACK_PAGES;
#endif
    /* inintialise event handler */
    for ( i = 0; i < NR_EVS; i++ )
	{
        ev_actions[i].handler = default_handler;
        mask_evtchn(i);
    }
}

void default_handler(int port, struct pt_regs *regs, void *ignore)
{
    printk("[Port %d] - event received\n", port);
}

/* Unfortunate confusion of terminology: the port is unbound as far
   as Xen is concerned, but we automatically bind a handler to it
   from inside mini-os. */
int evtchn_alloc_unbound(void (*handler)(int, struct pt_regs *regs,
										 void *data),
						 void *data)
{
	u32 port;
	evtchn_op_t op;
	int err;

	op.cmd = EVTCHNOP_alloc_unbound;
	op.u.alloc_unbound.dom = DOMID_SELF;
	op.u.alloc_unbound.remote_dom = 0;

	err = HYPERVISOR_event_channel_op(&op);
	if (err) {
		printk("Failed to alloc unbound evtchn: %d.\n", err);
		return -1;
	}
	port = op.u.alloc_unbound.port;
	bind_evtchn(port, handler, data);
	return port;
}
