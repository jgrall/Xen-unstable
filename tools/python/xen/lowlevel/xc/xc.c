/******************************************************************************
 * Xc.c
 * 
 * Copyright (c) 2003-2004, K A Fraser (University of Cambridge)
 */

#include <Python.h>
#include <xenctrl.h>
#include <xenguest.h>
#include <zlib.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "xenctrl.h"
#include <xen/elfnote.h>
#include <xen/tmem.h>
#include "xc_dom.h"
#include <xen/hvm/hvm_info_table.h>
#include <xen/hvm/params.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Needed for Python versions earlier than 2.3. */
#ifndef PyMODINIT_FUNC
#define PyMODINIT_FUNC DL_EXPORT(void)
#endif

#define PKG "xen.lowlevel.xc"
#define CLS "xc"

static PyObject *xc_error_obj, *zero;

typedef struct {
    PyObject_HEAD;
    int xc_handle;
} XcObject;


static PyObject *dom_op(XcObject *self, PyObject *args,
                        int (*fn)(int, uint32_t));

static PyObject *pyxc_error_to_exception(void)
{
    PyObject *pyerr;
    const xc_error *err = xc_get_last_error();
    const char *desc = xc_error_code_to_desc(err->code);

    if ( err->code == XC_ERROR_NONE )
        return PyErr_SetFromErrno(xc_error_obj);

    if ( err->message[0] != '\0' )
        pyerr = Py_BuildValue("(iss)", err->code, desc, err->message);
    else
        pyerr = Py_BuildValue("(is)", err->code, desc);

    xc_clear_last_error();

    if ( pyerr != NULL )
    {
        PyErr_SetObject(xc_error_obj, pyerr);
        Py_DECREF(pyerr);
    }

    return NULL;
}

static PyObject *pyxc_domain_dumpcore(XcObject *self, PyObject *args)
{
    uint32_t dom;
    char *corefile;

    if ( !PyArg_ParseTuple(args, "is", &dom, &corefile) )
        return NULL;

    if ( (corefile == NULL) || (corefile[0] == '\0') )
        return NULL;

    if ( xc_domain_dumpcore(self->xc_handle, dom, corefile) != 0 )
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_handle(XcObject *self)
{
    return PyInt_FromLong(self->xc_handle);
}

static PyObject *pyxc_domain_create(XcObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    uint32_t dom = 0, ssidref = 0, flags = 0, target = 0;
    int      ret, i;
    PyObject *pyhandle = NULL;
    xen_domain_handle_t handle = { 
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef };

    static char *kwd_list[] = { "domid", "ssidref", "handle", "flags", "target", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|iiOii", kwd_list,
                                      &dom, &ssidref, &pyhandle, &flags, &target))
        return NULL;
    if ( pyhandle != NULL )
    {
        if ( !PyList_Check(pyhandle) || 
             (PyList_Size(pyhandle) != sizeof(xen_domain_handle_t)) )
            goto out_exception;

        for ( i = 0; i < sizeof(xen_domain_handle_t); i++ )
        {
            PyObject *p = PyList_GetItem(pyhandle, i);
            if ( !PyInt_Check(p) )
                goto out_exception;
            handle[i] = (uint8_t)PyInt_AsLong(p);
        }
    }

    if ( (ret = xc_domain_create(self->xc_handle, ssidref,
                                 handle, flags, &dom)) < 0 )
        return pyxc_error_to_exception();

    if ( target )
        if ( (ret = xc_domain_set_target(self->xc_handle, dom, target)) < 0 )
            return pyxc_error_to_exception();


    return PyInt_FromLong(dom);

out_exception:
    errno = EINVAL;
    PyErr_SetFromErrno(xc_error_obj);
    return NULL;
}

static PyObject *pyxc_domain_max_vcpus(XcObject *self, PyObject *args)
{
    uint32_t dom, max;

    if (!PyArg_ParseTuple(args, "ii", &dom, &max))
      return NULL;

    if (xc_domain_max_vcpus(self->xc_handle, dom, max) != 0)
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_pause(XcObject *self, PyObject *args)
{
    return dom_op(self, args, xc_domain_pause);
}

static PyObject *pyxc_domain_unpause(XcObject *self, PyObject *args)
{
    return dom_op(self, args, xc_domain_unpause);
}

static PyObject *pyxc_domain_destroy_hook(XcObject *self, PyObject *args)
{
#ifdef __ia64__
    dom_op(self, args, xc_ia64_save_to_nvram);
#endif

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_destroy(XcObject *self, PyObject *args)
{
    return dom_op(self, args, xc_domain_destroy);
}

static PyObject *pyxc_domain_shutdown(XcObject *self, PyObject *args)
{
    uint32_t dom, reason;

    if ( !PyArg_ParseTuple(args, "ii", &dom, &reason) )
      return NULL;

    if ( xc_domain_shutdown(self->xc_handle, dom, reason) != 0 )
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_resume(XcObject *self, PyObject *args)
{
    uint32_t dom;
    int fast;

    if ( !PyArg_ParseTuple(args, "ii", &dom, &fast) )
        return NULL;

    if ( xc_domain_resume(self->xc_handle, dom, fast) != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vcpu_setaffinity(XcObject *self,
                                       PyObject *args,
                                       PyObject *kwds)
{
    uint32_t dom;
    int vcpu = 0, i;
    uint64_t  *cpumap;
    PyObject *cpulist = NULL;
    int nr_cpus, size;
    xc_physinfo_t info = {0}; 
    uint64_t cpumap_size = sizeof(*cpumap); 

    static char *kwd_list[] = { "domid", "vcpu", "cpumap", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|iO", kwd_list, 
                                      &dom, &vcpu, &cpulist) )
        return NULL;

    if ( xc_physinfo(self->xc_handle, &info) != 0 )
        return pyxc_error_to_exception();
  
    nr_cpus = info.nr_cpus;

    size = (nr_cpus + cpumap_size * 8 - 1)/ (cpumap_size * 8);
    cpumap = malloc(cpumap_size * size);
    if(cpumap == NULL)
        return pyxc_error_to_exception();

    if ( (cpulist != NULL) && PyList_Check(cpulist) )
    {
        for ( i = 0; i < size; i++)
        {
            cpumap[i] = 0ULL;
        }
        for ( i = 0; i < PyList_Size(cpulist); i++ ) 
        {
            long cpu = PyInt_AsLong(PyList_GetItem(cpulist, i));
            cpumap[cpu / (cpumap_size * 8)] |= (uint64_t)1 << (cpu % (cpumap_size * 8));
        }
    }
  
    if ( xc_vcpu_setaffinity(self->xc_handle, dom, vcpu, cpumap, size * cpumap_size) != 0 )
    {
        free(cpumap);
        return pyxc_error_to_exception();
    }
    Py_INCREF(zero);
    free(cpumap); 
    return zero;
}

static PyObject *pyxc_domain_sethandle(XcObject *self, PyObject *args)
{
    int i;
    uint32_t dom;
    PyObject *pyhandle;
    xen_domain_handle_t handle;

    if (!PyArg_ParseTuple(args, "iO", &dom, &pyhandle))
        return NULL;

    if ( !PyList_Check(pyhandle) || 
         (PyList_Size(pyhandle) != sizeof(xen_domain_handle_t)) )
    {
        goto out_exception;
    }

    for ( i = 0; i < sizeof(xen_domain_handle_t); i++ )
    {
        PyObject *p = PyList_GetItem(pyhandle, i);
        if ( !PyInt_Check(p) )
            goto out_exception;
        handle[i] = (uint8_t)PyInt_AsLong(p);
    }

    if (xc_domain_sethandle(self->xc_handle, dom, handle) < 0)
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;

out_exception:
    PyErr_SetFromErrno(xc_error_obj);
    return NULL;
}


static PyObject *pyxc_domain_getinfo(XcObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    PyObject *list, *info_dict, *pyhandle;

    uint32_t first_dom = 0;
    int max_doms = 1024, nr_doms, i, j;
    xc_dominfo_t *info;

    static char *kwd_list[] = { "first_dom", "max_doms", NULL };
    
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwd_list,
                                      &first_dom, &max_doms) )
        return NULL;

    info = calloc(max_doms, sizeof(xc_dominfo_t));
    if (info == NULL)
        return PyErr_NoMemory();

    nr_doms = xc_domain_getinfo(self->xc_handle, first_dom, max_doms, info);

    if (nr_doms < 0)
    {
        free(info);
        return pyxc_error_to_exception();
    }

    list = PyList_New(nr_doms);
    for ( i = 0 ; i < nr_doms; i++ )
    {
        info_dict = Py_BuildValue(
            "{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i"
            ",s:L,s:L,s:L,s:i,s:i}",
            "domid",           (int)info[i].domid,
            "online_vcpus",    info[i].nr_online_vcpus,
            "max_vcpu_id",     info[i].max_vcpu_id,
            "hvm",             info[i].hvm,
            "dying",           info[i].dying,
            "crashed",         info[i].crashed,
            "shutdown",        info[i].shutdown,
            "paused",          info[i].paused,
            "blocked",         info[i].blocked,
            "running",         info[i].running,
            "mem_kb",          (long long)info[i].nr_pages*(XC_PAGE_SIZE/1024),
            "cpu_time",        (long long)info[i].cpu_time,
            "maxmem_kb",       (long long)info[i].max_memkb,
            "ssidref",         (int)info[i].ssidref,
            "shutdown_reason", info[i].shutdown_reason);
        pyhandle = PyList_New(sizeof(xen_domain_handle_t));
        if ( (pyhandle == NULL) || (info_dict == NULL) )
        {
            Py_DECREF(list);
            if ( pyhandle  != NULL ) { Py_DECREF(pyhandle);  }
            if ( info_dict != NULL ) { Py_DECREF(info_dict); }
            free(info);
            return NULL;
        }
        for ( j = 0; j < sizeof(xen_domain_handle_t); j++ )
            PyList_SetItem(pyhandle, j, PyInt_FromLong(info[i].handle[j]));
        PyDict_SetItemString(info_dict, "handle", pyhandle);
        Py_DECREF(pyhandle);
        PyList_SetItem(list, i, info_dict);
    }

    free(info);

    return list;
}

static PyObject *pyxc_vcpu_getinfo(XcObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    PyObject *info_dict, *cpulist;

    uint32_t dom, vcpu = 0;
    xc_vcpuinfo_t info;
    int rc, i;
    uint64_t *cpumap;
    int nr_cpus, size;
    xc_physinfo_t pinfo = { 0 };
    uint64_t cpumap_size = sizeof(*cpumap);

    static char *kwd_list[] = { "domid", "vcpu", NULL };
    
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list,
                                      &dom, &vcpu) )
        return NULL;

    if ( xc_physinfo(self->xc_handle, &pinfo) != 0 ) 
        return pyxc_error_to_exception();
    nr_cpus = pinfo.nr_cpus;

    rc = xc_vcpu_getinfo(self->xc_handle, dom, vcpu, &info);
    if ( rc < 0 )
        return pyxc_error_to_exception();

    size = (nr_cpus + cpumap_size * 8 - 1)/ (cpumap_size * 8); 
    if((cpumap = malloc(cpumap_size * size)) == NULL)
        return pyxc_error_to_exception(); 
    memset(cpumap, 0, cpumap_size * size);

    rc = xc_vcpu_getaffinity(self->xc_handle, dom, vcpu, cpumap, cpumap_size * size);
    if ( rc < 0 )
    {
        free(cpumap);
        return pyxc_error_to_exception();
    }

    info_dict = Py_BuildValue("{s:i,s:i,s:i,s:L,s:i}",
                              "online",   info.online,
                              "blocked",  info.blocked,
                              "running",  info.running,
                              "cpu_time", info.cpu_time,
                              "cpu",      info.cpu);
    cpulist = PyList_New(0);
    for ( i = 0; i < nr_cpus; i++ )
    {
        if (*(cpumap + i / (cpumap_size * 8)) & 1 ) {
            PyObject *pyint = PyInt_FromLong(i);
            PyList_Append(cpulist, pyint);
            Py_DECREF(pyint);
        }
        cpumap[i / (cpumap_size * 8)] >>= 1;
    }
    PyDict_SetItemString(info_dict, "cpumap", cpulist);
    Py_DECREF(cpulist);
    free(cpumap);
    return info_dict;
}

