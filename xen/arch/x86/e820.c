#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <asm/e820.h>

struct e820map e820;

static void __init limit_regions(unsigned long long size)
{
    unsigned long long current_addr = 0;
    int i;

#if 0
    if (efi_enabled) {
        for (i = 0; i < memmap.nr_map; i++) {
            current_addr = memmap.map[i].phys_addr +
                (memmap.map[i].num_pages << 12);
            if (memmap.map[i].type == EFI_CONVENTIONAL_MEMORY) {
                if (current_addr >= size) {
                    memmap.map[i].num_pages -=
                        (((current_addr-size) + PAGE_SIZE-1) >> PAGE_SHIFT);
                    memmap.nr_map = i + 1;
                    return;
                }
            }
        }
    }
#endif

    for (i = 0; i < e820.nr_map; i++) {
        if (e820.map[i].type == E820_RAM) {
            current_addr = e820.map[i].addr + e820.map[i].size;
            if (current_addr >= size) {
                e820.map[i].size -= current_addr-size;
                e820.nr_map = i + 1;
                return;
            }
        }
    }
}

static void __init add_memory_region(unsigned long long start,
                                     unsigned long long size, int type)
{
    int x;

    /*if (!efi_enabled)*/ {
        x = e820.nr_map;

        if (x == E820MAX) {
            printk(KERN_ERR "Ooops! Too many entries in the memory map!\n");
            return;
        }

        e820.map[x].addr = start;
        e820.map[x].size = size;
        e820.map[x].type = type;
        e820.nr_map++;
    }
} /* add_memory_region */

#define E820_DEBUG	1

static void __init print_memory_map(char *who)
{
    int i;

    for (i = 0; i < e820.nr_map; i++) {
        printk(" %s: %016Lx - %016Lx ", who,
               e820.map[i].addr,
               e820.map[i].addr + e820.map[i].size);
        switch (e820.map[i].type) {
        case E820_RAM:	printk("(usable)\n");
            break;
        case E820_RESERVED:
            printk("(reserved)\n");
            break;
        case E820_ACPI:
            printk("(ACPI data)\n");
            break;
        case E820_NVS:
            printk("(ACPI NVS)\n");
            break;
        default:	printk("type %u\n", e820.map[i].type);
            break;
        }
    }
}

/*
 * Sanitize the BIOS e820 map.
 *
 * Some e820 responses include overlapping entries.  The following 
 * replaces the original e820 map with a new one, removing overlaps.
 *
 */
struct change_member {
    struct e820entry *pbios; /* pointer to original bios entry */
    unsigned long long addr; /* address for this change point */
};
static struct change_member change_point_list[2*E820MAX] __initdata;
static struct change_member *change_point[2*E820MAX] __initdata;
static struct e820entry *overlap_list[E820MAX] __initdata;
static struct e820entry new_bios[E820MAX] __initdata;

static int __init sanitize_e820_map(struct e820entry * biosmap, char * pnr_map)
{
    struct change_member *change_tmp;
    unsigned long current_type, last_type;
    unsigned long long last_addr;
    int chgidx, still_changing;
    int overlap_entries;
    int new_bios_entry;
    int old_nr, new_nr, chg_nr;
    int i;

    /*
      Visually we're performing the following (1,2,3,4 = memory types)...

      Sample memory map (w/overlaps):
      ____22__________________
      ______________________4_
      ____1111________________
      _44_____________________
      11111111________________
      ____________________33__
      ___________44___________
      __________33333_________
      ______________22________
      ___________________2222_
      _________111111111______
      _____________________11_
      _________________4______

      Sanitized equivalent (no overlap):
      1_______________________
      _44_____________________
      ___1____________________
      ____22__________________
      ______11________________
      _________1______________
      __________3_____________
      ___________44___________
      _____________33_________
      _______________2________
      ________________1_______
      _________________4______
      ___________________2____
      ____________________33__
      ______________________4_
    */

    /* if there's only one memory region, don't bother */
    if (*pnr_map < 2)
        return -1;

    old_nr = *pnr_map;

    /* bail out if we find any unreasonable addresses in bios map */
    for (i=0; i<old_nr; i++)
        if (biosmap[i].addr + biosmap[i].size < biosmap[i].addr)
            return -1;

    /* create pointers for initial change-point information (for sorting) */
    for (i=0; i < 2*old_nr; i++)
        change_point[i] = &change_point_list[i];

    /* record all known change-points (starting and ending addresses),
       omitting those that are for empty memory regions */
    chgidx = 0;
    for (i=0; i < old_nr; i++)	{
        if (biosmap[i].size != 0) {
            change_point[chgidx]->addr = biosmap[i].addr;
            change_point[chgidx++]->pbios = &biosmap[i];
            change_point[chgidx]->addr = biosmap[i].addr + biosmap[i].size;
            change_point[chgidx++]->pbios = &biosmap[i];
        }
    }
    chg_nr = chgidx;    	/* true number of change-points */

    /* sort change-point list by memory addresses (low -> high) */
    still_changing = 1;
    while (still_changing)	{
        still_changing = 0;
        for (i=1; i < chg_nr; i++)  {
            /* if <current_addr> > <last_addr>, swap */
            /* or, if current=<start_addr> & last=<end_addr>, swap */
            if ((change_point[i]->addr < change_point[i-1]->addr) ||
                ((change_point[i]->addr == change_point[i-1]->addr) &&
                 (change_point[i]->addr == change_point[i]->pbios->addr) &&
                 (change_point[i-1]->addr != change_point[i-1]->pbios->addr))
                )
            {
                change_tmp = change_point[i];
                change_point[i] = change_point[i-1];
                change_point[i-1] = change_tmp;
                still_changing=1;
            }
        }
    }

    /* create a new bios memory map, removing overlaps */
    overlap_entries=0;	 /* number of entries in the overlap table */
    new_bios_entry=0;	 /* index for creating new bios map entries */
    last_type = 0;		 /* start with undefined memory type */
    last_addr = 0;		 /* start with 0 as last starting address */
    /* loop through change-points, determining affect on the new bios map */
    for (chgidx=0; chgidx < chg_nr; chgidx++)
    {
        /* keep track of all overlapping bios entries */
        if (change_point[chgidx]->addr == change_point[chgidx]->pbios->addr)
        {
            /* add map entry to overlap list (> 1 entry implies an overlap) */
            overlap_list[overlap_entries++]=change_point[chgidx]->pbios;
        }
        else
        {
            /* remove entry from list (order independent, so swap with last) */
            for (i=0; i<overlap_entries; i++)
            {
                if (overlap_list[i] == change_point[chgidx]->pbios)
                    overlap_list[i] = overlap_list[overlap_entries-1];
            }
            overlap_entries--;
        }
        /* if there are overlapping entries, decide which "type" to use */
        /* (larger value takes precedence -- 1=usable, 2,3,4,4+=unusable) */
        current_type = 0;
        for (i=0; i<overlap_entries; i++)
            if (overlap_list[i]->type > current_type)
                current_type = overlap_list[i]->type;
        /* continue building up new bios map based on this information */
        if (current_type != last_type)	{
            if (last_type != 0)	 {
                new_bios[new_bios_entry].size =
                    change_point[chgidx]->addr - last_addr;
				/* move forward only if the new size was non-zero */
                if (new_bios[new_bios_entry].size != 0)
                    if (++new_bios_entry >= E820MAX)
                        break; 	/* no more space left for new bios entries */
            }
            if (current_type != 0)	{
                new_bios[new_bios_entry].addr = change_point[chgidx]->addr;
                new_bios[new_bios_entry].type = current_type;
                last_addr=change_point[chgidx]->addr;
            }
            last_type = current_type;
        }
    }
    new_nr = new_bios_entry;   /* retain count for new bios entries */

    /* copy new bios mapping into original location */
    memcpy(biosmap, new_bios, new_nr*sizeof(struct e820entry));
    *pnr_map = new_nr;

    return 0;
}

