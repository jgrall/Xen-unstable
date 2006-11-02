/******************************************************************************
 * console.c
 * 
 * Emergency console I/O for Xen and the domain-0 guest OS.
 * 
 * Copyright (c) 2002-2004, K A Fraser.
 *
 * Added printf_ratelimit
 *     Taken from Linux - Author: Andi Kleen (net_ratelimit)
 *     Ported to Xen - Steven Rostedt - Red Hat
 */

#include <xen/stdarg.h>
#include <xen/config.h>
#include <xen/version.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/spinlock.h>
#include <xen/console.h>
#include <xen/serial.h>
#include <xen/softirq.h>
#include <xen/keyhandler.h>
#include <xen/mm.h>
#include <xen/delay.h>
#include <xen/guest_access.h>
#include <xen/shutdown.h>
#include <xen/vga.h>
#include <asm/current.h>
#include <asm/debugger.h>
#include <asm/io.h>
#include <asm/div64.h>

/* console: comma-separated list of console outputs. */
static char opt_console[30] = OPT_CONSOLE_STR;
string_param("console", opt_console);

/* conswitch: a character pair controlling console switching. */
/* Char 1: CTRL+<char1> is used to switch console input between Xen and DOM0 */
/* Char 2: If this character is 'x', then do not auto-switch to DOM0 when it */
/*         boots. Any other value, or omitting the char, enables auto-switch */
static unsigned char opt_conswitch[5] = "a";
string_param("conswitch", opt_conswitch);

/* sync_console: force synchronous console output (useful for debugging). */
static int opt_sync_console;
boolean_param("sync_console", opt_sync_console);

#define CONRING_SIZE 16384
#define CONRING_IDX_MASK(i) ((i)&(CONRING_SIZE-1))
static char conring[CONRING_SIZE];
static unsigned int conringc, conringp;

static char printk_prefix[16] = "";

static int sercon_handle = -1;

static DEFINE_SPINLOCK(console_lock);

/*
 * To control the amount of printing, thresholds are added.
 * These thresholds correspond to the XENLOG logging levels.
 * There's an upper and lower threshold for non-guest messages and for
 * guest-provoked messages.  This works as follows, for a given log level L:
 *
 * L < lower_threshold                     : always logged
 * lower_threshold <= L < upper_threshold  : rate-limited logging
 * upper_threshold <= L                    : never logged
 *
 * Note, in the above algorithm, to disable rate limiting simply make
 * the lower threshold equal to the upper.
 */
#define XENLOG_UPPER_THRESHOLD       2 /* Do not print INFO and DEBUG  */
#define XENLOG_LOWER_THRESHOLD       2 /* Always print ERR and WARNING */
#define XENLOG_GUEST_UPPER_THRESHOLD 2 /* Do not print INFO and DEBUG  */
#define XENLOG_GUEST_LOWER_THRESHOLD 0 /* Rate-limit ERR and WARNING   */
/*
 * The XENLOG_DEFAULT is the default given to printks that
 * do not have any print level associated with them.
 */
#define XENLOG_DEFAULT       1 /* XENLOG_WARNING */
#define XENLOG_GUEST_DEFAULT 1 /* XENLOG_WARNING */

static int xenlog_upper_thresh = XENLOG_UPPER_THRESHOLD;
static int xenlog_lower_thresh = XENLOG_LOWER_THRESHOLD;
static int xenlog_guest_upper_thresh = XENLOG_GUEST_UPPER_THRESHOLD;
static int xenlog_guest_lower_thresh = XENLOG_GUEST_LOWER_THRESHOLD;

static void parse_loglvl(char *s);
static void parse_guest_loglvl(char *s);

/*
 * <lvl> := none|error|warning|info|debug|all
 * loglvl=<lvl_print_always>[/<lvl_print_ratelimit>]
 *  <lvl_print_always>: log level which is always printed
 *  <lvl_print_rlimit>: log level which is rate-limit printed
 * Similar definitions for guest_loglvl, but applies to guest tracing.
 * Defaults: loglvl=warning ; guest_loglvl=none/warning
 */
custom_param("loglvl", parse_loglvl);
custom_param("guest_loglvl", parse_guest_loglvl);

static int xen_startup = 1;

