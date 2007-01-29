#ifndef __LIBELF_PRIVATE_H__
#define __LIBELF_PRIVATE_H_

#ifdef __XEN__

#include <xen/config.h>
#include <xen/types.h>
#include <xen/string.h>
#include <xen/lib.h>
#include <asm/byteorder.h>
#include <public/elfnote.h>
#include <public/libelf.h>

#define elf_msg(elf, fmt, args ... ) \
	if (elf->verbose) printk(fmt, ## args )
#define elf_err(elf, fmt, args ... ) \
	printk(fmt, ## args )

#define strtoull(str, end, base) simple_strtoull(str, end, base)
#define bswap_16(x) swab16(x)
#define bswap_32(x) swab32(x)
#define bswap_64(x) swab64(x)

#define elf_strlcpy(d,s,c) strlcpy(d,s,c)

#else /* !__XEN__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>
#ifdef __sun__
#include <sys/byteorder.h>
#define bswap_16(x) BSWAP_16(x)
#define bswap_32(x) BSWAP_32(x)
#define bswap_64(x) BSWAP_64(x)
#else
#include <byteswap.h>
#endif
#include <xen/elfnote.h>
#include <xen/libelf.h>

#include "xenctrl.h"
#include "xc_private.h"

#define elf_msg(elf, fmt, args ... ) \
	if (elf->log && elf->verbose) fprintf(elf->log, fmt , ## args )
#define elf_err(elf, fmt, args ... ) do {                 \
	if (elf->log)                                     \
            fprintf(elf->log, fmt , ## args );            \
        xc_set_error(XC_INVALID_KERNEL, fmt , ## args );  \
	} while (0)

/* SysV unices have no strlcpy/strlcat. */
static inline size_t elf_strlcpy(char *dest, const char *src, size_t size)
{
    strncpy(dest, src, size-1);
    dest[size-1] = '\0';
    return strlen(src);
}

#endif

#endif /* __LIBELF_PRIVATE_H_ */
