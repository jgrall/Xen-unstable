/*
 * xen_block.c
 *
 * process incoming block io requests from guestos's.
 */

#include <xeno/config.h>
#include <xeno/types.h>
#include <xeno/lib.h>
#include <xeno/sched.h>
#include <xeno/blkdev.h>
#include <xeno/event.h>
#include <hypervisor-ifs/block.h>
#include <hypervisor-ifs/hypervisor-if.h>
#include <asm-i386/io.h>
#include <asm/domain_page.h>
#include <xeno/spinlock.h>
#include <xeno/keyhandler.h>
#include <xeno/interrupt.h>
#include <xeno/vbd.h>
#include <xeno/slab.h>

#if 0
#define DPRINTK(_f, _a...) printk( _f , ## _a )
#else
#define DPRINTK(_f, _a...) ((void)0)
#endif

/*
 * These are rather arbitrary. They are fairly large because adjacent
 * requests pulled from a communication ring are quite likely to end
 * up being part of the same scatter/gather request at the disc.
 * 
 * ** TRY INCREASING 'MAX_PENDING_REQS' IF WRITE SPEEDS SEEM TOO LOW **
 * This will increase the chances of being able to write whole tracks.
 * '64' should be enough to keep us competitive with Linux.
 */
#define MAX_PENDING_REQS 64
#define BATCH_PER_DOMAIN 16

/*
 * Each outstanding request which we've passed to the lower device layers
 * has a 'pending_req' allocated to it. Each buffer_head that completes
 * decrements the pendcnt towards zero. When it hits zero, the specified
 * domain has a response queued for it, with the saved 'id' passed back.
 * 
 * We can't allocate pending_req's in order, since they may complete out
 * of order. We therefore maintain an allocation ring. This ring also 
 * indicates when enough work has been passed down -- at that point the
 * allocation ring will be empty.
 */
static pending_req_t pending_reqs[MAX_PENDING_REQS];
static unsigned char pending_ring[MAX_PENDING_REQS];
static unsigned int pending_prod, pending_cons;
static spinlock_t pend_prod_lock = SPIN_LOCK_UNLOCKED;
#define PENDREQ_IDX_INC(_i) ((_i) = ((_i)+1) & (MAX_PENDING_REQS-1))

static kmem_cache_t *buffer_head_cachep;
static atomic_t nr_pending;

static struct buffer_head *completed_bhs[NR_CPUS] __cacheline_aligned;

static int __buffer_is_valid(struct task_struct *p, 
                             unsigned long buffer, 
                             unsigned short size,
                             int writeable_buffer);
static void __lock_buffer(unsigned long buffer,
                          unsigned short size,
                          int writeable_buffer);
static void unlock_buffer(struct task_struct *p,
                          unsigned long buffer,
                          unsigned short size,
                          int writeable_buffer);

static void io_schedule(unsigned long unused);
static int do_block_io_op_domain(struct task_struct *p, int max_to_do);
static void dispatch_rw_block_io(struct task_struct *p, int index);
static void dispatch_debug_block_io(struct task_struct *p, int index);
static void make_response(struct task_struct *p, unsigned long id, 
                          unsigned short op, unsigned long st);


/******************************************************************
 * BLOCK-DEVICE SCHEDULER LIST MAINTENANCE
 */

static struct list_head io_schedule_list;
static spinlock_t io_schedule_list_lock;

static int __on_blkdev_list(struct task_struct *p)
{
    return p->blkdev_list.next != NULL;
}

static void remove_from_blkdev_list(struct task_struct *p)
{
    unsigned long flags;
    if ( !__on_blkdev_list(p) ) return;
    spin_lock_irqsave(&io_schedule_list_lock, flags);
    if ( __on_blkdev_list(p) )
    {
        list_del(&p->blkdev_list);
        p->blkdev_list.next = NULL;
        put_task_struct(p);
    }
    spin_unlock_irqrestore(&io_schedule_list_lock, flags);
}

