/******************************************************************************
 * Arch-specific dom0_ops.c
 * 
 * Process command requests from domain-0 guest OS.
 * 
 * Copyright (c) 2002, K A Fraser
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <public/dom0_ops.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <asm/pdb.h>
#include <xen/trace.h>
#include <xen/console.h>
#include <public/sched_ctl.h>

long arch_do_dom0_op(dom0_op_t *op, dom0_op_t *u_dom0_op)
{
    long ret = 0;

    if ( !IS_PRIV(current->domain) )
        return -EPERM;

    switch ( op->cmd )
    {
    case DOM0_GETPAGEFRAMEINFO:
    {
        struct pfn_info *page;
        unsigned long pfn = op->u.getpageframeinfo.pfn;
        domid_t dom = op->u.getpageframeinfo.domain;
        struct domain *d;

        ret = -EINVAL;

        if ( unlikely(pfn >= max_page) || 
             unlikely((d = find_domain_by_id(dom)) == NULL) )
            break;

        page = &frame_table[pfn];

        if ( likely(get_page(page, d)) )
        {
            ret = 0;

            op->u.getpageframeinfo.type = NOTAB;

            if ( (page->u.inuse.type_info & PGT_count_mask) != 0 )
            {
                switch ( page->u.inuse.type_info & PGT_type_mask )
                {
		default:
		    panic("No such page type\n");
                    break;
                }
            }
            
            put_page(page);
        }

        put_domain(d);

        copy_to_user(u_dom0_op, op, sizeof(*op));
    }
    break;

    case DOM0_GETPAGEFRAMEINFO2:
    {
#define GPF2_BATCH 128
        int n,j;
        int num = op->u.getpageframeinfo2.num;
        domid_t dom = op->u.getpageframeinfo2.domain;
        unsigned long *s_ptr = (unsigned long*) op->u.getpageframeinfo2.array;
        struct domain *d;
        unsigned long *l_arr;
        ret = -ESRCH;

        if ( unlikely((d = find_domain_by_id(dom)) == NULL) )
            break;

        if ( unlikely(num > 1024) )
        {
            ret = -E2BIG;
            break;
        }

        l_arr = (unsigned long *)alloc_xenheap_page();
 
        ret = 0;
        for( n = 0; n < num; )
        {
            int k = ((num-n)>GPF2_BATCH)?GPF2_BATCH:(num-n);

            if ( copy_from_user(l_arr, &s_ptr[n], k*sizeof(unsigned long)) )
            {
                ret = -EINVAL;
                break;
            }
     
            for( j = 0; j < k; j++ )
            {      
                struct pfn_info *page;
                unsigned long mfn = l_arr[j];

                if ( unlikely(mfn >= max_page) )
                    goto e2_err;

                page = &frame_table[mfn];
  
                if ( likely(get_page(page, d)) )
                {
                    unsigned long type = 0;

                    switch( page->u.inuse.type_info & PGT_type_mask )
                    {
		    default:
			panic("No such page type\n");
			break;
                    }

                    if ( page->u.inuse.type_info & PGT_pinned )
                        type |= LPINTAB;
                    l_arr[j] |= type;
                    put_page(page);
                }
                else
                {
                e2_err:
                    l_arr[j] |= XTAB;
                }

            }

            if ( copy_to_user(&s_ptr[n], l_arr, k*sizeof(unsigned long)) )
            {
                ret = -EINVAL;
                break;
            }

            n += j;
        }

        free_xenheap_page((unsigned long)l_arr);

        put_domain(d);
    }
    break;
#ifndef CONFIG_VTI
    /*
     * NOTE: DOM0_GETMEMLIST has somewhat different semantics on IA64 -
     * it actually allocates and maps pages.
     */
    case DOM0_GETMEMLIST:
    {
        unsigned long i;
        struct domain *d = find_domain_by_id(op->u.getmemlist.domain);
        unsigned long start_page = op->u.getmemlist.max_pfns >> 32;
        unsigned long nr_pages = op->u.getmemlist.max_pfns & 0xffffffff;
        unsigned long pfn;
        unsigned long *buffer = op->u.getmemlist.buffer;
        struct page *page;

        ret = -EINVAL;
        if ( d != NULL )
        {
            ret = 0;

            for ( i = start_page; i < (start_page + nr_pages); i++ )
            {
                page = map_new_domain_page(d, i << PAGE_SHIFT);
                if ( page == NULL )
                {
                    ret = -ENOMEM;
                    break;
                }
                pfn = page_to_pfn(page);
                if ( put_user(pfn, buffer) )
                {
                    ret = -EFAULT;
                    break;
                }
                buffer++;
            }

            op->u.getmemlist.num_pfns = i - start_page;
            copy_to_user(u_dom0_op, op, sizeof(*op));
            
            put_domain(d);
        }
    }
    break;
#else
    case DOM0_GETMEMLIST:
    {
	int i;
	struct domain *d = find_domain_by_id(op->u.getmemlist.domain);
	unsigned long max_pfns = op->u.getmemlist.max_pfns;
	unsigned long pfn;
	unsigned long *buffer = op->u.getmemlist.buffer;
	struct list_head *list_ent;

	ret = -EINVAL;
	if (!d) {
	    ret = 0;

	    spin_lock(&d->page_alloc_lock);
	    list_ent = d->page_list.next;
	    for (i = 0; (i < max_pfns) && (list_ent != &d->page_list); i++) {
		pfn = list_entry(list_ent, struct pfn_info, list) -
		    frame_table;
		if (put_user(pfn, buffer)) {
		    ret = -EFAULT;
		    break;
		}
		buffer++;
		list_ent = frame_table[pfn].list.next;
	    }
	    spin_unlock(&d->page_alloc_lock);

	    op->u.getmemlist.num_pfns = i;
	    copy_to_user(u_dom0_op, op, sizeof(*op));

	    put_domain(d);
	}
    }
    break;
#endif // CONFIG_VTI
    default:
        ret = -ENOSYS;

    }

    return ret;
}
