/******************************************************************************
 * arch/x86/irq.c
 * 
 * Portions of this file are:
 *  Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 */

#include <xen/config.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/irq.h>
#include <xen/perfc.h>
#include <xen/sched.h>
#include <asm/smpboot.h>

irq_desc_t irq_desc[NR_IRQS] __cacheline_aligned;

static void __do_IRQ_guest(int irq);

void no_action(int cpl, void *dev_id, struct pt_regs *regs) { }

static void enable_none(unsigned int irq) { }
static unsigned int startup_none(unsigned int irq) { return 0; }
static void disable_none(unsigned int irq) { }
static void ack_none(unsigned int irq)
{
    printk("Unexpected IRQ trap at vector %02x.\n", irq);
    ack_APIC_irq();
}

#define shutdown_none   disable_none
#define end_none        enable_none

struct hw_interrupt_type no_irq_type = {
    "none",
    startup_none,
    shutdown_none,
    enable_none,
    disable_none,
    ack_none,
    end_none
};

atomic_t irq_err_count;
atomic_t irq_mis_count;

inline void disable_irq_nosync(unsigned int irq)
{
    irq_desc_t   *desc = &irq_desc[irq];
    unsigned long flags;

    spin_lock_irqsave(&desc->lock, flags);

    if ( desc->depth++ == 0 )
    {
        desc->status |= IRQ_DISABLED;
        desc->handler->disable(irq);
    }

    spin_unlock_irqrestore(&desc->lock, flags);
}

void disable_irq(unsigned int irq)
{
    disable_irq_nosync(irq);
    do { smp_mb(); } while ( irq_desc[irq].status & IRQ_INPROGRESS );
}

void enable_irq(unsigned int irq)
{
    irq_desc_t   *desc = &irq_desc[irq];
    unsigned long flags;

    spin_lock_irqsave(&desc->lock, flags);

    if ( --desc->depth == 0 )
    {
        desc->status &= ~IRQ_DISABLED;
        if ( (desc->status & (IRQ_PENDING | IRQ_REPLAY)) == IRQ_PENDING )
        {
            desc->status |= IRQ_REPLAY;
            hw_resend_irq(desc->handler,irq);
        }
        desc->handler->enable(irq);
    }

    spin_unlock_irqrestore(&desc->lock, flags);
}

asmlinkage void do_IRQ(struct pt_regs regs)
{       
#if defined(__i386__)
    unsigned int      irq = regs.orig_eax;
#else
    unsigned int      irq = 0; /* XXX */
#endif
    irq_desc_t       *desc = &irq_desc[irq];
    struct irqaction *action;

    perfc_incrc(irqs);

    spin_lock(&desc->lock);
    desc->handler->ack(irq);

    if ( likely(desc->status & IRQ_GUEST) )
    {
        __do_IRQ_guest(irq);
        spin_unlock(&desc->lock);
        return;
    }

    desc->status &= ~IRQ_REPLAY;
    desc->status |= IRQ_PENDING;

    /*
     * Since we set PENDING, if another processor is handling a different 
     * instance of this same irq, the other processor will take care of it.
     */
    if ( desc->status & (IRQ_DISABLED | IRQ_INPROGRESS) )
        goto out;

    desc->status |= IRQ_INPROGRESS;

    action = desc->action;
    while ( desc->status & IRQ_PENDING )
    {
        desc->status &= ~IRQ_PENDING;
        irq_enter(smp_processor_id(), irq);
        spin_unlock_irq(&desc->lock);
        action->handler(irq, action->dev_id, &regs);
        spin_lock_irq(&desc->lock);
        irq_exit(smp_processor_id(), irq);
    }

    desc->status &= ~IRQ_INPROGRESS;

 out:
    desc->handler->end(irq);
    spin_unlock(&desc->lock);
}

void free_irq(unsigned int irq)
{
    irq_desc_t   *desc = &irq_desc[irq];
    unsigned long flags;

    spin_lock_irqsave(&desc->lock,flags);
    desc->action  = NULL;
    desc->depth   = 1;
    desc->status |= IRQ_DISABLED;
    desc->handler->shutdown(irq);
    spin_unlock_irqrestore(&desc->lock,flags);

    /* Wait to make sure it's not being used on another CPU */
    do { smp_mb(); } while ( irq_desc[irq].status & IRQ_INPROGRESS );
}

int setup_irq(unsigned int irq, struct irqaction *new)
{
    irq_desc_t   *desc = &irq_desc[irq];
    unsigned long flags;
 
    spin_lock_irqsave(&desc->lock,flags);

    if ( desc->action != NULL )
    {
        spin_unlock_irqrestore(&desc->lock,flags);
        return -EBUSY;
    }

    desc->action  = new;
    desc->depth   = 0;
    desc->status &= ~IRQ_DISABLED;
    desc->handler->startup(irq);

    spin_unlock_irqrestore(&desc->lock,flags);

    return 0;
}


/*
 * HANDLING OF GUEST-BOUND PHYSICAL IRQS
 */

#define IRQ_MAX_GUESTS 7
typedef struct {
    u8 nr_guests;
    u8 in_flight;
    u8 shareable;
    struct domain *guest[IRQ_MAX_GUESTS];
} irq_guest_action_t;

