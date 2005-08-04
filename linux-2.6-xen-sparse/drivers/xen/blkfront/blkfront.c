/******************************************************************************
 * blkfront.c
 * 
 * XenLinux virtual block-device driver.
 * 
 * Copyright (c) 2003-2004, Keir Fraser & Steve Hand
 * Modifications by Mark A. Williamson are (c) Intel Research Cambridge
 * Copyright (c) 2004, Christian Limpach
 * Copyright (c) 2004, Andrew Warfield
 * Copyright (c) 2005, Christopher Clark
 * 
 * This file may be distributed separately from the Linux kernel, or
 * incorporated into other software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#if 1
#define ASSERT(_p) \
    if ( !(_p) ) { printk("Assertion '%s' failed, line %d, file %s", #_p , \
    __LINE__, __FILE__); *(int*)0=0; }
#else
#define ASSERT(_p)
#endif

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#include "block.h"
#else
#include "common.h"
#include <linux/blk.h>
#include <linux/tqueue.h>
#endif

#include <linux/cdrom.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <scsi/scsi.h>
#include <asm-xen/ctrl_if.h>
#include <asm-xen/evtchn.h>
#ifdef CONFIG_XEN_BLKDEV_GRANT
#include <asm-xen/xen-public/grant_table.h>
#include <asm-xen/gnttab.h>
#endif

typedef unsigned char byte; /* from linux/ide.h */

/* Control whether runtime update of vbds is enabled. */
#define ENABLE_VBD_UPDATE 1

#if ENABLE_VBD_UPDATE
static void vbd_update(void);
#else
static void vbd_update(void){};
#endif

#define BLKIF_STATE_CLOSED       0
#define BLKIF_STATE_DISCONNECTED 1
#define BLKIF_STATE_CONNECTED    2

static int blkif_handle = 0;
static unsigned int blkif_state = BLKIF_STATE_CLOSED;
static unsigned int blkif_evtchn = 0;

static int blkif_control_rsp_valid;
static blkif_response_t blkif_control_rsp;

static blkif_front_ring_t blk_ring;

#define BLK_RING_SIZE __RING_SIZE((blkif_sring_t *)0, PAGE_SIZE)

#ifdef CONFIG_XEN_BLKDEV_GRANT
static domid_t rdomid = 0;
static grant_ref_t gref_head, gref_terminal;
#define MAXIMUM_OUTSTANDING_BLOCK_REQS \
    (BLKIF_MAX_SEGMENTS_PER_REQUEST * BLKIF_RING_SIZE)
#define GRANTREF_INVALID (1<<15)
#endif

static struct blk_shadow {
    blkif_request_t req;
    unsigned long request;
    unsigned long frame[BLKIF_MAX_SEGMENTS_PER_REQUEST];
} blk_shadow[BLK_RING_SIZE];
unsigned long blk_shadow_free;

static int recovery = 0; /* Recovery in progress: protected by blkif_io_lock */

static void kick_pending_request_queues(void);

int __init xlblk_init(void);

static void blkif_completion(struct blk_shadow *s);

static inline int GET_ID_FROM_FREELIST(void)
{
    unsigned long free = blk_shadow_free;
    BUG_ON(free > BLK_RING_SIZE);
    blk_shadow_free = blk_shadow[free].req.id;
    blk_shadow[free].req.id = 0x0fffffee; /* debug */
    return free;
}

static inline void ADD_ID_TO_FREELIST(unsigned long id)
{
    blk_shadow[id].req.id  = blk_shadow_free;
    blk_shadow[id].request = 0;
    blk_shadow_free = id;
}


/************************  COMMON CODE  (inlined)  ************************/

/* Kernel-specific definitions used in the common code */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#define DISABLE_SCATTERGATHER() 
#else
static int sg_operation = -1;
#define DISABLE_SCATTERGATHER() (sg_operation = -1)
#endif

static inline void pickle_request(struct blk_shadow *s, blkif_request_t *r)
{
#ifndef CONFIG_XEN_BLKDEV_GRANT
    int i;
#endif

    s->req = *r;

#ifndef CONFIG_XEN_BLKDEV_GRANT
    for ( i = 0; i < r->nr_segments; i++ )
        s->req.frame_and_sects[i] = machine_to_phys(r->frame_and_sects[i]);
#endif
}

static inline void unpickle_request(blkif_request_t *r, struct blk_shadow *s)
{
#ifndef CONFIG_XEN_BLKDEV_GRANT
    int i;
#endif

    *r = s->req;

#ifndef CONFIG_XEN_BLKDEV_GRANT
    for ( i = 0; i < s->req.nr_segments; i++ )
        r->frame_and_sects[i] = phys_to_machine(s->req.frame_and_sects[i]);
#endif
}


static inline void flush_requests(void)
{
    DISABLE_SCATTERGATHER();
    RING_PUSH_REQUESTS(&blk_ring);
    notify_via_evtchn(blkif_evtchn);
}


/**************************  KERNEL VERSION 2.6  **************************/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)

module_init(xlblk_init);

#if ENABLE_VBD_UPDATE
static void update_vbds_task(void *unused)
{ 
    xlvbd_update_vbds();
}

static void vbd_update(void)
{
    static DECLARE_WORK(update_tq, update_vbds_task, NULL);
    schedule_work(&update_tq);
}
#endif /* ENABLE_VBD_UPDATE */

static struct xlbd_disk_info *head_waiting = NULL;
static void kick_pending_request_queues(void)
{
    struct xlbd_disk_info *di;
    while ( ((di = head_waiting) != NULL) && !RING_FULL(&blk_ring) )
    {
        head_waiting = di->next_waiting;
        di->next_waiting = NULL;
        /* Re-enable calldowns. */
        blk_start_queue(di->rq);
        /* Kick things off immediately. */
        do_blkif_request(di->rq);
    }
}

