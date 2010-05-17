#ifndef __ASM_MACH_WAKECPU_H
#define __ASM_MACH_WAKECPU_H

/* 
 * This file copes with machines that wakeup secondary CPUs by the
 * INIT, INIT, STARTUP sequence.
 */

#define WAKE_SECONDARY_VIA_INIT

#define TRAMPOLINE_LOW maddr_to_virt(0x467)
#define TRAMPOLINE_HIGH maddr_to_virt(0x469)

#define boot_cpu_apicid boot_cpu_physical_apicid

static inline void wait_for_init_deassert(atomic_t *deassert)
{
	while (!atomic_read(deassert));
	return;
}

/* Nothing to do for most platforms, since cleared by the INIT cycle */
static inline void smp_callin_clear_local_apic(void)
{
}

#if APIC_DEBUG
 #define inquire_remote_apic(apicid) __inquire_remote_apic(apicid)
#else
 #define inquire_remote_apic(apicid) {}
#endif

#endif /* __ASM_MACH_WAKECPU_H */
