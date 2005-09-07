/******************************************************************************
 * drivers/xen/tpmback/common.h
 */

#ifndef __NETIF__BACKEND__COMMON_H__
#define __NETIF__BACKEND__COMMON_H__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <asm-xen/evtchn.h>
#include <asm-xen/xen-public/io/tpmif.h>
#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm-xen/xen-public/io/domain_controller.h>

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

typedef struct tpmif_st {
        struct list_head tpmif_list;
	/* Unique identifier for this interface. */
	domid_t domid;
	unsigned int handle;

	/* Physical parameters of the comms window. */
	unsigned long tx_shmem_frame;
	unsigned int evtchn;
	unsigned int remote_evtchn;

	/* The shared rings and indexes. */
	tpmif_tx_interface_t *tx;

	/* Miscellaneous private stuff. */
	enum { DISCONNECTED, DISCONNECTING, CONNECTED } status;
	int active;

	struct tpmif_st *hash_next;
	struct list_head list;	/* scheduling list */
	atomic_t refcnt;

	long int tpm_instance;
	unsigned long mmap_vstart;

	struct work_struct work;

	u16 shmem_handle;
	unsigned long shmem_vaddr;
	grant_ref_t shmem_ref;

} tpmif_t;

void tpmif_disconnect_complete(tpmif_t * tpmif);
tpmif_t *tpmif_find(domid_t domid, long int instance);
void tpmif_interface_init(void);
void tpmif_schedule_work(tpmif_t * tpmif);
void tpmif_deschedule_work(tpmif_t * tpmif);
void tpmif_xenbus_init(void);
int tpmif_map(tpmif_t *tpmif, unsigned long shared_page, unsigned int evtchn);
irqreturn_t tpmif_be_int(int irq, void *dev_id, struct pt_regs *regs);
int tpmif_vtpm_open(tpmif_t *tpmif, domid_t domain, u32 instance);
int tpmif_vtpm_close(u32 instance);

int vtpm_release_packets(tpmif_t * tpmif, int send_msgs);

#define tpmif_get(_b) (atomic_inc(&(_b)->refcnt))
#define tpmif_put(_b)                             \
    do {                                          \
        if ( atomic_dec_and_test(&(_b)->refcnt) ) \
            tpmif_disconnect_complete(_b);        \
    } while (0)


extern int num_frontends;

#define MMAP_VADDR(t,_req) ((t)->mmap_vstart + ((_req) * PAGE_SIZE))

#endif /* __TPMIF__BACKEND__COMMON_H__ */
