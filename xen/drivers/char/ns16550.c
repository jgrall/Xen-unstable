/******************************************************************************
 * ns16550.c
 * 
 * Driver for 16550-series UARTs. This driver is to be kept within Xen as
 * it permits debugging of seriously-toasted machines (e.g., in situations
 * where a device driver within a guest OS would be inaccessible).
 * 
 * Copyright (c) 2003-2005, K A Fraser
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/serial.h>
#include <asm/io.h>

/* Config serial port with a string <baud>,DPS,<io-base>,<irq>. */
char opt_com1[30] = "", opt_com2[30] = "";
string_param("com1", opt_com1);
string_param("com2", opt_com2);

static struct ns16550 {
    int baud, data_bits, parity, stop_bits, irq;
    unsigned long io_base;   /* I/O port or memory-mapped I/O address. */
    char *remapped_io_base;  /* Remapped virtual address of mmap I/O.  */ 
    struct irqaction irqaction;
} ns16550_com[2] = {
    { 0, 0, 0, 0, 4, 0x3f8 },
    { 0, 0, 0, 0, 3, 0x2f8 }
};

/* Register offsets */
#define RBR             0x00    /* receive buffer       */
#define THR             0x00    /* transmit holding     */
#define IER             0x01    /* interrupt enable     */
#define IIR             0x02    /* interrupt identity   */
#define FCR             0x02    /* FIFO control         */
#define LCR             0x03    /* line control         */
#define MCR             0x04    /* Modem control        */
#define LSR             0x05    /* line status          */
#define MSR             0x06    /* Modem status         */
#define DLL             0x00    /* divisor latch (ls) (DLAB=1) */
#define DLM             0x01    /* divisor latch (ms) (DLAB=1) */

/* Interrupt Enable Register */
#define IER_ERDAI       0x01    /* rx data recv'd       */
#define IER_ETHREI      0x02    /* tx reg. empty        */
#define IER_ELSI        0x04    /* rx line status       */
#define IER_EMSI        0x08    /* MODEM status         */

/* FIFO control register */
#define FCR_ENABLE      0x01    /* enable FIFO          */
#define FCR_CLRX        0x02    /* clear Rx FIFO        */
#define FCR_CLTX        0x04    /* clear Tx FIFO        */
#define FCR_DMA         0x10    /* enter DMA mode       */
#define FCR_TRG1        0x00    /* Rx FIFO trig lev 1   */
#define FCR_TRG4        0x40    /* Rx FIFO trig lev 4   */
#define FCR_TRG8        0x80    /* Rx FIFO trig lev 8   */
#define FCR_TRG14       0xc0    /* Rx FIFO trig lev 14  */

/* Line control register */
#define LCR_DLAB        0x80    /* Divisor Latch Access */

/* Modem Control Register */
#define MCR_DTR         0x01    /* Data Terminal Ready  */
#define MCR_RTS         0x02    /* Request to Send      */
#define MCR_OUT2        0x08    /* OUT2: interrupt mask */

/* Line Status Register */
#define LSR_DR          0x01    /* Data ready           */
#define LSR_OE          0x02    /* Overrun              */
#define LSR_PE          0x04    /* Parity error         */
#define LSR_FE          0x08    /* Framing error        */
#define LSR_BI          0x10    /* Break                */
#define LSR_THRE        0x20    /* Xmit hold reg empty  */
#define LSR_TEMT        0x40    /* Xmitter empty        */
#define LSR_ERR         0x80    /* Error                */

/* These parity settings can be ORed directly into the LCR. */
#define PARITY_NONE     (0<<3)
#define PARITY_ODD      (1<<3)
#define PARITY_EVEN     (3<<3)
#define PARITY_MARK     (5<<3)
#define PARITY_SPACE    (7<<3)

static char ns_read_reg(struct ns16550 *uart, int reg)
{
    if ( uart->remapped_io_base == NULL )
        return inb(uart->io_base + reg);
    return readb(uart->remapped_io_base + reg);
}

static void ns_write_reg(struct ns16550 *uart, int reg, char c)
{
    if ( uart->remapped_io_base == NULL )
        return outb(c, uart->io_base + reg);
    writeb(c, uart->remapped_io_base + reg);
}

static void ns16550_interrupt(
    int irq, void *dev_id, struct cpu_user_regs *regs)
{
    serial_rx_interrupt(dev_id, regs);
}

static void ns16550_putc(struct serial_port *port, char c)
{
    struct ns16550 *uart = port->uart;

    while ( !(ns_read_reg(uart, LSR) & LSR_THRE) )
        cpu_relax();

    ns_write_reg(uart, THR, c);
}

static int ns16550_getc(struct serial_port *port, char *pc)
{
    struct ns16550 *uart = port->uart;

    if ( !(ns_read_reg(uart, LSR) & LSR_DR) )
        return 0;

    *pc = ns_read_reg(uart, RBR);
    return 1;
}

