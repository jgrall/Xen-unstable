/*
 *	atropos.c
 *	---------
 *
 * Copyright (c) 1994 University of Cambridge Computer Laboratory.
 * This is part of Nemesis; consult your contract for terms and conditions.
 *
 * ID : $Id: atropos.c 1.1 Tue, 13 Apr 1999 13:30:49 +0100 dr10009 $
 *
 * This is the "atropos" CPU scheduler. 
 */

/* Ported to Xen's generic scheduler interface by Mark Williamson
 * these modifications are (C) 2004 Intel Research Cambridge
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/time.h>
#include <xen/sched.h>
#include <xen/sched-if.h>
#include <hypervisor-ifs/sched_ctl.h>
#include <xen/trace.h>

/*
 * KAF -- Atropos is broken by the new scheduler interfaces.
 * It'll need fixing to get rid of use of ATROPOS_TASK__*
 */
#ifdef KAF_KILLED

#define ATROPOS_TASK_UNBLOCKED 16
#define ATROPOS_TASK_WAIT      32

#define Activation_Reason_Allocated 1
#define Activation_Reason_Preempted 2
#define Activation_Reason_Extra     3

/* Atropos-specific per-domain data */
struct at_dom_info
{
    /* MAW Xen additions */
    struct domain *owner;      /* the domain this data belongs to */
    struct list_head waitq;    /* wait queue                           */
    int reason;                /* reason domain was last scheduled     */

    /* (what remains of) the original fields */

    s_time_t     deadline;       /* Next deadline                        */
    s_time_t     prevddln;       /* Previous deadline                    */
    
    s_time_t     remain;         /* Time remaining this period           */
    s_time_t     period;         /* Current period of time allocation    */
    s_time_t     nat_period;     /* Natural period                       */
    s_time_t     slice;          /* Current length of allocation         */
    s_time_t     nat_slice;      /* Natural length of allocation         */
    s_time_t     latency;        /* Unblocking latency                   */

    int          xtratime;       /* Prepared to accept extra time?       */
};

/* Atropos-specific per-CPU data */
struct at_cpu_info
{
    struct list_head waitq; /* wait queue*/
};


#define DOM_INFO(_p) ((struct at_dom_info *)((_p)->sched_priv))
#define CPU_INFO(_c) ((struct at_cpu_info *)((_c).sched_priv))
#define WAITQ(cpu)   (&CPU_INFO(schedule_data[cpu])->waitq)
#define RUNQ(cpu)    (&schedule_data[cpu].runqueue)

#define BESTEFFORT_QUANTUM MILLISECS(5)


/* SLAB cache for struct at_dom_info objects */
static kmem_cache_t *dom_info_cache;


/** calculate the length of a linked list */
static int q_len(struct list_head *q) 
{
    int i = 0;
    struct list_head *tmp;
    list_for_each(tmp, q) i++;
    return i;
}


/** waitq_el - get the domain that owns a wait queue list element */
static inline struct domain *waitq_el(struct list_head *l)
{
    struct at_dom_info *inf;
    inf = list_entry(l, struct at_dom_info, waitq);
    return inf->owner;
}


/*
 * requeue
 *
 * Places the specified domain on the appropriate queue.
 * The wait queue is ordered by the time at which the domain
 * will receive more CPU time.  If a domain has no guaranteed time
 * left then the domain will be placed on the WAIT queue until
 * its next period. 
 *
 * Note that domains can be on the wait queue with remain > 0 
 * as a result of being blocked for a short time.
 * These are scheduled in preference to domains with remain < 0 
 * in an attempt to improve interactive performance.
 */
