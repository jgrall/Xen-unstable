/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2004 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "vl.h"
#ifdef __ia64__
#include <xen/arch-ia64.h>
#endif

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#ifndef _WIN32
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <dirent.h>
#ifdef _BSD
#include <sys/stat.h>
#ifndef __APPLE__
#include <libutil.h>
#endif
#else
#include <linux/if.h>
#include <linux/if_tun.h>
#include <pty.h>
#include <malloc.h>
#include <linux/rtc.h>
#endif
#endif

#if defined(CONFIG_SLIRP)
#include "libslirp.h"
#endif

#ifdef _WIN32
#include <malloc.h>
#include <sys/timeb.h>
#include <windows.h>
#define getopt_long_only getopt_long
#define memalign(align, size) malloc(size)
#endif

#ifdef CONFIG_SDL
#ifdef __APPLE__
#include <SDL/SDL.h>
#endif
#endif /* CONFIG_SDL */

#include "exec-all.h"

//#define DO_TB_FLUSH

#define DEFAULT_NETWORK_SCRIPT "/etc/xen/qemu-ifup"

#if !defined(CONFIG_SOFTMMU)
#define PHYS_RAM_MAX_SIZE (256 * 1024 * 1024)
#else
#define PHYS_RAM_MAX_SIZE (2047 * 1024 * 1024)
#endif

#ifdef TARGET_PPC
#define DEFAULT_RAM_SIZE 144
#else
#define DEFAULT_RAM_SIZE 128
#endif
/* in ms */
#define GUI_REFRESH_INTERVAL 30
#define POLLING_INTERVAL 5

/* XXX: use a two level table to limit memory usage */
#define MAX_IOPORTS 65536

const char *bios_dir = CONFIG_QEMU_SHAREDIR;
char phys_ram_file[1024];
CPUState *global_env;
CPUState *cpu_single_env;
void *ioport_opaque[MAX_IOPORTS];
IOPortReadFunc *ioport_read_table[3][MAX_IOPORTS];
IOPortWriteFunc *ioport_write_table[3][MAX_IOPORTS];
BlockDriverState *bs_table[MAX_DISKS], *fd_table[MAX_FD];
int vga_ram_size;
int bios_size;
static DisplayState display_state;
int nographic;
int usevnc; /* 1=vnc only, 2=vnc and sdl */
long vncport; /* server port */
const char* vncconnect; /* do a reverse connect to this host*/
const char* keyboard_layout = 0;
int64_t ticks_per_sec;
int boot_device = 'c';
uint64_t ram_size;
int domid = -1;
static char network_script[1024];
int pit_min_timer_count = 0;
int nb_nics;
char bridge[16];
char domain_name[1024] = { 'H','V', 'M', 'X', 'E', 'N', '-'};
NetDriverState nd_table[MAX_NICS];
QEMUTimer *gui_timer;
QEMUTimer *polling_timer;
int vm_running;
int audio_enabled = 0;
int nic_ne2000 = 0;
int vcpus = 1;
int sb16_enabled = 1;
int adlib_enabled = 1;
int gus_enabled = 1;
int pci_enabled = 1;
int prep_enabled = 0;
int rtc_utc = 1;
int cirrus_vga_enabled = 1;
int vga_accelerate = 1;
int graphic_width = 800;
int graphic_height = 600;
int graphic_depth = 15;
int full_screen = 0;
int repeat_key = 1;
TextConsole *vga_console;
CharDriverState *serial_hds[MAX_SERIAL_PORTS];
int xc_handle;
unsigned long *vgapage_array;
unsigned long *freepage_array;
unsigned long free_pages;
void *vtop_table;
unsigned long toptab;
unsigned long vgaram_pages;

/***********************************************************/
/* x86 ISA bus support */

target_phys_addr_t isa_mem_base = 0;

uint32_t default_ioport_readb(void *opaque, uint32_t address)
{
#ifdef DEBUG_UNUSED_IOPORT
    fprintf(stderr, "inb: port=0x%04x\n", address);
#endif
    return 0xff;
}

void default_ioport_writeb(void *opaque, uint32_t address, uint32_t data)
{
#ifdef DEBUG_UNUSED_IOPORT
    fprintf(stderr, "outb: port=0x%04x data=0x%02x\n", address, data);
#endif
}

/* default is to make two byte accesses */
uint32_t default_ioport_readw(void *opaque, uint32_t address)
{
    uint32_t data;
    data = ioport_read_table[0][address](ioport_opaque[address], address);
    address = (address + 1) & (MAX_IOPORTS - 1);
    data |= ioport_read_table[0][address](ioport_opaque[address], address) << 8;
    return data;
}

void default_ioport_writew(void *opaque, uint32_t address, uint32_t data)
{
    ioport_write_table[0][address](ioport_opaque[address], address, data & 0xff);
    address = (address + 1) & (MAX_IOPORTS - 1);
    ioport_write_table[0][address](ioport_opaque[address], address, (data >> 8) & 0xff);
}

uint32_t default_ioport_readl(void *opaque, uint32_t address)
{
#ifdef DEBUG_UNUSED_IOPORT
    fprintf(stderr, "inl: port=0x%04x\n", address);
#endif
    return 0xffffffff;
}

void default_ioport_writel(void *opaque, uint32_t address, uint32_t data)
{
#ifdef DEBUG_UNUSED_IOPORT
    fprintf(stderr, "outl: port=0x%04x data=0x%02x\n", address, data);
#endif
}

void init_ioports(void)
{
    int i;

    for(i = 0; i < MAX_IOPORTS; i++) {
        ioport_read_table[0][i] = default_ioport_readb;
        ioport_write_table[0][i] = default_ioport_writeb;
        ioport_read_table[1][i] = default_ioport_readw;
        ioport_write_table[1][i] = default_ioport_writew;
        ioport_read_table[2][i] = default_ioport_readl;
        ioport_write_table[2][i] = default_ioport_writel;
    }
}

/* size is the word size in byte */
int register_ioport_read(int start, int length, int size,
                         IOPortReadFunc *func, void *opaque)
{
    int i, bsize;

    if (size == 1) {
        bsize = 0;
    } else if (size == 2) {
        bsize = 1;
    } else if (size == 4) {
        bsize = 2;
    } else {
        hw_error("register_ioport_read: invalid size");
        return -1;
    }
    for(i = start; i < start + length; i += size) {
        ioport_read_table[bsize][i] = func;
        if (ioport_opaque[i] != NULL && ioport_opaque[i] != opaque)
            hw_error("register_ioport_read: invalid opaque");
        ioport_opaque[i] = opaque;
    }
    return 0;
}

/* size is the word size in byte */
int register_ioport_write(int start, int length, int size,
                          IOPortWriteFunc *func, void *opaque)
{
    int i, bsize;

    if (size == 1) {
        bsize = 0;
    } else if (size == 2) {
        bsize = 1;
    } else if (size == 4) {
        bsize = 2;
    } else {
        hw_error("register_ioport_write: invalid size");
        return -1;
    }
    for(i = start; i < start + length; i += size) {
        ioport_write_table[bsize][i] = func;
        if (ioport_opaque[i] != NULL && ioport_opaque[i] != opaque)
            hw_error("register_ioport_read: invalid opaque");
        ioport_opaque[i] = opaque;
    }
    return 0;
}

void isa_unassign_ioport(int start, int length)
{
    int i;

    for(i = start; i < start + length; i++) {
        ioport_read_table[0][i] = default_ioport_readb;
        ioport_read_table[1][i] = default_ioport_readw;
        ioport_read_table[2][i] = default_ioport_readl;

        ioport_write_table[0][i] = default_ioport_writeb;
        ioport_write_table[1][i] = default_ioport_writew;
        ioport_write_table[2][i] = default_ioport_writel;
    }
}

void pstrcpy(char *buf, int buf_size, const char *str)
{
    int c;
    char *q = buf;

    if (buf_size <= 0)
        return;

    for(;;) {
        c = *str++;
        if (c == 0 || q >= buf + buf_size - 1)
            break;
        *q++ = c;
    }
    *q = '\0';
}

/* strcat and truncate. */
char *pstrcat(char *buf, int buf_size, const char *s)
{
    int len;
    len = strlen(buf);
    if (len < buf_size)
        pstrcpy(buf + len, buf_size - len, s);
    return buf;
}

int strstart(const char *str, const char *val, const char **ptr)
{
    const char *p, *q;
    p = str;
    q = val;
    while (*q != '\0') {
        if (*p != *q)
            return 0;
        p++;
        q++;
    }
    if (ptr)
        *ptr = p;
    return 1;
}

/* return the size or -1 if error */
int get_image_size(const char *filename)
{
    int fd, size;
    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0)
        return -1;
    size = lseek(fd, 0, SEEK_END);
    close(fd);
    return size;
}

/* return the size or -1 if error */
int load_image(const char *filename, uint8_t *addr)
{
    int fd, size;
    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0)
        return -1;
    size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (read(fd, addr, size) != size) {
        close(fd);
        return -1;
    }
    close(fd);
    return size;
}

void cpu_outb(CPUState *env, int addr, int val)
{
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "outb: %04x %02x\n", addr, val);
#endif
    ioport_write_table[0][addr](ioport_opaque[addr], addr, val);
}

void cpu_outw(CPUState *env, int addr, int val)
{
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "outw: %04x %04x\n", addr, val);
#endif
    ioport_write_table[1][addr](ioport_opaque[addr], addr, val);
}

void cpu_outl(CPUState *env, int addr, int val)
{
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "outl: %04x %08x\n", addr, val);
#endif
    ioport_write_table[2][addr](ioport_opaque[addr], addr, val);
}

int cpu_inb(CPUState *env, int addr)
{
    int val;
    val = ioport_read_table[0][addr](ioport_opaque[addr], addr);
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "inb : %04x %02x\n", addr, val);
#endif
    return val;
}

int cpu_inw(CPUState *env, int addr)
{
    int val;
    val = ioport_read_table[1][addr](ioport_opaque[addr], addr);
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "inw : %04x %04x\n", addr, val);
#endif
    return val;
}

int cpu_inl(CPUState *env, int addr)
{
    int val;
    val = ioport_read_table[2][addr](ioport_opaque[addr], addr);
#ifdef DEBUG_IOPORT
    if (loglevel & CPU_LOG_IOPORT)
        fprintf(logfile, "inl : %04x %08x\n", addr, val);
#endif
    return val;
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    if (logfile) {
        fprintf(logfile, "qemu: hardware error: ");
        vfprintf(logfile, fmt, ap);
        fprintf(logfile, "\n");
    }
    va_end(ap);
    abort();
}

/***********************************************************/
/* keyboard/mouse */

static QEMUPutKBDEvent *qemu_put_kbd_event;
static void *qemu_put_kbd_event_opaque;
static QEMUPutMouseEvent *qemu_put_mouse_event;
static void *qemu_put_mouse_event_opaque;

void qemu_add_kbd_event_handler(QEMUPutKBDEvent *func, void *opaque)
{
    qemu_put_kbd_event_opaque = opaque;
    qemu_put_kbd_event = func;
}

void qemu_add_mouse_event_handler(QEMUPutMouseEvent *func, void *opaque)
{
    qemu_put_mouse_event_opaque = opaque;
    qemu_put_mouse_event = func;
}

void kbd_put_keycode(int keycode)
{
    if (qemu_put_kbd_event) {
        qemu_put_kbd_event(qemu_put_kbd_event_opaque, keycode);
    }
}

void kbd_mouse_event(int dx, int dy, int dz, int buttons_state, int x, int y)
{
    if (qemu_put_mouse_event) {
        qemu_put_mouse_event(qemu_put_mouse_event_opaque,
                             dx, dy, dz, buttons_state, x, y);
    }
}

/***********************************************************/
/* timers */

#if defined(__powerpc__)

static inline uint32_t get_tbl(void)
{
    uint32_t tbl;
    asm volatile("mftb %0" : "=r" (tbl));
    return tbl;
}

static inline uint32_t get_tbu(void)
{
	uint32_t tbl;
	asm volatile("mftbu %0" : "=r" (tbl));
	return tbl;
}

