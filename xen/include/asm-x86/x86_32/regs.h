#ifndef _I386_REGS_H
#define _I386_REGS_H

#include <asm/types.h>

struct xen_regs
{
    /* All saved activations contain the following fields. */
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 esi;
    u32 edi;
    u32 ebp;
    u32 eax;
    u16 error_code;
    u16 entry_vector;
    u32 eip;
    u32 cs;
    u32 eflags;

    /* Only saved guest activations contain the following fields. */
    u32 esp;
    u32 ss;
    u32 es;
    u32 ds;
    u32 fs;
    u32 gs;
} __attribute__ ((packed));

enum EFLAGS {
    EF_CF   = 0x00000001,
    EF_PF   = 0x00000004,
    EF_AF   = 0x00000010,
    EF_ZF   = 0x00000040,
    EF_SF   = 0x00000080,
    EF_TF   = 0x00000100,
    EF_IE   = 0x00000200,
    EF_DF   = 0x00000400,
    EF_OF   = 0x00000800,
    EF_IOPL = 0x00003000,
    EF_IOPL_RING0 = 0x00000000,
    EF_IOPL_RING1 = 0x00001000,
    EF_IOPL_RING2 = 0x00002000,
    EF_NT   = 0x00004000,   /* nested task */
    EF_RF   = 0x00010000,   /* resume */
    EF_VM   = 0x00020000,   /* virtual mode */
    EF_AC   = 0x00040000,   /* alignment */
    EF_VIF  = 0x00080000,   /* virtual interrupt */
    EF_VIP  = 0x00100000,   /* virtual interrupt pending */
    EF_ID   = 0x00200000,   /* id */
};

#define VM86_MODE(_r) ((_r)->eflags & EF_VM)
#define RING_0(_r)    (((_r)->cs & 3) == 0)
#define RING_1(_r)    (((_r)->cs & 3) == 1)
#define RING_2(_r)    (((_r)->cs & 3) == 2)
#define RING_3(_r)    (((_r)->cs & 3) == 3)

#endif
