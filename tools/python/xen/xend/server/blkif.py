# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>
"""Support for virtual block devices.
"""
import string

from xen.util import blkif
from xen.xend.XendError import XendError, VmError
from xen.xend.XendRoot import get_component
from xen.xend.XendLogging import log
from xen.xend import sxp
from xen.xend import Blkctl
from xen.xend.xenstore import DBVar

from xen.xend.server import channel
from xen.xend.server.controller import CtrlMsgRcvr, Dev, DevController
from xen.xend.server.messages import *

class BlkifBackend:
    """ Handler for the 'back-end' channel to a block device driver domain
    on behalf of a front-end domain.
    Must be connected using connect() before it can be used.
    """

    def __init__(self, controller, id, dom, recreate=False):
        self.controller = controller
        self.id = id
        self.frontendDomain = self.controller.getDomain()
        self.frontendChannel = None
        self.backendDomain = dom
        self.backendChannel = None
        self.destroyed = False
        self.connected = False
        self.evtchn = None
        self.status = None

    def init(self, recreate=False, reboot=False):
        self.destroyed = False
        self.status = BLKIF_INTERFACE_STATUS_DISCONNECTED
        self.frontendDomain = self.controller.getDomain()
        self.frontendChannel = self.controller.getChannel()
        cf = channel.channelFactory()
        self.backendChannel = cf.openChannel(self.backendDomain)

    def __str__(self):
        return ('<BlkifBackend frontend=%d backend=%d id=%d>'
                % (self.frontendDomain,
                   self.backendDomain,
                   self.id))

    def getId(self):
        return self.id

    def getEvtchn(self):
        return self.evtchn

    def closeEvtchn(self):
        if self.evtchn:
            channel.eventChannelClose(self.evtchn)
            self.evtchn = None

    def openEvtchn(self):
        self.evtchn = channel.eventChannel(self.backendDomain, self.frontendDomain)

    def getEventChannelBackend(self):
        val = 0
        if self.evtchn:
            val = self.evtchn['port1']
        return val

    def getEventChannelFrontend(self):
        val = 0
        if self.evtchn:
            val = self.evtchn['port2']
        return val

    def connect(self, recreate=False):
        """Connect to the blkif control interface.

        @param recreate: true if after xend restart
        """
        log.debug("Connecting blkif %s", str(self))
        if recreate or self.connected:
            self.connected = True
            pass
        else:
            self.send_be_create()
        
    def send_be_create(self):
        log.debug("send_be_create %s", str(self))
        msg = packMsg('blkif_be_create_t',
                      { 'domid'        : self.frontendDomain,
                        'blkif_handle' : self.id })
        msg = self.backendChannel.requestResponse(msg)
        #todo: check return status
        self.connected = True

    def destroy(self, change=False, reboot=False):
        """Disconnect from the blkif control interface and destroy it.
        """
        self.send_be_disconnect()
        self.send_be_destroy()
        self.closeEvtchn()
        self.destroyed = True
        # For change true need to notify front-end, or back-end will do it?

    def send_be_disconnect(self):
        msg = packMsg('blkif_be_disconnect_t',
                      { 'domid'        : self.frontendDomain,
                        'blkif_handle' : self.id })
        self.backendChannel.requestResponse(msg)
        #todo: check return status
        self.connected = False

    def send_be_destroy(self):
        msg = packMsg('blkif_be_destroy_t',
                      { 'domid'        : self.frontendDomain,
                        'blkif_handle' : self.id })
        self.backendChannel.requestResponse(msg)
        #todo: check return status

    def connectInterface(self, val):
        self.openEvtchn()
        log.debug("Connecting blkif to event channel %s ports=%d:%d",
                  str(self), self.evtchn['port1'], self.evtchn['port2'])
        msg = packMsg('blkif_be_connect_t',
                      { 'domid'        : self.frontendDomain,
                        'blkif_handle' : self.id,
                        'evtchn'       : self.getEventChannelBackend(),
                        'shmem_frame'  : val['shmem_frame'] })
        msg = self.backendChannel.requestResponse(msg)
        #todo: check return status
        val = unpackMsg('blkif_be_connect_t', msg)
        self.status = BLKIF_INTERFACE_STATUS_CONNECTED
        self.send_fe_interface_status()
            
    def send_fe_interface_status(self):
        msg = packMsg('blkif_fe_interface_status_t',
                      { 'handle' : self.id,
                        'status' : self.status,
                        'domid'  : self.backendDomain,
                        'evtchn' : self.getEventChannelFrontend() })
        self.frontendChannel.writeRequest(msg)

    def interfaceDisconnected(self):
        self.status = BLKIF_INTERFACE_STATUS_DISCONNECTED
        #todo?: Close evtchn:
        #self.closeEvtchn()
        self.send_fe_interface_status()
        
    def interfaceChanged(self):
        """Notify the front-end that devices have been added or removed.
        The front-end should then probe for devices.
        """
        msg = packMsg('blkif_fe_interface_status_t',
                      { 'handle' : self.id,
                        'status' : BLKIF_INTERFACE_STATUS_CHANGED,
                        'domid'  : self.backendDomain,
                        'evtchn' : 0 })
        self.frontendChannel.writeRequest(msg)

