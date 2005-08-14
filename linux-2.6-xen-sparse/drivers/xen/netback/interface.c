/******************************************************************************
 * arch/xen/drivers/netif/backend/interface.c
 * 
 * Network-device interface management.
 * 
 * Copyright (c) 2004-2005, Keir Fraser
 */

#include "common.h"
#include <linux/rtnetlink.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#define VMALLOC_VMADDR(x) ((unsigned long)(x))
#endif

#define NETIF_HASHSZ 1024
#define NETIF_HASH(_d,_h) (((int)(_d)^(int)(_h))&(NETIF_HASHSZ-1))

static netif_t *netif_hash[NETIF_HASHSZ];

netif_t *netif_find_by_handle(domid_t domid, unsigned int handle)
{
    netif_t *netif = netif_hash[NETIF_HASH(domid, handle)];
    while ( (netif != NULL) && 
            ((netif->domid != domid) || (netif->handle != handle)) )
        netif = netif->hash_next;
    return netif;
}

static void __netif_up(netif_t *netif)
{
    struct net_device *dev = netif->dev;
    spin_lock_bh(&dev->xmit_lock);
    netif->active = 1;
    spin_unlock_bh(&dev->xmit_lock);
    (void)bind_evtchn_to_irqhandler(
        netif->evtchn, netif_be_int, 0, dev->name, netif);
    netif_schedule_work(netif);
}

static void __netif_down(netif_t *netif)
{
    struct net_device *dev = netif->dev;
    spin_lock_bh(&dev->xmit_lock);
    netif->active = 0;
    spin_unlock_bh(&dev->xmit_lock);
    unbind_evtchn_from_irqhandler(netif->evtchn, netif);
    netif_deschedule_work(netif);
}

static int net_open(struct net_device *dev)
{
    netif_t *netif = netdev_priv(dev);
    if ( netif->status == CONNECTED )
        __netif_up(netif);
    netif_start_queue(dev);
    return 0;
}

static int net_close(struct net_device *dev)
{
    netif_t *netif = netdev_priv(dev);
    netif_stop_queue(dev);
    if ( netif->status == CONNECTED )
        __netif_down(netif);
    return 0;
}

static void __netif_disconnect_complete(void *arg)
{
    netif_t              *netif = (netif_t *)arg;
    ctrl_msg_t            cmsg;
    netif_be_disconnect_t disc;
#if defined(CONFIG_XEN_NETDEV_GRANT_RX) || defined(CONFIG_XEN_NETDEV_GRANT_TX)
    struct gnttab_unmap_grant_ref op;
#endif

    /*
     * These can't be done in netif_disconnect() because at that point there
     * may be outstanding requests in the network stack whose asynchronous
     * responses must still be notified to the remote driver.
     */

#ifdef CONFIG_XEN_NETDEV_GRANT_TX
    op.host_addr    = netif->tx_shmem_vaddr;
    op.handle       = netif->tx_shmem_handle;
    op.dev_bus_addr = 0;
    BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1));
#endif

#ifdef CONFIG_XEN_NETDEV_GRANT_RX
    op.host_addr    = netif->rx_shmem_vaddr;
    op.handle       = netif->rx_shmem_handle;
    op.dev_bus_addr = 0;
    BUG_ON(HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1));
#endif


    vfree(netif->tx); /* Frees netif->rx as well. */

    /* Construct the deferred response message. */
    cmsg.type         = CMSG_NETIF_BE;
    cmsg.subtype      = CMSG_NETIF_BE_DISCONNECT;
    cmsg.id           = netif->disconnect_rspid;
    cmsg.length       = sizeof(netif_be_disconnect_t);
    disc.domid        = netif->domid;
    disc.netif_handle = netif->handle;
    disc.status       = NETIF_BE_STATUS_OKAY;
    memcpy(cmsg.msg, &disc, sizeof(disc));

    /*
     * Make sure message is constructed /before/ status change, because
     * after the status change the 'netif' structure could be deallocated at
     * any time. Also make sure we send the response /after/ status change,
     * as otherwise a subsequent CONNECT request could spuriously fail if
     * another CPU doesn't see the status change yet.
     */
    mb();
    if ( netif->status != DISCONNECTING )
        BUG();
    netif->status = DISCONNECTED;
    mb();

    /* Send the successful response. */
    ctrl_if_send_response(&cmsg);
}

