/*
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
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright (C) IBM Corp. 2005
 *
 * Authors: Jimi Xenidis <jimix@watson.ibm.com>
 */

#ifndef _OFTREE_H
#define _OFTREE_H
#include <xen/multiboot.h>

extern ulong oftree;
extern ulong oftree_len;
extern ulong oftree_end;

extern int ofd_dom0_fixup(struct domain *d, ulong mem, start_info_t *si);
extern void ofd_memory_props(void *m, struct domain *d);

extern int firmware_image_start[0];
extern int firmware_image_size[0];

extern void memory_init(module_t *mod, int mcount);

#endif  /* #ifndef _OFTREE_H */
