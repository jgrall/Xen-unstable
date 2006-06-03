
/*
 * Miscellaneous process/domain related routines
 * 
 * Copyright (C) 2004 Hewlett-Packard Co.
 *	Dan Magenheimer (dan.magenheimer@hp.com)
 *
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <asm/ptrace.h>
#include <xen/delay.h>

#include <asm/sal.h>	/* FOR struct ia64_sal_retval */

#include <asm/system.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/desc.h>
//#include <asm/ldt.h>
#include <xen/irq.h>
#include <xen/event.h>
#include <asm/regionreg.h>
#include <asm/privop.h>
#include <asm/vcpu.h>
#include <asm/ia64_int.h>
#include <asm/dom_fw.h>
#include <asm/vhpt.h>
#include "hpsim_ssc.h"
#include <xen/multicall.h>
#include <asm/debugger.h>
#include <asm/fpswa.h>

extern void die_if_kernel(char *str, struct pt_regs *regs, long err);
/* FIXME: where these declarations shold be there ? */
extern void panic_domain(struct pt_regs *, const char *, ...);
extern long platform_is_hp_ski(void);
extern int ia64_hyperprivop(unsigned long, REGS *);
extern IA64FAULT ia64_hypercall(struct pt_regs *regs);
extern void vmx_do_launch(struct vcpu *);
extern unsigned long lookup_domain_mpa(struct domain *,unsigned long);

#define IA64_PSR_CPL1	(__IA64_UL(1) << IA64_PSR_CPL1_BIT)
// note IA64_PSR_PK removed from following, why is this necessary?
#define	DELIVER_PSR_SET	(IA64_PSR_IC | IA64_PSR_I | \
			IA64_PSR_DT | IA64_PSR_RT | IA64_PSR_CPL1 | \
			IA64_PSR_IT | IA64_PSR_BN)

#define	DELIVER_PSR_CLR	(IA64_PSR_AC | IA64_PSR_DFL | IA64_PSR_DFH | \
			IA64_PSR_SP | IA64_PSR_DI | IA64_PSR_SI |	\
			IA64_PSR_DB | IA64_PSR_LP | IA64_PSR_TB | \
			IA64_PSR_CPL | IA64_PSR_MC | IA64_PSR_IS | \
			IA64_PSR_ID | IA64_PSR_DA | IA64_PSR_DD | \
			IA64_PSR_SS | IA64_PSR_RI | IA64_PSR_ED | IA64_PSR_IA)

#include <xen/sched-if.h>

void schedule_tail(struct vcpu *prev)
{
	extern char ia64_ivt;
	context_saved(prev);

	if (VMX_DOMAIN(current)) {
		vmx_do_launch(current);
	} else {
		ia64_set_iva(&ia64_ivt);
        	ia64_set_pta(VHPT_ADDR | (1 << 8) | (VHPT_SIZE_LOG2 << 2) |
		        VHPT_ENABLED);
		load_region_regs(current);
		vcpu_load_kernel_regs(current);
	}
}

void tdpfoo(void) { }

