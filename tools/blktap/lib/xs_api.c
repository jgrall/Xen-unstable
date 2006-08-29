/*
 * xs_api.c
 * 
 * blocktap interface functions to xenstore
 *
 * (c) 2005 Andrew Warfield and Julian Chesterfield
 *
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
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <printf.h>
#include <string.h>
#include <err.h>
#include <stdarg.h>
#include <errno.h>
#include <xs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include "blktaplib.h"
#include "list.h"
#include "xs_api.h"

#if 0
#define DPRINTF(_f, _a...) printf ( _f , ## _a )
#else
#define DPRINTF(_f, _a...) ((void)0)
#endif

static LIST_HEAD(watches);
#define BASE_DEV_VAL 2048

int xs_gather(struct xs_handle *xs, const char *dir, ...)
{
	va_list ap;
	const char *name;
	char *path, **e;
	int ret = 0, num,i;
	unsigned int len;
	xs_transaction_t xth;

again:
	if ( (xth = xs_transaction_start(xs)) == XBT_NULL) {
		DPRINTF("unable to start xs trasanction\n");
		ret = ENOMEM;
		return ret;
	}
	
	va_start(ap, dir);
	while ( (ret == 0) && (name = va_arg(ap, char *)) != NULL) {
		const char *fmt = va_arg(ap, char *);
		void *result = va_arg(ap, void *);
		char *p;
		
		if (asprintf(&path, "%s/%s", dir, name) == -1)
		{
			printf("allocation error in xs_gather!\n");
			ret = ENOMEM;
			break;
		}
		
		p = xs_read(xs, xth, path, &len);
		
		
		free(path);
		if (p == NULL) {
			ret = ENOENT;
			break;
		}
		if (fmt) {
			if (sscanf(p, fmt, result) == 0)
				ret = EINVAL;
			free(p);
		} else
			*(char **)result = p;
	}
	va_end(ap);

	if (!xs_transaction_end(xs, xth, ret)) {
		if (ret == 0 && errno == EAGAIN)
			goto again;
                else
			ret = errno;
	}

	return ret;
}


/* Single printf and write: returns -errno or 0. */
int xs_printf(struct xs_handle *h, const char *dir, const char *node, 
	      const char *fmt, ...)
{
        char *buf, *path;
        va_list ap;
        int ret;
	
        va_start(ap, fmt);
        ret = vasprintf(&buf, fmt, ap);
        va_end(ap);
	
        asprintf(&path, "%s/%s", dir, node);
	
        if ( (path == NULL) || (buf == NULL) )
		return 0;

        ret = xs_write(h, XBT_NULL, path, buf, strlen(buf)+1);
	
        free(buf);
        free(path);
	
        return ret;
}


int xs_exists(struct xs_handle *h, const char *path)
{
	char **d;
	unsigned int num;
	xs_transaction_t xth;
	
	if ( (xth = xs_transaction_start(h)) == XBT_NULL) {
		printf("unable to start xs trasanction\n");
		return 0;
	}	
	
	d = xs_directory(h, xth, path, &num);
	xs_transaction_end(h, xth, 0);
	if (d == NULL)
		return 0;
	free(d);
	return 1;
}



/**
 * This assumes that the domain name we are looking for is unique. 
 * Name parameter Domain-0 
 */
char *get_dom_domid(struct xs_handle *h, const char *name)
{
	char **e, *val, *domid = NULL;
	unsigned int num, len;
	int i;
	char *path;
	xs_transaction_t xth;
	
	if ( (xth = xs_transaction_start(h)) == XBT_NULL) {
		warn("unable to start xs trasanction\n");
		return NULL;
	}
	
	e = xs_directory(h, xth, "/local/domain", &num);
	
	for (i = 0; (i < num) && (domid == NULL); i++) {
		asprintf(&path, "/local/domain/%s/name", e[i]);
		val = xs_read(h, xth, path, &len);
		free(path);
		if (val == NULL)
			continue;
		
		if (strcmp(val, name) == 0) {
			/* match! */
			asprintf(&path, "/local/domain/%s/domid", e[i]);
			domid = xs_read(h, xth, path, &len);
			free(path);
		}
		free(val);
	}
	xs_transaction_end(h, xth, 0);
	
	free(e);
	return domid;
}

