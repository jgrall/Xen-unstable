/******************************************************************************
 * Xc.c
 * 
 * Copyright (c) 2003-2004, K A Fraser (University of Cambridge)
 */

#include <Python.h>
#include <xc.h>
#include <zlib.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

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

    unsigned int mem_kb = 0;
    char        *name   = "(anon)";
    int          cpu = -1;
    u32          dom;
    int          ret;

    static char *kwd_list[] = { "mem_kb", "name", "cpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|isi", kwd_list, 
                                      &mem_kb, &name, &cpu) )
        return NULL;

    if ( (ret = xc_domain_create(xc->xc_handle, mem_kb, name, cpu, &dom)) < 0 )
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
    int cpu = -1;

    static char *kwd_list[] = { "dom", "cpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|i", kwd_list, 
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

    u32 first_dom = 0;
    int max_doms = 1024, nr_doms, i;
    xc_dominfo_t *info;

    static char *kwd_list[] = { "first_dom", "max_doms", NULL };
    
    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwd_list,
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
            Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i,s:i,s:i"
                          ",s:l,s:L,s:s,s:l,s:i}",
                          "dom",       info[i].domid,
                          "cpu",       info[i].cpu,
                          "dying",     info[i].dying,
                          "crashed",   info[i].crashed,
                          "shutdown",  info[i].shutdown,
                          "paused",    info[i].paused,
                          "blocked",   info[i].blocked,
                          "running",   info[i].running,
                          "mem_kb",    info[i].nr_pages*4,
                          "cpu_time",  info[i].cpu_time,
                          "name",      info[i].name,
                          "maxmem_kb", info[i].max_memkb,
                          "shutdown_reason", info[i].shutdown_reason
                ));
    }

    free(info);

    return list;
}

static PyObject *pyxc_linux_save(PyObject *self,
                                 PyObject *args,
                                 PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32   dom;
    char *state_file;
    int   progress = 1, live = -1, debug = 0;
    unsigned int flags = 0;

    static char *kwd_list[] = { "dom", "state_file", "progress", 
                                "live", "debug", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is|iii", kwd_list, 
                                      &dom, &state_file, &progress, 
                                      &live, &debug) )
        return NULL;

    if (progress)  flags |= XCFLAGS_VERBOSE;
    if (live == 1) flags |= XCFLAGS_LIVE;
    if (debug)     flags |= XCFLAGS_DEBUG;

    if ( strncmp(state_file,"tcp:", strlen("tcp:")) == 0 )
    {
#define max_namelen 64
        char server[max_namelen];
        char *port_s;
        int port=777;
        int sd = -1;
        struct hostent *h;
        struct sockaddr_in s;
        int sockbufsize;
        int rc = -1;

        int writerfn(void *fd, const void *buf, size_t count)
        {
            int tot = 0, rc;
            do {
                rc = write( (int) fd, ((char*)buf)+tot, count-tot );
                if ( rc < 0 ) { perror("WRITE"); return rc; };
                tot += rc;
            }
            while ( tot < count );
            return 0;
        }

        if (live == -1) flags |= XCFLAGS_LIVE; /* default to live for tcp */

        strncpy( server, state_file+strlen("tcp://"), max_namelen);
        server[max_namelen-1]='\0';
        if ( (port_s = strchr(server,':')) != NULL )
        {
            *port_s = '\0';
            port = atoi(port_s+1);
        }

        printf("X server=%s port=%d\n",server,port);
 
        h = gethostbyname(server);
        sd = socket (AF_INET,SOCK_STREAM,0);
        if ( sd < 0 )
            goto serr;
        s.sin_family = AF_INET;
        bcopy ( h->h_addr, &(s.sin_addr.s_addr), h->h_length);
        s.sin_port = htons(port);
        if ( connect(sd, (struct sockaddr *) &s, sizeof(s)) ) 
            goto serr;

        sockbufsize=128*1024;
        if ( setsockopt(sd, SOL_SOCKET, SO_SNDBUF, 
                        &sockbufsize, sizeof sockbufsize) < 0 ) 
            goto serr;

        if ( xc_linux_save(xc->xc_handle, dom, flags, 
                           writerfn, (void*)sd) == 0 )
        {
            if ( read( sd, &rc, sizeof(int) ) != sizeof(int) )
                goto serr;
  
            if ( rc == 0 )
            {
                printf("Migration succesful -- destroy local copy\n");
                xc_domain_destroy(xc->xc_handle, dom);
                close(sd);
                Py_INCREF(zero);
                return zero;
            }
            else
                errno = rc;
        }

    serr:
        printf("Migration failed -- restart local copy\n");
        xc_domain_unpause(xc->xc_handle, dom);
        PyErr_SetFromErrno(xc_error);
        if ( sd >= 0 ) close(sd);
        return NULL;
    }    
    else
    {
        int fd = -1;
        gzFile gfd = NULL;

        int writerfn(void *fd, const void *buf, size_t count)
        {
            int rc;
            while ( ((rc = gzwrite( (gzFile*)fd, (void*)buf, count)) == -1) && 
                    (errno = EINTR) )
                continue;
            return ! (rc == count);
        }

        if (strncmp(state_file,"file:",strlen("file:")) == 0)
            state_file += strlen("file:");

        if ( (fd = open(state_file, O_CREAT|O_EXCL|O_WRONLY, 0644)) == -1 )
        {
            perror("Could not open file for writing");
            goto err;
        }

        /*
         * Compression rate 1: we want speed over compression. 
         * We're mainly going for those zero pages, after all.
         */

        if ( (gfd = gzdopen(fd, "wb1")) == NULL )
        {
            perror("Could not allocate compression state for state file");
            close(fd);
            goto err;
        }


        if ( xc_linux_save(xc->xc_handle, dom, flags, writerfn, gfd) == 0 )
        {
            /* kill domain. We don't want to do this for checkpointing, but
               if we don't do it here I think people will hurt themselves
               by accident... */
            xc_domain_destroy(xc->xc_handle, dom);
            gzclose(gfd);
            close(fd);

            Py_INCREF(zero);
            return zero;
        }

    err:
        PyErr_SetFromErrno(xc_error);
        if ( gfd != NULL )
            gzclose(gfd);
        if ( fd >= 0 )
            close(fd);
        unlink(state_file);
        return NULL;
    }

}