// given a domain virtual address, pte and pagesize, extract the metaphysical
// address, convert the pte for a physical address for (possibly different)
// Xen PAGE_SIZE and return modified pte.  (NOTE: TLB insert should use
// PAGE_SIZE!)
u64 translate_domain_pte(u64 pteval, u64 address, u64 itir__, u64* logps)
{
	struct domain *d = current->domain;
	ia64_itir_t itir = {.itir = itir__};
	u64 mask, mpaddr, pteval2;
	u64 arflags;
	u64 arflags2;

	pteval &= ((1UL << 53) - 1);// ignore [63:53] bits

	// FIXME address had better be pre-validated on insert
	mask = ~itir_mask(itir.itir);
	mpaddr = (((pteval & ~_PAGE_ED) & _PAGE_PPN_MASK) & ~mask) |
	         (address & mask);
#ifdef CONFIG_XEN_IA64_DOM0_VP
	if (itir.ps > PAGE_SHIFT) {
		itir.ps = PAGE_SHIFT;
	}
#endif
	*logps = itir.ps;
#ifndef CONFIG_XEN_IA64_DOM0_VP
	if (d == dom0) {
		if (mpaddr < dom0_start || mpaddr >= dom0_start + dom0_size) {
			/*
			printk("translate_domain_pte: out-of-bounds dom0 mpaddr 0x%lx! itc=%lx...\n",
				mpaddr, ia64_get_itc());
			*/
			tdpfoo();
		}
	}
	else if ((mpaddr >> PAGE_SHIFT) > d->max_pages) {
		/* Address beyond the limit.  However the grant table is
		   also beyond the limit.  Display a message if not in the
		   grant table.  */
		if (mpaddr >= IA64_GRANT_TABLE_PADDR
		    && mpaddr < (IA64_GRANT_TABLE_PADDR 
				 + (ORDER_GRANT_FRAMES << PAGE_SHIFT)))
			printf("translate_domain_pte: bad mpa=0x%lx (> 0x%lx),"
			       "vadr=0x%lx,pteval=0x%lx,itir=0x%lx\n",
			       mpaddr, (unsigned long)d->max_pages<<PAGE_SHIFT,
			       address, pteval, itir.itir);
		tdpfoo();
	}
#endif
	pteval2 = lookup_domain_mpa(d,mpaddr);
	arflags  = pteval  & _PAGE_AR_MASK;
	arflags2 = pteval2 & _PAGE_AR_MASK;
	if (arflags != _PAGE_AR_R && arflags2 == _PAGE_AR_R) {
#if 0
		DPRINTK("%s:%d "
		        "pteval 0x%lx arflag 0x%lx address 0x%lx itir 0x%lx "
		        "pteval2 0x%lx arflags2 0x%lx mpaddr 0x%lx\n",
		        __func__, __LINE__,
		        pteval, arflags, address, itir__,
		        pteval2, arflags2, mpaddr);
#endif
		pteval = (pteval & ~_PAGE_AR_MASK) | _PAGE_AR_R;
}

	pteval2 &= _PAGE_PPN_MASK; // ignore non-addr bits
	pteval2 |= (pteval & _PAGE_ED);
	pteval2 |= _PAGE_PL_2; // force PL0->2 (PL3 is unaffected)
	pteval2 = (pteval & ~_PAGE_PPN_MASK) | pteval2;
	return pteval2;
}

// given a current domain metaphysical address, return the physical address
unsigned long translate_domain_mpaddr(unsigned long mpaddr)
{
	unsigned long pteval;

#ifndef CONFIG_XEN_IA64_DOM0_VP
	if (current->domain == dom0) {
		if (mpaddr < dom0_start || mpaddr >= dom0_start + dom0_size) {
			printk("translate_domain_mpaddr: out-of-bounds dom0 mpaddr 0x%lx! continuing...\n",
				mpaddr);
			tdpfoo();
		}
	}
#endif
	pteval = lookup_domain_mpa(current->domain,mpaddr);
	return ((pteval & _PAGE_PPN_MASK) | (mpaddr & ~PAGE_MASK));
}

unsigned long slow_reflect_count[0x80] = { 0 };
unsigned long fast_reflect_count[0x80] = { 0 };

#define inc_slow_reflect_count(vec) slow_reflect_count[vec>>8]++;

void zero_reflect_counts(void)
{
	int i;
	for (i=0; i<0x80; i++) slow_reflect_count[i] = 0;
	for (i=0; i<0x80; i++) fast_reflect_count[i] = 0;
}

int dump_reflect_counts(char *buf)
{
	int i,j,cnt;
	char *s = buf;

	s += sprintf(s,"Slow reflections by vector:\n");
	for (i = 0, j = 0; i < 0x80; i++) {
		if ( (cnt = slow_reflect_count[i]) != 0 ) {
			s += sprintf(s,"0x%02x00:%10d, ",i,cnt);
			if ((j++ & 3) == 3) s += sprintf(s,"\n");
		}
	}
	if (j & 3) s += sprintf(s,"\n");
	s += sprintf(s,"Fast reflections by vector:\n");
	for (i = 0, j = 0; i < 0x80; i++) {
		if ( (cnt = fast_reflect_count[i]) != 0 ) {
			s += sprintf(s,"0x%02x00:%10d, ",i,cnt);
			if ((j++ & 3) == 3) s += sprintf(s,"\n");
		}
	}
	if (j & 3) s += sprintf(s,"\n");
	return s - buf;
}

// should never panic domain... if it does, stack may have been overrun
void check_bad_nested_interruption(unsigned long isr, struct pt_regs *regs, unsigned long vector)
{
	struct vcpu *v = current;

	if (!(PSCB(v,ipsr) & IA64_PSR_DT)) {
		panic_domain(regs,"psr.dt off, trying to deliver nested dtlb!\n");
	}
	vector &= ~0xf;
	if (vector != IA64_DATA_TLB_VECTOR &&
	    vector != IA64_ALT_DATA_TLB_VECTOR &&
	    vector != IA64_VHPT_TRANS_VECTOR) {
		panic_domain(regs,"psr.ic off, delivering fault=%lx,ipsr=%lx,iip=%lx,ifa=%lx,isr=%lx,PSCB.iip=%lx\n",
		             vector,regs->cr_ipsr,regs->cr_iip,PSCB(v,ifa),isr,PSCB(v,iip));
	}
}

