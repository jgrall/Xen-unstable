#ifndef __ASM_SYSTEM_H
#define __ASM_SYSTEM_H

#include <linux/config.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm-xen/synch_bitops.h>
#include <asm-xen/hypervisor.h>
#include <asm-xen/xen-public/arch-x86_64.h>

#ifdef __KERNEL__

#ifdef CONFIG_SMP
#define LOCK_PREFIX "lock ; "
#else
#define LOCK_PREFIX ""
#endif

#define __STR(x) #x
#define STR(x) __STR(x)

#define __SAVE(reg,offset) "movq %%" #reg ",(14-" #offset ")*8(%%rsp)\n\t"
#define __RESTORE(reg,offset) "movq (14-" #offset ")*8(%%rsp),%%" #reg "\n\t"

/* frame pointer must be last for get_wchan */
#define SAVE_CONTEXT    "pushfq ; pushq %%rbp ; movq %%rsi,%%rbp\n\t"
#define RESTORE_CONTEXT "movq %%rbp,%%rsi ; popq %%rbp ; popfq\n\t" 

#define __EXTRA_CLOBBER  \
	,"rcx","rbx","rdx","r8","r9","r10","r11","r12","r13","r14","r15"

#define switch_to(prev,next,last) \
	asm volatile(SAVE_CONTEXT						    \
		     "movq %%rsp,%P[threadrsp](%[prev])\n\t" /* save RSP */	  \
		     "movq %P[threadrsp](%[next]),%%rsp\n\t" /* restore RSP */	  \
		     "call __switch_to\n\t"					  \
		     ".globl thread_return\n"					\
		     "thread_return:\n\t"					    \
		     "movq %%gs:%P[pda_pcurrent],%%rsi\n\t"			  \
		     "movq %P[thread_info](%%rsi),%%r8\n\t"			  \
		     LOCK "btr  %[tif_fork],%P[ti_flags](%%r8)\n\t"		  \
		     "movq %%rax,%%rdi\n\t" 					  \
		     "jc   ret_from_fork\n\t"					  \
		     RESTORE_CONTEXT						    \
		     : "=a" (last)					  	  \
		     : [next] "S" (next), [prev] "D" (prev),			  \
		       [threadrsp] "i" (offsetof(struct task_struct, thread.rsp)), \
		       [ti_flags] "i" (offsetof(struct thread_info, flags)),\
		       [tif_fork] "i" (TIF_FORK),			  \
		       [thread_info] "i" (offsetof(struct task_struct, thread_info)), \
		       [pda_pcurrent] "i" (offsetof(struct x8664_pda, pcurrent))   \
		     : "memory", "cc" __EXTRA_CLOBBER)
    

extern void load_gs_index(unsigned);

/*
 * Load a segment. Fall back on loading the zero
 * segment if something goes wrong..
 */
#define loadsegment(seg,value)	\
	asm volatile("\n"			\
		"1:\t"				\
		"movl %k0,%%" #seg "\n"		\
		"2:\n"				\
		".section .fixup,\"ax\"\n"	\
		"3:\t"				\
		"movl %1,%%" #seg "\n\t" 	\
		"jmp 2b\n"			\
		".previous\n"			\
		".section __ex_table,\"a\"\n\t"	\
		".align 8\n\t"			\
		".quad 1b,3b\n"			\
		".previous"			\
		: :"r" (value), "r" (0))

#define set_debug(value,register) \
                __asm__("movq %0,%%db" #register  \
		: /* no output */ \
		:"r" ((unsigned long) value))


#ifdef __KERNEL__
struct alt_instr { 
	__u8 *instr; 		/* original instruction */
	__u8 *replacement;
	__u8  cpuid;		/* cpuid bit set for replacement */
	__u8  instrlen;		/* length of original instruction */
	__u8  replacementlen; 	/* length of new instruction, <= instrlen */ 
	__u8  pad[5];
}; 
#endif

/*
 * Alternative instructions for different CPU types or capabilities.
 * 
 * This allows to use optimized instructions even on generic binary
 * kernels.
 * 
 * length of oldinstr must be longer or equal the length of newinstr
 * It can be padded with nops as needed.
 * 
 * For non barrier like inlines please define new variants
 * without volatile and memory clobber.
 */
#define alternative(oldinstr, newinstr, feature) 	\
	asm volatile ("661:\n\t" oldinstr "\n662:\n" 		     \
		      ".section .altinstructions,\"a\"\n"     	     \
		      "  .align 8\n"				       \
		      "  .quad 661b\n"            /* label */          \
		      "  .quad 663f\n"		  /* new instruction */ \
		      "  .byte %c0\n"             /* feature bit */    \
		      "  .byte 662b-661b\n"       /* sourcelen */      \
		      "  .byte 664f-663f\n"       /* replacementlen */ \
		      ".previous\n"					\
		      ".section .altinstr_replacement,\"ax\"\n"		\
		      "663:\n\t" newinstr "\n664:\n"   /* replacement */ \
		      ".previous" :: "i" (feature) : "memory")  

