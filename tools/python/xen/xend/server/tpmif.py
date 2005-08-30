# Copyright (C) 2005 IBM Corporation
#   Authort: Stefan Berger, stefanb@us.ibm.com
# Derived from netif.py:
# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>
"""Support for virtual TPM interfaces.
"""

import random

from xen.xend import sxp
from xen.xend.XendError import XendError, VmError
from xen.xend.XendLogging import log
from xen.xend.XendRoot import get_component
from xen.xend.xenstore import DBVar

from xen.xend.server import channel
from xen.xend.server.controller import CtrlMsgRcvr, Dev, DevController
from xen.xend.server.messages import *

class TPMifController(DevController):
    """TPM interface controller. Handles all TPM devices for a domain.
    """

    def __init__(self, vm, recreate=False):
        DevController.__init__(self, vm, recreate=recreate)
        self.rcvr = None
        self.channel = None

    def initController(self, recreate=False, reboot=False):
        self.destroyed = False
        self.channel = self.getChannel()

    def destroyController(self, reboot=False):
        """Destroy the controller and all devices.
        """
        self.destroyed = True
        self.destroyDevices(reboot=reboot)
        if self.rcvr:
            self.rcvr.deregisterChannel()

    def sxpr(self):
        val = ['tpmif', ['dom', self.getDomain()]]
        return val

    def newDevice(self, id, config, recreate=False):
        """Create a TPM device.

        @param id: interface id
        @param config: device configuration
        @param recreate: recreate flag (true after xend restart)
        """
        return None
