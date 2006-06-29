/******************************************************************************
 * drivers/xen/netback/netback.c
 * 
 * Back-end of the driver for virtual network devices. This portion of the
 * driver exports a 'unified' network-device interface that can be accessed
 * by any operating system that implements a compatible front end. A 
 * reference front-end implementation can be found in:
 *  drivers/xen/netfront/netfront.c
 * 
 * Copyright (c) 2002-2005, K A Fraser
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

#include "common.h"
#include <xen/balloon.h>
#include <xen/interface/memory.h>

/*#define NETBE_DEBUG_INTERRUPT*/

static void netif_idx_release(u16 pending_idx);
static void netif_page_release(struct page *page);
static void make_tx_response(netif_t *netif, 
			     netif_tx_request_t *txp,
			     s8       st);
static int  make_rx_response(netif_t *netif, 
			     u16      id, 
			     s8       st,
			     u16      offset,
			     u16      size,
			     u16      flags);

static void net_tx_action(unsigned long unused);
static DECLARE_TASKLET(net_tx_tasklet, net_tx_action, 0);

static void net_rx_action(unsigned long unused);
static DECLARE_TASKLET(net_rx_tasklet, net_rx_action, 0);

static struct timer_list net_timer;

#define MAX_PENDING_REQS 256

static struct sk_buff_head rx_queue;
static multicall_entry_t rx_mcl[NET_RX_RING_SIZE+1];
static mmu_update_t rx_mmu[NET_RX_RING_SIZE];
static gnttab_transfer_t grant_rx_op[NET_RX_RING_SIZE];
static unsigned char rx_notify[NR_IRQS];

static unsigned long mmap_vstart;
#define MMAP_VADDR(_req) (mmap_vstart + ((_req) * PAGE_SIZE))

#define PKT_PROT_LEN 64

static struct {
	netif_tx_request_t req;
	netif_t *netif;
} pending_tx_info[MAX_PENDING_REQS];
static u16 pending_ring[MAX_PENDING_REQS];
typedef unsigned int PEND_RING_IDX;
#define MASK_PEND_IDX(_i) ((_i)&(MAX_PENDING_REQS-1))
static PEND_RING_IDX pending_prod, pending_cons;
#define NR_PENDING_REQS (MAX_PENDING_REQS - pending_prod + pending_cons)

/* Freed TX SKBs get batched on this ring before return to pending_ring. */
static u16 dealloc_ring[MAX_PENDING_REQS];
static PEND_RING_IDX dealloc_prod, dealloc_cons;

static struct sk_buff_head tx_queue;

static grant_handle_t grant_tx_handle[MAX_PENDING_REQS];
static gnttab_unmap_grant_ref_t tx_unmap_ops[MAX_PENDING_REQS];
static gnttab_map_grant_ref_t tx_map_ops[MAX_PENDING_REQS];

static struct list_head net_schedule_list;
static spinlock_t net_schedule_list_lock;

#define MAX_MFN_ALLOC 64
static unsigned long mfn_list[MAX_MFN_ALLOC];
static unsigned int alloc_index = 0;
static DEFINE_SPINLOCK(mfn_lock);

static unsigned long alloc_mfn(void)
{
	unsigned long mfn = 0, flags;
	struct xen_memory_reservation reservation = {
		.nr_extents   = MAX_MFN_ALLOC,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};
	set_xen_guest_handle(reservation.extent_start, mfn_list);
	spin_lock_irqsave(&mfn_lock, flags);
	if ( unlikely(alloc_index == 0) )
		alloc_index = HYPERVISOR_memory_op(
			XENMEM_increase_reservation, &reservation);
	if ( alloc_index != 0 )
		mfn = mfn_list[--alloc_index];
	spin_unlock_irqrestore(&mfn_lock, flags);
	return mfn;
}

static inline void maybe_schedule_tx_action(void)
{
	smp_mb();
	if ((NR_PENDING_REQS < (MAX_PENDING_REQS/2)) &&
	    !list_empty(&net_schedule_list))
		tasklet_schedule(&net_tx_tasklet);
}

/*
 * A gross way of confirming the origin of an skb data page. The slab
 * allocator abuses a field in the page struct to cache the kmem_cache_t ptr.
 */
static inline int is_xen_skb(struct sk_buff *skb)
{
	extern kmem_cache_t *skbuff_cachep;
	kmem_cache_t *cp = (kmem_cache_t *)virt_to_page(skb->head)->lru.next;
	return (cp == skbuff_cachep);
}

