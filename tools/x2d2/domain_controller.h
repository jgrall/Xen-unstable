/******************************************************************************
 * domain_controller.h
 * 
 * Interface to server controller (e.g., 'xend'). This header file defines the 
 * interface that is shared with guest OSes.
 * 
 * Copyright (c) 2004, K A Fraser
 */

/* I've copied this from the xen source pool as getting Xen and
   userspace headers to play nicely together is beyond me -- sos22 */
#define PACKED __attribute__ ((packed))
typedef unsigned long memory_t;   /* Full-sized pointer/address/memory-size. */
#define __MEMORY_PADDING(_X) u32 __pad_ ## _X
#define _MEMORY_PADDING(_X)  __MEMORY_PADDING(_X)
#define MEMORY_PADDING       _MEMORY_PADDING(__LINE__)
typedef u16 domid_t;

/*
 * Reason codes for SCHEDOP_shutdown. These are opaque to Xen but may be
 * interpreted by control software to determine the appropriate action. These 
 * are only really advisories: the controller can actually do as it likes.
 */
#define SHUTDOWN_poweroff   0  /* Domain exited normally. Clean up and kill. */
#define SHUTDOWN_reboot     1  /* Clean up, kill, and then restart.          */
#define SHUTDOWN_suspend    2  /* Clean up, save suspend info, kill.         */


/*
 * CONTROLLER MESSAGING INTERFACE.
 */

typedef struct {
    u8 type;     /*  0: echoed in response */
    u8 subtype;  /*  1: echoed in response */
    u8 id;       /*  2: echoed in response */
    u8 length;   /*  3: number of bytes in 'msg' */
    u8 msg[60];  /*  4: type-specific message data */
} PACKED control_msg_t; /* 64 bytes */

#define CONTROL_RING_SIZE 8
typedef u32 CONTROL_RING_IDX;
#define MASK_CONTROL_IDX(_i) ((_i)&(CONTROL_RING_SIZE-1))

typedef struct {
    control_msg_t tx_ring[CONTROL_RING_SIZE];   /*    0: guest -> controller */
    control_msg_t rx_ring[CONTROL_RING_SIZE];   /*  512: controller -> guest */
    CONTROL_RING_IDX tx_req_prod, tx_resp_prod; /* 1024, 1028 */
    CONTROL_RING_IDX rx_req_prod, rx_resp_prod; /* 1032, 1036 */
} PACKED control_if_t; /* 1040 bytes */

/*
 * Top-level command types.
 */
#define CMSG_CONSOLE        0  /* Console                 */
#define CMSG_BLKIF_BE       1  /* Block-device backend    */
#define CMSG_BLKIF_FE       2  /* Block-device frontend   */
#define CMSG_NETIF_BE       3  /* Network-device backend  */
#define CMSG_NETIF_FE       4  /* Network-device frontend */
#define CMSG_SHUTDOWN       6  /* Shutdown messages       */
#define CMSG_PDB_BE         7  /* PDB backend.            */
#define CMSG_PDB_FE         8  /* PDB frontend.           */

/******************************************************************************
 * CONSOLE DEFINITIONS
 */

/*
 * Subtypes for console messages.
 */
#define CMSG_CONSOLE_DATA       0


/******************************************************************************
 * BLOCK-INTERFACE FRONTEND DEFINITIONS
 */

/* Messages from domain controller to guest. */
#define CMSG_BLKIF_FE_INTERFACE_STATUS_CHANGED   0

/* Messages from guest to domain controller. */
#define CMSG_BLKIF_FE_DRIVER_STATUS_CHANGED     32
#define CMSG_BLKIF_FE_INTERFACE_CONNECT         33
#define CMSG_BLKIF_FE_INTERFACE_DISCONNECT      34

/* These are used by both front-end and back-end drivers. */
#define blkif_vdev_t   u16
#define blkif_pdev_t   u16
#define blkif_sector_t u64

/*
 * CMSG_BLKIF_FE_INTERFACE_STATUS_CHANGED:
 *  Notify a guest about a status change on one of its block interfaces.
 *  If the interface is DESTROYED or DOWN then the interface is disconnected:
 *   1. The shared-memory frame is available for reuse.
 *   2. Any unacknowledged messgaes pending on the interface were dropped.
 */
#define BLKIF_INTERFACE_STATUS_DESTROYED    0 /* Interface doesn't exist.    */
#define BLKIF_INTERFACE_STATUS_DISCONNECTED 1 /* Exists but is disconnected. */
#define BLKIF_INTERFACE_STATUS_CONNECTED    2 /* Exists and is connected.    */
typedef struct {
    u32 handle; /*  0 */
    u32 status; /*  4 */
    u16 evtchn; /*  8: (only if status == BLKIF_INTERFACE_STATUS_CONNECTED). */
} PACKED blkif_fe_interface_status_changed_t; /* 10 bytes */

