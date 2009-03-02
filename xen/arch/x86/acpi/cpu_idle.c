/*
 * cpu_idle - xen idle state module derived from Linux 
 *            drivers/acpi/processor_idle.c & 
 *            arch/x86/kernel/acpi/cstate.c
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2004, 2005 Dominik Brodowski <linux@brodo.de>
 *  Copyright (C) 2004  Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>
 *                      - Added processor hotplug support
 *  Copyright (C) 2005  Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *                      - Added support for C3 on SMP
 *  Copyright (C) 2007, 2008 Intel Corporation
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <xen/config.h>
#include <xen/errno.h>
#include <xen/lib.h>
#include <xen/types.h>
#include <xen/acpi.h>
#include <xen/smp.h>
#include <xen/guest_access.h>
#include <xen/keyhandler.h>
#include <xen/cpuidle.h>
#include <xen/trace.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/hpet.h>
#include <asm/processor.h>
#include <public/platform.h>
#include <public/sysctl.h>

#define DEBUG_PM_CX

static void (*lapic_timer_off)(void);
static void (*lapic_timer_on)(void);

extern u32 pmtmr_ioport;
extern void (*pm_idle) (void);
extern void (*dead_idle) (void);

static void (*pm_idle_save) (void) __read_mostly;
unsigned int max_cstate __read_mostly = ACPI_PROCESSOR_MAX_POWER - 1;
integer_param("max_cstate", max_cstate);
static int local_apic_timer_c2_ok __read_mostly = 0;
boolean_param("lapic_timer_c2_ok", local_apic_timer_c2_ok);

static struct acpi_processor_power *__read_mostly processor_powers[NR_CPUS];

static void print_acpi_power(uint32_t cpu, struct acpi_processor_power *power)
{
    uint32_t i, idle_usage = 0;
    uint64_t res, idle_res = 0;

    printk("==cpu%d==\n", cpu);
    printk("active state:\t\tC%d\n",
           power->last_state ? power->last_state->idx : -1);
    printk("max_cstate:\t\tC%d\n", max_cstate);
    printk("states:\n");
    
    for ( i = 1; i < power->count; i++ )
    {
        res = acpi_pm_tick_to_ns(power->states[i].time);
        idle_usage += power->states[i].usage;
        idle_res += res;

        printk((power->last_state && power->last_state->idx == i) ?
               "   *" : "    ");
        printk("C%d:\t", i);
        printk("type[C%d] ", power->states[i].type);
        printk("latency[%03d] ", power->states[i].latency);
        printk("usage[%08d] ", power->states[i].usage);
        printk("duration[%"PRId64"]\n", res);
    }
    printk("    C0:\tusage[%08d] duration[%"PRId64"]\n",
           idle_usage, NOW() - idle_res);

}

static void dump_cx(unsigned char key)
{
    unsigned int cpu;

    for_each_online_cpu ( cpu )
        if (processor_powers[cpu])
            print_acpi_power(cpu, processor_powers[cpu]);
}

static int __init cpu_idle_key_init(void)
{
    register_keyhandler(
        'c', dump_cx,        "dump cx structures");
    return 0;
}
__initcall(cpu_idle_key_init);

static inline u32 ticks_elapsed(u32 t1, u32 t2)
{
    if ( t2 >= t1 )
        return (t2 - t1);
    else if ( !(acpi_gbl_FADT.flags & ACPI_FADT_32BIT_TIMER) )
        return (((0x00FFFFFF - t1) + t2) & 0x00FFFFFF);
    else
        return ((0xFFFFFFFF - t1) + t2);
}

static void acpi_safe_halt(void)
{
    smp_mb__after_clear_bit();
    safe_halt();
}

#define MWAIT_ECX_INTERRUPT_BREAK   (0x1)

static void mwait_idle_with_hints(unsigned long eax, unsigned long ecx)
{
    __monitor((void *)current, 0, 0);
    smp_mb();
    __mwait(eax, ecx);
}

static void acpi_processor_ffh_cstate_enter(struct acpi_processor_cx *cx)
{
    mwait_idle_with_hints(cx->address, MWAIT_ECX_INTERRUPT_BREAK);
}

static void acpi_idle_do_entry(struct acpi_processor_cx *cx)
{
    int unused;

    switch ( cx->entry_method )
    {
    case ACPI_CSTATE_EM_FFH:
        /* Call into architectural FFH based C-state */
        acpi_processor_ffh_cstate_enter(cx);
        return;
    case ACPI_CSTATE_EM_SYSIO:
        /* IO port based C-state */
        inb(cx->address);
        /* Dummy wait op - must do something useless after P_LVL2 read
           because chipsets cannot guarantee that STPCLK# signal
           gets asserted in time to freeze execution properly. */
        unused = inl(pmtmr_ioport);
        return;
    case ACPI_CSTATE_EM_HALT:
        acpi_safe_halt();
        local_irq_disable();
        return;
    }
}

