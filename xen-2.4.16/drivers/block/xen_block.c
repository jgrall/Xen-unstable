/*
 * xen-block.c
 *
 * process incoming block io requests from guestos's.
 */

#include <xeno/config.h>
#include <xeno/types.h>
#include <xeno/lib.h>
#include <xeno/sched.h>
#include <xeno/blkdev.h>
#include <xeno/event.h>                                    /* mark_hyp_event */
#include <hypervisor-ifs/block.h>
#include <hypervisor-ifs/hypervisor-if.h>
#include <asm-i386/io.h>
#include <asm/spinlock.h>

#include <xeno/keyhandler.h>

#define XEN_BLK_DEBUG 0
#define XEN_BLK_DEBUG_LEVEL KERN_ALERT


/*
 * KAF XXX: the current state of play with blk_requests.
 * 
 * The following infrastructure is really here for future use.
 * blk_requests are currently not used by any mechanism, but eventually
 * pending blk_requests will go into an IO scheduler. This entry point
 * will go where we currently increment 'nr_pending'. The scheduler will
 * refuse admission of a blk_request if it is already full.
 */
typedef struct blk_request
{
  struct list_head queue;
  struct buffer_head *bh;
  blk_ring_entry_t request;
  struct task_struct *domain;                           /* requesting domain */
} blk_request_t;
#define MAX_PENDING_REQS 256                 /* very arbitrary */
static kmem_cache_t *blk_request_cachep;
static atomic_t nr_pending, nr_done;
static int pending_work;              /* which domains have work for us? */


/* some definitions */
void dumpx (char *buffer, int count);
void printx (char * string);
long do_block_io_op_domain (struct task_struct* task);
int dispatch_rw_block_io (int index);
int dispatch_probe_block_io (int index);
int dispatch_debug_block_io (int index);

/*
 * end_block_io_op
 *
 * IO has completed.  Need to notify the guest operating system.
 * Called from ll_rw_block -- currently /DIRECTLY/ -- XXX FIXME 
 * (e.g. hook into proper end processing of ll_rw) 
 */

void end_block_io_op(struct buffer_head * bh)
{
    unsigned long cpu_mask;
    /* struct list_head *list;*/
    blk_request_t *blk_request = NULL;
    unsigned long flags; /* irq save */
    struct task_struct *p;

    if (XEN_BLK_DEBUG)  
	printk(XEN_BLK_DEBUG_LEVEL "XEN end_block_io_op,  bh: %lx\n",
	       (unsigned long)bh);
    
    if ( (blk_request = (blk_request_t *)bh->b_xen_request) == NULL) 
        goto bad_interrupt;
    atomic_dec(&nr_pending);
    
    p = blk_request->domain;

    atomic_inc(&nr_done);
    spin_lock_irqsave(&p->io_done_queue_lock, flags);
    list_add_tail(&blk_request->queue, &p->io_done_queue);
    /* enqueue work for 'flush_blk_queue' handler */
    cpu_mask = mark_hyp_event(p, _HYP_EVENT_BLK_RX);
    spin_unlock_irqrestore(&p->io_done_queue_lock, flags);
    
    /* now kick the hypervisor */
    hyp_event_notify(cpu_mask); 
    return;

 bad_interrupt:
    printk (KERN_ALERT
            "   block io interrupt received for unknown buffer [0x%lx]\n",
            (unsigned long) bh);
    BUG();
    return;
}

/*
 * flush_blk_queue
 *
 * Called by the hypervisor synchronously when there is something to do
 * (block transfers have completed)
 */

