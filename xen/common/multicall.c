/* -*-  Mode:C; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */
/******************************************************************************
 * multicall.c
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/perfc.h>
#include <xen/sched.h>
#include <xen/event.h>
#include <xen/multicall.h>

struct mc_state mc_state[NR_CPUS];

long do_multicall(multicall_entry_t *call_list, unsigned int nr_calls)
{
    struct mc_state *mcs = &mc_state[smp_processor_id()];
    unsigned int     i;

    if ( unlikely(__test_and_set_bit(_MCSF_in_multicall, &mcs->flags)) )
    {
        DPRINTK("Multicall reentry is disallowed.\n");
        return -EINVAL;
    }

    if ( unlikely(!array_access_ok(VERIFY_WRITE, call_list, 
                                   nr_calls, sizeof(*call_list))) )
    {
        DPRINTK("Bad memory range %p for %u*%u bytes.\n",
                call_list, nr_calls, sizeof(*call_list));
        goto fault;
    }

    for ( i = 0; i < nr_calls; i++ )
    {
        if ( unlikely(__copy_from_user(&mcs->call, &call_list[i], 
                                       sizeof(*call_list))) )
        {
            DPRINTK("Error copying from user range %p for %u bytes.\n",
                    &call_list[i], sizeof(*call_list));
            goto fault;
        }

        do_multicall_call(&mcs->call);

        if ( unlikely(__put_user(mcs->call.args[5], &call_list[i].args[5])) )
        {
            DPRINTK("Error writing result back to multicall block.\n");
            goto fault;
        }

        if ( hypercall_preempt_check() )
        {
            /*
             * Copy the sub-call continuation if it was preempted.
             * Otherwise skip over the sub-call entirely.
             */
            if ( !test_bit(_MCSF_call_preempted, &mcs->flags) )
                i++;
            else
                (void)__copy_to_user(&call_list[i], &mcs->call,
                                     sizeof(*call_list));

            /* Only create a continuation if there is work left to be done. */
            if ( i < nr_calls )
            {
                mcs->flags = 0;
                return hypercall2_create_continuation(
                    __HYPERVISOR_multicall, &call_list[i], nr_calls-i);
            }
        }
    }

    mcs->flags = 0;
    return 0;

 fault:
    mcs->flags = 0;
    return -EFAULT;
}