static void __do_IRQ_guest(int irq)
{
    irq_desc_t         *desc = &irq_desc[irq];
    irq_guest_action_t *action = (irq_guest_action_t *)desc->action;
    struct domain *p;
    int                 i;

    for ( i = 0; i < action->nr_guests; i++ )
    {
        p = action->guest[i];
        if ( !test_and_set_bit(irq, &p->pirq_mask) )
            action->in_flight++;
        send_guest_pirq(p, irq);
    }
}

int pirq_guest_unmask(struct domain *p)
{
    irq_desc_t    *desc;
    int            i, j, pirq;
    u32            m;
    shared_info_t *s = p->shared_info;

    for ( i = 0; i < 2; i++ )
    {
        m = p->pirq_mask[i];
        while ( (j = ffs(m)) != 0 )
        {
            m &= ~(1 << --j);
            pirq = (i << 5) + j;
            desc = &irq_desc[pirq];
            spin_lock_irq(&desc->lock);
            if ( !test_bit(p->pirq_to_evtchn[pirq], &s->evtchn_mask[0]) &&
                 test_and_clear_bit(pirq, &p->pirq_mask) &&
                 (--((irq_guest_action_t *)desc->action)->in_flight == 0) )
                desc->handler->end(pirq);
            spin_unlock_irq(&desc->lock);
        }
    }

    return 0;
}

int pirq_guest_bind(struct domain *p, int irq, int will_share)
{
    irq_desc_t         *desc = &irq_desc[irq];
    irq_guest_action_t *action;
    unsigned long       flags;
    int                 rc = 0;

    if ( !IS_CAPABLE_PHYSDEV(p) )
        return -EPERM;

    spin_lock_irqsave(&desc->lock, flags);

    action = (irq_guest_action_t *)desc->action;

    if ( !(desc->status & IRQ_GUEST) )
    {
        if ( desc->action != NULL )
        {
            DPRINTK("Cannot bind IRQ %d to guest. In use by '%s'.\n",
                    irq, desc->action->name);
            rc = -EBUSY;
            goto out;
        }

        action = kmalloc(sizeof(irq_guest_action_t));
        if ( (desc->action = (struct irqaction *)action) == NULL )
        {
            DPRINTK("Cannot bind IRQ %d to guest. Out of memory.\n", irq);
            rc = -ENOMEM;
            goto out;
        }

        action->nr_guests = 0;
        action->in_flight = 0;
        action->shareable = will_share;
        
        desc->depth = 0;
        desc->status |= IRQ_GUEST;
        desc->status &= ~IRQ_DISABLED;
        desc->handler->startup(irq);

        /* Attempt to bind the interrupt target to the correct CPU. */
        if ( desc->handler->set_affinity != NULL )
            desc->handler->set_affinity(
                irq, apicid_to_phys_cpu_present(p->processor));
    }
    else if ( !will_share || !action->shareable )
    {
        DPRINTK("Cannot bind IRQ %d to guest. Will not share with others.\n",
                irq);
        rc = -EBUSY;
        goto out;
    }

    if ( action->nr_guests == IRQ_MAX_GUESTS )
    {
        DPRINTK("Cannot bind IRQ %d to guest. Already at max share.\n", irq);
        rc = -EBUSY;
        goto out;
    }

    action->guest[action->nr_guests++] = p;

 out:
    spin_unlock_irqrestore(&desc->lock, flags);
    return rc;
}

int pirq_guest_unbind(struct domain *p, int irq)
{
    irq_desc_t         *desc = &irq_desc[irq];
    irq_guest_action_t *action;
    unsigned long       flags;
    int                 i;

    spin_lock_irqsave(&desc->lock, flags);

    action = (irq_guest_action_t *)desc->action;

    if ( test_and_clear_bit(irq, &p->pirq_mask) &&
         (--action->in_flight == 0) )
        desc->handler->end(irq);

    if ( action->nr_guests == 1 )
    {
        desc->action = NULL;
        kfree(action);
        desc->depth   = 1;
        desc->status |= IRQ_DISABLED;
        desc->status &= ~IRQ_GUEST;
        desc->handler->shutdown(irq);
    }
    else
    {
        i = 0;
        while ( action->guest[i] != p )
            i++;
        memmove(&action->guest[i], &action->guest[i+1], IRQ_MAX_GUESTS-i-1);
        action->nr_guests--;
    }

    spin_unlock_irqrestore(&desc->lock, flags);    
    return 0;
}

int pirq_guest_bindable(int irq, int will_share)
{
    irq_desc_t         *desc = &irq_desc[irq];
    irq_guest_action_t *action;
    unsigned long       flags;
    int                 okay;

    spin_lock_irqsave(&desc->lock, flags);

    action = (irq_guest_action_t *)desc->action;

    /*
     * To be bindable the IRQ must either be not currently bound (1), or
     * it must be shareable (2) and not at its share limit (3).
     */
    okay = ((!(desc->status & IRQ_GUEST) && (action == NULL)) || /* 1 */
            (action->shareable && will_share &&                  /* 2 */
             (action->nr_guests != IRQ_MAX_GUESTS)));            /* 3 */

    spin_unlock_irqrestore(&desc->lock, flags);
    return okay;
}
