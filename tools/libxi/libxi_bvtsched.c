/******************************************************************************
 * libxi_bvtsched.c
 * 
 * API for manipulating parameters of the Borrowed Virtual Time scheduler.
 * 
 * Copyright (c) 2003, K A Fraser.
 */

#include "libxi_private.h"

int xi_bvtsched_global_set(unsigned long ctx_allow)
{
    dom0_op_t op;
    op.cmd = DOM0_BVTCTL;
    op.u.bvtctl.ctx_allow = ctx_allow;
    return do_dom0_op(&op);
}

int xi_bvtsched_domain_set(unsigned int domid,
                           unsigned long mcuadv,
                           unsigned long warp,
                           unsigned long warpl,
                           unsigned long warpu)
{
    dom0_op_t op;
    op.cmd = DOM0_ADJUSTDOM;
    op.u.adjustdom.domain  = domid;
    op.u.adjustdom.mcu_adv = mcuadv;
    op.u.adjustdom.warp    = warp;
    op.u.adjustdom.warpl   = warpl;
    op.u.adjustdom.warpu   = warpu;
    return do_dom0_op(&op);
}