static void add_to_blkdev_list_tail(struct task_struct *p)
{
    unsigned long flags;
    if ( __on_blkdev_list(p) ) return;
    spin_lock_irqsave(&io_schedule_list_lock, flags);
    if ( !__on_blkdev_list(p) )
    {
        list_add_tail(&p->blkdev_list, &io_schedule_list);
        get_task_struct(p);
    }
    spin_unlock_irqrestore(&io_schedule_list_lock, flags);
}


/******************************************************************
 * SCHEDULER FUNCTIONS
 */

static DECLARE_TASKLET(io_schedule_tasklet, io_schedule, 0);

static void io_schedule(unsigned long unused)
{
    struct task_struct *p;
    struct list_head *ent;

    /* Queue up a batch of requests. */
    while ( (atomic_read(&nr_pending) < MAX_PENDING_REQS) &&
            !list_empty(&io_schedule_list) )
    {
        ent = io_schedule_list.next;
        p = list_entry(ent, struct task_struct, blkdev_list);
        get_task_struct(p);
        remove_from_blkdev_list(p);
        if ( do_block_io_op_domain(p, BATCH_PER_DOMAIN) )
            add_to_blkdev_list_tail(p);
        put_task_struct(p);
    }

    /* Push the batch through to disc. */
    run_task_queue(&tq_disk);
}

static void maybe_trigger_io_schedule(void)
{
    /*
     * Needed so that two processes, who together make the following predicate
     * true, don't both read stale values and evaluate the predicate
     * incorrectly. Incredibly unlikely to stall the scheduler on x86, but...
     */
    smp_mb();

    if ( (atomic_read(&nr_pending) < (MAX_PENDING_REQS/2)) &&
         !list_empty(&io_schedule_list) )
    {
        tasklet_schedule(&io_schedule_tasklet);
    }
}



/******************************************************************
 * COMPLETION CALLBACK -- Called as bh->b_end_io()
 */

static void end_block_io_op_softirq(struct softirq_action *h)
{
    pending_req_t *pending_req;
    struct buffer_head *bh, *nbh;
    unsigned int cpu = smp_processor_id();

    local_irq_disable();
    bh = completed_bhs[cpu];
    completed_bhs[cpu] = NULL;
    local_irq_enable();

    while ( bh != NULL )
    {
        pending_req = bh->pending_req;
        
        unlock_buffer(pending_req->domain, 
                      virt_to_phys(bh->b_data), 
                      bh->b_size, 
                      (pending_req->operation==READ));
        
        if ( atomic_dec_and_test(&pending_req->pendcnt) )
        {
            make_response(pending_req->domain, pending_req->id,
                          pending_req->operation, pending_req->status);
            put_task_struct(pending_req->domain);
            spin_lock(&pend_prod_lock);
            pending_ring[pending_prod] = pending_req - pending_reqs;
            PENDREQ_IDX_INC(pending_prod);
            spin_unlock(&pend_prod_lock);
            atomic_dec(&nr_pending);
            maybe_trigger_io_schedule();
        }
        
        nbh = bh->b_reqnext;
        kmem_cache_free(buffer_head_cachep, bh);
        bh = nbh;
    }
}

static void end_block_io_op(struct buffer_head *bh, int uptodate)
{
    unsigned long flags;
    unsigned int cpu = smp_processor_id();

    /* An error fails the entire request. */
    if ( !uptodate )
    {
        DPRINTK("Buffer not up-to-date at end of operation\n");
        bh->pending_req->status = 2;
    }

    local_irq_save(flags);
    bh->b_reqnext = completed_bhs[cpu];
    completed_bhs[cpu] = bh;
    local_irq_restore(flags);

    __cpu_raise_softirq(cpu, BLKDEV_RESPONSE_SOFTIRQ);
}


/* ----[ Syscall Interface ]------------------------------------------------*/

