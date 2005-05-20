/******************************************************************************
 * netback/loopback.c
 * 
 * A two-interface loopback device to emulate a local netfront-netback
 * connection. This ensures that local packet delivery looks identical
 * to inter-domain delivery. Most importantly, packets delivered locally
 * originating from other domains will get *copied* when they traverse this
 * driver. This prevents unbounded delays in socket-buffer queues from
 * causing the netback driver to "seize up".
 * 
 * This driver creates a symmetric pair of loopback interfaces with names
 * vif0.0 and veth0. The intention is that 'vif0.0' is bound to an Ethernet
 * bridge, just like a proper netback interface, while a local IP interface
 * is configured on 'veth0'.
 * 
 * As with a real netback interface, vif0.0 is configured with a suitable
 * dummy MAC address. No default is provided for veth0: a reasonable strategy
 * is to transfer eth0's MAC address to veth0, and give eth0 a dummy address
 * (to avoid confusing the Etherbridge).
 * 
 * Copyright (c) 2005 K A Fraser
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/dst.h>

struct net_private {
    struct net_device *loopback_dev;
    struct net_device_stats stats;
};

static int loopback_open(struct net_device *dev)
{
    struct net_private *np = netdev_priv(dev);
    memset(&np->stats, 0, sizeof(np->stats));
    netif_start_queue(dev);
    return 0;
}

static int loopback_close(struct net_device *dev)
{
    netif_stop_queue(dev);
    return 0;
}

static int loopback_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct net_private *np = netdev_priv(dev);

    dst_release(skb->dst);
    skb->dst = NULL;

    skb_orphan(skb);

    np->stats.tx_bytes += skb->len;
    np->stats.tx_packets++;

    /* Switch to loopback context. */
    dev = np->loopback_dev;
    np  = netdev_priv(dev);

    np->stats.rx_bytes += skb->len;
    np->stats.rx_packets++;

    skb->pkt_type = PACKET_HOST; /* overridden by eth_type_trans() */
    skb->protocol = eth_type_trans(skb, dev);
    skb->dev      = dev;
    dev->last_rx  = jiffies;
    netif_rx(skb);

    return 0;
}

static struct net_device_stats *loopback_get_stats(struct net_device *dev)
{
    struct net_private *np = netdev_priv(dev);
    return &np->stats;
}

static void loopback_construct(struct net_device *dev, struct net_device *lo)
{
    struct net_private *np = netdev_priv(dev);

    np->loopback_dev     = lo;

    dev->open            = loopback_open;
    dev->stop            = loopback_close;
    dev->hard_start_xmit = loopback_start_xmit;
    dev->get_stats       = loopback_get_stats;

    dev->tx_queue_len    = 0;

    /*
     * We do not set a jumbo MTU on the interface. Otherwise the network
     * stack will try to send large packets that will get dropped by the
     * Ethernet bridge (unless the physical Ethernet interface is configured
     * to transfer jumbo packets). If a larger MTU is desired then the system
     * administrator can specify it using the 'ifconfig' command.
     */
    /*dev->mtu             = 16*1024;*/
}

static int __init loopback_init(void)
{
    struct net_device *dev1, *dev2;
    int err = -ENOMEM;

    dev1 = alloc_netdev(sizeof(struct net_private), "vif0.0", ether_setup);
    dev2 = alloc_netdev(sizeof(struct net_private), "veth0", ether_setup);
    if ( (dev1 == NULL) || (dev2 == NULL) )
        goto fail;

    loopback_construct(dev1, dev2);
    loopback_construct(dev2, dev1);

    /*
     * Initialise a dummy MAC address for the 'dummy backend' interface. We
     * choose the numerically largest non-broadcast address to prevent the
     * address getting stolen by an Ethernet bridge for STP purposes.
     */
    memset(dev1->dev_addr, 0xFF, ETH_ALEN);
    dev1->dev_addr[0] &= ~0x01;

    if ( (err = register_netdev(dev1)) != 0 )
        goto fail;

    if ( (err = register_netdev(dev2)) != 0 )
    {
        unregister_netdev(dev1);
        goto fail;
    }

    return 0;

 fail:
    if ( dev1 != NULL )
        kfree(dev1);
    if ( dev2 != NULL )
        kfree(dev2);
    return err;
}

module_init(loopback_init);
