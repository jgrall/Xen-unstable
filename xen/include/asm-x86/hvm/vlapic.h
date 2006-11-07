/*
 * hvm_vlapic.h: virtualize LAPIC definitions.
 *
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
 */

#ifndef __ASM_X86_HVM_VLAPIC_H__
#define __ASM_X86_HVM_VLAPIC_H__

#include <asm/msr.h>
#include <public/hvm/ioreq.h>

#define MAX_VECTOR      256

#define vcpu_vlapic(vcpu)   (&(vcpu)->arch.hvm_vcpu.vlapic)
#define vlapic_vcpu(vpic)   (container_of((vpic), struct vcpu, \
                                          arch.hvm_vcpu.vlapic))
#define vlapic_domain(vpic) (vlapic_vcpu(vlapic)->domain)

#define VLAPIC_ID(vlapic)   \
    (GET_APIC_ID(vlapic_get_reg(vlapic, APIC_ID)))

#define _VLAPIC_GLOB_DISABLE            0x0
#define VLAPIC_GLOB_DISABLE_MASK        0x1
#define VLAPIC_SOFTWARE_DISABLE_MASK    0x2
#define _VLAPIC_BSP_ACCEPT_PIC          0x3

#define vlapic_enabled(vlapic)              \
    (!((vlapic)->status &                   \
       (VLAPIC_GLOB_DISABLE_MASK | VLAPIC_SOFTWARE_DISABLE_MASK)))

#define vlapic_global_enabled(vlapic)       \
    (!(test_bit(_VLAPIC_GLOB_DISABLE, &(vlapic)->status)))

struct vlapic {
    uint32_t           status;
    uint64_t           apic_base_msr;
    unsigned long      base_address;
    uint32_t           timer_divide_count;
    struct timer       vlapic_timer;
    int                timer_pending_count;
    int                flush_tpr_threshold;
    s_time_t           timer_last_update;
    struct page_info   *regs_page;
    void               *regs;
};

static inline uint32_t vlapic_get_reg(struct vlapic *vlapic, uint32_t reg)
{
    return *((uint32_t *)(vlapic->regs + reg));
}

static inline void vlapic_set_reg(
    struct vlapic *vlapic, uint32_t reg, uint32_t val)
{
    *((uint32_t *)(vlapic->regs + reg)) = val;
}


int vlapic_set_irq(struct vlapic *vlapic, uint8_t vec, uint8_t trig);

void vlapic_post_injection(struct vcpu *v, int vector, int deliver_mode);

int vlapic_find_highest_irr(struct vlapic *vlapic);

int cpu_get_apic_interrupt(struct vcpu *v, int *mode);

int  vlapic_init(struct vcpu *v);
void vlapic_destroy(struct vcpu *v);

void vlapic_msr_set(struct vlapic *vlapic, uint64_t value);

int vlapic_accept_pic_intr(struct vcpu *v);

struct vlapic *apic_round_robin(struct domain *d,
                                uint8_t dest_mode,
                                uint8_t vector,
                                uint32_t bitmap);

s_time_t get_apictime_scheduled(struct vcpu *v);

#endif /* __ASM_X86_HVM_VLAPIC_H__ */