int blkif_open(struct inode *inode, struct file *filep)
{
    struct gendisk *gd = inode->i_bdev->bd_disk;
    struct xlbd_disk_info *di = (struct xlbd_disk_info *)gd->private_data;

    /* Update of usage count is protected by per-device semaphore. */
    di->mi->usage++;
    
    return 0;
}


int blkif_release(struct inode *inode, struct file *filep)
{
    struct gendisk *gd = inode->i_bdev->bd_disk;
    struct xlbd_disk_info *di = (struct xlbd_disk_info *)gd->private_data;

    /*
     * When usage drops to zero it may allow more VBD updates to occur.
     * Update of usage count is protected by a per-device semaphore.
     */
    if ( --di->mi->usage == 0 )
        vbd_update();

    return 0;
}


int blkif_ioctl(struct inode *inode, struct file *filep,
                unsigned command, unsigned long argument)
{
    int i;

    DPRINTK_IOCTL("command: 0x%x, argument: 0x%lx, dev: 0x%04x\n",
                  command, (long)argument, inode->i_rdev); 
  
    switch ( command )
    {
    case HDIO_GETGEO:
        /* return ENOSYS to use defaults */
        return -ENOSYS;

    case CDROMMULTISESSION:
        DPRINTK("FIXME: support multisession CDs later\n");
        for ( i = 0; i < sizeof(struct cdrom_multisession); i++ )
            if ( put_user(0, (byte *)(argument + i)) ) return -EFAULT;
        return 0;

    default:
        /*printk(KERN_ALERT "ioctl %08x not supported by Xen blkdev\n",
          command);*/
        return -EINVAL; /* same return as native Linux */
    }

    return 0;
}


/*
 * blkif_queue_request
 *
 * request block io 
 * 
 * id: for guest use only.
 * operation: BLKIF_OP_{READ,WRITE,PROBE}
 * buffer: buffer to read/write into. this should be a
 *   virtual address in the guest os.
 */
static int blkif_queue_request(struct request *req)
{
    struct xlbd_disk_info *di = req->rq_disk->private_data;
    unsigned long buffer_ma;
    blkif_request_t *ring_req;
    struct bio *bio;
    struct bio_vec *bvec;
    int idx;
    unsigned long id;
    unsigned int fsect, lsect;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    int ref;
#endif

    if ( unlikely(blkif_state != BLKIF_STATE_CONNECTED) )
        return 1;

    /* Fill out a communications ring structure. */
    ring_req = RING_GET_REQUEST(&blk_ring, blk_ring.req_prod_pvt);
    id = GET_ID_FROM_FREELIST();
    blk_shadow[id].request = (unsigned long)req;

    ring_req->id = id;
    ring_req->operation = rq_data_dir(req) ? BLKIF_OP_WRITE :
        BLKIF_OP_READ;
    ring_req->sector_number = (blkif_sector_t)req->sector;
    ring_req->device = di->xd_device;

    ring_req->nr_segments = 0;
    rq_for_each_bio(bio, req)
    {
        bio_for_each_segment(bvec, bio, idx)
        {
            if ( ring_req->nr_segments == BLKIF_MAX_SEGMENTS_PER_REQUEST )
                BUG();
            buffer_ma = page_to_phys(bvec->bv_page);
            fsect = bvec->bv_offset >> 9;
            lsect = fsect + (bvec->bv_len >> 9) - 1;
#ifdef CONFIG_XEN_BLKDEV_GRANT
            /* install a grant reference. */
            ref = gnttab_claim_grant_reference(&gref_head, gref_terminal);
            ASSERT( ref != -ENOSPC );

            gnttab_grant_foreign_access_ref(
                        ref,
                        rdomid,
                        buffer_ma >> PAGE_SHIFT,
                        rq_data_dir(req) );

            blk_shadow[id].frame[ring_req->nr_segments] =
                buffer_ma >> PAGE_SHIFT;

            ring_req->frame_and_sects[ring_req->nr_segments++] =
                blkif_fas_from_gref(ref, fsect, lsect);

#else
            ring_req->frame_and_sects[ring_req->nr_segments++] =
                blkif_fas(buffer_ma, fsect, lsect);
#endif
        }
    }

    blk_ring.req_prod_pvt++;
    
    /* Keep a private copy so we can reissue requests when recovering. */
    pickle_request(&blk_shadow[id], ring_req);

    return 0;
}


/*
 * do_blkif_request
 *  read a block; request is in a request queue
 */
void do_blkif_request(request_queue_t *rq)
{
    struct xlbd_disk_info *di;
    struct request *req;
    int queued;

    DPRINTK("Entered do_blkif_request\n"); 

    queued = 0;

    while ( (req = elv_next_request(rq)) != NULL )
    {
        if ( !blk_fs_request(req) )
        {
            end_request(req, 0);
            continue;
        }

        if ( RING_FULL(&blk_ring) )
            goto wait;

        DPRINTK("do_blk_req %p: cmd %p, sec %lx, (%u/%li) buffer:%p [%s]\n",
                req, req->cmd, req->sector, req->current_nr_sectors,
                req->nr_sectors, req->buffer,
                rq_data_dir(req) ? "write" : "read");

        blkdev_dequeue_request(req);
        if ( blkif_queue_request(req) )
        {
        wait:
            di = req->rq_disk->private_data;
            if ( di->next_waiting == NULL )
            {
                di->next_waiting = head_waiting;
                head_waiting = di;
                /* Avoid pointless unplugs. */
                blk_stop_queue(rq);
            }
            break;
        }

        queued++;
    }

    if ( queued != 0 )
        flush_requests();
}