int64_t cpu_get_real_ticks(void)
{
    uint32_t l, h, h1;
    /* NOTE: we test if wrapping has occurred */
    do {
        h = get_tbu();
        l = get_tbl();
        h1 = get_tbu();
    } while (h != h1);
    return ((int64_t)h << 32) | l;
}

#elif defined(__i386__)

int64_t cpu_get_real_ticks(void)
{
    int64_t val;
    asm volatile ("rdtsc" : "=A" (val));
    return val;
}

#elif defined(__x86_64__)

int64_t cpu_get_real_ticks(void)
{
    uint32_t low,high;
    int64_t val;
    asm volatile("rdtsc" : "=a" (low), "=d" (high));
    val = high;
    val <<= 32;
    val |= low;
    return val;
}

#elif defined(__ia64__)
#include "ia64_intrinsic.h"
#define cpu_get_real_ticks()    \
    __ia64_getreg(_IA64_REG_AR_ITC)

#else
#error unsupported CPU
#endif

static int64_t cpu_ticks_offset;
static int cpu_ticks_enabled;
int64_t cpu_virt_tsc;

static inline int64_t cpu_get_ticks(void)
{
    if (!cpu_ticks_enabled) {
        return cpu_ticks_offset;
    } else {
        return cpu_get_real_ticks() + cpu_ticks_offset;
    }

}

/* enable cpu_get_ticks() */
void cpu_enable_ticks(void)
{
    if (!cpu_ticks_enabled) {
        cpu_ticks_offset -= cpu_get_real_ticks();
        cpu_ticks_enabled = 1;
    }
}

/* disable cpu_get_ticks() : the clock is stopped. You must not call
   cpu_get_ticks() after that.  */
void cpu_disable_ticks(void)
{
    if (cpu_ticks_enabled) {
        cpu_ticks_offset = cpu_get_ticks();
        cpu_ticks_enabled = 0;
    }
}

static int64_t get_clock(void)
{
#ifdef _WIN32
    struct _timeb tb;
    _ftime(&tb);
    return ((int64_t)tb.time * 1000 + (int64_t)tb.millitm) * 1000;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
#endif
}

void cpu_calibrate_ticks(void)
{
    int64_t usec, ticks;

    usec = get_clock();
    ticks = cpu_get_real_ticks();
#ifdef _WIN32
    Sleep(50);
#else
    usleep(50 * 1000);
#endif
    usec = get_clock() - usec;
    ticks = cpu_get_real_ticks() - ticks;
    ticks_per_sec = (ticks * 1000000LL + (usec >> 1)) / usec;
}

/* compute with 96 bit intermediate result: (a*b)/c */
uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    union {
        uint64_t ll;
        struct {
#ifdef WORDS_BIGENDIAN
            uint32_t high, low;
#else
            uint32_t low, high;
#endif
        } l;
    } u, res;
    uint64_t rl, rh;

    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}

#define QEMU_TIMER_REALTIME 0
#define QEMU_TIMER_VIRTUAL  1

struct QEMUClock {
    int type;
    /* XXX: add frequency */
};

struct QEMUTimer {
    QEMUClock *clock;
    int64_t expire_time;
    QEMUTimerCB *cb;
    void *opaque;
    struct QEMUTimer *next;
};

QEMUClock *rt_clock;
QEMUClock *vm_clock;

static QEMUTimer *active_timers[2];
#ifdef _WIN32
static MMRESULT timerID;
#else
/* frequency of the times() clock tick */
static int timer_freq;
#endif

QEMUClock *qemu_new_clock(int type)
{
    QEMUClock *clock;
    clock = qemu_mallocz(sizeof(QEMUClock));
    if (!clock)
        return NULL;
    clock->type = type;
    return clock;
}

QEMUTimer *qemu_new_timer(QEMUClock *clock, QEMUTimerCB *cb, void *opaque)
{
    QEMUTimer *ts;

    ts = qemu_mallocz(sizeof(QEMUTimer));
    ts->clock = clock;
    ts->cb = cb;
    ts->opaque = opaque;
    return ts;
}

void qemu_free_timer(QEMUTimer *ts)
{
    qemu_free(ts);
}

/* stop a timer, but do not dealloc it */
void qemu_del_timer(QEMUTimer *ts)
{
    QEMUTimer **pt, *t;

    /* NOTE: this code must be signal safe because
       qemu_timer_expired() can be called from a signal. */
    pt = &active_timers[ts->clock->type];
    for(;;) {
        t = *pt;
        if (!t)
            break;
        if (t == ts) {
            *pt = t->next;
            break;
        }
        pt = &t->next;
    }
}

/* modify the current timer so that it will be fired when current_time
   >= expire_time. The corresponding callback will be called. */
void qemu_mod_timer(QEMUTimer *ts, int64_t expire_time)
{
    QEMUTimer **pt, *t;

    qemu_del_timer(ts);

    /* add the timer in the sorted list */
    /* NOTE: this code must be signal safe because
       qemu_timer_expired() can be called from a signal. */
    pt = &active_timers[ts->clock->type];
    for(;;) {
        t = *pt;
        if (!t)
            break;
        if (t->expire_time > expire_time)
            break;
        pt = &t->next;
    }
    ts->expire_time = expire_time;
    ts->next = *pt;
    *pt = ts;
}

int qemu_timer_pending(QEMUTimer *ts)
{
    QEMUTimer *t;
    for(t = active_timers[ts->clock->type]; t != NULL; t = t->next) {
        if (t == ts)
            return 1;
    }
    return 0;
}

static inline int qemu_timer_expired(QEMUTimer *timer_head, int64_t current_time)
{
    if (!timer_head)
        return 0;
    return (timer_head->expire_time <= current_time);
}

static void qemu_run_timers(QEMUTimer **ptimer_head, int64_t current_time)
{
    QEMUTimer *ts;

    for(;;) {
        ts = *ptimer_head;
        if (!ts || ts->expire_time > current_time)
            break;
        /* remove timer from the list before calling the callback */
        *ptimer_head = ts->next;
        ts->next = NULL;

        /* run the callback (the timer list can be modified) */
        ts->cb(ts->opaque);
    }
}

int64_t qemu_get_clock(QEMUClock *clock)
{
    switch(clock->type) {
    case QEMU_TIMER_REALTIME:
#ifdef _WIN32
        return GetTickCount();
#else
        {
            struct tms tp;

            /* Note that using gettimeofday() is not a good solution
               for timers because its value change when the date is
               modified. */
            if (timer_freq == 100) {
                return times(&tp) * 10;
            } else {
                return ((int64_t)times(&tp) * 1000) / timer_freq;
            }
        }
#endif
    default:
    case QEMU_TIMER_VIRTUAL:
        return cpu_get_ticks();
    }
}

/* save a timer */
void qemu_put_timer(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    if (qemu_timer_pending(ts)) {
        expire_time = ts->expire_time;
    } else {
        expire_time = -1;
    }
    qemu_put_be64(f, expire_time);
}

void qemu_get_timer(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = qemu_get_be64(f);
    if (expire_time != -1) {
        qemu_mod_timer(ts, expire_time);
    } else {
        qemu_del_timer(ts);
    }
}

static void init_timers(void)
{
    rt_clock = qemu_new_clock(QEMU_TIMER_REALTIME);
    vm_clock = qemu_new_clock(QEMU_TIMER_VIRTUAL);

#ifdef _WIN32
    {
        int count=0;
        timerID = timeSetEvent(10,    // interval (ms)
                               0,     // resolution
                               host_alarm_handler, // function
                               (DWORD)&count,  // user parameter
                               TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
        if( !timerID ) {
            fprintf(logfile, "failed timer alarm");
            exit(1);
        }
    }
    pit_min_timer_count = ((uint64_t)10000 * PIT_FREQ) / 1000000;
#else
    {
        /* get times() syscall frequency */
        timer_freq = sysconf(_SC_CLK_TCK);
      
#ifndef TARGET_HVM
        /* timer signal */
        sigfillset(&act.sa_mask);
        act.sa_flags = 0;
#if defined (TARGET_I386) && defined(USE_CODE_COPY)
        act.sa_flags |= SA_ONSTACK;
#endif
        act.sa_handler = host_alarm_handler;
        sigaction(SIGALRM, &act, NULL);

        itv.it_interval.tv_sec = 0;
        itv.it_interval.tv_usec = 1000;
        itv.it_value.tv_sec = 0;
        itv.it_value.tv_usec = 10 * 1000;
        setitimer(ITIMER_REAL, &itv, NULL);
        /* we probe the tick duration of the kernel to inform the user if
           the emulated kernel requested a too high timer frequency */
        getitimer(ITIMER_REAL, &itv);

#if defined(__linux__)
        if (itv.it_interval.tv_usec > 1000) {
            /* try to use /dev/rtc to have a faster timer */
            if (start_rtc_timer() < 0)
                goto use_itimer;
            /* disable itimer */
            itv.it_interval.tv_sec = 0;
            itv.it_interval.tv_usec = 0;
            itv.it_value.tv_sec = 0;
            itv.it_value.tv_usec = 0;
            setitimer(ITIMER_REAL, &itv, NULL);

            /* use the RTC */
            sigaction(SIGIO, &act, NULL);
            fcntl(rtc_fd, F_SETFL, O_ASYNC);
            fcntl(rtc_fd, F_SETOWN, getpid());
        } else
#endif /* defined(__linux__) */
        {
        use_itimer:
            pit_min_timer_count = ((uint64_t)itv.it_interval.tv_usec *
                                   PIT_FREQ) / 1000000;
        }
#endif /* TARGET_HVM */
    }
#endif
}

void quit_timers(void)
{
#ifdef _WIN32
    timeKillEvent(timerID);
#endif
}

/***********************************************************/
/* character device */

int qemu_chr_write(CharDriverState *s, const uint8_t *buf, int len)
{
    return s->chr_write(s, buf, len);
}

void qemu_chr_printf(CharDriverState *s, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    qemu_chr_write(s, buf, strlen(buf));
    va_end(ap);
}

void qemu_chr_send_event(CharDriverState *s, int event)
{
    if (s->chr_send_event)
        s->chr_send_event(s, event);
}

void qemu_chr_add_read_handler(CharDriverState *s,
                               IOCanRWHandler *fd_can_read,
                               IOReadHandler *fd_read, void *opaque)
{
    s->chr_add_read_handler(s, fd_can_read, fd_read, opaque);
}

void qemu_chr_add_event_handler(CharDriverState *s, IOEventHandler *chr_event)
{
    s->chr_event = chr_event;
}

static int null_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    return len;
}

static void null_chr_add_read_handler(CharDriverState *chr,
                                    IOCanRWHandler *fd_can_read,
                                    IOReadHandler *fd_read, void *opaque)
{
}

CharDriverState *qemu_chr_open_null(void)
{
    CharDriverState *chr;

    chr = qemu_mallocz(sizeof(CharDriverState));
    if (!chr)
        return NULL;
    chr->chr_write = null_chr_write;
    chr->chr_add_read_handler = null_chr_add_read_handler;
    return chr;
}

#ifndef _WIN32

typedef struct {
    int fd_in, fd_out;
    /* for nographic stdio only */
    IOCanRWHandler *fd_can_read;
    IOReadHandler *fd_read;
    void *fd_opaque;
} FDCharDriver;

#define STDIO_MAX_CLIENTS 2

static int stdio_nb_clients;
static CharDriverState *stdio_clients[STDIO_MAX_CLIENTS];

static int unix_write(int fd, const uint8_t *buf, int len1)
{
    int ret,sel_ret,len;
    int max_fd;
    fd_set writefds;
    struct timeval timeout;

    max_fd = fd;  

    len = len1;
    while (len > 0) {
        FD_ZERO(&writefds);
        FD_SET(fd, &writefds);
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	sel_ret = select(max_fd + 1, NULL, &writefds, 0, &timeout);
	if (sel_ret <= 0) {
		/* Timeout or select error */
            return -1;
	} else {
            ret = write(fd, buf, len);
            if (ret < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    return -1;
            } else if (ret == 0) {
            	    break;
            } else {
            	    buf += ret;
                    len -= ret;
            }
        }
    }
    return len1 - len;
}

