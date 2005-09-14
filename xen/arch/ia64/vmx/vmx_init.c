/* -*-  Mode:C; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */
/*
 * vmx_init.c: initialization work for vt specific domain
 * Copyright (c) 2005, Intel Corporation.
 *	Kun Tian (Kevin Tian) <kevin.tian@intel.com>
 *	Xuefei Xu (Anthony Xu) <anthony.xu@intel.com>
 *	Fred Yang <fred.yang@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 */

/*
 * 05/08/16 Kun tian (Kevin Tian) <kevin.tian@intel.com>:
 * Disable doubling mapping
 *
 * 05/03/23 Kun Tian (Kevin Tian) <kevin.tian@intel.com>:
 * Simplied design in first step:
 *	- One virtual environment
 *	- Domain is bound to one LP
 * Later to support guest SMP:
 *	- Need interface to handle VP scheduled to different LP
 */
#include <xen/config.h>
#include <xen/types.h>
#include <xen/sched.h>
#include <asm/pal.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/vmx_vcpu.h>
#include <xen/lib.h>
#include <asm/vmmu.h>
#include <public/arch-ia64.h>
#include <public/io/ioreq.h>
#include <asm/vmx_phy_mode.h>
#include <asm/processor.h>
#include <asm/vmx.h>
#include <xen/mm.h>

/* Global flag to identify whether Intel vmx feature is on */
u32 vmx_enabled = 0;
static u32 vm_order;
static u64 buffer_size;
static u64 vp_env_info;
static u64 vm_buffer = 0;	/* Buffer required to bring up VMX feature */
u64 __vsa_base = 0;	/* Run-time service base of VMX */

/* Check whether vt feature is enabled or not. */
void
identify_vmx_feature(void)
{
	pal_status_t ret;
	u64 avail = 1, status = 1, control = 1;

	vmx_enabled = 0;
	/* Check VT-i feature */
	ret = ia64_pal_proc_get_features(&avail, &status, &control);
	if (ret != PAL_STATUS_SUCCESS) {
		printk("Get proc features failed.\n");
		goto no_vti;
	}

	/* FIXME: do we need to check status field, to see whether
	 * PSR.vm is actually enabled? If yes, aonther call to
	 * ia64_pal_proc_set_features may be reuqired then.
	 */
	printk("avail:0x%lx, status:0x%lx,control:0x%lx, vm?0x%lx\n",
		avail, status, control, avail & PAL_PROC_VM_BIT);
	if (!(avail & PAL_PROC_VM_BIT)) {
		printk("No VT feature supported.\n");
		goto no_vti;
	}

	ret = ia64_pal_vp_env_info(&buffer_size, &vp_env_info);
	if (ret != PAL_STATUS_SUCCESS) {
		printk("Get vp environment info failed.\n");
		goto no_vti;
	}

	/* Does xen has ability to decode itself? */
	if (!(vp_env_info & VP_OPCODE))
		printk("WARNING: no opcode provided from hardware(%lx)!!!\n", vp_env_info);
	vm_order = get_order(buffer_size);
	printk("vm buffer size: %d, order: %d\n", buffer_size, vm_order);

	vmx_enabled = 1;
no_vti:
	return;
}

/*
 * Init virtual environment on current LP
 * vsa_base is the indicator whether it's first LP to be initialized
 * for current domain.
 */ 
void
vmx_init_env(void)
{
	u64 status, tmp_base;

	if (!vm_buffer) {
		vm_buffer = alloc_xenheap_pages(vm_order);
		ASSERT(vm_buffer);
		printk("vm_buffer: 0x%lx\n", vm_buffer);
	}

	status=ia64_pal_vp_init_env(__vsa_base ? VP_INIT_ENV : VP_INIT_ENV_INITALIZE,
				    __pa(vm_buffer),
				    vm_buffer,
				    &tmp_base);

	if (status != PAL_STATUS_SUCCESS) {
		printk("ia64_pal_vp_init_env failed.\n");
		return -1;
	}

	if (!__vsa_base)
		__vsa_base = tmp_base;
	else
		ASSERT(tmp_base != __vsa_base);

#ifdef XEN_DBL_MAPPING
	/* Init stub for rr7 switch */
	vmx_init_double_mapping_stub();
#endif 
}

