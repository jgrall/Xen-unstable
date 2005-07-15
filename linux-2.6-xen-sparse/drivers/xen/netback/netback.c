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
 */

#include "common.h"
#include <asm-xen/balloon.h>
#include <asm-xen/evtchn.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#include <linux/delay.h>
#endif

static void netif_idx_release(u16 pending_idx);
static void netif_page_release(struct page *page);
static void make_tx_response(netif_t *netif, 
                             u16      id,
                             s8       st);
static int  make_rx_response(netif_t *netif, 
                             u16      id, 
                             s8       st,
                             memory_t addr,
                             u16      size,
                             u16      csum_valid);

static void net_tx_action(unsigned long unused);
static DECLARE_TASKLET(net_tx_tasklet, net_tx_action, 0);

static void net_rx_action(unsigned long unused);
static DECLARE_TASKLET(net_rx_tasklet, net_rx_action, 0);

static struct timer_list net_timer;

static struct sk_buff_head rx_queue;
static multicall_entry_t rx_mcl[NETIF_RX_RING_SIZE*2+1];
static mmu_update_t rx_mmu[NETIF_RX_RING_SIZE];
static struct mmuext_op rx_mmuext[NETIF_RX_RING_SIZE];
static unsigned char rx_notify[NR_EVENT_CHANNELS];

/* Don't currently gate addition of an interface to the tx scheduling list. */
#define tx_work_exists(_if) (1)

#define MAX_PENDING_REQS 256
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
static multicall_entry_t tx_mcl[MAX_PENDING_REQS];

static struct list_head net_schedule_list;
static spinlock_t net_schedule_list_lock;

#define MAX_MFN_ALLOC 64
static unsigned long mfn_list[MAX_MFN_ALLOC];
static unsigned int alloc_index = 0;
static spinlock_t mfn_lock = SPIN_LOCK_UNLOCKED;

static unsigned long alloc_mfn(void)
{
    unsigned long mfn = 0, flags;
    spin_lock_irqsave(&mfn_lock, flags);
    if ( unlikely(alloc_index == 0) )
        alloc_index = HYPERVISOR_dom_mem_op(
            MEMOP_increase_reservation, mfn_list, MAX_MFN_ALLOC, 0);
    if ( alloc_index != 0 )
        mfn = mfn_list[--alloc_index];
    spin_unlock_irqrestore(&mfn_lock, flags);
    return mfn;
}

static void free_mfn(unsigned long mfn)
{
    unsigned long flags;
    spin_lock_irqsave(&mfn_lock, flags);
    if ( alloc_index != MAX_MFN_ALLOC )
        mfn_list[alloc_index++] = mfn;
    else if ( HYPERVISOR_dom_mem_op(MEMOP_decrease_reservation,
                                    &mfn, 1, 0) != 1 )
        BUG();
    spin_unlock_irqrestore(&mfn_lock, flags);
}

static inline void maybe_schedule_tx_action(void)
{
    smp_mb();
    if ( (NR_PENDING_REQS < (MAX_PENDING_REQS/2)) &&
         !list_empty(&net_schedule_list) )
        tasklet_schedule(&net_tx_tasklet);
}

/*
 * A gross way of confirming the origin of an skb data page. The slab
 * allocator abuses a field in the page struct to cache the kmem_cache_t ptr.
 */
static inline int is_xen_skb(struct sk_buff *skb)
{
    extern kmem_cache_t *skbuff_cachep;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
    kmem_cache_t *cp = (kmem_cache_t *)virt_to_page(skb->head)->lru.next;
#else
    kmem_cache_t *cp = (kmem_cache_t *)virt_to_page(skb->head)->list.next;
#endif
    return (cp == skbuff_cachep);
}