void reflect_interruption(unsigned long isr, struct pt_regs *regs, unsigned long vector)
{
	struct vcpu *v = current;

	if (!PSCB(v,interrupt_collection_enabled))
		check_bad_nested_interruption(isr,regs,vector);
	PSCB(v,unat) = regs->ar_unat;  // not sure if this is really needed?
	PSCB(v,precover_ifs) = regs->cr_ifs;
	vcpu_bsw0(v);
	PSCB(v,ipsr) = vcpu_get_ipsr_int_state(v,regs->cr_ipsr);
	PSCB(v,isr) = isr;
	PSCB(v,iip) = regs->cr_iip;
	PSCB(v,ifs) = 0;
	PSCB(v,incomplete_regframe) = 0;

	regs->cr_iip = ((unsigned long) PSCBX(v,iva) + vector) & ~0xffUL;
	regs->cr_ipsr = (regs->cr_ipsr & ~DELIVER_PSR_CLR) | DELIVER_PSR_SET;
	regs->r31 = XSI_IPSR;

	v->vcpu_info->evtchn_upcall_mask = 1;
	PSCB(v,interrupt_collection_enabled) = 0;

	inc_slow_reflect_count(vector);
}

void foodpi(void) {}

static unsigned long pending_false_positive = 0;

void reflect_extint(struct pt_regs *regs)
{
	unsigned long isr = regs->cr_ipsr & IA64_PSR_RI;
	struct vcpu *v = current;
	static int first_extint = 1;

	if (first_extint) {
		printf("Delivering first extint to domain: isr=0x%lx, iip=0x%lx\n", isr, regs->cr_iip);
		first_extint = 0;
	}
	if (vcpu_timer_pending_early(v))
printf("*#*#*#* about to deliver early timer to domain %d!!!\n",v->domain->domain_id);
	PSCB(current,itir) = 0;
	reflect_interruption(isr,regs,IA64_EXTINT_VECTOR);
}

void reflect_event(struct pt_regs *regs)
{
	unsigned long isr = regs->cr_ipsr & IA64_PSR_RI;
	struct vcpu *v = current;

	/* Sanity check */
	if (is_idle_vcpu(v) || !user_mode(regs)) {
		//printk("WARN: invocation to reflect_event in nested xen\n");
		return;
	}

	if (!event_pending(v))
		return;

	if (!PSCB(v,interrupt_collection_enabled))
		printf("psr.ic off, delivering event, ipsr=%lx,iip=%lx,isr=%lx,viip=0x%lx\n",
		       regs->cr_ipsr, regs->cr_iip, isr, PSCB(v, iip));
	PSCB(v,unat) = regs->ar_unat;  // not sure if this is really needed?
	PSCB(v,precover_ifs) = regs->cr_ifs;
	vcpu_bsw0(v);
	PSCB(v,ipsr) = vcpu_get_ipsr_int_state(v,regs->cr_ipsr);
	PSCB(v,isr) = isr;
	PSCB(v,iip) = regs->cr_iip;
	PSCB(v,ifs) = 0;
	PSCB(v,incomplete_regframe) = 0;

	regs->cr_iip = v->arch.event_callback_ip;
	regs->cr_ipsr = (regs->cr_ipsr & ~DELIVER_PSR_CLR) | DELIVER_PSR_SET;
	regs->r31 = XSI_IPSR;

	v->vcpu_info->evtchn_upcall_mask = 1;
	PSCB(v,interrupt_collection_enabled) = 0;
}

// ONLY gets called from ia64_leave_kernel
// ONLY call with interrupts disabled?? (else might miss one?)
// NEVER successful if already reflecting a trap/fault because psr.i==0
void deliver_pending_interrupt(struct pt_regs *regs)
{
	struct domain *d = current->domain;
	struct vcpu *v = current;
	// FIXME: Will this work properly if doing an RFI???
	if (!is_idle_domain(d) && user_mode(regs)) {
		if (vcpu_deliverable_interrupts(v))
			reflect_extint(regs);
		else if (PSCB(v,pending_interruption))
			++pending_false_positive;
	}
}
unsigned long lazy_cover_count = 0;

static int
handle_lazy_cover(struct vcpu *v, struct pt_regs *regs)
{
	if (!PSCB(v,interrupt_collection_enabled)) {
		PSCB(v,ifs) = regs->cr_ifs;
		PSCB(v,incomplete_regframe) = 1;
		regs->cr_ifs = 0;
		lazy_cover_count++;
		return(1); // retry same instruction with cr.ifs off
	}
	return(0);
}

