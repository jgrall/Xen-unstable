/*
 * 	NET3	Protocol independent device support routines.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/config.h>
#include <linux/delay.h>
#include <linux/lib.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/brlock.h>
#include <linux/init.h>
#include <linux/module.h>

#include <linux/event.h>
#include <asm/domain_page.h>
#include <asm/pgalloc.h>

#include <xeno/perfc.h>

#define BUG_TRAP ASSERT
#define notifier_call_chain(_a,_b,_c) ((void)0)
#define rtmsg_ifinfo(_a,_b,_c) ((void)0)
#define rtnl_lock() ((void)0)
#define rtnl_unlock() ((void)0)

#if 0
#define DPRINTK(_f, _a...) printk(_f , ## _a)
#else 
#define DPRINTK(_f, _a...) ((void)0)
#endif

#define TX_RING_INC(_i)    (((_i)+1) & (TX_RING_SIZE-1))
#define RX_RING_INC(_i)    (((_i)+1) & (RX_RING_SIZE-1))
#define TX_RING_ADD(_i,_j) (((_i)+(_j)) & (TX_RING_SIZE-1))
#define RX_RING_ADD(_i,_j) (((_i)+(_j)) & (RX_RING_SIZE-1))

static void make_tx_response(net_vif_t *vif, 
                             unsigned short id, 
                             unsigned char  st);
static void make_rx_response(net_vif_t     *vif, 
                             unsigned short id, 
                             unsigned short size,
                             unsigned char  st,
                             unsigned char  off);

struct net_device *the_dev = NULL;

/*
 * Transmitted packets are fragmented, so we can copy the important headesr 
 * before checking them for validity. Avoids need for page protection.
 */
/* Ethernet + IP headers */
#define PKT_PROT_LEN (ETH_HLEN + 20)
static kmem_cache_t *net_header_cachep;

/**
 *	__dev_get_by_name	- find a device by its name 
 *	@name: name to find
 *
 *	Find an interface by name. Must be called under RTNL semaphore
 *	or @dev_base_lock. If the name is found a pointer to the device
 *	is returned. If the name is not found then %NULL is returned. The
 *	reference counters are not incremented so the caller must be
 *	careful with locks.
 */
 

struct net_device *__dev_get_by_name(const char *name)
{
    struct net_device *dev;

    for (dev = dev_base; dev != NULL; dev = dev->next) {
        if (strncmp(dev->name, name, IFNAMSIZ) == 0)
            return dev;
    }
    return NULL;
}

/**
 *	dev_get_by_name		- find a device by its name
 *	@name: name to find
 *
 *	Find an interface by name. This can be called from any 
 *	context and does its own locking. The returned handle has
 *	the usage count incremented and the caller must use dev_put() to
 *	release it when it is no longer needed. %NULL is returned if no
 *	matching device is found.
 */

struct net_device *dev_get_by_name(const char *name)
{
    struct net_device *dev;

    read_lock(&dev_base_lock);
    dev = __dev_get_by_name(name);
    if (dev)
        dev_hold(dev);
    read_unlock(&dev_base_lock);
    return dev;
}

/**
 *	dev_get	-	test if a device exists
 *	@name:	name to test for
 *
 *	Test if a name exists. Returns true if the name is found. In order
 *	to be sure the name is not allocated or removed during the test the
 *	caller must hold the rtnl semaphore.
 *
 *	This function primarily exists for back compatibility with older
 *	drivers. 
 */
 
int dev_get(const char *name)
{
    struct net_device *dev;

    read_lock(&dev_base_lock);
    dev = __dev_get_by_name(name);
    read_unlock(&dev_base_lock);
    return dev != NULL;
}

/**
 *	__dev_get_by_index - find a device by its ifindex
 *	@ifindex: index of device
 *
 *	Search for an interface by index. Returns %NULL if the device
 *	is not found or a pointer to the device. The device has not
 *	had its reference counter increased so the caller must be careful
 *	about locking. The caller must hold either the RTNL semaphore
 *	or @dev_base_lock.
 */

struct net_device * __dev_get_by_index(int ifindex)
{
    struct net_device *dev;

    for (dev = dev_base; dev != NULL; dev = dev->next) {
        if (dev->ifindex == ifindex)
            return dev;
    }
    return NULL;
}


/**
 *	dev_get_by_index - find a device by its ifindex
 *	@ifindex: index of device
 *
 *	Search for an interface by index. Returns NULL if the device
 *	is not found or a pointer to the device. The device returned has 
 *	had a reference added and the pointer is safe until the user calls
 *	dev_put to indicate they have finished with it.
 */

struct net_device * dev_get_by_index(int ifindex)
{
    struct net_device *dev;

    read_lock(&dev_base_lock);
    dev = __dev_get_by_index(ifindex);
    if (dev)
        dev_hold(dev);
    read_unlock(&dev_base_lock);
    return dev;
}

/**
 *	dev_getbyhwaddr - find a device by its hardware address
 *	@type: media type of device
 *	@ha: hardware address
 *
 *	Search for an interface by MAC address. Returns NULL if the device
 *	is not found or a pointer to the device. The caller must hold the
 *	rtnl semaphore. The returned device has not had its ref count increased
 *	and the caller must therefore be careful about locking
 *
 *	BUGS:
 *	If the API was consistent this would be __dev_get_by_hwaddr
 */

struct net_device *dev_getbyhwaddr(unsigned short type, char *ha)
{
    struct net_device *dev;

    for (dev = dev_base; dev != NULL; dev = dev->next) {
        if (dev->type == type &&
            memcmp(dev->dev_addr, ha, dev->addr_len) == 0)
            return dev;
    }
    return NULL;
}

/**
 *	dev_alloc_name - allocate a name for a device
 *	@dev: device 
 *	@name: name format string
 *
 *	Passed a format string - eg "lt%d" it will try and find a suitable
 *	id. Not efficient for many devices, not called a lot. The caller
 *	must hold the dev_base or rtnl lock while allocating the name and
 *	adding the device in order to avoid duplicates. Returns the number
 *	of the unit assigned or a negative errno code.
 */

int dev_alloc_name(struct net_device *dev, const char *name)
{
    int i;
    char buf[32];
    char *p;

    /*
     * Verify the string as this thing may have come from
     * the user.  There must be either one "%d" and no other "%"
     * characters, or no "%" characters at all.
     */
    p = strchr(name, '%');
    if (p && (p[1] != 'd' || strchr(p+2, '%')))
        return -EINVAL;

    /*
     * If you need over 100 please also fix the algorithm...
     */
    for (i = 0; i < 100; i++) {
        snprintf(buf,sizeof(buf),name,i);
        if (__dev_get_by_name(buf) == NULL) {
            strcpy(dev->name, buf);
            return i;
        }
    }
    return -ENFILE;	/* Over 100 of the things .. bail out! */
}

/**
 *	dev_alloc - allocate a network device and name
 *	@name: name format string
 *	@err: error return pointer
 *
 *	Passed a format string, eg. "lt%d", it will allocate a network device
 *	and space for the name. %NULL is returned if no memory is available.
 *	If the allocation succeeds then the name is assigned and the 
 *	device pointer returned. %NULL is returned if the name allocation
 *	failed. The cause of an error is returned as a negative errno code
 *	in the variable @err points to.
 *
 *	The caller must hold the @dev_base or RTNL locks when doing this in
 *	order to avoid duplicate name allocations.
 */

struct net_device *dev_alloc(const char *name, int *err)
{
    struct net_device *dev=kmalloc(sizeof(struct net_device), GFP_KERNEL);
    if (dev == NULL) {
        *err = -ENOBUFS;
        return NULL;
    }
    memset(dev, 0, sizeof(struct net_device));
    *err = dev_alloc_name(dev, name);
    if (*err < 0) {
        kfree(dev);
        return NULL;
    }
    return dev;
}

/**
 *	netdev_state_change - device changes state
 *	@dev: device to cause notification
 *
 *	Called to indicate a device has changed state. This function calls
 *	the notifier chains for netdev_chain and sends a NEWLINK message
 *	to the routing socket.
 */
 
