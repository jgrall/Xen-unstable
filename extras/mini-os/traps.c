
#include <os.h>
#include <hypervisor.h>
#include <mm.h>
#include <lib.h>

/*
 * These are assembler stubs in entry.S.
 * They are the actual entry points for virtual exceptions.
 */
void divide_error(void);
void debug(void);
void int3(void);
void overflow(void);
void bounds(void);
void invalid_op(void);
void device_not_available(void);
void coprocessor_segment_overrun(void);
void invalid_TSS(void);
void segment_not_present(void);
void stack_segment(void);
void general_protection(void);
void page_fault(void);
void coprocessor_error(void);
void simd_coprocessor_error(void);
void alignment_check(void);
void spurious_interrupt_bug(void);
void machine_check(void);


extern void do_exit(void);

void dump_regs(struct pt_regs *regs)
{
    unsigned long esp;
    unsigned short ss;

#ifdef __x86_64__
    esp = regs->rsp;
    ss  = regs->ss;
#else
    esp = (unsigned long) (&regs->esp);
    ss = __KERNEL_DS;
    if (regs->cs & 2) {
        esp = regs->esp;
        ss = regs->ss & 0xffff;
    }
#endif
    printf("EIP:    %04x:[<%p>] %08x\n",
           0xffff & regs->cs , regs->eip, regs->error_code);
    printf("EFLAGS: %p\n",regs->eflags);
    printf("eax: %p   ebx: %p   ecx: %p   edx: %p\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printf("esi: %p   edi: %p   ebp: %p   esp: %p\n",
           regs->esi, regs->edi, regs->ebp, esp);
#ifdef __x86_64__
    printf("r8 : %p   r9 : %p   r10: %p   r11: %p\n",
           regs->r8,  regs->r9,  regs->r10, regs->r11);
    printf("r12: %p   r13: %p   r14: %p   r15: %p\n",
           regs->r12, regs->r13, regs->r14, regs->r15);
#endif
    printf("ds: %04x   es: %04x   ss: %04x\n",
           regs->ds & 0xffff, regs->es & 0xffff, ss);
}	


static __inline__ void dump_code(unsigned long eip)
{
    unsigned char *ptr = (unsigned char *)eip;
    int x;
    
    printk("Bytes at eip: ");
    for ( x = -4; x < 5; x++ )
        printf("%02x ", ptr[x]);
    printk("\n");
}

static void __inline__ do_trap(int trapnr, char *str,
                               struct pt_regs * regs)
{
    printk("FATAL:  Unhandled Trap %d (%s)\n", trapnr, str);
    dump_regs(regs);
    dump_code(regs->eip);
    do_exit();
}

#define DO_ERROR(trapnr, str, name) \
void do_##name(struct pt_regs * regs) \
{ \
	do_trap(trapnr, str, regs); \
}

#define DO_ERROR_INFO(trapnr, str, name, sicode, siaddr) \
void do_##name(struct pt_regs * regs) \
{ \
	do_trap(trapnr, str, regs); \
}

DO_ERROR_INFO( 0, "divide error", divide_error, FPE_INTDIV, regs->eip)
DO_ERROR( 3, "int3", int3)
DO_ERROR( 4, "overflow", overflow)
DO_ERROR( 5, "bounds", bounds)
DO_ERROR_INFO( 6, "invalid operand", invalid_op, ILL_ILLOPN, regs->eip)
DO_ERROR( 7, "device not available", device_not_available)
DO_ERROR( 9, "coprocessor segment overrun", coprocessor_segment_overrun)
DO_ERROR(10, "invalid TSS", invalid_TSS)
DO_ERROR(11, "segment not present", segment_not_present)
DO_ERROR(12, "stack segment", stack_segment)
DO_ERROR_INFO(17, "alignment check", alignment_check, BUS_ADRALN, 0)
DO_ERROR(18, "machine check", machine_check)

