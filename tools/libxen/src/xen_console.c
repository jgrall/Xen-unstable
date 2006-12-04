/*
 * Copyright (c) 2006, XenSource Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */


#include <stddef.h>
#include <stdlib.h>

#include "xen_common.h"
#include "xen_console.h"
#include "xen_console_protocol_internal.h"
#include "xen_internal.h"
#include "xen_vm.h"


XEN_FREE(xen_console)
XEN_SET_ALLOC_FREE(xen_console)
XEN_ALLOC(xen_console_record)
XEN_SET_ALLOC_FREE(xen_console_record)
XEN_ALLOC(xen_console_record_opt)
XEN_RECORD_OPT_FREE(xen_console)
XEN_SET_ALLOC_FREE(xen_console_record_opt)


static const struct_member xen_console_record_struct_members[] =
    {
        { .key = "uuid",
          .type = &abstract_type_string,
          .offset = offsetof(xen_console_record, uuid) },
        { .key = "protocol",
          .type = &xen_console_protocol_abstract_type_,
          .offset = offsetof(xen_console_record, protocol) },
        { .key = "uri",
          .type = &abstract_type_string,
          .offset = offsetof(xen_console_record, uri) },
        { .key = "VM",
          .type = &abstract_type_ref,
          .offset = offsetof(xen_console_record, vm) }
    };

const abstract_type xen_console_record_abstract_type_ =
    {
       .typename = STRUCT,
       .struct_size = sizeof(xen_console_record),
       .member_count =
           sizeof(xen_console_record_struct_members) / sizeof(struct_member),
       .members = xen_console_record_struct_members
    };


void
xen_console_record_free(xen_console_record *record)
{
    if (record == NULL)
    {
        return;
    }
    free(record->handle);
    free(record->uuid);
    free(record->uri);
    xen_vm_record_opt_free(record->vm);
    free(record);
}


bool
xen_console_get_record(xen_session *session, xen_console_record **result, xen_console console)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = console }
        };

    abstract_type result_type = xen_console_record_abstract_type_;

    *result = NULL;
    XEN_CALL_("console.get_record");

    if (session->ok)
    {
       (*result)->handle = xen_strdup_((*result)->uuid);
    }

    return session->ok;
}


bool
xen_console_get_by_uuid(xen_session *session, xen_console *result, char *uuid)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = uuid }
        };

    abstract_type result_type = abstract_type_string;

    *result = NULL;
    XEN_CALL_("console.get_by_uuid");
    return session->ok;
}


bool
xen_console_create(xen_session *session, xen_console *result, xen_console_record *record)
{
    abstract_value param_values[] =
        {
            { .type = &xen_console_record_abstract_type_,
              .u.struct_val = record }
        };

    abstract_type result_type = abstract_type_string;

    *result = NULL;
    XEN_CALL_("console.create");
    return session->ok;
}


bool
xen_console_destroy(xen_session *session, xen_console console)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = console }
        };

    xen_call_(session, "console.destroy", param_values, 1, NULL, NULL);
    return session->ok;
}


bool
xen_console_get_protocol(xen_session *session, enum xen_console_protocol *result, xen_console console)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = console }
        };

    abstract_type result_type = xen_console_protocol_abstract_type_;
    char *result_str = NULL;
    XEN_CALL_("console.get_protocol");
    *result = xen_console_protocol_from_string(session, result_str);
    return session->ok;
}


bool
xen_console_get_uri(xen_session *session, char **result, xen_console console)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = console }
        };

    abstract_type result_type = abstract_type_string;

    *result = NULL;
    XEN_CALL_("console.get_uri");
    return session->ok;
}


bool
xen_console_get_vm(xen_session *session, xen_vm *result, xen_console console)
{
    abstract_value param_values[] =
        {
            { .type = &abstract_type_string,
              .u.string_val = console }
        };

    abstract_type result_type = abstract_type_string;

    *result = NULL;
    XEN_CALL_("console.get_VM");
    return session->ok;
}


bool
xen_console_get_uuid(xen_session *session, char **result, xen_console console)
{
    *result = session->ok ? xen_strdup_((char *)console) : NULL;
    return session->ok;
}
