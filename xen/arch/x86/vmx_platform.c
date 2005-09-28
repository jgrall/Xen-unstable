/*
 * vmx_platform.c: handling x86 platform related MMIO instructions
 * Copyright (c) 2004, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <asm/shadow.h>
#include <xen/domain_page.h>
#include <asm/page.h> 
#include <xen/event.h> 
#include <xen/trace.h>
#include <asm/vmx.h>
#include <asm/vmx_platform.h>
#include <public/io/ioreq.h>

#include <xen/lib.h>
#include <xen/sched.h>
#include <asm/current.h>
#if CONFIG_PAGING_LEVELS >= 3
#include <asm/shadow_64.h>
#endif
#ifdef CONFIG_VMX

#define DECODE_success  1
#define DECODE_failure  0

#if defined (__x86_64__)
void store_cpu_user_regs(struct cpu_user_regs *regs)
{
    __vmread(GUEST_SS_SELECTOR, &regs->ss);
    __vmread(GUEST_RSP, &regs->rsp);
    __vmread(GUEST_RFLAGS, &regs->rflags);
    __vmread(GUEST_CS_SELECTOR, &regs->cs);
    __vmread(GUEST_DS_SELECTOR, &regs->ds);
    __vmread(GUEST_ES_SELECTOR, &regs->es);
    __vmread(GUEST_RIP, &regs->rip);
}

static inline long __get_reg_value(unsigned long reg, int size)
{
    switch(size) {
    case BYTE_64:
        return (char)(reg & 0xFF);
    case WORD:
        return (short)(reg & 0xFFFF);
    case LONG:
        return (int)(reg & 0xFFFFFFFF);
    case QUAD:
        return (long)(reg);
    default:
        printf("Error: (__get_reg_value) Invalid reg size\n");
        domain_crash_synchronous();
    }
}

long get_reg_value(int size, int index, int seg, struct cpu_user_regs *regs) 
{
    if (size == BYTE) {
        switch (index) { 
        case 0: /* %al */
            return (char)(regs->rax & 0xFF);
        case 1: /* %cl */
            return (char)(regs->rcx & 0xFF);
        case 2: /* %dl */
            return (char)(regs->rdx & 0xFF); 
        case 3: /* %bl */
            return (char)(regs->rbx & 0xFF);
        case 4: /* %ah */
            return (char)((regs->rax & 0xFF00) >> 8);
        case 5: /* %ch */
            return (char)((regs->rcx & 0xFF00) >> 8);
        case 6: /* %dh */
            return (char)((regs->rdx & 0xFF00) >> 8);
        case 7: /* %bh */
            return (char)((regs->rbx & 0xFF00) >> 8);
        default:
            printf("Error: (get_reg_value) Invalid index value\n"); 
            domain_crash_synchronous();
        }
        /* NOTREACHED */
    }

    switch (index) {
    case 0: return __get_reg_value(regs->rax, size);
    case 1: return __get_reg_value(regs->rcx, size);
    case 2: return __get_reg_value(regs->rdx, size);
    case 3: return __get_reg_value(regs->rbx, size);
    case 4: return __get_reg_value(regs->rsp, size);
    case 5: return __get_reg_value(regs->rbp, size);
    case 6: return __get_reg_value(regs->rsi, size);
    case 7: return __get_reg_value(regs->rdi, size);
    case 8: return __get_reg_value(regs->r8, size);
    case 9: return __get_reg_value(regs->r9, size);
    case 10: return __get_reg_value(regs->r10, size);
    case 11: return __get_reg_value(regs->r11, size);
    case 12: return __get_reg_value(regs->r12, size);
    case 13: return __get_reg_value(regs->r13, size);
    case 14: return __get_reg_value(regs->r14, size);
    case 15: return __get_reg_value(regs->r15, size);
    default:
        printf("Error: (get_reg_value) Invalid index value\n"); 
        domain_crash_synchronous();
    }
}
#elif defined (__i386__)
void store_cpu_user_regs(struct cpu_user_regs *regs)
{
    __vmread(GUEST_SS_SELECTOR, &regs->ss);
    __vmread(GUEST_RSP, &regs->esp);
    __vmread(GUEST_RFLAGS, &regs->eflags);
    __vmread(GUEST_CS_SELECTOR, &regs->cs);
    __vmread(GUEST_DS_SELECTOR, &regs->ds);
    __vmread(GUEST_ES_SELECTOR, &regs->es);
    __vmread(GUEST_RIP, &regs->eip);
}

