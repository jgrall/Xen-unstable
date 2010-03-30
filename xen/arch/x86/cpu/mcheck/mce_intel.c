#include <xen/init.h>
#include <xen/types.h>
#include <xen/irq.h>
#include <xen/event.h>
#include <xen/kernel.h>
#include <xen/delay.h>
#include <xen/smp.h>
#include <xen/mm.h>
#include <asm/processor.h> 
#include <public/sysctl.h>
#include <asm/system.h>
#include <asm/msr.h>
#include <asm/p2m.h>
#include "mce.h"
#include "x86_mca.h"

DEFINE_PER_CPU(cpu_banks_t, mce_banks_owned);
DEFINE_PER_CPU(cpu_banks_t, no_cmci_banks);
int cmci_support = 0;
int ser_support = 0;

static int nr_intel_ext_msrs = 0;

/* Below are for MCE handling */
struct mce_softirq_barrier {
	atomic_t val;
	atomic_t ingen;
	atomic_t outgen;
};

static struct mce_softirq_barrier mce_inside_bar, mce_severity_bar;
static struct mce_softirq_barrier mce_trap_bar;

/*
 * mce_logout_lock should only be used in the trap handler,
 * while MCIP has not been cleared yet in the global status
 * register. Other use is not safe, since an MCE trap can
 * happen at any moment, which would cause lock recursion.
 */
static DEFINE_SPINLOCK(mce_logout_lock);

static atomic_t severity_cpu = ATOMIC_INIT(-1);
static atomic_t found_error = ATOMIC_INIT(0);

static void mce_barrier_enter(struct mce_softirq_barrier *);
static void mce_barrier_exit(struct mce_softirq_barrier *);

#ifdef CONFIG_X86_MCE_THERMAL
static void unexpected_thermal_interrupt(struct cpu_user_regs *regs)
{
    printk(KERN_ERR "Thermal: CPU%d: Unexpected LVT TMR interrupt!\n",
                smp_processor_id());
    add_taint(TAINT_MACHINE_CHECK);
}

/* P4/Xeon Thermal transition interrupt handler */
static void intel_thermal_interrupt(struct cpu_user_regs *regs)
{
    u32 l, h;
    unsigned int cpu = smp_processor_id();
    static s_time_t next[NR_CPUS];

    ack_APIC_irq();
    if (NOW() < next[cpu])
        return;

    next[cpu] = NOW() + MILLISECS(5000);
    rdmsr(MSR_IA32_THERM_STATUS, l, h);
    if (l & 0x1) {
        printk(KERN_EMERG "CPU%d: Temperature above threshold\n", cpu);
        printk(KERN_EMERG "CPU%d: Running in modulated clock mode\n",
                cpu);
        add_taint(TAINT_MACHINE_CHECK);
    } else {
        printk(KERN_INFO "CPU%d: Temperature/speed normal\n", cpu);
    }
}

/* Thermal interrupt handler for this CPU setup */
static void (*vendor_thermal_interrupt)(struct cpu_user_regs *regs) 
        = unexpected_thermal_interrupt;

fastcall void smp_thermal_interrupt(struct cpu_user_regs *regs)
{
    struct cpu_user_regs *old_regs = set_irq_regs(regs);
    irq_enter();
    vendor_thermal_interrupt(regs);
    irq_exit();
    set_irq_regs(old_regs);
}

/* P4/Xeon Thermal regulation detect and init */
static void intel_init_thermal(struct cpuinfo_x86 *c)
{
    u32 l, h;
    int tm2 = 0;
    unsigned int cpu = smp_processor_id();

    /* Thermal monitoring */
    if (!cpu_has(c, X86_FEATURE_ACPI))
        return; /* -ENODEV */

    /* Clock modulation */
    if (!cpu_has(c, X86_FEATURE_ACC))
        return; /* -ENODEV */

    /* first check if its enabled already, in which case there might
     * be some SMM goo which handles it, so we can't even put a handler
     * since it might be delivered via SMI already -zwanem.
     */
    rdmsr (MSR_IA32_MISC_ENABLE, l, h);
    h = apic_read(APIC_LVTTHMR);
    if ((l & (1<<3)) && (h & APIC_DM_SMI)) {
        printk(KERN_DEBUG "CPU%d: Thermal monitoring handled by SMI\n",cpu);
        return; /* -EBUSY */
    }

    if (cpu_has(c, X86_FEATURE_TM2) && (l & (1 << 13)))
        tm2 = 1;

    /* check whether a vector already exists, temporarily masked? */
    if (h & APIC_VECTOR_MASK) {
        printk(KERN_DEBUG "CPU%d: Thermal LVT vector (%#x) already installed\n",
                 cpu, (h & APIC_VECTOR_MASK));
        return; /* -EBUSY */
    }

    /* The temperature transition interrupt handler setup */
    h = THERMAL_APIC_VECTOR;    /* our delivery vector */
    h |= (APIC_DM_FIXED | APIC_LVT_MASKED);  /* we'll mask till we're ready */
    apic_write_around(APIC_LVTTHMR, h);

    rdmsr (MSR_IA32_THERM_INTERRUPT, l, h);
    wrmsr (MSR_IA32_THERM_INTERRUPT, l | 0x03 , h);

    /* ok we're good to go... */
    vendor_thermal_interrupt = intel_thermal_interrupt;

    rdmsr (MSR_IA32_MISC_ENABLE, l, h);
    wrmsr (MSR_IA32_MISC_ENABLE, l | (1<<3), h);

    l = apic_read (APIC_LVTTHMR);
    apic_write_around (APIC_LVTTHMR, l & ~APIC_LVT_MASKED);
    if (opt_cpu_info)
        printk(KERN_INFO "CPU%u: Thermal monitoring enabled (%s)\n",
                cpu, tm2 ? "TM2" : "TM1");
    return;
}
#endif /* CONFIG_X86_MCE_THERMAL */