static inline void acpi_idle_update_bm_rld(struct acpi_processor_power *power,
                                           struct acpi_processor_cx *target)
{
    if ( !power->flags.bm_check )
        return;

    if ( power->flags.bm_rld_set && target->type != ACPI_STATE_C3 )
    {
        acpi_set_register(ACPI_BITREG_BUS_MASTER_RLD, 0);
        power->flags.bm_rld_set = 0;
    }

    if ( !power->flags.bm_rld_set && target->type == ACPI_STATE_C3 )
    {
        acpi_set_register(ACPI_BITREG_BUS_MASTER_RLD, 1);
        power->flags.bm_rld_set = 1;
    }
}

static int acpi_idle_bm_check(void)
{
    u32 bm_status = 0;

    acpi_get_register(ACPI_BITREG_BUS_MASTER_STATUS, &bm_status);
    if ( bm_status )
        acpi_set_register(ACPI_BITREG_BUS_MASTER_STATUS, 1);
    /*
     * TBD: PIIX4 Erratum #18: Note that BM_STS doesn't always reflect
     * the true state of bus mastering activity; forcing us to
     * manually check the BMIDEA bit of each IDE channel.
     */
    return bm_status;
}

static struct {
    spinlock_t lock;
    unsigned int count;
} c3_cpu_status = { .lock = SPIN_LOCK_UNLOCKED };

static void acpi_processor_idle(void)
{
    struct acpi_processor_power *power = processor_powers[smp_processor_id()];
    struct acpi_processor_cx *cx = NULL;
    int next_state;
    int sleep_ticks = 0;
    u32 t1, t2 = 0;

    /*
     * Interrupts must be disabled during bus mastering calculations and
     * for C2/C3 transitions.
     */
    local_irq_disable();

    if ( softirq_pending(smp_processor_id()) )
    {
        local_irq_enable();
        return;
    }

    next_state = power ? cpuidle_current_governor->select(power) : -1;
    if ( next_state > 0 )
    {
        cx = &power->states[next_state];
        if ( power->flags.bm_check && acpi_idle_bm_check()
             && cx->type == ACPI_STATE_C3 )
            cx = power->safe_state;
        if ( cx->idx > max_cstate )
            cx = &power->states[max_cstate];
    }
    if ( !cx )
    {
        if ( pm_idle_save )
        {
            printk(XENLOG_DEBUG "call pm_idle_save()\n");
            pm_idle_save();
        }
        else
        {
            printk(XENLOG_DEBUG "call acpi_safe_halt()\n");
            acpi_safe_halt();
        }
        return;
    }

    power->last_state = cx;

    /*
     * Sleep:
     * ------
     * Invoke the current Cx state to put the processor to sleep.
     */
    acpi_idle_update_bm_rld(power, cx);

    switch ( cx->type )
    {
    case ACPI_STATE_C1:
    case ACPI_STATE_C2:
        if ( cx->type == ACPI_STATE_C1 || local_apic_timer_c2_ok )
        {
            /* Trace cpu idle entry */
            TRACE_1D(TRC_PM_IDLE_ENTRY, cx->idx);
            /* Get start time (ticks) */
            t1 = inl(pmtmr_ioport);
            /* Invoke C2 */
            acpi_idle_do_entry(cx);
            /* Get end time (ticks) */
            t2 = inl(pmtmr_ioport);
            /* Trace cpu idle exit */
            TRACE_1D(TRC_PM_IDLE_EXIT, cx->idx);

            /* Re-enable interrupts */
            local_irq_enable();
            /* Compute time (ticks) that we were actually asleep */
            sleep_ticks = ticks_elapsed(t1, t2);
            break;
        }

    case ACPI_STATE_C3:
        /*
         * disable bus master
         * bm_check implies we need ARB_DIS
         * !bm_check implies we need cache flush
         * bm_control implies whether we can do ARB_DIS
         *
         * That leaves a case where bm_check is set and bm_control is
         * not set. In that case we cannot do much, we enter C3
         * without doing anything.
         */
        if ( power->flags.bm_check && power->flags.bm_control )
        {
            spin_lock(&c3_cpu_status.lock);
            if ( ++c3_cpu_status.count == num_online_cpus() )
            {
                /*
                 * All CPUs are trying to go to C3
                 * Disable bus master arbitration
                 */
                acpi_set_register(ACPI_BITREG_ARB_DISABLE, 1);
            }
            spin_unlock(&c3_cpu_status.lock);
        }
        else if ( !power->flags.bm_check )
        {
            /* SMP with no shared cache... Invalidate cache  */
            ACPI_FLUSH_CPU_CACHE();
        }

        /* Trace cpu idle entry */
        TRACE_1D(TRC_PM_IDLE_ENTRY, cx->idx);
        /*
         * Before invoking C3, be aware that TSC/APIC timer may be 
         * stopped by H/W. Without carefully handling of TSC/APIC stop issues,
         * deep C state can't work correctly.
         */
        /* preparing APIC stop */
        lapic_timer_off();

        /* Get start time (ticks) */
        t1 = inl(pmtmr_ioport);
        /* Invoke C3 */
        acpi_idle_do_entry(cx);
        /* Get end time (ticks) */
        t2 = inl(pmtmr_ioport);

        /* recovering TSC */
        cstate_restore_tsc();
        /* Trace cpu idle exit */
        TRACE_1D(TRC_PM_IDLE_EXIT, cx->idx);

        if ( power->flags.bm_check && power->flags.bm_control )
        {
            /* Enable bus master arbitration */
            spin_lock(&c3_cpu_status.lock);
            if ( c3_cpu_status.count-- == num_online_cpus() )
                acpi_set_register(ACPI_BITREG_ARB_DISABLE, 0);
            spin_unlock(&c3_cpu_status.lock);
        }

        /* Re-enable interrupts */
        local_irq_enable();
        /* recovering APIC */
        lapic_timer_on();
        /* Compute time (ticks) that we were actually asleep */
        sleep_ticks = ticks_elapsed(t1, t2);

        break;

    default:
        local_irq_enable();
        return;
    }

    cx->usage++;
    if ( sleep_ticks > 0 )
    {
        power->last_residency = acpi_pm_tick_to_ns(sleep_ticks) / 1000UL;
        cx->time += sleep_ticks;
    }

    if ( cpuidle_current_governor->reflect )
        cpuidle_current_governor->reflect(power);
}

