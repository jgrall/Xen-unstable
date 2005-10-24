/******************************************************************************
 * xenbus_xs.c
 *
 * This is the kernel equivalent of the "xs" library.  We don't need everything
 * and we use xenbus_comms for communication.
 *
 * Copyright (C) 2005 Rusty Russell, IBM Corporation
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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/uio.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/kthread.h>
#include <asm-xen/xenbus.h>
#include "xenbus_comms.h"

#define streq(a, b) (strcmp((a), (b)) == 0)

struct xs_stored_msg {
	struct list_head list;

	struct xsd_sockmsg hdr;

	union {
		/* Queued replies. */
		struct {
			char *body;
		} reply;

		/* Queued watch events. */
		struct {
			struct xenbus_watch *handle;
			char **vec;
			unsigned int vec_size;
		} watch;
	} u;
};

struct xs_handle {
	/* A list of replies. Currently only one will ever be outstanding. */
	struct list_head reply_list;
	spinlock_t reply_lock;
	wait_queue_head_t reply_waitq;

	/* One request at a time. */
	struct semaphore request_mutex;

	/* Protect transactions against save/restore. */
	struct rw_semaphore suspend_mutex;
};

static struct xs_handle xs_state;

/* List of registered watches, and a lock to protect it. */
static LIST_HEAD(watches);
static DEFINE_SPINLOCK(watches_lock);

/* List of pending watch callback events, and a lock to protect it. */
static LIST_HEAD(watch_events);
static DEFINE_SPINLOCK(watch_events_lock);

/*
 * Details of the xenwatch callback kernel thread. The thread waits on the
 * watch_events_waitq for work to do (queued on watch_events list). When it
 * wakes up it acquires the xenwatch_mutex before reading the list and
 * carrying out work.
 */
static pid_t xenwatch_pid;
/* static */ DECLARE_MUTEX(xenwatch_mutex);
static DECLARE_WAIT_QUEUE_HEAD(watch_events_waitq);

static int get_error(const char *errorstring)
{
	unsigned int i;

	for (i = 0; !streq(errorstring, xsd_errors[i].errstring); i++) {
		if (i == ARRAY_SIZE(xsd_errors) - 1) {
			printk(KERN_WARNING
			       "XENBUS xen store gave: unknown error %s",
			       errorstring);
			return EINVAL;
		}
	}
	return xsd_errors[i].errnum;
}

static void *read_reply(enum xsd_sockmsg_type *type, unsigned int *len)
{
	struct xs_stored_msg *msg;
	char *body;

	spin_lock(&xs_state.reply_lock);

	while (list_empty(&xs_state.reply_list)) {
		spin_unlock(&xs_state.reply_lock);
		wait_event_interruptible(xs_state.reply_waitq,
					 !list_empty(&xs_state.reply_list));
		spin_lock(&xs_state.reply_lock);
	}

	msg = list_entry(xs_state.reply_list.next,
			 struct xs_stored_msg, list);
	list_del(&msg->list);

	spin_unlock(&xs_state.reply_lock);

	*type = msg->hdr.type;
	if (len)
		*len = msg->hdr.len;
	body = msg->u.reply.body;

	kfree(msg);

	return body;
}

/* Emergency write. */
void xenbus_debug_write(const char *str, unsigned int count)
{
	struct xsd_sockmsg msg = { 0 };

	msg.type = XS_DEBUG;
	msg.len = sizeof("print") + count + 1;

	down(&xs_state.request_mutex);
	xb_write(&msg, sizeof(msg));
	xb_write("print", sizeof("print"));
	xb_write(str, count);
	xb_write("", 1);
	up(&xs_state.request_mutex);
}

void *xenbus_dev_request_and_reply(struct xsd_sockmsg *msg)
{
	void *ret;
	struct xsd_sockmsg req_msg = *msg;
	int err;

	if (req_msg.type == XS_TRANSACTION_START)
		down_read(&xs_state.suspend_mutex);

	down(&xs_state.request_mutex);

	err = xb_write(msg, sizeof(*msg) + msg->len);
	if (err) {
		msg->type = XS_ERROR;
		ret = ERR_PTR(err);
	} else {
		ret = read_reply(&msg->type, &msg->len);
	}

	up(&xs_state.request_mutex);

	if ((msg->type == XS_TRANSACTION_END) ||
	    ((req_msg.type == XS_TRANSACTION_START) &&
	     (msg->type == XS_ERROR)))
		up_read(&xs_state.suspend_mutex);

	return ret;
}

