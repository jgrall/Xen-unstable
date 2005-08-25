/******************************************************************************
 * arch-ia64/hypervisor-if.h
 * 
 * Guest OS interface to IA64 Xen.
 */

#ifndef __HYPERVISOR_IF_IA64_H__
#define __HYPERVISOR_IF_IA64_H__

/* Maximum number of virtual CPUs in multi-processor guests. */
/* WARNING: before changing this, check that shared_info fits on a page */
#define MAX_VIRT_CPUS 1

#ifndef __ASSEMBLY__

#define MAX_NR_SECTION  32  // at most 32 memory holes
typedef struct {
    unsigned long	start; 	/* start of memory hole */
    unsigned long	end;	/* end of memory hole */
} mm_section_t;

typedef struct {
    unsigned long	mfn : 56;
    unsigned long	type: 8;
} pmt_entry_t;

#define GPFN_MEM		(0UL << 56)	/* Guest pfn is normal mem */
#define GPFN_FRAME_BUFFER	(1UL << 56)	/* VGA framebuffer */
#define GPFN_LOW_MMIO		(2UL << 56)	/* Low MMIO range */
#define GPFN_PIB		(3UL << 56)	/* PIB base */
#define GPFN_IOSAPIC		(4UL << 56)	/* IOSAPIC base */
#define GPFN_LEGACY_IO		(5UL << 56)	/* Legacy I/O base */
#define GPFN_GFW		(6UL << 56)	/* Guest Firmware */
#define GPFN_HIGH_MMIO		(7UL << 56)	/* High MMIO range */

#define GPFN_IO_MASK		(7UL << 56)	/* Guest pfn is I/O type */
#define GPFN_INV_MASK		(31UL << 59)	/* Guest pfn is invalid */

#define INVALID_MFN              (~0UL)

/*
 * NB. This may become a 64-bit count with no shift. If this happens then the 
 * structure size will still be 8 bytes, so no other alignments will change.
 */
typedef struct {
    unsigned int  tsc_bits;      /* 0: 32 bits read from the CPU's TSC. */
    unsigned int  tsc_bitshift;  /* 4: 'tsc_bits' uses N:N+31 of TSC.   */
} tsc_timestamp_t; /* 8 bytes */

struct pt_fpreg {
        union {
                unsigned long bits[2];
                long double __dummy;    /* force 16-byte alignment */
        } u;
};

