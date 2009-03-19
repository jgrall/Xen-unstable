/*****************************************************************************
#  pmstat.c - Power Management statistic information (Px/Cx/Tx, etc.)
#
#  Copyright (c) 2008, Liu Jinsong <jinsong.liu@intel.com>
#
# This program is free software; you can redistribute it and/or modify it 
# under the terms of the GNU General Public License as published by the Free 
# Software Foundation; either version 2 of the License, or (at your option) 
# any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT 
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
# more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 59 
# Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#
# The full GNU General Public License is included in this distribution in the
# file called LICENSE.
#
*****************************************************************************/

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <xen/irq.h>
#include <xen/iocap.h>
#include <xen/compat.h>
#include <xen/guest_access.h>
#include <asm/current.h>
#include <public/xen.h>
#include <xen/cpumask.h>
#include <asm/processor.h>
#include <xen/percpu.h>
#include <xen/domain.h>

#include <public/sysctl.h>
#include <acpi/cpufreq/cpufreq.h>

struct pm_px *__read_mostly cpufreq_statistic_data[NR_CPUS];

extern uint32_t pmstat_get_cx_nr(uint32_t cpuid);
extern int pmstat_get_cx_stat(uint32_t cpuid, struct pm_cx_stat *stat);
extern int pmstat_reset_cx_stat(uint32_t cpuid);

extern struct list_head cpufreq_governor_list;

/*
 * Get PM statistic info
 */
int do_get_pm_info(struct xen_sysctl_get_pmstat *op)
{
    int ret = 0;
    const struct processor_pminfo *pmpt;

    if ( !op || (op->cpuid >= NR_CPUS) || !cpu_online(op->cpuid) )
        return -EINVAL;
    pmpt = processor_pminfo[op->cpuid];

    switch ( op->type & PMSTAT_CATEGORY_MASK )
    {
    case PMSTAT_CX:
        if ( !(xen_processor_pmbits & XEN_PROCESSOR_PM_CX) )
            return -ENODEV;
        break;
    case PMSTAT_PX:
        if ( !(xen_processor_pmbits & XEN_PROCESSOR_PM_PX) )
            return -ENODEV;
        if ( !pmpt || !(pmpt->perf.init & XEN_PX_INIT) )
            return -EINVAL;
        break;
    default:
        return -ENODEV;
    }

    switch ( op->type )
    {
    case PMSTAT_get_max_px:
    {
        op->u.getpx.total = pmpt->perf.state_count;
        break;
    }

    case PMSTAT_get_pxstat:
    {
        uint32_t ct;
        struct pm_px *pxpt = cpufreq_statistic_data[op->cpuid];
        spinlock_t *cpufreq_statistic_lock = 
                   &per_cpu(cpufreq_statistic_lock, op->cpuid);

        spin_lock(cpufreq_statistic_lock);

        if ( !pxpt || !pxpt->u.pt || !pxpt->u.trans_pt )
        {
            spin_unlock(cpufreq_statistic_lock);
            return -ENODATA;
        }

        pxpt->u.usable = pmpt->perf.state_count - pmpt->perf.platform_limit;

        cpufreq_residency_update(op->cpuid, pxpt->u.cur);

        ct = pmpt->perf.state_count;
        if ( copy_to_guest(op->u.getpx.trans_pt, pxpt->u.trans_pt, ct*ct) )
        {
            spin_unlock(cpufreq_statistic_lock);
            ret = -EFAULT;
            break;
        }

        if ( copy_to_guest(op->u.getpx.pt, pxpt->u.pt, ct) )
        {
            spin_unlock(cpufreq_statistic_lock);
            ret = -EFAULT;
            break;
        }

        op->u.getpx.total = pxpt->u.total;
        op->u.getpx.usable = pxpt->u.usable;
        op->u.getpx.last = pxpt->u.last;
        op->u.getpx.cur = pxpt->u.cur;

        spin_unlock(cpufreq_statistic_lock);

        break;
    }

    case PMSTAT_reset_pxstat:
    {
        cpufreq_statistic_reset(op->cpuid);
        break;
    }

#ifdef CONFIG_X86
    case PMSTAT_get_max_cx:
    {
        op->u.getcx.nr = pmstat_get_cx_nr(op->cpuid);
        ret = 0;
        break;
    }

    case PMSTAT_get_cxstat:
    {
        ret = pmstat_get_cx_stat(op->cpuid, &op->u.getcx);
        break;
    }

    case PMSTAT_reset_cxstat:
    {
        ret = pmstat_reset_cx_stat(op->cpuid);
        break;
    }
#endif

    default:
        printk("not defined sub-hypercall @ do_get_pm_info\n");
        ret = -ENOSYS;
        break;
    }

    return ret;
}

