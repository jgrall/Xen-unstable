/*
 *  Xen domain firmware emulation support
 *  Copyright (C) 2004 Hewlett-Packard Co.
 *       Dan Magenheimer (dan.magenheimer@hp.com)
 *
 */

#include <xen/config.h>
#include <asm/system.h>
#include <asm/pgalloc.h>

#ifdef CONFIG_PCI
# include <linux/pci.h>
#endif

#include <linux/efi.h>
#include <asm/io.h>
#include <asm/pal.h>
#include <asm/sal.h>
#include <xen/acpi.h>

#include <asm/dom_fw.h>

struct ia64_boot_param *dom_fw_init(struct domain *, char *,int,char *,int);
extern unsigned long domain_mpa_to_imva(struct domain *,unsigned long mpaddr);
extern struct domain *dom0;
extern unsigned long dom0_start;

extern unsigned long running_on_sim;


unsigned long dom_fw_base_mpa = -1;
unsigned long imva_fw_base = -1;

// return domain (meta)physical address for a given imva
// this function is a call-back from dom_fw_init
unsigned long dom_pa(unsigned long imva)
{
	if (dom_fw_base_mpa == -1 || imva_fw_base == -1) {
		printf("dom_pa: uninitialized! (spinning...)\n");
		while(1);
	}
	if (imva - imva_fw_base > PAGE_SIZE) {
		printf("dom_pa: bad offset! imva=%p, imva_fw_base=%p (spinning...)\n",imva,imva_fw_base);
		while(1);
	}
	return dom_fw_base_mpa + (imva - imva_fw_base);
}

// builds a hypercall bundle at domain physical address
void dom_efi_hypercall_patch(struct domain *d, unsigned long paddr, unsigned long hypercall)
{
	unsigned long imva;

	if (d == dom0) paddr += dom0_start;
	imva = domain_mpa_to_imva(d,paddr);
	build_hypercall_bundle(imva,d->breakimm,hypercall,1);
}


// builds a hypercall bundle at domain physical address
void dom_fw_hypercall_patch(struct domain *d, unsigned long paddr, unsigned long hypercall,unsigned long ret)
{
	unsigned long imva;

	if (d == dom0) paddr += dom0_start;
	imva = domain_mpa_to_imva(d,paddr);
	build_hypercall_bundle(imva,d->breakimm,hypercall,ret);
}


// FIXME: This is really a hack: Forcing the boot parameter block
// at domain mpaddr 0 page, then grabbing only the low bits of the
// Xen imva, which is the offset into the page
unsigned long dom_fw_setup(struct domain *d, char *args, int arglen)
{
	struct ia64_boot_param *bp;

	dom_fw_base_mpa = 0;
	if (d == dom0) dom_fw_base_mpa += dom0_start;
	imva_fw_base = domain_mpa_to_imva(d,dom_fw_base_mpa);
	bp = dom_fw_init(d,args,arglen,imva_fw_base,PAGE_SIZE);
	return dom_pa((unsigned long)bp);
}


/* the following heavily leveraged from linux/arch/ia64/hp/sim/fw-emu.c */

#define MB	(1024*1024UL)

#define NUM_EFI_SYS_TABLES 6
#define PASS_THRU_IOPORT_SPACE
#ifdef PASS_THRU_IOPORT_SPACE
# define NUM_MEM_DESCS	4
#else
# define NUM_MEM_DESCS	3
#endif


#define SECS_PER_HOUR   (60 * 60)
#define SECS_PER_DAY    (SECS_PER_HOUR * 24)

/* Compute the `struct tm' representation of *T,
   offset OFFSET seconds east of UTC,
   and store year, yday, mon, mday, wday, hour, min, sec into *TP.
   Return nonzero if successful.  */