int netif_be_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);

	BUG_ON(skb->dev != dev);

	/* Drop the packet if the target domain has no receive buffers. */
	if (!netif->active || 
	    (netif->rx_req_cons_peek == netif->rx.sring->req_prod) ||
	    ((netif->rx_req_cons_peek - netif->rx.rsp_prod_pvt) ==
	     NET_RX_RING_SIZE))
		goto drop;

	/*
	 * We do not copy the packet unless:
	 *  1. The data is shared; or
	 *  2. The data is not allocated from our special cache.
	 * NB. We also couldn't cope with fragmented packets, but we won't get
	 *     any because we not advertise the NETIF_F_SG feature.
	 */
	if (skb_shared(skb) || skb_cloned(skb) || !is_xen_skb(skb)) {
		int hlen = skb->data - skb->head;
		int ret;
		struct sk_buff *nskb = dev_alloc_skb(hlen + skb->len);
		if ( unlikely(nskb == NULL) )
			goto drop;
		skb_reserve(nskb, hlen);
		__skb_put(nskb, skb->len);
		ret = skb_copy_bits(skb, -hlen, nskb->data - hlen,
				     skb->len + hlen);
		BUG_ON(ret);
		/* Copy only the header fields we use in this driver. */
		nskb->dev = skb->dev;
		nskb->ip_summed = skb->ip_summed;
		nskb->proto_data_valid = skb->proto_data_valid;
		dev_kfree_skb(skb);
		skb = nskb;
	}

	netif->rx_req_cons_peek++;
	netif_get(netif);

	skb_queue_tail(&rx_queue, skb);
	tasklet_schedule(&net_rx_tasklet);

	return 0;

 drop:
	netif->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return 0;
}

#if 0
static void xen_network_done_notify(void)
{
	static struct net_device *eth0_dev = NULL;
	if (unlikely(eth0_dev == NULL))
		eth0_dev = __dev_get_by_name("eth0");
	netif_rx_schedule(eth0_dev);
}
/* 
 * Add following to poll() function in NAPI driver (Tigon3 is example):
 *  if ( xen_network_done() )
 *      tg3_enable_ints(tp); 
 */
int xen_network_done(void)
{
	return skb_queue_empty(&rx_queue);
}
#endif

