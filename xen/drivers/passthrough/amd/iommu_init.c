/*
 * Copyright (C) 2007 Advanced Micro Devices, Inc.
 * Author: Leo Duran <leo.duran@amd.com>
 * Author: Wei Wang <wei.wang2@amd.com> - adapted to xen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <xen/config.h>
#include <xen/errno.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <asm/amd-iommu.h>
#include <asm/msi.h>
#include <asm/hvm/svm/amd-iommu-proto.h>
#include <asm-x86/fixmap.h>

static struct amd_iommu *vector_to_iommu[NR_VECTORS];
static int nr_amd_iommus;
static long amd_iommu_cmd_buffer_entries = IOMMU_CMD_BUFFER_DEFAULT_ENTRIES;
static long amd_iommu_event_log_entries = IOMMU_EVENT_LOG_DEFAULT_ENTRIES;

unsigned short ivrs_bdf_entries;
struct ivrs_mappings *ivrs_mappings;
struct list_head amd_iommu_head;
struct table_struct device_table;

static int __init map_iommu_mmio_region(struct amd_iommu *iommu)
{
    unsigned long mfn;

    if ( nr_amd_iommus > MAX_AMD_IOMMUS )
    {
        amd_iov_error("nr_amd_iommus %d > MAX_IOMMUS\n", nr_amd_iommus);
        return -ENOMEM;
    }

    iommu->mmio_base = (void *)fix_to_virt(
        FIX_IOMMU_MMIO_BASE_0 + nr_amd_iommus * MMIO_PAGES_PER_IOMMU);
    mfn = (unsigned long)(iommu->mmio_base_phys >> PAGE_SHIFT);
    map_pages_to_xen((unsigned long)iommu->mmio_base, mfn,
                     MMIO_PAGES_PER_IOMMU, PAGE_HYPERVISOR_NOCACHE);

    memset(iommu->mmio_base, 0, IOMMU_MMIO_REGION_LENGTH);

    return 0;
}

static void __init unmap_iommu_mmio_region(struct amd_iommu *iommu)
{
    if ( iommu->mmio_base )
    {
        iounmap(iommu->mmio_base);
        iommu->mmio_base = NULL;
    }
}

static void register_iommu_dev_table_in_mmio_space(struct amd_iommu *iommu)
{
    u64 addr_64, addr_lo, addr_hi;
    u32 entry;

    addr_64 = (u64)virt_to_maddr(iommu->dev_table.buffer);
    addr_lo = addr_64 & DMA_32BIT_MASK;
    addr_hi = addr_64 >> 32;

    set_field_in_reg_u32((u32)addr_lo >> PAGE_SHIFT, 0,
                         IOMMU_DEV_TABLE_BASE_LOW_MASK,
                         IOMMU_DEV_TABLE_BASE_LOW_SHIFT, &entry);
    set_field_in_reg_u32((iommu->dev_table.alloc_size / PAGE_SIZE) - 1,
                         entry, IOMMU_DEV_TABLE_SIZE_MASK,
                         IOMMU_DEV_TABLE_SIZE_SHIFT, &entry);
    writel(entry, iommu->mmio_base + IOMMU_DEV_TABLE_BASE_LOW_OFFSET);

    set_field_in_reg_u32((u32)addr_hi, 0,
                         IOMMU_DEV_TABLE_BASE_HIGH_MASK,
                         IOMMU_DEV_TABLE_BASE_HIGH_SHIFT, &entry);
    writel(entry, iommu->mmio_base + IOMMU_DEV_TABLE_BASE_HIGH_OFFSET);
}

static void register_iommu_cmd_buffer_in_mmio_space(struct amd_iommu *iommu)
{
    u64 addr_64, addr_lo, addr_hi;
    u32 power_of2_entries;
    u32 entry;

    addr_64 = (u64)virt_to_maddr(iommu->cmd_buffer.buffer);
    addr_lo = addr_64 & DMA_32BIT_MASK;
    addr_hi = addr_64 >> 32;

    set_field_in_reg_u32((u32)addr_lo >> PAGE_SHIFT, 0,
                         IOMMU_CMD_BUFFER_BASE_LOW_MASK,
                         IOMMU_CMD_BUFFER_BASE_LOW_SHIFT, &entry);
    writel(entry, iommu->mmio_base + IOMMU_CMD_BUFFER_BASE_LOW_OFFSET);

    power_of2_entries = get_order_from_bytes(iommu->cmd_buffer.alloc_size) +
        IOMMU_CMD_BUFFER_POWER_OF2_ENTRIES_PER_PAGE;

    set_field_in_reg_u32((u32)addr_hi, 0,
                         IOMMU_CMD_BUFFER_BASE_HIGH_MASK,
                         IOMMU_CMD_BUFFER_BASE_HIGH_SHIFT, &entry);
    set_field_in_reg_u32(power_of2_entries, entry,
                         IOMMU_CMD_BUFFER_LENGTH_MASK,
                         IOMMU_CMD_BUFFER_LENGTH_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_CMD_BUFFER_BASE_HIGH_OFFSET);
}

static void __init register_iommu_event_log_in_mmio_space(struct amd_iommu *iommu)
{
    u64 addr_64, addr_lo, addr_hi;
    u32 power_of2_entries;
    u32 entry;

    addr_64 = (u64)virt_to_maddr(iommu->event_log.buffer);
    addr_lo = addr_64 & DMA_32BIT_MASK;
    addr_hi = addr_64 >> 32;

    set_field_in_reg_u32((u32)addr_lo >> PAGE_SHIFT, 0,
                         IOMMU_EVENT_LOG_BASE_LOW_MASK,
                         IOMMU_EVENT_LOG_BASE_LOW_SHIFT, &entry);
    writel(entry, iommu->mmio_base + IOMMU_EVENT_LOG_BASE_LOW_OFFSET);

    power_of2_entries = get_order_from_bytes(iommu->event_log.alloc_size) +
                        IOMMU_EVENT_LOG_POWER_OF2_ENTRIES_PER_PAGE;

    set_field_in_reg_u32((u32)addr_hi, 0,
                        IOMMU_EVENT_LOG_BASE_HIGH_MASK,
                        IOMMU_EVENT_LOG_BASE_HIGH_SHIFT, &entry);
    set_field_in_reg_u32(power_of2_entries, entry,
                        IOMMU_EVENT_LOG_LENGTH_MASK,
                        IOMMU_EVENT_LOG_LENGTH_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_EVENT_LOG_BASE_HIGH_OFFSET);
}

static void set_iommu_translation_control(struct amd_iommu *iommu,
                                                 int enable)
{
    u32 entry;

    entry = readl(iommu->mmio_base + IOMMU_CONTROL_MMIO_OFFSET);

    if ( enable )
    {
        set_field_in_reg_u32(iommu->ht_tunnel_support ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_HT_TUNNEL_TRANSLATION_MASK,
                         IOMMU_CONTROL_HT_TUNNEL_TRANSLATION_SHIFT, &entry);
        set_field_in_reg_u32(iommu->isochronous ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_ISOCHRONOUS_MASK,
                         IOMMU_CONTROL_ISOCHRONOUS_SHIFT, &entry);
        set_field_in_reg_u32(iommu->coherent ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_COHERENT_MASK,
                         IOMMU_CONTROL_COHERENT_SHIFT, &entry);
        set_field_in_reg_u32(iommu->res_pass_pw ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_RESP_PASS_POSTED_WRITE_MASK,
                         IOMMU_CONTROL_RESP_PASS_POSTED_WRITE_SHIFT, &entry);
        /* do not set PassPW bit */
        set_field_in_reg_u32(IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_PASS_POSTED_WRITE_MASK,
                         IOMMU_CONTROL_PASS_POSTED_WRITE_SHIFT, &entry);
    }
    set_field_in_reg_u32(enable ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_TRANSLATION_ENABLE_MASK,
                         IOMMU_CONTROL_TRANSLATION_ENABLE_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_CONTROL_MMIO_OFFSET);
}