/*
 * CMSG_BLKIF_FE_DRIVER_STATUS_CHANGED:
 *  Notify the domain controller that the front-end driver is DOWN or UP.
 *  When the driver goes DOWN then the controller will send no more
 *  status-change notifications. When the driver comes UP then the controller
 *  will send a notification for each interface that currently exists.
 *  If the driver goes DOWN while interfaces are still UP, the domain
 *  will automatically take the interfaces DOWN.
 */
#define BLKIF_DRIVER_STATUS_DOWN   0
#define BLKIF_DRIVER_STATUS_UP     1
typedef struct {
    /* IN */
    u32 status;        /*  0: BLKIF_DRIVER_STATUS_??? */
    /* OUT */
    /*
     * Tells driver how many interfaces it should expect to immediately
     * receive notifications about.
     */
    u32 nr_interfaces; /*  4 */
} PACKED blkif_fe_driver_status_changed_t; /* 8 bytes */

/*
 * CMSG_BLKIF_FE_INTERFACE_CONNECT:
 *  If successful, the domain controller will acknowledge with a
 *  STATUS_CONNECTED message.
 */
typedef struct {
    u32      handle;      /*  0 */
    u32      __pad;
    memory_t shmem_frame; /*  8 */
    MEMORY_PADDING;
} PACKED blkif_fe_interface_connect_t; /* 16 bytes */

/*
 * CMSG_BLKIF_FE_INTERFACE_DISCONNECT:
 *  If successful, the domain controller will acknowledge with a
 *  STATUS_DISCONNECTED message.
 */
typedef struct {
    u32 handle; /*  0 */
} PACKED blkif_fe_interface_disconnect_t; /* 4 bytes */


/******************************************************************************
 * BLOCK-INTERFACE BACKEND DEFINITIONS
 */

/* Messages from domain controller. */
#define CMSG_BLKIF_BE_CREATE      0  /* Create a new block-device interface. */
#define CMSG_BLKIF_BE_DESTROY     1  /* Destroy a block-device interface.    */
#define CMSG_BLKIF_BE_CONNECT     2  /* Connect i/f to remote driver.        */
#define CMSG_BLKIF_BE_DISCONNECT  3  /* Disconnect i/f from remote driver.   */
#define CMSG_BLKIF_BE_VBD_CREATE  4  /* Create a new VBD for an interface.   */
#define CMSG_BLKIF_BE_VBD_DESTROY 5  /* Delete a VBD from an interface.      */
#define CMSG_BLKIF_BE_VBD_GROW    6  /* Append an extent to a given VBD.     */
#define CMSG_BLKIF_BE_VBD_SHRINK  7  /* Remove last extent from a given VBD. */

/* Messages to domain controller. */
#define CMSG_BLKIF_BE_DRIVER_STATUS_CHANGED 32

/*
 * Message request/response definitions for block-device messages.
 */

typedef struct {
    blkif_sector_t sector_start;   /*  0 */
    blkif_sector_t sector_length;  /*  8 */
    blkif_pdev_t   device;         /* 16 */
    u16            __pad;          /* 18 */
} PACKED blkif_extent_t; /* 20 bytes */

/* Non-specific 'okay' return. */
#define BLKIF_BE_STATUS_OKAY                0
/* Non-specific 'error' return. */
#define BLKIF_BE_STATUS_ERROR               1
/* The following are specific error returns. */
#define BLKIF_BE_STATUS_INTERFACE_EXISTS    2
#define BLKIF_BE_STATUS_INTERFACE_NOT_FOUND 3
#define BLKIF_BE_STATUS_INTERFACE_CONNECTED 4
#define BLKIF_BE_STATUS_VBD_EXISTS          5
#define BLKIF_BE_STATUS_VBD_NOT_FOUND       6
#define BLKIF_BE_STATUS_OUT_OF_MEMORY       7
#define BLKIF_BE_STATUS_EXTENT_NOT_FOUND    8
#define BLKIF_BE_STATUS_MAPPING_ERROR       9

/* This macro can be used to create an array of descriptive error strings. */
#define BLKIF_BE_STATUS_ERRORS {    \
    "Okay",                         \
    "Non-specific error",           \
    "Interface already exists",     \
    "Interface not found",          \
    "Interface is still connected", \
    "VBD already exists",           \
    "VBD not found",                \
    "Out of memory",                \
    "Extent not found for VBD",     \
    "Could not map domain memory" }

