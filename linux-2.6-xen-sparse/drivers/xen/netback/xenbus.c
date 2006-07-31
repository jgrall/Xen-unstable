/*  Xenbus code for netif backend
    Copyright (C) 2005 Rusty Russell <rusty@rustcorp.com.au>
    Copyright (C) 2005 XenSource Ltd

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
#include <xen/xenbus.h>
#include "common.h"

#if 0
#undef DPRINTK
#define DPRINTK(fmt, args...) \
    printk("netback/xenbus (%s:%d) " fmt ".\n", __FUNCTION__, __LINE__, ##args)
#endif

struct backend_info
{
	struct xenbus_device *dev;
	netif_t *netif;
	struct xenbus_watch backend_watch;
	enum xenbus_state frontend_state;
};

static int connect_rings(struct backend_info *);
static void connect(struct backend_info *);
static void maybe_connect(struct backend_info *);
static void backend_changed(struct xenbus_watch *, const char **,
			    unsigned int);

static int netback_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev->dev.driver_data;

	if (be->backend_watch.node) {
		unregister_xenbus_watch(&be->backend_watch);
		kfree(be->backend_watch.node);
		be->backend_watch.node = NULL;
	}
	if (be->netif) {
		netif_disconnect(be->netif);
		be->netif = NULL;
	}
	kfree(be);
	dev->dev.driver_data = NULL;
	return 0;
}


/**
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures, and watch the store waiting for the hotplug scripts to tell us
 * the device's handle.  Switch to InitWait.
 */
static int netback_probe(struct xenbus_device *dev,
			 const struct xenbus_device_id *id)
{
	const char *message;
	struct xenbus_transaction xbt;
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

	err = xenbus_watch_path2(dev, dev->nodename, "handle",
				 &be->backend_watch, backend_changed);
	if (err)
		goto fail;

	do {
		err = xenbus_transaction_start(&xbt);
		if (err) {
			xenbus_dev_fatal(dev, err, "starting transaction");
			goto fail;
		}

		err = xenbus_printf(xbt, dev->nodename, "feature-sg", "%d", 1);
		if (err) {
			message = "writing feature-sg";
			goto abort_transaction;
		}

#if 0 /* KAF: After the protocol is finalised. */
		err = xenbus_printf(xbt, dev->nodename, "feature-gso-tcpv4",
				    "%d", 1);
		if (err) {
			message = "writing feature-gso-tcpv4";
			goto abort_transaction;
		}
#endif

		err = xenbus_transaction_end(xbt, 0);
	} while (err == -EAGAIN);

	if (err) {
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto fail;
	}

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err) {
		goto fail;
	}

	return 0;

abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "%s", message);
fail:
	DPRINTK("failed");
	netback_remove(dev);
	return err;
}


/**
 * Handle the creation of the hotplug script environment.  We add the script
 * and vif variables to the environment, for the benefit of the vif-* hotplug
 * scripts.
 */
static int netback_uevent(struct xenbus_device *xdev, char **envp,
			  int num_envp, char *buffer, int buffer_size)
{
	struct backend_info *be = xdev->dev.driver_data;
	netif_t *netif = be->netif;
	int i = 0, length = 0;
	char *val;

	DPRINTK("netback_uevent");

	val = xenbus_read(XBT_NIL, xdev->nodename, "script", NULL);
	if (IS_ERR(val)) {
		int err = PTR_ERR(val);
		xenbus_dev_fatal(xdev, err, "reading script");
		return err;
	}
	else {
		add_uevent_var(envp, num_envp, &i, buffer, buffer_size,
			       &length, "script=%s", val);
		kfree(val);
	}

	add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
		       "vif=%s", netif->dev->name);

	envp[i] = NULL;

	return 0;
}


/**
 * Callback received when the hotplug scripts have placed the handle node.
 * Read it, and create a netif structure.  If the frontend is ready, connect.
 */
static void backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	int err;
	long handle;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;

	DPRINTK("");

	err = xenbus_scanf(XBT_NIL, dev->nodename, "handle", "%li", &handle);
	if (XENBUS_EXIST_ERR(err)) {
		/* Since this watch will fire once immediately after it is
		   registered, we expect this.  Ignore it, and wait for the
		   hotplug scripts. */
		return;
	}
	if (err != 1) {
		xenbus_dev_fatal(dev, err, "reading handle");
		return;
	}

	if (be->netif == NULL) {
		u8 be_mac[ETH_ALEN] = { 0, 0, 0, 0, 0, 0 };

		be->netif = netif_alloc(dev->otherend_id, handle, be_mac);
		if (IS_ERR(be->netif)) {
			err = PTR_ERR(be->netif);
			be->netif = NULL;
			xenbus_dev_fatal(dev, err, "creating interface");
			return;
		}

		kobject_uevent(&dev->dev.kobj, KOBJ_ONLINE);

		maybe_connect(be);
	}
}


/**
 * Callback received when the frontend's state changes.
 */