int convert_dev_name_to_num(char *name) {
	char *p_sd, *p_hd, *p_xvd, *p_plx, *p, *alpha,*ptr;
	int majors[10] = {3,22,33,34,56,57,88,89,90,91};
	int maj,i,ret = 0;

	asprintf(&p_sd,"/dev/sd");
	asprintf(&p_hd,"/dev/hd");
	asprintf(&p_xvd,"/dev/xvd");
	asprintf(&p_plx,"plx");
	asprintf(&alpha,"abcdefghijklmnop");
	

	if (strstr(name, p_sd) != NULL) {
		p = name + strlen(p_sd);
		for(i = 0, ptr = alpha; i < strlen(alpha); i++) {
			if(*ptr == *p)
				break;
			*ptr++;
		}
		*p++;
		ret = BASE_DEV_VAL + (16*i) + atoi(p);
	} else if (strstr(name, p_hd) != NULL) {
		p = name + strlen(p_hd);
		for (i = 0, ptr = alpha; i < strlen(alpha); i++) {
			if(*ptr == *p) break;
			*ptr++;
		}
		*p++;
		ret = (majors[i/2]*256) + atoi(p);

	} else if (strstr(name, p_xvd) != NULL) {
		p = name + strlen(p_xvd);
		for(i = 0, ptr = alpha; i < strlen(alpha); i++) {
			if(*ptr == *p) break;
			*ptr++;
		}
		*p++;
		ret = (202*256) + (16*i) + atoi(p);

	} else if (strstr(name, p_plx) != NULL) {
		p = name + strlen(p_plx);
		ret = atoi(p);

	} else {
		DPRINTF("Unknown device type, setting to default.\n");
		ret = BASE_DEV_VAL;
	}

        free(p_sd);
        free(p_hd);
        free(p_xvd);
        free(p_plx);
        free(alpha);
        
	return 0;
}

/**
 * A little paranoia: we don't just trust token. 
 */
static struct xenbus_watch *find_watch(const char *token)
{
	struct xenbus_watch *i, *cmp;
	
	cmp = (void *)strtoul(token, NULL, 16);
	
	list_for_each_entry(i, &watches, list)
		if (i == cmp)
			return i;
	return NULL;
}

/**
 * Register callback to watch this node. 
 * like xs_watch, return 0 on failure 
 */
int register_xenbus_watch(struct xs_handle *h, struct xenbus_watch *watch)
{
	/* Pointer in ascii is the token. */
	char token[sizeof(watch) * 2 + 1];
	int er;
	
	sprintf(token, "%lX", (long)watch);
	if (find_watch(token)) 
	{
		DPRINTF("watch collision!\n");
		return -EINVAL;
	}
	
	er = xs_watch(h, watch->node, token);
	if (er != 0) {
		list_add(&watch->list, &watches);
	} 
        
	return er;
}

int unregister_xenbus_watch(struct xs_handle *h, struct xenbus_watch *watch)
{
	char token[sizeof(watch) * 2 + 1];
	int er;
	
	sprintf(token, "%lX", (long)watch);
	if (!find_watch(token))
	{
		DPRINTF("no such watch!\n");
		return -EINVAL;
	}
	
	
	er = xs_unwatch(h, watch->node, token);
	list_del(&watch->list);
	
	if (er == 0)
		DPRINTF("XENBUS Failed to release watch %s: %i\n",
		     watch->node, er);
	return 0;
}

/**
 * Re-register callbacks to all watches. 
 */
void reregister_xenbus_watches(struct xs_handle *h)
{
	struct xenbus_watch *watch;
	char token[sizeof(watch) * 2 + 1];
	
	list_for_each_entry(watch, &watches, list) {
		sprintf(token, "%lX", (long)watch);
		xs_watch(h, watch->node, token);
	}
}

/**
 * based on watch_thread() 
 */
int xs_fire_next_watch(struct xs_handle *h)
{
	char **res;
	char *token;
	char *node = NULL;
	struct xenbus_watch *w;
	int er;
	unsigned int num;
	
	res = xs_read_watch(h, &num);
	if (res == NULL) 
		return -EAGAIN; /* in O_NONBLOCK, read_watch returns 0... */
	
	node  = res[XS_WATCH_PATH];
	token = res[XS_WATCH_TOKEN];
	
	w = find_watch(token);
	if (!w)
	{
		DPRINTF("unregistered watch fired\n");
		goto done;
	}
	w->callback(h, w, node);
	
 done:
	free(res);
	return 1;
}
