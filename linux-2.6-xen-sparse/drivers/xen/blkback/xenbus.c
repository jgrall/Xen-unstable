/*  Xenbus code for blkif backend
    Copyright (C) 2005 Rusty Russell <rusty@rustcorp.com.au>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <stdarg.h>
#include <linux/module.h>
#include <asm-xen/xenbus.h>
#include "common.h"

struct backend_info
{
	struct xenbus_device *dev;

	/* our communications channel */
	blkif_t *blkif;

	long int frontend_id;
	long int pdev;
	long int readonly;

	/* watch back end for changes */
	struct xenbus_watch backend_watch;

	/* watch front end for changes */
	struct xenbus_watch watch;
	char *frontpath;
};

static int blkback_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev->data;

	if (be->watch.node)
		unregister_xenbus_watch(&be->watch);
	unregister_xenbus_watch(&be->backend_watch);
	if (be->blkif)
		blkif_put(be->blkif);
	if (be->frontpath)
		kfree(be->frontpath);
	kfree(be);
	return 0;
}

/* Front end tells us frame. */
static void frontend_changed(struct xenbus_watch *watch,
			     const char **vec, unsigned int len)
{
	unsigned long ring_ref;
	unsigned int evtchn;
	int err;
	struct xenbus_transaction *xbt;
	struct backend_info *be
		= container_of(watch, struct backend_info, watch);

	/* If other end is gone, delete ourself. */
	if (vec && !xenbus_exists(NULL, be->frontpath, "")) {
		device_unregister(&be->dev->dev);
		return;
	}
	if (be->blkif == NULL || be->blkif->status == CONNECTED)
		return;

	err = xenbus_gather(NULL, be->frontpath, "ring-ref", "%lu", &ring_ref,
			    "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_error(be->dev, err,
				 "reading %s/ring-ref and event-channel",
				 be->frontpath);
		return;
	}

	/* Map the shared frame, irq etc. */
	err = blkif_map(be->blkif, ring_ref, evtchn);
	if (err) {
		xenbus_dev_error(be->dev, err,
				 "mapping ring-ref %lu port %u",
				 ring_ref, evtchn);
		return;
	}
	/* XXX From here on should 'blkif_unmap' on error. */

again:
	/* Supply the information about the device the frontend needs */
	xbt = xenbus_transaction_start();
	if (IS_ERR(xbt)) {
		xenbus_dev_error(be->dev, err, "starting transaction");
		return;
	}

	err = xenbus_printf(xbt, be->dev->nodename, "sectors", "%lu",
			    vbd_size(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/sectors",
				 be->dev->nodename);
		goto abort;
	}

	/* FIXME: use a typename instead */
	err = xenbus_printf(xbt, be->dev->nodename, "info", "%u",
			    vbd_info(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/info",
				 be->dev->nodename);
		goto abort;
	}
	err = xenbus_printf(xbt, be->dev->nodename, "sector-size", "%lu",
			    vbd_secsize(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/sector-size",
				 be->dev->nodename);
		goto abort;
	}

	err = xenbus_transaction_end(xbt, 0);
	if (err == -EAGAIN)
		goto again;
	if (err) {
		xenbus_dev_error(be->dev, err, "ending transaction",
				 ring_ref, evtchn);
		goto abort;
	}

	xenbus_dev_ok(be->dev);

	return;

 abort:
	xenbus_transaction_end(xbt, 1);
}

/* 
   Setup supplies physical device.  
   We provide event channel and device details to front end.
   Frontend supplies shared frame and event channel.
 */
static void backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	int err;
	char *p;
	long int handle, pdev;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;

	err = xenbus_scanf(NULL, dev->nodename,
			   "physical-device", "%li", &pdev);
	if (XENBUS_EXIST_ERR(err))
		return;
	if (err < 0) {
		xenbus_dev_error(dev, err, "reading physical-device");
		return;
	}
	if (be->pdev && be->pdev != pdev) {
		printk(KERN_WARNING
		       "changing physical-device not supported\n");
		return;
	}
	be->pdev = pdev;

	/* If there's a read-only node, we're read only. */
	p = xenbus_read(NULL, dev->nodename, "read-only", NULL);
	if (!IS_ERR(p)) {
		be->readonly = 1;
		kfree(p);
	}

	if (be->blkif == NULL) {
		/* Front end dir is a number, which is used as the handle. */
		p = strrchr(be->frontpath, '/') + 1;
		handle = simple_strtoul(p, NULL, 0);

		be->blkif = alloc_blkif(be->frontend_id);
		if (IS_ERR(be->blkif)) {
			err = PTR_ERR(be->blkif);
			be->blkif = NULL;
			xenbus_dev_error(dev, err,
					 "creating block interface");
			return;
		}

		err = vbd_create(be->blkif, handle, be->pdev, be->readonly);
		if (err) {
			blkif_put(be->blkif);
			be->blkif = NULL;
			xenbus_dev_error(dev, err,
					 "creating vbd structure");
			return;
		}

		/* Pass in NULL node to skip exist test. */
		frontend_changed(&be->watch, NULL, 0);
	}
}

static int blkback_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	struct backend_info *be;
	char *frontend;
	int err;

	be = kmalloc(sizeof(*be), GFP_KERNEL);
	if (!be) {
		xenbus_dev_error(dev, -ENOMEM,
				 "allocating backend structure");
		return -ENOMEM;
	}
	memset(be, 0, sizeof(*be));

	frontend = NULL;
	err = xenbus_gather(NULL, dev->nodename,
			    "frontend-id", "%li", &be->frontend_id,
			    "frontend", NULL, &frontend,
			    NULL);
	if (XENBUS_EXIST_ERR(err))
		goto free_be;
	if (err < 0) {
		xenbus_dev_error(dev, err,
				 "reading %s/frontend or frontend-id",
				 dev->nodename);
		goto free_be;
	}
	if (strlen(frontend) == 0 || !xenbus_exists(NULL, frontend, "")) {
		/* If we can't get a frontend path and a frontend-id,
		 * then our bus-id is no longer valid and we need to
		 * destroy the backend device.
		 */
		err = -ENOENT;
		goto free_be;
	}

	be->dev = dev;
	be->backend_watch.node = dev->nodename;
	be->backend_watch.callback = backend_changed;
	/* Will implicitly call backend_changed once. */
	err = register_xenbus_watch(&be->backend_watch);
	if (err) {
		be->backend_watch.node = NULL;
		xenbus_dev_error(dev, err,
				 "adding backend watch on %s",
				 dev->nodename);
		goto free_be;
	}

	be->frontpath = frontend;
	be->watch.node = be->frontpath;
	be->watch.callback = frontend_changed;
	err = register_xenbus_watch(&be->watch);
	if (err) {
		be->watch.node = NULL;
		xenbus_dev_error(dev, err,
				 "adding frontend watch on %s",
				 be->frontpath);
		goto free_be;
	}

	dev->data = be;
	return 0;

 free_be:
	if (be->backend_watch.node)
		unregister_xenbus_watch(&be->backend_watch);
	if (frontend)
		kfree(frontend);
	kfree(be);
	return err;
}

static struct xenbus_device_id blkback_ids[] = {
	{ "vbd" },
	{ "" }
};

static struct xenbus_driver blkback = {
	.name = "vbd",
	.owner = THIS_MODULE,
	.ids = blkback_ids,
	.probe = blkback_probe,
	.remove = blkback_remove,
};

void blkif_xenbus_init(void)
{
	xenbus_register_backend(&blkback);
}

/*
 * Local variables:
 *  c-file-style: "linux"
 *  indent-tabs-mode: t
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