int netif_be_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    netif_t *netif = netdev_priv(dev);

    ASSERT(skb->dev == dev);

    /* Drop the packet if the target domain has no receive buffers. */
    if ( !netif->active || 
         (netif->rx_req_cons == netif->rx->req_prod) ||
         ((netif->rx_req_cons-netif->rx_resp_prod) == NETIF_RX_RING_SIZE) )
        goto drop;

    /*
     * We do not copy the packet unless:
     *  1. The data is shared; or
     *  2. The data is not allocated from our special cache.
     * NB. We also couldn't cope with fragmented packets, but we won't get
     *     any because we not advertise the NETIF_F_SG feature.
     */
    if ( skb_shared(skb) || skb_cloned(skb) || !is_xen_skb(skb) )
    {
        int hlen = skb->data - skb->head;
        struct sk_buff *nskb = dev_alloc_skb(hlen + skb->len);
        if ( unlikely(nskb == NULL) )
            goto drop;
        skb_reserve(nskb, hlen);
        __skb_put(nskb, skb->len);
        if (skb_copy_bits(skb, -hlen, nskb->data - hlen, skb->len + hlen))
            BUG();
        nskb->dev = skb->dev;
        nskb->proto_csum_valid = skb->proto_csum_valid;
        dev_kfree_skb(skb);
        skb = nskb;
    }

    netif->rx_req_cons++;
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
    if ( unlikely(eth0_dev == NULL) )
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
    netif_t *netif;
    s8 status;
    u16 size, id, evtchn;
    multicall_entry_t *mcl;
    mmu_update_t *mmu;
    struct mmuext_op *mmuext;
    unsigned long vdata, mdata, new_mfn;
    struct sk_buff_head rxq;
    struct sk_buff *skb;
    u16 notify_list[NETIF_RX_RING_SIZE];
    int notify_nr = 0;

    skb_queue_head_init(&rxq);

    mcl = rx_mcl;
    mmu = rx_mmu;
    mmuext = rx_mmuext;
    while ( (skb = skb_dequeue(&rx_queue)) != NULL )
    {
        netif   = netdev_priv(skb->dev);
        vdata   = (unsigned long)skb->data;
        mdata   = virt_to_machine(vdata);

        /* Memory squeeze? Back off for an arbitrary while. */
        if ( (new_mfn = alloc_mfn()) == 0 )
        {
            if ( net_ratelimit() )
                printk(KERN_WARNING "Memory squeeze in netback driver.\n");
            mod_timer(&net_timer, jiffies + HZ);
            skb_queue_head(&rx_queue, skb);
            break;
        }

        /*
         * Set the new P2M table entry before reassigning the old data page.
         * Heed the comment in pgtable-2level.h:pte_page(). :-)
         */
        phys_to_machine_mapping[__pa(skb->data) >> PAGE_SHIFT] = new_mfn;

        MULTI_update_va_mapping(mcl, vdata,
				pfn_pte_ma(new_mfn, PAGE_KERNEL), 0);
        mcl++;

        mcl->op = __HYPERVISOR_mmuext_op;
        mcl->args[0] = (unsigned long)mmuext;
        mcl->args[1] = 1;
        mcl->args[2] = 0;
        mcl->args[3] = netif->domid;
        mcl++;

        mmuext->cmd = MMUEXT_REASSIGN_PAGE;
        mmuext->mfn = mdata >> PAGE_SHIFT;
        mmuext++;

        mmu->ptr = (new_mfn << PAGE_SHIFT) | MMU_MACHPHYS_UPDATE;
        mmu->val = __pa(vdata) >> PAGE_SHIFT;  
        mmu++;

        __skb_queue_tail(&rxq, skb);

        /* Filled the batch queue? */
        if ( (mcl - rx_mcl) == ARRAY_SIZE(rx_mcl) )
            break;
    }

    if ( mcl == rx_mcl )
        return;

    mcl->op = __HYPERVISOR_mmu_update;
    mcl->args[0] = (unsigned long)rx_mmu;
    mcl->args[1] = mmu - rx_mmu;
    mcl->args[2] = 0;
    mcl->args[3] = DOMID_SELF;
    mcl++;

    mcl[-3].args[2] = UVMF_TLB_FLUSH|UVMF_ALL;
    if ( unlikely(HYPERVISOR_multicall(rx_mcl, mcl - rx_mcl) != 0) )
        BUG();

    mcl = rx_mcl;
    mmuext = rx_mmuext;
    while ( (skb = __skb_dequeue(&rxq)) != NULL )
    {
        netif   = netdev_priv(skb->dev);
        size    = skb->tail - skb->data;

        /* Rederive the machine addresses. */
        new_mfn = mcl[0].args[1] >> PAGE_SHIFT;
        mdata   = ((mmuext[0].mfn << PAGE_SHIFT) |
                   ((unsigned long)skb->data & ~PAGE_MASK));
        
        atomic_set(&(skb_shinfo(skb)->dataref), 1);
        skb_shinfo(skb)->nr_frags = 0;
        skb_shinfo(skb)->frag_list = NULL;

        netif->stats.tx_bytes += size;
        netif->stats.tx_packets++;

        /* The update_va_mapping() must not fail. */
        BUG_ON(mcl[0].result != 0);

        /* Check the reassignment error code. */
        status = NETIF_RSP_OKAY;
        if ( unlikely(mcl[1].result != 0) )
        {
            DPRINTK("Failed MMU update transferring to DOM%u\n", netif->domid);
            free_mfn(mdata >> PAGE_SHIFT);
            status = NETIF_RSP_ERROR;
        }

        evtchn = netif->evtchn;
        id = netif->rx->ring[MASK_NETIF_RX_IDX(netif->rx_resp_prod)].req.id;
        if ( make_rx_response(netif, id, status, mdata,
                              size, skb->proto_csum_valid) &&
             (rx_notify[evtchn] == 0) )
        {
            rx_notify[evtchn] = 1;
            notify_list[notify_nr++] = evtchn;
        }

        netif_put(netif);
        dev_kfree_skb(skb);

        mcl += 2;
        mmuext += 1;
    }

    while ( notify_nr != 0 )
    {
        evtchn = notify_list[--notify_nr];
        rx_notify[evtchn] = 0;
        notify_via_evtchn(evtchn);
    }

    /* More work to do? */
    if ( !skb_queue_empty(&rx_queue) && !timer_pending(&net_timer) )
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
    if ( likely(__on_net_schedule_list(netif)) )
    {
        list_del(&netif->list);
        netif->list.next = NULL;
        netif_put(netif);
    }
    spin_unlock_irq(&net_schedule_list_lock);
}