void flush_blk_queue(void)
{
    struct task_struct *p = current;
    blk_request_t *blk_request;
    int position = 0;
    blk_ring_t *blk_ring;
    unsigned long flags;
    int loop;
    
    spin_lock_irqsave(&p->io_done_queue_lock, flags);
    clear_bit(_HYP_EVENT_BLK_RX, &p->hyp_events);
    
    while ( !list_empty(&p->io_done_queue) )
    {
	blk_request = list_entry(p->io_done_queue.next, blk_request_t, queue);
	list_del(&blk_request->queue);
	spin_unlock_irqrestore(&p->io_done_queue_lock, flags);
	atomic_dec(&nr_done);

	/* place on ring for guest os */ 
	blk_ring = p->blk_ring_base;
	position = blk_ring->brx_prod;

	if (XEN_BLK_DEBUG)  
	    printk(XEN_BLK_DEBUG_LEVEL "XEN flush_blk_queue [%d]\n", position);

	memcpy(&blk_ring->brx_ring[position], &blk_request->request,
	       sizeof(blk_ring_entry_t));
	blk_ring->brx_prod = BLK_RX_RING_INC(blk_ring->brx_prod);

	/* notify appropriate guest os */
	set_bit(_EVENT_BLK_RX, &p->shared_info->events);
	
	/* free the buffer header allocated in do_block_io_op */
	if ( blk_request->bh )
	    kfree(blk_request->bh); 

        kmem_cache_free(blk_request_cachep, blk_request);

	spin_lock_irqsave(&p->io_done_queue_lock, flags);
    }
    spin_unlock_irqrestore(&p->io_done_queue_lock, flags);


    /* XXX SMH: below is ugly and dangerous -- fix */
    /*
     * now check if there is any pending work from any domain
     * that we were previously unable to process.
     *
     * NOTE: the current algorithm will check _every_ domain
     * and wake up _every_ domain that has pending work.
     * In the future, we should stop waking up domains once
     * there isn't any space for their requests any more
     * ALSO, we need to maintain a counter of the last domain
     * that we woke up for fairness... we shouldn't restart
     * at domain 0 every time (although we might want to special
     * case domain 0);
     */
    for ( loop = 0; loop < XEN_BLOCK_MAX_DOMAINS; loop++ )
    {
	int domain = pending_work & (1 << loop);

	if ( domain ) 
        {
	    struct task_struct *mytask = current;

	    while ( mytask->domain != loop )
		mytask = mytask->next_task;

	    pending_work = pending_work & !(1 << loop);
	    do_block_io_op_domain(mytask);
	}
    }

    return; 
}


/*
 * do_block_io_op
 *
 * Accept a block io request from a guest operating system.
 * There is an entry in the hypervisor_call_table (xen/arch/i386/entry.S).
 */

long do_block_io_op (void)
{
    return do_block_io_op_domain(current);
}


/*
 * do_block_io_op
 *
 * handle the requests for a particular domain
 */
long do_block_io_op_domain (struct task_struct* task)
{
    blk_ring_t *blk_ring = task->blk_ring_base;
    int loop, status;

    if (XEN_BLK_DEBUG)  
	printk(XEN_BLK_DEBUG_LEVEL "XEN do_block_io_op %d %d\n",
	       blk_ring->btx_cons, blk_ring->btx_prod);

    for (loop = blk_ring->btx_cons; 
	 loop != blk_ring->btx_prod; 
	 loop = BLK_TX_RING_INC(loop)) {

	status = 1;

	switch (blk_ring->btx_ring[loop].operation) {

	case XEN_BLOCK_READ:
	case XEN_BLOCK_WRITE:
	    status = dispatch_rw_block_io(loop);
	    break;

	case XEN_BLOCK_PROBE:
	    status = dispatch_probe_block_io(loop);
	    break;

	case XEN_BLOCK_DEBUG:
	    status = dispatch_debug_block_io(loop);
	    break;

	default:
	    printk (KERN_ALERT "error: unknown block io operation [%d]\n",
		    blk_ring->btx_ring[loop].operation);
	    BUG();
	}


	if (status) {
	    /* 
	    ** Unable to successfully issue / complete command, maybe because
	    ** another resource (e.g. disk request buffers) is unavailable.
	    ** stop removing items from the communications ring and try later 
	    */
	    pending_work = pending_work | (1 << task->domain);
	    break;
	}
    }

    blk_ring->btx_cons = loop;
    return 0L;
}


int dispatch_debug_block_io (int index)
{
    printk (KERN_ALERT "dispatch_debug_block_io: UNIMPL\n"); 
    return 1; 
}

extern void ide_probe_devices(xen_disk_info_t *xdi);

int dispatch_probe_block_io (int index)
{
    blk_ring_t *blk_ring = current->blk_ring_base;
    xen_disk_info_t *xdi;
    
    xdi = phys_to_virt((unsigned long)blk_ring->btx_ring[index].buffer);
    
    ide_probe_devices(xdi);
    
    memcpy(&blk_ring->brx_ring[blk_ring->brx_prod], 
	   &blk_ring->btx_ring[index], 
	   sizeof(blk_ring_entry_t));
    blk_ring->brx_prod = BLK_RX_RING_INC(blk_ring->brx_prod);
    
    return 0;
}

extern void ll_rw_block(int rw, int nr, struct buffer_head * bhs[]); 