static PyObject *pyxc_getBitSize(XcObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    PyObject *info_type;
    char *image = NULL, *cmdline = "", *features = NULL;
    int type = 0;
    static char *kwd_list[] = { "image", "cmdline", "features", NULL };
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "sss", kwd_list,
                                      &image, &cmdline, &features) )
        return NULL;
    xc_get_bit_size(image, cmdline, features, &type);
    if (type < 0)
        return pyxc_error_to_exception();
    info_type = Py_BuildValue("{s:i}",
                              "type", type);
    return info_type;
}

static PyObject *pyxc_linux_build(XcObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    uint32_t domid;
    struct xc_dom_image *dom;
    char *image, *ramdisk = NULL, *cmdline = "", *features = NULL;
    int flags = 0;
    int store_evtchn, console_evtchn;
    int vhpt = 0;
    int superpages = 0;
    unsigned int mem_mb;
    unsigned long store_mfn = 0;
    unsigned long console_mfn = 0;
    PyObject* elfnote_dict;
    PyObject* elfnote = NULL;
    PyObject* ret;
    int i;

    static char *kwd_list[] = { "domid", "store_evtchn", "memsize",
                                "console_evtchn", "image",
                                /* optional */
                                "ramdisk", "cmdline", "flags",
                                "features", "vhpt", "superpages", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiis|ssisii", kwd_list,
                                      &domid, &store_evtchn, &mem_mb,
                                      &console_evtchn, &image,
                                      /* optional */
                                      &ramdisk, &cmdline, &flags,
                                      &features, &vhpt, &superpages) )
        return NULL;

    xc_dom_loginit();
    if (!(dom = xc_dom_allocate(cmdline, features)))
        return pyxc_error_to_exception();

    /* for IA64 */
    dom->vhpt_size_log2 = vhpt;

    dom->superpages = superpages;

    if ( xc_dom_linux_build(self->xc_handle, dom, domid, mem_mb, image,
                            ramdisk, flags, store_evtchn, &store_mfn,
                            console_evtchn, &console_mfn) != 0 ) {
        goto out;
    }

    if ( !(elfnote_dict = PyDict_New()) )
        goto out;
    
    for ( i = 0; i < ARRAY_SIZE(dom->parms.elf_notes); i++ )
    {
        switch ( dom->parms.elf_notes[i].type )
        {
        case XEN_ENT_NONE:
            continue;
        case XEN_ENT_LONG:
            elfnote = Py_BuildValue("k", dom->parms.elf_notes[i].data.num);
            break;
        case XEN_ENT_STR:
            elfnote = Py_BuildValue("s", dom->parms.elf_notes[i].data.str);
            break;
        }
        PyDict_SetItemString(elfnote_dict,
                             dom->parms.elf_notes[i].name,
                             elfnote);
        Py_DECREF(elfnote);
    }

    ret = Py_BuildValue("{s:i,s:i,s:N}",
                        "store_mfn", store_mfn,
                        "console_mfn", console_mfn,
                        "notes", elfnote_dict);

    if ( dom->arch_hooks->native_protocol )
    {
        PyObject *native_protocol =
            Py_BuildValue("s", dom->arch_hooks->native_protocol);
        PyDict_SetItemString(ret, "native_protocol", native_protocol);
        Py_DECREF(native_protocol);
    }

    xc_dom_release(dom);

    return ret;

  out:
    xc_dom_release(dom);
    return pyxc_error_to_exception();
}

static PyObject *pyxc_get_hvm_param(XcObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    uint32_t dom;
    int param;
    unsigned long value;

    static char *kwd_list[] = { "domid", "param", NULL }; 
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "ii", kwd_list,
                                      &dom, &param) )
        return NULL;

    if ( xc_get_hvm_param(self->xc_handle, dom, param, &value) != 0 )
        return pyxc_error_to_exception();

    return PyLong_FromUnsignedLong(value);

}

static PyObject *pyxc_set_hvm_param(XcObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    uint32_t dom;
    int param;
    uint64_t value;

    static char *kwd_list[] = { "domid", "param", "value", NULL }; 
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiL", kwd_list,
                                      &dom, &param, &value) )
        return NULL;

    if ( xc_set_hvm_param(self->xc_handle, dom, param, value) != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static int token_value(char *token)
{
    token = strchr(token, 'x') + 1;
    return strtol(token, NULL, 16);
}

static int next_bdf(char **str, int *seg, int *bus, int *dev, int *func)
{
    char *token;

    if ( !(*str) || !strchr(*str, ',') )
        return 0;

    token = *str;
    *seg  = token_value(token);
    token = strchr(token, ',') + 1;
    *bus  = token_value(token);
    token = strchr(token, ',') + 1;
    *dev  = token_value(token);
    token = strchr(token, ',') + 1;
    *func  = token_value(token);
    token = strchr(token, ',');
    *str = token ? token + 1 : NULL;

    return 1;
}

static PyObject *pyxc_test_assign_device(XcObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    uint32_t dom;
    char *pci_str;
    int32_t bdf = 0;
    int seg, bus, dev, func;

    static char *kwd_list[] = { "domid", "pci", NULL };
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is", kwd_list,
                                      &dom, &pci_str) )
        return NULL;

    while ( next_bdf(&pci_str, &seg, &bus, &dev, &func) )
    {
        bdf |= (bus & 0xff) << 16;
        bdf |= (dev & 0x1f) << 11;
        bdf |= (func & 0x7) << 8;

        if ( xc_test_assign_device(self->xc_handle, dom, bdf) != 0 )
        {
            if (errno == ENOSYS)
                bdf = -1;
            break;
        }
        bdf = 0;
    }

    return Py_BuildValue("i", bdf);
}