long do_block_io_op(block_io_op_t *u_block_io_op)
{
    long ret = 0;
    block_io_op_t op; 
    struct task_struct *p = current;

    if ( copy_from_user(&op, u_block_io_op, sizeof(op)) )
        return -EFAULT;

    switch ( op.cmd )
    {
    case BLOCK_IO_OP_SIGNAL: 
	/* simply indicates there're reqs outstanding => add current to list */
	add_to_blkdev_list_tail(p);
	maybe_trigger_io_schedule();
	break; 

    case BLOCK_IO_OP_RESET:
        /* Avoid a race with the tasklet. */
        remove_from_blkdev_list(p);
        if ( p->blk_req_cons != p->blk_resp_prod )
        {
            /* Interface isn't quiescent. */
            ret = -EINVAL;
        }
        else
        {
            p->blk_req_cons = p->blk_resp_prod = 0;
            ret = 0;
        }
        break;

    case BLOCK_IO_OP_RING_ADDRESS:
        op.u.ring_mfn = virt_to_phys(p->blk_ring_base) >> PAGE_SHIFT;
        ret = copy_to_user(u_block_io_op, &op, sizeof(op)) ? -EFAULT : 0;
        break;

    case BLOCK_IO_OP_VBD_CREATE:  
	/* create a new VBD */
	ret = vbd_create(&op.u.create_params); 
	break; 

    case BLOCK_IO_OP_VBD_ADD:  
	/* add an extent to a VBD */
	ret = vbd_add(&op.u.add_params); 
	break; 

    case BLOCK_IO_OP_VBD_REMOVE:  
	/* remove an extent from a VBD */
	ret = vbd_remove(&op.u.remove_params); 
	break; 

    case BLOCK_IO_OP_VBD_DELETE:  
	/* delete a VBD */
	ret = vbd_delete(&op.u.delete_params); 
	break; 

    case BLOCK_IO_OP_VBD_PROBE: 
	/* query VBD information for self or others (or all) */
	ret = vbd_probe(&op.u.probe_params); 
	if(ret == 0)
	    copy_to_user(u_block_io_op, &op, sizeof(op)); 
	break; 

    case BLOCK_IO_OP_VBD_INFO: 
	/* query information about a particular VBD */
	ret = vbd_info(&op.u.info_params); 
	if(ret == 0)
	    copy_to_user(u_block_io_op, &op, sizeof(op)); 
	break; 

    default: 
	ret = -ENOSYS; 
    } 

    return ret;
}



/******************************************************************
 * DOWNWARD CALLS -- These interface with the block-device layer proper.
 */

static int __buffer_is_valid(struct task_struct *p, 
                             unsigned long buffer, 
                             unsigned short size,
                             int writeable_buffer)
{
    unsigned long    pfn;
    struct pfn_info *page;
    int rc = 0;

    /* A request may span multiple page frames. Each must be checked. */
    for ( pfn = buffer >> PAGE_SHIFT; 
          pfn < ((buffer + size + PAGE_SIZE - 1) >> PAGE_SHIFT);
          pfn++ )
    {
        /* Each frame must be within bounds of machine memory. */
        if ( pfn >= max_page )
        {
            DPRINTK("pfn out of range: %08lx\n", pfn);
            goto out;
        }

        page = frame_table + pfn;

        /* Each frame must belong to the requesting domain. */
        if ( (page->flags & PG_domain_mask) != p->domain )
        {
            DPRINTK("bad domain: expected %d, got %ld\n", 
                    p->domain, page->flags & PG_domain_mask);
            goto out;
        }

        /* If reading into the frame, the frame must be writeable. */
        if ( writeable_buffer &&
             ((page->flags & PG_type_mask) != PGT_writeable_page) &&
             (page->type_count != 0) )
        {
            DPRINTK("non-writeable page passed for block read\n");
            goto out;
        }
    }    

    rc = 1;
 out:
    return rc;
}

static void __lock_buffer(unsigned long buffer,
                          unsigned short size,
                          int writeable_buffer)
{
    unsigned long    pfn;
    struct pfn_info *page;

    for ( pfn = buffer >> PAGE_SHIFT; 
          pfn < ((buffer + size + PAGE_SIZE - 1) >> PAGE_SHIFT);
          pfn++ )
    {
        page = frame_table + pfn;
        if ( writeable_buffer )
        {
            if ( page->type_count == 0 )
            {
                page->flags &= ~PG_type_mask;
                /* No need for PG_need_flush here. */
                page->flags |= PGT_writeable_page;
            }
            get_page_type(page);
        }
        get_page_tot(page);
    }
}

