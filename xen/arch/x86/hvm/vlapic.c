/*
 * vlapic.c: virtualize LAPIC for HVM vcpus.
 *
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2006 Keir Fraser, XenSource Inc.
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

#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <xen/xmalloc.h>
#include <xen/domain.h>
#include <xen/domain_page.h>
#include <xen/event.h>
#include <xen/trace.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/numa.h>
#include <asm/current.h>
#include <asm/page.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/vmx/vmx.h>
#include <public/hvm/ioreq.h>
#include <public/hvm/params.h>

#define VLAPIC_VERSION                  0x00050014
#define VLAPIC_LVT_NUM                  6

/* vlapic's frequence is 100 MHz */
#define APIC_BUS_CYCLE_NS               10

#define LVT_MASK \
    APIC_LVT_MASKED | APIC_SEND_PENDING | APIC_VECTOR_MASK

#define LINT_MASK   \
    LVT_MASK | APIC_MODE_MASK | APIC_INPUT_POLARITY |\
    APIC_LVT_REMOTE_IRR | APIC_LVT_LEVEL_TRIGGER

static unsigned int vlapic_lvt_mask[VLAPIC_LVT_NUM] =
{
     /* LVTT */
     LVT_MASK | APIC_LVT_TIMER_PERIODIC,
     /* LVTTHMR */
     LVT_MASK | APIC_MODE_MASK,
     /* LVTPC */
     LVT_MASK | APIC_MODE_MASK,
     /* LVT0-1 */
     LINT_MASK, LINT_MASK,
     /* LVTERR */
     LVT_MASK
};

/* Following could belong in apicdef.h */
#define APIC_SHORT_MASK                  0xc0000
#define APIC_DEST_NOSHORT                0x0
#define APIC_DEST_MASK                   0x800

#define vlapic_lvt_vector(vlapic, lvt_type)                     \
    (vlapic_get_reg(vlapic, lvt_type) & APIC_VECTOR_MASK)

#define vlapic_lvt_dm(vlapic, lvt_type)                         \
    (vlapic_get_reg(vlapic, lvt_type) & APIC_MODE_MASK)

#define vlapic_lvtt_period(vlapic)                              \
    (vlapic_get_reg(vlapic, APIC_LVTT) & APIC_LVT_TIMER_PERIODIC)


/*
 * Generic APIC bitmap vector update & search routines.
 */

#define VEC_POS(v) ((v)%32)
#define REG_POS(v) (((v)/32) * 0x10)
#define vlapic_test_and_set_vector(vec, bitmap)                         \
    test_and_set_bit(VEC_POS(vec),                                      \
                     (unsigned long *)((bitmap) + REG_POS(vec)))
#define vlapic_test_and_clear_vector(vec, bitmap)                       \
    test_and_clear_bit(VEC_POS(vec),                                    \
                       (unsigned long *)((bitmap) + REG_POS(vec)))
#define vlapic_set_vector(vec, bitmap)                                  \
    set_bit(VEC_POS(vec), (unsigned long *)((bitmap) + REG_POS(vec)))
#define vlapic_clear_vector(vec, bitmap)                                \
    clear_bit(VEC_POS(vec), (unsigned long *)((bitmap) + REG_POS(vec)))

static int vlapic_find_highest_vector(void *bitmap)
{
    uint32_t *word = bitmap;
    int word_offset = MAX_VECTOR / 32;

    /* Work backwards through the bitmap (first 32-bit word in every four). */
    while ( (word_offset != 0) && (word[(--word_offset)*4] == 0) )
        continue;

    return (fls(word[word_offset*4]) - 1) + (word_offset * 32);
}


/*
 * IRR-specific bitmap update & search routines.
 */

static int vlapic_test_and_set_irr(int vector, struct vlapic *vlapic)
{
    return vlapic_test_and_set_vector(vector, &vlapic->regs->data[APIC_IRR]);
}

static void vlapic_clear_irr(int vector, struct vlapic *vlapic)
{
    vlapic_clear_vector(vector, &vlapic->regs->data[APIC_IRR]);
}

static int vlapic_find_highest_irr(struct vlapic *vlapic)
{
    return vlapic_find_highest_vector(&vlapic->regs->data[APIC_IRR]);
}