static inline void intel_get_extended_msr(struct mcinfo_extended *ext, u32 msr)
{
    if ( ext->mc_msrs < ARRAY_SIZE(ext->mc_msr)
         && msr < MSR_IA32_MCG_EAX + nr_intel_ext_msrs ) {
        ext->mc_msr[ext->mc_msrs].reg = msr;
        mca_rdmsrl(msr, ext->mc_msr[ext->mc_msrs].value);
        ++ext->mc_msrs;
    }
}

static enum mca_extinfo
intel_get_extended_msrs(struct mc_info *mci, uint16_t bank, uint64_t status)
{
    struct mcinfo_extended mc_ext;

    if (mci == NULL || nr_intel_ext_msrs == 0 || !(status & MCG_STATUS_EIPV))
        return MCA_EXTINFO_IGNORED;

    /* this function will called when CAP(9).MCG_EXT_P = 1 */
    memset(&mc_ext, 0, sizeof(struct mcinfo_extended));
    mc_ext.common.type = MC_TYPE_EXTENDED;
    mc_ext.common.size = sizeof(mc_ext);

    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EAX);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EBX);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_ECX);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EDX);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_ESI);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EDI);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EBP);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_ESP);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EFLAGS);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_EIP);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_MISC);

#ifdef __x86_64__
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R8);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R9);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R10);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R11);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R12);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R13);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R14);
    intel_get_extended_msr(&mc_ext, MSR_IA32_MCG_R15);
#endif

    x86_mcinfo_add(mci, &mc_ext);

    return MCA_EXTINFO_GLOBAL;
}

/* This node list records errors impacting a domain. when one
 * MCE# happens, one error bank impacts a domain. This error node
 * will be inserted to the tail of the per_dom data for vMCE# MSR
 * virtualization. When one vMCE# injection is finished processing
 * processed by guest, the corresponding node will be deleted. 
 * This node list is for GUEST vMCE# MSRS virtualization.
 */
static struct bank_entry* alloc_bank_entry(void) {
    struct bank_entry *entry;

    entry = xmalloc(struct bank_entry);
    if (!entry) {
        printk(KERN_ERR "MCE: malloc bank_entry failed\n");
        return NULL;
    }
    memset(entry, 0x0, sizeof(entry));
    INIT_LIST_HEAD(&entry->list);
    return entry;
}

/* Fill error bank info for #vMCE injection and GUEST vMCE#
 * MSR virtualization data
 * 1) Log down how many nr_injections of the impacted.
 * 2) Copy MCE# error bank to impacted DOM node list, 
      for vMCE# MSRs virtualization
*/

static int fill_vmsr_data(struct mcinfo_bank *mc_bank, struct domain *d,
        uint64_t gstatus) {
    struct bank_entry *entry;

    /* This error bank impacts one domain, we need to fill domain related
     * data for vMCE MSRs virtualization and vMCE# injection */
    if (mc_bank->mc_domid != (uint16_t)~0) {
        /* For HVM guest, Only when first vMCE is consumed by HVM guest successfully,
         * will we generete another node and inject another vMCE
         */
        if ( (d->is_hvm) && (d->arch.vmca_msrs.nr_injection > 0) )
        {
            mce_printk(MCE_QUIET, "MCE: HVM guest has not handled previous"
                        " vMCE yet!\n");
            return -1;
        }
        entry = alloc_bank_entry();
        if (entry == NULL)
            return -1;

        entry->mci_status = mc_bank->mc_status;
        entry->mci_addr = mc_bank->mc_addr;
        entry->mci_misc = mc_bank->mc_misc;
        entry->bank = mc_bank->mc_bank;

        spin_lock(&d->arch.vmca_msrs.lock);
        /* New error Node, insert to the tail of the per_dom data */
        list_add_tail(&entry->list, &d->arch.vmca_msrs.impact_header);
        /* Fill MSR global status */
        d->arch.vmca_msrs.mcg_status = gstatus;
        /* New node impact the domain, need another vMCE# injection*/
        d->arch.vmca_msrs.nr_injection++;
        spin_unlock(&d->arch.vmca_msrs.lock);

        mce_printk(MCE_VERBOSE,"MCE: Found error @[BANK%d "
                "status %"PRIx64" addr %"PRIx64" domid %d]\n ",
                mc_bank->mc_bank, mc_bank->mc_status, mc_bank->mc_addr,
                mc_bank->mc_domid);
    }
    return 0;
}

