#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <xenctrl.h>
#include <xen/io/xenbus.h>
#include <xen/io/fbif.h>
#include <xen/io/kbdif.h>
#include <xen/io/protocols.h>
#include <stdbool.h>
#include <xen/event_channel.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <xs.h>
#include <linux/input.h>

#include "xenfb.h"

// FIXME defend against malicious frontend?

struct xenfb;

struct xenfb_device {
	const char *devicetype;
	char nodename[64];	/* backend xenstore dir */
	char otherend[64];	/* frontend xenstore dir */
	int otherend_id;	/* frontend domid */
	enum xenbus_state state; /* backend state */
	void *page;		/* shared page */
	evtchn_port_t port;
	struct xenfb *xenfb;
};

struct xenfb {
	DisplayState *ds;       /* QEMU graphical console state */
	int evt_xch;		/* event channel driver handle */
	int xc;			/* hypervisor interface handle */
	struct xs_handle *xsh;	/* xs daemon handle */
	struct xenfb_device fb, kbd;
	void *pixels;           /* guest framebuffer data */
	size_t fb_len;		/* size of framebuffer */
	int row_stride;         /* width of one row in framebuffer */
	int depth;              /* colour depth of guest framebuffer */
	int width;              /* pixel width of guest framebuffer */
	int height;             /* pixel height of guest framebuffer */
	int abs_pointer_wanted; /* Whether guest supports absolute pointer */
	int button_state;       /* Last seen pointer button state */
	char protocol[64];	/* frontend protocol */
};

/* Functions which tie the PVFB into the QEMU device model */
static void xenfb_key_event(void *opaque, int keycode);
static void xenfb_mouse_event(void *opaque,
			      int dx, int dy, int dz, int button_state);
static void xenfb_guest_copy(struct xenfb *xenfb, int x, int y, int w, int h);
static void xenfb_update(void *opaque);
static void xenfb_invalidate(void *opaque);
static void xenfb_screen_dump(void *opaque, const char *name);

/*
 * Tables to map from scancode to Linux input layer keycode.
 * Scancodes are hardware-specific.  These maps assumes a 
 * standard AT or PS/2 keyboard which is what QEMU feeds us.
 */
static const unsigned char atkbd_set2_keycode[512] = {

	  0, 67, 65, 63, 61, 59, 60, 88,  0, 68, 66, 64, 62, 15, 41,117,
	  0, 56, 42, 93, 29, 16,  2,  0,  0,  0, 44, 31, 30, 17,  3,  0,
	  0, 46, 45, 32, 18,  5,  4, 95,  0, 57, 47, 33, 20, 19,  6,183,
	  0, 49, 48, 35, 34, 21,  7,184,  0,  0, 50, 36, 22,  8,  9,185,
	  0, 51, 37, 23, 24, 11, 10,  0,  0, 52, 53, 38, 39, 25, 12,  0,
	  0, 89, 40,  0, 26, 13,  0,  0, 58, 54, 28, 27,  0, 43,  0, 85,
	  0, 86, 91, 90, 92,  0, 14, 94,  0, 79,124, 75, 71,121,  0,  0,
	 82, 83, 80, 76, 77, 72,  1, 69, 87, 78, 81, 74, 55, 73, 70, 99,

	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	217,100,255,  0, 97,165,  0,  0,156,  0,  0,  0,  0,  0,  0,125,
	173,114,  0,113,  0,  0,  0,126,128,  0,  0,140,  0,  0,  0,127,
	159,  0,115,  0,164,  0,  0,116,158,  0,150,166,  0,  0,  0,142,
	157,  0,  0,  0,  0,  0,  0,  0,155,  0, 98,  0,  0,163,  0,  0,
	226,  0,  0,  0,  0,  0,  0,  0,  0,255, 96,  0,  0,  0,143,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,107,  0,105,102,  0,  0,112,
	110,111,108,112,106,103,  0,119,  0,118,109,  0, 99,104,119,  0,

};