/*
 * CMSG_BLKIF_BE_CREATE:
 *  When the driver sends a successful response then the interface is fully
 *  created. The controller will send a DOWN notification to the front-end
 *  driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Domain attached to new interface.   */
    u16        __pad;
    u32        blkif_handle;  /*  4: Domain-specific interface handle.   */
    /* OUT */
    u32        status;        /*  8 */
} PACKED blkif_be_create_t; /* 12 bytes */

/*
 * CMSG_BLKIF_BE_DESTROY:
 *  When the driver sends a successful response then the interface is fully
 *  torn down. The controller will send a DESTROYED notification to the
 *  front-end driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Identify interface to be destroyed. */
    u16        __pad;
    u32        blkif_handle;  /*  4: ...ditto...                         */
    /* OUT */
    u32        status;        /*  8 */
} PACKED blkif_be_destroy_t; /* 12 bytes */

/*
 * CMSG_BLKIF_BE_CONNECT:
 *  When the driver sends a successful response then the interface is fully
 *  connected. The controller will send a CONNECTED notification to the
 *  front-end driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Domain attached to new interface.   */
    u16        __pad;
    u32        blkif_handle;  /*  4: Domain-specific interface handle.   */
    memory_t   shmem_frame;   /*  8: Page cont. shared comms window.     */
    MEMORY_PADDING;
    u32        evtchn;        /* 16: Event channel for notifications.    */
    /* OUT */
    u32        status;        /* 20 */
} PACKED blkif_be_connect_t;  /* 24 bytes */

/*
 * CMSG_BLKIF_BE_DISCONNECT:
 *  When the driver sends a successful response then the interface is fully
 *  disconnected. The controller will send a DOWN notification to the front-end
 *  driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Domain attached to new interface.   */
    u16        __pad;
    u32        blkif_handle;  /*  4: Domain-specific interface handle.   */
    /* OUT */
    u32        status;        /*  8 */
} PACKED blkif_be_disconnect_t; /* 12 bytes */

/* CMSG_BLKIF_BE_VBD_CREATE */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Identify blkdev interface.          */
    u16        __pad;
    u32        blkif_handle;  /*  4: ...ditto...                         */
    blkif_vdev_t vdevice;     /*  8: Interface-specific id for this VBD. */
    u16        readonly;      /* 10: Non-zero -> VBD isn't writable.     */
    /* OUT */
    u32        status;        /* 12 */
} PACKED blkif_be_vbd_create_t; /* 16 bytes */

/* CMSG_BLKIF_BE_VBD_DESTROY */
typedef struct {
    /* IN */
    domid_t    domid;         /*  0: Identify blkdev interface.          */
    u16        __pad0;        /*  2 */
    u32        blkif_handle;  /*  4: ...ditto...                         */
    blkif_vdev_t vdevice;     /*  8: Interface-specific id of the VBD.   */
    u16        __pad1;        /* 10 */
    /* OUT */
    u32        status;        /* 12 */
} PACKED blkif_be_vbd_destroy_t; /* 16 bytes */

/* CMSG_BLKIF_BE_VBD_GROW */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Identify blkdev interface.          */
    u16        __pad0;        /*  2 */
    u32        blkif_handle;  /*  4: ...ditto...                         */
    blkif_extent_t extent;    /*  8: Physical extent to append to VBD.   */
    blkif_vdev_t vdevice;     /* 28: Interface-specific id of the VBD.   */
    u16        __pad1;        /* 30 */
    /* OUT */
    u32        status;        /* 32 */
} PACKED blkif_be_vbd_grow_t; /* 36 bytes */

/* CMSG_BLKIF_BE_VBD_SHRINK */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Identify blkdev interface.          */
    u16        __pad0;        /*  2 */
    u32        blkif_handle;  /*  4: ...ditto...                         */
    blkif_vdev_t vdevice;     /*  8: Interface-specific id of the VBD.   */
    u16        __pad1;        /* 10 */
    /* OUT */
    u32        status;        /* 12 */
} PACKED blkif_be_vbd_shrink_t; /* 16 bytes */

/*
 * CMSG_BLKIF_BE_DRIVER_STATUS_CHANGED:
 *  Notify the domain controller that the back-end driver is DOWN or UP.
 *  If the driver goes DOWN while interfaces are still UP, the controller
 *  will automatically send DOWN notifications.
 */
typedef struct {
    u32        status;        /*  0: BLKIF_DRIVER_STATUS_??? */
} PACKED blkif_be_driver_status_changed_t; /* 4 bytes */