int vlapic_set_irq(struct vlapic *vlapic, uint8_t vec, uint8_t trig)
{
    int ret;

    ret = !vlapic_test_and_set_irr(vec, vlapic);
    if ( trig )
        vlapic_set_vector(vec, &vlapic->regs->data[APIC_TMR]);

    /* We may need to wake up target vcpu, besides set pending bit here */
    return ret;
}

static int vlapic_find_highest_isr(struct vlapic *vlapic)
{
    return vlapic_find_highest_vector(&vlapic->regs->data[APIC_ISR]);
}

uint32_t vlapic_get_ppr(struct vlapic *vlapic)
{
    uint32_t tpr, isrv, ppr;
    int isr;

    tpr  = vlapic_get_reg(vlapic, APIC_TASKPRI);
    isr  = vlapic_find_highest_isr(vlapic);
    isrv = (isr != -1) ? isr : 0;

    if ( (tpr & 0xf0) >= (isrv & 0xf0) )
        ppr = tpr & 0xff;
    else
        ppr = isrv & 0xf0;

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_INTERRUPT,
                "vlapic %p, ppr 0x%x, isr 0x%x, isrv 0x%x",
                vlapic, ppr, isr, isrv);

    return ppr;
}

int vlapic_match_logical_addr(struct vlapic *vlapic, uint8_t mda)
{
    int result = 0;
    uint8_t logical_id;

    logical_id = GET_xAPIC_LOGICAL_ID(vlapic_get_reg(vlapic, APIC_LDR));

    switch ( vlapic_get_reg(vlapic, APIC_DFR) )
    {
    case APIC_DFR_FLAT:
        if ( logical_id & mda )
            result = 1;
        break;
    case APIC_DFR_CLUSTER:
        if ( ((logical_id >> 4) == (mda >> 0x4)) && (logical_id & mda & 0xf) )
            result = 1;
        break;
    default:
        gdprintk(XENLOG_WARNING, "Bad DFR value for lapic of vcpu %d: %08x\n",
                 vlapic_vcpu(vlapic)->vcpu_id,
                 vlapic_get_reg(vlapic, APIC_DFR));
        break;
    }

    return result;
}

static int vlapic_match_dest(struct vcpu *v, struct vlapic *source,
                             int short_hand, int dest, int dest_mode)
{
    int result = 0;
    struct vlapic *target = vcpu_vlapic(v);

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "target %p, source %p, dest 0x%x, "
                "dest_mode 0x%x, short_hand 0x%x",
                target, source, dest, dest_mode, short_hand);

    switch ( short_hand )
    {
    case APIC_DEST_NOSHORT:
        if ( dest_mode == 0 )
        {
            /* Physical mode. */
            if ( (dest == 0xFF) || (dest == VLAPIC_ID(target)) )
                result = 1;
        }
        else
        {
            /* Logical mode. */
            result = vlapic_match_logical_addr(target, dest);
        }
        break;

    case APIC_DEST_SELF:
        if ( target == source )
            result = 1;
        break;

    case APIC_DEST_ALLINC:
        result = 1;
        break;

    case APIC_DEST_ALLBUT:
        if ( target != source )
            result = 1;
        break;

    default:
        gdprintk(XENLOG_WARNING, "Bad dest shorthand value %x\n", short_hand);
        break;
    }

    return result;
}

static int vlapic_vcpu_pause_async(struct vcpu *v)
{
    vcpu_pause_nosync(v);

    if ( v->is_running )
    {
        vcpu_unpause(v);
        return 0;
    }

    sync_vcpu_execstate(v);
    return 1;
}

static void vlapic_init_action(unsigned long _vcpu)
{
    struct vcpu *v = (struct vcpu *)_vcpu;
    struct domain *d = v->domain;
    bool_t fpu_initialised;

    /* If the VCPU is not on its way down we have nothing to do. */
    if ( !test_bit(_VPF_down, &v->pause_flags) )
        return;

    if ( !vlapic_vcpu_pause_async(v) )
    {
        tasklet_schedule(&vcpu_vlapic(v)->init_tasklet);
        return;
    }

    /* Reset necessary VCPU state. This does not include FPU state. */
    domain_lock(d);
    fpu_initialised = v->fpu_initialised;
    vcpu_reset(v);
    v->fpu_initialised = fpu_initialised;
    vlapic_reset(vcpu_vlapic(v));
    domain_unlock(d);

    vcpu_unpause(v);
}

