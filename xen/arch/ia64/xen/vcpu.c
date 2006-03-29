/*
 * Virtualized CPU functions
 *
 * Copyright (C) 2004-2005 Hewlett-Packard Co.
 *	Dan Magenheimer (dan.magenheimer@hp.com)
 *
 */

#include <linux/sched.h>
#include <public/arch-ia64.h>
#include <asm/ia64_int.h>
#include <asm/vcpu.h>
#include <asm/regionreg.h>
#include <asm/tlb.h>
#include <asm/processor.h>
#include <asm/delay.h>
#include <asm/vmx_vcpu.h>
#include <asm/vhpt.h>
#include <asm/tlbflush.h>
#include <xen/event.h>

/* FIXME: where these declarations should be there ? */
extern void getreg(unsigned long regnum, unsigned long *val, int *nat, struct pt_regs *regs);
extern void setreg(unsigned long regnum, unsigned long val, int nat, struct pt_regs *regs);
extern void panic_domain(struct pt_regs *, const char *, ...);
extern int set_metaphysical_rr0(void);
extern unsigned long translate_domain_pte(UINT64,UINT64,UINT64);
extern unsigned long translate_domain_mpaddr(unsigned long);
extern void ia64_global_tlb_purge(UINT64 start, UINT64 end, UINT64 nbits);


typedef	union {
	struct ia64_psr ia64_psr;
	unsigned long i64;
} PSR;

// this def for vcpu_regs won't work if kernel stack is present
//#define	vcpu_regs(vcpu) ((struct pt_regs *) vcpu->arch.regs

#define	TRUE	1
#define	FALSE	0
#define	IA64_PTA_SZ_BIT		2
#define	IA64_PTA_VF_BIT		8
#define	IA64_PTA_BASE_BIT	15
#define	IA64_PTA_LFMT		(1UL << IA64_PTA_VF_BIT)
#define	IA64_PTA_SZ(x)	(x##UL << IA64_PTA_SZ_BIT)

#define STATIC

#ifdef PRIVOP_ADDR_COUNT
struct privop_addr_count privop_addr_counter[PRIVOP_COUNT_NINSTS+1] = {
	{ "=ifa",  { 0 }, { 0 }, 0 },
	{ "thash", { 0 }, { 0 }, 0 },
	{ 0,       { 0 }, { 0 }, 0 }
};
extern void privop_count_addr(unsigned long addr, int inst);
#define	PRIVOP_COUNT_ADDR(regs,inst) privop_count_addr(regs->cr_iip,inst)
#else
#define	PRIVOP_COUNT_ADDR(x,y) do {} while (0)
#endif

unsigned long dtlb_translate_count = 0;
unsigned long tr_translate_count = 0;
unsigned long phys_translate_count = 0;

unsigned long vcpu_verbose = 0;

/**************************************************************************
 VCPU general register access routines
**************************************************************************/
#ifdef XEN
UINT64
vcpu_get_gr(VCPU *vcpu, unsigned long reg)
{
	REGS *regs = vcpu_regs(vcpu);
	UINT64 val;

	if (!reg) return 0;
	getreg(reg,&val,0,regs);	// FIXME: handle NATs later
	return val;
}
IA64FAULT
vcpu_get_gr_nat(VCPU *vcpu, unsigned long reg, UINT64 *val)
{
	REGS *regs = vcpu_regs(vcpu);
	int nat;

	getreg(reg,val,&nat,regs);	// FIXME: handle NATs later
	if (nat)
		return IA64_NAT_CONSUMPTION_VECTOR;
	return 0;
}

// returns:
//   IA64_ILLOP_FAULT if the register would cause an Illegal Operation fault
//   IA64_NO_FAULT otherwise
IA64FAULT
vcpu_set_gr(VCPU *vcpu, unsigned long reg, UINT64 value, int nat)
{
	REGS *regs = vcpu_regs(vcpu);
	long sof = (regs->cr_ifs) & 0x7f;

	if (!reg) return IA64_ILLOP_FAULT;
	if (reg >= sof + 32) return IA64_ILLOP_FAULT;
	setreg(reg,value,nat,regs);	// FIXME: handle NATs later
	return IA64_NO_FAULT;
}
#else
// returns:
//   IA64_ILLOP_FAULT if the register would cause an Illegal Operation fault
//   IA64_NO_FAULT otherwise
IA64FAULT
vcpu_set_gr(VCPU *vcpu, unsigned long reg, UINT64 value)
{
	REGS *regs = vcpu_regs(vcpu);
	long sof = (regs->cr_ifs) & 0x7f;

	if (!reg) return IA64_ILLOP_FAULT;
	if (reg >= sof + 32) return IA64_ILLOP_FAULT;
	setreg(reg,value,0,regs);	// FIXME: handle NATs later
	return IA64_NO_FAULT;
}

#endif
/**************************************************************************
 VCPU privileged application register access routines
**************************************************************************/

void vcpu_load_kernel_regs(VCPU *vcpu)
{
	ia64_set_kr(0, VCPU(vcpu, krs[0]));
	ia64_set_kr(1, VCPU(vcpu, krs[1]));
	ia64_set_kr(2, VCPU(vcpu, krs[2]));
	ia64_set_kr(3, VCPU(vcpu, krs[3]));
	ia64_set_kr(4, VCPU(vcpu, krs[4]));
	ia64_set_kr(5, VCPU(vcpu, krs[5]));
	ia64_set_kr(6, VCPU(vcpu, krs[6]));
	ia64_set_kr(7, VCPU(vcpu, krs[7]));
}

/* GCC 4.0.2 seems not to be able to suppress this call!.  */
#define ia64_setreg_unknown_kr() return IA64_ILLOP_FAULT

