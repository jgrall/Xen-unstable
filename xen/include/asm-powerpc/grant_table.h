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

#ifndef __ASM_PPC_GRANT_TABLE_H__
#define __ASM_PPC_GRANT_TABLE_H__

#include <asm/mm.h>

#define ORDER_GRANT_FRAMES 2

/*
 * Caller must own caller's BIGLOCK, is responsible for flushing the TLB, and
 * must hold a reference to the page.
 */
int create_grant_host_mapping(
    unsigned long addr, unsigned long frame, unsigned int flags);
int destroy_grant_host_mapping(
    unsigned long addr, unsigned long frame, unsigned int flags);

#define gnttab_create_shared_page(d, t, i)                               \
    do {                                                                 \
        share_xen_page_with_guest(                                       \
            virt_to_page((char *)(t)->shared + ((i) * PAGE_SIZE)),       \
            (d), XENSHARE_writable);                                     \
    } while ( 0 )

#define gnttab_shared_mfn(d, t, i)                      \
    ((virt_to_maddr((t)->shared) >> PAGE_SHIFT) + (i))

#define gnttab_shared_gmfn(d, t, i)                     \
    (mfn_to_gmfn(d, gnttab_shared_mfn(d, t, i)))

#define gnttab_log_dirty(d, f) ((void )0)

static inline void gnttab_clear_flag(unsigned long nr, uint16_t *addr)
{
    unsigned long *laddr;
    unsigned long lnr;

    BUG_ON((ulong)addr % sizeof(ulong));

    lnr = (BITS_PER_LONG - (sizeof(*addr) * 8)) + nr;
    laddr = (unsigned long *)addr;
    clear_bit(lnr, laddr);
}

#endif  /* __ASM_PPC_GRANT_TABLE_H__ */