static void set_iommu_command_buffer_control(struct amd_iommu *iommu,
                                                    int enable)
{
    u32 entry;

    entry = readl(iommu->mmio_base + IOMMU_CONTROL_MMIO_OFFSET);
    set_field_in_reg_u32(enable ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_COMMAND_BUFFER_ENABLE_MASK,
                         IOMMU_CONTROL_COMMAND_BUFFER_ENABLE_SHIFT, &entry);

    /*reset head and tail pointer manually before enablement */
    if ( enable == IOMMU_CONTROL_ENABLED )
    {
        writel(0x0, iommu->mmio_base + IOMMU_CMD_BUFFER_HEAD_OFFSET);
        writel(0x0, iommu->mmio_base + IOMMU_CMD_BUFFER_TAIL_OFFSET);
    }

    writel(entry, iommu->mmio_base+IOMMU_CONTROL_MMIO_OFFSET);
}

static void register_iommu_exclusion_range(struct amd_iommu *iommu)
{
    u64 addr_lo, addr_hi;
    u32 entry;

    addr_lo = iommu->exclusion_limit & DMA_32BIT_MASK;
    addr_hi = iommu->exclusion_limit >> 32;

    set_field_in_reg_u32((u32)addr_hi, 0,
                         IOMMU_EXCLUSION_LIMIT_HIGH_MASK,
                         IOMMU_EXCLUSION_LIMIT_HIGH_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_EXCLUSION_LIMIT_HIGH_OFFSET);

    set_field_in_reg_u32((u32)addr_lo >> PAGE_SHIFT, 0,
                         IOMMU_EXCLUSION_LIMIT_LOW_MASK,
                         IOMMU_EXCLUSION_LIMIT_LOW_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_EXCLUSION_LIMIT_LOW_OFFSET);

    addr_lo = iommu->exclusion_base & DMA_32BIT_MASK;
    addr_hi = iommu->exclusion_base >> 32;

    set_field_in_reg_u32((u32)addr_hi, 0,
                         IOMMU_EXCLUSION_BASE_HIGH_MASK,
                         IOMMU_EXCLUSION_BASE_HIGH_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_EXCLUSION_BASE_HIGH_OFFSET);

    set_field_in_reg_u32((u32)addr_lo >> PAGE_SHIFT, 0,
                         IOMMU_EXCLUSION_BASE_LOW_MASK,
                         IOMMU_EXCLUSION_BASE_LOW_SHIFT, &entry);

    set_field_in_reg_u32(iommu->exclusion_allow_all, entry,
                         IOMMU_EXCLUSION_ALLOW_ALL_MASK,
                         IOMMU_EXCLUSION_ALLOW_ALL_SHIFT, &entry);

    set_field_in_reg_u32(iommu->exclusion_enable, entry,
                         IOMMU_EXCLUSION_RANGE_ENABLE_MASK,
                         IOMMU_EXCLUSION_RANGE_ENABLE_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_EXCLUSION_BASE_LOW_OFFSET);
}