void netdev_state_change(struct net_device *dev)
{
    if (dev->flags&IFF_UP) {
        notifier_call_chain(&netdev_chain, NETDEV_CHANGE, dev);
        rtmsg_ifinfo(RTM_NEWLINK, dev, 0);
    }
}


#ifdef CONFIG_KMOD

/**
 *	dev_load 	- load a network module
 *	@name: name of interface
 *
 *	If a network interface is not present and the process has suitable
 *	privileges this function loads the module. If module loading is not
 *	available in this kernel then it becomes a nop.
 */

void dev_load(const char *name)
{
    if (!dev_get(name) && capable(CAP_SYS_MODULE))
        request_module(name);
}

#else

extern inline void dev_load(const char *unused){;}

#endif

static int default_rebuild_header(struct sk_buff *skb)
{
    printk(KERN_DEBUG "%s: default_rebuild_header called -- BUG!\n", 
           skb->dev ? skb->dev->name : "NULL!!!");
    kfree_skb(skb);
    return 1;
}

/**
 *	dev_open	- prepare an interface for use. 
 *	@dev:	device to open
 *
 *	Takes a device from down to up state. The device's private open
 *	function is invoked and then the multicast lists are loaded. Finally
 *	the device is moved into the up state and a %NETDEV_UP message is
 *	sent to the netdev notifier chain.
 *
 *	Calling this function on an active interface is a nop. On a failure
 *	a negative errno code is returned.
 */
 
int dev_open(struct net_device *dev)
{
    int ret = 0;

    /*
     *	Is it already up?
     */

    if (dev->flags&IFF_UP)
        return 0;

    /*
     *	Is it even present?
     */
    if (!netif_device_present(dev))
        return -ENODEV;

    /*
     *	Call device private open method
     */
    if (try_inc_mod_count(dev->owner)) {
        if (dev->open) {
            ret = dev->open(dev);
            if (ret != 0 && dev->owner)
                __MOD_DEC_USE_COUNT(dev->owner);
        }
    } else {
        ret = -ENODEV;
    }

    /*
     *	If it went open OK then:
     */
	 
    if (ret == 0) 
    {
        /*
         *	Set the flags.
         */
        dev->flags |= IFF_UP;

        set_bit(__LINK_STATE_START, &dev->state);

        /*
         *	Initialize multicasting status 
         */
        dev_mc_upload(dev);

        /*
         *	Wakeup transmit queue engine
         */
        dev_activate(dev);

        /*
         *	... and announce new interface.
         */
        notifier_call_chain(&netdev_chain, NETDEV_UP, dev);
    }
    return(ret);
}


/**
 *	dev_close - shutdown an interface.
 *	@dev: device to shutdown
 *
 *	This function moves an active device into down state. A 
 *	%NETDEV_GOING_DOWN is sent to the netdev notifier chain. The device
 *	is then deactivated and finally a %NETDEV_DOWN is sent to the notifier
 *	chain.
 */
 
int dev_close(struct net_device *dev)
{
    if (!(dev->flags&IFF_UP))
        return 0;

    /*
     *	Tell people we are going down, so that they can
     *	prepare to death, when device is still operating.
     */
    notifier_call_chain(&netdev_chain, NETDEV_GOING_DOWN, dev);

    dev_deactivate(dev);

    clear_bit(__LINK_STATE_START, &dev->state);

    /*
     *	Call the device specific close. This cannot fail.
     *	Only if device is UP
     *
     *	We allow it to be called even after a DETACH hot-plug
     *	event.
     */
	 
    if (dev->stop)
        dev->stop(dev);

    /*
     *	Device is now down.
     */

    dev->flags &= ~IFF_UP;

    /*
     *	Tell people we are down
     */
    notifier_call_chain(&netdev_chain, NETDEV_DOWN, dev);

    /*
     * Drop the module refcount
     */
    if (dev->owner)
        __MOD_DEC_USE_COUNT(dev->owner);

    return(0);
}


#ifdef CONFIG_HIGHMEM
/* Actually, we should eliminate this check as soon as we know, that:
 * 1. IOMMU is present and allows to map all the memory.
 * 2. No high memory really exists on this machine.
 */

static inline int
illegal_highdma(struct net_device *dev, struct sk_buff *skb)
{
    int i;

    if (dev->features&NETIF_F_HIGHDMA)
        return 0;

    for (i=0; i<skb_shinfo(skb)->nr_frags; i++)
        if (skb_shinfo(skb)->frags[i].page >= highmem_start_page)
            return 1;

    return 0;
}
#else
#define illegal_highdma(dev, skb)	(0)
#endif


/*=======================================================================
			Receiver routines
  =======================================================================*/

struct netif_rx_stats netdev_rx_stat[NR_CPUS];

void deliver_packet(struct sk_buff *skb, net_vif_t *vif)
{
    rx_shadow_entry_t *rx;
    unsigned long *ptep; 
    struct pfn_info *old_page, *new_page, *pte_page;
    unsigned int i; 
    unsigned short size;
    unsigned char  offset, status = RING_STATUS_OK;

    memcpy(skb->mac.ethernet->h_dest, vif->vmac, ETH_ALEN);
    if ( ntohs(skb->mac.ethernet->h_proto) == ETH_P_ARP )
        memcpy(skb->nh.raw + 18, vif->vmac, ETH_ALEN);

    /*
     * Slightly gross: we need the page_lock so that we can do PTE checking.
     * However, we take it slightly early so that it can protect the update
     * of rx_cons. This saves us from grabbing two locks.
     */
    spin_lock(&vif->domain->page_lock);

    if ( (i = vif->rx_cons) == vif->rx_prod )
    {
        spin_unlock(&vif->domain->page_lock);
        perfc_incr(net_rx_capacity_drop);
        return;
    }
    rx = vif->rx_shadow_ring + i;
    vif->rx_cons = RX_RING_INC(i);

    size   = (unsigned short)skb->len;
    offset = (unsigned char)((unsigned long)skb->data & ~PAGE_MASK);

    /* Release the page-table page. */
    pte_page = frame_table + (rx->pte_ptr >> PAGE_SHIFT);
    put_page_type(pte_page);
    put_page_tot(pte_page);

    old_page = frame_table + rx->buf_pfn;
    new_page = skb->pf;
    
    ptep = map_domain_mem(rx->pte_ptr);

    if ( (*ptep & _PAGE_PRESENT) )
    {
        /* Bail out if the PTE has been reused under our feet. */
        list_add(&old_page->list, &vif->domain->pg_head);
        old_page->flags = vif->domain->domain;
        unmap_domain_mem(ptep);
        spin_unlock(&vif->domain->page_lock);
        status = RING_STATUS_BAD_PAGE;
        goto out;
    }

    /* Give the new page to the domain, marking it writeable. */
    new_page->tot_count = new_page->type_count = 1;
    new_page->flags = vif->domain->domain | PGT_writeable_page | PG_need_flush;
    list_add(&new_page->list, &vif->domain->pg_head);
    
    /* Patch the PTE to map the new page as writeable. */
    machine_to_phys_mapping[new_page - frame_table] 
        = machine_to_phys_mapping[old_page - frame_table];        
    *ptep = (*ptep & ~PAGE_MASK) | _PAGE_RW | _PAGE_PRESENT |
        (((new_page - frame_table) << PAGE_SHIFT) & PAGE_MASK);
    
    unmap_domain_mem(ptep);

    spin_unlock(&vif->domain->page_lock);
    
    /* Our skbuff now points at the guest's old frame. */
    skb->pf = old_page;

    /* Updates must happen before releasing the descriptor. */
    smp_wmb();

    /*
     * NB. The remote flush here should be safe, as we hold no locks. The 
     * network driver that called us should also have no nasty locks.
     */
    if ( rx->flush_count == (unsigned short)
         atomic_read(&tlb_flush_count[vif->domain->processor]) )
    {
        perfc_incr(net_rx_tlbflush);
        flush_tlb_cpu(vif->domain->processor);
    }

    perfc_incr(net_rx_delivered);

    /* record this so they can be billed */
    vif->total_packets_received++;
    vif->total_bytes_received += size;

 out:
    make_rx_response(vif, rx->id, size, status, offset);
}

