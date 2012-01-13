/*
 * Copyright (C) 2009      Citrix Ltd.
 * Author Vincent Hanquez <vincent.hanquez@eu.citrix.com>
 * Author Stefano Stabellini <stefano.stabellini@eu.citrix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include "libxl_internal.h"

#define PAGE_TO_MEMKB(pages) ((pages) * 4)
#define BACKEND_STRING_SIZE 5

int libxl_ctx_alloc(libxl_ctx **pctx, int version,
                    unsigned flags, xentoollog_logger * lg)
{
    libxl_ctx *ctx = NULL;
    struct stat stat_buf;
    const pthread_mutex_t mutex_value = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    int rc;

    if (version != LIBXL_VERSION) { rc = ERROR_VERSION; goto out; }

    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Failed to allocate context\n");
        rc = ERROR_NOMEM; goto out;
    }

    memset(ctx, 0, sizeof(libxl_ctx));
    ctx->lg = lg;

    /* This somewhat convoluted approach is needed because
     * PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP is defined to be valid
     * only as an initialiser, not as an expression. */
    memcpy(&ctx->lock, &mutex_value, sizeof(ctx->lock));

    if ( stat(XENSTORE_PID_FILE, &stat_buf) != 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Is xenstore daemon running?\n"
                     "failed to stat %s", XENSTORE_PID_FILE);
        rc = ERROR_FAIL; goto out;
    }

    ctx->xch = xc_interface_open(lg,lg,0);
    if (!ctx->xch) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, errno,
                        "cannot open libxc handle");
        rc = ERROR_FAIL; goto out;
    }

    ctx->xsh = xs_daemon_open();
    if (!ctx->xsh)
        ctx->xsh = xs_domain_open();
    if (!ctx->xsh) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, errno,
                        "cannot connect to xenstore");
        rc = ERROR_FAIL; goto out;
    }

    *pctx = ctx;
    return 0;

 out:
    libxl_ctx_free(ctx);
    *pctx = NULL;
    return rc;
}

int libxl_ctx_free(libxl_ctx *ctx)
{
    if (!ctx) return 0;
    if (ctx->xch) xc_interface_close(ctx->xch);
    libxl_version_info_dispose(&ctx->version_info);
    if (ctx->xsh) xs_daemon_close(ctx->xsh);
    free(ctx);
    return 0;
}

void libxl_string_list_dispose(libxl_string_list *psl)
{
    int i;
    libxl_string_list sl = *psl;

    if (!sl)
        return;

    for (i = 0; sl[i] != NULL; i++)
        free(sl[i]);
    free(sl);
}

void libxl_key_value_list_dispose(libxl_key_value_list *pkvl)
{
    int i;
    libxl_key_value_list kvl = *pkvl;

    if (!kvl)
        return;

    for (i = 0; kvl[i] != NULL; i += 2) {
        free(kvl[i]);
        if (kvl[i + 1])
            free(kvl[i + 1]);
    }
    free(kvl);
}

/******************************************************************************/


int libxl__domain_rename(libxl__gc *gc, uint32_t domid,
                         const char *old_name, const char *new_name,
                         xs_transaction_t trans)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    char *dom_path = 0;
    const char *name_path;
    char *got_old_name;
    unsigned int got_old_len;
    xs_transaction_t our_trans = 0;
    int rc;

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path) goto x_nomem;

    name_path= libxl__sprintf(gc, "%s/name", dom_path);
    if (!name_path) goto x_nomem;

 retry_transaction:
    if (!trans) {
        trans = our_trans = xs_transaction_start(ctx->xsh);
        if (!our_trans) {
            LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, errno,
                            "create xs transaction for domain (re)name");
            goto x_fail;
        }
    }

    if (new_name[0]) {
        /* nonempty names must be unique */
        uint32_t domid_e;
        rc = libxl_name_to_domid(ctx, new_name, &domid_e);
        if (rc == ERROR_INVAL) {
            /* no such domain, good */
        } else if (rc != 0) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "unexpected error"
                       "checking for existing domain");
            goto x_rc;
        } else if (domid_e == domid) {
            /* domain already has this name, ok (but we do still
             * need the rest of the code as we may need to check
             * old_name, for example). */
        } else {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "domain with name \"%s\""
                       " already exists.", new_name);
            rc = ERROR_INVAL;
            goto x_rc;
        }
    }

    if (old_name) {
        got_old_name = xs_read(ctx->xsh, trans, name_path, &got_old_len);
        if (!got_old_name) {
            LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, errno, "check old name"
                            " for domain %"PRIu32" allegedly named `%s'",
                            domid, old_name);
            goto x_fail;
        }
        if (strcmp(old_name, got_old_name)) {
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "domain %"PRIu32" allegedly named "
                   "`%s' is actually named `%s' - racing ?",
                   domid, old_name, got_old_name);
            free(got_old_name);
            goto x_fail;
        }
        free(got_old_name);
    }
    if (!xs_write(ctx->xsh, trans, name_path,
                  new_name, strlen(new_name))) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "failed to write new name `%s'"
               " for domain %"PRIu32" previously named `%s'",
               new_name, domid, old_name);
        goto x_fail;
    }

    if (our_trans) {
        if (!xs_transaction_end(ctx->xsh, our_trans, 0)) {
            trans = our_trans = 0;
            if (errno != EAGAIN) {
                LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "failed to commit new name `%s'"
                       " for domain %"PRIu32" previously named `%s'",
                       new_name, domid, old_name);
                goto x_fail;
            }
            LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "need to retry rename transaction"
                   " for domain %"PRIu32" (name_path=\"%s\", new_name=\"%s\")",
                   domid, name_path, new_name);
            goto retry_transaction;
        }
        our_trans = 0;
    }

    rc = 0;
 x_rc:
    if (our_trans) xs_transaction_end(ctx->xsh, our_trans, 1);
    return rc;

 x_fail:  rc = ERROR_FAIL;  goto x_rc;
 x_nomem: rc = ERROR_NOMEM; goto x_rc;
}

int libxl_domain_rename(libxl_ctx *ctx, uint32_t domid,
                        const char *old_name, const char *new_name)
{
    GC_INIT(ctx);
    int rc;
    rc = libxl__domain_rename(gc, domid, old_name, new_name, XBT_NULL);
    GC_FREE;
    return rc;
}

int libxl_domain_resume(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    int rc = 0;

    if (LIBXL__DOMAIN_IS_TYPE(gc,  domid, HVM)) {
        LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "Called domain_resume on "
                "non-cooperative hvm domain %u", domid);
        rc = ERROR_NI;
        goto out;
    }
    if (xc_domain_resume(ctx->xch, domid, 0)) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                        "xc_domain_resume failed for domain %u",
                        domid);
        rc = ERROR_FAIL;
        goto out;
    }
    if (!xs_resume_domain(ctx->xsh, domid)) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                        "xs_resume_domain failed for domain %u",
                        domid);
        rc = ERROR_FAIL;
    }
out:
    GC_FREE;
    return rc;
}

/*
 * Preserves a domain but rewrites xenstore etc to make it unique so
 * that the domain can be restarted.
 *
 * Does not modify info so that it may be reused.
 */
int libxl_domain_preserve(libxl_ctx *ctx, uint32_t domid,
                          libxl_domain_create_info *info, const char *name_suffix, libxl_uuid new_uuid)
{
    GC_INIT(ctx);
    struct xs_permissions roperm[2];
    xs_transaction_t t;
    char *preserved_name;
    char *uuid_string;
    char *vm_path;
    char *dom_path;

    int rc;

    preserved_name = libxl__sprintf(gc, "%s%s", info->name, name_suffix);
    if (!preserved_name) {
        GC_FREE;
        return ERROR_NOMEM;
    }

    uuid_string = libxl__uuid2string(gc, new_uuid);
    if (!uuid_string) {
        GC_FREE;
        return ERROR_NOMEM;
    }

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path) {
        GC_FREE;
        return ERROR_FAIL;
    }

    vm_path = libxl__sprintf(gc, "/vm/%s", uuid_string);
    if (!vm_path) {
        GC_FREE;
        return ERROR_FAIL;
    }

    roperm[0].id = 0;
    roperm[0].perms = XS_PERM_NONE;
    roperm[1].id = domid;
    roperm[1].perms = XS_PERM_READ;

 retry_transaction:
    t = xs_transaction_start(ctx->xsh);

    xs_rm(ctx->xsh, t, vm_path);
    xs_mkdir(ctx->xsh, t, vm_path);
    xs_set_permissions(ctx->xsh, t, vm_path, roperm, ARRAY_SIZE(roperm));

    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/vm", dom_path), vm_path, strlen(vm_path));
    rc = libxl__domain_rename(gc, domid, info->name, preserved_name, t);
    if (rc) {
        GC_FREE;
        return rc;
    }

    xs_write(ctx->xsh, t, libxl__sprintf(gc, "%s/uuid", vm_path), uuid_string, strlen(uuid_string));

    if (!xs_transaction_end(ctx->xsh, t, 0))
        if (errno == EAGAIN)
            goto retry_transaction;

    GC_FREE;
    return 0;
}

static void xcinfo2xlinfo(const xc_domaininfo_t *xcinfo,
                          libxl_dominfo *xlinfo)
{
    memcpy(&(xlinfo->uuid), xcinfo->handle, sizeof(xen_domain_handle_t));
    xlinfo->domid = xcinfo->domain;
    xlinfo->ssidref = xcinfo->ssidref;

    xlinfo->dying    = !!(xcinfo->flags&XEN_DOMINF_dying);
    xlinfo->shutdown = !!(xcinfo->flags&XEN_DOMINF_shutdown);
    xlinfo->paused   = !!(xcinfo->flags&XEN_DOMINF_paused);
    xlinfo->blocked  = !!(xcinfo->flags&XEN_DOMINF_blocked);
    xlinfo->running  = !!(xcinfo->flags&XEN_DOMINF_running);

    if (xlinfo->shutdown || xlinfo->dying)
        xlinfo->shutdown_reason = (xcinfo->flags>>XEN_DOMINF_shutdownshift) & XEN_DOMINF_shutdownmask;
    else
        xlinfo->shutdown_reason  = ~0;

    xlinfo->current_memkb = PAGE_TO_MEMKB(xcinfo->tot_pages);
    xlinfo->shared_memkb = PAGE_TO_MEMKB(xcinfo->shr_pages);
    xlinfo->max_memkb = PAGE_TO_MEMKB(xcinfo->max_pages);
    xlinfo->cpu_time = xcinfo->cpu_time;
    xlinfo->vcpu_max_id = xcinfo->max_vcpu_id;
    xlinfo->vcpu_online = xcinfo->nr_online_vcpus;
    xlinfo->cpupool = xcinfo->cpupool;
}

libxl_dominfo * libxl_list_domain(libxl_ctx *ctx, int *nb_domain)
{
    libxl_dominfo *ptr;
    int i, ret;
    xc_domaininfo_t info[1024];
    int size = 1024;

    ptr = calloc(size, sizeof(libxl_dominfo));
    if (!ptr) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "allocating domain info");
        return NULL;
    }

    ret = xc_domain_getinfolist(ctx->xch, 0, 1024, info);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "geting domain info list");
        return NULL;
    }

    for (i = 0; i < ret; i++) {
        xcinfo2xlinfo(&info[i], &ptr[i]);
    }
    *nb_domain = ret;
    return ptr;
}

int libxl_domain_info(libxl_ctx *ctx, libxl_dominfo *info_r,
                      uint32_t domid) {
    xc_domaininfo_t xcinfo;
    int ret;

    ret = xc_domain_getinfolist(ctx->xch, domid, 1, &xcinfo);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "geting domain info list");
        return ERROR_FAIL;
    }
    if (ret==0 || xcinfo.domain != domid) return ERROR_INVAL;

    xcinfo2xlinfo(&xcinfo, info_r);
    return 0;
}

libxl_cpupoolinfo * libxl_list_cpupool(libxl_ctx *ctx, int *nb_pool)
{
    libxl_cpupoolinfo *ptr, *tmp;
    int i;
    xc_cpupoolinfo_t *info;
    uint32_t poolid;

    ptr = NULL;

    poolid = 0;
    for (i = 0;; i++) {
        info = xc_cpupool_getinfo(ctx->xch, poolid);
        if (info == NULL)
            break;
        tmp = realloc(ptr, (i + 1) * sizeof(libxl_cpupoolinfo));
        if (!tmp) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "allocating cpupool info");
            free(ptr);
            xc_cpupool_infofree(ctx->xch, info);
            return NULL;
        }
        ptr = tmp;
        ptr[i].poolid = info->cpupool_id;
        ptr[i].sched_id = info->sched_id;
        ptr[i].n_dom = info->n_dom;
        if (libxl_cpumap_alloc(ctx, &ptr[i].cpumap)) {
            xc_cpupool_infofree(ctx->xch, info);
            break;
        }
        memcpy(ptr[i].cpumap.map, info->cpumap, ptr[i].cpumap.size);
        poolid = info->cpupool_id + 1;
        xc_cpupool_infofree(ctx->xch, info);
    }

    *nb_pool = i;
    return ptr;
}