static PyObject *pyxc_assign_device(XcObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    uint32_t dom;
    char *pci_str;
    int32_t bdf = 0;
    int seg, bus, dev, func;

    static char *kwd_list[] = { "domid", "pci", NULL };
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is", kwd_list,
                                      &dom, &pci_str) )
        return NULL;

    while ( next_bdf(&pci_str, &seg, &bus, &dev, &func) )
    {
        bdf |= (bus & 0xff) << 16;
        bdf |= (dev & 0x1f) << 11;
        bdf |= (func & 0x7) << 8;

        if ( xc_assign_device(self->xc_handle, dom, bdf) != 0 )
        {
            if (errno == ENOSYS)
                bdf = -1;
            break;
        }
        bdf = 0;
    }

    return Py_BuildValue("i", bdf);
}

static PyObject *pyxc_deassign_device(XcObject *self,
                                      PyObject *args,
                                      PyObject *kwds)
{
    uint32_t dom;
    char *pci_str;
    int32_t bdf = 0;
    int seg, bus, dev, func;

    static char *kwd_list[] = { "domid", "pci", NULL };
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is", kwd_list,
                                      &dom, &pci_str) )
        return NULL;

    while ( next_bdf(&pci_str, &seg, &bus, &dev, &func) )
    {
        bdf |= (bus & 0xff) << 16;
        bdf |= (dev & 0x1f) << 11;
        bdf |= (func & 0x7) << 8;

        if ( xc_deassign_device(self->xc_handle, dom, bdf) != 0 )
        {
            if (errno == ENOSYS)
                bdf = -1;
            break;
        }
        bdf = 0;
    }

    return Py_BuildValue("i", bdf);
}

static PyObject *pyxc_get_device_group(XcObject *self,
                                         PyObject *args)
{
    uint32_t bdf = 0;
    uint32_t max_sdevs, num_sdevs;
    int domid, seg, bus, dev, func, rc, i;
    PyObject *Pystr;
    char *group_str;
    char dev_str[9];
    uint32_t *sdev_array;

    if ( !PyArg_ParseTuple(args, "iiiii", &domid, &seg, &bus, &dev, &func) )
        return NULL;

    /* Maximum allowed siblings device number per group */
    max_sdevs = 1024;

    sdev_array = calloc(max_sdevs, sizeof(*sdev_array));
    if (sdev_array == NULL)
        return PyErr_NoMemory();

    bdf |= (bus & 0xff) << 16;
    bdf |= (dev & 0x1f) << 11;
    bdf |= (func & 0x7) << 8;

    rc = xc_get_device_group(self->xc_handle,
        domid, bdf, max_sdevs, &num_sdevs, sdev_array);

    if ( rc < 0 )
    {
        free(sdev_array); 
        return pyxc_error_to_exception();
    }

    if ( !num_sdevs )
    {
        free(sdev_array);
        return Py_BuildValue("s", "");
    }

    group_str = calloc(num_sdevs, sizeof(dev_str));
    if (group_str == NULL)
    {
        free(sdev_array);
        return PyErr_NoMemory();
    }

    for ( i = 0; i < num_sdevs; i++ )
    {
        bus = (sdev_array[i] >> 16) & 0xff;
        dev = (sdev_array[i] >> 11) & 0x1f;
        func = (sdev_array[i] >> 8) & 0x7;
        snprintf(dev_str, sizeof(dev_str), "%02x:%02x.%x,", bus, dev, func);
        strcat(group_str, dev_str);
    }

    Pystr = Py_BuildValue("s", group_str);

    free(sdev_array);
    free(group_str);

    return Pystr;
}

#ifdef __ia64__
static PyObject *pyxc_nvram_init(XcObject *self,
                                 PyObject *args)
{
    char *dom_name;
    uint32_t dom;

    if ( !PyArg_ParseTuple(args, "si", &dom_name, &dom) )
        return NULL;

    xc_ia64_nvram_init(self->xc_handle, dom_name, dom);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_set_os_type(XcObject *self,
                                  PyObject *args)
{
    char *os_type;
    uint32_t dom;

    if ( !PyArg_ParseTuple(args, "si", &os_type, &dom) )
        return NULL;

    xc_ia64_set_os_type(self->xc_handle, os_type, dom);

    Py_INCREF(zero);
    return zero;
}
#endif /* __ia64__ */


#if defined(__i386__) || defined(__x86_64__)
static void pyxc_dom_extract_cpuid(PyObject *config,
                                  char **regs)
{
    const char *regs_extract[4] = { "eax", "ebx", "ecx", "edx" };
    PyObject *obj;
    int i;

    memset(regs, 0, 4*sizeof(*regs));

    if ( !PyDict_Check(config) )
        return;

    for ( i = 0; i < 4; i++ )
        if ( (obj = PyDict_GetItemString(config, regs_extract[i])) != NULL )
            regs[i] = PyString_AS_STRING(obj);
}

static PyObject *pyxc_create_cpuid_dict(char **regs)
{
   const char *regs_extract[4] = { "eax", "ebx", "ecx", "edx" };
   PyObject *dict;
   int i;

   dict = PyDict_New();
   for ( i = 0; i < 4; i++ )
   {
       if ( regs[i] == NULL )
           continue;
       PyDict_SetItemString(dict, regs_extract[i],
                            PyString_FromString(regs[i]));
       free(regs[i]);
       regs[i] = NULL;
   }
   return dict;
}

static PyObject *pyxc_dom_check_cpuid(XcObject *self,
                                      PyObject *args)
{
    PyObject *sub_input, *config;
    unsigned int input[2];
    char *regs[4], *regs_transform[4];

    if ( !PyArg_ParseTuple(args, "iOO", &input[0], &sub_input, &config) )
        return NULL;

    pyxc_dom_extract_cpuid(config, regs);

    input[1] = XEN_CPUID_INPUT_UNUSED;
    if ( PyLong_Check(sub_input) )
        input[1] = PyLong_AsUnsignedLong(sub_input);

    if ( xc_cpuid_check(self->xc_handle, input,
                        (const char **)regs, regs_transform) )
        return pyxc_error_to_exception();

    return pyxc_create_cpuid_dict(regs_transform);
}

static PyObject *pyxc_dom_set_policy_cpuid(XcObject *self,
                                           PyObject *args)
{
    int domid;

    if ( !PyArg_ParseTuple(args, "i", &domid) )
        return NULL;

    if ( xc_cpuid_apply_policy(self->xc_handle, domid) )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}


static PyObject *pyxc_dom_set_cpuid(XcObject *self,
                                    PyObject *args)
{
    PyObject *sub_input, *config;
    unsigned int domid, input[2];
    char *regs[4], *regs_transform[4];

    if ( !PyArg_ParseTuple(args, "IIOO", &domid,
                           &input[0], &sub_input, &config) )
        return NULL;

    pyxc_dom_extract_cpuid(config, regs);

    input[1] = XEN_CPUID_INPUT_UNUSED;
    if ( PyLong_Check(sub_input) )
        input[1] = PyLong_AsUnsignedLong(sub_input);

    if ( xc_cpuid_set(self->xc_handle, domid, input, (const char **)regs,
                      regs_transform) )
        return pyxc_error_to_exception();

    return pyxc_create_cpuid_dict(regs_transform);
}

static PyObject *pyxc_dom_set_machine_address_size(XcObject *self,
						   PyObject *args,
						   PyObject *kwds)
{
    uint32_t dom, width;

    if (!PyArg_ParseTuple(args, "ii", &dom, &width))
	return NULL;

    if (xc_domain_set_machine_address_size(self->xc_handle, dom, width) != 0)
	return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_dom_suppress_spurious_page_faults(XcObject *self,
						      PyObject *args,
						      PyObject *kwds)
{
    uint32_t dom;

    if (!PyArg_ParseTuple(args, "i", &dom))
	return NULL;

    if (xc_domain_suppress_spurious_page_faults(self->xc_handle, dom) != 0)
	return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}
#endif /* __i386__ || __x86_64__ */

static PyObject *pyxc_hvm_build(XcObject *self,
                                PyObject *args,
                                PyObject *kwds)
{
    uint32_t dom;
#if !defined(__ia64__)
    struct hvm_info_table *va_hvm;
    uint8_t *va_map, sum;
#endif
    int i;
    char *image;
    int memsize, target=-1, vcpus = 1, acpi = 0, apic = 1;
    PyObject *vcpu_avail_handle = NULL;
    uint8_t vcpu_avail[(HVM_MAX_VCPUS + 7)/8];

    static char *kwd_list[] = { "domid",
                                "memsize", "image", "target", "vcpus", 
                                "vcpu_avail", "acpi", "apic", NULL };
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iis|iiOii", kwd_list,
                                      &dom, &memsize, &image, &target, &vcpus,
                                      &vcpu_avail_handle, &acpi, &apic) )
        return NULL;

    memset(vcpu_avail, 0, sizeof(vcpu_avail));
    vcpu_avail[0] = 1;
    if ( vcpu_avail_handle != NULL )
    {
        if ( PyInt_Check(vcpu_avail_handle) )
        {
            unsigned long v = PyInt_AsLong(vcpu_avail_handle);
            for ( i = 0; i < sizeof(long); i++ )
                vcpu_avail[i] = (uint8_t)(v>>(i*8));
        }
        else if ( PyLong_Check(vcpu_avail_handle) )
        {
            if ( _PyLong_AsByteArray((PyLongObject *)vcpu_avail_handle,
                                     (unsigned char *)vcpu_avail,
                                     sizeof(vcpu_avail), 1, 0) )
                return NULL;
        }
        else
        {
            errno = EINVAL;
            PyErr_SetFromErrno(xc_error_obj);
            return NULL;
        }
    }

    if ( target == -1 )
        target = memsize;

    if ( xc_hvm_build_target_mem(self->xc_handle, dom, memsize,
                                 target, image) != 0 )
        return pyxc_error_to_exception();

#if !defined(__ia64__)
    /* Fix up the HVM info table. */
    va_map = xc_map_foreign_range(self->xc_handle, dom, XC_PAGE_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  HVM_INFO_PFN);
    if ( va_map == NULL )
        return PyErr_SetFromErrno(xc_error_obj);
    va_hvm = (struct hvm_info_table *)(va_map + HVM_INFO_OFFSET);
    va_hvm->acpi_enabled = acpi;
    va_hvm->apic_mode    = apic;
    va_hvm->nr_vcpus     = vcpus;
    memcpy(va_hvm->vcpu_online, vcpu_avail, sizeof(vcpu_avail));
    for ( i = 0, sum = 0; i < va_hvm->length; i++ )
        sum += ((uint8_t *)va_hvm)[i];
    va_hvm->checksum -= sum;
    munmap(va_map, XC_PAGE_SIZE);
#endif

    return Py_BuildValue("{}");
}