/**
 *	netif_rx	-	post buffer to the network code
 *	@skb: buffer to post
 *
 *	This function receives a packet from a device driver and queues it for
 *	the upper (protocol) levels to process.  It always succeeds. The buffer
 *	may be dropped during processing for congestion control or by the 
 *	protocol layers.
 *      
 *	return values:
 *	NET_RX_SUCCESS	(no congestion)           
 *	NET_RX_DROP    (packet was dropped)
 */

int netif_rx(struct sk_buff *skb)
{
    int offset, this_cpu = smp_processor_id();
    unsigned long flags;

    local_irq_save(flags);

    ASSERT(skb->skb_type == SKB_ZERO_COPY);

    /*
     * Offset will include 16 bytes padding from dev_alloc_skb, 14 bytes for 
     * ethernet header, plus any other alignment padding added by the driver.
     */
    offset = (int)skb->data & ~PAGE_MASK; 
    skb->head = (u8 *)map_domain_mem(((skb->pf - frame_table) << PAGE_SHIFT));
    skb->data = skb->nh.raw = skb->head + offset;
    skb->tail = skb->data + skb->len;
    skb_push(skb, ETH_HLEN);
    skb->mac.raw = skb->data;

    netdev_rx_stat[this_cpu].total++;

    if ( skb->dst_vif == NULL )
        skb->dst_vif = net_get_target_vif(skb->data, skb->len, skb->src_vif);
        
    if ( !VIF_LOCAL(skb->dst_vif) )
        skb->dst_vif = find_vif_by_id(0);

    deliver_packet(skb, skb->dst_vif);
    put_vif(skb->dst_vif);

    unmap_domain_mem(skb->head);
    kfree_skb(skb);
    local_irq_restore(flags);
    return NET_RX_SUCCESS;
}


/*************************************************************
 * NEW TRANSMIT SCHEDULER
 * 
 * NB. We ought also to only send a limited number of bytes to the NIC
 * for transmission at any one time (to avoid head-of-line blocking).
 * However, driver rings are small enough that they provide a reasonable
 * limit.
 * 
 * eg. 3c905 has 16 descriptors == 8 packets, at 100Mbps
 *     e1000 has 256 descriptors == 128 packets, at 1000Mbps
 *     tg3 has 512 descriptors == 256 packets, at 1000Mbps
 * 
 * So, worst case is tg3 with 256 1500-bytes packets == 375kB.
 * This would take 3ms, and represents our worst-case HoL blocking cost.
 * 
 * We think this is reasonable.
 */

struct list_head net_schedule_list;
spinlock_t net_schedule_list_lock;

static int __on_net_schedule_list(net_vif_t *vif)
{
    return vif->list.next != NULL;
}

static void remove_from_net_schedule_list(net_vif_t *vif)
{
    unsigned long flags;
    spin_lock_irqsave(&net_schedule_list_lock, flags);
    ASSERT(__on_net_schedule_list(vif));
    list_del(&vif->list);
    vif->list.next = NULL;
    put_vif(vif);
    spin_unlock_irqrestore(&net_schedule_list_lock, flags);
}

static void add_to_net_schedule_list_tail(net_vif_t *vif)
{
    unsigned long flags;
    if ( __on_net_schedule_list(vif) ) return;
    spin_lock_irqsave(&net_schedule_list_lock, flags);
    if ( !__on_net_schedule_list(vif) )
    {
        list_add_tail(&vif->list, &net_schedule_list);
        get_vif(vif);
    }
    spin_unlock_irqrestore(&net_schedule_list_lock, flags);
}


/* Destructor function for tx skbs. */
static void tx_skb_release(struct sk_buff *skb)
{
    int i;
    net_vif_t *vif = skb->src_vif;
    unsigned long flags;
    
    spin_lock_irqsave(&vif->domain->page_lock, flags);
    for ( i = 0; i < skb_shinfo(skb)->nr_frags; i++ )
        put_page_tot(skb_shinfo(skb)->frags[i].page);
    spin_unlock_irqrestore(&vif->domain->page_lock, flags);

    if ( skb->skb_type == SKB_NODATA )
        kmem_cache_free(net_header_cachep, skb->head);

    skb_shinfo(skb)->nr_frags = 0; 

    make_tx_response(vif, skb->guest_id, RING_STATUS_OK);

    put_vif(vif);
}

    
static void net_tx_action(unsigned long unused)
{
    struct net_device *dev = the_dev;
    struct list_head *ent;
    struct sk_buff *skb;
    net_vif_t *vif;
    tx_shadow_entry_t *tx;

    spin_lock(&dev->xmit_lock);
    while ( !netif_queue_stopped(dev) &&
            !list_empty(&net_schedule_list) )
    {
        /* Get a vif from the list with work to do. */
        ent = net_schedule_list.next;
        vif = list_entry(ent, net_vif_t, list);
        get_vif(vif);
        remove_from_net_schedule_list(vif);
        if ( vif->tx_cons == vif->tx_prod )
        {
            put_vif(vif);
            continue;
        }

        if ( (skb = alloc_skb_nodata(GFP_ATOMIC)) == NULL )
        {
            printk("Out of memory in net_tx_action()!\n");
            add_to_net_schedule_list_tail(vif);
            put_vif(vif);
            break;
        }
        
        /* Pick an entry from the transmit queue. */
        tx = &vif->tx_shadow_ring[vif->tx_cons];
        vif->tx_cons = TX_RING_INC(vif->tx_cons);
        if ( vif->tx_cons != vif->tx_prod )
            add_to_net_schedule_list_tail(vif);

        skb->destructor = tx_skb_release;

        skb->head = skb->data = tx->header;
        skb->end  = skb->tail = skb->head + PKT_PROT_LEN;
        
        skb->dev      = the_dev;
        skb->src_vif  = vif;
        skb->dst_vif  = NULL;
        skb->mac.raw  = skb->data; 
        skb->guest_id = tx->id;
        
        skb_shinfo(skb)->frags[0].page        = frame_table +
            (tx->payload >> PAGE_SHIFT);
        skb_shinfo(skb)->frags[0].size        = tx->size - PKT_PROT_LEN;
        skb_shinfo(skb)->frags[0].page_offset = tx->payload & ~PAGE_MASK;
        skb_shinfo(skb)->nr_frags = 1;

        skb->data_len = tx->size - PKT_PROT_LEN;
        skb->len      = tx->size;

        /* record the transmission so they can be billed */
        vif->total_packets_sent++;
        vif->total_bytes_sent += tx->size;

        /* Is the NIC crap? */
        if ( !(dev->features & NETIF_F_SG) )
            skb_linearize(skb, GFP_KERNEL);

        /* Transmit should always work, or the queue would be stopped. */
        if ( dev->hard_start_xmit(skb, dev) != 0 )
        {
            printk("Weird failure in hard_start_xmit!\n");
            kfree_skb(skb);
            break;
        }
    }
    spin_unlock(&dev->xmit_lock);
}

DECLARE_TASKLET_DISABLED(net_tx_tasklet, net_tx_action, 0);

static inline void maybe_schedule_tx_action(void)
{
    smp_mb();
    if ( !netif_queue_stopped(the_dev) &&
         !list_empty(&net_schedule_list) )
        tasklet_schedule(&net_tx_tasklet);
}


/*
 *	We need this ioctl for efficient implementation of the
 *	if_indextoname() function required by the IPv6 API.  Without
 *	it, we would have to search all the interfaces to find a
 *	match.  --pb
 */

static int dev_ifname(struct ifreq *arg)
{
    struct net_device *dev;
    struct ifreq ifr;

    /*
     *	Fetch the caller's info block. 
     */
	
    if (copy_from_user(&ifr, arg, sizeof(struct ifreq)))
        return -EFAULT;

    read_lock(&dev_base_lock);
    dev = __dev_get_by_index(ifr.ifr_ifindex);
    if (!dev) {
        read_unlock(&dev_base_lock);
        return -ENODEV;
    }

    strcpy(ifr.ifr_name, dev->name);
    read_unlock(&dev_base_lock);

    if (copy_to_user(arg, &ifr, sizeof(struct ifreq)))
        return -EFAULT;
    return 0;
}