int
offtime (unsigned long t, efi_time_t *tp)
{
	const unsigned short int __mon_yday[2][13] =
	{
		/* Normal years.  */
		{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
		/* Leap years.  */
		{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
	};
	long int days, rem, y;
	const unsigned short int *ip;

	days = t / SECS_PER_DAY;
	rem = t % SECS_PER_DAY;
	while (rem < 0) {
		rem += SECS_PER_DAY;
		--days;
	}
	while (rem >= SECS_PER_DAY) {
		rem -= SECS_PER_DAY;
		++days;
	}
	tp->hour = rem / SECS_PER_HOUR;
	rem %= SECS_PER_HOUR;
	tp->minute = rem / 60;
	tp->second = rem % 60;
	/* January 1, 1970 was a Thursday.  */
	y = 1970;

#	define DIV(a, b) ((a) / (b) - ((a) % (b) < 0))
#	define LEAPS_THRU_END_OF(y) (DIV (y, 4) - DIV (y, 100) + DIV (y, 400))
#	define __isleap(year) \
	  ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))

	while (days < 0 || days >= (__isleap (y) ? 366 : 365)) {
		/* Guess a corrected year, assuming 365 days per year.  */
		long int yg = y + days / 365 - (days % 365 < 0);

		/* Adjust DAYS and Y to match the guessed year.  */
		days -= ((yg - y) * 365 + LEAPS_THRU_END_OF (yg - 1)
			 - LEAPS_THRU_END_OF (y - 1));
		y = yg;
	}
	tp->year = y;
	ip = __mon_yday[__isleap(y)];
	for (y = 11; days < (long int) ip[y]; --y)
		continue;
	days -= ip[y];
	tp->month = y + 1;
	tp->day = days + 1;
	return 1;
}

extern void pal_emulator_static (void);

/* Macro to emulate SAL call using legacy IN and OUT calls to CF8, CFC etc.. */

#define BUILD_CMD(addr)		((0x80000000 | (addr)) & ~3)

#define REG_OFFSET(addr)	(0x00000000000000FF & (addr))
#define DEVICE_FUNCTION(addr)	(0x000000000000FF00 & (addr))
#define BUS_NUMBER(addr)	(0x0000000000FF0000 & (addr))

#ifndef XEN
static efi_status_t
fw_efi_get_time (efi_time_t *tm, efi_time_cap_t *tc)
{
#if defined(CONFIG_IA64_HP_SIM) || defined(CONFIG_IA64_GENERIC)
	struct {
		int tv_sec;	/* must be 32bits to work */
		int tv_usec;
	} tv32bits;

	ssc((unsigned long) &tv32bits, 0, 0, 0, SSC_GET_TOD);

	memset(tm, 0, sizeof(*tm));
	offtime(tv32bits.tv_sec, tm);

	if (tc)
		memset(tc, 0, sizeof(*tc));
#else
#	error Not implemented yet...
#endif
	return EFI_SUCCESS;
}

static void
efi_reset_system (int reset_type, efi_status_t status, unsigned long data_size, efi_char16_t *data)
{
#if defined(CONFIG_IA64_HP_SIM) || defined(CONFIG_IA64_GENERIC)
	ssc(status, 0, 0, 0, SSC_EXIT);
#else
#	error Not implemented yet...
#endif
}

static efi_status_t
efi_unimplemented (void)
{
	return EFI_UNSUPPORTED;
}
#endif /* !XEN */