static void unlock_buffer(struct task_struct *p,
                          unsigned long buffer,
                          unsigned short size,
                          int writeable_buffer)
{
    unsigned long    pfn;
    struct pfn_info *page;

    spin_lock(&p->page_lock);
    for ( pfn = buffer >> PAGE_SHIFT; 
          pfn < ((buffer + size + PAGE_SIZE - 1) >> PAGE_SHIFT);
          pfn++ )
    {
        page = frame_table + pfn;
        if ( writeable_buffer )
            put_page_type(page);
        put_page_tot(page);
    }
    spin_unlock(&p->page_lock);
}

static int do_block_io_op_domain(struct task_struct *p, int max_to_do)
{
    blk_ring_t *blk_ring = p->blk_ring_base;
    int i, more_to_do = 0;

    /*
     * Take items off the comms ring, taking care not to catch up
     * with the response-producer index.
     */
    for ( i = p->blk_req_cons; 
	  (i != blk_ring->req_prod) &&
              (((p->blk_resp_prod-i) & (BLK_RING_SIZE-1)) != 1); 
	  i = BLK_RING_INC(i) ) 
    {
        if ( (max_to_do-- == 0) || 
             (atomic_read(&nr_pending) == MAX_PENDING_REQS) )
        {
            more_to_do = 1;
            break;
        }
        
	switch ( blk_ring->ring[i].req.operation )
        {
	case XEN_BLOCK_READ:
	case XEN_BLOCK_WRITE:
	    dispatch_rw_block_io(p, i);
	    break;

	case XEN_BLOCK_DEBUG:
	    dispatch_debug_block_io(p, i);
	    break;

	default:
            DPRINTK("error: unknown block io operation [%d]\n",
                    blk_ring->ring[i].req.operation);
            make_response(p, blk_ring->ring[i].req.id, 
                          blk_ring->ring[i].req.operation, 1);
            break;
	}
    }

    p->blk_req_cons = i;
    return more_to_do;
}

static void dispatch_debug_block_io(struct task_struct *p, int index)
{
    DPRINTK("dispatch_debug_block_io: unimplemented\n"); 
}