static const unsigned char atkbd_unxlate_table[128] = {

	  0,118, 22, 30, 38, 37, 46, 54, 61, 62, 70, 69, 78, 85,102, 13,
	 21, 29, 36, 45, 44, 53, 60, 67, 68, 77, 84, 91, 90, 20, 28, 27,
	 35, 43, 52, 51, 59, 66, 75, 76, 82, 14, 18, 93, 26, 34, 33, 42,
	 50, 49, 58, 65, 73, 74, 89,124, 17, 41, 88,  5,  6,  4, 12,  3,
	 11,  2, 10,  1,  9,119,126,108,117,125,123,107,115,116,121,105,
	114,122,112,113,127, 96, 97,120,  7, 15, 23, 31, 39, 47, 55, 63,
	 71, 79, 86, 94,  8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 87,111,
	 19, 25, 57, 81, 83, 92, 95, 98, 99,100,101,103,104,106,109,110

};

static unsigned char scancode2linux[512];


static void xenfb_detach_dom(struct xenfb *);

static char *xenfb_path_in_dom(struct xs_handle *xsh,
			       char *buf, size_t size,
			       unsigned domid, const char *fmt, ...)
{
	va_list ap;
	char *domp = xs_get_domain_path(xsh, domid);
	int n;

        if (domp == NULL)
		return NULL;

	n = snprintf(buf, size, "%s/", domp);
	free(domp);
	if (n >= size)
		return NULL;

	va_start(ap, fmt);
	n += vsnprintf(buf + n, size - n, fmt, ap);
	va_end(ap);
	if (n >= size)
		return NULL;

	return buf;
}

static int xenfb_xs_scanf1(struct xs_handle *xsh,
			   const char *dir, const char *node,
			   const char *fmt, void *dest)
{
	char buf[1024];
	char *p;
	int ret;

	if (snprintf(buf, sizeof(buf), "%s/%s", dir, node) >= sizeof(buf)) {
		errno = ENOENT;
		return -1;
        }
	p = xs_read(xsh, XBT_NULL, buf, NULL);
	if (!p) {
		errno = ENOENT;
		return -1;
        }
	ret = sscanf(p, fmt, dest);
	free(p);
	if (ret != 1) {
		errno = EDOM;
		return -1;
        }
	return ret;
}

static int xenfb_xs_printf(struct xs_handle *xsh,
			   const char *dir, const char *node, char *fmt, ...)
{
	va_list ap;
	char key[1024];
	char val[1024];
	int n;

	if (snprintf(key, sizeof(key), "%s/%s", dir, node) >= sizeof(key)) {
		errno = ENOENT;
		return -1;
        }

	va_start(ap, fmt);
	n = vsnprintf(val, sizeof(val), fmt, ap);
	va_end(ap);
	if (n >= sizeof(val)) {
		errno = ENOSPC; /* close enough */
		return -1;
	}

	if (!xs_write(xsh, XBT_NULL, key, val, n))
		return -1;
	return 0;
}

static void xenfb_device_init(struct xenfb_device *dev,
			      const char *type,
			      struct xenfb *xenfb)
{
	dev->devicetype = type;
	dev->otherend_id = -1;
	dev->port = -1;
	dev->xenfb = xenfb;
}

static int xenfb_device_set_domain(struct xenfb_device *dev, int domid)
{
	dev->otherend_id = domid;

	if (!xenfb_path_in_dom(dev->xenfb->xsh,
			       dev->otherend, sizeof(dev->otherend),
			       domid, "device/%s/0", dev->devicetype)) {
		errno = ENOENT;
		return -1;
	}
	if (!xenfb_path_in_dom(dev->xenfb->xsh,
			       dev->nodename, sizeof(dev->nodename),
			       0, "backend/%s/%d/0", dev->devicetype, domid)) {
		errno = ENOENT;
		return -1;
	}

	return 0;
}

struct xenfb *xenfb_new(void)
{
	struct xenfb *xenfb = qemu_malloc(sizeof(struct xenfb));
	int serrno;
	int i;

	if (xenfb == NULL)
		return NULL;

	/* Prepare scancode mapping table */
	for (i = 0; i < 128; i++) {
		scancode2linux[i] = atkbd_set2_keycode[atkbd_unxlate_table[i]];
		scancode2linux[i | 0x80] = 
			atkbd_set2_keycode[atkbd_unxlate_table[i] | 0x80];
	}

	memset(xenfb, 0, sizeof(*xenfb));
	xenfb->evt_xch = xenfb->xc = -1;
	xenfb_device_init(&xenfb->fb, "vfb", xenfb);
	xenfb_device_init(&xenfb->kbd, "vkbd", xenfb);

