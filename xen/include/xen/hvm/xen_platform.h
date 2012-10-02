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

#ifndef __ASM_X86_HVM_XEN_PLATFORM_H__
#define __ASM_X86_HVM_XEN_PLATFORM_H__

# include <xen/types.h>

#define XEN_PLATFORM_IOPORT 0x10

/* Unplug events */
#define XEN_PLATFORM_UNPLUG_ALL_IDE_DISKS 1
#define XEN_PLATFORM_UNPLUG_ALL_NICS 2
#define XEN_PLATFORM_UNPLUG_AUX_IDE_DISKS 4

void xen_platform_init(struct domain *d);

#endif /* __ASM_X86_HVM_XEN_PLATFORM_H__ */