static irqreturn_t blkif_int(int irq, void *dev_id, struct pt_regs *ptregs)
{
    struct request *req;
    blkif_response_t *bret;
    RING_IDX i, rp;
    unsigned long flags; 
    
    spin_lock_irqsave(&blkif_io_lock, flags);     

    if ( unlikely(blkif_state == BLKIF_STATE_CLOSED) || 
         unlikely(recovery) )
    {
        spin_unlock_irqrestore(&blkif_io_lock, flags);
        return IRQ_HANDLED;
    }
    
    rp = blk_ring.sring->rsp_prod;
    rmb(); /* Ensure we see queued responses up to 'rp'. */

    for ( i = blk_ring.rsp_cons; i != rp; i++ )
    {
        unsigned long id;

        bret = RING_GET_RESPONSE(&blk_ring, i);
        id   = bret->id;
        req  = (struct request *)blk_shadow[id].request;

        blkif_completion(&blk_shadow[id]);

        ADD_ID_TO_FREELIST(id);

        switch ( bret->operation )
        {
        case BLKIF_OP_READ:
        case BLKIF_OP_WRITE:
            if ( unlikely(bret->status != BLKIF_RSP_OKAY) )
                DPRINTK("Bad return from blkdev data request: %x\n",
                        bret->status);

            if ( unlikely(end_that_request_first
                          (req, 
                           (bret->status == BLKIF_RSP_OKAY),
                           req->hard_nr_sectors)) )
                BUG();
            end_that_request_last(req);

            break;
        case BLKIF_OP_PROBE:
            memcpy(&blkif_control_rsp, bret, sizeof(*bret));
            blkif_control_rsp_valid = 1;
            break;
        default:
            BUG();
        }
    }

    blk_ring.rsp_cons = i;

    kick_pending_request_queues();

    spin_unlock_irqrestore(&blkif_io_lock, flags);

    return IRQ_HANDLED;
}

#else
/**************************  KERNEL VERSION 2.4  **************************/

static kdev_t        sg_dev;
static unsigned long sg_next_sect;

/*
 * Request queues with outstanding work, but ring is currently full.
 * We need no special lock here, as we always access this with the
 * blkif_io_lock held. We only need a small maximum list.
 */
#define MAX_PENDING 8
static request_queue_t *pending_queues[MAX_PENDING];
static int nr_pending;


#define blkif_io_lock io_request_lock

/*============================================================================*/
#if ENABLE_VBD_UPDATE

/*
 * blkif_update_int/update-vbds_task - handle VBD update events.
 *  Schedule a task for keventd to run, which will update the VBDs and perform 
 *  the corresponding updates to our view of VBD state.
 */
static void update_vbds_task(void *unused)
{ 
    xlvbd_update_vbds();
}

static void vbd_update(void)
{
    static struct tq_struct update_tq;
    update_tq.routine = update_vbds_task;
    schedule_task(&update_tq);
}

#endif /* ENABLE_VBD_UPDATE */
/*============================================================================*/

static void kick_pending_request_queues(void)
{
    /* We kick pending request queues if the ring is reasonably empty. */
    if ( (nr_pending != 0) && 
         (RING_PENDING_REQUESTS(&blk_ring) < (BLK_RING_SIZE >> 1)) )
    {
        /* Attempt to drain the queue, but bail if the ring becomes full. */
        while ( (nr_pending != 0) && !RING_FULL(&blk_ring) )
            do_blkif_request(pending_queues[--nr_pending]);
    }
}

int blkif_open(struct inode *inode, struct file *filep)
{
    short xldev = inode->i_rdev; 
    struct gendisk *gd = get_gendisk(xldev);
    xl_disk_t *disk = xldev_to_xldisk(inode->i_rdev);
    short minor = MINOR(xldev); 

    if ( gd->part[minor].nr_sects == 0 )
    { 
        /*
         * Device either doesn't exist, or has zero capacity; we use a few
         * cheesy heuristics to return the relevant error code
         */
        if ( (gd->sizes[minor >> gd->minor_shift] != 0) ||
             ((minor & (gd->max_p - 1)) != 0) )
        { 
            /*
             * We have a real device, but no such partition, or we just have a
             * partition number so guess this is the problem.
             */
            return -ENXIO;     /* no such device or address */
        }
        else if ( gd->flags[minor >> gd->minor_shift] & GENHD_FL_REMOVABLE )
        {
            /* This is a removable device => assume that media is missing. */ 
            return -ENOMEDIUM; /* media not present (this is a guess) */
        } 
        else
        { 
            /* Just go for the general 'no such device' error. */
            return -ENODEV;    /* no such device */
        }
    }
    
    /* Update of usage count is protected by per-device semaphore. */
    disk->usage++;

    return 0;
}


int blkif_release(struct inode *inode, struct file *filep)
{
    xl_disk_t *disk = xldev_to_xldisk(inode->i_rdev);

    /*
     * When usage drops to zero it may allow more VBD updates to occur.
     * Update of usage count is protected by a per-device semaphore.
     */
    if ( --disk->usage == 0 ) {
        vbd_update();
    }

    return 0;
}