/*
 * 1. Get PM parameter
 * 2. Provide user PM control
 */
static int read_scaling_available_governors(char *scaling_available_governors,
                                            unsigned int size)
{
    unsigned int i = 0;
    struct cpufreq_governor *t;

    if ( !scaling_available_governors )
        return -EINVAL;

    list_for_each_entry(t, &cpufreq_governor_list, governor_list)
    {
        i += scnprintf(&scaling_available_governors[i],
                       CPUFREQ_NAME_LEN, "%s ", t->name);
        if ( i > size )
            return -EINVAL;
    }
    scaling_available_governors[i-1] = '\0';

    return 0;
}

static int get_cpufreq_para(struct xen_sysctl_pm_op *op)
{
    uint32_t ret = 0;
    const struct processor_pminfo *pmpt;
    struct cpufreq_policy *policy;
    uint32_t gov_num = 0;
    uint32_t *affected_cpus;
    uint32_t *scaling_available_frequencies;
    char     *scaling_available_governors;
    struct list_head *pos;
    uint32_t cpu, i, j = 0;

    if ( !op || !cpu_online(op->cpuid) )
        return -EINVAL;
    pmpt = processor_pminfo[op->cpuid];
    policy = cpufreq_cpu_policy[op->cpuid];

    if ( !pmpt || !pmpt->perf.states ||
         !policy || !policy->governor )
        return -EINVAL;

    list_for_each(pos, &cpufreq_governor_list)
        gov_num++;

    if ( (op->get_para.cpu_num  != cpus_weight(policy->cpus)) ||
         (op->get_para.freq_num != pmpt->perf.state_count)    ||
         (op->get_para.gov_num  != gov_num) )
    {
        op->get_para.cpu_num =  cpus_weight(policy->cpus);
        op->get_para.freq_num = pmpt->perf.state_count;
        op->get_para.gov_num  = gov_num;
        return -EAGAIN;
    }

    if ( !(affected_cpus = xmalloc_array(uint32_t, op->get_para.cpu_num)) )
        return -ENOMEM;
    memset(affected_cpus, 0, op->get_para.cpu_num * sizeof(uint32_t));
    for_each_cpu_mask(cpu, policy->cpus)
        affected_cpus[j++] = cpu;
    ret = copy_to_guest(op->get_para.affected_cpus,
                       affected_cpus, op->get_para.cpu_num);
    xfree(affected_cpus);
    if ( ret )
        return ret;

    if ( !(scaling_available_frequencies =
        xmalloc_array(uint32_t, op->get_para.freq_num)) )
        return -ENOMEM;
    memset(scaling_available_frequencies, 0,
           op->get_para.freq_num * sizeof(uint32_t));
    for ( i = 0; i < op->get_para.freq_num; i++ )
        scaling_available_frequencies[i] =
                        pmpt->perf.states[i].core_frequency * 1000;
    ret = copy_to_guest(op->get_para.scaling_available_frequencies,
                   scaling_available_frequencies, op->get_para.freq_num);
    xfree(scaling_available_frequencies);
    if ( ret )
        return ret;

    if ( !(scaling_available_governors =
        xmalloc_array(char, gov_num * CPUFREQ_NAME_LEN)) )
        return -ENOMEM;
    memset(scaling_available_governors, 0,
                gov_num * CPUFREQ_NAME_LEN * sizeof(char));
    if ( (ret = read_scaling_available_governors(scaling_available_governors,
                gov_num * CPUFREQ_NAME_LEN * sizeof(char))) )
    {
        xfree(scaling_available_governors);
        return ret;
    }
    ret = copy_to_guest(op->get_para.scaling_available_governors,
                scaling_available_governors, gov_num * CPUFREQ_NAME_LEN);
    xfree(scaling_available_governors);
    if ( ret )
        return ret;

    op->get_para.cpuinfo_cur_freq =
        cpufreq_driver->get ? cpufreq_driver->get(op->cpuid) : policy->cur;
    op->get_para.cpuinfo_max_freq = policy->cpuinfo.max_freq;
    op->get_para.cpuinfo_min_freq = policy->cpuinfo.min_freq;
    op->get_para.scaling_cur_freq = policy->cur;
    op->get_para.scaling_max_freq = policy->max;
    op->get_para.scaling_min_freq = policy->min;

    if ( cpufreq_driver->name )
        strlcpy(op->get_para.scaling_driver, 
            cpufreq_driver->name, CPUFREQ_NAME_LEN);
    else
        strlcpy(op->get_para.scaling_driver, "Unknown", CPUFREQ_NAME_LEN);

    if ( policy->governor->name )
        strlcpy(op->get_para.scaling_governor, 
            policy->governor->name, CPUFREQ_NAME_LEN);
    else
        strlcpy(op->get_para.scaling_governor, "Unknown", CPUFREQ_NAME_LEN);

    /* governor specific para */
    if ( !strnicmp(op->get_para.scaling_governor, 
                   "userspace", CPUFREQ_NAME_LEN) )
    {
        op->get_para.u.userspace.scaling_setspeed = policy->cur;
    }

    if ( !strnicmp(op->get_para.scaling_governor, 
                   "ondemand", CPUFREQ_NAME_LEN) )
    {
        ret = get_cpufreq_ondemand_para(
            &op->get_para.u.ondemand.sampling_rate_max,
            &op->get_para.u.ondemand.sampling_rate_min,
            &op->get_para.u.ondemand.sampling_rate,
            &op->get_para.u.ondemand.up_threshold); 
    }

    return ret;
}