/* Send message to xs, get kmalloc'ed reply.  ERR_PTR() on error. */
static void *xs_talkv(struct xenbus_transaction *t,
		      enum xsd_sockmsg_type type,
		      const struct kvec *iovec,
		      unsigned int num_vecs,
		      unsigned int *len)
{
	struct xsd_sockmsg msg;
	void *ret = NULL;
	unsigned int i;
	int err;

	msg.tx_id = (u32)(unsigned long)t;
	msg.type = type;
	msg.len = 0;
	for (i = 0; i < num_vecs; i++)
		msg.len += iovec[i].iov_len;

	down(&xs_state.request_mutex);

	err = xb_write(&msg, sizeof(msg));
	if (err) {
		up(&xs_state.request_mutex);
		return ERR_PTR(err);
	}

	for (i = 0; i < num_vecs; i++) {
		err = xb_write(iovec[i].iov_base, iovec[i].iov_len);;
		if (err) {
			up(&xs_state.request_mutex);
			return ERR_PTR(err);
		}
	}

	ret = read_reply(&msg.type, len);

	up(&xs_state.request_mutex);

	if (IS_ERR(ret))
		return ret;

	if (msg.type == XS_ERROR) {
		err = get_error(ret);
		kfree(ret);
		return ERR_PTR(-err);
	}

	BUG_ON(msg.type != type);
	return ret;
}

/* Simplified version of xs_talkv: single message. */
static void *xs_single(struct xenbus_transaction *t,
		       enum xsd_sockmsg_type type,
		       const char *string,
		       unsigned int *len)
{
	struct kvec iovec;

	iovec.iov_base = (void *)string;
	iovec.iov_len = strlen(string) + 1;
	return xs_talkv(t, type, &iovec, 1, len);
}

/* Many commands only need an ack, don't care what it says. */
static int xs_error(char *reply)
{
	if (IS_ERR(reply))
		return PTR_ERR(reply);
	kfree(reply);
	return 0;
}

static unsigned int count_strings(const char *strings, unsigned int len)
{
	unsigned int num;
	const char *p;

	for (p = strings, num = 0; p < strings + len; p += strlen(p) + 1)
		num++;

	return num;
}

/* Return the path to dir with /name appended. Buffer must be kfree()'ed. */ 
static char *join(const char *dir, const char *name)
{
	char *buffer;

	buffer = kmalloc(strlen(dir) + strlen("/") + strlen(name) + 1,
			 GFP_KERNEL);
	if (buffer == NULL)
		return ERR_PTR(-ENOMEM);

	strcpy(buffer, dir);
	if (!streq(name, "")) {
		strcat(buffer, "/");
		strcat(buffer, name);
	}

	return buffer;
}

static char **split(char *strings, unsigned int len, unsigned int *num)
{
	char *p, **ret;

	/* Count the strings. */
	*num = count_strings(strings, len);

	/* Transfer to one big alloc for easy freeing. */
	ret = kmalloc(*num * sizeof(char *) + len, GFP_KERNEL);
	if (!ret) {
		kfree(strings);
		return ERR_PTR(-ENOMEM);
	}
	memcpy(&ret[*num], strings, len);
	kfree(strings);

	strings = (char *)&ret[*num];
	for (p = strings, *num = 0; p < strings + len; p += strlen(p) + 1)
		ret[(*num)++] = p;

	return ret;
}

char **xenbus_directory(struct xenbus_transaction *t,
			const char *dir, const char *node, unsigned int *num)
{
	char *strings, *path;
	unsigned int len;

	path = join(dir, node);
	if (IS_ERR(path))
		return (char **)path;

	strings = xs_single(t, XS_DIRECTORY, path, &len);
	kfree(path);
	if (IS_ERR(strings))
		return (char **)strings;

	return split(strings, len, num);
}
EXPORT_SYMBOL(xenbus_directory);

