/*
 *	Xen SMP booting functions
 *
 *	See arch/i386/kernel/smpboot.c for copyright and credits for derived
 *	portions of this file.
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/smp_lock.h>
#include <linux/irq.h>
#include <linux/bootmem.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <asm/desc.h>
#include <asm/arch_hooks.h>
#include <asm/pgalloc.h>
#include <xen/evtchn.h>
#include <xen/interface/vcpu.h>
#include <xen/xenbus.h>

#ifdef CONFIG_SMP_ALTERNATIVES
#include <asm/smp_alt.h>
#endif

extern irqreturn_t smp_reschedule_interrupt(int, void *, struct pt_regs *);
extern irqreturn_t smp_call_function_interrupt(int, void *, struct pt_regs *);

extern void local_setup_timer(unsigned int cpu);
extern void local_teardown_timer(unsigned int cpu);

extern void hypervisor_callback(void);
extern void failsafe_callback(void);
extern void system_call(void);
extern void smp_trap_init(trap_info_t *);

/* Number of siblings per CPU package */
int smp_num_siblings = 1;
int phys_proc_id[NR_CPUS]; /* Package ID of each logical CPU */
EXPORT_SYMBOL(phys_proc_id);
int cpu_core_id[NR_CPUS]; /* Core ID of each logical CPU */
EXPORT_SYMBOL(cpu_core_id);

cpumask_t cpu_online_map;
EXPORT_SYMBOL(cpu_online_map);
cpumask_t cpu_possible_map;
EXPORT_SYMBOL(cpu_possible_map);

struct cpuinfo_x86 cpu_data[NR_CPUS] __cacheline_aligned;
EXPORT_SYMBOL(cpu_data);

#ifdef CONFIG_HOTPLUG_CPU
DEFINE_PER_CPU(int, cpu_state) = { 0 };
#endif

static DEFINE_PER_CPU(int, resched_irq);
static DEFINE_PER_CPU(int, callfunc_irq);
static char resched_name[NR_CPUS][15];
static char callfunc_name[NR_CPUS][15];

u8 cpu_2_logical_apicid[NR_CPUS] = { [0 ... NR_CPUS-1] = BAD_APICID };

void *xquad_portio;

cpumask_t cpu_sibling_map[NR_CPUS] __cacheline_aligned;
cpumask_t cpu_core_map[NR_CPUS] __cacheline_aligned;
EXPORT_SYMBOL(cpu_core_map);

#if defined(__i386__)
u8 x86_cpu_to_apicid[NR_CPUS] = { [0 ... NR_CPUS-1] = 0xff };
EXPORT_SYMBOL(x86_cpu_to_apicid);
#elif !defined(CONFIG_X86_IO_APIC)
unsigned int maxcpus = NR_CPUS;
#endif

void __init prefill_possible_map(void)
{
	int i, rc;

	if (!cpus_empty(cpu_possible_map))
		return;

	for (i = 0; i < NR_CPUS; i++) {
		rc = HYPERVISOR_vcpu_op(VCPUOP_is_up, i, NULL);
		if (rc == -ENOENT)
			break;
		cpu_set(i, cpu_possible_map);
	}
}

void __init smp_alloc_memory(void)
{
}

static void xen_smp_intr_init(unsigned int cpu)
{
	sprintf(resched_name[cpu], "resched%d", cpu);
	per_cpu(resched_irq, cpu) =
		bind_ipi_to_irqhandler(
			RESCHEDULE_VECTOR,
			cpu,
			smp_reschedule_interrupt,
			SA_INTERRUPT,
			resched_name[cpu],
			NULL);
	BUG_ON(per_cpu(resched_irq, cpu) < 0);

	sprintf(callfunc_name[cpu], "callfunc%d", cpu);
	per_cpu(callfunc_irq, cpu) =
		bind_ipi_to_irqhandler(
			CALL_FUNCTION_VECTOR,
			cpu,
			smp_call_function_interrupt,
			SA_INTERRUPT,
			callfunc_name[cpu],
			NULL);
	BUG_ON(per_cpu(callfunc_irq, cpu) < 0);

	if (cpu != 0)
		local_setup_timer(cpu);
}

#ifdef CONFIG_HOTPLUG_CPU
static void xen_smp_intr_exit(unsigned int cpu)
{
	if (cpu != 0)
		local_teardown_timer(cpu);

	unbind_from_irqhandler(per_cpu(resched_irq, cpu), NULL);
	unbind_from_irqhandler(per_cpu(callfunc_irq, cpu), NULL);
}
#endif

static void cpu_bringup(void)
{
	cpu_init();
	touch_softlockup_watchdog();
	preempt_disable();
	local_irq_enable();
	cpu_idle();
}

