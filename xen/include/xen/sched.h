
#ifndef __SCHED_H__
#define __SCHED_H__

#include <xen/config.h>
#include <xen/types.h>
#include <xen/spinlock.h>
#include <xen/smp.h>
#include <public/xen.h>
#include <public/dom0_ops.h>
#include <xen/time.h>
#include <xen/ac_timer.h>
#include <xen/grant_table.h>
#include <asm/domain.h>

extern unsigned long volatile jiffies;
extern rwlock_t domlist_lock;

/* A global pointer to the initial domain (DOM0). */
extern struct domain *dom0;

#define MAX_EVTCHNS        1024
#define EVTCHNS_PER_BUCKET 128
#define NR_EVTCHN_BUCKETS  (MAX_EVTCHNS / EVTCHNS_PER_BUCKET)

struct evtchn
{
#define ECS_FREE         0 /* Channel is available for use.                  */
#define ECS_RESERVED     1 /* Channel is reserved.                           */
#define ECS_UNBOUND      2 /* Channel is waiting to bind to a remote domain. */
#define ECS_INTERDOMAIN  3 /* Channel is bound to another domain.            */
#define ECS_PIRQ         4 /* Channel is bound to a physical IRQ line.       */
#define ECS_VIRQ         5 /* Channel is bound to a virtual IRQ line.        */
#define ECS_IPI          6 /* Channel is bound to a virtual IPI line.        */
    u16 state;             /* ECS_* */
    u16 notify_vcpu_id;    /* VCPU for local delivery notification */
    union {
        struct {
            domid_t remote_domid;
        } unbound;     /* state == ECS_UNBOUND */
        struct {
            u16            remote_port;
            struct domain *remote_dom;
        } interdomain; /* state == ECS_INTERDOMAIN */
        u16 pirq;      /* state == ECS_PIRQ */
        u16 virq;      /* state == ECS_VIRQ */
    } u;
};

int  evtchn_init(struct domain *d);
void evtchn_destroy(struct domain *d);

#define CPUMAP_RUNANYWHERE 0xFFFFFFFF

struct vcpu 
{
    int              vcpu_id;

    int              processor;

    vcpu_info_t     *vcpu_info;

    struct domain   *domain;
    struct vcpu *next_in_list;

    struct ac_timer  timer;         /* one-shot timer for timeout values */
    unsigned long    sleep_tick;    /* tick at which this vcpu started sleep */

    s_time_t         lastschd;      /* time this domain was last scheduled */
    s_time_t         lastdeschd;    /* time this domain was last descheduled */
    s_time_t         cpu_time;      /* total CPU time received till now */
    s_time_t         wokenup;       /* time domain got woken up */
    void            *sched_priv;    /* scheduler-specific data */

    unsigned long    vcpu_flags;

    u16              virq_to_evtchn[NR_VIRQS];

    atomic_t         pausecnt;

    cpumap_t         cpumap;        /* which cpus this domain can run on */

    struct arch_vcpu arch;
};

/* Per-domain lock can be recursively acquired in fault handlers. */
#define LOCK_BIGLOCK(_d) spin_lock_recursive(&(_d)->big_lock)
#define UNLOCK_BIGLOCK(_d) spin_unlock_recursive(&(_d)->big_lock)

struct domain
{
    domid_t          domain_id;

    shared_info_t   *shared_info;     /* shared data area */
    spinlock_t       time_lock;

    spinlock_t       big_lock;

    spinlock_t       page_alloc_lock; /* protects all the following fields  */
    struct list_head page_list;       /* linked list, of size tot_pages     */
    struct list_head xenpage_list;    /* linked list, of size xenheap_pages */
    unsigned int     tot_pages;       /* number of pages currently possesed */
    unsigned int     max_pages;       /* maximum value for tot_pages        */
    unsigned int     next_io_page;    /* next io pfn to give to domain      */
    unsigned int     xenheap_pages;   /* # pages allocated from Xen heap    */

    /* Scheduling. */
    int              shutdown_code; /* code value from OS (if DOMF_shutdown) */
    void            *sched_priv;    /* scheduler-specific data */