static int inject_mce(struct domain *d)
{
    int cpu = smp_processor_id();
    cpumask_t affinity;

    /* PV guest and HVM guest have different vMCE# injection
     * methods*/

    if ( !test_and_set_bool(d->vcpu[0]->mce_pending) )
    {
        if (d->is_hvm)
        {
            mce_printk(MCE_VERBOSE, "MCE: inject vMCE to HVM DOM %d\n", 
                        d->domain_id);
            vcpu_kick(d->vcpu[0]);
        }
        /* PV guest including DOM0 */
        else
        {
            mce_printk(MCE_VERBOSE, "MCE: inject vMCE to PV DOM%d\n", 
                        d->domain_id);
            if (guest_has_trap_callback
                   (d, 0, TRAP_machine_check))
            {
                d->vcpu[0]->cpu_affinity_tmp =
                        d->vcpu[0]->cpu_affinity;
                cpus_clear(affinity);
                cpu_set(cpu, affinity);
                mce_printk(MCE_VERBOSE, "MCE: CPU%d set affinity, old %d\n", cpu,
                            d->vcpu[0]->processor);
                vcpu_set_affinity(d->vcpu[0], &affinity);
                vcpu_kick(d->vcpu[0]);
            }
            else
            {
                mce_printk(MCE_VERBOSE, "MCE: Kill PV guest with No MCE handler\n");
                domain_crash(d);
            }
        }
    }
    else {
        /* new vMCE comes while first one has not been injected yet,
         * in this case, inject fail. [We can't lose this vMCE for
         * the mce node's consistency].
        */
        mce_printk(MCE_QUIET, "There's a pending vMCE waiting to be injected "
                    " to this DOM%d!\n", d->domain_id);
        return -1;
    }
    return 0;
}

static void intel_UCR_handler(struct mcinfo_bank *bank,
             struct mcinfo_global *global,
             struct mcinfo_extended *extension,
             struct mca_handle_result *result)
{
    struct domain *d;
    unsigned long mfn, gfn;
    uint32_t status;

    mce_printk(MCE_VERBOSE, "MCE: Enter UCR recovery action\n");
    result->result = MCA_NEED_RESET;
    if (bank->mc_addr != 0) {
         mfn = bank->mc_addr >> PAGE_SHIFT;
         if (!offline_page(mfn, 1, &status)) {
              /* This is free page */
              if (status & PG_OFFLINE_OFFLINED)
                  result->result = MCA_RECOVERED;
              else if (status & PG_OFFLINE_PENDING) {
                 /* This page has owner */
                  if (status & PG_OFFLINE_OWNED) {
                      result->result |= MCA_OWNER;
                      result->owner = status >> PG_OFFLINE_OWNER_SHIFT;
                      mce_printk(MCE_QUIET, "MCE: This error page is ownded"
                                  " by DOM %d\n", result->owner);
                      /* Fill vMCE# injection and vMCE# MSR virtualization "
                       * "related data */
                      bank->mc_domid = result->owner;
                      /* XXX: Cannot handle shared pages yet 
                       * (this should identify all domains and gfn mapping to
                       *  the mfn in question) */
                      BUG_ON( result->owner == DOMID_COW );
                      if ( result->owner != DOMID_XEN ) {

                          d = get_domain_by_id(result->owner);
                          if ( mca_ctl_conflict(bank, d) )
                          {
                              /* Guest has different MCE ctl with hypervisor */
                              if ( d )
                                  put_domain(d);
                              return;
                          }

                          ASSERT(d);
                          gfn =
                              get_gpfn_from_mfn((bank->mc_addr) >> PAGE_SHIFT);
                          bank->mc_addr =  gfn << PAGE_SHIFT |
                                        (bank->mc_addr & (PAGE_SIZE -1 ));
                          if ( fill_vmsr_data(bank, d,
                                              global->mc_gstatus) == -1 )
                          {
                              mce_printk(MCE_QUIET, "Fill vMCE# data for DOM%d "
                                      "failed\n", result->owner);
                              put_domain(d);
                              domain_crash(d);
                              return;
                          }
                          /* We will inject vMCE to DOMU*/
                          if ( inject_mce(d) < 0 )
                          {
                              mce_printk(MCE_QUIET, "inject vMCE to DOM%d"
                                          " failed\n", d->domain_id);
                              put_domain(d);
                              domain_crash(d);
                              return;
                          }
                          /* Impacted domain go on with domain's recovery job
                           * if the domain has its own MCA handler.
                           * For xen, it has contained the error and finished
                           * its own recovery job.
                           */
                          result->result = MCA_RECOVERED;
                          put_domain(d);
                      }
                  }
              }
         }
    }
}

#define INTEL_MAX_RECOVERY 2
struct mca_error_handler intel_recovery_handler[INTEL_MAX_RECOVERY] =
            {{0x017A, intel_UCR_handler}, {0x00C0, intel_UCR_handler}};

/*
 * Called from mctelem_process_deferred. Return 1 if the telemetry
 * should be committed for dom0 consumption, 0 if it should be
 * dismissed.
 */
