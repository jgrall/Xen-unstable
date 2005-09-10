/******************************************************************************
 * evtchn.c
 * 
 * Xenolinux driver for receiving and demuxing event-channel signals.
 * 
 * Copyright (c) 2004, K A Fraser
 * Multi-process extensions Copyright (c) 2004, Steven Smith
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

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/major.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/poll.h>
#include <linux/irq.h>
#include <linux/init.h>
#define XEN_EVTCHN_MASK_OPS
#include <asm-xen/evtchn.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#include <linux/devfs_fs_kernel.h>
#define OLD_DEVFS
#else
#include <linux/gfp.h>
#endif

#ifdef OLD_DEVFS
/* NB. This must be shared amongst drivers if more things go in /dev/xen */
static devfs_handle_t xen_dev_dir;
#endif

struct per_user_data {
    /* Notification ring, accessed via /dev/xen/evtchn. */
#   define EVTCHN_RING_SIZE     2048  /* 2048 16-bit entries */
#   define EVTCHN_RING_MASK(_i) ((_i)&(EVTCHN_RING_SIZE-1))
    u16 *ring;
    unsigned int ring_cons, ring_prod, ring_overflow;

    /* Processes wait on this queue when ring is empty. */
    wait_queue_head_t evtchn_wait;
    struct fasync_struct *evtchn_async_queue;
};

/* Who's bound to each port? */
static struct per_user_data *port_user[NR_EVENT_CHANNELS];
static spinlock_t port_user_lock;

void evtchn_device_upcall(int port)
{
    struct per_user_data *u;

    spin_lock(&port_user_lock);

    mask_evtchn(port);
    clear_evtchn(port);

    if ( (u = port_user[port]) != NULL )
    {
        if ( (u->ring_prod - u->ring_cons) < EVTCHN_RING_SIZE )
        {
            u->ring[EVTCHN_RING_MASK(u->ring_prod)] = (u16)port;
            if ( u->ring_cons == u->ring_prod++ )
            {
                wake_up_interruptible(&u->evtchn_wait);
                kill_fasync(&u->evtchn_async_queue, SIGIO, POLL_IN);
            }
        }
        else
        {
            u->ring_overflow = 1;
        }
    }

    spin_unlock(&port_user_lock);
}

static ssize_t evtchn_read(struct file *file, char *buf,
                           size_t count, loff_t *ppos)
{
    int rc;
    unsigned int c, p, bytes1 = 0, bytes2 = 0;
    DECLARE_WAITQUEUE(wait, current);
    struct per_user_data *u = file->private_data;

    add_wait_queue(&u->evtchn_wait, &wait);

    count &= ~1; /* even number of bytes */

    if ( count == 0 )
    {
        rc = 0;
        goto out;
    }

    if ( count > PAGE_SIZE )
        count = PAGE_SIZE;

    for ( ; ; )
    {
        set_current_state(TASK_INTERRUPTIBLE);

        if ( (c = u->ring_cons) != (p = u->ring_prod) )
            break;

        if ( u->ring_overflow )
        {
            rc = -EFBIG;
            goto out;
        }

        if ( file->f_flags & O_NONBLOCK )
        {
            rc = -EAGAIN;
            goto out;
        }

        if ( signal_pending(current) )
        {
            rc = -ERESTARTSYS;
            goto out;
        }

        schedule();
    }

    /* Byte lengths of two chunks. Chunk split (if any) is at ring wrap. */
    if ( ((c ^ p) & EVTCHN_RING_SIZE) != 0 )
    {
        bytes1 = (EVTCHN_RING_SIZE - EVTCHN_RING_MASK(c)) * sizeof(u16);
        bytes2 = EVTCHN_RING_MASK(p) * sizeof(u16);
    }
    else
    {
        bytes1 = (p - c) * sizeof(u16);
        bytes2 = 0;
    }

    /* Truncate chunks according to caller's maximum byte count. */
    if ( bytes1 > count )
    {
        bytes1 = count;
        bytes2 = 0;
    }
    else if ( (bytes1 + bytes2) > count )
    {
        bytes2 = count - bytes1;
    }

    if ( copy_to_user(buf, &u->ring[EVTCHN_RING_MASK(c)], bytes1) ||
         ((bytes2 != 0) && copy_to_user(&buf[bytes1], &u->ring[0], bytes2)) )
    {
        rc = -EFAULT;
        goto out;
    }

    u->ring_cons += (bytes1 + bytes2) / sizeof(u16);

    rc = bytes1 + bytes2;

 out:
    __set_current_state(TASK_RUNNING);
    remove_wait_queue(&u->evtchn_wait, &wait);
    return rc;
}

static ssize_t evtchn_write(struct file *file, const char *buf,
                            size_t count, loff_t *ppos)
{
    int  rc, i;
    u16 *kbuf = (u16 *)__get_free_page(GFP_KERNEL);
    struct per_user_data *u = file->private_data;

    if ( kbuf == NULL )
        return -ENOMEM;

    count &= ~1; /* even number of bytes */

    if ( count == 0 )
    {
        rc = 0;
        goto out;
    }

    if ( count > PAGE_SIZE )
        count = PAGE_SIZE;

    if ( copy_from_user(kbuf, buf, count) != 0 )
    {
        rc = -EFAULT;
        goto out;
    }

    spin_lock_irq(&port_user_lock);
    for ( i = 0; i < (count/2); i++ )
        if ( (kbuf[i] < NR_EVENT_CHANNELS) && (port_user[kbuf[i]] == u) )
            unmask_evtchn(kbuf[i]);
    spin_unlock_irq(&port_user_lock);

    rc = count;

 out:
    free_page((unsigned long)kbuf);
    return rc;
}

