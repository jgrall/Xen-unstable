#ifndef __ARCH_DESC_H
#define __ARCH_DESC_H
#ifndef __ASSEMBLY__

#define LDT_ENTRY_SIZE 8

#define load_TR(n)  __asm__ __volatile__ ("ltr  %%ax" : : "a" (__TSS(n)<<3) )

#if defined(__x86_64__)
#define GUEST_KERNEL_RPL 3
#elif defined(__i386__)
#define GUEST_KERNEL_RPL 1
#endif

/*
 * Guest OS must provide its own code selectors, or use the one we provide. Any
 * LDT selector value is okay. Note that checking only the RPL is insufficient:
 * if the selector is poked into an interrupt, trap or call gate then the RPL
 * is ignored when the gate is accessed.
 */
#define VALID_SEL(_s)                                                      \
    (((((_s)>>3) < FIRST_RESERVED_GDT_ENTRY) ||                            \
      (((_s)>>3) >  LAST_RESERVED_GDT_ENTRY) ||                            \
      ((_s)&4)) &&                                                         \
     (((_s)&3) == GUEST_KERNEL_RPL))
#define VALID_CODESEL(_s) ((_s) == FLAT_KERNEL_CS || VALID_SEL(_s))

/* These are bitmasks for the high 32 bits of a descriptor table entry. */
#define _SEGMENT_TYPE    (15<< 8)
#define _SEGMENT_EC      ( 1<<10) /* Expand-down or Conforming segment */
#define _SEGMENT_CODE    ( 1<<11) /* Code (vs data) segment for non-system
                                     segments */
#define _SEGMENT_S       ( 1<<12) /* System descriptor (yes iff S==0) */
#define _SEGMENT_DPL     ( 3<<13) /* Descriptor Privilege Level */
#define _SEGMENT_P       ( 1<<15) /* Segment Present */
#define _SEGMENT_DB      ( 1<<22) /* 16- or 32-bit segment */
#define _SEGMENT_G       ( 1<<23) /* Granularity */

struct desc_struct {
    u32 a, b;
};

#if defined(__x86_64__)

#define __FIRST_TSS_ENTRY (FIRST_RESERVED_GDT_ENTRY + 8)
#define __FIRST_LDT_ENTRY (__FIRST_TSS_ENTRY + 2)

#define __TSS(n) (((n)<<2) + __FIRST_TSS_ENTRY)
#define __LDT(n) (((n)<<2) + __FIRST_LDT_ENTRY)

typedef struct {
    u64 a, b;
} idt_entry_t;

#define _set_gate(gate_addr,type,dpl,addr)               \
do {                                                     \
    (gate_addr)->a =                                     \
        (((unsigned long)(addr) & 0xFFFF0000UL) << 32) | \
        ((unsigned long)(dpl) << 45) |                   \
        ((unsigned long)(type) << 40) |                  \
        ((unsigned long)(addr) & 0xFFFFUL) |             \
        ((unsigned long)__HYPERVISOR_CS64 << 16) |       \
        (1UL << 47);                                     \
    (gate_addr)->b =                                     \
        ((unsigned long)(addr) >> 32);                   \
} while (0)

#define _set_tssldt_desc(desc,addr,limit,type)           \
do {                                                     \
    (desc)[0].a =                                        \
        ((u32)(addr) << 16) | ((u32)(limit) & 0xFFFF);   \
    (desc)[0].b =                                        \
        ((u32)(addr) & 0xFF000000U) |                    \
        ((u32)(type) << 8) | 0x8000U |                   \
        (((u32)(addr) & 0x00FF0000U) >> 16);             \
    (desc)[1].a = (u32)(((unsigned long)(addr)) >> 32);  \
    (desc)[1].b = 0;                                     \
} while (0)

#elif defined(__i386__)

#define __DOUBLEFAULT_TSS_ENTRY FIRST_RESERVED_GDT_ENTRY

#define __FIRST_TSS_ENTRY (FIRST_RESERVED_GDT_ENTRY + 8)
#define __FIRST_LDT_ENTRY (__FIRST_TSS_ENTRY + 1)

#define __TSS(n) (((n)<<1) + __FIRST_TSS_ENTRY)
#define __LDT(n) (((n)<<1) + __FIRST_LDT_ENTRY)

typedef struct desc_struct idt_entry_t;

#define _set_gate(gate_addr,type,dpl,addr) \
do { \
  int __d0, __d1; \
  __asm__ __volatile__ ("movw %%dx,%%ax\n\t" \
 "movw %4,%%dx\n\t" \
 "movl %%eax,%0\n\t" \
 "movl %%edx,%1" \
 :"=m" (*((long *) (gate_addr))), \
  "=m" (*(1+(long *) (gate_addr))), "=&a" (__d0), "=&d" (__d1) \
 :"i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
  "3" ((char *) (addr)),"2" (__HYPERVISOR_CS << 16)); \
} while (0)

#define _set_tssldt_desc(n,addr,limit,type) \
__asm__ __volatile__ ("movw %w3,0(%2)\n\t" \
 "movw %%ax,2(%2)\n\t" \
 "rorl $16,%%eax\n\t" \
 "movb %%al,4(%2)\n\t" \
 "movb %4,5(%2)\n\t" \
 "movb $0,6(%2)\n\t" \
 "movb %%ah,7(%2)\n\t" \
 "rorl $16,%%eax" \
 : "=m"(*(n)) : "a" (addr), "r"(n), "ir"(limit), "i"(type|0x80))

#endif

extern struct desc_struct gdt_table[];
extern struct desc_struct *gdt;
extern idt_entry_t        *idt;

struct Xgt_desc_struct {
    unsigned short size;
    unsigned long address __attribute__((packed));
};

#define idt_descr (*(struct Xgt_desc_struct *)((char *)&idt - 2))
#define gdt_descr (*(struct Xgt_desc_struct *)((char *)&gdt - 2))

extern void set_intr_gate(unsigned int irq, void * addr);
extern void set_system_gate(unsigned int n, void *addr);
extern void set_task_gate(unsigned int n, unsigned int sel);
extern void set_tss_desc(unsigned int n, void *addr);

#endif /* !__ASSEMBLY__ */
#endif /* __ARCH_DESC_H */