static inline long __get_reg_value(unsigned long reg, int size)
{                    
    switch(size) {
    case WORD:
        return (short)(reg & 0xFFFF);
    case LONG:
        return (int)(reg & 0xFFFFFFFF);
    default:
        printf("Error: (__get_reg_value) Invalid reg size\n");
        domain_crash_synchronous();
    }
}

long get_reg_value(int size, int index, int seg, struct cpu_user_regs *regs)
{                    
    if (size == BYTE) {
        switch (index) { 
        case 0: /* %al */
            return (char)(regs->eax & 0xFF);
        case 1: /* %cl */
            return (char)(regs->ecx & 0xFF);
        case 2: /* %dl */
            return (char)(regs->edx & 0xFF); 
        case 3: /* %bl */
            return (char)(regs->ebx & 0xFF);
        case 4: /* %ah */
            return (char)((regs->eax & 0xFF00) >> 8);
        case 5: /* %ch */
            return (char)((regs->ecx & 0xFF00) >> 8);
        case 6: /* %dh */
            return (char)((regs->edx & 0xFF00) >> 8);
        case 7: /* %bh */
            return (char)((regs->ebx & 0xFF00) >> 8);
        default:
            printf("Error: (get_reg_value) Invalid index value\n"); 
            domain_crash_synchronous();
        }
    }

    switch (index) {
    case 0: return __get_reg_value(regs->eax, size);
    case 1: return __get_reg_value(regs->ecx, size);
    case 2: return __get_reg_value(regs->edx, size);
    case 3: return __get_reg_value(regs->ebx, size);
    case 4: return __get_reg_value(regs->esp, size);
    case 5: return __get_reg_value(regs->ebp, size);
    case 6: return __get_reg_value(regs->esi, size);
    case 7: return __get_reg_value(regs->edi, size);
    default:
        printf("Error: (get_reg_value) Invalid index value\n"); 
        domain_crash_synchronous();
    }
}
#endif

static inline unsigned char *check_prefix(unsigned char *inst,
                                          struct instruction *thread_inst, unsigned char *rex_p)
{
    while (1) {
        switch (*inst) {
            /* rex prefix for em64t instructions */
        case 0x40 ... 0x4e:
            *rex_p = *inst;
            break;
        case 0xf3: /* REPZ */
            thread_inst->flags = REPZ;
            break;
        case 0xf2: /* REPNZ */
            thread_inst->flags = REPNZ;
            break;
        case 0xf0: /* LOCK */
            break;
        case 0x2e: /* CS */
        case 0x36: /* SS */
        case 0x3e: /* DS */
        case 0x26: /* ES */
        case 0x64: /* FS */
        case 0x65: /* GS */
            thread_inst->seg_sel = *inst;
            break;
        case 0x66: /* 32bit->16bit */
            thread_inst->op_size = WORD;
            break;
        case 0x67:
            printf("Error: Not handling 0x67 (yet)\n");
            domain_crash_synchronous();
            break;
        default:
            return inst;
        }
        inst++;
    }
}

static inline unsigned long get_immediate(int op16,const unsigned char *inst, int op_size)
{
    int mod, reg, rm;
    unsigned long val = 0;
    int i;

    mod = (*inst >> 6) & 3;
    reg = (*inst >> 3) & 7;
    rm = *inst & 7;

    inst++; //skip ModR/M byte
    if (mod != 3 && rm == 4) {
        inst++; //skip SIB byte
    }

    switch(mod) {
    case 0:
        if (rm == 5 || rm == 4) {
            if (op16)
                inst = inst + 2; //disp16, skip 2 bytes
            else
                inst = inst + 4; //disp32, skip 4 bytes
        }
        break;
    case 1:
        inst++; //disp8, skip 1 byte
        break;
    case 2:
        if (op16)
            inst = inst + 2; //disp16, skip 2 bytes
        else
            inst = inst + 4; //disp32, skip 4 bytes
        break;
    }

    if (op_size == QUAD)
        op_size = LONG;

    for (i = 0; i < op_size; i++) {
        val |= (*inst++ & 0xff) << (8 * i);
    }

    return val;
}

