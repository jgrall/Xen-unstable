/* -*-  Mode:C; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */
/*
 * vmx_virt.c:
 * Copyright (c) 2005, Intel Corporation.
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
 *  Fred yang (fred.yang@intel.com)
 *  Shaofan Li (Susue Li) <susie.li@intel.com>
 *  Xuefei Xu (Anthony Xu) (Anthony.xu@intel.com)
 */



#include <asm/privop.h>
#include <asm/vmx_vcpu.h>
#include <asm/processor.h>
#include <asm/delay.h>	// Debug only
#include <asm/vmmu.h>
#include <asm/vmx_mm_def.h>
#include <asm/smp.h>

#include <asm/virt_event.h>
extern UINT64 privop_trace;

void
ia64_priv_decoder(IA64_SLOT_TYPE slot_type, INST64 inst, UINT64  * cause)
{
    *cause=0;
    switch (slot_type) {
        case M:
        if (inst.generic.major==0){
            if(inst.M28.x3==0){
                if(inst.M44.x4==6){
                    *cause=EVENT_SSM;
                }else if(inst.M44.x4==7){
                    *cause=EVENT_RSM;
                }else if(inst.M30.x4==8&&inst.M30.x2==2){
                    *cause=EVENT_MOV_TO_AR_IMM;
                }
            }
        }
        else if(inst.generic.major==1){
            if(inst.M28.x3==0){
                if(inst.M32.x6==0x2c){
                    *cause=EVENT_MOV_TO_CR;
                }else if(inst.M33.x6==0x24){
                    *cause=EVENT_MOV_FROM_CR;
                }else if(inst.M35.x6==0x2d){
                    *cause=EVENT_MOV_TO_PSR;
                }else if(inst.M36.x6==0x25){
                    *cause=EVENT_MOV_FROM_PSR;
                }else if(inst.M29.x6==0x2A){
                    *cause=EVENT_MOV_TO_AR;
                }else if(inst.M31.x6==0x22){
                    *cause=EVENT_MOV_FROM_AR;
                }else if(inst.M45.x6==0x09){
                    *cause=EVENT_PTC_L;
                }else if(inst.M45.x6==0x0A){
                    *cause=EVENT_PTC_G;
                }else if(inst.M45.x6==0x0B){
                    *cause=EVENT_PTC_GA;
                }else if(inst.M45.x6==0x0C){
                    *cause=EVENT_PTR_D;
                }else if(inst.M45.x6==0x0D){
                    *cause=EVENT_PTR_I;
                }else if(inst.M46.x6==0x1A){
                    *cause=EVENT_THASH;
                }else if(inst.M46.x6==0x1B){
                    *cause=EVENT_TTAG;
                }else if(inst.M46.x6==0x1E){
                    *cause=EVENT_TPA;
                }else if(inst.M46.x6==0x1F){
                    *cause=EVENT_TAK;
                }else if(inst.M47.x6==0x34){
                    *cause=EVENT_PTC_E;
                }else if(inst.M41.x6==0x2E){
                    *cause=EVENT_ITC_D;
                }else if(inst.M41.x6==0x2F){
                    *cause=EVENT_ITC_I;
                }else if(inst.M42.x6==0x00){
                    *cause=EVENT_MOV_TO_RR;
                }else if(inst.M42.x6==0x01){
                    *cause=EVENT_MOV_TO_DBR;
                }else if(inst.M42.x6==0x02){
                    *cause=EVENT_MOV_TO_IBR;
                }else if(inst.M42.x6==0x03){
                    *cause=EVENT_MOV_TO_PKR;
                }else if(inst.M42.x6==0x04){
                    *cause=EVENT_MOV_TO_PMC;
                }else if(inst.M42.x6==0x05){
                    *cause=EVENT_MOV_TO_PMD;
                }else if(inst.M42.x6==0x0E){
                    *cause=EVENT_ITR_D;
                }else if(inst.M42.x6==0x0F){
                    *cause=EVENT_ITR_I;
                }else if(inst.M43.x6==0x10){
                    *cause=EVENT_MOV_FROM_RR;
                }else if(inst.M43.x6==0x11){
                    *cause=EVENT_MOV_FROM_DBR;
                }else if(inst.M43.x6==0x12){
                    *cause=EVENT_MOV_FROM_IBR;
                }else if(inst.M43.x6==0x13){
                    *cause=EVENT_MOV_FROM_PKR;
                }else if(inst.M43.x6==0x14){
                    *cause=EVENT_MOV_FROM_PMC;
/*
                }else if(inst.M43.x6==0x15){
                    *cause=EVENT_MOV_FROM_PMD;
*/
                }else if(inst.M43.x6==0x17){
                    *cause=EVENT_MOV_FROM_CPUID;
                }
            }
        }
        break;
        case B:
        if(inst.generic.major==0){
            if(inst.B8.x6==0x02){
                *cause=EVENT_COVER;
            }else if(inst.B8.x6==0x08){
                *cause=EVENT_RFI;
            }else if(inst.B8.x6==0x0c){
                *cause=EVENT_BSW_0;
            }else if(inst.B8.x6==0x0d){
                *cause=EVENT_BSW_1;
            }
        }
    }
}

