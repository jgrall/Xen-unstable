/******************************************************************************
 * xc_private.c
 *
 * Helper functions for the rest of the library.
 */

#include <inttypes.h>
#include "xc_private.h"
#include "xg_private.h"

int lock_pages(void *addr, size_t len)
{
      int e = 0;
#ifndef __sun__
      e = mlock(addr, len);
#endif
      return (e);
}

void unlock_pages(void *addr, size_t len)
{
#ifndef __sun__
	safe_munlock(addr, len);
#endif
}

/* NB: arr must be locked */
int xc_get_pfn_type_batch(int xc_handle,
                          uint32_t dom, int num, unsigned long *arr)
{
    DECLARE_DOMCTL;
    domctl.cmd = XEN_DOMCTL_getpageframeinfo2;
    domctl.domain = (domid_t)dom;
    domctl.u.getpageframeinfo2.num    = num;
    set_xen_guest_handle(domctl.u.getpageframeinfo2.array, arr);
    return do_domctl(xc_handle, &domctl);
}

int xc_mmuext_op(
    int xc_handle,
    struct mmuext_op *op,
    unsigned int nr_ops,
    domid_t dom)
{
    DECLARE_HYPERCALL;
    long ret = -EINVAL;

    hypercall.op     = __HYPERVISOR_mmuext_op;
    hypercall.arg[0] = (unsigned long)op;
    hypercall.arg[1] = (unsigned long)nr_ops;
    hypercall.arg[2] = (unsigned long)0;
    hypercall.arg[3] = (unsigned long)dom;

    if ( lock_pages(op, nr_ops*sizeof(*op)) != 0 )
    {
        PERROR("Could not lock memory for Xen hypercall");
        goto out1;
    }

    ret = do_xen_hypercall(xc_handle, &hypercall);

    unlock_pages(op, nr_ops*sizeof(*op));

 out1:
    return ret;
}

static int flush_mmu_updates(int xc_handle, xc_mmu_t *mmu)
{
    int err = 0;
    DECLARE_HYPERCALL;

    if ( mmu->idx == 0 )
        return 0;

    hypercall.op     = __HYPERVISOR_mmu_update;
    hypercall.arg[0] = (unsigned long)mmu->updates;
    hypercall.arg[1] = (unsigned long)mmu->idx;
    hypercall.arg[2] = 0;
    hypercall.arg[3] = mmu->subject;

    if ( lock_pages(mmu->updates, sizeof(mmu->updates)) != 0 )
    {
        PERROR("flush_mmu_updates: mmu updates lock_pages failed");
        err = 1;
        goto out;
    }

    if ( do_xen_hypercall(xc_handle, &hypercall) < 0 )
    {
        ERROR("Failure when submitting mmu updates");
        err = 1;
    }

    mmu->idx = 0;

    unlock_pages(mmu->updates, sizeof(mmu->updates));

 out:
    return err;
}

xc_mmu_t *xc_init_mmu_updates(int xc_handle, domid_t dom)
{
    xc_mmu_t *mmu = malloc(sizeof(xc_mmu_t));
    if ( mmu == NULL )
        return mmu;
    mmu->idx     = 0;
    mmu->subject = dom;
    return mmu;
}

int xc_add_mmu_update(int xc_handle, xc_mmu_t *mmu,
                      unsigned long long ptr, unsigned long long val)
{
    mmu->updates[mmu->idx].ptr = ptr;
    mmu->updates[mmu->idx].val = val;

    if ( ++mmu->idx == MAX_MMU_UPDATES )
        return flush_mmu_updates(xc_handle, mmu);

    return 0;
}

int xc_finish_mmu_updates(int xc_handle, xc_mmu_t *mmu)
{
    return flush_mmu_updates(xc_handle, mmu);
}