static int vlapic_accept_init(struct vcpu *v)
{
    /* Nothing to do if the VCPU is already reset. */
    if ( !v->is_initialised )
        return X86EMUL_OKAY;

    /* Asynchronously take the VCPU down and schedule reset work. */
    hvm_vcpu_down(v);
    tasklet_schedule(&vcpu_vlapic(v)->init_tasklet);
    return X86EMUL_RETRY;
}

static int vlapic_accept_sipi(struct vcpu *v, int trampoline_vector)
{
    /* If the VCPU is not on its way down we have nothing to do. */
    if ( !test_bit(_VPF_down, &v->pause_flags) )
        return X86EMUL_OKAY;

    if ( !vlapic_vcpu_pause_async(v) )
        return X86EMUL_RETRY;

    hvm_vcpu_reset_state(v, trampoline_vector << 8, 0);

    vcpu_unpause(v);

    return X86EMUL_OKAY;
}

/* Add a pending IRQ into lapic. */
static int vlapic_accept_irq(struct vcpu *v, int delivery_mode,
                             int vector, int level, int trig_mode)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    int rc = X86EMUL_OKAY;

    switch ( delivery_mode )
    {
    case APIC_DM_FIXED:
    case APIC_DM_LOWEST:
        /* FIXME add logic for vcpu on reset */
        if ( unlikely(!vlapic_enabled(vlapic)) )
            break;

        if ( vlapic_test_and_set_irr(vector, vlapic) && trig_mode )
        {
            HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                        "level trig mode repeatedly for vector %d", vector);
            break;
        }

        if ( trig_mode )
        {
            HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                        "level trig mode for vector %d", vector);
            vlapic_set_vector(vector, &vlapic->regs->data[APIC_TMR]);
        }

        vcpu_kick(v);
        break;

    case APIC_DM_REMRD:
        gdprintk(XENLOG_WARNING, "Ignoring delivery mode 3\n");
        break;

    case APIC_DM_SMI:
        gdprintk(XENLOG_WARNING, "Ignoring guest SMI\n");
        break;

    case APIC_DM_NMI:
        if ( !test_and_set_bool(v->nmi_pending) )
            vcpu_kick(v);
        break;

    case APIC_DM_INIT:
        /* No work on INIT de-assert for P4-type APIC. */
        if ( trig_mode && !(level & APIC_INT_ASSERT) )
            break;
        rc = vlapic_accept_init(v);
        break;

    case APIC_DM_STARTUP:
        rc = vlapic_accept_sipi(v, vector);
        break;

    default:
        gdprintk(XENLOG_ERR, "TODO: unsupported delivery mode %x\n",
                 delivery_mode);
        domain_crash(v->domain);
    }

    return rc;
}

/* This function is used by both ioapic and lapic.The bitmap is for vcpu_id. */
struct vlapic *apic_lowest_prio(struct domain *d, uint32_t bitmap)
{
    int old = d->arch.hvm_domain.irq.round_robin_prev_vcpu;
    uint32_t ppr, target_ppr = UINT_MAX;
    struct vlapic *vlapic, *target = NULL;
    struct vcpu *v;

    if ( unlikely(!d->vcpu) || unlikely((v = d->vcpu[old]) == NULL) )
        return NULL;

    do {
        v = v->next_in_list ? : d->vcpu[0];
        vlapic = vcpu_vlapic(v);
        if ( test_bit(v->vcpu_id, &bitmap) && vlapic_enabled(vlapic) &&
             ((ppr = vlapic_get_ppr(vlapic)) < target_ppr) )
        {
            target = vlapic;
            target_ppr = ppr;
        }
    } while ( v->vcpu_id != old );

    if ( target != NULL )
        d->arch.hvm_domain.irq.round_robin_prev_vcpu =
            vlapic_vcpu(target)->vcpu_id;

    return target;
}

void vlapic_EOI_set(struct vlapic *vlapic)
{
    int vector = vlapic_find_highest_isr(vlapic);

    /* Some EOI writes may not have a matching to an in-service interrupt. */
    if ( vector == -1 )
        return;

    vlapic_clear_vector(vector, &vlapic->regs->data[APIC_ISR]);

    if ( vlapic_test_and_clear_vector(vector, &vlapic->regs->data[APIC_TMR]) )
        vioapic_update_EOI(vlapic_domain(vlapic), vector);

    hvm_dpci_msi_eoi(current->domain, vector);
}

