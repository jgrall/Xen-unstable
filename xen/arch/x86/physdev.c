
#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/types.h>
#include <xen/sched.h>
#include <xen/irq.h>
#include <xen/event.h>
#include <xen/guest_access.h>
#include <xen/iocap.h>
#include <asm/current.h>
#include <asm/msi.h>
#include <asm/hypercall.h>
#include <public/xen.h>
#include <public/physdev.h>
#include <xsm/xsm.h>
#include <asm/p2m.h>

#ifndef COMPAT
typedef long ret_t;
#endif

int
ioapic_guest_read(
    unsigned long physbase, unsigned int reg, u32 *pval);
int
ioapic_guest_write(
    unsigned long physbase, unsigned int reg, u32 pval);

static int physdev_map_pirq(struct physdev_map_pirq *map)
{
    struct domain *d;
    int vector, pirq, ret = 0;
    struct msi_info _msi;
    void *map_data = NULL;

    if ( !IS_PRIV(current->domain) )
        return -EPERM;

    if ( !map )
        return -EINVAL;

    if ( map->domid == DOMID_SELF )
        d = rcu_lock_domain(current->domain);
    else
        d = rcu_lock_domain_by_id(map->domid);

    if ( d == NULL )
    {
        ret = -ESRCH;
        goto free_domain;
    }

    /* Verify or get vector. */
    switch ( map->type )
    {
        case MAP_PIRQ_TYPE_GSI:
            if ( map->index < 0 || map->index >= NR_IRQS )
            {
                dprintk(XENLOG_G_ERR, "dom%d: map invalid irq %d\n",
                        d->domain_id, map->index);
                ret = -EINVAL;
                goto free_domain;
            }
            vector = IO_APIC_VECTOR(map->index);
            if ( !vector )
            {
                dprintk(XENLOG_G_ERR, "dom%d: map irq with no vector %d\n",
                        d->domain_id, vector);
                ret = -EINVAL;
                goto free_domain;
            }
            break;

        case MAP_PIRQ_TYPE_MSI:
            vector = map->index;
            if ( vector == -1 )
                vector = assign_irq_vector(AUTO_ASSIGN);

            if ( vector < 0 || vector >= NR_VECTORS )
            {
                dprintk(XENLOG_G_ERR, "dom%d: map irq with wrong vector %d\n",
                        d->domain_id, vector);
                ret = -EINVAL;
                goto free_domain;
            }

            _msi.bus = map->bus;
            _msi.devfn = map->devfn;
            _msi.entry_nr = map->entry_nr;
            _msi.table_base = map->table_base;
            _msi.vector = vector;
            map_data = &_msi;
            break;

        default:
            dprintk(XENLOG_G_ERR, "dom%d: wrong map_pirq type %x\n",
                    d->domain_id, map->type);
            ret = -EINVAL;
            goto free_domain;
    }

    spin_lock(&pcidevs_lock);
    /* Verify or get pirq. */
    spin_lock(&d->event_lock);
    if ( map->pirq < 0 )
    {
        if ( d->arch.vector_pirq[vector] )
        {
            dprintk(XENLOG_G_ERR, "dom%d: %d:%d already mapped to %d\n",
                    d->domain_id, map->index, map->pirq,
                    d->arch.vector_pirq[vector]);
            pirq = d->arch.vector_pirq[vector];
            if ( pirq < 0 )
            {
                ret = -EBUSY;
                goto done;
            }
        }
        else
        {
            pirq = get_free_pirq(d, map->type, map->index);
            if ( pirq < 0 )
            {
                dprintk(XENLOG_G_ERR, "dom%d: no free pirq\n", d->domain_id);
                ret = pirq;
                goto done;
            }
        }
    }
    else
    {
        if ( d->arch.vector_pirq[vector] &&
             d->arch.vector_pirq[vector] != map->pirq )
        {
            dprintk(XENLOG_G_ERR, "dom%d: vector %d conflicts with irq %d\n",
                    d->domain_id, map->index, map->pirq);
            ret = -EEXIST;
            goto done;
        }
        else
            pirq = map->pirq;
    }

    ret = map_domain_pirq(d, pirq, vector, map->type, map_data);
    if ( ret == 0 )
        map->pirq = pirq;

done:
    spin_unlock(&d->event_lock);
    spin_unlock(&pcidevs_lock);
    if ( (ret != 0) && (map->type == MAP_PIRQ_TYPE_MSI) && (map->index == -1) )
        free_irq_vector(vector);
free_domain:
    rcu_unlock_domain(d);
    return ret;
}