static void acpi_dead_idle(void)
{
    struct acpi_processor_power *power;
    struct acpi_processor_cx *cx;
    int unused;

    if ( (power = processor_powers[smp_processor_id()]) == NULL )
        goto default_halt;

    if ( (cx = &power->states[power->count-1]) == NULL )
        goto default_halt;

    for ( ; ; )
    {
        if ( !power->flags.bm_check && cx->type == ACPI_STATE_C3 )
            ACPI_FLUSH_CPU_CACHE();

        switch ( cx->entry_method )
        {
            case ACPI_CSTATE_EM_FFH:
                /* Not treat interrupt as break event */
                mwait_idle_with_hints(cx->address, 0);
                break;
            case ACPI_CSTATE_EM_SYSIO:
                inb(cx->address);
                unused = inl(pmtmr_ioport);
                break;
            default:
                goto default_halt;
        }
    }

default_halt:
    for ( ; ; )
        halt();
}

static int init_cx_pminfo(struct acpi_processor_power *acpi_power)
{
    int i;

    memset(acpi_power, 0, sizeof(*acpi_power));

    for ( i = 0; i < ACPI_PROCESSOR_MAX_POWER; i++ )
        acpi_power->states[i].idx = i;

    acpi_power->states[ACPI_STATE_C1].type = ACPI_STATE_C1;
    acpi_power->states[ACPI_STATE_C1].entry_method = ACPI_CSTATE_EM_HALT;

    acpi_power->states[ACPI_STATE_C0].valid = 1;
    acpi_power->states[ACPI_STATE_C1].valid = 1;

    acpi_power->count = 2;

    return 0;
}

#define CPUID_MWAIT_LEAF (5)
#define CPUID5_ECX_EXTENSIONS_SUPPORTED (0x1)
#define CPUID5_ECX_INTERRUPT_BREAK      (0x2)

#define MWAIT_ECX_INTERRUPT_BREAK       (0x1)

#define MWAIT_SUBSTATE_MASK (0xf)
#define MWAIT_SUBSTATE_SIZE (4)

