#ifndef __XEN_SOFTIRQ_H__
#define __XEN_SOFTIRQ_H__

/* Common softirqs come first in the following list. */
#define AC_TIMER_SOFTIRQ                  0
#define SCHEDULE_SOFTIRQ                  1
#define NEW_TLBFLUSH_CLOCK_PERIOD_SOFTIRQ 2
#define KEYPRESS_SOFTIRQ                  3
#define NMI_SOFTIRQ                       4
#define PAGE_SCRUB_SOFTIRQ                5
#define DOMAIN_SHUTDOWN_FINALISE_SOFTIRQ  6
#define NR_SOFTIRQS                       7

#ifndef __ASSEMBLY__

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/smp.h>
#include <asm/bitops.h>
#include <asm/hardirq.h>

typedef void (*softirq_handler)(void);

asmlinkage void do_softirq(void);
extern void open_softirq(int nr, softirq_handler handler);

static inline void cpu_raise_softirq(unsigned int cpu, unsigned int nr)
{
    if ( !test_and_set_bit(nr, &softirq_pending(cpu)) )
        smp_send_event_check_cpu(cpu);
}

static inline void raise_softirq(unsigned int nr)
{
    set_bit(nr, &softirq_pending(smp_processor_id()));
}

#endif /* __ASSEMBLY__ */

#endif /* __XEN_SOFTIRQ_H__ */