#define ___parse_loglvl(s, ps, lvlstr, lvlnum)          \
    if ( !strncmp((s), (lvlstr), strlen(lvlstr)) ) {    \
        *(ps) = (s) + strlen(lvlstr);                   \
        return (lvlnum);                                \
    }

static int __parse_loglvl(char *s, char **ps)
{
    ___parse_loglvl(s, ps, "none",    0);
    ___parse_loglvl(s, ps, "error",   1);
    ___parse_loglvl(s, ps, "warning", 2);
    ___parse_loglvl(s, ps, "info",    3);
    ___parse_loglvl(s, ps, "debug",   4);
    ___parse_loglvl(s, ps, "all",     4);
    return 2; /* sane fallback */
}

static void _parse_loglvl(char *s, int *lower, int *upper)
{
    *lower = *upper = __parse_loglvl(s, &s);
    if ( *s == '/' )
        *upper = __parse_loglvl(s+1, &s);
    if ( *upper < *lower )
        *upper = *lower;
}

static void parse_loglvl(char *s)
{
    _parse_loglvl(s, &xenlog_lower_thresh, &xenlog_upper_thresh);
}

static void parse_guest_loglvl(char *s)
{
    _parse_loglvl(s, &xenlog_guest_lower_thresh, &xenlog_guest_upper_thresh);
}

static char *loglvl_str(int lvl)
{
    switch ( lvl )
    {
    case 0: return "Nothing";
    case 1: return "Errors";
    case 2: return "Errors and warnings";
    case 3: return "Errors, warnings and info";
    case 4: return "All";
    }
    return "???";
}

/*
 * ********************************************************
 * *************** ACCESS TO CONSOLE RING *****************
 * ********************************************************
 */

static void putchar_console_ring(int c)
{
    conring[CONRING_IDX_MASK(conringp++)] = c;
    if ( (conringp - conringc) > CONRING_SIZE )
        conringc = conringp - CONRING_SIZE;
}

long read_console_ring(XEN_GUEST_HANDLE(char) str, u32 *pcount, int clear)
{
    unsigned int idx, len, max, sofar, c;
    unsigned long flags;

    max   = *pcount;
    sofar = 0;

    c = conringc;
    while ( (c != conringp) && (sofar < max) )
    {
        idx = CONRING_IDX_MASK(c);
        len = conringp - c;
        if ( (idx + len) > CONRING_SIZE )
            len = CONRING_SIZE - idx;
        if ( (sofar + len) > max )
            len = max - sofar;
        if ( copy_to_guest_offset(str, sofar, &conring[idx], len) )
            return -EFAULT;
        sofar += len;
        c += len;
    }

    if ( clear )
    {
        spin_lock_irqsave(&console_lock, flags);
        if ( (conringp - c) > CONRING_SIZE )
            conringc = conringp - CONRING_SIZE;
        else
            conringc = c;
        spin_unlock_irqrestore(&console_lock, flags);
    }

    *pcount = sofar;
    return 0;
}


/*
 * *******************************************************
 * *************** ACCESS TO SERIAL LINE *****************
 * *******************************************************
 */

/* Characters received over the serial line are buffered for domain 0. */
#define SERIAL_RX_SIZE 128
#define SERIAL_RX_MASK(_i) ((_i)&(SERIAL_RX_SIZE-1))
static char serial_rx_ring[SERIAL_RX_SIZE];
static unsigned int serial_rx_cons, serial_rx_prod;

static void (*serial_steal_fn)(const char *);

int console_steal(int handle, void (*fn)(const char *))
{
    if ( (handle == -1) || (handle != sercon_handle) )
        return 0;

    if ( serial_steal_fn == NULL )
        return -EBUSY;

    serial_steal_fn = fn;
    return 1;
}

void console_giveback(int id)
{
    if ( id == 1 )
        serial_steal_fn = NULL;
}

static void sercon_puts(const char *s)
{
    if ( serial_steal_fn != NULL )
        (*serial_steal_fn)(s);
    else
        serial_puts(sercon_handle, s);
}

/* CTRL-<switch_char> switches input direction between Xen and DOM0. */
#define SWITCH_CODE (opt_conswitch[0]-'a'+1)
static int xen_rx = 1; /* FALSE => serial input passed to domain 0. */