static void set_iommu_event_log_control(struct amd_iommu *iommu,
            int enable)
{
    u32 entry;

    entry = readl(iommu->mmio_base + IOMMU_CONTROL_MMIO_OFFSET);
    set_field_in_reg_u32(enable ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_EVENT_LOG_ENABLE_MASK,
                         IOMMU_CONTROL_EVENT_LOG_ENABLE_SHIFT, &entry);
    set_field_in_reg_u32(enable ? IOMMU_CONTROL_ENABLED :
                         IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_EVENT_LOG_INT_MASK,
                         IOMMU_CONTROL_EVENT_LOG_INT_SHIFT, &entry);
    set_field_in_reg_u32(IOMMU_CONTROL_DISABLED, entry,
                         IOMMU_CONTROL_COMP_WAIT_INT_MASK,
                         IOMMU_CONTROL_COMP_WAIT_INT_SHIFT, &entry);

    /*reset head and tail pointer manually before enablement */
    if ( enable == IOMMU_CONTROL_ENABLED )
    {
        writel(0x0, iommu->mmio_base + IOMMU_EVENT_LOG_HEAD_OFFSET);
        writel(0x0, iommu->mmio_base + IOMMU_EVENT_LOG_TAIL_OFFSET);
    }
    writel(entry, iommu->mmio_base + IOMMU_CONTROL_MMIO_OFFSET);
}

