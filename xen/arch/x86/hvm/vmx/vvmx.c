/*
 * vvmx.c: Support virtual VMX for nested virtualization.
 *
 * Copyright (c) 2010, Intel Corporation.
 * Author: Qing He <qing.he@intel.com>
 *         Eddie Dong <eddie.dong@intel.com>
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
#include <asm/types.h>
#include <asm/p2m.h>
#include <asm/hvm/vmx/vmx.h>
#include <asm/hvm/vmx/vvmx.h>

static void nvmx_purge_vvmcs(struct vcpu *v);

int nvmx_vcpu_initialise(struct vcpu *v)
{
    struct nestedvmx *nvmx = &vcpu_2_nvmx(v);
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);

    nvcpu->nv_n2vmcx = alloc_xenheap_page();
    if ( !nvcpu->nv_n2vmcx )
    {
        gdprintk(XENLOG_ERR, "nest: allocation for shadow vmcs failed\n");
	goto out;
    }
    nvmx->vmxon_region_pa = 0;
    nvcpu->nv_vvmcx = NULL;
    nvcpu->nv_vvmcxaddr = VMCX_EADDR;
    nvmx->intr.intr_info = 0;
    nvmx->intr.error_code = 0;
    nvmx->iobitmap[0] = NULL;
    nvmx->iobitmap[1] = NULL;
    return 0;
out:
    return -ENOMEM;
}
 
void nvmx_vcpu_destroy(struct vcpu *v)
{
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);

    nvmx_purge_vvmcs(v);
    if ( nvcpu->nv_n2vmcx ) {
        __vmpclear(virt_to_maddr(nvcpu->nv_n2vmcx));
        free_xenheap_page(nvcpu->nv_n2vmcx);
        nvcpu->nv_n2vmcx = NULL;
    }
}
 
int nvmx_vcpu_reset(struct vcpu *v)
{
    return 0;
}

uint64_t nvmx_vcpu_guestcr3(struct vcpu *v)
{
    /* TODO */
    ASSERT(0);
    return 0;
}

uint64_t nvmx_vcpu_hostcr3(struct vcpu *v)
{
    /* TODO */
    ASSERT(0);
    return 0;
}

uint32_t nvmx_vcpu_asid(struct vcpu *v)
{
    /* TODO */
    ASSERT(0);
    return 0;
}

enum x86_segment sreg_to_index[] = {
    [VMX_SREG_ES] = x86_seg_es,
    [VMX_SREG_CS] = x86_seg_cs,
    [VMX_SREG_SS] = x86_seg_ss,
    [VMX_SREG_DS] = x86_seg_ds,
    [VMX_SREG_FS] = x86_seg_fs,
    [VMX_SREG_GS] = x86_seg_gs,
};

struct vmx_inst_decoded {
#define VMX_INST_MEMREG_TYPE_MEMORY 0
#define VMX_INST_MEMREG_TYPE_REG    1
    int type;
    union {
        struct {
            unsigned long mem;
            unsigned int  len;
        };
        enum vmx_regs_enc reg1;
    };

    enum vmx_regs_enc reg2;
};

enum vmx_ops_result {
    VMSUCCEED,
    VMFAIL_VALID,
    VMFAIL_INVALID,
};

#define CASE_GET_REG(REG, reg)      \
    case VMX_REG_ ## REG: value = regs->reg; break

static int vvmcs_offset(u32 width, u32 type, u32 index)
{
    int offset;

    offset = (index & 0x1f) | type << 5 | width << 7;

    if ( offset == 0 )    /* vpid */
        offset = 0x3f;

    return offset;
}

u64 __get_vvmcs(void *vvmcs, u32 vmcs_encoding)
{
    union vmcs_encoding enc;
    u64 *content = (u64 *) vvmcs;
    int offset;
    u64 res;

    enc.word = vmcs_encoding;
    offset = vvmcs_offset(enc.width, enc.type, enc.index);
    res = content[offset];

    switch ( enc.width ) {
    case VVMCS_WIDTH_16:
        res &= 0xffff;
        break;
   case VVMCS_WIDTH_64:
        if ( enc.access_type )
            res >>= 32;
        break;
    case VVMCS_WIDTH_32:
        res &= 0xffffffff;
        break;
    case VVMCS_WIDTH_NATURAL:
    default:
        break;
    }

    return res;
}