struct sal_ret_values
sal_emulator (long index, unsigned long in1, unsigned long in2,
	      unsigned long in3, unsigned long in4, unsigned long in5,
	      unsigned long in6, unsigned long in7)
{
	long r9  = 0;
	long r10 = 0;
	long r11 = 0;
	long status;

	/*
	 * Don't do a "switch" here since that gives us code that
	 * isn't self-relocatable.
	 */
	status = 0;
	if (index == SAL_FREQ_BASE) {
		switch (in1) {
		      case SAL_FREQ_BASE_PLATFORM:
			r9 = 200000000;
			break;

		      case SAL_FREQ_BASE_INTERVAL_TIMER:
			/*
			 * Is this supposed to be the cr.itc frequency
			 * or something platform specific?  The SAL
			 * doc ain't exactly clear on this...
			 */
			r9 = 700000000;
			break;

		      case SAL_FREQ_BASE_REALTIME_CLOCK:
			r9 = 1;
			break;

		      default:
			status = -1;
			break;
		}
	} else if (index == SAL_PCI_CONFIG_READ) {
		if (current->domain == dom0) {
			u64 value;
			// note that args 2&3 are swapped!!
			status = ia64_sal_pci_config_read(in1,in3,in2,&value);
			r9 = value;
		}
		else printf("NON-PRIV DOMAIN CALLED SAL_PCI_CONFIG_READ\n");
	} else if (index == SAL_PCI_CONFIG_WRITE) {
		if (current->domain == dom0) {
			if (((in1 & ~0xffffffffUL) && (in4 == 0)) ||
			    (in4 > 1) ||
			    (in2 > 8) || (in2 & (in2-1)))
			    	printf("*** SAL_PCI_CONF_WRITE?!?(adr=%p,typ=%p,sz=%p,val=%p)\n",in1,in4,in2,in3);
			// note that args are in a different order!!
			status = ia64_sal_pci_config_write(in1,in4,in2,in3);
		}
		else printf("NON-PRIV DOMAIN CALLED SAL_PCI_CONFIG_WRITE\n");
	} else if (index == SAL_SET_VECTORS) {
		printf("*** CALLED SAL_SET_VECTORS.  IGNORED...\n");
	} else if (index == SAL_GET_STATE_INFO) {
		printf("*** CALLED SAL_GET_STATE_INFO.  IGNORED...\n");
	} else if (index == SAL_GET_STATE_INFO_SIZE) {
		printf("*** CALLED SAL_GET_STATE_INFO_SIZE.  IGNORED...\n");
	} else if (index == SAL_CLEAR_STATE_INFO) {
		printf("*** CALLED SAL_CLEAR_STATE_INFO.  IGNORED...\n");
	} else if (index == SAL_MC_RENDEZ) {
		printf("*** CALLED SAL_MC_RENDEZ.  IGNORED...\n");
	} else if (index == SAL_MC_SET_PARAMS) {
		printf("*** CALLED SAL_MC_SET_PARAMS.  IGNORED...\n");
	} else if (index == SAL_CACHE_FLUSH) {
		printf("*** CALLED SAL_CACHE_FLUSH.  IGNORED...\n");
	} else if (index == SAL_CACHE_INIT) {
		printf("*** CALLED SAL_CACHE_INIT.  IGNORED...\n");
	} else if (index == SAL_UPDATE_PAL) {
		printf("*** CALLED SAL_UPDATE_PAL.  IGNORED...\n");
	} else {
		printf("*** CALLED SAL_ WITH UNKNOWN INDEX.  IGNORED...\n");
		status = -1;
	}
	return ((struct sal_ret_values) {status, r9, r10, r11});
}


#define NFUNCPTRS 20

void print_md(efi_memory_desc_t *md)
{
#if 1
	printk("domain mem: type=%u, attr=0x%lx, range=[0x%016lx-0x%016lx) (%luMB)\n",
		md->type, md->attribute, md->phys_addr,
		md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT),
		md->num_pages >> (20 - EFI_PAGE_SHIFT));
#endif
}

#define LSAPIC_NUM 16	// TEMP
static u32 lsapic_flag=1;

/* Provide only one LP to guest */
static int 
acpi_update_lsapic (acpi_table_entry_header *header)
{
	struct acpi_table_lsapic *lsapic;

	lsapic = (struct acpi_table_lsapic *) header;
	if (!lsapic)
		return -EINVAL;

	if (lsapic->flags.enabled && lsapic_flag) {
		printk("enable lsapic entry: 0x%lx\n", (u64)lsapic);
		lsapic_flag = 0; /* disable all the following processros */
	} else if (lsapic->flags.enabled) {
		printk("DISABLE lsapic entry: 0x%lx\n", (u64)lsapic);
		lsapic->flags.enabled = 0;
	} else
		printk("lsapic entry is already disabled: 0x%lx\n", (u64)lsapic);

	return 0;
}

