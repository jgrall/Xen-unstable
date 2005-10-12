/******************************************************************************
 * dom0_ops.c
 * 
 * Process command requests from domain-0 guest OS.
 * 
 * Copyright (c) 2002, K A Fraser
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/domain.h>
#include <xen/event.h>
#include <xen/domain_page.h>
#include <xen/trace.h>
#include <xen/console.h>
#include <asm/current.h>
#include <public/dom0_ops.h>
#include <public/sched_ctl.h>
#include <acm/acm_hooks.h>

extern long arch_do_dom0_op(dom0_op_t *op, dom0_op_t *u_dom0_op);
extern void arch_getdomaininfo_ctxt(
    struct vcpu *, struct vcpu_guest_context *);

static inline int is_free_domid(domid_t dom)
{
    struct domain *d;

    if ( dom >= DOMID_FIRST_RESERVED )
        return 0;

    if ( (d = find_domain_by_id(dom)) == NULL )
        return 1;

    put_domain(d);
    return 0;
}

static void getdomaininfo(struct domain *d, dom0_getdomaininfo_t *info)
{
    struct vcpu   *v;
    u64 cpu_time = 0;
    int vcpu_count = 0;
    int flags = DOMFLAGS_BLOCKED;
    
    info->domain = d->domain_id;
    
    memset(&info->vcpu_to_cpu, -1, sizeof(info->vcpu_to_cpu));
    memset(&info->cpumap, 0, sizeof(info->cpumap));

    /* 
     * - domain is marked as blocked only if all its vcpus are blocked
     * - domain is marked as running if any of its vcpus is running
     * - only map vcpus that aren't down.  Note, at some point we may
     *   wish to demux the -1 value to indicate down vs. not-ever-booted
     */
    for_each_vcpu ( d, v ) {
        /* only map vcpus that are up */
        if ( !(test_bit(_VCPUF_down, &v->vcpu_flags)) )
            info->vcpu_to_cpu[v->vcpu_id] = v->processor;
        info->cpumap[v->vcpu_id] = v->cpumap;
        if ( !(v->vcpu_flags & VCPUF_blocked) )
            flags &= ~DOMFLAGS_BLOCKED;
        if ( v->vcpu_flags & VCPUF_running )
            flags |= DOMFLAGS_RUNNING;
        cpu_time += v->cpu_time;
        vcpu_count++;
    }
    
    info->cpu_time = cpu_time;
    info->n_vcpu = vcpu_count;
    
    info->flags = flags |
        ((d->domain_flags & DOMF_dying)      ? DOMFLAGS_DYING    : 0) |
        ((d->domain_flags & DOMF_shutdown)   ? DOMFLAGS_SHUTDOWN : 0) |
        ((d->domain_flags & DOMF_ctrl_pause) ? DOMFLAGS_PAUSED   : 0) |
        d->shutdown_code << DOMFLAGS_SHUTDOWNSHIFT;

    if (d->ssid != NULL)
        info->ssidref = ((struct acm_ssid_domain *)d->ssid)->ssidref;
    else    
        info->ssidref = ACM_DEFAULT_SSID;
    
    info->tot_pages         = d->tot_pages;
    info->max_pages         = d->max_pages;
    info->shared_info_frame = __pa(d->shared_info) >> PAGE_SHIFT;
}