static int fd_chr_write(CharDriverState *chr, const uint8_t *buf, int len)
{
    FDCharDriver *s = chr->opaque;
    return unix_write(s->fd_out, buf, len);
}

static void fd_chr_add_read_handler(CharDriverState *chr,
                                    IOCanRWHandler *fd_can_read,
                                    IOReadHandler *fd_read, void *opaque)
{
    FDCharDriver *s = chr->opaque;

    if (nographic && s->fd_in == 0) {
        s->fd_can_read = fd_can_read;
        s->fd_read = fd_read;
        s->fd_opaque = opaque;
    } else {
        qemu_add_fd_read_handler(s->fd_in, fd_can_read, fd_read, opaque);
    }
}

/* open a character device to a unix fd */
CharDriverState *qemu_chr_open_fd(int fd_in, int fd_out)
{
    CharDriverState *chr;
    FDCharDriver *s;

    chr = qemu_mallocz(sizeof(CharDriverState));
    if (!chr)
        return NULL;
    s = qemu_mallocz(sizeof(FDCharDriver));
    if (!s) {
        free(chr);
        return NULL;
    }
    s->fd_in = fd_in;
    s->fd_out = fd_out;
    chr->opaque = s;
    chr->chr_write = fd_chr_write;
    chr->chr_add_read_handler = fd_chr_add_read_handler;
    return chr;
}

/* for STDIO, we handle the case where several clients use it
   (nographic mode) */

#define TERM_ESCAPE 0x01 /* ctrl-a is used for escape */

static int term_got_escape, client_index;

void term_print_help(void)
{
    printf("\n"
           "C-a h    print this help\n"
           "C-a x    exit emulator\n"
           "C-a s    save disk data back to file (if -snapshot)\n"
           "C-a b    send break (magic sysrq)\n"
           "C-a c    switch between console and monitor\n"
           "C-a C-a  send C-a\n"
           );
}

/* called when a char is received */
static void stdio_received_byte(int ch)
{
    if (term_got_escape) {
        term_got_escape = 0;
        switch(ch) {
        case 'h':
            term_print_help();
            break;
        case 'x':
            exit(0);
            break;
        case 's':
            {
                int i;
                for (i = 0; i < MAX_DISKS; i++) {
                    if (bs_table[i])
                        bdrv_commit(bs_table[i]);
                }
            }
            break;
        case 'b':
            if (client_index < stdio_nb_clients) {
                CharDriverState *chr;
                FDCharDriver *s;

                chr = stdio_clients[client_index];
                s = chr->opaque;
                chr->chr_event(s->fd_opaque, CHR_EVENT_BREAK);
            }
            break;
        case 'c':
            client_index++;
            if (client_index >= stdio_nb_clients)
                client_index = 0;
            if (client_index == 0) {
                /* send a new line in the monitor to get the prompt */
                ch = '\r';
                goto send_char;
            }
            break;
        case TERM_ESCAPE:
            goto send_char;
        }
    } else if (ch == TERM_ESCAPE) {
        term_got_escape = 1;
    } else {
    send_char:
        if (client_index < stdio_nb_clients) {
            uint8_t buf[1];
            CharDriverState *chr;
            FDCharDriver *s;

            chr = stdio_clients[client_index];
            s = chr->opaque;
            buf[0] = ch;
            /* XXX: should queue the char if the device is not
               ready */
            if (s->fd_can_read(s->fd_opaque) > 0)
                s->fd_read(s->fd_opaque, buf, 1);
        }
    }
}

static int stdio_can_read(void *opaque)
{
    /* XXX: not strictly correct */
    return 1;
}

static void stdio_read(void *opaque, const uint8_t *buf, int size)
{
    int i;
    for(i = 0; i < size; i++)
        stdio_received_byte(buf[i]);
}

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int old_fd0_flags;

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
    fcntl(0, F_SETFL, old_fd0_flags);
}

static void term_init(void)
{
    struct termios tty;

    tcgetattr (0, &tty);
    oldtty = tty;
    old_fd0_flags = fcntl(0, F_GETFL);

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    /* if graphical mode, we allow Ctrl-C handling */
    if (nographic)
        tty.c_lflag &= ~ISIG;
    tty.c_cflag &= ~(CSIZE|PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr (0, TCSANOW, &tty);

    atexit(term_exit);

    fcntl(0, F_SETFL, O_NONBLOCK);
}

CharDriverState *qemu_chr_open_stdio(void)
{
    CharDriverState *chr;

    if (nographic) {
        if (stdio_nb_clients >= STDIO_MAX_CLIENTS)
            return NULL;
        chr = qemu_chr_open_fd(0, 1);
        if (stdio_nb_clients == 0)
            qemu_add_fd_read_handler(0, stdio_can_read, stdio_read, NULL);
        client_index = stdio_nb_clients;
    } else {
        if (stdio_nb_clients != 0)
            return NULL;
        chr = qemu_chr_open_fd(0, 1);
    }
    stdio_clients[stdio_nb_clients++] = chr;
    if (stdio_nb_clients == 1) {
        /* set the terminal in raw mode */
        term_init();
    }
    return chr;
}

int store_console_dev(int domid, char *pts)
{
    int xc_handle;
    struct xs_handle *xs;
    char *path;

    xs = xs_daemon_open();
    if (xs == NULL) {
        fprintf(logfile, "Could not contact XenStore\n");
        return -1;
    }

    xc_handle = xc_interface_open();
    if (xc_handle == -1) {
        fprintf(logfile, "xc_interface_open() error\n");
        return -1;
    }

    path = xs_get_domain_path(xs, domid);
    if (path == NULL) {
        fprintf(logfile, "xs_get_domain_path() error\n");
        return -1;
    }
    path = realloc(path, strlen(path) + strlen("/console/tty") + 1);
    if (path == NULL) {
        fprintf(logfile, "realloc error\n");
        return -1;
    }
    strcat(path, "/console/tty");
    if (!xs_write(xs, XBT_NULL, path, pts, strlen(pts))) {
        fprintf(logfile, "xs_write for console fail");
        return -1;
    }

    free(path);
    xs_daemon_close(xs);
    close(xc_handle);

    return 0;
}

#if defined(__linux__)
CharDriverState *qemu_chr_open_pty(void)
{
    int master_fd, slave_fd;
    struct termios term;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) < 0)
        return NULL;

    /* Set raw attributes on the pty. */
    cfmakeraw(&term);
    tcsetattr(slave_fd, TCSAFLUSH, &term);

    fprintf(stderr, "char device redirected to %s\n", ptsname(master_fd));
    store_console_dev(domid, ptsname(master_fd));

    return qemu_chr_open_fd(master_fd, master_fd);
}
#else
CharDriverState *qemu_chr_open_pty(void)
{
    return NULL;
}
#endif

#endif /* !defined(_WIN32) */

CharDriverState *qemu_chr_open(const char *filename)
{
    if (!strcmp(filename, "vc")) {
        return text_console_init(&display_state);
    } else if (!strcmp(filename, "null")) {
        return qemu_chr_open_null();
    } else
#ifndef _WIN32
    if (!strcmp(filename, "pty")) {
        return qemu_chr_open_pty();
    } else if (!strcmp(filename, "stdio")) {
        return qemu_chr_open_stdio();
    } else
#endif
    {
        return NULL;
    }
}

/***********************************************************/
/* Linux network device redirectors */

void hex_dump(FILE *f, const uint8_t *buf, int size)
{
    int len, i, j, c;

    for(i=0;i<size;i+=16) {
        len = size - i;
        if (len > 16)
            len = 16;
        fprintf(f, "%08x ", i);
        for(j=0;j<16;j++) {
            if (j < len)
                fprintf(f, " %02x", buf[i+j]);
            else
                fprintf(f, "   ");
        }
        fprintf(f, " ");
        for(j=0;j<len;j++) {
            c = buf[i+j];
            if (c < ' ' || c > '~')
                c = '.';
            fprintf(f, "%c", c);
        }
        fprintf(f, "\n");
    }
}

void qemu_send_packet(NetDriverState *nd, const uint8_t *buf, int size)
{
    nd->send_packet(nd, buf, size);
}

void qemu_add_read_packet(NetDriverState *nd, IOCanRWHandler *fd_can_read,
                          IOReadHandler *fd_read, void *opaque)
{
    nd->add_read_packet(nd, fd_can_read, fd_read, opaque);
}

/* dummy network adapter */

static void dummy_send_packet(NetDriverState *nd, const uint8_t *buf, int size)
{
}

static void dummy_add_read_packet(NetDriverState *nd,
                                  IOCanRWHandler *fd_can_read,
                                  IOReadHandler *fd_read, void *opaque)
{
}

static int net_dummy_init(NetDriverState *nd)
{
    nd->send_packet = dummy_send_packet;
    nd->add_read_packet = dummy_add_read_packet;
    pstrcpy(nd->ifname, sizeof(nd->ifname), "dummy");
    return 0;
}

#if defined(CONFIG_SLIRP)

/* slirp network adapter */

static void *slirp_fd_opaque;
static IOCanRWHandler *slirp_fd_can_read;
static IOReadHandler *slirp_fd_read;
static int slirp_inited;

int slirp_can_output(void)
{
    return slirp_fd_can_read(slirp_fd_opaque);
}

void slirp_output(const uint8_t *pkt, int pkt_len)
{
#if 0
    printf("output:\n");
    hex_dump(stdout, pkt, pkt_len);
#endif
    slirp_fd_read(slirp_fd_opaque, pkt, pkt_len);
}

static void slirp_send_packet(NetDriverState *nd, const uint8_t *buf, int size)
{
#if 0
    printf("input:\n");
    hex_dump(stdout, buf, size);
#endif
    slirp_input(buf, size);
}

static void slirp_add_read_packet(NetDriverState *nd,
                                  IOCanRWHandler *fd_can_read,
                                  IOReadHandler *fd_read, void *opaque)
{
    slirp_fd_opaque = opaque;
    slirp_fd_can_read = fd_can_read;
    slirp_fd_read = fd_read;
}

static int net_slirp_init(NetDriverState *nd)
{
    if (!slirp_inited) {
        slirp_inited = 1;
        slirp_init();
    }
    nd->send_packet = slirp_send_packet;
    nd->add_read_packet = slirp_add_read_packet;
    pstrcpy(nd->ifname, sizeof(nd->ifname), "slirp");
    return 0;
}

static int get_str_sep(char *buf, int buf_size, const char **pp, int sep)
{
    const char *p, *p1;
    int len;
    p = *pp;
    p1 = strchr(p, sep);
    if (!p1)
        return -1;
    len = p1 - p;
    p1++;
    if (buf_size > 0) {
        if (len > buf_size - 1)
            len = buf_size - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    *pp = p1;
    return 0;
}

static void net_slirp_redir(const char *redir_str)
{
    int is_udp;
    char buf[256], *r;
    const char *p;
    struct in_addr guest_addr;
    int host_port, guest_port;

    if (!slirp_inited) {
        slirp_inited = 1;
        slirp_init();
    }

    p = redir_str;
    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0)
        goto fail;
    if (!strcmp(buf, "tcp")) {
        is_udp = 0;
    } else if (!strcmp(buf, "udp")) {
        is_udp = 1;
    } else {
        goto fail;
    }

    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0)
        goto fail;
    host_port = strtol(buf, &r, 0);
    if (r == buf)
        goto fail;

    if (get_str_sep(buf, sizeof(buf), &p, ':') < 0)
        goto fail;
    if (buf[0] == '\0') {
        pstrcpy(buf, sizeof(buf), "10.0.2.15");
    }
    if (!inet_aton(buf, &guest_addr))
        goto fail;

    guest_port = strtol(p, &r, 0);
    if (r == p)
        goto fail;

    if (slirp_redir(is_udp, host_port, guest_addr, guest_port) < 0) {
        fprintf(stderr, "qemu: could not set up redirection\n");
        exit(1);
    }
    return;
 fail:
    fprintf(stderr, "qemu: syntax: -redir [tcp|udp]:host-port:[guest-host]:guest-port\n");
    exit(1);
}

