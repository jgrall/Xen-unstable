/******************************************************************************
 * xc_domain.c
 * 
 * API for manipulating and obtaining information on domains.
 * 
 * Copyright (c) 2003, K A Fraser.
 */

#include "xc_private.h"

int xc_domain_create(int xc_handle,
                     unsigned int mem_kb, 
                     const char *name,
		     int cpu,
                     u64 *pdomid)
{
    int err;
    dom0_op_t op;

    op.cmd = DOM0_CREATEDOMAIN;
    op.u.createdomain.memory_kb = mem_kb;
    strncpy(op.u.createdomain.name, name, MAX_DOMAIN_NAME);
    op.u.createdomain.name[MAX_DOMAIN_NAME-1] = '\0';
    op.u.createdomain.cpu = cpu;

    if ( (err = do_dom0_op(xc_handle, &op)) == 0 )
        *pdomid = (u64)op.u.createdomain.domain;

    return err;
}    


int xc_domain_start(int xc_handle,
                    u64 domid)
{
    dom0_op_t op;
    op.cmd = DOM0_STARTDOMAIN;
    op.u.startdomain.domain = (domid_t)domid;
    return do_dom0_op(xc_handle, &op);
}    


int xc_domain_stop(int xc_handle, 
                   u64 domid)
{
    dom0_op_t op;
    op.cmd = DOM0_STOPDOMAIN;
    op.u.stopdomain.domain = (domid_t)domid;
    return do_dom0_op(xc_handle, &op);
}    


int xc_domain_destroy(int xc_handle,
                      u64 domid, 
                      int force)
{
    dom0_op_t op;
    op.cmd = DOM0_DESTROYDOMAIN;
    op.u.destroydomain.domain = (domid_t)domid;
    op.u.destroydomain.force  = !!force;
    return do_dom0_op(xc_handle, &op);
}

int xc_domain_pincpu(int xc_handle,
                     u64 domid, 
                     int cpu)
{
    dom0_op_t op;
    op.cmd = DOM0_PINCPUDOMAIN;
    op.u.pincpudomain.domain = (domid_t)domid;
    op.u.pincpudomain.cpu  = cpu;
    return do_dom0_op(xc_handle, &op);
}


int xc_domain_getinfo(int xc_handle,
                      u64 first_domid,
                      unsigned int max_doms,
                      xc_dominfo_t *info)
{
    unsigned int nr_doms;
    u64 next_domid = first_domid;
    dom0_op_t op;

    for ( nr_doms = 0; nr_doms < max_doms; nr_doms++ )
    {
        op.cmd = DOM0_GETDOMAININFO;
        op.u.getdomaininfo.domain = (domid_t)next_domid;
        op.u.getdomaininfo.ctxt = NULL; // no exec context info, thanks.
        if ( do_dom0_op(xc_handle, &op) < 0 )
            break;
        info->domid   = (u64)op.u.getdomaininfo.domain;
        info->cpu     = op.u.getdomaininfo.processor;
        info->has_cpu = op.u.getdomaininfo.has_cpu;
        info->stopped = (op.u.getdomaininfo.state == DOMSTATE_STOPPED);
        info->nr_pages = op.u.getdomaininfo.tot_pages;
        info->max_memkb = op.u.getdomaininfo.max_pages<<(PAGE_SHIFT-10);
        info->shared_info_frame = op.u.getdomaininfo.shared_info_frame;
        info->cpu_time = op.u.getdomaininfo.cpu_time;
        strncpy(info->name, op.u.getdomaininfo.name, XC_DOMINFO_MAXNAME);
        info->name[XC_DOMINFO_MAXNAME-1] = '\0';

        next_domid = (u64)op.u.getdomaininfo.domain + 1;
        info++;
    }

    return nr_doms;
}

int xc_shadow_control(int xc_handle,
                      u64 domid, 
                      unsigned int sop,
		      unsigned long *dirty_bitmap,
		      unsigned long pages)
{
    int rc;
    dom0_op_t op;
    op.cmd = DOM0_SHADOW_CONTROL;
    op.u.shadow_control.domain = (domid_t)domid;
    op.u.shadow_control.op     = sop;
    op.u.shadow_control.dirty_bitmap = dirty_bitmap;
    op.u.shadow_control.pages  = pages;

    rc = do_dom0_op(xc_handle, &op);

    if ( rc == 0 )
	return op.u.shadow_control.pages;
    else
	return rc;
}

int xc_domain_setname(int xc_handle,
                      u64 domid, 
		      char *name)
{
    dom0_op_t op;
    op.cmd = DOM0_SETDOMAINNAME;
    op.u.setdomainname.domain = (domid_t)domid;
    strncpy(op.u.setdomainname.name, name, MAX_DOMAIN_NAME);
    return do_dom0_op(xc_handle, &op);
}

int xc_domain_setinitialmem(int xc_handle,
			    u64 domid, 
			    unsigned int initial_memkb)
{
    dom0_op_t op;
    op.cmd = DOM0_SETDOMAININITIALMEM;
    op.u.setdomaininitialmem.domain = (domid_t)domid;
    op.u.setdomaininitialmem.initial_memkb = initial_memkb;
    return do_dom0_op(xc_handle, &op);
}

int xc_domain_setmaxmem(int xc_handle,
			    u64 domid, 
			    unsigned int max_memkb)
{
    dom0_op_t op;
    op.cmd = DOM0_SETDOMAINMAXMEM;
    op.u.setdomainmaxmem.domain = (domid_t)domid;
    op.u.setdomainmaxmem.max_memkb = max_memkb;
    return do_dom0_op(xc_handle, &op);
}