	xenfb->evt_xch = xc_evtchn_open();
	if (xenfb->evt_xch == -1)
		goto fail;

	xenfb->xc = xc_interface_open();
	if (xenfb->xc == -1)
		goto fail;

	xenfb->xsh = xs_daemon_open();
	if (!xenfb->xsh)
		goto fail;

	return xenfb;

 fail:
	serrno = errno;
	xenfb_delete(xenfb);
	errno = serrno;
	return NULL;
}

/* Remove the backend area in xenbus since the framebuffer really is
   going away. */
void xenfb_teardown(struct xenfb *xenfb)
{
       xs_rm(xenfb->xsh, XBT_NULL, xenfb->fb.nodename);
       xs_rm(xenfb->xsh, XBT_NULL, xenfb->kbd.nodename);
}


void xenfb_delete(struct xenfb *xenfb)
{
	xenfb_detach_dom(xenfb);
	if (xenfb->xc >= 0)
		xc_interface_close(xenfb->xc);
	if (xenfb->evt_xch >= 0)
		xc_evtchn_close(xenfb->evt_xch);
	if (xenfb->xsh)
		xs_daemon_close(xenfb->xsh);
	free(xenfb);
}

static enum xenbus_state xenfb_read_state(struct xs_handle *xsh,
					  const char *dir)
{
	int ret, state;

	ret = xenfb_xs_scanf1(xsh, dir, "state", "%d", &state);
	if (ret < 0)
		return XenbusStateUnknown;

	if ((unsigned)state > XenbusStateClosed)
		state = XenbusStateUnknown;
	return state;
}

static int xenfb_switch_state(struct xenfb_device *dev,
			      enum xenbus_state state)
{
	struct xs_handle *xsh = dev->xenfb->xsh;

	if (xenfb_xs_printf(xsh, dev->nodename, "state", "%d", state) < 0)
		return -1;
	dev->state = state;
	return 0;
}

static int xenfb_wait_for_state(struct xs_handle *xsh, const char *dir,
				unsigned awaited)
{
	unsigned state, dummy;
	char **vec;

	awaited |= 1 << XenbusStateUnknown;

	for (;;) {
		state = xenfb_read_state(xsh, dir);
		if ((1 << state) & awaited)
			return state;

		vec = xs_read_watch(xsh, &dummy);
		if (!vec)
			return -1;
		free(vec);
	}
}

static int xenfb_wait_for_backend_creation(struct xenfb_device *dev)
{
	struct xs_handle *xsh = dev->xenfb->xsh;
	int state;

	if (!xs_watch(xsh, dev->nodename, ""))
		return -1;
	state = xenfb_wait_for_state(xsh, dev->nodename,
			(1 << XenbusStateInitialising)
			| (1 << XenbusStateClosed)
#if 1 /* TODO fudging state to permit restarting; to be removed */
			| (1 << XenbusStateInitWait)
			| (1 << XenbusStateConnected)
			| (1 << XenbusStateClosing)
#endif
			);
	xs_unwatch(xsh, dev->nodename, "");

	switch (state) {
#if 1
	case XenbusStateInitWait:
	case XenbusStateConnected:
		printf("Fudging state to %d\n", XenbusStateInitialising); /* FIXME */
#endif
	case XenbusStateInitialising:
	case XenbusStateClosing:
	case XenbusStateClosed:
		break;
	default:
		return -1;
	}

	return 0;
}

static int xenfb_hotplug(struct xenfb_device *dev)
{
	if (xenfb_xs_printf(dev->xenfb->xsh, dev->nodename,
			    "hotplug-status", "connected"))
		return -1;
	return 0;
}

static int xenfb_wait_for_frontend_initialised(struct xenfb_device *dev)
{
	switch (xenfb_wait_for_state(dev->xenfb->xsh, dev->otherend,
#if 1 /* TODO fudging state to permit restarting; to be removed */
			(1 << XenbusStateInitialised)
			| (1 << XenbusStateConnected)
#else
			1 << XenbusStateInitialised,
#endif
			)) {
#if 1
	case XenbusStateConnected:
		printf("Fudging state to %d\n", XenbusStateInitialised); /* FIXME */
#endif
	case XenbusStateInitialised:
		break;
	default:
		return -1;
	}

	return 0;
}