static void switch_serial_input(void)
{
    static char *input_str[2] = { "DOM0", "Xen" };
    xen_rx = !xen_rx;
    if ( (SWITCH_CODE != 0) && (dom0 != NULL) )
    {
        printk("*** Serial input -> %s "
               "(type 'CTRL-%c' three times to switch input to %s).\n",
               input_str[xen_rx], opt_conswitch[0], input_str[!xen_rx]);
    }
}

static void __serial_rx(char c, struct cpu_user_regs *regs)
{
    if ( xen_rx )
        return handle_keypress(c, regs);

    /* Deliver input to guest buffer, unless it is already full. */
    if ( (serial_rx_prod-serial_rx_cons) != SERIAL_RX_SIZE )
        serial_rx_ring[SERIAL_RX_MASK(serial_rx_prod++)] = c;
    /* Always notify the guest: prevents receive path from getting stuck. */
    send_guest_global_virq(dom0, VIRQ_CONSOLE);
}

static void serial_rx(char c, struct cpu_user_regs *regs)
{
    static int switch_code_count = 0;

    if ( (SWITCH_CODE != 0) && (c == SWITCH_CODE) )
    {
        /* We eat CTRL-<switch_char> in groups of 3 to switch console input. */
        if ( ++switch_code_count == 3 )
        {
            switch_serial_input();
            switch_code_count = 0;
            return;
        }
    }
    else
    {
        switch_code_count = 0;
    }

    /* Finally process the just-received character. */
    __serial_rx(c, regs);
}

static long guest_console_write(XEN_GUEST_HANDLE(char) buffer, int count)
{
    char kbuf[128], *kptr;
    int kcount;

    while ( count > 0 )
    {
        while ( serial_tx_space(sercon_handle) < (SERIAL_TXBUFSZ / 2) )
        {
            if ( hypercall_preempt_check() )
                break;
            cpu_relax();
        }

        if ( hypercall_preempt_check() )
            return hypercall_create_continuation(
                __HYPERVISOR_console_io, "iih",
                CONSOLEIO_write, count, buffer);

        kcount = min_t(int, count, sizeof(kbuf)-1);
        if ( copy_from_guest((char *)kbuf, buffer, kcount) )
            return -EFAULT;
        kbuf[kcount] = '\0';

        sercon_puts(kbuf);

        for ( kptr = kbuf; *kptr != '\0'; kptr++ )
            vga_putchar(*kptr);

        guest_handle_add_offset(buffer, kcount);
        count -= kcount;
    }

    return 0;
}

long do_console_io(int cmd, int count, XEN_GUEST_HANDLE(char) buffer)
{
    long rc;
    unsigned int idx, len;

#ifndef VERBOSE
    /* Only domain 0 may access the emergency console. */
    if ( current->domain->domain_id != 0 )
        return -EPERM;
#endif

    switch ( cmd )
    {
    case CONSOLEIO_write:
        rc = guest_console_write(buffer, count);
        break;
    case CONSOLEIO_read:
        rc = 0;
        while ( (serial_rx_cons != serial_rx_prod) && (rc < count) )
        {
            idx = SERIAL_RX_MASK(serial_rx_cons);
            len = serial_rx_prod - serial_rx_cons;
            if ( (idx + len) > SERIAL_RX_SIZE )
                len = SERIAL_RX_SIZE - idx;
            if ( (rc + len) > count )
                len = count - rc;
            if ( copy_to_guest_offset(buffer, rc, &serial_rx_ring[idx], len) )
            {
                rc = -EFAULT;
                break;
            }
            rc += len;
            serial_rx_cons += len;
        }
        break;
    default:
        rc = -ENOSYS;
        break;
    }

    return rc;
}


/*
 * *****************************************************
 * *************** GENERIC CONSOLE I/O *****************
 * *****************************************************
 */

static inline void __putstr(const char *str)
{
    int c;

    sercon_puts(str);

    while ( (c = *str++) != '\0' )
    {
        vga_putchar(c);
        putchar_console_ring(c);
    }
}