long do_dom0_op(dom0_op_t *u_dom0_op)
{
    long ret = 0;
    dom0_op_t curop, *op = &curop;
    void *ssid = NULL; /* save security ptr between pre and post/fail hooks */
    static spinlock_t dom0_lock = SPIN_LOCK_UNLOCKED;

    if ( !IS_PRIV(current->domain) )
        return -EPERM;

    if ( copy_from_user(op, u_dom0_op, sizeof(*op)) )
        return -EFAULT;

    if ( op->interface_version != DOM0_INTERFACE_VERSION )
        return -EACCES;

    if ( acm_pre_dom0_op(op, &ssid) )
        return -EACCES;

    spin_lock(&dom0_lock);

    switch ( op->cmd )
    {

    case DOM0_SETDOMAININFO:
    {
        struct domain *d = find_domain_by_id(op->u.setdomaininfo.domain);
        ret = -ESRCH;
        if ( d != NULL )
        {
            ret = set_info_guest(d, &op->u.setdomaininfo);
            put_domain(d);
        }
    }
    break;

    case DOM0_PAUSEDOMAIN:
    {
        struct domain *d = find_domain_by_id(op->u.pausedomain.domain);
        ret = -ESRCH;
        if ( d != NULL )
        {
            ret = -EINVAL;
            if ( d != current->domain )
            {
                domain_pause_by_systemcontroller(d);
                ret = 0;
            }
            put_domain(d);
        }
    }
    break;

    case DOM0_UNPAUSEDOMAIN:
    {
        struct domain *d = find_domain_by_id(op->u.unpausedomain.domain);
        ret = -ESRCH;
        if ( d != NULL )
        {
            ret = -EINVAL;
            if ( (d != current->domain) && 
                 test_bit(_VCPUF_initialised, &d->vcpu[0]->vcpu_flags) )
            {
                domain_unpause_by_systemcontroller(d);
                ret = 0;
            }
            put_domain(d);
        }
    }
    break;

    case DOM0_CREATEDOMAIN:
    {
        struct domain *d;
        unsigned int   pro;
        domid_t        dom;
        struct vcpu   *v;
        unsigned int   i, cnt[NR_CPUS] = { 0 };
        static domid_t rover = 0;

        dom = op->u.createdomain.domain;
        if ( (dom > 0) && (dom < DOMID_FIRST_RESERVED) )
        {
            ret = -EINVAL;
            if ( !is_free_domid(dom) )
                break;
        }
        else
        {
            for ( dom = rover + 1; dom != rover; dom++ )
            {
                if ( dom == DOMID_FIRST_RESERVED )
                    dom = 0;
                if ( is_free_domid(dom) )
                    break;
            }

            ret = -ENOMEM;
            if ( dom == rover )
                break;

            rover = dom;
        }

        /* Do an initial CPU placement. Pick the least-populated CPU. */
        read_lock(&domlist_lock);
        for_each_domain ( d )
            for_each_vcpu ( d, v )
                cnt[v->processor]++;
        read_unlock(&domlist_lock);
        
        /*
         * If we're on a HT system, we only use the first HT for dom0, other 
         * domains will all share the second HT of each CPU. Since dom0 is on 
	     * CPU 0, we favour high numbered CPUs in the event of a tie.
         */
        pro = smp_num_siblings - 1;
        for ( i = pro; i < num_online_cpus(); i += smp_num_siblings )
            if ( cnt[i] <= cnt[pro] )
                pro = i;

        ret = -ENOMEM;
        if ( (d = do_createdomain(dom, pro)) == NULL )
            break;

        ret = 0;

        op->u.createdomain.domain = d->domain_id;
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;

    case DOM0_MAX_VCPUS:
    {
        struct domain *d;
        unsigned int i, max = op->u.max_vcpus.max;

        ret = -EINVAL;
        if ( max > MAX_VIRT_CPUS )
            break;

        ret = -ESRCH;
        if ( (d = find_domain_by_id(op->u.max_vcpus.domain)) == NULL )
            break;

        /*
         * Can only create new VCPUs while the domain is not fully constructed
         * (and hence not runnable). Xen needs auditing for races before
         * removing this check.
         */
        ret = -EINVAL;
        if ( test_bit(_VCPUF_initialised, &d->vcpu[0]->vcpu_flags) )
            goto maxvcpu_out;

        /* We cannot reduce maximum VCPUs. */
        ret = -EINVAL;
        if ( (max != MAX_VIRT_CPUS) && (d->vcpu[max] != NULL) )
            goto maxvcpu_out;

        ret = -ENOMEM;
        for ( i = 0; i < max; i++ )
            if ( (d->vcpu[i] == NULL) && (alloc_vcpu(d, i) == NULL) )
                goto maxvcpu_out;

        ret = 0;

    maxvcpu_out:
        put_domain(d);
    }
    break;

    case DOM0_DESTROYDOMAIN:
    {
        struct domain *d = find_domain_by_id(op->u.destroydomain.domain);
        ret = -ESRCH;
        if ( d != NULL )
        {
            ret = -EINVAL;
            if ( d != current->domain )
            {
                domain_kill(d);
                ret = 0;
            }
            put_domain(d);
        }
    }
    break;

    case DOM0_PINCPUDOMAIN:
    {
        domid_t dom = op->u.pincpudomain.domain;
        struct domain *d = find_domain_by_id(dom);
        struct vcpu *v;
        cpumap_t cpumap;


        if ( d == NULL )
        {
            ret = -ESRCH;            
            break;
        }
        
        if ( (op->u.pincpudomain.vcpu >= MAX_VIRT_CPUS) ||
             !d->vcpu[op->u.pincpudomain.vcpu] )
        {
            ret = -EINVAL;
            put_domain(d);
            break;
        }

        v = d->vcpu[op->u.pincpudomain.vcpu];
        if ( v == NULL )
        {
            ret = -ESRCH;
            put_domain(d);
            break;
        }

        if ( v == current )
        {
            ret = -EINVAL;
            put_domain(d);
            break;
        }

        if ( copy_from_user(&cpumap, op->u.pincpudomain.cpumap,
                            sizeof(cpumap)) )
        {
            ret = -EFAULT;
            put_domain(d);
            break;
        }

        /* update cpumap for this vcpu */
        v->cpumap = cpumap;

        if ( cpumap == CPUMAP_RUNANYWHERE )
        {
            clear_bit(_VCPUF_cpu_pinned, &v->vcpu_flags);
        }
        else
        {
            /* pick a new cpu from the usable map */
            int new_cpu = (int)find_first_set_bit(cpumap) % num_online_cpus();

            vcpu_pause(v);
            vcpu_migrate_cpu(v, new_cpu);
            set_bit(_VCPUF_cpu_pinned, &v->vcpu_flags);
            vcpu_unpause(v);
        }

        put_domain(d);
    }
    break;

    case DOM0_SCHEDCTL:
    {
        ret = sched_ctl(&op->u.schedctl);
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;

    case DOM0_ADJUSTDOM:
    {
        ret = sched_adjdom(&op->u.adjustdom);
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;

    case DOM0_GETDOMAININFO:
    { 
        struct domain *d;

        read_lock(&domlist_lock);

        for_each_domain ( d )
        {
            if ( d->domain_id >= op->u.getdomaininfo.domain )
                break;
        }

        if ( (d == NULL) || !get_domain(d) )
        {
            read_unlock(&domlist_lock);
            ret = -ESRCH;
            break;
        }

        read_unlock(&domlist_lock);

        getdomaininfo(d, &op->u.getdomaininfo);

        if ( copy_to_user(u_dom0_op, op, sizeof(*op)) )     
            ret = -EINVAL;

        put_domain(d);
    }
    break;

    case DOM0_GETDOMAININFOLIST:
    { 
        struct domain *d;
        dom0_getdomaininfo_t info;
        dom0_getdomaininfo_t *buffer = op->u.getdomaininfolist.buffer;
        u32 num_domains = 0;

        read_lock(&domlist_lock);

        for_each_domain ( d )
        {
            if ( d->domain_id < op->u.getdomaininfolist.first_domain )
                continue;
            if ( num_domains == op->u.getdomaininfolist.max_domains )
                break;
            if ( (d == NULL) || !get_domain(d) )
            {
                ret = -ESRCH;
                break;
            }

            getdomaininfo(d, &info);

            put_domain(d);

            if ( copy_to_user(buffer, &info, sizeof(dom0_getdomaininfo_t)) )
            {
                ret = -EINVAL;
                break;
            }
            
            buffer++;
            num_domains++;
        }
        
        read_unlock(&domlist_lock);
        
        if ( ret != 0 )
            break;
        
        op->u.getdomaininfolist.num_domains = num_domains;

        if ( copy_to_user(u_dom0_op, op, sizeof(*op)) )
            ret = -EINVAL;
    }
    break;

    case DOM0_GETVCPUCONTEXT:
    { 
        struct vcpu_guest_context *c;
        struct domain             *d;
        struct vcpu               *v;
        int i;

        d = find_domain_by_id(op->u.getvcpucontext.domain);
        if ( d == NULL )
        {
            ret = -ESRCH;
            break;
        }

        if ( op->u.getvcpucontext.vcpu >= MAX_VIRT_CPUS )
        {
            ret = -EINVAL;
            put_domain(d);
            break;
        }

        /* find first valid vcpu starting from request. */
        v = NULL;
        for ( i = op->u.getvcpucontext.vcpu; i < MAX_VIRT_CPUS; i++ )
        {
            v = d->vcpu[i];
            if ( v != NULL && !(test_bit(_VCPUF_down, &v->vcpu_flags)) )
                break;
        }
        
        if ( v == NULL )
        {
            ret = -ESRCH;
            put_domain(d);
            break;
        }

        op->u.getvcpucontext.cpu_time = v->cpu_time;

        if ( op->u.getvcpucontext.ctxt != NULL )
        {
            if ( (c = xmalloc(struct vcpu_guest_context)) == NULL )
            {
                ret = -ENOMEM;
                put_domain(d);
                break;
            }

            if ( v != current )
                vcpu_pause(v);

            arch_getdomaininfo_ctxt(v,c);

            if ( v != current )
                vcpu_unpause(v);

            if ( copy_to_user(op->u.getvcpucontext.ctxt, c, sizeof(*c)) )
                ret = -EINVAL;

            xfree(c);
        }

        if ( copy_to_user(u_dom0_op, op, sizeof(*op)) )     
            ret = -EINVAL;

        put_domain(d);
    }
    break;

    case DOM0_SETTIME:
    {
        do_settime(op->u.settime.secs, 
                   op->u.settime.nsecs, 
                   op->u.settime.system_time);
        ret = 0;
    }
    break;

#ifdef TRACE_BUFFER
    case DOM0_TBUFCONTROL:
    {
        ret = tb_control(&op->u.tbufcontrol);
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;
#endif
    
    case DOM0_READCONSOLE:
    {
        ret = read_console_ring(
            &op->u.readconsole.buffer, 
            &op->u.readconsole.count,
            op->u.readconsole.clear); 
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;

    case DOM0_SCHED_ID:
    {
        op->u.sched_id.sched_id = sched_id();
        copy_to_user(u_dom0_op, op, sizeof(*op));
        ret = 0;        
    }
    break;

    case DOM0_SETDOMAINMAXMEM:
    {
        struct domain *d; 
        ret = -ESRCH;
        d = find_domain_by_id(op->u.setdomainmaxmem.domain);
        if ( d != NULL )
        {
            d->max_pages = op->u.setdomainmaxmem.max_memkb >> (PAGE_SHIFT-10);
            put_domain(d);
            ret = 0;
        }
    }
    break;

#ifdef PERF_COUNTERS
    case DOM0_PERFCCONTROL:
    {
        extern int perfc_control(dom0_perfccontrol_t *);
        ret = perfc_control(&op->u.perfccontrol);
        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;
#endif

    default:
        ret = arch_do_dom0_op(op,u_dom0_op);

    }

    spin_unlock(&dom0_lock);

    if (!ret)
        acm_post_dom0_op(op, ssid);
    else
        acm_fail_dom0_op(op, ssid);

    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