/******************************************************************************
 * NETWORK-INTERFACE FRONTEND DEFINITIONS
 */

/* Messages from domain controller to guest. */
#define CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED   0

/* Messages from guest to domain controller. */
#define CMSG_NETIF_FE_DRIVER_STATUS_CHANGED     32
#define CMSG_NETIF_FE_INTERFACE_CONNECT         33
#define CMSG_NETIF_FE_INTERFACE_DISCONNECT      34

/*
 * CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED:
 *  Notify a guest about a status change on one of its network interfaces.
 *  If the interface is DESTROYED or DOWN then the interface is disconnected:
 *   1. The shared-memory frame is available for reuse.
 *   2. Any unacknowledged messgaes pending on the interface were dropped.
 */
#define NETIF_INTERFACE_STATUS_DESTROYED    0 /* Interface doesn't exist.    */
#define NETIF_INTERFACE_STATUS_DISCONNECTED 1 /* Exists but is disconnected. */
#define NETIF_INTERFACE_STATUS_CONNECTED    2 /* Exists and is connected.    */
typedef struct {
    u32        handle; /*  0 */
    u32        status; /*  4 */
    u16        evtchn; /*  8: status == NETIF_INTERFACE_STATUS_CONNECTED */
    u8         mac[6]; /* 10: status == NETIF_INTERFACE_STATUS_CONNECTED */
} PACKED netif_fe_interface_status_changed_t; /* 16 bytes */

/*
 * CMSG_NETIF_FE_DRIVER_STATUS_CHANGED:
 *  Notify the domain controller that the front-end driver is DOWN or UP.
 *  When the driver goes DOWN then the controller will send no more
 *  status-change notifications. When the driver comes UP then the controller
 *  will send a notification for each interface that currently exists.
 *  If the driver goes DOWN while interfaces are still UP, the domain
 *  will automatically take the interfaces DOWN.
 */
#define NETIF_DRIVER_STATUS_DOWN   0
#define NETIF_DRIVER_STATUS_UP     1
typedef struct {
    /* IN */
    u32        status;        /*  0: NETIF_DRIVER_STATUS_??? */
    /* OUT */
    /*
     * Tells driver how many interfaces it should expect to immediately
     * receive notifications about.
     */
    u32        nr_interfaces; /*  4 */
} PACKED netif_fe_driver_status_changed_t; /* 8 bytes */

/*
 * CMSG_NETIF_FE_INTERFACE_CONNECT:
 *  If successful, the domain controller will acknowledge with a
 *  STATUS_CONNECTED message.
 */
typedef struct {
    u32        handle;         /*  0 */
    u32        __pad;          /*  4 */
    memory_t   tx_shmem_frame; /*  8 */
    MEMORY_PADDING;
    memory_t   rx_shmem_frame; /* 16 */
    MEMORY_PADDING;
} PACKED netif_fe_interface_connect_t; /* 24 bytes */

/*
 * CMSG_NETIF_FE_INTERFACE_DISCONNECT:
 *  If successful, the domain controller will acknowledge with a
 *  STATUS_DISCONNECTED message.
 */
typedef struct {
    u32        handle;        /*  0 */
} PACKED netif_fe_interface_disconnect_t; /* 4 bytes */


/******************************************************************************
 * NETWORK-INTERFACE BACKEND DEFINITIONS
 */

/* Messages from domain controller. */
#define CMSG_NETIF_BE_CREATE      0  /* Create a new net-device interface. */
#define CMSG_NETIF_BE_DESTROY     1  /* Destroy a net-device interface.    */
#define CMSG_NETIF_BE_CONNECT     2  /* Connect i/f to remote driver.        */
#define CMSG_NETIF_BE_DISCONNECT  3  /* Disconnect i/f from remote driver.   */

/* Messages to domain controller. */
#define CMSG_NETIF_BE_DRIVER_STATUS_CHANGED 32

/*
 * Message request/response definitions for net-device messages.
 */

/* Non-specific 'okay' return. */
#define NETIF_BE_STATUS_OKAY                0
/* Non-specific 'error' return. */
#define NETIF_BE_STATUS_ERROR               1
/* The following are specific error returns. */
#define NETIF_BE_STATUS_INTERFACE_EXISTS    2
#define NETIF_BE_STATUS_INTERFACE_NOT_FOUND 3
#define NETIF_BE_STATUS_INTERFACE_CONNECTED 4
#define NETIF_BE_STATUS_OUT_OF_MEMORY       5
#define NETIF_BE_STATUS_MAPPING_ERROR       6

