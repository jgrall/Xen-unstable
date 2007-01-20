#define __KERNEL_SYSCALLS__
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/sysrq.h>
#include <linux/stringify.h>
#include <asm/irq.h>
#include <asm/mmu_context.h>
#include <xen/evtchn.h>
#include <asm/hypervisor.h>
#include <xen/interface/dom0_ops.h>
#include <xen/xenbus.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <xen/gnttab.h>
#include <xen/xencons.h>
#include <xen/cpu_hotplug.h>

#if defined(__i386__) || defined(__x86_64__)

/*
 * Power off function, if any
 */
void (*pm_power_off)(void);
EXPORT_SYMBOL(pm_power_off);

void machine_emergency_restart(void)
{
	/* We really want to get pending console data out before we die. */
	xencons_force_flush();
	HYPERVISOR_shutdown(SHUTDOWN_reboot);
}

void machine_restart(char * __unused)
{
	machine_emergency_restart();
}

void machine_halt(void)
{
	machine_power_off();
}

void machine_power_off(void)
{
	/* We really want to get pending console data out before we die. */
	xencons_force_flush();
	if (pm_power_off)
		pm_power_off();
	HYPERVISOR_shutdown(SHUTDOWN_poweroff);
}

int reboot_thru_bios = 0;	/* for dmi_scan.c */
EXPORT_SYMBOL(machine_restart);
EXPORT_SYMBOL(machine_halt);
EXPORT_SYMBOL(machine_power_off);

/* Ensure we run on the idle task page tables so that we will
   switch page tables before running user space. This is needed
   on architectures with separate kernel and user page tables
   because the user page table pointer is not saved/restored. */
static void switch_idle_mm(void)
{
	struct mm_struct *mm = current->active_mm;

	if (mm == &init_mm)
		return;

	atomic_inc(&init_mm.mm_count);
	switch_mm(mm, &init_mm, current);
	current->active_mm = &init_mm;
	mmdrop(mm);
}

static void pre_suspend(void)
{
	HYPERVISOR_shared_info = (shared_info_t *)empty_zero_page;
	clear_fixmap(FIX_SHARED_INFO);

	xen_start_info->store_mfn = mfn_to_pfn(xen_start_info->store_mfn);
	xen_start_info->console.domU.mfn =
		mfn_to_pfn(xen_start_info->console.domU.mfn);
}

static void post_suspend(int suspend_cancelled)
{
	int i, j, k, fpp;
	extern unsigned long max_pfn;
	extern unsigned long *pfn_to_mfn_frame_list_list;
	extern unsigned long *pfn_to_mfn_frame_list[];

	if (suspend_cancelled) {
		xen_start_info->store_mfn =
			pfn_to_mfn(xen_start_info->store_mfn);
		xen_start_info->console.domU.mfn =
			pfn_to_mfn(xen_start_info->console.domU.mfn);
	}
	
	set_fixmap(FIX_SHARED_INFO, xen_start_info->shared_info);

	HYPERVISOR_shared_info = (shared_info_t *)fix_to_virt(FIX_SHARED_INFO);

	memset(empty_zero_page, 0, PAGE_SIZE);

	HYPERVISOR_shared_info->arch.pfn_to_mfn_frame_list_list =
		virt_to_mfn(pfn_to_mfn_frame_list_list);

	fpp = PAGE_SIZE/sizeof(unsigned long);
	for (i = 0, j = 0, k = -1; i < max_pfn; i += fpp, j++) {
		if ((j % fpp) == 0) {
			k++;
			pfn_to_mfn_frame_list_list[k] =
				virt_to_mfn(pfn_to_mfn_frame_list[k]);
			j = 0;
		}
		pfn_to_mfn_frame_list[k][j] =
			virt_to_mfn(&phys_to_machine_mapping[i]);
	}
	HYPERVISOR_shared_info->arch.max_pfn = max_pfn;
}

#else /* !(defined(__i386__) || defined(__x86_64__)) */

#define switch_idle_mm()	((void)0)
#define mm_pin_all()		((void)0)
#define pre_suspend()		((void)0)
#define post_suspend(x)		((void)0)

#endif

int __xen_suspend(void)
{
	int err, suspend_cancelled;

	extern void time_resume(void);

	BUG_ON(smp_processor_id() != 0);
	BUG_ON(in_interrupt());

#if defined(__i386__) || defined(__x86_64__)
	if (xen_feature(XENFEAT_auto_translated_physmap)) {
		printk(KERN_WARNING "Cannot suspend in "
		       "auto_translated_physmap mode.\n");
		return -EOPNOTSUPP;
	}
#endif

	err = smp_suspend();
	if (err)
		return err;

	xenbus_suspend();

	preempt_disable();

	mm_pin_all();
	local_irq_disable();
	preempt_enable();

	gnttab_suspend();

	pre_suspend();

	/*
	 * This hypercall returns 1 if suspend was cancelled or the domain was
	 * merely checkpointed, and 0 if it is resuming in a new domain.
	 */
	suspend_cancelled = HYPERVISOR_suspend(virt_to_mfn(xen_start_info));

	post_suspend(suspend_cancelled);

	gnttab_resume();

	if (!suspend_cancelled)
		irq_resume();

	time_resume();

	switch_idle_mm();

	local_irq_enable();

	if (!suspend_cancelled) {
		xencons_resume();
		xenbus_resume();
	} else {
		xenbus_suspend_cancel();
	}

	smp_resume();

	return err;
}