/* this API call only list VM running on this host. a VM can be an aggregate of multiple domains. */
libxl_vminfo * libxl_list_vm(libxl_ctx *ctx, int *nb_vm)
{
    libxl_vminfo *ptr;
    int index, i, ret;
    xc_domaininfo_t info[1024];
    int size = 1024;

    ptr = calloc(size, sizeof(libxl_vminfo));
    if (!ptr)
        return NULL;

    ret = xc_domain_getinfolist(ctx->xch, 1, 1024, info);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "geting domain info list");
        return NULL;
    }
    for (index = i = 0; i < ret; i++) {
        if (libxl_is_stubdom(ctx, info[i].domain, NULL))
            continue;
        memcpy(&(ptr[index].uuid), info[i].handle, sizeof(xen_domain_handle_t));
        ptr[index].domid = info[i].domain;

        index++;
    }
    *nb_vm = index;
    return ptr;
}

int libxl_domain_suspend(libxl_ctx *ctx, libxl_domain_suspend_info *info,
                         uint32_t domid, int fd)
{
    GC_INIT(ctx);
    libxl_domain_type type = libxl__domain_type(gc, domid);
    int live = info != NULL && info->flags & XL_SUSPEND_LIVE;
    int debug = info != NULL && info->flags & XL_SUSPEND_DEBUG;
    int rc = 0;

    rc = libxl__domain_suspend_common(gc, domid, fd, type, live, debug);
    if (!rc && type == LIBXL_DOMAIN_TYPE_HVM)
        rc = libxl__domain_save_device_model(gc, domid, fd);
    GC_FREE;
    return rc;
}

int libxl_domain_pause(libxl_ctx *ctx, uint32_t domid)
{
    int ret;
    ret = xc_domain_pause(ctx->xch, domid);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "pausing domain %d", domid);
        return ERROR_FAIL;
    }
    return 0;
}

int libxl_domain_core_dump(libxl_ctx *ctx, uint32_t domid,
                           const char *filename)
{
    int ret;
    ret = xc_domain_dumpcore(ctx->xch, domid, filename);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "core dumping domain %d to %s",
                     domid, filename);
        return ERROR_FAIL;
    }
    return 0;
}

int libxl_domain_unpause(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    char *path;
    char *state;
    int ret, rc = 0;

    if (LIBXL__DOMAIN_IS_TYPE(gc,  domid, HVM)) {
        path = libxl__sprintf(gc, "/local/domain/0/device-model/%d/state", domid);
        state = libxl__xs_read(gc, XBT_NULL, path);
        if (state != NULL && !strcmp(state, "paused")) {
            libxl__xs_write(gc, XBT_NULL, libxl__sprintf(gc, "/local/domain/0/device-model/%d/command", domid), "continue");
            libxl__wait_for_device_model(gc, domid, "running",
                                         NULL, NULL, NULL);
        }
    }
    ret = xc_domain_unpause(ctx->xch, domid);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "unpausing domain %d", domid);
        rc = ERROR_FAIL;
    }
    GC_FREE;
    return rc;
}

int libxl__domain_pvcontrol_available(libxl__gc *gc, uint32_t domid)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);

    unsigned long pvdriver = 0;
    int ret;

    if (LIBXL__DOMAIN_IS_TYPE(gc, domid, PV))
        return 1;

    ret = xc_get_hvm_param(ctx->xch, domid, HVM_PARAM_CALLBACK_IRQ, &pvdriver);
    if (ret<0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting HVM callback IRQ");
        return ERROR_FAIL;
    }
    return !!pvdriver;
}

char * libxl__domain_pvcontrol_read(libxl__gc *gc, xs_transaction_t t,
                                    uint32_t domid)
{
    const char *shutdown_path;
    const char *dom_path;

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path)
        return NULL;

    shutdown_path = libxl__sprintf(gc, "%s/control/shutdown", dom_path);
    if (!shutdown_path)
        return NULL;

    return libxl__xs_read(gc, t, shutdown_path);
}

int libxl__domain_pvcontrol_write(libxl__gc *gc, xs_transaction_t t,
                                  uint32_t domid, const char *cmd)
{
    const char *shutdown_path;
    const char *dom_path;

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path)
        return ERROR_FAIL;

    shutdown_path = libxl__sprintf(gc, "%s/control/shutdown", dom_path);
    if (!shutdown_path)
        return ERROR_FAIL;

    return libxl__xs_write(gc, t, shutdown_path, "%s", cmd);
}

static int libxl__domain_pvcontrol(libxl__gc *gc, uint32_t domid,
                                   const char *cmd)
{
    int ret;

    ret = libxl__domain_pvcontrol_available(gc, domid);
    if (ret < 0)
        return ret;

    if (!ret)
        return ERROR_NOPARAVIRT;

    return libxl__domain_pvcontrol_write(gc, XBT_NULL, domid, cmd);
}

int libxl_domain_shutdown(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    int ret;
    ret = libxl__domain_pvcontrol(gc, domid, "poweroff");
    GC_FREE;
    return ret;
}

int libxl_domain_reboot(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    int ret;
    ret = libxl__domain_pvcontrol(gc, domid, "reboot");
    GC_FREE;
    return ret;
}

int libxl_get_wait_fd(libxl_ctx *ctx, int *fd)
{
    *fd = xs_fileno(ctx->xsh);
    return 0;
}

int libxl_wait_for_domain_death(libxl_ctx *ctx, uint32_t domid, libxl_waiter *waiter)
{
    waiter->path = strdup("@releaseDomain");
    if (asprintf(&(waiter->token), "%d", LIBXL_EVENT_TYPE_DOMAIN_DEATH) < 0)
        return -1;
    if (!xs_watch(ctx->xsh, waiter->path, waiter->token))
        return -1;
    return 0;
}

int libxl_wait_for_disk_ejects(libxl_ctx *ctx, uint32_t guest_domid, libxl_device_disk *disks, int num_disks, libxl_waiter *waiter)
{
    GC_INIT(ctx);
    int i, rc = -1;
    uint32_t domid = libxl_get_stubdom_id(ctx, guest_domid);

    if (!domid)
        domid = guest_domid;

    for (i = 0; i < num_disks; i++) {
        if (asprintf(&(waiter[i].path), "%s/device/vbd/%d/eject",
                     libxl__xs_get_dompath(gc, domid),
                     libxl__device_disk_dev_number(disks[i].vdev,
                                                   NULL, NULL)) < 0)
            goto out;
        if (asprintf(&(waiter[i].token), "%d", LIBXL_EVENT_TYPE_DISK_EJECT) < 0)
            goto out;
        xs_watch(ctx->xsh, waiter[i].path, waiter[i].token);
    }
    rc = 0;
out:
    GC_FREE;
    return rc;
}

int libxl_get_event(libxl_ctx *ctx, libxl_event *event)
{
    unsigned int num;
    char **events = xs_read_watch(ctx->xsh, &num);
    if (num != 2) {
        free(events);
        return ERROR_FAIL;
    }
    event->path = strdup(events[XS_WATCH_PATH]);
    event->token = strdup(events[XS_WATCH_TOKEN]);
    event->type = atoi(event->token);
    free(events);
    return 0;
}

int libxl_stop_waiting(libxl_ctx *ctx, libxl_waiter *waiter)
{
    if (!xs_unwatch(ctx->xsh, waiter->path, waiter->token))
        return ERROR_FAIL;
    else
        return 0;
}

int libxl_free_event(libxl_event *event)
{
    free(event->path);
    free(event->token);
    return 0;
}

int libxl_free_waiter(libxl_waiter *waiter)
{
    free(waiter->path);
    free(waiter->token);
    return 0;
}

int libxl_event_get_domain_death_info(libxl_ctx *ctx, uint32_t domid, libxl_event *event, libxl_dominfo *info)
{
    if (libxl_domain_info(ctx, info, domid) < 0)
        return 0;

    if (info->running || (!info->shutdown && !info->dying))
        return ERROR_INVAL;

    return 1;
}

int libxl_event_get_disk_eject_info(libxl_ctx *ctx, uint32_t domid, libxl_event *event, libxl_device_disk *disk)
{
    GC_INIT(ctx);
    char *path;
    char *backend;
    char *value;
    char backend_type[BACKEND_STRING_SIZE+1];

    value = libxl__xs_read(gc, XBT_NULL, event->path);

    if (!value || strcmp(value,  "eject")) {
        GC_FREE;
        return 0;
    }

    path = strdup(event->path);
    path[strlen(path) - 6] = '\0';
    backend = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/backend", path));

    sscanf(backend,
            "/local/domain/%d/backend/%" TOSTRING(BACKEND_STRING_SIZE) "[a-z]/%*d/%*d",
            &disk->backend_domid, backend_type);
    if (!strcmp(backend_type, "tap") || !strcmp(backend_type, "vbd")) {
        disk->backend = LIBXL_DISK_BACKEND_TAP;
    } else if (!strcmp(backend_type, "qdisk")) {
        disk->backend = LIBXL_DISK_BACKEND_QDISK;
    } else {
        disk->backend = LIBXL_DISK_BACKEND_UNKNOWN;
    }

    disk->pdev_path = strdup("");
    disk->format = LIBXL_DISK_FORMAT_EMPTY;
    /* this value is returned to the user: do not free right away */
    disk->vdev = xs_read(ctx->xsh, XBT_NULL, libxl__sprintf(gc, "%s/dev", backend), NULL);
    disk->removable = 1;
    disk->readwrite = 0;
    disk->is_cdrom = 1;

    free(path);
    GC_FREE;
    return 1;
}

int libxl_domain_destroy(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    libxl_dominfo dominfo;
    char *dom_path;
    char *vm_path;
    char *pid;
    int rc, dm_present;

    rc = libxl_domain_info(ctx, &dominfo, domid);
    switch(rc) {
    case 0:
        break;
    case ERROR_INVAL:
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "non-existant domain %d", domid);
    default:
        return rc;
    }

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        dm_present = 1;
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        pid = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "/local/domain/%d/image/device-model-pid", domid));
        dm_present = (pid != NULL);
        break;
    default:
        abort();
    }

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path) {
        rc = ERROR_FAIL;
        goto out;
    }

    if (libxl__device_pci_destroy_all(gc, domid) < 0)
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "pci shutdown failed for domid %d", domid);
    rc = xc_domain_pause(ctx->xch, domid);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc, "xc_domain_pause failed for %d", domid);
    }
    if (dm_present) {
        if (libxl__destroy_device_model(gc, domid) < 0)
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "libxl__destroy_device_model failed for %d", domid);

        libxl__qmp_cleanup(gc, domid);
    }
    if (libxl__devices_destroy(gc, domid) < 0)
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, 
                   "libxl__devices_destroy failed for %d", domid);

    vm_path = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/vm", dom_path));
    if (vm_path)
        if (!xs_rm(ctx->xsh, XBT_NULL, vm_path))
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "xs_rm failed for %s", vm_path);

    if (!xs_rm(ctx->xsh, XBT_NULL, dom_path))
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "xs_rm failed for %s", dom_path);

    xs_rm(ctx->xsh, XBT_NULL, libxl__xs_libxl_path(gc, domid));

    libxl__userdata_destroyall(gc, domid);

    rc = xc_domain_destroy(ctx->xch, domid);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc, "xc_domain_destroy failed for %d", domid);
        rc = ERROR_FAIL;
        goto out;
    }
    rc = 0;
out:
    GC_FREE;
    return rc;
}

int libxl_console_exec(libxl_ctx *ctx, uint32_t domid, int cons_num, libxl_console_type type)
{
    GC_INIT(ctx);
    char *p = libxl__sprintf(gc, "%s/xenconsole", libxl_private_bindir_path());
    char *domid_s = libxl__sprintf(gc, "%d", domid);
    char *cons_num_s = libxl__sprintf(gc, "%d", cons_num);
    char *cons_type_s;

    switch (type) {
    case LIBXL_CONSOLE_TYPE_PV:
        cons_type_s = "pv";
        break;
    case LIBXL_CONSOLE_TYPE_SERIAL:
        cons_type_s = "serial";
        break;
    default:
        goto out;
    }

    execl(p, p, domid_s, "--num", cons_num_s, "--type", cons_type_s, (void *)NULL);

out:
    GC_FREE;
    return ERROR_FAIL;
}

int libxl_primary_console_exec(libxl_ctx *ctx, uint32_t domid_vm)
{
    GC_INIT(ctx);
    uint32_t stubdomid = libxl_get_stubdom_id(ctx, domid_vm);
    int rc;
    if (stubdomid)
        rc = libxl_console_exec(ctx, stubdomid,
                                STUBDOM_CONSOLE_SERIAL, LIBXL_CONSOLE_TYPE_PV);
    else {
        switch (libxl__domain_type(gc, domid_vm)) {
        case LIBXL_DOMAIN_TYPE_HVM:
            rc = libxl_console_exec(ctx, domid_vm, 0, LIBXL_CONSOLE_TYPE_SERIAL);
            break;
        case LIBXL_DOMAIN_TYPE_PV:
            rc = libxl_console_exec(ctx, domid_vm, 0, LIBXL_CONSOLE_TYPE_PV);
            break;
        default:
            abort();
        }
    }
    GC_FREE;
    return rc;
}

