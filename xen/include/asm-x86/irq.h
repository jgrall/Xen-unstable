#ifndef _ASM_HW_IRQ_H
#define _ASM_HW_IRQ_H

/* (C) 1992, 1993 Linus Torvalds, (C) 1997 Ingo Molnar */

#include <xen/config.h>
#include <asm/atomic.h>
#include <asm/asm_defns.h>
#include <irq_vectors.h>

extern void disable_irq(unsigned int);
extern void disable_irq_nosync(unsigned int);
extern void enable_irq(unsigned int);

extern int vector_irq[NR_VECTORS];
extern u8 irq_vector[NR_IRQ_VECTORS];
#define IO_APIC_VECTOR(irq)     irq_vector[irq]
#define AUTO_ASSIGN             -1

extern void (*interrupt[NR_IRQS])(void);

#define platform_legacy_irq(irq)	((irq) < 16)

void disable_8259A_irq(unsigned int irq);
void enable_8259A_irq(unsigned int irq);
int i8259A_irq_pending(unsigned int irq);
void make_8259A_irq(unsigned int irq);
void init_8259A(int aeoi);
void send_IPI_self(int vector);
void init_VISWS_APIC_irqs(void);
void setup_IO_APIC(void);
void disable_IO_APIC(void);
void print_IO_APIC(void);
int IO_APIC_get_PCI_irq_vector(int bus, int slot, int fn);
void send_IPI(int dest, int vector);
void setup_ioapic_dest(void);

extern unsigned long io_apic_irqs;

extern atomic_t irq_err_count;
extern atomic_t irq_mis_count;

#define IO_APIC_IRQ(x) (((x) >= 16) || ((1<<(x)) & io_apic_irqs))

static inline void hw_resend_irq(struct hw_interrupt_type *h, unsigned int i)
{
    if (IO_APIC_IRQ(i))
        send_IPI_self(IO_APIC_VECTOR(i));
}

#endif /* _ASM_HW_IRQ_H */
