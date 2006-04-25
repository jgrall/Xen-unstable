/*
 * PCI Backend Operations - respond to PCI requests from Frontend
 *
 *   Author: Ryan Wilson <hap9@epoch.ncsc.mil>
 */
#include <linux/module.h>
#include <asm/bitops.h>
#include <xen/evtchn.h>
#include "pciback.h"

int verbose_request = 0;
module_param(verbose_request, int, 0644);

/* Ensure a device is "turned off" and ready to be exported.
 * (Also see pciback_config_reset to ensure virtual configuration space is
 * ready to be re-exported)
 */
void pciback_reset_device(struct pci_dev *dev)
{
	u16 cmd;

	/* Disable devices (but not bridges) */
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		pci_disable_device(dev);

		pci_write_config_word(dev, PCI_COMMAND, 0);

		dev->is_enabled = 0;
		dev->is_busmaster = 0;
	} else {
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		if (cmd & (PCI_COMMAND_INVALIDATE)) {
			cmd &= ~(PCI_COMMAND_INVALIDATE);
			pci_write_config_word(dev, PCI_COMMAND, cmd);

			dev->is_busmaster = 0;
		}
	}

	pciback_config_reset(dev);
}

static inline void test_and_schedule_op(struct pciback_device *pdev)
{
	/* Check that frontend is requesting an operation and that we are not
	 * already processing a request */
	if (test_bit(_XEN_PCIF_active, (unsigned long *)&pdev->sh_info->flags)
	    && !test_and_set_bit(_PDEVF_op_active, &pdev->flags))
		schedule_work(&pdev->op_work);
}

/* Performing the configuration space reads/writes must not be done in atomic
 * context because some of the pci_* functions can sleep (mostly due to ACPI
 * use of semaphores). This function is intended to be called from a work
 * queue in process context taking a struct pciback_device as a parameter */
void pciback_do_op(void *data)
{
	struct pciback_device *pdev = data;
	struct pci_dev *dev;
	struct xen_pci_op *op = &pdev->sh_info->op;

	dev = pciback_get_pci_dev(pdev, op->domain, op->bus, op->devfn);

	if (dev == NULL)
		op->err = XEN_PCI_ERR_dev_not_found;
	else if (op->cmd == XEN_PCI_OP_conf_read)
		op->err = pciback_config_read(dev, op->offset, op->size,
					      &op->value);
	else if (op->cmd == XEN_PCI_OP_conf_write)
		op->err = pciback_config_write(dev, op->offset, op->size,
					       op->value);
	else
		op->err = XEN_PCI_ERR_not_implemented;

	/* Tell the driver domain that we're done. */ 
	wmb();
	clear_bit(_XEN_PCIF_active, (unsigned long *)&pdev->sh_info->flags);
	notify_remote_via_irq(pdev->evtchn_irq);

	/* Mark that we're done */
	wmb();
	clear_bit(_PDEVF_op_active, &pdev->flags);

	/* Check to see if the driver domain tried to start another request
	 * in between clearing _XEN_PCIF_active and clearing _PDEVF_op_active */
	test_and_schedule_op(pdev);
}

irqreturn_t pciback_handle_event(int irq, void *dev_id, struct pt_regs *regs)
{
	struct pciback_device *pdev = dev_id;

	test_and_schedule_op(pdev);

	return IRQ_HANDLED;
}