static void xenfb_copy_mfns(int mode, int count, unsigned long *dst, void *src)
{
	uint32_t *src32 = src;
	uint64_t *src64 = src;
	int i;

	for (i = 0; i < count; i++)
		dst[i] = (mode == 32) ? src32[i] : src64[i];
}

static int xenfb_map_fb(struct xenfb *xenfb, int domid)
{
	struct xenfb_page *page = xenfb->fb.page;
	int n_fbmfns;
	int n_fbdirs;
	unsigned long *pgmfns = NULL;
	unsigned long *fbmfns = NULL;
	void *map, *pd;
	int mode, ret = -1;

	/* default to native */
	pd = page->pd;
	mode = sizeof(unsigned long) * 8;

	if (0 == strlen(xenfb->protocol)) {
		/*
		 * Undefined protocol, some guesswork needed.
		 *
		 * Old frontends which don't set the protocol use
		 * one page directory only, thus pd[1] must be zero.
		 * pd[1] of the 32bit struct layout and the lower
		 * 32 bits of pd[0] of the 64bit struct layout have
		 * the same location, so we can check that ...
		 */
		uint32_t *ptr32 = NULL;
		uint32_t *ptr64 = NULL;
#if defined(__i386__)
		ptr32 = (void*)page->pd;
		ptr64 = ((void*)page->pd) + 4;
#elif defined(__x86_64__)
		ptr32 = ((void*)page->pd) - 4;
		ptr64 = (void*)page->pd;
#endif
		if (ptr32) {
			if (0 == ptr32[1]) {
				mode = 32;
				pd   = ptr32;
			} else {
				mode = 64;
				pd   = ptr64;
			}
		}
#if defined(__x86_64__)
	} else if (0 == strcmp(xenfb->protocol, XEN_IO_PROTO_ABI_X86_32)) {
		/* 64bit dom0, 32bit domU */
		mode = 32;
		pd   = ((void*)page->pd) - 4;
#elif defined(__i386__)
	} else if (0 == strcmp(xenfb->protocol, XEN_IO_PROTO_ABI_X86_64)) {
		/* 32bit dom0, 64bit domU */
		mode = 64;
		pd   = ((void*)page->pd) + 4;
#endif
	}

	n_fbmfns = (xenfb->fb_len + (XC_PAGE_SIZE - 1)) / XC_PAGE_SIZE;
	n_fbdirs = n_fbmfns * mode / 8;
	n_fbdirs = (n_fbdirs + (XC_PAGE_SIZE - 1)) / XC_PAGE_SIZE;

	pgmfns = malloc(sizeof(unsigned long) * n_fbdirs);
	fbmfns = malloc(sizeof(unsigned long) * n_fbmfns);
	if (!pgmfns || !fbmfns)
		goto out;

	xenfb_copy_mfns(mode, n_fbdirs, pgmfns, pd);
	map = xc_map_foreign_pages(xenfb->xc, domid,
				   PROT_READ, pgmfns, n_fbdirs);
	if (map == NULL)
		goto out;
	xenfb_copy_mfns(mode, n_fbmfns, fbmfns, map);
	munmap(map, n_fbdirs * XC_PAGE_SIZE);

	xenfb->pixels = xc_map_foreign_pages(xenfb->xc, domid,
				PROT_READ | PROT_WRITE, fbmfns, n_fbmfns);
	if (xenfb->pixels == NULL)
		goto out;

	ret = 0; /* all is fine */

 out:
	if (pgmfns)
		free(pgmfns);
	if (fbmfns)
		free(fbmfns);
	return ret;
}

static int xenfb_bind(struct xenfb_device *dev)
{
	struct xenfb *xenfb = dev->xenfb;
	unsigned long mfn;
	evtchn_port_t evtchn;

	if (xenfb_xs_scanf1(xenfb->xsh, dev->otherend, "page-ref", "%lu",
			    &mfn) < 0)
		return -1;
	if (xenfb_xs_scanf1(xenfb->xsh, dev->otherend, "event-channel", "%u",
			    &evtchn) < 0)
		return -1;

	dev->port = xc_evtchn_bind_interdomain(xenfb->evt_xch,
					       dev->otherend_id, evtchn);
	if (dev->port == -1)
		return -1;

	dev->page = xc_map_foreign_range(xenfb->xc, dev->otherend_id,
			XC_PAGE_SIZE, PROT_READ | PROT_WRITE, mfn);
	if (dev->page == NULL)
		return -1;

	return 0;
}

