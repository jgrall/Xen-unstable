/******************************************************************************
 * block.h
 * 
 * Shared definitions between all levels of XenLinux Virtual block devices.
 * 
 * Copyright (c) 2003-2004, Keir Fraser & Steve Hand
 * Modifications by Mark A. Williamson are (c) Intel Research Cambridge
 * Copyright (c) 2004-2005, Christian Limpach
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

#ifndef __XEN_DRIVERS_BLOCK_H__
#define __XEN_DRIVERS_BLOCK_H__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/blkdev.h>
#include <linux/major.h>
#include <linux/devfs_fs_kernel.h>
#include <asm-xen/hypervisor.h>
#include <asm-xen/xenbus.h>
#include <asm-xen/xen-public/xen.h>
#include <asm-xen/xen-public/io/blkif.h>
#include <asm-xen/xen-public/io/ring.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#if 1 
#define IPRINTK(fmt, args...) \
    printk(KERN_INFO "xen_blk: " fmt, ##args)
#else
#define IPRINTK(fmt, args...) ((void)0)
#endif

#if 1 
#define WPRINTK(fmt, args...) \
    printk(KERN_WARNING "xen_blk: " fmt, ##args)
#else
#define WPRINTK(fmt, args...) ((void)0)
#endif
 
#if 0
#define DPRINTK(_f, _a...) printk ( KERN_ALERT _f , ## _a )
#else
#define DPRINTK(_f, _a...) ((void)0)
#endif

#if 0
#define DPRINTK_IOCTL(_f, _a...) printk ( KERN_ALERT _f , ## _a )
#else
#define DPRINTK_IOCTL(_f, _a...) ((void)0)
#endif

struct xlbd_type_info
{
	int partn_shift;
	int disks_per_major;
	char *devname;
	char *diskname;
};

struct xlbd_major_info
{
	int major;
	int index;
	int usage;
	struct xlbd_type_info *type;
};

/*
 * We have one of these per vbd, whether ide, scsi or 'other'.  They
 * hang in private_data off the gendisk structure. We may end up
 * putting all kinds of interesting stuff here :-)
 */
struct blkfront_info
{
	struct xenbus_device *xbdev;
	/* We watch the backend */
	struct xenbus_watch watch;
	dev_t dev;
	int vdevice;
	blkif_vdev_t handle;
	int connected;
	char *backend;
	int backend_id;
	int ring_ref;
	blkif_front_ring_t ring;
	unsigned int evtchn;
	struct xlbd_major_info *mi;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	request_queue_t *rq;
#endif
	struct work_struct work;
	struct gnttab_free_callback callback;
};

extern spinlock_t blkif_io_lock;

extern int blkif_open(struct inode *inode, struct file *filep);
extern int blkif_release(struct inode *inode, struct file *filep);
extern int blkif_ioctl(struct inode *inode, struct file *filep,
                       unsigned command, unsigned long argument);
extern int blkif_check(dev_t dev);
extern int blkif_revalidate(dev_t dev);
extern void do_blkif_request (request_queue_t *rq); 

/* Virtual block-device subsystem. */
int xlvbd_add(blkif_sector_t capacity, int device,
	      u16 vdisk_info, u16 sector_size, struct blkfront_info *info);
void xlvbd_del(struct blkfront_info *info);
#endif /* __XEN_DRIVERS_BLOCK_H__ */
