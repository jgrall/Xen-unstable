/******************************************************************************
 * os.h
 * 
 * random collection of macros and definition
 */

#ifndef _OS_H_
#define _OS_H_

#define NULL 0

#if __GNUC__ == 2 && __GNUC_MINOR__ < 96
#define __builtin_expect(x, expected_value) (x)
#endif
#define unlikely(x)  __builtin_expect((x),0)

#define smp_processor_id() 0
#define preempt_disable() ((void)0)
#define preempt_enable() ((void)0)

#define force_evtchn_callback() ((void)HYPERVISOR_xen_version(0))

#ifndef __ASSEMBLY__
#include <types.h>
#endif
#include <xen-public/xen.h>

#define __KERNEL_CS  FLAT_KERNEL_CS
#define __KERNEL_DS  FLAT_KERNEL_DS
#define __KERNEL_SS  FLAT_KERNEL_SS

/* Everything below this point is not included by assembler (.S) files. */
#ifndef __ASSEMBLY__

#define pt_regs xen_regs

void trap_init(void);
void dump_regs(struct pt_regs *regs);

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

#define local_irq_save(x)	__save_and_cli(x)
#define local_irq_restore(x)	__restore_flags(x)
#define local_save_flags(x)	__save_flags(x)
#define local_irq_disable()	__cli()
#define local_irq_enable()	__sti()

#define irqs_disabled()			\
    HYPERVISOR_shared_info->vcpu_data[smp_processor_id()].evtchn_upcall_mask

/* This is a barrier for the compiler only, NOT the processor! */
#define barrier() __asm__ __volatile__("": : :"memory")

#define LOCK_PREFIX ""
#define LOCK ""
#define ADDR (*(volatile long *) addr)
/*
 * Make sure gcc doesn't try to be clever and move things around
 * on us. We need to use _exactly_ the address the user gave us,
 * not some alias that contains the same information.
 */
typedef struct { volatile int counter; } atomic_t;


#define xchg(ptr,v) \
        ((__typeof__(*(ptr)))__xchg((unsigned long)(v),(ptr),sizeof(*(ptr))))
struct __xchg_dummy { unsigned long a[100]; };
#define __xg(x) ((volatile struct __xchg_dummy *)(x))
static __inline__ unsigned long __xchg(unsigned long x, volatile void * ptr,
                                   int size)
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
        __asm__ __volatile__("xchgl %0,%1"
                             :"=r" (x)
                             :"m" (*__xg(ptr)), "0" (x)
                             :"memory");
        break;
    }
    return x;
}

/**
 * test_and_clear_bit - Clear a bit and return its old value
 * @nr: Bit to set
 * @addr: Address to count from
 *
 * This operation is atomic and cannot be reordered.  
 * It also implies a memory barrier.
 */
static __inline__ int test_and_clear_bit(int nr, volatile void * addr)
{
        int oldbit;

        __asm__ __volatile__( LOCK_PREFIX
                "btrl %2,%1\n\tsbbl %0,%0"
                :"=r" (oldbit),"=m" (ADDR)
                :"Ir" (nr) : "memory");
        return oldbit;
}

static __inline__ int constant_test_bit(int nr, const volatile void * addr)
{
    return ((1UL << (nr & 31)) & (((const volatile unsigned int *) addr)[nr >> 5])) != 0;
}

static __inline__ int variable_test_bit(int nr, volatile void * addr)
{
    int oldbit;
    
    __asm__ __volatile__(
        "btl %2,%1\n\tsbbl %0,%0"
        :"=r" (oldbit)
        :"m" (ADDR),"Ir" (nr));
    return oldbit;
}

#define test_bit(nr,addr) \
(__builtin_constant_p(nr) ? \
 constant_test_bit((nr),(addr)) : \
 variable_test_bit((nr),(addr)))


/**
 * set_bit - Atomically set a bit in memory
 * @nr: the bit to set
 * @addr: the address to start counting from
 *
 * This function is atomic and may not be reordered.  See __set_bit()
 * if you do not require the atomic guarantees.
 * Note that @nr may be almost arbitrarily large; this function is not
 * restricted to acting on a single-word quantity.
 */
static __inline__ void set_bit(int nr, volatile void * addr)
{
        __asm__ __volatile__( LOCK_PREFIX
                "btsl %1,%0"
                :"=m" (ADDR)
                :"Ir" (nr));
}

/**
 * clear_bit - Clears a bit in memory
 * @nr: Bit to clear
 * @addr: Address to start counting from
 *
 * clear_bit() is atomic and may not be reordered.  However, it does
 * not contain a memory barrier, so if it is used for locking purposes,
 * you should call smp_mb__before_clear_bit() and/or smp_mb__after_clear_bit()
 * in order to ensure changes are visible on other processors.
 */
static __inline__ void clear_bit(int nr, volatile void * addr)
{
        __asm__ __volatile__( LOCK_PREFIX
                "btrl %1,%0"
                :"=m" (ADDR)
                :"Ir" (nr));
}

/**
 * atomic_inc - increment atomic variable
 * @v: pointer of type atomic_t
 * 
 * Atomically increments @v by 1.  Note that the guaranteed
 * useful range of an atomic_t is only 24 bits.
 */ 
static __inline__ void atomic_inc(atomic_t *v)
{
        __asm__ __volatile__(
                LOCK "incl %0"
                :"=m" (v->counter)
                :"m" (v->counter));
}


#define rdtscll(val) \
     __asm__ __volatile__("rdtsc" : "=A" (val))


#endif /* !__ASSEMBLY__ */

#endif /* _OS_H_ */