static int set_cpufreq_gov(struct xen_sysctl_pm_op *op)
{
    struct cpufreq_policy new_policy, *old_policy;

    if ( !op || !cpu_online(op->cpuid) )
        return -EINVAL;

    old_policy = cpufreq_cpu_policy[op->cpuid];
    if ( !old_policy )
        return -EINVAL;

    memcpy(&new_policy, old_policy, sizeof(struct cpufreq_policy));

    new_policy.governor = __find_governor(op->set_gov.scaling_governor);
    if (new_policy.governor == NULL)
        return -EINVAL;

    return __cpufreq_set_policy(old_policy, &new_policy);
}

static int set_cpufreq_para(struct xen_sysctl_pm_op *op)
{
    int ret = 0;
    struct cpufreq_policy *policy;

    if ( !op || !cpu_online(op->cpuid) )
        return -EINVAL;
    policy = cpufreq_cpu_policy[op->cpuid];

    if ( !policy || !policy->governor )
        return -EINVAL;

    switch(op->set_para.ctrl_type)
    {
    case SCALING_MAX_FREQ:
    {
        struct cpufreq_policy new_policy;

        memcpy(&new_policy, policy, sizeof(struct cpufreq_policy));
        new_policy.max = op->set_para.ctrl_value;
        ret = __cpufreq_set_policy(policy, &new_policy);

        break;
    }

    case SCALING_MIN_FREQ:
    {
        struct cpufreq_policy new_policy;

        memcpy(&new_policy, policy, sizeof(struct cpufreq_policy));
        new_policy.min = op->set_para.ctrl_value;
        ret = __cpufreq_set_policy(policy, &new_policy);

        break;
    }

    case SCALING_SETSPEED:
    {
        unsigned int freq =op->set_para.ctrl_value;

        if ( !strnicmp(policy->governor->name,
                       "userspace", CPUFREQ_NAME_LEN) )
            ret = write_userspace_scaling_setspeed(op->cpuid, freq);
        else
            ret = -EINVAL;

        break;
    }

    case SAMPLING_RATE:
    {
        unsigned int sampling_rate = op->set_para.ctrl_value;

        if ( !strnicmp(policy->governor->name,
                       "ondemand", CPUFREQ_NAME_LEN) )
            ret = write_ondemand_sampling_rate(sampling_rate);
        else
            ret = -EINVAL;

        break;
    }

    case UP_THRESHOLD:
    {
        unsigned int up_threshold = op->set_para.ctrl_value;

        if ( !strnicmp(policy->governor->name,
                       "ondemand", CPUFREQ_NAME_LEN) )
            ret = write_ondemand_up_threshold(up_threshold);
        else
            ret = -EINVAL;

        break;
    }

    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int get_cputopo (struct xen_sysctl_pm_op *op)
{
    uint32_t i, nr_cpus;
    XEN_GUEST_HANDLE_64(uint32) cpu_to_core_arr;
    XEN_GUEST_HANDLE_64(uint32) cpu_to_socket_arr;
    int arr_size, ret=0;

    cpu_to_core_arr = op->get_topo.cpu_to_core;
    cpu_to_socket_arr = op->get_topo.cpu_to_socket;
    arr_size= min_t(uint32_t, op->get_topo.max_cpus, NR_CPUS);

    if ( guest_handle_is_null( cpu_to_core_arr ) ||
            guest_handle_is_null(  cpu_to_socket_arr) )
    {
        ret = -EINVAL;
        goto out;
    }

    nr_cpus = 0;
    for ( i = 0; i < arr_size; i++ )
    {
        uint32_t core, socket;
        if ( cpu_online(i) )
        {
            core = cpu_to_core(i);
            socket = cpu_to_socket(i);
            nr_cpus = i;
        }
        else
        {
            core = socket = INVALID_TOPOLOGY_ID;
        }

        if ( copy_to_guest_offset(cpu_to_core_arr, i, &core, 1) ||
                copy_to_guest_offset(cpu_to_socket_arr, i, &socket, 1))
        {
            ret = -EFAULT;
            goto out;
        }
    }

    op->get_topo.nr_cpus = nr_cpus + 1;
out:
    return ret;
}

int do_pm_op(struct xen_sysctl_pm_op *op)
{
    int ret = 0;
    const struct processor_pminfo *pmpt;

    if ( !op || !cpu_online(op->cpuid) )
        return -EINVAL;
    pmpt = processor_pminfo[op->cpuid];

    switch ( op->cmd & PM_PARA_CATEGORY_MASK )
    {
    case CPUFREQ_PARA:
        if ( !(xen_processor_pmbits & XEN_PROCESSOR_PM_PX) )
            return -ENODEV;
        if ( !pmpt || !(pmpt->perf.init & XEN_PX_INIT) )
            return -EINVAL;
        break;
    }

    switch ( op->cmd )
    {
    case GET_CPUFREQ_PARA:
    {
        ret = get_cpufreq_para(op);
        break;
    }

    case SET_CPUFREQ_GOV:
    {
        ret = set_cpufreq_gov(op);
        break;
    }

    case SET_CPUFREQ_PARA:
    {
        ret = set_cpufreq_para(op);
        break;
    }

    case XEN_SYSCTL_pm_op_get_cputopo:
    {
        ret = get_cputopo(op);
        break;
    }

    default:
        printk("not defined sub-hypercall @ do_pm_op\n");
        ret = -ENOSYS;
        break;
    }

    return ret;
}
