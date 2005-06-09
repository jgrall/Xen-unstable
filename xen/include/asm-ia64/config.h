#ifndef	_IA64_CONFIG_H_
#define _IA64_CONFIG_H_

// control flags for turning on/off features under test
#undef CLONE_DOMAIN0
//#define CLONE_DOMAIN0 1
#define DOMU_BUILD_STAGING
#define VHPT_GLOBAL
#define DOMU_AUTO_RESTART

// manufactured from component pieces

// defined in linux/arch/ia64/defconfig
//#define	CONFIG_IA64_GENERIC
#define	CONFIG_IA64_HP_SIM
#define	CONFIG_IA64_L1_CACHE_SHIFT 7
// needed by include/asm-ia64/page.h
#define	CONFIG_IA64_PAGE_SIZE_16KB	// 4KB doesn't work?!?
#define	CONFIG_IA64_GRANULE_16MB

#define CONFIG_EFI_PCDP
#define CONFIG_SERIAL_SGI_L1_CONSOLE

#ifndef __ASSEMBLY__

// can't find where this typedef was before?!?
// needed by include/asm-ia64/processor.h (and other places)
typedef int pid_t;

// from include/linux/kernel.h
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

//////////////////////////////////////

#define FASTCALL(x) x	// see linux/include/linux/linkage.h
#define fastcall	// " "

#define watchdog_disable() ((void)0)
#define watchdog_enable()  ((void)0)
// from linux/include/linux/types.h
#define CLEAR_BITMAP(name,bits) \
	memset(name, 0, BITS_TO_LONGS(bits)*sizeof(unsigned long))

// FIXME?: x86-ism used in xen/mm.h
#define LOCK_PREFIX

extern unsigned long xenheap_phys_end;
extern unsigned long xen_pstart;
extern unsigned long xenheap_size;
extern unsigned long dom0_start;
extern unsigned long dom0_size;

// from linux/include/linux/mm.h
extern struct page *mem_map;

// xen/include/asm/config.h
extern char _end[]; /* standard ELF symbol */

// linux/include/linux/compiler.h
#define __attribute_const__
#define __user
//#define __kernel
//#define __safe
#define __force
#define __chk_user_ptr(x) (void)0
//#define __chk_io_ptr(x) (void)0
//#define __builtin_warning(x, y...) (1)
//#define __acquires(x)
//#define __releases(x)
//#define __acquire(x) (void)0
//#define __release(x) (void)0
//#define __cond_lock(x) (x)
#define __must_check
#define __deprecated

// xen/include/asm/config.h
#define HZ 100
// leave SMP for a later time
#define NR_CPUS 1
//#define NR_CPUS 16
//#define CONFIG_NR_CPUS 16
#define barrier() __asm__ __volatile__("": : :"memory")

///////////////////////////////////////////////////////////////
// xen/include/asm/config.h
// Natural boundary upon TR size to define xenheap space
#define XENHEAP_DEFAULT_MB (1 << (KERNEL_TR_PAGE_SHIFT - 20))
#define XENHEAP_DEFAULT_SIZE	(1 << KERNEL_TR_PAGE_SHIFT)
#define	ELFSIZE	64

///////////////////////////////////////////////////////////////

// get rid of difficult circular include dependency
#define CMPXCHG_BUGCHECK(v)
#define CMPXCHG_BUGCHECK_DECL

// from include/asm-ia64/smp.h
#ifdef CONFIG_SMP
#error "Lots of things to fix to enable CONFIG_SMP!"
#endif
#define	get_cpu()	0
#define put_cpu()	do {} while(0)

// from linux/include/linux/mm.h
struct page;

// function calls; see decl in xen/include/xen/sched.h
#undef free_task_struct
#undef alloc_task_struct

// initial task has a different name in Xen
//#define	idle0_task	init_task
#define	idle0_vcpu	init_task

// avoid redefining task_t in asm/thread_info.h
#define task_t	struct domain

// avoid redefining task_struct in asm/current.h
#define task_struct vcpu

// linux/include/asm-ia64/machvec.h (linux/arch/ia64/lib/io.c)
#define platform_inb	__ia64_inb
#define platform_inw	__ia64_inw
#define platform_inl	__ia64_inl
#define platform_outb	__ia64_outb
#define platform_outw	__ia64_outw
#define platform_outl	__ia64_outl

// FIXME: This just overrides a use in a typedef (not allowed in ia64,
//  or maybe just in older gcc's?) used in ac_timer.c but should be OK
//  (and indeed is probably required!) elsewhere
#undef __cacheline_aligned
#undef ____cacheline_aligned
#undef ____cacheline_aligned_in_smp
#define __cacheline_aligned
#define __cacheline_aligned_in_smp
#define ____cacheline_aligned
#define ____cacheline_aligned_in_smp
#define ____cacheline_maxaligned_in_smp