static PyObject *pyxc_evtchn_alloc_unbound(XcObject *self,
                                           PyObject *args,
                                           PyObject *kwds)
{
    uint32_t dom, remote_dom;
    int port;

    static char *kwd_list[] = { "domid", "remote_dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "ii", kwd_list,
                                      &dom, &remote_dom) )
        return NULL;

    if ( (port = xc_evtchn_alloc_unbound(self->xc_handle, dom, remote_dom)) < 0 )
        return pyxc_error_to_exception();

    return PyInt_FromLong(port);
}

static PyObject *pyxc_evtchn_reset(XcObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    uint32_t dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;

    if ( xc_evtchn_reset(self->xc_handle, dom) < 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_physdev_map_pirq(PyObject *self,
                                       PyObject *args,
                                       PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    uint32_t dom;
    int index, pirq, ret;

    static char *kwd_list[] = {"domid", "index", "pirq", NULL};

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iii", kwd_list,
                                      &dom, &index, &pirq) )
        return NULL;
    ret = xc_physdev_map_pirq(xc->xc_handle, dom, index, &pirq);
    if ( ret != 0 )
          return pyxc_error_to_exception();
    return PyLong_FromUnsignedLong(pirq);
}

static PyObject *pyxc_physdev_pci_access_modify(XcObject *self,
                                                PyObject *args,
                                                PyObject *kwds)
{
    uint32_t dom;
    int bus, dev, func, enable, ret;

    static char *kwd_list[] = { "domid", "bus", "dev", "func", "enable", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiii", kwd_list, 
                                      &dom, &bus, &dev, &func, &enable) )
        return NULL;

    ret = xc_physdev_pci_access_modify(
        self->xc_handle, dom, bus, dev, func, enable);
    if ( ret != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_readconsolering(XcObject *self,
                                      PyObject *args,
                                      PyObject *kwds)
{
    unsigned int clear = 0, index = 0, incremental = 0;
    unsigned int count = 16384 + 1, size = count;
    char        *str = malloc(size), *ptr;
    PyObject    *obj;
    int          ret;

    static char *kwd_list[] = { "clear", "index", "incremental", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|iii", kwd_list,
                                      &clear, &index, &incremental) ||
         !str )
        return NULL;

    ret = xc_readconsolering(self->xc_handle, &str, &count, clear,
                             incremental, &index);
    if ( ret < 0 )
        return pyxc_error_to_exception();

    while ( !incremental && count == size )
    {
        size += count - 1;
        if ( size < count )
            break;

        ptr = realloc(str, size);
        if ( !ptr )
            break;

        str = ptr + count;
        count = size - count;
        ret = xc_readconsolering(self->xc_handle, &str, &count, clear,
                                 1, &index);
        if ( ret < 0 )
            break;

        count += str - ptr;
        str = ptr;
    }

    obj = PyString_FromStringAndSize(str, count);
    free(str);
    return obj;
}


static unsigned long pages_to_kib(unsigned long pages)
{
    return pages * (XC_PAGE_SIZE / 1024);
}


static PyObject *pyxc_pages_to_kib(XcObject *self, PyObject *args)
{
    unsigned long pages;

    if (!PyArg_ParseTuple(args, "l", &pages))
        return NULL;

    return PyLong_FromUnsignedLong(pages_to_kib(pages));
}


static PyObject *pyxc_physinfo(XcObject *self)
{
#define MAX_CPU_ID 255
    xc_physinfo_t info;
    char cpu_cap[128], virt_caps[128], *p;
    int i, j, max_cpu_id, nr_nodes = 0;
    uint64_t free_heap;
    PyObject *ret_obj, *node_to_cpu_obj, *node_to_memory_obj;
    PyObject *node_to_dma32_mem_obj;
    xc_cpu_to_node_t map[MAX_CPU_ID + 1];
    const char *virtcap_names[] = { "hvm", "hvm_directio" };

    set_xen_guest_handle(info.cpu_to_node, map);
    info.max_cpu_id = MAX_CPU_ID;

    if ( xc_physinfo(self->xc_handle, &info) != 0 )
        return pyxc_error_to_exception();

    p = cpu_cap;
    *p = '\0';
    for ( i = 0; i < sizeof(info.hw_cap)/4; i++ )
        p += sprintf(p, "%08x:", info.hw_cap[i]);
    *(p-1) = 0;

    p = virt_caps;
    *p = '\0';
    for ( i = 0; i < 2; i++ )
        if ( (info.capabilities >> i) & 1 )
          p += sprintf(p, "%s ", virtcap_names[i]);
    if ( p != virt_caps )
      *(p-1) = '\0';

    max_cpu_id = info.max_cpu_id;
    if ( max_cpu_id > MAX_CPU_ID )
        max_cpu_id = MAX_CPU_ID;

    /* Construct node-to-* lists. */
    node_to_cpu_obj = PyList_New(0);
    node_to_memory_obj = PyList_New(0);
    node_to_dma32_mem_obj = PyList_New(0);
    for ( i = 0; i <= info.max_node_id; i++ )
    {
        int node_exists = 0;
        PyObject *pyint;

        /* CPUs. */
        PyObject *cpus = PyList_New(0);
        for ( j = 0; j <= max_cpu_id; j++ )
        {
            if ( i != map[j] )
                continue;
            pyint = PyInt_FromLong(j);
            PyList_Append(cpus, pyint);
            Py_DECREF(pyint);
            node_exists = 1;
        }
        PyList_Append(node_to_cpu_obj, cpus); 
        Py_DECREF(cpus);

        /* Memory. */
        xc_availheap(self->xc_handle, 0, 0, i, &free_heap);
        node_exists = node_exists || (free_heap != 0);
        pyint = PyInt_FromLong(free_heap / 1024);
        PyList_Append(node_to_memory_obj, pyint);
        Py_DECREF(pyint);

        /* DMA memory. */
        xc_availheap(self->xc_handle, 0, 32, i, &free_heap);
        pyint = PyInt_FromLong(free_heap / 1024);
        PyList_Append(node_to_dma32_mem_obj, pyint);
        Py_DECREF(pyint);

        if ( node_exists )
            nr_nodes++;
    }

    ret_obj = Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i,s:l,s:l,s:l,s:i,s:s:s:s}",
                            "nr_nodes",         nr_nodes,
                            "max_node_id",      info.max_node_id,
                            "max_cpu_id",       info.max_cpu_id,
                            "threads_per_core", info.threads_per_core,
                            "cores_per_socket", info.cores_per_socket,
                            "nr_cpus",          info.nr_cpus, 
                            "total_memory",     pages_to_kib(info.total_pages),
                            "free_memory",      pages_to_kib(info.free_pages),
                            "scrub_memory",     pages_to_kib(info.scrub_pages),
                            "cpu_khz",          info.cpu_khz,
                            "hw_caps",          cpu_cap,
                            "virt_caps",        virt_caps);
    PyDict_SetItemString(ret_obj, "node_to_cpu", node_to_cpu_obj);
    Py_DECREF(node_to_cpu_obj);
    PyDict_SetItemString(ret_obj, "node_to_memory", node_to_memory_obj);
    Py_DECREF(node_to_memory_obj);
    PyDict_SetItemString(ret_obj, "node_to_dma32_mem", node_to_dma32_mem_obj);
    Py_DECREF(node_to_dma32_mem_obj);
 
    return ret_obj;
#undef MAX_CPU_ID
}