/**
 *	netdev_set_master	-	set up master/slave pair
 *	@slave: slave device
 *	@master: new master device
 *
 *	Changes the master device of the slave. Pass %NULL to break the
 *	bonding. The caller must hold the RTNL semaphore. On a failure
 *	a negative errno code is returned. On success the reference counts
 *	are adjusted, %RTM_NEWLINK is sent to the routing socket and the
 *	function returns zero.
 */
 
int netdev_set_master(struct net_device *slave, struct net_device *master)
{
    struct net_device *old = slave->master;

    if (master) {
        if (old)
            return -EBUSY;
        dev_hold(master);
    }

    br_write_lock_bh(BR_NETPROTO_LOCK);
    slave->master = master;
    br_write_unlock_bh(BR_NETPROTO_LOCK);

    if (old)
        dev_put(old);

    if (master)
        slave->flags |= IFF_SLAVE;
    else
        slave->flags &= ~IFF_SLAVE;

    rtmsg_ifinfo(RTM_NEWLINK, slave, IFF_SLAVE);
    return 0;
}

/**
 *	dev_set_promiscuity	- update promiscuity count on a device
 *	@dev: device
 *	@inc: modifier
 *
 *	Add or remove promsicuity from a device. While the count in the device
 *	remains above zero the interface remains promiscuous. Once it hits zero
 *	the device reverts back to normal filtering operation. A negative inc
 *	value is used to drop promiscuity on the device.
 */
 
void dev_set_promiscuity(struct net_device *dev, int inc)
{
    unsigned short old_flags = dev->flags;

    dev->flags |= IFF_PROMISC;
    if ((dev->promiscuity += inc) == 0)
        dev->flags &= ~IFF_PROMISC;
    if (dev->flags^old_flags) {
#ifdef CONFIG_NET_FASTROUTE
        if (dev->flags&IFF_PROMISC) {
            netdev_fastroute_obstacles++;
            dev_clear_fastroute(dev);
        } else
            netdev_fastroute_obstacles--;
#endif
        dev_mc_upload(dev);
        printk(KERN_INFO "device %s %s promiscuous mode\n",
               dev->name, (dev->flags&IFF_PROMISC) ? "entered" : "left");
    }
}

/**
 *	dev_set_allmulti	- update allmulti count on a device
 *	@dev: device
 *	@inc: modifier
 *
 *	Add or remove reception of all multicast frames to a device. While the
 *	count in the device remains above zero the interface remains listening
 *	to all interfaces. Once it hits zero the device reverts back to normal
 *	filtering operation. A negative @inc value is used to drop the counter
 *	when releasing a resource needing all multicasts.
 */

void dev_set_allmulti(struct net_device *dev, int inc)
{
    unsigned short old_flags = dev->flags;

    dev->flags |= IFF_ALLMULTI;
    if ((dev->allmulti += inc) == 0)
        dev->flags &= ~IFF_ALLMULTI;
    if (dev->flags^old_flags)
        dev_mc_upload(dev);
}

int dev_change_flags(struct net_device *dev, unsigned flags)
{
    int ret;
    int old_flags = dev->flags;

    /*
     *	Set the flags on our device.
     */

    dev->flags = (flags & (IFF_DEBUG|IFF_NOTRAILERS|IFF_NOARP|IFF_DYNAMIC|
                           IFF_MULTICAST|IFF_PORTSEL|IFF_AUTOMEDIA)) |
        (dev->flags & (IFF_UP|IFF_VOLATILE|IFF_PROMISC|IFF_ALLMULTI));

    /*
     *	Load in the correct multicast list now the flags have changed.
     */				

    dev_mc_upload(dev);

    /*
     *	Have we downed the interface. We handle IFF_UP ourselves
     *	according to user attempts to set it, rather than blindly
     *	setting it.
     */

    ret = 0;
    if ((old_flags^flags)&IFF_UP)	/* Bit is different  ? */
    {
        ret = ((old_flags & IFF_UP) ? dev_close : dev_open)(dev);

        if (ret == 0) 
            dev_mc_upload(dev);
    }

    if (dev->flags&IFF_UP &&
        ((old_flags^dev->flags)&
         ~(IFF_UP|IFF_PROMISC|IFF_ALLMULTI|IFF_VOLATILE)))
        notifier_call_chain(&netdev_chain, NETDEV_CHANGE, dev);

    if ((flags^dev->gflags)&IFF_PROMISC) {
        int inc = (flags&IFF_PROMISC) ? +1 : -1;
        dev->gflags ^= IFF_PROMISC;
        dev_set_promiscuity(dev, inc);
    }

    /* NOTE: order of synchronization of IFF_PROMISC and IFF_ALLMULTI
       is important. Some (broken) drivers set IFF_PROMISC, when
       IFF_ALLMULTI is requested not asking us and not reporting.
    */
    if ((flags^dev->gflags)&IFF_ALLMULTI) {
        int inc = (flags&IFF_ALLMULTI) ? +1 : -1;
        dev->gflags ^= IFF_ALLMULTI;
        dev_set_allmulti(dev, inc);
    }

    if (old_flags^dev->flags)
        rtmsg_ifinfo(RTM_NEWLINK, dev, old_flags^dev->flags);

    return ret;
}

/*
 *	Perform the SIOCxIFxxx calls. 
 */
 
