/*
 * Copyright (C) 2011      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#ifndef LIBXL_JSON_H
#define LIBXL_JSON_H

#include <yajl/yajl_gen.h>
#include <yajl/yajl_parse.h>

#ifdef HAVE_YAJL_VERSION
#  include <yajl/yajl_version.h>
#endif

#include <_libxl_types_json.h>

/* YAJL version check */
#if defined(YAJL_MAJOR) && (YAJL_MAJOR > 1)
#  define HAVE_YAJL_V2 1
#endif

#ifdef HAVE_YAJL_V2

typedef size_t libxl_yajl_length;

static inline yajl_handle libxl__yajl_alloc(const yajl_callbacks *callbacks,
                                            yajl_alloc_funcs *allocFuncs,
                                            void *ctx)
{
    return yajl_alloc(callbacks, allocFuncs, ctx);
}

static inline yajl_gen libxl__yajl_gen_alloc(const yajl_alloc_funcs *allocFuncs)
{
    return yajl_gen_alloc(allocFuncs);
}

#else /* !HAVE_YAJL_V2 */

#define yajl_complete_parse yajl_parse_complete

typedef unsigned int libxl_yajl_length;

static inline yajl_handle libxl__yajl_alloc(const yajl_callbacks *callbacks,
                                            const yajl_alloc_funcs *allocFuncs,
                                            void *ctx)
{
    yajl_parser_config cfg = {
        .allowComments = 1,
        .checkUTF8 = 1,
    };
    return yajl_alloc(callbacks, &cfg, allocFuncs, ctx);
}

static inline yajl_gen libxl__yajl_gen_alloc(const yajl_alloc_funcs *allocFuncs)
{
    yajl_gen_config conf = { 1, "    " };
    return yajl_gen_alloc(&conf, allocFuncs);
}

#endif /* !HAVE_YAJL_V2 */

#endif /* LIBXL_JSON_H */