IA64FAULT vmx_emul_rsm(VCPU *vcpu, INST64 inst)
{
    UINT64 imm24 = (inst.M44.i<<23)|(inst.M44.i2<<21)|inst.M44.imm;
    return vmx_vcpu_reset_psr_sm(vcpu,imm24);
}

IA64FAULT vmx_emul_ssm(VCPU *vcpu, INST64 inst)
{
    UINT64 imm24 = (inst.M44.i<<23)|(inst.M44.i2<<21)|inst.M44.imm;
    return vmx_vcpu_set_psr_sm(vcpu,imm24);
}

unsigned long last_guest_psr = 0x0;
IA64FAULT vmx_emul_mov_from_psr(VCPU *vcpu, INST64 inst)
{
    UINT64 tgt = inst.M33.r1;
    UINT64 val;
    IA64FAULT fault;

/*
    if ((fault = vmx_vcpu_get_psr(vcpu,&val)) == IA64_NO_FAULT)
        return vmx_vcpu_set_gr(vcpu, tgt, val);
    else return fault;
    */
    val = vmx_vcpu_get_psr(vcpu);
    val = (val & MASK(0, 32)) | (val & MASK(35, 2));
    last_guest_psr = val;
    return vmx_vcpu_set_gr(vcpu, tgt, val, 0);
}

/**
 * @todo Check for reserved bits and return IA64_RSVDREG_FAULT.
 */
IA64FAULT vmx_emul_mov_to_psr(VCPU *vcpu, INST64 inst)
{
    UINT64 val;
    IA64FAULT fault;
    if(vmx_vcpu_get_gr(vcpu, inst.M35.r2, &val) != IA64_NO_FAULT)
	panic(" get_psr nat bit fault\n");

	val = (val & MASK(0, 32)) | (VMX_VPD(vcpu, vpsr) & MASK(32, 32));
#if 0
	if (last_mov_from_psr && (last_guest_psr != (val & MASK(0,32))))
		while(1);
	else
		last_mov_from_psr = 0;
#endif
        return vmx_vcpu_set_psr_l(vcpu,val);
}


/**************************************************************************
Privileged operation emulation routines
**************************************************************************/

IA64FAULT vmx_emul_rfi(VCPU *vcpu, INST64 inst)
{
    IA64_PSR  vpsr;
    REGS *regs;
#ifdef  CHECK_FAULT
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    regs=vcpu_regs(vcpu);
    vpsr.val=regs->cr_ipsr;
    if ( vpsr.is == 1 ) {
        panic ("We do not support IA32 instruction yet");
    }

    return vmx_vcpu_rfi(vcpu);
}

IA64FAULT vmx_emul_bsw0(VCPU *vcpu, INST64 inst)
{
#ifdef  CHECK_FAULT
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
   return vmx_vcpu_bsw0(vcpu);
}

IA64FAULT vmx_emul_bsw1(VCPU *vcpu, INST64 inst)
{
#ifdef  CHECK_FAULT
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    return vmx_vcpu_bsw1(vcpu);
}

IA64FAULT vmx_emul_cover(VCPU *vcpu, INST64 inst)
{
    return vmx_vcpu_cover(vcpu);
}