static void dispatch_rw_block_io(struct task_struct *p, int index)
{
    extern void ll_rw_block(int rw, int nr, struct buffer_head * bhs[]); 
    blk_ring_t *blk_ring = p->blk_ring_base;
    blk_ring_req_entry_t *req = &blk_ring->ring[index].req;
    struct buffer_head *bh;
    int operation = (req->operation == XEN_BLOCK_WRITE) ? WRITE : READ;
    unsigned short nr_sects;
    unsigned long buffer;
    int i, tot_sects;
    pending_req_t *pending_req;

    /* We map virtual scatter/gather segments to physical segments. */
    int new_segs, nr_psegs = 0;
    phys_seg_t phys_seg[MAX_BLK_SEGS * 2];

    spin_lock(&p->page_lock);

    /* Check that number of segments is sane. */
    if ( (req->nr_segments == 0) || (req->nr_segments > MAX_BLK_SEGS) )
    {
        DPRINTK("Bad number of segments in request (%d)\n", req->nr_segments);
        goto bad_descriptor;
    }

    /*
     * Check each address/size pair is sane, and convert into a
     * physical device and block offset. Note that if the offset and size
     * crosses a virtual extent boundary, we may end up with more
     * physical scatter/gather segments than virtual segments.
     */
    for ( i = tot_sects = 0; i < req->nr_segments; i++, tot_sects += nr_sects )
    {
        buffer   = req->buffer_and_sects[i] & ~0x1FF;
        nr_sects = req->buffer_and_sects[i] &  0x1FF;

        if ( nr_sects == 0 )
        {
            DPRINTK("zero-sized data request\n");
            goto bad_descriptor;
        }

        if ( !__buffer_is_valid(p, buffer, nr_sects<<9, (operation==READ)) )
	{
            DPRINTK("invalid buffer\n");
            goto bad_descriptor;
	}

	phys_seg[nr_psegs].dev           = req->device;
	phys_seg[nr_psegs].sector_number = req->sector_number + tot_sects;
	phys_seg[nr_psegs].buffer        = buffer;
	phys_seg[nr_psegs].nr_sects      = nr_sects;

        /* Translate the request into the relevant 'physical device' */
	new_segs = vbd_translate(&phys_seg[nr_psegs], p, operation); 

	/* If it fails we bail (unless the caller is privileged). */
	if ( new_segs < 0 )
        { 
            if ( unlikely(new_segs != -ENODEV) || unlikely(!IS_PRIV(p)) )
            {
                DPRINTK("access denied: %s of [%ld,%ld] on dev=%04x\n", 
                        operation == READ ? "read" : "write", 
                        req->sector_number + tot_sects, 
                        req->sector_number + tot_sects + nr_sects, 
                        req->device); 
                goto bad_descriptor;
            }

	    /*
             * XXX Clear any 'partition' info in device. This works because IDE
             * ignores the partition bits anyway. Only SCSI needs this hack,
             * and it has four bits to clear.
             */
	    phys_seg[nr_psegs].dev = req->device & 0xFFF0;
            new_segs = 1;
        }
	 
        nr_psegs += new_segs;
        if ( nr_psegs >= (MAX_BLK_SEGS*2) ) BUG();
    }

    /* Lock pages associated with each buffer head. */
    for ( i = 0; i < nr_psegs; i++ )
        __lock_buffer(phys_seg[i].buffer, phys_seg[i].nr_sects<<9, 
                      (operation==READ));
    spin_unlock(&p->page_lock);

    atomic_inc(&nr_pending);
    pending_req = pending_reqs + pending_ring[pending_cons];
    PENDREQ_IDX_INC(pending_cons);
    pending_req->domain    = p;
    pending_req->id        = req->id;
    pending_req->operation = operation;
    pending_req->status    = 0;
    atomic_set(&pending_req->pendcnt, nr_psegs);

    get_task_struct(p);

    /* Now we pass each segment down to the real blkdev layer. */
    for ( i = 0; i < nr_psegs; i++ )
    {
        bh = kmem_cache_alloc(buffer_head_cachep, GFP_KERNEL);
        if ( bh == NULL ) panic("bh is null\n");
        memset (bh, 0, sizeof (struct buffer_head));
    
        bh->b_size          = phys_seg[i].nr_sects << 9;
        bh->b_dev           = phys_seg[i].dev;
        bh->b_rsector       = phys_seg[i].sector_number;
        bh->b_data          = phys_to_virt(phys_seg[i].buffer);
        bh->b_end_io        = end_block_io_op;
        bh->pending_req     = pending_req;

        if ( operation == WRITE )
        {
            bh->b_state = (1 << BH_JBD) | (1 << BH_Mapped) | (1 << BH_Req) |
                (1 << BH_Dirty) | (1 << BH_Uptodate) | (1 << BH_Write);
        } 
        else
        {
            bh->b_state = (1 << BH_Mapped) | (1 << BH_Read);
        }

        /* Dispatch a single request. We'll flush it to disc later. */
        ll_rw_block(operation, 1, &bh);
    }

    return;

 bad_descriptor:
    spin_unlock(&p->page_lock);
    make_response(p, req->id, req->operation, 1);
} 



/******************************************************************
 * MISCELLANEOUS SETUP / TEARDOWN / DEBUGGING
 */


static void make_response(struct task_struct *p, unsigned long id, 
			  unsigned short op, unsigned long st)
{
    unsigned long cpu_mask;
    int position;
    blk_ring_t *blk_ring;

    /* Place on the response ring for the relevant domain. */ 
    spin_lock(&p->blk_ring_lock);
    blk_ring = p->blk_ring_base;
    position = p->blk_resp_prod;
    blk_ring->ring[position].resp.id        = id;
    blk_ring->ring[position].resp.operation = op;
    blk_ring->ring[position].resp.status    = st;
    p->blk_resp_prod = blk_ring->resp_prod = BLK_RING_INC(position);
    spin_unlock(&p->blk_ring_lock);
    
    /* Kick the relevant domain. */
    cpu_mask = mark_guest_event(p, _EVENT_BLKDEV);
    guest_event_notify(cpu_mask); 
}