static inline int get_index(const unsigned char *inst, unsigned char rex)
{
    int mod, reg, rm;
    int rex_r, rex_b;

    mod = (*inst >> 6) & 3;
    reg = (*inst >> 3) & 7;
    rm = *inst & 7;

    rex_r = (rex >> 2) & 1;
    rex_b = rex & 1;

    //Only one operand in the instruction is register
    if (mod == 3) {
        return (rm + (rex_b << 3)); 
    } else {
        return (reg + (rex_r << 3)); 
    }
    return 0;
}

static void init_instruction(struct instruction *mmio_inst)
{
    mmio_inst->instr = 0;
    mmio_inst->op_size = 0;
    mmio_inst->immediate = 0;
    mmio_inst->seg_sel = 0;

    mmio_inst->operand[0] = 0;
    mmio_inst->operand[1] = 0;
        
    mmio_inst->flags = 0;
}

#define GET_OP_SIZE_FOR_BYTE(op_size)   \
    do {    \
     if (rex)   \
     op_size = BYTE_64;  \
 else    \
     op_size = BYTE;  \
    } while(0)

#define GET_OP_SIZE_FOR_NONEBYTE(op_size)   \
    do {    \
     if (rex & 0x8)   \
     op_size = QUAD;  \
 else if (op_size != WORD) \
     op_size = LONG;  \
    } while(0)


/*
 * Decode mem,accumulator operands (as in <opcode> m8/m16/m32, al,ax,eax)
 */
static int mem_acc(unsigned char size, struct instruction *instr)
{
    instr->operand[0] = mk_operand(size, 0, 0, MEMORY);
    instr->operand[1] = mk_operand(size, 0, 0, REGISTER);
    return DECODE_success;
}

/*
 * Decode accumulator,mem operands (as in <opcode> al,ax,eax, m8/m16/m32)
 */
static int acc_mem(unsigned char size, struct instruction *instr)
{
    instr->operand[0] = mk_operand(size, 0, 0, REGISTER);
    instr->operand[1] = mk_operand(size, 0, 0, MEMORY);
    return DECODE_success;
}

/*
 * Decode mem,reg operands (as in <opcode> r32/16, m32/16)
 */
static int mem_reg(unsigned char size, unsigned char *opcode,
                   struct instruction *instr, unsigned char rex)
{
    int index = get_index(opcode + 1, rex);

    instr->operand[0] = mk_operand(size, 0, 0, MEMORY);
    instr->operand[1] = mk_operand(size, index, 0, REGISTER);
    return DECODE_success;
}

/*
 * Decode reg,mem operands (as in <opcode> m32/16, r32/16)
 */
static int reg_mem(unsigned char size, unsigned char *opcode,
                   struct instruction *instr, unsigned char rex)
{
    int index = get_index(opcode + 1, rex);

    instr->operand[0] = mk_operand(size, index, 0, REGISTER);
    instr->operand[1] = mk_operand(size, 0, 0, MEMORY);
    return DECODE_success;
}