static void xenfb_unbind(struct xenfb_device *dev)
{
	if (dev->page) {
		munmap(dev->page, XC_PAGE_SIZE);
		dev->page = NULL;
	}
        if (dev->port >= 0) {
		xc_evtchn_unbind(dev->xenfb->evt_xch, dev->port);
		dev->port = -1;
	}
}

static int xenfb_wait_for_frontend_connected(struct xenfb_device *dev)
{
	switch (xenfb_wait_for_state(dev->xenfb->xsh, dev->otherend,
				     1 << XenbusStateConnected)) {
	case XenbusStateConnected:
		break;
	default:
		return -1;
	}

	return 0;
}

static void xenfb_dev_fatal(struct xenfb_device *dev, int err,
			    const char *fmt, ...)
{
	struct xs_handle *xsh = dev->xenfb->xsh;
	va_list ap;
	char errdir[80];
	char buf[1024];
	int n;

	fprintf(stderr, "%s ", dev->nodename); /* somewhat crude */
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (err)
		fprintf(stderr, " (%s)", strerror(err));
	putc('\n', stderr);

	if (!xenfb_path_in_dom(xsh, errdir, sizeof(errdir), 0,
			       "error/%s", dev->nodename))
		goto out;	/* FIXME complain */

	va_start(ap, fmt);
	n = snprintf(buf, sizeof(buf), "%d ", err);
	snprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);

	if (xenfb_xs_printf(xsh, buf, "error", "%s", buf) < 0)
		goto out;	/* FIXME complain */

 out:
	xenfb_switch_state(dev, XenbusStateClosing);
}


static void xenfb_detach_dom(struct xenfb *xenfb)
{
	xenfb_unbind(&xenfb->fb);
	xenfb_unbind(&xenfb->kbd);
	if (xenfb->pixels) {
		munmap(xenfb->pixels, xenfb->fb_len);
		xenfb->pixels = NULL;
	}
}


static void xenfb_on_fb_event(struct xenfb *xenfb)
{
	uint32_t prod, cons;
	struct xenfb_page *page = xenfb->fb.page;

	prod = page->out_prod;
	if (prod == page->out_cons)
		return;
	rmb();			/* ensure we see ring contents up to prod */
	for (cons = page->out_cons; cons != prod; cons++) {
		union xenfb_out_event *event = &XENFB_OUT_RING_REF(page, cons);

		switch (event->type) {
		case XENFB_TYPE_UPDATE:
			xenfb_guest_copy(xenfb,
					 event->update.x, event->update.y,
					 event->update.width, event->update.height);
			break;
		}
	}
	mb();			/* ensure we're done with ring contents */
	page->out_cons = cons;
	xc_evtchn_notify(xenfb->evt_xch, xenfb->fb.port);
}

static void xenfb_on_kbd_event(struct xenfb *xenfb)
{
	struct xenkbd_page *page = xenfb->kbd.page;

	/* We don't understand any keyboard events, so just ignore them. */
	if (page->out_prod == page->out_cons)
		return;
	page->out_cons = page->out_prod;
	xc_evtchn_notify(xenfb->evt_xch, xenfb->kbd.port);
}

static int xenfb_on_state_change(struct xenfb_device *dev)
{
	enum xenbus_state state;

	state = xenfb_read_state(dev->xenfb->xsh, dev->otherend);

	switch (state) {
	case XenbusStateUnknown:
		/* There was an error reading the frontend state.  The
		   domain has probably gone away; in any case, there's
		   not much point in us continuing. */
		return -1;
	case XenbusStateInitialising:
	case XenbusStateInitWait:
	case XenbusStateInitialised:
	case XenbusStateConnected:
		break;
	case XenbusStateClosing:
		xenfb_unbind(dev);
		xenfb_switch_state(dev, state);
		break;
	case XenbusStateClosed:
		xenfb_switch_state(dev, state);
	}
	return 0;
}

static int xenfb_kbd_event(struct xenfb *xenfb,
			   union xenkbd_in_event *event)
{
	uint32_t prod;
	struct xenkbd_page *page = xenfb->kbd.page;

	if (xenfb->kbd.state != XenbusStateConnected)
		return 0;

	prod = page->in_prod;
	if (prod - page->in_cons == XENKBD_IN_RING_LEN) {
		errno = EAGAIN;
		return -1;
	}