int xc_memory_op(int xc_handle,
                 int cmd,
                 void *arg)
{
    DECLARE_HYPERCALL;
    struct xen_memory_reservation *reservation = arg;
    struct xen_machphys_mfn_list *xmml = arg;
    xen_pfn_t *extent_start;
    long ret = -EINVAL;

    hypercall.op     = __HYPERVISOR_memory_op;
    hypercall.arg[0] = (unsigned long)cmd;
    hypercall.arg[1] = (unsigned long)arg;

    switch ( cmd )
    {
    case XENMEM_increase_reservation:
    case XENMEM_decrease_reservation:
    case XENMEM_populate_physmap:
        if ( lock_pages(reservation, sizeof(*reservation)) != 0 )
        {
            PERROR("Could not lock");
            goto out1;
        }
        get_xen_guest_handle(extent_start, reservation->extent_start);
        if ( (extent_start != NULL) &&
             (lock_pages(extent_start,
                    reservation->nr_extents * sizeof(xen_pfn_t)) != 0) )
        {
            PERROR("Could not lock");
            unlock_pages(reservation, sizeof(*reservation));
            goto out1;
        }
        break;
    case XENMEM_machphys_mfn_list:
        if ( lock_pages(xmml, sizeof(*xmml)) != 0 )
        {
            PERROR("Could not lock");
            goto out1;
        }
        get_xen_guest_handle(extent_start, xmml->extent_start);
        if ( lock_pages(extent_start,
                   xmml->max_extents * sizeof(xen_pfn_t)) != 0 )
        {
            PERROR("Could not lock");
            unlock_pages(xmml, sizeof(*xmml));
            goto out1;
        }
        break;
    case XENMEM_add_to_physmap:
        if ( lock_pages(arg, sizeof(struct xen_add_to_physmap)) )
        {
            PERROR("Could not lock");
            goto out1;
        }
        break;
    }

    ret = do_xen_hypercall(xc_handle, &hypercall);

    switch ( cmd )
    {
    case XENMEM_increase_reservation:
    case XENMEM_decrease_reservation:
    case XENMEM_populate_physmap:
        unlock_pages(reservation, sizeof(*reservation));
        get_xen_guest_handle(extent_start, reservation->extent_start);
        if ( extent_start != NULL )
            unlock_pages(extent_start,
                         reservation->nr_extents * sizeof(xen_pfn_t));
        break;
    case XENMEM_machphys_mfn_list:
        unlock_pages(xmml, sizeof(*xmml));
        get_xen_guest_handle(extent_start, xmml->extent_start);
        unlock_pages(extent_start,
                     xmml->max_extents * sizeof(xen_pfn_t));
        break;
    case XENMEM_add_to_physmap:
        unlock_pages(arg, sizeof(struct xen_add_to_physmap));
        break;
    }

 out1:
    return ret;
}


long long xc_domain_get_cpu_usage( int xc_handle, domid_t domid, int vcpu )
{
    DECLARE_DOMCTL;

    domctl.cmd = XEN_DOMCTL_getvcpuinfo;
    domctl.domain = (domid_t)domid;
    domctl.u.getvcpuinfo.vcpu   = (uint16_t)vcpu;
    if ( (do_domctl(xc_handle, &domctl) < 0) )
    {
        PERROR("Could not get info on domain");
        return -1;
    }
    return domctl.u.getvcpuinfo.cpu_time;
}


#ifndef __ia64__
int xc_get_pfn_list(int xc_handle,
                    uint32_t domid,
                    xen_pfn_t *pfn_buf,
                    unsigned long max_pfns)
{
    DECLARE_DOMCTL;
    int ret;
    domctl.cmd = XEN_DOMCTL_getmemlist;
    domctl.domain   = (domid_t)domid;
    domctl.u.getmemlist.max_pfns = max_pfns;
    set_xen_guest_handle(domctl.u.getmemlist.buffer, pfn_buf);

#ifdef VALGRIND
    memset(pfn_buf, 0, max_pfns * sizeof(xen_pfn_t));
#endif

    if ( lock_pages(pfn_buf, max_pfns * sizeof(xen_pfn_t)) != 0 )
    {
        PERROR("xc_get_pfn_list: pfn_buf lock failed");
        return -1;
    }

    ret = do_domctl(xc_handle, &domctl);

    unlock_pages(pfn_buf, max_pfns * sizeof(xen_pfn_t));

#if 0
#ifdef DEBUG
    DPRINTF(("Ret for xc_get_pfn_list is %d\n", ret));
    if (ret >= 0) {
        int i, j;
        for (i = 0; i < domctl.u.getmemlist.num_pfns; i += 16) {
            DPRINTF("0x%x: ", i);
            for (j = 0; j < 16; j++)
                DPRINTF("0x%lx ", pfn_buf[i + j]);
            DPRINTF("\n");
        }
    }
#endif
#endif

    return (ret < 0) ? -1 : domctl.u.getmemlist.num_pfns;
}
#endif