static PyObject *pyxc_linux_restore(PyObject *self,
                                    PyObject *args,
                                    PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    char        *state_file;
    int          progress = 1;
    u32          dom;
    unsigned int flags = 0;

    static char *kwd_list[] = { "dom", "state_file", "progress", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is|i", kwd_list, 
                                      &dom, &state_file, &progress) )
        return NULL;

    if ( progress )
        flags |= XCFLAGS_VERBOSE;

    if ( strncmp(state_file,"tcp:", strlen("tcp:")) == 0 )
    {
#define max_namelen 64
        char server[max_namelen];
        char *port_s;
        int port=777;
        int ld = -1, sd = -1;
        struct hostent *h;
        struct sockaddr_in s, d, p;
        socklen_t dlen, plen;
        int sockbufsize;
        int on = 1, rc = -1;

        int readerfn(void *fd, void *buf, size_t count)
        {
            int rc, tot = 0;
            do { 
                rc = read( (int) fd, ((char*)buf)+tot, count-tot ); 
                if ( rc < 0 ) { perror("READ"); return rc; }
                if ( rc == 0 ) { printf("read: need %d, tot=%d got zero\n",
                                        count-tot, tot); return -1; }
                tot += rc;
            } 
            while ( tot < count );
            return 0;
        }

        strncpy( server, state_file+strlen("tcp://"), max_namelen);
        server[max_namelen-1]='\0';
        if ( (port_s = strchr(server,':')) != NULL )
        {
            *port_s = '\0';
            port = atoi(port_s+1);
        }

        printf("X server=%s port=%d\n",server,port);
 
        h = gethostbyname(server);
        ld = socket (AF_INET,SOCK_STREAM,0);
        if ( ld < 0 ) goto serr;
        s.sin_family = AF_INET;
        s.sin_addr.s_addr = htonl(INADDR_ANY);
        s.sin_port = htons(port);

        if ( setsockopt(ld, SOL_SOCKET, SO_REUSEADDR, &on, sizeof (on)) < 0 )
            goto serr;

        if ( bind(ld, (struct sockaddr *) &s, sizeof(s)) ) 
            goto serr;

        if ( listen(ld, 1) )
            goto serr;

        dlen=sizeof(struct sockaddr);
        if ( (sd = accept(ld, (struct sockaddr *) &d, &dlen )) < 0 )
            goto serr;

        plen = sizeof(p);
        if ( getpeername(sd, (struct sockaddr_in *) &p, 
                         &plen) < 0 )
            goto serr;

        printf("Accepted connection from %s\n", inet_ntoa(p.sin_addr));
 
        sockbufsize=128*1024;
        if ( setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &sockbufsize, 
                        sizeof sockbufsize) < 0 ) 
            goto serr;

        rc = xc_linux_restore(xc->xc_handle, dom, flags, 
                              readerfn, (void*)sd, &dom);

        write( sd, &rc, sizeof(int) ); 

        if (rc == 0)
        {
            close(sd);
            Py_INCREF(zero);
            return zero;
        }
        errno = rc;

    serr:
        PyErr_SetFromErrno(xc_error);
        if ( ld >= 0 ) close(ld);
        if ( sd >= 0 ) close(sd);
        return NULL;
    }    
    else
    {
        int fd = -1;
        gzFile gfd = NULL;

        int readerfn(void *fd, void *buf, size_t count)
        {
            int rc;
            while ( ((rc = gzread( (gzFile*)fd, (void*)buf, count)) == -1) && 
                    (errno = EINTR) )
                continue;
            return ! (rc == count);
        }

        if ( strncmp(state_file,"file:",strlen("file:")) == 0 )
            state_file += strlen("file:");

        if ( (fd = open(state_file, O_RDONLY)) == -1 )
        {
            perror("Could not open file for writing");
            goto err;
        }

        /*
         * Compression rate 1: we want speed over compression. 
         * We're mainly going for those zero pages, after all.
         */
        if ( (gfd = gzdopen(fd, "rb")) == NULL )
        {
            perror("Could not allocate compression state for state file");
            close(fd);
            goto err;
        }


        if ( xc_linux_restore(xc->xc_handle, dom, flags, 
                              readerfn, gfd, &dom) == 0 )
        {
            gzclose(gfd);
            close(fd);

            Py_INCREF(zero);
            return zero;
        }

    err:
        PyErr_SetFromErrno(xc_error);
        if ( gfd != NULL ) gzclose(gfd);
        if ( fd >= 0 ) close(fd);
        return NULL;
    }

}