static PyObject *pyxc_xeninfo(XcObject *self)
{
    xen_extraversion_t xen_extra;
    xen_compile_info_t xen_cc;
    xen_changeset_info_t xen_chgset;
    xen_capabilities_info_t xen_caps;
    xen_platform_parameters_t p_parms;
    xen_commandline_t xen_commandline;
    long xen_version;
    long xen_pagesize;
    char str[128];

    xen_version = xc_version(self->xc_handle, XENVER_version, NULL);

    if ( xc_version(self->xc_handle, XENVER_extraversion, &xen_extra) != 0 )
        return pyxc_error_to_exception();

    if ( xc_version(self->xc_handle, XENVER_compile_info, &xen_cc) != 0 )
        return pyxc_error_to_exception();

    if ( xc_version(self->xc_handle, XENVER_changeset, &xen_chgset) != 0 )
        return pyxc_error_to_exception();

    if ( xc_version(self->xc_handle, XENVER_capabilities, &xen_caps) != 0 )
        return pyxc_error_to_exception();

    if ( xc_version(self->xc_handle, XENVER_platform_parameters, &p_parms) != 0 )
        return pyxc_error_to_exception();

    if ( xc_version(self->xc_handle, XENVER_commandline, &xen_commandline) != 0 )
        return pyxc_error_to_exception();

    snprintf(str, sizeof(str), "virt_start=0x%lx", p_parms.virt_start);

    xen_pagesize = xc_version(self->xc_handle, XENVER_pagesize, NULL);
    if (xen_pagesize < 0 )
        return pyxc_error_to_exception();

    return Py_BuildValue("{s:i,s:i,s:s,s:s,s:i,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
                         "xen_major", xen_version >> 16,
                         "xen_minor", (xen_version & 0xffff),
                         "xen_extra", xen_extra,
                         "xen_caps",  xen_caps,
                         "xen_pagesize", xen_pagesize,
                         "platform_params", str,
                         "xen_changeset", xen_chgset,
                         "xen_commandline", xen_commandline,
                         "cc_compiler", xen_cc.compiler,
                         "cc_compile_by", xen_cc.compile_by,
                         "cc_compile_domain", xen_cc.compile_domain,
                         "cc_compile_date", xen_cc.compile_date);
}


