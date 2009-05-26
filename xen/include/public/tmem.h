/******************************************************************************
 * tmem.h
 * 
 * Guest OS interface to Xen Transcendent Memory.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Copyright (c) 2004, K A Fraser
 */

#ifndef __XEN_PUBLIC_TMEM_H__
#define __XEN_PUBLIC_TMEM_H__

#include "xen.h"

/* Commands to HYPERVISOR_tmem_op() */
#define TMEM_CONTROL               0
#define TMEM_NEW_POOL              1
#define TMEM_DESTROY_POOL          2
#define TMEM_NEW_PAGE              3
#define TMEM_PUT_PAGE              4
#define TMEM_GET_PAGE              5
#define TMEM_FLUSH_PAGE            6
#define TMEM_FLUSH_OBJECT          7
#define TMEM_READ                  8
#define TMEM_WRITE                 9
#define TMEM_XCHG                 10

/* Subops for HYPERVISOR_tmem_op(TMEM_CONTROL) */
#define TMEMC_THAW                 0
#define TMEMC_FREEZE               1
#define TMEMC_FLUSH                2
#define TMEMC_DESTROY              3
#define TMEMC_LIST                 4
#define TMEMC_SET_WEIGHT           5
#define TMEMC_SET_CAP              6
#define TMEMC_SET_COMPRESS         7

/* Bits for HYPERVISOR_tmem_op(TMEM_NEW_POOL) */
#define TMEM_POOL_PERSIST          1
#define TMEM_POOL_SHARED           2
#define TMEM_POOL_PAGESIZE_SHIFT   4
#define TMEM_POOL_PAGESIZE_MASK  0xf
#define TMEM_POOL_VERSION_SHIFT   24
#define TMEM_POOL_VERSION_MASK  0xff

/* Special errno values */
#define EFROZEN                 1000
#define EEMPTY                  1001


#ifndef __ASSEMBLY__
typedef XEN_GUEST_HANDLE(void) tmem_cli_mfn_t;
typedef XEN_GUEST_HANDLE(char) tmem_cli_va_t;
struct tmem_op {
    uint32_t cmd;
    int32_t pool_id; /* private > 0; shared < 0; 0 is invalid */
    union {
        struct {  /* for cmd == TMEM_NEW_POOL */
            uint64_t uuid[2];
            uint32_t flags;
        };
        struct {  /* for cmd == TMEM_CONTROL */
            uint32_t subop;
            uint32_t cli_id;
            uint32_t arg1;
            uint32_t arg2;
            tmem_cli_va_t buf;
        };
        struct {
            uint64_t object;
            uint32_t index;
            uint32_t tmem_offset;
            uint32_t pfn_offset;
            uint32_t len;
            tmem_cli_mfn_t cmfn; /* client machine page frame */
        };
    };
};
typedef struct tmem_op tmem_op_t;
DEFINE_XEN_GUEST_HANDLE(tmem_op_t);
typedef XEN_GUEST_HANDLE_64(tmem_op_t) tmem_cli_op_t;

#endif

#endif /* __XEN_PUBLIC_TMEM_H__ */

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
