#include <xen/config.h>
#include <xen/init.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/domain_page.h>
#include <xen/iommu.h>
#include <asm/fixmap.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/e820.h>
#include <asm/tboot.h>
#include <crypto/vmac.h>

/* tboot=<physical address of shared page> */
static char opt_tboot[20] = "";
string_param("tboot", opt_tboot);

/* Global pointer to shared data; NULL means no measured launch. */
tboot_shared_t *g_tboot_shared;

static vmac_t domain_mac;     /* MAC for all domains during S3 */
static vmac_t xenheap_mac;    /* MAC for xen heap during S3 */
static vmac_t frametable_mac; /* MAC for frame table during S3 */

static const uuid_t tboot_shared_uuid = TBOOT_SHARED_UUID;

/* used by tboot_protect_mem_regions() and/or tboot_parse_dmar_table() */
static uint64_t txt_heap_base, txt_heap_size;
static uint64_t sinit_base, sinit_size;

/*
 * TXT configuration registers (offsets from TXT_{PUB, PRIV}_CONFIG_REGS_BASE)
 */

#define TXT_PUB_CONFIG_REGS_BASE       0xfed30000
#define TXT_PRIV_CONFIG_REGS_BASE      0xfed20000

/* # pages for each config regs space - used by fixmap */
#define NR_TXT_CONFIG_PAGES     ((TXT_PUB_CONFIG_REGS_BASE -                \
                                  TXT_PRIV_CONFIG_REGS_BASE) >> PAGE_SHIFT)

/* offsets from pub/priv config space */
#define TXTCR_SINIT_BASE            0x0270
#define TXTCR_SINIT_SIZE            0x0278
#define TXTCR_HEAP_BASE             0x0300
#define TXTCR_HEAP_SIZE             0x0308

extern char __init_begin[], __per_cpu_start[], __bss_start[];
extern unsigned long allocator_bitmap_end;

#define SHA1_SIZE      20
typedef uint8_t   sha1_hash_t[SHA1_SIZE];

typedef struct __packed {
    uint32_t     version;             /* currently 6 */
    sha1_hash_t  bios_acm_id;
    uint32_t     edx_senter_flags;
    uint64_t     mseg_valid;
    sha1_hash_t  sinit_hash;
    sha1_hash_t  mle_hash;
    sha1_hash_t  stm_hash;
    sha1_hash_t  lcp_policy_hash;
    uint32_t     lcp_policy_control;
    uint32_t     rlp_wakeup_addr;
    uint32_t     reserved;
    uint32_t     num_mdrs;
    uint32_t     mdrs_off;
    uint32_t     num_vtd_dmars;
    uint32_t     vtd_dmars_off;
} sinit_mle_data_t;

void __init tboot_probe(void)
{
    tboot_shared_t *tboot_shared;
    unsigned long p_tboot_shared;
    uint32_t map_base, map_size;
    unsigned long map_addr;

    /* Look for valid page-aligned address for shared page. */
    p_tboot_shared = simple_strtoul(opt_tboot, NULL, 0);
    if ( (p_tboot_shared == 0) || ((p_tboot_shared & ~PAGE_MASK) != 0) )
        return;

    /* Map and check for tboot UUID. */
    set_fixmap(FIX_TBOOT_SHARED_BASE, p_tboot_shared);
    tboot_shared = (tboot_shared_t *)fix_to_virt(FIX_TBOOT_SHARED_BASE);
    if ( tboot_shared == NULL )
        return;
    if ( memcmp(&tboot_shared_uuid, (uuid_t *)tboot_shared, sizeof(uuid_t)) )
        return;

    /* new tboot_shared (w/ GAS support, integrity, etc.) is not backwards
       compatible */
    if ( tboot_shared->version < 4 ) {
        printk("unsupported version of tboot (%u)\n", tboot_shared->version);
        return;
    }

    g_tboot_shared = tboot_shared;
    printk("TBOOT: found shared page at phys addr %lx:\n", p_tboot_shared);
    printk("  version: %d\n", tboot_shared->version);
    printk("  log_addr: 0x%08x\n", tboot_shared->log_addr);
    printk("  shutdown_entry: 0x%08x\n", tboot_shared->shutdown_entry);
    printk("  tboot_base: 0x%08x\n", tboot_shared->tboot_base);
    printk("  tboot_size: 0x%x\n", tboot_shared->tboot_size);

    /* these will be needed by tboot_protect_mem_regions() and/or
       tboot_parse_dmar_table(), so get them now */

    map_base = PFN_DOWN(TXT_PUB_CONFIG_REGS_BASE);
    map_size = PFN_UP(NR_TXT_CONFIG_PAGES * PAGE_SIZE);
    map_addr = (unsigned long)__va(map_base << PAGE_SHIFT);
    if ( map_pages_to_xen(map_addr, map_base, map_size, __PAGE_HYPERVISOR) )
        return;

    /* TXT Heap */
    txt_heap_base =
        *(uint64_t *)__va(TXT_PUB_CONFIG_REGS_BASE + TXTCR_HEAP_BASE);
    txt_heap_size =
        *(uint64_t *)__va(TXT_PUB_CONFIG_REGS_BASE + TXTCR_HEAP_SIZE);

    /* SINIT */
    sinit_base =
        *(uint64_t *)__va(TXT_PUB_CONFIG_REGS_BASE + TXTCR_SINIT_BASE);
    sinit_size =
        *(uint64_t *)__va(TXT_PUB_CONFIG_REGS_BASE + TXTCR_SINIT_SIZE);

    destroy_xen_mappings((unsigned long)__va(map_base << PAGE_SHIFT),
                         (unsigned long)__va((map_base + map_size) << PAGE_SHIFT));
}

