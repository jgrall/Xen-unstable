/******************************************************************************
 * xenoprof.c 
 *
 * Copyright (c) 2006 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <xen/config.h>
#include <xen/sched.h>
#include <public/xen.h>
#include <xen/xenoprof.h>

int
xenoprofile_get_mode(struct vcpu *v, struct cpu_user_regs * const regs)
{
    int mode;

    // mode
    // 0: user, 1: kernel, 2: xen
    switch (ring(regs))
    {
        case 3:
                mode = 0;
                break;
        case CONFIG_CPL0_EMUL:
                mode = 1;
                break;
        case 0:
                mode = 2;
                break;
        default:
                gdprintk(XENLOG_ERR, "%s:%d ring%d is used!\n", __func__,
                         __LINE__, 3 - CONFIG_CPL0_EMUL);
                mode = 1; /* fall back to kernel mode. */
    }
    return mode;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