void ia64_do_page_fault (unsigned long address, unsigned long isr, struct pt_regs *regs, unsigned long itir)
{
	unsigned long iip = regs->cr_iip, iha;
	// FIXME should validate address here
	unsigned long pteval;
	unsigned long is_data = !((isr >> IA64_ISR_X_BIT) & 1UL);
	IA64FAULT fault;

	if ((isr & IA64_ISR_IR) && handle_lazy_cover(current, regs)) return;
	if ((isr & IA64_ISR_SP)
	    || ((isr & IA64_ISR_NA) && (isr & IA64_ISR_CODE_MASK) == IA64_ISR_CODE_LFETCH))
	{
		/*
		 * This fault was due to a speculative load or lfetch.fault, set the "ed"
		 * bit in the psr to ensure forward progress.  (Target register will get a
		 * NaT for ld.s, lfetch will be canceled.)
		 */
		ia64_psr(regs)->ed = 1;
		return;
	}

 again:
	fault = vcpu_translate(current,address,is_data,&pteval,&itir,&iha);
	if (fault == IA64_NO_FAULT || fault == IA64_USE_TLB) {
		u64 logps;
		pteval = translate_domain_pte(pteval, address, itir, &logps);
		vcpu_itc_no_srlz(current,is_data?2:1,address,pteval,-1UL,logps);
		if (fault == IA64_USE_TLB && !current->arch.dtlb.pte.p) {
			/* dtlb has been purged in-between.  This dtlb was
			   matching.  Undo the work.  */
			vcpu_flush_tlb_vhpt_range (address, 1);
			goto again;
		}
		return;
	}

	if (!user_mode (regs)) {
		/* The fault occurs inside Xen.  */
		if (!ia64_done_with_exception(regs)) {
			// should never happen.  If it does, region 0 addr may
			// indicate a bad xen pointer
			printk("*** xen_handle_domain_access: exception table"
			       " lookup failed, iip=0x%lx, addr=0x%lx, spinning...\n",
				iip, address);
			panic_domain(regs,"*** xen_handle_domain_access: exception table"
			       " lookup failed, iip=0x%lx, addr=0x%lx, spinning...\n",
				iip, address);
		}
		return;
	}
	if (!PSCB(current,interrupt_collection_enabled)) {
		check_bad_nested_interruption(isr,regs,fault);
		//printf("Delivering NESTED DATA TLB fault\n");
		fault = IA64_DATA_NESTED_TLB_VECTOR;
		regs->cr_iip = ((unsigned long) PSCBX(current,iva) + fault) & ~0xffUL;
		regs->cr_ipsr = (regs->cr_ipsr & ~DELIVER_PSR_CLR) | DELIVER_PSR_SET;
		// NOTE: nested trap must NOT pass PSCB address
		//regs->r31 = (unsigned long) &PSCB(current);
		inc_slow_reflect_count(fault);
		return;
	}

	PSCB(current,itir) = itir;
	PSCB(current,iha) = iha;
	PSCB(current,ifa) = address;
	reflect_interruption(isr, regs, fault);
}

fpswa_interface_t *fpswa_interface = 0;

void trap_init (void)
{
	if (ia64_boot_param->fpswa)
		/* FPSWA fixup: make the interface pointer a virtual address: */
		fpswa_interface = __va(ia64_boot_param->fpswa);
	else
		printk("No FPSWA supported.\n");
}

static fpswa_ret_t
fp_emulate (int fp_fault, void *bundle, unsigned long *ipsr,
	    unsigned long *fpsr, unsigned long *isr, unsigned long *pr,
	    unsigned long *ifs, struct pt_regs *regs)
{
	fp_state_t fp_state;
	fpswa_ret_t ret;

	if (!fpswa_interface)
		return ((fpswa_ret_t) {-1, 0, 0, 0});

	memset(&fp_state, 0, sizeof(fp_state_t));

	/*
	 * compute fp_state.  only FP registers f6 - f11 are used by the
	 * kernel, so set those bits in the mask and set the low volatile
	 * pointer to point to these registers.
	 */
	fp_state.bitmask_low64 = 0xfc0;  /* bit6..bit11 */

	fp_state.fp_state_low_volatile = (fp_state_low_volatile_t *) &regs->f6;
	/*
	 * unsigned long (*EFI_FPSWA) (
	 *      unsigned long    trap_type,
	 *      void             *Bundle,
	 *      unsigned long    *pipsr,
	 *      unsigned long    *pfsr,
	 *      unsigned long    *pisr,
	 *      unsigned long    *ppreds,
	 *      unsigned long    *pifs,
	 *      void             *fp_state);
	 */
	ret = (*fpswa_interface->fpswa)(fp_fault, bundle,
					ipsr, fpsr, isr, pr, ifs, &fp_state);

	return ret;
}

