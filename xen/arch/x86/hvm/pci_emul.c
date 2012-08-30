#include <asm/hvm/support.h>
#include <xen/hvm/pci_emul.h>
#include <xen/pci.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>

#define PCI_DEBUGSTR "%x:%x.%x"
#define PCI_DEBUG(bdf) ((bdf) >> 8) & 0xff, ((bdf) >> 3) & 0x1f, ((bdf)) & 0x7
#define PCI_MASK_BDF(bdf) (((bdf) & 0x00ffff00) >> 8)
#define PCI_CMP_BDF(Pci, Bdf) ((pci)->bdf == PCI_MASK_BDF(Bdf))

static int handle_config_space(int dir, uint32_t port, uint32_t bytes,
                               uint32_t *val)
{
    uint32_t pci_cf8;
    struct hvm_ioreq_server *s;
    ioreq_t *p = get_ioreq(current);
    int rc = X86EMUL_UNHANDLEABLE;
    struct vcpu *v = current;

    if ( port == 0xcf8 && bytes == 4 )
    {
        if ( dir == IOREQ_READ )
            *val = v->arch.hvm_vcpu.pci_cf8;
        else
            v->arch.hvm_vcpu.pci_cf8 = *val;
        return X86EMUL_OKAY;
    }
    else if ( port < 0xcfc )
        return X86EMUL_UNHANDLEABLE;

    spin_lock(&v->domain->arch.hvm_domain.pci_root.pci_lock);
    spin_lock(&v->domain->arch.hvm_domain.ioreq_server_lock);

    pci_cf8 = v->arch.hvm_vcpu.pci_cf8;

    /* Retrieve PCI */
    s = radix_tree_lookup(&v->domain->arch.hvm_domain.pci_root.pci_list,
                          PCI_MASK_BDF(pci_cf8));

    if ( unlikely(s == NULL) )
    {
        *val = ~0;
        rc = X86EMUL_OKAY;
        goto end_handle;
    }

    /**
     * We just fill the ioreq, hvm_send_assist_req will send the request
     * The size is used to find the right access
     **/
    /* We use the 16 high-bits for the offset (0 => 0xcfc, 1 => 0xcfd...) */
    p->size = (p->addr - 0xcfc) << 16 | (p->size & 0xffff);
    p->type = IOREQ_TYPE_PCI_CONFIG;
    p->addr = pci_cf8;

    set_ioreq(v, &s->ioreq, p);

end_handle:
    spin_unlock(&v->domain->arch.hvm_domain.ioreq_server_lock);
    spin_unlock(&v->domain->arch.hvm_domain.pci_root.pci_lock);

    return rc;
}

int hvm_register_pcidev(domid_t domid, ioservid_t id,
                        uint16_t domain, uint8_t bus,
                        uint8_t device, uint8_t function)
{
    struct domain *d;
    struct hvm_ioreq_server *s;
    int rc = 0;
    struct radix_tree_root *tree;
    uint16_t bdf = 0;

    /* For the moment we don't handle pci when domain != 0 */
    if ( domain != 0 )
        return -EINVAL;

    rc = rcu_lock_target_domain_by_id(domid, &d);

    if ( rc != 0 )
        return rc;

    if ( !is_hvm_domain(d) )
    {
        rcu_unlock_domain(d);
        return -EINVAL;
    }

    /* Search server */
    spin_lock(&d->arch.hvm_domain.ioreq_server_lock);
    s = d->arch.hvm_domain.ioreq_server_list;
    while ( (s != NULL) && (s->id != id) )
        s = s->next;

    spin_unlock(&d->arch.hvm_domain.ioreq_server_lock);

    if ( s == NULL )
    {
        gdprintk(XENLOG_ERR, "Cannot find server %u\n", id);
        rc = -ENOENT;
        goto fail;
    }

    spin_lock(&d->arch.hvm_domain.pci_root.pci_lock);

    tree = &d->arch.hvm_domain.pci_root.pci_list;

    bdf |= ((uint16_t)bus) << 8;
    bdf |= ((uint16_t)device & 0x1f) << 3;
    bdf |= ((uint16_t)function & 0x7);

    if ( radix_tree_lookup(tree, bdf) )
    {
        rc = -EEXIST;
        gdprintk(XENLOG_ERR, "Bdf " PCI_DEBUGSTR " is already allocated\n",
                 PCI_DEBUG(bdf));
        goto create_end;
    }

    rc = radix_tree_insert(tree, bdf, s);
    if ( rc )
    {
        gdprintk(XENLOG_ERR, "Cannot insert the bdf\n");
        goto create_end;
    }

create_end:
    spin_unlock(&d->arch.hvm_domain.pci_root.pci_lock);
fail:
    rcu_unlock_domain(d);

    return rc;
}

void hvm_init_pci_emul(struct domain *d)
{
    struct pci_root_emul *root = &d->arch.hvm_domain.pci_root;

    spin_lock_init(&root->pci_lock);

    radix_tree_init(&root->pci_list);

    /* Register the config space handler */
    register_portio_handler(d, 0xcf8, 8, handle_config_space);
}

void hvm_destroy_pci_emul(struct domain *d)
{
    struct pci_root_emul *root = &d->arch.hvm_domain.pci_root;

    spin_lock(&root->pci_lock);

    radix_tree_destroy(&root->pci_list, NULL);

    spin_unlock(&root->pci_lock);
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
