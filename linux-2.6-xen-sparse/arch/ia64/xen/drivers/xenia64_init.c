#ifdef __ia64__
#include <linux/config.h>
#include <linux/module.h>
#include <linux/efi.h>
#include <asm/sal.h>
#include <asm/hypervisor.h>
/* #include <asm-xen/evtchn.h> */
#include <xen/interface/arch-ia64.h>
#include <linux/vmalloc.h>

shared_info_t *HYPERVISOR_shared_info = (shared_info_t *)XSI_BASE;
EXPORT_SYMBOL(HYPERVISOR_shared_info);

start_info_t *xen_start_info;

int running_on_xen;
EXPORT_SYMBOL(running_on_xen);

int xen_init(void)
{
	static int initialized;
	shared_info_t *s = HYPERVISOR_shared_info;

	if (initialized)
		return running_on_xen ? 0 : -1;

	if (!is_running_on_xen())
		return -1;

	xen_start_info = __va(s->arch.start_info_pfn << PAGE_SHIFT);
	xen_start_info->flags = s->arch.flags;
	printk("Running on Xen! start_info_pfn=0x%lx nr_pages=%ld flags=0x%x\n",
		s->arch.start_info_pfn, xen_start_info->nr_pages,
		xen_start_info->flags);

	initialized = 1;
	return 0;
}

#ifndef CONFIG_XEN_IA64_DOM0_VP
/* We just need a range of legal va here, though finally identity
 * mapped one is instead used for gnttab mapping.
 */
unsigned long alloc_empty_foreign_map_page_range(unsigned long pages)
{
	struct vm_struct *vma;

	if ( (vma = get_vm_area(PAGE_SIZE * pages, VM_ALLOC)) == NULL )
		return NULL;

	return (unsigned long)vma->addr;
}
#endif

#if 0
/* These should be define'd but some drivers use them without
 * a convenient arch include */
unsigned long mfn_to_pfn(unsigned long mfn) { return mfn; }
#endif
#endif