static int acpi_processor_ffh_cstate_probe(xen_processor_cx_t *cx)
{
    struct cpuinfo_x86 *c = &current_cpu_data;
    unsigned int eax, ebx, ecx, edx;
    unsigned int edx_part;
    unsigned int cstate_type; /* C-state type and not ACPI C-state type */
    unsigned int num_cstate_subtype;

    if ( c->cpuid_level < CPUID_MWAIT_LEAF )
    {
        printk(XENLOG_INFO "MWAIT leaf not supported by cpuid\n");
        return -EFAULT;
    }

    cpuid(CPUID_MWAIT_LEAF, &eax, &ebx, &ecx, &edx);
    printk(XENLOG_DEBUG "cpuid.MWAIT[.eax=%x, .ebx=%x, .ecx=%x, .edx=%x]\n",
           eax, ebx, ecx, edx);

    /* Check whether this particular cx_type (in CST) is supported or not */
    cstate_type = (cx->reg.address >> MWAIT_SUBSTATE_SIZE) + 1;
    edx_part = edx >> (cstate_type * MWAIT_SUBSTATE_SIZE);
    num_cstate_subtype = edx_part & MWAIT_SUBSTATE_MASK;

    if ( num_cstate_subtype < (cx->reg.address & MWAIT_SUBSTATE_MASK) )
        return -EFAULT;

    /* mwait ecx extensions INTERRUPT_BREAK should be supported for C2/C3 */
    if ( !(ecx & CPUID5_ECX_EXTENSIONS_SUPPORTED) ||
         !(ecx & CPUID5_ECX_INTERRUPT_BREAK) )
        return -EFAULT;

    printk(XENLOG_INFO "Monitor-Mwait will be used to enter C-%d state\n", cx->type);
    return 0;
}

/*
 * Initialize bm_flags based on the CPU cache properties
 * On SMP it depends on cache configuration
 * - When cache is not shared among all CPUs, we flush cache
 *   before entering C3.
 * - When cache is shared among all CPUs, we use bm_check
 *   mechanism as in UP case
 *
 * This routine is called only after all the CPUs are online
 */
static void acpi_processor_power_init_bm_check(struct acpi_processor_flags *flags)
{
    struct cpuinfo_x86 *c = &current_cpu_data;

    flags->bm_check = 0;
    if ( num_online_cpus() == 1 )
        flags->bm_check = 1;
    else if ( c->x86_vendor == X86_VENDOR_INTEL )
    {
        /*
         * Today all CPUs that support C3 share cache.
         * TBD: This needs to look at cache shared map, once
         * multi-core detection patch makes to the base.
         */
        flags->bm_check = 1;
    }
}

#define VENDOR_INTEL                   (1)
#define NATIVE_CSTATE_BEYOND_HALT      (2)

static int check_cx(struct acpi_processor_power *power, xen_processor_cx_t *cx)
{
    static int bm_check_flag;

    switch ( cx->reg.space_id )
    {
    case ACPI_ADR_SPACE_SYSTEM_IO:
        if ( cx->reg.address == 0 )
            return -EINVAL;
        break;

    case ACPI_ADR_SPACE_FIXED_HARDWARE:
        if ( cx->reg.bit_width != VENDOR_INTEL || 
             cx->reg.bit_offset != NATIVE_CSTATE_BEYOND_HALT )
            return -EINVAL;

        /* assume all logical cpu has the same support for mwait */
        if ( acpi_processor_ffh_cstate_probe(cx) )
            return -EINVAL;
        break;

    default:
        return -ENODEV;
    }

    switch ( cx->type )
    {
    case ACPI_STATE_C2:
        if ( local_apic_timer_c2_ok )
            break;
    case ACPI_STATE_C3:
        /* We must be able to use HPET in place of LAPIC timers. */
        if ( hpet_broadcast_is_available() )
        {
            lapic_timer_off = hpet_broadcast_enter;
            lapic_timer_on = hpet_broadcast_exit;
        }
        else if ( pit_broadcast_is_available() )
        {
            lapic_timer_off = pit_broadcast_enter;
            lapic_timer_on = pit_broadcast_exit;
        }
        else
        {
            return -EINVAL;
        }

        /* All the logic here assumes flags.bm_check is same across all CPUs */
        if ( !bm_check_flag )
        {
            /* Determine whether bm_check is needed based on CPU  */
            acpi_processor_power_init_bm_check(&(power->flags));
            bm_check_flag = power->flags.bm_check;
        }
        else
        {
            power->flags.bm_check = bm_check_flag;
        }

        if ( power->flags.bm_check )
        {
            if ( !power->flags.bm_control )
            {
                if ( power->flags.has_cst != 1 )
                {
                    /* bus mastering control is necessary */
                    ACPI_DEBUG_PRINT((ACPI_DB_INFO,
                        "C3 support requires BM control\n"));
                    return -EINVAL;
                }
                else
                {
                    /* Here we enter C3 without bus mastering */
                    ACPI_DEBUG_PRINT((ACPI_DB_INFO,
                        "C3 support without BM control\n"));
                }
            }
        }
        else
        {
            /*
             * WBINVD should be set in fadt, for C3 state to be
             * supported on when bm_check is not required.
             */
            if ( !(acpi_gbl_FADT.flags & ACPI_FADT_WBINVD) )
            {
                ACPI_DEBUG_PRINT((ACPI_DB_INFO,
                          "Cache invalidation should work properly"
                          " for C3 to be enabled on SMP systems\n"));
                return -EINVAL;
            }
            acpi_set_register(ACPI_BITREG_BUS_MASTER_RLD, 0);
        }

        break;
    }

    return 0;
}

