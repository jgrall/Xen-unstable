#ifndef PCI_EMUL_H_
# define PCI_EMUL_H_

# include <xen/radix-tree.h>
# include <xen/spinlock.h>
# include <xen/types.h>

void hvm_init_pci_emul(struct domain *d);
void hvm_destroy_pci_emul(struct domain *d);
int hvm_register_pcidev(domid_t domid, ioservid_t id,
                        uint8_t domain, uint8_t bus,
                        uint8_t device, uint8_t function);

struct pci_root_emul {
    spinlock_t pci_lock;
    struct radix_tree_root pci_list;
};

#endif /* !PCI_EMUL_H_ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
