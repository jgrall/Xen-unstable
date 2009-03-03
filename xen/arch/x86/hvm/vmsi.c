/*
 *  Copyright (C) 2001  MandrakeSoft S.A.
 *
 *    MandrakeSoft S.A.
 *    43, rue d'Aboukir
 *    75002 Paris - France
 *    http://www.linux-mandrake.com/
 *    http://www.mandrakesoft.com/
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 * Support for virtual MSI logic
 * Will be merged it with virtual IOAPIC logic, since most is the same
*/

#include <xen/config.h>
#include <xen/types.h>
#include <xen/mm.h>
#include <xen/xmalloc.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/sched.h>
#include <public/hvm/ioreq.h>
#include <asm/hvm/io.h>
#include <asm/hvm/vpic.h>
#include <asm/hvm/vlapic.h>
#include <asm/hvm/support.h>
#include <asm/current.h>
#include <asm/event.h>

static uint32_t vmsi_get_delivery_bitmask(
    struct domain *d, uint16_t dest, uint8_t dest_mode)
{
    uint32_t mask = 0;
    struct vcpu *v;

    HVM_DBG_LOG(DBG_LEVEL_IOAPIC, "ioapic_get_delivery_bitmask "
                "dest %d dest_mode %d\n", dest, dest_mode);

    if ( dest_mode == 0 ) /* Physical mode. */
    {
        if ( dest == 0xFF ) /* Broadcast. */
        {
            for_each_vcpu ( d, v )
                mask |= 1 << v->vcpu_id;
            goto out;
        }

        for_each_vcpu ( d, v )
        {
            if ( VLAPIC_ID(vcpu_vlapic(v)) == dest )
            {
                mask = 1 << v->vcpu_id;
                break;
            }
        }
    }
    else if ( dest != 0 ) /* Logical mode, MDA non-zero. */
    {
        for_each_vcpu ( d, v )
            if ( vlapic_match_logical_addr(vcpu_vlapic(v), dest) )
                mask |= 1 << v->vcpu_id;
    }

 out:
    HVM_DBG_LOG(DBG_LEVEL_IOAPIC, "ioapic_get_delivery_bitmask mask %x\n",
                mask);
    return mask;
}

static void vmsi_inj_irq(
    struct domain *d,
    struct vlapic *target,
    uint8_t vector,
    uint8_t trig_mode,
    uint8_t delivery_mode)
{
    HVM_DBG_LOG(DBG_LEVEL_IOAPIC, "ioapic_inj_irq "
                "irq %d trig %d delive mode %d\n",
                vector, trig_mode, delivery_mode);

    switch ( delivery_mode )
    {
    case dest_Fixed:
    case dest_LowestPrio:
        if ( vlapic_set_irq(target, vector, trig_mode) )
            vcpu_kick(vlapic_vcpu(target));
        break;
    default:
        gdprintk(XENLOG_WARNING, "error delivery mode %d\n", delivery_mode);
        break;
    }
}

#define VMSI_DEST_ID_MASK 0xff
#define VMSI_RH_MASK      0x100
#define VMSI_DM_MASK      0x200
#define VMSI_DELIV_MASK   0x7000
#define VMSI_TRIG_MODE    0x8000

#define GFLAGS_SHIFT_DEST_ID        0
#define GFLAGS_SHIFT_RH             8
#define GFLAGS_SHIFT_DM             9
#define GLFAGS_SHIFT_DELIV_MODE     12
#define GLFAGS_SHIFT_TRG_MODE       15