void vmx_setup_platform(struct vcpu *v, struct vcpu_guest_context *c)
{
	struct domain *d = v->domain;
	shared_iopage_t *sp;

	ASSERT(d != dom0); /* only for non-privileged vti domain */
	d->arch.vmx_platform.shared_page_va = __va(c->share_io_pg);
	sp = get_sp(d);
	memset((char *)sp,0,PAGE_SIZE);
	/* FIXME: temp due to old CP */
	sp->sp_global.eport = 2;
#ifdef V_IOSAPIC_READY
	sp->vcpu_number = 1;
#endif
	/* TEMP */
	d->arch.vmx_platform.pib_base = 0xfee00000UL;

	/* One more step to enable interrupt assist */
	set_bit(ARCH_VMX_INTR_ASSIST, &v->arch.arch_vmx.flags);
	/* Only open one port for I/O and interrupt emulation */
	if (v == d->vcpu[0]) {
	    memset(&d->shared_info->evtchn_mask[0], 0xff,
		sizeof(d->shared_info->evtchn_mask));
	    clear_bit(iopacket_port(d), &d->shared_info->evtchn_mask[0]);
	}

	/* FIXME: only support PMT table continuously by far */
	d->arch.pmt = __va(c->pt_base);

	vmx_final_setup_domain(d);
}

typedef union {
	u64 value;
	struct {
		u64 number : 8;
		u64 revision : 8;
		u64 model : 8;
		u64 family : 8;
		u64 archrev : 8;
		u64 rv : 24;
	};
} cpuid3_t;

/* Allocate vpd from xenheap */
static vpd_t *alloc_vpd(void)
{
	int i;
	cpuid3_t cpuid3;
	vpd_t *vpd;

	vpd = alloc_xenheap_pages(get_order(VPD_SIZE));
	if (!vpd) {
		printk("VPD allocation failed.\n");
		return NULL;
	}

	printk("vpd base: 0x%lx, vpd size:%d\n", vpd, sizeof(vpd_t));
	memset(vpd, 0, VPD_SIZE);
	/* CPUID init */
	for (i = 0; i < 5; i++)
		vpd->vcpuid[i] = ia64_get_cpuid(i);

	/* Limit the CPUID number to 5 */
	cpuid3.value = vpd->vcpuid[3];
	cpuid3.number = 4;	/* 5 - 1 */
	vpd->vcpuid[3] = cpuid3.value;

	vpd->vdc.d_vmsw = 1;
	return vpd;
}


#ifdef CONFIG_VTI
/*
 * Create a VP on intialized VMX environment.
 */
static void
vmx_create_vp(struct vcpu *v)
{
	u64 ret;
	vpd_t *vpd = v->arch.privregs;
	u64 ivt_base;
    extern char vmx_ia64_ivt;
	/* ia64_ivt is function pointer, so need this tranlation */
	ivt_base = (u64) &vmx_ia64_ivt;
	printk("ivt_base: 0x%lx\n", ivt_base);
	ret = ia64_pal_vp_create(vpd, ivt_base, 0);
	if (ret != PAL_STATUS_SUCCESS)
		panic("ia64_pal_vp_create failed. \n");
}

#ifdef XEN_DBL_MAPPING
void vmx_init_double_mapping_stub(void)
{
	u64 base, psr;
	extern void vmx_switch_rr7(void);

	base = (u64) &vmx_switch_rr7;
	base = *((u64*)base);

	psr = ia64_clear_ic();
	ia64_itr(0x1, IA64_TR_RR7_SWITCH_STUB, XEN_RR7_SWITCH_STUB,
		 pte_val(pfn_pte(__pa(base) >> PAGE_SHIFT, PAGE_KERNEL)),
		 RR7_SWITCH_SHIFT);
	ia64_set_psr(psr);
	ia64_srlz_i();
	printk("Add TR mapping for rr7 switch stub, with physical: 0x%lx\n", (u64)(__pa(base)));
}
#endif

/* Other non-context related tasks can be done in context switch */
void
vmx_save_state(struct vcpu *v)
{
	u64 status, psr;
	u64 old_rr0, dom_rr7, rr0_xen_start, rr0_vhpt;

	/* FIXME: about setting of pal_proc_vector... time consuming */
	status = ia64_pal_vp_save(v->arch.privregs, 0);
	if (status != PAL_STATUS_SUCCESS)
		panic("Save vp status failed\n");

#ifdef XEN_DBL_MAPPING
	/* FIXME: Do we really need purge double mapping for old vcpu?
	 * Since rid is completely different between prev and next,
	 * it's not overlap and thus no MCA possible... */
	dom_rr7 = vmx_vrrtomrr(v, VMX(v, vrr[7]));
        vmx_purge_double_mapping(dom_rr7, KERNEL_START,
				 (u64)v->arch.vtlb->ts->vhpt->hash);
#endif

	/* Need to save KR when domain switch, though HV itself doesn;t
	 * use them.
	 */
	v->arch.arch_vmx.vkr[0] = ia64_get_kr(0);
	v->arch.arch_vmx.vkr[1] = ia64_get_kr(1);
	v->arch.arch_vmx.vkr[2] = ia64_get_kr(2);
	v->arch.arch_vmx.vkr[3] = ia64_get_kr(3);
	v->arch.arch_vmx.vkr[4] = ia64_get_kr(4);
	v->arch.arch_vmx.vkr[5] = ia64_get_kr(5);
	v->arch.arch_vmx.vkr[6] = ia64_get_kr(6);
	v->arch.arch_vmx.vkr[7] = ia64_get_kr(7);
}