int vlapic_ipi(
    struct vlapic *vlapic, uint32_t icr_low, uint32_t icr_high)
{
    unsigned int dest =         GET_xAPIC_DEST_FIELD(icr_high);
    unsigned int short_hand =   icr_low & APIC_SHORT_MASK;
    unsigned int trig_mode =    icr_low & APIC_INT_LEVELTRIG;
    unsigned int level =        icr_low & APIC_INT_ASSERT;
    unsigned int dest_mode =    icr_low & APIC_DEST_MASK;
    unsigned int delivery_mode =icr_low & APIC_MODE_MASK;
    unsigned int vector =       icr_low & APIC_VECTOR_MASK;

    struct vlapic *target;
    struct vcpu *v;
    uint32_t lpr_map = 0;
    int rc = X86EMUL_OKAY;

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "icr_high 0x%x, icr_low 0x%x, "
                "short_hand 0x%x, dest 0x%x, trig_mode 0x%x, level 0x%x, "
                "dest_mode 0x%x, delivery_mode 0x%x, vector 0x%x",
                icr_high, icr_low, short_hand, dest,
                trig_mode, level, dest_mode, delivery_mode, vector);

    for_each_vcpu ( vlapic_domain(vlapic), v )
    {
        if ( vlapic_match_dest(v, vlapic, short_hand, dest, dest_mode) )
        {
            if ( delivery_mode == APIC_DM_LOWEST )
                __set_bit(v->vcpu_id, &lpr_map);
            else
                rc = vlapic_accept_irq(v, delivery_mode,
                                       vector, level, trig_mode);
        }

        if ( rc != X86EMUL_OKAY )
            break;
    }

    if ( delivery_mode == APIC_DM_LOWEST )
    {
        target = apic_lowest_prio(vlapic_domain(vlapic), lpr_map);
        if ( target != NULL )
            rc = vlapic_accept_irq(vlapic_vcpu(target), delivery_mode,
                                   vector, level, trig_mode);
    }

    return rc;
}

static uint32_t vlapic_get_tmcct(struct vlapic *vlapic)
{
    struct vcpu *v = current;
    uint32_t tmcct, tmict = vlapic_get_reg(vlapic, APIC_TMICT);
    uint64_t counter_passed;

    counter_passed = ((hvm_get_guest_time(v) - vlapic->timer_last_update)
                      / APIC_BUS_CYCLE_NS / vlapic->hw.timer_divisor);
    tmcct = tmict - counter_passed;

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER,
                "timer initial count %d, timer current count %d, "
                "offset %"PRId64,
                tmict, tmcct, counter_passed);

    return tmcct;
}

static void vlapic_set_tdcr(struct vlapic *vlapic, unsigned int val)
{
    /* Only bits 0, 1 and 3 are settable; others are MBZ. */
    val &= 0xb;
    vlapic_set_reg(vlapic, APIC_TDCR, val);

    /* Update the demangled hw.timer_divisor. */
    val = ((val & 3) | ((val & 8) >> 1)) + 1;
    vlapic->hw.timer_divisor = 1 << (val & 7);

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER,
                "timer_divisor: %d", vlapic->hw.timer_divisor);
}

static void vlapic_read_aligned(
    struct vlapic *vlapic, unsigned int offset, unsigned int *result)
{
    switch ( offset )
    {
    case APIC_PROCPRI:
        *result = vlapic_get_ppr(vlapic);
        break;

    case APIC_TMCCT: /* Timer CCR */
        *result = vlapic_get_tmcct(vlapic);
        break;

    default:
        *result = vlapic_get_reg(vlapic, offset);
        break;
    }
}

