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
static void frontend_changed(struct xenbus_watch *watch, const char *node)
{
	unsigned long sharedmfn;
	unsigned int evtchn;
	int err;
	struct backend_info *be
		= container_of(watch, struct backend_info, watch);

	/* If other end is gone, delete ourself. */
	if (!xenbus_exists(be->frontpath, "")) {
		xenbus_rm(be->dev->nodename, "");
		device_unregister(&be->dev->dev);
		return;
	}
	if (be->blkif->status == CONNECTED)
		return;

	err = xenbus_gather(be->frontpath, "grant-id", "%lu", &sharedmfn,
			    "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_error(be->dev, err,
				 "reading %s/grant-id and event-channel",
				 be->frontpath);
		return;
	}

	/* Supply the information about the device the frontend needs */
	err = xenbus_transaction_start(be->dev->nodename);
	if (err) {
		xenbus_dev_error(be->dev, err, "starting transaction");
		return;
	}

	err = xenbus_printf(be->dev->nodename, "sectors", "%lu",
			    vbd_size(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/sectors",
				 be->dev->nodename);
		goto abort;
	}

	/* FIXME: use a typename instead */
	err = xenbus_printf(be->dev->nodename, "info", "%u",
			    vbd_info(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/info",
				 be->dev->nodename);
		goto abort;
	}
	err = xenbus_printf(be->dev->nodename, "sector-size", "%lu",
			    vbd_secsize(&be->blkif->vbd));
	if (err) {
		xenbus_dev_error(be->dev, err, "writing %s/sector-size",
				 be->dev->nodename);
		goto abort;
	}

	/* Map the shared frame, irq etc. */
	err = blkif_map(be->blkif, sharedmfn, evtchn);
	if (err) {
		xenbus_dev_error(be->dev, err,
				 "mapping shared-frame %lu port %u",
				 sharedmfn, evtchn);
		goto abort;
	}

	xenbus_transaction_end(0);
	xenbus_dev_ok(be->dev);

	return;

abort:
	xenbus_transaction_end(1);
}

/* 
   Setup supplies physical device.  
   We provide event channel and device details to front end.
   Frontend supplies shared frame and event channel.
 */
static void backend_changed(struct xenbus_watch *watch, const char *node)
{
	int err;
	char *p;
	char *frontend;
	long int handle, pdev;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;

	frontend = NULL;
	err = xenbus_gather(dev->nodename,
			    "frontend-id", "%li", &be->frontend_id,
			    "frontend", NULL, &frontend,
			    NULL);
	if (XENBUS_EXIST_ERR(err) ||
	    strlen(frontend) == 0 || !xenbus_exists(frontend, "")) {
		/* If we can't get a frontend path and a frontend-id,
		 * then our bus-id is no longer valid and we need to
		 * destroy the backend device.
		 */
		goto device_fail;
	}
	if (err < 0) {
		xenbus_dev_error(dev, err,
				 "reading %s/frontend or frontend-id",
				 dev->nodename);
		goto device_fail;
	}

	if (!be->frontpath || strcmp(frontend, be->frontpath)) {
		if (be->watch.node)
			unregister_xenbus_watch(&be->watch);
		if (be->frontpath)
			kfree(be->frontpath);
		be->frontpath = frontend;
		frontend = NULL;
		be->watch.node = be->frontpath;
		be->watch.callback = frontend_changed;
		err = register_xenbus_watch(&be->watch);
		if (err) {
			be->watch.node = NULL;
			xenbus_dev_error(dev, err,
					 "adding frontend watch on %s",
					 be->frontpath);
			goto device_fail;
		}
	}

	err = xenbus_scanf(dev->nodename, "physical-device", "%li", &pdev);
	if (XENBUS_EXIST_ERR(err))
		goto out;
	if (err < 0) {
		xenbus_dev_error(dev, err, "reading physical-device");
		goto device_fail;
	}
	if (be->pdev && be->pdev != pdev) {
		printk(KERN_WARNING
		       "changing physical-device not supported\n");
		goto device_fail;
	}
	be->pdev = pdev;

	/* If there's a read-only node, we're read only. */
	p = xenbus_read(dev->nodename, "read-only", NULL);
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
			xenbus_dev_error(dev, err, "creating block interface");
			goto device_fail;
		}

		err = vbd_create(be->blkif, handle, be->pdev, be->readonly);
		if (err) {
			xenbus_dev_error(dev, err, "creating vbd structure");
			goto device_fail;
		}

		frontend_changed(&be->watch, be->frontpath);
	}

 out:
	if (frontend)
		kfree(frontend);
	return;

 device_fail:
	device_unregister(&be->dev->dev);
	goto out;
}

static int blkback_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	struct backend_info *be;
	int err;

	be = kmalloc(sizeof(*be), GFP_KERNEL);
	if (!be) {
		xenbus_dev_error(dev, -ENOMEM, "allocating backend structure");
		return -ENOMEM;
	}

	memset(be, 0, sizeof(*be));

	be->dev = dev;
	be->backend_watch.node = dev->nodename;
	be->backend_watch.callback = backend_changed;
	err = register_xenbus_watch(&be->backend_watch);
	if (err) {
		xenbus_dev_error(dev, err, "adding backend watch on %s",
				 dev->nodename);
		goto free_be;
	}

	dev->data = be;

	backend_changed(&be->backend_watch, dev->nodename);
	return err;
 free_be:
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
