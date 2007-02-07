/* 
 * hvm/save.h
 *
 * Structure definitions for HVM state that is held by Xen and must
 * be saved along with the domain's memory and device-model state.
 *
 * 
 * Copyright (c) 2007 XenSource Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __XEN_PUBLIC_HVM_SAVE_H__
#define __XEN_PUBLIC_HVM_SAVE_H__

/*
 * Structures in this header *must* have the same layout in 32bit 
 * and 64bit environments: this means that all fields must be explicitly 
 * sized types and aligned to their sizes.
 *
 * Only the state necessary for saving and restoring (i.e. fields 
 * that are analogous to actual hardware state) should go in this file. 
 * Internal mechanisms should be kept in Xen-private headers.
 */

/* 
 * Each entry is preceded by a descriptor giving its type and length
 */
struct hvm_save_descriptor {
    uint16_t typecode;          /* Used to demux the various types below */
    uint16_t instance;          /* Further demux within a type */
    uint32_t length;            /* In bytes, *not* including this descriptor */
};


/* 
 * Each entry has a datatype associated with it: for example, the CPU state 
 * is saved as a HVM_SAVE_TYPE(CPU), which has HVM_SAVE_LENGTH(CPU), 
 * and is identified by a descriptor with typecode HVM_SAVE_CODE(CPU).
 * DECLARE_HVM_SAVE_TYPE binds these things together with some type-system
 * ugliness.
 */

#define DECLARE_HVM_SAVE_TYPE(_x, _code, _type)                   \
  struct __HVM_SAVE_TYPE_##_x { _type t; char c[_code]; }

#define HVM_SAVE_TYPE(_x) typeof (((struct __HVM_SAVE_TYPE_##_x *)(0))->t)
#define HVM_SAVE_LENGTH(_x) (sizeof (HVM_SAVE_TYPE(_x)))
#define HVM_SAVE_CODE(_x) (sizeof (((struct __HVM_SAVE_TYPE_##_x *)(0))->c))


/* 
 * Save/restore header: general info about the save file. 
 */

#define HVM_FILE_MAGIC   0x54381286
#define HVM_FILE_VERSION 0x00000001

struct hvm_save_header {
    uint32_t magic;             /* Must be HVM_FILE_MAGIC */
    uint32_t version;           /* File format version */
    uint64_t changeset;         /* Version of Xen that saved this file */
    uint32_t cpuid;             /* CPUID[0x01][%eax] on the saving machine */
};

DECLARE_HVM_SAVE_TYPE(HEADER, 1, struct hvm_save_header);


/*
 * Processor
 */

struct hvm_hw_cpu {
    uint64_t eip;
    uint64_t esp;
    uint64_t eflags;
    uint64_t cr0;
    uint64_t cr3;
    uint64_t cr4;

    uint32_t cs_sel;
    uint32_t ds_sel;
    uint32_t es_sel;
    uint32_t fs_sel;
    uint32_t gs_sel;
    uint32_t ss_sel;
    uint32_t tr_sel;
    uint32_t ldtr_sel;

    uint32_t cs_limit;
    uint32_t ds_limit;
    uint32_t es_limit;
    uint32_t fs_limit;
    uint32_t gs_limit;
    uint32_t ss_limit;
    uint32_t tr_limit;
    uint32_t ldtr_limit;
    uint32_t idtr_limit;
    uint32_t gdtr_limit;

    uint64_t cs_base;
    uint64_t ds_base;
    uint64_t es_base;
    uint64_t fs_base;
    uint64_t gs_base;
    uint64_t ss_base;
    uint64_t tr_base;
    uint64_t ldtr_base;
    uint64_t idtr_base;
    uint64_t gdtr_base;

    uint32_t cs_arbytes;
    uint32_t ds_arbytes;
    uint32_t es_arbytes;
    uint32_t fs_arbytes;
    uint32_t gs_arbytes;
    uint32_t ss_arbytes;
    uint32_t tr_arbytes;
    uint32_t ldtr_arbytes;