	mb();			/* ensure ring space available */
	XENKBD_IN_RING_REF(page, prod) = *event;
	wmb();			/* ensure ring contents visible */
	page->in_prod = prod + 1;
	return xc_evtchn_notify(xenfb->evt_xch, xenfb->kbd.port);
}

static int xenfb_send_key(struct xenfb *xenfb, bool down, int keycode)
{
	union xenkbd_in_event event;

	memset(&event, 0, XENKBD_IN_EVENT_SIZE);
	event.type = XENKBD_TYPE_KEY;
	event.key.pressed = down ? 1 : 0;
	event.key.keycode = keycode;

	return xenfb_kbd_event(xenfb, &event);
}

static int xenfb_send_motion(struct xenfb *xenfb, int rel_x, int rel_y)
{
	union xenkbd_in_event event;

	memset(&event, 0, XENKBD_IN_EVENT_SIZE);
	event.type = XENKBD_TYPE_MOTION;
	event.motion.rel_x = rel_x;
	event.motion.rel_y = rel_y;

	return xenfb_kbd_event(xenfb, &event);
}

static int xenfb_send_position(struct xenfb *xenfb, int abs_x, int abs_y)
{
	union xenkbd_in_event event;

	memset(&event, 0, XENKBD_IN_EVENT_SIZE);
	event.type = XENKBD_TYPE_POS;
	event.pos.abs_x = abs_x;
	event.pos.abs_y = abs_y;

	return xenfb_kbd_event(xenfb, &event);
}


static void xenfb_dispatch_channel(void *opaque)
{
	struct xenfb *xenfb = (struct xenfb *)opaque;
	evtchn_port_t port;
	port = xc_evtchn_pending(xenfb->evt_xch);
	if (port == -1)
		exit(1);

	if (port == xenfb->fb.port)
		xenfb_on_fb_event(xenfb);
	else if (port == xenfb->kbd.port)
		xenfb_on_kbd_event(xenfb);

	if (xc_evtchn_unmask(xenfb->evt_xch, port) == -1)
		exit(1);
}

static void xenfb_dispatch_store(void *opaque)
{
	struct xenfb *xenfb = (struct xenfb *)opaque;
	unsigned dummy;
	char **vec;
	int r;

	vec = xs_read_watch(xenfb->xsh, &dummy);
	free(vec);
	r = xenfb_on_state_change(&xenfb->fb);
	if (r == 0)
		r = xenfb_on_state_change(&xenfb->kbd);
	if (r == -1)
		exit(1);
}


