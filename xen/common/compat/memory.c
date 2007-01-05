#include <xen/config.h>
#include <xen/types.h>
#include <xen/hypercall.h>
#include <xen/guest_access.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <asm/current.h>
#include <compat/memory.h>

int compat_memory_op(unsigned int cmd, XEN_GUEST_HANDLE(void) compat)
{
    int rc, split, op = cmd & MEMOP_CMD_MASK;
    unsigned int start_extent = cmd >> MEMOP_EXTENT_SHIFT;

    do
    {
        unsigned int i, end_extent = 0;
        union {
            XEN_GUEST_HANDLE(void) hnd;
            struct xen_memory_reservation *rsrv;
            struct xen_memory_exchange *xchg;
            struct xen_translate_gpfn_list *xlat;
        } nat;
        union {
            struct compat_memory_reservation rsrv;
            struct compat_memory_exchange xchg;
            struct compat_translate_gpfn_list xlat;
        } cmp;

        set_xen_guest_handle(nat.hnd, (void *)COMPAT_ARG_XLAT_VIRT_START(current->vcpu_id));
        split = 0;
        switch ( op )
        {
            xen_pfn_t *space;

        case XENMEM_increase_reservation:
        case XENMEM_decrease_reservation:
        case XENMEM_populate_physmap:
            if ( copy_from_guest(&cmp.rsrv, compat, 1) )
                return start_extent;

            /* Is size too large for us to encode a continuation? */
            if ( cmp.rsrv.nr_extents > (UINT_MAX >> MEMOP_EXTENT_SHIFT) )
                return start_extent;

            if ( !compat_handle_is_null(cmp.rsrv.extent_start) &&
                 !compat_handle_okay(cmp.rsrv.extent_start, cmp.rsrv.nr_extents) )
                return start_extent;

            end_extent = start_extent + (COMPAT_ARG_XLAT_SIZE - sizeof(*nat.rsrv)) /
                                        sizeof(*space);
            if ( end_extent > cmp.rsrv.nr_extents )
                end_extent = cmp.rsrv.nr_extents;

            space = (xen_pfn_t *)(nat.rsrv + 1);
#define XLAT_memory_reservation_HNDL_extent_start(_d_, _s_) \
            do \
            { \
                if ( !compat_handle_is_null((_s_)->extent_start) ) \
                { \
                    set_xen_guest_handle((_d_)->extent_start, space - start_extent); \
                    if ( op != XENMEM_increase_reservation ) \
                    { \
                        for ( i = start_extent; i < end_extent; ++i ) \
                        { \
                            compat_pfn_t pfn; \
                            if ( __copy_from_compat_offset(&pfn, (_s_)->extent_start, i, 1) ) \
                            { \
                                end_extent = i; \
                                split = -1; \
                                break; \
                            } \
                            *space++ = pfn; \
                        } \
                    } \
                } \
                else \
                { \
                    set_xen_guest_handle((_d_)->extent_start, NULL); \
                    end_extent = cmp.rsrv.nr_extents; \
                } \
            } while (0)
            XLAT_memory_reservation(nat.rsrv, &cmp.rsrv);
#undef XLAT_memory_reservation_HNDL_extent_start

            if ( end_extent < cmp.rsrv.nr_extents )
            {
                nat.rsrv->nr_extents = end_extent;
                ++split;
            }

            break;

        case XENMEM_exchange:
        {
            int order_delta;

            if ( copy_from_guest(&cmp.xchg, compat, 1) )
                return -EFAULT;

            order_delta = cmp.xchg.out.extent_order - cmp.xchg.in.extent_order;
            /* Various sanity checks. */
            if ( (cmp.xchg.nr_exchanged > cmp.xchg.in.nr_extents) ||
                 (order_delta > 0 && (cmp.xchg.nr_exchanged & ((1U << order_delta) - 1))) ||
                 /* Sizes of input and output lists do not overflow an int? */
                 ((~0U >> cmp.xchg.in.extent_order) < cmp.xchg.in.nr_extents) ||
                 ((~0U >> cmp.xchg.out.extent_order) < cmp.xchg.out.nr_extents) ||
                 /* Sizes of input and output lists match? */
                 ((cmp.xchg.in.nr_extents << cmp.xchg.in.extent_order) !=
                  (cmp.xchg.out.nr_extents << cmp.xchg.out.extent_order)) )
                return -EINVAL;

            start_extent = cmp.xchg.nr_exchanged;
            end_extent = (COMPAT_ARG_XLAT_SIZE - sizeof(*nat.xchg)) /
                         (((1U << __builtin_abs(order_delta)) + 1) *
                          sizeof(*space));
            if ( end_extent == 0 )
            {
                printk("Cannot translate compatibility mode XENMEM_exchange extents (%u,%u)\n",
                       cmp.xchg.in.extent_order, cmp.xchg.out.extent_order);
                return -E2BIG;
            }
            if ( order_delta > 0 )
                end_extent <<= order_delta;
            end_extent += start_extent;
            if ( end_extent > cmp.xchg.in.nr_extents )
                end_extent = cmp.xchg.in.nr_extents;

            space = (xen_pfn_t *)(nat.xchg + 1);
            /* Code below depends upon .in preceding .out. */
            BUILD_BUG_ON(offsetof(xen_memory_exchange_t, in) > offsetof(xen_memory_exchange_t, out));
#define XLAT_memory_reservation_HNDL_extent_start(_d_, _s_) \
            do \
            { \
                set_xen_guest_handle((_d_)->extent_start, space - start_extent); \
                for ( i = start_extent; i < end_extent; ++i ) \
                { \
                    compat_pfn_t pfn; \
                    if ( __copy_from_compat_offset(&pfn, (_s_)->extent_start, i, 1) ) \
                        return -EFAULT; \
                    *space++ = pfn; \
                } \
                if ( order_delta > 0 ) \
                { \
                    start_extent >>= order_delta; \
                    end_extent >>= order_delta; \
                } \
                else \
                { \
                    start_extent <<= -order_delta; \
                    end_extent <<= -order_delta; \
                } \
                order_delta = -order_delta; \
            } while (0)
            XLAT_memory_exchange(nat.xchg, &cmp.xchg);
#undef XLAT_memory_reservation_HNDL_extent_start

            if ( end_extent < cmp.xchg.in.nr_extents )
            {
                nat.xchg->in.nr_extents = end_extent;
                if ( order_delta >= 0 )
                    nat.xchg->out.nr_extents = end_extent >> order_delta;
                else
                    nat.xchg->out.nr_extents = end_extent << order_delta;
                ++split;
            }

            break;
        }

        case XENMEM_current_reservation:
        case XENMEM_maximum_reservation:
        {
#define xen_domid_t domid_t
#define compat_domid_t domid_compat_t
            CHECK_TYPE(domid);
#undef compat_domid_t
#undef xen_domid_t
        }
        case XENMEM_maximum_ram_page:
            nat.hnd = compat;
            break;

        case XENMEM_translate_gpfn_list:
            if ( copy_from_guest(&cmp.xlat, compat, 1) )
                return -EFAULT;

            /* Is size too large for us to encode a continuation? */
            if ( cmp.xlat.nr_gpfns > (UINT_MAX >> MEMOP_EXTENT_SHIFT) )
                return -EINVAL;

            if ( !compat_handle_okay(cmp.xlat.gpfn_list, cmp.xlat.nr_gpfns) ||
                 !compat_handle_okay(cmp.xlat.mfn_list,  cmp.xlat.nr_gpfns) )
                return -EFAULT;

            end_extent = start_extent + (COMPAT_ARG_XLAT_SIZE - sizeof(*nat.xlat)) /
                                        sizeof(*space);
            if ( end_extent > cmp.xlat.nr_gpfns )
                end_extent = cmp.xlat.nr_gpfns;

            space = (xen_pfn_t *)(nat.xlat + 1);
            /* Code below depends upon .gpfn_list preceding .mfn_list. */
            BUILD_BUG_ON(offsetof(xen_translate_gpfn_list_t, gpfn_list) > offsetof(xen_translate_gpfn_list_t, mfn_list));
#define XLAT_translate_gpfn_list_HNDL_gpfn_list(_d_, _s_) \
            do \
            { \
                set_xen_guest_handle((_d_)->gpfn_list, space - start_extent); \
                for ( i = start_extent; i < end_extent; ++i ) \
                { \
                    compat_pfn_t pfn; \
                    if ( __copy_from_compat_offset(&pfn, (_s_)->gpfn_list, i, 1) ) \
                        return -EFAULT; \
                    *space++ = pfn; \
                } \
            } while (0)
#define XLAT_translate_gpfn_list_HNDL_mfn_list(_d_, _s_) \
            (_d_)->mfn_list = (_d_)->gpfn_list
            XLAT_translate_gpfn_list(nat.xlat, &cmp.xlat);
#undef XLAT_translate_gpfn_list_HNDL_mfn_list
#undef XLAT_translate_gpfn_list_HNDL_gpfn_list

            if ( end_extent < cmp.xlat.nr_gpfns )
            {
                nat.xlat->nr_gpfns = end_extent;
                ++split;
            }

            break;

        default:
            return compat_arch_memory_op(cmd, compat);
        }

        rc = do_memory_op(cmd, nat.hnd);
        if ( rc < 0 )
            return rc;

        cmd = 0;
        if ( hypercall_xlat_continuation(&cmd, 0x02, nat.hnd, compat) )
        {
            BUG_ON(rc != __HYPERVISOR_memory_op);
            BUG_ON((cmd & MEMOP_CMD_MASK) != op);
            split = -1;
        }

        switch ( op )
        {
        case XENMEM_increase_reservation:
        case XENMEM_decrease_reservation:
        case XENMEM_populate_physmap:
            end_extent = split >= 0 ? rc : cmd >> MEMOP_EXTENT_SHIFT;
            if ( op != XENMEM_decrease_reservation &&
                 !guest_handle_is_null(nat.rsrv->extent_start) )
            {
                for ( ; start_extent < end_extent; ++start_extent )
                {
                    compat_pfn_t pfn = nat.rsrv->extent_start.p[start_extent];

                    BUG_ON(pfn != nat.rsrv->extent_start.p[start_extent]);
                    if ( __copy_to_compat_offset(cmp.rsrv.extent_start, start_extent, &pfn, 1) )
                    {
                        if ( split >= 0 )
                        {
                            rc = start_extent;
                            split = 0;
                        }
                        else
                            /*
                             * Short of being able to cancel the continuation,
                             * force it to restart here; eventually we shall
                             * get out of this state.
                             */
                            rc = (start_extent << MEMOP_EXTENT_SHIFT) | op;
                        break;
                    }
                }
            }
            else
                start_extent = end_extent;
            break;

        case XENMEM_exchange:
        {
            DEFINE_XEN_GUEST_HANDLE(compat_memory_exchange_t);
            int order_delta;

            BUG_ON(split >= 0 && rc);
            BUG_ON(end_extent < nat.xchg->nr_exchanged);
            end_extent = nat.xchg->nr_exchanged;

            order_delta = cmp.xchg.out.extent_order - cmp.xchg.in.extent_order;
            if ( order_delta > 0 )
            {
                start_extent >>= order_delta;
                BUG_ON(end_extent & ((1U << order_delta) - 1));
                end_extent >>= order_delta;
            }
            else
            {
                start_extent <<= -order_delta;
                end_extent <<= -order_delta;
            }

            for ( ; start_extent < end_extent; ++start_extent )
            {
                compat_pfn_t pfn = nat.xchg->out.extent_start.p[start_extent];

                BUG_ON(pfn != nat.xchg->out.extent_start.p[start_extent]);
                /* Note that we ignore errors accessing the output extent list. */
                __copy_to_compat_offset(cmp.xchg.out.extent_start, start_extent, &pfn, 1);
            }

            cmp.xchg.nr_exchanged = nat.xchg->nr_exchanged;
            if ( copy_field_to_guest(guest_handle_cast(compat, compat_memory_exchange_t),
                                     &cmp.xchg, nr_exchanged) )
            {
                if ( split < 0 )
                    /* Cannot cancel the continuation... */
                    domain_crash(current->domain);
                return -EFAULT;
            }
            break;
        }

        case XENMEM_maximum_ram_page:
        case XENMEM_current_reservation:
        case XENMEM_maximum_reservation:
            break;

        case XENMEM_translate_gpfn_list:
            if ( split < 0 )
                end_extent = cmd >> MEMOP_EXTENT_SHIFT;
            else
                BUG_ON(rc);

            for ( ; start_extent < end_extent; ++start_extent )
            {
                compat_pfn_t pfn = nat.xlat->mfn_list.p[start_extent];

                BUG_ON(pfn != nat.xlat->mfn_list.p[start_extent]);
                if ( __copy_to_compat_offset(cmp.xlat.mfn_list, start_extent, &pfn, 1) )
                {
                    if ( split < 0 )
                        /* Cannot cancel the continuation... */
                        domain_crash(current->domain);
                    return -EFAULT;
                }
            }
            break;

        default:
            domain_crash(current->domain);
            split = 0;
            break;
        }

        cmd = op | (start_extent << MEMOP_EXTENT_SHIFT);
        if ( split > 0 && hypercall_preempt_check() )
            return hypercall_create_continuation(
                __HYPERVISOR_memory_op, "ih", cmd, compat);
    } while ( split > 0 );

    return rc;
}