    uint32_t sysenter_cs;
    uint32_t padding0;

    uint64_t sysenter_esp;
    uint64_t sysenter_eip;

    /* MSRs */
    uint64_t shadow_gs;
    uint64_t flags;
    uint64_t msr_items[6];

    /* guest's idea of what rdtsc() would return */
    uint64_t tsc;
};

DECLARE_HVM_SAVE_TYPE(CPU, 2, struct hvm_hw_cpu);


/*
 * PIC
 */

struct hvm_hw_vpic {
    /* IR line bitmasks. */
    uint8_t irr;
    uint8_t imr;
    uint8_t isr;

    /* Line IRx maps to IRQ irq_base+x */
    uint8_t irq_base;

    /*
     * Where are we in ICW2-4 initialisation (0 means no init in progress)?
     * Bits 0-1 (=x): Next write at A=1 sets ICW(x+1).
     * Bit 2: ICW1.IC4  (1 == ICW4 included in init sequence)
     * Bit 3: ICW1.SNGL (0 == ICW3 included in init sequence)
     */
    uint8_t init_state:4;

    /* IR line with highest priority. */
    uint8_t priority_add:4;

    /* Reads from A=0 obtain ISR or IRR? */
    uint8_t readsel_isr:1;

    /* Reads perform a polling read? */
    uint8_t poll:1;

    /* Automatically clear IRQs from the ISR during INTA? */
    uint8_t auto_eoi:1;

    /* Automatically rotate IRQ priorities during AEOI? */
    uint8_t rotate_on_auto_eoi:1;

    /* Exclude slave inputs when considering in-service IRQs? */
    uint8_t special_fully_nested_mode:1;

    /* Special mask mode excludes masked IRs from AEOI and priority checks. */
    uint8_t special_mask_mode:1;

    /* Is this a master PIC or slave PIC? (NB. This is not programmable.) */
    uint8_t is_master:1;

    /* Edge/trigger selection. */
    uint8_t elcr;

    /* Virtual INT output. */
    uint8_t int_output;
};

DECLARE_HVM_SAVE_TYPE(PIC, 3, struct hvm_hw_vpic);


/*
 * IO-APIC
 */

#ifdef __ia64__
#define VIOAPIC_IS_IOSAPIC 1
#define VIOAPIC_NUM_PINS  24
#else
#define VIOAPIC_NUM_PINS  48 /* 16 ISA IRQs, 32 non-legacy PCI IRQS. */
#endif

struct hvm_hw_vioapic {
    uint64_t base_address;
    uint32_t ioregsel;
    uint32_t id;
    union vioapic_redir_entry
    {
        uint64_t bits;
        struct {
            uint8_t vector;
            uint8_t delivery_mode:3;
            uint8_t dest_mode:1;
            uint8_t delivery_status:1;
            uint8_t polarity:1;
            uint8_t remote_irr:1;
            uint8_t trig_mode:1;
            uint8_t mask:1;
            uint8_t reserve:7;
#if !VIOAPIC_IS_IOSAPIC
            uint8_t reserved[4];
            uint8_t dest_id;
#else
            uint8_t reserved[3];
            uint16_t dest_id;
#endif
        } fields;
    } redirtbl[VIOAPIC_NUM_PINS];
};

DECLARE_HVM_SAVE_TYPE(IOAPIC, 4, struct hvm_hw_vioapic);


/*
 * LAPIC
 */

struct hvm_hw_lapic {
    uint64_t             apic_base_msr;
    uint32_t             disabled; /* VLAPIC_xx_DISABLED */
    uint32_t             timer_divisor;
};

DECLARE_HVM_SAVE_TYPE(LAPIC, 5, struct hvm_hw_lapic);

struct hvm_hw_lapic_regs {
    /* A 4k page of register state */
    uint8_t  data[0x400];
};

DECLARE_HVM_SAVE_TYPE(LAPIC_REGS, 6, struct hvm_hw_lapic_regs);