class BlkDev(Dev):
    """Info record for a block device.
    """

    __exports__ = Dev.__exports__ + [
        DBVar('dev',          ty='str'),
        DBVar('vdev',         ty='int'),
        DBVar('mode',         ty='str'),
        DBVar('viftype',      ty='str'),
        DBVar('params',       ty='str'),
        DBVar('node',         ty='str'),
        DBVar('device',       ty='long'),
        DBVar('dev_handle',   ty='long'),
        DBVar('start_sector', ty='long'),
        DBVar('nr_sectors',   ty='long'),
        ]

    def __init__(self, controller, id, config, recreate=False):
        Dev.__init__(self, controller, id, config, recreate=recreate)
        self.dev = None
        self.uname = None
        self.vdev = None
        self.mode = None
        self.type = None
        self.params = None
        self.node = None
        self.device = None
        self.dev_handle = 0
        self.start_sector = None
        self.nr_sectors = None
        
        self.frontendDomain = self.getDomain()
        self.frontendChannel = None
        self.backendDomain = None
        self.backendChannel = None
        self.backendId = 0
        self.configure(self.config, recreate=recreate)

    def exportToDB(self, save=False):
        Dev.exportToDB(self, save=save)
        backend = self.getBackend()
        if backend and backend.evtchn:
            db = self.db.addChild("evtchn")
            backend.evtchn.saveToDB(db, save=save)

    def init(self, recreate=False, reboot=False):
        self.frontendDomain = self.getDomain()
        self.frontendChannel = self.getChannel()
        backend = self.getBackend()
        self.backendChannel = backend.backendChannel
        self.backendId = backend.id

    def configure(self, config, change=False, recreate=False):
        if change:
            raise XendError("cannot reconfigure vbd")
        self.config = config
        self.uname = sxp.child_value(config, 'uname')
        if not self.uname:
            raise VmError('vbd: Missing uname')
        # Split into type and type-specific params (which are passed to the
        # type-specific control script).
        (self.type, self.params) = string.split(self.uname, ':', 1)
        self.dev = sxp.child_value(config, 'dev')
        if not self.dev:
            raise VmError('vbd: Missing dev')
        self.mode = sxp.child_value(config, 'mode', 'r')
        
        self.vdev = blkif.blkdev_name_to_number(self.dev)
        if not self.vdev:
            raise VmError('vbd: Device not found: %s' % self.dev)
        
        try:
            xd = get_component('xen.xend.XendDomain')
            self.backendDomain = xd.domain_lookup_by_name(sxp.child_value(config, 'backend', '0')).id
        except:
            raise XendError('invalid backend domain')

        return self.config

    def attach(self, recreate=False, change=False):
        if recreate:
            pass
        else:
            node = Blkctl.block('bind', self.type, self.params)
            self.setNode(node)
            self.attachBackend()
        if change:
            self.interfaceChanged()

    def unbind(self):
        if self.node is None: return
        log.debug("Unbinding vbd (type %s) from %s"
                  % (self.type, self.node))
        Blkctl.block('unbind', self.type, self.node)

    def setNode(self, node):
    
        # NOTE: 
        # This clause is testing code for storage system experiments.
        # Add a new disk type that will just pass an opaque id in the
        # dev_handle and use an experimental device type.
        # Please contact andrew.warfield@cl.cam.ac.uk with any concerns.
        if self.type == 'parallax':
            self.node   = node
            self.device =  61440 # (240,0)
            self.dev_handle = long(self.params)
            self.nr_sectors = long(0)
            return
        # done.
            
        mounted_mode = self.check_mounted(node)
        if not '!' in self.mode and mounted_mode:
            if mounted_mode == "w":
                raise VmError("vbd: Segment %s is in writable use" %
                              self.uname)
            elif 'w' in self.mode:
                raise VmError("vbd: Segment %s is in read-only use" %
                              self.uname)
            
        segment = blkif.blkdev_segment(node)
        if not segment:
            raise VmError("vbd: Segment not found: uname=%s" % self.uname)
        self.node = node
        self.device = segment['device']
        self.start_sector = segment['start_sector']
        self.nr_sectors = segment['nr_sectors']

    def check_mounted(self, name):
        mode = blkif.mount_mode(name)
        xd = get_component('xen.xend.XendDomain')
        for vm in xd.list():
            ctrl = vm.getDeviceController(self.getType(), error=False)
            if (not ctrl): continue
            for dev in ctrl.getDevices():
                if dev is self: continue
                if dev.type == 'phy' and name == blkif.expand_dev_name(dev.params):
                    mode = dev.mode
                    if 'w' in mode:
                        return 'w'
        if mode and 'r' in mode:
            return 'r'
        return None

    def readonly(self):
        return 'w' not in self.mode

    def sxpr(self):
        val = ['vbd',
               ['id', self.id],
               ['vdev', self.vdev],
               ['device', self.device],
               ['mode', self.mode]]
        if self.dev:
            val.append(['dev', self.dev])
        if self.uname:
            val.append(['uname', self.uname])
        if self.node:
            val.append(['node', self.node])
        return val

    def getBackend(self):
        return self.controller.getBackend(self.backendDomain)

    def refresh(self):
        log.debug("Refreshing vbd domain=%d id=%s", self.frontendDomain,
                  self.id)
        self.interfaceChanged()

    def destroy(self, change=False, reboot=False):
        """Destroy the device. If 'change' is true notify the front-end interface.

        @param change: change flag
        """
        self.destroyed = True
        log.debug("Destroying vbd domain=%d id=%s", self.frontendDomain,
                  self.id)
        self.send_be_vbd_destroy()
        if change:
            self.interfaceChanged()
        self.unbind()

    def interfaceChanged(self):
        """Tell the back-end to notify the front-end that a device has been
        added or removed.
        """
        self.getBackend().interfaceChanged()

    def attachBackend(self):
        """Attach the device to its controller.

        """
        self.getBackend().connect()
        self.send_be_vbd_create()
        
    def send_be_vbd_create(self):
        msg = packMsg('blkif_be_vbd_create_t',
                      { 'domid'        : self.frontendDomain,
                        'blkif_handle' : self.backendId,
                        'pdevice'      : self.device,
                        'dev_handle'   : self.dev_handle,
                        'vdevice'      : self.vdev,
                        'readonly'     : self.readonly() })
        msg = self.backendChannel.requestResponse(msg)
        
        val = unpackMsg('blkif_be_vbd_create_t', msg)
        status = val['status']
        if status != BLKIF_BE_STATUS_OKAY:
            raise XendError("Creating vbd failed: device %s, error %d"
                            % (sxp.to_string(self.config), status))

    def send_be_vbd_destroy(self):
        msg = packMsg('blkif_be_vbd_destroy_t',
                      { 'domid'                : self.frontendDomain,
                        'blkif_handle'         : self.backendId,
                        'vdevice'              : self.vdev })
        return self.backendChannel.writeRequest(msg)
        