static unsigned int latency_factor = 2;

static void set_cx(
    struct acpi_processor_power *acpi_power,
    xen_processor_cx_t *xen_cx)
{
    struct acpi_processor_cx *cx;

    if ( check_cx(acpi_power, xen_cx) != 0 )
        return;

    if ( xen_cx->type == ACPI_STATE_C1 )
        cx = &acpi_power->states[1];
    else
        cx = &acpi_power->states[acpi_power->count];

    if ( !cx->valid )
        acpi_power->count++;

    cx->valid    = 1;
    cx->type     = xen_cx->type;
    cx->address  = xen_cx->reg.address;

    switch ( xen_cx->reg.space_id )
    {
    case ACPI_ADR_SPACE_FIXED_HARDWARE:
        if ( xen_cx->reg.bit_width == VENDOR_INTEL &&
             xen_cx->reg.bit_offset == NATIVE_CSTATE_BEYOND_HALT )
            cx->entry_method = ACPI_CSTATE_EM_FFH;
        else
            cx->entry_method = ACPI_CSTATE_EM_HALT;
        break;
    case ACPI_ADR_SPACE_SYSTEM_IO:
        cx->entry_method = ACPI_CSTATE_EM_SYSIO;
        break;
    default:
        cx->entry_method = ACPI_CSTATE_EM_NONE;
    }

    cx->latency  = xen_cx->latency;
    cx->power    = xen_cx->power;
    
    cx->latency_ticks = ns_to_acpi_pm_tick(cx->latency * 1000UL);
    cx->target_residency = cx->latency * latency_factor;
    if ( cx->type == ACPI_STATE_C1 || cx->type == ACPI_STATE_C2 )
        acpi_power->safe_state = cx;
}

int get_cpu_id(u8 acpi_id)
{
    int i;
    u8 apic_id;

    apic_id = x86_acpiid_to_apicid[acpi_id];
    if ( apic_id == 0xff )
        return -1;

    for ( i = 0; i < NR_CPUS; i++ )
    {
        if ( apic_id == x86_cpu_to_apicid[i] )
            return i;
    }

    return -1;
}

#ifdef DEBUG_PM_CX
static void print_cx_pminfo(uint32_t cpu, struct xen_processor_power *power)
{
    XEN_GUEST_HANDLE(xen_processor_cx_t) states;
    xen_processor_cx_t  state;
    XEN_GUEST_HANDLE(xen_processor_csd_t) csd;
    xen_processor_csd_t dp;
    uint32_t i;

    printk("cpu%d cx acpi info:\n", cpu);
    printk("\tcount = %d\n", power->count);
    printk("\tflags: bm_cntl[%d], bm_chk[%d], has_cst[%d],\n"
           "\t       pwr_setup_done[%d], bm_rld_set[%d]\n",
           power->flags.bm_control, power->flags.bm_check, power->flags.has_cst,
           power->flags.power_setup_done, power->flags.bm_rld_set);
    
    states = power->states;
    
    for ( i = 0; i < power->count; i++ )
    {
        if ( unlikely(copy_from_guest_offset(&state, states, i, 1)) )
            return;
        
        printk("\tstates[%d]:\n", i);
        printk("\t\treg.space_id = 0x%x\n", state.reg.space_id);
        printk("\t\treg.bit_width = 0x%x\n", state.reg.bit_width);
        printk("\t\treg.bit_offset = 0x%x\n", state.reg.bit_offset);
        printk("\t\treg.access_size = 0x%x\n", state.reg.access_size);
        printk("\t\treg.address = 0x%"PRIx64"\n", state.reg.address);
        printk("\t\ttype    = %d\n", state.type);
        printk("\t\tlatency = %d\n", state.latency);
        printk("\t\tpower   = %d\n", state.power);

        csd = state.dp;
        printk("\t\tdp(@0x%p)\n", csd.p);
        
        if ( csd.p != NULL )
        {
            if ( unlikely(copy_from_guest(&dp, csd, 1)) )
                return;
            printk("\t\t\tdomain = %d\n", dp.domain);
            printk("\t\t\tcoord_type   = %d\n", dp.coord_type);
            printk("\t\t\tnum = %d\n", dp.num);
        }
    }
}
#else
#define print_cx_pminfo(c, p)
#endif