int vmsi_deliver(struct domain *d, int pirq)
{
    struct hvm_irq_dpci *hvm_irq_dpci = d->arch.hvm_domain.irq.dpci;
    uint32_t flags = hvm_irq_dpci->mirq[pirq].gmsi.gflags;
    int vector = hvm_irq_dpci->mirq[pirq].gmsi.gvec;
    uint16_t dest = (flags & VMSI_DEST_ID_MASK) >> GFLAGS_SHIFT_DEST_ID;
    uint8_t dest_mode = (flags & VMSI_DM_MASK) >> GFLAGS_SHIFT_DM;
    uint8_t delivery_mode = (flags & VMSI_DELIV_MASK) >> GLFAGS_SHIFT_DELIV_MODE;
    uint8_t trig_mode = (flags & VMSI_TRIG_MODE) >> GLFAGS_SHIFT_TRG_MODE;
    uint32_t deliver_bitmask;
    struct vlapic *target;
    struct vcpu *v;

    HVM_DBG_LOG(DBG_LEVEL_IOAPIC,
                "msi: dest=%x dest_mode=%x delivery_mode=%x "
                "vector=%x trig_mode=%x\n",
                dest, dest_mode, delivery_mode, vector, trig_mode);

    if ( !( hvm_irq_dpci->mirq[pirq].flags & HVM_IRQ_DPCI_GUEST_MSI ) )
    {
        gdprintk(XENLOG_WARNING, "pirq %x not msi \n", pirq);
        return 0;
    }

    deliver_bitmask = vmsi_get_delivery_bitmask(d, dest, dest_mode);
    if ( !deliver_bitmask )
    {
        HVM_DBG_LOG(DBG_LEVEL_IOAPIC, "ioapic deliver "
                    "no target on destination\n");
        return 0;
    }

    switch ( delivery_mode )
    {
    case dest_LowestPrio:
    {
        target = apic_lowest_prio(d, deliver_bitmask);
        if ( target != NULL )
            vmsi_inj_irq(d, target, vector, trig_mode, delivery_mode);
        else
            HVM_DBG_LOG(DBG_LEVEL_IOAPIC, "null round robin: "
                        "mask=%x vector=%x delivery_mode=%x\n",
                        deliver_bitmask, vector, dest_LowestPrio);
        break;
    }

    case dest_Fixed:
    case dest_ExtINT:
    {
        uint8_t bit;
        for ( bit = 0; deliver_bitmask != 0; bit++ )
        {
            if ( !(deliver_bitmask & (1 << bit)) )
                continue;
            deliver_bitmask &= ~(1 << bit);
            v = d->vcpu[bit];
            if ( v != NULL )
            {
                target = vcpu_vlapic(v);
                vmsi_inj_irq(d, target, vector, trig_mode, delivery_mode);
            }
        }
        break;
    }

    case dest_SMI:
    case dest_NMI:
    case dest_INIT:
    case dest__reserved_2:
    default:
        gdprintk(XENLOG_WARNING, "Unsupported delivery mode %d\n",
                 delivery_mode);
        break;
    }
    return 1;
}

/* MSI-X mask bit hypervisor interception */
struct msixtbl_entry
{
    struct list_head list;
    atomic_t refcnt;    /* how many bind_pt_irq called for the device */

    /* TODO: resolve the potential race by destruction of pdev */
    struct pci_dev *pdev;
    unsigned long gtable;       /* gpa of msix table */
    unsigned long table_len;
    unsigned long table_flags[MAX_MSIX_TABLE_ENTRIES / BITS_PER_LONG + 1];

    struct rcu_head rcu;
};

static struct msixtbl_entry *msixtbl_find_entry(
    struct vcpu *v, unsigned long addr)
{
    struct msixtbl_entry *entry;
    struct domain *d = v->domain;

    list_for_each_entry( entry, &d->arch.hvm_domain.msixtbl_list, list )
        if ( addr >= entry->gtable &&
             addr < entry->gtable + entry->table_len )
            return entry;

    return NULL;
}

static void __iomem *msixtbl_addr_to_virt(
    struct msixtbl_entry *entry, unsigned long addr)
{
    int idx, nr_page;

    if ( !entry )
        return NULL;

    nr_page = (addr >> PAGE_SHIFT) -
              (entry->gtable >> PAGE_SHIFT);

    if ( !entry->pdev )
        return NULL;

    idx = entry->pdev->msix_table_idx[nr_page];
    if ( !idx )
        return NULL;

    return (void *)(fix_to_virt(idx) +
                    (addr & ((1UL << PAGE_SHIFT) - 1)));
}

static int msixtbl_read(
    struct vcpu *v, unsigned long address,
    unsigned long len, unsigned long *pval)
{
    unsigned long offset;
    struct msixtbl_entry *entry;
    void *virt;
    int r = X86EMUL_UNHANDLEABLE;

    rcu_read_lock();

    if ( len != 4 )
        goto out;

    offset = address & (PCI_MSIX_ENTRY_SIZE - 1);
    if ( offset != PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET)
        goto out;

    entry = msixtbl_find_entry(v, address);
    virt = msixtbl_addr_to_virt(entry, address);
    if ( !virt )
        goto out;

    *pval = readl(virt);
    r = X86EMUL_OKAY;

out:
    rcu_read_unlock();
    return r;
}

static int msixtbl_write(struct vcpu *v, unsigned long address,
                        unsigned long len, unsigned long val)
{
    unsigned long offset;
    struct msixtbl_entry *entry;
    void *virt;
    int nr_entry;
    int r = X86EMUL_UNHANDLEABLE;

    rcu_read_lock();

    if ( len != 4 )
        goto out;

    entry = msixtbl_find_entry(v, address);
    nr_entry = (address - entry->gtable) % PCI_MSIX_ENTRY_SIZE;

    offset = address & (PCI_MSIX_ENTRY_SIZE - 1);
    if ( offset != PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET)
    {
        set_bit(nr_entry, &entry->table_flags);
        goto out;
    }

    /* exit to device model if address/data has been modified */
    if ( test_and_clear_bit(nr_entry, &entry->table_flags) )
        goto out;

    virt = msixtbl_addr_to_virt(entry, address);
    if ( !virt )
        goto out;

    writel(val, virt);
    r = X86EMUL_OKAY;

out:
    rcu_read_unlock();
    return r;
}