static PyObject *pyxc_sedf_domain_set(XcObject *self,
                                      PyObject *args,
                                      PyObject *kwds)
{
    uint32_t domid;
    uint64_t period, slice, latency;
    uint16_t extratime, weight;
    static char *kwd_list[] = { "domid", "period", "slice",
                                "latency", "extratime", "weight",NULL };
    
    if( !PyArg_ParseTupleAndKeywords(args, kwds, "iLLLhh", kwd_list, 
                                     &domid, &period, &slice,
                                     &latency, &extratime, &weight) )
        return NULL;
   if ( xc_sedf_domain_set(self->xc_handle, domid, period,
                           slice, latency, extratime,weight) != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_sedf_domain_get(XcObject *self, PyObject *args)
{
    uint32_t domid;
    uint64_t period, slice,latency;
    uint16_t weight, extratime;
    
    if(!PyArg_ParseTuple(args, "i", &domid))
        return NULL;
    
    if (xc_sedf_domain_get(self->xc_handle, domid, &period,
                           &slice,&latency,&extratime,&weight))
        return pyxc_error_to_exception();

    return Py_BuildValue("{s:i,s:L,s:L,s:L,s:i,s:i}",
                         "domid",    domid,
                         "period",    period,
                         "slice",     slice,
                         "latency",   latency,
                         "extratime", extratime,
                         "weight",    weight);
}

static PyObject *pyxc_shadow_control(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    uint32_t dom;
    int op=0;

    static char *kwd_list[] = { "dom", "op", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
                                      &dom, &op) )
        return NULL;
    
    if ( xc_shadow_control(xc->xc_handle, dom, op, NULL, 0, NULL, 0, NULL) 
         < 0 )
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_shadow_mem_control(PyObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    int op;
    uint32_t dom;
    int mbarg = -1;
    unsigned long mb;

    static char *kwd_list[] = { "dom", "mb", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
                                      &dom, &mbarg) )
        return NULL;
    
    if ( mbarg < 0 ) 
        op = XEN_DOMCTL_SHADOW_OP_GET_ALLOCATION;
    else 
    {
        mb = mbarg;
        op = XEN_DOMCTL_SHADOW_OP_SET_ALLOCATION;
    }
    if ( xc_shadow_control(xc->xc_handle, dom, op, NULL, 0, &mb, 0, NULL) < 0 )
        return pyxc_error_to_exception();
    
    mbarg = mb;
    return Py_BuildValue("i", mbarg);
}

static PyObject *pyxc_sched_id_get(XcObject *self) {
    
    int sched_id;
    if (xc_sched_id(self->xc_handle, &sched_id) != 0)
        return PyErr_SetFromErrno(xc_error_obj);

    return Py_BuildValue("i", sched_id);
}

static PyObject *pyxc_sched_credit_domain_set(XcObject *self,
                                              PyObject *args,
                                              PyObject *kwds)
{
    uint32_t domid;
    uint16_t weight;
    uint16_t cap;
    static char *kwd_list[] = { "domid", "weight", "cap", NULL };
    static char kwd_type[] = "I|HH";
    struct xen_domctl_sched_credit sdom;
    
    weight = 0;
    cap = (uint16_t)~0U;
    if( !PyArg_ParseTupleAndKeywords(args, kwds, kwd_type, kwd_list, 
                                     &domid, &weight, &cap) )
        return NULL;

    sdom.weight = weight;
    sdom.cap = cap;

    if ( xc_sched_credit_domain_set(self->xc_handle, domid, &sdom) != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_sched_credit_domain_get(XcObject *self, PyObject *args)
{
    uint32_t domid;
    struct xen_domctl_sched_credit sdom;
    
    if( !PyArg_ParseTuple(args, "I", &domid) )
        return NULL;
    
    if ( xc_sched_credit_domain_get(self->xc_handle, domid, &sdom) != 0 )
        return pyxc_error_to_exception();

    return Py_BuildValue("{s:H,s:H}",
                         "weight",  sdom.weight,
                         "cap",     sdom.cap);
}

static PyObject *pyxc_domain_setmaxmem(XcObject *self, PyObject *args)
{
    uint32_t dom;
    unsigned int maxmem_kb;

    if (!PyArg_ParseTuple(args, "ii", &dom, &maxmem_kb))
        return NULL;

    if (xc_domain_setmaxmem(self->xc_handle, dom, maxmem_kb) != 0)
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_set_target_mem(XcObject *self, PyObject *args)
{
    uint32_t dom;
    unsigned int mem_kb, mem_pages;

    if (!PyArg_ParseTuple(args, "ii", &dom, &mem_kb))
        return NULL;

    mem_pages = mem_kb / 4; 

    if (xc_domain_memory_set_pod_target(self->xc_handle, dom, mem_pages,
                                        NULL, NULL, NULL) != 0)
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_set_memmap_limit(XcObject *self, PyObject *args)
{
    uint32_t dom;
    unsigned int maplimit_kb;

    if ( !PyArg_ParseTuple(args, "ii", &dom, &maplimit_kb) )
        return NULL;

    if ( xc_domain_set_memmap_limit(self->xc_handle, dom, maplimit_kb) != 0 )
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_ioport_permission(XcObject *self,
                                               PyObject *args,
                                               PyObject *kwds)
{
    uint32_t dom;
    int first_port, nr_ports, allow_access, ret;

    static char *kwd_list[] = { "domid", "first_port", "nr_ports", "allow_access", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiii", kwd_list, 
                                      &dom, &first_port, &nr_ports, &allow_access) )
        return NULL;

    ret = xc_domain_ioport_permission(
        self->xc_handle, dom, first_port, nr_ports, allow_access);
    if ( ret != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_irq_permission(PyObject *self,
                                            PyObject *args,
                                            PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    uint32_t dom;
    int pirq, allow_access, ret;

    static char *kwd_list[] = { "domid", "pirq", "allow_access", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iii", kwd_list, 
                                      &dom, &pirq, &allow_access) )
        return NULL;

    ret = xc_domain_irq_permission(
        xc->xc_handle, dom, pirq, allow_access);
    if ( ret != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_iomem_permission(PyObject *self,
                                               PyObject *args,
                                               PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    uint32_t dom;
    unsigned long first_pfn, nr_pfns, allow_access, ret;

    static char *kwd_list[] = { "domid", "first_pfn", "nr_pfns", "allow_access", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "illi", kwd_list, 
                                      &dom, &first_pfn, &nr_pfns, &allow_access) )
        return NULL;

    ret = xc_domain_iomem_permission(
        xc->xc_handle, dom, first_pfn, nr_pfns, allow_access);
    if ( ret != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_set_time_offset(XcObject *self, PyObject *args)
{
    uint32_t dom;
    int32_t offset;

    if (!PyArg_ParseTuple(args, "ii", &dom, &offset))
        return NULL;

    if (xc_domain_set_time_offset(self->xc_handle, dom, offset) != 0)
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_set_tsc_info(XcObject *self, PyObject *args)
{
    uint32_t dom, tsc_mode;

    if (!PyArg_ParseTuple(args, "ii", &dom, &tsc_mode))
        return NULL;

    if (xc_domain_set_tsc_info(self->xc_handle, dom, tsc_mode, 0, 0, 0) != 0)
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_disable_migrate(XcObject *self, PyObject *args)
{
    uint32_t dom;

    if (!PyArg_ParseTuple(args, "i", &dom))
        return NULL;

    if (xc_domain_disable_migrate(self->xc_handle, dom) != 0)
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_send_trigger(XcObject *self,
                                          PyObject *args,
                                          PyObject *kwds)
{
    uint32_t dom;
    int trigger, vcpu = 0;

    static char *kwd_list[] = { "domid", "trigger", "vcpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "ii|i", kwd_list, 
                                      &dom, &trigger, &vcpu) )
        return NULL;

    if (xc_domain_send_trigger(self->xc_handle, dom, trigger, vcpu) != 0)
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_send_debug_keys(XcObject *self,
                                      PyObject *args,
                                      PyObject *kwds)
{
    char *keys;

    static char *kwd_list[] = { "keys", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "s", kwd_list, &keys) )
        return NULL;

    if ( xc_send_debug_keys(self->xc_handle, keys) != 0 )
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *dom_op(XcObject *self, PyObject *args,
                        int (*fn)(int, uint32_t))
{
    uint32_t dom;

    if (!PyArg_ParseTuple(args, "i", &dom))
        return NULL;

    if (fn(self->xc_handle, dom) != 0)
        return pyxc_error_to_exception();

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_tmem_control(XcObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    int32_t pool_id;
    uint32_t subop;
    uint32_t cli_id;
    uint32_t arg1;
    uint32_t arg2;
    uint64_t arg3;
    char *buf;
    char _buffer[32768], *buffer = _buffer;
    int rc;

    static char *kwd_list[] = { "pool_id", "subop", "cli_id", "arg1", "arg2", "arg3", "buf", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiiiis", kwd_list,
                        &pool_id, &subop, &cli_id, &arg1, &arg2, &arg3, &buf) )
        return NULL;

    if ( (subop == TMEMC_LIST) && (arg1 > 32768) )
        arg1 = 32768;

    if ( (rc = xc_tmem_control(self->xc_handle, pool_id, subop, cli_id, arg1, arg2, arg3, buffer)) < 0 )
        return Py_BuildValue("i", rc);

    switch (subop) {
        case TMEMC_LIST:
            return Py_BuildValue("s", buffer);
        case TMEMC_FLUSH:
            return Py_BuildValue("i", rc);
        case TMEMC_QUERY_FREEABLE_MB:
            return Py_BuildValue("i", rc);
        case TMEMC_THAW:
        case TMEMC_FREEZE:
        case TMEMC_DESTROY:
        case TMEMC_SET_WEIGHT:
        case TMEMC_SET_CAP:
        case TMEMC_SET_COMPRESS:
        default:
            break;
    }

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_tmem_shared_auth(XcObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    uint32_t cli_id;
    uint32_t arg1;
    char *uuid_str;
    int rc;

    static char *kwd_list[] = { "cli_id", "uuid_str", "arg1", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "isi", kwd_list,
                                   &cli_id, &uuid_str, &arg1) )
        return NULL;

    if ( (rc = xc_tmem_auth(self->xc_handle, cli_id, uuid_str, arg1)) < 0 )
        return Py_BuildValue("i", rc);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_dom_set_memshr(XcObject *self, PyObject *args)
{
    uint32_t dom;
    int enable;

    if (!PyArg_ParseTuple(args, "ii", &dom, &enable))
        return NULL;

    if (xc_memshr_control(self->xc_handle, dom, enable) != 0)
        return pyxc_error_to_exception();
    
    Py_INCREF(zero);
    return zero;
}


static PyMethodDef pyxc_methods[] = {
    { "handle",
      (PyCFunction)pyxc_handle,
      METH_NOARGS, "\n"
      "Query the xc control interface file descriptor.\n\n"
      "Returns: [int] file descriptor\n" },

    { "domain_create", 
      (PyCFunction)pyxc_domain_create, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Create a new domain.\n"
      " dom    [int, 0]:        Domain identifier to use (allocated if zero).\n"
      "Returns: [int] new domain identifier; -1 on error.\n" },

    { "domain_max_vcpus", 
      (PyCFunction)pyxc_domain_max_vcpus,
      METH_VARARGS, "\n"
      "Set the maximum number of VCPUs a domain may create.\n"
      " dom       [int, 0]:      Domain identifier to use.\n"
      " max     [int, 0]:      New maximum number of VCPUs in domain.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_dumpcore", 
      (PyCFunction)pyxc_domain_dumpcore, 
      METH_VARARGS, "\n"
      "Dump core of a domain.\n"
      " dom [int]: Identifier of domain to dump core of.\n"
      " corefile [string]: Name of corefile to be created.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_pause", 
      (PyCFunction)pyxc_domain_pause, 
      METH_VARARGS, "\n"
      "Temporarily pause execution of a domain.\n"
      " dom [int]: Identifier of domain to be paused.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_unpause", 
      (PyCFunction)pyxc_domain_unpause, 
      METH_VARARGS, "\n"
      "(Re)start execution of a domain.\n"
      " dom [int]: Identifier of domain to be unpaused.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_destroy", 
      (PyCFunction)pyxc_domain_destroy, 
      METH_VARARGS, "\n"
      "Destroy a domain.\n"
      " dom [int]:    Identifier of domain to be destroyed.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_destroy_hook", 
      (PyCFunction)pyxc_domain_destroy_hook, 
      METH_VARARGS, "\n"
      "Add a hook for arch stuff before destroy a domain.\n"
      " dom [int]:    Identifier of domain to be destroyed.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_resume", 
      (PyCFunction)pyxc_domain_resume,
      METH_VARARGS, "\n"
      "Resume execution of a suspended domain.\n"
      " dom [int]: Identifier of domain to be resumed.\n"
      " fast [int]: Use cooperative resume.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_shutdown", 
      (PyCFunction)pyxc_domain_shutdown,
      METH_VARARGS, "\n"
      "Shutdown a domain.\n"
      " dom       [int, 0]:      Domain identifier to use.\n"
      " reason     [int, 0]:      Reason for shutdown.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vcpu_setaffinity", 
      (PyCFunction)pyxc_vcpu_setaffinity, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Pin a VCPU to a specified set CPUs.\n"
      " dom [int]:     Identifier of domain to which VCPU belongs.\n"
      " vcpu [int, 0]: VCPU being pinned.\n"
      " cpumap [list, []]: list of usable CPUs.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_sethandle", 
      (PyCFunction)pyxc_domain_sethandle,
      METH_VARARGS, "\n"
      "Set domain's opaque handle.\n"
      " dom [int]:            Identifier of domain.\n"
      " handle [list of 16 ints]: New opaque handle.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_getinfo", 
      (PyCFunction)pyxc_domain_getinfo, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get information regarding a set of domains, in increasing id order.\n"
      " first_dom [int, 0]:    First domain to retrieve info about.\n"
      " max_doms  [int, 1024]: Maximum number of domains to retrieve info"
      " about.\n\n"
      "Returns: [list of dicts] if list length is less than 'max_doms'\n"
      "         parameter then there was an error, or the end of the\n"
      "         domain-id space was reached.\n"
      " dom      [int]: Identifier of domain to which this info pertains\n"
      " cpu      [int]:  CPU to which this domain is bound\n"
      " vcpus    [int]:  Number of Virtual CPUS in this domain\n"
      " dying    [int]:  Bool - is the domain dying?\n"
      " crashed  [int]:  Bool - has the domain crashed?\n"
      " shutdown [int]:  Bool - has the domain shut itself down?\n"
      " paused   [int]:  Bool - is the domain paused by control software?\n"
      " blocked  [int]:  Bool - is the domain blocked waiting for an event?\n"
      " running  [int]:  Bool - is the domain currently running?\n"
      " mem_kb   [int]:  Memory reservation, in kilobytes\n"
      " maxmem_kb [int]: Maximum memory limit, in kilobytes\n"
      " cpu_time [long]: CPU time consumed, in nanoseconds\n"
      " shutdown_reason [int]: Numeric code from guest OS, explaining "
      "reason why it shut itself down.\n" },

    { "vcpu_getinfo", 
      (PyCFunction)pyxc_vcpu_getinfo, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get information regarding a VCPU.\n"
      " dom  [int]:    Domain to retrieve info about.\n"
      " vcpu [int, 0]: VCPU to retrieve info about.\n\n"
      "Returns: [dict]\n"
      " online   [int]:  Bool - Is this VCPU currently online?\n"
      " blocked  [int]:  Bool - Is this VCPU blocked waiting for an event?\n"
      " running  [int]:  Bool - Is this VCPU currently running on a CPU?\n"
      " cpu_time [long]: CPU time consumed, in nanoseconds\n"
      " cpumap   [int]:  Bitmap of CPUs this VCPU can run on\n"
      " cpu      [int]:  CPU that this VCPU is currently bound to\n" },

    { "linux_build", 
      (PyCFunction)pyxc_linux_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new Linux guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " ramdisk [str, n/a]: Name of ramdisk file, if any.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"
      " vcpus   [int, 1]:   Number of Virtual CPUS in domain.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    {"getBitSize",
      (PyCFunction)pyxc_getBitSize,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get the bitsize of a guest OS.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"},

    { "hvm_build", 
      (PyCFunction)pyxc_hvm_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new HVM guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " image   [str]:      Name of HVM loader image file.\n"
      " vcpus   [int, 1]:   Number of Virtual CPUS in domain.\n\n"
      " vcpu_avail [long, 1]: Which Virtual CPUS available.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "hvm_get_param", 
      (PyCFunction)pyxc_get_hvm_param, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "get a parameter of HVM guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " param   [int]:      No. of HVM param.\n"
      "Returns: [long] value of the param.\n" },

    { "hvm_set_param", 
      (PyCFunction)pyxc_set_hvm_param, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "set a parameter of HVM guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " param   [int]:      No. of HVM param.\n"
      " value   [long]:     Value of param.\n"
      "Returns: [int] 0 on success.\n" },

    { "get_device_group",
      (PyCFunction)pyxc_get_device_group,
      METH_VARARGS, "\n"
      "get sibling devices infomation.\n"
      " dom     [int]:      Domain to assign device to.\n"
      " seg     [int]:      PCI segment.\n"
      " bus     [int]:      PCI bus.\n"
      " dev     [int]:      PCI dev.\n"
      " func    [int]:      PCI func.\n"
      "Returns: [string]:   Sibling devices \n" },

     { "test_assign_device",
       (PyCFunction)pyxc_test_assign_device,
       METH_VARARGS | METH_KEYWORDS, "\n"
       "test device assignment with VT-d.\n"
       " dom     [int]:      Identifier of domain to build into.\n"
       " pci_str [str]:      PCI devices.\n"
       "Returns: [int] 0 on success, or device bdf that can't be assigned.\n" },

     { "assign_device",
       (PyCFunction)pyxc_assign_device,
       METH_VARARGS | METH_KEYWORDS, "\n"
       "Assign device to IOMMU domain.\n"
       " dom     [int]:      Domain to assign device to.\n"
       " pci_str [str]:      PCI devices.\n"
       "Returns: [int] 0 on success, or device bdf that can't be assigned.\n" },

     { "deassign_device",
       (PyCFunction)pyxc_deassign_device,
       METH_VARARGS | METH_KEYWORDS, "\n"
       "Deassign device from IOMMU domain.\n"
       " dom     [int]:      Domain to deassign device from.\n"
       " pci_str [str]:      PCI devices.\n"
       "Returns: [int] 0 on success, or device bdf that can't be deassigned.\n" },
  
    { "sched_id_get",
      (PyCFunction)pyxc_sched_id_get,
      METH_NOARGS, "\n"
      "Get the current scheduler type in use.\n"
      "Returns: [int] sched_id.\n" },    

    { "sedf_domain_set",
      (PyCFunction)pyxc_sedf_domain_set,
      METH_KEYWORDS, "\n"
      "Set the scheduling parameters for a domain when running with Atropos.\n"
      " dom       [int]:  domain to set\n"
      " period    [long]: domain's scheduling period\n"
      " slice     [long]: domain's slice per period\n"
      " latency   [long]: domain's wakeup latency hint\n"
      " extratime [int]:  domain aware of extratime?\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "sedf_domain_get",
      (PyCFunction)pyxc_sedf_domain_get,
      METH_VARARGS, "\n"
      "Get the current scheduling parameters for a domain when running with\n"
      "the Atropos scheduler."
      " dom       [int]: domain to query\n"
      "Returns:   [dict]\n"
      " domain    [int]: domain ID\n"
      " period    [long]: scheduler period\n"
      " slice     [long]: CPU reservation per period\n"
      " latency   [long]: domain's wakeup latency hint\n"
      " extratime [int]:  domain aware of extratime?\n"},
    
    { "sched_credit_domain_set",
      (PyCFunction)pyxc_sched_credit_domain_set,
      METH_KEYWORDS, "\n"
      "Set the scheduling parameters for a domain when running with the\n"
      "SMP credit scheduler.\n"
      " domid     [int]:   domain id to set\n"
      " weight    [short]: domain's scheduling weight\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "sched_credit_domain_get",
      (PyCFunction)pyxc_sched_credit_domain_get,
      METH_VARARGS, "\n"
      "Get the scheduling parameters for a domain when running with the\n"
      "SMP credit scheduler.\n"
      " domid     [int]:   domain id to get\n"
      "Returns:   [dict]\n"
      " weight    [short]: domain's scheduling weight\n"},

    { "evtchn_alloc_unbound", 
      (PyCFunction)pyxc_evtchn_alloc_unbound,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allocate an unbound port that will await a remote connection.\n"
      " dom        [int]: Domain whose port space to allocate from.\n"
      " remote_dom [int]: Remote domain to accept connections from.\n\n"
      "Returns: [int] Unbound event-channel port.\n" },

    { "evtchn_reset", 
      (PyCFunction)pyxc_evtchn_reset,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Reset all connections.\n"
      " dom [int]: Domain to reset.\n" },

    { "physdev_map_pirq",
      (PyCFunction)pyxc_physdev_map_pirq,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "map physical irq to guest pirq.\n"
      " dom     [int]:      Identifier of domain to map for.\n"
      " index   [int]:      physical irq.\n"
      " pirq    [int]:      guest pirq.\n"
      "Returns: [long] value of the param.\n" },

    { "physdev_pci_access_modify",
      (PyCFunction)pyxc_physdev_pci_access_modify,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allow a domain access to a PCI device\n"
      " dom    [int]: Identifier of domain to be allowed access.\n"
      " bus    [int]: PCI bus\n"
      " dev    [int]: PCI slot\n"
      " func   [int]: PCI function\n"
      " enable [int]: Non-zero means enable access; else disable access\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },
 
    { "readconsolering", 
      (PyCFunction)pyxc_readconsolering, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Read Xen's console ring.\n"
      " clear [int, 0]: Bool - clear the ring after reading from it?\n\n"
      "Returns: [str] string is empty on failure.\n" },

    { "physinfo",
      (PyCFunction)pyxc_physinfo,
      METH_NOARGS, "\n"
      "Get information about the physical host machine\n"
      "Returns [dict]: information about the hardware"
      "        [None]: on failure.\n" },

    { "xeninfo",
      (PyCFunction)pyxc_xeninfo,
      METH_NOARGS, "\n"
      "Get information about the Xen host\n"
      "Returns [dict]: information about Xen"
      "        [None]: on failure.\n" },

    { "shadow_control", 
      (PyCFunction)pyxc_shadow_control, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set parameter for shadow pagetable interface\n"
      " dom [int]:   Identifier of domain.\n"
      " op [int, 0]: operation\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "shadow_mem_control", 
      (PyCFunction)pyxc_shadow_mem_control, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set or read shadow pagetable memory use\n"
      " dom [int]:   Identifier of domain.\n"
      " mb [int, -1]: MB of shadow memory this domain should have.\n\n"
      "Returns: [int] MB of shadow memory in use by this domain.\n" },

    { "domain_setmaxmem", 
      (PyCFunction)pyxc_domain_setmaxmem, 
      METH_VARARGS, "\n"
      "Set a domain's memory limit\n"
      " dom [int]: Identifier of domain.\n"
      " maxmem_kb [int]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_set_target_mem", 
      (PyCFunction)pyxc_domain_set_target_mem, 
      METH_VARARGS, "\n"
      "Set a domain's memory target\n"
      " dom [int]: Identifier of domain.\n"
      " mem_kb [int]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_set_memmap_limit", 
      (PyCFunction)pyxc_domain_set_memmap_limit, 
      METH_VARARGS, "\n"
      "Set a domain's physical memory mappping limit\n"
      " dom [int]: Identifier of domain.\n"
      " map_limitkb [int]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

#ifdef __ia64__
    { "nvram_init",
      (PyCFunction)pyxc_nvram_init,
      METH_VARARGS, "\n"
      "Init nvram in IA64 platform\n"
      "Returns: [int] 0 on success; -1 on error.\n" },
    { "set_os_type",
      (PyCFunction)pyxc_set_os_type,
      METH_VARARGS, "\n"
      "Set guest OS type on IA64 platform\n"
      "Returns: [int] 0 on success; -1 on error.\n" },
#endif /* __ia64__ */
    { "domain_ioport_permission",
      (PyCFunction)pyxc_domain_ioport_permission,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allow a domain access to a range of IO ports\n"
      " dom          [int]: Identifier of domain to be allowed access.\n"
      " first_port   [int]: First IO port\n"
      " nr_ports     [int]: Number of IO ports\n"
      " allow_access [int]: Non-zero means enable access; else disable access\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_irq_permission",
      (PyCFunction)pyxc_domain_irq_permission,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allow a domain access to a physical IRQ\n"
      " dom          [int]: Identifier of domain to be allowed access.\n"
      " pirq         [int]: The Physical IRQ\n"
      " allow_access [int]: Non-zero means enable access; else disable access\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_iomem_permission",
      (PyCFunction)pyxc_domain_iomem_permission,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allow a domain access to a range of IO memory pages\n"
      " dom          [int]: Identifier of domain to be allowed access.\n"
      " first_pfn   [long]: First page of I/O Memory\n"
      " nr_pfns     [long]: Number of pages of I/O Memory (>0)\n"
      " allow_access [int]: Non-zero means enable access; else disable access\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "pages_to_kib",
      (PyCFunction)pyxc_pages_to_kib,
      METH_VARARGS, "\n"
      "Returns: [int]: The size in KiB of memory spanning the given number "
      "of pages.\n" },

    { "domain_set_time_offset",
      (PyCFunction)pyxc_domain_set_time_offset,
      METH_VARARGS, "\n"
      "Set a domain's time offset to Dom0's localtime\n"
      " dom        [int]: Domain whose time offset is being set.\n"
      " offset     [int]: Time offset from UTC in seconds.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_set_tsc_info",
      (PyCFunction)pyxc_domain_set_tsc_info,
      METH_VARARGS, "\n"
      "Set a domain's TSC mode\n"
      " dom        [int]: Domain whose TSC mode is being set.\n"
      " tsc_mode   [int]: 0=default (monotonic, but native where possible)\n"
      "                   1=always emulate 2=never emulate 3=pvrdtscp\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_disable_migrate",
      (PyCFunction)pyxc_domain_disable_migrate,
      METH_VARARGS, "\n"
      "Marks domain as non-migratable AND non-restoreable\n"
      " dom        [int]: Domain whose TSC mode is being set.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_send_trigger",
      (PyCFunction)pyxc_domain_send_trigger,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Send trigger to a domain.\n"
      " dom     [int]: Identifier of domain to be sent trigger.\n"
      " trigger [int]: Trigger type number.\n"
      " vcpu    [int]: VCPU to be sent trigger.\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "send_debug_keys",
      (PyCFunction)pyxc_send_debug_keys,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Inject debug keys into Xen.\n"
      " keys    [str]: String of keys to inject.\n" },

#if defined(__i386__) || defined(__x86_64__)
    { "domain_check_cpuid", 
      (PyCFunction)pyxc_dom_check_cpuid, 
      METH_VARARGS, "\n"
      "Apply checks to host CPUID.\n"
      " input [long]: Input for cpuid instruction (eax)\n"
      " sub_input [long]: Second input (optional, may be None) for cpuid "
      "                     instruction (ecx)\n"
      " config [dict]: Dictionary of register\n"
      " config [dict]: Dictionary of register, use for checking\n\n"
      "Returns: [int] 0 on success; exception on error.\n" },
    
    { "domain_set_cpuid", 
      (PyCFunction)pyxc_dom_set_cpuid, 
      METH_VARARGS, "\n"
      "Set cpuid response for an input and a domain.\n"
      " dom [int]: Identifier of domain.\n"
      " input [long]: Input for cpuid instruction (eax)\n"
      " sub_input [long]: Second input (optional, may be None) for cpuid "
      "                     instruction (ecx)\n"
      " config [dict]: Dictionary of register\n\n"
      "Returns: [int] 0 on success; exception on error.\n" },

    { "domain_set_policy_cpuid", 
      (PyCFunction)pyxc_dom_set_policy_cpuid, 
      METH_VARARGS, "\n"
      "Set the default cpuid policy for a domain.\n"
      " dom [int]: Identifier of domain.\n\n"
      "Returns: [int] 0 on success; exception on error.\n" },

    { "domain_set_machine_address_size",
      (PyCFunction)pyxc_dom_set_machine_address_size,
      METH_VARARGS, "\n"
      "Set maximum machine address size for this domain.\n"
      " dom [int]: Identifier of domain.\n"
      " width [int]: Maximum machine address width.\n" },

    { "domain_suppress_spurious_page_faults",
      (PyCFunction)pyxc_dom_suppress_spurious_page_faults,
      METH_VARARGS, "\n"
      "Do not propagate spurious page faults to this guest.\n"
      " dom [int]: Identifier of domain.\n" },
#endif

    { "tmem_control",
      (PyCFunction)pyxc_tmem_control,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Do various control on a tmem pool.\n"
      " pool_id [int]: Identifier of the tmem pool (-1 == all).\n"
      " subop [int]: Supplementary Operation.\n"
      " cli_id [int]: Client identifier (-1 == all).\n"
      " arg1 [int]: Argument.\n"
      " arg2 [int]: Argument.\n"
      " buf [str]: Buffer.\n\n"
      "Returns: [int] 0 or [str] tmem info on success; exception on error.\n" },

    { "tmem_shared_auth",
      (PyCFunction)pyxc_tmem_shared_auth,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "De/authenticate a shared tmem pool.\n"
      " cli_id [int]: Client identifier (-1 == all).\n"
      " uuid_str [str]: uuid.\n"
      " auth [int]: 0|1 .\n"
      "Returns: [int] 0 on success; exception on error.\n" },

    { "dom_set_memshr", 
      (PyCFunction)pyxc_dom_set_memshr,
      METH_VARARGS, "\n"
      "Enable/disable memory sharing for the domain.\n"
      " dom     [int]:        Domain identifier.\n"
      " enable  [int,0|1]:    Disable or enable?\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { NULL, NULL, 0, NULL }
};


static PyObject *PyXc_getattr(PyObject *obj, char *name)
{
    return Py_FindMethod(pyxc_methods, obj, name);
}

static PyObject *PyXc_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    XcObject *self = (XcObject *)type->tp_alloc(type, 0);

    if (self == NULL)
        return NULL;

    self->xc_handle = -1;

    return (PyObject *)self;
}

static int
PyXc_init(XcObject *self, PyObject *args, PyObject *kwds)
{
    if ((self->xc_handle = xc_interface_open()) == -1) {
        pyxc_error_to_exception();
        return -1;
    }

    return 0;
}

static void PyXc_dealloc(XcObject *self)
{
    if (self->xc_handle != -1) {
        xc_interface_close(self->xc_handle);
        self->xc_handle = -1;
    }

    self->ob_type->tp_free((PyObject *)self);
}

static PyTypeObject PyXcType = {
    PyObject_HEAD_INIT(NULL)
    0,
    PKG "." CLS,
    sizeof(XcObject),
    0,
    (destructor)PyXc_dealloc,     /* tp_dealloc        */
    NULL,                         /* tp_print          */
    PyXc_getattr,                 /* tp_getattr        */
    NULL,                         /* tp_setattr        */
    NULL,                         /* tp_compare        */
    NULL,                         /* tp_repr           */
    NULL,                         /* tp_as_number      */
    NULL,                         /* tp_as_sequence    */
    NULL,                         /* tp_as_mapping     */
    NULL,                         /* tp_hash           */
    NULL,                         /* tp_call           */
    NULL,                         /* tp_str            */
    NULL,                         /* tp_getattro       */
    NULL,                         /* tp_setattro       */
    NULL,                         /* tp_as_buffer      */
    Py_TPFLAGS_DEFAULT,           /* tp_flags          */
    "Xen client connections",     /* tp_doc            */
    NULL,                         /* tp_traverse       */
    NULL,                         /* tp_clear          */
    NULL,                         /* tp_richcompare    */
    0,                            /* tp_weaklistoffset */
    NULL,                         /* tp_iter           */
    NULL,                         /* tp_iternext       */
    pyxc_methods,                 /* tp_methods        */
    NULL,                         /* tp_members        */
    NULL,                         /* tp_getset         */
    NULL,                         /* tp_base           */
    NULL,                         /* tp_dict           */
    NULL,                         /* tp_descr_get      */
    NULL,                         /* tp_descr_set      */
    0,                            /* tp_dictoffset     */
    (initproc)PyXc_init,          /* tp_init           */
    NULL,                         /* tp_alloc          */
    PyXc_new,                     /* tp_new            */
};

static PyMethodDef xc_methods[] = { { NULL } };

PyMODINIT_FUNC initxc(void)
{
    PyObject *m;

    if (PyType_Ready(&PyXcType) < 0)
        return;

    m = Py_InitModule(PKG, xc_methods);

    if (m == NULL)
      return;

    xc_error_obj = PyErr_NewException(PKG ".Error", PyExc_RuntimeError, NULL);
    zero = PyInt_FromLong(0);

    /* KAF: This ensures that we get debug output in a timely manner. */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    Py_INCREF(&PyXcType);
    PyModule_AddObject(m, CLS, (PyObject *)&PyXcType);

    Py_INCREF(xc_error_obj);
    PyModule_AddObject(m, "Error", xc_error_obj);

    /* Expose some libxc constants to Python */
    PyModule_AddIntConstant(m, "XEN_SCHEDULER_SEDF", XEN_SCHEDULER_SEDF);
    PyModule_AddIntConstant(m, "XEN_SCHEDULER_CREDIT", XEN_SCHEDULER_CREDIT);

}


/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 */
