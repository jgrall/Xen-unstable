/* -*-  Mode:C; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */
/******************************************************************************
 * keyhandler.c
 */

#include <xen/keyhandler.h> 
#include <xen/reboot.h>
#include <xen/event.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <xen/sched.h>
#include <xen/softirq.h>
#include <asm/debugger.h>

#define KEY_MAX 256
#define STR_MAX  64

static struct {
    union {
        keyhandler_t     *handler;
        irq_keyhandler_t *irq_handler;
    } u;
    unsigned int flags;
    char         desc[STR_MAX]; 
} key_table[KEY_MAX]; 

#define KEYHANDLER_IRQ_CALLBACK 0x1

static unsigned char keypress_key;

static void keypress_softirq(void)
{
    keyhandler_t *h;
    unsigned char key = keypress_key;
    if ( (h = key_table[key].u.handler) != NULL )
        (*h)(key);
}

void handle_keypress(unsigned char key, struct xen_regs *regs)
{
    irq_keyhandler_t *h;

    if ( key_table[key].flags & KEYHANDLER_IRQ_CALLBACK )
    {
        if ( (h = key_table[key].u.irq_handler) != NULL )
            (*h)(key, regs);
    }
    else
    {
        keypress_key = key;
        raise_softirq(KEYPRESS_SOFTIRQ);
    }
}

void register_keyhandler(
    unsigned char key, keyhandler_t *handler, char *desc)
{
    ASSERT(key_table[key].u.handler == NULL);
    key_table[key].u.handler = handler;
    key_table[key].flags     = 0;
    strncpy(key_table[key].desc, desc, STR_MAX);
    key_table[key].desc[STR_MAX-1] = '\0'; 
}

void register_irq_keyhandler(
    unsigned char key, irq_keyhandler_t *handler, char *desc)
{
    ASSERT(key_table[key].u.irq_handler == NULL);
    key_table[key].u.irq_handler = handler;
    key_table[key].flags         = KEYHANDLER_IRQ_CALLBACK;
    strncpy(key_table[key].desc, desc, STR_MAX);
    key_table[key].desc[STR_MAX-1] = '\0'; 
}

static void show_handlers(unsigned char key)
{
    int i; 
    printk("'%c' pressed -> showing installed handlers\n", key);
    for ( i = 0; i < KEY_MAX; i++ ) 
        if ( key_table[i].u.handler != NULL ) 
            printk(" key '%c' (ascii '%02x') => %s\n", 
                   (i<33 || i>126)?(' '):(i),i,
                   key_table[i].desc);
}

static void dump_registers(unsigned char key, struct xen_regs *regs)
{
    printk("'%c' pressed -> dumping registers\n", key); 
    show_registers(regs); 
}

static void halt_machine(unsigned char key, struct xen_regs *regs)
{
    printk("'%c' pressed -> rebooting machine\n", key); 
    machine_restart(NULL); 
}

static void do_task_queues(unsigned char key)
{
    struct domain *d;
    struct exec_domain *ed;
    s_time_t       now = NOW();

    printk("'%c' pressed -> dumping task queues (now=0x%X:%08X)\n", key,
           (u32)(now>>32), (u32)now); 

    read_lock(&domlist_lock);

    for_each_domain ( d )
    {
        printk("Xen: DOM %u, flags=%lx refcnt=%d nr_pages=%d "
               "xenheap_pages=%d\n", d->id, d->d_flags,
               atomic_read(&d->refcnt), d->tot_pages, d->xenheap_pages);

        dump_pageframe_info(d);
               
        for_each_exec_domain ( d, ed ) {
            printk("Guest: %p CPU %d [has=%c] flags=%lx "
                   "upcall_pend = %02x, upcall_mask = %02x\n", ed,
                   ed->processor,
                   test_bit(EDF_RUNNING, &ed->ed_flags) ? 'T':'F',
                   ed->ed_flags,
                   ed->vcpu_info->evtchn_upcall_pending, 
                   ed->vcpu_info->evtchn_upcall_mask);
            printk("Notifying guest... %d/%d\n", d->id, ed->eid); 
            printk("port %d/%d stat %d %d %d\n",
                   VIRQ_DEBUG, ed->virq_to_evtchn[VIRQ_DEBUG],
                   test_bit(ed->virq_to_evtchn[VIRQ_DEBUG], &d->shared_info->evtchn_pending[0]),
                   test_bit(ed->virq_to_evtchn[VIRQ_DEBUG], &d->shared_info->evtchn_mask[0]),
                   test_bit(ed->virq_to_evtchn[VIRQ_DEBUG]>>5, &ed->vcpu_info->evtchn_pending_sel));
            send_guest_virq(ed, VIRQ_DEBUG);
        }
    }

    read_unlock(&domlist_lock);
}

extern void dump_runq(unsigned char key);
extern void print_sched_histo(unsigned char key);
extern void reset_sched_histo(unsigned char key);
#ifndef NDEBUG
extern void audit_domains_key(unsigned char key);
#endif

#ifdef PERF_COUNTERS
extern void perfc_printall(unsigned char key);
extern void perfc_reset(unsigned char key);
#endif

void do_debug_key(unsigned char key, struct xen_regs *regs)
{
    (void)debugger_trap_fatal(0xf001, regs);
    nop(); /* Prevent the compiler doing tail call
                             optimisation, as that confuses xendbg a
                             bit. */
}

void initialize_keytable(void)
{
    open_softirq(KEYPRESS_SOFTIRQ, keypress_softirq);

    register_irq_keyhandler(
        'd', dump_registers, "dump registers"); 
    register_keyhandler(
        'h', show_handlers, "show this message");
    register_keyhandler(
        'l', print_sched_histo, "print sched latency histogram");
    register_keyhandler(
        'L', reset_sched_histo, "reset sched latency histogram");
    register_keyhandler(
        'q', do_task_queues, "dump task queues + guest state");
    register_keyhandler(
        'r', dump_runq,      "dump run queues");
    register_irq_keyhandler(
        'R', halt_machine,   "reboot machine"); 

#ifndef NDEBUG
    register_keyhandler(
        'o', audit_domains_key,  "audit domains >0 EXPERIMENTAL"); 
#endif

#ifdef PERF_COUNTERS
    register_keyhandler(
        'p', perfc_printall, "print performance counters"); 
    register_keyhandler(
        'P', perfc_reset,    "reset performance counters"); 
#endif

    register_irq_keyhandler('%', do_debug_key,   "Trap to xendbg");
}