static void net_rx_action(unsigned long unused)
{
	netif_t *netif = NULL; 
	s8 status;
	u16 size, id, irq, flags;
	multicall_entry_t *mcl;
	mmu_update_t *mmu;
	gnttab_transfer_t *gop;
	unsigned long vdata, old_mfn, new_mfn;
	struct sk_buff_head rxq;
	struct sk_buff *skb;
	u16 notify_list[NET_RX_RING_SIZE];
	int notify_nr = 0;
	int ret;

	skb_queue_head_init(&rxq);

	mcl = rx_mcl;
	mmu = rx_mmu;
	gop = grant_rx_op;

	while ((skb = skb_dequeue(&rx_queue)) != NULL) {
		netif   = netdev_priv(skb->dev);
		vdata   = (unsigned long)skb->data;
		old_mfn = virt_to_mfn(vdata);

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			/* Memory squeeze? Back off for an arbitrary while. */
			if ((new_mfn = alloc_mfn()) == 0) {
				if ( net_ratelimit() )
					WPRINTK("Memory squeeze in netback "
						"driver.\n");
				mod_timer(&net_timer, jiffies + HZ);
				skb_queue_head(&rx_queue, skb);
				break;
			}
			/*
			 * Set the new P2M table entry before reassigning
			 * the old data page. Heed the comment in
			 * pgtable-2level.h:pte_page(). :-)
			 */
			set_phys_to_machine(
				__pa(skb->data) >> PAGE_SHIFT,
				new_mfn);

			MULTI_update_va_mapping(mcl, vdata,
						pfn_pte_ma(new_mfn,
							   PAGE_KERNEL), 0);
			mcl++;

			mmu->ptr = ((maddr_t)new_mfn << PAGE_SHIFT) |
				MMU_MACHPHYS_UPDATE;
			mmu->val = __pa(vdata) >> PAGE_SHIFT;
			mmu++;
		}

		gop->mfn = old_mfn;
		gop->domid = netif->domid;
		gop->ref = RING_GET_REQUEST(
			&netif->rx, netif->rx.req_cons)->gref;
		netif->rx.req_cons++;
		gop++;

		__skb_queue_tail(&rxq, skb);

		/* Filled the batch queue? */
		if ((gop - grant_rx_op) == ARRAY_SIZE(grant_rx_op))
			break;
	}

	if (!xen_feature(XENFEAT_auto_translated_physmap)) {
		if (mcl == rx_mcl)
			return;

		mcl[-1].args[MULTI_UVMFLAGS_INDEX] = UVMF_TLB_FLUSH|UVMF_ALL;

		if (mmu - rx_mmu) {
			mcl->op = __HYPERVISOR_mmu_update;
			mcl->args[0] = (unsigned long)rx_mmu;
			mcl->args[1] = mmu - rx_mmu;
			mcl->args[2] = 0;
			mcl->args[3] = DOMID_SELF;
			mcl++;
		}

		ret = HYPERVISOR_multicall(rx_mcl, mcl - rx_mcl);
		BUG_ON(ret != 0);
	}

	ret = HYPERVISOR_grant_table_op(GNTTABOP_transfer, grant_rx_op, 
					gop - grant_rx_op);
	BUG_ON(ret != 0);

	mcl = rx_mcl;
	gop = grant_rx_op;
	while ((skb = __skb_dequeue(&rxq)) != NULL) {
		netif   = netdev_priv(skb->dev);
		size    = skb->tail - skb->data;

		atomic_set(&(skb_shinfo(skb)->dataref), 1);
		skb_shinfo(skb)->nr_frags = 0;
		skb_shinfo(skb)->frag_list = NULL;

		netif->stats.tx_bytes += size;
		netif->stats.tx_packets++;

		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			/* The update_va_mapping() must not fail. */
			BUG_ON(mcl->result != 0);
			mcl++;
		}

		/* Check the reassignment error code. */
		status = NETIF_RSP_OKAY;
		if (gop->status != 0) { 
			DPRINTK("Bad status %d from grant transfer to DOM%u\n",
				gop->status, netif->domid);
			/*
			 * Page no longer belongs to us unless GNTST_bad_page,
			 * but that should be a fatal error anyway.
			 */
			BUG_ON(gop->status == GNTST_bad_page);
			status = NETIF_RSP_ERROR; 
		}
		irq = netif->irq;
		id = RING_GET_REQUEST(&netif->rx, netif->rx.rsp_prod_pvt)->id;
		flags = 0;
		if (skb->ip_summed == CHECKSUM_HW) /* local packet? */
			flags |= NETRXF_csum_blank | NETRXF_data_validated;
		else if (skb->proto_data_valid) /* remote but checksummed? */
			flags |= NETRXF_data_validated;
		if (make_rx_response(netif, id, status,
				     (unsigned long)skb->data & ~PAGE_MASK,
				     size, flags) &&
		    (rx_notify[irq] == 0)) {
			rx_notify[irq] = 1;
			notify_list[notify_nr++] = irq;
		}

		netif_put(netif);
		dev_kfree_skb(skb);
		gop++;
	}

	while (notify_nr != 0) {
		irq = notify_list[--notify_nr];
		rx_notify[irq] = 0;
		notify_remote_via_irq(irq);
	}

	/* More work to do? */
	if (!skb_queue_empty(&rx_queue) && !timer_pending(&net_timer))
		tasklet_schedule(&net_rx_tasklet);
#if 0
	else
		xen_network_done_notify();
#endif
}

static void net_alarm(unsigned long unused)
{
	tasklet_schedule(&net_rx_tasklet);
}

struct net_device_stats *netif_be_get_stats(struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);
	return &netif->stats;
}

static int __on_net_schedule_list(netif_t *netif)
{
	return netif->list.next != NULL;
}

static void remove_from_net_schedule_list(netif_t *netif)
{
	spin_lock_irq(&net_schedule_list_lock);
	if (likely(__on_net_schedule_list(netif))) {
		list_del(&netif->list);
		netif->list.next = NULL;
		netif_put(netif);
	}
	spin_unlock_irq(&net_schedule_list_lock);
}

static void add_to_net_schedule_list_tail(netif_t *netif)
{
	if (__on_net_schedule_list(netif))
		return;

	spin_lock_irq(&net_schedule_list_lock);
	if (!__on_net_schedule_list(netif) && netif->active) {
		list_add_tail(&netif->list, &net_schedule_list);
		netif_get(netif);
	}
	spin_unlock_irq(&net_schedule_list_lock);
}

