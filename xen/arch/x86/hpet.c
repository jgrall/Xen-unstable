/******************************************************************************
 * arch/x86/hpet.c
 * 
 * HPET management.
 */

#include <xen/config.h>
#include <xen/errno.h>
#include <xen/time.h>
#include <xen/timer.h>
#include <xen/smp.h>
#include <xen/softirq.h>
#include <asm/fixmap.h>
#include <asm/div64.h>
#include <asm/hpet.h>

#define STIME_MAX ((s_time_t)((uint64_t)~0ull>>1))

#define MAX_DELTA_NS MILLISECS(10*1000)
#define MIN_DELTA_NS MICROSECS(20)

struct hpet_event_channel
{
    unsigned long mult;
    int           shift;
    s_time_t      next_event;
    cpumask_t     cpumask;
    spinlock_t    lock;
    void          (*event_handler)(struct hpet_event_channel *);
};
static struct hpet_event_channel hpet_event;

unsigned long hpet_address;

/*
 * Calculate a multiplication factor for scaled math, which is used to convert
 * nanoseconds based values to clock ticks:
 *
 * clock_ticks = (nanoseconds * factor) >> shift.
 *
 * div_sc is the rearranged equation to calculate a factor from a given clock
 * ticks / nanoseconds ratio:
 *
 * factor = (clock_ticks << shift) / nanoseconds
 */
static inline unsigned long div_sc(unsigned long ticks, unsigned long nsec,
                                   int shift)
{
    uint64_t tmp = ((uint64_t)ticks) << shift;

    do_div(tmp, nsec);
    return (unsigned long) tmp;
}

/*
 * Convert nanoseconds based values to clock ticks:
 *
 * clock_ticks = (nanoseconds * factor) >> shift.
 */
static inline unsigned long ns2ticks(unsigned long nsec, int shift,
                                     unsigned long factor)
{
    uint64_t tmp = ((uint64_t)nsec * factor) >> shift;

    return (unsigned long) tmp;
}

static int hpet_legacy_next_event(unsigned long delta)
{
    uint32_t cnt, cmp;
    unsigned long flags;

    local_irq_save(flags);
    cnt = hpet_read32(HPET_COUNTER);
    cmp = cnt + delta;
    hpet_write32(cmp, HPET_T0_CMP);
    cmp = hpet_read32(HPET_COUNTER);
    local_irq_restore(flags);

    /* Are we within two ticks of the deadline passing? Then we may miss. */
    return ((cmp + 2 - cnt) > delta) ? -ETIME : 0;
}

static int reprogram_hpet_evt_channel(
    struct hpet_event_channel *ch,
    s_time_t expire, s_time_t now, int force)
{
    int64_t delta;
    int ret;

    if ( unlikely(expire < 0) )
    {
        printk(KERN_DEBUG "reprogram: expire < 0\n");
        return -ETIME;
    }

    delta = expire - now;
    if ( delta <= 0 )
    {
        printk(KERN_DEBUG "reprogram: expire(%"PRIx64") < "
               "now(%"PRIx64")\n", expire, now);
        if ( !force )
            return -ETIME;
    }

    ch->next_event = expire;

    delta = min_t(int64_t, delta, MAX_DELTA_NS);
    delta = max_t(int64_t, delta, MIN_DELTA_NS);
    delta = ns2ticks(delta, ch->shift, ch->mult);

    ret = hpet_legacy_next_event(delta);
    while ( ret && force )
    {
        delta += delta;
        ret = hpet_legacy_next_event(delta);
    }

    return ret;
}

static int evt_do_broadcast(cpumask_t mask)
{
    int ret = 0, cpu = smp_processor_id();

    if ( cpu_isset(cpu, mask) )
    {
        cpu_clear(cpu, mask);
        raise_softirq(TIMER_SOFTIRQ);
        ret = 1;
    }

    if ( !cpus_empty(mask) )
    {
       cpumask_raise_softirq(mask, TIMER_SOFTIRQ);
       ret = 1;
    }
    return ret;
}

