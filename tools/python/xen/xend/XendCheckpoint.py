# Copyright (C) 2005 Christian Limpach <Christian.Limpach@cl.cam.ac.uk>
# Copyright (C) 2005 XenSource Ltd

# This file is subject to the terms and conditions of the GNU General
# Public License.  See the file "COPYING" in the main directory of
# this archive for more details.

import os
import re
import select
import string
import sxp
from struct import pack, unpack, calcsize

from xen.util.xpopen import xPopen3

import xen.util.auxbin

import xen.lowlevel.xc

from xen.xend.xenstore.xsutil import IntroduceDomain

from XendError import XendError
from XendLogging import log


SIGNATURE = "LinuxGuestRecord"
XC_SAVE = "xc_save"
XC_RESTORE = "xc_restore"


sizeof_int = calcsize("i")
sizeof_unsigned_long = calcsize("L")


xc = xen.lowlevel.xc.new()


def write_exact(fd, buf, errmsg):
    if os.write(fd, buf) != len(buf):
        raise XendError(errmsg)

def read_exact(fd, size, errmsg):
    buf = os.read(fd, size)
    if len(buf) != size:
        raise XendError(errmsg)
    return buf

def save(fd, dominfo, live):
    write_exact(fd, SIGNATURE, "could not write guest state file: signature")

    config = sxp.to_string(dominfo.sxpr())

    domain_name = dominfo.getName()
    # Rename the domain temporarily, so that we don't get a name clash if this
    # domain is migrating (live or non-live) to the local host.  Doing such a
    # thing is useful for debugging.
    dominfo.setName('migrating-' + domain_name)

    try:
        write_exact(fd, pack("!i", len(config)),
                    "could not write guest state file: config len")
        write_exact(fd, config, "could not write guest state file: config")

        # xc_save takes three customization parameters: maxit, max_f, and
        # flags the last controls whether or not save is 'live', while the
        # first two further customize behaviour when 'live' save is
        # enabled. Passing "0" simply uses the defaults compiled into
        # libxenguest; see the comments and/or code in xc_linux_save() for
        # more information.
        cmd = [xen.util.auxbin.pathTo(XC_SAVE), str(xc.handle()), str(fd),
               str(dominfo.getDomid()), "0", "0", str(int(live)) ]
        log.debug("[xc_save]: %s", string.join(cmd))

        def saveInputHandler(line, tochild):
            log.debug("In saveInputHandler %s", line)
            if line == "suspend":
                log.debug("Suspending %d ...", dominfo.getDomid())
                dominfo.shutdown('suspend')
                dominfo.waitForShutdown()
                log.info("Domain %d suspended.", dominfo.getDomid())
                tochild.write("done\n")
                tochild.flush()

        forkHelper(cmd, fd, saveInputHandler, False)

        dominfo.destroyDomain()

    except Exception, exn:
        log.exception("Save failed on domain %s (%d).", domain_name,
                      dominfo.getDomid())
        try:
            dominfo.setName(domain_name)
        except:
            log.exception("Failed to reset the migrating domain's name")
        raise Exception, exn


def restore(xd, fd):
    signature = read_exact(fd, len(SIGNATURE),
        "not a valid guest state file: signature read")
    if signature != SIGNATURE:
        raise XendError("not a valid guest state file: found '%s'" %
                        signature)

    l = read_exact(fd, sizeof_int,
                   "not a valid guest state file: config size read")
    vmconfig_size = unpack("!i", l)[0]
    vmconfig_buf = read_exact(fd, vmconfig_size,
        "not a valid guest state file: config read")

    p = sxp.Parser()
    p.input(vmconfig_buf)
    if not p.ready:
        raise XendError("not a valid guest state file: config parse")

    vmconfig = p.get_val()

    dominfo = xd.restore_(vmconfig)

    store_port   = dominfo.getStorePort()
    console_port = dominfo.getConsolePort()

    assert store_port
    assert console_port

    try:
        l = read_exact(fd, sizeof_unsigned_long,
                       "not a valid guest state file: pfn count read")
        nr_pfns = unpack("=L", l)[0]   # XXX endianess
        if nr_pfns > 1024*1024:     # XXX
            raise XendError(
                "not a valid guest state file: pfn count out of range")

        cmd = map(str, [xen.util.auxbin.pathTo(XC_RESTORE),
                        xc.handle(), fd, dominfo.getDomid(), nr_pfns,
                        store_port, console_port])
        log.debug("[xc_restore]: %s", string.join(cmd))

        handler = RestoreInputHandler()

        forkHelper(cmd, fd, handler.handler, True)

        if handler.store_mfn is None or handler.console_mfn is None:
            raise XendError('Could not read store/console MFN')

        dominfo.unpause()

        dominfo.completeRestore(handler.store_mfn, handler.console_mfn)

        return dominfo
    except:
        dominfo.destroy()
        raise


class RestoreInputHandler:
    def __init__(self):
        self.store_mfn = None
        self.console_mfn = None


    def handler(self, line, _):
        m = re.match(r"^(store-mfn) (\d+)$", line)
        if m:
            self.store_mfn = int(m.group(2))
        else:
            m = re.match(r"^(console-mfn) (\d+)$", line)
            if m:
                self.console_mfn = int(m.group(2))


def forkHelper(cmd, fd, inputHandler, closeToChild):
    child = xPopen3(cmd, True, -1, [fd, xc.handle()])

    if closeToChild:
        child.tochild.close()

    lasterr = "error unknown"
    try:
        fds = [child.fromchild.fileno(),
               child.childerr.fileno()]
        p = select.poll()
        map(p.register, fds)
        while len(fds) > 0:
            r = p.poll()
            for (fd, event) in r:
                if event & select.POLLIN:
                    if fd == child.childerr.fileno():
                        lasterr = child.childerr.readline().rstrip()
                        log.error('%s', lasterr)
                    else:
                        l = child.fromchild.readline().rstrip()
                        while l:
                            log.debug('%s', l)
                            inputHandler(l, child.tochild)
                            try:
                                l = child.fromchild.readline().rstrip()
                            except:
                                l = None

                if event & select.POLLERR:
                    raise XendError('Error reading from child process for %s',
                                    cmd)

                if event & select.POLLHUP:
                    fds.remove(fd)
                    p.unregister(fd)
    finally:
        child.fromchild.close()
        child.childerr.close()
        if not closeToChild:
            child.tochild.close()

    if child.wait() >> 8 == 127:
        lasterr = "popen failed"
    if child.wait() != 0:
        raise XendError("%s failed: %s" % (string.join(cmd), lasterr))