void printk(const char *fmt, ...)
{
    static char   buf[1024];
    static int    start_of_line = 1;

    va_list       args;
    char         *p, *q;
    unsigned long flags;
    int           level = -1;
    int           upper_thresh = xenlog_upper_thresh;
    int           lower_thresh = xenlog_lower_thresh;
    int           print_regardless = xen_startup;

    /* console_lock can be acquired recursively from __printk_ratelimit(). */
    local_irq_save(flags);
    spin_lock_recursive(&console_lock);

    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);        

    p = buf;

    while ( (p[0] == '<') && (p[1] != '\0') && (p[2] == '>') )
    {
        switch ( p[1] )
        {
        case 'G':
            upper_thresh = xenlog_guest_upper_thresh;
            lower_thresh = xenlog_guest_lower_thresh;
            if ( level == -1 )
                level = XENLOG_GUEST_DEFAULT;
            break;
        case '0' ... '3':
            level = p[1] - '0';
            break;
        }
        p += 3;
    }

    if ( level == -1 )
        level = XENLOG_DEFAULT;

    if ( !print_regardless )
    {
        if ( level >= upper_thresh )
            goto out;
        if ( (level >= lower_thresh) && (!printk_ratelimit()) )
            goto out;
    }

    while ( (q = strchr(p, '\n')) != NULL )
    {
        *q = '\0';
        if ( start_of_line )
            __putstr(printk_prefix);
        __putstr(p);
        __putstr("\n");
        start_of_line = 1;
        p = q + 1;
    }

    if ( *p != '\0' )
    {
        if ( start_of_line )
            __putstr(printk_prefix);
        __putstr(p);
        start_of_line = 0;
    }

 out:
    spin_unlock_recursive(&console_lock);
    local_irq_restore(flags);
}

void set_printk_prefix(const char *prefix)
{
    strcpy(printk_prefix, prefix);
}

void init_console(void)
{
    char *p;

    /* Where should console output go? */
    for ( p = opt_console; p != NULL; p = strchr(p, ',') )
    {
        if ( *p == ',' )
            p++;
        if ( strncmp(p, "com", 3) == 0 )
            sercon_handle = serial_parse_handle(p);
        else if ( strncmp(p, "vga", 3) == 0 )
            vga_init();
    }

    serial_set_rx_handler(sercon_handle, serial_rx);

    /* HELLO WORLD --- start-of-day banner text. */
    printk(xen_banner());
    printk(" http://www.cl.cam.ac.uk/netos/xen\n");
    printk(" University of Cambridge Computer Laboratory\n\n");
    printk(" Xen version %d.%d%s (%s@%s) (%s) %s\n",
           xen_major_version(), xen_minor_version(), xen_extra_version(),
           xen_compile_by(), xen_compile_domain(),
           xen_compiler(), xen_compile_date());
    printk(" Latest ChangeSet: %s\n\n", xen_changeset());
    set_printk_prefix("(XEN) ");

    if ( opt_sync_console )
    {
        serial_start_sync(sercon_handle);
        add_taint(TAINT_SYNC_CONSOLE);
        printk("Console output is synchronous.\n");
    }
}

void console_endboot(void)
{
    int i, j;

    printk("Std. Loglevel: %s", loglvl_str(xenlog_lower_thresh));
    if ( xenlog_upper_thresh != xenlog_lower_thresh )
        printk(" (Rate-limited: %s)", loglvl_str(xenlog_upper_thresh));
    printk("\nGuest Loglevel: %s", loglvl_str(xenlog_guest_lower_thresh));
    if ( xenlog_guest_upper_thresh != xenlog_guest_lower_thresh )
        printk(" (Rate-limited: %s)", loglvl_str(xenlog_guest_upper_thresh));
    printk("\n");

    if ( opt_sync_console )
    {
        printk("**********************************************\n");
        printk("******* WARNING: CONSOLE OUTPUT IS SYCHRONOUS\n");
        printk("******* This option is intended to aid debugging "
               "of Xen by ensuring\n");
        printk("******* that all output is synchronously delivered "
               "on the serial line.\n");
        printk("******* However it can introduce SIGNIFICANT latencies "
               "and affect\n");
        printk("******* timekeeping. It is NOT recommended for "
               "production use!\n");
        printk("**********************************************\n");
        for ( i = 0; i < 3; i++ )
        {
            printk("%d... ", 3-i);
            for ( j = 0; j < 100; j++ )
            {
                process_pending_timers();
                mdelay(10);
            }
        }
        printk("\n");
    }

    vga_endboot();

    /*
     * If user specifies so, we fool the switch routine to redirect input
     * straight back to Xen. I use this convoluted method so we still print
     * a useful 'how to switch' message.
     */
    if ( opt_conswitch[1] == 'x' )
        xen_rx = !xen_rx;

    /* Serial input is directed to DOM0 by default. */
    switch_serial_input();

    /* Now we implement the logging thresholds. */
    xen_startup = 0;
}

