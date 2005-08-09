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
# Copyright (C) 2005 Mike Wray <mike.wray@hp.com>
#============================================================================

class Factory:
    """Generic protocol factory.
    """

    starts = 0

    def __init__(self):
        pass

    def doStart(self):
        if self.starts == 0:
            self.startFactory()
        self.starts += 1

    def doStop(self):
        if self.starts > 0:
            self.starts -= 1
        else:
            return
        if self.starts == 0:
            self.stopFactory()

    def buildProtocol(self, addr):
        return Protocol(self)

    def startFactory(self):
        pass

    def stopFactory(self):
        pass

class ServerFactory(Factory):
    """Factory for server protocols.
    """
    pass
    
class ClientFactory(Factory):
    """Factory for client protocols.
    """
    
    def startedConnecting(self, connector):
        pass

    def clientConnectionLost(self, connector, reason):
        pass

    def clientConnectionFailed(self, connector, reason):
        pass


class Protocol:

    factory = None
    transport = None
    connected = False

    def __init__(self, factory):
        self.factory = factory

    def setTransport(self, transport):
        self.transport = transport
        self.connected = bool(transport)

    def getTransport(self):
        return self.transport

    def connectionMade(self, addr):
        print 'Protocol>connectionMade>', addr
        pass

    def connectionLost(self, reason=None):
        print 'Protocol>connectionLost>', reason
        pass

    def dataReceived(self, data):
        print 'Protocol>dataReceived>'
        pass

    def write(self, data):
        if self.transport:
            return self.transport.write(data)
        else:
            return 0

    def read(self):
        if self.transport:
            return self.transport.read()
        else:
            return None

class TestClientFactory(ClientFactory):

    def buildProtocol(self, addr):
        print 'TestClientFactory>buildProtocol>', addr
        return TestClientProtocol(self)
    
    def startedConnecting(self, connector):
        print 'TestClientFactory>startedConnecting>', connector

    def clientConnectionLost(self, connector, reason):
        print 'TestClientFactory>clientConnectionLost>', connector, reason

    def clientConnectionFailed(self, connector, reason):
        print 'TestClientFactory>clientConnectionFailed>', connector, reason

class TestClientProtocol(Protocol):

    def connectionMade(self, addr):
        print 'TestClientProtocol>connectionMade>', addr
        self.write("hello")
        self.write("there")

class TestServerFactory(Factory):

    def buildProtocol(self, addr):
        print 'TestServerFactory>buildProtocol>', addr
        return TestServerProtocol(self)
    
class TestServerProtocol(Protocol):

    def dataReceived(self, data):
        print 'TestServerProtocol>dataReceived>', len(data), data
        #sys.exit(0)
        import os
        os._exit(0)
        