/*
 * Note on CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER:
 * If this driver is pipelining transmit requests then we can be very
 * aggressive in avoiding new-packet notifications -- frontend only needs to
 * send a notification if there are no outstanding unreceived responses.
 * If we may be buffer transmit buffers for any reason then we must be rather
 * more conservative and treat this as the final check for pending work.
 */
void netif_schedule_work(netif_t *netif)
{
	int more_to_do;

#ifdef CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER
	more_to_do = RING_HAS_UNCONSUMED_REQUESTS(&netif->tx);
#else
	RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, more_to_do);
#endif

	if (more_to_do) {
		add_to_net_schedule_list_tail(netif);
		maybe_schedule_tx_action();
	}
}

void netif_deschedule_work(netif_t *netif)
{
	remove_from_net_schedule_list(netif);
}


static void tx_credit_callback(unsigned long data)
{
	netif_t *netif = (netif_t *)data;
	netif->remaining_credit = netif->credit_bytes;
	netif_schedule_work(netif);
}

inline static void net_tx_action_dealloc(void)
{
	gnttab_unmap_grant_ref_t *gop;
	u16 pending_idx;
	PEND_RING_IDX dc, dp;
	netif_t *netif;
	int ret;

	dc = dealloc_cons;
	dp = dealloc_prod;

	/* Ensure we see all indexes enqueued by netif_idx_release(). */
	smp_rmb();

	/*
	 * Free up any grants we have finished using
	 */
	gop = tx_unmap_ops;
	while (dc != dp) {
		pending_idx = dealloc_ring[MASK_PEND_IDX(dc++)];
		gnttab_set_unmap_op(gop, MMAP_VADDR(pending_idx),
				    GNTMAP_host_map,
				    grant_tx_handle[pending_idx]);
		gop++;
	}
	ret = HYPERVISOR_grant_table_op(
		GNTTABOP_unmap_grant_ref, tx_unmap_ops, gop - tx_unmap_ops);
	BUG_ON(ret);

	while (dealloc_cons != dp) {
		pending_idx = dealloc_ring[MASK_PEND_IDX(dealloc_cons++)];

		netif = pending_tx_info[pending_idx].netif;

		make_tx_response(netif, &pending_tx_info[pending_idx].req, 
				 NETIF_RSP_OKAY);

		pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;

		netif_put(netif);
	}
}

static void netbk_tx_err(netif_t *netif, netif_tx_request_t *txp, RING_IDX end)
{
	RING_IDX cons = netif->tx.req_cons;

	do {
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		if (++cons >= end)
			break;
		txp = RING_GET_REQUEST(&netif->tx, cons);
	} while (1);
	netif->tx.req_cons = cons;
	netif_schedule_work(netif);
	netif_put(netif);
}

static int netbk_count_requests(netif_t *netif, netif_tx_request_t *txp,
				int work_to_do)
{
	netif_tx_request_t *first = txp;
	RING_IDX cons = netif->tx.req_cons;
	int frags = 0;

	while (txp->flags & NETTXF_more_data) {
		if (frags >= work_to_do) {
			DPRINTK("Need more frags\n");
			return -frags;
		}

		txp = RING_GET_REQUEST(&netif->tx, cons + frags);
		if (txp->size > first->size) {
			DPRINTK("Frags galore\n");
			return -frags;
		}

		first->size -= txp->size;
		frags++;

		if (unlikely((txp->offset + txp->size) > PAGE_SIZE)) {
			DPRINTK("txp->offset: %x, size: %u\n",
				txp->offset, txp->size);
			return -frags;
		}
	}

	return frags;
}

static gnttab_map_grant_ref_t *netbk_get_requests(netif_t *netif,
						  struct sk_buff *skb,
						  gnttab_map_grant_ref_t *mop)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	skb_frag_t *frags = shinfo->frags;
	netif_tx_request_t *txp;
	unsigned long pending_idx = *((u16 *)skb->data);
	RING_IDX cons = netif->tx.req_cons;
	int i, start;

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = ((unsigned long)shinfo->frags[0].page == pending_idx);

	for (i = start; i < shinfo->nr_frags; i++) {
		txp = RING_GET_REQUEST(&netif->tx, cons++);
		pending_idx = pending_ring[MASK_PEND_IDX(pending_cons++)];

		gnttab_set_map_op(mop++, MMAP_VADDR(pending_idx),
				  GNTMAP_host_map | GNTMAP_readonly,
				  txp->gref, netif->domid);

		memcpy(&pending_tx_info[pending_idx].req, txp, sizeof(*txp));
		netif_get(netif);
		pending_tx_info[pending_idx].netif = netif;
		frags[i].page = (void *)pending_idx;
	}

	return mop;
}