static void ns16550_init_preirq(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;
    unsigned char lcr;

    /* I/O ports are distinguished by their size (16 bits). */
    if ( uart->io_base >= 0x10000 )
        uart->remapped_io_base = (char *)ioremap(uart->io_base, 8);

    lcr = (uart->data_bits - 5) | ((uart->stop_bits - 1) << 2) | uart->parity;

    /* No interrupts. */
    ns_write_reg(uart, IER, 0);

    /* Line control and baud-rate generator. */
    ns_write_reg(uart, LCR, lcr | LCR_DLAB);
    ns_write_reg(uart, DLL, 115200/uart->baud); /* baud lo */
    ns_write_reg(uart, DLM, 0);                 /* baud hi */
    ns_write_reg(uart, LCR, lcr);               /* parity, data, stop */

    /* No flow ctrl: DTR and RTS are both wedged high to keep remote happy. */
    ns_write_reg(uart, MCR, MCR_DTR | MCR_RTS);

    /* Enable and clear the FIFOs. Set a large trigger threshold. */
    ns_write_reg(uart, FCR, FCR_ENABLE | FCR_CLRX | FCR_CLTX | FCR_TRG14);
}

static void ns16550_init_postirq(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;
    int rc;

    uart->irqaction.handler = ns16550_interrupt;
    uart->irqaction.name    = "ns16550";
    uart->irqaction.dev_id  = port;
    if ( (rc = setup_irq(uart->irq, &uart->irqaction)) != 0 )
        printk("ERROR: Failed to allocate na16550 IRQ %d\n", uart->irq);

    /* For sanity, clear the receive FIFO. */
    ns_write_reg(uart, FCR, FCR_ENABLE | FCR_CLRX | FCR_TRG14);

    /* Master interrupt enable; also keep DTR/RTS asserted. */
    ns_write_reg(uart, MCR, MCR_OUT2 | MCR_DTR | MCR_RTS);

    /* Enable receive interrupts. */
    ns_write_reg(uart, IER, IER_ERDAI);
}

#ifdef CONFIG_X86
#include <asm/physdev.h>
static void ns16550_endboot(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;
    physdev_modify_ioport_access_range(dom0, 0, uart->io_base, 8);
}
#else
#define ns16550_endboot NULL
#endif

static struct uart_driver ns16550_driver = {
    .init_preirq  = ns16550_init_preirq,
    .init_postirq = ns16550_init_postirq,
    .endboot      = ns16550_endboot,
    .putc         = ns16550_putc,
    .getc         = ns16550_getc
};

#define PARSE_ERR(_f, _a...)                 \
    do {                                     \
        printk( "ERROR: " _f "\n" , ## _a ); \
        return;                              \
    } while ( 0 )

static void ns16550_parse_port_config(struct ns16550 *uart, char *conf)
{
    if ( *conf == '\0' )
        return;

    uart->baud = simple_strtol(conf, &conf, 10);
    if ( (uart->baud < 1200) || (uart->baud > 115200) )
        PARSE_ERR("Baud rate %d outside supported range.", uart->baud);

    if ( *conf != ',' )
        PARSE_ERR("Missing data/parity/stop specifiers.");

    conf++;

    uart->data_bits = simple_strtol(conf, &conf, 10);
    if ( (uart->data_bits < 5) || (uart->data_bits > 8) )
        PARSE_ERR("%d data bits are unsupported.", uart->data_bits);

    switch ( *conf )
    {
    case 'n':
        uart->parity = PARITY_NONE;
        break;
    case 'o': 
        uart->parity =  PARITY_ODD;
        break;
    case 'e': 
        uart->parity =  PARITY_EVEN;
        break;
    case 'm': 
        uart->parity =  PARITY_MARK;
        break;
    case 's': 
        uart->parity =  PARITY_SPACE;
        break;

    default:
        PARSE_ERR("Invalid parity specifier '%c'.", *conf);
    }

    conf++;

    uart->stop_bits = simple_strtol(conf, &conf, 10);
    if ( (uart->stop_bits < 1) || (uart->stop_bits > 2) )
        PARSE_ERR("%d stop bits are unsupported.", uart->stop_bits);

    if ( *conf == ',' )
    {
        conf++;
        uart->io_base = simple_strtol(conf, &conf, 0);

        if ( *conf == ',' )
        {
            conf++;
            uart->irq = simple_strtol(conf, &conf, 10);
        }
    }

    serial_register_uart(uart - ns16550_com, &ns16550_driver, uart);
}

void ns16550_init(void)
{
    ns16550_parse_port_config(&ns16550_com[0], opt_com1);
    ns16550_parse_port_config(&ns16550_com[1], opt_com2);
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