static int dev_ifsioc(struct ifreq *ifr, unsigned int cmd)
{
    struct net_device *dev;
    int err;

    if ((dev = __dev_get_by_name(ifr->ifr_name)) == NULL)
        return -ENODEV;

    switch(cmd) 
    {
    case SIOCGIFFLAGS:	/* Get interface flags */
        ifr->ifr_flags = (dev->flags&~(IFF_PROMISC|IFF_ALLMULTI|IFF_RUNNING))
            |(dev->gflags&(IFF_PROMISC|IFF_ALLMULTI));
        if (netif_running(dev) && netif_carrier_ok(dev))
            ifr->ifr_flags |= IFF_RUNNING;
        return 0;

    case SIOCSIFFLAGS:	/* Set interface flags */
        return dev_change_flags(dev, ifr->ifr_flags);
		
    case SIOCGIFMETRIC:	/* Get the metric on the interface */
        ifr->ifr_metric = 0;
        return 0;
			
    case SIOCSIFMETRIC:	/* Set the metric on the interface */
        return -EOPNOTSUPP;
	
    case SIOCGIFMTU:	/* Get the MTU of a device */
        ifr->ifr_mtu = dev->mtu;
        return 0;
	
    case SIOCSIFMTU:	/* Set the MTU of a device */
        if (ifr->ifr_mtu == dev->mtu)
            return 0;

        /*
         *	MTU must be positive.
         */
			 
        if (ifr->ifr_mtu<0)
            return -EINVAL;

        if (!netif_device_present(dev))
            return -ENODEV;

        if (dev->change_mtu)
            err = dev->change_mtu(dev, ifr->ifr_mtu);
        else {
            dev->mtu = ifr->ifr_mtu;
            err = 0;
        }
        if (!err && dev->flags&IFF_UP)
            notifier_call_chain(&netdev_chain, NETDEV_CHANGEMTU, dev);
        return err;

    case SIOCGIFHWADDR:
        memcpy(ifr->ifr_hwaddr.sa_data,dev->dev_addr, MAX_ADDR_LEN);
        ifr->ifr_hwaddr.sa_family=dev->type;
        return 0;
				
    case SIOCSIFHWADDR:
        if (dev->set_mac_address == NULL)
            return -EOPNOTSUPP;
        if (ifr->ifr_hwaddr.sa_family!=dev->type)
            return -EINVAL;
        if (!netif_device_present(dev))
            return -ENODEV;
        err = dev->set_mac_address(dev, &ifr->ifr_hwaddr);
        if (!err)
            notifier_call_chain(&netdev_chain, NETDEV_CHANGEADDR, dev);
        return err;
			
    case SIOCSIFHWBROADCAST:
        if (ifr->ifr_hwaddr.sa_family!=dev->type)
            return -EINVAL;
        memcpy(dev->broadcast, ifr->ifr_hwaddr.sa_data, MAX_ADDR_LEN);
        notifier_call_chain(&netdev_chain, NETDEV_CHANGEADDR, dev);
        return 0;

    case SIOCGIFMAP:
        ifr->ifr_map.mem_start=dev->mem_start;
        ifr->ifr_map.mem_end=dev->mem_end;
        ifr->ifr_map.base_addr=dev->base_addr;
        ifr->ifr_map.irq=dev->irq;
        ifr->ifr_map.dma=dev->dma;
        ifr->ifr_map.port=dev->if_port;
        return 0;
			
    case SIOCSIFMAP:
        if (dev->set_config) {
            if (!netif_device_present(dev))
                return -ENODEV;
            return dev->set_config(dev,&ifr->ifr_map);
        }
        return -EOPNOTSUPP;
			
    case SIOCADDMULTI:
        if (dev->set_multicast_list == NULL ||
            ifr->ifr_hwaddr.sa_family != AF_UNSPEC)
            return -EINVAL;
        if (!netif_device_present(dev))
            return -ENODEV;
        dev_mc_add(dev,ifr->ifr_hwaddr.sa_data, dev->addr_len, 1);
        return 0;

    case SIOCDELMULTI:
        if (dev->set_multicast_list == NULL ||
            ifr->ifr_hwaddr.sa_family!=AF_UNSPEC)
            return -EINVAL;
        if (!netif_device_present(dev))
            return -ENODEV;
        dev_mc_delete(dev,ifr->ifr_hwaddr.sa_data,dev->addr_len, 1);
        return 0;

    case SIOCGIFINDEX:
        ifr->ifr_ifindex = dev->ifindex;
        return 0;

    case SIOCSIFNAME:
        if (dev->flags&IFF_UP)
            return -EBUSY;
        if (__dev_get_by_name(ifr->ifr_newname))
            return -EEXIST;
        memcpy(dev->name, ifr->ifr_newname, IFNAMSIZ);
        dev->name[IFNAMSIZ-1] = 0;
        notifier_call_chain(&netdev_chain, NETDEV_CHANGENAME, dev);
        return 0;

#ifdef WIRELESS_EXT
    case SIOCGIWSTATS:
        return dev_iwstats(dev, ifr);
#endif	/* WIRELESS_EXT */

        /*
         *	Unknown or private ioctl
         */

    default:
        if ((cmd >= SIOCDEVPRIVATE &&
             cmd <= SIOCDEVPRIVATE + 15) ||
            cmd == SIOCBONDENSLAVE ||
            cmd == SIOCBONDRELEASE ||
            cmd == SIOCBONDSETHWADDR ||
            cmd == SIOCBONDSLAVEINFOQUERY ||
            cmd == SIOCBONDINFOQUERY ||
            cmd == SIOCBONDCHANGEACTIVE ||
            cmd == SIOCETHTOOL ||
            cmd == SIOCGMIIPHY ||
            cmd == SIOCGMIIREG ||
            cmd == SIOCSMIIREG) {
            if (dev->do_ioctl) {
                if (!netif_device_present(dev))
                    return -ENODEV;
                return dev->do_ioctl(dev, ifr, cmd);
            }
            return -EOPNOTSUPP;
        }

#ifdef WIRELESS_EXT
        if (cmd >= SIOCIWFIRST && cmd <= SIOCIWLAST) {
            if (dev->do_ioctl) {
                if (!netif_device_present(dev))
                    return -ENODEV;
                return dev->do_ioctl(dev, ifr, cmd);
            }
            return -EOPNOTSUPP;
        }
#endif	/* WIRELESS_EXT */

    }
    return -EINVAL;
}

/*
 * This function handles all "interface"-type I/O control requests. The actual
 * 'doing' part of this is dev_ifsioc above.
 */

/**
 *	dev_ioctl	-	network device ioctl
 *	@cmd: command to issue
 *	@arg: pointer to a struct ifreq in user space
 *
 *	Issue ioctl functions to devices. This is normally called by the
 *	user space syscall interfaces but can sometimes be useful for 
 *	other purposes. The return value is the return from the syscall if
 *	positive or a negative errno code on error.
 */

int dev_ioctl(unsigned int cmd, void *arg)
{
    struct ifreq ifr;
    int ret;
    char *colon;

    /* One special case: SIOCGIFCONF takes ifconf argument
       and requires shared lock, because it sleeps writing
       to user space.
    */
	   
    if (cmd == SIOCGIFCONF) {
        return -ENOSYS;
    }
    if (cmd == SIOCGIFNAME) {
        return dev_ifname((struct ifreq *)arg);
    }

    if (copy_from_user(&ifr, arg, sizeof(struct ifreq)))
        return -EFAULT;

    ifr.ifr_name[IFNAMSIZ-1] = 0;

    colon = strchr(ifr.ifr_name, ':');
    if (colon)
        *colon = 0;

    /*
     *	See which interface the caller is talking about. 
     */
	 
    switch(cmd) 
    {
        /*
         *	These ioctl calls:
         *	- can be done by all.
         *	- atomic and do not require locking.
         *	- return a value
         */
		 
    case SIOCGIFFLAGS:
    case SIOCGIFMETRIC:
    case SIOCGIFMTU:
    case SIOCGIFHWADDR:
    case SIOCGIFSLAVE:
    case SIOCGIFMAP:
    case SIOCGIFINDEX:
        dev_load(ifr.ifr_name);
        read_lock(&dev_base_lock);
        ret = dev_ifsioc(&ifr, cmd);
        read_unlock(&dev_base_lock);
        if (!ret) {
            if (colon)
                *colon = ':';
            if (copy_to_user(arg, &ifr, sizeof(struct ifreq)))
                return -EFAULT;
        }
        return ret;

        /*
         *	These ioctl calls:
         *	- require superuser power.
         *	- require strict serialization.
         *	- return a value
         */
		 
    case SIOCETHTOOL:
    case SIOCGMIIPHY:
    case SIOCGMIIREG:
        if (!capable(CAP_NET_ADMIN))
            return -EPERM;
        dev_load(ifr.ifr_name);
        dev_probe_lock();
        rtnl_lock();
        ret = dev_ifsioc(&ifr, cmd);
        rtnl_unlock();
        dev_probe_unlock();
        if (!ret) {
            if (colon)
                *colon = ':';
            if (copy_to_user(arg, &ifr, sizeof(struct ifreq)))
                return -EFAULT;
        }
        return ret;

        /*
         *	These ioctl calls:
         *	- require superuser power.
         *	- require strict serialization.
         *	- do not return a value
         */
		 
    case SIOCSIFFLAGS:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFMAP:
    case SIOCSIFHWADDR:
    case SIOCSIFSLAVE:
    case SIOCADDMULTI:
    case SIOCDELMULTI:
    case SIOCSIFHWBROADCAST:
    case SIOCSIFNAME:
    case SIOCSMIIREG:
    case SIOCBONDENSLAVE:
    case SIOCBONDRELEASE:
    case SIOCBONDSETHWADDR:
    case SIOCBONDSLAVEINFOQUERY:
    case SIOCBONDINFOQUERY:
    case SIOCBONDCHANGEACTIVE:
        if (!capable(CAP_NET_ADMIN))
            return -EPERM;
        dev_load(ifr.ifr_name);
        dev_probe_lock();
        rtnl_lock();
        ret = dev_ifsioc(&ifr, cmd);
        rtnl_unlock();
        dev_probe_unlock();
        return ret;
	
    case SIOCGIFMEM:
        /* Get the per device memory space. We can add this but currently
           do not support it */
    case SIOCSIFMEM:
        /* Set the per device memory buffer space. */
    case SIOCSIFLINK:
        return -EINVAL;

        /*
         *	Unknown or private ioctl.
         */	
		 
    default:
        if (cmd >= SIOCDEVPRIVATE &&
            cmd <= SIOCDEVPRIVATE + 15) {
            dev_load(ifr.ifr_name);
            dev_probe_lock();
            rtnl_lock();
            ret = dev_ifsioc(&ifr, cmd);
            rtnl_unlock();
            dev_probe_unlock();
            if (!ret && copy_to_user(arg, &ifr, sizeof(struct ifreq)))
                return -EFAULT;
            return ret;
        }
#ifdef WIRELESS_EXT
        /* Take care of Wireless Extensions */
        if (cmd >= SIOCIWFIRST && cmd <= SIOCIWLAST) {
				/* If command is `set a parameter', or
				 * `get the encoding parameters', check if
				 * the user has the right to do it */
            if (IW_IS_SET(cmd) || (cmd == SIOCGIWENCODE)) {
                if(!capable(CAP_NET_ADMIN))
                    return -EPERM;
            }
            dev_load(ifr.ifr_name);
            rtnl_lock();
            ret = dev_ifsioc(&ifr, cmd);
            rtnl_unlock();
            if (!ret && IW_IS_GET(cmd) &&
                copy_to_user(arg, &ifr, 
                             sizeof(struct ifreq)))
                return -EFAULT;
            return ret;
        }
#endif	/* WIRELESS_EXT */
        return -EINVAL;
    }
}