typedef struct cpu_user_regs{
	/* The following registers are saved by SAVE_MIN: */
	unsigned long b6;		/* scratch */
	unsigned long b7;		/* scratch */

	unsigned long ar_csd;           /* used by cmp8xchg16 (scratch) */
	unsigned long ar_ssd;           /* reserved for future use (scratch) */

	unsigned long r8;		/* scratch (return value register 0) */
	unsigned long r9;		/* scratch (return value register 1) */
	unsigned long r10;		/* scratch (return value register 2) */
	unsigned long r11;		/* scratch (return value register 3) */

	unsigned long cr_ipsr;		/* interrupted task's psr */
	unsigned long cr_iip;		/* interrupted task's instruction pointer */
	unsigned long cr_ifs;		/* interrupted task's function state */

	unsigned long ar_unat;		/* interrupted task's NaT register (preserved) */
	unsigned long ar_pfs;		/* prev function state  */
	unsigned long ar_rsc;		/* RSE configuration */
	/* The following two are valid only if cr_ipsr.cpl > 0: */
	unsigned long ar_rnat;		/* RSE NaT */
	unsigned long ar_bspstore;	/* RSE bspstore */

	unsigned long pr;		/* 64 predicate registers (1 bit each) */
	unsigned long b0;		/* return pointer (bp) */
	unsigned long loadrs;		/* size of dirty partition << 16 */

	unsigned long r1;		/* the gp pointer */
	unsigned long r12;		/* interrupted task's memory stack pointer */
	unsigned long r13;		/* thread pointer */

	unsigned long ar_fpsr;		/* floating point status (preserved) */
	unsigned long r15;		/* scratch */

	/* The remaining registers are NOT saved for system calls.  */

	unsigned long r14;		/* scratch */
	unsigned long r2;		/* scratch */
	unsigned long r3;		/* scratch */

#ifdef CONFIG_VTI
	unsigned long r4;		/* preserved */
	unsigned long r5;		/* preserved */
	unsigned long r6;		/* preserved */
	unsigned long r7;		/* preserved */
	unsigned long cr_iipa;   /* for emulation */
	unsigned long cr_isr;    /* for emulation */
	unsigned long eml_unat;    /* used for emulating instruction */
	unsigned long rfi_pfs;     /* used for elulating rfi */
#endif

	/* The following registers are saved by SAVE_REST: */
	unsigned long r16;		/* scratch */
	unsigned long r17;		/* scratch */
	unsigned long r18;		/* scratch */
	unsigned long r19;		/* scratch */
	unsigned long r20;		/* scratch */
	unsigned long r21;		/* scratch */
	unsigned long r22;		/* scratch */
	unsigned long r23;		/* scratch */
	unsigned long r24;		/* scratch */
	unsigned long r25;		/* scratch */
	unsigned long r26;		/* scratch */
	unsigned long r27;		/* scratch */
	unsigned long r28;		/* scratch */
	unsigned long r29;		/* scratch */
	unsigned long r30;		/* scratch */
	unsigned long r31;		/* scratch */

	unsigned long ar_ccv;		/* compare/exchange value (scratch) */

	/*
	 * Floating point registers that the kernel considers scratch:
	 */
	struct pt_fpreg f6;		/* scratch */
	struct pt_fpreg f7;		/* scratch */
	struct pt_fpreg f8;		/* scratch */
	struct pt_fpreg f9;		/* scratch */
	struct pt_fpreg f10;		/* scratch */
	struct pt_fpreg f11;		/* scratch */
}cpu_user_regs_t;

typedef union {
	unsigned long value;
	struct {
		int 	a_int:1;
		int 	a_from_int_cr:1;
		int	a_to_int_cr:1;
		int	a_from_psr:1;
		int	a_from_cpuid:1;
		int	a_cover:1;
		int	a_bsw:1;
		long	reserved:57;
	};
} vac_t;

typedef union {
	unsigned long value;
	struct {
		int 	d_vmsw:1;
		int 	d_extint:1;
		int	d_ibr_dbr:1;
		int	d_pmc:1;
		int	d_to_pmd:1;
		int	d_itm:1;
		long	reserved:58;
	};
} vdc_t;