#ifndef _WIN32

char smb_dir[1024];

static void smb_exit(void)
{
    DIR *d;
    struct dirent *de;
    char filename[1024];

    /* erase all the files in the directory */
    d = opendir(smb_dir);
    for(;;) {
        de = readdir(d);
        if (!de)
            break;
        if (strcmp(de->d_name, ".") != 0 &&
            strcmp(de->d_name, "..") != 0) {
            snprintf(filename, sizeof(filename), "%s/%s",
                     smb_dir, de->d_name);
            unlink(filename);
        }
    }
    closedir(d);
    rmdir(smb_dir);
}

/* automatic user mode samba server configuration */
void net_slirp_smb(const char *exported_dir)
{
    char smb_conf[1024];
    char smb_cmdline[1024];
    FILE *f;

    if (!slirp_inited) {
        slirp_inited = 1;
        slirp_init();
    }

    /* XXX: better tmp dir construction */
    snprintf(smb_dir, sizeof(smb_dir), "/tmp/qemu-smb.%d", getpid());
    if (mkdir(smb_dir, 0700) < 0) {
        fprintf(stderr, "qemu: could not create samba server dir '%s'\n", smb_dir);
        exit(1);
    }
    snprintf(smb_conf, sizeof(smb_conf), "%s/%s", smb_dir, "smb.conf");

    f = fopen(smb_conf, "w");
    if (!f) {
        fprintf(stderr, "qemu: could not create samba server configuration file '%s'\n", smb_conf);
        exit(1);
    }
    fprintf(f,
            "[global]\n"
            "pid directory=%s\n"
            "lock directory=%s\n"
            "log file=%s/log.smbd\n"
            "smb passwd file=%s/smbpasswd\n"
            "security = share\n"
            "[qemu]\n"
            "path=%s\n"
            "read only=no\n"
            "guest ok=yes\n",
            smb_dir,
            smb_dir,
            smb_dir,
            smb_dir,
            exported_dir
            );
    fclose(f);
    atexit(smb_exit);

    snprintf(smb_cmdline, sizeof(smb_cmdline), "/usr/sbin/smbd -s %s",
             smb_conf);

    slirp_add_exec(0, smb_cmdline, 4, 139);
}

#endif /* !defined(_WIN32) */

#endif /* CONFIG_SLIRP */

#if !defined(_WIN32)
#ifdef _BSD
static int tun_open(char *ifname, int ifname_size)
{
    int fd;
    char *dev;
    struct stat s;

    fd = open("/dev/tap", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "warning: could not open /dev/tap: no virtual network emulation\n");
        return -1;
    }

    fstat(fd, &s);
    dev = devname(s.st_rdev, S_IFCHR);
    pstrcpy(ifname, ifname_size, dev);

    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}
#else
static int tun_open(char *ifname, int ifname_size)
{
    struct ifreq ifr;
    int fd, ret;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "warning: could not open /dev/net/tun: no virtual network emulation\n");
        return -1;
    }
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    pstrcpy(ifr.ifr_name, IFNAMSIZ, "tun%d");
    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
    if (ret != 0) {
        fprintf(stderr, "warning: could not configure /dev/net/tun: no virtual network emulation\n");
        close(fd);
        return -1;
    }
    fprintf(logfile, "Connected to host network interface: %s\n", ifr.ifr_name);
    pstrcpy(ifname, ifname_size, ifr.ifr_name);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}
#endif

static void tun_send_packet(NetDriverState *nd, const uint8_t *buf, int size)
{
    write(nd->fd, buf, size);
}

static void tun_add_read_packet(NetDriverState *nd,
                                IOCanRWHandler *fd_can_read,
                                IOReadHandler *fd_read, void *opaque)
{
    qemu_add_fd_event_read_handler(nd->fd, fd_can_read, fd_read, opaque);
}

static int net_tun_init(NetDriverState *nd)
{
    int pid, status;
    char *args[4];
    char **parg;
    extern int highest_fds;

    nd->fd = tun_open(nd->ifname, sizeof(nd->ifname));
    if (nd->fd < 0)
        return -1;

    if ( nd->fd > highest_fds ) highest_fds = nd->fd;
    /* try to launch network init script */
    pid = fork();
    if (pid >= 0) {
        if (pid == 0) {
            parg = args;
            *parg++ = network_script;
            *parg++ = nd->ifname;
            *parg++ = bridge;
            *parg++ = NULL;
            execv(network_script, args);
            exit(1);
        }
        while (waitpid(pid, &status, 0) != pid);
        if (!WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            fprintf(stderr, "%s: could not launch network script\n",
                    network_script);
        }
    }
    nd->send_packet = tun_send_packet;
    nd->add_read_packet = tun_add_read_packet;
    return 0;
}

static int net_fd_init(NetDriverState *nd, int fd)
{
    nd->fd = fd;
    nd->send_packet = tun_send_packet;
    nd->add_read_packet = tun_add_read_packet;
    pstrcpy(nd->ifname, sizeof(nd->ifname), "tunfd");
    return 0;
}

#endif /* !_WIN32 */

/***********************************************************/
/* dumb display */

static void dumb_update(DisplayState *ds, int x, int y, int w, int h)
{
}

static void dumb_resize(DisplayState *ds, int w, int h)
{
}

static void dumb_refresh(DisplayState *ds)
{
    vga_update_display();
}

void dumb_display_init(DisplayState *ds)
{
    ds->data = NULL;
    ds->linesize = 0;
    ds->depth = 0;
    ds->dpy_update = dumb_update;
    ds->dpy_resize = dumb_resize;
    ds->dpy_refresh = dumb_refresh;
}

#if !defined(CONFIG_SOFTMMU)
/***********************************************************/
/* cpu signal handler */
static void host_segv_handler(int host_signum, siginfo_t *info,
                              void *puc)
{
    abort();
}
#endif

/***********************************************************/
/* I/O handling */

#define MAX_IO_HANDLERS 64

typedef struct IOHandlerRecord {
    int fd;
    IOCanRWHandler *fd_can_read;
    IOReadHandler *fd_read;
    void *opaque;
    /* temporary data */
    struct pollfd *ufd;
    int max_size;
    struct IOHandlerRecord *next;
} IOHandlerRecord;

static IOHandlerRecord *first_io_handler;
static IOHandlerRecord *first_eventio_handler;

int qemu_add_fd_read_handler(int fd, IOCanRWHandler *fd_can_read,
                             IOReadHandler *fd_read, void *opaque)
{
    IOHandlerRecord *ioh;

    ioh = qemu_mallocz(sizeof(IOHandlerRecord));
    if (!ioh)
        return -1;
    ioh->fd = fd;
    ioh->fd_can_read = fd_can_read;
    ioh->fd_read = fd_read;
    ioh->opaque = opaque;
    ioh->next = first_io_handler;
    first_io_handler = ioh;
    return 0;
}

int qemu_add_fd_event_read_handler(int fd, IOCanRWHandler *fd_can_read,
                             IOReadHandler *fd_read, void *opaque)
{
    IOHandlerRecord *ioh;

    ioh = qemu_mallocz(sizeof(IOHandlerRecord));
    if (!ioh)
        return -1;
    ioh->fd = fd;
    ioh->fd_can_read = fd_can_read;
    ioh->fd_read = fd_read;
    ioh->opaque = opaque;
    ioh->next = first_eventio_handler;
    first_eventio_handler = ioh;
    return 0;
}

void qemu_del_fd_read_handler(int fd)
{
    IOHandlerRecord **pioh, *ioh;

    pioh = &first_io_handler;
    for(;;) {
        ioh = *pioh;
        if (ioh == NULL)
            break;
        if (ioh->fd == fd) {
            *pioh = ioh->next;
            break;
        }
        pioh = &ioh->next;
    }
}

/***********************************************************/
/* savevm/loadvm support */

void qemu_put_buffer(QEMUFile *f, const uint8_t *buf, int size)
{
    fwrite(buf, 1, size, f);
}

void qemu_put_byte(QEMUFile *f, int v)
{
    fputc(v, f);
}

void qemu_put_be16(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be32(QEMUFile *f, unsigned int v)
{
    qemu_put_byte(f, v >> 24);
    qemu_put_byte(f, v >> 16);
    qemu_put_byte(f, v >> 8);
    qemu_put_byte(f, v);
}

void qemu_put_be64(QEMUFile *f, uint64_t v)
{
    qemu_put_be32(f, v >> 32);
    qemu_put_be32(f, v);
}

int qemu_get_buffer(QEMUFile *f, uint8_t *buf, int size)
{
    return fread(buf, 1, size, f);
}

int qemu_get_byte(QEMUFile *f)
{
    int v;
    v = fgetc(f);
    if (v == EOF)
        return 0;
    else
        return v;
}

unsigned int qemu_get_be16(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

unsigned int qemu_get_be32(QEMUFile *f)
{
    unsigned int v;
    v = qemu_get_byte(f) << 24;
    v |= qemu_get_byte(f) << 16;
    v |= qemu_get_byte(f) << 8;
    v |= qemu_get_byte(f);
    return v;
}

uint64_t qemu_get_be64(QEMUFile *f)
{
    uint64_t v;
    v = (uint64_t)qemu_get_be32(f) << 32;
    v |= qemu_get_be32(f);
    return v;
}

int64_t qemu_ftell(QEMUFile *f)
{
    return ftell(f);
}

int64_t qemu_fseek(QEMUFile *f, int64_t pos, int whence)
{
    if (fseek(f, pos, whence) < 0)
        return -1;
    return ftell(f);
}

typedef struct SaveStateEntry {
    char idstr[256];
    int instance_id;
    int version_id;
    SaveStateHandler *save_state;
    LoadStateHandler *load_state;
    void *opaque;
    struct SaveStateEntry *next;
} SaveStateEntry;

static SaveStateEntry *first_se;

int register_savevm(const char *idstr,
                    int instance_id,
                    int version_id,
                    SaveStateHandler *save_state,
                    LoadStateHandler *load_state,
                    void *opaque)
{
    SaveStateEntry *se, **pse;

    se = qemu_malloc(sizeof(SaveStateEntry));
    if (!se)
        return -1;
    pstrcpy(se->idstr, sizeof(se->idstr), idstr);
    se->instance_id = instance_id;
    se->version_id = version_id;
    se->save_state = save_state;
    se->load_state = load_state;
    se->opaque = opaque;
    se->next = NULL;

    /* add at the end of list */
    pse = &first_se;
    while (*pse != NULL)
        pse = &(*pse)->next;
    *pse = se;
    return 0;
}

#define QEMU_VM_FILE_MAGIC   0x5145564d
#define QEMU_VM_FILE_VERSION 0x00000001

int qemu_savevm(const char *filename)
{
    SaveStateEntry *se;
    QEMUFile *f;
    int len, len_pos, cur_pos, saved_vm_running, ret;

    saved_vm_running = vm_running;
    vm_stop(0);

    f = fopen(filename, "wb");
    if (!f) {
        ret = -1;
        goto the_end;
    }

    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);

    for(se = first_se; se != NULL; se = se->next) {
        /* ID string */
        len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);

        /* record size: filled later */
        len_pos = ftell(f);
        qemu_put_be32(f, 0);

        se->save_state(f, se->opaque);

        /* fill record size */
        cur_pos = ftell(f);
        len = ftell(f) - len_pos - 4;
        fseek(f, len_pos, SEEK_SET);
        qemu_put_be32(f, len);
        fseek(f, cur_pos, SEEK_SET);
    }

    fclose(f);
    ret = 0;
 the_end:
    if (saved_vm_running)
        vm_start();
    return ret;
}

static SaveStateEntry *find_se(const char *idstr, int instance_id)
{
    SaveStateEntry *se;

    for(se = first_se; se != NULL; se = se->next) {
        if (!strcmp(se->idstr, idstr) &&
            instance_id == se->instance_id)
            return se;
    }
    return NULL;
}