    struct domain   *next_in_list;
    struct domain   *next_in_hashbucket;

    /* Event channel information. */
    struct evtchn   *evtchn[NR_EVTCHN_BUCKETS];
    spinlock_t       evtchn_lock;

    grant_table_t   *grant_table;

    /*
     * Interrupt to event-channel mappings. Updates should be protected by the 
     * domain's event-channel spinlock. Read accesses can also synchronise on 
     * the lock, but races don't usually matter.
     */
#define NR_PIRQS 128 /* Put this somewhere sane! */
    u16              pirq_to_evtchn[NR_PIRQS];
    u32              pirq_mask[NR_PIRQS/32];

    unsigned long    domain_flags;
    unsigned long    vm_assist;

    atomic_t         refcnt;

    struct vcpu *vcpu[MAX_VIRT_CPUS];

    /* Bitmask of CPUs which are holding onto this domain's state. */
    cpumask_t        cpumask;

    struct arch_domain arch;
};

struct domain_setup_info
{
    /* Initialised by caller. */
    unsigned long image_addr;
    unsigned long image_len;
    /* Initialised by loader: Public. */
    unsigned long v_start;
    unsigned long v_end;
    unsigned long v_kernstart;
    unsigned long v_kernend;
    unsigned long v_kernentry;
    /* Initialised by loader: Private. */
    unsigned int  load_symtab;
    unsigned long symtab_addr;
    unsigned long symtab_len;
    /* Indicate whether it's xen specific image */
    unsigned int xen_elf_image;
};

extern struct domain idle0_domain;
extern struct vcpu idle0_vcpu;

extern struct vcpu *idle_task[NR_CPUS];
#define IDLE_DOMAIN_ID   (0x7FFFU)
#define is_idle_task(_d) (test_bit(_DOMF_idle_domain, &(_d)->domain_flags))

struct vcpu *alloc_vcpu_struct(struct domain *d,
                                             unsigned long vcpu);

void free_domain_struct(struct domain *d);
struct domain *alloc_domain_struct();

#define DOMAIN_DESTRUCTED (1<<31) /* assumes atomic_t is >= 32 bits */
#define put_domain(_d) \
  if ( atomic_dec_and_test(&(_d)->refcnt) ) domain_destruct(_d)

/*
 * Use this when you don't have an existing reference to @d. It returns
 * FALSE if @d is being destructed.
 */
static always_inline int get_domain(struct domain *d)
{
    atomic_t old, new, seen = d->refcnt;
    do
    {
        old = seen;
        if ( unlikely(_atomic_read(old) & DOMAIN_DESTRUCTED) )
            return 0;
        _atomic_set(new, _atomic_read(old) + 1);
        seen = atomic_compareandswap(old, new, &d->refcnt);
    }
    while ( unlikely(_atomic_read(seen) != _atomic_read(old)) );
    return 1;
}

/*
 * Use this when you already have, or are borrowing, a reference to @d.
 * In this case we know that @d cannot be destructed under our feet.
 */
static inline void get_knownalive_domain(struct domain *d)
{
    atomic_inc(&d->refcnt);
    ASSERT(!(atomic_read(&d->refcnt) & DOMAIN_DESTRUCTED));
}

extern struct domain *do_createdomain(
    domid_t dom_id, unsigned int cpu);
extern int construct_dom0(
    struct domain *d,
    unsigned long image_start, unsigned long image_len, 
    unsigned long initrd_start, unsigned long initrd_len,
    char *cmdline);
extern int set_info_guest(struct domain *d, dom0_setdomaininfo_t *);

struct domain *find_domain_by_id(domid_t dom);
extern void domain_destruct(struct domain *d);
extern void domain_kill(struct domain *d);
extern void domain_shutdown(u8 reason);
extern void domain_pause_for_debugger(void);

/*
 * Mark current domain as crashed. This function returns: the domain is not
 * synchronously descheduled from any processor.
 */
extern void domain_crash(void);

/*
 * Mark current domain as crashed and synchronously deschedule from the local
 * processor. This function never returns.
 */