static void requeue(struct domain *sdom)
{
    struct at_dom_info *inf = DOM_INFO(sdom);
    struct list_head *prev = WAITQ(sdom->processor);
    struct list_head *next;

    if(sdom->state == ATROPOS_TASK_WAIT ||
       sdom->state == ATROPOS_TASK_UNBLOCKED )
    {
        /* insert into ordered wait queue */

        prev = WAITQ(sdom->processor);
        list_for_each(next, WAITQ(sdom->processor))
        {
            struct at_dom_info *i = 
                list_entry(next, struct at_dom_info, waitq);
            if ( i->deadline > inf->deadline )
            {
                __list_add(&inf->waitq, prev, next);
                break;
            }

            prev = next;
        }

        /* put the domain on the end of the list if it hasn't been put
         * elsewhere */
        if ( next == WAITQ(sdom->processor) )
            list_add_tail(&inf->waitq, WAITQ(sdom->processor));
    }
    else if ( domain_runnable(sdom) )
    {
        /* insert into ordered run queue */
        prev = RUNQ(sdom->processor);

        list_for_each(next, RUNQ(sdom->processor))
        {
            struct domain *p = list_entry(next, struct domain,
                                               run_list);

            if( DOM_INFO(p)->deadline > inf->deadline || is_idle_task(p) )
            {
                __list_add(&sdom->run_list, prev, next);
                break;
            }

            prev = next;
        }

        if ( next == RUNQ(sdom->processor) )
            list_add_tail(&sdom->run_list, RUNQ(sdom->processor));
    }
    /* silently ignore tasks in other states like BLOCKED, DYING, STOPPED, etc
     * - they shouldn't be on any queue */
}

/* prepare a task to be added to scheduling */
static void at_add_task(struct domain *p)
{
    s_time_t now = NOW();

    ASSERT( p->sched_priv != NULL );

    DOM_INFO(p)->owner = p;
    p->lastschd = now;
 
    /* DOM 0's parameters must be set here for it to boot the system! */
    if(p->domain == 0)
    {
        DOM_INFO(p)->remain = MILLISECS(15);
        DOM_INFO(p)->nat_period =
            DOM_INFO(p)->period = MILLISECS(20);
        DOM_INFO(p)->nat_slice =
            DOM_INFO(p)->slice = MILLISECS(15);
        DOM_INFO(p)->latency = MILLISECS(5);
        DOM_INFO(p)->xtratime = 1;
        DOM_INFO(p)->deadline = now;
        DOM_INFO(p)->prevddln = now;
    }
    else /* other domains run basically best effort unless otherwise set */
    {
        DOM_INFO(p)->remain = 0;
        DOM_INFO(p)->nat_period =
            DOM_INFO(p)->period = SECONDS(10);
        DOM_INFO(p)->nat_slice =
            DOM_INFO(p)->slice  = MILLISECS(10);
        DOM_INFO(p)->latency = SECONDS(10);
        DOM_INFO(p)->xtratime = 1;
        DOM_INFO(p)->deadline = now + SECONDS(10);
        DOM_INFO(p)->prevddln = 0;
    }

    INIT_LIST_HEAD(&(DOM_INFO(p)->waitq));
}


/**
 * dequeue - remove a domain from any queues it is on.
 * @sdom:    the task to remove
 */
static void dequeue(struct domain *sdom)
{
    struct at_dom_info *inf = DOM_INFO(sdom);

    ASSERT(sdom->domain != IDLE_DOMAIN_ID);
    
    /* just delete it from all the queues! */
    list_del(&inf->waitq);
    INIT_LIST_HEAD(&inf->waitq);
    
    if(__task_on_runqueue(sdom))
        __del_from_runqueue(sdom);

    sdom->run_list.next = NULL;
    sdom->run_list.prev = NULL;

}


/*
 * unblock
 *
 * This function deals with updating the sdom for a domain
 * which has just been unblocked.  
 *
 * Xen's Atropos treats unblocking slightly differently to Nemesis:
 *
 * - "Short blocking" domains (i.e. that unblock before their deadline has
 *  expired) are treated the same as in nemesis (put on the wait queue and
 *  given preferential treatment in selecting domains for extra time).
 *
 * - "Long blocking" domains do not simply have their period truncated to their
 *  unblocking latency as before but also have their slice recomputed to be the
 *  same fraction of their new period.  Each time the domain is scheduled, the
 *  period and slice are doubled until they reach their original ("natural")
 *  values, as set by the user (and stored in nat_period and nat_slice).  The
 *  idea is to give better response times to unblocking whilst preserving QoS
 *  guarantees to other domains.
 */