IA64FAULT vmx_emul_ptc_l(VCPU *vcpu, INST64 inst)
{
    u64 r2,r3;
    ISR isr;
    IA64_PSR  vpsr;

    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
    if(vmx_vcpu_get_gr(vcpu,inst.M45.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M45.r2,&r2)){
#ifdef  VMAL_NO_FAULT_CHECK
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif // VMAL_NO_FAULT_CHECK
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if (unimplemented_gva(vcpu,r3) ) {
        isr.val = set_isr_ei_ni(vcpu);
        isr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif // VMAL_NO_FAULT_CHECK
    return vmx_vcpu_ptc_l(vcpu,r3,bits(r2,2,7));
}

IA64FAULT vmx_emul_ptc_e(VCPU *vcpu, INST64 inst)
{
    u64 r3;
    ISR isr;
    IA64_PSR  vpsr;

    vpsr.val=vmx_vcpu_get_psr(vcpu);
#ifdef  VMAL_NO_FAULT_CHECK
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK
    if(vmx_vcpu_get_gr(vcpu,inst.M47.r3,&r3)){
#ifdef  VMAL_NO_FAULT_CHECK
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif // VMAL_NO_FAULT_CHECK
    }
    return vmx_vcpu_ptc_e(vcpu,r3);
}

IA64FAULT vmx_emul_ptc_g(VCPU *vcpu, INST64 inst)
{
    return vmx_emul_ptc_l(vcpu, inst);
}

IA64FAULT vmx_emul_ptc_ga(VCPU *vcpu, INST64 inst)
{
    return vmx_emul_ptc_l(vcpu, inst);
}

IA64FAULT ptr_fault_check(VCPU *vcpu, INST64 inst, u64 *pr2, u64 *pr3)
{
    ISR isr;
    IA64FAULT	ret1, ret2;

#ifdef  VMAL_NO_FAULT_CHECK
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK
    ret1 = vmx_vcpu_get_gr(vcpu,inst.M45.r3,pr3);
    ret2 = vmx_vcpu_get_gr(vcpu,inst.M45.r2,pr2);
#ifdef  VMAL_NO_FAULT_CHECK
    if ( ret1 != IA64_NO_FAULT || ret2 != IA64_NO_FAULT ) {
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
    }
    if (unimplemented_gva(vcpu,r3) ) {
        isr.val = set_isr_ei_ni(vcpu);
        isr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif // VMAL_NO_FAULT_CHECK
   return IA64_NO_FAULT;
}

IA64FAULT vmx_emul_ptr_d(VCPU *vcpu, INST64 inst)
{
    u64 r2,r3;
    if ( ptr_fault_check(vcpu, inst, &r2, &r3 ) == IA64_FAULT )
    	return IA64_FAULT;
    return vmx_vcpu_ptr_d(vcpu,r3,bits(r2,2,7));
}

IA64FAULT vmx_emul_ptr_i(VCPU *vcpu, INST64 inst)
{
    u64 r2,r3;
    if ( ptr_fault_check(vcpu, inst, &r2, &r3 ) == IA64_FAULT )
    	return IA64_FAULT;
    return vmx_vcpu_ptr_i(vcpu,r3,bits(r2,2,7));
}


IA64FAULT vmx_emul_thash(VCPU *vcpu, INST64 inst)
{
    u64 r1,r3;
    ISR visr;
    IA64_PSR vpsr;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M46.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#endif //CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu, inst.M46.r3, &r3)){
#ifdef  CHECK_FAULT
        vmx_vcpu_set_gr(vcpu, inst.M46.r1, 0, 1);
        return IA64_NO_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(unimplemented_gva(vcpu, r3)){
        vmx_vcpu_set_gr(vcpu, inst.M46.r1, 0, 1);
        return IA64_NO_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_thash(vcpu, r3, &r1);
    vmx_vcpu_set_gr(vcpu, inst.M46.r1, r1, 0);
    return(IA64_NO_FAULT);
}


IA64FAULT vmx_emul_ttag(VCPU *vcpu, INST64 inst)
{
    u64 r1,r3;
    ISR visr;
    IA64_PSR vpsr;
 #ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M46.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#endif //CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu, inst.M46.r3, &r3)){
#ifdef  CHECK_FAULT
        vmx_vcpu_set_gr(vcpu, inst.M46.r1, 0, 1);
        return IA64_NO_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(unimplemented_gva(vcpu, r3)){
        vmx_vcpu_set_gr(vcpu, inst.M46.r1, 0, 1);
        return IA64_NO_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_ttag(vcpu, r3, &r1);
    vmx_vcpu_set_gr(vcpu, inst.M46.r1, r1, 0);
    return(IA64_NO_FAULT);
}


IA64FAULT vmx_emul_tpa(VCPU *vcpu, INST64 inst)
{
    u64 r1,r3;
    ISR visr;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M46.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if(vpsr.cpl!=0){
        visr.val=0;
        vcpu_set_isr(vcpu, visr.val);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu, inst.M46.r3, &r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,1);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if (unimplemented_gva(vcpu,r3) ) {
        // inject unimplemented_data_address_fault
        visr.val = set_isr_ei_ni(vcpu);
        visr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        // FAULT_UNIMPLEMENTED_DATA_ADDRESS.
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif  //CHECK_FAULT

    if(vmx_vcpu_tpa(vcpu, r3, &r1)){
        return IA64_FAULT;
    }
    vmx_vcpu_set_gr(vcpu, inst.M46.r1, r1, 0);
    return(IA64_NO_FAULT);
}

IA64FAULT vmx_emul_tak(VCPU *vcpu, INST64 inst)
{
    u64 r1,r3;
    ISR visr;
    IA64_PSR vpsr;
    int fault=IA64_NO_FAULT;
#ifdef  CHECK_FAULT
    visr.val=0;
    if(check_target_register(vcpu, inst.M46.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if(vpsr.cpl!=0){
        vcpu_set_isr(vcpu, visr.val);
        return IA64_FAULT;
    }
#endif
    if(vmx_vcpu_get_gr(vcpu, inst.M46.r3, &r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,1);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif
    }
    if(vmx_vcpu_tak(vcpu, r3, &r1)){
        return IA64_FAULT;
    }
    vmx_vcpu_set_gr(vcpu, inst.M46.r1, r1, 0);
    return(IA64_NO_FAULT);
}


/************************************
 * Insert translation register/cache
************************************/

IA64FAULT vmx_emul_itr_d(VCPU *vcpu, INST64 inst)
{
    UINT64 fault, itir, ifa, pte, slot;
    ISR isr;
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.ic ) {
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK
    if(vmx_vcpu_get_gr(vcpu,inst.M45.r3,&slot)||vmx_vcpu_get_gr(vcpu,inst.M45.r2,&pte)){
#ifdef  VMAL_NO_FAULT_CHECK
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif // VMAL_NO_FAULT_CHECK
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if(is_reserved_rr_register(vcpu, slot)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK

    if (vmx_vcpu_get_itir(vcpu,&itir)){
        return(IA64_FAULT);
    }
    if (vmx_vcpu_get_ifa(vcpu,&ifa)){
        return(IA64_FAULT);
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if (is_reserved_itir_field(vcpu, itir)) {
    	// TODO
    	return IA64_FAULT;
    }
    if (unimplemented_gva(vcpu,ifa) ) {
        isr.val = set_isr_ei_ni(vcpu);
        isr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif // VMAL_NO_FAULT_CHECK

    return (vmx_vcpu_itr_d(vcpu,pte,itir,ifa,slot));
}

IA64FAULT vmx_emul_itr_i(VCPU *vcpu, INST64 inst)
{
    UINT64 fault, itir, ifa, pte, slot;
    ISR isr;
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.ic ) {
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK
    if(vmx_vcpu_get_gr(vcpu,inst.M45.r3,&slot)||vmx_vcpu_get_gr(vcpu,inst.M45.r2,&pte)){
#ifdef  VMAL_NO_FAULT_CHECK
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif // VMAL_NO_FAULT_CHECK
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if(is_reserved_rr_register(vcpu, slot)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK

    if (vmx_vcpu_get_itir(vcpu,&itir)){
        return(IA64_FAULT);
    }
    if (vmx_vcpu_get_ifa(vcpu,&ifa)){
        return(IA64_FAULT);
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if (is_reserved_itir_field(vcpu, itir)) {
    	// TODO
    	return IA64_FAULT;
    }
    if (unimplemented_gva(vcpu,ifa) ) {
        isr.val = set_isr_ei_ni(vcpu);
        isr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif // VMAL_NO_FAULT_CHECK

   return (vmx_vcpu_itr_i(vcpu,pte,itir,ifa,slot));
}

IA64FAULT itc_fault_check(VCPU *vcpu, INST64 inst, u64 *itir, u64 *ifa,u64 *pte)
{
    UINT64 fault;
    ISR isr;
    IA64_PSR  vpsr;
    IA64FAULT	ret1;

    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.ic ) {
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }

#ifdef  VMAL_NO_FAULT_CHECK
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK
    ret1 = vmx_vcpu_get_gr(vcpu,inst.M45.r2,pte);
#ifdef  VMAL_NO_FAULT_CHECK
    if( ret1 != IA64_NO_FAULT ){
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
    }
#endif // VMAL_NO_FAULT_CHECK

    if (vmx_vcpu_get_itir(vcpu,itir)){
        return(IA64_FAULT);
    }
    if (vmx_vcpu_get_ifa(vcpu,ifa)){
        return(IA64_FAULT);
    }
#ifdef  VMAL_NO_FAULT_CHECK
    if (unimplemented_gva(vcpu,ifa) ) {
        isr.val = set_isr_ei_ni(vcpu);
        isr.code = IA64_RESERVED_REG_FAULT;
        vcpu_set_isr(vcpu, isr.val);
        unimpl_daddr(vcpu);
        return IA64_FAULT;
   }
#endif // VMAL_NO_FAULT_CHECK
   return IA64_NO_FAULT;
}

IA64FAULT vmx_emul_itc_d(VCPU *vcpu, INST64 inst)
{
    UINT64 itir, ifa, pte;

    if ( itc_fault_check(vcpu, inst, &itir, &ifa, &pte) == IA64_FAULT ) {
    	return IA64_FAULT;
    }

   return (vmx_vcpu_itc_d(vcpu,pte,itir,ifa));
}

IA64FAULT vmx_emul_itc_i(VCPU *vcpu, INST64 inst)
{
    UINT64 itir, ifa, pte;

    if ( itc_fault_check(vcpu, inst, &itir, &ifa, &pte) == IA64_FAULT ) {
    	return IA64_FAULT;
    }

   return (vmx_vcpu_itc_i(vcpu,pte,itir,ifa));

}

/*************************************
 * Moves to semi-privileged registers
*************************************/

IA64FAULT vmx_emul_mov_to_ar_imm(VCPU *vcpu, INST64 inst)
{
    // I27 and M30 are identical for these fields
    if(inst.M30.ar3!=44){
        panic("Can't support ar register other than itc");
    }
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    UINT64  imm;
    if(inst.M30.s){
        imm = -inst.M30.imm;
    }else{
        imm = inst.M30.imm;
    }
    return (vmx_vcpu_set_itc(vcpu, imm));
}

IA64FAULT vmx_emul_mov_to_ar_reg(VCPU *vcpu, INST64 inst)
{
    // I26 and M29 are identical for these fields
    u64 r2;
    if(inst.M29.ar3!=44){
        panic("Can't support ar register other than itc");
    }
    if(vmx_vcpu_get_gr(vcpu,inst.M29.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    return (vmx_vcpu_set_itc(vcpu, r2));
}


IA64FAULT vmx_emul_mov_from_ar_reg(VCPU *vcpu, INST64 inst)
{
    // I27 and M30 are identical for these fields
    if(inst.M31.ar3!=44){
        panic("Can't support ar register other than itc");
    }
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu,inst.M31.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.si&& vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    u64 r1;
    vmx_vcpu_get_itc(vcpu,&r1);
    vmx_vcpu_set_gr(vcpu,inst.M31.r1,r1,0);
    return IA64_NO_FAULT;
}


/********************************
 * Moves to privileged registers
********************************/

IA64FAULT vmx_emul_mov_to_pkr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_pkr(vcpu,r3,r2));
}

IA64FAULT vmx_emul_mov_to_rr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_rr(vcpu,r3,r2));
}

IA64FAULT vmx_emul_mov_to_dbr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_dbr(vcpu,r3,r2));
}

IA64FAULT vmx_emul_mov_to_ibr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_ibr(vcpu,r3,r2));
}

IA64FAULT vmx_emul_mov_to_pmc(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_pmc(vcpu,r3,r2));
}

IA64FAULT vmx_emul_mov_to_pmd(VCPU *vcpu, INST64 inst)
{
    u64 r3,r2;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu,inst.M42.r3,&r3)||vmx_vcpu_get_gr(vcpu,inst.M42.r2,&r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
    return (vmx_vcpu_set_pmd(vcpu,r3,r2));
}


/**********************************
 * Moves from privileged registers
 **********************************/

IA64FAULT vmx_emul_mov_from_rr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }

#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_rr_register(vcpu,r3>>VRN_SHIFT)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_rr(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_from_pkr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }

#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_indirect_register(vcpu,r3)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_pkr(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_from_dbr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }

#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_indirect_register(vcpu,r3)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_dbr(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_from_ibr(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }

#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_indirect_register(vcpu,r3)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_ibr(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_from_pmc(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if (vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }

#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_indirect_register(vcpu,r3)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_pmc(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_from_cpuid(VCPU *vcpu, INST64 inst)
{
    u64 r3,r1;
#ifdef  CHECK_FAULT
    if(check_target_register(vcpu, inst.M43.r1)){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
#endif //CHECK_FAULT
     if(vmx_vcpu_get_gr(vcpu,inst.M43.r3,&r3)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef  CHECK_FAULT
    if(is_reserved_indirect_register(vcpu,r3)){
        set_rsv_reg_field_isr(vcpu);
        rsv_reg_field(vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    vmx_vcpu_get_cpuid(vcpu,r3,&r1);
    return vmx_vcpu_set_gr(vcpu, inst.M43.r1, r1,0);
}

IA64FAULT vmx_emul_mov_to_cr(VCPU *vcpu, INST64 inst)
{
    u64 r2,cr3;
#ifdef  CHECK_FAULT
    IA64_PSR  vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if(is_reserved_cr(inst.M32.cr3)||(vpsr.ic&&is_interruption_control_cr(inst.M32.cr3))){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT
    if(vmx_vcpu_get_gr(vcpu, inst.M32.r2, &r2)){
#ifdef  CHECK_FAULT
        set_isr_reg_nat_consumption(vcpu,0,0);
        rnat_comsumption(vcpu);
        return IA64_FAULT;
#endif  //CHECK_FAULT
    }
#ifdef   CHECK_FAULT
    if ( check_cr_rsv_fields (inst.M32.cr3, r2)) {
        /* Inject Reserved Register/Field fault
         * into guest */
        set_rsv_reg_field_isr (vcpu,0);
        rsv_reg_field (vcpu);
        return IA64_FAULT;
    }
#endif  //CHECK_FAULT
    extern u64 cr_igfld_mask(int index, u64 value);
    r2 = cr_igfld_mask(inst.M32.cr3,r2);
    VMX_VPD(vcpu, vcr[inst.M32.cr3]) = r2;
    switch (inst.M32.cr3) {
        case 0: return vmx_vcpu_set_dcr(vcpu,r2);
        case 1: return vmx_vcpu_set_itm(vcpu,r2);
        case 2: return vmx_vcpu_set_iva(vcpu,r2);
        case 8: return vmx_vcpu_set_pta(vcpu,r2);
        case 16:return vmx_vcpu_set_ipsr(vcpu,r2);
        case 17:return vmx_vcpu_set_isr(vcpu,r2);
        case 19:return vmx_vcpu_set_iip(vcpu,r2);
        case 20:return vmx_vcpu_set_ifa(vcpu,r2);
        case 21:return vmx_vcpu_set_itir(vcpu,r2);
        case 22:return vmx_vcpu_set_iipa(vcpu,r2);
        case 23:return vmx_vcpu_set_ifs(vcpu,r2);
        case 24:return vmx_vcpu_set_iim(vcpu,r2);
        case 25:return vmx_vcpu_set_iha(vcpu,r2);
        case 64:return vmx_vcpu_set_lid(vcpu,r2);
        case 65:return IA64_NO_FAULT;
        case 66:return vmx_vcpu_set_tpr(vcpu,r2);
        case 67:return vmx_vcpu_set_eoi(vcpu,r2);
        case 68:return IA64_NO_FAULT;
        case 69:return IA64_NO_FAULT;
        case 70:return IA64_NO_FAULT;
        case 71:return IA64_NO_FAULT;
        case 72:return vmx_vcpu_set_itv(vcpu,r2);
        case 73:return vmx_vcpu_set_pmv(vcpu,r2);
        case 74:return vmx_vcpu_set_cmcv(vcpu,r2);
        case 80:return vmx_vcpu_set_lrr0(vcpu,r2);
        case 81:return vmx_vcpu_set_lrr1(vcpu,r2);
        default: return IA64_NO_FAULT;
    }
}


#define cr_get(cr) \
    ((fault=vmx_vcpu_get_##cr(vcpu,&val))==IA64_NO_FAULT)?\
        vmx_vcpu_set_gr(vcpu, tgt, val,0):fault;


IA64FAULT vmx_emul_mov_from_cr(VCPU *vcpu, INST64 inst)
{
    UINT64 tgt = inst.M33.r1;
    UINT64 val;
    IA64FAULT fault;
#ifdef  CHECK_FAULT
    IA64_PSR vpsr;
    vpsr.val=vmx_vcpu_get_psr(vcpu);
    if(is_reserved_cr(inst.M33.cr3)||is_read_only_cr(inst.M33.cr3||
        (vpsr.ic&&is_interruption_control_cr(inst.M33.cr3)))){
        set_illegal_op_isr(vcpu);
        illegal_op(vcpu);
        return IA64_FAULT;
    }
    if ( vpsr.cpl != 0) {
        /* Inject Privileged Operation fault into guest */
        set_privileged_operation_isr (vcpu, 0);
        privilege_op (vcpu);
        return IA64_FAULT;
    }
#endif // CHECK_FAULT

//    from_cr_cnt[inst.M33.cr3]++;
    switch (inst.M33.cr3) {
        case 0: return cr_get(dcr);
        case 1: return cr_get(itm);
        case 2: return cr_get(iva);
        case 8: return cr_get(pta);
        case 16:return cr_get(ipsr);
        case 17:return cr_get(isr);
        case 19:return cr_get(iip);
        case 20:return cr_get(ifa);
        case 21:return cr_get(itir);
        case 22:return cr_get(iipa);
        case 23:return cr_get(ifs);
        case 24:return cr_get(iim);
        case 25:return cr_get(iha);
	case 64:val = ia64_getreg(_IA64_REG_CR_LID);
	     return vmx_vcpu_set_gr(vcpu,tgt,val,0);
//        case 64:return cr_get(lid);
        case 65:
             vmx_vcpu_get_ivr(vcpu,&val);
             return vmx_vcpu_set_gr(vcpu,tgt,val,0);
        case 66:return cr_get(tpr);
        case 67:return vmx_vcpu_set_gr(vcpu,tgt,0L,0);
        case 68:return cr_get(irr0);
        case 69:return cr_get(irr1);
        case 70:return cr_get(irr2);
        case 71:return cr_get(irr3);
        case 72:return cr_get(itv);
        case 73:return cr_get(pmv);
        case 74:return cr_get(cmcv);
        case 80:return cr_get(lrr0);
        case 81:return cr_get(lrr1);
        default:
            panic("Read reserved cr register");
    }
}


static void post_emulation_action(VCPU *vcpu)
{
    if ( vcpu->arch.irq_new_condition ) {
        vcpu->arch.irq_new_condition = 0;
        vhpi_detection(vcpu);
    }
}

//#define  BYPASS_VMAL_OPCODE
extern IA64_SLOT_TYPE  slot_types[0x20][3];
IA64_BUNDLE __vmx_get_domain_bundle(u64 iip)
{
	IA64_BUNDLE bundle;

	fetch_code( current,iip, &bundle.i64[0]);
	fetch_code( current,iip+8, &bundle.i64[1]);
	return bundle;
}

/** Emulate a privileged operation.
 *
 *
 * @param vcpu virtual cpu
 * @cause the reason cause virtualization fault
 * @opcode the instruction code which cause virtualization fault
 */

void
vmx_emulate(VCPU *vcpu, UINT64 cause, UINT64 opcode)
{
    IA64_BUNDLE bundle;
    int slot;
    IA64_SLOT_TYPE slot_type;
    IA64FAULT status;
    INST64 inst;
    REGS * regs;
    UINT64 iip;
    regs = vcpu_regs(vcpu);
    iip = regs->cr_iip;
    IA64_PSR vpsr;
/*
    if (privop_trace) {
        static long i = 400;
        //if (i > 0) printf("privop @%p\n",iip);
        if (i > 0) printf("priv_handle_op: @%p, itc=%lx, itm=%lx\n",
            iip,ia64_get_itc(),ia64_get_itm());
        i--;
    }
*/
#ifdef  VTLB_DEBUG
    check_vtlb_sanity(vmx_vcpu_get_vtlb(vcpu));
    dump_vtlb(vmx_vcpu_get_vtlb(vcpu));
#endif
#if 0
if ( (cause == 0xff && opcode == 0x1e000000000) || cause == 0 ) {
		printf ("VMAL decode error: cause - %lx; op - %lx\n", 
			cause, opcode );
		return;
}
#endif
#ifdef BYPASS_VMAL_OPCODE
    // make a local copy of the bundle containing the privop
    bundle = __vmx_get_domain_bundle(iip);
    slot = ((struct ia64_psr *)&(regs->cr_ipsr))->ri;
    if (!slot) inst.inst = bundle.slot0;
    else if (slot == 1)
        inst.inst = bundle.slot1a + (bundle.slot1b<<18);
    else if (slot == 2) inst.inst = bundle.slot2;
    else printf("priv_handle_op: illegal slot: %d\n", slot);
    slot_type = slot_types[bundle.template][slot];
    ia64_priv_decoder(slot_type, inst, &cause);
    if(cause==0){
        printf("This instruction at 0x%lx slot %d can't be  virtualized", iip, slot);
        panic("123456\n");
    }
#else
    inst.inst=opcode;
#endif /* BYPASS_VMAL_OPCODE */

    /*
     * Switch to actual virtual rid in rr0 and rr4,
     * which is required by some tlb related instructions.
     */
    prepare_if_physical_mode(vcpu);

    switch(cause) {
    case EVENT_RSM:
        status=vmx_emul_rsm(vcpu, inst);
        break;
    case EVENT_SSM:
        status=vmx_emul_ssm(vcpu, inst);
        break;
    case EVENT_MOV_TO_PSR:
        status=vmx_emul_mov_to_psr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_PSR:
        status=vmx_emul_mov_from_psr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_CR:
        status=vmx_emul_mov_from_cr(vcpu, inst);
        break;
    case EVENT_MOV_TO_CR:
        status=vmx_emul_mov_to_cr(vcpu, inst);
        break;
    case EVENT_BSW_0:
        status=vmx_emul_bsw0(vcpu, inst);
        break;
    case EVENT_BSW_1:
        status=vmx_emul_bsw1(vcpu, inst);
        break;
    case EVENT_COVER:
        status=vmx_emul_cover(vcpu, inst);
        break;
    case EVENT_RFI:
        status=vmx_emul_rfi(vcpu, inst);
        break;
    case EVENT_ITR_D:
        status=vmx_emul_itr_d(vcpu, inst);
        break;
    case EVENT_ITR_I:
        status=vmx_emul_itr_i(vcpu, inst);
        break;
    case EVENT_PTR_D:
        status=vmx_emul_ptr_d(vcpu, inst);
        break;
    case EVENT_PTR_I:
        status=vmx_emul_ptr_i(vcpu, inst);
        break;
    case EVENT_ITC_D:
        status=vmx_emul_itc_d(vcpu, inst);
        break;
    case EVENT_ITC_I:
        status=vmx_emul_itc_i(vcpu, inst);
        break;
    case EVENT_PTC_L:
        status=vmx_emul_ptc_l(vcpu, inst);
        break;
    case EVENT_PTC_G:
        status=vmx_emul_ptc_g(vcpu, inst);
        break;
    case EVENT_PTC_GA:
        status=vmx_emul_ptc_ga(vcpu, inst);
        break;
    case EVENT_PTC_E:
        status=vmx_emul_ptc_e(vcpu, inst);
        break;
    case EVENT_MOV_TO_RR:
        status=vmx_emul_mov_to_rr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_RR:
        status=vmx_emul_mov_from_rr(vcpu, inst);
        break;
    case EVENT_THASH:
        status=vmx_emul_thash(vcpu, inst);
        break;
    case EVENT_TTAG:
        status=vmx_emul_ttag(vcpu, inst);
        break;
    case EVENT_TPA:
        status=vmx_emul_tpa(vcpu, inst);
        break;
    case EVENT_TAK:
        status=vmx_emul_tak(vcpu, inst);
        break;
    case EVENT_MOV_TO_AR_IMM:
        status=vmx_emul_mov_to_ar_imm(vcpu, inst);
        break;
    case EVENT_MOV_TO_AR:
        status=vmx_emul_mov_to_ar_reg(vcpu, inst);
        break;
    case EVENT_MOV_FROM_AR:
        status=vmx_emul_mov_from_ar_reg(vcpu, inst);
        break;
    case EVENT_MOV_TO_DBR:
        status=vmx_emul_mov_to_dbr(vcpu, inst);
        break;
    case EVENT_MOV_TO_IBR:
        status=vmx_emul_mov_to_ibr(vcpu, inst);
        break;
    case EVENT_MOV_TO_PMC:
        status=vmx_emul_mov_to_pmc(vcpu, inst);
        break;
    case EVENT_MOV_TO_PMD:
        status=vmx_emul_mov_to_pmd(vcpu, inst);
        break;
    case EVENT_MOV_TO_PKR:
        status=vmx_emul_mov_to_pkr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_DBR:
        status=vmx_emul_mov_from_dbr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_IBR:
        status=vmx_emul_mov_from_ibr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_PMC:
        status=vmx_emul_mov_from_pmc(vcpu, inst);
        break;
    case EVENT_MOV_FROM_PKR:
        status=vmx_emul_mov_from_pkr(vcpu, inst);
        break;
    case EVENT_MOV_FROM_CPUID:
        status=vmx_emul_mov_from_cpuid(vcpu, inst);
        break;
    case EVENT_VMSW:
        printf ("Unimplemented instruction %d\n", cause);
	status=IA64_FAULT;
        break;
    default:
        printf("unknown cause %d, iip: %lx, ipsr: %lx\n", cause,regs->cr_iip,regs->cr_ipsr);
        while(1);
	/* For unknown cause, let hardware to re-execute */
	status=IA64_RETRY;
        break;
//        panic("unknown cause in virtualization intercept");
    };

#if 0
    if (status == IA64_FAULT)
	panic("Emulation failed with cause %d:\n", cause);
#endif

    if ( status == IA64_NO_FAULT && cause !=EVENT_RFI ) {
        vmx_vcpu_increment_iip(vcpu);
    }

    recover_if_physical_mode(vcpu);
    post_emulation_action (vcpu);
//TODO    set_irq_check(v);
    return;

}