extern void domain_crash_synchronous(void) __attribute__((noreturn));

void new_thread(struct vcpu *d,
                unsigned long start_pc,
                unsigned long start_stack,
                unsigned long start_info);

#define set_current_state(_s) do { current->state = (_s); } while (0)
void scheduler_init(void);
void schedulers_start(void);
void sched_add_domain(struct vcpu *);
void sched_rem_domain(struct vcpu *);
long sched_ctl(struct sched_ctl_cmd *);
long sched_adjdom(struct sched_adjdom_cmd *);
int  sched_id();
void domain_wake(struct vcpu *d);
void domain_sleep_nosync(struct vcpu *d);
void domain_sleep_sync(struct vcpu *d);

/*
 * Force loading of currently-executing domain state on the specified set
 * of CPUs. This is used to counteract lazy state switching where required.
 */
extern void sync_lazy_execstate_cpu(unsigned int cpu);
extern void sync_lazy_execstate_mask(cpumask_t mask);
extern void sync_lazy_execstate_all(void);
extern int __sync_lazy_execstate(void);

/* Called by the scheduler to switch to another vcpu. */
extern void context_switch(
    struct vcpu *prev, 
    struct vcpu *next);

/* Called by the scheduler to continue running the current vcpu. */
extern void continue_running(
    struct vcpu *same);

int idle_cpu(int cpu); /* Is CPU 'cpu' idle right now? */

void startup_cpu_idle_loop(void);

unsigned long __hypercall_create_continuation(
    unsigned int op, unsigned int nr_args, ...);
#define hypercall0_create_continuation(_op)                               \
    __hypercall_create_continuation((_op), 0)
#define hypercall1_create_continuation(_op, _a1)                          \
    __hypercall_create_continuation((_op), 1,                             \
        (unsigned long)(_a1))
#define hypercall2_create_continuation(_op, _a1, _a2)                     \
    __hypercall_create_continuation((_op), 2,                             \
        (unsigned long)(_a1), (unsigned long)(_a2))
#define hypercall3_create_continuation(_op, _a1, _a2, _a3)                \
    __hypercall_create_continuation((_op), 3,                             \
        (unsigned long)(_a1), (unsigned long)(_a2), (unsigned long)(_a3))
#define hypercall4_create_continuation(_op, _a1, _a2, _a3, _a4)           \
    __hypercall_create_continuation((_op), 4,                             \
        (unsigned long)(_a1), (unsigned long)(_a2), (unsigned long)(_a3), \
        (unsigned long)(_a4))
#define hypercall5_create_continuation(_op, _a1, _a2, _a3, _a4, _a5)      \
    __hypercall_create_continuation((_op), 5,                             \
        (unsigned long)(_a1), (unsigned long)(_a2), (unsigned long)(_a3), \
        (unsigned long)(_a4), (unsigned long)(_a5))
#define hypercall6_create_continuation(_op, _a1, _a2, _a3, _a4, _a5, _a6) \
    __hypercall_create_continuation((_op), 6,                             \
        (unsigned long)(_a1), (unsigned long)(_a2), (unsigned long)(_a3), \
        (unsigned long)(_a4), (unsigned long)(_a5), (unsigned long)(_a6))

#define hypercall_preempt_check() (unlikely(            \
        softirq_pending(smp_processor_id()) |           \
        (!!current->vcpu_info->evtchn_upcall_pending &  \
          !current->vcpu_info->evtchn_upcall_mask)      \
    ))

/* This domain_hash and domain_list are protected by the domlist_lock. */
#define DOMAIN_HASH_SIZE 256
#define DOMAIN_HASH(_id) ((int)(_id)&(DOMAIN_HASH_SIZE-1))
extern struct domain *domain_hash[DOMAIN_HASH_SIZE];
extern struct domain *domain_list;

#define for_each_domain(_d) \
 for ( (_d) = domain_list; (_d) != NULL; (_d) = (_d)->next_in_list )

#define for_each_vcpu(_d,_ed) \
 for ( (_ed) = (_d)->vcpu[0]; \
       (_ed) != NULL;                \
       (_ed) = (_ed)->next_in_list )

