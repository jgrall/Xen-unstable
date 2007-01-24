#include "xc_private.h"
#include "xg_private.h"
#include "xg_save_restore.h"

#if defined(__i386__) || defined(__x86_64__)
static int modify_returncode(int xc_handle, uint32_t domid)
{
    vcpu_guest_context_t ctxt;
    int rc;

    if ( (rc = xc_vcpu_getcontext(xc_handle, domid, 0, &ctxt)) != 0 )
        return rc;
    ctxt.user_regs.eax = 1;
    if ( (rc = xc_vcpu_setcontext(xc_handle, domid, 0, &ctxt)) != 0 )
        return rc;

    return 0;
}
#else
static int modify_returncode(int xc_handle, uint32_t domid)
{
    return 0;
}
#endif

static int xc_domain_resume_cooperative(int xc_handle, uint32_t domid)
{
    DECLARE_DOMCTL;
    int rc;

    /*
     * Set hypercall return code to indicate that suspend is cancelled
     * (rather than resuming in a new domain context).
     */
    if ( (rc = modify_returncode(xc_handle, domid)) != 0 )
        return rc;

    domctl.cmd = XEN_DOMCTL_resumedomain;
    domctl.domain = domid;
    return do_domctl(xc_handle, &domctl);
}

static int xc_domain_resume_any(int xc_handle, uint32_t domid)
{
    DECLARE_DOMCTL;
    xc_dominfo_t info;
    int i, rc = -1;
#if defined(__i386__) || defined(__x86_64__)
    unsigned long mfn, max_pfn = 0;
    vcpu_guest_context_t ctxt;
    start_info_t *start_info;
    shared_info_t *shinfo = NULL;
    xen_pfn_t *p2m_frame_list_list = NULL;
    xen_pfn_t *p2m_frame_list = NULL;
    xen_pfn_t *p2m = NULL;
#endif

    if ( xc_domain_getinfo(xc_handle, domid, 1, &info) != 1 )
    {
        PERROR("Could not get domain info");
        goto out;
    }

    /*
     * (x86 only) Rewrite store_mfn and console_mfn back to MFN (from PFN).
     */
#if defined(__i386__) || defined(__x86_64__)
    /* Map the shared info frame */
    shinfo = xc_map_foreign_range(xc_handle, domid, PAGE_SIZE,
                                  PROT_READ, info.shared_info_frame);
    if ( shinfo == NULL )
    {
        ERROR("Couldn't map shared info");
        goto out;
    }

    max_pfn = shinfo->arch.max_pfn;

    p2m_frame_list_list =
        xc_map_foreign_range(xc_handle, domid, PAGE_SIZE, PROT_READ,
                             shinfo->arch.pfn_to_mfn_frame_list_list);
    if ( p2m_frame_list_list == NULL )
    {
        ERROR("Couldn't map p2m_frame_list_list");
        goto out;
    }

    p2m_frame_list = xc_map_foreign_batch(xc_handle, domid, PROT_READ,
                                          p2m_frame_list_list,
                                          P2M_FLL_ENTRIES);
    if ( p2m_frame_list == NULL )
    {
        ERROR("Couldn't map p2m_frame_list");
        goto out;
    }

    /* Map all the frames of the pfn->mfn table. For migrate to succeed,
       the guest must not change which frames are used for this purpose.
       (its not clear why it would want to change them, and we'll be OK
       from a safety POV anyhow. */
    p2m = xc_map_foreign_batch(xc_handle, domid, PROT_READ,
                               p2m_frame_list,
                               P2M_FL_ENTRIES);
    if ( p2m == NULL )
    {
        ERROR("Couldn't map p2m table");
        goto out;
    }

    if ( lock_pages(&ctxt, sizeof(ctxt)) )
    {
        ERROR("Unable to lock ctxt");
        goto out;
    }

    if ( xc_vcpu_getcontext(xc_handle, domid, 0, &ctxt) )
    {
        ERROR("Could not get vcpu context");
        goto out;
    }

    mfn = ctxt.user_regs.edx;

    start_info = xc_map_foreign_range(xc_handle, domid, PAGE_SIZE,
                                      PROT_READ | PROT_WRITE, mfn);
    if ( start_info == NULL )
    {
        ERROR("Couldn't map start_info");
        goto out;
    }

    start_info->store_mfn        = p2m[start_info->store_mfn];
    start_info->console.domU.mfn = p2m[start_info->console.domU.mfn];

    munmap(start_info, PAGE_SIZE);
#endif /* defined(__i386__) || defined(__x86_64__) */

    /* Reset all secondary CPU states. */
    for ( i = 1; i <= info.max_vcpu_id; i++ )
        xc_vcpu_setcontext(xc_handle, domid, i, NULL);

    /* Ready to resume domain execution now. */
    domctl.cmd = XEN_DOMCTL_resumedomain;
    domctl.domain = domid;
    rc = do_domctl(xc_handle, &domctl);

#if defined(__i386__) || defined(__x86_64__)
 out:
    unlock_pages((void *)&ctxt, sizeof ctxt);
    if (p2m)
        munmap(p2m, P2M_FL_ENTRIES*PAGE_SIZE);
    if (p2m_frame_list)
        munmap(p2m_frame_list, P2M_FLL_ENTRIES*PAGE_SIZE);
    if (p2m_frame_list_list)
        munmap(p2m_frame_list_list, PAGE_SIZE);
    if (shinfo)
        munmap(shinfo, PAGE_SIZE);
#endif

    return rc;
}

/*
 * Resume execution of a domain after suspend shutdown.
 * This can happen in one of two ways:
 *  1. Resume with special return code.
 *  2. Reset guest environment so it believes it is resumed in a new
 *     domain context.
 * (2) should be used only for guests which cannot handle the special
 * new return code. (1) is always safe (but slower).
 */
int xc_domain_resume(int xc_handle, uint32_t domid)
{
    /*
     * XXX: Implement a way to select between options (1) and (2).
     * Or expose the options as two different methods to Python.
     */
    return (0
            ? xc_domain_resume_cooperative(xc_handle, domid)
            : xc_domain_resume_any(xc_handle, domid));
}
