import sys
import socket
import os
import os.path

from connection import *
from protocol import *

class UnixServerConnection(SocketServerConnection):
    pass

class UnixListener(SocketListener):

    def __init__(self, path, factory, backlog=None):
        SocketListener.__init__(self, factory, backlog=backlog)
        self.path = path
        
    def createSocket(self):
        pathdir = os.path.dirname(self.path)
        if not os.path.exists(pathdir):
            os.makedirs(pathdir)
        else:
            try:
                os.unlink(self.path)
            except SystemExit:
                raise
            except Exception, ex:
                pass
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(self.path)
        return sock

    def acceptConnection(self, sock, protocol, addr):
        return UnixServerConnection(sock, protocol, self.path, self)

class UnixClientConnection(SocketClientConnection):

    def __init__(self, addr, connector):
        SocketClientConnection.__init__(self, connector)
        self.addr = addr
        
    def createSocket(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        return sock
    
class UnixConnector(SocketConnector):

    def __init__(self, path, factory, timeout=None):
        SocketConnector.__init__(self, factory)
        self.addr = path
        self.timeout = timeout

    def getDestination(self):
        return self.addr

    def connectTransport(self):
        self.transport = UnixClientConnection(self.addr, self)
        self.transport.connect(self.timeout)

def listenUNIX(path, factory, backlog=None):
    l = UnixListener(path, factory, backlog=backlog)
    l.startListening()
    return l

def connectUNIX(path, factory, timeout=None):
    c = UnixConnector(path, factory, timeout=timeout)
    c.connect()
    return c

def main(argv):
    path = "/tmp/test-foo"
    if argv[1] == "client":
        c = connectUNIX(path, TestClientFactory())
        print "client:", c
    else:
        s = listenUNIX(path, TestServeractory())
        print "server:", s

if __name__ == "__main__":
    main(sys.argv)