static int vmx_decode(unsigned char *opcode, struct instruction *instr)
{
    unsigned long eflags;
    int index, vm86 = 0;
    unsigned char rex = 0;
    unsigned char tmp_size = 0;

    init_instruction(instr);

    opcode = check_prefix(opcode, instr, &rex);

    __vmread(GUEST_RFLAGS, &eflags);
    if (eflags & X86_EFLAGS_VM)
        vm86 = 1;

    if (vm86) { /* meaning is reversed */
        if (instr->op_size == WORD)
            instr->op_size = LONG;
        else if (instr->op_size == LONG)
            instr->op_size = WORD;
        else if (instr->op_size == 0)
            instr->op_size = WORD;
    }

    switch (*opcode) {
    case 0x0B: /* or m32/16, r32/16 */
        instr->instr = INSTR_OR;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return mem_reg(instr->op_size, opcode, instr, rex);

    case 0x20: /* and r8, m8 */
        instr->instr = INSTR_AND;
        GET_OP_SIZE_FOR_BYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x21: /* and r32/16, m32/16 */
        instr->instr = INSTR_AND;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x23: /* and m32/16, r32/16 */
        instr->instr = INSTR_AND;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return mem_reg(instr->op_size, opcode, instr, rex);

    case 0x30: /* xor r8, m8 */
        instr->instr = INSTR_XOR;
        GET_OP_SIZE_FOR_BYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x31: /* xor r32/16, m32/16 */
        instr->instr = INSTR_XOR;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x39: /* cmp r32/16, m32/16 */
        instr->instr = INSTR_CMP;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x80:
    case 0x81:
        if (((opcode[1] >> 3) & 7) == 7) { /* cmp $imm, m32/16 */
            instr->instr = INSTR_CMP;

            if (opcode[0] == 0x80)
                GET_OP_SIZE_FOR_BYTE(instr->op_size);
            else
                GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);

            instr->operand[0] = mk_operand(instr->op_size, 0, 0, IMMEDIATE);
            instr->immediate = get_immediate(vm86, opcode+1, BYTE);
            instr->operand[1] = mk_operand(instr->op_size, 0, 0, MEMORY);

            return DECODE_success;
        } else
            return DECODE_failure;

    case 0x84:  /* test m8, r8 */
        instr->instr = INSTR_TEST;
        instr->op_size = BYTE;
        GET_OP_SIZE_FOR_BYTE(tmp_size);
        return mem_reg(tmp_size, opcode, instr, rex);

    case 0x88: /* mov r8, m8 */
        instr->instr = INSTR_MOV;
        instr->op_size = BYTE;
        GET_OP_SIZE_FOR_BYTE(tmp_size);
        return reg_mem(tmp_size, opcode, instr, rex);

    case 0x89: /* mov r32/16, m32/16 */
        instr->instr = INSTR_MOV;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return reg_mem(instr->op_size, opcode, instr, rex);

    case 0x8A: /* mov m8, r8 */
        instr->instr = INSTR_MOV;
        instr->op_size = BYTE;
        GET_OP_SIZE_FOR_BYTE(tmp_size);
        return mem_reg(tmp_size, opcode, instr, rex);

    case 0x8B: /* mov m32/16, r32/16 */
        instr->instr = INSTR_MOV;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return mem_reg(instr->op_size, opcode, instr, rex);

    case 0xA0: /* mov <addr>, al */
        instr->instr = INSTR_MOV;
        instr->op_size = BYTE;
        GET_OP_SIZE_FOR_BYTE(tmp_size);
        return mem_acc(tmp_size, instr);

    case 0xA1: /* mov <addr>, ax/eax */
        instr->instr = INSTR_MOV;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return mem_acc(instr->op_size, instr);

    case 0xA2: /* mov al, <addr> */
        instr->instr = INSTR_MOV;
        instr->op_size = BYTE;
        GET_OP_SIZE_FOR_BYTE(tmp_size);
        return acc_mem(tmp_size, instr);

    case 0xA3: /* mov ax/eax, <addr> */
        instr->instr = INSTR_MOV;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return acc_mem(instr->op_size, instr);

    case 0xA4: /* movsb */
        instr->instr = INSTR_MOVS;
        instr->op_size = BYTE;
        return DECODE_success;
            
    case 0xA5: /* movsw/movsl */
        instr->instr = INSTR_MOVS;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return DECODE_success;
    
    case 0xAA: /* stosb */
        instr->instr = INSTR_STOS;
        instr->op_size = BYTE;
        return DECODE_success;

    case 0xAB: /* stosw/stosl */
        instr->instr = INSTR_STOS;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        return DECODE_success;
                    
    case 0xC6:
        if (((opcode[1] >> 3) & 7) == 0) { /* mov $imm8, m8 */
            instr->instr = INSTR_MOV;
            instr->op_size = BYTE;

            instr->operand[0] = mk_operand(instr->op_size, 0, 0, IMMEDIATE);
            instr->immediate = get_immediate(vm86, opcode+1, instr->op_size);
            instr->operand[1] = mk_operand(instr->op_size, 0, 0, MEMORY);
            
            return DECODE_success;
        } else
            return DECODE_failure;
            
    case 0xC7:
        if (((opcode[1] >> 3) & 7) == 0) { /* mov $imm16/32, m16/32 */
            instr->instr = INSTR_MOV;
            GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);

            instr->operand[0] = mk_operand(instr->op_size, 0, 0, IMMEDIATE);
            instr->immediate = get_immediate(vm86, opcode+1, instr->op_size);
            instr->operand[1] = mk_operand(instr->op_size, 0, 0, MEMORY);
            
            return DECODE_success;
        } else
            return DECODE_failure;

    case 0xF6:
        if (((opcode[1] >> 3) & 7) == 0) { /* testb $imm8, m8 */
            instr->instr = INSTR_TEST;
            instr->op_size = BYTE;

            instr->operand[0] = mk_operand(instr->op_size, 0, 0, IMMEDIATE);
            instr->immediate = get_immediate(vm86, opcode+1, instr->op_size);
            instr->operand[1] = mk_operand(instr->op_size, 0, 0, MEMORY);

            return DECODE_success;
        } else
            return DECODE_failure;

    case 0x0F:
        break;

    default:
        printf("%x, This opcode isn't handled yet!\n", *opcode);
        return DECODE_failure;
    }

    switch (*++opcode) {
    case 0xB6: /* movz m8, r16/r32 */
        instr->instr = INSTR_MOVZ;
        GET_OP_SIZE_FOR_NONEBYTE(instr->op_size);
        index = get_index(opcode + 1, rex);
        instr->operand[0] = mk_operand(BYTE, 0, 0, MEMORY);
        instr->operand[1] = mk_operand(instr->op_size, index, 0, REGISTER);
        return DECODE_success;

    case 0xB7: /* movz m16, r32 */
        instr->instr = INSTR_MOVZ;
        index = get_index(opcode + 1, rex);
        if (rex & 0x8) {
            instr->op_size = LONG;
            instr->operand[1] = mk_operand(QUAD, index, 0, REGISTER);
        } else {
            instr->op_size = WORD;
            instr->operand[1] = mk_operand(LONG, index, 0, REGISTER);
        }
        instr->operand[0] = mk_operand(instr->op_size, 0, 0, MEMORY);
        return DECODE_success;

    default:
        printf("0f %x, This opcode isn't handled yet\n", *opcode);
        return DECODE_failure;
    }
}