/* Even guest is in physical mode, we still need such double mapping */
void
vmx_load_state(struct vcpu *v)
{
	u64 status, psr;
	u64 old_rr0, dom_rr7, rr0_xen_start, rr0_vhpt;
	u64 pte_xen, pte_vhpt;
	int i;

	status = ia64_pal_vp_restore(v->arch.privregs, 0);
	if (status != PAL_STATUS_SUCCESS)
		panic("Restore vp status failed\n");

#ifdef XEN_DBL_MAPPING
	dom_rr7 = vmx_vrrtomrr(v, VMX(v, vrr[7]));
	pte_xen = pte_val(pfn_pte((xen_pstart >> PAGE_SHIFT), PAGE_KERNEL));
	pte_vhpt = pte_val(pfn_pte((__pa(v->arch.vtlb->ts->vhpt->hash) >> PAGE_SHIFT), PAGE_KERNEL));
	vmx_insert_double_mapping(dom_rr7, KERNEL_START,
				  (u64)v->arch.vtlb->ts->vhpt->hash,
				  pte_xen, pte_vhpt);
#endif

	ia64_set_kr(0, v->arch.arch_vmx.vkr[0]);
	ia64_set_kr(1, v->arch.arch_vmx.vkr[1]);
	ia64_set_kr(2, v->arch.arch_vmx.vkr[2]);
	ia64_set_kr(3, v->arch.arch_vmx.vkr[3]);
	ia64_set_kr(4, v->arch.arch_vmx.vkr[4]);
	ia64_set_kr(5, v->arch.arch_vmx.vkr[5]);
	ia64_set_kr(6, v->arch.arch_vmx.vkr[6]);
	ia64_set_kr(7, v->arch.arch_vmx.vkr[7]);
	/* Guest vTLB is not required to be switched explicitly, since
	 * anchored in vcpu */
}

#ifdef XEN_DBL_MAPPING
/* Purge old double mapping and insert new one, due to rr7 change */
void
vmx_change_double_mapping(struct vcpu *v, u64 oldrr7, u64 newrr7)
{
	u64 pte_xen, pte_vhpt, vhpt_base;

    vhpt_base = (u64)v->arch.vtlb->ts->vhpt->hash;
    vmx_purge_double_mapping(oldrr7, KERNEL_START,
				 vhpt_base);

	pte_xen = pte_val(pfn_pte((xen_pstart >> PAGE_SHIFT), PAGE_KERNEL));
	pte_vhpt = pte_val(pfn_pte((__pa(vhpt_base) >> PAGE_SHIFT), PAGE_KERNEL));
	vmx_insert_double_mapping(newrr7, KERNEL_START,
				  vhpt_base,
				  pte_xen, pte_vhpt);
}
#endif // XEN_DBL_MAPPING
#endif // CONFIG_VTI

/*
 * Initialize VMX envirenment for guest. Only the 1st vp/vcpu
 * is registered here.
 */
void
vmx_final_setup_domain(struct domain *d)
{
	struct vcpu *v = d->vcpu[0];
	vpd_t *vpd;

	/* Allocate resources for vcpu 0 */
	//memset(&v->arch.arch_vmx, 0, sizeof(struct arch_vmx_struct));

	vpd = alloc_vpd();
	ASSERT(vpd);

//	v->arch.arch_vmx.vpd = vpd;
    v->arch.privregs = vpd;
	vpd->virt_env_vaddr = vm_buffer;

#ifdef CONFIG_VTI
	/* v->arch.schedule_tail = arch_vmx_do_launch; */
	vmx_create_vp(v);

	/* Set this ed to be vmx */
	set_bit(ARCH_VMX_VMCS_LOADED, &v->arch.arch_vmx.flags);

	/* Physical mode emulation initialization, including
	* emulation ID allcation and related memory request
	*/
	physical_mode_init(v);

	vlsapic_reset(v);
	vtm_init(v);
#endif

	/* Other vmx specific initialization work */
}

/*
 * Following stuff should really move to domain builder. However currently
 * XEN/IA64 doesn't export physical -> machine page table to domain builder,
 * instead only the copy. Also there's no hypercall to notify hypervisor
 * IO ranges by far. Let's enhance it later.
 */