long set_cx_pminfo(uint32_t cpu, struct xen_processor_power *power)
{
    XEN_GUEST_HANDLE(xen_processor_cx_t) states;
    xen_processor_cx_t xen_cx;
    struct acpi_processor_power *acpi_power;
    int cpu_id, i;

    if ( unlikely(!guest_handle_okay(power->states, power->count)) )
        return -EFAULT;

    print_cx_pminfo(cpu, power);

    /* map from acpi_id to cpu_id */
    cpu_id = get_cpu_id((u8)cpu);
    if ( cpu_id == -1 )
    {
        printk(XENLOG_ERR "no cpu_id for acpi_id %d\n", cpu);
        return -EFAULT;
    }

    acpi_power = processor_powers[cpu_id];
    if ( !acpi_power )
    {
        acpi_power = xmalloc(struct acpi_processor_power);
        if ( !acpi_power )
            return -ENOMEM;
        memset(acpi_power, 0, sizeof(*acpi_power));
        processor_powers[cpu_id] = acpi_power;
    }

    init_cx_pminfo(acpi_power);

    acpi_power->cpu = cpu_id;
    acpi_power->flags.bm_check = power->flags.bm_check;
    acpi_power->flags.bm_control = power->flags.bm_control;
    acpi_power->flags.has_cst = power->flags.has_cst;

    states = power->states;

    for ( i = 0; i < power->count; i++ )
    {
        if ( unlikely(copy_from_guest_offset(&xen_cx, states, i, 1)) )
            return -EFAULT;

        set_cx(acpi_power, &xen_cx);
    }

    if ( cpuidle_current_governor->enable &&
         cpuidle_current_governor->enable(acpi_power) )
        return -EFAULT;

    /* FIXME: C-state dependency is not supported by far */

    print_acpi_power(cpu_id, acpi_power);

    if ( cpu_id == 0 && pm_idle_save == NULL )
    {
        pm_idle_save = pm_idle;
        pm_idle = acpi_processor_idle;
    }

    if ( cpu_id == 0 )
    {
        dead_idle = acpi_dead_idle;
    }
        
    return 0;
}

uint32_t pmstat_get_cx_nr(uint32_t cpuid)
{
    return processor_powers[cpuid] ? processor_powers[cpuid]->count : 0;
}

int pmstat_get_cx_stat(uint32_t cpuid, struct pm_cx_stat *stat)
{
    const struct acpi_processor_power *power = processor_powers[cpuid];
    uint64_t usage, res, idle_usage = 0, idle_res = 0;
    int i;

    if ( power == NULL )
    {
        stat->last = 0;
        stat->nr = 0;
        stat->idle_time = 0;
        return 0;
    }

    stat->last = power->last_state ? power->last_state->idx : 0;
    stat->nr = power->count;
    stat->idle_time = get_cpu_idle_time(cpuid);

    for ( i = power->count - 1; i >= 0; i-- )
    {
        if ( i != 0 )
        {
            usage = power->states[i].usage;
            res = acpi_pm_tick_to_ns(power->states[i].time);
            idle_usage += usage;
            idle_res += res;
        }
        else
        {
            usage = idle_usage;
            res = NOW() - idle_res;
        }
        if ( copy_to_guest_offset(stat->triggers, i, &usage, 1) ||
             copy_to_guest_offset(stat->residencies, i, &res, 1) )
            return -EFAULT;
    }

    return 0;
}

int pmstat_reset_cx_stat(uint32_t cpuid)
{
    return 0;
}