int qemu_loadvm(const char *filename)
{
    SaveStateEntry *se;
    QEMUFile *f;
    int len, cur_pos, ret, instance_id, record_len, version_id;
    int saved_vm_running;
    unsigned int v;
    char idstr[256];

    saved_vm_running = vm_running;
    vm_stop(0);

    f = fopen(filename, "rb");
    if (!f) {
        ret = -1;
        goto the_end;
    }

    v = qemu_get_be32(f);
    if (v != QEMU_VM_FILE_MAGIC)
        goto fail;
    v = qemu_get_be32(f);
    if (v != QEMU_VM_FILE_VERSION) {
    fail:
        fclose(f);
        ret = -1;
        goto the_end;
    }
    for(;;) {
#if defined (DO_TB_FLUSH)
        tb_flush(global_env);
#endif
        len = qemu_get_byte(f);
        if (feof(f))
            break;
        qemu_get_buffer(f, idstr, len);
        idstr[len] = '\0';
        instance_id = qemu_get_be32(f);
        version_id = qemu_get_be32(f);
        record_len = qemu_get_be32(f);
#if 0
        printf("idstr=%s instance=0x%x version=%d len=%d\n",
               idstr, instance_id, version_id, record_len);
#endif
        cur_pos = ftell(f);
        se = find_se(idstr, instance_id);
        if (!se) {
            fprintf(stderr, "qemu: warning: instance 0x%x of device '%s' not present in current VM\n",
                    instance_id, idstr);
        } else {
            ret = se->load_state(f, se->opaque, version_id);
            if (ret < 0) {
                fprintf(stderr, "qemu: warning: error while loading state for instance 0x%x of device '%s'\n",
                        instance_id, idstr);
            }
        }
        /* always seek to exact end of record */
        qemu_fseek(f, cur_pos + record_len, SEEK_SET);
    }
    fclose(f);
    ret = 0;
 the_end:
    if (saved_vm_running)
        vm_start();
    return ret;
}

/***********************************************************/
/* main execution loop */

void gui_update(void *opaque)
{
    display_state.dpy_refresh(&display_state);
    qemu_mod_timer(gui_timer, GUI_REFRESH_INTERVAL + qemu_get_clock(rt_clock));
}
void polling_handler(void *opaque)
{
#ifndef _WIN32
    struct pollfd ufds[MAX_IO_HANDLERS + 1], *pf;
    IOHandlerRecord *ioh, *ioh_next;
    uint8_t buf[4096];
    int n, max_size;
#endif
    int timeout = 0;
    int ret;

#ifdef _WIN32
        if (timeout > 0)
            Sleep(timeout);
#else
        /* poll any events */
        /* XXX: separate device handlers from system ones */
        pf = ufds;
        for(ioh = first_io_handler; ioh != NULL; ioh = ioh->next) {
            if (!ioh->fd_can_read) {
                max_size = 0;
                pf->fd = ioh->fd;
                pf->events = POLLIN;
                ioh->ufd = pf;
                pf++;
            } else {
                max_size = ioh->fd_can_read(ioh->opaque);
                if (max_size > 0) {
                    if (max_size > sizeof(buf))
                        max_size = sizeof(buf);
                    pf->fd = ioh->fd;
                    pf->events = POLLIN;
                    ioh->ufd = pf;
                    pf++;
                } else {
                    ioh->ufd = NULL;
                }
            }
            ioh->max_size = max_size;
        }

        ret = poll(ufds, pf - ufds, timeout);
        if (ret > 0) {
            /* XXX: better handling of removal */
            for(ioh = first_io_handler; ioh != NULL; ioh = ioh_next) {
                ioh_next = ioh->next;
                pf = ioh->ufd;
                if (pf) {
                    if (pf->revents & POLLIN) {
                        if (ioh->max_size == 0) {
                            /* just a read event */
                            ioh->fd_read(ioh->opaque, NULL, 0);
                        } else {
                            n = read(ioh->fd, buf, ioh->max_size);
                            if (n >= 0) {
                                ioh->fd_read(ioh->opaque, buf, n);
                            } else if (errno != EAGAIN) {
                                ioh->fd_read(ioh->opaque, NULL, -errno);
                            }
                        }
                    }
                }
            }
        }
#endif /* !defined(_WIN32) */

    qemu_mod_timer(polling_timer, POLLING_INTERVAL + qemu_get_clock(rt_clock));
}


/* XXX: support several handlers */
VMStopHandler *vm_stop_cb;
VMStopHandler *vm_stop_opaque;

int qemu_add_vm_stop_handler(VMStopHandler *cb, void *opaque)
{
    vm_stop_cb = cb;
    vm_stop_opaque = opaque;
    return 0;
}

void qemu_del_vm_stop_handler(VMStopHandler *cb, void *opaque)
{
    vm_stop_cb = NULL;
}

void vm_start(void)
{
    if (!vm_running) {
        cpu_enable_ticks();
        vm_running = 1;
    }
}

void vm_stop(int reason)
{
    if (vm_running) {
        cpu_disable_ticks();
        vm_running = 0;
        if (reason != 0) {
            if (vm_stop_cb) {
                vm_stop_cb(vm_stop_opaque, reason);
            }
        }
    }
}

/* reset/shutdown handler */

typedef struct QEMUResetEntry {
    QEMUResetHandler *func;
    void *opaque;
    struct QEMUResetEntry *next;
} QEMUResetEntry;

static QEMUResetEntry *first_reset_entry;
int reset_requested;
int shutdown_requested;

void qemu_register_reset(QEMUResetHandler *func, void *opaque)
{
    QEMUResetEntry **pre, *re;

    pre = &first_reset_entry;
    while (*pre != NULL)
        pre = &(*pre)->next;
    re = qemu_mallocz(sizeof(QEMUResetEntry));
    re->func = func;
    re->opaque = opaque;
    re->next = NULL;
    *pre = re;
}

void qemu_system_reset(void)
{
    QEMUResetEntry *re;

    /* reset all devices */
    for(re = first_reset_entry; re != NULL; re = re->next) {
        re->func(re->opaque);
    }
}

void qemu_system_reset_request(void)
{
    reset_requested = 1;
    cpu_interrupt(cpu_single_env, CPU_INTERRUPT_EXIT);
}

void qemu_system_shutdown_request(void)
{
    shutdown_requested = 1;
    cpu_interrupt(cpu_single_env, CPU_INTERRUPT_EXIT);
}

void main_loop_wait(int timeout)
{
        if (vm_running) {
            qemu_run_timers(&active_timers[QEMU_TIMER_VIRTUAL],
                            qemu_get_clock(vm_clock));
            /* run dma transfers, if any */
            DMA_run();
        }

        /* real time timers */
        qemu_run_timers(&active_timers[QEMU_TIMER_REALTIME],
                        qemu_get_clock(rt_clock));
}

void help(void)
{
    printf("QEMU PC emulator version " QEMU_VERSION ", Copyright (c) 2003-2004 Fabrice Bellard\n"
           "usage: %s [options] [disk_image]\n"
           "\n"
           "'disk_image' is a raw hard image image for IDE hard disk 0\n"
           "\n"
           "Standard options:\n"
           "-fda/-fdb file  use 'file' as floppy disk 0/1 image\n"
           "-hda/-hdb file  use 'file' as IDE hard disk 0/1 image\n"
           "-hdc/-hdd file  use 'file' as IDE hard disk 2/3 image\n"
           "-cdrom file     use 'file' as IDE cdrom image (cdrom is ide1 master)\n"
           "-boot [a|c|d]   boot on floppy (a), hard disk (c) or CD-ROM (d)\n"
           "-snapshot       write to temporary files instead of disk image files\n"
           "-m megs         set virtual RAM size to megs MB [default=%d]\n"
           "-nographic      disable graphical output and redirect serial I/Os to console\n"
           "-vcpus          set CPU number of guest platform\n"
#ifdef CONFIG_VNC
           "-vnc port             use vnc instead of sdl\n"
           "-vncport port         use a different port\n"
           "-vncconnect host:port do a reverse connect\n"
#ifdef CONFIG_SDL
           "-vnc-and-sdl    use vnc and sdl simultaneously\n"
#endif
#endif
           "-k <language>   use keyboard layout (for example \"fr\" for french)\n"
           "-enable-audio   enable audio support\n"
           "-localtime      set the real time clock to local time [default=utc]\n"
           "-full-screen    start in full screen\n"
#ifdef TARGET_PPC
           "-prep           Simulate a PREP system (default is PowerMAC)\n"
           "-g WxH[xDEPTH]  Set the initial VGA graphic mode\n"
#endif
           "-nic-ne2000     simulate an Realtek ne2k PCI ethernet adaptor\n"
           "\n"
           "Network options:\n"
           "-nics n         simulate 'n' network cards [default=1]\n"
           "-macaddr addr   set the mac address of the first interface\n"
           "-bridge  br     set the bridge interface for nic\n"
           "-n script       set tap/tun network init script [default=%s]\n"
           "-tun-fd fd      use this fd as already opened tap/tun interface\n"
#ifdef CONFIG_SLIRP
           "-user-net       use user mode network stack [default if no tap/tun script]\n"
           "-tftp prefix    allow tftp access to files starting with prefix [-user-net]\n"
#ifndef _WIN32
           "-smb dir        allow SMB access to files in 'dir' [-user-net]\n"
#endif
           "-redir [tcp|udp]:host-port:[guest-host]:guest-port\n"
           "                redirect TCP or UDP connections from host to guest [-user-net]\n"
#endif
           "-dummy-net      use dummy network stack\n"
           "\n"
           "Linux boot specific:\n"
           "-kernel bzImage use 'bzImage' as kernel image\n"
           "-append cmdline use 'cmdline' as kernel command line\n"
           "-initrd file    use 'file' as initial ram disk\n"
           "\n"
           "Debug/Expert options:\n"
           "-monitor dev    redirect the monitor to char device 'dev'\n"
           "-serial dev     redirect the serial port to char device 'dev'\n"
           "-S              freeze CPU at startup (use 'c' to start execution)\n"
           "-s              wait gdb connection to port %d\n"
           "-p port         ioreq port for xen\n"
           "-d domain       domain that we're serving\n"
           "-domain-namn    domain name that we're serving\n"
           "-hdachs c,h,s   force hard disk 0 geometry (usually qemu can guess it)\n"
           "-L path         set the directory for the BIOS and VGA BIOS\n"
#ifdef USE_CODE_COPY
           "-no-code-copy   disable code copy acceleration\n"
#endif
#ifdef TARGET_I386
           "-isa            simulate an ISA-only system (default is PCI system)\n"
           "-std-vga        simulate a standard VGA card with VESA Bochs Extensions\n"
           "                (default is CL-GD5446 PCI VGA)\n"
           "-vgaacc [0|1]   1 to accelerate CL-GD5446 speed, default is 1\n"
           "-no-repeatkey   disable key repeat feature for SDL keyboard simulation"
#endif
           "-loadvm file    start right away with a saved state (loadvm in monitor)\n"
           "\n"
           "During emulation, the following keys are useful:\n"
           "ctrl-alt-f      toggle full screen\n"
           "ctrl-alt-n      switch to virtual console 'n'\n"
           "ctrl-alt        toggle mouse and keyboard grab\n"
           "\n"
           "When using -nographic, press 'ctrl-a h' to get some help.\n"
           ,
#ifdef CONFIG_SOFTMMU
           "qemu",
#else
           "qemu-fast",
#endif
           DEFAULT_RAM_SIZE,
           DEFAULT_NETWORK_SCRIPT,
           DEFAULT_GDBSTUB_PORT);
#ifndef CONFIG_SOFTMMU
    printf("\n"
           "NOTE: this version of QEMU is faster but it needs slightly patched OSes to\n"
           "work. Please use the 'qemu' executable to have a more accurate (but slower)\n"
           "PC emulation.\n");
#endif
    exit(1);
}

#define HAS_ARG 0x0001

