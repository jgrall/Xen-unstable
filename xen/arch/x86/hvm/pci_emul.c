#include <asm/hvm/support.h>
#include <xen/hvm/pci_emul.h>
#include <xen/pci.h>
#include <xen/sched.h>
#include <xen/xmalloc.h>

#define PCI_DEBUGSTR "%x:%x.%x"
#define PCI_DEBUG(bdf) ((bdf) >> 16) & 0xff, ((bdf) >> 11) & 0x1f, ((bdf) >> 8) & 0x7

static int handle_config_space(int dir, uint32_t port, uint32_t bytes,
                               uint32_t *val)
{
    uint32_t pci_cf8;
    struct pci_device_emul *pci;
    ioreq_t *p = get_ioreq(current);
    int rc = X86EMUL_UNHANDLEABLE;
    struct vcpu *v = current;

    spin_lock(&v->domain->arch.hvm_domain.pci_root.pci_lock);

    if (port == 0xcf8)
    {
        rc = X86EMUL_OKAY;
        v->arch.hvm_vcpu.pci_cf8 = *val;
        goto end_handle;
    }

    pci_cf8 = v->arch.hvm_vcpu.pci_cf8;

    /* Retrieve PCI */
    pci = v->domain->arch.hvm_domain.pci_root.pci;

    while (pci && !PCI_CMP_BDF(pci, pci_cf8))
        pci = pci->next;

    /* We just fill the ioreq, hvm_send_assist_req will send the request */
    if (unlikely(pci == NULL))
    {
        *val = ~0;
        rc = X86EMUL_OKAY;
        goto end_handle;
    }

    p->type = IOREQ_TYPE_PCI_CONFIG;
    p->addr = (pci_cf8 & ~3) + (p->addr & 3);

    set_ioreq(v, &pci->server->ioreq, p);

end_handle:
    spin_unlock(&v->domain->arch.hvm_domain.pci_root.pci_lock);
    return rc;
}

int hvm_register_pcidev(domid_t domid, unsigned int id, u16 bdf)
{
    struct domain *d;
    struct hvm_ioreq_server *s;
    struct pci_device_emul *x;
    int rc = 0;

    rc = rcu_lock_target_domain_by_id(domid, &d);

    if (rc != 0)
        return rc;

    if (!is_hvm_domain(d))
    {
        rcu_unlock_domain(d);
        return -EINVAL;
    }

    /* Search server */
    spin_lock(&d->arch.hvm_domain.ioreq_server_lock);
    s = d->arch.hvm_domain.ioreq_server_list;
    while ((s != NULL) && (s->id != id))
        s = s->next;

    if (s == NULL)
    {
        dprintk(XENLOG_DEBUG, "Cannot find server\n");
        rc = -ENOENT;
        goto create_end;
    }

    spin_unlock(&d->arch.hvm_domain.ioreq_server_lock);

    spin_lock(&d->arch.hvm_domain.pci_root.pci_lock);
    x = xmalloc(struct pci_device_emul);

    if (!x)
    {
        dprintk(XENLOG_DEBUG, "Cannot allocate pci\n");
        rc = -ENOMEM;
        goto create_end;
    }

    x->bdf = PCI_MASK_BDF(bdf);
    x->server = s;
    x->next = d->arch.hvm_domain.pci_root.pci;
    d->arch.hvm_domain.pci_root.pci = x;

create_end:
    spin_unlock(&d->arch.hvm_domain.pci_root.pci_lock);
    rcu_unlock_domain(d);

    return rc;
}

int hvm_init_pci_emul(struct domain *d)
{
    struct pci_root_emul *root = &d->arch.hvm_domain.pci_root;

    spin_lock_init(&root->pci_lock);

    root->pci = NULL;

    /* Register the config space handler */
    register_portio_handler(d, 0xcf8, 8, handle_config_space);

    return 0;
}

void hvm_destroy_pci_emul(struct domain *d)
{
    struct pci_root_emul *root = &d->arch.hvm_domain.pci_root;
    struct pci_device_emul *p;

    spin_lock(&root->pci_lock);

    while ( (p = root->pci) != NULL )
    {
        root->pci = p->next;
        xfree(p);
    }

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
