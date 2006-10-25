/******************************************************************************
 * Talks to Xen Store to figure out what devices we have (backend half).
 *
 * Copyright (C) 2005 Rusty Russell, IBM Corporation
 * Copyright (C) 2005 Mike Wray, Hewlett-Packard
 * Copyright (C) 2005, 2006 XenSource Ltd
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

#define DPRINTK(fmt, args...)				\
	pr_debug("xenbus_probe (%s:%d) " fmt ".\n",	\
		 __FUNCTION__, __LINE__, ##args)

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/kthread.h>

#include <asm/io.h>
#include <asm/page.h>
#include <asm/maddr.h>
#include <asm/pgtable.h>
#include <asm/hypervisor.h>
#include <xen/xenbus.h>
#include <xen/xen_proc.h>
#include <xen/evtchn.h>
#include <xen/features.h>
#include <xen/hvm.h>

#include "xenbus_comms.h"
#include "xenbus_probe.h"

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

static int xenbus_uevent_backend(struct device *dev, char **envp,
				 int num_envp, char *buffer, int buffer_size);
static int xenbus_probe_backend(const char *type, const char *domid);

extern int read_otherend_details(struct xenbus_device *xendev,
				 char *id_node, char *path_node);

static int read_frontend_details(struct xenbus_device *xendev)
{
	return read_otherend_details(xendev, "frontend-id", "frontend");
}

/* backend/<type>/<fe-uuid>/<id> => <type>-<fe-domid>-<id> */
static int backend_bus_id(char bus_id[BUS_ID_SIZE], const char *nodename)
{
	int domid, err;
	const char *devid, *type, *frontend;
	unsigned int typelen;

	type = strchr(nodename, '/');
	if (!type)
		return -EINVAL;
	type++;
	typelen = strcspn(type, "/");
	if (!typelen || type[typelen] != '/')
		return -EINVAL;

	devid = strrchr(nodename, '/') + 1;

	err = xenbus_gather(XBT_NIL, nodename, "frontend-id", "%i", &domid,
			    "frontend", NULL, &frontend,
			    NULL);
	if (err)
		return err;
	if (strlen(frontend) == 0)
		err = -ERANGE;
	if (!err && !xenbus_exists(XBT_NIL, frontend, ""))
		err = -ENOENT;
	kfree(frontend);

	if (err)
		return err;

	if (snprintf(bus_id, BUS_ID_SIZE,
		     "%.*s-%i-%s", typelen, type, domid, devid) >= BUS_ID_SIZE)
		return -ENOSPC;
	return 0;
}

static struct xen_bus_type xenbus_backend = {
	.root = "backend",
	.levels = 3, 		/* backend/type/<frontend>/<id> */
	.get_bus_id = backend_bus_id,
	.probe = xenbus_probe_backend,
	.bus = {
		.name     = "xen-backend",
		.match    = xenbus_match,
		.probe    = xenbus_dev_probe,
		.remove   = xenbus_dev_remove,
//		.shutdown = xenbus_dev_shutdown,
		.uevent   = xenbus_uevent_backend,
	},
	.dev = {
		.bus_id = "xen-backend",
	},
};

static int xenbus_uevent_backend(struct device *dev, char **envp,
				 int num_envp, char *buffer, int buffer_size)
{
	struct xenbus_device *xdev;
	struct xenbus_driver *drv;
	int i = 0;
	int length = 0;

	DPRINTK("");

	if (dev == NULL)
		return -ENODEV;

	xdev = to_xenbus_device(dev);
	if (xdev == NULL)
		return -ENODEV;

	/* stuff we want to pass to /sbin/hotplug */
	add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
		       "XENBUS_TYPE=%s", xdev->devicetype);

	add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
		       "XENBUS_PATH=%s", xdev->nodename);

	add_uevent_var(envp, num_envp, &i, buffer, buffer_size, &length,
		       "XENBUS_BASE_PATH=%s", xenbus_backend.root);

	/* terminate, set to next free slot, shrink available space */
	envp[i] = NULL;
	envp = &envp[i];
	num_envp -= i;
	buffer = &buffer[length];
	buffer_size -= length;

	if (dev->driver) {
		drv = to_xenbus_driver(dev->driver);
		if (drv && drv->uevent)
			return drv->uevent(xdev, envp, num_envp, buffer,
					   buffer_size);
	}

	return 0;
}

int xenbus_register_backend(struct xenbus_driver *drv)
{
	drv->read_otherend_details = read_frontend_details;

	return xenbus_register_driver_common(drv, &xenbus_backend);
}
EXPORT_SYMBOL_GPL(xenbus_register_backend);

/* backend/<typename>/<frontend-uuid>/<name> */
static int xenbus_probe_backend_unit(const char *dir,
				     const char *type,
				     const char *name)
{
	char *nodename;
	int err;

	nodename = kasprintf("%s/%s", dir, name);
	if (!nodename)
		return -ENOMEM;

	DPRINTK("%s\n", nodename);

	err = xenbus_probe_node(&xenbus_backend, type, nodename);
	kfree(nodename);
	return err;
}

/* backend/<typename>/<frontend-domid> */
static int xenbus_probe_backend(const char *type, const char *domid)
{
	char *nodename;
	int err = 0;
	char **dir;
	unsigned int i, dir_n = 0;

	DPRINTK("");

	nodename = kasprintf("%s/%s/%s", xenbus_backend.root, type, domid);
	if (!nodename)
		return -ENOMEM;

	dir = xenbus_directory(XBT_NIL, nodename, "", &dir_n);
	if (IS_ERR(dir)) {
		kfree(nodename);
		return PTR_ERR(dir);
	}

	for (i = 0; i < dir_n; i++) {
		err = xenbus_probe_backend_unit(nodename, type, dir[i]);
		if (err)
			break;
	}
	kfree(dir);
	kfree(nodename);
	return err;
}

static void backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	DPRINTK("");

	dev_changed(vec[XS_WATCH_PATH], &xenbus_backend);
}

static struct xenbus_watch be_watch = {
	.node = "backend",
	.callback = backend_changed,
};

void xenbus_backend_suspend(int (*fn)(struct device *, void *))
{
	DPRINTK("");
	bus_for_each_dev(&xenbus_backend.bus, NULL, NULL, fn);
}

void xenbus_backend_resume(int (*fn)(struct device *, void *))
{
	DPRINTK("");
	bus_for_each_dev(&xenbus_backend.bus, NULL, NULL, fn);
}

void xenbus_backend_probe_and_watch(void)
{
	xenbus_probe_devices(&xenbus_backend);
	register_xenbus_watch(&be_watch);
}

void xenbus_backend_bus_register(void)
{
	bus_register(&xenbus_backend.bus);
}

void xenbus_backend_device_register(void)
{
	device_register(&xenbus_backend.dev);
}