static int netbk_tx_check_mop(struct sk_buff *skb,
			       gnttab_map_grant_ref_t **mopp)
{
	gnttab_map_grant_ref_t *mop = *mopp;
	int pending_idx = *((u16 *)skb->data);
	netif_t *netif = pending_tx_info[pending_idx].netif;
	netif_tx_request_t *txp;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i, err, start;

	/* Check status of header. */
	err = mop->status;
	if (unlikely(err)) {
		txp = &pending_tx_info[pending_idx].req;
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;
		netif_put(netif);
	} else {
		set_phys_to_machine(
			__pa(MMAP_VADDR(pending_idx)) >> PAGE_SHIFT,
			FOREIGN_FRAME(mop->dev_bus_addr >> PAGE_SHIFT));
		grant_tx_handle[pending_idx] = mop->handle;
	}

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = ((unsigned long)shinfo->frags[0].page == pending_idx);

	for (i = start; i < nr_frags; i++) {
		int j, newerr;

		pending_idx = (unsigned long)shinfo->frags[i].page;

		/* Check error status: if okay then remember grant handle. */
		newerr = (++mop)->status;
		if (likely(!newerr)) {
			set_phys_to_machine(
				__pa(MMAP_VADDR(pending_idx))>>PAGE_SHIFT,
				FOREIGN_FRAME(mop->dev_bus_addr>>PAGE_SHIFT));
			grant_tx_handle[pending_idx] = mop->handle;
			/* Had a previous error? Invalidate this fragment. */
			if (unlikely(err))
				netif_idx_release(pending_idx);
			continue;
		}

		/* Error on this fragment: respond to client with an error. */
		txp = &pending_tx_info[pending_idx].req;
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;
		netif_put(netif);

		/* Not the first error? Preceding frags already invalidated. */
		if (err)
			continue;

		/* First error: invalidate header and preceding fragments. */
		pending_idx = *((u16 *)skb->data);
		netif_idx_release(pending_idx);
		for (j = start; j < i; j++) {
			pending_idx = (unsigned long)shinfo->frags[i].page;
			netif_idx_release(pending_idx);
		}

		/* Remember the error: invalidate all subsequent fragments. */
		err = newerr;
	}

	*mopp = mop + 1;
	return err;
}

static void netbk_fill_frags(struct sk_buff *skb)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i;

	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = shinfo->frags + i;
		netif_tx_request_t *txp;
		unsigned long pending_idx;

		pending_idx = (unsigned long)frag->page;
		txp = &pending_tx_info[pending_idx].req;
		frag->page = virt_to_page(MMAP_VADDR(pending_idx));
		frag->size = txp->size;
		frag->page_offset = txp->offset;

		skb->len += txp->size;
		skb->data_len += txp->size;
		skb->truesize += txp->size;
	}
}

int netbk_get_extras(netif_t *netif, struct netif_extra_info *extras,
		     int work_to_do)
{
	struct netif_extra_info *extra;
	RING_IDX cons = netif->tx.req_cons;

	do {
		if (unlikely(work_to_do-- <= 0)) {
			DPRINTK("Missing extra info\n");
			return -EBADR;
		}

		extra = (struct netif_extra_info *)
			RING_GET_REQUEST(&netif->tx, cons);
		if (unlikely(!extra->type ||
			     extra->type >= XEN_NETIF_EXTRA_TYPE_MAX)) {
			netif->tx.req_cons = ++cons;
			DPRINTK("Invalid extra type: %d\n", extra->type);
			return -EINVAL;
		}

		memcpy(&extras[extra->type - 1], extra, sizeof(*extra));
		netif->tx.req_cons = ++cons;
	} while (extra->flags & XEN_NETIF_EXTRA_FLAG_MORE);

	return work_to_do;
}