/* definitions from xen/drivers/passthrough/vtd/iommu.h
 * used to walk through vtd page tables */
#define LEVEL_STRIDE (9)
#define PTE_NUM (1<<LEVEL_STRIDE)
#define dma_pte_present(p) (((p).val & 3) != 0)
#define dma_pte_addr(p) ((p).val & PAGE_MASK_4K)
#define agaw_to_level(val) ((val)+2)
struct dma_pte {
    u64 val;
};

static void update_iommu_mac(vmac_ctx_t *ctx, uint64_t pt_maddr, int level)
{
    int i;
    struct dma_pte *pt_vaddr, *pte;
    int next_level = level - 1;

    if ( pt_maddr == 0 )
        return;

    pt_vaddr = (struct dma_pte *)map_domain_page(pt_maddr >> PAGE_SHIFT_4K);
    vmac_update((void *)pt_vaddr, PAGE_SIZE, ctx);

    for ( i = 0; i < PTE_NUM; i++ )
    {
        pte = &pt_vaddr[i];
        if ( !dma_pte_present(*pte) )
            continue;

        if ( next_level >= 1 )
            update_iommu_mac(ctx, dma_pte_addr(*pte), next_level);
    }

    unmap_domain_page(pt_vaddr);
}

#define is_page_in_use(page) \
    ((page->count_info & PGC_count_mask) != 0 || page->count_info == 0)

static void update_pagetable_mac(vmac_ctx_t *ctx)
{
    unsigned long mfn;

    for ( mfn = 0; mfn < max_page; mfn++ )
    {
        struct page_info *page = mfn_to_page(mfn);
        if ( is_page_in_use(page) && !is_xen_heap_page(page) ) {
            if ( page->count_info & PGC_page_table ) {
                void *pg = map_domain_page(mfn);
                vmac_update(pg, PAGE_SIZE, ctx);
                unmap_domain_page(pg);
            }
        }
    }
}
 
static void tboot_gen_domain_integrity(const uint8_t key[TB_KEY_SIZE],
                                       vmac_t *mac)
{
    struct domain *d;
    struct page_info *page;
    uint8_t nonce[16] = {};
    vmac_ctx_t ctx;

    vmac_set_key((uint8_t *)key, &ctx);
    for_each_domain( d )
    {
        if ( !d->arch.s3_integrity )
            continue;
        printk("MACing Domain %u\n", d->domain_id);

        page_list_for_each(page, &d->page_list)
        {
            void *pg;
            pg = map_domain_page(page_to_mfn(page));
            vmac_update(pg, PAGE_SIZE, &ctx);
            unmap_domain_page(pg);
        }

        if ( !is_idle_domain(d) )
        {
            struct hvm_iommu *hd = domain_hvm_iommu(d);
            update_iommu_mac(&ctx, hd->pgd_maddr, agaw_to_level(hd->agaw));
        }
    }

    /* MAC all shadow page tables */
    update_pagetable_mac(&ctx);

    *mac = vmac(NULL, 0, nonce, NULL, &ctx);

    printk("MAC for domains is: 0x%08"PRIx64"\n", *mac);

    /* wipe ctx to ensure key is not left in memory */
    memset(&ctx, 0, sizeof(ctx));
}