int libxl_vncviewer_exec(libxl_ctx *ctx, uint32_t domid, int autopass)
{
    GC_INIT(ctx);
    const char *vnc_port;
    const char *vnc_listen = NULL, *vnc_pass = NULL;
    int port = 0, autopass_fd = -1;
    char *vnc_bin, *args[] = {
        "vncviewer",
        NULL, /* hostname:display */
        NULL, /* -autopass */
        NULL,
    };

    vnc_port = libxl__xs_read(gc, XBT_NULL,
                            libxl__sprintf(gc,
                            "/local/domain/%d/console/vnc-port", domid));
    if ( vnc_port )
        port = atoi(vnc_port) - 5900;

    vnc_listen = libxl__xs_read(gc, XBT_NULL,
                                libxl__sprintf(gc,
                            "/local/domain/%d/console/vnc-listen", domid));

    if ( autopass )
        vnc_pass = libxl__xs_read(gc, XBT_NULL,
                                  libxl__sprintf(gc,
                            "/local/domain/%d/console/vnc-pass", domid));

    if ( NULL == vnc_listen )
        vnc_listen = "localhost";

    if ( (vnc_bin = getenv("VNCVIEWER")) )
        args[0] = vnc_bin;

    args[1] = libxl__sprintf(gc, "%s:%d", vnc_listen, port);

    if ( vnc_pass ) {
        char tmpname[] = "/tmp/vncautopass.XXXXXX";
        autopass_fd = mkstemp(tmpname);
        if ( autopass_fd < 0 ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                             "mkstemp %s failed", tmpname);
            goto x_fail;
        }

        if ( unlink(tmpname) ) {
            /* should never happen */
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                             "unlink %s failed", tmpname);
            goto x_fail;
        }

        if ( libxl_write_exactly(ctx, autopass_fd, vnc_pass, strlen(vnc_pass),
                                    tmpname, "vnc password") )
            goto x_fail;

        if ( lseek(autopass_fd, SEEK_SET, 0) ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                             "rewind %s (autopass) failed", tmpname);
            goto x_fail;
        }

        args[2] = "-autopass";
    }

    libxl__exec(autopass_fd, -1, -1, args[0], args);
    abort();

 x_fail:
    GC_FREE;
    return ERROR_FAIL;
}

/******************************************************************************/

int libxl_device_disk_init(libxl_ctx *ctx, libxl_device_disk *disk)
{
    memset(disk, 0x00, sizeof(libxl_device_disk));
    return 0;
}

static int libxl__device_from_disk(libxl__gc *gc, uint32_t domid,
                                   libxl_device_disk *disk,
                                   libxl__device *device)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    int devid;

    devid = libxl__device_disk_dev_number(disk->vdev, NULL, NULL);
    if (devid==-1) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Invalid or unsupported"
               " virtual disk identifier %s", disk->vdev);
        return ERROR_INVAL;
    }

    device->backend_domid = disk->backend_domid;
    device->backend_devid = devid;

    switch (disk->backend) {
        case LIBXL_DISK_BACKEND_PHY:
            device->backend_kind = LIBXL__DEVICE_KIND_VBD;
            break;
        case LIBXL_DISK_BACKEND_TAP:
            device->backend_kind = LIBXL__DEVICE_KIND_VBD;
            break;
        case LIBXL_DISK_BACKEND_QDISK:
            device->backend_kind = LIBXL__DEVICE_KIND_QDISK;
            break;
        default:
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "unrecognized disk backend type: %d\n",
                       disk->backend);
            return ERROR_INVAL;
    }

    device->domid = domid;
    device->devid = devid;
    device->kind  = LIBXL__DEVICE_KIND_VBD;

    return 0;
}

int libxl_device_disk_add(libxl_ctx *ctx, uint32_t domid, libxl_device_disk *disk)
{
    GC_INIT(ctx);
    flexarray_t *front;
    flexarray_t *back;
    char *dev;
    libxl__device device;
    int major, minor, rc;

    rc = libxl__device_disk_set_backend(gc, disk);
    if (rc) goto out;

    rc = libxl__device_disk_set_backend(gc, disk);
    if (rc) goto out;

    front = flexarray_make(16, 1);
    if (!front) {
        rc = ERROR_NOMEM;
        goto out;
    }
    back = flexarray_make(16, 1);
    if (!back) {
        rc = ERROR_NOMEM;
        goto out_free;
    }

    if (disk->script) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "External block scripts"
                   " not yet supported, sorry");
        rc = ERROR_INVAL;
        goto out_free;
    }

    rc = libxl__device_from_disk(gc, domid, disk, &device);
    if (rc != 0) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Invalid or unsupported"
               " virtual disk identifier %s", disk->vdev);
        goto out_free;
    }

    switch (disk->backend) {
        case LIBXL_DISK_BACKEND_PHY:
            dev = disk->pdev_path;
    do_backend_phy:
            libxl__device_physdisk_major_minor(dev, &major, &minor);
            flexarray_append(back, "physical-device");
            flexarray_append(back, libxl__sprintf(gc, "%x:%x", major, minor));

            flexarray_append(back, "params");
            flexarray_append(back, dev);

            assert(device.backend_kind == LIBXL__DEVICE_KIND_VBD);
            break;
        case LIBXL_DISK_BACKEND_TAP:
            dev = libxl__blktap_devpath(gc, disk->pdev_path, disk->format);
            if (!dev) {
                rc = ERROR_FAIL;
                goto out_free;
            }
            flexarray_append(back, "tapdisk-params");
            flexarray_append(back, libxl__sprintf(gc, "%s:%s",
                libxl__device_disk_string_of_format(disk->format),
                disk->pdev_path));

            /* now create a phy device to export the device to the guest */
            goto do_backend_phy;
        case LIBXL_DISK_BACKEND_QDISK:
            flexarray_append(back, "params");
            flexarray_append(back, libxl__sprintf(gc, "%s:%s",
                          libxl__device_disk_string_of_format(disk->format), disk->pdev_path));
            assert(device.backend_kind == LIBXL__DEVICE_KIND_QDISK);
            break;
        default:
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "unrecognized disk backend type: %d\n", disk->backend);
            rc = ERROR_INVAL;
            goto out_free;
    }

    flexarray_append(back, "frontend-id");
    flexarray_append(back, libxl__sprintf(gc, "%d", domid));
    flexarray_append(back, "online");
    flexarray_append(back, "1");
    flexarray_append(back, "removable");
    flexarray_append(back, libxl__sprintf(gc, "%d", (disk->removable) ? 1 : 0));
    flexarray_append(back, "bootable");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    flexarray_append(back, "state");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    flexarray_append(back, "dev");
    flexarray_append(back, disk->vdev);
    flexarray_append(back, "type");
    flexarray_append(back, libxl__device_disk_string_of_backend(disk->backend));
    flexarray_append(back, "mode");
    flexarray_append(back, disk->readwrite ? "w" : "r");
    flexarray_append(back, "device-type");
    flexarray_append(back, disk->is_cdrom ? "cdrom" : "disk");

    flexarray_append(front, "backend-id");
    flexarray_append(front, libxl__sprintf(gc, "%d", disk->backend_domid));
    flexarray_append(front, "state");
    flexarray_append(front, libxl__sprintf(gc, "%d", 1));
    flexarray_append(front, "virtual-device");
    flexarray_append(front, libxl__sprintf(gc, "%d", device.devid));
    flexarray_append(front, "device-type");
    flexarray_append(front, disk->is_cdrom ? "cdrom" : "disk");

    libxl__device_generic_add(gc, &device,
                             libxl__xs_kvs_of_flexarray(gc, back, back->count),
                             libxl__xs_kvs_of_flexarray(gc, front, front->count));

    rc = 0;

out_free:
    flexarray_free(back);
    flexarray_free(front);
out:
    GC_FREE;
    return rc;
}

int libxl_device_disk_remove(libxl_ctx *ctx, uint32_t domid,
                             libxl_device_disk *disk)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_disk(gc, domid, disk, &device);
    if (rc != 0) goto out;

    rc = libxl__device_remove(gc, &device, 1);
out:
    GC_FREE;
    return rc;
}

int libxl_device_disk_destroy(libxl_ctx *ctx, uint32_t domid,
                              libxl_device_disk *disk)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_disk(gc, domid, disk, &device);
    if (rc != 0) goto out;

    rc = libxl__device_destroy(gc, &device);
out:
    GC_FREE;
    return rc;
}

static void libxl__device_disk_from_xs_be(libxl__gc *gc,
                                          const char *be_path,
                                          libxl_device_disk *disk)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    unsigned int len;
    char *tmp;

    libxl_device_disk_init(ctx, disk);

    tmp = xs_read(ctx->xsh, XBT_NULL,
                  libxl__sprintf(gc, "%s/params", be_path), &len);
    if (tmp && strchr(tmp, ':')) {
        disk->pdev_path = strdup(strchr(tmp, ':') + 1);
        free(tmp);
    } else {
        disk->pdev_path = tmp;
    }
    libxl_string_to_backend(ctx,
                        libxl__xs_read(gc, XBT_NULL,
                                       libxl__sprintf(gc, "%s/type", be_path)),
                        &(disk->backend));
    disk->vdev = xs_read(ctx->xsh, XBT_NULL,
                         libxl__sprintf(gc, "%s/dev", be_path), &len);
    tmp = libxl__xs_read(gc, XBT_NULL, libxl__sprintf
                         (gc, "%s/removable", be_path));

    if (tmp)
        disk->removable = atoi(tmp);
    else
        disk->removable = 0;

    tmp = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/mode", be_path));
    if (!strcmp(tmp, "w"))
        disk->readwrite = 1;
    else
        disk->readwrite = 0;

    tmp = libxl__xs_read(gc, XBT_NULL,
                         libxl__sprintf(gc, "%s/device-type", be_path));
    disk->is_cdrom = !strcmp(tmp, "cdrom");

    disk->format = LIBXL_DISK_FORMAT_UNKNOWN;
}

int libxl_devid_to_device_disk(libxl_ctx *ctx, uint32_t domid,
                               int devid, libxl_device_disk *disk)
{
    GC_INIT(ctx);
    char *dompath, *path;
    int rc = ERROR_FAIL;

    libxl_device_disk_init(ctx, disk);

    dompath = libxl__xs_get_dompath(gc, domid);
    if (!dompath) {
        goto out;
    }
    path = libxl__xs_read(gc, XBT_NULL,
                          libxl__sprintf(gc, "%s/device/vbd/%d/backend",
                                         dompath, devid));
    if (!path)
        goto out;

    libxl__device_disk_from_xs_be(gc, path, disk);

    rc = 0;
out:
    GC_FREE;
    return rc;
}


static int libxl__append_disk_list_of_type(libxl__gc *gc,
                                           uint32_t domid,
                                           const char *type,
                                           libxl_device_disk **disks,
                                           int *ndisks)
{
    char *be_path = NULL;
    char **dir = NULL;
    unsigned int n = 0;
    libxl_device_disk *pdisk = NULL, *pdisk_end = NULL;

    be_path = libxl__sprintf(gc, "%s/backend/%s/%d",
                             libxl__xs_get_dompath(gc, 0), type, domid);
    dir = libxl__xs_directory(gc, XBT_NULL, be_path, &n);
    if (dir) {
        libxl_device_disk *tmp;
        tmp = realloc(*disks, sizeof (libxl_device_disk) * (*ndisks + n));
        if (tmp == NULL)
            return ERROR_NOMEM;
        *disks = tmp;
        pdisk = *disks + *ndisks;
        *ndisks += n;
        pdisk_end = *disks + *ndisks;
        for (; pdisk < pdisk_end; pdisk++, dir++) {
            const char *p;
            p = libxl__sprintf(gc, "%s/%s", be_path, *dir);
            libxl__device_disk_from_xs_be(gc, p, pdisk);
            pdisk->backend_domid = 0;
        }
    }
    return 0;
}

libxl_device_disk *libxl_device_disk_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    libxl_device_disk *disks = NULL;
    int rc;

    *num = 0;

    rc = libxl__append_disk_list_of_type(gc, domid, "vbd", &disks, num);
    if (rc) goto out_err;

    rc = libxl__append_disk_list_of_type(gc, domid, "tap", &disks, num);
    if (rc) goto out_err;

    rc = libxl__append_disk_list_of_type(gc, domid, "qdisk", &disks, num);
    if (rc) goto out_err;

    GC_FREE;
    return disks;

out_err:
    LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Unable to list disks");
    while (disks && *num) {
        (*num)--;
        libxl_device_disk_dispose(&disks[*num]);
    }
    free(disks);
    return NULL;
}

int libxl_device_disk_getinfo(libxl_ctx *ctx, uint32_t domid,
                              libxl_device_disk *disk, libxl_diskinfo *diskinfo)
{
    GC_INIT(ctx);
    char *dompath, *diskpath;
    char *val;

    dompath = libxl__xs_get_dompath(gc, domid);
    diskinfo->devid = libxl__device_disk_dev_number(disk->vdev, NULL, NULL);

    /* tap devices entries in xenstore are written as vbd devices. */
    diskpath = libxl__sprintf(gc, "%s/device/vbd/%d", dompath, diskinfo->devid);
    diskinfo->backend = xs_read(ctx->xsh, XBT_NULL,
                                libxl__sprintf(gc, "%s/backend", diskpath), NULL);
    if (!diskinfo->backend) {
        GC_FREE;
        return ERROR_FAIL;
    }
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/backend-id", diskpath));
    diskinfo->backend_id = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/state", diskpath));
    diskinfo->state = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/event-channel", diskpath));
    diskinfo->evtch = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/ring-ref", diskpath));
    diskinfo->rref = val ? strtoul(val, NULL, 10) : -1;
    diskinfo->frontend = xs_read(ctx->xsh, XBT_NULL,
                                 libxl__sprintf(gc, "%s/frontend", diskinfo->backend), NULL);
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/frontend-id", diskinfo->backend));
    diskinfo->frontend_id = val ? strtoul(val, NULL, 10) : -1;

    GC_FREE;
    return 0;
}