/*
 * Handle floating-point assist faults and traps for domain.
 */
unsigned long
handle_fpu_swa (int fp_fault, struct pt_regs *regs, unsigned long isr)
{
	struct vcpu *v = current;
	IA64_BUNDLE bundle;
	IA64_BUNDLE __get_domain_bundle(UINT64);
	unsigned long fault_ip;
	fpswa_ret_t ret;

	fault_ip = regs->cr_iip;
	/*
	 * When the FP trap occurs, the trapping instruction is completed.
	 * If ipsr.ri == 0, there is the trapping instruction in previous bundle.
	 */
	if (!fp_fault && (ia64_psr(regs)->ri == 0))
		fault_ip -= 16;
	bundle = __get_domain_bundle(fault_ip);
	if (!bundle.i64[0] && !bundle.i64[1]) {
		printk("%s: floating-point bundle at 0x%lx not mapped\n",
		       __FUNCTION__, fault_ip);
		return -1;
	}

	ret = fp_emulate(fp_fault, &bundle, &regs->cr_ipsr, &regs->ar_fpsr,
	                 &isr, &regs->pr, &regs->cr_ifs, regs);

	if (ret.status) {
		PSCBX(v, fpswa_ret) = ret;
		printk("%s(%s): fp_emulate() returned %ld\n",
		       __FUNCTION__, fp_fault?"fault":"trap", ret.status);
	}

	return ret.status;
}

void
ia64_fault (unsigned long vector, unsigned long isr, unsigned long ifa,
	    unsigned long iim, unsigned long itir, unsigned long arg5,
	    unsigned long arg6, unsigned long arg7, unsigned long stack)
{
	struct pt_regs *regs = (struct pt_regs *) &stack;
	unsigned long code;
	static const char *reason[] = {
		"IA-64 Illegal Operation fault",
		"IA-64 Privileged Operation fault",
		"IA-64 Privileged Register fault",
		"IA-64 Reserved Register/Field fault",
		"Disabled Instruction Set Transition fault",
		"Unknown fault 5", "Unknown fault 6", "Unknown fault 7", "Illegal Hazard fault",
		"Unknown fault 9", "Unknown fault 10", "Unknown fault 11", "Unknown fault 12",
		"Unknown fault 13", "Unknown fault 14", "Unknown fault 15"
	};

	printf("ia64_fault, vector=0x%lx, ifa=0x%016lx, iip=0x%016lx, ipsr=0x%016lx, isr=0x%016lx\n",
	       vector, ifa, regs->cr_iip, regs->cr_ipsr, isr);


	if ((isr & IA64_ISR_NA) && ((isr & IA64_ISR_CODE_MASK) == IA64_ISR_CODE_LFETCH)) {
		/*
		 * This fault was due to lfetch.fault, set "ed" bit in the psr to cancel
		 * the lfetch.
		 */
		ia64_psr(regs)->ed = 1;
		printf("ia64_fault: handled lfetch.fault\n");
		return;
	}

	switch (vector) {
	    case 0:
		printk("VHPT Translation.\n");
		break;
	  
	    case 4:
		printk("Alt DTLB.\n");
		break;
	  
	    case 6:
		printk("Instruction Key Miss.\n");
		break;

	    case 7: 
		printk("Data Key Miss.\n");
		break;

	    case 8: 
		printk("Dirty-bit.\n");
		break;

	    case 20:
		printk("Page Not Found.\n");
		break;

	    case 21:
		printk("Key Permission.\n");
		break;

	    case 22:
		printk("Instruction Access Rights.\n");
		break;

	    case 24: /* General Exception */
		code = (isr >> 4) & 0xf;
		printk("General Exception: %s%s.\n", reason[code],
		        (code == 3) ? ((isr & (1UL << 37)) ? " (RSE access)" :
		                       " (data access)") : "");
		if (code == 8) {
# ifdef CONFIG_IA64_PRINT_HAZARDS
			printk("%s[%d]: possible hazard @ ip=%016lx (pr = %016lx)\n",
			       current->comm, current->pid,
			       regs->cr_iip + ia64_psr(regs)->ri,
			       regs->pr);
# endif
			printf("ia64_fault: returning on hazard\n");
			return;
		}
		break;

	    case 25:
		printk("Disabled FP-Register.\n");
		break;

	    case 26:
		printk("NaT consumption.\n");
		break;

	    case 29:
		printk("Debug.\n");
		break;

	    case 30:
		printk("Unaligned Reference.\n");
		break;

	    case 31:
		printk("Unsupported data reference.\n");
		break;

	    case 32:
		printk("Floating-Point Fault.\n");
		break;

	    case 33:
		printk("Floating-Point Trap.\n");
		break;

	    case 34:
		printk("Lower Privilege Transfer Trap.\n");
		break;

	    case 35:
		printk("Taken Branch Trap.\n");
		break;

	    case 36:
		printk("Single Step Trap.\n");
		break;
    
	    case 45:
		printk("IA-32 Exception.\n");
		break;

	    case 46:
		printk("IA-32 Intercept.\n");
		break;

	    case 47:
		printk("IA-32 Interrupt.\n");
		break;

	    default:
		printk("Fault %lu\n", vector);
		break;
	}

	show_registers(regs);
	panic("Fault in Xen.\n");
}