static int mce_action(mctelem_cookie_t mctc)
{
    struct mc_info *local_mi;
    uint32_t i;
    struct mcinfo_common *mic = NULL;
    struct mcinfo_global *mc_global;
    struct mcinfo_bank *mc_bank;
    struct mca_handle_result mca_res;

    local_mi = (struct mc_info*)mctelem_dataptr(mctc);
    x86_mcinfo_lookup(mic, local_mi, MC_TYPE_GLOBAL);
    if (mic == NULL) {
        printk(KERN_ERR "MCE: get local buffer entry failed\n ");
        return 0;
    }

    mc_global = (struct mcinfo_global *)mic;

    /* Processing bank information */
    x86_mcinfo_lookup(mic, local_mi, MC_TYPE_BANK);

    for ( ; mic && mic->size; mic = x86_mcinfo_next(mic) ) {
        if (mic->type != MC_TYPE_BANK) {
            continue;
        }
        mc_bank = (struct mcinfo_bank*)mic;

        /* TODO: Add recovery actions here, such as page-offline, etc */
        memset(&mca_res, 0x0f, sizeof(mca_res));
        for ( i = 0; i < INTEL_MAX_RECOVERY; i++ ) {
            if ( ((mc_bank->mc_status & 0xffff) ==
                        intel_recovery_handler[i].mca_code) ||
                  ((mc_bank->mc_status & 0xfff0) ==
                        intel_recovery_handler[i].mca_code)) {
                /* For SRAR, OVER = 1 should have caused reset
                 * For SRAO, OVER = 1 skip recovery action, continue execution
                 */
                if (!(mc_bank->mc_status & MCi_STATUS_OVER))
                    intel_recovery_handler[i].recovery_handler
                                (mc_bank, mc_global, NULL, &mca_res);
                else {
                   if (!(mc_global->mc_gstatus & MCG_STATUS_RIPV))
                       mca_res.result = MCA_NEED_RESET;
                   else
                       mca_res.result = MCA_NO_ACTION;
                }
                if (mca_res.result & MCA_OWNER)
                    mc_bank->mc_domid = mca_res.owner;
                if (mca_res.result == MCA_NEED_RESET)
                    /* DOMID_XEN*/
                    mc_panic("MCE: Software recovery failed for the UCR "
                                "error\n");
                else if (mca_res.result == MCA_RECOVERED)
                    mce_printk(MCE_VERBOSE, "MCE: The UCR error is"
                                "successfully recovered by software!\n");
                else if (mca_res.result == MCA_NO_ACTION)
                    mce_printk(MCE_VERBOSE, "MCE: Overwrite SRAO error can't"
                                "do recover action, RIPV=1, let it be.\n");
                break;
            }
        }
        /* For SRAR, no defined recovery action should have caused reset
         * in MCA Handler
         */
        if ( i >= INTEL_MAX_RECOVERY )
            mce_printk(MCE_VERBOSE, "MCE: No software recovery action"
                            " found for this SRAO error\n");

    }
    return 1;
}

/* Softirq Handler for this MCE# processing */
static void mce_softirq(void)
{
    int cpu = smp_processor_id();
    unsigned int workcpu;

    mce_printk(MCE_VERBOSE, "CPU%d enter softirq\n", cpu);

    mce_barrier_enter(&mce_inside_bar);

    /*
     * Everybody is here. Now let's see who gets to do the
     * recovery work. Right now we just see if there's a CPU
     * that did not have any problems, and pick that one.
     *
     * First, just set a default value: the last CPU who reaches this
     * will overwrite the value and become the default.
     */

    atomic_set(&severity_cpu, cpu);

    mce_barrier_enter(&mce_severity_bar);
    if (!mctelem_has_deferred(cpu))
        atomic_set(&severity_cpu, cpu);
    mce_barrier_exit(&mce_severity_bar);

    /* We choose severity_cpu for further processing */
    if (atomic_read(&severity_cpu) == cpu) {

        mce_printk(MCE_VERBOSE, "CPU%d handling errors\n", cpu);

        /* Step1: Fill DOM0 LOG buffer, vMCE injection buffer and
         * vMCE MSRs virtualization buffer
         */
        for_each_online_cpu(workcpu) {
            mctelem_process_deferred(workcpu, mce_action);
        }

        /* Step2: Send Log to DOM0 through vIRQ */
        if (dom0_vmce_enabled()) {
            mce_printk(MCE_VERBOSE, "MCE: send MCE# to DOM0 through virq\n");
            send_guest_global_virq(dom0, VIRQ_MCA);
        }
    }

    mce_barrier_exit(&mce_inside_bar);
}

/* Machine Check owner judge algorithm:
 * When error happens, all cpus serially read its msr banks.
 * The first CPU who fetches the error bank's info will clear
 * this bank. Later readers can't get any infor again.
 * The first CPU is the actual mce_owner
 *
 * For Fatal (pcc=1) error, it might cause machine crash
 * before we're able to log. For avoiding log missing, we adopt two
 * round scanning:
 * Round1: simply scan. If found pcc = 1 or ripv = 0, simply reset.
 * All MCE banks are sticky, when boot up, MCE polling mechanism
 * will help to collect and log those MCE errors.
 * Round2: Do all MCE processing logic as normal.
 */

