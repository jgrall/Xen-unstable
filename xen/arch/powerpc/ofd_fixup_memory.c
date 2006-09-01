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
 * Copyright (C) IBM Corp. 2006
 *
 * Authors: Jimi Xenidis <jimix@watson.ibm.com>
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <public/xen.h>
#include "of-devtree.h"
#include "oftree.h"

static char memory[] = "memory";

struct mem_reg {
    u64 addr;
    u64 sz;
};

static void ofd_memory_clean(void *m)
{
    ofdn_t old;

    /* Remove all old memory props */
    do {
        old = ofd_node_find_by_prop(m, OFD_ROOT, "device_type",
                                    memory, sizeof(memory));
        if (old <= 0)
            break;

        ofd_node_prune(m, old);
    } while (1);
}

static ofdn_t ofd_memory_node_create(
    void *m, ofdn_t p, const char *ppath, const char *name,
    const char *dt, ulong start, ulong size)
{
    struct mem_reg reg;
    char path[128];
    ulong l;
    ofdn_t n;
    ulong nl = strlen(name) + 1;
    ulong dtl = strlen(dt) + 1;

    l = snprintf(path, sizeof (path), "%s/%s@%lx", ppath, name, start);
    n = ofd_node_add(m, p, path, l + 1);
    ofd_prop_add(m, n, "name", name, nl);
    ofd_prop_add(m, n, "device_type", dt, dtl);

    /* physical addresses usable without regard to OF */
    reg.addr = start;
    reg.sz = size;
    ofd_prop_add(m, n, "reg", &reg, sizeof (reg));

    return n;
}

static void ofd_memory_rma_node(void *m, struct domain *d)
{
    ulong size = rma_size(d->arch.rma_order);
    ofdn_t n;

    n = ofd_memory_node_create(m, OFD_ROOT, "", memory, memory, 0, size);
    BUG_ON(n <= 0);
}

static void ofd_memory_extent_nodes(void *m, struct domain *d)
{
    ulong start;
    ulong size;
    ofdn_t n;
    struct page_extents *pe;

    list_for_each_entry (pe, &d->arch.extent_list, pe_list) {

        start = pe->pfn << PAGE_SHIFT;
        size = 1UL << (pe->order + PAGE_SHIFT);

        n = ofd_memory_node_create(m, OFD_ROOT, "", memory, memory,
                                    start, size);

        BUG_ON(n <= 0);
    }
}

void ofd_memory_props(void *m, struct domain *d)
{
    ofd_memory_clean(m);
    ofd_memory_rma_node(m, d);
    ofd_memory_extent_nodes(m,d);
}