/* Check if a path exists. Return 1 if it does. */
int xenbus_exists(struct xenbus_transaction *t,
		  const char *dir, const char *node)
{
	char **d;
	int dir_n;

	d = xenbus_directory(t, dir, node, &dir_n);
	if (IS_ERR(d))
		return 0;
	kfree(d);
	return 1;
}
EXPORT_SYMBOL(xenbus_exists);

/* Get the value of a single file.
 * Returns a kmalloced value: call free() on it after use.
 * len indicates length in bytes.
 */
void *xenbus_read(struct xenbus_transaction *t,
		  const char *dir, const char *node, unsigned int *len)
{
	char *path;
	void *ret;

	path = join(dir, node);
	if (IS_ERR(path))
		return (void *)path;

	ret = xs_single(t, XS_READ, path, len);
	kfree(path);
	return ret;
}
EXPORT_SYMBOL(xenbus_read);

/* Write the value of a single file.
 * Returns -err on failure.
 */
int xenbus_write(struct xenbus_transaction *t,
		 const char *dir, const char *node, const char *string)
{
	const char *path;
	struct kvec iovec[2];
	int ret;

	path = join(dir, node);
	if (IS_ERR(path))
		return PTR_ERR(path);

	iovec[0].iov_base = (void *)path;
	iovec[0].iov_len = strlen(path) + 1;
	iovec[1].iov_base = (void *)string;
	iovec[1].iov_len = strlen(string);

	ret = xs_error(xs_talkv(t, XS_WRITE, iovec, ARRAY_SIZE(iovec), NULL));
	kfree(path);
	return ret;
}
EXPORT_SYMBOL(xenbus_write);

/* Create a new directory. */
int xenbus_mkdir(struct xenbus_transaction *t,
		 const char *dir, const char *node)
{
	char *path;
	int ret;

	path = join(dir, node);
	if (IS_ERR(path))
		return PTR_ERR(path);

	ret = xs_error(xs_single(t, XS_MKDIR, path, NULL));
	kfree(path);
	return ret;
}
EXPORT_SYMBOL(xenbus_mkdir);

/* Destroy a file or directory (directories must be empty). */
int xenbus_rm(struct xenbus_transaction *t, const char *dir, const char *node)
{
	char *path;
	int ret;

	path = join(dir, node);
	if (IS_ERR(path))
		return PTR_ERR(path);

	ret = xs_error(xs_single(t, XS_RM, path, NULL));
	kfree(path);
	return ret;
}
EXPORT_SYMBOL(xenbus_rm);

/* Start a transaction: changes by others will not be seen during this
 * transaction, and changes will not be visible to others until end.
 */
struct xenbus_transaction *xenbus_transaction_start(void)
{
	char *id_str;
	unsigned long id;

	down_read(&xs_state.suspend_mutex);

	id_str = xs_single(NULL, XS_TRANSACTION_START, "", NULL);
	if (IS_ERR(id_str)) {
		up_read(&xs_state.suspend_mutex);
		return (struct xenbus_transaction *)id_str;
	}

	id = simple_strtoul(id_str, NULL, 0);
	kfree(id_str);

	return (struct xenbus_transaction *)id;
}
EXPORT_SYMBOL(xenbus_transaction_start);

/* End a transaction.
 * If abandon is true, transaction is discarded instead of committed.
 */
int xenbus_transaction_end(struct xenbus_transaction *t, int abort)
{
	char abortstr[2];
	int err;

	if (abort)
		strcpy(abortstr, "F");
	else
		strcpy(abortstr, "T");

	err = xs_error(xs_single(t, XS_TRANSACTION_END, abortstr, NULL));

	up_read(&xs_state.suspend_mutex);

	return err;
}
EXPORT_SYMBOL(xenbus_transaction_end);