static int vlapic_read(
    struct vcpu *v, unsigned long address,
    unsigned long len, unsigned long *pval)
{
    unsigned int alignment;
    unsigned int tmp;
    unsigned long result = 0;
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned int offset = address - vlapic_base_address(vlapic);

    if ( offset > (APIC_TDCR + 0x3) )
        goto out;

    alignment = offset & 0x3;

    vlapic_read_aligned(vlapic, offset & ~0x3, &tmp);
    switch ( len )
    {
    case 1:
        result = *((unsigned char *)&tmp + alignment);
        break;

    case 2:
        if ( alignment == 3 )
            goto unaligned_exit_and_crash;
        result = *(unsigned short *)((unsigned char *)&tmp + alignment);
        break;

    case 4:
        if ( alignment != 0 )
            goto unaligned_exit_and_crash;
        result = *(unsigned int *)((unsigned char *)&tmp + alignment);
        break;

    default:
        gdprintk(XENLOG_ERR, "Local APIC read with len=0x%lx, "
                 "should be 4 instead.\n", len);
        goto exit_and_crash;
    }

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "offset 0x%x with length 0x%lx, "
                "and the result is 0x%lx", offset, len, result);

 out:
    *pval = result;
    return X86EMUL_OKAY;

 unaligned_exit_and_crash:
    gdprintk(XENLOG_ERR, "Unaligned LAPIC read len=0x%lx at offset=0x%x.\n",
             len, offset);
 exit_and_crash:
    domain_crash(v->domain);
    return X86EMUL_OKAY;
}

void vlapic_pt_cb(struct vcpu *v, void *data)
{
    *(s_time_t *)data = hvm_get_guest_time(v);
}

static int vlapic_write(struct vcpu *v, unsigned long address,
                        unsigned long len, unsigned long val)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned int offset = address - vlapic_base_address(vlapic);
    int rc = X86EMUL_OKAY;

    if ( offset != 0xb0 )
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                    "offset 0x%x with length 0x%lx, and value is 0x%lx",
                    offset, len, val);

    /*
     * According to the IA32 Manual, all accesses should be 32 bits.
     * Some OSes do 8- or 16-byte accesses, however.
     */
    val = (uint32_t)val;
    if ( len != 4 )
    {
        unsigned long tmp;
        unsigned char alignment;

        gdprintk(XENLOG_INFO, "Notice: Local APIC write with len = %lx\n",len);

        alignment = offset & 0x3;
        (void)vlapic_read(v, offset & ~0x3, 4, &tmp);

        switch ( len )
        {
        case 1:
            val = ((tmp & ~(0xff << (8*alignment))) |
                   ((val & 0xff) << (8*alignment)));
            break;

        case 2:
            if ( alignment & 1 )
                goto unaligned_exit_and_crash;
            val = ((tmp & ~(0xffff << (8*alignment))) |
                   ((val & 0xffff) << (8*alignment)));
            break;

        default:
            gdprintk(XENLOG_ERR, "Local APIC write with len = %lx, "
                     "should be 4 instead\n", len);
            goto exit_and_crash;
        }
    }
    else if ( (offset & 0x3) != 0 )
        goto unaligned_exit_and_crash;

    offset &= ~0x3;

    switch ( offset )
    {
    case APIC_TASKPRI:
        vlapic_set_reg(vlapic, APIC_TASKPRI, val & 0xff);
        break;

    case APIC_EOI:
        vlapic_EOI_set(vlapic);
        break;

    case APIC_LDR:
        vlapic_set_reg(vlapic, APIC_LDR, val & APIC_LDR_MASK);
        break;

    case APIC_DFR:
        vlapic_set_reg(vlapic, APIC_DFR, val | 0x0FFFFFFF);
        break;

    case APIC_SPIV:
        vlapic_set_reg(vlapic, APIC_SPIV, val & 0x3ff);

        if ( !(val & APIC_SPIV_APIC_ENABLED) )
        {
            int i;
            uint32_t lvt_val;

            vlapic->hw.disabled |= VLAPIC_SW_DISABLED;

            for ( i = 0; i < VLAPIC_LVT_NUM; i++ )
            {
                lvt_val = vlapic_get_reg(vlapic, APIC_LVTT + 0x10 * i);
                vlapic_set_reg(vlapic, APIC_LVTT + 0x10 * i,
                               lvt_val | APIC_LVT_MASKED);
            }
        }
        else
            vlapic->hw.disabled &= ~VLAPIC_SW_DISABLED;
        break;

    case APIC_ESR:
        /* Nothing to do. */
        break;

    case APIC_ICR:
        val &= ~(1 << 12); /* always clear the pending bit */
        rc = vlapic_ipi(vlapic, val, vlapic_get_reg(vlapic, APIC_ICR2));
        if ( rc == X86EMUL_OKAY )
            vlapic_set_reg(vlapic, APIC_ICR, val);
        break;

    case APIC_ICR2:
        vlapic_set_reg(vlapic, APIC_ICR2, val & 0xff000000);
        break;

    case APIC_LVTT:         /* LVT Timer Reg */
        vlapic->pt.irq = val & APIC_VECTOR_MASK;
    case APIC_LVTTHMR:      /* LVT Thermal Monitor */
    case APIC_LVTPC:        /* LVT Performance Counter */
    case APIC_LVT0:         /* LVT LINT0 Reg */
    case APIC_LVT1:         /* LVT Lint1 Reg */
    case APIC_LVTERR:       /* LVT Error Reg */
        if ( vlapic_sw_disabled(vlapic) )
            val |= APIC_LVT_MASKED;
        val &= vlapic_lvt_mask[(offset - APIC_LVTT) >> 4];
        vlapic_set_reg(vlapic, offset, val);
        if ( offset == APIC_LVT0 )
            vlapic_adjust_i8259_target(v->domain);
        break;

    case APIC_TMICT:
    {
        uint64_t period = (uint64_t)APIC_BUS_CYCLE_NS *
                            (uint32_t)val * vlapic->hw.timer_divisor;

        vlapic_set_reg(vlapic, APIC_TMICT, val);
        create_periodic_time(current, &vlapic->pt, period, 
                             vlapic_lvtt_period(vlapic) ? period : 0,
                             vlapic->pt.irq, vlapic_pt_cb,
                             &vlapic->timer_last_update);
        vlapic->timer_last_update = vlapic->pt.last_plt_gtime;

        HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                    "bus cycle is %uns, "
                    "initial count %lu, period %"PRIu64"ns",
                    APIC_BUS_CYCLE_NS, val, period);
    }
    break;

    case APIC_TDCR:
        vlapic_set_tdcr(vlapic, val & 0xb);
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER, "timer divisor is 0x%x",
                    vlapic->hw.timer_divisor);
        break;

    default:
        gdprintk(XENLOG_DEBUG,
                 "Local APIC Write to read-only register 0x%x\n", offset);
        break;
    }

    return rc;

 unaligned_exit_and_crash:
    gdprintk(XENLOG_ERR, "Unaligned LAPIC write len=0x%lx at offset=0x%x.\n",
             len, offset);
 exit_and_crash:
    domain_crash(v->domain);
    return rc;
}

