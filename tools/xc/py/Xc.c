/******************************************************************************
 * Xc.c
 * 
 * Copyright (c) 2003-2004, K A Fraser (University of Cambridge)
 */

#include <Python.h>
#include <xc.h>

/* Needed for Python versions earlier than 2.3. */
#ifndef PyMODINIT_FUNC
#define PyMODINIT_FUNC DL_EXPORT(void)
#endif

static PyObject *xc_error, *zero;

typedef struct {
    PyObject_HEAD;
    int xc_handle;
} XcObject;

/*
 * Definitions for the 'xc' object type.
 */

static PyObject *pyxc_domain_create(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    unsigned int mem_kb = 65536;
    char        *name   = "(anon)";
    u64          dom;
    int          ret;

    static char *kwd_list[] = { "mem_kb", "name", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|is", kwd_list, 
                                      &mem_kb, &name) )
        return NULL;

    if ( (ret = xc_domain_create(xc->xc_handle, mem_kb, name, &dom)) < 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyLong_FromUnsignedLongLong(dom);
}

static PyObject *pyxc_domain_start(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "L", kwd_list, &dom) )
        return NULL;

    if ( xc_domain_start(xc->xc_handle, dom) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_stop(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "L", kwd_list, &dom) )
        return NULL;

    if ( xc_domain_stop(xc->xc_handle, dom) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_destroy(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom;
    int force = 0;

    static char *kwd_list[] = { "dom", "force", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "L|i", kwd_list, 
                                      &dom, &force) )
        return NULL;

    if ( xc_domain_destroy(xc->xc_handle, dom, force) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_pincpu(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom;
    int cpu = -1;

    static char *kwd_list[] = { "dom", "cpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "L|i", kwd_list, 
                                      &dom, &cpu) )
        return NULL;

    if ( xc_domain_pincpu(xc->xc_handle, dom, cpu) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_getinfo(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *list;

    u64 first_dom = 0;
    int max_doms = 1024, nr_doms, i;
    xc_dominfo_t *info;

    static char *kwd_list[] = { "first_dom", "max_doms", NULL };
    
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|Li", kwd_list,
                                      &first_dom, &max_doms) )
        return NULL;

    if ( (info = malloc(max_doms * sizeof(xc_dominfo_t))) == NULL )
        return PyErr_NoMemory();

    nr_doms = xc_domain_getinfo(xc->xc_handle, first_dom, max_doms, info);
    
    list = PyList_New(nr_doms);
    for ( i = 0 ; i < nr_doms; i++ )
    {
        PyList_SetItem(
            list, i, 
            Py_BuildValue("{s:L,s:i,s:i,s:i,s:l,s:L,s:s}",
                          "dom",      info[i].domid,
                          "cpu",      info[i].cpu,
                          "running",  info[i].has_cpu,
                          "stopped",  info[i].stopped,
                          "mem_kb",   info[i].nr_pages*4,
                          "cpu_time", info[i].cpu_time,
                          "name",     info[i].name));
    }

    free(info);

    return list;
}

static PyObject *pyxc_linux_save(PyObject *self,
                                 PyObject *args,
                                 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64   dom;
    char *state_file;
    int   progress = 1;

    static char *kwd_list[] = { "dom", "state_file", "progress", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Ls|i", kwd_list, 
                                      &dom, &state_file, &progress) )
        return NULL;

    if ( xc_linux_save(xc->xc_handle, dom, state_file, progress) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_linux_restore(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    char        *state_file;
    int          progress = 1;
    u64          dom;

    static char *kwd_list[] = { "state_file", "progress", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "s|i", kwd_list, 
                                      &state_file, &progress) )
        return NULL;

    if ( xc_linux_restore(xc->xc_handle, state_file, progress, &dom) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyLong_FromUnsignedLongLong(dom);
}

static PyObject *pyxc_linux_build(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64   dom;
    char *image, *ramdisk = NULL, *cmdline = "";

    static char *kwd_list[] = { "dom", "image", "ramdisk", "cmdline", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Ls|ss", kwd_list, 
                                      &dom, &image, &ramdisk, &cmdline) )
        return NULL;

    if ( xc_linux_build(xc->xc_handle, dom, image, ramdisk, cmdline) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_netbsd_build(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64   dom;
    char *image, *ramdisk = NULL, *cmdline = "";

    static char *kwd_list[] = { "dom", "image", "ramdisk", "cmdline", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Ls|ss", kwd_list, 
                                      &dom, &image, &ramdisk, &cmdline) )
        return NULL;

    if ( xc_netbsd_build(xc->xc_handle, dom, image, cmdline) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_bvtsched_global_set(PyObject *self,
                                          PyObject *args,
                                          PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    unsigned long ctx_allow;

    static char *kwd_list[] = { "ctx_allow", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "l", kwd_list, &ctx_allow) )
        return NULL;

    if ( xc_bvtsched_global_set(xc->xc_handle, ctx_allow) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_bvtsched_global_get(PyObject *self,
					  PyObject *args,
					  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    
    unsigned long ctx_allow;
    
    if ( !PyArg_ParseTuple(args, "") )
        return NULL;
    
    if ( xc_bvtsched_global_get(xc->xc_handle, &ctx_allow) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    return Py_BuildValue("s:l", "ctx_allow", ctx_allow);
}

static PyObject *pyxc_bvtsched_domain_set(PyObject *self,
                                          PyObject *args,
                                          PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64           dom;
    unsigned long mcuadv, warp, warpl, warpu;

    static char *kwd_list[] = { "dom", "mcuadv", "warp", "warpl",
                                "warpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Lllll", kwd_list,
                                      &dom, &mcuadv, &warp, &warpl, &warpu) )
        return NULL;

    if ( xc_bvtsched_domain_set(xc->xc_handle, dom, mcuadv, 
                                warp, warpl, warpu) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_bvtsched_domain_get(PyObject *self,
                                          PyObject *args,
                                          PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 dom;
    unsigned long mcuadv, warp, warpl, warpu;
    
    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "L", kwd_list, &dom) )
        return NULL;
    
    if ( xc_bvtsched_domain_get(xc->xc_handle, dom, &mcuadv, &warp,
                                &warpl, &warpu) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:L,s:l,s:l,s:l,s:l}",
                         "domain", dom,
                         "mcuadv", mcuadv,
                         "warp",   warp,
                         "warpl",  warpl,
                         "warpu",  warpu);
}

static PyObject *pyxc_vif_scheduler_set(PyObject *self,
                                        PyObject *args,
                                        PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64           dom;
    unsigned int  vif;
    xc_vif_sched_params_t sched = { 0, 0 };

    static char *kwd_list[] = { "dom", "vif", "credit_bytes", 
                                "credit_usecs", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li|ll", kwd_list, 
                                      &dom, &vif, 
                                      &sched.credit_bytes, 
                                      &sched.credit_usec) )
        return NULL;

    if ( xc_vif_scheduler_set(xc->xc_handle, dom, vif, &sched) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vif_scheduler_get(PyObject *self,
                                        PyObject *args,
                                        PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64           dom;
    unsigned int  vif;
    xc_vif_sched_params_t sched;

    static char *kwd_list[] = { "dom", "vif", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li", kwd_list, 
                                      &dom, &vif) )
        return NULL;

    if ( xc_vif_scheduler_get(xc->xc_handle, dom, vif, &sched) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:l,s:l}", 
                         "credit_bytes", sched.credit_bytes,
                         "credit_usecs", sched.credit_usec);
}

static PyObject *pyxc_vif_stats_get(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64            dom;
    unsigned int   vif;
    xc_vif_stats_t stats;

    static char *kwd_list[] = { "dom", "vif", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li", kwd_list, 
                                      &dom, &vif) )
        return NULL;

    if ( xc_vif_stats_get(xc->xc_handle, dom, vif, &stats) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:L,s:L,s:L,s:L}", 
                         "tx_bytes", stats.tx_bytes,
                         "tx_packets", stats.tx_pkts,
                         "rx_bytes", stats.rx_bytes,
                         "rx_packets", stats.rx_pkts);
}

static PyObject *pyxc_vbd_create(PyObject *self,
                                 PyObject *args,
                                 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64          dom;
    unsigned int vbd;
    int          writeable;

    static char *kwd_list[] = { "dom", "vbd", "writeable", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Lii", kwd_list, 
                                      &dom, &vbd, &writeable) )
        return NULL;

    if ( xc_vbd_create(xc->xc_handle, dom, vbd, writeable) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vbd_destroy(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64          dom;
    unsigned int vbd;

    static char *kwd_list[] = { "dom", "vbd", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li", kwd_list, 
                                      &dom, &vbd) )
        return NULL;

    if ( xc_vbd_destroy(xc->xc_handle, dom, vbd) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vbd_grow(PyObject *self,
                               PyObject *args,
                               PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64            dom;
    unsigned int   vbd;
    xc_vbdextent_t extent;

    static char *kwd_list[] = { "dom", "vbd", "device", 
                                "start_sector", "nr_sectors", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "LiiLL", kwd_list, 
                                      &dom, &vbd, 
                                      &extent.real_device, 
                                      &extent.start_sector, 
                                      &extent.nr_sectors) )
        return NULL;

    if ( xc_vbd_grow(xc->xc_handle, dom, vbd, &extent) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vbd_shrink(PyObject *self,
                                 PyObject *args,
                                 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64          dom;
    unsigned int vbd;

    static char *kwd_list[] = { "dom", "vbd", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li", kwd_list, 
                                      &dom, &vbd) )
        return NULL;

    if ( xc_vbd_shrink(xc->xc_handle, dom, vbd) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_vbd_setextents(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *list, *dict, *obj;

    u64             dom;
    unsigned int    vbd;
    xc_vbdextent_t *extents = NULL;
    int             i, nr_extents;

    static char *kwd_list[] = { "dom", "vbd", "extents", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "LiO", kwd_list, 
                                      &dom, &vbd, &list) )
        return NULL;

    if ( !PyList_Check(list) )
    {
        PyErr_SetString(PyExc_TypeError, "parameter 'extents' is not a list");
        return NULL;
    }

    if ( (nr_extents = PyList_Size(list)) != 0 )
    {
        if ( (extents = malloc(nr_extents * sizeof(xc_vbdextent_t))) == NULL )
            return PyErr_NoMemory();

        for ( i = 0; i < nr_extents; i++ )
        {
            dict = PyList_GetItem(list, i);
            if ( !PyDict_Check(dict) )
            {
                PyErr_SetString(PyExc_TypeError, "extent is not a dictionary");
                goto fail;
            }

            if ( (obj = PyDict_GetItemString(dict, "device")) == NULL )
            {
                PyErr_SetString(PyExc_TypeError,
                                "'device' is not in the dictionary");
                goto fail;
            }
            if ( PyInt_Check(obj) )
            {
                extents[i].real_device = (unsigned short)PyInt_AsLong(obj);
            }
            else if ( PyLong_Check(obj) )
            {
                extents[i].real_device = (unsigned short)PyLong_AsLong(obj);
            }
            else
            {
                PyErr_SetString(PyExc_TypeError,
                                "'device' is not an int or long");
                goto fail;
            }

            if ( (obj = PyDict_GetItemString(dict, "start_sector")) == NULL )
            {
                PyErr_SetString(PyExc_TypeError,
                                "'start_sector' is not in the dictionary");
                goto fail;
            }
            if ( PyInt_Check(obj) )
            {
                extents[i].start_sector = PyInt_AsLong(obj);
            }
            else if ( PyLong_Check(obj) )
            {
                extents[i].start_sector = PyLong_AsUnsignedLongLong(obj);
            }
            else
            {
                PyErr_SetString(PyExc_TypeError,
                                "'start_sector' is not an int or long");
                goto fail;
            }

            if ( (obj = PyDict_GetItemString(dict, "nr_sectors")) == NULL )
            {
                PyErr_SetString(PyExc_TypeError,
                                "'nr_sectors' is not in the dictionary");
                goto fail;
            }
            if ( PyInt_Check(obj) )
            {
                extents[i].nr_sectors = PyInt_AsLong(obj);
            }
            else if ( PyLong_Check(obj) )
            {
                extents[i].nr_sectors = PyLong_AsUnsignedLongLong(obj);
            }
            else
            {
                PyErr_SetString(PyExc_TypeError,
                                "'nr_sectors' is not an int or long");
                goto fail;
            }
        }
    }

    if ( xc_vbd_setextents(xc->xc_handle, dom, vbd, nr_extents, extents) != 0 )
    {
        PyErr_SetFromErrno(xc_error);
        goto fail;
    }

    if ( extents != NULL )
        free(extents);
    
    Py_INCREF(zero);
    return zero;

 fail:
    if ( extents != NULL )
        free(extents);
    return NULL;
}

#define MAX_EXTENTS 1024
static PyObject *pyxc_vbd_getextents(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *list;

    u64             dom;
    unsigned int    vbd;
    xc_vbdextent_t *extents;
    int             i, nr_extents;

    static char *kwd_list[] = { "dom", "vbd", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Li", kwd_list, 
                                      &dom, &vbd) )
        return NULL;

    if ( (extents = malloc(MAX_EXTENTS * sizeof(xc_vbdextent_t))) == NULL )
        return PyErr_NoMemory();

    nr_extents = xc_vbd_getextents(xc->xc_handle, dom, vbd, MAX_EXTENTS,
                                   extents, NULL);
    
    if ( nr_extents < 0 )
    {
        free(extents);
        return PyErr_SetFromErrno(xc_error);
    }

    list = PyList_New(nr_extents);
    for ( i = 0; i < nr_extents; i++ )
    {
        PyList_SetItem(
            list, i, 
            Py_BuildValue("{s:i,s:L,s:L}",
                          "device",       extents[i].real_device,
                          "start_sector", extents[i].start_sector,
                          "nr_sectors",   extents[i].nr_sectors));
    }

    free(extents);
    
    return list;
}

static PyObject *pyxc_vbd_probe(PyObject *self,
                                PyObject *args,
                                PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *list;

    u64          dom = XC_VBDDOM_PROBE_ALL;
    unsigned int max_vbds = 1024;
    xc_vbd_t    *info;
    int          nr_vbds, i;

    static char *kwd_list[] = { "dom", "max_vbds", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|Li", kwd_list, 
                                      &dom, &max_vbds) )
        return NULL;

    if ( (info = malloc(max_vbds * sizeof(xc_vbd_t))) == NULL )
        return PyErr_NoMemory();

    if ( (nr_vbds = xc_vbd_probe(xc->xc_handle, dom, max_vbds, info)) < 0 )
    {
        free(info);
        return PyErr_SetFromErrno(xc_error);
    }

    list = PyList_New(nr_vbds);
    for ( i = 0; i < nr_vbds; i++ )
    {
        PyList_SetItem(
            list, i, 
            Py_BuildValue("{s:L,s:i,s:i,s:L}",
                          "dom",        info[i].domid,
                          "vbd",        info[i].vbdid,
                          "writeable",  !!(info[i].flags & XC_VBDF_WRITEABLE),
                          "nr_sectors", info[i].nr_sectors));
    }

    free(info);

    return list;
}

static PyObject *pyxc_evtchn_bind_interdomain(PyObject *self,
                                              PyObject *args,
                                              PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom1 = DOMID_SELF, dom2 = DOMID_SELF;
    int port1, port2;

    static char *kwd_list[] = { "dom1", "dom2", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|LL", kwd_list, 
                                      &dom1, &dom2) )
        return NULL;

    if ( xc_evtchn_bind_interdomain(xc->xc_handle, dom1, 
                                    dom2, &port1, &port2) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:i}", 
                         "port1", port1,
                         "port2", port2);
}