static void unblock(struct domain *sdom)
{
    s_time_t time = NOW();
    struct at_dom_info *inf = DOM_INFO(sdom);
    
    dequeue(sdom);

    /* We distinguish two cases... short and long blocks */

    if ( inf->deadline < time )
    {
        /* Long blocking case */

	/* The sdom has passed its deadline since it was blocked. 
	   Give it its new deadline based on the latency value. */
	inf->prevddln = time;

        /* Scale the scheduling parameters as requested by the latency hint. */
	inf->deadline = time + inf->latency;
        inf->slice = inf->nat_slice / ( inf->nat_period / inf->latency );
        inf->period = inf->latency;
	inf->remain = inf->slice;
    }
    else
    {
        /* Short blocking case */

	/* We leave REMAIN intact, but put this domain on the WAIT
	   queue marked as recently unblocked.  It will be given
	   priority over other domains on the wait queue until while
	   REMAIN>0 in a generous attempt to help it make up for its
	   own foolishness. */
	if(inf->remain > 0)
            sdom->state = ATROPOS_TASK_UNBLOCKED;
        else
            sdom->state = ATROPOS_TASK_WAIT;
    }

    requeue(sdom);

}

/**
 * ATROPOS - main scheduler function
 */
task_slice_t ksched_scheduler(s_time_t time)
{
    struct domain	*cur_sdom = current;  /* Current sdom           */
    s_time_t     newtime;
    s_time_t      ranfor;	        /* How long the domain ran      */
    struct domain	*sdom;	        /* tmp. scheduling domain	*/
    int   reason;                       /* reason for reschedule        */
    int cpu = cur_sdom->processor;      /* current CPU                  */
    struct at_dom_info *cur_info;
    static unsigned long waitq_rrobin = 0;
    int i;
    task_slice_t ret;

    cur_info = DOM_INFO(cur_sdom);

    ASSERT( cur_sdom != NULL);

    /* If we were spinning in the idle loop, there is no current
     * domain to deschedule. */
    if (is_idle_task(cur_sdom))
	goto deschedule_done;

    /*****************************
     * 
     * Deschedule the current scheduling domain
     *
     ****************************/

   /* Record the time the domain was preempted and for how long it
       ran.  Work out if the domain is going to be blocked to save
       some pointless queue shuffling */
    cur_sdom->lastdeschd = time;

    ranfor = (time - cur_sdom->lastschd);

    dequeue(cur_sdom);

    if ( domain_runnable(cur_sdom) || 
         (cur_sdom->state == ATROPOS_TASK_UNBLOCKED) )
    {

	/* In this block, we are doing accounting for an sdom which has 
	   been running in contracted time.  Note that this could now happen
	   even if the domain is on the wait queue (i.e. if it blocked) */

	/* Deduct guaranteed time from the domain */
	cur_info->remain  -= ranfor;

	/* If guaranteed time has run out... */
	if ( cur_info->remain <= 0 )
        {
	    /* Move domain to correct position in WAIT queue */
            /* XXX sdom_unblocked doesn't need this since it is 
               already in the correct place. */
	    cur_sdom->state = ATROPOS_TASK_WAIT;
	}
    }

    requeue(cur_sdom);

  deschedule_done:

    /*****************************
     * 
     * We have now successfully descheduled the current sdom.
     * The next task is the allocate CPU time to any sdom it is due to.
     *
       ****************************/
    cur_sdom = NULL;

    /*****************************
     * 
     * Allocate CPU time to any waiting domains who have passed their
     * period deadline.  If necessary, move them to run queue.
     *
     ****************************/
    while(!list_empty(WAITQ(cpu)) && 
	  DOM_INFO(sdom = waitq_el(WAITQ(cpu)->next))->deadline <= time ) {

	struct at_dom_info *inf = DOM_INFO(sdom);

        dequeue(sdom);

        if ( inf->period != inf->nat_period )
        {
            /* This domain has had its parameters adjusted as a result of
             * unblocking and they need to be adjusted before requeuing it */
            inf->slice  *= 2;
            inf->period *= 2;
            
            if ( inf->period > inf->nat_period )
            {
                inf->period = inf->nat_period;
                inf->slice  = inf->nat_slice;
            }
        }

	/* Domain begins a new period and receives a slice of CPU 
	 * If this domain has been blocking then throw away the
	 * rest of it's remain - it can't be trusted */
	if (inf->remain > 0) 
	    inf->remain = inf->slice;
    	else 
	    inf->remain += inf->slice;

	inf->prevddln = inf->deadline;
	inf->deadline += inf->period;

        if ( inf->remain <= 0 )
            sdom->state = ATROPOS_TASK_WAIT;

	/* Place on the appropriate queue */
	requeue(sdom);
    }

    /*****************************
     * 
     * Next we need to pick an sdom to run.
     * If anything is actually 'runnable', we run that. 
     * If nothing is, we pick a waiting sdom to run optimistically.
     * If there aren't even any of those, we have to spin waiting for an
     * event or a suitable time condition to happen.
     *
     ****************************/
    
    /* we guarantee there's always something on the runqueue */
    cur_sdom = list_entry(RUNQ(cpu)->next,
                          struct domain, run_list);

    cur_info = DOM_INFO(cur_sdom);
    newtime = time + cur_info->remain;
    reason  = (cur_info->prevddln > cur_sdom->lastschd) ?
      Activation_Reason_Allocated : Activation_Reason_Preempted;

    /* MAW - the idle domain is always on the run queue.  We run from the
     * runqueue if it's NOT the idle domain or if there's nothing on the wait
     * queue */
    if (cur_sdom->domain == IDLE_DOMAIN_ID && !list_empty(WAITQ(cpu)))
    {
        struct list_head *item;

	/* Try running a domain on the WAIT queue - this part of the
	   scheduler isn't particularly efficient but then again, we
	   don't have any guaranteed domains to worry about. */
	
	/* See if there are any unblocked domains on the WAIT
	   queue who we can give preferential treatment to. */
        list_for_each(item, WAITQ(cpu))
        {
            struct at_dom_info *inf =
                list_entry(item, struct at_dom_info, waitq);

            sdom = inf->owner;
            
	    if (sdom->state == ATROPOS_TASK_UNBLOCKED) {
		cur_sdom = sdom;
		cur_info  = inf;
		newtime  = time + inf->remain;
		reason   = Activation_Reason_Preempted;
		goto found;
	    }
	}

        /* init values needed to approximate round-robin for slack time */
        i = 0;
        if ( waitq_rrobin >= q_len(WAITQ(cpu)))
            waitq_rrobin = 0;
        
	/* Last chance: pick a domain on the wait queue with the XTRA
	   flag set.  The NEXT_OPTM field is used to cheaply achieve
	   an approximation of round-robin order */
        list_for_each(item, WAITQ(cpu))
        {
            struct at_dom_info *inf =
                list_entry(item, struct at_dom_info, waitq);
            
            sdom = inf->owner;
            
            if (inf->xtratime && i >= waitq_rrobin) {
                cur_sdom = sdom;
                cur_info  = inf;
                newtime = time + BESTEFFORT_QUANTUM;
                reason  = Activation_Reason_Extra;
                waitq_rrobin = i + 1; /* set this value ready for next */
                goto found;
            }
            
            i++;
        }
    }

    found:
    /**********************
     * 
     * We now have to work out the time when we next need to
     * make a scheduling decision.  We set the alarm timer
     * to cause an interrupt at that time.
     *
     **********************/

#define MIN(x,y) ( ( x < y ) ? x : y )
#define MAX(x,y) ( ( x > y ) ? x : y )

    /* If we might be able to run a waiting domain before this one has */
    /* exhausted its time, cut short the time allocation */
    if (!list_empty(WAITQ(cpu)))
    {
	newtime = MIN(newtime,
                      DOM_INFO(waitq_el(WAITQ(cpu)->next))->deadline);
    }

    /* don't allow pointlessly small time slices */
    newtime = MAX(newtime, time + BESTEFFORT_QUANTUM);
    
    ret.task = cur_sdom;
    ret.time = newtime - time;

    cur_sdom->min_slice = newtime - time;
    DOM_INFO(cur_sdom)->reason = reason;

    TRACE_1D(0, cur_sdom->domain);
 
    return ret;
}