int blkif_ioctl(struct inode *inode, struct file *filep,
                unsigned command, unsigned long argument)
{
    kdev_t dev = inode->i_rdev;
    struct hd_geometry *geo = (struct hd_geometry *)argument;
    struct gendisk *gd;     
    struct hd_struct *part; 
    int i;
    unsigned short cylinders;
    byte heads, sectors;

    /* NB. No need to check permissions. That is done for us. */
    
    DPRINTK_IOCTL("command: 0x%x, argument: 0x%lx, dev: 0x%04x\n",
                  command, (long) argument, dev); 
  
    gd = get_gendisk(dev);
    part = &gd->part[MINOR(dev)]; 

    switch ( command )
    {
    case BLKGETSIZE:
        DPRINTK_IOCTL("   BLKGETSIZE: %x %lx\n", BLKGETSIZE, part->nr_sects); 
        return put_user(part->nr_sects, (unsigned long *) argument);

    case BLKGETSIZE64:
        DPRINTK_IOCTL("   BLKGETSIZE64: %x %llx\n", BLKGETSIZE64,
                      (u64)part->nr_sects * 512);
        return put_user((u64)part->nr_sects * 512, (u64 *) argument);

    case BLKRRPART:                               /* re-read partition table */
        DPRINTK_IOCTL("   BLKRRPART: %x\n", BLKRRPART);
        return blkif_revalidate(dev);

    case BLKSSZGET:
        return hardsect_size[MAJOR(dev)][MINOR(dev)]; 

    case BLKBSZGET:                                        /* get block size */
        DPRINTK_IOCTL("   BLKBSZGET: %x\n", BLKBSZGET);
        break;

    case BLKBSZSET:                                        /* set block size */
        DPRINTK_IOCTL("   BLKBSZSET: %x\n", BLKBSZSET);
        break;

    case BLKRASET:                                         /* set read-ahead */
        DPRINTK_IOCTL("   BLKRASET: %x\n", BLKRASET);
        break;

    case BLKRAGET:                                         /* get read-ahead */
        DPRINTK_IOCTL("   BLKRAFET: %x\n", BLKRAGET);
        break;

    case HDIO_GETGEO:
        DPRINTK_IOCTL("   HDIO_GETGEO: %x\n", HDIO_GETGEO);
        if (!argument) return -EINVAL;

        /* We don't have real geometry info, but let's at least return
           values consistent with the size of the device */

        heads = 0xff;
        sectors = 0x3f; 
        cylinders = part->nr_sects / (heads * sectors);

        if (put_user(0x00,  (unsigned long *) &geo->start)) return -EFAULT;
        if (put_user(heads,  (byte *)&geo->heads)) return -EFAULT;
        if (put_user(sectors,  (byte *)&geo->sectors)) return -EFAULT;
        if (put_user(cylinders, (unsigned short *)&geo->cylinders)) return -EFAULT;

        return 0;

    case HDIO_GETGEO_BIG: 
        DPRINTK_IOCTL("   HDIO_GETGEO_BIG: %x\n", HDIO_GETGEO_BIG);
        if (!argument) return -EINVAL;

        /* We don't have real geometry info, but let's at least return
           values consistent with the size of the device */

        heads = 0xff;
        sectors = 0x3f; 
        cylinders = part->nr_sects / (heads * sectors);

        if (put_user(0x00,  (unsigned long *) &geo->start))  return -EFAULT;
        if (put_user(heads,  (byte *)&geo->heads))   return -EFAULT;
        if (put_user(sectors,  (byte *)&geo->sectors)) return -EFAULT;
        if (put_user(cylinders, (unsigned int *) &geo->cylinders)) return -EFAULT;

        return 0;

    case CDROMMULTISESSION:
        DPRINTK("FIXME: support multisession CDs later\n");
        for ( i = 0; i < sizeof(struct cdrom_multisession); i++ )
            if ( put_user(0, (byte *)(argument + i)) ) return -EFAULT;
        return 0;

    case SCSI_IOCTL_GET_BUS_NUMBER:
        DPRINTK("FIXME: SCSI_IOCTL_GET_BUS_NUMBER ioctl in XL blkif");
        return -ENOSYS;

    default:
        WPRINTK("ioctl %08x not supported by XL blkif\n", command);
        return -ENOSYS;
    }
    
    return 0;
}



/* check media change: should probably do something here in some cases :-) */
int blkif_check(kdev_t dev)
{
    DPRINTK("blkif_check\n");
    return 0;
}

int blkif_revalidate(kdev_t dev)
{
    struct block_device *bd;
    struct gendisk *gd;
    xl_disk_t *disk;
    unsigned long capacity;
    int i, rc = 0;
    
    if ( (bd = bdget(dev)) == NULL )
        return -EINVAL;

    /*
     * Update of partition info, and check of usage count, is protected
     * by the per-block-device semaphore.
     */
    down(&bd->bd_sem);

    if ( ((gd = get_gendisk(dev)) == NULL) ||
         ((disk = xldev_to_xldisk(dev)) == NULL) ||
         ((capacity = gd->part[MINOR(dev)].nr_sects) == 0) )
    {
        rc = -EINVAL;
        goto out;
    }

    if ( disk->usage > 1 )
    {
        rc = -EBUSY;
        goto out;
    }

    /* Only reread partition table if VBDs aren't mapped to partitions. */
    if ( !(gd->flags[MINOR(dev) >> gd->minor_shift] & GENHD_FL_VIRT_PARTNS) )
    {
        for ( i = gd->max_p - 1; i >= 0; i-- )
        {
            invalidate_device(dev+i, 1);
            gd->part[MINOR(dev+i)].start_sect = 0;
            gd->part[MINOR(dev+i)].nr_sects   = 0;
            gd->sizes[MINOR(dev+i)]           = 0;
        }

        grok_partitions(gd, MINOR(dev)>>gd->minor_shift, gd->max_p, capacity);
    }

 out:
    up(&bd->bd_sem);
    bdput(bd);
    return rc;
}


/*
 * blkif_queue_request
 *
 * request block io 
 * 
 * id: for guest use only.
 * operation: BLKIF_OP_{READ,WRITE,PROBE}
 * buffer: buffer to read/write into. this should be a
 *   virtual address in the guest os.
 */