static void mce_panic_check(void)
{
      if (is_mc_panic) {
              local_irq_enable();
              for ( ; ; )
                      halt();
      }
}

/*
 * Initialize a barrier. Just set it to 0.
 */
static void mce_barrier_init(struct mce_softirq_barrier *bar)
{
      atomic_set(&bar->val, 0);
      atomic_set(&bar->ingen, 0);
      atomic_set(&bar->outgen, 0);
}

#if 0
/*
 * This function will need to be used when offlining a CPU in the
 * recovery actions.
 *
 * Decrement a barrier only. Needed for cases where the CPU
 * in question can't do it itself (e.g. it is being offlined).
 */
static void mce_barrier_dec(struct mce_softirq_barrier *bar)
{
      atomic_inc(&bar->outgen);
      wmb();
      atomic_dec(&bar->val);
}
#endif

static void mce_spin_lock(spinlock_t *lk)
{
      while (!spin_trylock(lk)) {
              cpu_relax();
              mce_panic_check();
      }
}

static void mce_spin_unlock(spinlock_t *lk)
{
      spin_unlock(lk);
}

/*
 * Increment the generation number and the value. The generation number
 * is incremented when entering a barrier. This way, it can be checked
 * on exit if a CPU is trying to re-enter the barrier. This can happen
 * if the first CPU to make it out immediately exits or re-enters, while
 * another CPU that is still in the loop becomes otherwise occupied
 * (e.g. it needs to service an interrupt, etc), missing the value
 * it's waiting for.
 *
 * These barrier functions should always be paired, so that the
 * counter value will reach 0 again after all CPUs have exited.
 */
static void mce_barrier_enter(struct mce_softirq_barrier *bar)
{
      int gen;

      if (!mce_broadcast)
          return;
      atomic_inc(&bar->ingen);
      gen = atomic_read(&bar->outgen);
      mb();
      atomic_inc(&bar->val);
      while ( atomic_read(&bar->val) != num_online_cpus() &&
          atomic_read(&bar->outgen) == gen) {
              mb();
              mce_panic_check();
      }
}

static void mce_barrier_exit(struct mce_softirq_barrier *bar)
{
      int gen;

      if (!mce_broadcast)
          return;
      atomic_inc(&bar->outgen);
      gen = atomic_read(&bar->ingen);
      mb();
      atomic_dec(&bar->val);
      while ( atomic_read(&bar->val) != 0 &&
          atomic_read(&bar->ingen) == gen ) {
              mb();
              mce_panic_check();
      }
}

#if 0
static void mce_barrier(struct mce_softirq_barrier *bar)
{
      mce_barrier_enter(bar);
      mce_barrier_exit(bar);
}
#endif

static void intel_machine_check(struct cpu_user_regs * regs, long error_code)
{
    uint64_t gstatus;
    mctelem_cookie_t mctc = NULL;
    struct mca_summary bs;
    cpu_banks_t clear_bank;

    mce_spin_lock(&mce_logout_lock);

    memset( &clear_bank, 0x0, sizeof(cpu_banks_t));
    mctc = mcheck_mca_logout(MCA_MCE_SCAN, mca_allbanks, &bs, &clear_bank);

    if (bs.errcnt) {
        /* dump MCE error */
        if (mctc != NULL)
            x86_mcinfo_dump(mctelem_dataptr(mctc));

        /*
         * Uncorrected errors must be dealth with in softirq context.
         */
        if (bs.uc || bs.pcc) {
            add_taint(TAINT_MACHINE_CHECK);
            if (mctc != NULL)
                mctelem_defer(mctc);
            /*
             * For PCC=1 and can't be recovered, context is lost, so reboot now without
             * clearing  the banks, and deal with the telemetry after reboot
             * (the MSRs are sticky)
             */
            if (bs.pcc)
                mc_panic("State lost due to machine check exception.\n");
            if (!bs.ripv)
                mc_panic("RIPV =0 can't resume execution!\n");
            if (!bs.recoverable)
                mc_panic("Machine check exception software recovery fail.\n");
        } else {
            if (mctc != NULL)
                mctelem_commit(mctc);
        }
        atomic_set(&found_error, 1);

        mce_printk(MCE_CRITICAL, "MCE: clear_bank map %lx on CPU%d\n",
                *((unsigned long*)clear_bank), smp_processor_id());
        mcheck_mca_clearbanks(clear_bank);
    } else {
        if (mctc != NULL)
            mctelem_dismiss(mctc);
    }
    mce_spin_unlock(&mce_logout_lock);

    /*
     * Wait until everybody has processed the trap.
     */
    mce_barrier_enter(&mce_trap_bar);
    /* According to latest MCA OS writer guide, if no error bank found
     * on all cpus, something unexpected happening, we can't do any
     * recovery job but to reset the system.
     */
    if (atomic_read(&found_error) == 0)
        mc_panic("Unexpected condition for the MCE handler, need reset\n");
    mce_barrier_exit(&mce_trap_bar);

    /* Clear error finding flags after all cpus finishes above judgement */
    mce_barrier_enter(&mce_trap_bar);
    if (atomic_read(&found_error)) {
        mce_printk(MCE_CRITICAL, "MCE: Choose one CPU "
		        "to clear error finding flag\n ");
        atomic_set(&found_error, 0);
    }
    mca_rdmsrl(MSR_IA32_MCG_STATUS, gstatus);
    if ((gstatus & MCG_STATUS_MCIP) != 0) {
        mce_printk(MCE_CRITICAL, "MCE: Clear MCIP@ last step");
        mca_wrmsrl(MSR_IA32_MCG_STATUS, gstatus & ~MCG_STATUS_MCIP);
    }
    mce_barrier_exit(&mce_trap_bar);

    raise_softirq(MACHINE_CHECK_SOFTIRQ);
}

