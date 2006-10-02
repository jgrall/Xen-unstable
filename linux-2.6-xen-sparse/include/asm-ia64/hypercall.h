/******************************************************************************
 * hypercall.h
 * 
 * Linux-specific hypervisor handling.
 * 
 * Copyright (c) 2002-2004, K A Fraser
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef __HYPERCALL_H__
#define __HYPERCALL_H__

#ifndef __HYPERVISOR_H__
# error "please don't include this file directly"
#endif

#include <asm/xen/xcom_hcall.h>
struct xencomm_handle;

/*
 * Assembler stubs for hyper-calls.
 */

#define _hypercall0(type, name)					\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"				\
			      "mov r2=%1\n"			\
			      "break 0x1000 ;;\n"		\
			      "mov %0=r8 ;;\n"			\
			      : "=r" (__res)			\
			      : "J" (__HYPERVISOR_##name)	\
			      : "r2","r8",			\
			        "memory" );			\
	(type)__res;						\
})

#define _hypercall1(type, name, a1)				\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"				\
			      "mov r14=%2\n"			\
			      "mov r2=%1\n"			\
			      "break 0x1000 ;;\n"		\
			      "mov %0=r8 ;;\n"			\
			      : "=r" (__res)			\
			      : "J" (__HYPERVISOR_##name),	\
				"rI" ((unsigned long)(a1))	\
			      : "r14","r2","r8",		\
				"memory" );			\
	(type)__res;						\
})

#define _hypercall2(type, name, a1, a2)				\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"				\
			      "mov r14=%2\n"			\
			      "mov r15=%3\n"			\
			      "mov r2=%1\n"			\
			      "break 0x1000 ;;\n"		\
			      "mov %0=r8 ;;\n"			\
			      : "=r" (__res)			\
			      : "J" (__HYPERVISOR_##name),	\
				"rI" ((unsigned long)(a1)),	\
				"rI" ((unsigned long)(a2))	\
			      : "r14","r15","r2","r8",		\
				"memory" );			\
	(type)__res;						\
})

#define _hypercall3(type, name, a1, a2, a3)			\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"                            \
			      "mov r14=%2\n"                    \
			      "mov r15=%3\n"                    \
			      "mov r16=%4\n"                    \
			      "mov r2=%1\n"                     \
			      "break 0x1000 ;;\n"               \
			      "mov %0=r8 ;;\n"                  \
			      : "=r" (__res)                    \
			      : "J" (__HYPERVISOR_##name),      \
				"rI" ((unsigned long)(a1)),     \
				"rI" ((unsigned long)(a2)),     \
				"rI" ((unsigned long)(a3))      \
			      : "r14","r15","r16","r2","r8",	\
			        "memory" );                     \
	(type)__res;                                            \
})

#define _hypercall4(type, name, a1, a2, a3, a4)			\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"                            \
			      "mov r14=%2\n"                    \
			      "mov r15=%3\n"                    \
			      "mov r16=%4\n"                    \
			      "mov r17=%5\n"                    \
			      "mov r2=%1\n"                     \
			      "break 0x1000 ;;\n"               \
			      "mov %0=r8 ;;\n"                  \
			      : "=r" (__res)                    \
			      : "J" (__HYPERVISOR_##name),      \
				"rI" ((unsigned long)(a1)),     \
				"rI" ((unsigned long)(a2)),     \
				"rI" ((unsigned long)(a3)),     \
				"rI" ((unsigned long)(a4))      \
			      : "r14","r15","r16","r2","r8",	\
			        "r17","memory" );               \
	(type)__res;                                            \
})

#define _hypercall5(type, name, a1, a2, a3, a4, a5)		\
({								\
	long __res;						\
	__asm__ __volatile__ (";;\n"                            \
			      "mov r14=%2\n"                    \
			      "mov r15=%3\n"                    \
			      "mov r16=%4\n"                    \
			      "mov r17=%5\n"                    \
			      "mov r18=%6\n"                    \
			      "mov r2=%1\n"                     \
			      "break 0x1000 ;;\n"               \
			      "mov %0=r8 ;;\n"                  \
			      : "=r" (__res)                    \
			      : "J" (__HYPERVISOR_##name),      \
				"rI" ((unsigned long)(a1)),     \
				"rI" ((unsigned long)(a2)),     \
				"rI" ((unsigned long)(a3)),     \
				"rI" ((unsigned long)(a4)),     \
				"rI" ((unsigned long)(a5))      \
			      : "r14","r15","r16","r2","r8",	\
			        "r17","r18","memory" );         \
	(type)__res;                                            \
})


static inline int
xencomm_arch_hypercall_sched_op(int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, sched_op, cmd, arg);
}

