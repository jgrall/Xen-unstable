/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright (C) IBM Corp. 2005, 2006
 *
 * Authors: Jimi Xenidis <jimix@watson.ibm.com>
 */

#include <stdarg.h>
#include <xen/config.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/mm.h>
#include <xen/serial.h>
#include <xen/domain.h>
#include <xen/console.h>
#include <xen/shutdown.h>
#include <xen/shadow.h>
#include <xen/mm.h>
#include <xen/softirq.h>
#include <asm/htab.h>
#include <asm/current.h>
#include <asm/hcalls.h>

#define next_arg(fmt, args) ({                                              \
    unsigned long __arg;                                                    \
    switch ( *(fmt)++ )                                                     \
    {                                                                       \
    case 'i': __arg = (unsigned long)va_arg(args, unsigned int);  break;    \
    case 'l': __arg = (unsigned long)va_arg(args, unsigned long); break;    \
    case 'p': __arg = (unsigned long)va_arg(args, void *);        break;    \
    case 'h': __arg = (unsigned long)va_arg(args, void *);        break;    \
    default:  __arg = 0; BUG();                                             \
    }                                                                       \
    __arg;                                                                  \
})
extern void idle_loop(void);

unsigned long hypercall_create_continuation(unsigned int op,
        const char *format, ...)
{
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    const char *p = format;
    va_list args;
    int gprnum = 4;
    int i;

    va_start(args, format);

    regs->pc -= 4; /* re-execute 'sc' */

    for (i = 0; *p != '\0'; i++) {
        regs->gprs[gprnum++] = next_arg(p, args);
    }

    va_end(args);

    /* As luck would have it, we use the same register for hcall opcodes and
     * for hcall return values. The return value from this function is placed
     * in r3 on return, so modifying regs->gprs[3] would have no effect. */
    return XEN_MARK(op);
}

int arch_domain_create(struct domain *d)
{
    if (d->domain_id == IDLE_DOMAIN_ID) {
        d->shared_info = (void *)alloc_xenheap_page();
        clear_page(d->shared_info);

        return 0;
    }

    d->arch.large_page_sizes = cpu_large_page_orders(
        d->arch.large_page_order, ARRAY_SIZE(d->arch.large_page_order));

    INIT_LIST_HEAD(&d->arch.extent_list);

    return 0;
}

void arch_domain_destroy(struct domain *d)
{
    shadow_teardown(d);
}

void machine_halt(void)
{
    printk("machine_halt called: spinning....\n");
    console_start_sync();
    while(1);
}

void machine_restart(char * __unused)
{
    printk("machine_restart called: spinning....\n");
    console_start_sync();
    while(1);
}

struct vcpu *alloc_vcpu_struct(void)
{
    struct vcpu *v;
    if ( (v = xmalloc(struct vcpu)) != NULL )
        memset(v, 0, sizeof(*v));
    return v;
}

void free_vcpu_struct(struct vcpu *v)
{
    xfree(v);
}

int vcpu_initialise(struct vcpu *v)
{
    return 0;
}

void vcpu_destroy(struct vcpu *v)
{
}

int arch_set_info_guest(struct vcpu *v, vcpu_guest_context_t *c)
{ 
    memcpy(&v->arch.ctxt, &c->user_regs, sizeof(c->user_regs));

    printk("Domain[%d].%d: initializing\n",
           v->domain->domain_id, v->vcpu_id);

    if (v->domain->arch.htab.order == 0)
        panic("Page table never allocated for Domain: %d\n",
              v->domain->domain_id);
    if (v->domain->arch.rma_order == 0)
        panic("RMA never allocated for Domain: %d\n",
              v->domain->domain_id);

    set_bit(_VCPUF_initialised, &v->vcpu_flags);

    cpu_init_vcpu(v);

    return 0;
}

