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
#include <netdb.h>
#include <arpa/inet.h>

#include "xc_private.h"
#include "linux_boot_params.h"

/* Needed for Python versions earlier than 2.3. */
#ifndef PyMODINIT_FUNC
#define PyMODINIT_FUNC DL_EXPORT(void)
#endif

#define XENPKG "xen.lowlevel.xc"

static PyObject *xc_error, *zero;

typedef struct {
    PyObject_HEAD;
    int xc_handle;
} XcObject;

/*
 * Definitions for the 'xc' object type.
 */

static PyObject *pyxc_domain_dumpcore(PyObject *self,
				      PyObject *args,
				      PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    char *corefile;

    static char *kwd_list[] = { "dom", "corefile", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is", kwd_list,
                                      &dom, &corefile) )
        goto exit;

    if ( (corefile == NULL) || (corefile[0] == '\0') )
        goto exit;

    if ( xc_domain_dumpcore(xc->xc_handle, dom, corefile) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;

 exit:
    return NULL;
}

static PyObject *pyxc_handle(PyObject *self)
{
    XcObject *xc = (XcObject *)self;

    return PyInt_FromLong(xc->xc_handle);
}

static PyObject *pyxc_domain_create(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32          dom = 0;
    int          ret;
    u32          ssidref = 0x0;

    static char *kwd_list[] = { "dom", "ssidref", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwd_list,
                                      &dom, &ssidref))
        return NULL;

    if ( (ret = xc_domain_create(xc->xc_handle, ssidref, &dom)) < 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyInt_FromLong(dom);
}