unsigned long running_on_sim = 0;

void
do_ssc(unsigned long ssc, struct pt_regs *regs)
{
	unsigned long arg0, arg1, arg2, arg3, retval;
	char buf[2];
/**/	static int last_fd, last_count;	// FIXME FIXME FIXME
/**/					// BROKEN FOR MULTIPLE DOMAINS & SMP
/**/	struct ssc_disk_stat { int fd; unsigned count;} *stat, last_stat;

	arg0 = vcpu_get_gr(current,32);
	switch(ssc) {
	    case SSC_PUTCHAR:
		buf[0] = arg0;
		buf[1] = '\0';
		printf(buf);
		break;
	    case SSC_GETCHAR:
		retval = ia64_ssc(0,0,0,0,ssc);
		vcpu_set_gr(current,8,retval,0);
		break;
	    case SSC_WAIT_COMPLETION:
		if (arg0) {	// metaphysical address

			arg0 = translate_domain_mpaddr(arg0);
/**/			stat = (struct ssc_disk_stat *)__va(arg0);
///**/			if (stat->fd == last_fd) stat->count = last_count;
/**/			stat->count = last_count;
//if (last_count >= PAGE_SIZE) printf("ssc_wait: stat->fd=%d,last_fd=%d,last_count=%d\n",stat->fd,last_fd,last_count);
///**/			retval = ia64_ssc(arg0,0,0,0,ssc);
/**/			retval = 0;
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval,0);
		break;
	    case SSC_OPEN:
		arg1 = vcpu_get_gr(current,33);	// access rights
if (!running_on_sim) { printf("SSC_OPEN, not implemented on hardware.  (ignoring...)\n"); arg0 = 0; }
		if (arg0) {	// metaphysical address
			arg0 = translate_domain_mpaddr(arg0);
			retval = ia64_ssc(arg0,arg1,0,0,ssc);
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval,0);
		break;
	    case SSC_WRITE:
	    case SSC_READ:
//if (ssc == SSC_WRITE) printf("DOING AN SSC_WRITE\n");
		arg1 = vcpu_get_gr(current,33);
		arg2 = vcpu_get_gr(current,34);
		arg3 = vcpu_get_gr(current,35);
		if (arg2) {	// metaphysical address of descriptor
			struct ssc_disk_req *req;
			unsigned long mpaddr;
			long len;

			arg2 = translate_domain_mpaddr(arg2);
			req = (struct ssc_disk_req *) __va(arg2);
			req->len &= 0xffffffffL;	// avoid strange bug
			len = req->len;
/**/			last_fd = arg1;
/**/			last_count = len;
			mpaddr = req->addr;
//if (last_count >= PAGE_SIZE) printf("do_ssc: read fd=%d, addr=%p, len=%lx ",last_fd,mpaddr,len);
			retval = 0;
			if ((mpaddr & PAGE_MASK) != ((mpaddr+len-1) & PAGE_MASK)) {
				// do partial page first
				req->addr = translate_domain_mpaddr(mpaddr);
				req->len = PAGE_SIZE - (req->addr & ~PAGE_MASK);
				len -= req->len; mpaddr += req->len;
				retval = ia64_ssc(arg0,arg1,arg2,arg3,ssc);
				arg3 += req->len; // file offset
/**/				last_stat.fd = last_fd;
/**/				(void)ia64_ssc(__pa(&last_stat),0,0,0,SSC_WAIT_COMPLETION);
//if (last_count >= PAGE_SIZE) printf("ssc(%p,%lx)[part]=%x ",req->addr,req->len,retval);
			}
			if (retval >= 0) while (len > 0) {
				req->addr = translate_domain_mpaddr(mpaddr);
				req->len = (len > PAGE_SIZE) ? PAGE_SIZE : len;
				len -= PAGE_SIZE; mpaddr += PAGE_SIZE;
				retval = ia64_ssc(arg0,arg1,arg2,arg3,ssc);
				arg3 += req->len; // file offset
// TEMP REMOVED AGAIN				arg3 += req->len; // file offset
/**/				last_stat.fd = last_fd;
/**/				(void)ia64_ssc(__pa(&last_stat),0,0,0,SSC_WAIT_COMPLETION);
//if (last_count >= PAGE_SIZE) printf("ssc(%p,%lx)=%x ",req->addr,req->len,retval);
			}
			// set it back to the original value
			req->len = last_count;
		}
		else retval = -1L;
		vcpu_set_gr(current,8,retval,0);
//if (last_count >= PAGE_SIZE) printf("retval=%x\n",retval);
		break;
	    case SSC_CONNECT_INTERRUPT:
		arg1 = vcpu_get_gr(current,33);
		arg2 = vcpu_get_gr(current,34);
		arg3 = vcpu_get_gr(current,35);
		if (!running_on_sim) { printf("SSC_CONNECT_INTERRUPT, not implemented on hardware.  (ignoring...)\n"); break; }
		(void)ia64_ssc(arg0,arg1,arg2,arg3,ssc);
		break;
	    case SSC_NETDEV_PROBE:
		vcpu_set_gr(current,8,-1L,0);
		break;
	    default:
		printf("ia64_handle_break: bad ssc code %lx, iip=0x%lx, b0=0x%lx... spinning\n",
			ssc, regs->cr_iip, regs->b0);
		while(1);
		break;
	}
	vcpu_increment_iip(current);
}