static int amd_iommu_read_event_log(struct amd_iommu *iommu, u32 event[])
{
    u32 tail, head, *event_log;
    int i;

     BUG_ON( !iommu || !event );

    /* make sure there's an entry in the log */
    tail = get_field_from_reg_u32(
                readl(iommu->mmio_base + IOMMU_EVENT_LOG_TAIL_OFFSET),
                IOMMU_EVENT_LOG_TAIL_MASK,
                IOMMU_EVENT_LOG_TAIL_SHIFT);
    if ( tail != iommu->event_log_head )
    {
        /* read event log entry */
        event_log = (u32 *)(iommu->event_log.buffer +
                                        (iommu->event_log_head *
                                        IOMMU_EVENT_LOG_ENTRY_SIZE));
        for ( i = 0; i < IOMMU_EVENT_LOG_U32_PER_ENTRY; i++ )
            event[i] = event_log[i];
        if ( ++iommu->event_log_head == iommu->event_log.entries )
            iommu->event_log_head = 0;

        /* update head pointer */
        set_field_in_reg_u32(iommu->event_log_head, 0,
                             IOMMU_EVENT_LOG_HEAD_MASK,
                             IOMMU_EVENT_LOG_HEAD_SHIFT, &head);
        writel(head, iommu->mmio_base + IOMMU_EVENT_LOG_HEAD_OFFSET);
        return 0;
    }

    return -EFAULT;
}

static void amd_iommu_msi_data_init(struct amd_iommu *iommu)
{
    u32 msi_data;
    u8 bus = (iommu->bdf >> 8) & 0xff;
    u8 dev = PCI_SLOT(iommu->bdf & 0xff);
    u8 func = PCI_FUNC(iommu->bdf & 0xff);
    int vector = iommu->vector;

    msi_data = MSI_DATA_TRIGGER_EDGE |
        MSI_DATA_LEVEL_ASSERT |
        MSI_DATA_DELIVERY_FIXED |
        MSI_DATA_VECTOR(vector);

    pci_conf_write32(bus, dev, func,
        iommu->msi_cap + PCI_MSI_DATA_64, msi_data);
}

static void amd_iommu_msi_addr_init(struct amd_iommu *iommu, int phy_cpu)
{

    int bus = (iommu->bdf >> 8) & 0xff;
    int dev = PCI_SLOT(iommu->bdf & 0xff);
    int func = PCI_FUNC(iommu->bdf & 0xff);

    u32 address_hi = 0;
    u32 address_lo = MSI_ADDR_HEADER |
            MSI_ADDR_DESTMODE_PHYS |
            MSI_ADDR_REDIRECTION_CPU |
            MSI_ADDR_DEST_ID(phy_cpu);

    pci_conf_write32(bus, dev, func,
        iommu->msi_cap + PCI_MSI_ADDRESS_LO, address_lo);
    pci_conf_write32(bus, dev, func,
        iommu->msi_cap + PCI_MSI_ADDRESS_HI, address_hi);
}