void vcpu_prepare(int vcpu)
{
	vcpu_guest_context_t ctxt;
	struct task_struct *idle = idle_task(vcpu);

	if (vcpu == 0)
		return;

	memset(&ctxt, 0, sizeof(ctxt));

	ctxt.flags = VGCF_IN_KERNEL;
	ctxt.user_regs.ds = __USER_DS;
	ctxt.user_regs.es = __USER_DS;
	ctxt.user_regs.fs = 0;
	ctxt.user_regs.gs = 0;
	ctxt.user_regs.ss = __KERNEL_DS;
	ctxt.user_regs.eip = (unsigned long)cpu_bringup;
	ctxt.user_regs.eflags = X86_EFLAGS_IF | 0x1000; /* IOPL_RING1 */

	memset(&ctxt.fpu_ctxt, 0, sizeof(ctxt.fpu_ctxt));

	smp_trap_init(ctxt.trap_ctxt);

	ctxt.ldt_ents = 0;

	ctxt.gdt_frames[0] = virt_to_mfn(cpu_gdt_descr[vcpu].address);
	ctxt.gdt_ents      = cpu_gdt_descr[vcpu].size / 8;

#ifdef __i386__
	ctxt.user_regs.cs = __KERNEL_CS;
	ctxt.user_regs.esp = idle->thread.esp0 - sizeof(struct pt_regs);

	ctxt.kernel_ss = __KERNEL_DS;
	ctxt.kernel_sp = idle->thread.esp0;

	ctxt.event_callback_cs     = __KERNEL_CS;
	ctxt.event_callback_eip    = (unsigned long)hypervisor_callback;
	ctxt.failsafe_callback_cs  = __KERNEL_CS;
	ctxt.failsafe_callback_eip = (unsigned long)failsafe_callback;

	ctxt.ctrlreg[3] = virt_to_mfn(swapper_pg_dir) << PAGE_SHIFT;
#else /* __x86_64__ */
	ctxt.user_regs.cs = __KERNEL_CS | 3;
	ctxt.user_regs.esp = idle->thread.rsp0 - sizeof(struct pt_regs);

	ctxt.kernel_ss = __KERNEL_DS;
	ctxt.kernel_sp = idle->thread.rsp0;

	ctxt.event_callback_eip    = (unsigned long)hypervisor_callback;
	ctxt.failsafe_callback_eip = (unsigned long)failsafe_callback;
	ctxt.syscall_callback_eip  = (unsigned long)system_call;

	ctxt.ctrlreg[3] = virt_to_mfn(init_level4_pgt) << PAGE_SHIFT;

	ctxt.gs_base_kernel = (unsigned long)(cpu_pda(vcpu));
#endif

	BUG_ON(HYPERVISOR_vcpu_op(VCPUOP_initialise, vcpu, &ctxt));
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	int cpu;
	struct task_struct *idle;

	cpu_data[0] = boot_cpu_data;

	cpu_2_logical_apicid[0] = 0;
	x86_cpu_to_apicid[0] = 0;

	current_thread_info()->cpu = 0;
	cpu_sibling_map[0] = cpumask_of_cpu(0);
	cpu_core_map[0]    = cpumask_of_cpu(0);

	xen_smp_intr_init(0);

	for_each_cpu_mask (cpu, cpu_possible_map) {
		if (cpu == 0)
			continue;

		cpu_data[cpu] = boot_cpu_data;
		cpu_2_logical_apicid[cpu] = cpu;
		x86_cpu_to_apicid[cpu] = cpu;

		idle = fork_idle(cpu);
		if (IS_ERR(idle))
			panic("failed fork for CPU %d", cpu);

#ifdef __x86_64__
		cpu_pda(cpu)->pcurrent = idle;
		cpu_pda(cpu)->cpunumber = cpu;
		per_cpu(init_tss,cpu).rsp0 = idle->thread.rsp;
		clear_ti_thread_flag(idle->thread_info, TIF_FORK);
#endif

		irq_ctx_init(cpu);

		cpu_gdt_descr[cpu].address =
			__get_free_page(GFP_KERNEL|__GFP_ZERO);
		BUG_ON(cpu_gdt_descr[0].size > PAGE_SIZE);
		cpu_gdt_descr[cpu].size = cpu_gdt_descr[0].size;
		memcpy((void *)cpu_gdt_descr[cpu].address,
		       (void *)cpu_gdt_descr[0].address,
		       cpu_gdt_descr[0].size);
		make_page_readonly(
			(void *)cpu_gdt_descr[cpu].address,
			XENFEAT_writable_descriptor_tables);

#ifdef CONFIG_HOTPLUG_CPU
		if (xen_start_info->flags & SIF_INITDOMAIN)
			cpu_set(cpu, cpu_present_map);
#else
		cpu_set(cpu, cpu_present_map);
#endif

		vcpu_prepare(cpu);
	}

	/* Currently, Xen gives no dynamic NUMA/HT info. */
	for (cpu = 1; cpu < NR_CPUS; cpu++) {
		cpu_sibling_map[cpu] = cpumask_of_cpu(cpu);
		cpu_core_map[cpu]    = cpumask_of_cpu(cpu);
	}

#ifdef CONFIG_X86_IO_APIC
	/*
	 * Here we can be sure that there is an IO-APIC in the system. Let's
	 * go and set it up:
	 */
	if (!skip_ioapic_setup && nr_ioapics)
		setup_IO_APIC();
#endif
}

