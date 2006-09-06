/******************************************************************************
 * common/grant_table.c
 * 
 * Mechanism for granting foreign access to page frames, and receiving
 * page-ownership transfers.
 * 
 * Copyright (c) 2005 Christopher Clark
 * Copyright (c) 2004 K A Fraser
 * Copyright (c) 2005 Andrew Warfield
 * Modifications by Geoffrey Lefebvre are (c) Intel Research Cambridge
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
 */

#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/shadow.h>
#include <xen/mm.h>
#include <xen/trace.h>
#include <xen/guest_access.h>
#include <xen/domain_page.h>
#include <acm/acm_hooks.h>

/*
 * The first two members of a grant entry are updated as a combined pair.
 * The following union allows that to happen in an endian-neutral fashion.
 */
union grant_combo {
    uint32_t word;
    struct {
        uint16_t flags;
        domid_t  domid;
    } shorts;
};

#define PIN_FAIL(_lbl, _rc, _f, _a...)          \
    do {                                        \
        DPRINTK( _f, ## _a );                   \
        rc = (_rc);                             \
        goto _lbl;                              \
    } while ( 0 )

static inline int
get_maptrack_handle(
    struct grant_table *t)
{
    unsigned int h;
    if ( unlikely((h = t->maptrack_head) == (t->maptrack_limit - 1)) )
        return -1;
    t->maptrack_head = t->maptrack[h].ref;
    t->map_count++;
    return h;
}

static inline void
put_maptrack_handle(
    struct grant_table *t, int handle)
{
    t->maptrack[handle].ref = t->maptrack_head;
    t->maptrack_head = handle;
    t->map_count--;
}

/*
 * Returns 0 if TLB flush / invalidate required by caller.
 * va will indicate the address to be invalidated.
 * 
 * addr is _either_ a host virtual address, or the address of the pte to
 * update, as indicated by the GNTMAP_contains_pte flag.
 */
static void
__gnttab_map_grant_ref(
    struct gnttab_map_grant_ref *op)
{
    struct domain *ld, *rd;
    struct vcpu   *led;
    int            handle;
    unsigned long  frame = 0;
    int            rc = GNTST_okay;
    struct active_grant_entry *act;
    grant_entry_t *sha;
    union grant_combo scombo, prev_scombo, new_scombo;

    /*
     * We bound the number of times we retry CMPXCHG on memory locations that
     * we share with a guest OS. The reason is that the guest can modify that
     * location at a higher rate than we can read-modify-CMPXCHG, so the guest
     * could cause us to livelock. There are a few cases where it is valid for
     * the guest to race our updates (e.g., to change the GTF_readonly flag),
     * so we allow a few retries before failing.
     */
    int retries = 0;

    led = current;
    ld = led->domain;

    if ( unlikely(op->ref >= NR_GRANT_ENTRIES) ||
         unlikely((op->flags & (GNTMAP_device_map|GNTMAP_host_map)) == 0) )
    {
        DPRINTK("Bad ref (%d) or flags (%x).\n", op->ref, op->flags);
        op->status = GNTST_bad_gntref;
        return;
    }

    if ( acm_pre_grant_map_ref(op->dom) )
    {
        op->status = GNTST_permission_denied;
        return;
    }

    if ( unlikely((rd = find_domain_by_id(op->dom)) == NULL) )
    {
        if ( rd != NULL )
            put_domain(rd);
        DPRINTK("Could not find domain %d\n", op->dom);
        op->status = GNTST_bad_domain;
        return;
    }

    /* Get a maptrack handle. */
    if ( unlikely((handle = get_maptrack_handle(ld->grant_table)) == -1) )
    {
        int                   i;
        struct grant_mapping *new_mt;
        struct grant_table   *lgt = ld->grant_table;

        if ( (lgt->maptrack_limit << 1) > MAPTRACK_MAX_ENTRIES )
        {
            put_domain(rd);
            DPRINTK("Maptrack table is at maximum size.\n");
            op->status = GNTST_no_device_space;
            return;
        }

        /* Grow the maptrack table. */
        new_mt = alloc_xenheap_pages(lgt->maptrack_order + 1);
        if ( new_mt == NULL )
        {
            put_domain(rd);
            DPRINTK("No more map handles available.\n");
            op->status = GNTST_no_device_space;
            return;
        }

        memcpy(new_mt, lgt->maptrack, PAGE_SIZE << lgt->maptrack_order);
        for ( i = lgt->maptrack_limit; i < (lgt->maptrack_limit << 1); i++ )
            new_mt[i].ref = i+1;

        free_xenheap_pages(lgt->maptrack, lgt->maptrack_order);
        lgt->maptrack          = new_mt;
        lgt->maptrack_order   += 1;
        lgt->maptrack_limit  <<= 1;

        DPRINTK("Doubled maptrack size\n");
        handle = get_maptrack_handle(ld->grant_table);
    }

    act = &rd->grant_table->active[op->ref];
    sha = &rd->grant_table->shared[op->ref];

    spin_lock(&rd->grant_table->lock);

    /* If already pinned, check the active domid and avoid refcnt overflow. */
    if ( act->pin &&
         ((act->domid != ld->domain_id) ||
          (act->pin & 0x80808080U) != 0) )
        PIN_FAIL(unlock_out, GNTST_general_error,
                 "Bad domain (%d != %d), or risk of counter overflow %08x\n",
                 act->domid, ld->domain_id, act->pin);

    if ( !act->pin ||
         (!(op->flags & GNTMAP_readonly) &&
          !(act->pin & (GNTPIN_hstw_mask|GNTPIN_devw_mask))) )
    {
        scombo.word = *(u32 *)&sha->flags;

        /*
         * This loop attempts to set the access (reading/writing) flags
         * in the grant table entry.  It tries a cmpxchg on the field
         * up to five times, and then fails under the assumption that 
         * the guest is misbehaving.
         */
        for ( ; ; )
        {
            /* If not already pinned, check the grant domid and type. */
            if ( !act->pin &&
                 (((scombo.shorts.flags & GTF_type_mask) !=
                   GTF_permit_access) ||
                  (scombo.shorts.domid != ld->domain_id)) )
                 PIN_FAIL(unlock_out, GNTST_general_error,
                          "Bad flags (%x) or dom (%d). (expected dom %d)\n",
                          scombo.shorts.flags, scombo.shorts.domid,
                          ld->domain_id);

            new_scombo = scombo;
            new_scombo.shorts.flags |= GTF_reading;

            if ( !(op->flags & GNTMAP_readonly) )
            {
                new_scombo.shorts.flags |= GTF_writing;
                if ( unlikely(scombo.shorts.flags & GTF_readonly) )
                    PIN_FAIL(unlock_out, GNTST_general_error,
                             "Attempt to write-pin a r/o grant entry.\n");
            }

            prev_scombo.word = cmpxchg((u32 *)&sha->flags,
                                       scombo.word, new_scombo.word);
            if ( likely(prev_scombo.word == scombo.word) )
                break;

            if ( retries++ == 4 )
                PIN_FAIL(unlock_out, GNTST_general_error,
                         "Shared grant entry is unstable.\n");

            scombo = prev_scombo;
        }

        if ( !act->pin )
        {
            act->domid = scombo.shorts.domid;
            act->frame = gmfn_to_mfn(rd, sha->frame);
        }
    }

    if ( op->flags & GNTMAP_device_map )
        act->pin += (op->flags & GNTMAP_readonly) ?
            GNTPIN_devr_inc : GNTPIN_devw_inc;
    if ( op->flags & GNTMAP_host_map )
        act->pin += (op->flags & GNTMAP_readonly) ?
            GNTPIN_hstr_inc : GNTPIN_hstw_inc;

    spin_unlock(&rd->grant_table->lock);

    frame = act->frame;
    if ( unlikely(!mfn_valid(frame)) ||
         unlikely(!((op->flags & GNTMAP_readonly) ?
                    get_page(mfn_to_page(frame), rd) :
                    get_page_and_type(mfn_to_page(frame), rd,
                                      PGT_writable_page))) )
        PIN_FAIL(undo_out, GNTST_general_error,
                 "Could not pin the granted frame (%lx)!\n", frame);

    if ( op->flags & GNTMAP_host_map )
    {
        rc = create_grant_host_mapping(op->host_addr, frame, op->flags);
        if ( rc != GNTST_okay )
        {
            if ( !(op->flags & GNTMAP_readonly) )
                put_page_type(mfn_to_page(frame));
            put_page(mfn_to_page(frame));
            goto undo_out;
        }

        if ( op->flags & GNTMAP_device_map )
        {
            (void)get_page(mfn_to_page(frame), rd);
            if ( !(op->flags & GNTMAP_readonly) )
                get_page_type(mfn_to_page(frame), PGT_writable_page);
        }
    }

    TRACE_1D(TRC_MEM_PAGE_GRANT_MAP, op->dom);

    ld->grant_table->maptrack[handle].domid = op->dom;
    ld->grant_table->maptrack[handle].ref   = op->ref;
    ld->grant_table->maptrack[handle].flags = op->flags;

    op->dev_bus_addr = (u64)frame << PAGE_SHIFT;
    op->handle       = handle;
    op->status       = GNTST_okay;

    put_domain(rd);
    return;

 undo_out:
    spin_lock(&rd->grant_table->lock);

    if ( op->flags & GNTMAP_device_map )
        act->pin -= (op->flags & GNTMAP_readonly) ?
            GNTPIN_devr_inc : GNTPIN_devw_inc;
    if ( op->flags & GNTMAP_host_map )
        act->pin -= (op->flags & GNTMAP_readonly) ?
            GNTPIN_hstr_inc : GNTPIN_hstw_inc;

    if ( !(op->flags & GNTMAP_readonly) &&
         !(act->pin & (GNTPIN_hstw_mask|GNTPIN_devw_mask)) )
        gnttab_clear_flag(_GTF_writing, &sha->flags);

    if ( !act->pin )
        gnttab_clear_flag(_GTF_reading, &sha->flags);

 unlock_out:
    spin_unlock(&rd->grant_table->lock);
    op->status = rc;
    put_maptrack_handle(ld->grant_table, handle);
    put_domain(rd);
}

static long
gnttab_map_grant_ref(
    XEN_GUEST_HANDLE(gnttab_map_grant_ref_t) uop, unsigned int count)
{
    int i;
    struct gnttab_map_grant_ref op;

    for ( i = 0; i < count; i++ )
    {
        if ( unlikely(__copy_from_guest_offset(&op, uop, i, 1)) )
            return -EFAULT;
        __gnttab_map_grant_ref(&op);
        if ( unlikely(__copy_to_guest_offset(uop, i, &op, 1)) )
            return -EFAULT;
    }

    return 0;
}

static void
__gnttab_unmap_grant_ref(
    struct gnttab_unmap_grant_ref *op)
{
    domid_t          dom;
    grant_ref_t      ref;
    struct domain   *ld, *rd;
    struct active_grant_entry *act;
    grant_entry_t   *sha;
    struct grant_mapping *map;
    u16              flags;
    s16              rc = 0;
    unsigned long    frame;

    ld = current->domain;

    frame = (unsigned long)(op->dev_bus_addr >> PAGE_SHIFT);

    map = &ld->grant_table->maptrack[op->handle];

    if ( unlikely(op->handle >= ld->grant_table->maptrack_limit) ||
         unlikely(!map->flags) )
    {
        DPRINTK("Bad handle (%d).\n", op->handle);
        op->status = GNTST_bad_handle;
        return;
    }

    dom   = map->domid;
    ref   = map->ref;
    flags = map->flags;

    if ( unlikely((rd = find_domain_by_id(dom)) == NULL) )
    {
        if ( rd != NULL )
            put_domain(rd);
        DPRINTK("Could not find domain %d\n", dom);
        op->status = GNTST_bad_domain;
        return;
    }

    TRACE_1D(TRC_MEM_PAGE_GRANT_UNMAP, dom);

    act = &rd->grant_table->active[ref];
    sha = &rd->grant_table->shared[ref];

    spin_lock(&rd->grant_table->lock);

    if ( frame == 0 )
    {
        frame = act->frame;
    }
    else
    {
        if ( unlikely(frame != act->frame) )
            PIN_FAIL(unmap_out, GNTST_general_error,
                     "Bad frame number doesn't match gntref.\n");
        if ( flags & GNTMAP_device_map )
        {
            ASSERT(act->pin & (GNTPIN_devw_mask | GNTPIN_devr_mask));
            map->flags &= ~GNTMAP_device_map;
            if ( flags & GNTMAP_readonly )
            {
                act->pin -= GNTPIN_devr_inc;
                put_page(mfn_to_page(frame));
            }
            else
            {
                act->pin -= GNTPIN_devw_inc;
                put_page_and_type(mfn_to_page(frame));
            }
        }
    }

    if ( (op->host_addr != 0) && (flags & GNTMAP_host_map) )
    {
        if ( (rc = destroy_grant_host_mapping(op->host_addr,
                                              frame, flags)) < 0 )
            goto unmap_out;

        ASSERT(act->pin & (GNTPIN_hstw_mask | GNTPIN_hstr_mask));
        map->flags &= ~GNTMAP_host_map;
        if ( flags & GNTMAP_readonly )
        {
            act->pin -= GNTPIN_hstr_inc;
            put_page(mfn_to_page(frame));
        }
        else
        {
            act->pin -= GNTPIN_hstw_inc;
            put_page_and_type(mfn_to_page(frame));
        }
    }

    if ( (map->flags & (GNTMAP_device_map|GNTMAP_host_map)) == 0 )
    {
        map->flags = 0;
        put_maptrack_handle(ld->grant_table, op->handle);
    }

    /* If just unmapped a writable mapping, mark as dirtied */
    if ( !(flags & GNTMAP_readonly) )
         gnttab_mark_dirty(rd, frame);

    if ( ((act->pin & (GNTPIN_devw_mask|GNTPIN_hstw_mask)) == 0) &&
         !(flags & GNTMAP_readonly) )
        gnttab_clear_flag(_GTF_writing, &sha->flags);

    if ( act->pin == 0 )
        gnttab_clear_flag(_GTF_reading, &sha->flags);

 unmap_out:
    op->status = rc;
    spin_unlock(&rd->grant_table->lock);
    put_domain(rd);
}

static long
gnttab_unmap_grant_ref(
    XEN_GUEST_HANDLE(gnttab_unmap_grant_ref_t) uop, unsigned int count)
{
    int i;
    struct gnttab_unmap_grant_ref op;

    for ( i = 0; i < count; i++ )
    {
        if ( unlikely(__copy_from_guest_offset(&op, uop, i, 1)) )
            goto fault;
        __gnttab_unmap_grant_ref(&op);
        if ( unlikely(__copy_to_guest_offset(uop, i, &op, 1)) )
            goto fault;
    }

    flush_tlb_mask(current->domain->domain_dirty_cpumask);
    return 0;

fault:
    flush_tlb_mask(current->domain->domain_dirty_cpumask);
    return -EFAULT;    
}

static long 
gnttab_setup_table(
    XEN_GUEST_HANDLE(gnttab_setup_table_t) uop, unsigned int count)
{
    struct gnttab_setup_table op;
    struct domain *d;
    int            i;
    unsigned long  gmfn;
    domid_t        dom;

    if ( count != 1 )
        return -EINVAL;

    if ( unlikely(copy_from_guest(&op, uop, 1) != 0) )
    {
        DPRINTK("Fault while reading gnttab_setup_table_t.\n");
        return -EFAULT;
    }

    if ( unlikely(op.nr_frames > NR_GRANT_FRAMES) )
    {
        DPRINTK("Xen only supports up to %d grant-table frames per domain.\n",
                NR_GRANT_FRAMES);
        op.status = GNTST_general_error;
        goto out;
    }

    dom = op.dom;
    if ( dom == DOMID_SELF )
    {
        dom = current->domain->domain_id;
    }
    else if ( unlikely(!IS_PRIV(current->domain)) )
    {
        op.status = GNTST_permission_denied;
        goto out;
    }

    if ( unlikely((d = find_domain_by_id(dom)) == NULL) )
    {
        DPRINTK("Bad domid %d.\n", dom);
        op.status = GNTST_bad_domain;
        goto out;
    }

    ASSERT(d->grant_table != NULL);
    op.status = GNTST_okay;
    for ( i = 0; i < op.nr_frames; i++ )
    {
        gmfn = gnttab_shared_gmfn(d, d->grant_table, i);
        (void)copy_to_guest_offset(op.frame_list, i, &gmfn, 1);
    }

    put_domain(d);

 out:
    if ( unlikely(copy_to_guest(uop, &op, 1)) )
        return -EFAULT;

    return 0;
}

/*
 * Check that the given grant reference (rd,ref) allows 'ld' to transfer
 * ownership of a page frame. If so, lock down the grant entry.
 */
static int 
gnttab_prepare_for_transfer(
    struct domain *rd, struct domain *ld, grant_ref_t ref)
{
    struct grant_table *rgt;
    struct grant_entry *sha;
    union grant_combo   scombo, prev_scombo, new_scombo;
    int                 retries = 0;

    if ( unlikely((rgt = rd->grant_table) == NULL) ||
         unlikely(ref >= NR_GRANT_ENTRIES) )
    {
        DPRINTK("Dom %d has no g.t., or ref is bad (%d).\n",
                rd->domain_id, ref);
        return 0;
    }

    spin_lock(&rgt->lock);

    sha = &rgt->shared[ref];
    
    scombo.word = *(u32 *)&sha->flags;

    for ( ; ; )
    {
        if ( unlikely(scombo.shorts.flags != GTF_accept_transfer) ||
             unlikely(scombo.shorts.domid != ld->domain_id) )
        {
            DPRINTK("Bad flags (%x) or dom (%d). (NB. expected dom %d)\n",
                    scombo.shorts.flags, scombo.shorts.domid,
                    ld->domain_id);
            goto fail;
        }

        new_scombo = scombo;
        new_scombo.shorts.flags |= GTF_transfer_committed;

        prev_scombo.word = cmpxchg((u32 *)&sha->flags,
                                   scombo.word, new_scombo.word);
        if ( likely(prev_scombo.word == scombo.word) )
            break;

        if ( retries++ == 4 )
        {
            DPRINTK("Shared grant entry is unstable.\n");
            goto fail;
        }

        scombo = prev_scombo;
    }

    spin_unlock(&rgt->lock);
    return 1;

 fail:
    spin_unlock(&rgt->lock);
    return 0;
}

static long
gnttab_transfer(
    XEN_GUEST_HANDLE(gnttab_transfer_t) uop, unsigned int count)
{
    struct domain *d = current->domain;
    struct domain *e;
    struct page_info *page;
    int i;
    grant_entry_t *sha;
    struct gnttab_transfer gop;
    unsigned long mfn;

    for ( i = 0; i < count; i++ )
    {
        /* Read from caller address space. */
        if ( unlikely(__copy_from_guest_offset(&gop, uop, i, 1)) )
        {
            DPRINTK("gnttab_transfer: error reading req %d/%d\n", i, count);
            return -EFAULT;
        }

        mfn = gmfn_to_mfn(d, gop.mfn);

        /* Check the passed page frame for basic validity. */
        if ( unlikely(!mfn_valid(mfn)) )
        { 
            DPRINTK("gnttab_transfer: out-of-range %lx\n",
                    (unsigned long)gop.mfn);
            gop.status = GNTST_bad_page;
            goto copyback;
        }

        page = mfn_to_page(mfn);
        if ( unlikely(IS_XEN_HEAP_FRAME(page)) )
        { 
            DPRINTK("gnttab_transfer: xen frame %lx\n",
                    (unsigned long)gop.mfn);
            gop.status = GNTST_bad_page;
            goto copyback;
        }

        if ( steal_page(d, page, 0) < 0 )
        {
            gop.status = GNTST_bad_page;
            goto copyback;
        }

        /* Find the target domain. */
        if ( unlikely((e = find_domain_by_id(gop.domid)) == NULL) )
        {
            DPRINTK("gnttab_transfer: can't find domain %d\n", gop.domid);
            page->count_info &= ~(PGC_count_mask|PGC_allocated);
            free_domheap_page(page);
            gop.status = GNTST_bad_domain;
            goto copyback;
        }

        spin_lock(&e->page_alloc_lock);

        /*
         * Check that 'e' will accept the page and has reservation
         * headroom.  Also, a domain mustn't have PGC_allocated
         * pages when it is dying.
         */
        if ( unlikely(test_bit(_DOMF_dying, &e->domain_flags)) ||
             unlikely(e->tot_pages >= e->max_pages) ||
             unlikely(!gnttab_prepare_for_transfer(e, d, gop.ref)) )
        {
            if ( !test_bit(_DOMF_dying, &e->domain_flags) )
                DPRINTK("gnttab_transfer: Transferee has no reservation "
                        "headroom (%d,%d) or provided a bad grant ref (%08x) "
                        "or is dying (%lx)\n",
                        e->tot_pages, e->max_pages, gop.ref, e->domain_flags);
            spin_unlock(&e->page_alloc_lock);
            put_domain(e);
            page->count_info &= ~(PGC_count_mask|PGC_allocated);
            free_domheap_page(page);
            gop.status = GNTST_general_error;
            goto copyback;
        }

        /* Okay, add the page to 'e'. */
        if ( unlikely(e->tot_pages++ == 0) )
            get_knownalive_domain(e);
        list_add_tail(&page->list, &e->page_list);
        page_set_owner(page, e);

        spin_unlock(&e->page_alloc_lock);

        TRACE_1D(TRC_MEM_PAGE_GRANT_TRANSFER, e->domain_id);

        /* Tell the guest about its new page frame. */
        sha = &e->grant_table->shared[gop.ref];
        guest_physmap_add_page(e, sha->frame, mfn);
        sha->frame = mfn;
        wmb();
        sha->flags |= GTF_transfer_completed;

        put_domain(e);

        gop.status = GNTST_okay;

    copyback:
        if ( unlikely(__copy_to_guest_offset(uop, i, &gop, 1)) )
        {
            DPRINTK("gnttab_transfer: error writing resp %d/%d\n", i, count);
            return -EFAULT;
        }
    }

    return 0;
}

/* Undo __acquire_grant_for_copy.  Again, this has no effect on page
   type and reference counts. */
static void
__release_grant_for_copy(
    struct domain *rd, unsigned long gref, int readonly)
{
    grant_entry_t *const sha = &rd->grant_table->shared[gref];
    struct active_grant_entry *const act = &rd->grant_table->active[gref];
    const unsigned long r_frame = act->frame;

    if ( !readonly )
        gnttab_mark_dirty(rd, r_frame);

    spin_lock(&rd->grant_table->lock);

    if ( readonly )
    {
        act->pin -= GNTPIN_hstr_inc;
    }
    else
    {
        act->pin -= GNTPIN_hstw_inc;
        if ( !(act->pin & (GNTPIN_devw_mask|GNTPIN_hstw_mask)) )
            gnttab_clear_flag(_GTF_writing, &sha->flags);
    }

    if ( !act->pin )
        gnttab_clear_flag(_GTF_reading, &sha->flags);

    spin_unlock(&rd->grant_table->lock);
}

/* Grab a frame number from a grant entry and update the flags and pin
   count as appropriate.  Note that this does *not* update the page
   type or reference counts. */
static int
__acquire_grant_for_copy(
    struct domain *rd, unsigned long gref, int readonly,
    unsigned long *frame)
{
    grant_entry_t *sha;
    struct active_grant_entry *act;
    s16 rc = GNTST_okay;
    int retries = 0;
    union grant_combo scombo, prev_scombo, new_scombo;

    if ( unlikely(gref >= NR_GRANT_ENTRIES) )
        PIN_FAIL(error_out, GNTST_bad_gntref,
                 "Bad grant reference %ld\n", gref);
    
    act = &rd->grant_table->active[gref];
    sha = &rd->grant_table->shared[gref];

    spin_lock(&rd->grant_table->lock);
    
    /* If already pinned, check the active domid and avoid refcnt overflow. */
    if ( act->pin &&
         ((act->domid != current->domain->domain_id) ||
          (act->pin & 0x80808080U) != 0) )
        PIN_FAIL(unlock_out, GNTST_general_error,
                 "Bad domain (%d != %d), or risk of counter overflow %08x\n",
                 act->domid, current->domain->domain_id, act->pin);

    if ( !act->pin ||
         (!readonly && !(act->pin & (GNTPIN_devw_mask|GNTPIN_hstw_mask))) )
    {
        scombo.word = *(u32 *)&sha->flags;

        for ( ; ; )
        {
            /* If not already pinned, check the grant domid and type. */
            if ( !act->pin &&
                 (((scombo.shorts.flags & GTF_type_mask) !=
                   GTF_permit_access) ||
                  (scombo.shorts.domid != current->domain->domain_id)) )
                 PIN_FAIL(unlock_out, GNTST_general_error,
                          "Bad flags (%x) or dom (%d). (expected dom %d)\n",
                          scombo.shorts.flags, scombo.shorts.domid,
                          current->domain->domain_id);

            new_scombo = scombo;
            new_scombo.shorts.flags |= GTF_reading;

            if ( !readonly )
            {
                new_scombo.shorts.flags |= GTF_writing;
                if ( unlikely(scombo.shorts.flags & GTF_readonly) )
                    PIN_FAIL(unlock_out, GNTST_general_error,
                             "Attempt to write-pin a r/o grant entry.\n");
            }

            prev_scombo.word = cmpxchg((u32 *)&sha->flags,
                                       scombo.word, new_scombo.word);
            if ( likely(prev_scombo.word == scombo.word) )
                break;

            if ( retries++ == 4 )
                PIN_FAIL(unlock_out, GNTST_general_error,
                         "Shared grant entry is unstable.\n");

            scombo = prev_scombo;
        }

        if ( !act->pin )
        {
            act->domid = scombo.shorts.domid;
            act->frame = gmfn_to_mfn(rd, sha->frame);
        }
    }

    act->pin += readonly ? GNTPIN_hstr_inc : GNTPIN_hstw_inc;

    *frame = act->frame;

 unlock_out:
    spin_unlock(&rd->grant_table->lock);
 error_out:
    return rc;
}

static void
__gnttab_copy(
    struct gnttab_copy *op)
{
    struct domain *sd = NULL, *dd = NULL;
    unsigned long s_frame, d_frame;
    char *sp, *dp;
    s16 rc = GNTST_okay;
    int have_d_grant = 0, have_s_grant = 0, have_s_ref = 0;
    int src_is_gref, dest_is_gref;

    if ( ((op->source.offset + op->len) > PAGE_SIZE) ||
         ((op->dest.offset + op->len) > PAGE_SIZE) )
        PIN_FAIL(error_out, GNTST_bad_copy_arg, "copy beyond page area.\n");

    src_is_gref = op->flags & GNTCOPY_source_gref;
    dest_is_gref = op->flags & GNTCOPY_dest_gref;

    if ( (op->source.domid != DOMID_SELF && !src_is_gref ) ||
         (op->dest.domid   != DOMID_SELF && !dest_is_gref)   )
        PIN_FAIL(error_out, GNTST_permission_denied,
                 "only allow copy-by-mfn for DOMID_SELF.\n");

    if ( op->source.domid == DOMID_SELF )
    {
        sd = current->domain;
        get_knownalive_domain(sd);
    }
    else if ( (sd = find_domain_by_id(op->source.domid)) == NULL )
    {
        PIN_FAIL(error_out, GNTST_bad_domain,
                 "couldn't find %d\n", op->source.domid);
    }

    if ( op->dest.domid == DOMID_SELF )
    {
        dd = current->domain;
        get_knownalive_domain(dd);
    }
    else if ( (dd = find_domain_by_id(op->dest.domid)) == NULL )
    {
        PIN_FAIL(error_out, GNTST_bad_domain,
                 "couldn't find %d\n", op->dest.domid);
    }

    if ( src_is_gref )
    {
        rc = __acquire_grant_for_copy(sd, op->source.u.ref, 1, &s_frame);
        if ( rc != GNTST_okay )
            goto error_out;
        have_s_grant = 1;
    }
    else
    {
        s_frame = gmfn_to_mfn(sd, op->source.u.gmfn);
    }
    if ( !get_page(mfn_to_page(s_frame), sd) )
        PIN_FAIL(error_out, GNTST_general_error,
                 "could not get source frame %lx.\n", s_frame);
    have_s_ref = 1;

    if ( dest_is_gref )
    {
        rc = __acquire_grant_for_copy(dd, op->dest.u.ref, 0, &d_frame);
        if ( rc != GNTST_okay )
            goto error_out;
        have_d_grant = 1;
    }
    else
    {
        d_frame = gmfn_to_mfn(sd, op->dest.u.gmfn);
    }
    if ( !get_page_and_type(mfn_to_page(d_frame), dd, PGT_writable_page) )
        PIN_FAIL(error_out, GNTST_general_error,
                 "could not get source frame %lx.\n", d_frame);

    sp = map_domain_page(s_frame);
    dp = map_domain_page(d_frame);

    memcpy(dp + op->dest.offset, sp + op->source.offset, op->len);

    unmap_domain_page(dp);
    unmap_domain_page(sp);

    put_page_and_type(mfn_to_page(d_frame));
 error_out:
    if ( have_s_ref )
        put_page(mfn_to_page(s_frame));
    if ( have_s_grant )
        __release_grant_for_copy(sd, op->source.u.ref, 1);
    if ( have_d_grant )
        __release_grant_for_copy(dd, op->dest.u.ref, 0);
    if ( sd )
        put_domain(sd);
    if ( dd )
        put_domain(dd);
    op->status = rc;
}

static long
gnttab_copy(
    XEN_GUEST_HANDLE(gnttab_copy_t) uop, unsigned int count)
{
    int i;
    struct gnttab_copy op;

    for ( i = 0; i < count; i++ )
    {
        if ( unlikely(__copy_from_guest_offset(&op, uop, i, 1)) )
            return -EFAULT;
        __gnttab_copy(&op);
        if ( unlikely(__copy_to_guest_offset(uop, i, &op, 1)) )
            return -EFAULT;
    }
    return 0;
}

long
do_grant_table_op(
    unsigned int cmd, XEN_GUEST_HANDLE(void) uop, unsigned int count)
{
    long rc;
    struct domain *d = current->domain;
    
    if ( count > 512 )
        return -EINVAL;
    
    LOCK_BIGLOCK(d);
    
    rc = -EFAULT;
    switch ( cmd )
    {
    case GNTTABOP_map_grant_ref:
    {
        XEN_GUEST_HANDLE(gnttab_map_grant_ref_t) map =
            guest_handle_cast(uop, gnttab_map_grant_ref_t);
        if ( unlikely(!guest_handle_okay(map, count)) )
            goto out;
        rc = gnttab_map_grant_ref(map, count);
        break;
    }
    case GNTTABOP_unmap_grant_ref:
    {
        XEN_GUEST_HANDLE(gnttab_unmap_grant_ref_t) unmap =
            guest_handle_cast(uop, gnttab_unmap_grant_ref_t);
        if ( unlikely(!guest_handle_okay(unmap, count)) )
            goto out;
        rc = gnttab_unmap_grant_ref(unmap, count);
        break;
    }
    case GNTTABOP_setup_table:
    {
        rc = gnttab_setup_table(
            guest_handle_cast(uop, gnttab_setup_table_t), count);
        break;
    }
    case GNTTABOP_transfer:
    {
        XEN_GUEST_HANDLE(gnttab_transfer_t) transfer =
            guest_handle_cast(uop, gnttab_transfer_t);
        if ( unlikely(!guest_handle_okay(transfer, count)) )
            goto out;
        rc = gnttab_transfer(transfer, count);
        break;
    }
    case GNTTABOP_copy:
    {
        XEN_GUEST_HANDLE(gnttab_copy_t) copy =
            guest_handle_cast(uop, gnttab_copy_t);
        if ( unlikely(!guest_handle_okay(copy, count)) )
            goto out;
        rc = gnttab_copy(copy, count);
        break;
    }
    default:
        rc = -ENOSYS;
        break;
    }
    
  out:
    UNLOCK_BIGLOCK(d);
    
    return rc;
}

int 
grant_table_create(
    struct domain *d)
{
    struct grant_table *t;
    int                 i;

    BUG_ON(MAPTRACK_MAX_ENTRIES < NR_GRANT_ENTRIES);
    if ( (t = xmalloc(struct grant_table)) == NULL )
        goto no_mem;

    /* Simple stuff. */
    memset(t, 0, sizeof(*t));
    spin_lock_init(&t->lock);

    /* Active grant table. */
    t->active = xmalloc_array(struct active_grant_entry, NR_GRANT_ENTRIES);
    if ( t->active == NULL )
        goto no_mem;
    memset(t->active, 0, sizeof(struct active_grant_entry) * NR_GRANT_ENTRIES);

    /* Tracking of mapped foreign frames table */
    if ( (t->maptrack = alloc_xenheap_page()) == NULL )
        goto no_mem;
    t->maptrack_order = 0;
    t->maptrack_limit = PAGE_SIZE / sizeof(struct grant_mapping);
    memset(t->maptrack, 0, PAGE_SIZE);
    for ( i = 0; i < t->maptrack_limit; i++ )
        t->maptrack[i].ref = i+1;

    /* Shared grant table. */
    t->shared = alloc_xenheap_pages(ORDER_GRANT_FRAMES);
    if ( t->shared == NULL )
        goto no_mem;
    memset(t->shared, 0, NR_GRANT_FRAMES * PAGE_SIZE);

    for ( i = 0; i < NR_GRANT_FRAMES; i++ )
        gnttab_create_shared_page(d, t, i);

    /* Okay, install the structure. */
    wmb(); /* avoid races with lock-free access to d->grant_table */
    d->grant_table = t;
    return 0;

 no_mem:
    if ( t != NULL )
    {
        xfree(t->active);
        free_xenheap_page(t->maptrack);
        xfree(t);
    }
    return -ENOMEM;
}

void
gnttab_release_mappings(
    struct domain *d)
{
    struct grant_table   *gt = d->grant_table;
    struct grant_mapping *map;
    grant_ref_t           ref;
    grant_handle_t        handle;
    struct domain        *rd;
    struct active_grant_entry *act;
    struct grant_entry   *sha;

    BUG_ON(!test_bit(_DOMF_dying, &d->domain_flags));

    for ( handle = 0; handle < gt->maptrack_limit; handle++ )
    {
        map = &gt->maptrack[handle];
        if ( !(map->flags & (GNTMAP_device_map|GNTMAP_host_map)) )
            continue;

        ref = map->ref;

        DPRINTK("Grant release (%hu) ref:(%hu) flags:(%x) dom:(%hu)\n",
                handle, ref, map->flags, map->domid);

        rd = find_domain_by_id(map->domid);
        BUG_ON(rd == NULL);

        spin_lock(&rd->grant_table->lock);

        act = &rd->grant_table->active[ref];
        sha = &rd->grant_table->shared[ref];

        if ( map->flags & GNTMAP_readonly )
        {
            if ( map->flags & GNTMAP_device_map )
            {
                BUG_ON(!(act->pin & GNTPIN_devr_mask));
                act->pin -= GNTPIN_devr_inc;
                put_page(mfn_to_page(act->frame));
            }

            if ( map->flags & GNTMAP_host_map )
            {
                BUG_ON(!(act->pin & GNTPIN_hstr_mask));
                act->pin -= GNTPIN_hstr_inc;
                /* Done implicitly when page tables are destroyed. */
                /* put_page(mfn_to_page(act->frame)); */
            }
        }
        else
        {
            if ( map->flags & GNTMAP_device_map )
            {
                BUG_ON(!(act->pin & GNTPIN_devw_mask));
                act->pin -= GNTPIN_devw_inc;
                put_page_and_type(mfn_to_page(act->frame));
            }

            if ( map->flags & GNTMAP_host_map )
            {
                BUG_ON(!(act->pin & GNTPIN_hstw_mask));
                act->pin -= GNTPIN_hstw_inc;
                /* Done implicitly when page tables are destroyed. */
                /* put_page_and_type(mfn_to_page(act->frame)); */
            }

            if ( (act->pin & (GNTPIN_devw_mask|GNTPIN_hstw_mask)) == 0 )
                gnttab_clear_flag(_GTF_writing, &sha->flags);
        }

        if ( act->pin == 0 )
            gnttab_clear_flag(_GTF_reading, &sha->flags);

        spin_unlock(&rd->grant_table->lock);

        put_domain(rd);

        map->flags = 0;
    }
}


void
grant_table_destroy(
    struct domain *d)
{
    struct grant_table *t = d->grant_table;

    if ( t == NULL )
        return;
    
    free_xenheap_pages(t->shared, ORDER_GRANT_FRAMES);
    free_xenheap_page(t->maptrack);
    xfree(t->active);
    xfree(t);

    d->grant_table = NULL;
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