void netif_disconnect_complete(netif_t *netif)
{
    INIT_WORK(&netif->work, __netif_disconnect_complete, (void *)netif);
    schedule_work(&netif->work);
}

void netif_create(netif_be_create_t *create)
{
    int                err = 0;
    domid_t            domid  = create->domid;
    unsigned int       handle = create->netif_handle;
    struct net_device *dev;
    netif_t          **pnetif, *netif;
    char               name[IFNAMSIZ] = {};

    snprintf(name, IFNAMSIZ - 1, "vif%u.%u", domid, handle);
    dev = alloc_netdev(sizeof(netif_t), name, ether_setup);
    if ( dev == NULL )
    {
        DPRINTK("Could not create netif: out of memory\n");
        create->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        return;
    }

    netif = netdev_priv(dev);
    memset(netif, 0, sizeof(*netif));
    netif->domid  = domid;
    netif->handle = handle;
    netif->status = DISCONNECTED;
    atomic_set(&netif->refcnt, 0);
    netif->dev = dev;

    netif->credit_bytes = netif->remaining_credit = ~0UL;
    netif->credit_usec  = 0UL;
    init_timer(&netif->credit_timeout);

    pnetif = &netif_hash[NETIF_HASH(domid, handle)];
    while ( *pnetif != NULL )
    {
        if ( ((*pnetif)->domid == domid) && ((*pnetif)->handle == handle) )
        {
            DPRINTK("Could not create netif: already exists\n");
            create->status = NETIF_BE_STATUS_INTERFACE_EXISTS;
            free_netdev(dev);
            return;
        }
        pnetif = &(*pnetif)->hash_next;
    }

    dev->hard_start_xmit = netif_be_start_xmit;
    dev->get_stats       = netif_be_get_stats;
    dev->open            = net_open;
    dev->stop            = net_close;
    dev->features        = NETIF_F_NO_CSUM;

    /* Disable queuing. */
    dev->tx_queue_len = 0;

    if ( (create->be_mac[0] == 0) && (create->be_mac[1] == 0) &&
         (create->be_mac[2] == 0) && (create->be_mac[3] == 0) &&
         (create->be_mac[4] == 0) && (create->be_mac[5] == 0) )
    {
        /*
         * Initialise a dummy MAC address. We choose the numerically largest
         * non-broadcast address to prevent the address getting stolen by an
         * Ethernet bridge for STP purposes. (FE:FF:FF:FF:FF:FF)
         */ 
        memset(dev->dev_addr, 0xFF, ETH_ALEN);
        dev->dev_addr[0] &= ~0x01;
    }
    else
    {
        memcpy(dev->dev_addr, create->be_mac, ETH_ALEN);
    }

    memcpy(netif->fe_dev_addr, create->mac, ETH_ALEN);

    rtnl_lock();
    err = register_netdevice(dev);
    rtnl_unlock();

    if ( err != 0 )
    {
        DPRINTK("Could not register new net device %s: err=%d\n",
                dev->name, err);
        create->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        free_netdev(dev);
        return;
    }

    netif->hash_next = *pnetif;
    *pnetif = netif;

    DPRINTK("Successfully created netif\n");
    create->status = NETIF_BE_STATUS_OKAY;
}