static int
acpi_update_madt_checksum (unsigned long phys_addr, unsigned long size)
{
	u8 checksum=0;
    	u8* ptr;
	int len;
	struct acpi_table_madt* acpi_madt;

	if (!phys_addr || !size)
		return -EINVAL;

	acpi_madt = (struct acpi_table_madt *) __va(phys_addr);
	acpi_madt->header.checksum=0;

    	/* re-calculate MADT checksum */
	ptr = (u8*)acpi_madt;
    	len = acpi_madt->header.length;
	while (len>0){
		checksum = (u8)( checksum + (*ptr++) );
		len--;
	}
    	acpi_madt->header.checksum = 0x0 - checksum;	
	
	return 0;
}

/* base is physical address of acpi table */
void touch_acpi_table(void)
{
	u64 count = 0;
	count = acpi_table_parse_madt(ACPI_MADT_LSAPIC, acpi_update_lsapic, NR_CPUS);
	if ( count < 1)
		printk("Error parsing MADT - no LAPIC entires\n");
	printk("Total %d lsapic entry\n", count);
	acpi_table_parse(ACPI_APIC, acpi_update_madt_checksum);

	return;
}


struct ia64_boot_param *
dom_fw_init (struct domain *d, char *args, int arglen, char *fw_mem, int fw_mem_size)
{
	efi_system_table_t *efi_systab;
	efi_runtime_services_t *efi_runtime;
	efi_config_table_t *efi_tables;
	struct ia64_sal_systab *sal_systab;
	efi_memory_desc_t *efi_memmap, *md;
	unsigned long *pal_desc, *sal_desc;
	struct ia64_sal_desc_entry_point *sal_ed;
	struct ia64_boot_param *bp;
	unsigned long *pfn;
	unsigned char checksum = 0;
	char *cp, *cmd_line, *fw_vendor;
	int i = 0;
	unsigned long maxmem = d->max_pages * PAGE_SIZE;
	unsigned long start_mpaddr = ((d==dom0)?dom0_start:0);

#	define MAKE_MD(typ, attr, start, end, abs) 	\	
	do {						\
		md = efi_memmap + i++;			\
		md->type = typ;				\
		md->pad = 0;				\
		md->phys_addr = abs ? start : start_mpaddr + start;	\
		md->virt_addr = 0;			\
		md->num_pages = (end - start) >> 12;	\
		md->attribute = attr;			\
		print_md(md);				\
	} while (0)

/* FIXME: should check size but for now we have a whole MB to play with.
   And if stealing code from fw-emu.c, watch out for new fw_vendor on the end!
	if (fw_mem_size < sizeof(fw_mem_proto)) {
		printf("sys_fw_init: insufficient space for fw_mem\n");
		return 0;
	}
*/
	memset(fw_mem, 0, fw_mem_size);

#ifdef XEN
#else
	pal_desc = (unsigned long *) &pal_emulator_static;
	sal_desc = (unsigned long *) &sal_emulator;
#endif

	cp = fw_mem;
	efi_systab  = (void *) cp; cp += sizeof(*efi_systab);
	efi_runtime = (void *) cp; cp += sizeof(*efi_runtime);
	efi_tables  = (void *) cp; cp += NUM_EFI_SYS_TABLES * sizeof(*efi_tables);
	sal_systab  = (void *) cp; cp += sizeof(*sal_systab);
	sal_ed      = (void *) cp; cp += sizeof(*sal_ed);
	efi_memmap  = (void *) cp; cp += NUM_MEM_DESCS*sizeof(*efi_memmap);
	bp	    = (void *) cp; cp += sizeof(*bp);
	pfn        = (void *) cp; cp += NFUNCPTRS * 2 * sizeof(pfn);
	cmd_line    = (void *) cp;

	if (args) {
		if (arglen >= 1024)
			arglen = 1023;
		memcpy(cmd_line, args, arglen);
	} else {
		arglen = 0;
	}
	cmd_line[arglen] = '\0';

	memset(efi_systab, 0, sizeof(efi_systab));
	efi_systab->hdr.signature = EFI_SYSTEM_TABLE_SIGNATURE;
	efi_systab->hdr.revision  = EFI_SYSTEM_TABLE_REVISION;
	efi_systab->hdr.headersize = sizeof(efi_systab->hdr);
	cp = fw_vendor = &cmd_line[arglen] + (2-(arglen&1)); // round to 16-bit boundary
#define FW_VENDOR "X\0e\0n\0/\0i\0a\0\066\0\064\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
	cp += sizeof(FW_VENDOR) + (8-((unsigned long)cp & 7)); // round to 64-bit boundary

	memcpy(fw_vendor,FW_VENDOR,sizeof(FW_VENDOR));
	efi_systab->fw_vendor = dom_pa(fw_vendor);
	
	efi_systab->fw_revision = 1;
	efi_systab->runtime = (void *) dom_pa(efi_runtime);
	efi_systab->nr_tables = NUM_EFI_SYS_TABLES;
	efi_systab->tables = dom_pa(efi_tables);

	efi_runtime->hdr.signature = EFI_RUNTIME_SERVICES_SIGNATURE;
	efi_runtime->hdr.revision = EFI_RUNTIME_SERVICES_REVISION;
	efi_runtime->hdr.headersize = sizeof(efi_runtime->hdr);
#define EFI_HYPERCALL_PATCH(tgt,call) do { \
    dom_efi_hypercall_patch(d,FW_HYPERCALL_##call##_PADDR,FW_HYPERCALL_##call); \
    tgt = dom_pa(pfn); \
    *pfn++ = FW_HYPERCALL_##call##_PADDR + ((d==dom0)?dom0_start:0); \
    *pfn++ = 0; \
    } while (0)

	EFI_HYPERCALL_PATCH(efi_runtime->get_time,EFI_GET_TIME);
	EFI_HYPERCALL_PATCH(efi_runtime->set_time,EFI_SET_TIME);
	EFI_HYPERCALL_PATCH(efi_runtime->get_wakeup_time,EFI_GET_WAKEUP_TIME);
	EFI_HYPERCALL_PATCH(efi_runtime->set_wakeup_time,EFI_SET_WAKEUP_TIME);
	EFI_HYPERCALL_PATCH(efi_runtime->set_virtual_address_map,EFI_SET_VIRTUAL_ADDRESS_MAP);
	EFI_HYPERCALL_PATCH(efi_runtime->get_variable,EFI_GET_VARIABLE);
	EFI_HYPERCALL_PATCH(efi_runtime->get_next_variable,EFI_GET_NEXT_VARIABLE);
	EFI_HYPERCALL_PATCH(efi_runtime->set_variable,EFI_SET_VARIABLE);
	EFI_HYPERCALL_PATCH(efi_runtime->get_next_high_mono_count,EFI_GET_NEXT_HIGH_MONO_COUNT);
	EFI_HYPERCALL_PATCH(efi_runtime->reset_system,EFI_RESET_SYSTEM);

	efi_tables[0].guid = SAL_SYSTEM_TABLE_GUID;
	efi_tables[0].table = dom_pa(sal_systab);
	for (i = 1; i < NUM_EFI_SYS_TABLES; i++) {
		efi_tables[i].guid = NULL_GUID;
		efi_tables[i].table = 0;
	}
	if (d == dom0) {
		printf("Domain0 EFI passthrough:");
		i = 1;
		if (efi.mps) {
			efi_tables[i].guid = MPS_TABLE_GUID;
			efi_tables[i].table = __pa(efi.mps);
			printf(" MPS=%0xlx",efi_tables[i].table);
			i++;
		}

		touch_acpi_table();

		if (efi.acpi20) {
			efi_tables[i].guid = ACPI_20_TABLE_GUID;
			efi_tables[i].table = __pa(efi.acpi20);
			printf(" ACPI 2.0=%0xlx",efi_tables[i].table);
			i++;
		}
		if (efi.acpi) {
			efi_tables[i].guid = ACPI_TABLE_GUID;
			efi_tables[i].table = __pa(efi.acpi);
			printf(" ACPI=%0xlx",efi_tables[i].table);
			i++;
		}
		if (efi.smbios) {
			efi_tables[i].guid = SMBIOS_TABLE_GUID;
			efi_tables[i].table = __pa(efi.smbios);
			printf(" SMBIOS=%0xlx",efi_tables[i].table);
			i++;
		}
		if (efi.hcdp) {
			efi_tables[i].guid = HCDP_TABLE_GUID;
			efi_tables[i].table = __pa(efi.hcdp);
			printf(" HCDP=%0xlx",efi_tables[i].table);
			i++;
		}
		printf("\n");
	}

	/* fill in the SAL system table: */
	memcpy(sal_systab->signature, "SST_", 4);
	sal_systab->size = sizeof(*sal_systab);
	sal_systab->sal_rev_minor = 1;
	sal_systab->sal_rev_major = 0;
	sal_systab->entry_count = 1;

	strcpy(sal_systab->oem_id, "Xen/ia64");
	strcpy(sal_systab->product_id, "Xen/ia64");

	/* fill in an entry point: */
	sal_ed->type = SAL_DESC_ENTRY_POINT;
#define FW_HYPERCALL_PATCH(tgt,call,ret) do { \
    dom_fw_hypercall_patch(d,FW_HYPERCALL_##call##_PADDR,FW_HYPERCALL_##call,ret); \
    tgt = FW_HYPERCALL_##call##_PADDR + ((d==dom0)?dom0_start:0); \
    } while (0)
	FW_HYPERCALL_PATCH(sal_ed->pal_proc,PAL_CALL,0);
	FW_HYPERCALL_PATCH(sal_ed->sal_proc,SAL_CALL,1);
	sal_ed->gp = 0;  // will be ignored

	for (cp = (char *) sal_systab; cp < (char *) efi_memmap; ++cp)
		checksum += *cp;

	sal_systab->checksum = -checksum;

	/* simulate 1MB free memory at physical address zero */
	i = 0;
	MAKE_MD(EFI_BOOT_SERVICES_DATA,EFI_MEMORY_WB,0*MB,1*MB, 0);
	/* hypercall patches live here, masquerade as reserved PAL memory */
	MAKE_MD(EFI_PAL_CODE,EFI_MEMORY_WB,HYPERCALL_START,HYPERCALL_END, 0);
	MAKE_MD(EFI_CONVENTIONAL_MEMORY,EFI_MEMORY_WB,HYPERCALL_END,maxmem, 0);
#ifdef PASS_THRU_IOPORT_SPACE
	if (d == dom0 && !running_on_sim) {
		/* pass through the I/O port space */
		efi_memory_desc_t *efi_get_io_md(void);
		efi_memory_desc_t *ia64_efi_io_md = efi_get_io_md();
		u32 type;
		u64 iostart, ioend, ioattr;
		
		type = ia64_efi_io_md->type;
		iostart = ia64_efi_io_md->phys_addr;
		ioend = ia64_efi_io_md->phys_addr +
			(ia64_efi_io_md->num_pages << 12);
		ioattr = ia64_efi_io_md->attribute;
		MAKE_MD(type,ioattr,iostart,ioend, 1);
	}
	else
		MAKE_MD(EFI_RESERVED_TYPE,0,0,0,0);
#endif

	bp->efi_systab = dom_pa(fw_mem);
	bp->efi_memmap = dom_pa(efi_memmap);
	bp->efi_memmap_size = NUM_MEM_DESCS*sizeof(efi_memory_desc_t);
	bp->efi_memdesc_size = sizeof(efi_memory_desc_t);
	bp->efi_memdesc_version = 1;
	bp->command_line = dom_pa(cmd_line);
	bp->console_info.num_cols = 80;
	bp->console_info.num_rows = 25;
	bp->console_info.orig_x = 0;
	bp->console_info.orig_y = 24;
	bp->fpswa = 0;

	return bp;
}
