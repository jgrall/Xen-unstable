/******************************************************************************
 * xc_tbuf.c
 *
 * API for manipulating and accessing trace buffer parameters
 *
 * Copyright (c) 2005, Rob Gardner
 *
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 */

#include "xc_private.h"

static int tbuf_enable(int xc_handle, int enable)
{
    DECLARE_DOM0_OP;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    if (enable)
        op.u.tbufcontrol.op  = DOM0_TBUF_ENABLE;
    else
        op.u.tbufcontrol.op  = DOM0_TBUF_DISABLE;

    return xc_dom0_op(xc_handle, &op);
}

int xc_tbuf_set_size(int xc_handle, unsigned long size)
{
    DECLARE_DOM0_OP;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    op.u.tbufcontrol.op  = DOM0_TBUF_SET_SIZE;
    op.u.tbufcontrol.size = size;

    return xc_dom0_op(xc_handle, &op);
}

int xc_tbuf_get_size(int xc_handle, unsigned long *size)
{
    int rc;
    DECLARE_DOM0_OP;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    op.u.tbufcontrol.op  = DOM0_TBUF_GET_INFO;

    rc = xc_dom0_op(xc_handle, &op);
    if (rc == 0)
        *size = op.u.tbufcontrol.size;
    return rc;
}

int xc_tbuf_enable(int xc_handle, size_t cnt, unsigned long *mfn,
                   unsigned long *size)
{
    DECLARE_DOM0_OP;
    int rc;

    /*
     * Ignore errors (at least for now) as we get an error if size is already
     * set (since trace buffers cannot be reallocated). If we really have no
     * buffers at all then tbuf_enable() will fail, so this is safe.
     */
    (void)xc_tbuf_set_size(xc_handle, cnt);

    if ( tbuf_enable(xc_handle, 1) != 0 )
        return -1;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    op.u.tbufcontrol.op  = DOM0_TBUF_GET_INFO;

    rc = xc_dom0_op(xc_handle, &op);
    if ( rc == 0 )
    {
        *size = op.u.tbufcontrol.size;
        *mfn = op.u.tbufcontrol.buffer_mfn;
    }

    return 0;
}

int xc_tbuf_disable(int xc_handle)
{
    return tbuf_enable(xc_handle, 0);
}

int xc_tbuf_set_cpu_mask(int xc_handle, uint32_t mask)
{
    DECLARE_DOM0_OP;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    op.u.tbufcontrol.op  = DOM0_TBUF_SET_CPU_MASK;
    op.u.tbufcontrol.cpu_mask = mask;

    return do_dom0_op(xc_handle, &op);
}

int xc_tbuf_set_evt_mask(int xc_handle, uint32_t mask)
{
    DECLARE_DOM0_OP;

    op.cmd = DOM0_TBUFCONTROL;
    op.interface_version = DOM0_INTERFACE_VERSION;
    op.u.tbufcontrol.op  = DOM0_TBUF_SET_EVT_MASK;
    op.u.tbufcontrol.evt_mask = mask;

    return do_dom0_op(xc_handle, &op);
}

