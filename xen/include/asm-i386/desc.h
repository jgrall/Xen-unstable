#ifndef __ARCH_DESC_H
#define __ARCH_DESC_H

#define __FIRST_TSS_ENTRY 8
#define __FIRST_LDT_ENTRY (__FIRST_TSS_ENTRY+1)

#define __TSS(n) (((n)<<1) + __FIRST_TSS_ENTRY)
#define __LDT(n) (((n)<<1) + __FIRST_LDT_ENTRY)

#define load_TR(n)  __asm__ __volatile__ ( "ltr  %%ax" : : "a" (__TSS(n)<<3) )
#define __load_LDT(n) __asm__ __volatile__ ( "lldt %%ax" : : "a" (n) )

/* Guest OS must provide its own code selectors, or use the one we provide. */
#define VALID_CODESEL(_s) \
    ((((_s)>>2) >= FIRST_DOMAIN_GDT_ENTRY) || ((_s) == FLAT_RING1_CS))

/* These are bitmasks for the first 32 bits of a descriptor table entry. */
#define _SEGMENT_TYPE    (15<< 8)
#define _SEGMENT_S       ( 1<<12) /* System descriptor (yes iff S==0) */
#define _SEGMENT_DPL     ( 3<<13) /* Descriptor Privilege Level */
#define _SEGMENT_P       ( 1<<15) /* Segment Present */
#define _SEGMENT_G       ( 1<<23) /* Granularity */

#ifndef __ASSEMBLY__
struct desc_struct {
	unsigned long a,b;
};

extern struct desc_struct gdt_table[];
extern struct desc_struct *idt, *gdt;

struct Xgt_desc_struct {
	unsigned short size;
	unsigned long address __attribute__((packed));
};

#define idt_descr (*(struct Xgt_desc_struct *)((char *)&idt - 2))
#define gdt_descr (*(struct Xgt_desc_struct *)((char *)&gdt - 2))

extern void set_intr_gate(unsigned int irq, void * addr);
extern void set_tss_desc(unsigned int n, void *addr);

#endif /* !__ASSEMBLY__ */

#endif