static int vlapic_range(struct vcpu *v, unsigned long addr)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned long offset  = addr - vlapic_base_address(vlapic);
    return (!vlapic_hw_disabled(vlapic) && (offset < PAGE_SIZE));
}

struct hvm_mmio_handler vlapic_mmio_handler = {
    .check_handler = vlapic_range,
    .read_handler = vlapic_read,
    .write_handler = vlapic_write
};

void vlapic_msr_set(struct vlapic *vlapic, uint64_t value)
{
    if ( (vlapic->hw.apic_base_msr ^ value) & MSR_IA32_APICBASE_ENABLE )
    {
        if ( value & MSR_IA32_APICBASE_ENABLE )
        {
            vlapic_reset(vlapic);
            vlapic->hw.disabled &= ~VLAPIC_HW_DISABLED;
        }
        else
        {
            vlapic->hw.disabled |= VLAPIC_HW_DISABLED;
        }
    }

    vlapic->hw.apic_base_msr = value;

    vmx_vlapic_msr_changed(vlapic_vcpu(vlapic));

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                "apic base msr is 0x%016"PRIx64, vlapic->hw.apic_base_msr);
}

static int __vlapic_accept_pic_intr(struct vcpu *v)
{
    struct domain *d = v->domain;
    struct vlapic *vlapic = vcpu_vlapic(v);
    uint32_t lvt0 = vlapic_get_reg(vlapic, APIC_LVT0);
    union vioapic_redir_entry redir0 = domain_vioapic(d)->redirtbl[0];

    /* We deliver 8259 interrupts to the appropriate CPU as follows. */
    return ((/* IOAPIC pin0 is unmasked and routing to this LAPIC? */
             ((redir0.fields.delivery_mode == dest_ExtINT) &&
              !redir0.fields.mask &&
              redir0.fields.dest_id == VLAPIC_ID(vlapic) &&
              !vlapic_disabled(vlapic)) ||
             /* LAPIC has LVT0 unmasked for ExtInts? */
             ((lvt0 & (APIC_MODE_MASK|APIC_LVT_MASKED)) == APIC_DM_EXTINT) ||
             /* LAPIC is fully disabled? */
             vlapic_hw_disabled(vlapic)));
}