/**
 *	dev_new_index	-	allocate an ifindex
 *
 *	Returns a suitable unique value for a new device interface
 *	number.  The caller must hold the rtnl semaphore or the
 *	dev_base_lock to be sure it remains unique.
 */
 
int dev_new_index(void)
{
    static int ifindex;
    for (;;) {
        if (++ifindex <= 0)
            ifindex=1;
        if (__dev_get_by_index(ifindex) == NULL)
            return ifindex;
    }
}

static int dev_boot_phase = 1;

/**
 *	register_netdevice	- register a network device
 *	@dev: device to register
 *	
 *	Take a completed network device structure and add it to the kernel
 *	interfaces. A %NETDEV_REGISTER message is sent to the netdev notifier
 *	chain. 0 is returned on success. A negative errno code is returned
 *	on a failure to set up the device, or if the name is a duplicate.
 *
 *	Callers must hold the rtnl semaphore.  See the comment at the
 *	end of Space.c for details about the locking.  You may want
 *	register_netdev() instead of this.
 *
 *	BUGS:
 *	The locking appears insufficient to guarantee two parallel registers
 *	will not get the same name.
 */

int net_dev_init(void);

int register_netdevice(struct net_device *dev)
{
    struct net_device *d, **dp;
#ifdef CONFIG_NET_DIVERT
    int ret;
#endif

    spin_lock_init(&dev->queue_lock);
    spin_lock_init(&dev->xmit_lock);
    dev->xmit_lock_owner = -1;
#ifdef CONFIG_NET_FASTROUTE
    dev->fastpath_lock=RW_LOCK_UNLOCKED;
#endif

    if (dev_boot_phase)
        net_dev_init();

#ifdef CONFIG_NET_DIVERT
    ret = alloc_divert_blk(dev);
    if (ret)
        return ret;
#endif /* CONFIG_NET_DIVERT */
	
    dev->iflink = -1;

    /* Init, if this function is available */
    if (dev->init && dev->init(dev) != 0) {
#ifdef CONFIG_NET_DIVERT
        free_divert_blk(dev);
#endif
        return -EIO;
    }

    dev->ifindex = dev_new_index();
    if (dev->iflink == -1)
        dev->iflink = dev->ifindex;

    /* Check for existence, and append to tail of chain */
    for (dp=&dev_base; (d=*dp) != NULL; dp=&d->next) {
        if (d == dev || strcmp(d->name, dev->name) == 0) {
#ifdef CONFIG_NET_DIVERT
            free_divert_blk(dev);
#endif
            return -EEXIST;
        }
    }
    /*
     *	nil rebuild_header routine,
     *	that should be never called and used as just bug trap.
     */

    if (dev->rebuild_header == NULL)
        dev->rebuild_header = default_rebuild_header;

    /*
     *	Default initial state at registry is that the
     *	device is present.
     */

    set_bit(__LINK_STATE_PRESENT, &dev->state);

    dev->next = NULL;
    dev_init_scheduler(dev);
    write_lock_bh(&dev_base_lock);
    *dp = dev;
    dev_hold(dev);
    dev->deadbeaf = 0;
    write_unlock_bh(&dev_base_lock);

    /* Notify protocols, that a new device appeared. */
    notifier_call_chain(&netdev_chain, NETDEV_REGISTER, dev);

    return 0;
}

/**
 *	netdev_finish_unregister - complete unregistration
 *	@dev: device
 *
 *	Destroy and free a dead device. A value of zero is returned on
 *	success.
 */
 
int netdev_finish_unregister(struct net_device *dev)
{
    BUG_TRAP(dev->ip_ptr==NULL);
    BUG_TRAP(dev->ip6_ptr==NULL);
    BUG_TRAP(dev->dn_ptr==NULL);

    if (!dev->deadbeaf) {
        printk(KERN_ERR "Freeing alive device %p, %s\n",
               dev, dev->name);
        return 0;
    }
#ifdef NET_REFCNT_DEBUG
    printk(KERN_DEBUG "netdev_finish_unregister: %s%s.\n", dev->name,
           (dev->features & NETIF_F_DYNALLOC)?"":", old style");
#endif
    if (dev->destructor)
        dev->destructor(dev);
    if (dev->features & NETIF_F_DYNALLOC)
        kfree(dev);
    return 0;
}

/**
 *	unregister_netdevice - remove device from the kernel
 *	@dev: device
 *
 *	This function shuts down a device interface and removes it
 *	from the kernel tables. On success 0 is returned, on a failure
 *	a negative errno code is returned.
 *
 *	Callers must hold the rtnl semaphore.  See the comment at the
 *	end of Space.c for details about the locking.  You may want
 *	unregister_netdev() instead of this.
 */

int unregister_netdevice(struct net_device *dev)
{
    unsigned long now, warning_time;
    struct net_device *d, **dp;

    /* If device is running, close it first. */
    if (dev->flags & IFF_UP)
        dev_close(dev);

    BUG_TRAP(dev->deadbeaf==0);
    dev->deadbeaf = 1;

    /* And unlink it from device chain. */
    for (dp = &dev_base; (d=*dp) != NULL; dp=&d->next) {
        if (d == dev) {
            write_lock_bh(&dev_base_lock);
            *dp = d->next;
            write_unlock_bh(&dev_base_lock);
            break;
        }
    }
    if (d == NULL) {
        printk(KERN_DEBUG "unregister_netdevice: device %s/%p"
               " not registered\n", dev->name, dev);
        return -ENODEV;
    }

    /* Synchronize to net_rx_action. */
    br_write_lock_bh(BR_NETPROTO_LOCK);
    br_write_unlock_bh(BR_NETPROTO_LOCK);

    if (dev_boot_phase == 0) {

        /* Shutdown queueing discipline. */
        dev_shutdown(dev);

        /* Notify protocols, that we are about to destroy
           this device. They should clean all the things.
        */
        notifier_call_chain(&netdev_chain, NETDEV_UNREGISTER, dev);

        /*
         *	Flush the multicast chain
         */
        dev_mc_discard(dev);
    }

    if (dev->uninit)
        dev->uninit(dev);

    /* Notifier chain MUST detach us from master device. */
    BUG_TRAP(dev->master==NULL);

#ifdef CONFIG_NET_DIVERT
    free_divert_blk(dev);
#endif

    if (dev->features & NETIF_F_DYNALLOC) {
#ifdef NET_REFCNT_DEBUG
        if (atomic_read(&dev->refcnt) != 1)
            printk(KERN_DEBUG "unregister_netdevice: holding %s refcnt=%d\n",
                   dev->name, atomic_read(&dev->refcnt)-1);
#endif
        dev_put(dev);
        return 0;
    }

    /* Last reference is our one */
    if (atomic_read(&dev->refcnt) == 1) {
        dev_put(dev);
        return 0;
    }

#ifdef NET_REFCNT_DEBUG
    printk("unregister_netdevice: waiting %s refcnt=%d\n",
           dev->name, atomic_read(&dev->refcnt));
#endif

    /* EXPLANATION. If dev->refcnt is not now 1 (our own reference)
       it means that someone in the kernel still has a reference
       to this device and we cannot release it.

       "New style" devices have destructors, hence we can return from this
       function and destructor will do all the work later.  As of kernel 2.4.0
       there are very few "New Style" devices.

       "Old style" devices expect that the device is free of any references
       upon exit from this function.
       We cannot return from this function until all such references have
       fallen away.  This is because the caller of this function will probably
       immediately kfree(*dev) and then be unloaded via sys_delete_module.

       So, we linger until all references fall away.  The duration of the
       linger is basically unbounded! It is driven by, for example, the
       current setting of sysctl_ipfrag_time.

       After 1 second, we start to rebroadcast unregister notifications
       in hope that careless clients will release the device.

    */

    now = warning_time = jiffies;
    while (atomic_read(&dev->refcnt) != 1) {
        if ((jiffies - now) > 1*HZ) {
            /* Rebroadcast unregister notification */
            notifier_call_chain(&netdev_chain, NETDEV_UNREGISTER, dev);
        }
        mdelay(250);
        if ((jiffies - warning_time) > 10*HZ) {
            printk(KERN_EMERG "unregister_netdevice: waiting for %s to "
                   "become free. Usage count = %d\n",
                   dev->name, atomic_read(&dev->refcnt));
            warning_time = jiffies;
        }
    }
    dev_put(dev);
    return 0;
}