static void frontend_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state)
{
	struct backend_info *be = dev->dev.driver_data;

	DPRINTK("");

	be->frontend_state = frontend_state;

	switch (frontend_state) {
	case XenbusStateInitialising:
	case XenbusStateInitialised:
		break;

	case XenbusStateConnected:
		maybe_connect(be);
		break;

	case XenbusStateClosing:
		xenbus_switch_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		if (be->netif != NULL)
			kobject_uevent(&dev->dev.kobj, KOBJ_OFFLINE);
		device_unregister(&dev->dev);
		break;

	case XenbusStateUnknown:
	case XenbusStateInitWait:
	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}


/* ** Connection ** */


static void maybe_connect(struct backend_info *be)
{
	if (be->netif && (be->frontend_state == XenbusStateConnected))
		connect(be);
}

static void xen_net_read_rate(struct xenbus_device *dev,
			      unsigned long *bytes, unsigned long *usec)
{
	char *s, *e;
	unsigned long b, u;
	char *ratestr;

	/* Default to unlimited bandwidth. */
	*bytes = ~0UL;
	*usec = 0;

	ratestr = xenbus_read(XBT_NIL, dev->nodename, "rate", NULL);
	if (IS_ERR(ratestr))
		return;

	s = ratestr;
	b = simple_strtoul(s, &e, 10);
	if ((s == e) || (*e != ','))
		goto fail;

	s = e + 1;
	u = simple_strtoul(s, &e, 10);
	if ((s == e) || (*e != '\0'))
		goto fail;

	*bytes = b;
	*usec = u;

	kfree(ratestr);
	return;

 fail:
	WPRINTK("Failed to parse network rate limit. Traffic unlimited.\n");
	kfree(ratestr);
}

static int xen_net_read_mac(struct xenbus_device *dev, u8 mac[])
{
	char *s, *e, *macstr;
	int i;

	macstr = s = xenbus_read(XBT_NIL, dev->nodename, "mac", NULL);
	if (IS_ERR(macstr))
		return PTR_ERR(macstr);

	for (i = 0; i < ETH_ALEN; i++) {
		mac[i] = simple_strtoul(s, &e, 16);
		if ((s == e) || (*e != ((i == ETH_ALEN-1) ? '\0' : ':'))) {
			kfree(macstr);
			return -ENOENT;
		}
		s = e+1;
	}

	kfree(macstr);
	return 0;
}

static void connect(struct backend_info *be)
{
	int err;
	struct xenbus_device *dev = be->dev;

	err = connect_rings(be);
	if (err)
		return;

	err = xen_net_read_mac(dev, be->netif->fe_dev_addr);
	if (err) {
		xenbus_dev_fatal(dev, err, "parsing %s/mac", dev->nodename);
		return;
	}

	xen_net_read_rate(dev, &be->netif->credit_bytes,
			  &be->netif->credit_usec);
	be->netif->remaining_credit = be->netif->credit_bytes;

	xenbus_switch_state(dev, XenbusStateConnected);
}


static int connect_rings(struct backend_info *be)
{
	struct xenbus_device *dev = be->dev;
	unsigned long tx_ring_ref, rx_ring_ref;
	unsigned int evtchn;
	int err;
	int val;

	DPRINTK("");

	err = xenbus_gather(XBT_NIL, dev->otherend,
			    "tx-ring-ref", "%lu", &tx_ring_ref,
			    "rx-ring-ref", "%lu", &rx_ring_ref,
			    "event-channel", "%u", &evtchn, NULL);
	if (err) {
		xenbus_dev_fatal(dev, err,
				 "reading %s/ring-ref and event-channel",
				 dev->otherend);
		return err;
	}

	if (xenbus_scanf(XBT_NIL, dev->otherend, "feature-rx-notify", "%d",
			 &val) < 0)
		val = 0;
	if (val)
		be->netif->can_queue = 1;
	else
		/* Must be non-zero for pfifo_fast to work. */
		be->netif->dev->tx_queue_len = 1;

	if (xenbus_scanf(XBT_NIL, dev->otherend, "feature-sg", "%d", &val) < 0)
		val = 0;
	if (val) {
		be->netif->features |= NETIF_F_SG;
		be->netif->dev->features |= NETIF_F_SG;
	}

	/* Map the shared frame, irq etc. */
	err = netif_map(be->netif, tx_ring_ref, rx_ring_ref, evtchn);
	if (err) {
		xenbus_dev_fatal(dev, err,
				 "mapping shared-frames %lu/%lu port %u",
				 tx_ring_ref, rx_ring_ref, evtchn);
		return err;
	}
	return 0;
}


/* ** Driver Registration ** */


static struct xenbus_device_id netback_ids[] = {
	{ "vif" },
	{ "" }
};


static struct xenbus_driver netback = {
	.name = "vif",
	.owner = THIS_MODULE,
	.ids = netback_ids,
	.probe = netback_probe,
	.remove = netback_remove,
	.uevent = netback_uevent,
	.otherend_changed = frontend_changed,
};


void netif_xenbus_init(void)
{
	xenbus_register_backend(&netback);
}
