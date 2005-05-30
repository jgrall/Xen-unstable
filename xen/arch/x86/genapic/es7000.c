/*
 * APIC driver for the Unisys ES7000 chipset.
 */
#define APIC_DEFINITION 1
#include <xen/config.h>
#include <xen/cpumask.h>
#include <asm/mpspec.h>
#include <asm/genapic.h>
#include <asm/fixmap.h>
#include <asm/apicdef.h>
#include <asm/atomic.h>
#include <xen/kernel.h>
#include <xen/string.h>
#include <xen/smp.h>
#include <xen/init.h>
#include <asm/mach-es7000/mach_apicdef.h>
#include <asm/mach-es7000/mach_apic.h>
#include <asm/mach-es7000/mach_ipi.h>
#include <asm/mach-es7000/mach_mpparse.h>
#include <asm/mach-es7000/mach_wakecpu.h>

static __init int probe_es7000(void)
{
	/* probed later in mptable/ACPI hooks */
	return 0;
}

struct genapic apic_es7000 = APIC_INIT("es7000", probe_es7000);
