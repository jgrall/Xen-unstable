/*
 * PCI Backend - Functions for creating a virtual configuration space for
 *               exported PCI Devices.
 *               It's dangerous to allow PCI Driver Domains to change their
 *               device's resources (memory, i/o ports, interrupts). We need to
 *               restrict changes to certain PCI Configuration registers:
 *               BARs, INTERRUPT_PIN, most registers in the header...
 *
 * Author: Ryan Wilson <hap9@epoch.ncsc.mil>
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include "pciback.h"
#include "conf_space.h"

#define DEFINE_PCI_CONFIG(op,size,type) 					\
int pciback_##op##_config_##size 							\
(struct pci_dev *dev, int offset, type value, void *data)	\
{															\
	return pci_##op##_config_##size (dev, offset, value);	\
}

DEFINE_PCI_CONFIG(read, byte, u8 *)
DEFINE_PCI_CONFIG(read, word, u16 *)
DEFINE_PCI_CONFIG(read, dword, u32 *)

DEFINE_PCI_CONFIG(write, byte, u8)
DEFINE_PCI_CONFIG(write, word, u16)
DEFINE_PCI_CONFIG(write, dword, u32)

static int conf_space_read(struct pci_dev *dev,
			   struct config_field_entry *entry, int offset,
			   u32 * value)
{
	int ret = 0;
	struct config_field *field = entry->field;

	*value = 0;

	switch (field->size) {
	case 1:
		if (field->u.b.read)
			ret = field->u.b.read(dev, offset, (u8 *) value,
					      entry->data);
		break;
	case 2:
		if (field->u.w.read)
			ret = field->u.w.read(dev, offset, (u16 *) value,
					      entry->data);
		break;
	case 4:
		if (field->u.dw.read)
			ret = field->u.dw.read(dev, offset, value, entry->data);
		break;
	}
	return ret;
}

static int conf_space_write(struct pci_dev *dev,
			    struct config_field_entry *entry, int offset,
			    u32 value)
{
	int ret = 0;
	struct config_field *field = entry->field;

	switch (field->size) {
	case 1:
		if (field->u.b.write)
			ret = field->u.b.write(dev, offset, (u8) value,
					       entry->data);
		break;
	case 2:
		if (field->u.w.write)
			ret = field->u.w.write(dev, offset, (u16) value,
					       entry->data);
		break;
	case 4:
		if (field->u.dw.write)
			ret = field->u.dw.write(dev, offset, value,
					        entry->data);
		break;
	}
	return ret;
}

static inline u32 get_mask(int size)
{
	if (size == 1)
		return 0xff;
	else if (size == 2)
		return 0xffff;
	else
		return 0xffffffff;
}

static inline int valid_request(int offset, int size)
{
	/* Validate request (no un-aligned requests) */
	if ((size == 1 || size == 2 || size == 4) && (offset % size) == 0)
		return 1;
	return 0;
}

static inline u32 merge_value(u32 val, u32 new_val, u32 new_val_mask,
			      u32 offset)
{
	if (offset >= 0) {
		new_val_mask <<= (offset * 8);
		new_val <<= (offset * 8);
	} else {
		new_val_mask >>= (offset * -8);
		new_val >>= (offset * -8);
	}
	val = (val & ~new_val_mask) | (new_val & new_val_mask);

	return val;
}

static int pcibios_err_to_errno(int err)
{
	switch (err) {
	case PCIBIOS_SUCCESSFUL:
		return XEN_PCI_ERR_success;
	case PCIBIOS_DEVICE_NOT_FOUND:
		return XEN_PCI_ERR_dev_not_found;
	case PCIBIOS_BAD_REGISTER_NUMBER:
		return XEN_PCI_ERR_invalid_offset;
	case PCIBIOS_FUNC_NOT_SUPPORTED:
		return XEN_PCI_ERR_not_implemented;
	case PCIBIOS_SET_FAILED:
		return XEN_PCI_ERR_access_denied;
	}
	return err;
}