/* Called after netfront has transmitted */
static void net_tx_action(unsigned long unused)
{
	struct list_head *ent;
	struct sk_buff *skb;
	netif_t *netif;
	netif_tx_request_t txreq;
	struct netif_extra_info extras[XEN_NETIF_EXTRA_TYPE_MAX - 1];
	u16 pending_idx;
	RING_IDX i;
	gnttab_map_grant_ref_t *mop;
	unsigned int data_len;
	int ret, work_to_do;

	if (dealloc_cons != dealloc_prod)
		net_tx_action_dealloc();

	mop = tx_map_ops;
	while (((NR_PENDING_REQS + MAX_SKB_FRAGS) < MAX_PENDING_REQS) &&
		!list_empty(&net_schedule_list)) {
		/* Get a netif from the list with work to do. */
		ent = net_schedule_list.next;
		netif = list_entry(ent, netif_t, list);
		netif_get(netif);
		remove_from_net_schedule_list(netif);

		RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, work_to_do);
		if (!work_to_do) {
			netif_put(netif);
			continue;
		}

		i = netif->tx.req_cons;
		rmb(); /* Ensure that we see the request before we copy it. */
		memcpy(&txreq, RING_GET_REQUEST(&netif->tx, i), sizeof(txreq));
		/* Credit-based scheduling. */
		if (txreq.size > netif->remaining_credit) {
			unsigned long now = jiffies;
			unsigned long next_credit = 
				netif->credit_timeout.expires +
				msecs_to_jiffies(netif->credit_usec / 1000);

			/* Timer could already be pending in rare cases. */
			if (timer_pending(&netif->credit_timeout))
				break;

			/* Passed the point where we can replenish credit? */
			if (time_after_eq(now, next_credit)) {
				netif->credit_timeout.expires = now;
				netif->remaining_credit = netif->credit_bytes;
			}

			/* Still too big to send right now? Set a callback. */
			if (txreq.size > netif->remaining_credit) {
				netif->remaining_credit = 0;
				netif->credit_timeout.data     =
					(unsigned long)netif;
				netif->credit_timeout.function =
					tx_credit_callback;
				__mod_timer(&netif->credit_timeout,
					    next_credit);
				break;
			}
		}
		netif->remaining_credit -= txreq.size;

		work_to_do--;
		netif->tx.req_cons = ++i;

		memset(extras, 0, sizeof(extras));
		if (txreq.flags & NETTXF_extra_info) {
			work_to_do = netbk_get_extras(netif, extras,
						      work_to_do);
			if (unlikely(work_to_do < 0)) {
				netbk_tx_err(netif, &txreq, 0);
				continue;
			}
			i = netif->tx.req_cons;
		}

		ret = netbk_count_requests(netif, &txreq, work_to_do);
		if (unlikely(ret < 0)) {
			netbk_tx_err(netif, &txreq, i - ret);
			continue;
		}
		i += ret;

		if (unlikely(ret > MAX_SKB_FRAGS)) {
			DPRINTK("Too many frags\n");
			netbk_tx_err(netif, &txreq, i);
			continue;
		}

		if (unlikely(txreq.size < ETH_HLEN)) {
			DPRINTK("Bad packet size: %d\n", txreq.size);
			netbk_tx_err(netif, &txreq, i);
			continue; 
		}

		/* No crossing a page as the payload mustn't fragment. */
		if (unlikely((txreq.offset + txreq.size) > PAGE_SIZE)) {
			DPRINTK("txreq.offset: %x, size: %u, end: %lu\n", 
				txreq.offset, txreq.size, 
				(txreq.offset &~PAGE_MASK) + txreq.size);
			netbk_tx_err(netif, &txreq, i);
			continue;
		}

		pending_idx = pending_ring[MASK_PEND_IDX(pending_cons)];

		data_len = (txreq.size > PKT_PROT_LEN &&
			    ret < MAX_SKB_FRAGS) ?
			PKT_PROT_LEN : txreq.size;

		skb = alloc_skb(data_len+16, GFP_ATOMIC);
		if (unlikely(skb == NULL)) {
			DPRINTK("Can't allocate a skb in start_xmit.\n");
			netbk_tx_err(netif, &txreq, i);
			break;
		}

		/* Packets passed to netif_rx() must have some headroom. */
		skb_reserve(skb, 16);

		if (extras[XEN_NETIF_EXTRA_TYPE_GSO - 1].type) {
			struct netif_extra_info *gso;
			gso = &extras[XEN_NETIF_EXTRA_TYPE_GSO - 1];

			if (gso->u.gso.type != XEN_NETIF_GSO_TCPV4) {
				DPRINTK("Bad GSO type.\n");
				kfree_skb(skb);
				netbk_tx_err(netif, &txreq, i);
				break;
			}

			skb_shinfo(skb)->gso_size = gso->u.gso.size;
			skb_shinfo(skb)->gso_segs = gso->u.gso.segs;
			skb_shinfo(skb)->gso_type =
				SKB_GSO_TCPV4 | SKB_GSO_DODGY;
		}

		gnttab_set_map_op(mop, MMAP_VADDR(pending_idx),
				  GNTMAP_host_map | GNTMAP_readonly,
				  txreq.gref, netif->domid);
		mop++;

		memcpy(&pending_tx_info[pending_idx].req,
		       &txreq, sizeof(txreq));
		pending_tx_info[pending_idx].netif = netif;
		*((u16 *)skb->data) = pending_idx;

		__skb_put(skb, data_len);

		skb_shinfo(skb)->nr_frags = ret;
		if (data_len < txreq.size) {
			skb_shinfo(skb)->nr_frags++;
			skb_shinfo(skb)->frags[0].page =
				(void *)(unsigned long)pending_idx;
		}

		__skb_queue_tail(&tx_queue, skb);

		pending_cons++;

		mop = netbk_get_requests(netif, skb, mop);

		netif->tx.req_cons = i;
		netif_schedule_work(netif);

		if ((mop - tx_map_ops) >= ARRAY_SIZE(tx_map_ops))
			break;
	}

	if (mop == tx_map_ops)
		return;

	ret = HYPERVISOR_grant_table_op(
		GNTTABOP_map_grant_ref, tx_map_ops, mop - tx_map_ops);
	BUG_ON(ret);

	mop = tx_map_ops;
	while ((skb = __skb_dequeue(&tx_queue)) != NULL) {
		netif_tx_request_t *txp;

		pending_idx = *((u16 *)skb->data);
		netif       = pending_tx_info[pending_idx].netif;
		txp         = &pending_tx_info[pending_idx].req;

		/* Check the remap error code. */
		if (unlikely(netbk_tx_check_mop(skb, &mop))) {
			printk(KERN_ALERT "#### netback grant fails\n");
			skb_shinfo(skb)->nr_frags = 0;
			kfree_skb(skb);
			continue;
		}

		data_len = skb->len;
		memcpy(skb->data, 
		       (void *)(MMAP_VADDR(pending_idx)|txp->offset),
		       data_len);
		if (data_len < txp->size) {
			/* Append the packet payload as a fragment. */
			txp->offset += data_len;
			txp->size -= data_len;
		} else {
			/* Schedule a response immediately. */
			netif_idx_release(pending_idx);
		}

		/*
		 * Old frontends do not assert data_validated but we
		 * can infer it from csum_blank so test both flags.
		 */
		if (txp->flags & (NETTXF_data_validated|NETTXF_csum_blank)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			skb->proto_data_valid = 1;
		} else {
			skb->ip_summed = CHECKSUM_NONE;
			skb->proto_data_valid = 0;
		}
		skb->proto_csum_blank = !!(txp->flags & NETTXF_csum_blank);

		netbk_fill_frags(skb);

		skb->dev      = netif->dev;
		skb->protocol = eth_type_trans(skb, skb->dev);

		netif->stats.rx_bytes += skb->len;
		netif->stats.rx_packets++;

		netif_rx(skb);
		netif->dev->last_rx = jiffies;
	}
}