/*
 *	Initialize the DEV module. At boot time this walks the device list and
 *	unhooks any devices that fail to initialise (normally hardware not 
 *	present) and leaves us with a valid list of present and active devices.
 *
 */

extern void net_device_init(void);
extern void ip_auto_config(void);
#ifdef CONFIG_NET_DIVERT
extern void dv_init(void);
#endif /* CONFIG_NET_DIVERT */


/*
 *       Callers must hold the rtnl semaphore.  See the comment at the
 *       end of Space.c for details about the locking.
 */
int __init net_dev_init(void)
{
    struct net_device *dev, **dp;

    if ( !dev_boot_phase )
        return 0;

    skb_init();

    net_header_cachep = kmem_cache_create(
        "net_header_cache", 
        (PKT_PROT_LEN + sizeof(void *) - 1) & ~(sizeof(void *) - 1),
        0, SLAB_HWCACHE_ALIGN, NULL, NULL);

    spin_lock_init(&net_schedule_list_lock);
    INIT_LIST_HEAD(&net_schedule_list);

    /*
     *	Add the devices.
     *	If the call to dev->init fails, the dev is removed
     *	from the chain disconnecting the device until the
     *	next reboot.
     *
     *	NB At boot phase networking is dead. No locking is required.
     *	But we still preserve dev_base_lock for sanity.
     */
    dp = &dev_base;
    while ((dev = *dp) != NULL) {
        spin_lock_init(&dev->queue_lock);
        spin_lock_init(&dev->xmit_lock);

        dev->xmit_lock_owner = -1;
        dev->iflink = -1;
        dev_hold(dev);

        /*
         * Allocate name. If the init() fails
         * the name will be reissued correctly.
         */
        if (strchr(dev->name, '%'))
            dev_alloc_name(dev, dev->name);

        if (dev->init && dev->init(dev)) {
            /*
             * It failed to come up. It will be unhooked later.
             * dev_alloc_name can now advance to next suitable
             * name that is checked next.
             */
            dev->deadbeaf = 1;
            dp = &dev->next;
        } else {
            dp = &dev->next;
            dev->ifindex = dev_new_index();
            if (dev->iflink == -1)
                dev->iflink = dev->ifindex;
            if (dev->rebuild_header == NULL)
                dev->rebuild_header = default_rebuild_header;
            dev_init_scheduler(dev);
            set_bit(__LINK_STATE_PRESENT, &dev->state);
        }
    }

    /*
     * Unhook devices that failed to come up
     */
    dp = &dev_base;
    while ((dev = *dp) != NULL) {
        if (dev->deadbeaf) {
            write_lock_bh(&dev_base_lock);
            *dp = dev->next;
            write_unlock_bh(&dev_base_lock);
            dev_put(dev);
        } else {
            dp = &dev->next;
        }
    }

    dev_boot_phase = 0;

    dev_mcast_init();

    /*
     *	Initialise network devices
     */
	 
    net_device_init();

    return 0;
}

inline int init_tx_header(u8 *data, unsigned int len, struct net_device *dev)
{
    memcpy(data + ETH_ALEN, dev->dev_addr, ETH_ALEN);
        
    switch ( ntohs(*(unsigned short *)(data + 12)) )
    {
    case ETH_P_ARP:
        if ( len < 42 ) break;
        memcpy(data + 22, dev->dev_addr, ETH_ALEN);
        return ETH_P_ARP;
    case ETH_P_IP:
        return ETH_P_IP;
    }
    return 0;
}


/*
 * do_net_update:
 * 
 * Called from guest OS to notify updates to its transmit and/or receive
 * descriptor rings.
 */