/* set up some private data structures */
static int at_init_scheduler()
{
    int i;
    
    for ( i = 0; i < NR_CPUS; i++ )
    {
        schedule_data[i].sched_priv = kmalloc(sizeof(struct at_cpu_info));
        if ( schedule_data[i].sched_priv == NULL )
            return -1;
        WAITQ(i)->next = WAITQ(i);
        WAITQ(i)->prev = WAITQ(i);
    }

    dom_info_cache = kmem_cache_create("Atropos dom info",
                                       sizeof(struct at_dom_info),
                                       0, 0, NULL, NULL);

    return 0;
}

/* dump relevant per-cpu state for a run queue dump */
static void at_dump_cpu_state(int cpu)
{
    printk("Waitq len: %d Runq len: %d ",
           q_len(WAITQ(cpu)),
           q_len(RUNQ(cpu)));
}

/* print relevant per-domain info for a run queue dump */
static void at_dump_runq_el(struct domain *p)
{
    printk("lastschd = %llu, xtratime = %d ",
           p->lastschd, DOM_INFO(p)->xtratime);
}


/* set or fetch domain scheduling parameters */
static int at_adjdom(struct domain *p, struct sched_adjdom_cmd *cmd)
{
    if ( cmd->direction == SCHED_INFO_PUT )
    {
        /* sanity checking! */
        if( cmd->u.atropos.latency > cmd->u.atropos.nat_period
            || cmd->u.atropos.latency == 0
            || cmd->u.atropos.nat_slice > cmd->u.atropos.nat_period )
            return -EINVAL;

        DOM_INFO(p)->nat_period   = cmd->u.atropos.nat_period;
        DOM_INFO(p)->nat_slice    = cmd->u.atropos.nat_slice;
        DOM_INFO(p)->latency  = cmd->u.atropos.latency;
        DOM_INFO(p)->xtratime = !!cmd->u.atropos.xtratime;
    }
    else if ( cmd->direction == SCHED_INFO_GET )
    {
        cmd->u.atropos.nat_period   = DOM_INFO(p)->nat_period;
        cmd->u.atropos.nat_slice    = DOM_INFO(p)->nat_slice;
        cmd->u.atropos.latency  = DOM_INFO(p)->latency;
        cmd->u.atropos.xtratime = DOM_INFO(p)->xtratime;
    }

    return 0;
}