int libxl_cdrom_insert(libxl_ctx *ctx, uint32_t domid, libxl_device_disk *disk)
{
    int num, i;
    uint32_t stubdomid;
    libxl_device_disk *disks;
    int ret = ERROR_FAIL;

    if (!disk->pdev_path) {
        disk->pdev_path = strdup("");
        disk->format = LIBXL_DISK_FORMAT_EMPTY;
    }
    disks = libxl_device_disk_list(ctx, domid, &num);
    for (i = 0; i < num; i++) {
        if (disks[i].is_cdrom && !strcmp(disk->vdev, disks[i].vdev))
            /* found */
            break;
    }
    if (i == num) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Virtual device not found");
        goto out;
    }

    ret = 0;

    libxl_device_disk_remove(ctx, domid, disks + i);
    libxl_device_disk_add(ctx, domid, disk);
    stubdomid = libxl_get_stubdom_id(ctx, domid);
    if (stubdomid) {
        libxl_device_disk_remove(ctx, stubdomid, disks + i);
        libxl_device_disk_add(ctx, stubdomid, disk);
    }
out:
    for (i = 0; i < num; i++)
        libxl_device_disk_dispose(&disks[i]);
    free(disks);
    return ret;
}

char * libxl_device_disk_local_attach(libxl_ctx *ctx, libxl_device_disk *disk)
{
    GC_INIT(ctx);
    char *dev = NULL;
    char *ret = NULL;
    int rc;

    rc = libxl__device_disk_set_backend(gc, disk);
    if (rc) goto out;

    switch (disk->backend) {
        case LIBXL_DISK_BACKEND_PHY:
            LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "locally attaching PHY disk %s",
                       disk->pdev_path);
            dev = disk->pdev_path;
            break;
        case LIBXL_DISK_BACKEND_TAP:
            switch (disk->format) {
            case LIBXL_DISK_FORMAT_RAW:
                /* optimise away the early tapdisk attach in this case */
                LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "locally attaching"
                           " tap disk %s directly (ie without using blktap)",
                           disk->pdev_path);
                dev = disk->pdev_path;
                break;
            case LIBXL_DISK_FORMAT_VHD:
                dev = libxl__blktap_devpath(gc, disk->pdev_path,
                                            disk->format);
                break;
            case LIBXL_DISK_FORMAT_QCOW:
            case LIBXL_DISK_FORMAT_QCOW2:
                abort(); /* prevented by libxl__device_disk_set_backend */
            default:
                LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                           "unrecognized disk format: %d", disk->format);
                break;
            }
            break;
        case LIBXL_DISK_BACKEND_QDISK:
            if (disk->format != LIBXL_DISK_FORMAT_RAW) {
                LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "cannot locally"
                           " attach a qdisk image if the format is not raw");
                break;
            }
            LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "locally attaching qdisk %s\n",
                       disk->pdev_path);
            dev = disk->pdev_path;
            break;
        default:
            LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "unrecognized disk backend "
                "type: %d", disk->backend);
            break;
    }

 out:
    if (dev != NULL)
        ret = strdup(dev);
    GC_FREE;
    return ret;
}

int libxl_device_disk_local_detach(libxl_ctx *ctx, libxl_device_disk *disk)
{
    /* Nothing to do for PHYSTYPE_PHY. */

    /*
     * For other device types assume that the blktap2 process is
     * needed by the soon to be started domain and do nothing.
     */

    return 0;
}

/******************************************************************************/
int libxl_device_nic_init(libxl_ctx *ctx, libxl_device_nic *nic)
{
    const uint8_t *r;
    libxl_uuid uuid;

    libxl_uuid_generate(&uuid);
    r = libxl_uuid_bytearray(&uuid);
    memset(nic, '\0', sizeof(*nic));

    nic->backend_domid = 0;
    nic->devid = -1;
    nic->mtu = 1492;
    nic->model = strdup("rtl8139");
    nic->mac[0] = 0x00;
    nic->mac[1] = 0x16;
    nic->mac[2] = 0x3e;
    nic->mac[3] = r[0] & 0x7f;
    nic->mac[4] = r[1];
    nic->mac[5] = r[2];
    nic->ifname = NULL;
    nic->bridge = strdup("xenbr0");
    nic->ip = NULL;
    if ( asprintf(&nic->script, "%s/vif-bridge",
               libxl_xen_script_dir_path()) < 0 )
        return ERROR_FAIL;
    nic->nictype = LIBXL_NIC_TYPE_IOEMU;
    return 0;
}

static int libxl__device_from_nic(libxl__gc *gc, uint32_t domid,
                                  libxl_device_nic *nic,
                                  libxl__device *device)
{
    device->backend_devid    = nic->devid;
    device->backend_domid    = nic->backend_domid;
    device->backend_kind     = LIBXL__DEVICE_KIND_VIF;
    device->devid            = nic->devid;
    device->domid            = domid;
    device->kind             = LIBXL__DEVICE_KIND_VIF;

    return 0;
}

int libxl_device_nic_add(libxl_ctx *ctx, uint32_t domid, libxl_device_nic *nic)
{
    GC_INIT(ctx);
    flexarray_t *front;
    flexarray_t *back;
    libxl__device device;
    char *dompath, **l;
    unsigned int nb, rc;

    front = flexarray_make(16, 1);
    if (!front) {
        rc = ERROR_NOMEM;
        goto out;
    }
    back = flexarray_make(16, 1);
    if (!back) {
        rc = ERROR_NOMEM;
        goto out_free;
    }

    if (nic->devid == -1) {
        if (!(dompath = libxl__xs_get_dompath(gc, domid))) {
            rc = ERROR_FAIL;
            goto out_free;
        }
        if (!(l = libxl__xs_directory(gc, XBT_NULL,
                                     libxl__sprintf(gc, "%s/device/vif", dompath), &nb))) {
            nic->devid = 0;
        } else {
            nic->devid = strtoul(l[nb - 1], NULL, 10) + 1;
        }
    }

    rc = libxl__device_from_nic(gc, domid, nic, &device);
    if ( rc != 0 ) goto out_free;

    flexarray_append(back, "frontend-id");
    flexarray_append(back, libxl__sprintf(gc, "%d", domid));
    flexarray_append(back, "online");
    flexarray_append(back, "1");
    flexarray_append(back, "state");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    if (nic->script) {
        flexarray_append(back, "script");
        flexarray_append(back, nic->script[0]=='/' ? nic->script
                         : libxl__sprintf(gc, "%s/%s",
                                          libxl_xen_script_dir_path(),
                                          nic->script));
    }

    if (nic->ifname) {
        flexarray_append(back, "vifname");
        flexarray_append(back, nic->ifname);
    }

    flexarray_append(back, "mac");
    flexarray_append(back,libxl__sprintf(gc,
                                    LIBXL_MAC_FMT, LIBXL_MAC_BYTES(nic->mac)));
    if (nic->ip) {
        flexarray_append(back, "ip");
        flexarray_append(back, libxl__strdup(gc, nic->ip));
    }

    flexarray_append(back, "bridge");
    flexarray_append(back, libxl__strdup(gc, nic->bridge));
    flexarray_append(back, "handle");
    flexarray_append(back, libxl__sprintf(gc, "%d", nic->devid));

    flexarray_append(front, "backend-id");
    flexarray_append(front, libxl__sprintf(gc, "%d", nic->backend_domid));
    flexarray_append(front, "state");
    flexarray_append(front, libxl__sprintf(gc, "%d", 1));
    flexarray_append(front, "handle");
    flexarray_append(front, libxl__sprintf(gc, "%d", nic->devid));
    flexarray_append(front, "mac");
    flexarray_append(front, libxl__sprintf(gc,
                                    LIBXL_MAC_FMT, LIBXL_MAC_BYTES(nic->mac)));
    libxl__device_generic_add(gc, &device,
                             libxl__xs_kvs_of_flexarray(gc, back, back->count),
                             libxl__xs_kvs_of_flexarray(gc, front, front->count));

    /* FIXME: wait for plug */
    rc = 0;
out_free:
    flexarray_free(back);
    flexarray_free(front);
out:
    GC_FREE;
    return rc;
}

int libxl_device_nic_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_nic *nic)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_nic(gc, domid, nic, &device);
    if (rc != 0) goto out;

    rc = libxl__device_remove(gc, &device, 1);
out:
    GC_FREE;
    return rc;
}

int libxl_device_nic_destroy(libxl_ctx *ctx, uint32_t domid,
                                  libxl_device_nic *nic)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_nic(gc, domid, nic, &device);
    if (rc != 0) goto out;

    rc = libxl__device_destroy(gc, &device);
out:
    GC_FREE;
    return rc;
}

static void libxl__device_nic_from_xs_be(libxl__gc *gc,
                                         const char *be_path,
                                         libxl_device_nic *nic)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    unsigned int len;
    char *tmp;
    int rc;

    memset(nic, 0, sizeof(*nic));

    tmp = xs_read(ctx->xsh, XBT_NULL,
                  libxl__sprintf(gc, "%s/handle", be_path), &len);
    if ( tmp )
        nic->devid = atoi(tmp);
    else
        nic->devid = 0;

    /* nic->mtu = */

    tmp = xs_read(ctx->xsh, XBT_NULL,
                  libxl__sprintf(gc, "%s/mac", be_path), &len);
    rc = libxl__parse_mac(tmp, nic->mac);
    if (rc)
        memset(nic->mac, 0, sizeof(nic->mac));

    nic->ip = xs_read(ctx->xsh, XBT_NULL,
                      libxl__sprintf(gc, "%s/ip", be_path), &len);

    nic->bridge = xs_read(ctx->xsh, XBT_NULL,
                      libxl__sprintf(gc, "%s/bridge", be_path), &len);

    nic->script = xs_read(ctx->xsh, XBT_NULL,
                      libxl__sprintf(gc, "%s/script", be_path), &len);

    /* XXX ioemu nics are not in xenstore at all? */
    nic->nictype = LIBXL_NIC_TYPE_VIF;
    nic->model = NULL; /* XXX Only for TYPE_IOEMU */
    nic->ifname = NULL; /* XXX Only for TYPE_IOEMU */
}

int libxl_devid_to_device_nic(libxl_ctx *ctx, uint32_t domid,
                              int devid, libxl_device_nic *nic)
{
    GC_INIT(ctx);
    char *dompath, *path;
    int rc = ERROR_FAIL;

    memset(nic, 0, sizeof (libxl_device_nic));
    dompath = libxl__xs_get_dompath(gc, domid);
    if (!dompath)
        goto out;

    path = libxl__xs_read(gc, XBT_NULL,
                          libxl__sprintf(gc, "%s/device/vif/%d/backend",
                                         dompath, devid));
    if (!path)
        goto out;

    libxl__device_nic_from_xs_be(gc, path, nic);

    rc = 0;
out:
    GC_FREE;
    return rc;
}

static int libxl__append_nic_list_of_type(libxl__gc *gc,
                                           uint32_t domid,
                                           const char *type,
                                           libxl_device_nic **nics,
                                           int *nnics)
{
    char *be_path = NULL;
    char **dir = NULL;
    unsigned int n = 0;
    libxl_device_nic *pnic = NULL, *pnic_end = NULL;

    be_path = libxl__sprintf(gc, "%s/backend/%s/%d",
                             libxl__xs_get_dompath(gc, 0), type, domid);
    dir = libxl__xs_directory(gc, XBT_NULL, be_path, &n);
    if (dir) {
        libxl_device_nic *tmp;
        tmp = realloc(*nics, sizeof (libxl_device_nic) * (*nnics + n));
        if (tmp == NULL)
            return ERROR_NOMEM;
        *nics = tmp;
        pnic = *nics + *nnics;
        *nnics += n;
        pnic_end = *nics + *nnics;
        for (; pnic < pnic_end; pnic++, dir++) {
            const char *p;
            p = libxl__sprintf(gc, "%s/%s", be_path, *dir);
            libxl__device_nic_from_xs_be(gc, p, pnic);
            pnic->backend_domid = 0;
        }
    }
    return 0;
}

libxl_device_nic *libxl_device_nic_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    libxl_device_nic *nics = NULL;
    int rc;

    *num = 0;

    rc = libxl__append_nic_list_of_type(gc, domid, "vif", &nics, num);
    if (rc) goto out_err;

    GC_FREE;
    return nics;

out_err:
    LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Unable to list nics");
    while (*num) {
        (*num)--;
        libxl_device_nic_dispose(&nics[*num]);
    }
    free(nics);
    return NULL;
}

int libxl_device_nic_getinfo(libxl_ctx *ctx, uint32_t domid,
                              libxl_device_nic *nic, libxl_nicinfo *nicinfo)
{
    GC_INIT(ctx);
    char *dompath, *nicpath;
    char *val;

    dompath = libxl__xs_get_dompath(gc, domid);
    nicinfo->devid = nic->devid;

    nicpath = libxl__sprintf(gc, "%s/device/vif/%d", dompath, nicinfo->devid);
    nicinfo->backend = xs_read(ctx->xsh, XBT_NULL,
                                libxl__sprintf(gc, "%s/backend", nicpath), NULL);
    if (!nicinfo->backend) {
        GC_FREE;
        return ERROR_FAIL;
    }
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/backend-id", nicpath));
    nicinfo->backend_id = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/state", nicpath));
    nicinfo->state = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/event-channel", nicpath));
    nicinfo->evtch = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/tx-ring-ref", nicpath));
    nicinfo->rref_tx = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/rx-ring-ref", nicpath));
    nicinfo->rref_rx = val ? strtoul(val, NULL, 10) : -1;
    nicinfo->frontend = xs_read(ctx->xsh, XBT_NULL,
                                 libxl__sprintf(gc, "%s/frontend", nicinfo->backend), NULL);
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/frontend-id", nicinfo->backend));
    nicinfo->frontend_id = val ? strtoul(val, NULL, 10) : -1;

    GC_FREE;
    return 0;
}