int inst_copy_from_guest(unsigned char *buf, unsigned long guest_eip, int inst_len)
{
    if (inst_len > MAX_INST_LEN || inst_len <= 0)
        return 0;
    if (!vmx_copy(buf, guest_eip, inst_len, VMX_COPY_IN))
        return 0;
    return inst_len;
}

void send_mmio_req(unsigned char type, unsigned long gpa, 
                   unsigned long count, int size, long value, int dir, int pvalid)
{
    struct vcpu *d = current;
    vcpu_iodata_t *vio;
    ioreq_t *p;
    int vm86;
    struct cpu_user_regs *regs;
    extern long evtchn_send(int lport);

    regs = current->domain->arch.vmx_platform.mpci.inst_decoder_regs;

    vio = get_vio(d->domain, d->vcpu_id);
    if (vio == NULL) {
        printf("bad shared page\n");
        domain_crash_synchronous(); 
    }

    p = &vio->vp_ioreq;

    vm86 = regs->eflags & X86_EFLAGS_VM;

    if (test_bit(ARCH_VMX_IO_WAIT, &d->arch.arch_vmx.flags)) {
        printf("VMX I/O has not yet completed\n");
        domain_crash_synchronous();
    }

    set_bit(ARCH_VMX_IO_WAIT, &d->arch.arch_vmx.flags);
    p->dir = dir;
    p->pdata_valid = pvalid;

    p->type = type;
    p->size = size;
    p->addr = gpa;
    p->count = count;
    p->df = regs->eflags & EF_DF ? 1 : 0;

    if (pvalid) {
        if (vmx_paging_enabled(current))
            p->u.pdata = (void *) gva_to_gpa(value);
        else
            p->u.pdata = (void *) value; /* guest VA == guest PA */
    } else
        p->u.data = value;

    p->state = STATE_IOREQ_READY;

    if (vmx_mmio_intercept(p)){
        p->state = STATE_IORESP_READY;
        vmx_io_assist(d);
        return;
    }

    evtchn_send(iopacket_port(d->domain));
    vmx_wait_io();
}