int vlapic_accept_pic_intr(struct vcpu *v)
{
    return ((v == v->domain->arch.hvm_domain.i8259_target) &&
            __vlapic_accept_pic_intr(v));
}

void vlapic_adjust_i8259_target(struct domain *d)
{
    struct vcpu *v;

    for_each_vcpu ( d, v )
        if ( __vlapic_accept_pic_intr(v) )
            goto found;

    v = d->vcpu ? d->vcpu[0] : NULL;

 found:
    if ( d->arch.hvm_domain.i8259_target == v )
        return;
    d->arch.hvm_domain.i8259_target = v;
    pt_adjust_global_vcpu_target(v);
}

int vlapic_has_pending_irq(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    int irr, isr;

    if ( !vlapic_enabled(vlapic) )
        return -1;

    irr = vlapic_find_highest_irr(vlapic);
    if ( irr == -1 )
        return -1;

    isr = vlapic_find_highest_isr(vlapic);
    isr = (isr != -1) ? isr : 0;
    if ( (isr & 0xf0) >= (irr & 0xf0) )
        return -1;

    return irr;
}

int vlapic_ack_pending_irq(struct vcpu *v, int vector)
{
    struct vlapic *vlapic = vcpu_vlapic(v);

    vlapic_set_vector(vector, &vlapic->regs->data[APIC_ISR]);
    vlapic_clear_irr(vector, vlapic);

    return 1;
}

/* Reset the VLPAIC back to its power-on/reset state. */
void vlapic_reset(struct vlapic *vlapic)
{
    struct vcpu *v = vlapic_vcpu(vlapic);
    int i;

    vlapic_set_reg(vlapic, APIC_ID,  (v->vcpu_id * 2) << 24);
    vlapic_set_reg(vlapic, APIC_LVR, VLAPIC_VERSION);

    for ( i = 0; i < 8; i++ )
    {
        vlapic_set_reg(vlapic, APIC_IRR + 0x10 * i, 0);
        vlapic_set_reg(vlapic, APIC_ISR + 0x10 * i, 0);
        vlapic_set_reg(vlapic, APIC_TMR + 0x10 * i, 0);
    }
    vlapic_set_reg(vlapic, APIC_ICR,     0);
    vlapic_set_reg(vlapic, APIC_ICR2,    0);
    vlapic_set_reg(vlapic, APIC_LDR,     0);
    vlapic_set_reg(vlapic, APIC_TASKPRI, 0);
    vlapic_set_reg(vlapic, APIC_TMICT,   0);
    vlapic_set_reg(vlapic, APIC_TMCCT,   0);
    vlapic_set_tdcr(vlapic, 0);

    vlapic_set_reg(vlapic, APIC_DFR, 0xffffffffU);

    for ( i = 0; i < VLAPIC_LVT_NUM; i++ )
        vlapic_set_reg(vlapic, APIC_LVTT + 0x10 * i, APIC_LVT_MASKED);

    vlapic_set_reg(vlapic, APIC_SPIV, 0xff);
    vlapic->hw.disabled |= VLAPIC_SW_DISABLED;

    destroy_periodic_time(&vlapic->pt);
}

/* rearm the actimer if needed, after a HVM restore */
static void lapic_rearm(struct vlapic *s)
{
    unsigned long tmict = vlapic_get_reg(s, APIC_TMICT);
    uint64_t period;

    if ( (tmict = vlapic_get_reg(s, APIC_TMICT)) == 0 )
        return;

    period = ((uint64_t)APIC_BUS_CYCLE_NS *
              (uint32_t)tmict * s->hw.timer_divisor);
    s->pt.irq = vlapic_get_reg(s, APIC_LVTT) & APIC_VECTOR_MASK;
    create_periodic_time(vlapic_vcpu(s), &s->pt, period,
                         vlapic_lvtt_period(s) ? period : 0,
                         s->pt.irq, vlapic_pt_cb,
                         &s->timer_last_update);
    s->timer_last_update = s->pt.last_plt_gtime;
}

static int lapic_save_hidden(struct domain *d, hvm_domain_context_t *h)
{
    struct vcpu *v;
    struct vlapic *s;
    int rc = 0;

    for_each_vcpu ( d, v )
    {
        s = vcpu_vlapic(v);
        if ( (rc = hvm_save_entry(LAPIC, v->vcpu_id, h, &s->hw)) != 0 )
            break;
    }

    return rc;
}