long xc_get_tot_pages(int xc_handle, uint32_t domid)
{
    DECLARE_DOMCTL;
    domctl.cmd = XEN_DOMCTL_getdomaininfo;
    domctl.domain = (domid_t)domid;
    return (do_domctl(xc_handle, &domctl) < 0) ?
        -1 : domctl.u.getdomaininfo.tot_pages;
}

int xc_copy_to_domain_page(int xc_handle,
                           uint32_t domid,
                           unsigned long dst_pfn,
                           const char *src_page)
{
    void *vaddr = xc_map_foreign_range(
        xc_handle, domid, PAGE_SIZE, PROT_WRITE, dst_pfn);
    if ( vaddr == NULL )
        return -1;
    memcpy(vaddr, src_page, PAGE_SIZE);
    munmap(vaddr, PAGE_SIZE);
    return 0;
}

int xc_clear_domain_page(int xc_handle,
                         uint32_t domid,
                         unsigned long dst_pfn)
{
    void *vaddr = xc_map_foreign_range(
        xc_handle, domid, PAGE_SIZE, PROT_WRITE, dst_pfn);
    if ( vaddr == NULL )
        return -1;
    memset(vaddr, 0, PAGE_SIZE);
    munmap(vaddr, PAGE_SIZE);
    return 0;
}

void xc_map_memcpy(unsigned long dst, const char *src, unsigned long size,
                   int xch, uint32_t dom, xen_pfn_t *parray,
                   unsigned long vstart)
{
    char *va;
    unsigned long chunksz, done, pa;

    for ( done = 0; done < size; done += chunksz )
    {
        pa = dst + done - vstart;
        va = xc_map_foreign_range(
            xch, dom, PAGE_SIZE, PROT_WRITE, parray[pa>>PAGE_SHIFT]);
        chunksz = size - done;
        if ( chunksz > (PAGE_SIZE - (pa & (PAGE_SIZE-1))) )
            chunksz = PAGE_SIZE - (pa & (PAGE_SIZE-1));
        memcpy(va + (pa & (PAGE_SIZE-1)), src + done, chunksz);
        munmap(va, PAGE_SIZE);
    }
}

int xc_domctl(int xc_handle, struct xen_domctl *domctl)
{
    return do_domctl(xc_handle, domctl);
}

int xc_sysctl(int xc_handle, struct xen_sysctl *sysctl)
{
    return do_sysctl(xc_handle, sysctl);
}

int xc_version(int xc_handle, int cmd, void *arg)
{
    int rc, argsize = 0;

    switch ( cmd )
    {
    case XENVER_extraversion:
        argsize = sizeof(xen_extraversion_t);
        break;
    case XENVER_compile_info:
        argsize = sizeof(xen_compile_info_t);
        break;
    case XENVER_capabilities:
        argsize = sizeof(xen_capabilities_info_t);
        break;
    case XENVER_changeset:
        argsize = sizeof(xen_changeset_info_t);
        break;
    case XENVER_platform_parameters:
        argsize = sizeof(xen_platform_parameters_t);
        break;
    }

    if ( (argsize != 0) && (lock_pages(arg, argsize) != 0) )
    {
        PERROR("Could not lock memory for version hypercall");
        return -ENOMEM;
    }

#ifdef VALGRIND
    if (argsize != 0)
        memset(arg, 0, argsize);
#endif

    rc = do_xen_version(xc_handle, cmd, arg);

    if ( argsize != 0 )
        unlock_pages(arg, argsize);

    return rc;
}

unsigned long xc_make_page_below_4G(
    int xc_handle, uint32_t domid, unsigned long mfn)
{
    xen_pfn_t old_mfn = mfn;
    xen_pfn_t new_mfn;

    if ( xc_domain_memory_decrease_reservation(
        xc_handle, domid, 1, 0, &old_mfn) != 0 )
    {
        DPRINTF("xc_make_page_below_4G decrease failed. mfn=%lx\n",mfn);
        return 0;
    }

    if ( xc_domain_memory_increase_reservation(
        xc_handle, domid, 1, 0, 32, &new_mfn) != 0 )
    {
        DPRINTF("xc_make_page_below_4G increase failed. mfn=%lx\n",mfn);
        return 0;
    }

    return new_mfn;
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
