#ifndef __ASM_HARDIRQ_H
#define __ASM_HARDIRQ_H

#include <xen/config.h>
#include <xen/cache.h>

typedef struct {
	unsigned int __softirq_pending;
	unsigned int __local_irq_count;
	unsigned int __nmi_count;
	unsigned long idle_timestamp;
} __cacheline_aligned irq_cpustat_t;

#include <xen/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */

#define in_irq() (local_irq_count(smp_processor_id()) != 0)

#define irq_enter(cpu, irq)	(local_irq_count(cpu)++)
#define irq_exit(cpu, irq)	(local_irq_count(cpu)--)

#endif /* __ASM_HARDIRQ_H */