static PyObject *pyxc_domain_pause(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;

    if ( xc_domain_pause(xc->xc_handle, dom) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_unpause(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;

    if ( xc_domain_unpause(xc->xc_handle, dom) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_destroy(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;

    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;

    if ( xc_domain_destroy(xc->xc_handle, dom) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_pincpu(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    int vcpu = 0;
    cpumap_t cpumap = 0xFFFFFFFF;

    static char *kwd_list[] = { "dom", "vcpu", "cpumap", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|ii", kwd_list, 
                                      &dom, &vcpu, &cpumap) )
        return NULL;

    if ( xc_domain_pincpu(xc->xc_handle, dom, vcpu, &cpumap) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_setcpuweight(PyObject *self,
					  PyObject *args,
					  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    float cpuweight = 1;

    static char *kwd_list[] = { "dom", "cpuweight", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|f", kwd_list, 
                                      &dom, &cpuweight) )
        return NULL;

    if ( xc_domain_setcpuweight(xc->xc_handle, dom, cpuweight) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_getinfo(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    PyObject *list, *vcpu_list, *cpumap_list, *info_dict;

    u32 first_dom = 0;
    int max_doms = 1024, nr_doms, i, j;
    xc_dominfo_t *info;

    static char *kwd_list[] = { "first_dom", "max_doms", NULL };
    
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwd_list,
                                      &first_dom, &max_doms) )
        return NULL;

    if ( (info = malloc(max_doms * sizeof(xc_dominfo_t))) == NULL )
        return PyErr_NoMemory();

    nr_doms = xc_domain_getinfo(xc->xc_handle, first_dom, max_doms, info);

    if (nr_doms < 0)
    {
        free(info);
        return PyErr_SetFromErrno(xc_error);
    }

    list = PyList_New(nr_doms);
    for ( i = 0 ; i < nr_doms; i++ )
    {
        vcpu_list = PyList_New(MAX_VIRT_CPUS);
        cpumap_list = PyList_New(MAX_VIRT_CPUS);
        for ( j = 0; j < MAX_VIRT_CPUS; j++ ) {
            PyList_SetItem( vcpu_list, j, 
                            Py_BuildValue("i", info[i].vcpu_to_cpu[j]));
            PyList_SetItem( cpumap_list, j, 
                            Py_BuildValue("i", info[i].cpumap[j]));
        }
                 
        info_dict = Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i"
                                  ",s:l,s:L,s:l,s:i,s:i}",
                                  "dom",       info[i].domid,
                                  "vcpus",     info[i].vcpus,
                                  "dying",     info[i].dying,
                                  "crashed",   info[i].crashed,
                                  "shutdown",  info[i].shutdown,
                                  "paused",    info[i].paused,
                                  "blocked",   info[i].blocked,
                                  "running",   info[i].running,
                                  "mem_kb",    info[i].nr_pages*(XC_PAGE_SIZE/1024),
                                  "cpu_time",  info[i].cpu_time,
                                  "maxmem_kb", info[i].max_memkb,
                                  "ssidref",   info[i].ssidref,
                                  "shutdown_reason", info[i].shutdown_reason);
        PyDict_SetItemString( info_dict, "vcpu_to_cpu", vcpu_list );
        PyDict_SetItemString( info_dict, "cpumap", cpumap_list );
        PyList_SetItem( list, i, info_dict);
 
    }

    free(info);

    return list;
}

static PyObject *pyxc_linux_build(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    char *image, *ramdisk = NULL, *cmdline = "";
    int flags = 0, vcpus = 1;
    int store_evtchn, console_evtchn;
    unsigned long store_mfn = 0;
    unsigned long console_mfn = 0;

    static char *kwd_list[] = { "dom", "store_evtchn", 
                                "console_evtchn", "image", 
				/* optional */
				"ramdisk", "cmdline", "flags",
				"vcpus", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiis|ssii", kwd_list,
                                      &dom, &store_evtchn,
				      &console_evtchn, &image, 
				      /* optional */
				      &ramdisk, &cmdline, &flags,
                                      &vcpus) )
        return NULL;

    if ( xc_linux_build(xc->xc_handle, dom, image,
                        ramdisk, cmdline, flags, vcpus,
                        store_evtchn, &store_mfn, 
			console_evtchn, &console_mfn) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    return Py_BuildValue("{s:i,s:i}", 
			 "store_mfn", store_mfn,
			 "console_mfn", console_mfn);
}

static PyObject *pyxc_vmx_build(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32   dom;
    char *image, *ramdisk = NULL, *cmdline = "";
    PyObject *memmap;
    int   control_evtchn, store_evtchn;
    int flags = 0, vcpus = 1;
    int numItems, i;
    int memsize;
    struct mem_map mem_map;
    unsigned long store_mfn = 0;

    static char *kwd_list[] = { "dom", "control_evtchn", "store_evtchn",
                                "memsize", "image", "memmap",
				"ramdisk", "cmdline", "flags",
				"vcpus", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiisO!|ssii", kwd_list, 
                                      &dom, &control_evtchn, &store_evtchn,
                                      &memsize,
                                      &image, &PyList_Type, &memmap,
				      &ramdisk, &cmdline, &flags, &vcpus) )
        return NULL;

    memset(&mem_map, 0, sizeof(mem_map));
    /* Parse memmap */

    /* get the number of lines passed to us */
    numItems = PyList_Size(memmap) - 1;	/* removing the line 
					   containing "memmap" */
    mem_map.nr_map = numItems;
   
    /* should raise an error here. */
    if (numItems < 0) return NULL; /* Not a list */

    /* iterate over items of the list, grabbing ranges and parsing them */
    for (i = 1; i <= numItems; i++) {	// skip over "memmap"
	    PyObject *item, *f1, *f2, *f3, *f4;
	    int numFields;
	    unsigned long lf1, lf2, lf3, lf4;
	    char *sf1, *sf2;
	    
	    /* grab the string object from the next element of the list */
	    item = PyList_GetItem(memmap, i); /* Can't fail */

	    /* get the number of lines passed to us */
	    numFields = PyList_Size(item);

	    if (numFields != 4)
		    return NULL;

	    f1 = PyList_GetItem(item, 0);
	    f2 = PyList_GetItem(item, 1);
	    f3 = PyList_GetItem(item, 2);
	    f4 = PyList_GetItem(item, 3);

	    /* Convert objects to strings/longs */
	    sf1 = PyString_AsString(f1);
	    sf2 = PyString_AsString(f2);
	    lf3 = PyLong_AsLong(f3);
	    lf4 = PyLong_AsLong(f4);
	    if ( sscanf(sf1, "%lx", &lf1) != 1 )
                return NULL;
	    if ( sscanf(sf2, "%lx", &lf2) != 1 )
                return NULL;

            mem_map.map[i-1].addr = lf1;
            mem_map.map[i-1].size = lf2 - lf1;
            mem_map.map[i-1].type = lf3;
            mem_map.map[i-1].caching_attr = lf4;
    }

    if ( xc_vmx_build(xc->xc_handle, dom, memsize, image, &mem_map,
                        ramdisk, cmdline, control_evtchn, flags,
                        vcpus, store_evtchn, &store_mfn) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    return Py_BuildValue("{s:i}", "store_mfn", store_mfn);
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

    u32 dom;
    u32 mcuadv;
    int warpback; 
    s32 warpvalue;
    long long warpl;
    long long warpu;

    static char *kwd_list[] = { "dom", "mcuadv", "warpback", "warpvalue",
                                "warpl", "warpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiiLL", kwd_list,
                                      &dom, &mcuadv, &warpback, &warpvalue, 
                                      &warpl, &warpu) )
        return NULL;

    if ( xc_bvtsched_domain_set(xc->xc_handle, dom, mcuadv, 
                                warpback, warpvalue, warpl, warpu) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_bvtsched_domain_get(PyObject *self,
                                          PyObject *args,
                                          PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u32 dom;
    u32 mcuadv;
    int warpback; 
    s32 warpvalue;
    long long warpl;
    long long warpu;
    
    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;
    
    if ( xc_bvtsched_domain_get(xc->xc_handle, dom, &mcuadv, &warpback,
                            &warpvalue, &warpl, &warpu) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:l,s:l,s:l,s:l}",
                         "domain", dom,
                         "mcuadv", mcuadv,
                         "warpback", warpback,
                         "warpvalue", warpvalue,
                         "warpl", warpl,
                         "warpu", warpu);
}

static PyObject *pyxc_evtchn_alloc_unbound(PyObject *self,
                                           PyObject *args,
                                           PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    int port = 0;

    static char *kwd_list[] = { "dom", "port", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list,
                                      &dom, &port) )
        return NULL;

    if ( xc_evtchn_alloc_unbound(xc->xc_handle, dom, &port) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyInt_FromLong(port);
}

static PyObject *pyxc_evtchn_bind_interdomain(PyObject *self,
                                              PyObject *args,
                                              PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom1 = DOMID_SELF, dom2 = DOMID_SELF;
    int port1 = 0, port2 = 0;

    static char *kwd_list[] = { "dom1", "dom2", "port1", "port2", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|iiii", kwd_list, 
                                      &dom1, &dom2, &port1, &port2) )
        return NULL;

    if ( xc_evtchn_bind_interdomain(xc->xc_handle, dom1, 
                                    dom2, &port1, &port2) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:i}", 
                         "port1", port1,
                         "port2", port2);
}

static PyObject *pyxc_evtchn_bind_virq(PyObject *self,
                                       PyObject *args,
                                       PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    int virq, port;

    static char *kwd_list[] = { "virq", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &virq) )
        return NULL;

    if ( xc_evtchn_bind_virq(xc->xc_handle, virq, &port) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyInt_FromLong(port);
}

static PyObject *pyxc_evtchn_close(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom = DOMID_SELF;
    int port;

    static char *kwd_list[] = { "port", "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
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

    u32 dom = DOMID_SELF;
    int port, ret;
    xc_evtchn_status_t status;

    static char *kwd_list[] = { "port", "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
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
        dict = Py_BuildValue("{s:s,s:i,s:i}", 
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
    u32 dom;
    int bus, dev, func, enable, ret;

    static char *kwd_list[] = { "dom", "bus", "dev", "func", "enable", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iiiii", kwd_list, 
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
    char         _str[32768], *str = _str;
    unsigned int count = 32768;
    int          ret;

    static char *kwd_list[] = { "clear", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwd_list, &clear) )
        return NULL;

    ret = xc_readconsolering(xc->xc_handle, &str, &count, clear);
    if ( ret < 0 )
        return PyErr_SetFromErrno(xc_error);

    return PyString_FromStringAndSize(str, count);
}

static PyObject *pyxc_physinfo(PyObject *self,
                               PyObject *args,
                               PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    xc_physinfo_t info;
    char cpu_cap[128], *p=cpu_cap, *q=cpu_cap;
    int i;
    
    if ( !PyArg_ParseTuple(args, "") )
        return NULL;

    if ( xc_physinfo(xc->xc_handle, &info) != 0 )
        return PyErr_SetFromErrno(xc_error);

    *q=0;
    for(i=0;i<sizeof(info.hw_cap)/4;i++)
    {
        p+=sprintf(p,"%08x:",info.hw_cap[i]);
        if(info.hw_cap[i])
	    q=p;
    }
    if(q>cpu_cap)
        *(q-1)=0;

    return Py_BuildValue("{s:i,s:i,s:i,s:i,s:l,s:l,s:i,s:s}",
                         "threads_per_core", info.threads_per_core,
                         "cores_per_socket", info.cores_per_socket,
                         "sockets_per_node", info.sockets_per_node,
                         "nr_nodes",         info.nr_nodes,
                         "total_pages",      info.total_pages,
                         "free_pages",       info.free_pages,
                         "cpu_khz",          info.cpu_khz,
                         "hw_caps",          cpu_cap);
}

static PyObject *pyxc_xeninfo(PyObject *self,
                              PyObject *args,
                              PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    xen_extraversion_t xen_extra;
    xen_compile_info_t xen_cc;
    xen_changeset_info_t xen_chgset;
    xen_capabilities_info_t xen_caps;
    xen_parameters_info_t xen_parms;
    long xen_version;
    char str[128];

    xen_version = xc_version(xc->xc_handle, XENVER_version, NULL);

    if ( xc_version(xc->xc_handle, XENVER_extraversion, &xen_extra) != 0 )
        return PyErr_SetFromErrno(xc_error);

    if ( xc_version(xc->xc_handle, XENVER_compile_info, &xen_cc) != 0 )
        return PyErr_SetFromErrno(xc_error);

    if ( xc_version(xc->xc_handle, XENVER_changeset, &xen_chgset) != 0 )
        return PyErr_SetFromErrno(xc_error);

    if ( xc_version(xc->xc_handle, XENVER_capabilities, &xen_caps) != 0 )
        return PyErr_SetFromErrno(xc_error);

    if ( xc_version(xc->xc_handle, XENVER_parameters, &xen_parms) != 0 )
        return PyErr_SetFromErrno(xc_error);

    sprintf(str,"virt_start=0x%lx",xen_parms.virt_start);

    return Py_BuildValue("{s:i,s:i,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s}",
                         "xen_major", xen_version >> 16,
                         "xen_minor", (xen_version & 0xffff),
                         "xen_extra", xen_extra,
                         "xen_caps",  xen_caps,
                         "xen_params", str,
                         "xen_changeset", xen_chgset,
                         "cc_compiler", xen_cc.compiler,
                         "cc_compile_by", xen_cc.compile_by,
                         "cc_compile_domain", xen_cc.compile_domain,
                         "cc_compile_date", xen_cc.compile_date);
}


static PyObject *pyxc_sedf_domain_set(PyObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u32 domid;
    u64 period, slice, latency;
    u16 extratime, weight;
    static char *kwd_list[] = { "dom", "period", "slice",
                                "latency", "extratime", "weight",NULL };
    
    if( !PyArg_ParseTupleAndKeywords(args, kwds, "iLLLhh", kwd_list, 
                                     &domid, &period, &slice,
                                     &latency, &extratime, &weight) )
        return NULL;
   if ( xc_sedf_domain_set(xc->xc_handle, domid, period,
                           slice, latency, extratime,weight) != 0 )
        return PyErr_SetFromErrno(xc_error);

    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_sedf_domain_get(PyObject *self,
                                         PyObject *args,
                                         PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u32 domid;
    u64 period, slice,latency;
    u16 weight, extratime;
    
    static char *kwd_list[] = { "dom", NULL };

    if( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &domid) )
        return NULL;
    
    if ( xc_sedf_domain_get( xc->xc_handle, domid, &period,
                                &slice,&latency,&extratime,&weight) )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:L,s:L,s:L,s:i}",
                         "domain",    domid,
                         "period",    period,
                         "slice",     slice,
			 "latency",   latency,
			 "extratime", extratime);
}

static PyObject *pyxc_shadow_control(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    int op=0;

    static char *kwd_list[] = { "dom", "op", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
                                      &dom, &op) )
        return NULL;

    if ( xc_shadow_control(xc->xc_handle, dom, op, NULL, 0, NULL) < 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_setmaxmem(PyObject *self,
                                       PyObject *args,
                                       PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    unsigned int maxmem_kb;

    static char *kwd_list[] = { "dom", "maxmem_kb", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "ii", kwd_list, 
                                      &dom, &maxmem_kb) )
        return NULL;

    if ( xc_domain_setmaxmem(xc->xc_handle, dom, maxmem_kb) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_domain_memory_increase_reservation(PyObject *self,
							 PyObject *args,
							 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom;
    unsigned long mem_kb;
    unsigned int extent_order = 0 , address_bits = 0;
    unsigned long nr_extents;

    static char *kwd_list[] = { "dom", "mem_kb", "extent_order", "address_bits", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "il|ii", kwd_list, 
                                      &dom, &mem_kb, &extent_order, &address_bits) )
        return NULL;

    /* round down to nearest power of 2. Assume callers using extent_order>0
       know what they are doing */
    nr_extents = (mem_kb / (XC_PAGE_SIZE/1024)) >> extent_order;
    if ( xc_domain_memory_increase_reservation(xc->xc_handle, dom, 
					       nr_extents, extent_order, 
					       address_bits, NULL) )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_init_store(PyObject *self, PyObject *args,
				 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    int remote_port;

    static char *kwd_list[] = { "remote_port", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, 
                                      &remote_port) )
        return NULL;

    return PyInt_FromLong(xc_init_store(xc->xc_handle, remote_port));
}


static PyMethodDef pyxc_methods[] = {
    { "handle",
      (PyCFunction)pyxc_handle,
      0, "\n"
      "Query the xc control interface file descriptor.\n\n"
      "Returns: [int] file descriptor\n" },

    { "domain_create", 
      (PyCFunction)pyxc_domain_create, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Create a new domain.\n"
      " dom    [int, 0]:        Domain identifier to use (allocated if zero).\n"
      "Returns: [int] new domain identifier; -1 on error.\n" },

    { "domain_dumpcore", 
      (PyCFunction)pyxc_domain_dumpcore, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Dump core of a domain.\n"
      " dom [int]: Identifier of domain to dump core of.\n"
      " corefile [string]: Name of corefile to be created.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_pause", 
      (PyCFunction)pyxc_domain_pause, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Temporarily pause execution of a domain.\n"
      " dom [int]: Identifier of domain to be paused.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_unpause", 
      (PyCFunction)pyxc_domain_unpause, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "(Re)start execution of a domain.\n"
      " dom [int]: Identifier of domain to be unpaused.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_destroy", 
      (PyCFunction)pyxc_domain_destroy, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Destroy a domain.\n"
      " dom [int]:    Identifier of domain to be destroyed.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_pincpu", 
      (PyCFunction)pyxc_domain_pincpu, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Pin a VCPU to a specified set CPUs.\n"
      " dom [int]:     Identifier of domain to which VCPU belongs.\n"
      " vcpu [int, 0]: VCPU being pinned.\n"
      " cpumap [int, -1]: Bitmap of usable CPUs.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_setcpuweight", 
      (PyCFunction)pyxc_domain_setcpuweight, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set cpuweight scheduler parameter for domain.\n"
      " dom [int]:            Identifier of domain to be changed.\n"
      " cpuweight [float, 1]: VCPU being pinned.\n"
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
      "reason why it shut itself down.\n" 
      " vcpu_to_cpu [[int]]: List that maps VCPUS to CPUS\n" },

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

    { "vmx_build", 
      (PyCFunction)pyxc_vmx_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new Linux guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " memmap  [str]: 	    Memory map.\n\n"
      " ramdisk [str, n/a]: Name of ramdisk file, if any.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "bvtsched_global_set",
      (PyCFunction)pyxc_bvtsched_global_set,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set global tuning parameters for Borrowed Virtual Time scheduler.\n"
      " ctx_allow [int]: Minimal guaranteed quantum.\n\n"
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
      " dom       [int]: Identifier of domain to be tuned.\n"
      " mcuadv    [int]: Proportional to the inverse of the domain's weight.\n"
      " warpback  [int]: Warp ? \n"
      " warpvalue [int]: How far to warp domain's EVT on unblock.\n"
      " warpl     [int]: How long the domain can run warped.\n"
      " warpu     [int]: How long before the domain can warp again.\n\n"
      "Returns:   [int] 0 on success; -1 on error.\n" },

    { "bvtsched_domain_get",
      (PyCFunction)pyxc_bvtsched_domain_get,
      METH_KEYWORDS, "\n"
      "Get per-domain tuning parameters under the BVT scheduler.\n"
      " dom [int]: Identifier of domain to be queried.\n"
      "Returns [dict]:\n"
      " domain [int]:  Domain ID.\n"
      " mcuadv [long]: MCU Advance.\n"
      " warp   [long]: Warp.\n"
      " warpu  [long]: Unwarp requirement.\n"
      " warpl  [long]: Warp limit,\n"
    },
    
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
      METH_KEYWORDS, "\n"
      "Get the current scheduling parameters for a domain when running with\n"
      "the Atropos scheduler."
      " dom       [int]: domain to query\n"
      "Returns:   [dict]\n"
      " domain    [int]: domain ID\n"
      " period    [long]: scheduler period\n"
      " slice     [long]: CPU reservation per period\n"
      " latency   [long]: domain's wakeup latency hint\n"
      " extratime [int]:  domain aware of extratime?\n"},

    { "evtchn_alloc_unbound", 
      (PyCFunction)pyxc_evtchn_alloc_unbound,
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Allocate an unbound local port that will await a remote connection.\n"
      " dom [int]: Remote domain to accept connections from.\n\n"
      "Returns: [int] Unbound event-channel port.\n" },

    { "evtchn_bind_interdomain", 
      (PyCFunction)pyxc_evtchn_bind_interdomain, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Open an event channel between two domains.\n"
      " dom1 [int, SELF]: First domain to be connected.\n"
      " dom2 [int, SELF]: Second domain to be connected.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " port1 [int]: Port-id for endpoint at dom1.\n"
      " port2 [int]: Port-id for endpoint at dom2.\n" },

    { "evtchn_bind_virq", 
      (PyCFunction)pyxc_evtchn_bind_virq, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Bind an event channel to the specified VIRQ.\n"
      " virq [int]: VIRQ to bind.\n\n"
      "Returns: [int] Bound event-channel port.\n" },

    { "evtchn_close", 
      (PyCFunction)pyxc_evtchn_close, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Close an event channel. If interdomain, sets remote end to 'unbound'.\n"
      " dom  [int, SELF]: Dom-id of one endpoint of the channel.\n"
      " port [int]:       Port-id of one endpoint of the channel.\n\n"
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
      " dom  [int, SELF]: Dom-id of one endpoint of the channel.\n"
      " port [int]:       Port-id of one endpoint of the channel.\n\n"
      "Returns: [dict] dictionary is empty on failure.\n"
      " status [str]:  'closed', 'unbound', 'interdomain', 'pirq',"
      " or 'virq'.\n"
      "The following are returned if 'status' is 'interdomain':\n"
      " dom  [int]: Dom-id of remote endpoint.\n"
      " port [int]: Port-id of remote endpoint.\n"
      "The following are returned if 'status' is 'pirq' or 'virq':\n"
      " irq  [int]: IRQ number.\n" },

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
      METH_VARARGS, "\n"
      "Get information about the physical host machine\n"
      "Returns [dict]: information about the hardware"
      "        [None]: on failure.\n" },

    { "xeninfo",
      (PyCFunction)pyxc_xeninfo,
      METH_VARARGS, "\n"
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

    { "domain_setmaxmem", 
      (PyCFunction)pyxc_domain_setmaxmem, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set a domain's memory limit\n"
      " dom [int]: Identifier of domain.\n"
      " maxmem_kb [int]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_memory_increase_reservation", 
      (PyCFunction)pyxc_domain_memory_increase_reservation, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Increase a domain's memory reservation\n"
      " dom [int]: Identifier of domain.\n"
      " mem_kb [long]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "init_store", 
      (PyCFunction)pyxc_init_store, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Initialize the store event channel and return the store page mfn.\n"
      " remote_port [int]: store event channel port number.\n"
      "Returns: [int] mfn on success; <0 on error.\n" },

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
        return PyErr_SetFromErrno(xc_error);
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
    { "new", PyXc_new, METH_VARARGS, "Create a new " XENPKG " object." },
    { NULL, NULL, 0, NULL }
};

PyMODINIT_FUNC initxc(void)
{
    PyObject *m, *d;

    m = Py_InitModule(XENPKG, PyXc_methods);

    d = PyModule_GetDict(m);
    xc_error = PyErr_NewException(XENPKG ".error", NULL, NULL);
    PyDict_SetItemString(d, "error", xc_error);
    PyDict_SetItemString(d, "VIRQ_DOM_EXC", PyInt_FromLong(VIRQ_DOM_EXC));

    zero = PyInt_FromLong(0);

    /* KAF: This ensures that we get debug output in a timely manner. */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}
