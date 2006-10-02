/* drivers/xen/blktap/xenbus.c
 *
 * Xenbus code for blktap
 *
 * Copyright (c) 2004-2005, Andrew Warfield and Julian Chesterfield
 *
 * Based on the blkback xenbus code:
 *
 * Copyright (C) 2005 Rusty Russell <rusty@rustcorp.com.au>
 * Copyright (C) 2005 XenSource Ltd
 *
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

#include <stdarg.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <xen/xenbus.h>
#include "common.h"


struct backend_info
{
	struct xenbus_device *dev;
	blkif_t *blkif;
	struct xenbus_watch backend_watch;
	int xenbus_id;
};


static void connect(struct backend_info *);
static int connect_ring(struct backend_info *);
static int blktap_remove(struct xenbus_device *dev);
static int blktap_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id);
static void tap_backend_changed(struct xenbus_watch *, const char **,
			    unsigned int);
static void tap_frontend_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state);

static int strsep_len(const char *str, char c, unsigned int len)
{
        unsigned int i;

        for (i = 0; str[i]; i++)
                if (str[i] == c) {
                        if (len == 0)
                                return i;
                        len--;
                }
        return (len == 0) ? i : -ERANGE;
}

static long get_id(const char *str)
{
        int len,end;
        const char *ptr;
        char *tptr, num[10];
	
        len = strsep_len(str, '/', 2);
        end = strlen(str);
        if ( (len < 0) || (end < 0) ) return -1;
	
        ptr = str + len + 1;
        strncpy(num,ptr,end - len);
        tptr = num + (end - (len + 1));
        *tptr = '\0';
	DPRINTK("Get_id called for %s (%s)\n",str,num);
	
        return simple_strtol(num, NULL, 10);
}				

static void tap_update_blkif_status(blkif_t *blkif)
{ 
	int err;

	/* Not ready to connect? */
	if(!blkif->irq || !blkif->sectors) {
		return;
	} 

	/* Already connected? */
	if (blkif->be->dev->state == XenbusStateConnected)
		return;

	/* Attempt to connect: exit if we fail to. */
	connect(blkif->be);
	if (blkif->be->dev->state != XenbusStateConnected)
		return;

	blkif->xenblkd = kthread_run(tap_blkif_schedule, blkif,
				     "xvd %d",
				     blkif->domid);

	if (IS_ERR(blkif->xenblkd)) {
		err = PTR_ERR(blkif->xenblkd);
		blkif->xenblkd = NULL;
		xenbus_dev_fatal(blkif->be->dev, err, "start xenblkd");
		WPRINTK("Error starting thread\n");
	}
}

static int blktap_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev->dev.driver_data;

	if (be->backend_watch.node) {
		unregister_xenbus_watch(&be->backend_watch);
		kfree(be->backend_watch.node);
		be->backend_watch.node = NULL;
	}
	if (be->blkif) {
		if (be->blkif->xenblkd)
			kthread_stop(be->blkif->xenblkd);
		signal_tapdisk(be->blkif->dev_num);
		tap_blkif_free(be->blkif);
		be->blkif = NULL;
	}
	kfree(be);
	dev->dev.driver_data = NULL;
	return 0;
}

/**
 * Entry point to this code when a new device is created.  Allocate
 * the basic structures, and watch the store waiting for the
 * user-space program to tell us the physical device info.  Switch to
 * InitWait.
 */
static int blktap_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	int err;
	struct backend_info *be = kzalloc(sizeof(struct backend_info),
					  GFP_KERNEL);
	if (!be) {
		xenbus_dev_fatal(dev, -ENOMEM,
				 "allocating backend structure");
		return -ENOMEM;
	}

	be->dev = dev;
	dev->dev.driver_data = be;
	be->xenbus_id = get_id(dev->nodename);

	be->blkif = tap_alloc_blkif(dev->otherend_id);
	if (IS_ERR(be->blkif)) {
		err = PTR_ERR(be->blkif);
		be->blkif = NULL;
		xenbus_dev_fatal(dev, err, "creating block interface");
		goto fail;
	}

	/* setup back pointer */
	be->blkif->be = be;
	be->blkif->sectors = 0;

	/* set a watch on disk info, waiting for userspace to update details*/
	err = xenbus_watch_path2(dev, dev->nodename, "info",
				 &be->backend_watch, tap_backend_changed);
	if (err)
		goto fail;
	
	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		goto fail;
	return 0;