enum {
    QEMU_OPTION_h,

    QEMU_OPTION_fda,
    QEMU_OPTION_fdb,
    QEMU_OPTION_hda,
    QEMU_OPTION_hdb,
    QEMU_OPTION_hdc,
    QEMU_OPTION_hdd,
    QEMU_OPTION_cdrom,
    QEMU_OPTION_boot,
    QEMU_OPTION_snapshot,
    QEMU_OPTION_m,
    QEMU_OPTION_nographic,
#ifdef CONFIG_VNC
    QEMU_OPTION_vnc,
    QEMU_OPTION_vncport,
    QEMU_OPTION_vncconnect,
#ifdef CONFIG_SDL
    QEMU_OPTION_vnc_and_sdl,
#endif
#endif
    QEMU_OPTION_enable_audio,

    QEMU_OPTION_nics,
    QEMU_OPTION_macaddr,
    QEMU_OPTION_bridge,
    QEMU_OPTION_n,
    QEMU_OPTION_tun_fd,
    QEMU_OPTION_user_net,
    QEMU_OPTION_tftp,
    QEMU_OPTION_smb,
    QEMU_OPTION_redir,
    QEMU_OPTION_dummy_net,

    QEMU_OPTION_kernel,
    QEMU_OPTION_append,
    QEMU_OPTION_initrd,

    QEMU_OPTION_S,
    QEMU_OPTION_s,
    QEMU_OPTION_d,
    QEMU_OPTION_l,
    QEMU_OPTION_hdachs,
    QEMU_OPTION_L,
    QEMU_OPTION_no_code_copy,
    QEMU_OPTION_vcpus,
    QEMU_OPTION_pci,
    QEMU_OPTION_nic_ne2000,
    QEMU_OPTION_isa,
    QEMU_OPTION_prep,
    QEMU_OPTION_k,
    QEMU_OPTION_localtime,
    QEMU_OPTION_cirrusvga,
    QEMU_OPTION_g,
    QEMU_OPTION_std_vga,
    QEMU_OPTION_monitor,
    QEMU_OPTION_domainname,
    QEMU_OPTION_serial,
    QEMU_OPTION_loadvm,
    QEMU_OPTION_full_screen,
    QEMU_OPTION_vgaacc,
    QEMU_OPTION_repeatkey,
};

typedef struct QEMUOption {
    const char *name;
    int flags;
    int index;
} QEMUOption;

const QEMUOption qemu_options[] = {
    { "h", 0, QEMU_OPTION_h },

    { "fda", HAS_ARG, QEMU_OPTION_fda },
    { "fdb", HAS_ARG, QEMU_OPTION_fdb },
    { "hda", HAS_ARG, QEMU_OPTION_hda },
    { "hdb", HAS_ARG, QEMU_OPTION_hdb },
    { "hdc", HAS_ARG, QEMU_OPTION_hdc },
    { "hdd", HAS_ARG, QEMU_OPTION_hdd },
    { "cdrom", HAS_ARG, QEMU_OPTION_cdrom },
    { "boot", HAS_ARG, QEMU_OPTION_boot },
    { "snapshot", 0, QEMU_OPTION_snapshot },
    { "m", HAS_ARG, QEMU_OPTION_m },
    { "nographic", 0, QEMU_OPTION_nographic },
#ifdef CONFIG_VNC
    { "vnc", 0, QEMU_OPTION_vnc },
    { "vncport", HAS_ARG, QEMU_OPTION_vncport },
    { "vncconnect", HAS_ARG, QEMU_OPTION_vncconnect },
#ifdef CONFIG_SDL
    { "vnc-and-sdl", 0, QEMU_OPTION_vnc_and_sdl },
#endif
#endif
    { "k", HAS_ARG, QEMU_OPTION_k },
    { "enable-audio", 0, QEMU_OPTION_enable_audio },

    { "nics", HAS_ARG, QEMU_OPTION_nics},
    { "macaddr", HAS_ARG, QEMU_OPTION_macaddr},
    { "bridge", HAS_ARG, QEMU_OPTION_bridge},
    { "n", HAS_ARG, QEMU_OPTION_n },
    { "tun-fd", HAS_ARG, QEMU_OPTION_tun_fd },
#ifdef CONFIG_SLIRP
    { "user-net", 0, QEMU_OPTION_user_net },
    { "tftp", HAS_ARG, QEMU_OPTION_tftp },
#ifndef _WIN32
    { "smb", HAS_ARG, QEMU_OPTION_smb },
#endif
    { "redir", HAS_ARG, QEMU_OPTION_redir },
#endif
    { "dummy-net", 0, QEMU_OPTION_dummy_net },

    { "kernel", HAS_ARG, QEMU_OPTION_kernel },
    { "append", HAS_ARG, QEMU_OPTION_append },
    { "initrd", HAS_ARG, QEMU_OPTION_initrd },

    { "S", 0, QEMU_OPTION_S },
    { "s", 0, QEMU_OPTION_s },
    { "d", HAS_ARG, QEMU_OPTION_d },
    { "l", HAS_ARG, QEMU_OPTION_l },
    { "hdachs", HAS_ARG, QEMU_OPTION_hdachs },
    { "L", HAS_ARG, QEMU_OPTION_L },
    { "no-code-copy", 0, QEMU_OPTION_no_code_copy },
    { "vcpus", 1, QEMU_OPTION_vcpus },
#ifdef TARGET_PPC
    { "prep", 0, QEMU_OPTION_prep },
    { "g", 1, QEMU_OPTION_g },
#endif
    { "localtime", 0, QEMU_OPTION_localtime },
    { "isa", 0, QEMU_OPTION_isa },
    { "std-vga", 0, QEMU_OPTION_std_vga },
    { "monitor", 1, QEMU_OPTION_monitor },
    { "domain-name", 1, QEMU_OPTION_domainname },
    { "serial", 1, QEMU_OPTION_serial },
    { "loadvm", HAS_ARG, QEMU_OPTION_loadvm },
    { "full-screen", 0, QEMU_OPTION_full_screen },

    /* temporary options */
    { "pci", 0, QEMU_OPTION_pci },
    { "nic-ne2000", 0, QEMU_OPTION_nic_ne2000 },
    { "cirrusvga", 0, QEMU_OPTION_cirrusvga },
    { "vgaacc", HAS_ARG, QEMU_OPTION_vgaacc },
    { "no-repeatkey", 0, QEMU_OPTION_repeatkey },
    { NULL },
};

#if defined (TARGET_I386) && defined(USE_CODE_COPY)

/* this stack is only used during signal handling */
#define SIGNAL_STACK_SIZE 32768

static uint8_t *signal_stack;

#endif

#define NET_IF_TUN   0
#define NET_IF_USER  1
#define NET_IF_DUMMY 2

#include <xg_private.h>

#define L1_PROT (_PAGE_PRESENT|_PAGE_RW|_PAGE_ACCESSED|_PAGE_USER)
#define L2_PROT (_PAGE_PRESENT|_PAGE_RW|_PAGE_ACCESSED|_PAGE_DIRTY|_PAGE_USER)

#ifdef __i386__
#define _LEVEL_3_ 0
#else
#define _LEVEL_3_ 1
#endif

#if _LEVEL_3_
#define L3_PROT (_PAGE_PRESENT)
#define L1_PAGETABLE_ENTRIES    512
#else
#define L1_PAGETABLE_ENTRIES    1024
#endif

inline int
get_vl2_table(unsigned long count, unsigned long start)
{
#if _LEVEL_3_
    return ((start + (count << PAGE_SHIFT)) >> L3_PAGETABLE_SHIFT) & 0x3;
#else
    return 0;
#endif
}

/* FIXME Flush the shadow page */
int unset_mm_mapping(int xc_handle,
                     uint32_t domid,
                     unsigned long nr_pages,
                     unsigned int address_bits,
                     unsigned long *extent_start)
{
    int err = 0;
    xc_dominfo_t info;

    err = xc_domain_memory_decrease_reservation(xc_handle, domid,
      nr_pages, 0, extent_start);

    if ( err )
        fprintf(stderr, "Failed to decrease physmap\n");

    xc_domain_getinfo(xc_handle, domid, 1, &info);

    if ( (info.nr_pages - nr_pages) <= 0 )
    {
        fprintf(stderr, "unset_mm_mapping: error nr_pages\n");
        err = -1;
    }

    if ( xc_domain_setmaxmem(xc_handle, domid,
                             (info.nr_pages - nr_pages) * PAGE_SIZE/1024) != 0)
    {
        fprintf(logfile, "set maxmem returned error %d\n", errno);
        err = -1;
    }

    return err;
}

int set_mm_mapping(int xc_handle,
                    uint32_t domid,
                    unsigned long nr_pages,
                    unsigned int address_bits,
                    unsigned long *extent_start)
{
    xc_dominfo_t info;
    int err = 0;

    xc_domain_getinfo(xc_handle, domid, 1, &info);

    if ( xc_domain_setmaxmem(xc_handle, domid,
                             (info.nr_pages + nr_pages) * PAGE_SIZE/1024) != 0)
    {
        fprintf(logfile, "set maxmem returned error %d\n", errno);
        return -1;
    }

    err = xc_domain_memory_populate_physmap(xc_handle, domid,
                                            nr_pages, 0,
                                            address_bits, extent_start);

    if ( err )
    {
        fprintf(stderr, "Failed to populate physmap\n");
        return -1;
    }

    err = xc_domain_translate_gpfn_list(xc_handle, domid,
                                        nr_pages,
                                        extent_start, extent_start);

    if ( err )
    {
        fprintf(stderr, "Failed to translate gpfn list\n");
        return -1;
    }

#if 0 /* Generates lots of log file output - turn on for debugging */
    for (i = 0; i < nr_pages; i++)
        fprintf(stderr, "set_map result i %x result %lx\n", i, extent_start[i]);
#endif

    return 0;
}