extern unsigned long virt_cr2;
void do_page_fault(struct pt_regs *regs)
{
    unsigned long addr = virt_cr2;
    printk("Page fault at linear address %p\n", addr);
    dump_regs(regs);
    dump_code(regs->eip);
#ifdef __x86_64__
    {
        unsigned long *tab = (unsigned long *)start_info.pt_base;
        unsigned long page;
    
        printk("Pagetable walk from %p:\n", tab);
        
        page = tab[l4_table_offset(addr)];
        tab = __va(mfn_to_pfn(pte_to_mfn(page)) << PAGE_SHIFT);
        printk(" L4 = %p (%p)\n", page, tab);
        if ( !(page & _PAGE_PRESENT) )
            goto out;

        page = tab[l3_table_offset(addr)];
        tab = __va(mfn_to_pfn(pte_to_mfn(page)) << PAGE_SHIFT);
        printk("  L3 = %p (%p)\n", page, tab);
        if ( !(page & _PAGE_PRESENT) )
            goto out;
        
        page = tab[l2_table_offset(addr)];
        tab = __va(mfn_to_pfn(pte_to_mfn(page)) << PAGE_SHIFT);
        printk("   L2 = %p (%p) %s\n", page, tab,
               (page & _PAGE_PSE) ? "(2MB)" : "");
        if ( !(page & _PAGE_PRESENT) || (page & _PAGE_PSE) )
            goto out;
        
        page = tab[l1_table_offset(addr)];
        printk("    L1 = %p\n", page);
    }
#endif
 out:
    do_exit();
}

void do_general_protection(struct pt_regs *regs)
{
    printk("GPF\n");
    dump_regs(regs);
    dump_code(regs->eip);
    do_exit();
}


void do_debug(struct pt_regs * regs)
{
    printk("Debug exception\n");
#define TF_MASK 0x100
    regs->eflags &= ~TF_MASK;
    dump_regs(regs);
    do_exit();
}

void do_coprocessor_error(struct pt_regs * regs)
{
    printk("Copro error\n");
    dump_regs(regs);
    dump_code(regs->eip);
    do_exit();
}

void simd_math_error(void *eip)
{
    printk("SIMD error\n");
}

void do_simd_coprocessor_error(struct pt_regs * regs)
{
    printk("SIMD copro error\n");
}

void do_spurious_interrupt_bug(struct pt_regs * regs)
{
}

/*
 * Submit a virtual IDT to teh hypervisor. This consists of tuples
 * (interrupt vector, privilege ring, CS:EIP of handler).
 * The 'privilege ring' field specifies the least-privileged ring that
 * can trap to that vector using a software-interrupt instruction (INT).
 */
#ifdef __x86_64__
#define _P 0,
#endif
static trap_info_t trap_table[] = {
    {  0, 0, __KERNEL_CS, _P (unsigned long)divide_error                },
    {  1, 0, __KERNEL_CS, _P (unsigned long)debug                       },
    {  3, 3, __KERNEL_CS, _P (unsigned long)int3                        },
    {  4, 3, __KERNEL_CS, _P (unsigned long)overflow                    },
    {  5, 3, __KERNEL_CS, _P (unsigned long)bounds                      },
    {  6, 0, __KERNEL_CS, _P (unsigned long)invalid_op                  },
    {  7, 0, __KERNEL_CS, _P (unsigned long)device_not_available        },
    {  9, 0, __KERNEL_CS, _P (unsigned long)coprocessor_segment_overrun },
    { 10, 0, __KERNEL_CS, _P (unsigned long)invalid_TSS                 },
    { 11, 0, __KERNEL_CS, _P (unsigned long)segment_not_present         },
    { 12, 0, __KERNEL_CS, _P (unsigned long)stack_segment               },
    { 13, 0, __KERNEL_CS, _P (unsigned long)general_protection          },
    { 14, 0, __KERNEL_CS, _P (unsigned long)page_fault                  },
    { 15, 0, __KERNEL_CS, _P (unsigned long)spurious_interrupt_bug      },
    { 16, 0, __KERNEL_CS, _P (unsigned long)coprocessor_error           },
    { 17, 0, __KERNEL_CS, _P (unsigned long)alignment_check             },
    { 18, 0, __KERNEL_CS, _P (unsigned long)machine_check               },
    { 19, 0, __KERNEL_CS, _P (unsigned long)simd_coprocessor_error      },
    {  0, 0,           0, 0                           }
};
    


void trap_init(void)
{
    HYPERVISOR_set_trap_table(trap_table);    
}
