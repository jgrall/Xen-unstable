/******************************************************************************
 * arch/xen/drivers/netif/backend/common.h
 */

#ifndef __NETIF__BACKEND__COMMON_H__
#define __NETIF__BACKEND__COMMON_H__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <asm-xen/ctrl_if.h>
#include <asm-xen/evtchn.h>
#include <asm-xen/xen-public/io/netif.h>
#include <asm/io.h>
#include <asm/pgalloc.h>

#if defined(CONFIG_XEN_NETDEV_GRANT_TX) || defined(CONFIG_XEN_NETDEV_GRANT_RX)
#include <asm-xen/xen-public/grant_table.h>
#include <asm-xen/gnttab.h>
#endif



#if 0
#define ASSERT(_p) \
    if ( !(_p) ) { printk("Assertion '%s' failed, line %d, file %s", #_p , \
    __LINE__, __FILE__); *(int*)0=0; }
#define DPRINTK(_f, _a...) printk(KERN_ALERT "(file=%s, line=%d) " _f, \
                           __FILE__ , __LINE__ , ## _a )
#else
#define ASSERT(_p) ((void)0)
#define DPRINTK(_f, _a...) ((void)0)
#endif

typedef struct netif_st {
    /* Unique identifier for this interface. */
    domid_t          domid;
    unsigned int     handle;

    u8               fe_dev_addr[6];

    /* Physical parameters of the comms window. */
    unsigned long    tx_shmem_frame;
#ifdef CONFIG_XEN_NETDEV_GRANT_TX
    u16              tx_shmem_handle;
    unsigned long    tx_shmem_vaddr; 
    grant_ref_t      tx_shmem_ref; 
#endif
    unsigned long    rx_shmem_frame;
#ifdef CONFIG_XEN_NETDEV_GRANT_RX
    u16              rx_shmem_handle;
    unsigned long    rx_shmem_vaddr; 
    grant_ref_t      rx_shmem_ref; 
#endif
    unsigned int     evtchn;
    unsigned int     remote_evtchn;

    /* The shared rings and indexes. */
    netif_tx_interface_t *tx;
    netif_rx_interface_t *rx;

    /* Private indexes into shared ring. */
    NETIF_RING_IDX rx_req_cons;
    NETIF_RING_IDX rx_resp_prod; /* private version of shared variable */
#ifdef CONFIG_XEN_NETDEV_GRANT_RX
    NETIF_RING_IDX rx_resp_prod_copy; /* private version of shared variable */
#endif
    NETIF_RING_IDX tx_req_cons;
    NETIF_RING_IDX tx_resp_prod; /* private version of shared variable */

    /* Transmit shaping: allow 'credit_bytes' every 'credit_usec'. */
    unsigned long   credit_bytes;
    unsigned long   credit_usec;
    unsigned long   remaining_credit;
    struct timer_list credit_timeout;

    /* Miscellaneous private stuff. */
    enum { DISCONNECTED, DISCONNECTING, CONNECTED } status;
    int active;
    struct list_head list;  /* scheduling list */
    atomic_t         refcnt;
    struct net_device *dev;
    struct net_device_stats stats;

    struct work_struct free_work;
} netif_t;

void netif_creditlimit(netif_t *netif);
int  netif_disconnect(netif_t *netif);

netif_t *alloc_netif(domid_t domid, unsigned int handle, u8 be_mac[ETH_ALEN]);
void free_netif_callback(netif_t *netif);
int netif_map(netif_t *netif, unsigned long tx_ring_ref,
	      unsigned long rx_ring_ref, unsigned int evtchn);

#define netif_get(_b) (atomic_inc(&(_b)->refcnt))
#define netif_put(_b)                             \
    do {                                          \
        if ( atomic_dec_and_test(&(_b)->refcnt) ) \
            free_netif_callback(_b);              \
    } while (0)

void netif_xenbus_init(void);

void netif_schedule_work(netif_t *netif);
void netif_deschedule_work(netif_t *netif);

int netif_be_start_xmit(struct sk_buff *skb, struct net_device *dev);
struct net_device_stats *netif_be_get_stats(struct net_device *dev);
irqreturn_t netif_be_int(int irq, void *dev_id, struct pt_regs *regs);

#endif /* __NETIF__BACKEND__COMMON_H__ */
