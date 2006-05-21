/******************************************************************************
 * callback.h
 *
 * Register guest OS callbacks with Xen.
 *
 * Copyright (c) 2006, Ian Campbell
 */

#ifndef __XEN_PUBLIC_CALLBACK_H__
#define __XEN_PUBLIC_CALLBACK_H__

#include "xen.h"

/*
 * Prototype for this hypercall is:
 *   long callback_op(int cmd, void *extra_args)
 * @cmd        == CALLBACKOP_??? (callback operation).
 * @extra_args == Operation-specific extra arguments (NULL if none).
 */

#define CALLBACKTYPE_event                 0
#define CALLBACKTYPE_failsafe              1
#define CALLBACKTYPE_syscall               2 /* x86_64 only */
/*
 * sysenter is only available on x86_32 with the
 * supervisor_mode_kernel option enabled.
 */
#define CALLBACKTYPE_sysenter              3
#define CALLBACKTYPE_nmi                   4

/*
 * Register a callback.
 */
#define CALLBACKOP_register                0
struct callback_register {
     int type;
     xen_callback_t address;
};
typedef struct callback_register callback_register_t;
DEFINE_XEN_GUEST_HANDLE(callback_register_t);

/*
 * Unregister a callback.
 *
 * Not all callbacks can be unregistered. -EINVAL will be returned if
 * you attempt to unregister such a callback.
 */
#define CALLBACKOP_unregister              1
struct callback_unregister {
     int type;
};
typedef struct callback_unregister callback_unregister_t;
DEFINE_XEN_GUEST_HANDLE(callback_unregister_t);

#endif /* __XEN_PUBLIC_CALLBACK_H__ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