static void add_to_net_schedule_list_tail(netif_t *netif)
{
    if ( __on_net_schedule_list(netif) )
        return;

    spin_lock_irq(&net_schedule_list_lock);
    if ( !__on_net_schedule_list(netif) && netif->active )
    {
        list_add_tail(&netif->list, &net_schedule_list);
        netif_get(netif);
    }
    spin_unlock_irq(&net_schedule_list_lock);
}

void netif_schedule_work(netif_t *netif)
{
    if ( (netif->tx_req_cons != netif->tx->req_prod) &&
         ((netif->tx_req_cons-netif->tx_resp_prod) != NETIF_TX_RING_SIZE) )
    {
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

static void net_tx_action(unsigned long unused)
{
    struct list_head *ent;
    struct sk_buff *skb;
    netif_t *netif;
    netif_tx_request_t txreq;
    u16 pending_idx;
    NETIF_RING_IDX i;
    multicall_entry_t *mcl;
    PEND_RING_IDX dc, dp;
    unsigned int data_len;

    if ( (dc = dealloc_cons) == (dp = dealloc_prod) )
        goto skip_dealloc;

    mcl = tx_mcl;
    while ( dc != dp )
    {
        pending_idx = dealloc_ring[MASK_PEND_IDX(dc++)];
	MULTI_update_va_mapping(mcl, MMAP_VADDR(pending_idx),
				__pte(0), 0);
        mcl++;     
    }

    mcl[-1].args[2] = UVMF_TLB_FLUSH|UVMF_ALL;
    if ( unlikely(HYPERVISOR_multicall(tx_mcl, mcl - tx_mcl) != 0) )
        BUG();

    mcl = tx_mcl;
    while ( dealloc_cons != dp )
    {
        /* The update_va_mapping() must not fail. */
        BUG_ON(mcl[0].result != 0);

        pending_idx = dealloc_ring[MASK_PEND_IDX(dealloc_cons++)];

        netif = pending_tx_info[pending_idx].netif;

        make_tx_response(netif, pending_tx_info[pending_idx].req.id, 
                         NETIF_RSP_OKAY);
        
        pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;

        /*
         * Scheduling checks must happen after the above response is posted.
         * This avoids a possible race with a guest OS on another CPU if that
         * guest is testing against 'resp_prod' when deciding whether to notify
         * us when it queues additional packets.
         */
        mb();
        if ( (netif->tx_req_cons != netif->tx->req_prod) &&
             ((netif->tx_req_cons-netif->tx_resp_prod) != NETIF_TX_RING_SIZE) )
            add_to_net_schedule_list_tail(netif);
        
        netif_put(netif);

        mcl++;
    }

 skip_dealloc:
    mcl = tx_mcl;
    while ( (NR_PENDING_REQS < MAX_PENDING_REQS) &&
            !list_empty(&net_schedule_list) )
    {
        /* Get a netif from the list with work to do. */
        ent = net_schedule_list.next;
        netif = list_entry(ent, netif_t, list);
        netif_get(netif);
        remove_from_net_schedule_list(netif);

        /* Work to do? */
        i = netif->tx_req_cons;
        if ( (i == netif->tx->req_prod) ||
             ((i-netif->tx_resp_prod) == NETIF_TX_RING_SIZE) )
        {
            netif_put(netif);
            continue;
        }

        rmb(); /* Ensure that we see the request before we copy it. */
        memcpy(&txreq, &netif->tx->ring[MASK_NETIF_TX_IDX(i)].req, 
               sizeof(txreq));

        /* Credit-based scheduling. */
        if ( txreq.size > netif->remaining_credit )
        {
            unsigned long now = jiffies;
            unsigned long next_credit = 
                netif->credit_timeout.expires +
                msecs_to_jiffies(netif->credit_usec / 1000);

            /* Timer could already be pending in some rare cases. */
            if ( timer_pending(&netif->credit_timeout) )
                break;

            /* Already passed the point at which we can replenish credit? */
            if ( time_after_eq(now, next_credit) )
            {
                netif->credit_timeout.expires = now;
                netif->remaining_credit = netif->credit_bytes;
            }

            /* Still too big to send right now? Then set a timer callback. */
            if ( txreq.size > netif->remaining_credit )
            {
                netif->remaining_credit = 0;
                netif->credit_timeout.expires  = next_credit;
                netif->credit_timeout.data     = (unsigned long)netif;
                netif->credit_timeout.function = tx_credit_callback;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
                add_timer_on(&netif->credit_timeout, smp_processor_id());
#else
                add_timer(&netif->credit_timeout); 
#endif
                break;
            }
        }
        netif->remaining_credit -= txreq.size;

        /*
         * Why the barrier? It ensures that the frontend sees updated req_cons
         * before we check for more work to schedule.
         */
        netif->tx->req_cons = ++netif->tx_req_cons;
        mb();

        netif_schedule_work(netif);

        if ( unlikely(txreq.size < ETH_HLEN) || 
             unlikely(txreq.size > ETH_FRAME_LEN) )
        {
            DPRINTK("Bad packet size: %d\n", txreq.size);
            make_tx_response(netif, txreq.id, NETIF_RSP_ERROR);
            netif_put(netif);
            continue; 
        }

        /* No crossing a page boundary as the payload mustn't fragment. */
        if ( unlikely(((txreq.addr & ~PAGE_MASK) + txreq.size) >= PAGE_SIZE) ) 
        {
            DPRINTK("txreq.addr: %lx, size: %u, end: %lu\n", 
                    txreq.addr, txreq.size, 
                    (txreq.addr &~PAGE_MASK) + txreq.size);
            make_tx_response(netif, txreq.id, NETIF_RSP_ERROR);
            netif_put(netif);
            continue;
        }

        pending_idx = pending_ring[MASK_PEND_IDX(pending_cons)];

        data_len = (txreq.size > PKT_PROT_LEN) ? PKT_PROT_LEN : txreq.size;

        if ( unlikely((skb = alloc_skb(data_len+16, GFP_ATOMIC)) == NULL) )
        {
            DPRINTK("Can't allocate a skb in start_xmit.\n");
            make_tx_response(netif, txreq.id, NETIF_RSP_ERROR);
            netif_put(netif);
            break;
        }

        /* Packets passed to netif_rx() must have some headroom. */
        skb_reserve(skb, 16);

	MULTI_update_va_mapping_otherdomain(
	    mcl, MMAP_VADDR(pending_idx),
	    pfn_pte_ma(txreq.addr >> PAGE_SHIFT, PAGE_KERNEL),
	    0, netif->domid);
        mcl++;

        memcpy(&pending_tx_info[pending_idx].req, &txreq, sizeof(txreq));
        pending_tx_info[pending_idx].netif = netif;
        *((u16 *)skb->data) = pending_idx;

        __skb_queue_tail(&tx_queue, skb);

        pending_cons++;

        /* Filled the batch queue? */
        if ( (mcl - tx_mcl) == ARRAY_SIZE(tx_mcl) )
            break;
    }

    if ( mcl == tx_mcl )
        return;

    if ( unlikely(HYPERVISOR_multicall(tx_mcl, mcl - tx_mcl) != 0) )
        BUG();

    mcl = tx_mcl;
    while ( (skb = __skb_dequeue(&tx_queue)) != NULL )
    {
        pending_idx = *((u16 *)skb->data);
        netif       = pending_tx_info[pending_idx].netif;
        memcpy(&txreq, &pending_tx_info[pending_idx].req, sizeof(txreq));

        /* Check the remap error code. */
        if ( unlikely(mcl[0].result != 0) )
        {
            DPRINTK("Bad page frame\n");
            make_tx_response(netif, txreq.id, NETIF_RSP_ERROR);
            netif_put(netif);
            kfree_skb(skb);
            mcl++;
            pending_ring[MASK_PEND_IDX(pending_prod++)] = pending_idx;
            continue;
        }

        phys_to_machine_mapping[__pa(MMAP_VADDR(pending_idx)) >> PAGE_SHIFT] =
            FOREIGN_FRAME(txreq.addr >> PAGE_SHIFT);

        data_len = (txreq.size > PKT_PROT_LEN) ? PKT_PROT_LEN : txreq.size;

        __skb_put(skb, data_len);
        memcpy(skb->data, 
               (void *)(MMAP_VADDR(pending_idx)|(txreq.addr&~PAGE_MASK)),
               data_len);

        if ( data_len < txreq.size )
        {
            /* Append the packet payload as a fragment. */
            skb_shinfo(skb)->frags[0].page        = 
                virt_to_page(MMAP_VADDR(pending_idx));
            skb_shinfo(skb)->frags[0].size        = txreq.size - data_len;
            skb_shinfo(skb)->frags[0].page_offset = 
                (txreq.addr + data_len) & ~PAGE_MASK;
            skb_shinfo(skb)->nr_frags = 1;
        }
        else
        {
            /* Schedule a response immediately. */
            netif_idx_release(pending_idx);
        }

        skb->data_len  = txreq.size - data_len;
        skb->len      += skb->data_len;

        skb->dev      = netif->dev;
        skb->protocol = eth_type_trans(skb, skb->dev);

        /* No checking needed on localhost, but remember the field is blank. */
        skb->ip_summed        = CHECKSUM_UNNECESSARY;
        skb->proto_csum_valid = 1;
        skb->proto_csum_blank = txreq.csum_blank;

        netif->stats.rx_bytes += txreq.size;
        netif->stats.rx_packets++;

        netif_rx(skb);
        netif->dev->last_rx = jiffies;

        mcl++;
    }
}

static void netif_idx_release(u16 pending_idx)
{
    static spinlock_t _lock = SPIN_LOCK_UNLOCKED;
    unsigned long flags;

    spin_lock_irqsave(&_lock, flags);
    dealloc_ring[MASK_PEND_IDX(dealloc_prod++)] = pending_idx;
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
    if ( tx_work_exists(netif) )
    {
        add_to_net_schedule_list_tail(netif);
        maybe_schedule_tx_action();
    }
    return IRQ_HANDLED;
}

static void make_tx_response(netif_t *netif, 
                             u16      id,
                             s8       st)
{
    NETIF_RING_IDX i = netif->tx_resp_prod;
    netif_tx_response_t *resp;

    resp = &netif->tx->ring[MASK_NETIF_TX_IDX(i)].resp;
    resp->id     = id;
    resp->status = st;
    wmb();
    netif->tx->resp_prod = netif->tx_resp_prod = ++i;

    mb(); /* Update producer before checking event threshold. */
    if ( i == netif->tx->event )
        notify_via_evtchn(netif->evtchn);
}

static int make_rx_response(netif_t *netif, 
                            u16      id, 
                            s8       st,
                            memory_t addr,
                            u16      size,
                            u16      csum_valid)
{
    NETIF_RING_IDX i = netif->rx_resp_prod;
    netif_rx_response_t *resp;

    resp = &netif->rx->ring[MASK_NETIF_RX_IDX(i)].resp;
    resp->addr       = addr;
    resp->csum_valid = csum_valid;
    resp->id         = id;
    resp->status     = (s16)size;
    if ( st < 0 )
        resp->status = (s16)st;
    wmb();
    netif->rx->resp_prod = netif->rx_resp_prod = ++i;

    mb(); /* Update producer before checking event threshold. */
    return (i == netif->rx->event);
}

static irqreturn_t netif_be_dbg(int irq, void *dev_id, struct pt_regs *regs)
{
    struct list_head *ent;
    netif_t *netif;
    int i = 0;

    printk(KERN_ALERT "netif_schedule_list:\n");
    spin_lock_irq(&net_schedule_list_lock);

    list_for_each ( ent, &net_schedule_list )
    {
        netif = list_entry(ent, netif_t, list);
        printk(KERN_ALERT " %d: private(rx_req_cons=%08x rx_resp_prod=%08x\n",
               i, netif->rx_req_cons, netif->rx_resp_prod);               
        printk(KERN_ALERT "   tx_req_cons=%08x tx_resp_prod=%08x)\n",
               netif->tx_req_cons, netif->tx_resp_prod);
        printk(KERN_ALERT "   shared(rx_req_prod=%08x rx_resp_prod=%08x\n",
               netif->rx->req_prod, netif->rx->resp_prod);
        printk(KERN_ALERT "   rx_event=%08x tx_req_prod=%08x\n",
               netif->rx->event, netif->tx->req_prod);
        printk(KERN_ALERT "   tx_resp_prod=%08x, tx_event=%08x)\n",
               netif->tx->resp_prod, netif->tx->event);
        i++;
    }

    spin_unlock_irq(&net_schedule_list_lock);
    printk(KERN_ALERT " ** End of netif_schedule_list **\n");

    return IRQ_HANDLED;
}

static int __init netback_init(void)
{
    int i;
    struct page *page;

    if ( !(xen_start_info.flags & SIF_NET_BE_DOMAIN) &&
         !(xen_start_info.flags & SIF_INITDOMAIN) )
        return 0;

    printk("Initialising Xen netif backend\n");

    /* We can increase reservation by this much in net_rx_action(). */
    balloon_update_driver_allowance(NETIF_RX_RING_SIZE);

    skb_queue_head_init(&rx_queue);
    skb_queue_head_init(&tx_queue);

    init_timer(&net_timer);
    net_timer.data = 0;
    net_timer.function = net_alarm;
    
    netif_interface_init();

    mmap_vstart = allocate_empty_lowmem_region(MAX_PENDING_REQS);
    BUG_ON(mmap_vstart == 0);

    for ( i = 0; i < MAX_PENDING_REQS; i++ )
    {
        page = virt_to_page(MMAP_VADDR(i));
        set_page_count(page, 1);
        SetPageForeign(page, netif_page_release);
    }

    pending_cons = 0;
    pending_prod = MAX_PENDING_REQS;
    for ( i = 0; i < MAX_PENDING_REQS; i++ )
        pending_ring[i] = i;

    spin_lock_init(&net_schedule_list_lock);
    INIT_LIST_HEAD(&net_schedule_list);

    netif_ctrlif_init();

    (void)request_irq(bind_virq_to_irq(VIRQ_DEBUG),
                      netif_be_dbg, SA_SHIRQ, 
                      "net-be-dbg", &netif_be_dbg);

    return 0;
}

static void netback_cleanup(void)
{
    BUG();
}

module_init(netback_init);
module_exit(netback_cleanup);