/*
 * Copy the BIOS e820 map into a safe place.
 *
 * Sanity-check it while we're at it..
 *
 * If we're lucky and live on a modern system, the setup code
 * will have given us a memory map that we can use to properly
 * set up memory.  If we aren't, we'll fake a memory map.
 *
 * We check to see that the memory map contains at least 2 elements
 * before we'll use it, because the detection code in setup.S may
 * not be perfect and most every PC known to man has two memory
 * regions: one from 0 to 640k, and one from 1mb up.  (The IBM
 * thinkpad 560x, for example, does not cooperate with the memory
 * detection code.)
 */
static int __init copy_e820_map(struct e820entry * biosmap, int nr_map)
{
    /* Only one memory region (or negative)? Ignore it */
    if (nr_map < 2)
        return -1;

    do {
        unsigned long long start = biosmap->addr;
        unsigned long long size = biosmap->size;
        unsigned long long end = start + size;
        unsigned long type = biosmap->type;

        /* Overflow in 64 bits? Ignore the memory map. */
        if (start > end)
            return -1;

        /*
         * Some BIOSes claim RAM in the 640k - 1M region.
         * Not right. Fix it up.
         */
        if (type == E820_RAM) {
            if (start < 0x100000ULL && end > 0xA0000ULL) {
                if (start < 0xA0000ULL)
                    add_memory_region(start, 0xA0000ULL-start, type);
                if (end <= 0x100000ULL)
                    continue;
                start = 0x100000ULL;
                size = end - start;
            }
        }
        add_memory_region(start, size, type);
    } while (biosmap++,--nr_map);
    return 0;
}


/*
 * Find the highest page frame number we have available
 */
static unsigned long __init find_max_pfn(void)
{
    int i;
    unsigned long max_pfn = 0;

#if 0
    if (efi_enabled) {
        efi_memmap_walk(efi_find_max_pfn, &max_pfn);
        return;
    }
#endif

    for (i = 0; i < e820.nr_map; i++) {
        unsigned long start, end;
        /* RAM? */
        if (e820.map[i].type != E820_RAM)
            continue;
        start = PFN_UP(e820.map[i].addr);
        end = PFN_DOWN(e820.map[i].addr + e820.map[i].size);
        if (start >= end)
            continue;
        if (end > max_pfn)
            max_pfn = end;
    }

    return max_pfn;
}

static char * __init machine_specific_memory_setup(
    struct e820entry *raw, int raw_nr)
{
    char nr = (char)raw_nr;
    char *who = "Pseudo-e820";
    sanitize_e820_map(raw, &nr);
    (void)copy_e820_map(raw, nr);
    return who;
}

unsigned long init_e820(struct e820entry *raw, int raw_nr)
{
    printk(KERN_INFO "Physical RAM map:\n");
    print_memory_map(machine_specific_memory_setup(raw, raw_nr));
    return find_max_pfn();
}
