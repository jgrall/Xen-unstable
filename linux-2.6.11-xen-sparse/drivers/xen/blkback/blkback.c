/******************************************************************************
 * arch/xen/drivers/blkif/backend/main.c
 * 
 * Back-end of the driver for virtual block devices. This portion of the
 * driver exports a 'unified' block-device interface that can be accessed
 * by any operating system that implements a compatible front end. A 
 * reference front-end implementation can be found in:
 *  arch/xen/drivers/blkif/frontend
 * 
 * Copyright (c) 2003-2004, Keir Fraser & Steve Hand
 * Copyright (c) 2005, Christopher Clark
 */

#include "common.h"
#include <asm-xen/evtchn.h>
#ifdef CONFIG_XEN_BLKDEV_GRANT
#include <asm-xen/xen-public/grant_table.h>
#endif

/*
 * These are rather arbitrary. They are fairly large because adjacent requests
 * pulled from a communication ring are quite likely to end up being part of
 * the same scatter/gather request at the disc.
 * 
 * ** TRY INCREASING 'MAX_PENDING_REQS' IF WRITE SPEEDS SEEM TOO LOW **
 * This will increase the chances of being able to write whole tracks.
 * 64 should be enough to keep us competitive with Linux.
 */
#define MAX_PENDING_REQS 64
#define BATCH_PER_DOMAIN 16

static unsigned long mmap_vstart;
#define MMAP_PAGES                                              \
    (MAX_PENDING_REQS * BLKIF_MAX_SEGMENTS_PER_REQUEST)
#define MMAP_VADDR(_req,_seg)                                   \
    (mmap_vstart +                                              \
     ((_req) * BLKIF_MAX_SEGMENTS_PER_REQUEST * PAGE_SIZE) +    \
     ((_seg) * PAGE_SIZE))

/*
 * Each outstanding request that we've passed to the lower device layers has a 
 * 'pending_req' allocated to it. Each buffer_head that completes decrements 
 * the pendcnt towards zero. When it hits zero, the specified domain has a 
 * response queued for it, with the saved 'id' passed back.
 */
typedef struct {
    blkif_t       *blkif;
    unsigned long  id;
    int            nr_pages;
    atomic_t       pendcnt;
    unsigned short operation;
    int            status;
} pending_req_t;

/*
 * We can't allocate pending_req's in order, since they may complete out of 
 * order. We therefore maintain an allocation ring. This ring also indicates 
 * when enough work has been passed down -- at that point the allocation ring 
 * will be empty.
 */
static pending_req_t pending_reqs[MAX_PENDING_REQS];
static unsigned char pending_ring[MAX_PENDING_REQS];
static spinlock_t pend_prod_lock = SPIN_LOCK_UNLOCKED;
/* NB. We use a different index type to differentiate from shared blk rings. */
typedef unsigned int PEND_RING_IDX;
#define MASK_PEND_IDX(_i) ((_i)&(MAX_PENDING_REQS-1))
static PEND_RING_IDX pending_prod, pending_cons;
#define NR_PENDING_REQS (MAX_PENDING_REQS - pending_prod + pending_cons)

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
static kmem_cache_t *buffer_head_cachep;
#else
static request_queue_t *plugged_queue;
static inline void flush_plugged_queue(void)
{
    request_queue_t *q = plugged_queue;
    if ( q != NULL )
    {
        if ( q->unplug_fn != NULL )
            q->unplug_fn(q);
        blk_put_queue(q);
        plugged_queue = NULL;
    }
}
#endif

#ifdef CONFIG_XEN_BLKDEV_GRANT
/* When using grant tables to map a frame for device access then the
 * handle returned must be used to unmap the frame. This is needed to
 * drop the ref count on the frame.
 */
static u16 pending_grant_handles[MMAP_PAGES];
#define pending_handle(_idx, _i) \
    (pending_grant_handles[((_idx) * BLKIF_MAX_SEGMENTS_PER_REQUEST) + (_i)])