void dump_pageframe_info(struct domain *d)
{
    struct page_info *page;

    printk("Memory pages belonging to domain %u:\n", d->domain_id);

    if ( d->tot_pages >= 10 )
    {
        printk("    DomPage list too long to display\n");
    }
    else
    {
        list_for_each_entry ( page, &d->page_list, list )
        {
            printk("    DomPage %p: mfn=%p, caf=%016lx, taf=%" PRtype_info "\n",
                   _p(page_to_maddr(page)), _p(page_to_mfn(page)),
                   page->count_info, page->u.inuse.type_info);
        }
    }

    list_for_each_entry ( page, &d->xenpage_list, list )
    {
        printk("    XenPage %p: mfn=%p, caf=%016lx, taf=%" PRtype_info "\n",
               _p(page_to_maddr(page)), _p(page_to_mfn(page)),
               page->count_info, page->u.inuse.type_info);
    }
}

void context_switch(struct vcpu *prev, struct vcpu *next)
{
    struct cpu_user_regs *stack_regs = guest_cpu_user_regs();
    cpumask_t dirty_mask = next->vcpu_dirty_cpumask;
    unsigned int cpu = smp_processor_id();

#if 0
    printk("%s: dom %x to dom %x\n", __func__, prev->domain->domain_id,
            next->domain->domain_id);
#endif

    /* Allow at most one CPU at a time to be dirty. */
    ASSERT(cpus_weight(dirty_mask) <= 1);
    if (unlikely(!cpu_isset(cpu, dirty_mask) && !cpus_empty(dirty_mask)))
    {
        /* Other cpus call __sync_lazy_execstate from flush ipi handler. */
        if (!cpus_empty(next->vcpu_dirty_cpumask))
            flush_tlb_mask(next->vcpu_dirty_cpumask);
    }

    /* copy prev guest state off the stack into its vcpu */
    memcpy(&prev->arch.ctxt, stack_regs, sizeof(struct cpu_user_regs));

    set_current(next);

    /* copy next guest state onto the stack */
    memcpy(stack_regs, &next->arch.ctxt, sizeof(struct cpu_user_regs));

    /* save old domain state */
    save_sprs(prev);
    save_float(prev);
    save_segments(prev);

    context_saved(prev);

    /* load up new domain */
    load_sprs(next);
    load_float(next);
    load_segments(next);

    mtsdr1(next->domain->arch.htab.sdr1);
    local_flush_tlb(); /* XXX maybe flush_tlb_mask? */

    if (is_idle_vcpu(next)) {
        reset_stack_and_jump(idle_loop);
    }

    reset_stack_and_jump(full_resume);
    /* not reached */
}

void continue_running(struct vcpu *same)
{
    /* nothing to do */
    return;
}

void sync_vcpu_execstate(struct vcpu *v)
{
    /* do nothing */
    return;
}

static void relinquish_memory(struct domain *d, struct list_head *list)
{
    struct list_head *ent;
    struct page_info  *page;

    /* Use a recursive lock, as we may enter 'free_domheap_page'. */
    spin_lock_recursive(&d->page_alloc_lock);

    ent = list->next;
    while ( ent != list )
    {
        page = list_entry(ent, struct page_info, list);

        /* Grab a reference to the page so it won't disappear from under us. */
        if ( unlikely(!get_page(page, d)) )
        {
            /* Couldn't get a reference -- someone is freeing this page. */
            ent = ent->next;
            continue;
        }
        if ( test_and_clear_bit(_PGT_pinned, &page->u.inuse.type_info) )
            put_page_and_type(page);

        if ( test_and_clear_bit(_PGC_allocated, &page->count_info) )
            put_page(page);

        /* Follow the list chain and /then/ potentially free the page. */
        ent = ent->next;
        put_page(page);
    }
    spin_unlock_recursive(&d->page_alloc_lock);
}

void domain_relinquish_resources(struct domain *d)
{
    relinquish_memory(d, &d->page_list);
    free_extents(d);
    return;
}

void arch_dump_domain_info(struct domain *d)
{
}

void arch_dump_vcpu_info(struct vcpu *v)
{
}

extern void sleep(void);
static void safe_halt(void)
{
    int cpu = smp_processor_id();

    while (!softirq_pending(cpu))
        sleep();
}

static void default_idle(void)
{
    local_irq_disable();
    if ( !softirq_pending(smp_processor_id()) )
        safe_halt();
    else
        local_irq_enable();
}

void idle_loop(void)
{
    for ( ; ; ) {
        page_scrub_schedule_work();
        default_idle();
        do_softirq();
    }
}