static void amd_iommu_msi_enable(struct amd_iommu *iommu, int flag)
{
    u16 control;
    int bus = (iommu->bdf >> 8) & 0xff;
    int dev = PCI_SLOT(iommu->bdf & 0xff);
    int func = PCI_FUNC(iommu->bdf & 0xff);

    control = pci_conf_read16(bus, dev, func,
        iommu->msi_cap + PCI_MSI_FLAGS);
    control &= ~(1);
    if ( flag )
        control |= flag;
    pci_conf_write16(bus, dev, func,
        iommu->msi_cap + PCI_MSI_FLAGS, control);
}

static void iommu_msi_unmask(unsigned int vector)
{
    unsigned long flags;
    struct amd_iommu *iommu = vector_to_iommu[vector];

    /* FIXME: do not support mask bits at the moment */
    if ( iommu->maskbit )
        return;

    spin_lock_irqsave(&iommu->lock, flags);
    amd_iommu_msi_enable(iommu, IOMMU_CONTROL_ENABLED);
    spin_unlock_irqrestore(&iommu->lock, flags);
}

static void iommu_msi_mask(unsigned int vector)
{
    unsigned long flags;
    struct amd_iommu *iommu = vector_to_iommu[vector];

    /* FIXME: do not support mask bits at the moment */
    if ( iommu->maskbit )
        return;

    spin_lock_irqsave(&iommu->lock, flags);
    amd_iommu_msi_enable(iommu, IOMMU_CONTROL_DISABLED);
    spin_unlock_irqrestore(&iommu->lock, flags);
}

static unsigned int iommu_msi_startup(unsigned int vector)
{
    iommu_msi_unmask(vector);
    return 0;
}

static void iommu_msi_end(unsigned int vector)
{
    iommu_msi_unmask(vector);
    ack_APIC_irq();
}

static void iommu_msi_set_affinity(unsigned int vector, cpumask_t dest)
{
    struct amd_iommu *iommu = vector_to_iommu[vector];
    amd_iommu_msi_addr_init(iommu, cpu_physical_id(first_cpu(dest)));
}

static struct hw_interrupt_type iommu_msi_type = {
    .typename = "AMD_IOV_MSI",
    .startup = iommu_msi_startup,
    .shutdown = iommu_msi_mask,
    .enable = iommu_msi_unmask,
    .disable = iommu_msi_mask,
    .ack = iommu_msi_mask,
    .end = iommu_msi_end,
    .set_affinity = iommu_msi_set_affinity,
};

static void parse_event_log_entry(u32 entry[])
{
    u16 domain_id, device_id;
    u32 code;
    u64 *addr;
    char * event_str[] = {"ILLEGAL_DEV_TABLE_ENTRY",
                          "IO_PAGE_FALT",
                          "DEV_TABLE_HW_ERROR",
                          "PAGE_TABLE_HW_ERROR",
                          "ILLEGAL_COMMAND_ERROR",
                          "COMMAND_HW_ERROR",
                          "IOTLB_INV_TIMEOUT",
                          "INVALID_DEV_REQUEST"};

    code = get_field_from_reg_u32(entry[1], IOMMU_EVENT_CODE_MASK,
                                            IOMMU_EVENT_CODE_SHIFT);

    if ( (code > IOMMU_EVENT_INVALID_DEV_REQUEST) ||
        (code < IOMMU_EVENT_ILLEGAL_DEV_TABLE_ENTRY) )
    {
        amd_iov_error("Invalid event log entry!\n");
        return;
    }

    if ( code == IOMMU_EVENT_IO_PAGE_FALT )
    {
        device_id = get_field_from_reg_u32(entry[0],
                                           IOMMU_EVENT_DEVICE_ID_MASK,
                                           IOMMU_EVENT_DEVICE_ID_SHIFT);
        domain_id = get_field_from_reg_u32(entry[1],
                                           IOMMU_EVENT_DOMAIN_ID_MASK,
                                           IOMMU_EVENT_DOMAIN_ID_SHIFT);
        addr= (u64*) (entry + 2);
        printk(XENLOG_ERR "AMD_IOV: "
            "%s: domain:%d, device id:0x%x, fault address:0x%"PRIx64"\n",
            event_str[code-1], domain_id, device_id, *addr);
    }
}