static PyObject *pyxc_linux_build(PyObject *self,
                                  PyObject *args,
                                  PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32   dom;
    char *image, *ramdisk = NULL, *cmdline = "";
    int   control_evtchn, flags = 0;

    static char *kwd_list[] = { "dom", "control_evtchn", 
                                "image", "ramdisk", "cmdline", "flags",
                                NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iis|ssi", kwd_list, 
                                      &dom, &control_evtchn, 
                                      &image, &ramdisk, &cmdline, &flags) )
        return NULL;

    if ( xc_linux_build(xc->xc_handle, dom, image,
                        ramdisk, cmdline, control_evtchn, flags) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}

static PyObject *pyxc_netbsd_build(PyObject *self,
                                   PyObject *args,
                                   PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32   dom;
    char *image, *ramdisk = NULL, *cmdline = "";
    int   control_evtchn;

    static char *kwd_list[] = { "dom", "control_evtchn",
                                "image", "ramdisk", "cmdline", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "iis|ssi", kwd_list, 
                                      &dom, &control_evtchn,
                                      &image, &ramdisk, &cmdline) )
        return NULL;

    if ( xc_netbsd_build(xc->xc_handle, dom, image, 
                         cmdline, control_evtchn) != 0 )
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

    u32           dom;
    unsigned long mcuadv, warp, warpl, warpu;

    static char *kwd_list[] = { "dom", "mcuadv", "warp", "warpl",
                                "warpu", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "illll", kwd_list,
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
    u32 dom;
    unsigned long mcuadv, warp, warpl, warpu;
    
    static char *kwd_list[] = { "dom", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &dom) )
        return NULL;
    
    if ( xc_bvtsched_domain_get(xc->xc_handle, dom, &mcuadv, &warp,
                                &warpl, &warpu) != 0 )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:l,s:l,s:l,s:l}",
                         "domain", dom,
                         "mcuadv", mcuadv,
                         "warp",   warp,
                         "warpl",  warpl,
                         "warpu",  warpu);
}

