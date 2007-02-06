/*
 * This file contains work-arounds for x86 and x86_64 platform bugs.
 */
#include <linux/pci.h>
#include <linux/irq.h>

#if defined(CONFIG_X86_IO_APIC) && (defined(CONFIG_SMP) || defined(CONFIG_XEN)) && defined(CONFIG_PCI)

static void __devinit quirk_intel_irqbalance(struct pci_dev *dev)
{
	u8 config, rev;
	u32 word;

	/* BIOS may enable hardware IRQ balancing for
	 * E7520/E7320/E7525(revision ID 0x9 and below)
	 * based platforms.
	 * Disable SW irqbalance/affinity on those platforms.
	 */
	pci_read_config_byte(dev, PCI_CLASS_REVISION, &rev);
	if (rev > 0x9)
		return;

	printk(KERN_INFO "Intel E7520/7320/7525 detected.");

	/* enable access to config space*/
	pci_read_config_byte(dev, 0xf4, &config);
	pci_write_config_byte(dev, 0xf4, config|0x2);

	/* read xTPR register */
	raw_pci_ops->read(0, 0, 0x40, 0x4c, 2, &word);

	if (!(word & (1 << 13))) {
		dom0_op_t op;
		printk(KERN_INFO "Disabling irq balancing and affinity\n");
		op.cmd = DOM0_PLATFORM_QUIRK;
		op.u.platform_quirk.quirk_id = QUIRK_NOIRQBALANCING;
		(void)HYPERVISOR_dom0_op(&op);
	}

	/* put back the original value for config space*/
	if (!(config & 0x2))
		pci_write_config_byte(dev, 0xf4, config);
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7320_MCH,	quirk_intel_irqbalance);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7525_MCH,	quirk_intel_irqbalance);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7520_MCH,	quirk_intel_irqbalance);
#endif