static int blkif_queue_request(unsigned long   id,
                               int             operation,
                               char *          buffer,
                               unsigned long   sector_number,
                               unsigned short  nr_sectors,
                               kdev_t          device)
{
    unsigned long       buffer_ma = virt_to_bus(buffer);
    unsigned long       xid;
    struct gendisk     *gd;
    blkif_request_t    *req;
    struct buffer_head *bh;
    unsigned int        fsect, lsect;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    int ref;
#endif

    fsect = (buffer_ma & ~PAGE_MASK) >> 9;
    lsect = fsect + nr_sectors - 1;

    /* Buffer must be sector-aligned. Extent mustn't cross a page boundary. */
    if ( unlikely((buffer_ma & ((1<<9)-1)) != 0) )
        BUG();
    if ( lsect > ((PAGE_SIZE/512)-1) )
        BUG();

    buffer_ma &= PAGE_MASK;

    if ( unlikely(blkif_state != BLKIF_STATE_CONNECTED) )
        return 1;

    switch ( operation )
    {

    case BLKIF_OP_READ:
    case BLKIF_OP_WRITE:
        gd = get_gendisk(device); 

        /*
         * Update the sector_number we'll pass down as appropriate; note that
         * we could sanity check that resulting sector will be in this
         * partition, but this will happen in driver backend anyhow.
         */
        sector_number += gd->part[MINOR(device)].start_sect;

        /*
         * If this unit doesn't consist of virtual partitions then we clear 
         * the partn bits from the device number.
         */
        if ( !(gd->flags[MINOR(device)>>gd->minor_shift] & 
               GENHD_FL_VIRT_PARTNS) )
            device &= ~(gd->max_p - 1);

        if ( (sg_operation == operation) &&
             (sg_dev == device) &&
             (sg_next_sect == sector_number) )
        {
            req = RING_GET_REQUEST(&blk_ring, 
                                   blk_ring.req_prod_pvt - 1);
            bh = (struct buffer_head *)id;
     
            bh->b_reqnext = (struct buffer_head *)blk_shadow[req->id].request;
            blk_shadow[req->id].request = (unsigned long)id;

#ifdef CONFIG_XEN_BLKDEV_GRANT
            /* install a grant reference. */
            ref = gnttab_claim_grant_reference(&gref_head, gref_terminal);
            ASSERT( ref != -ENOSPC );

            gnttab_grant_foreign_access_ref(
                        ref,
                        rdomid,
                        buffer_ma >> PAGE_SHIFT,
                        ( operation == BLKIF_OP_WRITE ? 1 : 0 ) );

            blk_shadow[req->id].frame[req->nr_segments] =
                buffer_ma >> PAGE_SHIFT;

            req->frame_and_sects[req->nr_segments] =
                blkif_fas_from_gref(ref, fsect, lsect);
#else
            req->frame_and_sects[req->nr_segments] =
                blkif_fas(buffer_ma, fsect, lsect);
#endif
            if ( ++req->nr_segments < BLKIF_MAX_SEGMENTS_PER_REQUEST )
                sg_next_sect += nr_sectors;
            else
                DISABLE_SCATTERGATHER();

            /* Update the copy of the request in the recovery ring. */
            pickle_request(&blk_shadow[req->id], req );

            return 0;
        }
        else if ( RING_FULL(&blk_ring) )
        {
            return 1;
        }
        else
        {
            sg_operation = operation;
            sg_dev       = device;
            sg_next_sect = sector_number + nr_sectors;
        }
        break;

    default:
        panic("unknown op %d\n", operation);
    }

    /* Fill out a communications ring structure. */
    req = RING_GET_REQUEST(&blk_ring, blk_ring.req_prod_pvt);

    xid = GET_ID_FROM_FREELIST();
    blk_shadow[xid].request = (unsigned long)id;

    req->id            = xid;
    req->operation     = operation;
    req->sector_number = (blkif_sector_t)sector_number;
    req->device        = device; 
    req->nr_segments   = 1;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    /* install a grant reference. */
    ref = gnttab_claim_grant_reference(&gref_head, gref_terminal);
    ASSERT( ref != -ENOSPC );

    gnttab_grant_foreign_access_ref(
                ref,
                rdomid,
                buffer_ma >> PAGE_SHIFT,
                ( operation == BLKIF_OP_WRITE ? 1 : 0 ) );

    blk_shadow[xid].frame[0] = buffer_ma >> PAGE_SHIFT;

    req->frame_and_sects[0] = blkif_fas_from_gref(ref, fsect, lsect);
#else
    req->frame_and_sects[0] = blkif_fas(buffer_ma, fsect, lsect);
#endif

    /* Keep a private copy so we can reissue requests when recovering. */    
    pickle_request(&blk_shadow[xid], req);

    blk_ring.req_prod_pvt++;
    
    return 0;
}


/*
 * do_blkif_request
 *  read a block; request is in a request queue
 */