/*
 * Per-VCPU flags (vcpu_flags).
 */
 /* Has the FPU been initialised? */
#define _VCPUF_fpu_initialised 0
#define VCPUF_fpu_initialised  (1UL<<_VCPUF_fpu_initialised)
 /* Has the FPU been used since it was last saved? */
#define _VCPUF_fpu_dirtied     1
#define VCPUF_fpu_dirtied      (1UL<<_VCPUF_fpu_dirtied)
 /* Has the guest OS requested 'stts'? */
#define _VCPUF_guest_stts      2
#define VCPUF_guest_stts       (1UL<<_VCPUF_guest_stts)
 /* Domain is blocked waiting for an event. */
#define _VCPUF_blocked         3
#define VCPUF_blocked          (1UL<<_VCPUF_blocked)
 /* Domain is paused by controller software. */
#define _VCPUF_ctrl_pause      4
#define VCPUF_ctrl_pause       (1UL<<_VCPUF_ctrl_pause)
 /* Currently running on a CPU? */
#define _VCPUF_running         5
#define VCPUF_running          (1UL<<_VCPUF_running)
 /* Disables auto-migration between CPUs. */
#define _VCPUF_cpu_pinned      6
#define VCPUF_cpu_pinned       (1UL<<_VCPUF_cpu_pinned)
 /* Domain migrated between CPUs. */
#define _VCPUF_cpu_migrated    7
#define VCPUF_cpu_migrated     (1UL<<_VCPUF_cpu_migrated)
 /* Initialization completed. */
#define _VCPUF_initialised     8
#define VCPUF_initialised      (1UL<<_VCPUF_initialised)

/*
 * Per-domain flags (domain_flags).
 */
 /* Has the guest OS been fully built yet? */
#define _DOMF_constructed      0
#define DOMF_constructed       (1UL<<_DOMF_constructed)
 /* Is this one of the per-CPU idle domains? */
#define _DOMF_idle_domain      1
#define DOMF_idle_domain       (1UL<<_DOMF_idle_domain)
 /* Is this domain privileged? */
#define _DOMF_privileged       2
#define DOMF_privileged        (1UL<<_DOMF_privileged)
 /* May this domain do IO to physical devices? */
#define _DOMF_physdev_access   3
#define DOMF_physdev_access    (1UL<<_DOMF_physdev_access)
 /* Guest shut itself down for some reason. */
#define _DOMF_shutdown         4
#define DOMF_shutdown          (1UL<<_DOMF_shutdown)
 /* Guest is in process of shutting itself down (becomes DOMF_shutdown). */
#define _DOMF_shuttingdown     5
#define DOMF_shuttingdown      (1UL<<_DOMF_shuttingdown)
 /* Death rattle. */
#define _DOMF_dying            6
#define DOMF_dying             (1UL<<_DOMF_dying)

static inline int domain_runnable(struct vcpu *v)
{
    return ( (atomic_read(&v->pausecnt) == 0) &&
             !(v->vcpu_flags & (VCPUF_blocked|VCPUF_ctrl_pause)) &&
             !(v->domain->domain_flags & (DOMF_shutdown|DOMF_shuttingdown)) );
}

void vcpu_pause(struct vcpu *v);
void domain_pause(struct domain *d);
void vcpu_unpause(struct vcpu *v);
void domain_unpause(struct domain *d);
void domain_pause_by_systemcontroller(struct domain *d);
void domain_unpause_by_systemcontroller(struct domain *d);

static inline void vcpu_unblock(struct vcpu *v)
{
    if ( test_and_clear_bit(_VCPUF_blocked, &v->vcpu_flags) )
        domain_wake(v);
}

#define IS_PRIV(_d)                                         \
    (test_bit(_DOMF_privileged, &(_d)->domain_flags))
#define IS_CAPABLE_PHYSDEV(_d)                              \
    (test_bit(_DOMF_physdev_access, &(_d)->domain_flags))

#define VM_ASSIST(_d,_t) (test_bit((_t), &(_d)->vm_assist))

#endif /* __SCHED_H__ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