/* According to MCA OS writer guide, CMCI handler need to clear bank when
 * 1) CE (UC = 0)
 * 2) ser_support = 1, Superious error, OVER = 0, EN = 0, [UC = 1]
 * 3) ser_support = 1, UCNA, OVER = 0, S = 1, AR = 0, PCC = 0, [UC = 1, EN = 1]
 * MCA handler need to clear bank when
 * 1) ser_support = 1, Superious error, OVER = 0, EN = 0, UC = 1
 * 2) ser_support = 1, SRAR, UC = 1, OVER = 0, S = 1, AR = 1, [EN = 1]
 * 3) ser_support = 1, SRAO, UC = 1, S = 1, AR = 0, [EN = 1]
*/

static int intel_need_clearbank_scan(enum mca_source who, u64 status)
{
    if ( who == MCA_CMCI_HANDLER) {
        /* CMCI need clear bank */
        if ( !(status & MCi_STATUS_UC) )
            return 1;
        /* Spurious need clear bank */
        else if ( ser_support && !(status & MCi_STATUS_OVER)
                    && !(status & MCi_STATUS_EN) )
            return 1;
        /* UCNA OVER = 0 need clear bank */
        else if ( ser_support && !(status & MCi_STATUS_OVER) 
                    && !(status & MCi_STATUS_PCC) && !(status & MCi_STATUS_S) 
                    && !(status & MCi_STATUS_AR))
            return 1;
        /* Only Log, no clear */
        else return 0;
    }
    else if ( who == MCA_MCE_SCAN) {
        /* Spurious need clear bank */
        if ( ser_support && !(status & MCi_STATUS_OVER)
                    && (status & MCi_STATUS_UC) && !(status & MCi_STATUS_EN))
            return 1;
        /* SRAR OVER=0 clear bank. OVER = 1 have caused reset */
        else if ( ser_support && (status & MCi_STATUS_UC)
                    && (status & MCi_STATUS_S) && (status & MCi_STATUS_AR )
                    && (status & MCi_STATUS_OVER) )
            return 1;
        /* SRAO need clear bank */
        else if ( ser_support && !(status & MCi_STATUS_AR) 
                    && (status & MCi_STATUS_S) && (status & MCi_STATUS_UC))
            return 1; 
        else
            return 0;
    }

    return 1;
}

/* MCE continues/is recoverable when 
 * 1) CE UC = 0
 * 2) Supious ser_support = 1, OVER = 0, En = 0 [UC = 1]
 * 3) SRAR ser_support = 1, OVER = 0, PCC = 0, S = 1, AR = 1 [UC =1, EN = 1]
 * 4) SRAO ser_support = 1, PCC = 0, S = 1, AR = 0, EN = 1 [UC = 1]
 * 5) UCNA ser_support = 1, OVER = 0, EN = 1, PCC = 0, S = 0, AR = 0, [UC = 1]
 */
static int intel_recoverable_scan(u64 status)
{

    if ( !(status & MCi_STATUS_UC ) )
        return 1;
    else if ( ser_support && !(status & MCi_STATUS_EN) 
                && !(status & MCi_STATUS_OVER) )
        return 1;
    /* SRAR error */
    else if ( ser_support && !(status & MCi_STATUS_OVER) 
                && !(status & MCi_STATUS_PCC) && (status & MCi_STATUS_S)
                && (status & MCi_STATUS_AR) ) {
        mce_printk(MCE_VERBOSE, "MCE: No SRAR error defined currently.\n");
        return 0;
    }
    /* SRAO error */
    else if (ser_support && !(status & MCi_STATUS_PCC)
                && (status & MCi_STATUS_S) && !(status & MCi_STATUS_AR)
                && (status & MCi_STATUS_EN))
        return 1;
    /* UCNA error */
    else if (ser_support && !(status & MCi_STATUS_OVER)
                && (status & MCi_STATUS_EN) && !(status & MCi_STATUS_PCC)
                && !(status & MCi_STATUS_S) && !(status & MCi_STATUS_AR))
        return 1;
    return 0;
}

static DEFINE_SPINLOCK(cmci_discover_lock);

/*
 * Discover bank sharing using the algorithm recommended in the SDM.
 */