void __set_vvmcs(void *vvmcs, u32 vmcs_encoding, u64 val)
{
    union vmcs_encoding enc;
    u64 *content = (u64 *) vvmcs;
    int offset;
    u64 res;

    enc.word = vmcs_encoding;
    offset = vvmcs_offset(enc.width, enc.type, enc.index);
    res = content[offset];

    switch ( enc.width ) {
    case VVMCS_WIDTH_16:
        res = val & 0xffff;
        break;
    case VVMCS_WIDTH_64:
        if ( enc.access_type )
        {
            res &= 0xffffffff;
            res |= val << 32;
        }
        else
            res = val;
        break;
    case VVMCS_WIDTH_32:
        res = val & 0xffffffff;
        break;
    case VVMCS_WIDTH_NATURAL:
    default:
        res = val;
        break;
    }

    content[offset] = res;
}

static unsigned long reg_read(struct cpu_user_regs *regs,
                              enum vmx_regs_enc index)
{
    unsigned long value = 0;

    switch ( index ) {
    CASE_GET_REG(RAX, eax);
    CASE_GET_REG(RCX, ecx);
    CASE_GET_REG(RDX, edx);
    CASE_GET_REG(RBX, ebx);
    CASE_GET_REG(RBP, ebp);
    CASE_GET_REG(RSI, esi);
    CASE_GET_REG(RDI, edi);
    CASE_GET_REG(RSP, esp);
#ifdef CONFIG_X86_64
    CASE_GET_REG(R8, r8);
    CASE_GET_REG(R9, r9);
    CASE_GET_REG(R10, r10);
    CASE_GET_REG(R11, r11);
    CASE_GET_REG(R12, r12);
    CASE_GET_REG(R13, r13);
    CASE_GET_REG(R14, r14);
    CASE_GET_REG(R15, r15);
#endif
    default:
        break;
    }

    return value;
}