void do_blkif_request(request_queue_t *rq)
{
    struct request *req;
    struct buffer_head *bh, *next_bh;
    int rw, nsect, full, queued = 0;

    DPRINTK("Entered do_blkif_request\n"); 

    while ( !rq->plugged && !list_empty(&rq->queue_head))
    {
        if ( (req = blkdev_entry_next_request(&rq->queue_head)) == NULL ) 
            goto out;
  
        DPRINTK("do_blkif_request %p: cmd %i, sec %lx, (%li/%li) bh:%p\n",
                req, req->cmd, req->sector,
                req->current_nr_sectors, req->nr_sectors, req->bh);

        rw = req->cmd;
        if ( rw == READA )
            rw = READ;
        if ( unlikely((rw != READ) && (rw != WRITE)) )
            panic("XenoLinux Virtual Block Device: bad cmd: %d\n", rw);

        req->errors = 0;

        bh = req->bh;
        while ( bh != NULL )
        {
            next_bh = bh->b_reqnext;
            bh->b_reqnext = NULL;

            full = blkif_queue_request(
                (unsigned long)bh,
                (rw == READ) ? BLKIF_OP_READ : BLKIF_OP_WRITE, 
                bh->b_data, bh->b_rsector, bh->b_size>>9, bh->b_rdev);

            if ( full )
            { 
                bh->b_reqnext = next_bh;
                pending_queues[nr_pending++] = rq;
                if ( unlikely(nr_pending >= MAX_PENDING) )
                    BUG();
                goto out; 
            }

            queued++;

            /* Dequeue the buffer head from the request. */
            nsect = bh->b_size >> 9;
            bh = req->bh = next_bh;
            
            if ( bh != NULL )
            {
                /* There's another buffer head to do. Update the request. */
                req->hard_sector += nsect;
                req->hard_nr_sectors -= nsect;
                req->sector = req->hard_sector;
                req->nr_sectors = req->hard_nr_sectors;
                req->current_nr_sectors = bh->b_size >> 9;
                req->buffer = bh->b_data;
            }
            else
            {
                /* That was the last buffer head. Finalise the request. */
                if ( unlikely(end_that_request_first(req, 1, "XenBlk")) )
                    BUG();
                blkdev_dequeue_request(req);
                end_that_request_last(req);
            }
        }
    }

 out:
    if ( queued != 0 )
        flush_requests();
}


static void blkif_int(int irq, void *dev_id, struct pt_regs *ptregs)
{
    RING_IDX i, rp; 
    unsigned long flags; 
    struct buffer_head *bh, *next_bh;
    
    spin_lock_irqsave(&io_request_lock, flags);     

    if ( unlikely(blkif_state == BLKIF_STATE_CLOSED || recovery) )
    {
        spin_unlock_irqrestore(&io_request_lock, flags);
        return;
    }

    rp = blk_ring.sring->rsp_prod;
    rmb(); /* Ensure we see queued responses up to 'rp'. */

    for ( i = blk_ring.rsp_cons; i != rp; i++ )
    {
        unsigned long id;
        blkif_response_t *bret;
        
        bret = RING_GET_RESPONSE(&blk_ring, i);
        id = bret->id;
        bh = (struct buffer_head *)blk_shadow[id].request;

        blkif_completion(&blk_shadow[id]);

        ADD_ID_TO_FREELIST(id);

        switch ( bret->operation )
        {
        case BLKIF_OP_READ:
        case BLKIF_OP_WRITE:
            if ( unlikely(bret->status != BLKIF_RSP_OKAY) )
                DPRINTK("Bad return from blkdev data request: %lx\n",
                        bret->status);
            for ( ; bh != NULL; bh = next_bh )
            {
                next_bh = bh->b_reqnext;
                bh->b_reqnext = NULL;
                bh->b_end_io(bh, bret->status == BLKIF_RSP_OKAY);
            }

            break;
        case BLKIF_OP_PROBE:
            memcpy(&blkif_control_rsp, bret, sizeof(*bret));
            blkif_control_rsp_valid = 1;
            break;
        default:
            BUG();
        }

    }
    blk_ring.rsp_cons = i;
    
    kick_pending_request_queues();

    spin_unlock_irqrestore(&io_request_lock, flags);
}

#endif

/*****************************  COMMON CODE  *******************************/

#ifdef CONFIG_XEN_BLKDEV_GRANT
void blkif_control_probe_send(blkif_request_t *req, blkif_response_t *rsp,
                              unsigned long address)
{
    int ref = gnttab_claim_grant_reference(&gref_head, gref_terminal);
    ASSERT( ref != -ENOSPC );

    gnttab_grant_foreign_access_ref( ref, rdomid, address >> PAGE_SHIFT, 0 );

    req->frame_and_sects[0] = blkif_fas_from_gref(ref, 0, (PAGE_SIZE/512)-1);

    blkif_control_send(req, rsp);
}
#endif

void blkif_control_send(blkif_request_t *req, blkif_response_t *rsp)
{
    unsigned long flags, id;
    blkif_request_t *req_d;

 retry:
    while ( RING_FULL(&blk_ring) )
    {
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(1);
    }

    spin_lock_irqsave(&blkif_io_lock, flags);
    if ( RING_FULL(&blk_ring) )
    {
        spin_unlock_irqrestore(&blkif_io_lock, flags);
        goto retry;
    }

    DISABLE_SCATTERGATHER();
    req_d = RING_GET_REQUEST(&blk_ring, blk_ring.req_prod_pvt);
    *req_d = *req;    

    id = GET_ID_FROM_FREELIST();
    req_d->id = id;
    blk_shadow[id].request = (unsigned long)req;

    pickle_request(&blk_shadow[id], req);

    blk_ring.req_prod_pvt++;
    flush_requests();

    spin_unlock_irqrestore(&blkif_io_lock, flags);

    while ( !blkif_control_rsp_valid )
    {
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(1);
    }

    memcpy(rsp, &blkif_control_rsp, sizeof(*rsp));
    blkif_control_rsp_valid = 0;
}


/* Send a driver status notification to the domain controller. */
static void send_driver_status(int ok)
{
    ctrl_msg_t cmsg = {
        .type    = CMSG_BLKIF_FE,
        .subtype = CMSG_BLKIF_FE_DRIVER_STATUS,
        .length  = sizeof(blkif_fe_driver_status_t),
    };
    blkif_fe_driver_status_t *msg = (void*)cmsg.msg;
    
    msg->status = (ok ? BLKIF_DRIVER_STATUS_UP : BLKIF_DRIVER_STATUS_DOWN);

    ctrl_if_send_message_block(&cmsg, NULL, 0, TASK_UNINTERRUPTIBLE);
}