/* Single read and scanf: returns -errno or num scanned. */
int xenbus_scanf(struct xenbus_transaction *t,
		 const char *dir, const char *node, const char *fmt, ...)
{
	va_list ap;
	int ret;
	char *val;

	val = xenbus_read(t, dir, node, NULL);
	if (IS_ERR(val))
		return PTR_ERR(val);

	va_start(ap, fmt);
	ret = vsscanf(val, fmt, ap);
	va_end(ap);
	kfree(val);
	/* Distinctive errno. */
	if (ret == 0)
		return -ERANGE;
	return ret;
}
EXPORT_SYMBOL(xenbus_scanf);

/* Single printf and write: returns -errno or 0. */
int xenbus_printf(struct xenbus_transaction *t,
		  const char *dir, const char *node, const char *fmt, ...)
{
	va_list ap;
	int ret;
#define PRINTF_BUFFER_SIZE 4096
	char *printf_buffer;

	printf_buffer = kmalloc(PRINTF_BUFFER_SIZE, GFP_KERNEL);
	if (printf_buffer == NULL)
		return -ENOMEM;

	va_start(ap, fmt);
	ret = vsnprintf(printf_buffer, PRINTF_BUFFER_SIZE, fmt, ap);
	va_end(ap);

	BUG_ON(ret > PRINTF_BUFFER_SIZE-1);
	ret = xenbus_write(t, dir, node, printf_buffer);

	kfree(printf_buffer);

	return ret;
}
EXPORT_SYMBOL(xenbus_printf);

/**
 * Return the path to the error node for the given device, or NULL on failure.
 * If the value returned is non-NULL, then it is the caller's to kfree.
 */
static char *error_path(struct xenbus_device *dev)
{
	char *path_buffer = kmalloc(strlen("error/") + strlen(dev->nodename) +
				    1, GFP_KERNEL);
	if (path_buffer == NULL) {
		return NULL;
	}

	strcpy(path_buffer, "error/");
	strcpy(path_buffer + strlen("error/"), dev->nodename);

	return path_buffer;
}

/* Report a (negative) errno into the store, with explanation. */
void xenbus_dev_error(struct xenbus_device *dev, int err, const char *fmt, ...)
{
	va_list ap;
	int ret;
	unsigned int len;
	char *printf_buffer = NULL, *path_buffer = NULL;

	printf_buffer = kmalloc(PRINTF_BUFFER_SIZE, GFP_KERNEL);
	if (printf_buffer == NULL)
		goto fail;

	len = sprintf(printf_buffer, "%i ", -err);
	va_start(ap, fmt);
	ret = vsnprintf(printf_buffer+len, PRINTF_BUFFER_SIZE-len, fmt, ap);
	va_end(ap);

	BUG_ON(len + ret > PRINTF_BUFFER_SIZE-1);
	dev->has_error = 1;

	path_buffer = error_path(dev);

	if (path_buffer == NULL) {
		printk("xenbus: failed to write error node for %s (%s)\n",
		       dev->nodename, printf_buffer);
		goto fail;
	}

	if (xenbus_write(NULL, path_buffer, "error", printf_buffer) != 0) {
		printk("xenbus: failed to write error node for %s (%s)\n",
		       dev->nodename, printf_buffer);
		goto fail;
	}

fail:
	if (printf_buffer)
		kfree(printf_buffer);
	if (path_buffer)
		kfree(path_buffer);
}
EXPORT_SYMBOL(xenbus_dev_error);

/* Clear any error. */
void xenbus_dev_ok(struct xenbus_device *dev)
{
	if (dev->has_error) {
		char *path_buffer = error_path(dev);

		if (path_buffer == NULL) {
			printk("xenbus: failed to clear error node for %s\n",
			       dev->nodename);
			return;
		}

		if (xenbus_rm(NULL, path_buffer, "error") != 0)
			printk("xenbus: failed to clear error node for %s\n",
			       dev->nodename);
		else
			dev->has_error = 0;

		kfree(path_buffer);
	}
}
EXPORT_SYMBOL(xenbus_dev_ok);
	
