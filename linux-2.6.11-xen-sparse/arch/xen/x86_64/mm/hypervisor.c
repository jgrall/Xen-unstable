/******************************************************************************
 * mm/hypervisor.c
 * 
 * Update page tables via the hypervisor.
 * 
 * Copyright (c) 2002-2004, K A Fraser
 * 
 * This file may be distributed separately from the Linux kernel, or
 * incorporated into other software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
/*
 * Jun Nakajima <jun.nakajima@intel.com>
 *   Added hypercalls for x86-64.
 *
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm-xen/hypervisor.h>
#include <asm-xen/balloon.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#include <linux/percpu.h>
#endif

void xen_l1_entry_update(pte_t *ptr, unsigned long val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val;
    BUG_ON(HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_l2_entry_update(pmd_t *ptr, pmd_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pmd;
    BUG_ON(HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_l3_entry_update(pud_t *ptr, pud_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pud;
    BUG_ON(HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_l4_entry_update(pgd_t *ptr, pgd_t val)
{
    mmu_update_t u;
    u.ptr = virt_to_machine(ptr);
    u.val = val.pgd;
    BUG_ON(HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

void xen_pt_switch(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_NEW_BASEPTR;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_new_user_pt(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_NEW_USER_BASEPTR;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_tlb_flush(void)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_TLB_FLUSH_LOCAL;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_invlpg(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_INVLPG_LOCAL;
    op.linear_addr = ptr & PAGE_MASK;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

#ifdef CONFIG_SMP
void xen_tlb_flush_all(void)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_TLB_FLUSH_ALL;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_tlb_flush_mask(cpumask_t *mask)
{
    struct mmuext_op op;
    if ( cpus_empty(*mask) )
        return;
    op.cmd = MMUEXT_TLB_FLUSH_MULTI;
    op.vcpumask = mask->bits;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_invlpg_all(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_INVLPG_ALL;
    op.linear_addr = ptr & PAGE_MASK;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_invlpg_mask(cpumask_t *mask, unsigned long ptr)
{
    struct mmuext_op op;
    if ( cpus_empty(*mask) )
        return;
    op.cmd = MMUEXT_INVLPG_MULTI;
    op.vcpumask = mask->bits;
    op.linear_addr = ptr & PAGE_MASK;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}
#endif

void xen_pgd_pin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_PIN_L4_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pgd_unpin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_UNPIN_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pud_pin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_PIN_L3_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pud_unpin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_UNPIN_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pmd_pin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_PIN_L2_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pmd_unpin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_UNPIN_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_pte_pin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_PIN_L1_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);

}

void xen_pte_unpin(unsigned long ptr)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_UNPIN_TABLE;
    op.mfn = pfn_to_mfn(ptr >> PAGE_SHIFT);
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_set_ldt(unsigned long ptr, unsigned long len)
{
    struct mmuext_op op;
    op.cmd = MMUEXT_SET_LDT;
    op.linear_addr = ptr;
    op.nr_ents = len;
    BUG_ON(HYPERVISOR_mmuext_op(&op, 1, NULL, DOMID_SELF) < 0);
}

void xen_machphys_update(unsigned long mfn, unsigned long pfn)
{
    mmu_update_t u;
    u.ptr = (mfn << PAGE_SHIFT) | MMU_MACHPHYS_UPDATE;
    u.val = pfn;
    BUG_ON(HYPERVISOR_mmu_update(&u, 1, NULL, DOMID_SELF) < 0);
}

#ifdef CONFIG_XEN_PHYSDEV_ACCESS
unsigned long allocate_empty_lowmem_region(unsigned long pages)
{
    pgd_t         *pgd;
    pud_t         *pud; 
    pmd_t         *pmd;
    pte_t         *pte;
    unsigned long *pfn_array;
    unsigned long  vstart;
    unsigned long  i;
    unsigned int   order = get_order(pages*PAGE_SIZE);

    vstart = __get_free_pages(GFP_KERNEL, order);
    if ( vstart == 0 )
        return 0UL;

    scrub_pages(vstart, 1 << order);

    pfn_array = vmalloc((1<<order) * sizeof(*pfn_array));
    if ( pfn_array == NULL )
        BUG();

    for ( i = 0; i < (1<<order); i++ )
    {
        pgd = pgd_offset_k(   (vstart + (i*PAGE_SIZE)));
        pud = pud_offset(pgd, (vstart + (i*PAGE_SIZE)));
        pmd = pmd_offset(pud, (vstart + (i*PAGE_SIZE)));
        pte = pte_offset_kernel(pmd, (vstart + (i*PAGE_SIZE))); 
        pfn_array[i] = pte->pte >> PAGE_SHIFT;
        xen_l1_entry_update(pte, 0);
        phys_to_machine_mapping[(__pa(vstart)>>PAGE_SHIFT)+i] =
            (u32)INVALID_P2M_ENTRY;
    }

    /* Flush updates through and flush the TLB. */
    flush_tlb_all();

    balloon_put_pages(pfn_array, 1 << order);

    vfree(pfn_array);

    return vstart;
}

#endif /* CONFIG_XEN_PHYSDEV_ACCESS */