IA64FAULT vcpu_set_ar(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	if (reg == 44) return (vcpu_set_itc(vcpu,val));
	else if (reg == 27) return (IA64_ILLOP_FAULT);
	else if (reg == 24)
	    printf("warning: setting ar.eflg is a no-op; no IA-32 support\n");
	else if (reg > 7) return (IA64_ILLOP_FAULT);
	else {
		PSCB(vcpu,krs[reg]) = val;
		ia64_set_kr(reg,val);
	}
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_get_ar(VCPU *vcpu, UINT64 reg, UINT64 *val)
{
	if (reg == 24)
	    printf("warning: getting ar.eflg is a no-op; no IA-32 support\n");
	else if (reg > 7) return (IA64_ILLOP_FAULT);
	else *val = PSCB(vcpu,krs[reg]);
	return IA64_NO_FAULT;
}

/**************************************************************************
 VCPU processor status register access routines
**************************************************************************/

void vcpu_set_metaphysical_mode(VCPU *vcpu, BOOLEAN newmode)
{
	/* only do something if mode changes */
	if (!!newmode ^ !!PSCB(vcpu,metaphysical_mode)) {
		PSCB(vcpu,metaphysical_mode) = newmode;
		if (newmode) set_metaphysical_rr0();
		else if (PSCB(vcpu,rrs[0]) != -1)
			set_one_rr(0, PSCB(vcpu,rrs[0]));
	}
}

IA64FAULT vcpu_reset_psr_dt(VCPU *vcpu)
{
	vcpu_set_metaphysical_mode(vcpu,TRUE);
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_reset_psr_sm(VCPU *vcpu, UINT64 imm24)
{
	struct ia64_psr psr, imm, *ipsr;
	REGS *regs = vcpu_regs(vcpu);

	//PRIVOP_COUNT_ADDR(regs,_RSM);
	// TODO: All of these bits need to be virtualized
	// TODO: Only allowed for current vcpu
	__asm__ __volatile ("mov %0=psr;;" : "=r"(psr) :: "memory");
	ipsr = (struct ia64_psr *)&regs->cr_ipsr;
	imm = *(struct ia64_psr *)&imm24;
	// interrupt flag
	if (imm.i) PSCB(vcpu,interrupt_delivery_enabled) = 0;
	if (imm.ic)  PSCB(vcpu,interrupt_collection_enabled) = 0;
	// interrupt collection flag
	//if (imm.ic) PSCB(vcpu,interrupt_delivery_enabled) = 0;
	// just handle psr.up and psr.pp for now
	if (imm24 & ~(IA64_PSR_BE | IA64_PSR_PP | IA64_PSR_UP | IA64_PSR_SP
		| IA64_PSR_I | IA64_PSR_IC | IA64_PSR_DT
		| IA64_PSR_DFL | IA64_PSR_DFH))
			return (IA64_ILLOP_FAULT);
	if (imm.dfh) ipsr->dfh = 0;
	if (imm.dfl) ipsr->dfl = 0;
	if (imm.pp) {
		ipsr->pp = 1;
		psr.pp = 1;	// priv perf ctrs always enabled
// FIXME: need new field in mapped_regs_t for virtual psr.pp (psr.be too?)
		PSCB(vcpu,tmp[8]) = 0;	// but fool the domain if it gets psr
	}
	if (imm.up) { ipsr->up = 0; psr.up = 0; }
	if (imm.sp) { ipsr->sp = 0; psr.sp = 0; }
	if (imm.be) ipsr->be = 0;
	if (imm.dt) vcpu_set_metaphysical_mode(vcpu,TRUE);
	__asm__ __volatile (";; mov psr.l=%0;; srlz.d"::"r"(psr):"memory");
	return IA64_NO_FAULT;
}

#define SPURIOUS_VECTOR 0xf

IA64FAULT vcpu_set_psr_dt(VCPU *vcpu)
{
	vcpu_set_metaphysical_mode(vcpu,FALSE);
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_psr_i(VCPU *vcpu)
{
	PSCB(vcpu,interrupt_delivery_enabled) = 1;
	PSCB(vcpu,interrupt_collection_enabled) = 1;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_psr_sm(VCPU *vcpu, UINT64 imm24)
{
	struct ia64_psr psr, imm, *ipsr;
	REGS *regs = vcpu_regs(vcpu);
	UINT64 mask, enabling_interrupts = 0;

	//PRIVOP_COUNT_ADDR(regs,_SSM);
	// TODO: All of these bits need to be virtualized
	__asm__ __volatile ("mov %0=psr;;" : "=r"(psr) :: "memory");
	imm = *(struct ia64_psr *)&imm24;
	ipsr = (struct ia64_psr *)&regs->cr_ipsr;
	// just handle psr.sp,pp and psr.i,ic (and user mask) for now
	mask = IA64_PSR_PP|IA64_PSR_SP|IA64_PSR_I|IA64_PSR_IC|IA64_PSR_UM |
		IA64_PSR_DT|IA64_PSR_DFL|IA64_PSR_DFH;
	if (imm24 & ~mask) return (IA64_ILLOP_FAULT);
	if (imm.dfh) ipsr->dfh = 1;
	if (imm.dfl) ipsr->dfl = 1;
	if (imm.pp) {
		ipsr->pp = 1; psr.pp = 1;
// FIXME: need new field in mapped_regs_t for virtual psr.pp (psr.be too?)
		PSCB(vcpu,tmp[8]) = 1;
	}
	if (imm.sp) { ipsr->sp = 1; psr.sp = 1; }
	if (imm.i) {
		if (!PSCB(vcpu,interrupt_delivery_enabled)) {
//printf("vcpu_set_psr_sm: psr.ic 0->1 ");
			enabling_interrupts = 1;
		}
		PSCB(vcpu,interrupt_delivery_enabled) = 1;
	}
	if (imm.ic)  PSCB(vcpu,interrupt_collection_enabled) = 1;
	// TODO: do this faster
	if (imm.mfl) { ipsr->mfl = 1; psr.mfl = 1; }
	if (imm.mfh) { ipsr->mfh = 1; psr.mfh = 1; }
	if (imm.ac) { ipsr->ac = 1; psr.ac = 1; }
	if (imm.up) { ipsr->up = 1; psr.up = 1; }
	if (imm.be) {
		printf("*** DOMAIN TRYING TO TURN ON BIG-ENDIAN!!!\n");
		return (IA64_ILLOP_FAULT);
	}
	if (imm.dt) vcpu_set_metaphysical_mode(vcpu,FALSE);
	__asm__ __volatile (";; mov psr.l=%0;; srlz.d"::"r"(psr):"memory");
	if (enabling_interrupts &&
		vcpu_check_pending_interrupts(vcpu) != SPURIOUS_VECTOR)
			PSCB(vcpu,pending_interruption) = 1;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_psr_l(VCPU *vcpu, UINT64 val)
{
	struct ia64_psr psr, newpsr, *ipsr;
	REGS *regs = vcpu_regs(vcpu);
	UINT64 enabling_interrupts = 0;

	// TODO: All of these bits need to be virtualized
	__asm__ __volatile ("mov %0=psr;;" : "=r"(psr) :: "memory");
	newpsr = *(struct ia64_psr *)&val;
	ipsr = (struct ia64_psr *)&regs->cr_ipsr;
	// just handle psr.up and psr.pp for now
	//if (val & ~(IA64_PSR_PP | IA64_PSR_UP | IA64_PSR_SP)) return (IA64_ILLOP_FAULT);
	// however trying to set other bits can't be an error as it is in ssm
	if (newpsr.dfh) ipsr->dfh = 1;
	if (newpsr.dfl) ipsr->dfl = 1;
	if (newpsr.pp) {
		ipsr->pp = 1; psr.pp = 1;
// FIXME: need new field in mapped_regs_t for virtual psr.pp (psr.be too?)
		PSCB(vcpu,tmp[8]) = 1;
	}
	else {
		ipsr->pp = 1; psr.pp = 1;
		PSCB(vcpu,tmp[8]) = 0;
	}
	if (newpsr.up) { ipsr->up = 1; psr.up = 1; }
	if (newpsr.sp) { ipsr->sp = 1; psr.sp = 1; }
	if (newpsr.i) {
		if (!PSCB(vcpu,interrupt_delivery_enabled))
			enabling_interrupts = 1;
		PSCB(vcpu,interrupt_delivery_enabled) = 1;
	}
	if (newpsr.ic)  PSCB(vcpu,interrupt_collection_enabled) = 1;
	if (newpsr.mfl) { ipsr->mfl = 1; psr.mfl = 1; }
	if (newpsr.mfh) { ipsr->mfh = 1; psr.mfh = 1; }
	if (newpsr.ac) { ipsr->ac = 1; psr.ac = 1; }
	if (newpsr.up) { ipsr->up = 1; psr.up = 1; }
	if (newpsr.dt && newpsr.rt) vcpu_set_metaphysical_mode(vcpu,FALSE);
	else vcpu_set_metaphysical_mode(vcpu,TRUE);
	if (newpsr.be) {
		printf("*** DOMAIN TRYING TO TURN ON BIG-ENDIAN!!!\n");
		return (IA64_ILLOP_FAULT);
	}
	if (enabling_interrupts &&
		vcpu_check_pending_interrupts(vcpu) != SPURIOUS_VECTOR)
			PSCB(vcpu,pending_interruption) = 1;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_get_psr(VCPU *vcpu, UINT64 *pval)
{
	REGS *regs = vcpu_regs(vcpu);
	struct ia64_psr newpsr;

	newpsr = *(struct ia64_psr *)&regs->cr_ipsr;
	if (newpsr.cpl == 2) newpsr.cpl = 0;
	if (PSCB(vcpu,interrupt_delivery_enabled)) newpsr.i = 1;
	else newpsr.i = 0;
	if (PSCB(vcpu,interrupt_collection_enabled)) newpsr.ic = 1;
	else newpsr.ic = 0;
	if (PSCB(vcpu,metaphysical_mode)) newpsr.dt = 0;
	else newpsr.dt = 1;
// FIXME: need new field in mapped_regs_t for virtual psr.pp (psr.be too?)
	if (PSCB(vcpu,tmp[8])) newpsr.pp = 1;
	else newpsr.pp = 0;
	*pval = *(unsigned long *)&newpsr;
	return IA64_NO_FAULT;
}

BOOLEAN vcpu_get_psr_ic(VCPU *vcpu)
{
	return !!PSCB(vcpu,interrupt_collection_enabled);
}

BOOLEAN vcpu_get_psr_i(VCPU *vcpu)
{
	return !!PSCB(vcpu,interrupt_delivery_enabled);
}

UINT64 vcpu_get_ipsr_int_state(VCPU *vcpu,UINT64 prevpsr)
{
	UINT64 dcr = PSCBX(vcpu,dcr);
	PSR psr;

	//printf("*** vcpu_get_ipsr_int_state (0x%016lx)...",prevpsr);
	psr.i64 = prevpsr;
	psr.ia64_psr.be = 0; if (dcr & IA64_DCR_BE) psr.ia64_psr.be = 1;
	psr.ia64_psr.pp = 0; if (dcr & IA64_DCR_PP) psr.ia64_psr.pp = 1;
	psr.ia64_psr.ic = PSCB(vcpu,interrupt_collection_enabled);
	psr.ia64_psr.i = PSCB(vcpu,interrupt_delivery_enabled);
	psr.ia64_psr.bn = PSCB(vcpu,banknum);
	psr.ia64_psr.dt = 1; psr.ia64_psr.it = 1; psr.ia64_psr.rt = 1;
	if (psr.ia64_psr.cpl == 2) psr.ia64_psr.cpl = 0; // !!!! fool domain
	// psr.pk = 1;
	//printf("returns 0x%016lx...",psr.i64);
	return psr.i64;
}

/**************************************************************************
 VCPU control register access routines
**************************************************************************/

IA64FAULT vcpu_get_dcr(VCPU *vcpu, UINT64 *pval)
{
//extern unsigned long privop_trace;
//privop_trace=0;
//verbose("vcpu_get_dcr: called @%p\n",PSCB(vcpu,iip));
	// Reads of cr.dcr on Xen always have the sign bit set, so
	// a domain can differentiate whether it is running on SP or not
	*pval = PSCBX(vcpu,dcr) | 0x8000000000000000L;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_iva(VCPU *vcpu, UINT64 *pval)
{
    if(VMX_DOMAIN(vcpu)){
    	*pval = PSCB(vcpu,iva) & ~0x7fffL;
    }else{
        *pval = PSCBX(vcpu,iva) & ~0x7fffL;
    }
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_pta(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,pta);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_ipsr(VCPU *vcpu, UINT64 *pval)
{
	//REGS *regs = vcpu_regs(vcpu);
	//*pval = regs->cr_ipsr;
	*pval = PSCB(vcpu,ipsr);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_isr(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,isr);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_iip(VCPU *vcpu, UINT64 *pval)
{
	//REGS *regs = vcpu_regs(vcpu);
	//*pval = regs->cr_iip;
	*pval = PSCB(vcpu,iip);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_ifa(VCPU *vcpu, UINT64 *pval)
{
	UINT64 val = PSCB(vcpu,ifa);
	REGS *regs = vcpu_regs(vcpu);
	PRIVOP_COUNT_ADDR(regs,_GET_IFA);
	*pval = val;
	return (IA64_NO_FAULT);
}

unsigned long vcpu_get_rr_ps(VCPU *vcpu,UINT64 vadr)
{
	ia64_rr rr;

	rr.rrval = PSCB(vcpu,rrs)[vadr>>61];
	return(rr.ps);
}

unsigned long vcpu_get_rr_rid(VCPU *vcpu,UINT64 vadr)
{
	ia64_rr rr;

	rr.rrval = PSCB(vcpu,rrs)[vadr>>61];
	return(rr.rid);
}

unsigned long vcpu_get_itir_on_fault(VCPU *vcpu, UINT64 ifa)
{
	ia64_rr rr;

	rr.rrval = 0;
	rr.ps = vcpu_get_rr_ps(vcpu,ifa);
	rr.rid = vcpu_get_rr_rid(vcpu,ifa);
	return (rr.rrval);
}


IA64FAULT vcpu_get_itir(VCPU *vcpu, UINT64 *pval)
{
	UINT64 val = PSCB(vcpu,itir);
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_iipa(VCPU *vcpu, UINT64 *pval)
{
	UINT64 val = PSCB(vcpu,iipa);
	// SP entry code does not save iipa yet nor does it get
	//  properly delivered in the pscb
//	printf("*** vcpu_get_iipa: cr.iipa not fully implemented yet!!\n");
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_ifs(VCPU *vcpu, UINT64 *pval)
{
	//PSCB(vcpu,ifs) = PSCB(vcpu)->regs.cr_ifs;
	//*pval = PSCB(vcpu,regs).cr_ifs;
	*pval = PSCB(vcpu,ifs);
	PSCB(vcpu,incomplete_regframe) = 0;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_iim(VCPU *vcpu, UINT64 *pval)
{
	UINT64 val = PSCB(vcpu,iim);
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_iha(VCPU *vcpu, UINT64 *pval)
{
	//return vcpu_thash(vcpu,PSCB(vcpu,ifa),pval);
	UINT64 val = PSCB(vcpu,iha);
	REGS *regs = vcpu_regs(vcpu);
	PRIVOP_COUNT_ADDR(regs,_THASH);
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_dcr(VCPU *vcpu, UINT64 val)
{
//extern unsigned long privop_trace;
//privop_trace=1;
	// Reads of cr.dcr on SP always have the sign bit set, so
	// a domain can differentiate whether it is running on SP or not
	// Thus, writes of DCR should ignore the sign bit
//verbose("vcpu_set_dcr: called\n");
	PSCBX(vcpu,dcr) = val & ~0x8000000000000000L;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_iva(VCPU *vcpu, UINT64 val)
{
    if(VMX_DOMAIN(vcpu)){
    	PSCB(vcpu,iva) = val & ~0x7fffL;
    }else{
        PSCBX(vcpu,iva) = val & ~0x7fffL;
    }
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_pta(VCPU *vcpu, UINT64 val)
{
	if (val & IA64_PTA_LFMT) {
		printf("*** No support for VHPT long format yet!!\n");
		return (IA64_ILLOP_FAULT);
	}
	if (val & (0x3f<<9)) /* reserved fields */ return IA64_RSVDREG_FAULT;
	if (val & 2) /* reserved fields */ return IA64_RSVDREG_FAULT;
	PSCB(vcpu,pta) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_ipsr(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,ipsr) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_isr(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,isr) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_iip(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,iip) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_increment_iip(VCPU *vcpu)
{
	REGS *regs = vcpu_regs(vcpu);
	struct ia64_psr *ipsr = (struct ia64_psr *)&regs->cr_ipsr;
	if (ipsr->ri == 2) { ipsr->ri=0; regs->cr_iip += 16; }
	else ipsr->ri++;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_ifa(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,ifa) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_itir(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,itir) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_iipa(VCPU *vcpu, UINT64 val)
{
	// SP entry code does not save iipa yet nor does it get
	//  properly delivered in the pscb
//	printf("*** vcpu_set_iipa: cr.iipa not fully implemented yet!!\n");
	PSCB(vcpu,iipa) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_ifs(VCPU *vcpu, UINT64 val)
{
	//REGS *regs = vcpu_regs(vcpu);
	PSCB(vcpu,ifs) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_iim(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,iim) = val;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_set_iha(VCPU *vcpu, UINT64 val)
{
	PSCB(vcpu,iha) = val;
	return IA64_NO_FAULT;
}

/**************************************************************************
 VCPU interrupt control register access routines
**************************************************************************/

void vcpu_pend_unspecified_interrupt(VCPU *vcpu)
{
	PSCB(vcpu,pending_interruption) = 1;
}

void vcpu_pend_interrupt(VCPU *vcpu, UINT64 vector)
{
	if (vector & ~0xff) {
		printf("vcpu_pend_interrupt: bad vector\n");
		return;
	}
    if ( VMX_DOMAIN(vcpu) ) {
	    set_bit(vector,VCPU(vcpu,irr));
    } else
    {
	if (test_bit(vector,PSCBX(vcpu,irr))) {
//printf("vcpu_pend_interrupt: overrun\n");
	}
	set_bit(vector,PSCBX(vcpu,irr));
	PSCB(vcpu,pending_interruption) = 1;
    }
}

#define	IA64_TPR_MMI	0x10000
#define	IA64_TPR_MIC	0x000f0

/* checks to see if a VCPU has any unmasked pending interrupts
 * if so, returns the highest, else returns SPURIOUS_VECTOR */
/* NOTE: Since this gets called from vcpu_get_ivr() and the
 * semantics of "mov rx=cr.ivr" ignore the setting of the psr.i bit,
 * this routine also ignores pscb.interrupt_delivery_enabled
 * and this must be checked independently; see vcpu_deliverable interrupts() */
UINT64 vcpu_check_pending_interrupts(VCPU *vcpu)
{
	UINT64 *p, *r, bits, bitnum, mask, i, vector;

	/* Always check pending event, since guest may just ack the
	 * event injection without handle. Later guest may throw out
	 * the event itself.
	 */
check_start:
	if (event_pending(vcpu) && 
		!test_bit(vcpu->vcpu_info->arch.evtchn_vector,
			&PSCBX(vcpu, insvc[0])))
		vcpu_pend_interrupt(vcpu, vcpu->vcpu_info->arch.evtchn_vector);

	p = &PSCBX(vcpu,irr[3]);
	r = &PSCBX(vcpu,insvc[3]);
	for (i = 3; ; p--, r--, i--) {
		bits = *p ;
		if (bits) break; // got a potential interrupt
		if (*r) {
			// nothing in this word which is pending+inservice
			// but there is one inservice which masks lower
			return SPURIOUS_VECTOR;
		}
		if (i == 0) {
		// checked all bits... nothing pending+inservice
			return SPURIOUS_VECTOR;
		}
	}
	// have a pending,deliverable interrupt... see if it is masked
	bitnum = ia64_fls(bits);
//printf("XXXXXXX vcpu_check_pending_interrupts: got bitnum=%p...",bitnum);
	vector = bitnum+(i*64);
	mask = 1L << bitnum;
	/* sanity check for guest timer interrupt */
	if (vector == (PSCB(vcpu,itv) & 0xff)) {
		uint64_t now = ia64_get_itc();
		if (now < PSCBX(vcpu,domain_itm)) {
//			printk("Ooops, pending guest timer before its due\n");
			PSCBX(vcpu,irr[i]) &= ~mask;
			goto check_start;
		}
	}
//printf("XXXXXXX vcpu_check_pending_interrupts: got vector=%p...",vector);
	if (*r >= mask) {
		// masked by equal inservice
//printf("but masked by equal inservice\n");
		return SPURIOUS_VECTOR;
	}
	if (PSCB(vcpu,tpr) & IA64_TPR_MMI) {
		// tpr.mmi is set
//printf("but masked by tpr.mmi\n");
		return SPURIOUS_VECTOR;
	}
	if (((PSCB(vcpu,tpr) & IA64_TPR_MIC) + 15) >= vector) {
		//tpr.mic masks class
//printf("but masked by tpr.mic\n");
		return SPURIOUS_VECTOR;
	}

//printf("returned to caller\n");
	return vector;
}

UINT64 vcpu_deliverable_interrupts(VCPU *vcpu)
{
	return (vcpu_get_psr_i(vcpu) &&
		vcpu_check_pending_interrupts(vcpu) != SPURIOUS_VECTOR);
}

UINT64 vcpu_deliverable_timer(VCPU *vcpu)
{
	return (vcpu_get_psr_i(vcpu) &&
		vcpu_check_pending_interrupts(vcpu) == PSCB(vcpu,itv));
}

IA64FAULT vcpu_get_lid(VCPU *vcpu, UINT64 *pval)
{
	/* Use real LID for domain0 until vIOSAPIC is present.
	   Use EID=0, ID=vcpu_id for domU.  */
	if (vcpu->domain == dom0)
		*pval = ia64_getreg(_IA64_REG_CR_LID);
	else
		*pval = vcpu->vcpu_id << 24;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_get_ivr(VCPU *vcpu, UINT64 *pval)
{
	int i;
	UINT64 vector, mask;

#define HEARTBEAT_FREQ 16	// period in seconds
#ifdef HEARTBEAT_FREQ
#define N_DOMS 16	// period in seconds
#if 0
	static long count[N_DOMS] = { 0 };
#endif
	static long nonclockcount[N_DOMS] = { 0 };
	unsigned domid = vcpu->domain->domain_id;
#endif
#ifdef IRQ_DEBUG
	static char firstivr = 1;
	static char firsttime[256];
	if (firstivr) {
		int i;
		for (i=0;i<256;i++) firsttime[i]=1;
		firstivr=0;
	}
#endif

	vector = vcpu_check_pending_interrupts(vcpu);
	if (vector == SPURIOUS_VECTOR) {
		PSCB(vcpu,pending_interruption) = 0;
		*pval = vector;
		return IA64_NO_FAULT;
	}
#ifdef HEARTBEAT_FREQ
	if (domid >= N_DOMS) domid = N_DOMS-1;
#if 0
	if (vector == (PSCB(vcpu,itv) & 0xff)) {
	    if (!(++count[domid] & ((HEARTBEAT_FREQ*1024)-1))) {
		printf("Dom%d heartbeat... ticks=%lx,nonticks=%lx\n",
			domid, count[domid], nonclockcount[domid]);
		//count[domid] = 0;
		//dump_runq();
	    }
	}
#endif
	else nonclockcount[domid]++;
#endif
	// now have an unmasked, pending, deliverable vector!
	// getting ivr has "side effects"
#ifdef IRQ_DEBUG
	if (firsttime[vector]) {
		printf("*** First get_ivr on vector=%lu,itc=%lx\n",
			vector,ia64_get_itc());
		firsttime[vector]=0;
	}
#endif
	/* if delivering a timer interrupt, remember domain_itm, which
	 * needs to be done before clearing irr
	 */
	if (vector == (PSCB(vcpu,itv) & 0xff)) {
		PSCBX(vcpu,domain_itm_last) = PSCBX(vcpu,domain_itm);
	}

	i = vector >> 6;
	mask = 1L << (vector & 0x3f);
//printf("ZZZZZZ vcpu_get_ivr: setting insvc mask for vector %lu\n",vector);
	PSCBX(vcpu,insvc[i]) |= mask;
	PSCBX(vcpu,irr[i]) &= ~mask;
	//PSCB(vcpu,pending_interruption)--;
	*pval = vector;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_get_tpr(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,tpr);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_eoi(VCPU *vcpu, UINT64 *pval)
{
	*pval = 0L;  // reads of eoi always return 0
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_irr0(VCPU *vcpu, UINT64 *pval)
{
#ifndef IRR_USE_FIXED
	printk("vcpu_get_irr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
	*pval = vcpu->irr[0];
	return (IA64_NO_FAULT);
#endif
}

IA64FAULT vcpu_get_irr1(VCPU *vcpu, UINT64 *pval)
{
#ifndef IRR_USE_FIXED
	printk("vcpu_get_irr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
	*pval = vcpu->irr[1];
	return (IA64_NO_FAULT);
#endif
}

IA64FAULT vcpu_get_irr2(VCPU *vcpu, UINT64 *pval)
{
#ifndef IRR_USE_FIXED
	printk("vcpu_get_irr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
	*pval = vcpu->irr[2];
	return (IA64_NO_FAULT);
#endif
}

IA64FAULT vcpu_get_irr3(VCPU *vcpu, UINT64 *pval)
{
#ifndef IRR_USE_FIXED
	printk("vcpu_get_irr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
	*pval = vcpu->irr[3];
	return (IA64_NO_FAULT);
#endif
}

IA64FAULT vcpu_get_itv(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,itv);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_pmv(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,pmv);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_cmcv(VCPU *vcpu, UINT64 *pval)
{
	*pval = PSCB(vcpu,cmcv);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_lrr0(VCPU *vcpu, UINT64 *pval)
{
	// fix this when setting values other than m-bit is supported
	printf("vcpu_get_lrr0: Unmasked interrupts unsupported\n");
	*pval = (1L << 16);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_lrr1(VCPU *vcpu, UINT64 *pval)
{
	// fix this when setting values other than m-bit is supported
	printf("vcpu_get_lrr1: Unmasked interrupts unsupported\n");
	*pval = (1L << 16);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_lid(VCPU *vcpu, UINT64 val)
{
	printf("vcpu_set_lid: Setting cr.lid is unsupported\n");
	return (IA64_ILLOP_FAULT);
}

IA64FAULT vcpu_set_tpr(VCPU *vcpu, UINT64 val)
{
	if (val & 0xff00) return IA64_RSVDREG_FAULT;
	PSCB(vcpu,tpr) = val;
	/* This can unmask interrupts.  */
	if (vcpu_check_pending_interrupts(vcpu) != SPURIOUS_VECTOR)
		PSCB(vcpu,pending_interruption) = 1;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_eoi(VCPU *vcpu, UINT64 val)
{
	UINT64 *p, bits, vec, bitnum;
	int i;

	p = &PSCBX(vcpu,insvc[3]);
	for (i = 3; (i >= 0) && !(bits = *p); i--, p--);
	if (i < 0) {
		printf("Trying to EOI interrupt when none are in-service.\n");
		return IA64_NO_FAULT;
	}
	bitnum = ia64_fls(bits);
	vec = bitnum + (i*64);
	/* clear the correct bit */
	bits &= ~(1L << bitnum);
	*p = bits;
	/* clearing an eoi bit may unmask another pending interrupt... */
	if (PSCB(vcpu,interrupt_delivery_enabled)) { // but only if enabled...
		// worry about this later... Linux only calls eoi
		// with interrupts disabled
		printf("Trying to EOI interrupt with interrupts enabled\n");
	}
	if (vcpu_check_pending_interrupts(vcpu) != SPURIOUS_VECTOR)
		PSCB(vcpu,pending_interruption) = 1;
//printf("YYYYY vcpu_set_eoi: Successful\n");
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_lrr0(VCPU *vcpu, UINT64 val)
{
	if (!(val & (1L << 16))) {
		printf("vcpu_set_lrr0: Unmasked interrupts unsupported\n");
		return (IA64_ILLOP_FAULT);
	}
	// no place to save this state but nothing to do anyway
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_lrr1(VCPU *vcpu, UINT64 val)
{
	if (!(val & (1L << 16))) {
		printf("vcpu_set_lrr0: Unmasked interrupts unsupported\n");
		return (IA64_ILLOP_FAULT);
	}
	// no place to save this state but nothing to do anyway
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_itv(VCPU *vcpu, UINT64 val)
{
//extern unsigned long privop_trace;
//privop_trace=1;
	if (val & 0xef00) return (IA64_ILLOP_FAULT);
	PSCB(vcpu,itv) = val;
	if (val & 0x10000) {
		printf("**** vcpu_set_itv(%lu): vitm=%lx, setting to 0\n",
		       val,PSCBX(vcpu,domain_itm));
		PSCBX(vcpu,domain_itm) = 0;
	}
	else vcpu_set_next_timer(vcpu);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_pmv(VCPU *vcpu, UINT64 val)
{
	if (val & 0xef00) /* reserved fields */ return IA64_RSVDREG_FAULT;
	PSCB(vcpu,pmv) = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_cmcv(VCPU *vcpu, UINT64 val)
{
	if (val & 0xef00) /* reserved fields */ return IA64_RSVDREG_FAULT;
	PSCB(vcpu,cmcv) = val;
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU temporary register access routines
**************************************************************************/
UINT64 vcpu_get_tmp(VCPU *vcpu, UINT64 index)
{
	if (index > 7) return 0;
	return PSCB(vcpu,tmp[index]);
}

void vcpu_set_tmp(VCPU *vcpu, UINT64 index, UINT64 val)
{
	if (index <= 7) PSCB(vcpu,tmp[index]) = val;
}

/**************************************************************************
Interval timer routines
**************************************************************************/

BOOLEAN vcpu_timer_disabled(VCPU *vcpu)
{
	UINT64 itv = PSCB(vcpu,itv);
	return(!itv || !!(itv & 0x10000));
}

BOOLEAN vcpu_timer_inservice(VCPU *vcpu)
{
	UINT64 itv = PSCB(vcpu,itv);
	return (test_bit(itv, PSCBX(vcpu,insvc)));
}

BOOLEAN vcpu_timer_expired(VCPU *vcpu)
{
	unsigned long domain_itm = PSCBX(vcpu,domain_itm);
	unsigned long now = ia64_get_itc();

	if (!domain_itm) return FALSE;
	if (now < domain_itm) return FALSE;
	if (vcpu_timer_disabled(vcpu)) return FALSE;
	return TRUE;
}

void vcpu_safe_set_itm(unsigned long val)
{
	unsigned long epsilon = 100;
	unsigned long flags;
	UINT64 now = ia64_get_itc();

	local_irq_save(flags);
	while (1) {
//printf("*** vcpu_safe_set_itm: Setting itm to %lx, itc=%lx\n",val,now);
		ia64_set_itm(val);
		if (val > (now = ia64_get_itc())) break;
		val = now + epsilon;
		epsilon <<= 1;
	}
	local_irq_restore(flags);
}

void vcpu_set_next_timer(VCPU *vcpu)
{
	UINT64 d = PSCBX(vcpu,domain_itm);
	//UINT64 s = PSCBX(vcpu,xen_itm);
	UINT64 s = local_cpu_data->itm_next;
	UINT64 now = ia64_get_itc();

	/* gloss over the wraparound problem for now... we know it exists
	 * but it doesn't matter right now */

	if (is_idle_domain(vcpu->domain)) {
//		printf("****** vcpu_set_next_timer called during idle!!\n");
		vcpu_safe_set_itm(s);
		return;
	}
	//s = PSCBX(vcpu,xen_itm);
	if (d && (d > now) && (d < s)) {
		vcpu_safe_set_itm(d);
		//using_domain_as_itm++;
	}
	else {
		vcpu_safe_set_itm(s);
		//using_xen_as_itm++;
	}
}

IA64FAULT vcpu_set_itm(VCPU *vcpu, UINT64 val)
{
	//UINT now = ia64_get_itc();

	//if (val < now) val = now + 1000;
//printf("*** vcpu_set_itm: called with %lx\n",val);
	PSCBX(vcpu,domain_itm) = val;
	vcpu_set_next_timer(vcpu);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_itc(VCPU *vcpu, UINT64 val)
{
#define DISALLOW_SETTING_ITC_FOR_NOW
#ifdef DISALLOW_SETTING_ITC_FOR_NOW
printf("vcpu_set_itc: Setting ar.itc is currently disabled\n");
#else
	UINT64 oldnow = ia64_get_itc();
	UINT64 olditm = PSCBX(vcpu,domain_itm);
	unsigned long d = olditm - oldnow;
	unsigned long x = local_cpu_data->itm_next - oldnow;

	UINT64 newnow = val, min_delta;

	local_irq_disable();
	if (olditm) {
printf("**** vcpu_set_itc(%lx): vitm changed to %lx\n",val,newnow+d);
		PSCBX(vcpu,domain_itm) = newnow + d;
	}
	local_cpu_data->itm_next = newnow + x;
	d = PSCBX(vcpu,domain_itm);
	x = local_cpu_data->itm_next;

	ia64_set_itc(newnow);
	if (d && (d > newnow) && (d < x)) {
		vcpu_safe_set_itm(d);
		//using_domain_as_itm++;
	}
	else {
		vcpu_safe_set_itm(x);
		//using_xen_as_itm++;
	}
	local_irq_enable();
#endif
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_itm(VCPU *vcpu, UINT64 *pval)
{
	//FIXME: Implement this
	printf("vcpu_get_itm: Getting cr.itm is unsupported... continuing\n");
	return (IA64_NO_FAULT);
	//return (IA64_ILLOP_FAULT);
}

IA64FAULT vcpu_get_itc(VCPU *vcpu, UINT64 *pval)
{
	//TODO: Implement this
	printf("vcpu_get_itc: Getting ar.itc is unsupported\n");
	return (IA64_ILLOP_FAULT);
}

void vcpu_pend_timer(VCPU *vcpu)
{
	UINT64 itv = PSCB(vcpu,itv) & 0xff;

	if (vcpu_timer_disabled(vcpu)) return;
	//if (vcpu_timer_inservice(vcpu)) return;
	if (PSCBX(vcpu,domain_itm_last) == PSCBX(vcpu,domain_itm)) {
		// already delivered an interrupt for this so
		// don't deliver another
		return;
	}
	vcpu_pend_interrupt(vcpu, itv);
}

// returns true if ready to deliver a timer interrupt too early
UINT64 vcpu_timer_pending_early(VCPU *vcpu)
{
	UINT64 now = ia64_get_itc();
	UINT64 itm = PSCBX(vcpu,domain_itm);

	if (vcpu_timer_disabled(vcpu)) return 0;
	if (!itm) return 0;
	return (vcpu_deliverable_timer(vcpu) && (now < itm));
}

/**************************************************************************
Privileged operation emulation routines
**************************************************************************/

IA64FAULT vcpu_force_data_miss(VCPU *vcpu, UINT64 ifa)
{
	PSCB(vcpu,ifa) = ifa;
	PSCB(vcpu,itir) = vcpu_get_itir_on_fault(vcpu,ifa);
	vcpu_thash(current, ifa, &PSCB(current,iha));
	return (vcpu_get_rr_ve(vcpu,ifa) ? IA64_DATA_TLB_VECTOR : IA64_ALT_DATA_TLB_VECTOR);
}


IA64FAULT vcpu_rfi(VCPU *vcpu)
{
	// TODO: Only allowed for current vcpu
	PSR psr;
	UINT64 int_enable, regspsr = 0;
	UINT64 ifs;
	REGS *regs = vcpu_regs(vcpu);
	extern void dorfirfi(void);

	psr.i64 = PSCB(vcpu,ipsr);
	if (psr.ia64_psr.cpl < 3) psr.ia64_psr.cpl = 2;
	if (psr.ia64_psr.i) PSCB(vcpu,interrupt_delivery_enabled) = 1;
	int_enable = psr.ia64_psr.i;
	if (psr.ia64_psr.ic)  PSCB(vcpu,interrupt_collection_enabled) = 1;
	if (psr.ia64_psr.dt && psr.ia64_psr.rt && psr.ia64_psr.it) vcpu_set_metaphysical_mode(vcpu,FALSE);
	else vcpu_set_metaphysical_mode(vcpu,TRUE);
	psr.ia64_psr.ic = 1; psr.ia64_psr.i = 1;
	psr.ia64_psr.dt = 1; psr.ia64_psr.rt = 1; psr.ia64_psr.it = 1;
	psr.ia64_psr.bn = 1;
	//psr.pk = 1;  // checking pkeys shouldn't be a problem but seems broken
	if (psr.ia64_psr.be) {
		printf("*** DOMAIN TRYING TO TURN ON BIG-ENDIAN!!!\n");
		return (IA64_ILLOP_FAULT);
	}
	PSCB(vcpu,incomplete_regframe) = 0; // is this necessary?
	ifs = PSCB(vcpu,ifs);
	//if ((ifs & regs->cr_ifs & 0x8000000000000000L) && ifs != regs->cr_ifs) {
	//if ((ifs & 0x8000000000000000L) && ifs != regs->cr_ifs) {
	if (ifs & regs->cr_ifs & 0x8000000000000000L) {
		// TODO: validate PSCB(vcpu,iip)
		// TODO: PSCB(vcpu,ipsr) = psr;
		PSCB(vcpu,ipsr) = psr.i64;
		// now set up the trampoline
		regs->cr_iip = *(unsigned long *)dorfirfi; // function pointer!!
		__asm__ __volatile ("mov %0=psr;;":"=r"(regspsr)::"memory");
		regs->cr_ipsr = regspsr & ~(IA64_PSR_I | IA64_PSR_IC | IA64_PSR_BN);
	}
	else {
		regs->cr_ipsr = psr.i64;
		regs->cr_iip = PSCB(vcpu,iip);
	}
	PSCB(vcpu,interrupt_collection_enabled) = 1;
	vcpu_bsw1(vcpu);
	PSCB(vcpu,interrupt_delivery_enabled) = int_enable;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_cover(VCPU *vcpu)
{
	// TODO: Only allowed for current vcpu
	REGS *regs = vcpu_regs(vcpu);

	if (!PSCB(vcpu,interrupt_collection_enabled)) {
		if (!PSCB(vcpu,incomplete_regframe))
			PSCB(vcpu,ifs) = regs->cr_ifs;
		else PSCB(vcpu,incomplete_regframe) = 0;
	}
	regs->cr_ifs = 0;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_thash(VCPU *vcpu, UINT64 vadr, UINT64 *pval)
{
	UINT64 pta = PSCB(vcpu,pta);
	UINT64 pta_sz = (pta & IA64_PTA_SZ(0x3f)) >> IA64_PTA_SZ_BIT;
	UINT64 pta_base = pta & ~((1UL << IA64_PTA_BASE_BIT)-1);
	UINT64 Mask = (1L << pta_sz) - 1;
	UINT64 Mask_60_15 = (Mask >> 15) & 0x3fffffffffff;
	UINT64 compMask_60_15 = ~Mask_60_15;
	UINT64 rr_ps = vcpu_get_rr_ps(vcpu,vadr);
	UINT64 VHPT_offset = (vadr >> rr_ps) << 3;
	UINT64 VHPT_addr1 = vadr & 0xe000000000000000L;
	UINT64 VHPT_addr2a =
		((pta_base >> 15) & 0x3fffffffffff) & compMask_60_15;
	UINT64 VHPT_addr2b =
		((VHPT_offset >> 15) & 0x3fffffffffff) & Mask_60_15;
	UINT64 VHPT_addr3 = VHPT_offset & 0x7fff;
	UINT64 VHPT_addr = VHPT_addr1 | ((VHPT_addr2a | VHPT_addr2b) << 15) |
			VHPT_addr3;

//verbose("vcpu_thash: vadr=%p, VHPT_addr=%p\n",vadr,VHPT_addr);
	*pval = VHPT_addr;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_ttag(VCPU *vcpu, UINT64 vadr, UINT64 *padr)
{
	printf("vcpu_ttag: ttag instruction unsupported\n");
	return (IA64_ILLOP_FAULT);
}

unsigned long vhpt_translate_count = 0;
unsigned long fast_vhpt_translate_count = 0;
unsigned long recover_to_page_fault_count = 0;
unsigned long recover_to_break_fault_count = 0;

int warn_region0_address = 0; // FIXME later: tie to a boot parameter?

IA64FAULT vcpu_translate(VCPU *vcpu, UINT64 address, BOOLEAN is_data, BOOLEAN in_tpa, UINT64 *pteval, UINT64 *itir, UINT64 *iha)
{
	unsigned long region = address >> 61;
	unsigned long pta, pte, rid, rr;
	int i;
	TR_ENTRY *trp;

	if (PSCB(vcpu,metaphysical_mode) && !(!is_data && region)) {
		// dom0 may generate an uncacheable physical address (msb=1)
		if (region && ((region != 4) || (vcpu->domain != dom0))) {
// FIXME: This seems to happen even though it shouldn't.  Need to track
// this down, but since it has been apparently harmless, just flag it for now
//			panic_domain(vcpu_regs(vcpu),

			/*
			 * Guest may execute itc.d and rfi with psr.dt=0
			 * When VMM try to fetch opcode, tlb miss may happen,
			 * At this time PSCB(vcpu,metaphysical_mode)=1,
			 * region=5,VMM need to handle this tlb miss as if
			 * PSCB(vcpu,metaphysical_mode)=0
			 */           
			printk("vcpu_translate: bad physical address: 0x%lx\n",
			       address);
		} else {
			*pteval = (address & _PAGE_PPN_MASK) | __DIRTY_BITS |
			          _PAGE_PL_2 | _PAGE_AR_RWX;
			*itir = PAGE_SHIFT << 2;
			phys_translate_count++;
			return IA64_NO_FAULT;
		}
	}
	else if (!region && warn_region0_address) {
		REGS *regs = vcpu_regs(vcpu);
		unsigned long viip = PSCB(vcpu,iip);
		unsigned long vipsr = PSCB(vcpu,ipsr);
		unsigned long iip = regs->cr_iip;
		unsigned long ipsr = regs->cr_ipsr;
		printk("vcpu_translate: bad address 0x%lx, viip=0x%lx, vipsr=0x%lx, iip=0x%lx, ipsr=0x%lx continuing\n",
			address, viip, vipsr, iip, ipsr);
	}

	rr = PSCB(vcpu,rrs)[region];
	rid = rr & RR_RID_MASK;
	if (is_data) {
		if (vcpu_quick_region_check(vcpu->arch.dtr_regions,address)) {
			for (trp = vcpu->arch.dtrs, i = NDTRS; i; i--, trp++) {
				if (vcpu_match_tr_entry(trp,address,rid)) {
					*pteval = trp->page_flags;
					*itir = trp->itir;
					tr_translate_count++;
					return IA64_NO_FAULT;
				}
			}
		}
	}
	// FIXME?: check itr's for data accesses too, else bad things happen?
	/* else */ {
		if (vcpu_quick_region_check(vcpu->arch.itr_regions,address)) {
			for (trp = vcpu->arch.itrs, i = NITRS; i; i--, trp++) {
				if (vcpu_match_tr_entry(trp,address,rid)) {
					*pteval = trp->page_flags;
					*itir = trp->itir;
					tr_translate_count++;
					return IA64_NO_FAULT;
				}
			}
		}
	}

	/* check 1-entry TLB */
	// FIXME?: check dtlb for inst accesses too, else bad things happen?
	trp = &vcpu->arch.dtlb;
	if (/* is_data && */ vcpu_match_tr_entry(trp,address,rid)) {
		if (vcpu->domain==dom0 && !in_tpa) *pteval = trp->page_flags;
		else *pteval = vcpu->arch.dtlb_pte;
		*itir = trp->itir;
		dtlb_translate_count++;
		return IA64_NO_FAULT;
	}

	/* check guest VHPT */
	pta = PSCB(vcpu,pta);
	if (pta & IA64_PTA_VF) { /* long format VHPT - not implemented */
		panic_domain(vcpu_regs(vcpu),"can't do long format VHPT\n");
		//return (is_data ? IA64_DATA_TLB_VECTOR:IA64_INST_TLB_VECTOR);
	}

	*itir = rr & (RR_RID_MASK | RR_PS_MASK);
	// note: architecturally, iha is optionally set for alt faults but
	// xenlinux depends on it so should document it as part of PV interface
	vcpu_thash(vcpu, address, iha);
	if (!(rr & RR_VE_MASK) || !(pta & IA64_PTA_VE))
		return (is_data ? IA64_ALT_DATA_TLB_VECTOR : IA64_ALT_INST_TLB_VECTOR);

	/* avoid recursively walking (short format) VHPT */
	if (((address ^ pta) & ((itir_mask(pta) << 3) >> 3)) == 0)
		return (is_data ? IA64_DATA_TLB_VECTOR : IA64_INST_TLB_VECTOR);

	if (__copy_from_user(&pte, (void *)(*iha), sizeof(pte)) != 0)
		// virtual VHPT walker "missed" in TLB
		return IA64_VHPT_FAULT;

	/*
	* Optimisation: this VHPT walker aborts on not-present pages
	* instead of inserting a not-present translation, this allows
	* vectoring directly to the miss handler.
	*/
	if (!(pte & _PAGE_P))
		return (is_data ? IA64_DATA_TLB_VECTOR : IA64_INST_TLB_VECTOR);

	/* found mapping in guest VHPT! */
	*itir = rr & RR_PS_MASK;
	*pteval = pte;
	vhpt_translate_count++;
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_tpa(VCPU *vcpu, UINT64 vadr, UINT64 *padr)
{
	UINT64 pteval, itir, mask, iha;
	IA64FAULT fault;

	fault = vcpu_translate(vcpu, vadr, TRUE, TRUE, &pteval, &itir, &iha);
	if (fault == IA64_NO_FAULT)
	{
		mask = itir_mask(itir);
		*padr = (pteval & _PAGE_PPN_MASK & mask) | (vadr & ~mask);
		return (IA64_NO_FAULT);
	}
	return vcpu_force_data_miss(vcpu,vadr);
}

IA64FAULT vcpu_tak(VCPU *vcpu, UINT64 vadr, UINT64 *key)
{
	printf("vcpu_tak: tak instruction unsupported\n");
	return (IA64_ILLOP_FAULT);
	// HACK ALERT: tak does a thash for now
	//return vcpu_thash(vcpu,vadr,key);
}

/**************************************************************************
 VCPU debug breakpoint register access routines
**************************************************************************/

IA64FAULT vcpu_set_dbr(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	// TODO: unimplemented DBRs return a reserved register fault
	// TODO: Should set Logical CPU state, not just physical
	ia64_set_dbr(reg,val);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_ibr(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	// TODO: unimplemented IBRs return a reserved register fault
	// TODO: Should set Logical CPU state, not just physical
	ia64_set_ibr(reg,val);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_dbr(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	// TODO: unimplemented DBRs return a reserved register fault
	UINT64 val = ia64_get_dbr(reg);
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_ibr(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	// TODO: unimplemented IBRs return a reserved register fault
	UINT64 val = ia64_get_ibr(reg);
	*pval = val;
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU performance monitor register access routines
**************************************************************************/

IA64FAULT vcpu_set_pmc(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	// TODO: Should set Logical CPU state, not just physical
	// NOTE: Writes to unimplemented PMC registers are discarded
#ifdef DEBUG_PFMON
printf("vcpu_set_pmc(%x,%lx)\n",reg,val);
#endif
	ia64_set_pmc(reg,val);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_set_pmd(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	// TODO: Should set Logical CPU state, not just physical
	// NOTE: Writes to unimplemented PMD registers are discarded
#ifdef DEBUG_PFMON
printf("vcpu_set_pmd(%x,%lx)\n",reg,val);
#endif
	ia64_set_pmd(reg,val);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_pmc(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	// NOTE: Reads from unimplemented PMC registers return zero
	UINT64 val = (UINT64)ia64_get_pmc(reg);
#ifdef DEBUG_PFMON
printf("%lx=vcpu_get_pmc(%x)\n",val,reg);
#endif
	*pval = val;
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_pmd(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	// NOTE: Reads from unimplemented PMD registers return zero
	UINT64 val = (UINT64)ia64_get_pmd(reg);
#ifdef DEBUG_PFMON
printf("%lx=vcpu_get_pmd(%x)\n",val,reg);
#endif
	*pval = val;
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU banked general register access routines
**************************************************************************/
#define vcpu_bsw0_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT)     \
do{     \
    __asm__ __volatile__ (                      \
        ";;extr.u %0 = %3,%6,16;;\n"            \
        "dep %1 = %0, %1, 0, 16;;\n"            \
        "st8 [%4] = %1\n"                       \
        "extr.u %0 = %2, 16, 16;;\n"            \
        "dep %3 = %0, %3, %6, 16;;\n"           \
        "st8 [%5] = %3\n"                       \
        ::"r"(i),"r"(*b1unat),"r"(*b0unat),"r"(*runat),"r"(b1unat), \
        "r"(runat),"i"(IA64_PT_REGS_R16_SLOT):"memory");    \
}while(0)

IA64FAULT vcpu_bsw0(VCPU *vcpu)
{
	// TODO: Only allowed for current vcpu
	REGS *regs = vcpu_regs(vcpu);
	unsigned long *r = &regs->r16;
	unsigned long *b0 = &PSCB(vcpu,bank0_regs[0]);
	unsigned long *b1 = &PSCB(vcpu,bank1_regs[0]);
	unsigned long *runat = &regs->eml_unat;
	unsigned long *b0unat = &PSCB(vcpu,vbnat);
	unsigned long *b1unat = &PSCB(vcpu,vnat);

	unsigned long i;

    if(VMX_DOMAIN(vcpu)){
        if(VCPU(vcpu,vpsr)&IA64_PSR_BN){
            for (i = 0; i < 16; i++) { *b1++ = *r; *r++ = *b0++; }
            vcpu_bsw0_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT);
            VCPU(vcpu,vpsr) &= ~IA64_PSR_BN;
        }
    }else{
        if (PSCB(vcpu,banknum)) {
            for (i = 0; i < 16; i++) { *b1++ = *r; *r++ = *b0++; }
            vcpu_bsw0_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT);
            PSCB(vcpu,banknum) = 0;
        }
    }
	return (IA64_NO_FAULT);
}

#define vcpu_bsw1_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT)     \
do{             \
    __asm__ __volatile__ (      \
        ";;extr.u %0 = %3,%6,16;;\n"                \
        "dep %1 = %0, %1, 16, 16;;\n"               \
        "st8 [%4] = %1\n"                           \
        "extr.u %0 = %2, 0, 16;;\n"                 \
        "dep %3 = %0, %3, %6, 16;;\n"               \
        "st8 [%5] = %3\n"                           \
        ::"r"(i),"r"(*b0unat),"r"(*b1unat),"r"(*runat),"r"(b0unat), \
        "r"(runat),"i"(IA64_PT_REGS_R16_SLOT):"memory");            \
}while(0)

IA64FAULT vcpu_bsw1(VCPU *vcpu)
{
	// TODO: Only allowed for current vcpu
	REGS *regs = vcpu_regs(vcpu);
	unsigned long *r = &regs->r16;
	unsigned long *b0 = &PSCB(vcpu,bank0_regs[0]);
	unsigned long *b1 = &PSCB(vcpu,bank1_regs[0]);
	unsigned long *runat = &regs->eml_unat;
	unsigned long *b0unat = &PSCB(vcpu,vbnat);
	unsigned long *b1unat = &PSCB(vcpu,vnat);

	unsigned long i;

    if(VMX_DOMAIN(vcpu)){
        if(!(VCPU(vcpu,vpsr)&IA64_PSR_BN)){
            for (i = 0; i < 16; i++) { *b0++ = *r; *r++ = *b1++; }
            vcpu_bsw1_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT);
            VCPU(vcpu,vpsr) |= IA64_PSR_BN;
        }
    }else{
        if (!PSCB(vcpu,banknum)) {
            for (i = 0; i < 16; i++) { *b0++ = *r; *r++ = *b1++; }
            vcpu_bsw1_unat(i,b0unat,b1unat,runat,IA64_PT_REGS_R16_SLOT);
            PSCB(vcpu,banknum) = 1;
        }
    }
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU cpuid access routines
**************************************************************************/


IA64FAULT vcpu_get_cpuid(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	// FIXME: This could get called as a result of a rsvd-reg fault
	// if reg > 3
	switch(reg) {
	    case 0:
		memcpy(pval,"Xen/ia64",8);
		break;
	    case 1:
		*pval = 0;
		break;
	    case 2:
		*pval = 0;
		break;
	    case 3:
		*pval = ia64_get_cpuid(3);
		break;
	    case 4:
		*pval = ia64_get_cpuid(4);
		break;
	    default:
		if (reg > (ia64_get_cpuid(3) & 0xff))
			return IA64_RSVDREG_FAULT;
		*pval = ia64_get_cpuid(reg);
		break;
	}
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU region register access routines
**************************************************************************/

unsigned long vcpu_get_rr_ve(VCPU *vcpu,UINT64 vadr)
{
	ia64_rr rr;

	rr.rrval = PSCB(vcpu,rrs)[vadr>>61];
	return(rr.ve);
}

IA64FAULT vcpu_set_rr(VCPU *vcpu, UINT64 reg, UINT64 val)
{
	PSCB(vcpu,rrs)[reg>>61] = val;
	// warning: set_one_rr() does it "live"
	set_one_rr(reg,val);
	return (IA64_NO_FAULT);
}

IA64FAULT vcpu_get_rr(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
	if(VMX_DOMAIN(vcpu)){
		*pval = VMX(vcpu,vrr[reg>>61]);
	}else{
		*pval = PSCB(vcpu,rrs)[reg>>61];
	}
	return (IA64_NO_FAULT);
}

/**************************************************************************
 VCPU protection key register access routines
**************************************************************************/

IA64FAULT vcpu_get_pkr(VCPU *vcpu, UINT64 reg, UINT64 *pval)
{
#ifndef PKR_USE_FIXED
	printk("vcpu_get_pkr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
	UINT64 val = (UINT64)ia64_get_pkr(reg);
	*pval = val;
	return (IA64_NO_FAULT);
#endif
}

IA64FAULT vcpu_set_pkr(VCPU *vcpu, UINT64 reg, UINT64 val)
{
#ifndef PKR_USE_FIXED
	printk("vcpu_set_pkr: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
#else
//	if (reg >= NPKRS) return (IA64_ILLOP_FAULT);
	vcpu->pkrs[reg] = val;
	ia64_set_pkr(reg,val);
	return (IA64_NO_FAULT);
#endif
}

/**************************************************************************
 VCPU translation register access routines
**************************************************************************/

static inline void vcpu_purge_tr_entry(TR_ENTRY *trp)
{
	trp->p = 0;
}

static void vcpu_set_tr_entry(TR_ENTRY *trp, UINT64 pte, UINT64 itir, UINT64 ifa)
{
	UINT64 ps;

	trp->itir = itir;
	trp->rid = VCPU(current,rrs[ifa>>61]) & RR_RID_MASK;
	trp->p = 1;
	ps = trp->ps;
	trp->page_flags = pte;
	if (trp->pl < 2) trp->pl = 2;
	trp->vadr = ifa & ~0xfff;
	if (ps > 12) { // "ignore" relevant low-order bits
		trp->ppn &= ~((1UL<<(ps-12))-1);
		trp->vadr &= ~((1UL<<ps)-1);
	}
}

IA64FAULT vcpu_itr_d(VCPU *vcpu, UINT64 slot, UINT64 pte,
		UINT64 itir, UINT64 ifa)
{
	TR_ENTRY *trp;

	if (slot >= NDTRS) return IA64_RSVDREG_FAULT;
	trp = &PSCBX(vcpu,dtrs[slot]);
//printf("***** itr.d: setting slot %d: ifa=%p\n",slot,ifa);
	vcpu_set_tr_entry(trp,pte,itir,ifa);
	vcpu_quick_region_set(PSCBX(vcpu,dtr_regions),ifa);
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_itr_i(VCPU *vcpu, UINT64 slot, UINT64 pte,
		UINT64 itir, UINT64 ifa)
{
	TR_ENTRY *trp;

	if (slot >= NITRS) return IA64_RSVDREG_FAULT;
	trp = &PSCBX(vcpu,itrs[slot]);
//printf("***** itr.i: setting slot %d: ifa=%p\n",slot,ifa);
	vcpu_set_tr_entry(trp,pte,itir,ifa);
	vcpu_quick_region_set(PSCBX(vcpu,itr_regions),ifa);
	return IA64_NO_FAULT;
}

/**************************************************************************
 VCPU translation cache access routines
**************************************************************************/

void foobar(void) { /*vcpu_verbose = 1;*/ }

void vcpu_itc_no_srlz(VCPU *vcpu, UINT64 IorD, UINT64 vaddr, UINT64 pte, UINT64 mp_pte, UINT64 logps)
{
	unsigned long psr;
	unsigned long ps = (vcpu->domain==dom0) ? logps : PAGE_SHIFT;

	// FIXME: validate ifa here (not in Xen space), COULD MACHINE CHECK!
	// FIXME, must be inlined or potential for nested fault here!
	if ((vcpu->domain==dom0) && (logps < PAGE_SHIFT)) {
		printf("vcpu_itc_no_srlz: domain0 use of smaller page size!\n");
		//FIXME: kill domain here
		while(1);
	}
	psr = ia64_clear_ic();
	ia64_itc(IorD,vaddr,pte,ps); // FIXME: look for bigger mappings
	ia64_set_psr(psr);
	// ia64_srlz_i(); // no srls req'd, will rfi later
#ifdef VHPT_GLOBAL
	if (vcpu->domain==dom0 && ((vaddr >> 61) == 7)) {
		// FIXME: this is dangerous... vhpt_flush_address ensures these
		// addresses never get flushed.  More work needed if this
		// ever happens.
//printf("vhpt_insert(%p,%p,%p)\n",vaddr,pte,1L<<logps);
		if (logps > PAGE_SHIFT) vhpt_multiple_insert(vaddr,pte,logps);
		else vhpt_insert(vaddr,pte,logps<<2);
	}
	// even if domain pagesize is larger than PAGE_SIZE, just put
	// PAGE_SIZE mapping in the vhpt for now, else purging is complicated
	else vhpt_insert(vaddr,pte,PAGE_SHIFT<<2);
#endif
	if ((mp_pte == -1UL) || (IorD & 0x4)) // don't place in 1-entry TLB
		return;
	if (IorD & 0x1) {
		vcpu_set_tr_entry(&PSCBX(vcpu,itlb),pte,ps<<2,vaddr);
		PSCBX(vcpu,itlb_pte) = mp_pte;
	}
	if (IorD & 0x2) {
		vcpu_set_tr_entry(&PSCBX(vcpu,dtlb),pte,ps<<2,vaddr);
		PSCBX(vcpu,dtlb_pte) = mp_pte;
	}
}

IA64FAULT vcpu_itc_d(VCPU *vcpu, UINT64 pte, UINT64 itir, UINT64 ifa)
{
	unsigned long pteval, logps = itir_ps(itir);
	BOOLEAN swap_rr0 = (!(ifa>>61) && PSCB(vcpu,metaphysical_mode));

	if (logps < PAGE_SHIFT) {
		printf("vcpu_itc_d: domain trying to use smaller page size!\n");
		//FIXME: kill domain here
		while(1);
	}
	//itir = (itir & ~0xfc) | (PAGE_SHIFT<<2); // ignore domain's pagesize
	pteval = translate_domain_pte(pte,ifa,itir);
	if (!pteval) return IA64_ILLOP_FAULT;
	if (swap_rr0) set_one_rr(0x0,PSCB(vcpu,rrs[0]));
	vcpu_itc_no_srlz(vcpu,2,ifa,pteval,pte,logps);
	if (swap_rr0) set_metaphysical_rr0();
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_itc_i(VCPU *vcpu, UINT64 pte, UINT64 itir, UINT64 ifa)
{
	unsigned long pteval, logps = itir_ps(itir);
	BOOLEAN swap_rr0 = (!(ifa>>61) && PSCB(vcpu,metaphysical_mode));

	// FIXME: validate ifa here (not in Xen space), COULD MACHINE CHECK!
	if (logps < PAGE_SHIFT) {
		printf("vcpu_itc_i: domain trying to use smaller page size!\n");
		//FIXME: kill domain here
		while(1);
	}
	//itir = (itir & ~0xfc) | (PAGE_SHIFT<<2); // ignore domain's pagesize
	pteval = translate_domain_pte(pte,ifa,itir);
	// FIXME: what to do if bad physical address? (machine check?)
	if (!pteval) return IA64_ILLOP_FAULT;
	if (swap_rr0) set_one_rr(0x0,PSCB(vcpu,rrs[0]));
	vcpu_itc_no_srlz(vcpu, 1,ifa,pteval,pte,logps);
	if (swap_rr0) set_metaphysical_rr0();
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_ptc_l(VCPU *vcpu, UINT64 vadr, UINT64 addr_range)
{
	printk("vcpu_ptc_l: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
}

// At privlvl=0, fc performs no access rights or protection key checks, while
// at privlvl!=0, fc performs access rights checks as if it were a 1-byte
// read but no protection key check.  Thus in order to avoid an unexpected
// access rights fault, we have to translate the virtual address to a
// physical address (possibly via a metaphysical address) and do the fc
// on the physical address, which is guaranteed to flush the same cache line
IA64FAULT vcpu_fc(VCPU *vcpu, UINT64 vadr)
{
	// TODO: Only allowed for current vcpu
	UINT64 mpaddr, paddr;
	IA64FAULT fault;

	fault = vcpu_tpa(vcpu, vadr, &mpaddr);
	if (fault == IA64_NO_FAULT) {
		paddr = translate_domain_mpaddr(mpaddr);
		ia64_fc(__va(paddr));
	}
	return fault;
}

int ptce_count = 0;
IA64FAULT vcpu_ptc_e(VCPU *vcpu, UINT64 vadr)
{
	// Note that this only needs to be called once, i.e. the
	// architected loop to purge the entire TLB, should use
	//  base = stride1 = stride2 = 0, count0 = count 1 = 1

#ifdef VHPT_GLOBAL
	vhpt_flush();	// FIXME: This is overdoing it
#endif
	local_flush_tlb_all();
	// just invalidate the "whole" tlb
	vcpu_purge_tr_entry(&PSCBX(vcpu,dtlb));
	vcpu_purge_tr_entry(&PSCBX(vcpu,itlb));
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_ptc_g(VCPU *vcpu, UINT64 vadr, UINT64 addr_range)
{
	printk("vcpu_ptc_g: called, not implemented yet\n");
	return IA64_ILLOP_FAULT;
}

#if defined(CONFIG_XEN_SMP) && defined(VHPT_GLOBAL)
struct ptc_ga_args {
	unsigned long vadr;
	unsigned long addr_range;
};

static void ptc_ga_remote_func (void *varg)
{
	struct ptc_ga_args *args = (struct ptc_ga_args *)varg;
	vhpt_flush_address (args->vadr, args->addr_range);
}
#endif

IA64FAULT vcpu_ptc_ga(VCPU *vcpu,UINT64 vadr,UINT64 addr_range)
{
	// FIXME: validate not flushing Xen addresses
	// if (Xen address) return(IA64_ILLOP_FAULT);
	// FIXME: ??breaks if domain PAGE_SIZE < Xen PAGE_SIZE
//printf("######## vcpu_ptc_ga(%p,%p) ##############\n",vadr,addr_range);

#ifdef CONFIG_XEN_SMP
	struct domain *d = vcpu->domain;
	struct vcpu *v;
	struct ptc_ga_args args;

	args.vadr = vadr;
	args.addr_range = addr_range;

	/* This method is very conservative and should be optimized:
	   - maybe IPI calls can be avoided,
	   - a processor map can be built to avoid duplicate purge
	   - maybe ptc.ga can be replaced by ptc.l+invala.
	   Hopefully, it has no impact when UP.
	*/
	for_each_vcpu (d, v) {
		if (v != vcpu) {
			/* Purge tc entry.
			   Can we do this directly ?  Well, this is just a
			   single atomic write.  */
			vcpu_purge_tr_entry(&PSCBX(v,dtlb));
			vcpu_purge_tr_entry(&PSCBX(v,itlb));
#ifdef VHPT_GLOBAL
			/* Flush VHPT on remote processors.
			   FIXME: invalidate directly the entries? */
			smp_call_function_single
				(v->processor, &ptc_ga_remote_func,
				 &args, 0, 1);
#endif
		}
	}
#endif

#ifdef VHPT_GLOBAL
	vhpt_flush_address(vadr,addr_range);
#endif
	ia64_global_tlb_purge(vadr,vadr+addr_range,PAGE_SHIFT);
	/* Purge tc.  */
	vcpu_purge_tr_entry(&PSCBX(vcpu,dtlb));
	vcpu_purge_tr_entry(&PSCBX(vcpu,itlb));
	return IA64_NO_FAULT;
}

IA64FAULT vcpu_ptr_d(VCPU *vcpu,UINT64 vadr,UINT64 addr_range)
{
	printf("vcpu_ptr_d: Purging TLB is unsupported\n");
	// don't forget to recompute dtr_regions
	return (IA64_ILLOP_FAULT);
}

IA64FAULT vcpu_ptr_i(VCPU *vcpu,UINT64 vadr,UINT64 addr_range)
{
	printf("vcpu_ptr_i: Purging TLB is unsupported\n");
	// don't forget to recompute itr_regions
	return (IA64_ILLOP_FAULT);
}