/* Takes tuples of names, scanf-style args, and void **, NULL terminated. */
int xenbus_gather(struct xenbus_transaction *t, const char *dir, ...)
{
	va_list ap;
	const char *name;
	int ret = 0;

	va_start(ap, dir);
	while (ret == 0 && (name = va_arg(ap, char *)) != NULL) {
		const char *fmt = va_arg(ap, char *);
		void *result = va_arg(ap, void *);
		char *p;

		p = xenbus_read(t, dir, name, NULL);
		if (IS_ERR(p)) {
			ret = PTR_ERR(p);
			break;
		}
		if (fmt) {
			if (sscanf(p, fmt, result) == 0)
				ret = -EINVAL;
			kfree(p);
		} else
			*(char **)result = p;
	}
	va_end(ap);
	return ret;
}
EXPORT_SYMBOL(xenbus_gather);

static int xs_watch(const char *path, const char *token)
{
	struct kvec iov[2];

	iov[0].iov_base = (void *)path;
	iov[0].iov_len = strlen(path) + 1;
	iov[1].iov_base = (void *)token;
	iov[1].iov_len = strlen(token) + 1;

	return xs_error(xs_talkv(NULL, XS_WATCH, iov,
				 ARRAY_SIZE(iov), NULL));
}

static int xs_unwatch(const char *path, const char *token)
{
	struct kvec iov[2];

	iov[0].iov_base = (char *)path;
	iov[0].iov_len = strlen(path) + 1;
	iov[1].iov_base = (char *)token;
	iov[1].iov_len = strlen(token) + 1;

	return xs_error(xs_talkv(NULL, XS_UNWATCH, iov,
				 ARRAY_SIZE(iov), NULL));
}

static struct xenbus_watch *find_watch(const char *token)
{
	struct xenbus_watch *i, *cmp;

	cmp = (void *)simple_strtoul(token, NULL, 16);

	list_for_each_entry(i, &watches, list)
		if (i == cmp)
			return i;

	return NULL;
}

/* Register callback to watch this node. */
int register_xenbus_watch(struct xenbus_watch *watch)
{
	/* Pointer in ascii is the token. */
	char token[sizeof(watch) * 2 + 1];
	int err;

	sprintf(token, "%lX", (long)watch);

	down_read(&xs_state.suspend_mutex);

	spin_lock(&watches_lock);
	BUG_ON(find_watch(token));
	spin_unlock(&watches_lock);

	err = xs_watch(watch->node, token);

	/* Ignore errors due to multiple registration. */
	if ((err == 0) || (err == -EEXIST)) {
		spin_lock(&watches_lock);
		list_add(&watch->list, &watches);
		spin_unlock(&watches_lock);
	}

	up_read(&xs_state.suspend_mutex);

	return err;
}
EXPORT_SYMBOL(register_xenbus_watch);

void unregister_xenbus_watch(struct xenbus_watch *watch)
{
	struct xs_stored_msg *msg, *tmp;
	char token[sizeof(watch) * 2 + 1];
	int err;

	sprintf(token, "%lX", (long)watch);

	down_read(&xs_state.suspend_mutex);

	spin_lock(&watches_lock);
	BUG_ON(!find_watch(token));
	list_del(&watch->list);
	spin_unlock(&watches_lock);

	err = xs_unwatch(watch->node, token);
	if (err)
		printk(KERN_WARNING
		       "XENBUS Failed to release watch %s: %i\n",
		       watch->node, err);

	up_read(&xs_state.suspend_mutex);

	/* Cancel pending watch events. */
	spin_lock(&watch_events_lock);
	list_for_each_entry_safe(msg, tmp, &watch_events, list) {
		if (msg->u.watch.handle != watch)
			continue;
		list_del(&msg->list);
		kfree(msg->u.watch.vec);
		kfree(msg);
	}
	spin_unlock(&watch_events_lock);

	/* Flush any currently-executing callback, unless we are it. :-) */
	if (current->pid != xenwatch_pid) {
		down(&xenwatch_mutex);
		up(&xenwatch_mutex);
	}
}
EXPORT_SYMBOL(unregister_xenbus_watch);

void xs_suspend(void)
{
	down_write(&xs_state.suspend_mutex);
	down(&xs_state.request_mutex);
}

