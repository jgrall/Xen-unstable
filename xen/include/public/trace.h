/******************************************************************************
 * include/public/trace.h
 * 
 * Mark Williamson, (C) 2004 Intel Research Cambridge
 * Copyright (C) 2005 Bin Ren
 */

#ifndef __XEN_PUBLIC_TRACE_H__
#define __XEN_PUBLIC_TRACE_H__

/* Trace classes */
#define TRC_CLS_SHIFT 16
#define TRC_GEN     0x0001f000    /* General trace            */
#define TRC_SCHED   0x0002f000    /* Xen Scheduler trace      */
#define TRC_DOM0OP  0x0004f000    /* Xen DOM0 operation trace */
#define TRC_VMX     0x0008f000    /* Xen VMX trace            */
#define TRC_ALL     0xfffff000

/* Trace subclasses */
#define TRC_SUBCLS_SHIFT 12
/* trace subclasses for VMX */
#define TRC_VMXEXIT  0x00081000   /* VMX exit trace            */
#define TRC_VMXTIMER 0x00082000   /* VMX timer trace           */
#define TRC_VMXINT   0x00084000   /* VMX interrupt trace       */
#define TRC_VMXIO    0x00088000   /* VMX io emulation trace  */


/* Trace events per class */

#define TRC_SCHED_DOM_ADD       (TRC_SCHED +  1)
#define TRC_SCHED_DOM_REM       (TRC_SCHED +  2)
#define TRC_SCHED_SLEEP         (TRC_SCHED +  3)
#define TRC_SCHED_WAKE          (TRC_SCHED +  4)
#define TRC_SCHED_YIELD         (TRC_SCHED +  5)
#define TRC_SCHED_BLOCK         (TRC_SCHED +  6)
#define TRC_SCHED_SHUTDOWN      (TRC_SCHED +  7)
#define TRC_SCHED_CTL           (TRC_SCHED +  8)
#define TRC_SCHED_ADJDOM        (TRC_SCHED +  9)
#define TRC_SCHED_SWITCH        (TRC_SCHED + 10)
#define TRC_SCHED_S_TIMER_FN    (TRC_SCHED + 11)
#define TRC_SCHED_T_TIMER_FN    (TRC_SCHED + 12)
#define TRC_SCHED_DOM_TIMER_FN  (TRC_SCHED + 13)

/* trace events per subclass */
#define TRC_VMX_VMEXIT          (TRC_VMXEXIT + 1)
#define TRC_VMX_VECTOR          (TRC_VMXEXIT + 2)

#define TRC_VMX_TIMER_INTR      (TRC_VMXTIMER + 1)

#define TRC_VMX_INT             (TRC_VMXINT + 1)

/* This structure represents a single trace buffer record. */
struct t_rec {
    u64 cycles;               /* cycle counter timestamp */
    u32 event;                /* event ID                */
    unsigned long data[5];    /* event data items        */
};

/*
 * This structure contains the metadata for a single trace buffer.  The head
 * field, indexes into an array of struct t_rec's.
 */
struct t_buf {
    /* Used by both Xen and user space. */
    atomic_t      rec_idx;   /* the next record to save to */
    unsigned int  rec_num;   /* number of records in this trace buffer  */
    /* Used by Xen only. */
    struct t_rec  *rec;      /* start of records */
    /* Used by user space only. */
    unsigned long rec_addr;  /* machine address of the start of records */
};

#endif /* __XEN_PUBLIC_TRACE_H__ */