static void dump_blockq(u_char key, void *dev_id, struct pt_regs *regs) 
{
    unsigned long flags;
    struct task_struct *p;
    blk_ring_t *blk_ring ;
    int i;

    printk("Dumping block queue stats: nr_pending = %d (prod=%d,cons=%d)\n",
           atomic_read(&nr_pending), pending_prod, pending_cons);

    read_lock_irqsave(&tasklist_lock, flags);
    p = &idle0_task;
    do {
        if ( !is_idle_task(p) )
        {
            printk("Domain: %d\n", p->domain);
            blk_ring = p->blk_ring_base;
            
            printk("  req_prod:%d, req_cons:%d resp_prod:%d/%d on_list=%d\n",
                   blk_ring->req_prod, p->blk_req_cons,
                   blk_ring->resp_prod, p->blk_resp_prod,
                   __on_blkdev_list(p));
        }
        p = p->next_task;
    } 
    while ( (p = p->next_task) != &idle0_task );
    read_unlock_irqrestore(&tasklist_lock, flags);

    for ( i = 0; i < MAX_PENDING_REQS; i++ )
    {
        printk("Pend%d: dom=%p, id=%08lx, cnt=%d, op=%d, status=%d\n",
               i, pending_reqs[i].domain, pending_reqs[i].id,
               atomic_read(&pending_reqs[i].pendcnt), 
               pending_reqs[i].operation, pending_reqs[i].status);
    }
}

/* Start-of-day initialisation for a new domain. */
void init_blkdev_info(struct task_struct *p)
{
    if ( sizeof(*p->blk_ring_base) > PAGE_SIZE ) BUG();
    p->blk_ring_base = (blk_ring_t *)get_free_page(GFP_KERNEL);
    clear_page(p->blk_ring_base);
    SHARE_PFN_WITH_DOMAIN(virt_to_page(p->blk_ring_base), p->domain);
    p->blkdev_list.next = NULL;
    spin_lock_init(&p->vbd_lock);
}

/* End-of-day teardown for a domain. */
void destroy_blkdev_info(struct task_struct *p)
{
    ASSERT(!__on_blkdev_list(p));
    UNSHARE_PFN(virt_to_page(p->blk_ring_base));
    free_page((unsigned long)p->blk_ring_base);
    destroy_all_vbds(p);
}

void unlink_blkdev_info(struct task_struct *p)
{
    unsigned long flags;

    spin_lock_irqsave(&io_schedule_list_lock, flags);
    if ( __on_blkdev_list(p) )
    {
        list_del(&p->blkdev_list);
        p->blkdev_list.next = (void *)0xdeadbeef; /* prevent reinsertion */
        put_task_struct(p);
    }
    spin_unlock_irqrestore(&io_schedule_list_lock, flags);
}

void initialize_block_io ()
{
    int i;

    atomic_set(&nr_pending, 0);
    pending_prod = pending_cons = 0;
    memset(pending_reqs, 0, sizeof(pending_reqs));
    for ( i = 0; i < MAX_PENDING_REQS; i++ )
        pending_ring[i] = i;
    
    for ( i = 0; i < NR_CPUS; i++ )
        completed_bhs[i] = NULL;
        
    open_softirq(BLKDEV_RESPONSE_SOFTIRQ, end_block_io_op_softirq, NULL);

    spin_lock_init(&io_schedule_list_lock);
    INIT_LIST_HEAD(&io_schedule_list);

    buffer_head_cachep = kmem_cache_create(
        "buffer_head_cache", sizeof(struct buffer_head),
        0, SLAB_HWCACHE_ALIGN, NULL, NULL);

    add_key_handler('b', dump_blockq, "dump xen ide blkdev statistics");
}
