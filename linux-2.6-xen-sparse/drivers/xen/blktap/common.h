/* 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
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

#ifndef __BLKIF__BACKEND__COMMON_H__
#define __BLKIF__BACKEND__COMMON_H__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <asm/pgalloc.h>
#include <xen/evtchn.h>
#include <asm/hypervisor.h>
#include <xen/interface/io/blkif.h>
#include <xen/interface/io/ring.h>
#include <xen/gnttab.h>
#include <xen/driver_util.h>

#define DPRINTK(_f, _a...) pr_debug("(file=%s, line=%d) " _f, \
                                    __FILE__ , __LINE__ , ## _a )

#define WPRINTK(fmt, args...) printk(KERN_WARNING "blk_tap: " fmt, ##args)

struct backend_info;

typedef struct blkif_st {
	/* Unique identifier for this interface. */
	domid_t           domid;
	unsigned int      handle;
	/* Physical parameters of the comms window. */
	unsigned int      evtchn;
	unsigned int      irq;
	/* Comms information. */
	blkif_back_ring_t blk_ring;
	struct vm_struct *blk_ring_area;
	/* Back pointer to the backend_info. */
	struct backend_info *be;
	/* Private fields. */
	spinlock_t       blk_ring_lock;
	atomic_t         refcnt;

	wait_queue_head_t   wq;
	struct task_struct  *xenblkd;
	unsigned int        waiting_reqs;
	request_queue_t     *plug;

	/* statistics */
	unsigned long       st_print;
	int                 st_rd_req;
	int                 st_wr_req;
	int                 st_oo_req;

	wait_queue_head_t waiting_to_free;

	grant_handle_t shmem_handle;
	grant_ref_t    shmem_ref;
	
	int		dev_num;
	uint64_t        sectors;
} blkif_t;

blkif_t *tap_alloc_blkif(domid_t domid);
void tap_blkif_free(blkif_t *blkif);
int tap_blkif_map(blkif_t *blkif, unsigned long shared_page, 
		  unsigned int evtchn);
void tap_blkif_unmap(blkif_t *blkif);

#define blkif_get(_b) (atomic_inc(&(_b)->refcnt))
#define blkif_put(_b)					\
	do {						\
		if (atomic_dec_and_test(&(_b)->refcnt))	\
			wake_up(&(_b)->waiting_to_free);\
	} while (0)


struct phys_req {
	unsigned short       dev;
	unsigned short       nr_sects;
	struct block_device *bdev;
	blkif_sector_t       sector_number;
};

void tap_blkif_interface_init(void);

void tap_blkif_xenbus_init(void);

irqreturn_t tap_blkif_be_int(int irq, void *dev_id, struct pt_regs *regs);
int tap_blkif_schedule(void *arg);

int dom_to_devid(domid_t domid, int xenbus_id, blkif_t *blkif);
void signal_tapdisk(int idx);

#endif /* __BLKIF__BACKEND__COMMON_H__ */