void netif_destroy(netif_be_destroy_t *destroy)
{
    domid_t       domid  = destroy->domid;
    unsigned int  handle = destroy->netif_handle;
    netif_t     **pnetif, *netif;

    pnetif = &netif_hash[NETIF_HASH(domid, handle)];
    while ( (netif = *pnetif) != NULL )
    {
        if ( (netif->domid == domid) && (netif->handle == handle) )
        {
            if ( netif->status != DISCONNECTED )
                goto still_connected;
            goto destroy;
        }
        pnetif = &netif->hash_next;
    }

    destroy->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
    return;

 still_connected:
    destroy->status = NETIF_BE_STATUS_INTERFACE_CONNECTED;
    return;

 destroy:
    *pnetif = netif->hash_next;
    unregister_netdev(netif->dev);
    free_netdev(netif->dev);
    destroy->status = NETIF_BE_STATUS_OKAY;
}

void netif_creditlimit(netif_be_creditlimit_t *creditlimit)
{
    domid_t       domid  = creditlimit->domid;
    unsigned int  handle = creditlimit->netif_handle;
    netif_t      *netif;

    netif = netif_find_by_handle(domid, handle);
    if ( unlikely(netif == NULL) )
    {
        DPRINTK("netif_creditlimit attempted for non-existent netif"
                " (%u,%u)\n", creditlimit->domid, creditlimit->netif_handle); 
        creditlimit->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
        return; 
    }

    /* Set the credit limit (reset remaining credit to new limit). */
    netif->credit_bytes = netif->remaining_credit = creditlimit->credit_bytes;
    netif->credit_usec = creditlimit->period_usec;

    if ( netif->status == CONNECTED )
    {
        /*
         * Schedule work so that any packets waiting under previous credit 
         * limit are dealt with (acts like a replenishment point).
         */
        netif->credit_timeout.expires = jiffies;
        netif_schedule_work(netif);
    }
    
    creditlimit->status = NETIF_BE_STATUS_OKAY;
}

void netif_connect(netif_be_connect_t *connect)
{
    domid_t       domid  = connect->domid;
    unsigned int  handle = connect->netif_handle;
    unsigned int  evtchn = connect->evtchn;
    unsigned long tx_shmem_frame = connect->tx_shmem_frame;
    unsigned long rx_shmem_frame = connect->rx_shmem_frame;
    struct vm_struct *vma;
#if !defined(CONFIG_XEN_NETDEV_GRANT_TX)||!defined(CONFIG_XEN_NETDEV_GRANT_RX)
    pgprot_t      prot = __pgprot(_KERNPG_TABLE);
    int           error;
#endif
    netif_t      *netif;

    netif = netif_find_by_handle(domid, handle);
    if ( unlikely(netif == NULL) ) {
        DPRINTK("netif_connect attempted for non-existent netif (%u,%u)\n", 
                connect->domid, connect->netif_handle); 
        connect->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
        return;
    }

    if ( netif->status != DISCONNECTED ) {
        connect->status = NETIF_BE_STATUS_INTERFACE_CONNECTED;
        return;
    }

    if ( (vma = get_vm_area(2*PAGE_SIZE, VM_IOREMAP)) == NULL ) {
        connect->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        return;
    }


#if defined(CONFIG_XEN_NETDEV_GRANT_TX)
    {
        struct gnttab_map_grant_ref op;
        int tx_ref = connect->tx_shmem_ref; 

        /* Map: Use the Grant table reference */
        op.host_addr = VMALLOC_VMADDR(vma->addr);
        op.flags     = GNTMAP_host_map;
        op.ref       = tx_ref;
        op.dom       = domid;
       
        if ((HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1) < 0) || 
            (op.handle < 0)) { 
            DPRINTK(" Grant table operation failure !\n");
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
            vfree(vma->addr);
            return;
        }

        netif->tx_shmem_ref    = tx_ref;
        netif->tx_shmem_handle = op.handle;
        netif->tx_shmem_vaddr  = VMALLOC_VMADDR(vma->addr);
    }
        