/** at_alloc_task - allocate private info for a task */
static int at_alloc_task(struct domain *p)
{
    ASSERT(p != NULL);

    p->sched_priv = kmem_cache_alloc(dom_info_cache);
    if( p->sched_priv == NULL )
        return -1;

    memset(p->sched_priv, 0, sizeof(struct at_dom_info));

    return 0;
}


/* free memory associated with a task */
static void at_free_task(struct domain *p)
{
    kmem_cache_free( dom_info_cache, DOM_INFO(p) );
}


/* print decoded domain private state value (if known) */
static int at_prn_state(int state)
{
    int ret = 0;
    
    switch(state)
    {
    case ATROPOS_TASK_UNBLOCKED:
        printk("Unblocked");
        break;
    case ATROPOS_TASK_WAIT:
        printk("Wait");
        break;
    default:
        ret = -1;
    }

    return ret;
}
    
#endif /* KAF_KILLED */

struct scheduler sched_atropos_def = {
    .name           = "Atropos Soft Real Time Scheduler",
    .opt_name       = "atropos",
    .sched_id       = SCHED_ATROPOS,
#ifdef KAF_KILLED
    .init_scheduler = at_init_scheduler,
    .alloc_task     = at_alloc_task,
    .add_task       = at_add_task,
    .free_task      = at_free_task,
    .wake_up        = unblock,
    .do_schedule    = ksched_scheduler,
    .adjdom         = at_adjdom,
    .dump_cpu_state = at_dump_cpu_state,
    .dump_runq_el   = at_dump_runq_el,
    .prn_state      = at_prn_state,
#endif /* KAF_KILLED */
};