static void mmio_operands(int type, unsigned long gpa, struct instruction *inst,
                          struct mi_per_cpu_info *mpcip, struct cpu_user_regs *regs)
{
    unsigned long value = 0;
    int index, size;
    
    size = operand_size(inst->operand[0]);

    mpcip->flags = inst->flags;
    mpcip->instr = inst->instr;
    mpcip->operand[0] = inst->operand[0]; /* source */
    mpcip->operand[1] = inst->operand[1]; /* destination */
    mpcip->immediate = inst->immediate;

    if (inst->operand[0] & REGISTER) { /* dest is memory */
        index = operand_index(inst->operand[0]);
        value = get_reg_value(size, index, 0, regs);
        send_mmio_req(type, gpa, 1, inst->op_size, value, IOREQ_WRITE, 0);
    } else if (inst->operand[0] & IMMEDIATE) { /* dest is memory */
        value = inst->immediate;
        send_mmio_req(type, gpa, 1, inst->op_size, value, IOREQ_WRITE, 0);
    } else if (inst->operand[0] & MEMORY) { /* dest is register */
        /* send the request and wait for the value */
        send_mmio_req(type, gpa, 1, inst->op_size, 0, IOREQ_READ, 0);
    } else {
        printf("mmio_operands: invalid operand\n");
        domain_crash_synchronous();
    }
}

#define GET_REPEAT_COUNT() \
     (mmio_inst.flags & REPZ ? (vm86 ? regs->ecx & 0xFFFF : regs->ecx) : 1)
 
