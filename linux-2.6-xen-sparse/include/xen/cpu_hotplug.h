#ifndef __XEN_CPU_HOTPLUG_H__
#define __XEN_CPU_HOTPLUG_H__

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/cpumask.h>

#if defined(CONFIG_HOTPLUG_CPU)

#if defined(CONFIG_X86)
void cpu_initialize_context(unsigned int cpu);
#else
#define cpu_initialize_context(cpu)	((void)0)
#endif

int cpu_up_is_allowed(unsigned int cpu);
void init_xenbus_allowed_cpumask(void);
int smp_suspend(void);
void smp_resume(void);

#else /* !defined(CONFIG_HOTPLUG_CPU) */

#define cpu_up_is_allowed(cpu)		(1)
#define init_xenbus_allowed_cpumask()	((void)0)

static inline int smp_suspend(void)
{
	if (num_online_cpus() > 1) {
		printk(KERN_WARNING "Can't suspend SMP guests "
		       "without CONFIG_HOTPLUG_CPU\n");
		return -EOPNOTSUPP;
	}
	return 0;
}

static inline void smp_resume(void)
{
}

#endif /* !defined(CONFIG_HOTPLUG_CPU) */

#endif /* __XEN_CPU_HOTPLUG_H__ */
