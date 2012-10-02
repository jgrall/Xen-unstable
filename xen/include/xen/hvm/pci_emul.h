/*
 * pci_emul.h: Emulation of configuration space registers.
 *
 * Copyright (c) 2012, Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#ifndef __XEN_HVM_PCI_EMUL_H__
# define __XEN_HVM_PCI_EMUL_H__

# include <xen/radix-tree.h>
# include <xen/spinlock.h>
# include <xen/types.h>

void hvm_init_pci_emul(struct domain *d);
void hvm_destroy_pci_emul(struct domain *d);
int hvm_register_pcidev(domid_t domid, ioservid_t id,
                        uint16_t domain, uint8_t bus,
                        uint8_t device, uint8_t function);

struct pci_root_emul {
    spinlock_t pci_lock;
    struct radix_tree_root pci_list;
};

#endif /* !__XEN_HVM_PCI_EMUL_H__ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
