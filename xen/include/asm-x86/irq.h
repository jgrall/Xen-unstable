#ifndef _ASM_HW_IRQ_H
#define _ASM_HW_IRQ_H

/* (C) 1992, 1993 Linus Torvalds, (C) 1997 Ingo Molnar */

#include <xen/config.h>
#include <asm/atomic.h>
#include <asm/asm_defns.h>
#include <irq_vectors.h>

#define IO_APIC_IRQ(irq)    (((irq) >= 16) || ((1<<(irq)) & io_apic_irqs))
#define IO_APIC_VECTOR(irq) (irq_vector[irq])

#define LEGACY_VECTOR(irq)          ((irq) + FIRST_EXTERNAL_VECTOR)
#define LEGACY_IRQ_FROM_VECTOR(vec) ((vec) - FIRST_EXTERNAL_VECTOR)

#define irq_to_vector(irq)  \
    (IO_APIC_IRQ(irq) ? IO_APIC_VECTOR(irq) : LEGACY_VECTOR(irq))
#define vector_to_irq(vec)  (vector_irq[vec])

extern int vector_irq[NR_VECTORS];
extern u8 irq_vector[NR_IRQ_VECTORS];
#define AUTO_ASSIGN             -1

#define platform_legacy_irq(irq)	((irq) < 16)

void disable_8259A_irq(unsigned int irq);
void enable_8259A_irq(unsigned int irq);
int i8259A_irq_pending(unsigned int irq);
void init_8259A(int aeoi);

void setup_IO_APIC(void);
void disable_IO_APIC(void);
void print_IO_APIC(void);
void setup_ioapic_dest(void);

extern unsigned long io_apic_irqs;

extern atomic_t irq_err_count;
extern atomic_t irq_mis_count;

#endif /* _ASM_HW_IRQ_H */
