/******************************************************************************
 * guest_access.h
 * 
 * Copyright (x) 2006, K A Fraser
 */

#ifndef __XEN_GUEST_ACCESS_H__
#define __XEN_GUEST_ACCESS_H__

#include <asm/uaccess.h>

/* Is the guest handle a NULL reference? */
#define guest_handle_is_null(hnd)        ((hnd).p == NULL)

/* Offset the given guest handle into the array it refers to. */
#define guest_handle_add_offset(hnd, nr) ((hnd).p += (nr))

/* Cast a guest handle to the specified type of handle. */
#define guest_handle_cast(hnd, type) ({         \
    type *_x = (hnd).p;                         \
    (guest_handle(type)) { _x };                \
})

/*
 * Copy an array of objects to guest context via a guest handle.
 * Optionally specify an offset into the guest array.
 */
#define copy_to_guest_offset(hnd, off, ptr, nr) ({      \
    const typeof(ptr) _x = (hnd).p;                     \
    const typeof(ptr) _y = (ptr);                       \
    copy_to_user(_x+(off), _y, sizeof(*_x)*(nr));       \
})
#define copy_to_guest(hnd, ptr, nr)                     \
    copy_to_guest_offset(hnd, 0, ptr, nr)

/*
 * Copy an array of objects from guest context via a guest handle.
 * Optionally specify an offset into the guest array.
 */
#define copy_from_guest_offset(ptr, hnd, off, nr) ({    \
    const typeof(ptr) _x = (hnd).p;                     \
    const typeof(ptr) _y = (ptr);                       \
    copy_from_user(_y, _x+(off), sizeof(*_x)*(nr));     \
})
#define copy_from_guest(ptr, hnd, nr)                   \
    copy_from_guest_offset(ptr, hnd, 0, nr)

/*
 * Pre-validate a guest handle.
 * Allows use of faster __copy_* functions.
 */
#define guest_handle_okay(hnd, nr)                      \
    array_access_ok((hnd).p, (nr), sizeof(*(hnd).p))

#define __copy_to_guest_offset(hnd, off, ptr, nr) ({    \
    const typeof(ptr) _x = (hnd).p;                     \
    const typeof(ptr) _y = (ptr);                       \
    __copy_to_user(_x+(off), _y, sizeof(*_x)*(nr));     \
})
#define __copy_to_guest(hnd, ptr, nr)                   \
    __copy_to_guest_offset(hnd, 0, ptr, nr)

#define __copy_from_guest_offset(ptr, hnd, off, nr) ({  \
    const typeof(ptr) _x = (hnd).p;                     \
    const typeof(ptr) _y = (ptr);                       \
    __copy_from_user(_y, _x+(off), sizeof(*_x)*(nr));   \
})
#define __copy_from_guest(ptr, hnd, nr)                 \
    __copy_from_guest_offset(ptr, hnd, 0, nr)

#endif /* __XEN_GUEST_ACCESS_H__ */