/* Also read in hyperprivop.S  */
int first_break = 1;

void
ia64_handle_break (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long iim)
{
	struct domain *d = current->domain;
	struct vcpu *v = current;
	IA64FAULT vector;

	if (first_break) {
		if (platform_is_hp_ski()) running_on_sim = 1;
		else running_on_sim = 0;
		first_break = 0;
	}
	if (iim == 0x80001 || iim == 0x80002) {	//FIXME: don't hardcode constant
		do_ssc(vcpu_get_gr(current,36), regs);
	} 
#ifdef CRASH_DEBUG
	else if ((iim == 0 || iim == CDB_BREAK_NUM) && !user_mode(regs)) {
		if (iim == 0)
			show_registers(regs);
		debugger_trap_fatal(0 /* don't care */, regs);
	} 
#endif
	else if (iim == d->arch.breakimm) {
		/* by default, do not continue */
		v->arch.hypercall_continuation = 0;

		if ((vector = ia64_hypercall(regs)) == IA64_NO_FAULT) {
			if (!PSCBX(v, hypercall_continuation))
				vcpu_increment_iip(current);
		}
		else reflect_interruption(isr, regs, vector);
	}
	else if (!PSCB(v,interrupt_collection_enabled)) {
		if (ia64_hyperprivop(iim,regs))
			vcpu_increment_iip(current);
	}
	else {
		if (iim == 0) 
			die_if_kernel("bug check", regs, iim);
		PSCB(v,iim) = iim;
		reflect_interruption(isr,regs,IA64_BREAK_VECTOR);
	}
}

void
ia64_handle_privop (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long itir)
{
	IA64FAULT vector;

	vector = priv_emulate(current,regs,isr);
	if (vector != IA64_NO_FAULT && vector != IA64_RFI_IN_PROGRESS) {
		// Note: if a path results in a vector to reflect that requires
		// iha/itir (e.g. vcpu_force_data_miss), they must be set there
		reflect_interruption(isr,regs,vector);
	}
}

/* Used in vhpt.h.  */
#define INTR_TYPE_MAX	10
UINT64 int_counts[INTR_TYPE_MAX];

