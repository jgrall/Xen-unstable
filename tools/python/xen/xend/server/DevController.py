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
# Copyright (C) 2005 XenSource Ltd
#============================================================================


from xen.xend import sxp
from xen.xend.XendError import VmError
from xen.xend.XendLogging import log
from xen.xend.xenstore.xstransact import xstransact


class DevController:
    """Abstract base class for a device controller.  Device controllers create
    appropriate entries in the store to trigger the creation, reconfiguration,
    and destruction of devices in guest domains.  Each subclass of
    DevController is responsible for a particular device-class, and
    understands the details of configuration specific to that device-class.

    DevController itself provides the functionality common to all device
    creation tasks, as well as providing an interface to XendDomainInfo for
    triggering those events themselves.
    """

    # Set when registered.
    deviceClass = None


    ## public:

    def __init__(self, vm):
        self.vm = vm


    def createDevice(self, config):
        """Trigger the creation of a device with the given configuration.

        @return The ID for the newly created device.
        """
        (devid, back, front) = self.getDeviceDetails(config)

        self.writeDetails(config, devid, back, front)

        return devid


    def reconfigureDevice(self, devid, config):
        """Reconfigure the specified device.

        The implementation here just raises VmError.  This may be overridden
        by those subclasses that can reconfigure their devices.
        """
        raise VmError('%s devices may not be reconfigured' % self.deviceClass)


    def destroyDevice(self, devid):
        """Destroy the specified device.

        The implementation here simply deletes the appropriate paths from
        the store.  This may be overridden by subclasses who need to perform
        other tasks on destruction.
        """

        frontpath = self.frontendPath(devid)
        backpath = xstransact.Read(frontpath, "backend")

        xstransact.Remove(frontpath)

        if backpath:
            xstransact.Remove(backpath)
        else:
            raise VmError("Device not connected")
           

    def configurations(self):
        return map(lambda x: self.configuration(int(x)),
                   xstransact.List(self.frontendRoot()))


    def configuration(self, devid):
        """@return an s-expression giving the current configuration of the
        specified device.  This would be suitable for giving to {@link
        #createDevice} in order to recreate that device."""

        backdomid = int(xstransact.Read(self.frontendPath(devid),
                                        "backend-id"))

        return [self.deviceClass, ['backend', backdomid]]


    def sxprs(self):
        """@return an s-expression describing all the devices of this
        controller's device-class.
        """
        return xstransact.ListRecursive(self.frontendRoot())


    def sxpr(self, devid):
        """@return an s-expression describing the specified device.
        """
        return [self.deviceClass, ['dom', self.vm.getDomid(),
                                   'id', devid]]


    ## protected:

    def getDeviceDetails(self, config):
        """Compute the details for creation of a device corresponding to the
        given configuration.  These details consist of a tuple of (devID,
        backDetails, frontDetails), where devID is the ID for the new device,
        and backDetails and frontDetails are the device configuration
        specifics for the backend and frontend respectively.

        backDetails and frontDetails should be dictionaries, the keys and
        values of which will be used as paths in the store.  There is no need
        for these dictionaries to include the references from frontend to
        backend, nor vice versa, as these will be handled by DevController.

        Abstract; must be implemented by every subclass.

        @return (devID, backDetails, frontDetails), as specified above.
        """

        raise NotImplementedError()


    def getDomid(self):
        """Stub to {@link XendDomainInfo.getDomid}, for use by our
        subclasses.
        """
        return self.vm.getDomid()


    def allocateDeviceID(self):
        """Allocate a device ID, allocating them consecutively on a
        per-domain, per-device-class basis, and using the store to record the
        next available ID.

        This method is available to our subclasses, though it is not
        compulsory to use it; subclasses may prefer to allocate IDs based upon
        the device configuration instead.
        """
        path = self.frontendMiscPath()
        while True:
            t = xstransact(path)
            try:
                result = t.read("nextDeviceID")
                if result:
                    result = int(result)
                else:
                    result = 1
                t.write("nextDeviceID", str(result + 1))
                if t.commit():
                    return result
            except:
                t.abort()
                raise


    def readBackend(self, devid, *args):
        frontpath = self.frontendPath(devid)
        backpath = xstransact.Read(frontpath, "backend")
        return xstransact.Read(backpath, *args)


    ## private:

    def writeDetails(self, config, devid, backDetails, frontDetails):
        """Write the details in the store to trigger creation of a device.
        The backend domain ID is taken from the given config, paths for
        frontend and backend are computed, and these are written to the store
        appropriately, including references from frontend to backend and vice
        versa.

        @param config The configuration of the device, as given to
        {@link #createDevice}.
        @param devid        As returned by {@link #getDeviceDetails}.
        @param backDetails  As returned by {@link #getDeviceDetails}.
        @param frontDetails As returned by {@link #getDeviceDetails}.
        """

        import xen.xend.XendDomain
        xd = xen.xend.XendDomain.instance()

        backdom_name = sxp.child_value(config, 'backend')
        if backdom_name:
            backdom = xd.domain_lookup_by_name_or_id_nr(backdom_name)
        else:
            backdom = xd.privilegedDomain()

        if not backdom:
            raise VmError("Cannot configure device for unknown backend %s" %
                          backdom_name)

        frontpath = self.frontendPath(devid)
        backpath  = self.backendPath(backdom, devid)
        
        frontDetails.update({
            'backend' : backpath,
            'backend-id' : "%i" % backdom.getDomid()
            })


        backDetails.update({
            'domain' : self.vm.getName(),
            'frontend' : frontpath,
            'frontend-id' : "%i" % self.vm.getDomid()
            })

        log.debug('DevController: writing %s to %s.', str(frontDetails),
                  frontpath)
        log.debug('DevController: writing %s to %s.', str(backDetails),
                  backpath)

        xstransact.Write(frontpath, frontDetails)
        xstransact.Write(backpath, backDetails)


    def backendPath(self, backdom, devid):
        """@param backdom [XendDomainInfo] The backend domain info."""

        return "%s/backend/%s/%s/%d" % (backdom.getDomainPath(),
                                        self.deviceClass,
                                        self.vm.getDomid(), devid)


    def frontendPath(self, devid):
        return "%s/%d" % (self.frontendRoot(), devid)


    def frontendRoot(self):
        return "%s/device/%s" % (self.vm.getDomainPath(), self.deviceClass)


    def frontendMiscPath(self):
        return "%s/device-misc/%s" % (self.vm.getDomainPath(),
                                      self.deviceClass)