static PyObject *pyxc_evtchn_bind_interdomain(PyObject *self,
                                              PyObject *args,
                                              PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;

    u32 dom1 = DOMID_SELF, dom2 = DOMID_SELF;
    int port1, port2;

    static char *kwd_list[] = { "dom1", "dom2", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "|ii", kwd_list, 
                                      &dom1, &dom2) )
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
    u32 domid;
    u64 period, slice, latency;
    int xtratime;

    static char *kwd_list[] = { "dom", "period", "slice", "latency",
                                "xtratime", NULL };
    
    if( !PyArg_ParseTupleAndKeywords(args, kwds, "iLLLi", kwd_list, &domid,
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
    u32 domid;
    u64 period, slice, latency;
    int xtratime;
    
    static char *kwd_list[] = { "dom", NULL };

    if( !PyArg_ParseTupleAndKeywords(args, kwds, "i", kwd_list, &domid) )
        return NULL;
    
    if ( xc_atropos_domain_get( xc->xc_handle, domid, &period,
                                &slice, &latency, &xtratime ) )
        return PyErr_SetFromErrno(xc_error);

    return Py_BuildValue("{s:i,s:L,s:L,s:L,s:i}",
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
    
    return Py_BuildValue("{s:L}", "slice", slice);
}

static PyObject *pyxc_domain_setname(PyObject *self,
                                     PyObject *args,
                                     PyObject *kwds)
{
    XcObject *xc = (XcObject *)self;
    u32 dom;
    char *name;

    static char *kwd_list[] = { "dom", "name", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "is", kwd_list, 
                                      &dom, &name) )
        return NULL;

    if ( xc_domain_setname(xc->xc_handle, dom, name) != 0 )
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
    unsigned long max_memkb;

    static char *kwd_list[] = { "dom", "max_memkb", NULL };

    if ( !PyArg_ParseTupleAndKeywords(args, kwds, "ii", kwd_list, 
                                      &dom, &max_memkb) )
        return NULL;

    if ( xc_domain_setmaxmem(xc->xc_handle, dom, max_memkb) != 0 )
        return PyErr_SetFromErrno(xc_error);
    
    Py_INCREF(zero);
    return zero;
}


static PyMethodDef pyxc_methods[] = {
    { "domain_create", 
      (PyCFunction)pyxc_domain_create, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Create a new domain.\n"
      " mem_kb [int, 0]:        Memory allocation, in kilobytes.\n"
      " name   [str, '(anon)']: Informative textual name.\n\n"
      "Returns: [int] new domain identifier; -1 on error.\n" },

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
      "Pin a domain to a specified CPU.\n"
      " dom [int]:     Identifier of domain to be pinned.\n"
      " cpu [int, -1]: CPU to pin to, or -1 to unpin\n\n"
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
      " dying    [int]:  Bool - is the domain dying?\n"
      " crashed  [int]:  Bool - has the domain crashed?\n"
      " shutdown [int]:  Bool - has the domain shut itself down?\n"
      " paused   [int]:  Bool - is the domain paused by control software?\n"
      " blocked  [int]:  Bool - is the domain blocked waiting for an event?\n"
      " running  [int]:  Bool - is the domain currently running?\n"
      " mem_kb   [int]:  Memory reservation, in kilobytes\n"
      " cpu_time [long]: CPU time consumed, in nanoseconds\n"
      " name     [str]:  Identifying name\n"
      " shutdown_reason [int]: Numeric code from guest OS, explaining "
      "reason why it shut itself down.\n" },

    { "linux_save", 
      (PyCFunction)pyxc_linux_save, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Save the CPU and memory state of a Linux guest OS.\n"
      " dom        [int]:    Identifier of domain to be saved.\n"
      " state_file [str]:    Name of state file. Must not currently exist.\n"
      " progress   [int, 1]: Bool - display a running progress indication?\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "linux_restore", 
      (PyCFunction)pyxc_linux_restore, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Restore the CPU and memory state of a Linux guest OS.\n"
      " state_file [str]:    Name of state file. Must not currently exist.\n"
      " progress   [int, 1]: Bool - display a running progress indication?\n\n"
      "Returns: [int] new domain identifier on success; -1 on error.\n" },

    { "linux_build", 
      (PyCFunction)pyxc_linux_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new Linux guest OS.\n"
      " dom     [int]:      Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
      " ramdisk [str, n/a]: Name of ramdisk file, if any.\n"
      " cmdline [str, n/a]: Kernel parameters, if any.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "netbsd_build", 
      (PyCFunction)pyxc_netbsd_build, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Build a new NetBSD guest OS.\n"
      " dom     [int]:     Identifier of domain to build into.\n"
      " image   [str]:      Name of kernel image file. May be gzipped.\n"
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
      " dom    [int]: Identifier of domain to be tuned.\n"
      " mcuadv [int]: Proportional to the inverse of the domain's weight.\n"
      " warp   [int]: How far to warp domain's EVT on unblock.\n"
      " warpl  [int]: How long the domain can run warped.\n"
      " warpu  [int]: How long before the domain can warp again.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

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

    { "atropos_domain_set",
      (PyCFunction)pyxc_atropos_domain_set,
      METH_KEYWORDS, "\n"
      "Set the scheduling parameters for a domain when running with Atropos.\n"
      " dom      [int]:  domain to set\n"
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
      " dom      [int]: domain to query\n"
      "Returns:  [dict]\n"
      " domain   [int]: domain ID\n"
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
      "Close an event channel.\n"
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

    { "shadow_control", 
      (PyCFunction)pyxc_shadow_control, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set parameter for shadow pagetable interface\n"
      " dom [int]:   Identifier of domain.\n"
      " op [int, 0]: operation\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_setname", 
      (PyCFunction)pyxc_domain_setname, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set domain informative textual name\n"
      " dom [int]:  Identifier of domain.\n"
      " name [str]: Text string.\n\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

    { "domain_setmaxmem", 
      (PyCFunction)pyxc_domain_setname, 
      METH_VARARGS | METH_KEYWORDS, "\n"
      "Set a domain's memory limit\n"
      " dom [int]: Identifier of domain.\n"
      " max_memkb [long]: .\n"
      "Returns: [int] 0 on success; -1 on error.\n" },

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