/******************************************************************************/
int libxl__device_console_add(libxl__gc *gc, uint32_t domid,
                              libxl_device_console *console,
                              libxl__domain_build_state *state)
{
    flexarray_t *front;
    flexarray_t *back;
    libxl__device device;
    int rc;

    if (console->devid && state) {
        rc = ERROR_INVAL;
        goto out;
    }

    front = flexarray_make(16, 1);
    if (!front) {
        rc = ERROR_NOMEM;
        goto out;
    }
    back = flexarray_make(16, 1);
    if (!back) {
        rc = ERROR_NOMEM;
        goto out_free;
    }

    device.backend_devid = console->devid;
    device.backend_domid = console->backend_domid;
    device.backend_kind = LIBXL__DEVICE_KIND_CONSOLE;
    device.devid = console->devid;
    device.domid = domid;
    device.kind = LIBXL__DEVICE_KIND_CONSOLE;

    flexarray_append(back, "frontend-id");
    flexarray_append(back, libxl__sprintf(gc, "%d", domid));
    flexarray_append(back, "online");
    flexarray_append(back, "1");
    flexarray_append(back, "state");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    flexarray_append(back, "domain");
    flexarray_append(back, libxl__domid_to_name(gc, domid));
    flexarray_append(back, "protocol");
    flexarray_append(back, LIBXL_XENCONSOLE_PROTOCOL);

    flexarray_append(front, "backend-id");
    flexarray_append(front, libxl__sprintf(gc, "%d", console->backend_domid));
    flexarray_append(front, "limit");
    flexarray_append(front, libxl__sprintf(gc, "%d", LIBXL_XENCONSOLE_LIMIT));
    flexarray_append(front, "type");
    if (console->consback == LIBXL_CONSOLE_BACKEND_XENCONSOLED)
        flexarray_append(front, "xenconsoled");
    else
        flexarray_append(front, "ioemu");
    flexarray_append(front, "output");
    flexarray_append(front, console->output);

    if (state) {
        flexarray_append(front, "port");
        flexarray_append(front, libxl__sprintf(gc, "%"PRIu32, state->console_port));
        flexarray_append(front, "ring-ref");
        flexarray_append(front, libxl__sprintf(gc, "%lu", state->console_mfn));
    } else {
        flexarray_append(front, "state");
        flexarray_append(front, libxl__sprintf(gc, "%d", 1));
        flexarray_append(front, "protocol");
        flexarray_append(front, LIBXL_XENCONSOLE_PROTOCOL);
    }

    libxl__device_generic_add(gc, &device,
                             libxl__xs_kvs_of_flexarray(gc, back, back->count),
                             libxl__xs_kvs_of_flexarray(gc, front, front->count));
    rc = 0;
out_free:
    flexarray_free(back);
    flexarray_free(front);
out:
    return rc;
}

/******************************************************************************/
int libxl_device_vkb_init(libxl_ctx *ctx, libxl_device_vkb *vkb)
{
    memset(vkb, 0x00, sizeof(libxl_device_vkb));
    return 0;
}

static int libxl__device_from_vkb(libxl__gc *gc, uint32_t domid,
                                  libxl_device_vkb *vkb,
                                  libxl__device *device)
{
    device->backend_devid = vkb->devid;
    device->backend_domid = vkb->backend_domid;
    device->backend_kind = LIBXL__DEVICE_KIND_VKBD;
    device->devid = vkb->devid;
    device->domid = domid;
    device->kind = LIBXL__DEVICE_KIND_VKBD;

    return 0;
}

int libxl_device_vkb_add(libxl_ctx *ctx, uint32_t domid, libxl_device_vkb *vkb)
{
    GC_INIT(ctx);
    flexarray_t *front;
    flexarray_t *back;
    libxl__device device;
    int rc;

    front = flexarray_make(16, 1);
    if (!front) {
        rc = ERROR_NOMEM;
        goto out;
    }
    back = flexarray_make(16, 1);
    if (!back) {
        rc = ERROR_NOMEM;
        goto out_free;
    }

    rc = libxl__device_from_vkb(gc, domid, vkb, &device);
    if (rc != 0) goto out_free;

    flexarray_append(back, "frontend-id");
    flexarray_append(back, libxl__sprintf(gc, "%d", domid));
    flexarray_append(back, "online");
    flexarray_append(back, "1");
    flexarray_append(back, "state");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    flexarray_append(back, "domain");
    flexarray_append(back, libxl__domid_to_name(gc, domid));

    flexarray_append(front, "backend-id");
    flexarray_append(front, libxl__sprintf(gc, "%d", vkb->backend_domid));
    flexarray_append(front, "state");
    flexarray_append(front, libxl__sprintf(gc, "%d", 1));

    libxl__device_generic_add(gc, &device,
                             libxl__xs_kvs_of_flexarray(gc, back, back->count),
                             libxl__xs_kvs_of_flexarray(gc, front, front->count));
    rc = 0;
out_free:
    flexarray_free(back);
    flexarray_free(front);
out:
    GC_FREE;
    return rc;
}

int libxl_device_vkb_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_vkb *vkb)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_vkb(gc, domid, vkb, &device);
    if (rc != 0) goto out;

    rc = libxl__device_remove(gc, &device, 1);
out:
    GC_FREE;
    return rc;
}

int libxl_device_vkb_destroy(libxl_ctx *ctx, uint32_t domid,
                                  libxl_device_vkb *vkb)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_vkb(gc, domid, vkb, &device);
    if (rc != 0) goto out;

    rc = libxl__device_destroy(gc, &device);
out:
    GC_FREE;
    return rc;
}

/******************************************************************************/
int libxl_device_vfb_init(libxl_ctx *ctx, libxl_device_vfb *vfb)
{
    memset(vfb, 0x00, sizeof(libxl_device_vfb));
    vfb->display = NULL;
    vfb->xauthority = NULL;
    vfb->vnc = 1;
    vfb->vncpasswd = NULL;
    vfb->vnclisten = strdup("127.0.0.1");
    vfb->vncdisplay = 0;
    vfb->vncunused = 1;
    vfb->keymap = NULL;
    vfb->sdl = 0;
    vfb->opengl = 0;
    return 0;
}

static int libxl__device_from_vfb(libxl__gc *gc, uint32_t domid,
                                  libxl_device_vfb *vfb,
                                  libxl__device *device)
{
    device->backend_devid = vfb->devid;
    device->backend_domid = vfb->backend_domid;
    device->backend_kind = LIBXL__DEVICE_KIND_VFB;
    device->devid = vfb->devid;
    device->domid = domid;
    device->kind = LIBXL__DEVICE_KIND_VFB;
    return 0;
}

int libxl_device_vfb_add(libxl_ctx *ctx, uint32_t domid, libxl_device_vfb *vfb)
{
    GC_INIT(ctx);
    flexarray_t *front;
    flexarray_t *back;
    libxl__device device;
    int rc;

    front = flexarray_make(16, 1);
    if (!front) {
        rc = ERROR_NOMEM;
        goto out;
    }
    back = flexarray_make(16, 1);
    if (!back) {
        rc = ERROR_NOMEM;
        goto out_free;
    }

    rc = libxl__device_from_vfb(gc, domid, vfb, &device);
    if (rc != 0) goto out_free;

    flexarray_append_pair(back, "frontend-id", libxl__sprintf(gc, "%d", domid));
    flexarray_append_pair(back, "online", "1");
    flexarray_append_pair(back, "state", libxl__sprintf(gc, "%d", 1));
    flexarray_append_pair(back, "domain", libxl__domid_to_name(gc, domid));
    flexarray_append_pair(back, "vnc", libxl__sprintf(gc, "%d", vfb->vnc));
    flexarray_append_pair(back, "vnclisten", vfb->vnclisten);
    flexarray_append_pair(back, "vncpasswd", vfb->vncpasswd);
    flexarray_append_pair(back, "vncdisplay", libxl__sprintf(gc, "%d", vfb->vncdisplay));
    flexarray_append_pair(back, "vncunused", libxl__sprintf(gc, "%d", vfb->vncunused));
    flexarray_append_pair(back, "sdl", libxl__sprintf(gc, "%d", vfb->sdl));
    flexarray_append_pair(back, "opengl", libxl__sprintf(gc, "%d", vfb->opengl));
    if (vfb->xauthority) {
        flexarray_append_pair(back, "xauthority", vfb->xauthority);
    }
    if (vfb->display) {
        flexarray_append_pair(back, "display", vfb->display);
    }

    flexarray_append_pair(front, "backend-id", libxl__sprintf(gc, "%d", vfb->backend_domid));
    flexarray_append_pair(front, "state", libxl__sprintf(gc, "%d", 1));

    libxl__device_generic_add(gc, &device,
                             libxl__xs_kvs_of_flexarray(gc, back, back->count),
                             libxl__xs_kvs_of_flexarray(gc, front, front->count));
    rc = 0;
out_free:
    flexarray_free(front);
    flexarray_free(back);
out:
    GC_FREE;
    return rc;
}

int libxl_device_vfb_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_vfb *vfb)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_vfb(gc, domid, vfb, &device);
    if (rc != 0) goto out;

    rc = libxl__device_remove(gc, &device, 1);
out:
    GC_FREE;
    return rc;
}

int libxl_device_vfb_destroy(libxl_ctx *ctx, uint32_t domid,
                                  libxl_device_vfb *vfb)
{
    GC_INIT(ctx);
    libxl__device device;
    int rc;

    rc = libxl__device_from_vfb(gc, domid, vfb, &device);
    if (rc != 0) goto out;

    rc = libxl__device_destroy(gc, &device);
out:
    GC_FREE;
    return rc;
}

/******************************************************************************/

int libxl_domain_setmaxmem(libxl_ctx *ctx, uint32_t domid, uint32_t max_memkb)
{
    GC_INIT(ctx);
    char *mem, *endptr;
    uint32_t memorykb;
    char *dompath = libxl__xs_get_dompath(gc, domid);
    int rc = 1;

    mem = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/memory/target", dompath));
    if (!mem) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "cannot get memory info from %s/memory/target\n", dompath);
        goto out;
    }
    memorykb = strtoul(mem, &endptr, 10);
    if (*endptr != '\0') {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "invalid memory %s from %s/memory/target\n", mem, dompath);
        goto out;
    }

    if (max_memkb < memorykb) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "memory_static_max must be greater than or or equal to memory_dynamic_max\n");
        goto out;
    }
    rc = xc_domain_setmaxmem(ctx->xch, domid, max_memkb + LIBXL_MAXMEM_CONSTANT);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "xc_domain_setmaxmem domid=%d memkb=%d failed "
                "rc=%d\n", domid, max_memkb + LIBXL_MAXMEM_CONSTANT, rc);
        goto out;
    }

    rc = 0;
out:
    GC_FREE;
    return rc;
}

static int libxl__fill_dom0_memory_info(libxl__gc *gc, uint32_t *target_memkb)
{
    int rc;
    libxl_dominfo info;
    libxl_physinfo physinfo;
    char *target = NULL, *staticmax = NULL, *freememslack = NULL, *endptr = NULL;
    char *target_path = "/local/domain/0/memory/target";
    char *max_path = "/local/domain/0/memory/static-max";
    char *free_mem_slack_path = "/local/domain/0/memory/freemem-slack";
    xs_transaction_t t;
    libxl_ctx *ctx = libxl__gc_owner(gc);
    uint32_t free_mem_slack_kb = 0;

retry_transaction:
    t = xs_transaction_start(ctx->xsh);

    target = libxl__xs_read(gc, t, target_path);
    staticmax = libxl__xs_read(gc, t, max_path);
    freememslack = libxl__xs_read(gc, t, free_mem_slack_path);
    if (target && staticmax && freememslack) {
        rc = 0;
        goto out;
    }

    if (target) {
        *target_memkb = strtoul(target, &endptr, 10);
        if (*endptr != '\0') {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                    "invalid memory target %s from %s\n", target, target_path);
            rc = ERROR_FAIL;
            goto out;
        }
    }

    rc = libxl_domain_info(ctx, &info, 0);
    if (rc < 0)
        goto out;

    rc = libxl_get_physinfo(ctx, &physinfo);
    if (rc < 0)
        goto out;

    if (target == NULL) {
        libxl__xs_write(gc, t, target_path, "%"PRIu32,
                (uint32_t) info.current_memkb);
        *target_memkb = (uint32_t) info.current_memkb;
    }
    if (staticmax == NULL)
        libxl__xs_write(gc, t, max_path, "%"PRIu32,
                (uint32_t) info.max_memkb);

    if (freememslack == NULL) {
        free_mem_slack_kb = (uint32_t) (PAGE_TO_MEMKB(physinfo.total_pages) -
                info.current_memkb);
        /* From empirical measurements the free_mem_slack shouldn't be more
         * than 15% of the total memory present on the system. */
        if (free_mem_slack_kb > PAGE_TO_MEMKB(physinfo.total_pages) * 0.15)
            free_mem_slack_kb = PAGE_TO_MEMKB(physinfo.total_pages) * 0.15;
        libxl__xs_write(gc, t, free_mem_slack_path, "%"PRIu32, free_mem_slack_kb);
    }
    rc = 0;