void __devinit smp_prepare_boot_cpu(void)
{
	prefill_possible_map();
	cpu_present_map  = cpumask_of_cpu(0);
	cpu_online_map   = cpumask_of_cpu(0);
}

#ifdef CONFIG_HOTPLUG_CPU

/*
 * Initialize cpu_present_map late to skip SMP boot code in init/main.c.
 * But do it early enough to catch critical for_each_present_cpu() loops
 * in i386-specific code.
 */
static int __init initialize_cpu_present_map(void)
{
	cpu_present_map = cpu_possible_map;
	return 0;
}
core_initcall(initialize_cpu_present_map);

static void vcpu_hotplug(unsigned int cpu)
{
	int err;
	char dir[32], state[32];

	if ((cpu >= NR_CPUS) || !cpu_possible(cpu))
		return;

	sprintf(dir, "cpu/%d", cpu);
	err = xenbus_scanf(XBT_NULL, dir, "availability", "%s", state);
	if (err != 1) {
		printk(KERN_ERR "XENBUS: Unable to read cpu state\n");
		return;
	}

	if (strcmp(state, "online") == 0) {
		(void)cpu_up(cpu);
	} else if (strcmp(state, "offline") == 0) {
		(void)cpu_down(cpu);
	} else {
		printk(KERN_ERR "XENBUS: unknown state(%s) on CPU%d\n",
		       state, cpu);
	}
}

static void handle_vcpu_hotplug_event(
	struct xenbus_watch *watch, const char **vec, unsigned int len)
{
	int cpu;
	char *cpustr;
	const char *node = vec[XS_WATCH_PATH];

	if ((cpustr = strstr(node, "cpu/")) != NULL) {
		sscanf(cpustr, "cpu/%d", &cpu);
		vcpu_hotplug(cpu);
	}
}

static int setup_cpu_watcher(struct notifier_block *notifier,
			      unsigned long event, void *data)
{
	int i;

	static struct xenbus_watch cpu_watch = {
		.node = "cpu",
		.callback = handle_vcpu_hotplug_event };
	(void)register_xenbus_watch(&cpu_watch);

	if (!(xen_start_info->flags & SIF_INITDOMAIN)) {
		for_each_cpu(i)
			vcpu_hotplug(i);
		printk(KERN_INFO "Brought up %ld CPUs\n",
		       (long)num_online_cpus());
	}

	return NOTIFY_DONE;
}

static int __init setup_vcpu_hotplug_event(void)
{
	static struct notifier_block xsn_cpu = {
		.notifier_call = setup_cpu_watcher };
	register_xenstore_notifier(&xsn_cpu);
	return 0;
}

arch_initcall(setup_vcpu_hotplug_event);

int __cpu_disable(void)
{
	cpumask_t map = cpu_online_map;
	int cpu = smp_processor_id();

	if (cpu == 0)
		return -EBUSY;

	cpu_clear(cpu, map);
	fixup_irqs(map);
	cpu_clear(cpu, cpu_online_map);

	return 0;
}

void __cpu_die(unsigned int cpu)
{
	while (HYPERVISOR_vcpu_op(VCPUOP_is_up, cpu, NULL)) {
		current->state = TASK_UNINTERRUPTIBLE;
		schedule_timeout(HZ/10);
	}

	xen_smp_intr_exit(cpu);

#ifdef CONFIG_SMP_ALTERNATIVES
	if (num_online_cpus() == 1)
		unprepare_for_smp();
#endif
}

#else /* !CONFIG_HOTPLUG_CPU */

int __cpu_disable(void)
{
	return -ENOSYS;
}

void __cpu_die(unsigned int cpu)
{
	BUG();
}

#endif /* CONFIG_HOTPLUG_CPU */

int __devinit __cpu_up(unsigned int cpu)
{
#ifdef CONFIG_SMP_ALTERNATIVES
	if (num_online_cpus() == 1)
		prepare_for_smp();
#endif

	xen_smp_intr_init(cpu);
	cpu_set(cpu, cpu_online_map);
	if (HYPERVISOR_vcpu_op(VCPUOP_up, cpu, NULL) != 0)
		BUG();

	return 0;
}

void __init smp_cpus_done(unsigned int max_cpus)
{
}

#ifndef CONFIG_X86_LOCAL_APIC
int setup_profiling_timer(unsigned int multiplier)
{
	return -EINVAL;
}
#endif

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
