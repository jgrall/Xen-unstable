/**
 * @file init.c
 *
 * @remark Copyright 2002 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon <levon@movementarian.org>
 */

#include <linux/kernel.h>
#include <linux/oprofile.h>
#include <linux/init.h>
#include <linux/errno.h>
#include "oprofile_perfmon.h"
 
extern int perfmon_init(struct oprofile_operations * ops);
extern void perfmon_exit(void);
extern void ia64_backtrace(struct pt_regs * const regs, unsigned int depth);

int __init oprofile_arch_init(struct oprofile_operations * ops)
{
	int ret = -ENODEV;

	if (is_running_on_xen()) {
		ret = xen_perfmon_init();
		if (ret)
			return ret;
		return xenoprofile_init(ops);
	}

#ifdef CONFIG_PERFMON
	/* perfmon_init() can fail, but we have no way to report it */
	ret = perfmon_init(ops);
#endif
	ops->backtrace = ia64_backtrace;

	return ret;
}


void oprofile_arch_exit(void)
{
	if (is_running_on_xen()) {
		xenoprofile_exit();
		xen_perfmon_exit();
		return;
	}

#ifdef CONFIG_PERFMON
	perfmon_exit();
#endif
}