static int evtchn_ioctl(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
    int rc = 0;
    struct per_user_data *u = file->private_data;

    spin_lock_irq(&port_user_lock);
    
    switch ( cmd )
    {
    case EVTCHN_RESET:
        /* Initialise the ring to empty. Clear errors. */
        u->ring_cons = u->ring_prod = u->ring_overflow = 0;
        break;

    case EVTCHN_BIND:
        if ( arg >= NR_EVENT_CHANNELS )
        {
            rc = -EINVAL;
        }
        else if ( port_user[arg] != NULL )
        {
            rc = -EISCONN;
        }
        else
        {
            port_user[arg] = u;
            unmask_evtchn(arg);
        }
        break;

    case EVTCHN_UNBIND:
        if ( arg >= NR_EVENT_CHANNELS )
        {
            rc = -EINVAL;
        }
        else if ( port_user[arg] != u )
        {
            rc = -ENOTCONN;
        }
        else
        {
            port_user[arg] = NULL;
            mask_evtchn(arg);
        }
        break;

    default:
        rc = -ENOSYS;
        break;
    }

    spin_unlock_irq(&port_user_lock);   

    return rc;
}

static unsigned int evtchn_poll(struct file *file, poll_table *wait)
{
    unsigned int mask = POLLOUT | POLLWRNORM;
    struct per_user_data *u = file->private_data;

    poll_wait(file, &u->evtchn_wait, wait);
    if ( u->ring_cons != u->ring_prod )
        mask |= POLLIN | POLLRDNORM;
    if ( u->ring_overflow )
        mask = POLLERR;
    return mask;
}

static int evtchn_fasync(int fd, struct file *filp, int on)
{
    struct per_user_data *u = filp->private_data;
    return fasync_helper(fd, filp, on, &u->evtchn_async_queue);
}

static int evtchn_open(struct inode *inode, struct file *filp)
{
    struct per_user_data *u;

    if ( (u = kmalloc(sizeof(*u), GFP_KERNEL)) == NULL )
        return -ENOMEM;

    memset(u, 0, sizeof(*u));
    init_waitqueue_head(&u->evtchn_wait);

    if ( (u->ring = (u16 *)__get_free_page(GFP_KERNEL)) == NULL )
    {
        kfree(u);
        return -ENOMEM;
    }

    filp->private_data = u;

    return 0;
}

static int evtchn_release(struct inode *inode, struct file *filp)
{
    int i;
    struct per_user_data *u = filp->private_data;

    spin_lock_irq(&port_user_lock);

    free_page((unsigned long)u->ring);

    for ( i = 0; i < NR_EVENT_CHANNELS; i++ )
    {
        if ( port_user[i] == u )
        {
            port_user[i] = NULL;
            mask_evtchn(i);
        }
    }

    spin_unlock_irq(&port_user_lock);

    kfree(u);

    return 0;
}

static struct file_operations evtchn_fops = {
    .owner   = THIS_MODULE,
    .read    = evtchn_read,
    .write   = evtchn_write,
    .ioctl   = evtchn_ioctl,
    .poll    = evtchn_poll,
    .fasync  = evtchn_fasync,
    .open    = evtchn_open,
    .release = evtchn_release,
};

static struct miscdevice evtchn_miscdev = {
    .minor        = EVTCHN_MINOR,
    .name         = "evtchn",
    .fops         = &evtchn_fops,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
    .devfs_name   = "misc/evtchn",
#endif
};

static int __init evtchn_init(void)
{
#ifdef OLD_DEVFS
    devfs_handle_t symlink_handle;
    int            pos;
    char           link_dest[64];
#endif
    int err;

    spin_lock_init(&port_user_lock);
    memset(port_user, 0, sizeof(port_user));

    /* (DEVFS) create '/dev/misc/evtchn'. */
    err = misc_register(&evtchn_miscdev);
    if ( err != 0 )
    {
        printk(KERN_ALERT "Could not register /dev/misc/evtchn\n");
        return err;
    }

#ifdef OLD_DEVFS
    /* (DEVFS) create directory '/dev/xen'. */
    xen_dev_dir = devfs_mk_dir(NULL, "xen", NULL);

    /* (DEVFS) &link_dest[pos] == '../misc/evtchn'. */
    pos = devfs_generate_path(evtchn_miscdev.devfs_handle, 
                              &link_dest[3], 
                              sizeof(link_dest) - 3);
    if ( pos >= 0 )
        strncpy(&link_dest[pos], "../", 3);

    /* (DEVFS) symlink '/dev/xen/evtchn' -> '../misc/evtchn'. */
    (void)devfs_mk_symlink(xen_dev_dir, 
                           "evtchn", 
                           DEVFS_FL_DEFAULT, 
                           &link_dest[pos],
                           &symlink_handle, 
                           NULL);

    /* (DEVFS) automatically destroy the symlink with its destination. */
    devfs_auto_unregister(evtchn_miscdev.devfs_handle, symlink_handle);
#endif

    printk("Event-channel device installed.\n");

    return 0;
}

static void evtchn_cleanup(void)
{
    misc_deregister(&evtchn_miscdev);
}

module_init(evtchn_init);
module_exit(evtchn_cleanup);