static void netif_idx_release(u16 pending_idx)
{
	static DEFINE_SPINLOCK(_lock);
	unsigned long flags;

	spin_lock_irqsave(&_lock, flags);
	dealloc_ring[MASK_PEND_IDX(dealloc_prod)] = pending_idx;
	/* Sync with net_tx_action_dealloc: insert idx /then/ incr producer. */
	smp_wmb();
	dealloc_prod++;
	spin_unlock_irqrestore(&_lock, flags);

	tasklet_schedule(&net_tx_tasklet);
}

static void netif_page_release(struct page *page)
{
	u16 pending_idx = page - virt_to_page(mmap_vstart);

	/* Ready for next use. */
	set_page_count(page, 1);

	netif_idx_release(pending_idx);
}

irqreturn_t netif_be_int(int irq, void *dev_id, struct pt_regs *regs)
{
	netif_t *netif = dev_id;
	add_to_net_schedule_list_tail(netif);
	maybe_schedule_tx_action();
	return IRQ_HANDLED;
}

static void make_tx_response(netif_t *netif, 
			     netif_tx_request_t *txp,
			     s8       st)
{
	RING_IDX i = netif->tx.rsp_prod_pvt;
	netif_tx_response_t *resp;
	int notify;

	resp = RING_GET_RESPONSE(&netif->tx, i);
	resp->id     = txp->id;
	resp->status = st;

	if (txp->flags & NETTXF_extra_info)
		RING_GET_RESPONSE(&netif->tx, ++i)->status = NETIF_RSP_NULL;

	netif->tx.rsp_prod_pvt = ++i;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&netif->tx, notify);
	if (notify)
		notify_remote_via_irq(netif->irq);

