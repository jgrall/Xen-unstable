/******************************************************************************
 * arch-x86_64/hypervisor-if.h
 * 
 * Guest OS interface to AMD x86-64 bit Xen.
 */

#ifndef __HYPERVISOR_IF_X86_64_H__
#define __HYPERVISOR_IF_X86_64_H__

/*
 * SEGMENT DESCRIPTOR TABLES
 */
/*
 * A number of GDT entries are reserved by Xen. These are not situated at the
 * start of the GDT because some stupid OSes export hard-coded selector values
 * in their ABI. These hard-coded values are always near the start of the GDT,
 * so Xen places itself out of the way.
 * 
 * NB. The reserved range is inclusive (that is, both FIRST_RESERVED_GDT_ENTRY
 * and LAST_RESERVED_GDT_ENTRY are reserved).
 */
#define NR_RESERVED_GDT_ENTRIES    40 
#define FIRST_RESERVED_GDT_ENTRY   256
#define LAST_RESERVED_GDT_ENTRY    \
  (FIRST_RESERVED_GDT_ENTRY + NR_RESERVED_GDT_ENTRIES - 1)

/*
 * 64-bit segment selectors
 * These flat segments are in the Xen-private section of every GDT. Since these
 * are also present in the initial GDT, many OSes will be able to avoid
 * installing their own GDT.
 */

#define FLAT_RING3_CS32 0x0823  /* GDT index 260 */
#define FLAT_RING3_CS64 0x082b  /* GDT index 261 */
#define FLAT_RING3_DS   0x0833  /* GDT index 262 */

#define FLAT_GUESTOS_DS   FLAT_RING3_DS
#define FLAT_GUESTOS_CS   FLAT_RING3_CS64
#define FLAT_GUESTOS_CS32 FLAT_RING3_CS32

#define FLAT_USER_DS      FLAT_RING3_DS
#define FLAT_USER_CS      FLAT_RING3_CS64
#define FLAT_USER_CS32    FLAT_RING3_CS32

/* And the trap vector is... */
#define TRAP_INSTR "syscall"

/* The machine->physical mapping table starts at this address, read-only. */
#ifndef machine_to_phys_mapping
#define machine_to_phys_mapping ((unsigned long *)0xffff810000000000ULL)
#endif

#ifndef __ASSEMBLY__

/*
 * Send an array of these to HYPERVISOR_set_trap_table()
 */
#define TI_GET_DPL(_ti)      ((_ti)->flags & 3)
#define TI_GET_IF(_ti)       ((_ti)->flags & 4)
#define TI_SET_DPL(_ti,_dpl) ((_ti)->flags |= (_dpl))
#define TI_SET_IF(_ti,_if)   ((_ti)->flags |= ((!!(_if))<<2))
typedef struct trap_info_st
{
    unsigned char  vector;  /* exception vector                              */
    unsigned char  flags;   /* 0-3: privilege level; 4: clear event enable?  */
    unsigned short cs;      /* code selector                                 */
    unsigned long  address; /* code address                                  */
} trap_info_t;

typedef struct
{
    unsigned long r15;
    unsigned long r14;
    unsigned long r13;
    unsigned long r12;
    unsigned long rbp;
    unsigned long rbx;
    unsigned long r11;
    unsigned long r10;
    unsigned long r9;
    unsigned long r8;
    unsigned long rax;
    unsigned long rcx;
    unsigned long rdx;
    unsigned long rsi;
    unsigned long rdi;
    unsigned long rip;
    unsigned long cs;
    unsigned long eflags;
    unsigned long rsp;
    unsigned long ss;
} execution_context_t;

typedef struct {
    unsigned long  tsc_bits;      /* 32 bits read from the CPU's TSC. */
    unsigned int   tsc_bitshift;  /* 'tsc_bits' uses N:N+31 of TSC.   */
} tsc_timestamp_t;

/*
 * The following is all CPU context. Note that the i387_ctxt block is filled 
 * in by FXSAVE if the CPU has feature FXSR; otherwise FSAVE is used.
 */
typedef struct full_execution_context_st
{
#define ECF_I387_VALID (1<<0)
    unsigned long flags;
    execution_context_t cpu_ctxt;           /* User-level CPU registers     */
    char          fpu_ctxt[512];            /* User-level FPU registers     */
    trap_info_t   trap_ctxt[256];           /* Virtual IDT                  */
    unsigned long ldt_base, ldt_ents;       /* LDT (linear address, # ents) */
    unsigned long gdt_frames[16], gdt_ents; /* GDT (machine frames, # ents) */
    unsigned long guestos_ss, guestos_esp;  /* Virtual TSS (only SS1/ESP1)  */
    unsigned long pt_base;                  /* CR3 (pagetable base)         */
    unsigned long debugreg[8];              /* DB0-DB7 (debug registers)    */
    unsigned long event_callback_cs;        /* CS:EIP of event callback     */
    unsigned long event_callback_eip;
    unsigned long failsafe_callback_cs;     /* CS:EIP of failsafe callback  */
    unsigned long failsafe_callback_eip;
} full_execution_context_t;

#endif /* !__ASSEMBLY__ */

#endif /* __HYPERVISOR_IF_H__ */