int xenfb_attach_dom(struct xenfb *xenfb, int domid, DisplayState *ds)
{
	struct xs_handle *xsh = xenfb->xsh;
	int val, serrno;
	struct xenfb_page *fb_page;

	xenfb_detach_dom(xenfb);

	xenfb_device_set_domain(&xenfb->fb, domid);
	xenfb_device_set_domain(&xenfb->kbd, domid);

	if (xenfb_wait_for_backend_creation(&xenfb->fb) < 0)
		goto error;
	if (xenfb_wait_for_backend_creation(&xenfb->kbd) < 0)
		goto error;

	if (xenfb_xs_printf(xsh, xenfb->kbd.nodename, "feature-abs-pointer", "1"))
		goto error;
	if (xenfb_switch_state(&xenfb->fb, XenbusStateInitWait))
		goto error;
	if (xenfb_switch_state(&xenfb->kbd, XenbusStateInitWait))
		goto error;

	if (xenfb_hotplug(&xenfb->fb) < 0)
		goto error;
	if (xenfb_hotplug(&xenfb->kbd) < 0)
		goto error;

	if (!xs_watch(xsh, xenfb->fb.otherend, ""))
		goto error;
	if (!xs_watch(xsh, xenfb->kbd.otherend, ""))
		goto error;

	if (xenfb_wait_for_frontend_initialised(&xenfb->fb) < 0)
		goto error;
	if (xenfb_wait_for_frontend_initialised(&xenfb->kbd) < 0)
		goto error;

	if (xenfb_bind(&xenfb->fb) < 0)
		goto error;
	if (xenfb_bind(&xenfb->kbd) < 0)
		goto error;

	if (xenfb_xs_scanf1(xsh, xenfb->fb.otherend, "feature-update",
			    "%d", &val) < 0)
		val = 0;
	if (!val) {
		errno = ENOTSUP;
		goto error;
	}
	if (xenfb_xs_scanf1(xsh, xenfb->fb.otherend, "protocol", "%63s",
			    xenfb->protocol) < 0)
		xenfb->protocol[0] = '\0';
	xenfb_xs_printf(xsh, xenfb->fb.nodename, "request-update", "1");

	/* TODO check for permitted ranges */
	fb_page = xenfb->fb.page;
	xenfb->depth = fb_page->depth;
	xenfb->width = fb_page->width;
	xenfb->height = fb_page->height;
	/* TODO check for consistency with the above */
	xenfb->fb_len = fb_page->mem_length;
	xenfb->row_stride = fb_page->line_length;

	if (xenfb_map_fb(xenfb, domid) < 0)
		goto error;

	if (xenfb_switch_state(&xenfb->fb, XenbusStateConnected))
		goto error;
	if (xenfb_switch_state(&xenfb->kbd, XenbusStateConnected))
		goto error;

	if (xenfb_wait_for_frontend_connected(&xenfb->kbd) < 0)
		goto error;
	if (xenfb_xs_scanf1(xsh, xenfb->kbd.otherend, "request-abs-pointer",
			    "%d", &val) < 0)
		val = 0;
	xenfb->abs_pointer_wanted = val;

	/* Listen for events from xenstore */
	if (qemu_set_fd_handler2(xs_fileno(xenfb->xsh), NULL, xenfb_dispatch_store, NULL, xenfb) < 0)
		goto error;

	/* Listen for events from the event channel */
	if (qemu_set_fd_handler2(xc_evtchn_fd(xenfb->evt_xch), NULL, xenfb_dispatch_channel, NULL, xenfb) < 0)
		goto error;

	/* Register our keyboard & mouse handlers */
	qemu_add_kbd_event_handler(xenfb_key_event, xenfb);
	qemu_add_mouse_event_handler(xenfb_mouse_event, xenfb,
				     xenfb->abs_pointer_wanted,
				     "Xen PVFB Mouse");

	xenfb->ds = ds;

	/* Tell QEMU to allocate a graphical console */
	graphic_console_init(ds,
			     xenfb_update,
			     xenfb_invalidate,
			     xenfb_screen_dump,
			     xenfb);
	dpy_resize(ds, xenfb->width, xenfb->height);

	return 0;

 error:
	serrno = errno;
	xenfb_detach_dom(xenfb);
	xenfb_dev_fatal(&xenfb->fb, serrno, "on fire");
	xenfb_dev_fatal(&xenfb->kbd, serrno, "on fire");
        errno = serrno;
        return -1;
}

/* 
 * Send a key event from the client to the guest OS
 * QEMU gives us a raw scancode from an AT / PS/2 style keyboard.
 * We have to turn this into a Linux Input layer keycode.
 * 
 * Extra complexity from the fact that with extended scancodes 
 * (like those produced by arrow keys) this method gets called
 * twice, but we only want to send a single event. So we have to
 * track the '0xe0' scancode state & collapse the extended keys
 * as needed.
 * 
 * Wish we could just send scancodes straight to the guest which
 * already has code for dealing with this...
 */
static void xenfb_key_event(void *opaque, int scancode)
{
    static int extended = 0;
    int down = 1;
    if (scancode == 0xe0) {
        extended = 1;
        return;
    } else if (scancode & 0x80) {
        scancode &= 0x7f;
        down = 0;
    }
    if (extended) {
        scancode |= 0x80;
        extended = 0;
    }
    xenfb_send_key(opaque, down, scancode2linux[scancode]);
}

/*
 * Send a mouse event from the client to the guest OS
 * 
 * The QEMU mouse can be in either relative, or absolute mode.
 * Movement is sent separately from button state, which has to
 * be encoded as virtual key events. We also don't actually get
 * given any button up/down events, so have to track changes in
 * the button state.
 */
static void xenfb_mouse_event(void *opaque,
			      int dx, int dy, int dz, int button_state)
{
    int i;
    struct xenfb *xenfb = opaque;
    if (xenfb->abs_pointer_wanted)
	    xenfb_send_position(xenfb,
				dx * xenfb->ds->width / 0x7fff,
				dy * xenfb->ds->height / 0x7fff);
    else
	    xenfb_send_motion(xenfb, dx, dy);

    for (i = 0 ; i < 8 ; i++) {
	    int lastDown = xenfb->button_state & (1 << i);
	    int down = button_state & (1 << i);
	    if (down == lastDown)
		    continue;

	    if (xenfb_send_key(xenfb, down, BTN_LEFT+i) < 0)
		    return;
    }
    xenfb->button_state = button_state;
}