static int lapic_save_regs(struct domain *d, hvm_domain_context_t *h)
{
    struct vcpu *v;
    struct vlapic *s;
    int rc = 0;

    for_each_vcpu ( d, v )
    {
        s = vcpu_vlapic(v);
        if ( (rc = hvm_save_entry(LAPIC_REGS, v->vcpu_id, h, s->regs)) != 0 )
            break;
    }

    return rc;
}

static int lapic_load_hidden(struct domain *d, hvm_domain_context_t *h)
{
    uint16_t vcpuid;
    struct vcpu *v;
    struct vlapic *s;
    
    /* Which vlapic to load? */
    vcpuid = hvm_load_instance(h); 
    if ( vcpuid >= d->max_vcpus || (v = d->vcpu[vcpuid]) == NULL )
    {
        gdprintk(XENLOG_ERR, "HVM restore: domain has no vlapic %u\n", vcpuid);
        return -EINVAL;
    }
    s = vcpu_vlapic(v);
    
    if ( hvm_load_entry(LAPIC, h, &s->hw) != 0 ) 
        return -EINVAL;

    vmx_vlapic_msr_changed(v);

    return 0;
}

static int lapic_load_regs(struct domain *d, hvm_domain_context_t *h)
{
    uint16_t vcpuid;
    struct vcpu *v;
    struct vlapic *s;
    
    /* Which vlapic to load? */
    vcpuid = hvm_load_instance(h); 
    if ( vcpuid >= d->max_vcpus || (v = d->vcpu[vcpuid]) == NULL )
    {
        gdprintk(XENLOG_ERR, "HVM restore: domain has no vlapic %u\n", vcpuid);
        return -EINVAL;
    }
    s = vcpu_vlapic(v);
    
    if ( hvm_load_entry(LAPIC_REGS, h, s->regs) != 0 ) 
        return -EINVAL;

    vlapic_adjust_i8259_target(d);
    lapic_rearm(s);
    return 0;
}

HVM_REGISTER_SAVE_RESTORE(LAPIC, lapic_save_hidden, lapic_load_hidden,
                          1, HVMSR_PER_VCPU);
HVM_REGISTER_SAVE_RESTORE(LAPIC_REGS, lapic_save_regs, lapic_load_regs,
                          1, HVMSR_PER_VCPU);

int vlapic_init(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned int memflags = MEMF_node(vcpu_to_node(v));

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "%d", v->vcpu_id);

    vlapic->pt.source = PTSRC_lapic;

#ifdef __i386__
    /* 32-bit VMX may be limited to 32-bit physical addresses. */
    if ( boot_cpu_data.x86_vendor == X86_VENDOR_INTEL )
        memflags |= MEMF_bits(32);
#endif
    if (vlapic->regs_page == NULL)
    {
        vlapic->regs_page = alloc_domheap_page(NULL, memflags);
        if ( vlapic->regs_page == NULL )
        {
            dprintk(XENLOG_ERR, "alloc vlapic regs error: %d/%d\n",
                    v->domain->domain_id, v->vcpu_id);
            return -ENOMEM;
        }
    }
    if (vlapic->regs == NULL) 
    {
        vlapic->regs = map_domain_page_global(page_to_mfn(vlapic->regs_page));
        if ( vlapic->regs == NULL )
        {
            dprintk(XENLOG_ERR, "map vlapic regs error: %d/%d\n",
                    v->domain->domain_id, v->vcpu_id);
            return -ENOMEM;
        }
    }
    clear_page(vlapic->regs);

    vlapic_reset(vlapic);

    vlapic->hw.apic_base_msr = (MSR_IA32_APICBASE_ENABLE |
                                APIC_DEFAULT_PHYS_BASE);
    if ( v->vcpu_id == 0 )
        vlapic->hw.apic_base_msr |= MSR_IA32_APICBASE_BSP;

    tasklet_init(&vlapic->init_tasklet, vlapic_init_action, (unsigned long)v);

    return 0;
}

void vlapic_destroy(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);

    tasklet_kill(&vlapic->init_tasklet);
    destroy_periodic_time(&vlapic->pt);
    unmap_domain_page_global(vlapic->regs);
    free_domheap_page(vlapic->regs_page);
}