void console_force_unlock(void)
{
    spin_lock_init(&console_lock);
    serial_force_unlock(sercon_handle);
    console_start_sync();
}

void console_force_lock(void)
{
    spin_lock(&console_lock);
}

void console_start_sync(void)
{
    serial_start_sync(sercon_handle);
}

void console_end_sync(void)
{
    serial_end_sync(sercon_handle);
}

void console_putc(char c)
{
    serial_putc(sercon_handle, c);
}

int console_getc(void)
{
    return serial_getc(sercon_handle);
}

/*
 * printk rate limiting, lifted from Linux.
 *
 * This enforces a rate limit: not more than one kernel message
 * every printk_ratelimit_ms (millisecs).
 */
int __printk_ratelimit(int ratelimit_ms, int ratelimit_burst)
{
    static DEFINE_SPINLOCK(ratelimit_lock);
    static unsigned long toks = 10 * 5 * 1000;
    static unsigned long last_msg;
    static int missed;
    unsigned long flags;
    unsigned long long now = NOW(); /* ns */
    unsigned long ms;

    do_div(now, 1000000);
    ms = (unsigned long)now;

    spin_lock_irqsave(&ratelimit_lock, flags);
    toks += ms - last_msg;
    last_msg = ms;
    if ( toks > (ratelimit_burst * ratelimit_ms))
        toks = ratelimit_burst * ratelimit_ms;
    if ( toks >= ratelimit_ms )
    {
        int lost = missed;
        missed = 0;
        toks -= ratelimit_ms;
        spin_unlock(&ratelimit_lock);
        if ( lost )
        {
            char lost_str[8];
            snprintf(lost_str, sizeof(lost_str), "%d", lost);
            /* console_lock may already be acquired by printk(). */
            spin_lock_recursive(&console_lock);
            __putstr(printk_prefix);
            __putstr("printk: ");
            __putstr(lost_str);
            __putstr(" messages suppressed.\n");
            spin_unlock_recursive(&console_lock);
        }
        local_irq_restore(flags);
        return 1;
    }
    missed++;
    spin_unlock_irqrestore(&ratelimit_lock, flags);
    return 0;
}

/* minimum time in ms between messages */
int printk_ratelimit_ms = 5 * 1000;

/* number of messages we send before ratelimiting */
int printk_ratelimit_burst = 10;

int printk_ratelimit(void)
{
    return __printk_ratelimit(printk_ratelimit_ms, printk_ratelimit_burst);
}

/*
 * **************************************************************
 * *************** Serial console ring buffer *******************
 * **************************************************************
 */

#ifdef DEBUG_TRACE_DUMP

/* Send output direct to console, or buffer it? */
static volatile int debugtrace_send_to_console;

static char        *debugtrace_buf; /* Debug-trace buffer */
static unsigned int debugtrace_prd; /* Producer index     */
static unsigned int debugtrace_kilobytes = 128, debugtrace_bytes;
static unsigned int debugtrace_used;
static DEFINE_SPINLOCK(debugtrace_lock);
integer_param("debugtrace", debugtrace_kilobytes);

static void debugtrace_dump_worker(void)
{
    if ( (debugtrace_bytes == 0) || !debugtrace_used )
        return;

    printk("debugtrace_dump() starting\n");

    /* Print oldest portion of the ring. */
    ASSERT(debugtrace_buf[debugtrace_bytes - 1] == 0);
    sercon_puts(&debugtrace_buf[debugtrace_prd]);

    /* Print youngest portion of the ring. */
    debugtrace_buf[debugtrace_prd] = '\0';
    sercon_puts(&debugtrace_buf[0]);

    memset(debugtrace_buf, '\0', debugtrace_bytes);

    printk("debugtrace_dump() finished\n");
}