typedef struct {
	vac_t			vac;
	vdc_t			vdc;
	unsigned long		virt_env_vaddr;
	unsigned long		reserved1[29];
	unsigned long		vhpi;
	unsigned long		reserved2[95];
	union {
	  unsigned long		vgr[16];
  	  unsigned long bank1_regs[16]; // bank1 regs (r16-r31) when bank0 active
	};
	union {
	  unsigned long		vbgr[16];
	  unsigned long bank0_regs[16]; // bank0 regs (r16-r31) when bank1 active
	};
	unsigned long		vnat;
	unsigned long		vbnat;
	unsigned long		vcpuid[5];
	unsigned long		reserved3[11];
	unsigned long		vpsr;
	unsigned long		vpr;
	unsigned long		reserved4[76];
	union {
	  unsigned long		vcr[128];
          struct {
  	    unsigned long	dcr;		// CR0
	    unsigned long	itm;
	    unsigned long	iva;
	    unsigned long	rsv1[5];
	    unsigned long	pta;		// CR8
	    unsigned long	rsv2[7];
	    unsigned long	ipsr;		// CR16
	    unsigned long	isr;
	    unsigned long	rsv3;
	    unsigned long	iip;
	    unsigned long	ifa;
	    unsigned long	itir;
	    unsigned long	iipa;
	    unsigned long	ifs;
	    unsigned long	iim;		// CR24
	    unsigned long	iha;
	    unsigned long	rsv4[38];
	    unsigned long	lid;		// CR64
	    unsigned long	ivr;
	    unsigned long	tpr;
	    unsigned long	eoi;
	    unsigned long	irr[4];
	    unsigned long	itv;		// CR72
	    unsigned long	pmv;
	    unsigned long	cmcv;
	    unsigned long	rsv5[5];
	    unsigned long	lrr0;		// CR80
	    unsigned long	lrr1;
	    unsigned long	rsv6[46];
          };
	};
	union {
	  unsigned long		reserved5[128];
	  struct {
	    unsigned long precover_ifs;
	    unsigned long unat;  // not sure if this is needed until NaT arch is done
	    int interrupt_collection_enabled; // virtual psr.ic
	    int interrupt_delivery_enabled; // virtual psr.i
	    int pending_interruption;
	    int incomplete_regframe;	// see SDM vol2 6.8
	    unsigned long delivery_mask[4];
	    int metaphysical_mode;	// 1 = use metaphys mapping, 0 = use virtual
	    int banknum;	// 0 or 1, which virtual register bank is active
	    unsigned long rrs[8];	// region registers
	    unsigned long krs[8];	// kernel registers
	    unsigned long pkrs[8];	// protection key registers
	    unsigned long tmp[8];	// temp registers (e.g. for hyperprivops)
	  };
        };
#ifdef CONFIG_VTI
	unsigned long		reserved6[3456];
	unsigned long		vmm_avail[128];
	unsigned long		reserved7[4096];
#endif
} mapped_regs_t;

typedef struct {
	mapped_regs_t *privregs;
	int evtchn_vector;
} arch_vcpu_info_t;

typedef mapped_regs_t vpd_t;

#define __ARCH_HAS_VCPU_INFO

typedef struct {
	int domain_controller_evtchn;
	unsigned int flags;
//} arch_shared_info_t;
} arch_shared_info_t;		// DON'T PACK 

typedef struct vcpu_guest_context {
#define VGCF_FPU_VALID (1<<0)
#define VGCF_VMX_GUEST (1<<1)
#define VGCF_IN_KERNEL (1<<2)
	unsigned long flags;       /* VGCF_* flags */
	unsigned long pt_base;     /* PMT table base */
	unsigned long pt_max_pfn;  /* Max pfn including holes */
	unsigned long share_io_pg; /* Shared page for I/O emulation */
	unsigned long vm_assist;   /* VMASST_TYPE_* bitmap, now none on IPF */
	unsigned long guest_iip;   /* Guest entry point */

	cpu_user_regs_t regs;
	arch_vcpu_info_t vcpu;
	arch_shared_info_t shared;
} vcpu_guest_context_t;

#endif /* !__ASSEMBLY__ */

#define	XEN_HYPER_RFI			0x1
#define	XEN_HYPER_RSM_DT		0x2
#define	XEN_HYPER_SSM_DT		0x3
#define	XEN_HYPER_COVER			0x4
#define	XEN_HYPER_ITC_D			0x5
#define	XEN_HYPER_ITC_I			0x6
#define	XEN_HYPER_SSM_I			0x7
#define	XEN_HYPER_GET_IVR		0x8
#define	XEN_HYPER_GET_TPR		0x9
#define	XEN_HYPER_SET_TPR		0xa
#define	XEN_HYPER_EOI			0xb
#define	XEN_HYPER_SET_ITM		0xc
#define	XEN_HYPER_THASH			0xd
#define	XEN_HYPER_PTC_GA		0xe
#define	XEN_HYPER_ITR_D			0xf
#define	XEN_HYPER_GET_RR		0x10
#define	XEN_HYPER_SET_RR		0x11

#endif /* __HYPERVISOR_IF_IA64_H__ */
