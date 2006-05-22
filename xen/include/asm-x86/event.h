/******************************************************************************
 * event.h
 *
 * A nice interface for passing asynchronous events to guest OSes.
 * (architecture-dependent part)
 *
 */

#ifndef __ASM_EVENT_H__
#define __ASM_EVENT_H__

static inline void evtchn_notify(struct vcpu *v)
{
    /*
     * NB1. 'vcpu_flags' and 'processor' must be checked /after/ update of
     * pending flag. These values may fluctuate (after all, we hold no
     * locks) but the key insight is that each change will cause
     * evtchn_upcall_pending to be polled.
     * 
     * NB2. We save VCPUF_running across the unblock to avoid a needless
     * IPI for domains that we IPI'd to unblock.
     */
    int running = test_bit(_VCPUF_running, &v->vcpu_flags);
    vcpu_unblock(v);
    if ( running )
        smp_send_event_check_cpu(v->processor);
}

/* Note: Bitwise operations result in fast code with no branches. */
#define event_pending(v)                        \
    (!!(v)->vcpu_info->evtchn_upcall_pending &  \
      !(v)->vcpu_info->evtchn_upcall_mask)

/* No arch specific virq definition now. Default to global. */
static inline int arch_virq_is_global(int virq)
{
    return 1;
}

#endif