int main(int argc, char **argv)
{
#ifdef CONFIG_GDBSTUB
    int use_gdbstub, gdbstub_port;
#endif
    int i, has_cdrom;
    int snapshot, linux_boot;
    CPUState *env;
    const char *initrd_filename;
    const char *hd_filename[MAX_DISKS], *fd_filename[MAX_FD];
    const char *kernel_filename, *kernel_cmdline;
    DisplayState *ds = &display_state;
    int cyls, heads, secs;
    int start_emulation = 1;
    uint8_t macaddr[6];
    int net_if_type, nb_tun_fds, tun_fds[MAX_NICS];
    int optind;
    const char *r, *optarg;
    CharDriverState *monitor_hd;
    char monitor_device[128];
    char serial_devices[MAX_SERIAL_PORTS][128];
    int serial_device_index;
    char qemu_dm_logfilename[64];
    const char *loadvm = NULL;
    unsigned long nr_pages, extra_pages, ram_pages, *page_array;
    xc_dominfo_t info;
    extern void *shared_page;
    extern void *shared_vram;

#if !defined(CONFIG_SOFTMMU)
    /* we never want that malloc() uses mmap() */
    mallopt(M_MMAP_THRESHOLD, 4096 * 1024);
#endif
    initrd_filename = NULL;
    for(i = 0; i < MAX_FD; i++)
        fd_filename[i] = NULL;
    for(i = 0; i < MAX_DISKS; i++)
        hd_filename[i] = NULL;
    ram_size = DEFAULT_RAM_SIZE * 1024 * 1024;
    vga_ram_size = VGA_RAM_SIZE;
    bios_size = BIOS_SIZE;
    pstrcpy(network_script, sizeof(network_script), DEFAULT_NETWORK_SCRIPT);
#ifdef CONFIG_GDBSTUB
    use_gdbstub = 0;
    gdbstub_port = DEFAULT_GDBSTUB_PORT;
#endif
    snapshot = 0;
    nographic = 0;
    usevnc = 0;
    vncport=0;
    vncconnect=NULL;
    kernel_filename = NULL;
    kernel_cmdline = "";
    has_cdrom = 1;
    cyls = heads = secs = 0;
    pstrcpy(monitor_device, sizeof(monitor_device), "vc");

    pstrcpy(serial_devices[0], sizeof(serial_devices[0]), "vc");
    pstrcpy(serial_devices[1], sizeof(serial_devices[1]), "null");
    for(i = 2; i < MAX_SERIAL_PORTS; i++)
        serial_devices[i][0] = '\0';
    serial_device_index = 0;

    nb_tun_fds = 0;
    net_if_type = -1;
    nb_nics = 1;
    /* default mac address of the first network interface */
    macaddr[0] = 0x52;
    macaddr[1] = 0x54;
    macaddr[2] = 0x00;
    macaddr[3] = 0x12;
    macaddr[4] = 0x34;
    macaddr[5] = 0x56;

    /* init debug */
    sprintf(qemu_dm_logfilename, "/var/log/qemu-dm.%d.log", getpid());
    cpu_set_log_filename(qemu_dm_logfilename);
    cpu_set_log(0);

    optind = 1;
    for(;;) {
        if (optind >= argc)
            break;
        r = argv[optind];
        if (r[0] != '-') {
            hd_filename[0] = argv[optind++];
        } else {
            const QEMUOption *popt;

            optind++;
            popt = qemu_options;
            for(;;) {
                if (!popt->name) {
                    fprintf(stderr, "%s: invalid option -- '%s'\n",
                            argv[0], r);
                    exit(1);
                }
                if (!strcmp(popt->name, r + 1))
                    break;
                popt++;
            }
            if (popt->flags & HAS_ARG) {
                if (optind >= argc) {
                    fprintf(stderr, "%s: option '%s' requires an argument\n",
                            argv[0], r);
                    exit(1);
                }
                optarg = argv[optind++];
            } else {
                optarg = NULL;
            }

            switch(popt->index) {
            case QEMU_OPTION_initrd:
                initrd_filename = optarg;
                break;
            case QEMU_OPTION_hda:
                hd_filename[0] = optarg;
                break;
            case QEMU_OPTION_hdb:
                hd_filename[1] = optarg;
                break;
            case QEMU_OPTION_snapshot:
                snapshot = 1;
                break;
            case QEMU_OPTION_hdachs:
                {
                    const char *p;
                    p = optarg;
                    cyls = strtol(p, (char **)&p, 0);
                    if (*p != ',')
                        goto chs_fail;
                    p++;
                    heads = strtol(p, (char **)&p, 0);
                    if (*p != ',')
                        goto chs_fail;
                    p++;
                    secs = strtol(p, (char **)&p, 0);
                    if (*p != '\0') {
                    chs_fail:
                        cyls = 0;
                    }
                }
                break;
            case QEMU_OPTION_nographic:
                pstrcpy(monitor_device, sizeof(monitor_device), "stdio");
                if(!strcmp(serial_devices[0], "vc"))
                    pstrcpy(serial_devices[0], sizeof(serial_devices[0]),
                            "stdio");
                nographic = 1;
                break;
#ifdef CONFIG_VNC
            case QEMU_OPTION_vnc:
                usevnc = 1;
                break;
            case QEMU_OPTION_vncport:
                {
                    const char *p;
                    p = optarg;
                    vncport= strtol(optarg, (char **)&p, 0);
                }
                break;
            case QEMU_OPTION_vncconnect:
                vncconnect = optarg;
                break;
#ifdef CONFIG_SDL
            case QEMU_OPTION_vnc_and_sdl:
                usevnc = 2;
                break;
#endif
#endif
            case QEMU_OPTION_kernel:
                kernel_filename = optarg;
                break;
            case QEMU_OPTION_append:
                kernel_cmdline = optarg;
                break;
            case QEMU_OPTION_tun_fd:
                {
                    const char *p;
                    int fd;
                    net_if_type = NET_IF_TUN;
                    if ( nb_tun_fds < MAX_NICS ) {
                        fd = strtol(optarg, (char **)&p, 0);
                        if (*p != '\0') {
                            fprintf(stderr,
                                    "qemu: invalid fd for network interface %d\n",
                                    nb_tun_fds);
                            exit(1);
                        }
                        tun_fds[nb_tun_fds++] = fd;
                    }
                }
                break;
            case QEMU_OPTION_hdc:
                hd_filename[2] = optarg;
                has_cdrom = 0;
                break;
            case QEMU_OPTION_hdd:
                hd_filename[3] = optarg;
                break;
            case QEMU_OPTION_cdrom:
                hd_filename[2] = optarg;
                has_cdrom = 1;
                break;
            case QEMU_OPTION_boot:
                boot_device = optarg[0];
                if ( boot_device != 'a' &&
                     boot_device != 'c' &&
                     boot_device != 'd' ) {
                    fprintf(stderr, "qemu: invalid boot device '%c'\n",
                                    boot_device);
                    exit(1);
                }
                break;
            case QEMU_OPTION_fda:
                fd_filename[0] = optarg;
                break;
            case QEMU_OPTION_fdb:
                fd_filename[1] = optarg;
                break;
            case QEMU_OPTION_nics:
                nb_nics = atoi(optarg);
                if (nb_nics < 0 || nb_nics > MAX_NICS) {
                    fprintf(stderr,
                            "qemu: invalid number of network interfaces\n");
                    exit(1);
                }
                break;
            case QEMU_OPTION_bridge:
                pstrcpy(bridge, sizeof(bridge), optarg);
                break;
            case QEMU_OPTION_macaddr:
                {
                    const char *p;
                    int i;

                    p = optarg;
                    for (i = 0; i < 6; i++) {
                        macaddr[i] = strtol(p, (char **)&p, 16);
                        if (i == 5) {
                            if (*p != '\0')
                                goto macaddr_error;
                        } else {
                            if (*p != ':') {
                            macaddr_error:
                                fprintf(stderr, "qemu: invalid syntax "
                                                "for ethernet address\n");
                                exit(1);
                            }
                            p++;
                        }
                    }
                }
                break;
#ifdef CONFIG_SLIRP
            case QEMU_OPTION_tftp:
                tftp_prefix = optarg;
                break;
#ifndef _WIN32
            case QEMU_OPTION_smb:
                net_slirp_smb(optarg);
                break;
#endif
            case QEMU_OPTION_user_net:
                net_if_type = NET_IF_USER;
                break;
            case QEMU_OPTION_redir:
                net_slirp_redir(optarg);
                break;
#endif
            case QEMU_OPTION_dummy_net:
                net_if_type = NET_IF_DUMMY;
                break;
            case QEMU_OPTION_enable_audio:
                audio_enabled = 1;
                break;
            case QEMU_OPTION_h:
                help();
                break;
            case QEMU_OPTION_m:
                ram_size = atol(optarg) * 1024 * 1024;
                if (ram_size <= 0)
                    help();
                break;
            case QEMU_OPTION_d:
                {
                    domid = atoi(optarg);
                    fprintf(logfile, "domid: %d\n", domid);
                }
                break;
            case QEMU_OPTION_l:
                {
                    int mask;
                    mask = cpu_str_to_log_mask(optarg);
                    fprintf(logfile, "mask: %x\n", mask);
                    cpu_set_log(mask);
                }
                break;
            case QEMU_OPTION_n:
                pstrcpy(network_script, sizeof(network_script), optarg);
                break;
#ifdef CONFIG_GDBSTUB
            case QEMU_OPTION_s:
                use_gdbstub = 1;
                break;
#endif
            case QEMU_OPTION_L:
                bios_dir = optarg;
                break;
            case QEMU_OPTION_S:
                start_emulation = 0;
                break;
            case QEMU_OPTION_vcpus:
                vcpus = atoi(optarg);
                fprintf(logfile, "qemu: the number of cpus is %d\n", vcpus);
                break;
            case QEMU_OPTION_pci:
                pci_enabled = 1;
                break;
            case QEMU_OPTION_nic_ne2000:
                nic_ne2000 = 1;
                break;
            case QEMU_OPTION_isa:
                pci_enabled = 0;
                break;
            case QEMU_OPTION_prep:
                prep_enabled = 1;
                break;
            case QEMU_OPTION_k:
                keyboard_layout = optarg;
                break;
            case QEMU_OPTION_localtime:
                rtc_utc = 0;
                break;
            case QEMU_OPTION_cirrusvga:
                cirrus_vga_enabled = 1;
                break;
            case QEMU_OPTION_vgaacc:
                {
                    const char *p;
                    p = optarg;
                    vga_accelerate = strtol(p, (char **)&p, 0);
                    if (*p != '\0') {
                        fprintf(stderr, "qemu: invalid vgaacc option\n");
                        exit(1);
                    }
                }
                break;
            case QEMU_OPTION_repeatkey:
                repeat_key = 0;
                break;
            case QEMU_OPTION_std_vga:
                cirrus_vga_enabled = 0;
                break;
            case QEMU_OPTION_g:
                {
                    const char *p;
                    int w, h, depth;
                    p = optarg;
                    w = strtol(p, (char **)&p, 10);
                    if (w <= 0) {
                    graphic_error:
                        fprintf(stderr, "qemu: invalid resolution or depth\n");
                        exit(1);
                    }
                    if (*p != 'x')
                        goto graphic_error;
                    p++;
                    h = strtol(p, (char **)&p, 10);
                    if (h <= 0)
                        goto graphic_error;
                    if (*p == 'x') {
                        p++;
                        depth = strtol(p, (char **)&p, 10);
                        if (depth != 8 && depth != 15 && depth != 16 &&
                            depth != 24 && depth != 32)
                            goto graphic_error;
                    } else if (*p == '\0') {
                        depth = graphic_depth;
                    } else {
                        goto graphic_error;
                    }

                    graphic_width = w;
                    graphic_height = h;
                    graphic_depth = depth;
                }
                break;
            case QEMU_OPTION_monitor:
                pstrcpy(monitor_device, sizeof(monitor_device), optarg);
                break;
            case QEMU_OPTION_serial:
                if (serial_device_index >= MAX_SERIAL_PORTS) {
                    fprintf(stderr, "qemu: too many serial ports\n");
                    exit(1);
                }
                pstrcpy(serial_devices[serial_device_index],
                        sizeof(serial_devices[0]), optarg);
                serial_device_index++;
                break;
            case QEMU_OPTION_loadvm:
                loadvm = optarg;
                break;
            case QEMU_OPTION_full_screen:
                full_screen = 1;
                break;
            case QEMU_OPTION_domainname:
                strncat(domain_name, optarg, sizeof(domain_name) - 20);
                break;

            }
        }
    }

    linux_boot = (kernel_filename != NULL);

    if ( !linux_boot && hd_filename[0] == '\0' &&
         hd_filename[2] == '\0' && fd_filename[0] == '\0' )
        help();

    /* boot to cd by default if no hard disk */
    if (hd_filename[0] == '\0' && boot_device == 'c') {
        if (fd_filename[0] != '\0')
            boot_device = 'a';
        else
            boot_device = 'd';
    }

#if !defined(CONFIG_SOFTMMU)
    /* must avoid mmap() usage of glibc by setting a buffer "by hand" */
    {
        static uint8_t stdout_buf[4096];
        setvbuf(stdout, stdout_buf, _IOLBF, sizeof(stdout_buf));
    }
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    /* init host network redirectors */
    if (net_if_type == -1) {
        net_if_type = NET_IF_TUN;
#if defined(CONFIG_SLIRP)
        if (access(network_script, R_OK) < 0) {
            net_if_type = NET_IF_USER;
        }
#endif
    }

    for(i = 0; i < nb_nics; i++) {
        NetDriverState *nd = &nd_table[i];
        nd->index = i;
        /* init virtual mac address */
        nd->macaddr[0] = macaddr[0];
        nd->macaddr[1] = macaddr[1];
        nd->macaddr[2] = macaddr[2];
        nd->macaddr[3] = macaddr[3];
        nd->macaddr[4] = macaddr[4];
        nd->macaddr[5] = macaddr[5] + i;
        switch(net_if_type) {
#if defined(CONFIG_SLIRP)
        case NET_IF_USER:
            net_slirp_init(nd);
            break;
#endif
#if !defined(_WIN32)
        case NET_IF_TUN:
            if (i < nb_tun_fds) {
                net_fd_init(nd, tun_fds[i]);
            } else {
                if (net_tun_init(nd) < 0)
                    net_dummy_init(nd);
            }
            break;
#endif
        case NET_IF_DUMMY:
        default:
            net_dummy_init(nd);
            break;
        }
    }

    /* init the memory */
    phys_ram_size = ram_size + vga_ram_size + bios_size;

    ram_pages = ram_size/PAGE_SIZE;
#if defined(__i386__) || defined(__x86_64__)
    vgaram_pages =  (vga_ram_size -1) / PAGE_SIZE + 1;
    free_pages = vgaram_pages / L1_PAGETABLE_ENTRIES;
    extra_pages = vgaram_pages + free_pages;
#else
    /* Test vga acceleration later */
    extra_pages = 0;
#endif

    xc_handle = xc_interface_open();

    xc_domain_getinfo(xc_handle, domid, 1, &info);

    nr_pages = info.nr_pages + extra_pages;

    if ( xc_domain_setmaxmem(xc_handle, domid,
                             (nr_pages) * PAGE_SIZE/1024 ) != 0)
    {
        fprintf(logfile, "set maxmem returned error %d\n", errno);
        exit(-1);
    }

    if ( (page_array = (unsigned long *)
                        malloc(nr_pages * sizeof(unsigned long))) == NULL)
    {
        fprintf(logfile, "malloc returned error %d\n", errno);
        exit(-1);
    }

    if (xc_domain_memory_increase_reservation(xc_handle, domid,
                                              extra_pages , 0, 0, NULL) != 0)
    {
        fprintf(logfile, "increase reservation returned error %d\n", errno);
        exit(-1);
    }

#if defined(__i386__) || defined(__x86_64__)
    if ( xc_get_pfn_list(xc_handle, domid, page_array, nr_pages) != nr_pages )
    {
        fprintf(logfile, "xc_get_pfn_list returned error %d\n", errno);
        exit(-1);
    }

    if ( (phys_ram_base = xc_map_foreign_batch(xc_handle, domid,
                          PROT_READ|PROT_WRITE,
                          page_array,
                          ram_pages - 1)) == 0 )
    {
        fprintf(logfile, "xc_map_foreign_batch returned error %d\n", errno);
        exit(-1);
    }

    shared_page = xc_map_foreign_range(xc_handle, domid, PAGE_SIZE,
                                       PROT_READ|PROT_WRITE,
                                       page_array[ram_pages - 1]);

    vgapage_array = &page_array[nr_pages - vgaram_pages];

    if ( (shared_vram =  xc_map_foreign_batch(xc_handle, domid,
                                              PROT_READ|PROT_WRITE,
                                              vgapage_array,
                                              vgaram_pages)) == 0)
    {
        fprintf(logfile,
                "xc_map_foreign_batch vgaram returned error %d\n", errno);
        exit(-1);
    }

    memset(shared_vram, 0, vgaram_pages * PAGE_SIZE);
    toptab = page_array[ram_pages] << PAGE_SHIFT;

    vtop_table = xc_map_foreign_range(xc_handle, domid, PAGE_SIZE,
                                      PROT_READ|PROT_WRITE,
                                      page_array[ram_pages]);

    freepage_array = &page_array[nr_pages - extra_pages];
#elif defined(__ia64__)
    if ( xc_ia64_get_pfn_list(xc_handle, domid, page_array, 0, ram_pages)
         != ram_pages )
    {
        fprintf(logfile, "xc_ia64_get_pfn_list returned error %d\n", errno);
        exit(-1);
    }

    if ( (phys_ram_base = xc_map_foreign_batch(xc_handle, domid,
                          PROT_READ|PROT_WRITE,
                          page_array,
                          ram_pages)) == 0 )
    {
        fprintf(logfile, "xc_map_foreign_batch returned error %d\n", errno);
        exit(-1);
    }

    if ( xc_ia64_get_pfn_list(xc_handle, domid,
                              page_array,
                              ram_pages + (GFW_SIZE >> PAGE_SHIFT), 1) != 1 )
    {
        fprintf(logfile, "xc_ia64_get_pfn_list returned error %d\n", errno);
        exit(-1);
    }

    shared_page = xc_map_foreign_range(xc_handle, domid, PAGE_SIZE,
                                       PROT_READ|PROT_WRITE,
                                       page_array[0]);
#endif

    fprintf(logfile, "shared page at pfn:%lx, mfn: %lx\n", (nr_pages-1),
           (page_array[nr_pages - 1]));

    /* we always create the cdrom drive, even if no disk is there */
    bdrv_init();
    if (has_cdrom) {
        int fd;
        if ( (fd = open(hd_filename[2], O_RDONLY | O_BINARY)) < 0) {
                hd_filename[2]=NULL;
                bs_table[2]=NULL;
                fprintf(logfile, "Could not open CD %s.\n", hd_filename[i]);
        }
        else {
                close(fd);
                bs_table[2] = bdrv_new("cdrom");
                bdrv_set_type_hint(bs_table[2], BDRV_TYPE_CDROM);
        }
    }

    /* open the virtual block devices */
    for(i = 0; i < MAX_DISKS; i++) {
        if (hd_filename[i]) {
            if (!bs_table[i]) {
                char buf[64];
                snprintf(buf, sizeof(buf), "hd%c", i + 'a');
                bs_table[i] = bdrv_new(buf);
            }
            if (bdrv_open(bs_table[i], hd_filename[i], snapshot) < 0) {
                fprintf(stderr, "qemu: could not open hard disk image '%s'\n",
                        hd_filename[i]);
                exit(1);
            }
            if (i == 0 && cyls != 0)
                bdrv_set_geometry_hint(bs_table[i], cyls, heads, secs);
        }
    }

    /* we always create at least one floppy disk */
    fd_table[0] = bdrv_new("fda");
    bdrv_set_type_hint(fd_table[0], BDRV_TYPE_FLOPPY);

    for(i = 0; i < MAX_FD; i++) {
        if (fd_filename[i]) {
            if (!fd_table[i]) {
                char buf[64];
                snprintf(buf, sizeof(buf), "fd%c", i + 'a');
                fd_table[i] = bdrv_new(buf);
                bdrv_set_type_hint(fd_table[i], BDRV_TYPE_FLOPPY);
            }
            if (fd_filename[i] != '\0') {
                if (bdrv_open(fd_table[i], fd_filename[i], snapshot) < 0) {
                    fprintf(stderr, "qemu: could not open floppy disk image '%s'\n",
                            fd_filename[i]);
                    exit(1);
                }
            }
        }
    }

    /* init CPU state */
    env = cpu_init();
    global_env = env;
    cpu_single_env = env;

    init_ioports();
    cpu_calibrate_ticks();

    /* terminal init */
    if (nographic) {
        dumb_display_init(ds);
    } else {
        if (usevnc) {
#ifdef CONFIG_VNC
            vnc_display_init(ds, (usevnc==2), vncport, vncconnect);
#else
            fprintf(logfile, "qemu not configured with vnc support\n");
#endif
        } else {
#ifdef CONFIG_SDL
            sdl_display_init(ds, full_screen);
#else
            dumb_display_init(ds);
#endif
        }
    }

    vga_console = graphic_console_init(ds);

    monitor_hd = qemu_chr_open(monitor_device);
    if (!monitor_hd) {
        fprintf(stderr, "qemu: could not open monitor device '%s'\n", monitor_device);
        exit(1);
    }
    monitor_init(monitor_hd, !nographic);

    for(i = 0; i < MAX_SERIAL_PORTS; i++) {
        if (serial_devices[i][0] != '\0') {
            serial_hds[i] = qemu_chr_open(serial_devices[i]);
            if (!serial_hds[i]) {
                fprintf(stderr, "qemu: could not open serial device '%s'\n",
                        serial_devices[i]);
                exit(1);
            }
            if (!strcmp(serial_devices[i], "vc"))
                qemu_chr_printf(serial_hds[i], "serial%d console\n", i);
        }
    }

    /* setup cpu signal handlers for MMU / self modifying code handling */
#if !defined(CONFIG_SOFTMMU)

#if defined (TARGET_I386) && defined(USE_CODE_COPY)
    {
        stack_t stk;
        signal_stack = memalign(16, SIGNAL_STACK_SIZE);
        stk.ss_sp = signal_stack;
        stk.ss_size = SIGNAL_STACK_SIZE;
        stk.ss_flags = 0;

        if (sigaltstack(&stk, NULL) < 0) {
            fprintf(logfile, "sigaltstack returned error %d\n", errno);
            exit(1);
        }
    }
#endif
    {
        struct sigaction act;

        sigfillset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO;
#if defined (TARGET_I386) && defined(USE_CODE_COPY)
        act.sa_flags |= SA_ONSTACK;
#endif
        act.sa_sigaction = host_segv_handler;
        sigaction(SIGSEGV, &act, NULL);
        sigaction(SIGBUS, &act, NULL);
#if defined (TARGET_I386) && defined(USE_CODE_COPY)
        sigaction(SIGFPE, &act, NULL);
#endif
    }
#endif

#ifndef _WIN32
    {
        struct sigaction act;
        sigfillset(&act.sa_mask);
        act.sa_flags = 0;
        act.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &act, NULL);
    }