static void debugtrace_toggle(void)
{
    unsigned long flags;

    watchdog_disable();
    spin_lock_irqsave(&debugtrace_lock, flags);

    // dump the buffer *before* toggling, in case the act of dumping the
    // buffer itself causes more printk's...
    //
    printk("debugtrace_printk now writing to %s.\n",
           !debugtrace_send_to_console ? "console": "buffer");
    if ( !debugtrace_send_to_console )
        debugtrace_dump_worker();

    debugtrace_send_to_console = !debugtrace_send_to_console;

    spin_unlock_irqrestore(&debugtrace_lock, flags);
    watchdog_enable();

}

void debugtrace_dump(void)
{
    unsigned long flags;

    watchdog_disable();
    spin_lock_irqsave(&debugtrace_lock, flags);

    debugtrace_dump_worker();

    spin_unlock_irqrestore(&debugtrace_lock, flags);
    watchdog_enable();
}

void debugtrace_printk(const char *fmt, ...)
{
    static char    buf[1024];
    static u32 count;

    va_list       args;
    char         *p;
    unsigned long flags;

    if ( debugtrace_bytes == 0 )
        return;

    debugtrace_used = 1;

    spin_lock_irqsave(&debugtrace_lock, flags);

    ASSERT(debugtrace_buf[debugtrace_bytes - 1] == 0);

    sprintf(buf, "%u ", ++count);

    va_start(args, fmt);
    (void)vsnprintf(buf + strlen(buf), sizeof(buf), fmt, args);
    va_end(args);

    if ( debugtrace_send_to_console )
    {
        serial_puts(sercon_handle, buf);
    }
    else
    {
        for ( p = buf; *p != '\0'; p++ )
        {
            debugtrace_buf[debugtrace_prd++] = *p;            
            /* Always leave a nul byte at the end of the buffer. */
            if ( debugtrace_prd == (debugtrace_bytes - 1) )
                debugtrace_prd = 0;
        }
    }

    spin_unlock_irqrestore(&debugtrace_lock, flags);
}

static void debugtrace_key(unsigned char key)
{
    debugtrace_toggle();
}

static int __init debugtrace_init(void)
{
    int order;
    unsigned int kbytes, bytes;

    /* Round size down to next power of two. */
    while ( (kbytes = (debugtrace_kilobytes & (debugtrace_kilobytes-1))) != 0 )
        debugtrace_kilobytes = kbytes;

    bytes = debugtrace_kilobytes << 10;
    if ( bytes == 0 )
        return 0;

    order = get_order_from_bytes(bytes);
    debugtrace_buf = alloc_xenheap_pages(order);
    ASSERT(debugtrace_buf != NULL);

    memset(debugtrace_buf, '\0', bytes);

    debugtrace_bytes = bytes;

    register_keyhandler(
        'T', debugtrace_key, "toggle debugtrace to console/buffer");

    return 0;
}
__initcall(debugtrace_init);

#endif /* !NDEBUG */



/*
 * **************************************************************
 * *************** Debugging/tracing/error-report ***************
 * **************************************************************
 */

void panic(const char *fmt, ...)
{
    va_list args;
    char buf[128];
    unsigned long flags;
    static DEFINE_SPINLOCK(lock);
    
    debugtrace_dump();

    va_start(args, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Spit out multiline message in one go. */
    console_start_sync();
    spin_lock_irqsave(&lock, flags);
    printk("\n****************************************\n");
    printk("Panic on CPU %d:\n", smp_processor_id());
    printk(buf);
    printk("****************************************\n\n");
    if ( opt_noreboot )
        printk("Manual reset required ('noreboot' specified)\n");
    else
        printk("Reboot in five seconds...\n");
    spin_unlock_irqrestore(&lock, flags);

    debugger_trap_immediate();

    if ( opt_noreboot )
    {
        machine_halt();
    }
    else
    {
        watchdog_disable();
        mdelay(5000);
        machine_restart(NULL);
    }
}

void __bug(char *file, int line)
{
    console_start_sync();
    debugtrace_dump();
    printk("BUG at %s:%d\n", file, line);
    FORCE_CRASH();
    for ( ; ; ) ;
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