#define BLKBACK_INVALID_HANDLE (0xFFFF)
#endif

#ifdef CONFIG_XEN_BLKDEV_TAP_BE
/*
 * If the tap driver is used, we may get pages belonging to either the tap
 * or (more likely) the real frontend.  The backend must specify which domain
 * a given page belongs to in update_va_mapping though.  For the moment, 
 * the tap rewrites the ID field of the request to contain the request index
 * and the id of the real front end domain.
 */
#define BLKTAP_COOKIE 0xbeadfeed
static inline domid_t ID_TO_DOM(unsigned long id) { return (id >> 16); }
#endif

static int do_block_io_op(blkif_t *blkif, int max_to_do);
static void dispatch_probe(blkif_t *blkif, blkif_request_t *req);
static void dispatch_rw_block_io(blkif_t *blkif, blkif_request_t *req);
static void make_response(blkif_t *blkif, unsigned long id, 
                          unsigned short op, int st);

static void fast_flush_area(int idx, int nr_pages)
{
#ifdef CONFIG_XEN_BLKDEV_GRANT
    struct gnttab_unmap_grant_ref unmap[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    unsigned int i, invcount = 0;
    u16 handle;

    for ( i = 0; i < nr_pages; i++ )
    {
        if ( BLKBACK_INVALID_HANDLE != ( handle = pending_handle(idx, i) ) )
        {
            unmap[i].host_virt_addr = MMAP_VADDR(idx, i);
            unmap[i].dev_bus_addr   = 0;
            unmap[i].handle         = handle;
            pending_handle(idx, i) = BLKBACK_INVALID_HANDLE;
            invcount++;
        }
    }
    if ( unlikely(HYPERVISOR_grant_table_op(
                    GNTTABOP_unmap_grant_ref, unmap, invcount)))
        BUG();
#else

    multicall_entry_t mcl[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    int               i;

    for ( i = 0; i < nr_pages; i++ )
    {
	MULTI_update_va_mapping(mcl+i, MMAP_VADDR(idx, i),
				__pte(0), 0);
    }

    mcl[nr_pages-1].args[2] = UVMF_TLB_FLUSH|UVMF_ALL;
    if ( unlikely(HYPERVISOR_multicall(mcl, nr_pages) != 0) )
        BUG();
#endif
}


/******************************************************************
 * BLOCK-DEVICE SCHEDULER LIST MAINTENANCE
 */

static struct list_head blkio_schedule_list;
static spinlock_t blkio_schedule_list_lock;

static int __on_blkdev_list(blkif_t *blkif)
{
    return blkif->blkdev_list.next != NULL;
}

static void remove_from_blkdev_list(blkif_t *blkif)
{
    unsigned long flags;
    if ( !__on_blkdev_list(blkif) ) return;
    spin_lock_irqsave(&blkio_schedule_list_lock, flags);
    if ( __on_blkdev_list(blkif) )
    {
        list_del(&blkif->blkdev_list);
        blkif->blkdev_list.next = NULL;
        blkif_put(blkif);
    }
    spin_unlock_irqrestore(&blkio_schedule_list_lock, flags);
}

static void add_to_blkdev_list_tail(blkif_t *blkif)
{
    unsigned long flags;
    if ( __on_blkdev_list(blkif) ) return;
    spin_lock_irqsave(&blkio_schedule_list_lock, flags);
    if ( !__on_blkdev_list(blkif) && (blkif->status == CONNECTED) )
    {
        list_add_tail(&blkif->blkdev_list, &blkio_schedule_list);
        blkif_get(blkif);
    }
    spin_unlock_irqrestore(&blkio_schedule_list_lock, flags);
}


/******************************************************************
 * SCHEDULER FUNCTIONS
 */

static DECLARE_WAIT_QUEUE_HEAD(blkio_schedule_wait);

static int blkio_schedule(void *arg)
{
    DECLARE_WAITQUEUE(wq, current);

    blkif_t          *blkif;
    struct list_head *ent;

    daemonize(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
        "xenblkd"
#endif
        );

    for ( ; ; )
    {
        /* Wait for work to do. */
        add_wait_queue(&blkio_schedule_wait, &wq);
        set_current_state(TASK_INTERRUPTIBLE);
        if ( (NR_PENDING_REQS == MAX_PENDING_REQS) || 
             list_empty(&blkio_schedule_list) )
            schedule();
        __set_current_state(TASK_RUNNING);
        remove_wait_queue(&blkio_schedule_wait, &wq);

        /* Queue up a batch of requests. */
        while ( (NR_PENDING_REQS < MAX_PENDING_REQS) &&
                !list_empty(&blkio_schedule_list) )
        {
            ent = blkio_schedule_list.next;
            blkif = list_entry(ent, blkif_t, blkdev_list);
            blkif_get(blkif);
            remove_from_blkdev_list(blkif);
            if ( do_block_io_op(blkif, BATCH_PER_DOMAIN) )
                add_to_blkdev_list_tail(blkif);
            blkif_put(blkif);
        }

        /* Push the batch through to disc. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
        run_task_queue(&tq_disk);
#else
        flush_plugged_queue();
#endif
    }
}

static void maybe_trigger_blkio_schedule(void)
{
    /*
     * Needed so that two processes, who together make the following predicate
     * true, don't both read stale values and evaluate the predicate
     * incorrectly. Incredibly unlikely to stall the scheduler on x86, but...
     */
    smp_mb();

    if ( (NR_PENDING_REQS < (MAX_PENDING_REQS/2)) &&
         !list_empty(&blkio_schedule_list) )
        wake_up(&blkio_schedule_wait);
}



/******************************************************************
 * COMPLETION CALLBACK -- Called as bh->b_end_io()
 */

static void __end_block_io_op(pending_req_t *pending_req, int uptodate)
{
    unsigned long flags;

    /* An error fails the entire request. */
    if ( !uptodate )
    {
        DPRINTK("Buffer not up-to-date at end of operation\n");
        pending_req->status = BLKIF_RSP_ERROR;
    }

    if ( atomic_dec_and_test(&pending_req->pendcnt) )
    {
        int pending_idx = pending_req - pending_reqs;
        fast_flush_area(pending_idx, pending_req->nr_pages);
        make_response(pending_req->blkif, pending_req->id,
                      pending_req->operation, pending_req->status);
        blkif_put(pending_req->blkif);
        spin_lock_irqsave(&pend_prod_lock, flags);
        pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;
        spin_unlock_irqrestore(&pend_prod_lock, flags);
        maybe_trigger_blkio_schedule();
    }
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
static void end_block_io_op(struct buffer_head *bh, int uptodate)
{
    __end_block_io_op(bh->b_private, uptodate);
    kmem_cache_free(buffer_head_cachep, bh);
}
#else
static int end_block_io_op(struct bio *bio, unsigned int done, int error)
{
    if ( bio->bi_size != 0 )
        return 1;
    __end_block_io_op(bio->bi_private, !error);
    bio_put(bio);
    return error;
}
#endif


/******************************************************************************
 * NOTIFICATION FROM GUEST OS.
 */

irqreturn_t blkif_be_int(int irq, void *dev_id, struct pt_regs *regs)
{
    blkif_t *blkif = dev_id;
    add_to_blkdev_list_tail(blkif);
    maybe_trigger_blkio_schedule();
    return IRQ_HANDLED;
}



/******************************************************************
 * DOWNWARD CALLS -- These interface with the block-device layer proper.
 */

static int do_block_io_op(blkif_t *blkif, int max_to_do)
{
    blkif_back_ring_t *blk_ring = &blkif->blk_ring;
    blkif_request_t *req;
    RING_IDX i, rp;
    int more_to_do = 0;

    rp = blk_ring->sring->req_prod;
    rmb(); /* Ensure we see queued requests up to 'rp'. */

    for ( i = blk_ring->req_cons; 
         (i != rp) && !RING_REQUEST_CONS_OVERFLOW(blk_ring, i);
          i++ )
    {
        if ( (max_to_do-- == 0) || (NR_PENDING_REQS == MAX_PENDING_REQS) )
        {
            more_to_do = 1;
            break;
        }
        
        req = RING_GET_REQUEST(blk_ring, i);
        switch ( req->operation )
        {
        case BLKIF_OP_READ:
        case BLKIF_OP_WRITE:
            dispatch_rw_block_io(blkif, req);
            break;

        case BLKIF_OP_PROBE:
            dispatch_probe(blkif, req);
            break;

        default:
            DPRINTK("error: unknown block io operation [%d]\n",
                    req->operation);
            make_response(blkif, req->id, req->operation, BLKIF_RSP_ERROR);
            break;
        }
    }

    blk_ring->req_cons = i;
    return more_to_do;
}

static void dispatch_probe(blkif_t *blkif, blkif_request_t *req)
{
    int rsp = BLKIF_RSP_ERROR;
    int pending_idx = pending_ring[MASK_PEND_IDX(pending_cons)];

    /* We expect one buffer only. */
    if ( unlikely(req->nr_segments != 1) )
        goto out;

    /* Make sure the buffer is page-sized. */
    if ( (blkif_first_sect(req->frame_and_sects[0]) != 0) ||
         (blkif_last_sect(req->frame_and_sects[0]) != 7) )
        goto out;

#ifdef CONFIG_XEN_BLKDEV_GRANT
    {
        struct gnttab_map_grant_ref map;

        map.host_virt_addr = MMAP_VADDR(pending_idx, 0);
        map.flags = GNTMAP_host_map;
        map.ref = blkif_gref_from_fas(req->frame_and_sects[0]);
        map.dom = blkif->domid;

        if ( unlikely(HYPERVISOR_grant_table_op(
                        GNTTABOP_map_grant_ref, &map, 1)))
            BUG();

        if ( map.handle < 0 )
            goto out;

        pending_handle(pending_idx, 0) = map.handle;
    }
#else /* else CONFIG_XEN_BLKDEV_GRANT */

#ifdef CONFIG_XEN_BLKDEV_TAP_BE
    /* Grab the real frontend out of the probe message. */
    if (req->frame_and_sects[1] == BLKTAP_COOKIE) 
        blkif->is_blktap = 1;
#endif


#ifdef CONFIG_XEN_BLKDEV_TAP_BE
    if ( HYPERVISOR_update_va_mapping_otherdomain(
        MMAP_VADDR(pending_idx, 0),
        (pte_t) { (req->frame_and_sects[0] & PAGE_MASK) | __PAGE_KERNEL },
        0, (blkif->is_blktap ? ID_TO_DOM(req->id) : blkif->domid) ) )
        
        goto out;
#else
    if ( HYPERVISOR_update_va_mapping_otherdomain(
        MMAP_VADDR(pending_idx, 0),
        (pte_t) { (req->frame_and_sects[0] & PAGE_MASK) | __PAGE_KERNEL },
        0, blkif->domid) ) 
        
        goto out;
#endif
#endif /* endif CONFIG_XEN_BLKDEV_GRANT */
   
    rsp = vbd_probe(blkif, (vdisk_t *)MMAP_VADDR(pending_idx, 0), 
                    PAGE_SIZE / sizeof(vdisk_t));

 out:
    fast_flush_area(pending_idx, 1);
    make_response(blkif, req->id, req->operation, rsp);
}

static void dispatch_rw_block_io(blkif_t *blkif, blkif_request_t *req)
{
    extern void ll_rw_block(int rw, int nr, struct buffer_head * bhs[]); 
    int operation = (req->operation == BLKIF_OP_WRITE) ? WRITE : READ;
    unsigned long fas = 0;
    int i, pending_idx = pending_ring[MASK_PEND_IDX(pending_cons)];
    pending_req_t *pending_req;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    struct gnttab_map_grant_ref map[BLKIF_MAX_SEGMENTS_PER_REQUEST];
#else
    unsigned long remap_prot;
    multicall_entry_t mcl[BLKIF_MAX_SEGMENTS_PER_REQUEST];
#endif
    struct phys_req preq;
    struct { 
        unsigned long buf; unsigned int nsec;
    } seg[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    unsigned int nseg;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    struct buffer_head *bh;
#else
    struct bio *bio = NULL, *biolist[BLKIF_MAX_SEGMENTS_PER_REQUEST];
    int nbio = 0;
    request_queue_t *q;
#endif

    /* Check that number of segments is sane. */
    nseg = req->nr_segments;
    if ( unlikely(nseg == 0) || 
         unlikely(nseg > BLKIF_MAX_SEGMENTS_PER_REQUEST) )
    {
        DPRINTK("Bad number of segments in request (%d)\n", nseg);
        goto bad_descriptor;
    }

    preq.dev           = req->device;
    preq.sector_number = req->sector_number;
    preq.nr_sects      = 0;

#ifdef CONFIG_XEN_BLKDEV_GRANT
    for ( i = 0; i < nseg; i++ )
    {
        fas         = req->frame_and_sects[i];
        seg[i].nsec = blkif_last_sect(fas) - blkif_first_sect(fas) + 1;

        if ( seg[i].nsec <= 0 )
            goto bad_descriptor;
        preq.nr_sects += seg[i].nsec;

        map[i].host_virt_addr = MMAP_VADDR(pending_idx, i);
        map[i].dom = blkif->domid;
        map[i].ref = blkif_gref_from_fas(fas);
        map[i].flags = GNTMAP_host_map;
        if ( operation == WRITE )
            map[i].flags |= GNTMAP_readonly;
    }

    if ( unlikely(HYPERVISOR_grant_table_op(
                    GNTTABOP_map_grant_ref, map, nseg)))
        BUG();

    for ( i = 0; i < nseg; i++ )
    {
        if ( unlikely(map[i].handle < 0) )
        {
            DPRINTK("invalid buffer -- could not remap it\n");
            fast_flush_area(pending_idx, nseg);
            goto bad_descriptor;
        }

        phys_to_machine_mapping[__pa(MMAP_VADDR(pending_idx, i))>>PAGE_SHIFT] =
            FOREIGN_FRAME(map[i].dev_bus_addr);

        pending_handle(pending_idx, i) = map[i].handle;
    }
#endif

    for ( i = 0; i < nseg; i++ )
    {
        fas         = req->frame_and_sects[i];
#ifdef CONFIG_XEN_BLKDEV_GRANT
        seg[i].buf  = (map[i].dev_bus_addr << PAGE_SHIFT) |
                      (blkif_first_sect(fas) << 9);
#else
        seg[i].buf  = (fas & PAGE_MASK) | (blkif_first_sect(fas) << 9);
        seg[i].nsec = blkif_last_sect(fas) - blkif_first_sect(fas) + 1;
        if ( seg[i].nsec <= 0 )
            goto bad_descriptor;
        preq.nr_sects += seg[i].nsec;
#endif
    }

    if ( vbd_translate(&preq, blkif, operation) != 0 )
    {
        DPRINTK("access denied: %s of [%llu,%llu] on dev=%04x\n", 
                operation == READ ? "read" : "write", preq.sector_number,
                preq.sector_number + preq.nr_sects, preq.dev); 
        goto bad_descriptor;
    }

#ifndef CONFIG_XEN_BLKDEV_GRANT
    if ( operation == READ )
        remap_prot = _PAGE_PRESENT|_PAGE_DIRTY|_PAGE_ACCESSED|_PAGE_RW;
    else
        remap_prot = _PAGE_PRESENT|_PAGE_DIRTY|_PAGE_ACCESSED;

    for ( i = 0; i < nseg; i++ )
    {
	MULTI_update_va_mapping_otherdomain(
	    mcl+i, MMAP_VADDR(pending_idx, i),
	    pfn_pte_ma(seg[i].buf >> PAGE_SHIFT, remap_prot),
	    0, blkif->domid);
#ifdef CONFIG_XEN_BLKDEV_TAP_BE
        if ( blkif->is_blktap )
            mcl[i].args[3] = ID_TO_DOM(req->id);
#endif
        phys_to_machine_mapping[__pa(MMAP_VADDR(pending_idx, i))>>PAGE_SHIFT] =
            FOREIGN_FRAME(seg[i].buf >> PAGE_SHIFT);
    }

    BUG_ON(HYPERVISOR_multicall(mcl, nseg) != 0);

    for ( i = 0; i < nseg; i++ )
    {
        if ( unlikely(mcl[i].result != 0) )
        {
            DPRINTK("invalid buffer -- could not remap it\n");
            fast_flush_area(pending_idx, nseg);
            goto bad_descriptor;
        }
    }
#endif /* end ifndef CONFIG_XEN_BLKDEV_GRANT */

    pending_req = &pending_reqs[pending_idx];
    pending_req->blkif     = blkif;
    pending_req->id        = req->id;
    pending_req->operation = operation;
    pending_req->status    = BLKIF_RSP_OKAY;
    pending_req->nr_pages  = nseg;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)

    atomic_set(&pending_req->pendcnt, nseg);
    pending_cons++;
    blkif_get(blkif);

    for ( i = 0; i < nseg; i++ )
    {
        bh = kmem_cache_alloc(buffer_head_cachep, GFP_KERNEL);
        if ( unlikely(bh == NULL) )
        {
            __end_block_io_op(pending_req, 0);
            continue;
        }

        memset(bh, 0, sizeof (struct buffer_head));

        init_waitqueue_head(&bh->b_wait);
        bh->b_size          = seg[i].nsec << 9;
        bh->b_dev           = preq.dev;
        bh->b_rdev          = preq.dev;
        bh->b_rsector       = (unsigned long)preq.sector_number;
        bh->b_data          = (char *)MMAP_VADDR(pending_idx, i) +
            (seg[i].buf & ~PAGE_MASK);
        bh->b_page          = virt_to_page(MMAP_VADDR(pending_idx, i));
        bh->b_end_io        = end_block_io_op;
        bh->b_private       = pending_req;

        bh->b_state = (1 << BH_Mapped) | (1 << BH_Lock) | 
            (1 << BH_Req) | (1 << BH_Launder);
        if ( operation == WRITE )
            bh->b_state |= (1 << BH_JBD) | (1 << BH_Req) | (1 << BH_Uptodate);

        atomic_set(&bh->b_count, 1);

        /* Dispatch a single request. We'll flush it to disc later. */
        generic_make_request(operation, bh);

        preq.sector_number += seg[i].nsec;
    }

#else

    for ( i = 0; i < nseg; i++ )
    {
        if ( ((int)preq.sector_number|(int)seg[i].nsec) &
             ((bdev_hardsect_size(preq.bdev) >> 9) - 1) )
        {
            DPRINTK("Misaligned I/O request from domain %d", blkif->domid);
            goto cleanup_and_fail;
        }

        while ( (bio == NULL) ||
                (bio_add_page(bio,
                              virt_to_page(MMAP_VADDR(pending_idx, i)),
                              seg[i].nsec << 9,
                              seg[i].buf & ~PAGE_MASK) == 0) )
        {
            bio = biolist[nbio++] = bio_alloc(GFP_KERNEL, nseg-i);
            if ( unlikely(bio == NULL) )
            {
            cleanup_and_fail:
                for ( i = 0; i < (nbio-1); i++ )
                    bio_put(biolist[i]);
                fast_flush_area(pending_idx, nseg);
                goto bad_descriptor;
            }
                
            bio->bi_bdev    = preq.bdev;
            bio->bi_private = pending_req;
            bio->bi_end_io  = end_block_io_op;
            bio->bi_sector  = preq.sector_number;
        }

        preq.sector_number += seg[i].nsec;
    }

    if ( (q = bdev_get_queue(bio->bi_bdev)) != plugged_queue )
    {
        flush_plugged_queue();
        blk_get_queue(q);
        plugged_queue = q;
    }

    atomic_set(&pending_req->pendcnt, nbio);
    pending_cons++;
    blkif_get(blkif);

    for ( i = 0; i < nbio; i++ )
        submit_bio(operation, biolist[i]);

#endif

    return;

 bad_descriptor:
    make_response(blkif, req->id, req->operation, BLKIF_RSP_ERROR);
} 



/******************************************************************
 * MISCELLANEOUS SETUP / TEARDOWN / DEBUGGING
 */


static void make_response(blkif_t *blkif, unsigned long id, 
                          unsigned short op, int st)
{
    blkif_response_t *resp;
    unsigned long     flags;
    blkif_back_ring_t *blk_ring = &blkif->blk_ring;

    /* Place on the response ring for the relevant domain. */ 
    spin_lock_irqsave(&blkif->blk_ring_lock, flags);
    resp = RING_GET_RESPONSE(blk_ring, blk_ring->rsp_prod_pvt);
    resp->id        = id;
    resp->operation = op;
    resp->status    = st;
    wmb(); /* Ensure other side can see the response fields. */
    blk_ring->rsp_prod_pvt++;
    RING_PUSH_RESPONSES(blk_ring);
    spin_unlock_irqrestore(&blkif->blk_ring_lock, flags);

    /* Kick the relevant domain. */
    notify_via_evtchn(blkif->evtchn);
}

void blkif_deschedule(blkif_t *blkif)
{
    remove_from_blkdev_list(blkif);
}

static int __init blkif_init(void)
{
    int i;

    if ( !(xen_start_info.flags & SIF_INITDOMAIN) &&
         !(xen_start_info.flags & SIF_BLK_BE_DOMAIN) )
        return 0;

    blkif_interface_init();

    if ( (mmap_vstart = allocate_empty_lowmem_region(MMAP_PAGES)) == 0 )
        BUG();

    pending_cons = 0;
    pending_prod = MAX_PENDING_REQS;
    memset(pending_reqs, 0, sizeof(pending_reqs));
    for ( i = 0; i < MAX_PENDING_REQS; i++ )
        pending_ring[i] = i;
    
    spin_lock_init(&blkio_schedule_list_lock);
    INIT_LIST_HEAD(&blkio_schedule_list);

    if ( kernel_thread(blkio_schedule, 0, CLONE_FS | CLONE_FILES) < 0 )
        BUG();

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
    buffer_head_cachep = kmem_cache_create(
        "buffer_head_cache", sizeof(struct buffer_head),
        0, SLAB_HWCACHE_ALIGN, NULL, NULL);
#endif

    blkif_ctrlif_init();
    
#ifdef CONFIG_XEN_BLKDEV_GRANT
    memset( pending_grant_handles,  BLKBACK_INVALID_HANDLE, MMAP_PAGES );
    printk(KERN_ALERT "Blkif backend is using grant tables.\n");
#endif

#ifdef CONFIG_XEN_BLKDEV_TAP_BE
    printk(KERN_ALERT "NOTE: Blkif backend is running with tap support on!\n");
#endif

    return 0;
}

__initcall(blkif_init);