static int do_cmci_discover(int i)
{
    unsigned msr = MSR_IA32_MC0_CTL2 + i;
    u64 val;

    rdmsrl(msr, val);
    /* Some other CPU already owns this bank. */
    if (val & CMCI_EN) {
        clear_bit(i, __get_cpu_var(mce_banks_owned));
        goto out;
    }

    val &= ~CMCI_THRESHOLD_MASK;
    wrmsrl(msr, val | CMCI_EN | CMCI_THRESHOLD);
    rdmsrl(msr, val);

    if (!(val & CMCI_EN)) {
        /* This bank does not support CMCI. Polling timer has to handle it. */
        set_bit(i, __get_cpu_var(no_cmci_banks));
        return 0;
    }
    set_bit(i, __get_cpu_var(mce_banks_owned));
out:
    clear_bit(i, __get_cpu_var(no_cmci_banks));
    return 1;
}

static void cmci_discover(void)
{
    unsigned long flags;
    int i;
    mctelem_cookie_t mctc;
    struct mca_summary bs;

    mce_printk(MCE_VERBOSE, "CMCI: find owner on CPU%d\n", smp_processor_id());

    spin_lock_irqsave(&cmci_discover_lock, flags);

    for (i = 0; i < nr_mce_banks; i++)
        if (!test_bit(i, __get_cpu_var(mce_banks_owned)))
            do_cmci_discover(i);

    spin_unlock_irqrestore(&cmci_discover_lock, flags);

    /* In case CMCI happended when do owner change.
     * If CMCI happened yet not processed immediately,
     * MCi_status (error_count bit 38~52) is not cleared,
     * the CMCI interrupt will never be triggered again.
     */

    mctc = mcheck_mca_logout(
        MCA_CMCI_HANDLER, __get_cpu_var(mce_banks_owned), &bs, NULL);

    if (bs.errcnt && mctc != NULL) {
        if (dom0_vmce_enabled()) {
            mctelem_commit(mctc);
            send_guest_global_virq(dom0, VIRQ_MCA);
        } else {
            x86_mcinfo_dump(mctelem_dataptr(mctc));
            mctelem_dismiss(mctc);
        }
    } else if (mctc != NULL)
        mctelem_dismiss(mctc);

    mce_printk(MCE_VERBOSE, "CMCI: CPU%d owner_map[%lx], no_cmci_map[%lx]\n",
           smp_processor_id(),
           *((unsigned long *)__get_cpu_var(mce_banks_owned)),
           *((unsigned long *)__get_cpu_var(no_cmci_banks)));
}

/*
 * Define an owner for each bank. Banks can be shared between CPUs
 * and to avoid reporting events multiple times always set up one
 * CPU as owner. 
 *
 * The assignment has to be redone when CPUs go offline and
 * any of the owners goes away. Also pollers run in parallel so we
 * have to be careful to update the banks in a way that doesn't
 * lose or duplicate events.
 */

static void mce_set_owner(void)
{
    if (!cmci_support || mce_disabled == 1)
        return;

    cmci_discover();
}

static void __cpu_mcheck_distribute_cmci(void *unused)
{
    cmci_discover();
}

void cpu_mcheck_distribute_cmci(void)
{
    if (cmci_support && !mce_disabled)
        on_each_cpu(__cpu_mcheck_distribute_cmci, NULL, 0);
}

static void clear_cmci(void)
{
    int i;

    if (!cmci_support || mce_disabled == 1)
        return;

    mce_printk(MCE_VERBOSE, "CMCI: clear_cmci support on CPU%d\n",
            smp_processor_id());

    for (i = 0; i < nr_mce_banks; i++) {
        unsigned msr = MSR_IA32_MC0_CTL2 + i;
        u64 val;
        if (!test_bit(i, __get_cpu_var(mce_banks_owned)))
            continue;
        rdmsrl(msr, val);
        if (val & (CMCI_EN|CMCI_THRESHOLD_MASK))
            wrmsrl(msr, val & ~(CMCI_EN|CMCI_THRESHOLD_MASK));
        clear_bit(i, __get_cpu_var(mce_banks_owned));
    }
}

void cpu_mcheck_disable(void)
{
    clear_in_cr4(X86_CR4_MCE);

    if (cmci_support && !mce_disabled)
        clear_cmci();
}

static void intel_init_cmci(struct cpuinfo_x86 *c)
{
    u32 l, apic;
    int cpu = smp_processor_id();

    if (!mce_available(c) || !cmci_support) {
        if (opt_cpu_info)
            mce_printk(MCE_QUIET, "CMCI: CPU%d has no CMCI support\n", cpu);
        return;
    }

    apic = apic_read(APIC_CMCI);
    if ( apic & APIC_VECTOR_MASK )
    {
        mce_printk(MCE_QUIET, "CPU%d CMCI LVT vector (%#x) already installed\n",
            cpu, ( apic & APIC_VECTOR_MASK ));
        return;
    }

    apic = CMCI_APIC_VECTOR;
    apic |= (APIC_DM_FIXED | APIC_LVT_MASKED);
    apic_write_around(APIC_CMCI, apic);

    l = apic_read(APIC_CMCI);
    apic_write_around(APIC_CMCI, l & ~APIC_LVT_MASKED);
}