int pciback_config_read(struct pci_dev *dev, int offset, int size,
			u32 * ret_val)
{
	int err = 0;
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);
	struct config_field_entry *cfg_entry;
	struct config_field *field;
	int req_start, req_end, field_start, field_end;
	/* if read fails for any reason, return 0 (as if device didn't respond) */
	u32 value = 0, tmp_val;

	if (unlikely(verbose_request))
		printk(KERN_DEBUG "pciback: %s: read %d bytes at 0x%x\n",
		       pci_name(dev), size, offset);

	if (!valid_request(offset, size)) {
		err = XEN_PCI_ERR_invalid_offset;
		goto out;
	}

	/* Get the real value first, then modify as appropriate */
	switch (size) {
	case 1:
		err = pci_read_config_byte(dev, offset, (u8 *) & value);
		break;
	case 2:
		err = pci_read_config_word(dev, offset, (u16 *) & value);
		break;
	case 4:
		err = pci_read_config_dword(dev, offset, &value);
		break;
	}

	list_for_each_entry(cfg_entry, &dev_data->config_fields, list) {
		field = cfg_entry->field;

		req_start = offset;
		req_end = offset + size;
		field_start = field->offset;
		field_end = field->offset + field->size;

		if ((req_start >= field_start && req_start < field_end)
		    || (req_end > field_start && req_end <= field_end)) {
			err = conf_space_read(dev, cfg_entry, offset, &tmp_val);
			if (err)
				goto out;

			value = merge_value(value, tmp_val,
					    get_mask(field->size),
					    field_start - req_start);
		}
	}

      out:
	if (unlikely(verbose_request))
		printk(KERN_DEBUG "pciback: %s: read %d bytes at 0x%x = %x\n",
		       pci_name(dev), size, offset, value);

	*ret_val = value;
	return pcibios_err_to_errno(err);
}

int pciback_config_write(struct pci_dev *dev, int offset, int size, u32 value)
{
	int err = 0;
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);
	struct config_field_entry *cfg_entry;
	struct config_field *field;
	u32 tmp_val;
	int req_start, req_end, field_start, field_end;

	if (unlikely(verbose_request))
		printk(KERN_DEBUG
		       "pciback: %s: write request %d bytes at 0x%x = %x\n",
		       pci_name(dev), size, offset, value);

	if (!valid_request(offset, size))
		return XEN_PCI_ERR_invalid_offset;

	list_for_each_entry(cfg_entry, &dev_data->config_fields, list) {
		field = cfg_entry->field;

		req_start = offset;
		req_end = offset + size;
		field_start = field->offset;
		field_end = field->offset + field->size;

		if ((req_start >= field_start && req_start < field_end)
		    || (req_end > field_start && req_end <= field_end)) {
			tmp_val = 0;

			err = pciback_config_read(dev, offset, size, &tmp_val);
			if (err)
				break;

			tmp_val = merge_value(tmp_val, value, get_mask(size),
					      field_start - req_start);

			err = conf_space_write(dev, cfg_entry, offset, tmp_val);
		}
	}

	return pcibios_err_to_errno(err);
}

void pciback_config_reset(struct pci_dev *dev)
{
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);
	struct config_field_entry *cfg_entry;
	struct config_field *field;

	list_for_each_entry(cfg_entry, &dev_data->config_fields, list) {
		field = cfg_entry->field;

		if (field->reset)
			field->reset(dev, field->offset, cfg_entry->data);
	}
}

void pciback_config_free(struct pci_dev *dev)
{
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);
	struct config_field_entry *cfg_entry, *t;
	struct config_field *field;

	list_for_each_entry_safe(cfg_entry, t, &dev_data->config_fields, list) {
		list_del(&cfg_entry->list);

		field = cfg_entry->field;

		if (field->release)
			field->release(dev, field->offset, cfg_entry->data);

		kfree(cfg_entry);
	}
}

int pciback_config_add_field(struct pci_dev *dev, struct config_field *field)
{
	int err = 0;
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);
	struct config_field_entry *cfg_entry;
	void *tmp;

	cfg_entry = kmalloc(sizeof(*cfg_entry), GFP_KERNEL);
	if (!cfg_entry) {
		err = -ENOMEM;
		goto out;
	}

	cfg_entry->data = NULL;
	cfg_entry->field = field;

	if (field->init) {
		tmp = field->init(dev, field->offset);

		if (IS_ERR(tmp)) {
			err = PTR_ERR(tmp);
			goto out;
		}

		cfg_entry->data = tmp;
	}

	list_add_tail(&cfg_entry->list, &dev_data->config_fields);

      out:
	if (err)
		kfree(cfg_entry);

	return err;
}

/* This sets up the device's virtual configuration space to keep track of 
 * certain registers (like the base address registers (BARs) so that we can
 * keep the client from manipulating them directly.
 */
int pciback_config_init(struct pci_dev *dev)
{
	int err = 0;
	struct pciback_dev_data *dev_data = pci_get_drvdata(dev);

	INIT_LIST_HEAD(&dev_data->config_fields);

	err = pciback_config_header_add_fields(dev);

	return err;
}