/*
 * Alternative inline assembly with input.
 * 
 * Pecularities:
 * No memory clobber here. 
 * Argument numbers start with 1.
 * Best is to use constraints that are fixed size (like (%1) ... "r")
 * If you use variable sized constraints like "m" or "g" in the 
 * replacement maake sure to pad to the worst case length.
 */
#define alternative_input(oldinstr, newinstr, feature, input...)	\
	asm volatile ("661:\n\t" oldinstr "\n662:\n"			\
		      ".section .altinstructions,\"a\"\n"		\
		      "  .align 8\n"					\
		      "  .quad 661b\n"            /* label */		\
		      "  .quad 663f\n"		  /* new instruction */	\
		      "  .byte %c0\n"             /* feature bit */	\
		      "  .byte 662b-661b\n"       /* sourcelen */	\
		      "  .byte 664f-663f\n"       /* replacementlen */	\
		      ".previous\n"					\
		      ".section .altinstr_replacement,\"ax\"\n"		\
		      "663:\n\t" newinstr "\n664:\n"   /* replacement */ \
		      ".previous" :: "i" (feature), ##input)

/*
 * Clear and set 'TS' bit respectively
 */
#define clts() (HYPERVISOR_fpu_taskswitch(0))

static inline unsigned long read_cr0(void)
{ 
	unsigned long cr0;
	asm volatile("movq %%cr0,%0" : "=r" (cr0));
	return cr0;
} 

static inline void write_cr0(unsigned long val) 
{ 
	asm volatile("movq %0,%%cr0" :: "r" (val));
} 

static inline unsigned long read_cr3(void)
{ 
	unsigned long cr3;
	asm("movq %%cr3,%0" : "=r" (cr3));
	return cr3;
} 

static inline unsigned long read_cr4(void)
{ 
	unsigned long cr4;
	asm("movq %%cr4,%0" : "=r" (cr4));
	return cr4;
} 

static inline void write_cr4(unsigned long val)
{ 
	asm volatile("movq %0,%%cr4" :: "r" (val));
} 

#define stts() (HYPERVISOR_fpu_taskswitch(1))

#define wbinvd() \
	__asm__ __volatile__ ("wbinvd": : :"memory");

#endif	/* __KERNEL__ */

#define nop() __asm__ __volatile__ ("nop")

#define xchg(ptr,v) ((__typeof__(*(ptr)))__xchg((unsigned long)(v),(ptr),sizeof(*(ptr))))

#define tas(ptr) (xchg((ptr),1))

#define __xg(x) ((volatile long *)(x))

extern inline void set_64bit(volatile unsigned long *ptr, unsigned long val)
{
	*ptr = val;
}

#define _set_64bit set_64bit

/*
 * Note: no "lock" prefix even on SMP: xchg always implies lock anyway
 * Note 2: xchg has side effect, so that attribute volatile is necessary,
 *	  but generally the primitive is invalid, *ptr is output argument. --ANK
 */
static inline unsigned long __xchg(unsigned long x, volatile void * ptr, int size)
{
	switch (size) {
		case 1:
			__asm__ __volatile__("xchgb %b0,%1"
				:"=q" (x)
				:"m" (*__xg(ptr)), "0" (x)
				:"memory");
			break;
		case 2:
			__asm__ __volatile__("xchgw %w0,%1"
				:"=r" (x)
				:"m" (*__xg(ptr)), "0" (x)
				:"memory");
			break;
		case 4:
			__asm__ __volatile__("xchgl %k0,%1"
				:"=r" (x)
				:"m" (*__xg(ptr)), "0" (x)
				:"memory");
			break;
		case 8:
			__asm__ __volatile__("xchgq %0,%1"
				:"=r" (x)
				:"m" (*__xg(ptr)), "0" (x)
				:"memory");
			break;
	}
	return x;
}

/*
 * Atomic compare and exchange.  Compare OLD with MEM, if identical,
 * store NEW in MEM.  Return the initial value in MEM.  Success is
 * indicated by comparing RETURN with OLD.
 */

#define __HAVE_ARCH_CMPXCHG 1

static inline unsigned long __cmpxchg(volatile void *ptr, unsigned long old,
				      unsigned long new, int size)
{
	unsigned long prev;
	switch (size) {
	case 1:
		__asm__ __volatile__(LOCK_PREFIX "cmpxchgb %b1,%2"
				     : "=a"(prev)
				     : "q"(new), "m"(*__xg(ptr)), "0"(old)
				     : "memory");
		return prev;
	case 2:
		__asm__ __volatile__(LOCK_PREFIX "cmpxchgw %w1,%2"
				     : "=a"(prev)
				     : "q"(new), "m"(*__xg(ptr)), "0"(old)
				     : "memory");
		return prev;
	case 4:
		__asm__ __volatile__(LOCK_PREFIX "cmpxchgl %k1,%2"
				     : "=a"(prev)
				     : "q"(new), "m"(*__xg(ptr)), "0"(old)
				     : "memory");
		return prev;
	case 8:
		__asm__ __volatile__(LOCK_PREFIX "cmpxchgq %1,%2"
				     : "=a"(prev)
				     : "q"(new), "m"(*__xg(ptr)), "0"(old)
				     : "memory");
		return prev;
	}
	return old;
}

#define cmpxchg(ptr,o,n)\
	((__typeof__(*(ptr)))__cmpxchg((ptr),(unsigned long)(o),\
					(unsigned long)(n),sizeof(*(ptr))))

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#define smp_rmb()	rmb()
#define smp_wmb()	wmb()
#define smp_read_barrier_depends()	do {} while(0)
#else
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#define smp_read_barrier_depends()	do {} while(0)
#endif

    
/*
 * Force strict CPU ordering.
 * And yes, this is required on UP too when we're talking
 * to devices.
 */
#define mb() 	asm volatile("mfence":::"memory")
#define rmb()	asm volatile("lfence":::"memory")

#ifdef CONFIG_UNORDERED_IO
#define wmb()	asm volatile("sfence" ::: "memory")
#else
#define wmb()	asm volatile("" ::: "memory")
#endif
#define read_barrier_depends()	do {} while(0)
#define set_mb(var, value) do { xchg(&var, value); } while (0)
#define set_wmb(var, value) do { var = value; wmb(); } while (0)

#define warn_if_not_ulong(x) do { unsigned long foo; (void) (&(x) == &foo); } while (0)


/* 
 * The use of 'barrier' in the following reflects their use as local-lock
 * operations. Reentrancy must be prevented (e.g., __cli()) /before/ following
 * critical operations are executed. All critical operations must complete
 * /before/ reentrancy is permitted (e.g., __sti()). Alpha architecture also
 * includes these barriers, for example.
 */

#define __cli()								\
do {									\
	vcpu_info_t *_vcpu;						\
	preempt_disable();						\
	_vcpu = &HYPERVISOR_shared_info->vcpu_data[smp_processor_id()];	\
	_vcpu->evtchn_upcall_mask = 1;					\
	preempt_enable_no_resched();					\
	barrier();							\
} while (0)

#define __sti()								\
do {									\
	vcpu_info_t *_vcpu;						\
	barrier();							\
	preempt_disable();						\
	_vcpu = &HYPERVISOR_shared_info->vcpu_data[smp_processor_id()];	\
	_vcpu->evtchn_upcall_mask = 0;					\
	barrier(); /* unmask then check (avoid races) */		\
	if ( unlikely(_vcpu->evtchn_upcall_pending) )			\
		force_evtchn_callback();				\
	preempt_enable();						\
} while (0)

#define __save_flags(x)							\
do {									\
	vcpu_info_t *_vcpu;						\
	_vcpu = &HYPERVISOR_shared_info->vcpu_data[smp_processor_id()];	\
	(x) = _vcpu->evtchn_upcall_mask;				\
} while (0)

#define __restore_flags(x)						\
do {									\
	vcpu_info_t *_vcpu;						\
	barrier();							\
	preempt_disable();						\
	_vcpu = &HYPERVISOR_shared_info->vcpu_data[smp_processor_id()];	\
	if ((_vcpu->evtchn_upcall_mask = (x)) == 0) {			\
		barrier(); /* unmask then check (avoid races) */	\
		if ( unlikely(_vcpu->evtchn_upcall_pending) )		\
			force_evtchn_callback();			\
		preempt_enable();					\
	} else								\
		preempt_enable_no_resched();				\
} while (0)

#define safe_halt()		((void)0)

#define __save_and_cli(x)						\
do {									\
	vcpu_info_t *_vcpu;						\
	preempt_disable();						\
	_vcpu = &HYPERVISOR_shared_info->vcpu_data[smp_processor_id()];	\
	(x) = _vcpu->evtchn_upcall_mask;				\
	_vcpu->evtchn_upcall_mask = 1;					\
	preempt_enable_no_resched();					\
	barrier();							\
} while (0)

void cpu_idle_wait(void);

#define local_irq_save(x)	__save_and_cli(x)
#define local_irq_restore(x)	__restore_flags(x)
#define local_save_flags(x)	__save_flags(x)
#define local_irq_disable()	__cli()
#define local_irq_enable()	__sti()

#define irqs_disabled()			\
    HYPERVISOR_shared_info->vcpu_data[smp_processor_id()].evtchn_upcall_mask

/*
 * disable hlt during certain critical i/o operations
 */
#define HAVE_DISABLE_HLT
void disable_hlt(void);
void enable_hlt(void);

#define HAVE_EAT_KEY
void eat_key(void);

extern unsigned long arch_align_stack(unsigned long sp);

#endif