void handle_mmio(unsigned long va, unsigned long gpa)
{
    unsigned long eip, eflags, cs;
    unsigned long inst_len, inst_addr;
    struct mi_per_cpu_info *mpcip;
    struct cpu_user_regs *regs;
    struct instruction mmio_inst;
    unsigned char inst[MAX_INST_LEN];
    int i, vm86, ret;
     
    mpcip = &current->domain->arch.vmx_platform.mpci;
    regs = mpcip->inst_decoder_regs;

    __vmread(GUEST_RIP, &eip);
    __vmread(VM_EXIT_INSTRUCTION_LEN, &inst_len);
    __vmread(GUEST_RFLAGS, &eflags);
    vm86 = eflags & X86_EFLAGS_VM;

    if (vm86) {
        __vmread(GUEST_CS_SELECTOR, &cs);
        inst_addr = (cs << 4) + eip;
    } else
        inst_addr = eip;

    memset(inst, 0, MAX_INST_LEN);
    ret = inst_copy_from_guest(inst, inst_addr, inst_len);
    if (ret != inst_len) {
        printf("handle_mmio - EXIT: get guest instruction fault\n");
        domain_crash_synchronous();
    }

    init_instruction(&mmio_inst);
    
    if (vmx_decode(inst, &mmio_inst) == DECODE_failure) {
        printf("mmio opcode: va 0x%lx, gpa 0x%lx, len %ld:",
               va, gpa, inst_len);
        for (i = 0; i < inst_len; i++)
            printf(" %02x", inst[i] & 0xFF);
        printf("\n");
        domain_crash_synchronous();
    }

    store_cpu_user_regs(regs);
    regs->eip += inst_len; /* advance %eip */

    switch (mmio_inst.instr) {
    case INSTR_MOV:
        mmio_operands(IOREQ_TYPE_COPY, gpa, &mmio_inst, mpcip, regs);
        break;

    case INSTR_MOVS:
    {
        unsigned long count = GET_REPEAT_COUNT();
        unsigned long size = mmio_inst.op_size;
        int sign = regs->eflags & EF_DF ? -1 : 1;
        unsigned long addr = 0;
        int dir;

        /* determine non-MMIO address */
        if (vm86) {
            unsigned long seg;

            __vmread(GUEST_ES_SELECTOR, &seg);
            if (((seg << 4) + (regs->edi & 0xFFFF)) == va) {
                dir = IOREQ_WRITE;
                __vmread(GUEST_DS_SELECTOR, &seg);
                addr = (seg << 4) + (regs->esi & 0xFFFF);
            } else {
                dir = IOREQ_READ;
                addr = (seg << 4) + (regs->edi & 0xFFFF);
            }
        } else {
            if (va == regs->edi) {
                dir = IOREQ_WRITE;
                addr = regs->esi;
            } else {
                dir = IOREQ_READ;
                addr = regs->edi;
            }
        }

        mpcip->flags = mmio_inst.flags;
        mpcip->instr = mmio_inst.instr;

        /*
         * In case of a movs spanning multiple pages, we break the accesses
         * up into multiple pages (the device model works with non-continguous
         * physical guest pages). To copy just one page, we adjust %ecx and
         * do not advance %eip so that the next "rep movs" copies the next page.
         * Unaligned accesses, for example movsl starting at PGSZ-2, are
         * turned into a single copy where we handle the overlapping memory
         * copy ourself. After this copy succeeds, "rep movs" is executed
         * again.
         */
        if ((addr & PAGE_MASK) != ((addr + size - 1) & PAGE_MASK)) {
            unsigned long value = 0;

            mpcip->flags |= OVERLAP;

            regs->eip -= inst_len; /* do not advance %eip */

            if (dir == IOREQ_WRITE)
                vmx_copy(&value, addr, size, VMX_COPY_IN);
            send_mmio_req(IOREQ_TYPE_COPY, gpa, 1, size, value, dir, 0);
        } else {
            if ((addr & PAGE_MASK) != ((addr + count * size - 1) & PAGE_MASK)) {
                regs->eip -= inst_len; /* do not advance %eip */

                if (sign > 0)
                    count = (PAGE_SIZE - (addr & ~PAGE_MASK)) / size;
                else
                    count = (addr & ~PAGE_MASK) / size;
            }

            send_mmio_req(IOREQ_TYPE_COPY, gpa, count, size, addr, dir, 1);
        }
        break;
    }

    case INSTR_MOVZ:
        mmio_operands(IOREQ_TYPE_COPY, gpa, &mmio_inst, mpcip, regs);
        break;

    case INSTR_STOS:
        /*
         * Since the destination is always in (contiguous) mmio space we don't
         * need to break it up into pages.
         */
        mpcip->flags = mmio_inst.flags;
        mpcip->instr = mmio_inst.instr;
        send_mmio_req(IOREQ_TYPE_COPY, gpa,
                      GET_REPEAT_COUNT(), mmio_inst.op_size, regs->eax, IOREQ_WRITE, 0);
        break;

    case INSTR_OR:
        mmio_operands(IOREQ_TYPE_OR, gpa, &mmio_inst, mpcip, regs);
        break;

    case INSTR_AND:
        mmio_operands(IOREQ_TYPE_AND, gpa, &mmio_inst, mpcip, regs);
        break;

    case INSTR_XOR:
        mmio_operands(IOREQ_TYPE_XOR, gpa, &mmio_inst, mpcip, regs);
        break;

    case INSTR_CMP:        /* Pass through */
    case INSTR_TEST:
        mpcip->flags = mmio_inst.flags;
        mpcip->instr = mmio_inst.instr;
        mpcip->operand[0] = mmio_inst.operand[0]; /* source */
        mpcip->operand[1] = mmio_inst.operand[1]; /* destination */
        mpcip->immediate = mmio_inst.immediate;

        /* send the request and wait for the value */
        send_mmio_req(IOREQ_TYPE_COPY, gpa, 1, mmio_inst.op_size, 0, IOREQ_READ, 0);
        break;

    default:
        printf("Unhandled MMIO instruction\n");
        domain_crash_synchronous();
    }
}

#endif /* CONFIG_VMX */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