static int physdev_unmap_pirq(struct physdev_unmap_pirq *unmap)
{
    struct domain *d;
    int ret;

    if ( !IS_PRIV(current->domain) )
        return -EPERM;

    if ( unmap->domid == DOMID_SELF )
        d = rcu_lock_domain(current->domain);
    else
        d = rcu_lock_domain_by_id(unmap->domid);

    if ( d == NULL )
        return -ESRCH;

    spin_lock(&pcidevs_lock);
    spin_lock(&d->event_lock);
    ret = unmap_domain_pirq(d, unmap->pirq);
    spin_unlock(&d->event_lock);
    spin_unlock(&pcidevs_lock);

    rcu_unlock_domain(d);

    return ret;
}

ret_t do_physdev_op(int cmd, XEN_GUEST_HANDLE(void) arg)
{
    int irq;
    ret_t ret;
    struct vcpu *v = current;

    switch ( cmd )
    {
    case PHYSDEVOP_eoi: {
        struct physdev_eoi eoi;
        ret = -EFAULT;
        if ( copy_from_guest(&eoi, arg, 1) != 0 )
            break;
        ret = -EINVAL;
        if ( eoi.irq < 0 || eoi.irq >= NR_IRQS )
            break;
        if ( v->domain->arch.pirq_eoi_map )
            evtchn_unmask(v->domain->pirq_to_evtchn[eoi.irq]);
        ret = pirq_guest_eoi(v->domain, eoi.irq);
        break;
    }

    case PHYSDEVOP_pirq_eoi_gmfn: {
        struct physdev_pirq_eoi_gmfn info;
        unsigned long mfn;

        BUILD_BUG_ON(NR_IRQS > (PAGE_SIZE * 8));

        ret = -EFAULT;
        if ( copy_from_guest(&info, arg, 1) != 0 )
            break;

        ret = -EINVAL;
        mfn = gmfn_to_mfn(current->domain, info.gmfn);
        if ( !mfn_valid(mfn) ||
             !get_page_and_type(mfn_to_page(mfn), v->domain,
                                PGT_writable_page) )
            break;

        if ( cmpxchg(&v->domain->arch.pirq_eoi_map_mfn, 0, mfn) != 0 )
        {
            put_page_and_type(mfn_to_page(mfn));
            ret = -EBUSY;
            break;
        }

        v->domain->arch.pirq_eoi_map = map_domain_page_global(mfn);
        if ( v->domain->arch.pirq_eoi_map == NULL )
        {
            v->domain->arch.pirq_eoi_map_mfn = 0;
            put_page_and_type(mfn_to_page(mfn));
            ret = -ENOSPC;
            break;
        }

        ret = 0;
        break;
    }

    /* Legacy since 0x00030202. */
    case PHYSDEVOP_IRQ_UNMASK_NOTIFY: {
        ret = pirq_guest_unmask(v->domain);
        break;
    }

    case PHYSDEVOP_irq_status_query: {
        struct physdev_irq_status_query irq_status_query;
        ret = -EFAULT;
        if ( copy_from_guest(&irq_status_query, arg, 1) != 0 )
            break;
        irq = irq_status_query.irq;
        ret = -EINVAL;
        if ( (irq < 0) || (irq >= NR_IRQS) )
            break;
        irq_status_query.flags = 0;
        if ( pirq_acktype(v->domain, irq) != 0 )
            irq_status_query.flags |= XENIRQSTAT_needs_eoi;
        if ( pirq_shared(v->domain, irq) )
            irq_status_query.flags |= XENIRQSTAT_shared;
        ret = copy_to_guest(arg, &irq_status_query, 1) ? -EFAULT : 0;
        break;
    }

    case PHYSDEVOP_map_pirq: {
        struct physdev_map_pirq map;

        ret = -EFAULT;
        if ( copy_from_guest(&map, arg, 1) != 0 )
            break;

        ret = physdev_map_pirq(&map);

        if ( copy_to_guest(arg, &map, 1) != 0 )
            ret = -EFAULT;
        break;
    }

    case PHYSDEVOP_unmap_pirq: {
        struct physdev_unmap_pirq unmap;

        ret = -EFAULT;
        if ( copy_from_guest(&unmap, arg, 1) != 0 )
            break;

        ret = physdev_unmap_pirq(&unmap);
        break;
    }

    case PHYSDEVOP_apic_read: {
        struct physdev_apic apic;
        ret = -EFAULT;
        if ( copy_from_guest(&apic, arg, 1) != 0 )
            break;
        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;
        ret = xsm_apic(v->domain, cmd);
        if ( ret )
            break;
        ret = ioapic_guest_read(apic.apic_physbase, apic.reg, &apic.value);
        if ( copy_to_guest(arg, &apic, 1) != 0 )
            ret = -EFAULT;
        break;
    }

    case PHYSDEVOP_apic_write: {
        struct physdev_apic apic;
        ret = -EFAULT;
        if ( copy_from_guest(&apic, arg, 1) != 0 )
            break;
        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;
        ret = xsm_apic(v->domain, cmd);
        if ( ret )
            break;
        ret = ioapic_guest_write(apic.apic_physbase, apic.reg, apic.value);
        break;
    }

    case PHYSDEVOP_alloc_irq_vector: {
        struct physdev_irq irq_op;

        ret = -EFAULT;
        if ( copy_from_guest(&irq_op, arg, 1) != 0 )
            break;

        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;

        ret = xsm_assign_vector(v->domain, irq_op.irq);
        if ( ret )
            break;

        irq = irq_op.irq;
        ret = -EINVAL;
        if ( (irq < 0) || (irq >= NR_IRQS) )
            break;

        irq_op.vector = assign_irq_vector(irq);

        spin_lock(&pcidevs_lock);
        spin_lock(&dom0->event_lock);
        ret = map_domain_pirq(dom0, irq_op.irq, irq_op.vector,
                              MAP_PIRQ_TYPE_GSI, NULL);
        spin_unlock(&dom0->event_lock);
        spin_unlock(&pcidevs_lock);

        if ( copy_to_guest(arg, &irq_op, 1) != 0 )
            ret = -EFAULT;
        break;
    }

    case PHYSDEVOP_set_iopl: {
        struct physdev_set_iopl set_iopl;
        ret = -EFAULT;
        if ( copy_from_guest(&set_iopl, arg, 1) != 0 )
            break;
        ret = -EINVAL;
        if ( set_iopl.iopl > 3 )
            break;
        ret = 0;
        v->arch.iopl = set_iopl.iopl;
        break;
    }

    case PHYSDEVOP_set_iobitmap: {
        struct physdev_set_iobitmap set_iobitmap;
        ret = -EFAULT;
        if ( copy_from_guest(&set_iobitmap, arg, 1) != 0 )
            break;
        ret = -EINVAL;
        if ( !guest_handle_okay(set_iobitmap.bitmap, IOBMP_BYTES) ||
             (set_iobitmap.nr_ports > 65536) )
            break;
        ret = 0;
#ifndef COMPAT
        v->arch.iobmp       = set_iobitmap.bitmap;
#else
        guest_from_compat_handle(v->arch.iobmp, set_iobitmap.bitmap);
#endif
        v->arch.iobmp_limit = set_iobitmap.nr_ports;
        break;
    }

    case PHYSDEVOP_manage_pci_add: {
        struct physdev_manage_pci manage_pci;
        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;
        ret = -EFAULT;
        if ( copy_from_guest(&manage_pci, arg, 1) != 0 )
            break;

        ret = pci_add_device(manage_pci.bus, manage_pci.devfn);
        break;
    }

    case PHYSDEVOP_manage_pci_remove: {
        struct physdev_manage_pci manage_pci;
        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;
        ret = -EFAULT;
        if ( copy_from_guest(&manage_pci, arg, 1) != 0 )
            break;

        ret = pci_remove_device(manage_pci.bus, manage_pci.devfn);
        break;
    }

    case PHYSDEVOP_restore_msi: {
        struct physdev_restore_msi restore_msi;
        struct pci_dev *pdev;

        ret = -EPERM;
        if ( !IS_PRIV(v->domain) )
            break;

        ret = -EFAULT;
        if ( copy_from_guest(&restore_msi, arg, 1) != 0 )
            break;

        spin_lock(&pcidevs_lock);
        pdev = pci_get_pdev(restore_msi.bus, restore_msi.devfn);
        ret = pdev ? pci_restore_msi_state(pdev) : -ENODEV;
        spin_unlock(&pcidevs_lock);
        break;
    }
    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
