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
# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>
# Copyright (C) 2004 Intel Research Cambridge
# Copyright (C) 2004 Mark Williamson <mark.williamson@cl.cam.ac.uk>
# Copyright (C) 2005 XenSource Ltd
#============================================================================


"""Support for virtual USB hubs.
"""

from xen.xend.server.DevController import DevController


next_devid = 1


class UsbifController(DevController):
    """USB device interface controller. Handles all USB devices
    for a domain.
    """
    
    def __init__(self, vm):
        """Create a USB device controller.
        """
        DevController.__init__(self, vm)


    def getDeviceDetails(self, _):
        """@see DevController.getDeviceDetails"""

        global next_devid

        devid = next_devid
        next_devid += 1

        return (devid, {}, {})