static void amd_iommu_page_fault(int vector, void *dev_id,
                             struct cpu_user_regs *regs)
{
    u32 event[4];
    u32 entry;
    unsigned long flags;
    int ret = 0;
    struct amd_iommu *iommu = dev_id;

    spin_lock_irqsave(&iommu->lock, flags);
    ret = amd_iommu_read_event_log(iommu, event);
    /* reset interrupt status bit */
    entry = readl(iommu->mmio_base + IOMMU_STATUS_MMIO_OFFSET);
    set_field_in_reg_u32(IOMMU_CONTROL_ENABLED, entry,
                         IOMMU_STATUS_EVENT_LOG_INT_MASK,
                         IOMMU_STATUS_EVENT_LOG_INT_SHIFT, &entry);
    writel(entry, iommu->mmio_base+IOMMU_STATUS_MMIO_OFFSET);
    spin_unlock_irqrestore(&iommu->lock, flags);

    if ( ret != 0 )
        return;
    parse_event_log_entry(event);
}

static int set_iommu_interrupt_handler(struct amd_iommu *iommu)
{
    int vector, ret;

    vector = assign_irq_vector(AUTO_ASSIGN_IRQ);
    if ( vector <= 0 )
    {
        gdprintk(XENLOG_ERR VTDPREFIX, "IOMMU: no vectors\n");
        return 0;
    }

    irq_desc[vector].handler = &iommu_msi_type;
    vector_to_iommu[vector] = iommu;
    ret = request_irq_vector(vector, amd_iommu_page_fault, 0,
                             "amd_iommu", iommu);
    if ( ret )
    {
        irq_desc[vector].handler = &no_irq_type;
        vector_to_iommu[vector] = NULL;
        free_irq_vector(vector);
        amd_iov_error("can't request irq\n");
        return 0;
    }

    /* Make sure that vector is never re-used. */
    vector_irq[vector] = NEVER_ASSIGN_IRQ;
    iommu->vector = vector;
    return vector;
}

void enable_iommu(struct amd_iommu *iommu)
{
    unsigned long flags;

    spin_lock_irqsave(&iommu->lock, flags);

    if ( iommu->enabled )
    {
        spin_unlock_irqrestore(&iommu->lock, flags); 
        return;
    }

    register_iommu_dev_table_in_mmio_space(iommu);
    register_iommu_cmd_buffer_in_mmio_space(iommu);
    register_iommu_event_log_in_mmio_space(iommu);
    register_iommu_exclusion_range(iommu);

    amd_iommu_msi_data_init (iommu);
    amd_iommu_msi_addr_init(iommu, cpu_physical_id(first_cpu(cpu_online_map)));
    amd_iommu_msi_enable(iommu, IOMMU_CONTROL_ENABLED);

    set_iommu_command_buffer_control(iommu, IOMMU_CONTROL_ENABLED);
    set_iommu_event_log_control(iommu, IOMMU_CONTROL_ENABLED);
    set_iommu_translation_control(iommu, IOMMU_CONTROL_ENABLED);

    iommu->enabled = 1;
    spin_unlock_irqrestore(&iommu->lock, flags);

}

static void __init deallocate_iommu_table_struct(
    struct table_struct *table)
{
    int order = 0;
    if ( table->buffer )
    {
        order = get_order_from_bytes(table->alloc_size);
        __free_amd_iommu_tables(table->buffer, order);
        table->buffer = NULL;
    }
}

static void __init deallocate_iommu_tables(struct amd_iommu *iommu)
{
    deallocate_iommu_table_struct(&iommu->cmd_buffer);
    deallocate_iommu_table_struct(&iommu->event_log);
}