static void tboot_gen_xenheap_integrity(const uint8_t key[TB_KEY_SIZE],
                                        vmac_t *mac)
{
    unsigned long mfn;
    uint8_t nonce[16] = {};
    vmac_ctx_t ctx;

    vmac_set_key((uint8_t *)key, &ctx);
    for ( mfn = 0; mfn < max_page; mfn++ )
    {
        struct page_info *page = __mfn_to_page(mfn);
        if ( is_page_in_use(page) && is_xen_heap_page(page) ) {
            void *pg = mfn_to_virt(mfn);
            vmac_update((uint8_t *)pg, PAGE_SIZE, &ctx);
        }
    }
    *mac = vmac(NULL, 0, nonce, NULL, &ctx);

    printk("MAC for xenheap is: 0x%08"PRIx64"\n", *mac);

    /* wipe ctx to ensure key is not left in memory */
    memset(&ctx, 0, sizeof(ctx));
}

static void tboot_gen_frametable_integrity(const uint8_t key[TB_KEY_SIZE],
                                           vmac_t *mac)
{
    uint8_t nonce[16] = {};
    vmac_ctx_t ctx;

    vmac_set_key((uint8_t *)key, &ctx);
    *mac = vmac((uint8_t *)frame_table,
                PFN_UP(max_page * sizeof(*frame_table)), nonce, NULL, &ctx);

    printk("MAC for frametable is: 0x%08"PRIx64"\n", *mac);

    /* wipe ctx to ensure key is not left in memory */
    memset(&ctx, 0, sizeof(ctx));
}

void tboot_shutdown(uint32_t shutdown_type)
{
    uint32_t map_base, map_size;
    int err;

    g_tboot_shared->shutdown_type = shutdown_type;

    local_irq_disable();

    /* we may be called from an interrupt context, so to prevent */
    /* 'ASSERT(!in_irq());' in alloc_domheap_pages(), decrease count */
    while ( in_irq() )
        irq_exit();

    /* Create identity map for tboot shutdown code. */
    /* do before S3 integrity because mapping tboot may change xenheap */
    map_base = PFN_DOWN(g_tboot_shared->tboot_base);
    map_size = PFN_UP(g_tboot_shared->tboot_size);

    err = map_pages_to_xen(map_base << PAGE_SHIFT, map_base, map_size,
                           __PAGE_HYPERVISOR);
    if ( err != 0 ) {
        printk("error (0x%x) mapping tboot pages (mfns) @ 0x%x, 0x%x\n", err,
               map_base, map_size);
        return;
    }

    /* if this is S3 then set regions to MAC */
    if ( shutdown_type == TB_SHUTDOWN_S3 ) {
        /*
         * Xen regions for tboot to MAC
         */
        g_tboot_shared->num_mac_regions = 5;
        /* S3 resume code (and other real mode trampoline code) */
        g_tboot_shared->mac_regions[0].start = bootsym_phys(trampoline_start);
        g_tboot_shared->mac_regions[0].size = bootsym_phys(trampoline_end) -
                                              bootsym_phys(trampoline_start);
        /* hypervisor code + data */
        g_tboot_shared->mac_regions[1].start = (uint64_t)__pa(&_stext);
        g_tboot_shared->mac_regions[1].size = __pa(&__init_begin) -
                                              __pa(&_stext);
        /* per-cpu data */
        g_tboot_shared->mac_regions[2].start = (uint64_t)__pa(&__per_cpu_start);
        g_tboot_shared->mac_regions[2].size =
            (((uint64_t)last_cpu(cpu_possible_map) + 1) << PERCPU_SHIFT);
        /* bss */
        g_tboot_shared->mac_regions[3].start = (uint64_t)__pa(&__bss_start);
        g_tboot_shared->mac_regions[3].size = __pa(&_end) - __pa(&__bss_start);
        /* boot allocator bitmap */
        g_tboot_shared->mac_regions[4].start = (uint64_t)__pa(&_end);
        g_tboot_shared->mac_regions[4].size = allocator_bitmap_end -
                                              __pa(&_end);

        /*
         * MAC domains and other Xen memory
         */
        /* Xen has no better entropy source for MAC key than tboot's */
        /* MAC domains first in case it perturbs xenheap */
        tboot_gen_domain_integrity(g_tboot_shared->s3_key, &domain_mac);
        tboot_gen_frametable_integrity(g_tboot_shared->s3_key, &frametable_mac);
        tboot_gen_xenheap_integrity(g_tboot_shared->s3_key, &xenheap_mac);
    }

    write_ptbase(idle_vcpu[0]);

    ((void(*)(void))(unsigned long)g_tboot_shared->shutdown_entry)();

    BUG(); /* should not reach here */
}