/* This macro can be used to create an array of descriptive error strings. */
#define NETIF_BE_STATUS_ERRORS {    \
    "Okay",                         \
    "Non-specific error",           \
    "Interface already exists",     \
    "Interface not found",          \
    "Interface is still connected", \
    "Out of memory",                \
    "Could not map domain memory" }

/*
 * CMSG_NETIF_BE_CREATE:
 *  When the driver sends a successful response then the interface is fully
 *  created. The controller will send a DOWN notification to the front-end
 *  driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Domain attached to new interface.   */
    u16        __pad0;        /*  2 */
    u32        netif_handle;  /*  4: Domain-specific interface handle.   */
    u8         mac[6];        /*  8 */
    u16        __pad1;        /* 14 */
    /* OUT */
    u32        status;        /* 16 */
} PACKED netif_be_create_t; /* 20 bytes */

/*
 * CMSG_NETIF_BE_DESTROY:
 *  When the driver sends a successful response then the interface is fully
 *  torn down. The controller will send a DESTROYED notification to the
 *  front-end driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Identify interface to be destroyed. */
    u16        __pad;
    u32        netif_handle;  /*  4: ...ditto...                         */
    /* OUT */
    u32   status;             /*  8 */
} PACKED netif_be_destroy_t; /* 12 bytes */

/*
 * CMSG_NETIF_BE_CONNECT:
 *  When the driver sends a successful response then the interface is fully
 *  connected. The controller will send a CONNECTED notification to the
 *  front-end driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;          /*  0: Domain attached to new interface.   */
    u16        __pad0;         /*  2 */
    u32        netif_handle;   /*  4: Domain-specific interface handle.   */
    memory_t   tx_shmem_frame; /*  8: Page cont. tx shared comms window.  */
    MEMORY_PADDING;
    memory_t   rx_shmem_frame; /* 16: Page cont. rx shared comms window.  */
    MEMORY_PADDING;
    u16        evtchn;         /* 24: Event channel for notifications.    */
    u16        __pad1;         /* 26 */
    /* OUT */
    u32        status;         /* 28 */
} PACKED netif_be_connect_t; /* 32 bytes */

/*
 * CMSG_NETIF_BE_DISCONNECT:
 *  When the driver sends a successful response then the interface is fully
 *  disconnected. The controller will send a DOWN notification to the front-end
 *  driver.
 */
typedef struct { 
    /* IN */
    domid_t    domid;         /*  0: Domain attached to new interface.   */
    u16        __pad;
    u32        netif_handle;  /*  4: Domain-specific interface handle.   */
    /* OUT */
    u32        status;        /*  8 */
} PACKED netif_be_disconnect_t; /* 12 bytes */

/*
 * CMSG_NETIF_BE_DRIVER_STATUS_CHANGED:
 *  Notify the domain controller that the back-end driver is DOWN or UP.
 *  If the driver goes DOWN while interfaces are still UP, the domain
 *  will automatically send DOWN notifications.
 */
typedef struct {
    u32        status;        /*  0: NETIF_DRIVER_STATUS_??? */
} PACKED netif_be_driver_status_changed_t; /* 4 bytes */


/******************************************************************************
 * SHUTDOWN DEFINITIONS
 */

/*
 * Subtypes for shutdown messages.
 */
#define CMSG_SHUTDOWN_POWEROFF  0   /* Clean shutdown (SHUTDOWN_poweroff).   */
#define CMSG_SHUTDOWN_REBOOT    1   /* Clean shutdown (SHUTDOWN_reboot).     */
#define CMSG_SHUTDOWN_SUSPEND   2   /* Create suspend info, then             */
                                    /* SHUTDOWN_suspend.                     */

/******************************************************************************
 * PDB DEFINITIONS
 */

/* Notify the backend that a new frontend has connected. */
#define CMSG_PDB_BE_INTERFACE_CONNECTED 0
typedef struct {
    int assist_port;
    int event_port;
} pdb_be_connected_t;

/* Notify the domain controller that the status of the backend has
   changed. */
#define CMSG_PDB_BE_DRIVER_STATUS_CHANGED 1
typedef struct {
    unsigned status;
#define PDB_DRIVER_STATUS_UP 1
    unsigned long event_page;
    unsigned long assist_page;
} pdb_be_driver_status_changed_t;

/* Notify a front end that a back end just popped up. */
#define CMSG_PDB_FE_NEW_BE 0
typedef struct {
    int domain;
    int assist_evtchn;
    int event_evtchn;
    unsigned long assist_frame;
    unsigned long event_frame;
} pdb_fe_new_be_t;