fastcall void smp_cmci_interrupt(struct cpu_user_regs *regs)
{
    mctelem_cookie_t mctc;
    struct mca_summary bs;
    struct cpu_user_regs *old_regs = set_irq_regs(regs);

    ack_APIC_irq();
    irq_enter();

    mctc = mcheck_mca_logout(
        MCA_CMCI_HANDLER, __get_cpu_var(mce_banks_owned), &bs, NULL);

    if (bs.errcnt && mctc != NULL) {
        if (dom0_vmce_enabled()) {
            mctelem_commit(mctc);
            mce_printk(MCE_VERBOSE, "CMCI: send CMCI to DOM0 through virq\n");
            send_guest_global_virq(dom0, VIRQ_MCA);
        } else {
            x86_mcinfo_dump(mctelem_dataptr(mctc));
            mctelem_dismiss(mctc);
       }
    } else if (mctc != NULL)
        mctelem_dismiss(mctc);

    irq_exit();
    set_irq_regs(old_regs);
}

void mce_intel_feature_init(struct cpuinfo_x86 *c)
{

#ifdef CONFIG_X86_MCE_THERMAL
    intel_init_thermal(c);
#endif
    intel_init_cmci(c);
}

static void _mce_cap_init(struct cpuinfo_x86 *c)
{
    u32 l = mce_cap_init();

    if ((l & MCG_CMCI_P) && cpu_has_apic)
        cmci_support = 1;

    /* Support Software Error Recovery */
    if (l & MCG_SER_P)
        ser_support = 1;

    if (l & MCG_EXT_P)
    {
        nr_intel_ext_msrs = (l >> MCG_EXT_CNT) & 0xff;
        mce_printk (MCE_QUIET, "CPU%d: Intel Extended MCE MSRs (%d) available\n",
            smp_processor_id(), nr_intel_ext_msrs);
    }
    firstbank = mce_firstbank(c);
}

static void mce_init(void)
{
    u32 l, h;
    int i;
    mctelem_cookie_t mctc;
    struct mca_summary bs;

    clear_in_cr4(X86_CR4_MCE);

    mce_barrier_init(&mce_inside_bar);
    mce_barrier_init(&mce_severity_bar);
    mce_barrier_init(&mce_trap_bar);
    spin_lock_init(&mce_logout_lock);

    /* log the machine checks left over from the previous reset.
     * This also clears all registers*/

    mctc = mcheck_mca_logout(MCA_RESET, mca_allbanks, &bs, NULL);

    /* in the boot up stage, print out and also log in DOM0 boot process */
    if (bs.errcnt && mctc != NULL) {
        x86_mcinfo_dump(mctelem_dataptr(mctc));
        mctelem_commit(mctc);
    }

    set_in_cr4(X86_CR4_MCE);

    for (i = firstbank; i < nr_mce_banks; i++)
    {
        /* Some banks are shared across cores, use MCi_CTRL to judge whether
         * this bank has been initialized by other cores already. */
        rdmsr(MSR_IA32_MC0_CTL + 4*i, l, h);
        if (!(l | h))
        {
            /* if ctl is 0, this bank is never initialized */
            mce_printk(MCE_VERBOSE, "mce_init: init bank%d\n", i);
            wrmsr (MSR_IA32_MC0_CTL + 4*i, 0xffffffff, 0xffffffff);
            wrmsr (MSR_IA32_MC0_STATUS + 4*i, 0x0, 0x0);
        }
    }
    if (firstbank) /* if cmci enabled, firstbank = 0 */
        wrmsr (MSR_IA32_MC0_STATUS, 0x0, 0x0);
}

/* p4/p6 family have similar MCA initialization process */
enum mcheck_type intel_mcheck_init(struct cpuinfo_x86 *c)
{
    _mce_cap_init(c);

    /* machine check is available */
    x86_mce_vector_register(intel_machine_check);
    x86_mce_callback_register(intel_get_extended_msrs);
    mce_recoverable_register(intel_recoverable_scan);
    mce_need_clearbank_register(intel_need_clearbank_scan);

    mce_init();
    mce_intel_feature_init(c);
    mce_set_owner();

    open_softirq(MACHINE_CHECK_SOFTIRQ, mce_softirq);
    return mcheck_intel;
}

int intel_mce_wrmsr(uint32_t msr, uint64_t val)
{
    int ret = 1;

    switch ( msr )
    {
    case MSR_IA32_MC0_CTL2 ... MSR_IA32_MC0_CTL2 + MAX_NR_BANKS - 1:
        mce_printk(MCE_QUIET, "We have disabled CMCI capability, "
                 "Guest should not write this MSR!\n");
        break;
    default:
        ret = 0;
        break;
    }

    return ret;
}

int intel_mce_rdmsr(uint32_t msr, uint64_t *val)
{
    int ret = 1;

    switch ( msr )
    {
    case MSR_IA32_MC0_CTL2 ... MSR_IA32_MC0_CTL2 + MAX_NR_BANKS - 1:
        mce_printk(MCE_QUIET, "We have disabled CMCI capability, "
                 "Guest should not read this MSR!\n");
        break;
    default:
        ret = 0;
        break;
    }

    return ret;
}