static PyObject *pyxc_evtchn_close(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u64 dom = DOMID_SELF;
    int port;

    static char *kwd_list[] = { "port", "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|L", kwd_list, 
                                      &port, &dom) )
        return NULL;

    if ( xc_evtchn_close(xc->xc_handle, dom, port) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_evtchn_send(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    int port;

    static char *kwd_list[] = { "port", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &port) )
        return NULL;

    if ( xc_evtchn_send(xc->xc_handle, port) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_evtchn_status(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *dict;

    u64 dom = DOMID_SELF;
    int port, ret;
    xc_evtchn_status_t status;

    static char *kwd_list[] = { "port", "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|L", kwd_list, 
                                      &port, &dom) )
        return NULL;

    ret = xc_evtchn_status(xc->xc_handle, dom, port, &status);
    if ( ret != 0 )
        return PyErr_SetFromErrno(xc_error);

    switch ( status.status )
    {
    case EVTCHNSTAT_closed:
        dict = Py_BuildValue("{s:s}", 
                             "status", "closed");
        break;
    case EVTCHNSTAT_unbound:
        dict = Py_BuildValue("{s:s}", 
                             "status", "unbound");
        break;
    case EVTCHNSTAT_interdomain:
        dict = Py_BuildValue("{s:s,s:L,s:i}", 
                             "status", "interdomain",
                             "dom", status.u.interdomain.dom,
                             "port", status.u.interdomain.port);
        break;
    case EVTCHNSTAT_pirq:
        dict = Py_BuildValue("{s:s,s:i}", 
                             "status", "pirq",
                             "irq", status.u.pirq);
        break;
    case EVTCHNSTAT_virq:
        dict = Py_BuildValue("{s:s,s:i}", 
                             "status", "virq",
                             "irq", status.u.virq);
        break;
    default:
        dict = Py_BuildValue("{}");
        break;
    }
    
    return dict;
}

static PyObject *pyxc_physdev_pci_access_modify(PyObject *self,
                                                PyObject *args,
                                                PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 dom;
    int bus, dev, func, enable, ret;

    static char *kwd_list[] = { "dom", "bus", "dev", "func", "enable", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "Liiii", kwd_list, 
                                      &dom, &bus, &dev, &func, &enable) )
        return NULL;

    ret = xc_physdev_pci_access_modify(
        xc->xc_handle, dom, bus, dev, func, enable);
    if ( ret != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_readconsolering(PyObject *self,
                                      PyObject *args,
                                      PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    unsigned int clear = 0;
    char         str[32768];
    int          ret;

    static char *kwd_list[] = { "clear", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwd_list, &clear) )
        return NULL;

    ret = xc_readconsolering(xc->xc_handle, str, sizeof(str), clear);
    if ( ret < 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyString_FromStringAndSize(str, ret);
}

static PyObject *pyxc_physinfo(PyObject *self,
			       PyObject *args,
			       PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    xc_physinfo_t info;
    
    if ( !PyArg_ParseTuple(args, "") )
        return NULL;

    if ( xc_physinfo(xc->xc_handle, &info) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:i,s:l,s:l,s:l}",
                         "ht_per_core", info.ht_per_core,
                         "cores",       info.cores,
                         "total_pages", info.total_pages,
                         "free_pages",  info.free_pages,
                         "cpu_khz",     info.cpu_khz);
}

static PyObject *pyxc_atropos_domain_set(PyObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 domid;
    u64 period, slice, latency;
    int xtratime;

    static char *kwd_list[] = { "dom", "period", "slice", "latency",
				"xtratime", NULL };
    
    if( !PyArg_ParseTupleAndKeywords(args, kwds, "LLLLi", kwd_list, &domid,
                                     &period, &slice, &latency, &xtratime) )
        return NULL;
   
    if ( xc_atropos_domain_set(xc->xc_handle, domid, period, slice,
			       latency, xtratime) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_atropos_domain_get(PyObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 domid;
    u64 period, slice, latency;
    int xtratime;
    
    static char *kwd_list[] = { "dom", NULL };

    if( !PyArg_ParseTupleAndKeywords(args, kwds, "L", kwd_list, &domid) )
        return NULL;
    
    if ( xc_atropos_domain_get( xc->xc_handle, domid, &period,
                                &slice, &latency, &xtratime ) )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:L,s:L,s:L,s:L,s:i}",
                         "domain",  domid,
                         "period",  period,
                         "slice",   slice,
                         "latency", latency,
                         "xtratime", xtratime);
}


static PyObject *pyxc_rrobin_global_set(PyObject *self,
                                        PyObject *args,
                                        PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 slice;
    
    static char *kwd_list[] = { "slice", NULL };

    if( !PyArg_ParseTupleAndKeywords(args, kwds, "L", kwd_list, &slice) )
        return NULL;
    
    if ( xc_rrobin_global_set(xc->xc_handle, slice) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_rrobin_global_get(PyObject *self,
                                        PyObject *args,
                                        PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u64 slice;

    if ( !PyArg_ParseTuple(args, "") )
        return NULL;

    if ( xc_rrobin_global_get(xc->xc_handle, &slice) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    return Py_BuildValue("s:L", "slice", slice);
}


static PyMethodDef pyxc_methods[] = {
    { "domain_create", 
      (PyCFunction)pyxc_domain_create, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Create a new domain.\n"
      " mem_kb [int, 65536]:    Memory allocation, in kilobytes.\n"
      " name   [str, '(anon)']: Informative textual name.\n\n"
      "Returns: [long] new domain identifier; -1 on error.\n" },

    { "domain_start", 
      (PyCFunction)pyxc_domain_start, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Start execution of a domain.\n"
      " dom [long]: Identifier of domain to be started.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_stop", 
      (PyCFunction)pyxc_domain_stop, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Stop execution of a domain.\n"
      " dom [long]: Identifier of domain to be stopped.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_destroy", 
      (PyCFunction)pyxc_domain_destroy, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Destroy a domain.\n"
      " dom   [long]:   Identifier of domain to be destroyed.\n"
      " force [int, 0]: Bool - force immediate destruction?\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_pincpu", 
      (PyCFunction)pyxc_domain_pincpu, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Pin a domain to a specified CPU.\n"
      " dom [long]:    Identifier of domain to be pinned.\n"
      " cpu [int, -1]: CPU to pin to, or -1 to unpin\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_getinfo", 
      (PyCFunction)pyxc_domain_getinfo, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get information regarding a set of domains, in increasing id order.\n"
      " first_dom [long, 0]:   First domain to retrieve info about.\n"
      " max_doms  [int, 1024]: Maximum number of domains to retrieve info"
      " about.\n\n"
      "Returns: [list of dicts] if list length is less than 'max_doms'\n"
      "         parameter then there was an error, or the end of the\n"
      "         domain-id space was reached.\n"
      " dom      [long]: Identifier of domain to which this info pertains\n"
      " cpu      [int]:  CPU to which this domain is bound\n"
      " running  [int]:  Bool - is the domain currently running?\n"
      " stopped  [int]:  Bool - is the domain suspended?\n"
      " mem_kb   [int]:  Memory reservation, in kilobytes\n"
      " cpu_time [long]: CPU time consumed, in nanoseconds\n"
      " name     [str]:  Identifying name\n" },

    { "linux_save", 
      (PyCFunction)pyxc_linux_save, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Save the CPU and memory state of a Linux guest OS.\n"
      " dom        [long]:   Identifier of domain to be saved.\n"
      " state_file [str]:    Name of state file. Must not currently exist.\n"
      " progress   [int, 1]: Bool - display a running progress indication?\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "linux_restore", 
      (PyCFunction)pyxc_linux_restore, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Restore the CPU and memory state of a Linux guest OS.\n"
      " state_file [str]:    Name of state file. Must not currently exist.\n"
      " progress   [int, 1]: Bool - display a running progress indication?\n\n"
      "Returns: [long] new domain identifier on success; -1 on error.\n" },

    { "linux_build", 
      (PyCFunction)pyxc_linux_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new Linux guest OS.\n"
      " dom     [long]:     Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " ramdisk [str, n/a]: Name of ramdisk file, if any.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "netbsd_build", 
      (PyCFunction)pyxc_netbsd_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new NetBSD guest OS.\n"
      " dom     [long]:     Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "bvtsched_global_set",
      (PyCFunction)pyxc_bvtsched_global_set,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set global tuning parameters for Borrowed Virtual Time scheduler.\n"
      " ctx_allow [int]: Minimal guaranteed quantum (I think!).\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "bvtsched_global_get",
      (PyCFunction)pyxc_bvtsched_global_get,
      METH_KEYWORDS, "\n"
      "Get global tuning parameters for BVT scheduler.\n"
      "Returns: [dict]:\n"
      " ctx_allow [int]: context switch allowance\n" },

    { "bvtsched_domain_set",
      (PyCFunction)pyxc_bvtsched_domain_set,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set per-domain tuning parameters for Borrowed Virtual Time scheduler.\n"
      " dom    [long]: Identifier of domain to be tuned.\n"
      " mcuadv [int]:  Internal BVT parameter.\n"
      " warp   [int]:  Internal BVT parameter.\n"
      " warpl  [int]:  Internal BVT parameter.\n"
      " warpu  [int]:  Internal BVT parameter.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "bvtsched_domain_get",
      (PyCFunction)pyxc_bvtsched_domain_get,
      METH_KEYWORDS, "\n"
      "Get per-domain tuning parameters under the BVT scheduler.\n"
      " dom [long]: Identifier of domain to be queried.\n"
      "Returns [dict]:\n"
      " domain [long]: Domain ID.\n"
      " mcuadv [long]: MCU Advance.\n"
      " warp   [long]: Warp.\n"
      " warpu  [long]:\n"
      " warpl  [long]: Warp limit,\n"
    },

    { "atropos_domain_set",
      (PyCFunction)pyxc_atropos_domain_set,
      METH_KEYWORDS, "\n"
      "Set the scheduling parameters for a domain when running with Atropos.\n"
      " dom      [long]: domain to set\n"
      " period   [long]: domain's scheduling period\n"
      " slice    [long]: domain's slice per period\n"
      " latency  [long]: wakeup latency hint\n"
      " xtratime [int]: boolean\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "atropos_domain_get",
      (PyCFunction)pyxc_atropos_domain_get,
      METH_KEYWORDS, "\n"
      "Get the current scheduling parameters for a domain when running with\n"
      "the Atropos scheduler."
      " dom      [long]: domain to query\n"
      "Returns:  [dict]\n"
      " domain   [long]: domain ID\n"
      " period   [long]: scheduler period\n"
      " slice    [long]: CPU reservation per period\n"
      " latency  [long]: unblocking latency hint\n"
      " xtratime [int] : 0 if not using slack time, nonzero otherwise\n" },

    { "rrobin_global_set",
      (PyCFunction)pyxc_rrobin_global_set,
      METH_KEYWORDS, "\n"
      "Set Round Robin scheduler slice.\n"
      " slice [long]: Round Robin scheduler slice\n"
      "Returns: [int] 0 on success, throws an exception on failure\n" },

    { "rrobin_global_get",
      (PyCFunction)pyxc_rrobin_global_get,
      METH_KEYWORDS, "\n"
      "Get Round Robin scheduler settings\n"
      "Returns [dict]:\n"
      " slice  [long]: Scheduler time slice.\n" },    

    { "vif_scheduler_set", 
      (PyCFunction)pyxc_vif_scheduler_set, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set per-network-interface scheduling parameters.\n"
      " dom          [long]:   Identifier of domain to be adjusted.\n"
      " vif          [int]:    Identifier of VIF to be adjusted.\n"
      " credit_bytes [int, 0]: Tx bytes permitted each interval.\n"
      " credit_usecs [int, 0]: Interval, in usecs. 0 == no scheduling.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vif_scheduler_get", 
      (PyCFunction)pyxc_vif_scheduler_get, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Query the per-network-interface scheduling parameters.\n"
      " dom          [long]:   Identifier of domain to be queried.\n"
      " vif          [int]:    Identifier of VIF to be queried.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " credit_bytes [int]: Tx bytes permitted each interval.\n"
      " credit_usecs [int]: Interval, in usecs. 0 == no scheduling.\n" },

    { "vif_stats_get", 
      (PyCFunction)pyxc_vif_stats_get, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Query the per-network-interface statistics.\n"
      " dom          [long]: Identifier of domain to be queried.\n"
      " vif          [int]:  Identifier of VIF to be queried.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " tx_bytes   [long]: Bytes transmitted.\n"
      " tx_packets [long]: Packets transmitted.\n"
      " rx_bytes   [long]: Bytes received.\n"
      " rx_packets [long]: Packets received.\n" },

    { "vbd_create", 
      (PyCFunction)pyxc_vbd_create, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Create a new virtual block device associated with a given domain.\n"
      " dom       [long]: Identifier of domain to get a new VBD.\n"
      " vbd       [int]:  Identifier for new VBD.\n"
      " writeable [int]:  Bool - is the new VBD writeable?\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vbd_destroy", 
      (PyCFunction)pyxc_vbd_destroy, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Destroy a virtual block device.\n"
      " dom       [long]: Identifier of domain containing the VBD.\n"
      " vbd       [int]:  Identifier of the VBD.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vbd_grow", 
      (PyCFunction)pyxc_vbd_grow, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Grow a virtual block device by appending a new extent.\n"
      " dom          [long]: Identifier of domain containing the VBD.\n"
      " vbd          [int]:  Identifier of the VBD.\n"
      " device       [int]:  Identifier of the real underlying block device.\n"
      " start_sector [long]: Real start sector of this extent.\n"
      " nr_sectors   [long]: Length, in sectors, of this extent.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vbd_shrink", 
      (PyCFunction)pyxc_vbd_shrink, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Shrink a virtual block device by deleting its final extent.\n"
      " dom          [long]: Identifier of domain containing the VBD.\n"
      " vbd          [int]:  Identifier of the VBD.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vbd_setextents", 
      (PyCFunction)pyxc_vbd_setextents, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set all the extent information for a virtual block device.\n"
      " dom          [long]: Identifier of domain containing the VBD.\n"
      " vbd          [int]:  Identifier of the VBD.\n"
      " extents      [list of dicts]: Per-extent information.\n"
      "  device       [int]:  Id of the real underlying block device.\n"
      "  start_sector [long]: Real start sector of this extent.\n"
      "  nr_sectors   [long]: Length, in sectors, of this extent.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "vbd_getextents", 
      (PyCFunction)pyxc_vbd_getextents, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get info on all the extents in a virtual block device.\n"
      " dom          [long]: Identifier of domain containing the VBD.\n"
      " vbd          [int]:  Identifier of the VBD.\n\n"
      "Returns: [list of dicts] per-extent information; empty on error.\n"
      " device       [int]:  Identifier of the real underlying block device.\n"
      " start_sector [long]: Real start sector of this extent.\n"
      " nr_sectors   [long]: Length, in sectors, of this extent.\n" },

    { "vbd_probe", 
      (PyCFunction)pyxc_vbd_probe, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Get information regarding extant virtual block devices.\n"
      " dom          [long, ALL]: Domain to query (default is to query all).\n"
      " max_vbds     [int, 1024]: Maximum VBDs to query.\n\n"
      "Returns: [list of dicts] if list length is less than 'max_vbds'\n"
      "         parameter then there was an error, or there were fewer vbds.\n"
      " dom        [long]: Domain containing this VBD.\n"
      " vbd        [int]:  Domain-specific identifier of this VBD.\n"
      " writeable  [int]:  Bool - is this VBD writeable?\n"
      " nr_sectors [long]: Size of this VBD, in 512-byte sectors.\n" },

    { "evtchn_bind_interdomain", 
      (PyCFunction)pyxc_evtchn_bind_interdomain, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Open an event channel between two domains.\n"
      " dom1 [long, SELF]: First domain to be connected.\n"
      " dom2 [long, SELF]: Second domain to be connected.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " port1 [int]: Port-id for endpoint at dom1.\n"
      " port2 [int]: Port-id for endpoint at dom2.\n" },

    { "evtchn_close", 
      (PyCFunction)pyxc_evtchn_close, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Close an event channel.\n"
      " dom  [long, SELF]: Dom-id of one endpoint of the channel.\n"
      " port [int]:        Port-id of one endpoint of the channel.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "evtchn_send", 
      (PyCFunction)pyxc_evtchn_send, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Send an event along a locally-connected event channel.\n"
      " port [int]: Port-id of a local channel endpoint.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "evtchn_status", 
      (PyCFunction)pyxc_evtchn_status, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Query the status of an event channel.\n"
      " dom  [long, SELF]: Dom-id of one endpoint of the channel.\n"
      " port [int]:        Port-id of one endpoint of the channel.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " status [str]:  'closed', 'unbound', 'interdomain', 'pirq',"
      " or 'virq'.\n"
      "The following are returned if 'status' is 'interdomain':\n"
      " dom  [long]: Dom-id of remote endpoint.\n"
      " port [int]:  Port-id of remote endpoint.\n"
      "The following are returned if 'status' is 'pirq' or 'virq':\n"
      " irq  [int]:  IRQ number.\n" },

    { "physdev_pci_access_modify",
      (PyCFunction)pyxc_physdev_pci_access_modify,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allow a domain access to a PCI device\n"
      " dom    [long]: Identifier of domain to be allowed access.\n"
      " bus    [int]:  PCI bus\n"
      " dev    [int]:  PCI slot\n"
      " func   [int]:  PCI function\n"
      " enable [int]:  Non-zero means enable access; else disable access\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },
 
    { "readconsolering", 
      (PyCFunction)pyxc_readconsolering, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Read Xen's console ring.\n"
      " clear [int, 0]: Bool - clear the ring after reading from it?\n\n"
      "Returns: [str] string is empty on failure.\n" },

    { "physinfo",
      (PyCFunction)pyxc_physinfo,
      METH_VARARGS, "\n"
      "Get information about the physical host machine\n"
      "Returns [dict]: information about the hardware"
      "        [None]: on failure.\n" },

    { NULL, NULL, 0, NULL }
};


/*
 * Definitions for the 'Xc' module wrapper.
 */

staticforward PyTypeObject PyXcType;

static PyObject *PyXc_new(PyObject *self, PyObject *args)
{
    XcObject *xc;

    if ( !PyArg_ParseTuple(args, ":new") )
        return NULL;

    xc = PyObject_New(XcObject, &PyXcType);

    if ( (xc->xc_handle = xc_interface_open()) == -1 )
    {
        PyObject_Del((PyObject *)xc);
        return NULL;
    }

    return (PyObject *)xc;
}

static PyObject *PyXc_getattr(PyObject *obj, char *name)
{
    return Py_FindMethod(pyxc_methods, obj, name);
}

static void PyXc_dealloc(PyObject *self)
{
    XcObject *xc = (XcObject *)self;
    (void)xc_interface_close(xc->xc_handle);
    PyObject_Del(self);
}

static PyTypeObject PyXcType = {
    PyObject_HEAD_INIT(&PyType_Type)
    0,
    "Xc",
    sizeof(XcObject),
    0,
    PyXc_dealloc,    /* tp_dealloc     */
    NULL,            /* tp_print       */
    PyXc_getattr,    /* tp_getattr     */
    NULL,            /* tp_setattr     */
    NULL,            /* tp_compare     */
    NULL,            /* tp_repr        */
    NULL,            /* tp_as_number   */
    NULL,            /* tp_as_sequence */
    NULL,            /* tp_as_mapping  */
    NULL             /* tp_hash        */
};

static PyMethodDef PyXc_methods[] = {
    { "new", PyXc_new, METH_VARARGS, "Create a new Xc object." },
    { NULL, NULL, 0, NULL }
};

PyMODINIT_FUNC initXc(void)
{
    PyObject *m, *d;

    m = Py_InitModule("Xc", PyXc_methods);

    d = PyModule_GetDict(m);
    xc_error = PyErr_NewException("Xc.error", NULL, NULL);
    PyDict_SetItemString(d, "error", xc_error);

    zero = PyInt_FromLong(0);
}