#include "asm/types.h"	// for u64

// warning: unless search_extable is declared, the return value gets
// truncated to 32-bits, causing a very strange error in privop handling
struct exception_table_entry;

const struct exception_table_entry *
search_extable(const struct exception_table_entry *first,
	       const struct exception_table_entry *last,
	       unsigned long value);
void sort_extable(struct exception_table_entry *start,
		  struct exception_table_entry *finish);
void sort_main_extable(void);

#define printk printf

#undef  __ARCH_IRQ_STAT

#define find_first_set_bit(x)	(ffs(x)-1)	// FIXME: Is this right???

// from include/asm-x86/*/uaccess.h
#define array_access_ok(addr,count,size)			\
    (likely(sizeof(count) <= 4) /* disallow 64-bit counts */ &&  \
     access_ok(type,addr,count*size))

// see drivers/char/console.c
#ifndef CONFIG_VTI
#define	OPT_CONSOLE_STR "com1"
#else // CONFIG_VTI
#define	OPT_CONSOLE_STR "com2"
#endif // CONFIG_VTI

#define __attribute_used__	__attribute__ ((unused))

// see include/asm-x86/atomic.h (different from standard linux)
#define _atomic_set(v,i) (((v).counter) = (i))
#define _atomic_read(v) ((v).counter)
// FIXME following needs work
#define atomic_compareandswap(old, new, v) old

// see include/asm-ia64/mm.h, handle remaining pfn_info uses until gone
#define pfn_info page

// see common/keyhandler.c
#define	nop()	asm volatile ("nop 0")

// from include/linux/preempt.h (needs including from interrupt.h or smp.h)
#define preempt_enable()	do { } while (0)
#define preempt_disable()	do { } while (0)

// needed for include/xen/linuxtime.h
typedef s64 time_t;
typedef s64 suseconds_t;

// needed for include/linux/jiffies.h
typedef long clock_t;

// from include/linux/kernel.h, needed by jiffies.h
#define typecheck(type,x) \
({	type __dummy; \
	typeof(x) __dummy2; \
	(void)(&__dummy == &__dummy2); \
	1; \
})

// from include/linux/timex.h, needed by arch/ia64/time.c
#define	TIME_SOURCE_CPU 0

// used in common code
#define softirq_pending(cpu)	(cpu_data(cpu)->softirq_pending)

// dup'ed from signal.h to avoid changes to includes
#define	SA_SHIRQ	0x04000000
#define	SA_INTERRUPT	0x20000000

// needed for setup.c
extern unsigned long loops_per_jiffy;
extern char saved_command_line[];
struct screen_info { };
#define seq_printf(a,b...) printf(b)
#define CONFIG_BLK_DEV_INITRD // needed to reserve memory for domain0

// needed for newer ACPI code
#define asmlinkage

#define FORCE_CRASH()	asm("break 0;;");

// these declarations got moved at some point, find a better place for them
extern int ht_per_core;

// needed for include/xen/smp.h
#define __smp_processor_id()	0

// xen/include/asm/config.h
/******************************************************************************
 * config.h
 * 
 * A Linux-style configuration list.
 */

#ifndef __XEN_IA64_CONFIG_H__
#define __XEN_IA64_CONFIG_H__

#undef CONFIG_X86

#define CONFIG_MCKINLEY

//#define CONFIG_SMP 1
//#define CONFIG_NR_CPUS 2
//leave SMP for a later time
#undef CONFIG_SMP
#undef CONFIG_X86_LOCAL_APIC
#undef CONFIG_X86_IO_APIC
#undef CONFIG_X86_L1_CACHE_SHIFT

// this needs to be on to run on hp zx1 with more than 4GB
// it is hacked around for now though
//#define	CONFIG_VIRTUAL_MEM_MAP

//#ifndef CONFIG_IA64_HP_SIM
// looks like this is hard to turn off for Xen
#define CONFIG_ACPI 1
#define CONFIG_ACPI_BOOT 1
//#endif

#define CONFIG_XEN_ATTENTION_KEY 1
#endif /* __ASSEMBLY__ */
#endif /* __XEN_IA64_CONFIG_H__ */

// FOLLOWING ADDED FOR XEN POST-NGIO and/or LINUX 2.6.7

// following derived from linux/include/linux/compiler-gcc3.h
// problem because xen (over?)simplifies include/xen/compiler.h
#if __GNUC_MAJOR < 3 || __GNUC_MINOR__ >= 3
# define __attribute_used__	__attribute__((__used__))
#else
# define __attribute_used__	__attribute__((__unused__))
#endif
#endif	/* _IA64_CONFIG_H_ */