static inline long
HYPERVISOR_set_timer_op(u64 timeout)
{
	unsigned long timeout_hi = (unsigned long)(timeout >> 32);
	unsigned long timeout_lo = (unsigned long)timeout;
	return _hypercall2(long, set_timer_op, timeout_lo, timeout_hi);
}

static inline int
xencomm_arch_hypercall_dom0_op(struct xencomm_handle *op)
{
	return _hypercall1(int, dom0_op, op);
}

static inline int
xencomm_arch_hypercall_sysctl(struct xencomm_handle *op)
{
	return _hypercall1(int, sysctl, op);
}

static inline int
xencomm_arch_hypercall_domctl(struct xencomm_handle *op)
{
	return _hypercall1(int, domctl, op);
}

static inline int
xencomm_arch_hypercall_multicall(struct xencomm_handle *call_list,
				 int nr_calls)
{
	return _hypercall2(int, multicall, call_list, nr_calls);
}

static inline int
xencomm_arch_hypercall_memory_op(unsigned int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, memory_op, cmd, arg);
}

static inline int
xencomm_arch_hypercall_event_channel_op(int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, event_channel_op, cmd, arg);
}

static inline int
xencomm_arch_hypercall_acm_op(unsigned int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, acm_op, cmd, arg);
}

static inline int
xencomm_arch_hypercall_xen_version(int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, xen_version, cmd, arg);
}

static inline int
xencomm_arch_hypercall_console_io(int cmd, int count,
                                  struct xencomm_handle *str)
{
	return _hypercall3(int, console_io, cmd, count, str);
}

static inline int
xencomm_arch_hypercall_physdev_op(int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, physdev_op, cmd, arg);
}

static inline int
xencomm_arch_hypercall_grant_table_op(unsigned int cmd,
                                      struct xencomm_handle *uop,
                                      unsigned int count)
{
	return _hypercall3(int, grant_table_op, cmd, uop, count);
}

int HYPERVISOR_grant_table_op(unsigned int cmd, void *uop, unsigned int count);

extern int xencomm_arch_hypercall_suspend(struct xencomm_handle *arg);

static inline int
xencomm_arch_hypercall_callback_op(int cmd, struct xencomm_handle *arg)
{
	return _hypercall2(int, callback_op, cmd, arg);
}

static inline unsigned long
xencomm_arch_hypercall_hvm_op(int cmd, void *arg)
{
	return _hypercall2(unsigned long, hvm_op, cmd, arg);
}

extern fastcall unsigned int __do_IRQ(unsigned int irq, struct pt_regs *regs);
static inline void exit_idle(void) {}
#define do_IRQ(irq, regs) ({			\
	irq_enter();				\
	__do_IRQ((irq), (regs));		\
	irq_exit();				\
})

#include <linux/err.h>
#ifdef CONFIG_XEN
#include <asm/xen/privop.h>
#endif /* CONFIG_XEN */

static inline unsigned long
__HYPERVISOR_ioremap(unsigned long ioaddr, unsigned long size)
{
	return _hypercall3(unsigned long, ia64_dom0vp_op,
	                   IA64_DOM0VP_ioremap, ioaddr, size);
}

static inline unsigned long
HYPERVISOR_ioremap(unsigned long ioaddr, unsigned long size)
{
	unsigned long ret = ioaddr;
	if (is_running_on_xen()) {
		ret = __HYPERVISOR_ioremap(ioaddr, size);
		if (unlikely(ret == -ENOSYS))
			panic("hypercall %s failed with %ld. "
			      "Please check Xen and Linux config mismatch\n",
			      __func__, -ret);
		else if (unlikely(IS_ERR_VALUE(ret)))
			ret = ioaddr;
	}
	return ret;
}