class BlkifController(DevController):
    """Block device interface controller. Handles all block devices
    for a domain.
    """
    
    def __init__(self, vm, recreate=False):
        """Create a block device controller.
        """
        DevController.__init__(self, vm, recreate=recreate)
        self.backends = {}
        self.backendId = 0
        self.rcvr = None

    def initController(self, recreate=False, reboot=False):
        self.destroyed = False
        # Add our handlers for incoming requests.
        self.rcvr = CtrlMsgRcvr(self.getChannel())
        self.rcvr.addHandler(CMSG_BLKIF_FE,
                             CMSG_BLKIF_FE_DRIVER_STATUS,
                             self.recv_fe_driver_status)
        self.rcvr.addHandler(CMSG_BLKIF_FE,
                             CMSG_BLKIF_FE_INTERFACE_CONNECT,
                             self.recv_fe_interface_connect)
        self.rcvr.registerChannel()
        if reboot:
            self.rebootBackends()
            self.rebootDevices()

    def sxpr(self):
        val = ['blkif', ['dom', self.getDomain()]]
        return val

    def rebootBackends(self):
        for backend in self.backends.values():
            backend.init(reboot=True)

    def getBackendById(self, id):
        return self.backends.get(id)

    def getBackendByDomain(self, dom):
        for backend in self.backends.values():
            if backend.backendDomain == dom:
                return backend
        return None

    def getBackend(self, dom):
        backend = self.getBackendByDomain(dom)
        if backend: return backend
        backend = BlkifBackend(self, self.backendId, dom)
        self.backendId += 1
        self.backends[backend.getId()] = backend
        backend.init()
        return backend

    def newDevice(self, id, config, recreate=False):
        """Create a device..

        @param id:      device id
        @param config:   device configuration
        @param recreate: if true it's being recreated (after xend restart)
        @type  recreate: bool
        @return: device
        @rtype:  BlkDev
        """
        return BlkDev(self, id, config, recreate=recreate)
        
    def destroyController(self, reboot=False):
        """Destroy the controller and all devices.
        """
        self.destroyed = True
        log.debug("Destroying blkif domain=%d", self.getDomain())
        self.destroyDevices(reboot=reboot)
        self.destroyBackends(reboot=reboot)
        self.rcvr.deregisterChannel()

    def destroyBackends(self, reboot=False):
        for backend in self.backends.values():
            backend.destroy(reboot=reboot)

    def recv_fe_driver_status(self, msg):
        val = unpackMsg('blkif_fe_driver_status_t', msg)
        for backend in self.backends.values():
            backend.interfaceDisconnected()

    def recv_fe_interface_connect(self, msg):
        val = unpackMsg('blkif_fe_interface_connect_t', msg)
        id = val['handle']
        backend = self.getBackendById(id)
        if backend:
            try:
                backend.connectInterface(val)
            except IOError, ex:
                log.error("Exception connecting backend: %s", ex)
        else:
            log.error('interface connect on unknown interface: id=%d', id)