#define MEM_G   (1UL << 30)	
#define MEM_M   (1UL << 20)	

#define MMIO_START       (3 * MEM_G)
#define MMIO_SIZE        (512 * MEM_M)

#define VGA_IO_START     0xA0000UL
#define VGA_IO_SIZE      0x20000

#define LEGACY_IO_START  (MMIO_START + MMIO_SIZE)
#define LEGACY_IO_SIZE   (64*MEM_M)  

#define IO_PAGE_START (LEGACY_IO_START + LEGACY_IO_SIZE)
#define IO_PAGE_SIZE  PAGE_SIZE

#define STORE_PAGE_START (IO_PAGE_START + IO_PAGE_SIZE)
#define STORE_PAGE_SIZE	 PAGE_SIZE

#define IO_SAPIC_START   0xfec00000UL
#define IO_SAPIC_SIZE    0x100000

#define PIB_START 0xfee00000UL
#define PIB_SIZE 0x100000 

#define GFW_START        (4*MEM_G -16*MEM_M)
#define GFW_SIZE         (16*MEM_M)

typedef struct io_range {
	unsigned long start;
	unsigned long size;
	unsigned long type;
} io_range_t;

io_range_t io_ranges[] = {
	{VGA_IO_START, VGA_IO_SIZE, GPFN_FRAME_BUFFER},
	{MMIO_START, MMIO_SIZE, GPFN_LOW_MMIO},
	{LEGACY_IO_START, LEGACY_IO_SIZE, GPFN_LEGACY_IO},
	{IO_SAPIC_START, IO_SAPIC_SIZE, GPFN_IOSAPIC},
	{PIB_START, PIB_SIZE, GPFN_PIB},
};

#define VMX_SYS_PAGES	(2 + GFW_SIZE >> PAGE_SHIFT)
#define VMX_CONFIG_PAGES(d) ((d)->max_pages - VMX_SYS_PAGES)

int vmx_alloc_contig_pages(struct domain *d)
{
	unsigned int order, i, j;
	unsigned long start, end, pgnr, conf_nr;
	struct pfn_info *page;
	struct vcpu *v = d->vcpu[0];

	ASSERT(!test_bit(ARCH_VMX_CONTIG_MEM, &v->arch.arch_vmx.flags));

	conf_nr = VMX_CONFIG_PAGES(d);
	order = get_order_from_pages(conf_nr);
	if (unlikely((page = alloc_domheap_pages(d, order, 0)) == NULL)) {
	    printk("Could not allocate order=%d pages for vmx contig alloc\n",
			order);
	    return -1;
	}

	/* Map normal memory below 3G */
	pgnr = page_to_pfn(page);
	end = conf_nr << PAGE_SHIFT;
	for (i = 0;
	     i < (end < MMIO_START ? end : MMIO_START);
	     i += PAGE_SIZE, pgnr++)
	    map_domain_page(d, i, pgnr << PAGE_SHIFT);

	/* Map normal memory beyond 4G */
	if (unlikely(end > MMIO_START)) {
	    start = 4 * MEM_G;
	    end = start + (end - 3 * MEM_G);
	    for (i = start; i < end; i += PAGE_SIZE, pgnr++)
		map_domain_page(d, i, pgnr << PAGE_SHIFT);
	}

	d->arch.max_pfn = end >> PAGE_SHIFT;

	order = get_order_from_pages(VMX_SYS_PAGES);
	if (unlikely((page = alloc_domheap_pages(d, order, 0)) == NULL)) {
	    printk("Could not allocate order=%d pages for vmx contig alloc\n",
			order);
	    return -1;
	}

	/* Map for shared I/O page and xenstore */
	pgnr = page_to_pfn(page);
	map_domain_page(d, IO_PAGE_START, pgnr << PAGE_SHIFT);
	pgnr++;
	map_domain_page(d, STORE_PAGE_START, pgnr << PAGE_SHIFT);
	pgnr++;

	/* Map guest firmware */
	for (i = GFW_START; i < GFW_START + GFW_SIZE; i += PAGE_SIZE, pgnr++)
	    map_domain_page(d, i, pgnr << PAGE_SHIFT);

	/* Mark I/O ranges */
	for (i = 0; i < (sizeof(io_ranges) / sizeof(io_range_t)); i++) {
	    for (j = io_ranges[i].start;
		 j < io_ranges[i].start + io_ranges[i].size;
		 j += PAGE_SIZE)
		map_domain_io_page(d, j);
	}

	set_bit(ARCH_VMX_CONTIG_MEM, &v->arch.arch_vmx.flags);
	return 0;
}