void
ia64_handle_reflection (unsigned long ifa, struct pt_regs *regs, unsigned long isr, unsigned long iim, unsigned long vector)
{
	struct vcpu *v = current;
	unsigned long check_lazy_cover = 0;
	unsigned long psr = regs->cr_ipsr;

	/* Following faults shouldn'g be seen from Xen itself */
	if (!(psr & IA64_PSR_CPL)) BUG();

	switch(vector) {
	    case 8:
		vector = IA64_DIRTY_BIT_VECTOR; break;
	    case 9:
		vector = IA64_INST_ACCESS_BIT_VECTOR; break;
	    case 10:
		check_lazy_cover = 1;
		vector = IA64_DATA_ACCESS_BIT_VECTOR; break;
	    case 20:
		check_lazy_cover = 1;
		vector = IA64_PAGE_NOT_PRESENT_VECTOR; break;
	    case 22:
		vector = IA64_INST_ACCESS_RIGHTS_VECTOR; break;
	    case 23:
		check_lazy_cover = 1;
		vector = IA64_DATA_ACCESS_RIGHTS_VECTOR; break;
	    case 25:
		vector = IA64_DISABLED_FPREG_VECTOR;
		break;
	    case 26:
		if (((isr >> 4L) & 0xfL) == 1) {
			//regs->eml_unat = 0;  FIXME: DO WE NEED THIS??
			printf("ia64_handle_reflection: handling regNaT fault\n");
			vector = IA64_NAT_CONSUMPTION_VECTOR; break;
		}
#if 1
		// pass null pointer dereferences through with no error
		// but retain debug output for non-zero ifa
		if (!ifa) {
			vector = IA64_NAT_CONSUMPTION_VECTOR; break;
		}
#endif
		printf("*** NaT fault... attempting to handle as privop\n");
		printf("isr=%016lx, ifa=%016lx, iip=%016lx, ipsr=%016lx\n",
		       isr, ifa, regs->cr_iip, psr);
		//regs->eml_unat = 0;  FIXME: DO WE NEED THIS???
		// certain NaT faults are higher priority than privop faults
		vector = priv_emulate(v,regs,isr);
		if (vector == IA64_NO_FAULT) {
			printf("*** Handled privop masquerading as NaT fault\n");
			return;
		}
		vector = IA64_NAT_CONSUMPTION_VECTOR; break;
	    case 27:
		//printf("*** Handled speculation vector, itc=%lx!\n",ia64_get_itc());
		PSCB(current,iim) = iim;
		vector = IA64_SPECULATION_VECTOR; break;
	    case 30:
		// FIXME: Should we handle unaligned refs in Xen??
		vector = IA64_UNALIGNED_REF_VECTOR; break;
	    case 32:
		if (!(handle_fpu_swa(1, regs, isr))) {
		    vcpu_increment_iip(v);
		    return;
		}
		printf("ia64_handle_reflection: handling FP fault\n");
		vector = IA64_FP_FAULT_VECTOR; break;
	    case 33:
		if (!(handle_fpu_swa(0, regs, isr))) return;
		printf("ia64_handle_reflection: handling FP trap\n");
		vector = IA64_FP_TRAP_VECTOR; break;
	    case 34:
		printf("ia64_handle_reflection: handling lowerpriv trap\n");
		vector = IA64_LOWERPRIV_TRANSFER_TRAP_VECTOR; break;
	    case 35:
		printf("ia64_handle_reflection: handling taken branch trap\n");
		vector = IA64_TAKEN_BRANCH_TRAP_VECTOR; break;
	    case 36:
		printf("ia64_handle_reflection: handling single step trap\n");
		vector = IA64_SINGLE_STEP_TRAP_VECTOR; break;

	    default:
		printf("ia64_handle_reflection: unhandled vector=0x%lx\n",vector);
		while(vector);
		return;
	}
	if (check_lazy_cover && (isr & IA64_ISR_IR) && handle_lazy_cover(v, regs)) return;
	PSCB(current,ifa) = ifa;
	PSCB(current,itir) = vcpu_get_itir_on_fault(v,ifa);
	reflect_interruption(isr,regs,vector);
}

unsigned long hypercall_create_continuation(
	unsigned int op, const char *format, ...)
{
    struct mc_state *mcs = &mc_state[smp_processor_id()];
    struct vcpu *v = current;
    const char *p = format;
    unsigned long arg;
    unsigned int i;
    va_list args;

    va_start(args, format);
    if ( test_bit(_MCSF_in_multicall, &mcs->flags) ) {
	panic("PREEMPT happen in multicall\n");	// Not support yet
    } else {
	vcpu_set_gr(v, 2, op, 0);
	for ( i = 0; *p != '\0'; i++) {
            switch ( *p++ )
            {
            case 'i':
                arg = (unsigned long)va_arg(args, unsigned int);
                break;
            case 'l':
                arg = (unsigned long)va_arg(args, unsigned long);
                break;
            case 'h':
                arg = (unsigned long)va_arg(args, void *);
                break;
            default:
                arg = 0;
                BUG();
            }
	    switch (i) {
	    case 0: vcpu_set_gr(v, 14, arg, 0);
		    break;
	    case 1: vcpu_set_gr(v, 15, arg, 0);
		    break;
	    case 2: vcpu_set_gr(v, 16, arg, 0);
		    break;
	    case 3: vcpu_set_gr(v, 17, arg, 0);
		    break;
	    case 4: vcpu_set_gr(v, 18, arg, 0);
		    break;
	    default: panic("Too many args for hypercall continuation\n");
		    break;
	    }
	}
    }
    v->arch.hypercall_continuation = 1;
    va_end(args);
    return op;
}

