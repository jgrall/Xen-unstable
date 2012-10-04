/*
 * XEN platform pci device, formerly known as the event channel device
 *
 * Copyright (c) 2003-2004 Intel Corp.
 * Copyright (c) 2006 XenSource
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <asm/hvm/support.h>
#include <xen/hvm/xen_platform.h>

/*
 * With disaggregation we need to forward unplug event to all ioreq servers.
 * This function will trap the ioport the prepare request.
 */
static int handle_platform_io(int dir, uint32_t port, uint32_t bytes,
                              uint32_t *val)
{
    struct vcpu *v = current;
    ioreq_t *p = get_ioreq(v);

    /* Dispatch to another handler if it's not the right size and ioport */
    if ( port != XEN_PLATFORM_IOPORT && bytes != 2 && dir != IOREQ_WRITE )
        return X86EMUL_UNHANDLEABLE;

    p->type = IOREQ_TYPE_EVENT;
    p->size = 2;
    p->data = 0x0;
    p->data_is_ptr = 0;
    if ( *val & XEN_PLATFORM_UNPLUG_ALL_IDE_DISKS )
        p->data |= IOREQ_EVENT_UNPLUG_ALL_IDE_DISKS;
    if ( *val & XEN_PLATFORM_UNPLUG_ALL_NICS )
        p->data |= IOREQ_EVENT_UNPLUG_ALL_NICS;
    if ( *val & XEN_PLATFORM_UNPLUG_AUX_IDE_DISKS )
        p->data |= IOREQ_EVENT_UNPLUG_AUX_IDE_DISKS;
    p->count = 0;
    p->addr = 0;

    return X86EMUL_UNHANDLEABLE;
}

void xen_platform_init(struct domain *d)
{
    register_portio_handler(d, XEN_PLATFORM_IOPORT, 16, handle_platform_io);
}
