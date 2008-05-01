/******************************************************************************
 * xc_physdev.c
 *
 * API for manipulating physical-device access permissions.
 *
 * Copyright (c) 2004, Rolf Neugebauer (Intel Research Cambridge)
 * Copyright (c) 2004, K A Fraser (University of Cambridge)
 */

#include "xc_private.h"

int xc_physdev_pci_access_modify(int xc_handle,
                                 uint32_t domid,
                                 int bus,
                                 int dev,
                                 int func,
                                 int enable)
{
    errno = ENOSYS;
    return -1;
}

int xc_physdev_map_pirq(int xc_handle,
                        int domid,
                        int type,
                        int index,
                        int *pirq)
{
    int rc;
    struct physdev_map_pirq map;

    if ( !pirq )
        return -EINVAL;

    map.domid = domid;
    map.type = type;
    map.index = index;
    map.pirq = *pirq;

    rc = do_physdev_op(xc_handle, PHYSDEVOP_map_pirq, &map);

    if ( !rc )
        *pirq = map.pirq;

    return rc;
}

int xc_physdev_unmap_pirq(int xc_handle,
                          int domid,
                          int pirq)
{
    int rc;
    struct physdev_unmap_pirq unmap;

    unmap.domid = domid;
    unmap.pirq = pirq;

    rc = do_physdev_op(xc_handle, PHYSDEVOP_unmap_pirq, &unmap);

    return rc;
}