/* Tell the controller to bring up the interface. */
static void blkif_send_interface_connect(void)
{
    ctrl_msg_t cmsg = {
        .type    = CMSG_BLKIF_FE,
        .subtype = CMSG_BLKIF_FE_INTERFACE_CONNECT,
        .length  = sizeof(blkif_fe_interface_connect_t),
    };
    blkif_fe_interface_connect_t *msg = (void*)cmsg.msg;
    
    msg->handle      = 0;
    msg->shmem_frame = (virt_to_machine(blk_ring.sring) >> PAGE_SHIFT);
    
#ifdef CONFIG_XEN_BLKDEV_GRANT
    msg->shmem_ref   = gnttab_claim_grant_reference( &gref_head, gref_terminal );
    ASSERT( msg->shmem_ref != -ENOSPC );
    gnttab_grant_foreign_access_ref ( msg->shmem_ref , rdomid, msg->shmem_frame, 0 );
#endif

    ctrl_if_send_message_block(&cmsg, NULL, 0, TASK_UNINTERRUPTIBLE);
}

static void blkif_free(void)
{
    /* Prevent new requests being issued until we fix things up. */
    spin_lock_irq(&blkif_io_lock);
    recovery = 1;
    blkif_state = BLKIF_STATE_DISCONNECTED;
    spin_unlock_irq(&blkif_io_lock);

    /* Free resources associated with old device channel. */
    if ( blk_ring.sring != NULL )
    {
        free_page((unsigned long)blk_ring.sring);
        blk_ring.sring = NULL;
    }
    unbind_evtchn_from_irqhandler(blkif_evtchn, NULL);
    blkif_evtchn = 0;
}

static void blkif_close(void)
{
}

/* Move from CLOSED to DISCONNECTED state. */
static void blkif_disconnect(void)
{
    blkif_sring_t *sring;
    
    if ( blk_ring.sring != NULL )
        free_page((unsigned long)blk_ring.sring);
    
    sring = (blkif_sring_t *)__get_free_page(GFP_KERNEL);
    SHARED_RING_INIT(sring);
    FRONT_RING_INIT(&blk_ring, sring, PAGE_SIZE);
    blkif_state  = BLKIF_STATE_DISCONNECTED;
    blkif_send_interface_connect();
}

static void blkif_reset(void)
{
    blkif_free();
    blkif_disconnect();
}

static void blkif_recover(void)
{
    int i;
    blkif_request_t *req;
    struct blk_shadow *copy;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    int j;
#endif

    /* Stage 1: Make a safe copy of the shadow state. */
    copy = (struct blk_shadow *)kmalloc(sizeof(blk_shadow), GFP_KERNEL);
    BUG_ON(copy == NULL);
    memcpy(copy, blk_shadow, sizeof(blk_shadow));

    /* Stage 2: Set up free list. */
    memset(&blk_shadow, 0, sizeof(blk_shadow));
    for ( i = 0; i < BLK_RING_SIZE; i++ )
        blk_shadow[i].req.id = i+1;
    blk_shadow_free = blk_ring.req_prod_pvt;
    blk_shadow[BLK_RING_SIZE-1].req.id = 0x0fffffff;

    /* Stage 3: Find pending requests and requeue them. */
    for ( i = 0; i < BLK_RING_SIZE; i++ )
    {
        /* Not in use? */
        if ( copy[i].request == 0 )
            continue;

        /* Grab a request slot and unpickle shadow state into it. */
        req = RING_GET_REQUEST(
            &blk_ring, blk_ring.req_prod_pvt);
        unpickle_request(req, &copy[i]);

        /* We get a new request id, and must reset the shadow state. */
        req->id = GET_ID_FROM_FREELIST();
        memcpy(&blk_shadow[req->id], &copy[i], sizeof(copy[i]));

#ifdef CONFIG_XEN_BLKDEV_GRANT
        /* Rewrite any grant references invalidated by suspend/resume. */
        for ( j = 0; j < req->nr_segments; j++ )
        {
            if ( req->frame_and_sects[j] & GRANTREF_INVALID )
                gnttab_grant_foreign_access_ref(
                    blkif_gref_from_fas(req->frame_and_sects[j]),
                    rdomid,
                    blk_shadow[req->id].frame[j],
                    rq_data_dir((struct request *)
                                blk_shadow[req->id].request));
            req->frame_and_sects[j] &= ~GRANTREF_INVALID;
        }
        blk_shadow[req->id].req = *req;
#endif

        blk_ring.req_prod_pvt++;
    }

    kfree(copy);

    recovery = 0;

    /* blk_ring->req_prod will be set when we flush_requests().*/
    wmb();

    /* Kicks things back into life. */
    flush_requests();

    /* Now safe to left other people use the interface. */
    blkif_state = BLKIF_STATE_CONNECTED;
}

static void blkif_connect(blkif_fe_interface_status_t *status)
{
    int err = 0;

    blkif_evtchn = status->evtchn;

    err = bind_evtchn_to_irqhandler(
        blkif_evtchn, blkif_int, SA_SAMPLE_RANDOM, "blkif", NULL);
    if ( err != 0 )
    {
        WPRINTK("bind_evtchn_to_irqhandler failed (err=%d)\n", err);
        return;
    }

    if ( recovery ) 
    {
        blkif_recover();
    } 
    else 
    {
        /* Transition to connected in case we need to do 
         *  a partition probe on a whole disk. */
        blkif_state = BLKIF_STATE_CONNECTED;
        
        /* Probe for discs attached to the interface. */
        xlvbd_init();
    }
    
    /* Kick pending requests. */
    spin_lock_irq(&blkif_io_lock);
    kick_pending_request_queues();
    spin_unlock_irq(&blkif_io_lock);
}