int dispatch_rw_block_io (int index)
{
    blk_ring_t *blk_ring = current->blk_ring_base;
    struct buffer_head *bh;
    struct request_queue *rq;
    int operation;
    blk_request_t *blk_request;
    
    /*
     * check to make sure that the block request seems at least
     * a bit legitimate
     */
    if ((blk_ring->btx_ring[index].block_size & (0x200 - 1)) != 0) {
	printk(KERN_ALERT "    error: dodgy block size: %d\n", 
	       blk_ring->btx_ring[index].block_size);
	BUG();
    }
    
    if(blk_ring->btx_ring[index].buffer == NULL) { 
	printk(KERN_ALERT "xen_block: bogus buffer from guestOS\n"); 
	BUG();
    }

    if (XEN_BLK_DEBUG) {
	printk(XEN_BLK_DEBUG_LEVEL "    btx_cons: %d  btx_prod %d  index: %d "
	       "op: %s, pri: %s\n", blk_ring->btx_cons, blk_ring->btx_prod, 
	       index, 
	       (blk_ring->btx_ring[index].operation == XEN_BLOCK_READ ? 
		"read" : "write"), 
	       (blk_ring->btx_ring[index].priority == XEN_BLOCK_SYNC ? 
		"sync" : "async"));
    }

    /* XXX KAF: A bit racey maybe? The whole wake-up pending needs fixing. */
    if ( atomic_read(&nr_pending) >= MAX_PENDING_REQS )
        return 1;
    atomic_inc(&nr_pending);
    blk_request = kmem_cache_alloc(blk_request_cachep, GFP_ATOMIC);

    /* we'll be doing this frequently, would a cache be appropriate? */
    /* free in flush_blk_queue */
    bh = (struct buffer_head *) kmalloc(sizeof(struct buffer_head), 
					GFP_KERNEL);
    if (!bh) {
	printk(KERN_ALERT "ERROR: bh is null\n");
	BUG();
    }

    /* set just the important bits of the buffer header */
    memset (bh, 0, sizeof (struct buffer_head));
    
    bh->b_blocknr       = blk_ring->btx_ring[index].block_number;
    bh->b_size          = blk_ring->btx_ring[index].block_size; 
    bh->b_dev           = blk_ring->btx_ring[index].device; 
    bh->b_rsector       = blk_ring->btx_ring[index].sector_number;
    bh->b_data          = phys_to_virt((unsigned long)
				       blk_ring->btx_ring[index].buffer);
    bh->b_count.counter = 1;
    bh->b_xen_request   = (void *)blk_request;  
    
    if (blk_ring->btx_ring[index].operation == XEN_BLOCK_WRITE) {
	bh->b_state = ((1 << BH_JBD) | (1 << BH_Mapped) | (1 << BH_Req) |
		       (1 << BH_Dirty) | (1 << BH_Uptodate));
	operation = WRITE;
    } else {
	bh->b_state = (1 << BH_Mapped);
	operation = READ;
    }

    /* save meta data about request XXX SMH: should copy_from_user() */
    memcpy(&blk_request->request,
	   &blk_ring->btx_ring[index], sizeof(blk_ring_entry_t));
    blk_request->bh     = bh;
    blk_request->domain = current; 
    
    /* dispatch single block request */
    ll_rw_block(operation, 1, &bh);       /* linux top half */
    rq = blk_get_queue(bh->b_rdev);                         
    generic_unplug_device(rq);            /* linux bottom half */

    return 0;
}


/*
 * debug dump_queue
 * arguments: queue head, name of queue
 */
void dump_queue(struct list_head *queue, char *name)
{
    struct list_head *list;
    int loop = 0;
    
    printk ("QUEUE %s %lx   n: %lx, p: %lx\n", name,  (unsigned long)queue,
	    (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each (list, queue) {
	printk ("  %s %d : %lx   n: %lx, p: %lx\n", name, loop++, 
		(unsigned long)list,
		(unsigned long)list->next, (unsigned long)list->prev);
    }
    return; 
}

void dump_queue_head(struct list_head *queue, char *name)
{
    struct list_head *list;
    int loop = 0;
    
    printk ("QUEUE %s %lx   n: %lx, p: %lx\n", name,  (unsigned long)queue,
	    (unsigned long) queue->next, (unsigned long) queue->prev);
    list_for_each (list, queue) {
	printk ("      %d : %lx   n: %lx, p: %lx\n", loop++, 
		(unsigned long)list,
		(unsigned long)list->next, (unsigned long)list->prev);
	if (loop >= 5) return;
    }
}


static void dump_blockq(u_char key, void *dev_id, struct pt_regs *regs) 
{
    printk("Dumping block queue stats:\n"); 
    printk("nr_pending = %d, nr_done = %d\n",
           atomic_read(&nr_pending), atomic_read(&nr_done));
}


/*
 * initialize_block_io
 *
 * initialize everything for block io called from 
 * arch/i386/setup.c::start_of_day
 */
void initialize_block_io ()
{
    blk_request_cachep = kmem_cache_create(
        "blk_request_cache", sizeof(blk_request_t),
        0, SLAB_HWCACHE_ALIGN, NULL, NULL);
    
    add_key_handler('b', dump_blockq, "dump xen ide blkdev stats"); 
    
    /* If bit i is true then domain i has work for us to do. */
    pending_work = 0;
}



