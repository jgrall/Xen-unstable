
#ifndef __BLKIF__BACKEND__COMMON_H__
#define __BLKIF__BACKEND__COMMON_H__

#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/rbtree.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <asm/pgalloc.h>
#include <asm-xen/evtchn.h>
#include <asm-xen/hypervisor.h>
#include <asm-xen/xen-public/io/blkif.h>
#include <asm-xen/xen-public/io/ring.h>
#ifdef CONFIG_XEN_BLKDEV_GRANT
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
typedef struct rb_root rb_root_t;
typedef struct rb_node rb_node_t;
#else
struct block_device;
#endif

typedef struct blkif_st {
    /* Unique identifier for this interface. */
    domid_t           domid;
    unsigned int      handle;
    /* Physical parameters of the comms window. */
    unsigned long     shmem_frame;
    unsigned int      evtchn;
    unsigned int      remote_evtchn;
    /* Comms information. */
    blkif_back_ring_t blk_ring;
    /* VBDs attached to this interface. */
    rb_root_t         vbd_rb;        /* Mapping from 16-bit vdevices to VBDs.*/
    spinlock_t        vbd_lock;      /* Protects VBD mapping. */
    /* Private fields. */
    enum { DISCONNECTED, DISCONNECTING, CONNECTED } status;
    /*
     * DISCONNECT response is deferred until pending requests are ack'ed.
     * We therefore need to store the id from the original request.
     */
    u8               disconnect_rspid;
#ifdef CONFIG_XEN_BLKDEV_TAP_BE
    /* Is this a blktap frontend */
    unsigned int     is_blktap;
#endif
    struct blkif_st *hash_next;
    struct list_head blkdev_list;
    spinlock_t       blk_ring_lock;
    atomic_t         refcnt;

    struct work_struct work;
#ifdef CONFIG_XEN_BLKDEV_GRANT
    u16 shmem_handle;
    unsigned long shmem_vaddr;
    grant_ref_t shmem_ref;
#endif
} blkif_t;

void blkif_create(blkif_be_create_t *create);
void blkif_destroy(blkif_be_destroy_t *destroy);
void blkif_connect(blkif_be_connect_t *connect);
int  blkif_disconnect(blkif_be_disconnect_t *disconnect, u8 rsp_id);
void blkif_disconnect_complete(blkif_t *blkif);
blkif_t *blkif_find(domid_t domid);
void free_blkif(blkif_t *blkif);
int blkif_map(blkif_t *blkif, unsigned long shared_page, unsigned int evtchn);

#define blkif_get(_b) (atomic_inc(&(_b)->refcnt))
#define blkif_put(_b)                             \
    do {                                          \
        if ( atomic_dec_and_test(&(_b)->refcnt) ) \
            free_blkif(_b);			  \
    } while (0)

struct vbd;
void vbd_free(blkif_t *blkif, struct vbd *vbd);

/* Creates inactive vbd. */
struct vbd *vbd_create(blkif_t *blkif, blkif_vdev_t vdevice, blkif_pdev_t pdevice, int readonly);
int vbd_is_active(struct vbd *vbd);
void vbd_activate(blkif_t *blkif, struct vbd *vbd);

unsigned long vbd_size(struct vbd *vbd);
unsigned int vbd_info(struct vbd *vbd);
unsigned long vbd_secsize(struct vbd *vbd);
void vbd_destroy(blkif_be_vbd_destroy_t *delete); 
void destroy_all_vbds(blkif_t *blkif);

struct phys_req {
    unsigned short       dev;
    unsigned short       nr_sects;
    struct block_device *bdev;
    blkif_sector_t       sector_number;
};

int vbd_translate(struct phys_req *req, blkif_t *blkif, int operation); 

void blkif_interface_init(void);

void blkif_deschedule(blkif_t *blkif);

void blkif_xenbus_init(void);

irqreturn_t blkif_be_int(int irq, void *dev_id, struct pt_regs *regs);

#endif /* __BLKIF__BACKEND__COMMON_H__ */