static int __init allocate_iommu_table_struct(struct table_struct *table,
                                              const char *name)
{
    int order = 0;
    if ( table->buffer == NULL )
    {
        order = get_order_from_bytes(table->alloc_size);
        table->buffer = __alloc_amd_iommu_tables(order);

        if ( table->buffer == NULL )
        {
            amd_iov_error("Error allocating %s\n", name);
            return -ENOMEM;
        }
        memset(table->buffer, 0, PAGE_SIZE * (1UL << order));
    }
    return 0;
}

static int __init allocate_iommu_tables(struct amd_iommu *iommu)
{
    /* allocate 'command buffer' in power of 2 increments of 4K */
    iommu->cmd_buffer_tail = 0;
    iommu->cmd_buffer.alloc_size = PAGE_SIZE <<
                                   get_order_from_bytes(
                                   PAGE_ALIGN(amd_iommu_cmd_buffer_entries *
                                   IOMMU_CMD_BUFFER_ENTRY_SIZE));
    iommu->cmd_buffer.entries = iommu->cmd_buffer.alloc_size /
                                IOMMU_CMD_BUFFER_ENTRY_SIZE;

    if ( allocate_iommu_table_struct(&iommu->cmd_buffer, "Command Buffer") != 0 )
        goto error_out;

    /* allocate 'event log' in power of 2 increments of 4K */
    iommu->event_log_head = 0;
    iommu->event_log.alloc_size = PAGE_SIZE <<
                                  get_order_from_bytes(
                                  PAGE_ALIGN(amd_iommu_event_log_entries *
                                  IOMMU_EVENT_LOG_ENTRY_SIZE));
    iommu->event_log.entries = iommu->event_log.alloc_size /
                               IOMMU_EVENT_LOG_ENTRY_SIZE;

    if ( allocate_iommu_table_struct(&iommu->event_log, "Event Log") != 0 )
        goto error_out;

    return 0;

 error_out:
    deallocate_iommu_tables(iommu);
    return -ENOMEM;
}

int __init amd_iommu_init_one(struct amd_iommu *iommu)
{
    if ( allocate_iommu_tables(iommu) != 0 )
        goto error_out;

    if ( map_iommu_mmio_region(iommu) != 0 )
        goto error_out;

    if ( set_iommu_interrupt_handler(iommu) == 0 )
        goto error_out;

    /* To make sure that device_table.buffer has been successfully allocated */
    if ( device_table.buffer == NULL )
        goto error_out;

    iommu->dev_table.alloc_size = device_table.alloc_size;
    iommu->dev_table.entries = device_table.entries;
    iommu->dev_table.buffer = device_table.buffer;

    enable_iommu(iommu);
    printk("AMD-Vi: IOMMU %d Enabled.\n", nr_amd_iommus );
    nr_amd_iommus++;

    return 0;

error_out:
    return -ENODEV;
}

void __init amd_iommu_init_cleanup(void)
{
    struct amd_iommu *iommu, *next;

    list_for_each_entry_safe ( iommu, next, &amd_iommu_head, list )
    {
        list_del(&iommu->list);
        if ( iommu->enabled )
        {
            deallocate_iommu_tables(iommu);
            unmap_iommu_mmio_region(iommu);
        }
        xfree(iommu);
    }
}

static int __init init_ivrs_mapping(void)
{
    int bdf;

    BUG_ON( !ivrs_bdf_entries );

    ivrs_mappings = xmalloc_array( struct ivrs_mappings, ivrs_bdf_entries);
    if ( ivrs_mappings == NULL )
    {
        amd_iov_error("Error allocating IVRS Mappings table\n");
        return -ENOMEM;
    }
    memset(ivrs_mappings, 0, ivrs_bdf_entries * sizeof(struct ivrs_mappings));

    /* assign default values for device entries */
    for ( bdf = 0; bdf < ivrs_bdf_entries; bdf++ )
    {
        ivrs_mappings[bdf].dte_requestor_id = bdf;
        ivrs_mappings[bdf].dte_sys_mgt_enable =
            IOMMU_DEV_TABLE_SYS_MGT_MSG_FORWARDED;
        ivrs_mappings[bdf].dte_allow_exclusion = IOMMU_CONTROL_DISABLED;
        ivrs_mappings[bdf].unity_map_enable = IOMMU_CONTROL_DISABLED;
        ivrs_mappings[bdf].iommu = NULL;
    }
    return 0;
}