/* A convenient function for munging pixels between different depths */
#define BLT(SRC_T,DST_T,RLS,GLS,BLS,RRS,GRS,BRS,RM,GM,BM)               \
    for (line = y ; line < h ; line++) {                                \
        SRC_T *src = (SRC_T *)(xenfb->pixels                            \
                               + (line * xenfb->row_stride)             \
                               + (x * xenfb->depth / 8));               \
        DST_T *dst = (DST_T *)(xenfb->ds->data                                 \
                               + (line * xenfb->ds->linesize)                  \
                               + (x * xenfb->ds->depth / 8));                  \
        int col;                                                        \
        for (col = x ; col < w ; col++) {                               \
            *dst = (((*src >> RRS) & RM) << RLS) |                      \
                (((*src >> GRS) & GM) << GLS) |                         \
                (((*src >> GRS) & BM) << BLS);                          \
            src++;                                                      \
            dst++;                                                      \
        }                                                               \
    }


/* This copies data from the guest framebuffer region, into QEMU's copy
 * NB. QEMU's copy is stored in the pixel format of a) the local X 
 * server (SDL case) or b) the current VNC client pixel format.
 * When shifting between colour depths we preserve the MSB.
 */
static void xenfb_guest_copy(struct xenfb *xenfb, int x, int y, int w, int h)
{
    int line;

    if (xenfb->depth == xenfb->ds->depth) { /* Perfect match can use fast path */
        for (line = y ; line < (y+h) ; line++) {
            memcpy(xenfb->ds->data + (line * xenfb->ds->linesize) + (x * xenfb->ds->depth / 8),
                   xenfb->pixels + (line * xenfb->row_stride) + (x * xenfb->depth / 8),
                   w * xenfb->depth / 8);
        }
    } else { /* Mismatch requires slow pixel munging */
        if (xenfb->depth == 8) {
            /* 8 bit source == r:3 g:3 b:2 */
            if (xenfb->ds->depth == 16) {
                BLT(uint8_t, uint16_t,   5, 2, 0,   11, 5, 0,   7, 7, 3);
            } else if (xenfb->ds->depth == 32) {
                BLT(uint8_t, uint32_t,   5, 2, 0,   16, 8, 0,   7, 7, 3);
            }
        } else if (xenfb->depth == 16) {
            /* 16 bit source == r:5 g:6 b:5 */
            if (xenfb->ds->depth == 8) {
                BLT(uint16_t, uint8_t,    11, 5, 0,   5, 2, 0,    31, 63, 31);
            } else if (xenfb->ds->depth == 32) {
                BLT(uint16_t, uint32_t,   11, 5, 0,   16, 8, 0,   31, 63, 31);
            }
        } else if (xenfb->depth == 32) {
            /* 32 bit source == r:8 g:8 b:8 (padding:8) */
            if (xenfb->ds->depth == 8) {
                BLT(uint32_t, uint8_t,    16, 8, 0,   5, 2, 0,    255, 255, 255);
            } else if (xenfb->ds->depth == 16) {
                BLT(uint32_t, uint16_t,   16, 8, 0,   11, 5, 0,   255, 255, 255);
            }
        }
    }
    dpy_update(xenfb->ds, x, y, w, h);
}

/* QEMU display state changed, so refresh the framebuffer copy */
/* XXX - can we optimize this, or the next func at all ? */ 
static void xenfb_update(void *opaque)
{
    struct xenfb *xenfb = opaque;
    xenfb_guest_copy(xenfb, 0, 0, xenfb->width, xenfb->height);
}

/* QEMU display state changed, so refresh the framebuffer copy */
static void xenfb_invalidate(void *opaque)
{
    struct xenfb *xenfb = opaque;
    xenfb_guest_copy(xenfb, 0, 0, xenfb->width, xenfb->height);
}

/* Screen dump is not used in Xen, so no need to impl this....yet */
static void xenfb_screen_dump(void *opaque, const char *name) { }


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
