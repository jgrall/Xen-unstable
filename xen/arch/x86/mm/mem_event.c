/******************************************************************************
 * arch/x86/mm/mem_event.c
 *
 * Memory event support.
 *
 * Copyright (c) 2009 Citrix (R)&D) Ltd. (Patrick Colp)
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <xen/event.h>
#include <asm/p2m.h>
#include <asm/mem_event.h>


#define xen_mb()   mb()
#define xen_rmb()  rmb()
#define xen_wmb()  wmb()


#define MEM_EVENT_RING_THRESHOLD 4


static void mem_event_notify(struct domain *d)
{
    prepare_wait_on_xen_event_channel(d->mem_event.xen_port);
    notify_via_xen_event_channel(d->mem_event.xen_port);
}


int mem_event_enable(struct domain *d, mfn_t ring_mfn, mfn_t shared_mfn)
{
    int rc;

    /* Map ring and shared pages */
    d->mem_event.ring_page = map_domain_page(mfn_x(ring_mfn));
    if ( d->mem_event.ring_page == NULL )
        goto err;

    d->mem_event.shared_page = map_domain_page(mfn_x(shared_mfn));
    if ( d->mem_event.shared_page == NULL )
        goto err_ring;

    /* Allocate event channel */
    rc = alloc_unbound_xen_event_channel(d->vcpu[0],
                                         current->domain->domain_id);
    if ( rc < 0 )
        goto err_shared;

    ((mem_event_shared_page_t *)d->mem_event.shared_page)->port = rc;
    d->mem_event.xen_port = rc;

    /* Initialise tasklet */
    tasklet_init(&d->mem_event.tasklet,
                 (void(*)(unsigned long))mem_event_notify,
                 (unsigned long)d);

    /* Prepare ring buffer */
    FRONT_RING_INIT(&d->mem_event.front_ring,
                    (mem_event_sring_t *)d->mem_event.ring_page,
                    PAGE_SIZE);

    mem_event_ring_lock_init(d);

    d->mem_event.paused = 0;
    d->mem_event.enabled = 1;

    return 0;

 err_shared:
    unmap_domain_page(d->mem_event.shared_page);
    d->mem_event.shared_page = NULL;
 err_ring:
    unmap_domain_page(d->mem_event.ring_page);
    d->mem_event.ring_page = NULL;
 err:
    return 1;
}

int mem_event_disable(struct domain *d)
{
    d->mem_event.enabled = 0;
    d->mem_event.paused = 0;

    unmap_domain_page(d->mem_event.ring_page);
    d->mem_event.ring_page = NULL;

    unmap_domain_page(d->mem_event.shared_page);
    d->mem_event.shared_page = NULL;

    return 0;
}

void mem_event_put_request(struct domain *d, mem_event_request_t *req)
{
    mem_event_front_ring_t *front_ring;
    RING_IDX req_prod;

    mem_event_ring_lock(d);

    front_ring = &d->mem_event.front_ring;
    req_prod = front_ring->req_prod_pvt;

    /* Copy request */
    memcpy(RING_GET_REQUEST(front_ring, req_prod), req, sizeof(*req));
    req_prod++;

    /* Update ring */
    front_ring->req_prod_pvt = req_prod;
    RING_PUSH_REQUESTS(front_ring);

    mem_event_ring_unlock(d);

    tasklet_schedule(&d->mem_event.tasklet);
}

void mem_event_get_response(struct domain *d, mem_event_response_t *rsp)
{
    mem_event_front_ring_t *front_ring;
    RING_IDX rsp_cons;

    mem_event_ring_lock(d);

    front_ring = &d->mem_event.front_ring;
    rsp_cons = front_ring->rsp_cons;

    /* Copy response */
    memcpy(rsp, RING_GET_RESPONSE(front_ring, rsp_cons), sizeof(*rsp));
    rsp_cons++;

    /* Update ring */
    front_ring->rsp_cons = rsp_cons;
    front_ring->sring->rsp_event = rsp_cons + 1;

    mem_event_ring_unlock(d);
}

void mem_event_unpause_vcpus(struct domain *d)
{
    struct vcpu *v;

    for_each_vcpu(d, v)
    {
        if ( d->mem_event.paused_vcpus[v->vcpu_id] )
        {
            vcpu_unpause(v);
            d->mem_event.paused_vcpus[v->vcpu_id] = 0;
        }
    }
}

int mem_event_pause_vcpu(struct domain *d, struct vcpu *v)
{
    vcpu_pause_nosync(v);
    d->mem_event.paused_vcpus[v->vcpu_id] = 1;

    return 0;
}

int mem_event_check_ring(struct domain *d)
{
    int free_requests;
    int ring_full;

    mem_event_ring_lock(d);

    free_requests = RING_FREE_REQUESTS(&d->mem_event.front_ring);
    ring_full = free_requests < MEM_EVENT_RING_THRESHOLD;

    if ( (current->domain->domain_id == d->domain_id) && ring_full )
        mem_event_pause_vcpu(d, current);

    mem_event_ring_unlock(d);

    return ring_full;
}


/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