out:
    if (!xs_transaction_end(ctx->xsh, t, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
        else
            rc = ERROR_FAIL;
    }


    return rc;
}

/* returns how much memory should be left free in the system */
static int libxl__get_free_memory_slack(libxl__gc *gc, uint32_t *free_mem_slack)
{
    int rc;
    char *free_mem_slack_path = "/local/domain/0/memory/freemem-slack";
    char *free_mem_slack_s, *endptr;
    uint32_t target_memkb;

retry:
    free_mem_slack_s = libxl__xs_read(gc, XBT_NULL, free_mem_slack_path);
    if (!free_mem_slack_s) {
        rc = libxl__fill_dom0_memory_info(gc, &target_memkb);
        if (rc < 0)
            return rc;
        goto retry;
    } else {
        *free_mem_slack = strtoul(free_mem_slack_s, &endptr, 10);
        if (*endptr != '\0') {
            LIBXL__LOG_ERRNO(gc->owner, LIBXL__LOG_ERROR,
                    "invalid free_mem_slack %s from %s\n",
                    free_mem_slack_s, free_mem_slack_path);
            return ERROR_FAIL;
        }
    }
    return 0;
}

int libxl_set_memory_target(libxl_ctx *ctx, uint32_t domid,
        int32_t target_memkb, int relative, int enforce)
{
    GC_INIT(ctx);
    int rc = 1, abort = 0;
    uint32_t memorykb = 0, videoram = 0;
    uint32_t current_target_memkb = 0, new_target_memkb = 0;
    char *memmax, *endptr, *videoram_s = NULL, *target = NULL;
    char *dompath = libxl__xs_get_dompath(gc, domid);
    xc_domaininfo_t info;
    libxl_dominfo ptr;
    char *uuid;
    xs_transaction_t t;

retry_transaction:
    t = xs_transaction_start(ctx->xsh);

    target = libxl__xs_read(gc, t, libxl__sprintf(gc,
                "%s/memory/target", dompath));
    if (!target && !domid) {
        xs_transaction_end(ctx->xsh, t, 1);
        rc = libxl__fill_dom0_memory_info(gc, &current_target_memkb);
        if (rc < 0) {
            abort = 1;
            goto out;
        }
        goto retry_transaction;
    } else if (!target) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "cannot get target memory info from %s/memory/target\n",
                dompath);
        abort = 1;
        goto out;
    } else {
        current_target_memkb = strtoul(target, &endptr, 10);
        if (*endptr != '\0') {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                    "invalid memory target %s from %s/memory/target\n",
                    target, dompath);
            abort = 1;
            goto out;
        }
    }
    memmax = libxl__xs_read(gc, t, libxl__sprintf(gc,
                "%s/memory/static-max", dompath));
    if (!memmax) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "cannot get memory info from %s/memory/static-max\n",
                dompath);
        abort = 1;
        goto out;
    }
    memorykb = strtoul(memmax, &endptr, 10);
    if (*endptr != '\0') {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "invalid max memory %s from %s/memory/static-max\n",
                memmax, dompath);
        abort = 1;
        goto out;
    }

    if (relative) {
        if (target_memkb < 0 && abs(target_memkb) > current_target_memkb)
            new_target_memkb = 0;
        else
            new_target_memkb = current_target_memkb + target_memkb;
    } else
        new_target_memkb = target_memkb;
    if (new_target_memkb > memorykb) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                "memory_dynamic_max must be less than or equal to"
                " memory_static_max\n");
        abort = 1;
        goto out;
    }

    if (!domid && new_target_memkb < LIBXL_MIN_DOM0_MEM) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                "new target %d for dom0 is below the minimum threshold\n",
                 new_target_memkb);
        abort = 1;
        goto out;
    }
    videoram_s = libxl__xs_read(gc, t, libxl__sprintf(gc,
                "%s/memory/videoram", dompath));
    videoram = videoram_s ? atoi(videoram_s) : 0;

    if (enforce) {
        memorykb = new_target_memkb;
        rc = xc_domain_setmaxmem(ctx->xch, domid, memorykb +
                LIBXL_MAXMEM_CONSTANT);
        if (rc != 0) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                    "xc_domain_setmaxmem domid=%d memkb=%d failed "
                    "rc=%d\n", domid, memorykb + LIBXL_MAXMEM_CONSTANT, rc);
            abort = 1;
            goto out;
        }
    }

    new_target_memkb -= videoram;
    rc = xc_domain_set_pod_target(ctx->xch, domid,
            new_target_memkb / 4, NULL, NULL, NULL);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "xc_domain_set_pod_target domid=%d, memkb=%d "
                "failed rc=%d\n", domid, new_target_memkb / 4,
                rc);
        abort = 1;
        goto out;
    }

    libxl__xs_write(gc, t, libxl__sprintf(gc, "%s/memory/target",
                dompath), "%"PRIu32, new_target_memkb);
    rc = xc_domain_getinfolist(ctx->xch, domid, 1, &info);
    if (rc != 1 || info.domain != domid) {
        abort = 1;
        goto out;
    }
    xcinfo2xlinfo(&info, &ptr);
    uuid = libxl__uuid2string(gc, ptr.uuid);
    libxl__xs_write(gc, t, libxl__sprintf(gc, "/vm/%s/memory", uuid),
            "%"PRIu32, new_target_memkb / 1024);

out:
    if (!xs_transaction_end(ctx->xsh, t, abort) && !abort)
        if (errno == EAGAIN)
            goto retry_transaction;

    GC_FREE;
    return rc;
}

int libxl_get_memory_target(libxl_ctx *ctx, uint32_t domid, uint32_t *out_target)
{
    GC_INIT(ctx);
    int rc = 1;
    char *target = NULL, *endptr = NULL;
    char *dompath = libxl__xs_get_dompath(gc, domid);
    uint32_t target_memkb;

    target = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc,
                "%s/memory/target", dompath));
    if (!target && !domid) {
        rc = libxl__fill_dom0_memory_info(gc, &target_memkb);
        if (rc < 0)
            goto out;
    } else if (!target) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                "cannot get target memory info from %s/memory/target\n",
                dompath);
        goto out;
    } else {
        target_memkb = strtoul(target, &endptr, 10);
        if (*endptr != '\0') {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                    "invalid memory target %s from %s/memory/target\n",
                    target, dompath);
            goto out;
        }
    }
    *out_target = target_memkb;
    rc = 0;

out:
    GC_FREE;
    return rc;
}

int libxl_domain_need_memory(libxl_ctx *ctx, libxl_domain_build_info *b_info,
        libxl_device_model_info *dm_info, uint32_t *need_memkb)
{
    GC_INIT(ctx);
    int rc = ERROR_INVAL;
    *need_memkb = b_info->target_memkb;
    switch (b_info->type) {
    case LIBXL_DOMAIN_TYPE_HVM:
        *need_memkb += b_info->shadow_memkb + LIBXL_HVM_EXTRA_MEMORY;
        if (dm_info->device_model_stubdomain)
            *need_memkb += 32 * 1024;
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        *need_memkb += b_info->shadow_memkb + LIBXL_PV_EXTRA_MEMORY;
        break;
    default:
        goto out;
    }
    if (*need_memkb % (2 * 1024))
        *need_memkb += (2 * 1024) - (*need_memkb % (2 * 1024));
    rc = 0;
out:
    GC_FREE;
    return rc;

}

int libxl_get_free_memory(libxl_ctx *ctx, uint32_t *memkb)
{
    int rc = 0;
    libxl_physinfo info;
    uint32_t freemem_slack;
    GC_INIT(ctx);

    rc = libxl_get_physinfo(ctx, &info);
    if (rc < 0)
        goto out;
    rc = libxl__get_free_memory_slack(gc, &freemem_slack);
    if (rc < 0)
        goto out;

    if ((info.free_pages + info.scrub_pages) * 4 > freemem_slack)
        *memkb = (info.free_pages + info.scrub_pages) * 4 - freemem_slack;
    else
        *memkb = 0;

out:
    GC_FREE;
    return rc;
}

int libxl_wait_for_free_memory(libxl_ctx *ctx, uint32_t domid, uint32_t
        memory_kb, int wait_secs)
{
    int rc = 0;
    libxl_physinfo info;
    uint32_t freemem_slack;
    GC_INIT(ctx);

    rc = libxl__get_free_memory_slack(gc, &freemem_slack);
    if (rc < 0)
        goto out;
    while (wait_secs > 0) {
        rc = libxl_get_physinfo(ctx, &info);
        if (rc < 0)
            goto out;
        if (info.free_pages * 4 - freemem_slack >= memory_kb) {
            rc = 0;
            goto out;
        }
        wait_secs--;
        sleep(1);
    }
    rc = ERROR_NOMEM;

out:
    GC_FREE;
    return rc;
}

int libxl_wait_for_memory_target(libxl_ctx *ctx, uint32_t domid, int wait_secs)
{
    int rc = 0;
    uint32_t target_memkb = 0;
    libxl_dominfo info;

    do {
        wait_secs--;
        sleep(1);

        rc = libxl_get_memory_target(ctx, domid, &target_memkb);
        if (rc < 0)
            goto out;

        rc = libxl_domain_info(ctx, &info, domid);
        if (rc < 0)
            return rc;
    } while (wait_secs > 0 && info.current_memkb > target_memkb);

    if (info.current_memkb <= target_memkb)
        rc = 0;
    else
        rc = ERROR_FAIL;

out:
    return 0;
}

int libxl_button_press(libxl_ctx *ctx, uint32_t domid, libxl_button button)
{
    int rc = -1;

    switch (button) {
    case LIBXL_BUTTON_POWER:
        rc = xc_domain_send_trigger(ctx->xch, domid, XEN_DOMCTL_SENDTRIGGER_POWER, 0);
        break;
    case LIBXL_BUTTON_SLEEP:
        rc = xc_domain_send_trigger(ctx->xch, domid, XEN_DOMCTL_SENDTRIGGER_SLEEP, 0);
        break;
    default:
        break;
    }

    return rc;
}

int libxl_get_physinfo(libxl_ctx *ctx, libxl_physinfo *physinfo)
{
    xc_physinfo_t xcphysinfo = { 0 };
    int rc;

    rc = xc_physinfo(ctx->xch, &xcphysinfo);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting physinfo");
        return ERROR_FAIL;
    }
    physinfo->threads_per_core = xcphysinfo.threads_per_core;
    physinfo->cores_per_socket = xcphysinfo.cores_per_socket;
    physinfo->max_cpu_id = xcphysinfo.max_cpu_id;
    physinfo->nr_cpus = xcphysinfo.nr_cpus;
    physinfo->cpu_khz = xcphysinfo.cpu_khz;
    physinfo->total_pages = xcphysinfo.total_pages;
    physinfo->free_pages = xcphysinfo.free_pages;
    physinfo->scrub_pages = xcphysinfo.scrub_pages;
    physinfo->nr_nodes = xcphysinfo.nr_nodes;
    memcpy(physinfo->hw_cap,xcphysinfo.hw_cap, sizeof(physinfo->hw_cap));
    physinfo->phys_cap = xcphysinfo.capabilities;

    return 0;
}

int libxl_get_topologyinfo(libxl_ctx *ctx, libxl_topologyinfo *info)
{
    xc_topologyinfo_t tinfo;
    DECLARE_HYPERCALL_BUFFER(xc_cpu_to_core_t, coremap);
    DECLARE_HYPERCALL_BUFFER(xc_cpu_to_socket_t, socketmap);
    DECLARE_HYPERCALL_BUFFER(xc_cpu_to_node_t, nodemap);
    int i;
    int rc = 0;

    rc += libxl_cpuarray_alloc(ctx, &info->coremap);
    rc += libxl_cpuarray_alloc(ctx, &info->socketmap);
    rc += libxl_cpuarray_alloc(ctx, &info->nodemap);
    if (rc)
        goto fail;

    coremap = xc_hypercall_buffer_alloc(ctx->xch, coremap, sizeof(*coremap) * info->coremap.entries);
    socketmap = xc_hypercall_buffer_alloc(ctx->xch, socketmap, sizeof(*socketmap) * info->socketmap.entries);
    nodemap = xc_hypercall_buffer_alloc(ctx->xch, nodemap, sizeof(*nodemap) * info->nodemap.entries);
    if ((coremap == NULL) || (socketmap == NULL) || (nodemap == NULL))
        goto fail;

    set_xen_guest_handle(tinfo.cpu_to_core, coremap);
    set_xen_guest_handle(tinfo.cpu_to_socket, socketmap);
    set_xen_guest_handle(tinfo.cpu_to_node, nodemap);
    tinfo.max_cpu_index = info->coremap.entries - 1;
    if (xc_topologyinfo(ctx->xch, &tinfo) != 0)
        goto fail;

    for (i = 0; i <= tinfo.max_cpu_index; i++) {
        if (i < info->coremap.entries)
            info->coremap.array[i] = (coremap[i] == INVALID_TOPOLOGY_ID) ?
                LIBXL_CPUARRAY_INVALID_ENTRY : coremap[i];
        if (i < info->socketmap.entries)
            info->socketmap.array[i] = (socketmap[i] == INVALID_TOPOLOGY_ID) ?
                LIBXL_CPUARRAY_INVALID_ENTRY : socketmap[i];
        if (i < info->nodemap.entries)
            info->nodemap.array[i] = (nodemap[i] == INVALID_TOPOLOGY_ID) ?
                LIBXL_CPUARRAY_INVALID_ENTRY : nodemap[i];
    }

    xc_hypercall_buffer_free(ctx->xch, coremap);
    xc_hypercall_buffer_free(ctx->xch, socketmap);
    xc_hypercall_buffer_free(ctx->xch, nodemap);
    return 0;

fail:
    xc_hypercall_buffer_free(ctx->xch, coremap);
    xc_hypercall_buffer_free(ctx->xch, socketmap);
    xc_hypercall_buffer_free(ctx->xch, nodemap);
    libxl_topologyinfo_dispose(info);
    return ERROR_FAIL;
}

