#ifndef __ASM_SMP_H
#define __ASM_SMP_H

/*
 * We need the APIC definitions automatically as part of 'smp.h'
 */
#ifndef __ASSEMBLY__
#include <xen/config.h>
#include <xen/kernel.h>
#include <xen/cpumask.h>
#include <asm/current.h>
#endif

#ifdef CONFIG_X86_LOCAL_APIC
#ifndef __ASSEMBLY__
#include <asm/fixmap.h>
#include <asm/bitops.h>
#include <asm/mpspec.h>
#ifdef CONFIG_X86_IO_APIC
#include <asm/io_apic.h>
#endif
#include <asm/apic.h>
#endif
#endif

#define BAD_APICID 0xFFu
#ifdef CONFIG_SMP
#ifndef __ASSEMBLY__

/*
 * Private routines/data
 */
 
extern void smp_alloc_memory(void);
extern int pic_mode;
extern int smp_num_siblings;
extern cpumask_t cpu_sibling_map[];

extern void smp_flush_tlb(void);
extern void smp_invalidate_rcv(void);		/* Process an NMI */
extern void (*mtrr_hook) (void);

#ifdef CONFIG_X86_64
extern void zap_low_mappings(void);
#else
extern void zap_low_mappings(l2_pgentry_t *base);
#endif

#define MAX_APICID 256
extern u8 x86_cpu_to_apicid[];

/*
 * This function is needed by all SMP systems. It must _always_ be valid
 * from the initial startup. We map APIC_BASE very early in page_setup(),
 * so this is correct in the x86 case.
 */
#define __smp_processor_id() (get_processor_id())

extern cpumask_t cpu_callout_map;
extern cpumask_t cpu_callin_map;
#define cpu_possible_map cpu_callout_map

/* We don't mark CPUs online until __cpu_up(), so we need another measure */
static inline int num_booting_cpus(void)
{
	return cpus_weight(cpu_callout_map);
}

extern void map_cpu_to_logical_apicid(void);
extern void unmap_cpu_to_logical_apicid(int cpu);

#ifdef CONFIG_X86_LOCAL_APIC

#ifdef APIC_DEFINITION
extern int hard_smp_processor_id(void);
#else
#include <mach_apicdef.h>
static inline int hard_smp_processor_id(void)
{
	/* we don't want to mark this access volatile - bad code generation */
	return GET_APIC_ID(*(unsigned int *)(APIC_BASE+APIC_ID));
}
#endif

static __inline int logical_smp_processor_id(void)
{
	/* we don't want to mark this access volatile - bad code generation */
	return GET_APIC_LOGICAL_ID(*(unsigned int *)(APIC_BASE+APIC_LDR));
}

#endif
#endif /* !__ASSEMBLY__ */

#define NO_PROC_ID		0xFF		/* No processor magic marker */

#endif
#endif