static void unexpected(blkif_fe_interface_status_t *status)
{
    DPRINTK(" Unexpected blkif status %u in state %u\n", 
            status->status, blkif_state);
}

static void blkif_status(blkif_fe_interface_status_t *status)
{
#ifdef CONFIG_XEN_BLKDEV_GRANT
    rdomid       = status->domid; /* need to set rdomid early */
#endif

    if ( status->handle != blkif_handle )
    {
        WPRINTK(" Invalid blkif: handle=%u\n", status->handle);
        unexpected(status);
        return;
    }

    switch ( status->status ) 
    {
    case BLKIF_INTERFACE_STATUS_CLOSED:
        switch ( blkif_state )
        {
        case BLKIF_STATE_CLOSED:
            unexpected(status);
            break;
        case BLKIF_STATE_DISCONNECTED:
        case BLKIF_STATE_CONNECTED:
            unexpected(status);
            blkif_close();
            break;
        }
        break;

    case BLKIF_INTERFACE_STATUS_DISCONNECTED:
        switch ( blkif_state )
        {
        case BLKIF_STATE_CLOSED:
            blkif_disconnect();
            break;
        case BLKIF_STATE_DISCONNECTED:
        case BLKIF_STATE_CONNECTED:
            /* unexpected(status); */ /* occurs during suspend/resume */
            blkif_reset();
            break;
        }
        break;

    case BLKIF_INTERFACE_STATUS_CONNECTED:
        switch ( blkif_state )
        {
        case BLKIF_STATE_CLOSED:
            unexpected(status);
            blkif_disconnect();
            blkif_connect(status);
            break;
        case BLKIF_STATE_DISCONNECTED:
            blkif_connect(status);
            break;
        case BLKIF_STATE_CONNECTED:
            unexpected(status);
            blkif_connect(status);
            break;
        }
        break;

    case BLKIF_INTERFACE_STATUS_CHANGED:
        switch ( blkif_state )
        {
        case BLKIF_STATE_CLOSED:
        case BLKIF_STATE_DISCONNECTED:
            unexpected(status);
            break;
        case BLKIF_STATE_CONNECTED:
            vbd_update();
            break;
        }
        break;

    default:
        WPRINTK(" Invalid blkif status: %d\n", status->status);
        break;
    }
}


static void blkif_ctrlif_rx(ctrl_msg_t *msg, unsigned long id)
{
    switch ( msg->subtype )
    {
    case CMSG_BLKIF_FE_INTERFACE_STATUS:
        blkif_status((blkif_fe_interface_status_t *)
                     &msg->msg[0]);
        break;
    default:
        msg->length = 0;
        break;
    }

    ctrl_if_send_response(msg);
}

int wait_for_blkif(void)
{
    int err = 0;
    int i;
    send_driver_status(1);

    /*
     * We should read 'nr_interfaces' from response message and wait
     * for notifications before proceeding. For now we assume that we
     * will be notified of exactly one interface.
     */
    for ( i=0; (blkif_state != BLKIF_STATE_CONNECTED) && (i < 10*HZ); i++ )
    {
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(1);
    }

    if ( blkif_state != BLKIF_STATE_CONNECTED )
    {
        WPRINTK("Timeout connecting to device!\n");
        err = -ENOSYS;
    }
    return err;
}

int __init xlblk_init(void)
{
    int i;

#ifdef CONFIG_XEN_BLKDEV_GRANT
    /* A grant for every ring slot, plus one for the ring itself. */
    if ( 0 > gnttab_alloc_grant_references(MAXIMUM_OUTSTANDING_BLOCK_REQS + 1,
                                           &gref_head, &gref_terminal) )
        return 1;
    printk(KERN_ALERT "Blkif frontend is using grant tables.\n");
#endif

    if ( (xen_start_info.flags & SIF_INITDOMAIN) ||
         (xen_start_info.flags & SIF_BLK_BE_DOMAIN) )
        return 0;

    IPRINTK("Initialising virtual block device driver\n");

    blk_shadow_free = 0;
    memset(blk_shadow, 0, sizeof(blk_shadow));
    for ( i = 0; i < BLK_RING_SIZE; i++ )
        blk_shadow[i].req.id = i+1;
    blk_shadow[BLK_RING_SIZE-1].req.id = 0x0fffffff;

    (void)ctrl_if_register_receiver(CMSG_BLKIF_FE, blkif_ctrlif_rx,
                                    CALLBACK_IN_BLOCKING_CONTEXT);

    wait_for_blkif();

    return 0;
}

void blkdev_suspend(void)
{
}

void blkdev_resume(void)
{
#ifdef CONFIG_XEN_BLKDEV_GRANT
    int i, j;
    for ( i = 0; i < BLK_RING_SIZE; i++ )
        for ( j = 0; j < BLKIF_MAX_SEGMENTS_PER_REQUEST; j++ )
            blk_shadow[i].req.frame_and_sects[j] |= GRANTREF_INVALID;
#endif
    send_driver_status(1);
}

static void blkif_completion(struct blk_shadow *s)
{
    int i;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    for ( i = 0; i < s->req.nr_segments; i++ )
        gnttab_release_grant_reference(
            &gref_head, blkif_gref_from_fas(s->req.frame_and_sects[i]));
#else
    /* This is a hack to get the dirty logging bits set */
    if ( s->req.operation == BLKIF_OP_READ )
    {
        for ( i = 0; i < s->req.nr_segments; i++ )
        {
            unsigned long pfn = s->req.frame_and_sects[i] >> PAGE_SHIFT;
            unsigned long mfn = phys_to_machine_mapping[pfn];
            xen_machphys_update(mfn, pfn);
        }
    }
#endif
}