#else 
    error = direct_remap_area_pages(&init_mm, 
                                    VMALLOC_VMADDR(vma->addr),
                                    tx_shmem_frame<<PAGE_SHIFT, PAGE_SIZE,
                                    prot, domid); 
    if ( error != 0 )
    {
        if ( error == -ENOMEM )
            connect->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        else if ( error == -EFAULT )
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
        else
            connect->status = NETIF_BE_STATUS_ERROR;
        vfree(vma->addr);
        return;
    }
#endif


#if defined(CONFIG_XEN_NETDEV_GRANT_RX)
    {
        struct gnttab_map_grant_ref op;
        int rx_ref = connect->rx_shmem_ref; 


        /* Map: Use the Grant table reference */
        op.host_addr = VMALLOC_VMADDR(vma->addr) + PAGE_SIZE;
        op.flags     = GNTMAP_host_map;
        op.ref       = rx_ref;
        op.dom       = domid;

        if ((HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1) < 0) || 
            (op.handle < 0)) { 
            DPRINTK(" Grant table operation failure !\n");
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
            vfree(vma->addr);
            return;
        }

            DPRINTK(" Grant table operation failure !\n");
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
            vfree(vma->addr);
            return;
        }

        netif->rx_shmem_ref    = rx_ref;
        netif->rx_shmem_handle = handle;
        netif->rx_shmem_vaddr  = VMALLOC_VMADDR(vma->addr) + PAGE_SIZE;
    }
#else 
    error = direct_remap_area_pages(&init_mm, 
                                     VMALLOC_VMADDR(vma->addr) + PAGE_SIZE,
                                     rx_shmem_frame<<PAGE_SHIFT, PAGE_SIZE,
                                     prot, domid);
    if ( error != 0 )
    {
        if ( error == -ENOMEM )
            connect->status = NETIF_BE_STATUS_OUT_OF_MEMORY;
        else if ( error == -EFAULT )
            connect->status = NETIF_BE_STATUS_MAPPING_ERROR;
        else
            connect->status = NETIF_BE_STATUS_ERROR;
        vfree(vma->addr);
        return;
    }

#endif

    netif->evtchn         = evtchn;
    netif->tx_shmem_frame = tx_shmem_frame;
    netif->rx_shmem_frame = rx_shmem_frame;
    netif->tx             = 
        (netif_tx_interface_t *)vma->addr;
    netif->rx             = 
        (netif_rx_interface_t *)((char *)vma->addr + PAGE_SIZE);
    netif->tx->resp_prod = netif->rx->resp_prod = 0;
    netif_get(netif);
    wmb(); /* Other CPUs see new state before interface is started. */

    rtnl_lock();
    netif->status = CONNECTED;
    wmb();
    if ( netif_running(netif->dev) )
        __netif_up(netif);
    rtnl_unlock();

    connect->status = NETIF_BE_STATUS_OKAY;
}

int netif_disconnect(netif_be_disconnect_t *disconnect, u8 rsp_id)
{
    domid_t       domid  = disconnect->domid;
    unsigned int  handle = disconnect->netif_handle;
    netif_t      *netif;

    netif = netif_find_by_handle(domid, handle);
    if ( unlikely(netif == NULL) )
    {
        DPRINTK("netif_disconnect attempted for non-existent netif"
                " (%u,%u)\n", disconnect->domid, disconnect->netif_handle); 
        disconnect->status = NETIF_BE_STATUS_INTERFACE_NOT_FOUND;
        return 1; /* Caller will send response error message. */
    }

    if ( netif->status == CONNECTED )
    {
        rtnl_lock();
        netif->status = DISCONNECTING;
        netif->disconnect_rspid = rsp_id;
        wmb();
        if ( netif_running(netif->dev) )
            __netif_down(netif);
        rtnl_unlock();
        netif_put(netif);
        return 0; /* Caller should not send response message. */
    }

    disconnect->status = NETIF_BE_STATUS_OKAY;
    return 1;
}

void netif_interface_init(void)
{
    memset(netif_hash, 0, sizeof(netif_hash));
}