static int __init amd_iommu_setup_device_table(void)
{
    /* allocate 'device table' on a 4K boundary */
    device_table.alloc_size = PAGE_SIZE <<
                              get_order_from_bytes(
                              PAGE_ALIGN(ivrs_bdf_entries *
                              IOMMU_DEV_TABLE_ENTRY_SIZE));
    device_table.entries = device_table.alloc_size /
                           IOMMU_DEV_TABLE_ENTRY_SIZE;

    return ( allocate_iommu_table_struct(&device_table, "Device Table") );
}

int __init amd_iommu_setup_shared_tables(void)
{
    BUG_ON( !ivrs_bdf_entries );

    if ( init_ivrs_mapping() != 0 )
        goto error_out;

    if ( amd_iommu_setup_device_table() != 0 )
        goto error_out;

    if ( amd_iommu_setup_intremap_table() != 0 )
        goto error_out;

    return 0;

error_out:
    deallocate_intremap_table();
    deallocate_iommu_table_struct(&device_table);

    if ( ivrs_mappings )
    {
        xfree(ivrs_mappings);
        ivrs_mappings = NULL;
    }
    return -ENOMEM;
}

static void disable_iommu(struct amd_iommu *iommu)
{
    unsigned long flags;

    spin_lock_irqsave(&iommu->lock, flags);

    if ( !iommu->enabled )
    {
        spin_unlock_irqrestore(&iommu->lock, flags); 
        return;
    }

    amd_iommu_msi_enable(iommu, IOMMU_CONTROL_DISABLED);
    set_iommu_command_buffer_control(iommu, IOMMU_CONTROL_DISABLED);
    set_iommu_event_log_control(iommu, IOMMU_CONTROL_DISABLED);
    set_iommu_translation_control(iommu, IOMMU_CONTROL_DISABLED);

    iommu->enabled = 0;

    spin_unlock_irqrestore(&iommu->lock, flags);

}

static void invalidate_all_domain_pages(void)
{
    struct domain *d;
    for_each_domain( d )
        invalidate_all_iommu_pages(d);
}

static void invalidate_all_devices(void)
{
    u16 bus, devfn, bdf, req_id;
    unsigned long flags;
    struct amd_iommu *iommu;

    for ( bdf = 0; bdf < ivrs_bdf_entries; bdf++ )
    {
        bus = bdf >> 8;
        devfn = bdf & 0xFF;
        iommu = find_iommu_for_device(bus, devfn);
        req_id = ivrs_mappings[bdf].dte_requestor_id;
        if ( iommu )
        {
            spin_lock_irqsave(&iommu->lock, flags);
            invalidate_dev_table_entry(iommu, req_id);
            invalidate_interrupt_table(iommu, req_id);
            flush_command_buffer(iommu);
            spin_unlock_irqrestore(&iommu->lock, flags);
        }
    }
}

void amd_iommu_suspend(void)
{
    struct amd_iommu *iommu;

    for_each_amd_iommu ( iommu )
        disable_iommu(iommu);
}

void amd_iommu_resume(void)
{
    struct amd_iommu *iommu;

    for_each_amd_iommu ( iommu )
    {
       /*
        * To make sure that iommus have not been touched 
        * before re-enablement
        */
        disable_iommu(iommu);
        enable_iommu(iommu);
    }

    /* flush all cache entries after iommu re-enabled */
    invalidate_all_devices();
    invalidate_all_domain_pages();
}
