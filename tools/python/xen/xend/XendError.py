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

from xmlrpclib import Fault

import XendClient

class XendInvalidDomain(Fault):
    def __init__(self, value):
        Fault.__init__(self, XendClient.ERROR_INVALID_DOMAIN, value)

class XendError(Fault):
    
    def __init__(self, value):
        Fault.__init__(self, XendClient.ERROR_GENERIC, value)
        self.value = value

    def __str__(self):
        return self.value

class VmError(XendError):
    """Vm construction error."""
    pass


XEND_ERROR_AUTHENTICATION_FAILED = ('ELUSER', 'Authentication Failed')
XEND_ERROR_SESSION_INVALID       = ('EPERMDENIED', 'Session Invalid')
XEND_ERROR_DOMAIN_INVALID        = ('EINVALIDDOMAIN', 'Domain Invalid')
XEND_ERROR_HOST_INVALID          = ('EINVALIDHOST', 'Host Invalid')
XEND_ERROR_HOST_RUNNING          = ('EHOSTRUNNING', 'Host is still Running')
XEND_ERROR_HOST_CPU_INVALID      = ('EHOSTCPUINVALID', 'Host CPU Invalid')
XEND_ERROR_UNSUPPORTED           = ('EUNSUPPORTED', 'Method Unsupported')
XEND_ERROR_VM_INVALID            = ('EVMINVALID', 'VM Invalid')
XEND_ERROR_VBD_INVALID           = ('EVBDINVALID', 'VBD Invalid')
XEND_ERROR_VIF_INVALID           = ('EVIFINVALID', 'VIF Invalid')
XEND_ERROR_VTPM_INVALID          = ('EVTPMINVALID', 'VTPM Invalid')
XEND_ERROR_VDI_INVALID           = ('EVDIINVALID', 'VDI Invalid')
XEND_ERROR_SR_INVALID           = ('ESRINVALID', 'SR Invalid')
XEND_ERROR_TODO                  = ('ETODO', 'Lazy Programmer Error')