int tboot_in_measured_env(void)
{
    return (g_tboot_shared != NULL);
}

int __init tboot_protect_mem_regions(void)
{
    int rc;

    if ( !tboot_in_measured_env() )
        return 1;

    /* TXT Heap */
    if ( txt_heap_base == 0 )
        return 0;
    rc = e820_change_range_type(
        &e820, txt_heap_base, txt_heap_base + txt_heap_size,
        E820_RESERVED, E820_UNUSABLE);
    if ( !rc )
        return 0;

    /* SINIT */
    if ( sinit_base == 0 )
        return 0;
    rc = e820_change_range_type(
        &e820, sinit_base, sinit_base + sinit_size,
        E820_RESERVED, E820_UNUSABLE);
    if ( !rc )
        return 0;

    /* TXT Private Space */
    rc = e820_change_range_type(
        &e820, TXT_PRIV_CONFIG_REGS_BASE,
        TXT_PRIV_CONFIG_REGS_BASE + NR_TXT_CONFIG_PAGES * PAGE_SIZE,
        E820_RESERVED, E820_UNUSABLE);
    if ( !rc )
        return 0;

    return 1;
}

int __init tboot_parse_dmar_table(acpi_table_handler dmar_handler)
{
    uint32_t map_base, map_size;
    unsigned long map_vaddr;
    void *heap_ptr;
    struct acpi_table_header *dmar_table;
    int rc;

    if ( !tboot_in_measured_env() )
        return acpi_table_parse(ACPI_SIG_DMAR, dmar_handler);

    /* ACPI tables may not be DMA protected by tboot, so use DMAR copy */
    /* SINIT saved in SinitMleData in TXT heap (which is DMA protected) */

    if ( txt_heap_base == 0 )
        return 1;

    /* map TXT heap into Xen addr space */
    map_base = PFN_DOWN(txt_heap_base);
    map_size = PFN_UP(txt_heap_size);
    map_vaddr = (unsigned long)__va(map_base << PAGE_SHIFT);
    if ( map_pages_to_xen(map_vaddr, map_base, map_size, __PAGE_HYPERVISOR) )
        return 1;

    /* walk heap to SinitMleData */
    heap_ptr = __va(txt_heap_base);
    /* skip BiosData */
    heap_ptr += *(uint64_t *)heap_ptr;
    /* skip OsMleData */
    heap_ptr += *(uint64_t *)heap_ptr;
    /* skip OsSinitData */
    heap_ptr += *(uint64_t *)heap_ptr;
    /* now points to SinitMleDataSize; set to SinitMleData */
    heap_ptr += sizeof(uint64_t);
    /* get addr of DMAR table */
    dmar_table = (struct acpi_table_header *)(heap_ptr +
            ((sinit_mle_data_t *)heap_ptr)->vtd_dmars_off - sizeof(uint64_t));

    rc = dmar_handler(dmar_table);

    destroy_xen_mappings(
        (unsigned long)__va(map_base << PAGE_SHIFT),
        (unsigned long)__va((map_base + map_size) << PAGE_SHIFT));
  
    /* acpi_parse_dmar() zaps APCI DMAR signature in TXT heap table */
    /* but dom0 will read real table, so must zap it there too */
    dmar_table = NULL;
    acpi_get_table(ACPI_SIG_DMAR, 0, &dmar_table);
    if ( dmar_table != NULL )
        ((struct acpi_table_dmar *)dmar_table)->header.signature[0] = '\0';

    return rc;
}

int tboot_s3_resume(void)
{
    vmac_t mac;

    if ( !tboot_in_measured_env() )
        return 0;

    /* need to do these in reverse order of shutdown */
    tboot_gen_xenheap_integrity(g_tboot_shared->s3_key, &mac);
    if ( mac != xenheap_mac )
        return -1;

    tboot_gen_frametable_integrity(g_tboot_shared->s3_key, &mac);
    if ( mac != frametable_mac )
        return -2;

    tboot_gen_domain_integrity(g_tboot_shared->s3_key, &mac);
    if ( mac != domain_mac )
        return -3;

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