static int vmx_inst_check_privilege(struct cpu_user_regs *regs, int vmxop_check)
{
    struct vcpu *v = current;
    struct segment_register cs;

    hvm_get_segment_register(v, x86_seg_cs, &cs);

    if ( vmxop_check )
    {
        if ( !(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PE) ||
             !(v->arch.hvm_vcpu.guest_cr[4] & X86_CR4_VMXE) )
            goto invalid_op;
    }
    else if ( !vcpu_2_nvmx(v).vmxon_region_pa )
        goto invalid_op;

    if ( (regs->eflags & X86_EFLAGS_VM) ||
         (hvm_long_mode_enabled(v) && cs.attr.fields.l == 0) )
        goto invalid_op;
    /* TODO: check vmx operation mode */

    if ( (cs.sel & 3) > 0 )
        goto gp_fault;

    return X86EMUL_OKAY;

invalid_op:
    gdprintk(XENLOG_ERR, "vmx_inst_check_privilege: invalid_op\n");
    hvm_inject_exception(TRAP_invalid_op, 0, 0);
    return X86EMUL_EXCEPTION;

gp_fault:
    gdprintk(XENLOG_ERR, "vmx_inst_check_privilege: gp_fault\n");
    hvm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

static int decode_vmx_inst(struct cpu_user_regs *regs,
                           struct vmx_inst_decoded *decode,
                           unsigned long *poperandS, int vmxon_check)
{
    struct vcpu *v = current;
    union vmx_inst_info info;
    struct segment_register seg;
    unsigned long base, index, seg_base, disp, offset;
    int scale, size;

    if ( vmx_inst_check_privilege(regs, vmxon_check) != X86EMUL_OKAY )
        return X86EMUL_EXCEPTION;

    info.word = __vmread(VMX_INSTRUCTION_INFO);

    if ( info.fields.memreg ) {
        decode->type = VMX_INST_MEMREG_TYPE_REG;
        decode->reg1 = info.fields.reg1;
        if ( poperandS != NULL )
            *poperandS = reg_read(regs, decode->reg1);
    }
    else
    {
        decode->type = VMX_INST_MEMREG_TYPE_MEMORY;
        if ( info.fields.segment > 5 )
            goto gp_fault;
        hvm_get_segment_register(v, sreg_to_index[info.fields.segment], &seg);
        seg_base = seg.base;

        base = info.fields.base_reg_invalid ? 0 :
            reg_read(regs, info.fields.base_reg);

        index = info.fields.index_reg_invalid ? 0 :
            reg_read(regs, info.fields.index_reg);

        scale = 1 << info.fields.scaling;

        disp = __vmread(EXIT_QUALIFICATION);

        size = 1 << (info.fields.addr_size + 1);

        offset = base + index * scale + disp;
        if ( (offset > seg.limit || offset + size > seg.limit) &&
            (!hvm_long_mode_enabled(v) || info.fields.segment == VMX_SREG_GS) )
            goto gp_fault;

        if ( poperandS != NULL &&
             hvm_copy_from_guest_virt(poperandS, seg_base + offset, size, 0)
                  != HVMCOPY_okay )
            return X86EMUL_EXCEPTION;
        decode->mem = seg_base + offset;
        decode->len = size;
    }

    decode->reg2 = info.fields.reg2;

    return X86EMUL_OKAY;

gp_fault:
    hvm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

static void vmreturn(struct cpu_user_regs *regs, enum vmx_ops_result ops_res)
{
    unsigned long eflags = regs->eflags;
    unsigned long mask = X86_EFLAGS_CF | X86_EFLAGS_PF | X86_EFLAGS_AF |
                         X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF;

    eflags &= ~mask;

    switch ( ops_res ) {
    case VMSUCCEED:
        break;
    case VMFAIL_VALID:
        /* TODO: error number, useful for guest VMM debugging */
        eflags |= X86_EFLAGS_ZF;
        break;
    case VMFAIL_INVALID:
    default:
        eflags |= X86_EFLAGS_CF;
        break;
    }

    regs->eflags = eflags;
}

static void __clear_current_vvmcs(struct vcpu *v)
{
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    
    if ( nvcpu->nv_n2vmcx )
        __vmpclear(virt_to_maddr(nvcpu->nv_n2vmcx));
}

static void __map_io_bitmap(struct vcpu *v, u64 vmcs_reg)
{
    struct nestedvmx *nvmx = &vcpu_2_nvmx(v);
    unsigned long gpa;
    int index;

    index = vmcs_reg == IO_BITMAP_A ? 0 : 1;
    if (nvmx->iobitmap[index])
        hvm_unmap_guest_frame (nvmx->iobitmap[index]);
    gpa = __get_vvmcs(vcpu_nestedhvm(v).nv_vvmcx, vmcs_reg);
    nvmx->iobitmap[index] = hvm_map_guest_frame_ro (gpa >> PAGE_SHIFT);
}

static inline void map_io_bitmap_all(struct vcpu *v)
{
   __map_io_bitmap (v, IO_BITMAP_A);
   __map_io_bitmap (v, IO_BITMAP_B);
}

static void nvmx_purge_vvmcs(struct vcpu *v)
{
    struct nestedvmx *nvmx = &vcpu_2_nvmx(v);
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    int i;

    __clear_current_vvmcs(v);
    if ( nvcpu->nv_vvmcxaddr != VMCX_EADDR )
        hvm_unmap_guest_frame (nvcpu->nv_vvmcx);
    nvcpu->nv_vvmcx == NULL;
    nvcpu->nv_vvmcxaddr = VMCX_EADDR;
    for (i=0; i<2; i++) {
        if ( nvmx->iobitmap[i] ) {
            hvm_unmap_guest_frame (nvmx->iobitmap[i]);
            nvmx->iobitmap[i] = NULL;
        }
    }
}

/*
 * VMX instructions handling
 */

int nvmx_handle_vmxon(struct cpu_user_regs *regs)
{
    struct vcpu *v=current;
    struct nestedvmx *nvmx = &vcpu_2_nvmx(v);
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    struct vmx_inst_decoded decode;
    unsigned long gpa = 0;
    int rc;

    rc = decode_vmx_inst(regs, &decode, &gpa, 1);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( nvmx->vmxon_region_pa )
        gdprintk(XENLOG_WARNING, 
                 "vmxon again: orig %"PRIpaddr" new %lx\n",
                 nvmx->vmxon_region_pa, gpa);

    nvmx->vmxon_region_pa = gpa;

    /*
     * `fork' the host vmcs to shadow_vmcs
     * vmcs_lock is not needed since we are on current
     */
    nvcpu->nv_n1vmcx = v->arch.hvm_vmx.vmcs;
    __vmpclear(virt_to_maddr(v->arch.hvm_vmx.vmcs));
    memcpy(nvcpu->nv_n2vmcx, v->arch.hvm_vmx.vmcs, PAGE_SIZE);
    __vmptrld(virt_to_maddr(v->arch.hvm_vmx.vmcs));
    v->arch.hvm_vmx.launched = 0;
    vmreturn(regs, VMSUCCEED);

    return X86EMUL_OKAY;
}

int nvmx_handle_vmxoff(struct cpu_user_regs *regs)
{
    struct vcpu *v=current;
    struct nestedvmx *nvmx = &vcpu_2_nvmx(v);
    int rc;

    rc = vmx_inst_check_privilege(regs, 0);
    if ( rc != X86EMUL_OKAY )
        return rc;

    nvmx_purge_vvmcs(v);
    nvmx->vmxon_region_pa = 0;

    vmreturn(regs, VMSUCCEED);
    return X86EMUL_OKAY;
}

int nvmx_handle_vmptrld(struct cpu_user_regs *regs)
{
    struct vcpu *v = current;
    struct vmx_inst_decoded decode;
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    unsigned long gpa = 0;
    int rc;

    rc = decode_vmx_inst(regs, &decode, &gpa, 0);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( gpa == vcpu_2_nvmx(v).vmxon_region_pa || gpa & 0xfff )
    {
        vmreturn(regs, VMFAIL_INVALID);
        goto out;
    }

    if ( nvcpu->nv_vvmcxaddr != gpa )
        nvmx_purge_vvmcs(v);

    if ( nvcpu->nv_vvmcxaddr == VMCX_EADDR )
    {
        nvcpu->nv_vvmcx = hvm_map_guest_frame_rw (gpa >> PAGE_SHIFT);
        nvcpu->nv_vvmcxaddr = gpa;
        map_io_bitmap_all (v);
    }

    vmreturn(regs, VMSUCCEED);

out:
    return X86EMUL_OKAY;
}

int nvmx_handle_vmptrst(struct cpu_user_regs *regs)
{
    struct vcpu *v = current;
    struct vmx_inst_decoded decode;
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    unsigned long gpa = 0;
    int rc;

    rc = decode_vmx_inst(regs, &decode, &gpa, 0);
    if ( rc != X86EMUL_OKAY )
        return rc;

    gpa = nvcpu->nv_vvmcxaddr;

    rc = hvm_copy_to_guest_virt(decode.mem, &gpa, decode.len, 0);
    if ( rc != HVMCOPY_okay )
        return X86EMUL_EXCEPTION;

    vmreturn(regs, VMSUCCEED);
    return X86EMUL_OKAY;
}

int nvmx_handle_vmclear(struct cpu_user_regs *regs)
{
    struct vcpu *v = current;
    struct vmx_inst_decoded decode;
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    unsigned long gpa = 0;
    int rc;

    rc = decode_vmx_inst(regs, &decode, &gpa, 0);
    if ( rc != X86EMUL_OKAY )
        return rc;

    if ( gpa & 0xfff )
    {
        vmreturn(regs, VMFAIL_INVALID);
        goto out;
    }

    if ( gpa != nvcpu->nv_vvmcxaddr && nvcpu->nv_vvmcxaddr != VMCX_EADDR )
    {
        gdprintk(XENLOG_WARNING, 
                 "vmclear gpa %lx not the same as current vmcs %"PRIpaddr"\n",
                 gpa, nvcpu->nv_vvmcxaddr);
        vmreturn(regs, VMSUCCEED);
        goto out;
    }
    if ( nvcpu->nv_vvmcxaddr != VMCX_EADDR )
        __set_vvmcs(nvcpu->nv_vvmcx, NVMX_LAUNCH_STATE, 0);
    nvmx_purge_vvmcs(v);

    vmreturn(regs, VMSUCCEED);

out:
    return X86EMUL_OKAY;
}

int nvmx_handle_vmwrite(struct cpu_user_regs *regs)
{
    struct vcpu *v = current;
    struct vmx_inst_decoded decode;
    struct nestedvcpu *nvcpu = &vcpu_nestedhvm(v);
    unsigned long operand; 
    u64 vmcs_encoding;

    if ( decode_vmx_inst(regs, &decode, &operand, 0)
             != X86EMUL_OKAY )
        return X86EMUL_EXCEPTION;

    vmcs_encoding = reg_read(regs, decode.reg2);
    __set_vvmcs(nvcpu->nv_vvmcx, vmcs_encoding, operand);

    if ( vmcs_encoding == IO_BITMAP_A || vmcs_encoding == IO_BITMAP_A_HIGH )
        __map_io_bitmap (v, IO_BITMAP_A);
    else if ( vmcs_encoding == IO_BITMAP_B || 
              vmcs_encoding == IO_BITMAP_B_HIGH )
        __map_io_bitmap (v, IO_BITMAP_B);

    vmreturn(regs, VMSUCCEED);
    return X86EMUL_OKAY;
}