static int msixtbl_range(struct vcpu *v, unsigned long addr)
{
    struct msixtbl_entry *entry;
    void *virt;

    rcu_read_lock();

    entry = msixtbl_find_entry(v, addr);
    virt = msixtbl_addr_to_virt(entry, addr);

    rcu_read_unlock();

    return !!virt;
}

struct hvm_mmio_handler msixtbl_mmio_handler = {
    .check_handler = msixtbl_range,
    .read_handler = msixtbl_read,
    .write_handler = msixtbl_write
};

static struct msixtbl_entry *add_msixtbl_entry(struct domain *d,
                                               struct pci_dev *pdev,
                                               uint64_t gtable)
{
    struct msixtbl_entry *entry;
    u32 len;

    entry = xmalloc(struct msixtbl_entry);
    if ( !entry )
        return NULL;

    memset(entry, 0, sizeof(struct msixtbl_entry));
        
    INIT_LIST_HEAD(&entry->list);
    INIT_RCU_HEAD(&entry->rcu);
    atomic_set(&entry->refcnt, 0);

    len = pci_msix_get_table_len(pdev);
    entry->table_len = len;
    entry->pdev = pdev;
    entry->gtable = (unsigned long) gtable;

    list_add_rcu(&entry->list, &d->arch.hvm_domain.msixtbl_list);

    return entry;
}

static void free_msixtbl_entry(struct rcu_head *rcu)
{
    struct msixtbl_entry *entry;

    entry = container_of (rcu, struct msixtbl_entry, rcu);

    xfree(entry);
}

static void del_msixtbl_entry(struct msixtbl_entry *entry)
{
    list_del_rcu(&entry->list);
    call_rcu(&entry->rcu, free_msixtbl_entry);
}

int msixtbl_pt_register(struct domain *d, int pirq, uint64_t gtable)
{
    irq_desc_t *irq_desc;
    struct msi_desc *msi_desc;
    struct pci_dev *pdev;
    struct msixtbl_entry *entry;
    int r = -EINVAL;

    ASSERT(spin_is_locked(&pcidevs_lock));

    irq_desc = domain_spin_lock_irq_desc(d, pirq, NULL);

    if ( irq_desc->handler != &pci_msi_type )
        goto out;

    msi_desc = irq_desc->msi_desc;
    if ( !msi_desc )
        goto out;

    pdev = msi_desc->dev;

    spin_lock(&d->arch.hvm_domain.msixtbl_list_lock);

    list_for_each_entry( entry, &d->arch.hvm_domain.msixtbl_list, list )
        if ( pdev == entry->pdev )
            goto found;

    entry = add_msixtbl_entry(d, pdev, gtable);
    if ( !entry )
    {
        spin_unlock(&d->arch.hvm_domain.msixtbl_list_lock);
        goto out;
    }

found:
    atomic_inc(&entry->refcnt);
    spin_unlock(&d->arch.hvm_domain.msixtbl_list_lock);
    r = 0;

out:
    spin_unlock_irq(&irq_desc->lock);
    return r;

}

void msixtbl_pt_unregister(struct domain *d, int pirq)
{
    irq_desc_t *irq_desc;
    struct msi_desc *msi_desc;
    struct pci_dev *pdev;
    struct msixtbl_entry *entry;

    ASSERT(spin_is_locked(&pcidevs_lock));

    irq_desc = domain_spin_lock_irq_desc(d, pirq, NULL);

    if ( irq_desc->handler != &pci_msi_type )
        goto out;

    msi_desc = irq_desc->msi_desc;
    if ( !msi_desc )
        goto out;

    pdev = msi_desc->dev;

    spin_lock(&d->arch.hvm_domain.msixtbl_list_lock);

    list_for_each_entry( entry, &d->arch.hvm_domain.msixtbl_list, list )
        if ( pdev == entry->pdev )
            goto found;

    spin_unlock(&d->arch.hvm_domain.msixtbl_list_lock);


out:
    spin_unlock(&irq_desc->lock);
    return;

found:
    if ( !atomic_dec_and_test(&entry->refcnt) )
        del_msixtbl_entry(entry);

    spin_unlock(&d->arch.hvm_domain.msixtbl_list_lock);
    spin_unlock(&irq_desc->lock);
}

void msixtbl_pt_cleanup(struct domain *d, int pirq)
{
    struct msixtbl_entry *entry, *temp;

    spin_lock(&d->arch.hvm_domain.msixtbl_list_lock);

    list_for_each_entry_safe( entry, temp,
                              &d->arch.hvm_domain.msixtbl_list, list )
        del_msixtbl_entry(entry);

    spin_unlock(&d->arch.hvm_domain.msixtbl_list_lock);
}