static void handle_hpet_broadcast(struct hpet_event_channel *ch)
{
    cpumask_t mask;
    s_time_t now, next_event;
    int cpu, current_cpu = smp_processor_id();

    spin_lock(&ch->lock);

    if ( cpu_isset(current_cpu, ch->cpumask) )
        printk(KERN_DEBUG "WARNING: current cpu%d in bc_mask\n", current_cpu);
again:
    ch->next_event = STIME_MAX;
    next_event = STIME_MAX;
    mask = (cpumask_t)CPU_MASK_NONE;
    now = NOW();

    /* find all expired events */
    for_each_cpu_mask(cpu, ch->cpumask)
    {
        if ( per_cpu(timer_deadline, cpu) <= now )
            cpu_set(cpu, mask);
        else if ( per_cpu(timer_deadline, cpu) < next_event )
            next_event = per_cpu(timer_deadline, cpu);
    }
    if ( per_cpu(timer_deadline, current_cpu) <= now )
        cpu_set(current_cpu, mask);

    /* wakeup the cpus which have an expired event. */
    evt_do_broadcast(mask);

    if ( next_event != STIME_MAX )
    {
        if ( reprogram_hpet_evt_channel(ch, next_event, now, 0) )
            goto again;
    }
    spin_unlock(&ch->lock);
}

void hpet_broadcast_init(void)
{
    u64 hpet_rate;
    u32 hpet_id, cfg;

    hpet_rate = hpet_setup();
    if ( hpet_rate == 0 )
        return;

    hpet_id = hpet_read32(HPET_ID);
    if ( !(hpet_id & HPET_ID_LEGSUP) )
        return;

    /* Start HPET legacy interrupts */
    cfg = hpet_read32(HPET_CFG);
    cfg |= HPET_CFG_LEGACY;
    hpet_write32(cfg, HPET_CFG);

    /* set HPET T0 as oneshot */
    cfg = hpet_read32(HPET_T0_CFG);
    cfg &= ~HPET_TN_PERIODIC;
    cfg |= HPET_TN_ENABLE | HPET_TN_32BIT;
    hpet_write32(cfg, HPET_T0_CFG);

    /*
     * The period is a femto seconds value. We need to calculate the scaled
     * math multiplication factor for nanosecond to hpet tick conversion.
     */
    hpet_event.mult = div_sc((unsigned long)hpet_rate, 1000000000ul, 32);
    hpet_event.shift = 32;
    hpet_event.next_event = STIME_MAX;
    hpet_event.event_handler = handle_hpet_broadcast;
    spin_lock_init(&hpet_event.lock);
}

void hpet_broadcast_enter(void)
{
    struct hpet_event_channel *ch = &hpet_event;

    cpu_set(smp_processor_id(), ch->cpumask);

    spin_lock(&ch->lock);

    /* reprogram if current cpu expire time is nearer */
    if ( this_cpu(timer_deadline) < ch->next_event )
        reprogram_hpet_evt_channel(ch, this_cpu(timer_deadline), NOW(), 1);

    spin_unlock(&ch->lock);
}

void hpet_broadcast_exit(void)
{
    struct hpet_event_channel *ch = &hpet_event;
    int cpu = smp_processor_id();

    if ( cpu_test_and_clear(cpu, ch->cpumask) )
        reprogram_timer(per_cpu(timer_deadline, cpu));
}

int hpet_legacy_irq_tick(void)
{
    if ( !hpet_event.event_handler )
        return 0;
    hpet_event.event_handler(&hpet_event);
    return 1;
}

u64 hpet_setup(void)
{
    static u64 hpet_rate;
    static int initialised;
    u32 hpet_id, hpet_period, cfg;
    int i;

    if ( initialised )
        return hpet_rate;
    initialised = 1;

    if ( hpet_address == 0 )
        return 0;

    set_fixmap_nocache(FIX_HPET_BASE, hpet_address);

    hpet_id = hpet_read32(HPET_ID);
    if ( hpet_id == 0 )
    {
        printk("BAD HPET vendor id.\n");
        return 0;
    }

    /* Check for sane period (100ps <= period <= 100ns). */
    hpet_period = hpet_read32(HPET_PERIOD);
    if ( (hpet_period > 100000000) || (hpet_period < 100000) )
    {
        printk("BAD HPET period %u.\n", hpet_period);
        return 0;
    }

    cfg = hpet_read32(HPET_CFG);
    cfg &= ~(HPET_CFG_ENABLE | HPET_CFG_LEGACY);
    hpet_write32(cfg, HPET_CFG);

    for ( i = 0; i <= ((hpet_id >> 8) & 31); i++ )
    {
        cfg = hpet_read32(HPET_T0_CFG + i*0x20);
        cfg &= ~HPET_TN_ENABLE;
        hpet_write32(cfg & ~HPET_TN_ENABLE, HPET_T0_CFG);
    }

    cfg = hpet_read32(HPET_CFG);
    cfg |= HPET_CFG_ENABLE;
    hpet_write32(cfg, HPET_CFG);

    hpet_rate = 1000000000000000ULL; /* 10^15 */
    (void)do_div(hpet_rate, hpet_period);

    return hpet_rate;
}
