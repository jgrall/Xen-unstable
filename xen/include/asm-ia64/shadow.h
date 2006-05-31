/******************************************************************************
 * include/asm-ia64/shadow.h
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

#ifndef _XEN_SHADOW_H
#define _XEN_SHADOW_H

#include <xen/config.h>

#ifdef CONFIG_XEN_IA64_DOM0_VP
#ifndef CONFIG_SHADOW
# error "CONFIG_SHADOW must be defined"
#endif

#define shadow_drop_references(d, p)          ((void)0)

// this is used only x86-specific code
//#define shadow_sync_and_drop_references(d, p) ((void)0)

#define shadow_mode_translate(d)              (1)

/*
 * Utilities to change relationship of gpfn->mfn for designated domain,
 * which is required by gnttab transfer, balloon, device model and etc.
 */
void guest_physmap_add_page(struct domain *d, unsigned long gpfn, unsigned long mfn);
void guest_physmap_remove_page(struct domain *d, unsigned long gpfn, unsigned long mfn);
#endif

#endif // _XEN_SHADOW_H

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