const libxl_version_info* libxl_get_version_info(libxl_ctx *ctx)
{
    union {
        xen_extraversion_t xen_extra;
        xen_compile_info_t xen_cc;
        xen_changeset_info_t xen_chgset;
        xen_capabilities_info_t xen_caps;
        xen_platform_parameters_t p_parms;
        xen_commandline_t xen_commandline;
    } u;
    long xen_version;
    libxl_version_info *info = &ctx->version_info;

    if (info->xen_version_extra != NULL)
        return info;

    xen_version = xc_version(ctx->xch, XENVER_version, NULL);
    info->xen_version_major = xen_version >> 16;
    info->xen_version_minor = xen_version & 0xFF;

    xc_version(ctx->xch, XENVER_extraversion, &u.xen_extra);
    info->xen_version_extra = strdup(u.xen_extra);

    xc_version(ctx->xch, XENVER_compile_info, &u.xen_cc);
    info->compiler = strdup(u.xen_cc.compiler);
    info->compile_by = strdup(u.xen_cc.compile_by);
    info->compile_domain = strdup(u.xen_cc.compile_domain);
    info->compile_date = strdup(u.xen_cc.compile_date);

    xc_version(ctx->xch, XENVER_capabilities, &u.xen_caps);
    info->capabilities = strdup(u.xen_caps);

    xc_version(ctx->xch, XENVER_changeset, &u.xen_chgset);
    info->changeset = strdup(u.xen_chgset);

    xc_version(ctx->xch, XENVER_platform_parameters, &u.p_parms);
    info->virt_start = u.p_parms.virt_start;

    info->pagesize = xc_version(ctx->xch, XENVER_pagesize, NULL);

    xc_version(ctx->xch, XENVER_commandline, &u.xen_commandline);
    info->commandline = strdup(u.xen_commandline);

    return info;
}

libxl_vcpuinfo *libxl_list_vcpu(libxl_ctx *ctx, uint32_t domid,
                                       int *nb_vcpu, int *nrcpus)
{
    libxl_vcpuinfo *ptr, *ret;
    xc_domaininfo_t domaininfo;
    xc_vcpuinfo_t vcpuinfo;

    if (xc_domain_getinfolist(ctx->xch, domid, 1, &domaininfo) != 1) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting infolist");
        return NULL;
    }
    *nrcpus = libxl_get_max_cpus(ctx);
    ret = ptr = calloc(domaininfo.max_vcpu_id + 1, sizeof (libxl_vcpuinfo));
    if (!ptr) {
        return NULL;
    }

    for (*nb_vcpu = 0; *nb_vcpu <= domaininfo.max_vcpu_id; ++*nb_vcpu, ++ptr) {
        if (libxl_cpumap_alloc(ctx, &ptr->cpumap)) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "allocating cpumap");
            return NULL;
        }
        if (xc_vcpu_getinfo(ctx->xch, domid, *nb_vcpu, &vcpuinfo) == -1) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting vcpu info");
            return NULL;
        }
        if (xc_vcpu_getaffinity(ctx->xch, domid, *nb_vcpu, ptr->cpumap.map) == -1) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting vcpu affinity");
            return NULL;
        }
        ptr->vcpuid = *nb_vcpu;
        ptr->cpu = vcpuinfo.cpu;
        ptr->online = !!vcpuinfo.online;
        ptr->blocked = !!vcpuinfo.blocked;
        ptr->running = !!vcpuinfo.running;
        ptr->vcpu_time = vcpuinfo.cpu_time;
    }
    return ret;
}

int libxl_set_vcpuaffinity(libxl_ctx *ctx, uint32_t domid, uint32_t vcpuid,
                           libxl_cpumap *cpumap)
{
    if (xc_vcpu_setaffinity(ctx->xch, domid, vcpuid, cpumap->map)) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "setting vcpu affinity");
        return ERROR_FAIL;
    }
    return 0;
}

int libxl_set_vcpuonline(libxl_ctx *ctx, uint32_t domid, libxl_cpumap *cpumap)
{
    GC_INIT(ctx);
    libxl_dominfo info;
    char *dompath;
    xs_transaction_t t;
    int i, rc = ERROR_FAIL;

    if (libxl_domain_info(ctx, &info, domid) < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain info list");
        goto out;
    }
    if (!(dompath = libxl__xs_get_dompath(gc, domid)))
        goto out;

retry_transaction:
    t = xs_transaction_start(ctx->xsh);
    for (i = 0; i <= info.vcpu_max_id; i++)
        libxl__xs_write(gc, t,
                       libxl__sprintf(gc, "%s/cpu/%u/availability", dompath, i),
                       "%s", libxl_cpumap_test(cpumap, i) ? "online" : "offline");
    if (!xs_transaction_end(ctx->xsh, t, 0)) {
        if (errno == EAGAIN)
            goto retry_transaction;
    } else
        rc = 0;
out:
    GC_FREE;
    return rc;
}

/*
 * returns one of the XEN_SCHEDULER_* constants from public/domctl.h
 */
int libxl_get_sched_id(libxl_ctx *ctx)
{
    int sched, ret;

    if ((ret = xc_sched_id(ctx->xch, &sched)) != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain info list");
        return ERROR_FAIL;
    }
    return sched;
}

int libxl_sched_credit_domain_get(libxl_ctx *ctx, uint32_t domid, libxl_sched_credit *scinfo)
{
    struct xen_domctl_sched_credit sdom;
    int rc;

    rc = xc_sched_credit_domain_get(ctx->xch, domid, &sdom);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain sched credit");
        return ERROR_FAIL;
    }

    scinfo->weight = sdom.weight;
    scinfo->cap = sdom.cap;

    return 0;
}

int libxl_sched_credit_domain_set(libxl_ctx *ctx, uint32_t domid, libxl_sched_credit *scinfo)
{
    struct xen_domctl_sched_credit sdom;
    xc_domaininfo_t domaininfo;
    int rc;

    rc = xc_domain_getinfolist(ctx->xch, domid, 1, &domaininfo);
    if (rc < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain info list");
        return ERROR_FAIL;
    }
    if (rc != 1 || domaininfo.domain != domid)
        return ERROR_INVAL;


    if (scinfo->weight < 1 || scinfo->weight > 65535) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Cpu weight out of range, valid values are within range from 1 to 65535");
        return ERROR_INVAL;
    }

    if (scinfo->cap < 0 || scinfo->cap > (domaininfo.max_vcpu_id + 1) * 100) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Cpu cap out of range, valid range is from 0 to %d for specified number of vcpus",
            ((domaininfo.max_vcpu_id + 1) * 100));
        return ERROR_INVAL;
    }

    sdom.weight = scinfo->weight;
    sdom.cap = scinfo->cap;

    rc = xc_sched_credit_domain_set(ctx->xch, domid, &sdom);
    if ( rc < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "setting domain sched credit");
        return ERROR_FAIL;
    }

    return 0;
}

int libxl_sched_credit2_domain_get(libxl_ctx *ctx, uint32_t domid,
                                   libxl_sched_credit2 *scinfo)
{
    struct xen_domctl_sched_credit2 sdom;
    int rc;

    rc = xc_sched_credit2_domain_get(ctx->xch, domid, &sdom);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "getting domain sched credit2");
        return ERROR_FAIL;
    }

    scinfo->weight = sdom.weight;

    return 0;
}

int libxl_sched_credit2_domain_set(libxl_ctx *ctx, uint32_t domid,
                                   libxl_sched_credit2 *scinfo)
{
    struct xen_domctl_sched_credit2 sdom;
    xc_domaininfo_t domaininfo;
    int rc;

    rc = xc_domain_getinfolist(ctx->xch, domid, 1, &domaininfo);
    if (rc < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain info list");
        return ERROR_FAIL;
    }
    if (rc != 1 || domaininfo.domain != domid)
        return ERROR_INVAL;


    if (scinfo->weight < 1 || scinfo->weight > 65535) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Cpu weight out of range, valid values are within range from "
            "1 to 65535");
        return ERROR_INVAL;
    }

    sdom.weight = scinfo->weight;

    rc = xc_sched_credit2_domain_set(ctx->xch, domid, &sdom);
    if ( rc < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "setting domain sched credit2");
        return ERROR_FAIL;
    }

    return 0;
}

int libxl_sched_sedf_domain_get(libxl_ctx *ctx, uint32_t domid,
                                libxl_sched_sedf *scinfo)
{
    uint64_t period;
    uint64_t slice;
    uint64_t latency;
    uint16_t extratime;
    uint16_t weight;
    int rc;

    rc = xc_sedf_domain_get(ctx->xch, domid, &period, &slice, &latency,
                            &extratime, &weight);
    if (rc != 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain sched sedf");
        return ERROR_FAIL;
    }

    scinfo->period = period / 1000000;
    scinfo->slice = slice / 1000000;
    scinfo->latency = latency / 1000000;
    scinfo->extratime = extratime;
    scinfo->weight = weight;

    return 0;
}

int libxl_sched_sedf_domain_set(libxl_ctx *ctx, uint32_t domid,
                                libxl_sched_sedf *scinfo)
{
    xc_domaininfo_t domaininfo;
    int rc;

    rc = xc_domain_getinfolist(ctx->xch, domid, 1, &domaininfo);
    if (rc < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "getting domain info list");
        return ERROR_FAIL;
    }
    if (rc != 1 || domaininfo.domain != domid)
        return ERROR_INVAL;


    rc = xc_sedf_domain_set(ctx->xch, domid, scinfo->period * 1000000,
                            scinfo->slice * 1000000, scinfo->latency * 1000000,
                            scinfo->extratime, scinfo->weight);
    if ( rc < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "setting domain sched sedf");
        return ERROR_FAIL;
    }

    return 0;
}

static int trigger_type_from_string(char *trigger_name)
{
    if (!strcmp(trigger_name, "nmi"))
        return XEN_DOMCTL_SENDTRIGGER_NMI;
    else if (!strcmp(trigger_name, "reset"))
        return XEN_DOMCTL_SENDTRIGGER_RESET;
    else if (!strcmp(trigger_name, "init"))
        return XEN_DOMCTL_SENDTRIGGER_INIT;
    else if (!strcmp(trigger_name, "power"))
        return XEN_DOMCTL_SENDTRIGGER_POWER;
    else if (!strcmp(trigger_name, "sleep"))
        return XEN_DOMCTL_SENDTRIGGER_SLEEP;
    else
        return -1;
}

int libxl_send_trigger(libxl_ctx *ctx, uint32_t domid, char *trigger_name, uint32_t vcpuid)
{
    int rc = -1;
    int trigger_type = -1;

    if (!strcmp(trigger_name, "s3resume")) {
        xc_set_hvm_param(ctx->xch, domid, HVM_PARAM_ACPI_S_STATE, 0);
        return 0;
    }

    trigger_type = trigger_type_from_string(trigger_name);
    if (trigger_type == -1) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, -1,
            "Invalid trigger, valid triggers are <nmi|reset|init|power|sleep>");
        return ERROR_INVAL;
    }

    rc = xc_domain_send_trigger(ctx->xch, domid, trigger_type, vcpuid);
    if (rc != 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Send trigger '%s' failed", trigger_name);
        return ERROR_FAIL;
    }

    return 0;
}

int libxl_send_sysrq(libxl_ctx *ctx, uint32_t domid, char sysrq)
{
    GC_INIT(ctx);
    char *dompath = libxl__xs_get_dompath(gc, domid);

    libxl__xs_write(gc, XBT_NULL, libxl__sprintf(gc, "%s/control/sysrq", dompath), "%c", sysrq);

    GC_FREE;
    return 0;
}

int libxl_send_debug_keys(libxl_ctx *ctx, char *keys)
{
    int ret;
    ret = xc_send_debug_keys(ctx->xch, keys);
    if ( ret < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "sending debug keys");
        return ERROR_FAIL;
    }
    return 0;
}

libxl_xen_console_reader *
    libxl_xen_console_read_start(libxl_ctx *ctx, int clear)
{
    libxl_xen_console_reader *cr;
    unsigned int size = 16384;
    char *buf = malloc(size);

    if (!buf) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "cannot malloc buffer for libxl_xen_console_reader,"
            " size is %u", size);
        return NULL;
    }

    cr = malloc(sizeof(libxl_xen_console_reader));
    if (!cr) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "cannot malloc libxl_xen_console_reader");
        return NULL;
    }

    memset(cr, 0, sizeof(libxl_xen_console_reader));
    cr->buffer = buf;
    cr->size = size;
    cr->count = size;
    cr->clear = clear;
    cr->incremental = 1;

    return cr;
}

/* return values:                                          *line_r
 *   1          success, whole line obtained from buffer    non-0
 *   0          no more lines available right now           0
 *   negative   error code ERROR_*                          0
 * On success *line_r is updated to point to a nul-terminated
 * string which is valid until the next call on the same console
 * reader.  The libxl caller may overwrite parts of the string
 * if it wishes. */