#ifdef CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER
	if (i == netif->tx.req_cons) {
		int more_to_do;
		RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, more_to_do);
		if (more_to_do)
			add_to_net_schedule_list_tail(netif);
	}
#endif
}

static int make_rx_response(netif_t *netif, 
			    u16      id, 
			    s8       st,
			    u16      offset,
			    u16      size,
			    u16      flags)
{
	RING_IDX i = netif->rx.rsp_prod_pvt;
	netif_rx_response_t *resp;
	int notify;

	resp = RING_GET_RESPONSE(&netif->rx, i);
	resp->offset     = offset;
	resp->flags      = flags;
	resp->id         = id;
	resp->status     = (s16)size;
	if (st < 0)
		resp->status = (s16)st;

	netif->rx.rsp_prod_pvt = ++i;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&netif->rx, notify);

	return notify;
}

#ifdef NETBE_DEBUG_INTERRUPT
static irqreturn_t netif_be_dbg(int irq, void *dev_id, struct pt_regs *regs)
{
	struct list_head *ent;
	netif_t *netif;
	int i = 0;

	printk(KERN_ALERT "netif_schedule_list:\n");
	spin_lock_irq(&net_schedule_list_lock);

	list_for_each (ent, &net_schedule_list) {
		netif = list_entry(ent, netif_t, list);
		printk(KERN_ALERT " %d: private(rx_req_cons=%08x "
		       "rx_resp_prod=%08x\n",
		       i, netif->rx.req_cons, netif->rx.rsp_prod_pvt);
		printk(KERN_ALERT "   tx_req_cons=%08x tx_resp_prod=%08x)\n",
		       netif->tx.req_cons, netif->tx.rsp_prod_pvt);
		printk(KERN_ALERT "   shared(rx_req_prod=%08x "
		       "rx_resp_prod=%08x\n",
		       netif->rx.sring->req_prod, netif->rx.sring->rsp_prod);
		printk(KERN_ALERT "   rx_event=%08x tx_req_prod=%08x\n",
		       netif->rx.sring->rsp_event, netif->tx.sring->req_prod);
		printk(KERN_ALERT "   tx_resp_prod=%08x, tx_event=%08x)\n",
		       netif->tx.sring->rsp_prod, netif->tx.sring->rsp_event);
		i++;
	}

	spin_unlock_irq(&net_schedule_list_lock);
	printk(KERN_ALERT " ** End of netif_schedule_list **\n");

	return IRQ_HANDLED;
}
#endif

static int __init netback_init(void)
{
	int i;
	struct page *page;

	if (!is_running_on_xen())
		return -ENODEV;

	/* We can increase reservation by this much in net_rx_action(). */
	balloon_update_driver_allowance(NET_RX_RING_SIZE);

	skb_queue_head_init(&rx_queue);
	skb_queue_head_init(&tx_queue);

	init_timer(&net_timer);
	net_timer.data = 0;
	net_timer.function = net_alarm;
    
	page = balloon_alloc_empty_page_range(MAX_PENDING_REQS);
	BUG_ON(page == NULL);
	mmap_vstart = (unsigned long)pfn_to_kaddr(page_to_pfn(page));

	for (i = 0; i < MAX_PENDING_REQS; i++) {
		page = virt_to_page(MMAP_VADDR(i));
		set_page_count(page, 1);
		SetPageForeign(page, netif_page_release);
	}

	pending_cons = 0;
	pending_prod = MAX_PENDING_REQS;
	for (i = 0; i < MAX_PENDING_REQS; i++)
		pending_ring[i] = i;

	spin_lock_init(&net_schedule_list_lock);
	INIT_LIST_HEAD(&net_schedule_list);

	netif_xenbus_init();

#ifdef NETBE_DEBUG_INTERRUPT
	(void)bind_virq_to_irqhandler(
		VIRQ_DEBUG,
		0,
		netif_be_dbg,
		SA_SHIRQ, 
		"net-be-dbg",
		&netif_be_dbg);
#endif

	return 0;
}

module_init(netback_init);

MODULE_LICENSE("Dual BSD/GPL");
