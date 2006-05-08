 /*****************************************************************************
 * drivers/xen/tpmback/interface.c
 *
 * Vritual TPM interface management.
 *
 * Copyright (c) 2005, IBM Corporation
 *
 * Author: Stefan Berger, stefanb@us.ibm.com
 *
 * This code has been derived from drivers/xen/netback/interface.c
 * Copyright (c) 2004, Keir Fraser
 */

#include "common.h"
#include <xen/balloon.h>
#include <xen/gnttab.h>

static kmem_cache_t *tpmif_cachep;
int num_frontends = 0;

LIST_HEAD(tpmif_list);

static tpmif_t *alloc_tpmif(domid_t domid, long int instance)
{
	tpmif_t *tpmif;

	tpmif = kmem_cache_alloc(tpmif_cachep, GFP_KERNEL);
	if (!tpmif)
		return ERR_PTR(-ENOMEM);

	memset(tpmif, 0, sizeof (*tpmif));
	tpmif->domid = domid;
	tpmif->status = DISCONNECTED;
	tpmif->tpm_instance = instance;
	snprintf(tpmif->devname, sizeof(tpmif->devname), "tpmif%d", domid);
	atomic_set(&tpmif->refcnt, 1);

	tpmif->pagerange = balloon_alloc_empty_page_range(TPMIF_TX_RING_SIZE);
	BUG_ON(tpmif->pagerange == NULL);
	tpmif->mmap_vstart = (unsigned long)pfn_to_kaddr(
	                                    page_to_pfn(tpmif->pagerange));

	list_add(&tpmif->tpmif_list, &tpmif_list);
	num_frontends++;

	return tpmif;
}

static void free_tpmif(tpmif_t * tpmif)
{
	num_frontends--;
	list_del(&tpmif->tpmif_list);
	balloon_dealloc_empty_page_range(tpmif->pagerange, TPMIF_TX_RING_SIZE);
	kmem_cache_free(tpmif_cachep, tpmif);
}

tpmif_t *tpmif_find(domid_t domid, long int instance)
{
	tpmif_t *tpmif;

	list_for_each_entry(tpmif, &tpmif_list, tpmif_list) {
		if (tpmif->tpm_instance == instance) {
			if (tpmif->domid == domid) {
				tpmif_get(tpmif);
				return tpmif;
			} else {
				return ERR_PTR(-EEXIST);
			}
		}
	}

	return alloc_tpmif(domid, instance);
}

static int map_frontend_page(tpmif_t *tpmif, unsigned long shared_page)
{
	int ret;
	struct gnttab_map_grant_ref op;

	gnttab_set_map_op(&op, (unsigned long)tpmif->tx_area->addr,
			  GNTMAP_host_map, shared_page, tpmif->domid);

	lock_vm_area(tpmif->tx_area);
	ret = HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1);
	unlock_vm_area(tpmif->tx_area);
	BUG_ON(ret);

	if (op.status) {
		DPRINTK(" Grant table operation failure !\n");
		return op.status;
	}

	tpmif->shmem_ref = shared_page;
	tpmif->shmem_handle = op.handle;

	return 0;
}

static void unmap_frontend_page(tpmif_t *tpmif)
{
	struct gnttab_unmap_grant_ref op;
	int ret;

	gnttab_set_unmap_op(&op, (unsigned long)tpmif->tx_area->addr,
			    GNTMAP_host_map, tpmif->shmem_handle);

	lock_vm_area(tpmif->tx_area);
	ret = HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1);
	unlock_vm_area(tpmif->tx_area);
	BUG_ON(ret);
}

int tpmif_map(tpmif_t *tpmif, unsigned long shared_page, unsigned int evtchn)
{
	int err;
	struct evtchn_bind_interdomain bind_interdomain;

	if (tpmif->irq) {
		return 0;
	}

	if ((tpmif->tx_area = alloc_vm_area(PAGE_SIZE)) == NULL)
		return -ENOMEM;

	err = map_frontend_page(tpmif, shared_page);
	if (err) {
		free_vm_area(tpmif->tx_area);
		return err;
	}


	bind_interdomain.remote_dom  = tpmif->domid;
	bind_interdomain.remote_port = evtchn;

	err = HYPERVISOR_event_channel_op(EVTCHNOP_bind_interdomain,
					  &bind_interdomain);
	if (err) {
		unmap_frontend_page(tpmif);
		free_vm_area(tpmif->tx_area);
		return err;
	}

	tpmif->evtchn = bind_interdomain.local_port;

	tpmif->tx = (tpmif_tx_interface_t *)tpmif->tx_area->addr;

	tpmif->irq = bind_evtchn_to_irqhandler(
		tpmif->evtchn, tpmif_be_int, 0, tpmif->devname, tpmif);
	tpmif->shmem_ref = shared_page;
	tpmif->active = 1;

	return 0;
}

static void __tpmif_disconnect_complete(void *arg)
{
	tpmif_t *tpmif = (tpmif_t *) arg;

	if (tpmif->irq)
		unbind_from_irqhandler(tpmif->irq, tpmif);

	if (tpmif->tx) {
		unmap_frontend_page(tpmif);
		free_vm_area(tpmif->tx_area);
	}

	free_tpmif(tpmif);
}

void tpmif_disconnect_complete(tpmif_t * tpmif)
{
	INIT_WORK(&tpmif->work, __tpmif_disconnect_complete, (void *)tpmif);
	schedule_work(&tpmif->work);
}

void __init tpmif_interface_init(void)
{
	tpmif_cachep = kmem_cache_create("tpmif_cache", sizeof (tpmif_t),
					 0, 0, NULL, NULL);
}

void __init tpmif_interface_exit(void)
{
	kmem_cache_destroy(tpmif_cachep);
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