/*
 * IRQs
 */

struct hvm_hw_pci_irqs {
    /*
     * Virtual interrupt wires for a single PCI bus.
     * Indexed by: device*4 + INTx#.
     */
    union {
        DECLARE_BITMAP(i, 32*4);
        uint64_t pad[2];
    };
};

DECLARE_HVM_SAVE_TYPE(PCI_IRQ, 7, struct hvm_hw_pci_irqs);

struct hvm_hw_isa_irqs {
    /*
     * Virtual interrupt wires for ISA devices.
     * Indexed by ISA IRQ (assumes no ISA-device IRQ sharing).
     */
    union {
        DECLARE_BITMAP(i, 16);
        uint64_t pad[1];
    };
};

DECLARE_HVM_SAVE_TYPE(ISA_IRQ, 8, struct hvm_hw_isa_irqs);

struct hvm_hw_pci_link {
    /*
     * PCI-ISA interrupt router.
     * Each PCI <device:INTx#> is 'wire-ORed' into one of four links using
     * the traditional 'barber's pole' mapping ((device + INTx#) & 3).
     * The router provides a programmable mapping from each link to a GSI.
     */
    u8 route[4];
};

DECLARE_HVM_SAVE_TYPE(PCI_LINK, 9, struct hvm_hw_pci_link);


/* 
 *  PIT
 */

struct hvm_hw_pit {
    struct hvm_hw_pit_channel {
        uint32_t count; /* can be 65536 */
        uint16_t latched_count;
        uint8_t count_latched;
        uint8_t status_latched;
        uint8_t status;
        uint8_t read_state;
        uint8_t write_state;
        uint8_t write_latch;
        uint8_t rw_mode;
        uint8_t mode;
        uint8_t bcd; /* not supported */
        uint8_t gate; /* timer start */
    } channels[3];  /* 3 x 16 bytes */
    uint32_t speaker_data_on;
};

DECLARE_HVM_SAVE_TYPE(PIT, 10, struct hvm_hw_pit);


/* 
 * RTC
 */ 

#define RTC_CMOS_SIZE 14
struct hvm_hw_rtc {
    /* CMOS bytes */
    uint8_t cmos_data[RTC_CMOS_SIZE];
    /* Index register for 2-part operations */
    uint8_t cmos_index;
};

DECLARE_HVM_SAVE_TYPE(RTC, 11, struct hvm_hw_rtc);


/*
 * HPET
 */

#define HPET_TIMER_NUM     3    /* 3 timers supported now */
struct hvm_hw_hpet {
    /* Memory-mapped, software visible registers */
    uint64_t capability;        /* capabilities */
    uint64_t res0;              /* reserved */
    uint64_t config;            /* configuration */
    uint64_t res1;              /* reserved */
    uint64_t isr;               /* interrupt status reg */
    uint64_t res2[25];          /* reserved */
    uint64_t mc64;              /* main counter */
    uint64_t res3;              /* reserved */
    struct {                    /* timers */
        uint64_t config;        /* configuration/cap */
        uint64_t cmp;           /* comparator */
        uint64_t fsb;           /* FSB route, not supported now */
        uint64_t res4;          /* reserved */
    } timers[HPET_TIMER_NUM];
    uint64_t res5[4*(24-HPET_TIMER_NUM)];  /* reserved, up to 0x3ff */

    /* Hidden register state */
    uint64_t period[HPET_TIMER_NUM]; /* Last value written to comparator */
};

DECLARE_HVM_SAVE_TYPE(HPET, 12, struct hvm_hw_hpet);


/* 
 * Largest type-code in use
 */
#define HVM_SAVE_CODE_MAX 12


/* 
 * The series of save records is teminated by a zero-type, zero-length 
 * descriptor.
 */

struct hvm_save_end {};
DECLARE_HVM_SAVE_TYPE(END, 0, struct hvm_save_end);

#endif /* __XEN_PUBLIC_HVM_SAVE_H__ */