void xs_resume(void)
{
	struct xenbus_watch *watch;
	char token[sizeof(watch) * 2 + 1];

	up(&xs_state.request_mutex);

	/* No need for watches_lock: the suspend_mutex is sufficient. */
	list_for_each_entry(watch, &watches, list) {
		sprintf(token, "%lX", (long)watch);
		xs_watch(watch->node, token);
	}

	up_write(&xs_state.suspend_mutex);
}

static int xenwatch_thread(void *unused)
{
	struct list_head *ent;
	struct xs_stored_msg *msg;

	for (;;) {
		wait_event_interruptible(watch_events_waitq,
					 !list_empty(&watch_events));

		down(&xenwatch_mutex);

		spin_lock(&watch_events_lock);
		ent = watch_events.next;
		if (ent != &watch_events)
			list_del(ent);
		spin_unlock(&watch_events_lock);

		if (ent != &watch_events) {
			msg = list_entry(ent, struct xs_stored_msg, list);
			msg->u.watch.handle->callback(
				msg->u.watch.handle,
				(const char **)msg->u.watch.vec,
				msg->u.watch.vec_size);
			kfree(msg->u.watch.vec);
			kfree(msg);
		}

		up(&xenwatch_mutex);
	}
}

static int process_msg(void)
{
	struct xs_stored_msg *msg;
	char *body;
	int err;

	msg = kmalloc(sizeof(*msg), GFP_KERNEL);
	if (msg == NULL)
		return -ENOMEM;

	err = xb_read(&msg->hdr, sizeof(msg->hdr));
	if (err) {
		kfree(msg);
		return err;
	}

	body = kmalloc(msg->hdr.len + 1, GFP_KERNEL);
	if (body == NULL) {
		kfree(msg);
		return -ENOMEM;
	}

	err = xb_read(body, msg->hdr.len);
	if (err) {
		kfree(body);
		kfree(msg);
		return err;
	}
	body[msg->hdr.len] = '\0';

	if (msg->hdr.type == XS_WATCH_EVENT) {
		msg->u.watch.vec = split(body, msg->hdr.len,
					 &msg->u.watch.vec_size);
		if (IS_ERR(msg->u.watch.vec)) {
			kfree(msg);
			return PTR_ERR(msg->u.watch.vec);
		}

		spin_lock(&watches_lock);
		msg->u.watch.handle = find_watch(
			msg->u.watch.vec[XS_WATCH_TOKEN]);
		if (msg->u.watch.handle != NULL) {
			spin_lock(&watch_events_lock);
			list_add_tail(&msg->list, &watch_events);
			wake_up(&watch_events_waitq);
			spin_unlock(&watch_events_lock);
		} else {
			kfree(msg->u.watch.vec);
			kfree(msg);
		}
		spin_unlock(&watches_lock);
	} else {
		msg->u.reply.body = body;
		spin_lock(&xs_state.reply_lock);
		list_add_tail(&msg->list, &xs_state.reply_list);
		spin_unlock(&xs_state.reply_lock);
		wake_up(&xs_state.reply_waitq);
	}

	return 0;
}

static int xenbus_thread(void *unused)
{
	int err;

	for (;;) {
		err = process_msg();
		if (err)
			printk(KERN_WARNING "XENBUS error %d while reading "
			       "message\n", err);
	}
}

int xs_init(void)
{
	int err;
	struct task_struct *task;

	INIT_LIST_HEAD(&xs_state.reply_list);
	spin_lock_init(&xs_state.reply_lock);
	init_waitqueue_head(&xs_state.reply_waitq);

	init_MUTEX(&xs_state.request_mutex);
	init_rwsem(&xs_state.suspend_mutex);

	/* Initialize the shared memory rings to talk to xenstored */
	err = xb_init_comms();
	if (err)
		return err;

	task = kthread_run(xenwatch_thread, NULL, "xenwatch");
	if (IS_ERR(task))
		return PTR_ERR(task);
	xenwatch_pid = task->pid;

	task = kthread_run(xenbus_thread, NULL, "xenbus");
	if (IS_ERR(task))
		return PTR_ERR(task);

	return 0;
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