static inline unsigned long
__HYPERVISOR_phystomach(unsigned long gpfn)
{
	return _hypercall2(unsigned long, ia64_dom0vp_op,
	                   IA64_DOM0VP_phystomach, gpfn);
}

static inline unsigned long
HYPERVISOR_phystomach(unsigned long gpfn)
{
	unsigned long ret = gpfn;
	if (is_running_on_xen()) {
		ret = __HYPERVISOR_phystomach(gpfn);
	}
	return ret;
}

static inline unsigned long
__HYPERVISOR_machtophys(unsigned long mfn)
{
	return _hypercall2(unsigned long, ia64_dom0vp_op,
	                   IA64_DOM0VP_machtophys, mfn);
}

static inline unsigned long
HYPERVISOR_machtophys(unsigned long mfn)
{
	unsigned long ret = mfn;
	if (is_running_on_xen()) {
		ret = __HYPERVISOR_machtophys(mfn);
	}
	return ret;
}

static inline unsigned long
__HYPERVISOR_zap_physmap(unsigned long gpfn, unsigned int extent_order)
{
	return _hypercall3(unsigned long, ia64_dom0vp_op,
	                   IA64_DOM0VP_zap_physmap, gpfn, extent_order);
}

static inline unsigned long
HYPERVISOR_zap_physmap(unsigned long gpfn, unsigned int extent_order)
{
	unsigned long ret = 0;
	if (is_running_on_xen()) {
		ret = __HYPERVISOR_zap_physmap(gpfn, extent_order);
	}
	return ret;
}

static inline unsigned long
__HYPERVISOR_add_physmap(unsigned long gpfn, unsigned long mfn,
			 unsigned long flags, domid_t domid)
{
	return _hypercall5(unsigned long, ia64_dom0vp_op,
	                   IA64_DOM0VP_add_physmap, gpfn, mfn, flags, domid);
}

static inline unsigned long
HYPERVISOR_add_physmap(unsigned long gpfn, unsigned long mfn,
		       unsigned long flags, domid_t domid)
{
	unsigned long ret = 0;
	BUG_ON(!is_running_on_xen());//XXX
	if (is_running_on_xen()) {
		ret = __HYPERVISOR_add_physmap(gpfn, mfn, flags, domid);
	}
	return ret;
}

// for balloon driver
#define HYPERVISOR_update_va_mapping(va, new_val, flags) (0)

/* Use xencomm to do hypercalls.  */
#ifdef MODULE
#define HYPERVISOR_sched_op xencomm_mini_hypercall_sched_op
#define HYPERVISOR_event_channel_op xencomm_mini_hypercall_event_channel_op
#define HYPERVISOR_callback_op xencomm_mini_hypercall_callback_op
#define HYPERVISOR_multicall xencomm_mini_hypercall_multicall
#define HYPERVISOR_xen_version xencomm_mini_hypercall_xen_version
#define HYPERVISOR_console_io xencomm_mini_hypercall_console_io
#define HYPERVISOR_physdev_op xencomm_mini_hypercall_physdev_op
#define HYPERVISOR_hvm_op xencomm_mini_hypercall_hvm_op
#ifdef CONFIG_VMX_GUEST
#define HYPERVISOR_memory_op 0
#else
#define HYPERVISOR_memory_op xencomm_mini_hypercall_memory_op
#endif
#else
#define HYPERVISOR_sched_op xencomm_hypercall_sched_op
#define HYPERVISOR_event_channel_op xencomm_hypercall_event_channel_op
#define HYPERVISOR_callback_op xencomm_hypercall_callback_op
#define HYPERVISOR_multicall xencomm_hypercall_multicall
#define HYPERVISOR_xen_version xencomm_hypercall_xen_version
#define HYPERVISOR_console_io xencomm_hypercall_console_io
#define HYPERVISOR_physdev_op xencomm_hypercall_physdev_op
#define HYPERVISOR_hvm_op xencomm_hypercall_hvm_op
#define HYPERVISOR_memory_op xencomm_hypercall_memory_op
#endif

#define HYPERVISOR_suspend xencomm_hypercall_suspend

#endif /* __HYPERCALL_H__ */