long do_net_update(void)
{
    net_ring_t *shared_rings;
    net_vif_t *vif;
    net_idx_t *shared_idxs;
    unsigned int i, j, idx;
    struct sk_buff *skb, *interdom_skb = NULL;
    tx_req_entry_t tx;
    rx_req_entry_t rx;
    unsigned long pte_pfn, buf_pfn;
    struct pfn_info *pte_page, *buf_page;
    unsigned long *ptep;    
    net_vif_t *target;
    u8 *g_data;
    unsigned short protocol;

    for ( idx = 0; idx < MAX_DOMAIN_VIFS; idx++ )
    {
        if ( (vif = current->net_vif_list[idx]) == NULL )
            break;

        shared_idxs  = vif->shared_idxs;
        shared_rings = vif->shared_rings;
        
        /*
         * PHASE 1 -- TRANSMIT RING
         */

        /*
         * Collect up new transmit buffers. We collect up to the guest OS's
         * new producer index, but take care not to catch up with our own
         * consumer index.
         */
        j = vif->tx_prod;
        for ( i = vif->tx_req_cons; 
              (i != shared_idxs->tx_req_prod) && 
                  (((vif->tx_resp_prod-i) & (TX_RING_SIZE-1)) != 1); 
              i = TX_RING_INC(i) )
        {
            tx     = shared_rings->tx_ring[i].req;
            target = VIF_DROP;

            if ( (tx.size < PKT_PROT_LEN) || (tx.size > ETH_FRAME_LEN) )
            {
                DPRINTK("Bad packet size: %d\n", tx.size);
                make_tx_response(vif, tx.id, RING_STATUS_BAD_PAGE);
                continue; 
            }

            /* No crossing a page boundary as the payload mustn't fragment. */
            if ( ((tx.addr & ~PAGE_MASK) + tx.size) >= PAGE_SIZE ) 
            {
                DPRINTK("tx.addr: %lx, size: %u, end: %lu\n", 
                        tx.addr, tx.size, (tx.addr &~PAGE_MASK) + tx.size);
                make_tx_response(vif, tx.id, RING_STATUS_BAD_PAGE);
                continue;
            }

            buf_pfn  = tx.addr >> PAGE_SHIFT;
            buf_page = frame_table + buf_pfn;
            spin_lock_irq(&current->page_lock);
            if ( (buf_pfn >= max_page) || 
                 ((buf_page->flags & PG_domain_mask) != current->domain) ) 
            {
                DPRINTK("Bad page frame\n");
                spin_unlock_irq(&current->page_lock);
                make_tx_response(vif, tx.id, RING_STATUS_BAD_PAGE);
                continue;
            }
            
            g_data = map_domain_mem(tx.addr);

            protocol = __constant_htons(
                init_tx_header(g_data, tx.size, the_dev));
            if ( protocol == 0 )
            {
                make_tx_response(vif, tx.id, RING_STATUS_BAD_PAGE);
                goto tx_unmap_and_continue;
            }

            target = net_get_target_vif(g_data, tx.size, vif);

            if ( VIF_LOCAL(target) )
            {
                /* Local delivery */
                if ( (skb = dev_alloc_skb(ETH_FRAME_LEN + 32)) == NULL )
                {
                    make_tx_response(vif, tx.id, RING_STATUS_BAD_PAGE);
                    put_vif(target);
                    goto tx_unmap_and_continue;
                }

                skb->src_vif = vif;
                skb->dst_vif = target;
                skb->protocol = protocol;                

                /*
                 * We don't need a well-formed skb as netif_rx will fill these
                 * fields in as necessary. All we actually need is the right
                 * page offset in skb->data, and the right length in skb->len.
                 * Note that the correct address/length *excludes* link header.
                 */
                skb->head = (u8 *)map_domain_mem(
                    ((skb->pf - frame_table) << PAGE_SHIFT));
                skb->data = skb->head + 18;
                memcpy(skb->data, g_data, tx.size);
                skb->data += ETH_HLEN;
                skb->len = tx.size - ETH_HLEN;
                unmap_domain_mem(skb->head);

                /*
                 * We must defer netif_rx until we have released the current
                 * domain's page_lock, or we may deadlock on SMP.
                 */
                interdom_skb = skb;

                make_tx_response(vif, tx.id, RING_STATUS_OK);
            }
            else if ( (target == VIF_PHYS) || IS_PRIV(current) )
            {
                vif->tx_shadow_ring[j].id     = tx.id;
                vif->tx_shadow_ring[j].size   = tx.size;
                vif->tx_shadow_ring[j].header = 
                    kmem_cache_alloc(net_header_cachep, GFP_KERNEL);
                if ( vif->tx_shadow_ring[j].header == NULL )
                { 
                    make_tx_response(vif, tx.id, RING_STATUS_OK);
                    goto tx_unmap_and_continue;
                }

                memcpy(vif->tx_shadow_ring[j].header, g_data, PKT_PROT_LEN);
                vif->tx_shadow_ring[j].payload = tx.addr + PKT_PROT_LEN;
                get_page_tot(buf_page);
                j = TX_RING_INC(j);
            }
            else
            {
                make_tx_response(vif, tx.id, RING_STATUS_DROPPED);
            }

        tx_unmap_and_continue:
            unmap_domain_mem(g_data);
            spin_unlock_irq(&current->page_lock);
            if ( interdom_skb != NULL )
            {
                (void)netif_rx(interdom_skb);
                interdom_skb = NULL;
            }
        }

        vif->tx_req_cons = i;

        if ( vif->tx_prod != j )
        {
            smp_mb(); /* Let other CPUs see new descriptors first. */
            vif->tx_prod = j;
            add_to_net_schedule_list_tail(vif);
            maybe_schedule_tx_action();
        }

        /*
         * PHASE 2 -- RECEIVE RING
         */

        /*
         * Collect up new receive buffers. We collect up to the guest OS's
         * new producer index, but take care not to catch up with our own
         * consumer index.
         */
        j = vif->rx_prod;
        for ( i = vif->rx_req_cons; 
              (i != shared_idxs->rx_req_prod) && 
                  (((vif->rx_resp_prod-i) & (RX_RING_SIZE-1)) != 1); 
              i = RX_RING_INC(i) )
        {
            rx = shared_rings->rx_ring[i].req;

            pte_pfn = rx.addr >> PAGE_SHIFT;
            pte_page = frame_table + pte_pfn;
            
            spin_lock_irq(&current->page_lock);
            if ( (pte_pfn >= max_page) || 
                 ((pte_page->flags & (PG_type_mask | PG_domain_mask)) != 
                  (PGT_l1_page_table | current->domain)) ) 
            {
                DPRINTK("Bad page frame for ppte %d,%08lx,%08lx,%08lx\n",
                        current->domain, pte_pfn, max_page, pte_page->flags);
                spin_unlock_irq(&current->page_lock);
                make_rx_response(vif, rx.id, 0, RING_STATUS_BAD_PAGE, 0);
                continue;
            }
            
            ptep = map_domain_mem(rx.addr);
            
            if ( !(*ptep & _PAGE_PRESENT) )
            {
                DPRINTK("Invalid PTE passed down (not present)\n");
                make_rx_response(vif, rx.id, 0, RING_STATUS_BAD_PAGE, 0);
                goto rx_unmap_and_continue;
            }
            
            buf_pfn  = *ptep >> PAGE_SHIFT;
            buf_page = frame_table + buf_pfn;

            if ( ((buf_page->flags & (PG_type_mask | PG_domain_mask)) !=
                  (PGT_writeable_page | current->domain)) || 
                 (buf_page->tot_count != 1) )
            {
		DPRINTK("Need a mapped-once writeable page (%ld/%ld/%08lx)\n",
      		buf_page->type_count, buf_page->tot_count, buf_page->flags);
                make_rx_response(vif, rx.id, 0, RING_STATUS_BAD_PAGE, 0);
                goto rx_unmap_and_continue;
            }
            
            /*
             * The pte they passed was good, so take it away from them. We 
             * also lock down the page-table page, so it doesn't go away.
             */
            get_page_type(pte_page);
            get_page_tot(pte_page);
            *ptep &= ~_PAGE_PRESENT;
            buf_page->flags = buf_page->type_count = buf_page->tot_count = 0;
            list_del(&buf_page->list);

            vif->rx_shadow_ring[j].id          = rx.id;
            vif->rx_shadow_ring[j].pte_ptr     = rx.addr;
            vif->rx_shadow_ring[j].buf_pfn     = buf_pfn;
            vif->rx_shadow_ring[j].flush_count = (unsigned short) 
                atomic_read(&tlb_flush_count[smp_processor_id()]);
            j = RX_RING_INC(j);
            
        rx_unmap_and_continue:
            unmap_domain_mem(ptep);
            spin_unlock_irq(&current->page_lock);
        }

        vif->rx_req_cons = i;

        if ( vif->rx_prod != j )
        {
            smp_mb(); /* Let other CPUs see new descriptors first. */
            vif->rx_prod = j;
        }
    }

    return 0;
}


static void make_tx_response(net_vif_t     *vif, 
                             unsigned short id, 
                             unsigned char  st)
{
    unsigned long flags;
    unsigned int pos;
    tx_resp_entry_t *resp;

    /* Place on the response ring for the relevant domain. */ 
    spin_lock_irqsave(&vif->tx_lock, flags);
    pos  = vif->tx_resp_prod;
    resp = &vif->shared_rings->tx_ring[pos].resp;
    resp->id     = id;
    resp->status = st;
    pos = TX_RING_INC(pos);
    vif->tx_resp_prod = vif->shared_idxs->tx_resp_prod = pos;
    if ( pos == vif->shared_idxs->tx_event )
    {
        unsigned long cpu_mask = mark_guest_event(vif->domain, _EVENT_NET);
        guest_event_notify(cpu_mask);    
    }
    spin_unlock_irqrestore(&vif->tx_lock, flags);
}


static void make_rx_response(net_vif_t     *vif, 
                             unsigned short id, 
                             unsigned short size,
                             unsigned char  st,
                             unsigned char  off)
{
    unsigned long flags;
    unsigned int pos;
    rx_resp_entry_t *resp;

    /* Place on the response ring for the relevant domain. */ 
    spin_lock_irqsave(&vif->rx_lock, flags);
    pos  = vif->rx_resp_prod;
    resp = &vif->shared_rings->rx_ring[pos].resp;
    resp->id     = id;
    resp->size   = size;
    resp->status = st;
    resp->offset = off;
    pos = RX_RING_INC(pos);
    vif->rx_resp_prod = vif->shared_idxs->rx_resp_prod = pos;
    if ( pos == vif->shared_idxs->rx_event )
    {
        unsigned long cpu_mask = mark_guest_event(vif->domain, _EVENT_NET);
        guest_event_notify(cpu_mask);    
    }
    spin_unlock_irqrestore(&vif->rx_lock, flags);
}


int setup_network_devices(void)
{
    int ret;
    extern char opt_ifname[];
    struct net_device *dev = dev_get_by_name(opt_ifname);

    if ( dev == NULL ) 
    {
        printk("Could not find device %s\n", opt_ifname);
        return 0;
    }

    ret = dev_open(dev);
    if ( ret != 0 )
    {
        printk("Error opening device %s for use (%d)\n", opt_ifname, ret);
        return 0;
    }
    printk("Device %s opened and ready for use.\n", opt_ifname);
    the_dev = dev;

    tasklet_enable(&net_tx_tasklet);

    return 1;
}

