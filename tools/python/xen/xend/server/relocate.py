#============================================================================
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#============================================================================
# Copyright (C) 2004, 2005 Mike Wray <mike.wray@hp.com>
#============================================================================

import socket
import sys
import StringIO

from xen.web import reactor, protocol

from xen.xend import scheduler
from xen.xend import sxp
from xen.xend import EventServer; eserver = EventServer.instance()
from xen.xend.XendError import XendError
from xen.xend import XendRoot; xroot = XendRoot.instance()
from xen.xend.XendLogging import log
from xen.xend import XendCheckpoint

DEBUG = 0

class RelocationProtocol(protocol.Protocol):
    """Asynchronous handler for a connected relocation socket.
    """

    def __init__(self):
        #protocol.Protocol.__init__(self)
        self.parser = sxp.Parser()

    def dataReceived(self, data):
        try:
            self.parser.input(data)
            while(self.parser.ready()):
                val = self.parser.get_val()
                res = self.dispatch(val)
                self.send_result(res)
            if self.parser.at_eof():
                self.loseConnection()
        except SystemExit:
            raise
        except:
            self.send_error()

    def loseConnection(self):
        if self.transport:
            self.transport.loseConnection()
        if self.connected:
            scheduler.now(self.connectionLost)

    def connectionLost(self, reason=None):
        pass

    def send_reply(self, sxpr):
        io = StringIO.StringIO()
        sxp.show(sxpr, out=io)
        print >> io
        io.seek(0)
        if self.transport:
            return self.transport.write(io.getvalue())
        else:
            return 0

    def send_result(self, res):
        if res is None:
            resp = ['ok']
        else:
            resp = ['ok', res]
        return self.send_reply(resp)

    def send_error(self):
        (extype, exval) = sys.exc_info()[:2]
        return self.send_reply(['err',
                                ['type', str(extype)],
                                ['value', str(exval)]])

    def opname(self, name):
         return 'op_' + name.replace('.', '_')

    def operror(self, name, req):
        raise XendError('Invalid operation: ' +name)

    def dispatch(self, req):
        op_name = sxp.name(req)
        op_method_name = self.opname(op_name)
        op_method = getattr(self, op_method_name, self.operror)
        return op_method(op_name, req)

    def op_help(self, name, req):
        def nameop(x):
            if x.startswith('op_'):
                return x[3:].replace('_', '.')
            else:
                return x
        
        l = [ nameop(k) for k in dir(self) if k.startswith('op_') ]
        return l

    def op_quit(self, name, req):
        self.loseConnection()

    def op_receive(self, name, req):
        if self.transport:
            self.send_reply(["ready", name])
            self.transport.sock.setblocking(1)
            xd = xroot.get_component("xen.xend.XendDomain")
            XendCheckpoint.restore(xd, self.transport.sock.fileno())
            self.transport.sock.setblocking(0)
        else:
            log.error(name + ": no transport")
            raise XendError(name + ": no transport")

class RelocationFactory(protocol.ServerFactory):
    """Asynchronous handler for the relocation server socket.
    """

    def __init__(self):
        #protocol.ServerFactory.__init__(self)
        pass

    def buildProtocol(self, addr):
        return RelocationProtocol()

def listenRelocation():
    factory = RelocationFactory()
    if xroot.get_xend_unix_server():
        path = '/var/lib/xend/relocation-socket'
        reactor.listenUNIX(path, factory)
    if xroot.get_xend_relocation_server():
        port = xroot.get_xend_relocation_port()
        interface = xroot.get_xend_relocation_address()
        l = reactor.listenTCP(port, factory, interface=interface)
        l.setCloExec()

def setupRelocation(dst, port):
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((dst, port))
    except socket.error, err:
        raise XendError("can't connect: %s" % err[1])

    sock.send("receive\n")
    print sock.recv(80)

    return sock
