/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright (C) IBM Corp. 2005
 *
 * Authors: Hollis Blanchard <hollisb@us.ibm.com>
 */

#ifndef _ASM_DOMAIN_H_
#define _ASM_DOMAIN_H_

#include <xen/cache.h>
#include <xen/sched.h>
#include <xen/list.h>
#include <xen/errno.h>
#include <xen/mm.h>
#include <public/arch-powerpc.h>
#include <asm/htab.h>
#include <asm/powerpc64/ppc970.h>

struct arch_domain {
    struct domain_htab htab;

    /* The Real Mode area is fixed to the domain and is accessible while the
     * processor is in real mode */
    struct page_info *rma_page;
    uint rma_order;

    /* I/O-port access bitmap mask. */
    u8 *iobmp_mask;       /* Address of IO bitmap mask, or NULL.      */

    uint large_page_sizes;
    uint large_page_order[4];
} __cacheline_aligned;

struct slb_entry {
    ulong slb_vsid;
    ulong slb_esid;
};

struct xencomm;

typedef struct {
    u32 u[4];
} __attribute__((aligned(16))) vector128;

struct arch_vcpu {
    cpu_user_regs_t ctxt; /* User-level CPU registers */

#ifdef HAS_FLOAT
    double fprs[NUM_FPRS];
#endif
#ifdef HAS_VMX
    vector128 vrs[32];
    vector128 vscr;
    u32 vrsave;
#endif

    /* Special-Purpose Registers */
    ulong sprg[4];
    ulong timebase;
    ulong dar;
    ulong dsisr;
    
    /* Segment Lookaside Buffer */
    struct slb_entry slb_entries[NUM_SLB_ENTRIES];

    /* I/O-port access bitmap. */
    u8 *iobmp;        /* Guest kernel virtual address of the bitmap. */
    int iobmp_limit;  /* Number of ports represented in the bitmap.  */
    int iopl;         /* Current IOPL for this VCPU. */

    u32 dec;
    struct cpu_vcpu cpu; /* CPU-specific bits */
    struct xencomm *xencomm;
} __cacheline_aligned;

extern void full_resume(void);

extern void save_sprs(struct vcpu *);
extern void load_sprs(struct vcpu *);
extern void save_segments(struct vcpu *);
extern void load_segments(struct vcpu *);
extern void save_float(struct vcpu *);
extern void load_float(struct vcpu *);

#define RMA_SHARED_INFO 1
#define RMA_START_INFO 2
#define RMA_LAST_DOM0 2
/* these are not used for dom0 so they should be last */
#define RMA_CONSOLE 3
#define RMA_LAST_DOMU 3

#define rma_size(rma_order) (1UL << ((rma_order) + PAGE_SHIFT))

static inline ulong rma_addr(struct arch_domain *ad, int type)
{
    return rma_size(ad->rma_order) - (type * PAGE_SIZE);
}

#endif
