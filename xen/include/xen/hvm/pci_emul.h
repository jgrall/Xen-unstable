#ifndef PCI_EMUL_H_
# define PCI_EMUL_H_

# include <xen/radix-tree.h>
# include <xen/spinlock.h>
# include <xen/types.h>

void hvm_init_pci_emul(struct domain *d);
void hvm_destroy_pci_emul(struct domain *d);
int hvm_register_pcidev(domid_t domid, ioservid_t id, u16 bdf);

/* Size of the standard PCI config space */
#define PCI_CONFIG_SPACE_SIZE 0x100
#define PCI_CMP_BDF(Pci, Bdf) ((Pci)->bdf == PCI_MASK_BDF(Bdf))
#define PCI_MASK_BDF(bdf) (((bdf) & 0x00ffff00) >> 8)

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