int libxl_xen_console_read_line(libxl_ctx *ctx,
                                libxl_xen_console_reader *cr,
                                char **line_r)
{
    int ret;

    memset(cr->buffer, 0, cr->size);
    ret = xc_readconsolering(ctx->xch, cr->buffer, &cr->count,
                             cr->clear, cr->incremental, &cr->index);
    if (ret < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "reading console ring buffer");
        return ERROR_FAIL;
    }
    if (!ret) {
        if (cr->count) {
            *line_r = cr->buffer;
            ret = 1;
        } else {
            *line_r = NULL;
            ret = 0;
        }
    }

    return ret;
}

void libxl_xen_console_read_finish(libxl_ctx *ctx,
                                   libxl_xen_console_reader *cr)
{
    free(cr->buffer);
    free(cr);
}

uint32_t libxl_vm_get_start_time(libxl_ctx *ctx, uint32_t domid)
{
    GC_INIT(ctx);
    char *dompath = libxl__xs_get_dompath(gc, domid);
    char *vm_path, *start_time;
    uint32_t ret;

    vm_path = libxl__xs_read(
        gc, XBT_NULL, libxl__sprintf(gc, "%s/vm", dompath));
    start_time = libxl__xs_read(
        gc, XBT_NULL, libxl__sprintf(gc, "%s/start_time", vm_path));
    if (start_time == NULL) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, -1,
                        "Can't get start time of domain '%d'", domid);
        ret = -1;
    }else{
        ret = strtoul(start_time, NULL, 10);
    }
    GC_FREE;
    return ret;
}

char *libxl_tmem_list(libxl_ctx *ctx, uint32_t domid, int use_long)
{
    int rc;
    char _buf[32768];

    rc = xc_tmem_control(ctx->xch, -1, TMEMC_LIST, domid, 32768, use_long,
                         0, _buf);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not get tmem list");
        return NULL;
    }

    return strdup(_buf);
}

int libxl_tmem_freeze(libxl_ctx *ctx, uint32_t domid)
{
    int rc;

    rc = xc_tmem_control(ctx->xch, -1, TMEMC_FREEZE, domid, 0, 0,
                         0, NULL);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not freeze tmem pools");
        return ERROR_FAIL;
    }

    return rc;
}

int libxl_tmem_destroy(libxl_ctx *ctx, uint32_t domid)
{
    int rc;

    rc = xc_tmem_control(ctx->xch, -1, TMEMC_DESTROY, domid, 0, 0,
                         0, NULL);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not destroy tmem pools");
        return ERROR_FAIL;
    }

    return rc;
}

int libxl_tmem_thaw(libxl_ctx *ctx, uint32_t domid)
{
    int rc;

    rc = xc_tmem_control(ctx->xch, -1, TMEMC_THAW, domid, 0, 0,
                         0, NULL);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not thaw tmem pools");
        return ERROR_FAIL;
    }

    return rc;
}

static int32_t tmem_setop_from_string(char *set_name)
{
    if (!strcmp(set_name, "weight"))
        return TMEMC_SET_WEIGHT;
    else if (!strcmp(set_name, "cap"))
        return TMEMC_SET_CAP;
    else if (!strcmp(set_name, "compress"))
        return TMEMC_SET_COMPRESS;
    else
        return -1;
}

int libxl_tmem_set(libxl_ctx *ctx, uint32_t domid, char* name, uint32_t set)
{
    int rc;
    int32_t subop = tmem_setop_from_string(name);

    if (subop == -1) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, -1,
            "Invalid set, valid sets are <weight|cap|compress>");
        return ERROR_INVAL;
    }
    rc = xc_tmem_control(ctx->xch, -1, subop, domid, set, 0, 0, NULL);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not set tmem %s", name);
        return ERROR_FAIL;
    }

    return rc;
}

int libxl_tmem_shared_auth(libxl_ctx *ctx, uint32_t domid,
                           char* uuid, int auth)
{
    int rc;

    rc = xc_tmem_auth(ctx->xch, domid, uuid, auth);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not set tmem shared auth");
        return ERROR_FAIL;
    }

    return rc;
}

int libxl_tmem_freeable(libxl_ctx *ctx)
{
    int rc;

    rc = xc_tmem_control(ctx->xch, -1, TMEMC_QUERY_FREEABLE_MB, -1, 0, 0, 0, 0);
    if (rc < 0) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Can not get tmem freeable memory");
        return ERROR_FAIL;
    }

    return rc;
}

void libxl_file_reference_dispose(libxl_file_reference *f)
{
    libxl__file_reference_unmap(f);
    free(f->path);
}

int libxl_get_freecpus(libxl_ctx *ctx, libxl_cpumap *cpumap)
{
    int ncpus;

    ncpus = libxl_get_max_cpus(ctx);
    if (ncpus == 0)
        return ERROR_FAIL;

    cpumap->map = xc_cpupool_freeinfo(ctx->xch);
    if (cpumap->map == NULL)
        return ERROR_FAIL;

    cpumap->size = (ncpus + 7) / 8;

    return 0;
}

int libxl_create_cpupool(libxl_ctx *ctx, const char *name, int schedid,
                         libxl_cpumap cpumap, libxl_uuid *uuid,
                         uint32_t *poolid)
{
    GC_INIT(ctx);
    int rc;
    int i;
    xs_transaction_t t;
    char *uuid_string;

    uuid_string = libxl__uuid2string(gc, *uuid);
    if (!uuid_string) {
        GC_FREE;
        return ERROR_NOMEM;
    }

    rc = xc_cpupool_create(ctx->xch, poolid, schedid);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
           "Could not create cpupool");
        GC_FREE;
        return ERROR_FAIL;
    }

    libxl_for_each_cpu(i, cpumap)
        if (libxl_cpumap_test(&cpumap, i)) {
            rc = xc_cpupool_addcpu(ctx->xch, *poolid, i);
            if (rc) {
                LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
                    "Error moving cpu to cpupool");
                libxl_cpupool_destroy(ctx, *poolid);
                GC_FREE;
                return ERROR_FAIL;
            }
        }

    for (;;) {
        t = xs_transaction_start(ctx->xsh);

        xs_mkdir(ctx->xsh, t, libxl__sprintf(gc, "/local/pool/%d", *poolid));
        libxl__xs_write(gc, t,
                        libxl__sprintf(gc, "/local/pool/%d/uuid", *poolid),
                        "%s", uuid_string);
        libxl__xs_write(gc, t,
                        libxl__sprintf(gc, "/local/pool/%d/name", *poolid),
                        "%s", name);

        if (xs_transaction_end(ctx->xsh, t, 0) || (errno != EAGAIN)) {
            GC_FREE;
            return 0;
        }
    }
}

int libxl_cpupool_destroy(libxl_ctx *ctx, uint32_t poolid)
{
    GC_INIT(ctx);
    int rc, i;
    xc_cpupoolinfo_t *info;
    xs_transaction_t t;
    libxl_cpumap cpumap;

    info = xc_cpupool_getinfo(ctx->xch, poolid);
    if (info == NULL) {
        GC_FREE;
        return ERROR_NOMEM;
    }

    rc = ERROR_INVAL;
    if ((info->cpupool_id != poolid) || (info->n_dom))
        goto out;

    rc = ERROR_NOMEM;
    if (libxl_cpumap_alloc(ctx, &cpumap))
        goto out;

    memcpy(cpumap.map, info->cpumap, cpumap.size);
    libxl_for_each_cpu(i, cpumap)
        if (libxl_cpumap_test(&cpumap, i)) {
            rc = xc_cpupool_removecpu(ctx->xch, poolid, i);
            if (rc) {
                LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
                    "Error removing cpu from cpupool");
                rc = ERROR_FAIL;
                goto out1;
            }
        }

    rc = xc_cpupool_destroy(ctx->xch, poolid);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc, "Could not destroy cpupool");
        rc = ERROR_FAIL;
        goto out1;
    }

    for (;;) {
        t = xs_transaction_start(ctx->xsh);

        xs_rm(ctx->xsh, XBT_NULL, libxl__sprintf(gc, "/local/pool/%d", poolid));

        if (xs_transaction_end(ctx->xsh, t, 0) || (errno != EAGAIN))
            break;
    }

    rc = 0;

out1:
    libxl_cpumap_dispose(&cpumap);
out:
    xc_cpupool_infofree(ctx->xch, info);
    GC_FREE;

    return rc;
}

int libxl_cpupool_rename(libxl_ctx *ctx, const char *name, uint32_t poolid)
{
    GC_INIT(ctx);
    xs_transaction_t t;
    xc_cpupoolinfo_t *info;
    int rc;

    info = xc_cpupool_getinfo(ctx->xch, poolid);
    if (info == NULL) {
        GC_FREE;
        return ERROR_NOMEM;
    }

    rc = ERROR_INVAL;
    if (info->cpupool_id != poolid)
        goto out;

    rc = 0;

    for (;;) {
        t = xs_transaction_start(ctx->xsh);

        libxl__xs_write(gc, t,
                        libxl__sprintf(gc, "/local/pool/%d/name", poolid),
                        "%s", name);

        if (xs_transaction_end(ctx->xsh, t, 0))
            break;

        if (errno == EAGAIN)
            continue;

        rc = ERROR_FAIL;
        break;
    }

out:
    xc_cpupool_infofree(ctx->xch, info);
    GC_FREE;

    return rc;
}

int libxl_cpupool_cpuadd(libxl_ctx *ctx, uint32_t poolid, int cpu)
{
    int rc;

    rc = xc_cpupool_addcpu(ctx->xch, poolid, cpu);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Error moving cpu to cpupool");
        return ERROR_FAIL;
    }
    return 0;
}

int libxl_cpupool_cpuadd_node(libxl_ctx *ctx, uint32_t poolid, int node, int *cpus)
{
    int rc = 0;
    int cpu;
    libxl_cpumap freemap;
    libxl_topologyinfo topology;

    if (libxl_get_freecpus(ctx, &freemap)) {
        return ERROR_FAIL;
    }

    if (libxl_get_topologyinfo(ctx, &topology)) {
        rc = ERROR_FAIL;
        goto out;
    }

    *cpus = 0;
    for (cpu = 0; cpu < topology.nodemap.entries; cpu++) {
        if (libxl_cpumap_test(&freemap, cpu) &&
            (topology.nodemap.array[cpu] == node) &&
            !libxl_cpupool_cpuadd(ctx, poolid, cpu)) {
                (*cpus)++;
        }
    }

    libxl_topologyinfo_dispose(&topology);

out:
    libxl_cpumap_dispose(&freemap);
    return rc;
}

int libxl_cpupool_cpuremove(libxl_ctx *ctx, uint32_t poolid, int cpu)
{
    int rc;

    rc = xc_cpupool_removecpu(ctx->xch, poolid, cpu);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Error removing cpu from cpupool");
        return ERROR_FAIL;
    }
    return 0;
}

int libxl_cpupool_cpuremove_node(libxl_ctx *ctx, uint32_t poolid, int node, int *cpus)
{
    int ret = 0;
    int n_pools;
    int p;
    int cpu;
    libxl_topologyinfo topology;
    libxl_cpupoolinfo *poolinfo;

    poolinfo = libxl_list_cpupool(ctx, &n_pools);
    if (!poolinfo) {
        return ERROR_NOMEM;
    }

    if (libxl_get_topologyinfo(ctx, &topology)) {
        ret = ERROR_FAIL;
        goto out;
    }

    *cpus = 0;
    for (p = 0; p < n_pools; p++) {
        if (poolinfo[p].poolid == poolid) {
            for (cpu = 0; cpu < topology.nodemap.entries; cpu++) {
                if ((topology.nodemap.array[cpu] == node) &&
                    libxl_cpumap_test(&poolinfo[p].cpumap, cpu) &&
                    !libxl_cpupool_cpuremove(ctx, poolid, cpu)) {
                        (*cpus)++;
                }
            }
        }
    }

    libxl_topologyinfo_dispose(&topology);

out:
    for (p = 0; p < n_pools; p++) {
        libxl_cpupoolinfo_dispose(poolinfo + p);
    }

    return ret;
}

int libxl_cpupool_movedomain(libxl_ctx *ctx, uint32_t poolid, uint32_t domid)
{
    GC_INIT(ctx);
    int rc;
    char *dom_path;
    char *vm_path;
    char *poolname;
    xs_transaction_t t;

    dom_path = libxl__xs_get_dompath(gc, domid);
    if (!dom_path) {
        GC_FREE;
        return ERROR_FAIL;
    }

    rc = xc_cpupool_movedomain(ctx->xch, poolid, domid);
    if (rc) {
        LIBXL__LOG_ERRNOVAL(ctx, LIBXL__LOG_ERROR, rc,
            "Error moving domain to cpupool");
        GC_FREE;
        return ERROR_FAIL;
    }

    for (;;) {
        t = xs_transaction_start(ctx->xsh);

        poolname = libxl__cpupoolid_to_name(gc, poolid);
        vm_path = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/vm", dom_path));
        if (!vm_path)
            break;

        libxl__xs_write(gc, t, libxl__sprintf(gc, "%s/pool_name", vm_path),
                        "%s", poolname);

        if (xs_transaction_end(ctx->xsh, t, 0) || (errno != EAGAIN))
            break;
    }

    GC_FREE;
    return 0;
}

int libxl_fd_set_cloexec(int fd)
{
    int flags = 0;

    if ((flags = fcntl(fd, F_GETFD)) == -1) {
        flags = 0;
    }
    if ((flags & FD_CLOEXEC)) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