fail:
	DPRINTK("blktap probe failed");
	blktap_remove(dev);
	return err;
}


/**
 * Callback received when the user space code has placed the device
 * information in xenstore. 
 */
static void tap_backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	int err;
	unsigned long info;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;
	
	/** 
	 * Check to see whether userspace code has opened the image 
	 * and written sector
	 * and disk info to xenstore
	 */
	err = xenbus_gather(XBT_NIL, dev->nodename, "info", "%lu", &info, 
			    NULL);	
	if (err) {
		xenbus_dev_error(dev, err, "getting info");
		return;
	}

	DPRINTK("Userspace update on disk info, %lu\n",info);

	err = xenbus_gather(XBT_NIL, dev->nodename, "sectors", "%llu", 
			    &be->blkif->sectors, NULL);

	/* Associate tap dev with domid*/
	be->blkif->dev_num = dom_to_devid(be->blkif->domid, be->xenbus_id, 
					  be->blkif);
	DPRINTK("Thread started for domid [%d], connecting disk\n", 
		be->blkif->dev_num);

	tap_update_blkif_status(be->blkif);
}

/**
 * Callback received when the frontend's state changes.
 */
static void tap_frontend_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state)
{
	struct backend_info *be = dev->dev.driver_data;
	int err;

	DPRINTK("");

	switch (frontend_state) {
	case XenbusStateInitialising:
		if (dev->state == XenbusStateClosed) {
			printk("%s: %s: prepare for reconnect\n",
			       __FUNCTION__, dev->nodename);
			xenbus_switch_state(dev, XenbusStateInitWait);
		}
		break;

	case XenbusStateInitialised:
	case XenbusStateConnected:
		/* Ensure we connect even when two watches fire in 
		   close successsion and we miss the intermediate value 
		   of frontend_state. */
		if (dev->state == XenbusStateConnected)
			break;

		err = connect_ring(be);
		if (err)
			break;
		tap_update_blkif_status(be->blkif);
		break;

	case XenbusStateClosing:
		if (be->blkif->xenblkd) {
			kthread_stop(be->blkif->xenblkd);
			be->blkif->xenblkd = NULL;
		}
		xenbus_switch_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		xenbus_switch_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		/* fall through if not online */
	case XenbusStateUnknown:
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}


/**
 * Switch to Connected state.
 */
static void connect(struct backend_info *be)
{
	int err;

	struct xenbus_device *dev = be->dev;

	err = xenbus_switch_state(dev, XenbusStateConnected);
	if (err)
		xenbus_dev_fatal(dev, err, "switching to Connected state",
				 dev->nodename);

	return;
}


static int connect_ring(struct backend_info *be)
{
	struct xenbus_device *dev = be->dev;
	unsigned long ring_ref;
	unsigned int evtchn;
	int err;

	DPRINTK("%s", dev->otherend);

	err = xenbus_gather(XBT_NIL, dev->otherend, "ring-ref", "%lu", 
			    &ring_ref, "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_fatal(dev, err,
				 "reading %s/ring-ref and event-channel",
				 dev->otherend);
		return err;
	}

	/* Map the shared frame, irq etc. */
	err = tap_blkif_map(be->blkif, ring_ref, evtchn);
	if (err) {
		xenbus_dev_fatal(dev, err, "mapping ring-ref %lu port %u",
				 ring_ref, evtchn);
		return err;
	} 

	return 0;
}


/* ** Driver Registration ** */


static struct xenbus_device_id blktap_ids[] = {
	{ "tap" },
	{ "" }
};


static struct xenbus_driver blktap = {
	.name = "tap",
	.owner = THIS_MODULE,
	.ids = blktap_ids,
	.probe = blktap_probe,
	.remove = blktap_remove,
	.otherend_changed = tap_frontend_changed
};


void tap_blkif_xenbus_init(void)
{
	xenbus_register_backend(&blktap);
}