#endif
    init_timers();

#if defined(TARGET_I386)
    pc_init(ram_size, vga_ram_size, boot_device,
            ds, fd_filename, snapshot,
            kernel_filename, kernel_cmdline, initrd_filename);
#elif defined(TARGET_PPC)
    ppc_init(ram_size, vga_ram_size, boot_device,
            ds, fd_filename, snapshot,
            kernel_filename, kernel_cmdline, initrd_filename);
#elif defined(TARGET_SPARC)
    sun4m_init(ram_size, vga_ram_size, boot_device,
            ds, fd_filename, snapshot,
            kernel_filename, kernel_cmdline, initrd_filename);
#endif

    gui_timer = qemu_new_timer(rt_clock, gui_update, NULL);
    qemu_mod_timer(gui_timer, qemu_get_clock(rt_clock));

    polling_timer = qemu_new_timer(rt_clock, polling_handler, NULL);
    qemu_mod_timer(polling_timer, qemu_get_clock(rt_clock));

#ifdef CONFIG_GDBSTUB
    if (use_gdbstub) {
        if (gdbserver_start(gdbstub_port) < 0) {
            fprintf(stderr, "Could not open gdbserver socket on port %d\n",
                    gdbstub_port);
            exit(1);
        } else {
            fprintf(logfile, "Waiting gdb connection on port %d\n", gdbstub_port);
        }
    } else
#endif
    if (loadvm)
        qemu_loadvm(loadvm);

    {
        /* XXX: simplify init */
        if (start_emulation) {
            vm_start();
        }
    }
    main_loop();
    quit_timers();
    return 0;
}

extern fd_set wakeup_rfds;
void tun_receive_handler(fd_set *rfds)
{
    IOHandlerRecord *ioh;
    static uint8_t buf[4096];
    int n, max_size;

    for (ioh = first_eventio_handler; ioh != NULL; ioh = ioh->next) {
        if ( FD_ISSET(ioh->fd, rfds) ) {
            max_size = ioh->fd_can_read(ioh->opaque);
            if (max_size > 0) {
                if (max_size > sizeof(buf))
                    max_size = sizeof(buf);
                n = read(ioh->fd, buf, max_size);
                if (n >= 0) {
                    ioh->fd_read(ioh->opaque, buf, n);
                }
            }
        }
    }
    update_select_wakeup_events();
}

void update_select_wakeup_events(void)
{
    IOHandlerRecord *ioh;
    int max_size;

    for(ioh = first_eventio_handler; ioh != NULL; ioh = ioh->next) {
        FD_CLR(ioh->fd, &wakeup_rfds);
        if (ioh->fd_can_read) {
             max_size = ioh->fd_can_read(ioh->opaque);
             if (max_size > 0) {
                 FD_SET(ioh->fd, &wakeup_rfds);
             }
        }
    }
}
