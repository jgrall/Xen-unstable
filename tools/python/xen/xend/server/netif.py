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

"""Support for virtual network interfaces.
"""

import random

from xen.util.mac import macFromString, macToString

from xen.xend import sxp
from xen.xend import Vifctl
from xen.xend.XendError import XendError, VmError
from xen.xend.XendLogging import log
from xen.xend import XendVnet
from xen.xend.XendRoot import get_component
from xen.xend.xenstore import DBVar

from xen.xend.server import channel
from xen.xend.server.controller import CtrlMsgRcvr, Dev, DevController
from xen.xend.server.messages import *

class NetDev(Dev):
    """A network device.
    """

    # State:
    # inherited + 
    # ./config
    # ./mac
    # ./be_mac
    # ./bridge
    # ./script
    # ./ipaddr ?
    #
    # ./credit
    # ./period
    #
    # ./vifctl: up/down?
    # ./vifname
    #
    #
    # Poss should have no backend state here - except for ref to backend's own tree
    # for the device? And a status - the one we want.
    # ./back/dom
    # ./back/devid - id for back-end (netif_handle) - same as front/devid
    # ./back/id    - backend id (if more than one b/e per domain)
    # ./back/status
    # ./back/tx_shmem_frame  - actually these belong in back-end state
    # ./back/rx_shmem_frame
    #
    # ./front/dom
    # ./front/devid
    # ./front/status - need 2: one for requested, one for actual? Or drive from dev status
    # and this is front status only.
    # ./front/tx_shmem_frame
    # ./front/rx_shmem_frame
    #
    # ./evtchn/front - here or in front/back?
    # ./evtchn/back
    # ./evtchn/status ?
    # At present created by dev: but should be created unbound by front/back
    # separately and then bound (by back)?

    __exports__ = Dev.__exports__ + [
        DBVar('config',  ty='sxpr'),
        DBVar('mac',     ty='mac'),
        DBVar('be_mac',  ty='mac'),
        DBVar('bridge',  ty='str'),
        DBVar('script',  ty='str'),
        DBVar('credit',  ty='int'),
        DBVar('period',  ty='int'),
        DBVar('vifname', ty='str'),
        ]

    def __init__(self, controller, id, config, recreate=False):
        Dev.__init__(self, controller, id, config, recreate=recreate)
        self.vif = int(self.id)
        self.evtchn = None
        self.status = None
        self.frontendDomain = self.getDomain()
        self.frontendChannel = None
        self.backendDomain = None
        self.backendChannel = None
        self.credit = None
        self.period = None
        self.mac = None
        self.be_mac = None
        self.bridge = None
        self.script = None
        self.ipaddr = None
        self.mtu = None
        self.vifname = None
        self.configure(self.config, recreate=recreate)

    def exportToDB(self, save=False):
        Dev.exportToDB(self, save=save)
        if self.evtchn:
            db = self.db.addChild("evtchn")
            self.evtchn.saveToDB(db, save=save)

    def init(self, recreate=False, reboot=False):
        self.destroyed = False
        self.status = NETIF_INTERFACE_STATUS_DISCONNECTED
        self.frontendDomain = self.getDomain()
        self.frontendChannel = self.getChannel()
        cf = channel.channelFactory()
        self.backendChannel = cf.openChannel(self.backendDomain)

    def _get_config_mac(self, config):
        vmac = sxp.child_value(config, 'mac')
        if not vmac: return None
        try:
            mac = macFromString(vmac)
        except:
            raise XendError("invalid mac: %s" % vmac)
        return mac

    def _get_config_be_mac(self, config):
        vmac = sxp.child_value(config, 'be_mac')
        if not vmac: return None
        try:
            mac = macFromString(vmac)
        except:
            raise XendError("invalid backend mac: %s" % vmac)
        return mac

    def _get_config_ipaddr(self, config):
        ips = sxp.children(config, elt='ip')
        if ips:
            val = []
            for ipaddr in ips:
                val.append(sxp.child0(ipaddr))
        else:
            val = None
        return val

    def _get_config_mtu(self, config):
        mtu = sxp.child_value(config, 'mtu')
        if not mtu: return None
        try:
            mtu = int(mtu)
        except:
            raise XendError("invalid mtu: %s" & mtu)
        return mtu

    def configure(self, config, change=False, recreate=False):
        if change:
            return self.reconfigure(config)
        self.config = config
        self.mac = None
        self.be_mac = None
        self.bridge = None
        self.script = None
        self.ipaddr = []
        self.vifname = None

        self.vifname = sxp.child_value(config, 'vifname')
        if self.vifname is None:
            self.vifname = self.default_vifname()
        if len(self.vifname) > 15:
            raise XendError('invalid vifname: too long: ' + self.vifname)
        mac = self._get_config_mac(config)
        if mac is None:
            raise XendError("invalid mac")
        self.mac = mac
        self.be_mac = self._get_config_be_mac(config)
        self.bridge = sxp.child_value(config, 'bridge')
        self.script = sxp.child_value(config, 'script')
        self.ipaddr = self._get_config_ipaddr(config) or []
        self.mtu = self._get_config_mtu(config)
        self._config_credit_limit(config)
        
        try:
            if recreate:
                self.backendDomain = int(sxp.child_value(config, 'backend', '0'))
            else:
                #todo: Code below will fail on xend restart when backend is not domain 0.
                xd = get_component('xen.xend.XendDomain')
                self.backendDomain = xd.domain_lookup_by_name(sxp.child_value(config, 'backend', '0')).id
        except:
            raise XendError('invalid backend domain')
        return self.config

    def reconfigure(self, config):
        """Reconfigure the interface with new values.
        Not all configuration parameters can be changed:
        bridge, script and ip addresses can,
        backend and mac cannot.

        To leave a parameter unchanged, omit it from the changes.

        @param config configuration changes
        @return updated interface configuration
        @raise XendError on errors
        """
        changes = {}
        mac = self._get_config_mac(config)
        be_mac = self._get_config_be_mac(config)
        bridge = sxp.child_value(config, 'bridge')
        script = sxp.child_value(config, 'script')
        ipaddr = self._get_config_ipaddr(config)
        mtu = self._get_config_mtu(config)
        
        xd = get_component('xen.xend.XendDomain')
        backendDomain = xd.domain_lookup_by_name(sxp.child_value(config, 'backend', '0')).id

        if (mac is not None) and (mac != self.mac):
            raise XendError("cannot change mac")
        if (be_mac is not None) and (be_mac != self.be_mac):
            raise XendError("cannot change backend mac")
        if (backendDomain is not None) and (backendDomain != self.backendDomain):
            raise XendError("cannot change backend")
        if (bridge is not None) and (bridge != self.bridge):
            changes['bridge'] = bridge
        if (script is not None) and (script != self.script):
            changes['script'] = script
        if (ipaddr is not None) and (ipaddr != self.ipaddr):
            changes['ipaddr'] = ipaddr
        if (mtu is not None) and (mtu != self.mtu):
            changes['mtu'] = mtu

        if changes:
            self.vifctl("down")
            for (k, v) in changes.items():
                setattr(self, k, v)
            self.config = sxp.merge(config, self.config)
            self.vifctl("up")

        self._config_credit_limit(config, change=True)
        return self.config

    def _config_credit_limit(self, config, change=False):
        period = sxp.child_value(config, 'period')
        credit = sxp.child_value(config, 'credit')
        if period and credit:
            try:
                period = int(period)
                credit = int(credit)
            except ex:
                raise XendError('vif: invalid credit limit')
            if change:
                self.setCreditLimit(credit, period)
                self.config = sxp.merge([sxp.name(self.config),
                                         ['credit', credit],
                                         ['period', period]],
                                        self.config)
            else:
                self.period = period
                self.credit = credit
        elif period or credit:
            raise XendError('vif: invalid credit limit')

    def sxpr(self):
        vif = str(self.vif)
        mac = self.get_mac()
        val = ['vif',
               ['id', self.id],
               ['vif', vif],
               ['mac', mac],
               ['vifname', self.vifname],
               ]

        if self.be_mac:
            val.append(['be_mac', self.get_be_mac()])
        if self.bridge:
            val.append(['bridge', self.bridge])
        if self.script:
            val.append(['script', self.script])
        for ip in self.ipaddr:
            val.append(['ip', ip])
        if self.credit:
            val.append(['credit', self.credit])
        if self.period:
            val.append(['period', self.period])
        if self.evtchn:
            val.append(['evtchn',
                        self.evtchn['port1'],
                        self.evtchn['port2']])
        return val

    def get_vifname(self):
        """Get the virtual interface device name.
        """
        return self.vifname

    def default_vifname(self):
        return "vif%d.%d" % (self.frontendDomain, self.vif)
    
    def get_mac(self):
        """Get the MAC address as a string.
        """
        return macToString(self.mac)

    def get_be_mac(self):
        """Get the backend MAC address as a string.
        """
        return macToString(self.be_mac)

    def vifctl_params(self, vmname=None):
        """Get the parameters to pass to vifctl.
        """
        dom = self.frontendDomain
        if vmname is None:
            xd = get_component('xen.xend.XendDomain')
            try:
                vm = xd.domain_lookup(dom)
                vmname = vm.name
            except:
                vmname = 'Domain-%d' % dom
        return { 'domain': vmname,
                 'vif'   : self.get_vifname(), 
                 'mac'   : self.get_mac(),
                 'bridge': self.bridge,
                 'script': self.script,
                 'ipaddr': self.ipaddr, }

    def vifctl(self, op, vmname=None):
        """Bring the device up or down.
        The vmname is needed when bringing a device up for a new domain because
        the domain is not yet in the table so we can't look its name up.

        @param op: operation name (up, down)
        @param vmname: vmname
        """
        if op == 'up':
            Vifctl.set_vif_name(self.default_vifname(), self.vifname)
        Vifctl.vifctl(op, **self.vifctl_params(vmname=vmname))
        vnet = XendVnet.instance().vnet_of_bridge(self.bridge)
        if vnet:
            vnet.vifctl(op, self.get_vifname(), self.get_mac())

    def attach(self, recreate=False, change=False):
        if recreate:
            pass
        else:
            self.send_be_create()
            if self.credit and self.period:
                self.send_be_creditlimit(self.credit, self.period)
            self.vifctl('up', vmname=self.getDomainName())
        
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

    def send_be_create(self):
        msg = packMsg('netif_be_create_t',
                      { 'domid'        : self.frontendDomain,
                        'netif_handle' : self.vif,
                        'be_mac'       : self.be_mac or [0, 0, 0, 0, 0, 0],
                        'mac'          : self.mac,
                        #'vifname'      : self.vifname
                        })
        msg = self.backendChannel.requestResponse(msg)
        # todo: check return status

    def destroy(self, change=False, reboot=False):
        """Destroy the device's resources and disconnect from the back-end
        device controller. If 'change' is true notify the front-end interface.

        @param change: change flag
        """
        self.destroyed = True
        self.status = NETIF_INTERFACE_STATUS_CLOSED
        log.debug("Destroying vif domain=%d vif=%d", self.frontendDomain, self.vif)
        self.closeEvtchn()
        self.vifctl('down')
        self.send_be_disconnect()
        self.send_be_destroy()
        if change:
            self.reportStatus()

    def send_be_disconnect(self):
        msg = packMsg('netif_be_disconnect_t',
                      { 'domid'        : self.frontendDomain,
                        'netif_handle' : self.vif })
        self.backendChannel.requestResponse(msg)
        #todo: check return status

    def send_be_destroy(self, response=None):
        msg = packMsg('netif_be_destroy_t',
                      { 'domid'        : self.frontendDomain,
                        'netif_handle' : self.vif })
        self.backendChannel.requestResponse(msg)
        #todo: check return status
    
    def recv_fe_interface_connect(self, val):
        self.openEvtchn()
        msg = packMsg('netif_be_connect_t',
                      { 'domid'          : self.frontendDomain,
                        'netif_handle'   : self.vif,
                        'evtchn'         : self.getEventChannelBackend(),
                        'tx_shmem_frame' : val['tx_shmem_frame'],
                        'rx_shmem_frame' : val['rx_shmem_frame'] })
        msg = self.backendChannel.requestResponse(msg)
        #todo: check return status
        self.status = NETIF_INTERFACE_STATUS_CONNECTED
        self.reportStatus()

    def setCreditLimit(self, credit, period):
        #todo: these params should be in sxpr and vif config.
        self.credit = credit
        self.period = period
        self.send_be_creditlimit(credit, period)

    def getCredit(self):
        return self.credit

    def getPeriod(self):
        return self.period
        
    def send_be_creditlimit(self, credit, period):
        msg = packMsg('netif_be_creditlimit_t',
                      { 'domid'          : self.frontendDomain,
                        'netif_handle'   : self.vif,
                        'credit_bytes'   : credit,
                        'period_usec'    : period })
        msg = self.backendChannel.requestResponse(msg)
        # todo: check return status
        
    def reportStatus(self, resp=False):
        msg = packMsg('netif_fe_interface_status_t',
                      { 'handle' : self.vif,
                        'status' : self.status,
                        'evtchn' : self.getEventChannelFrontend(),
                        'domid'  : self.backendDomain,
                        'mac'    : self.mac })
        if resp:
            self.frontendChannel.writeResponse(msg)
        else:
            self.frontendChannel.writeRequest(msg)

    def interfaceChanged(self):
        """Notify the front-end that a device has been added or removed.
        """
        self.reportStatus()
        
class NetifController(DevController):
    """Network interface controller. Handles all network devices for a domain.
    """
    
    def __init__(self, vm, recreate=False):
        DevController.__init__(self, vm, recreate=recreate)
        self.channel = None
        self.rcvr = None
        self.channel = None

    def initController(self, recreate=False, reboot=False):
        self.destroyed = False
        self.channel = self.getChannel()
        # Register our handlers for incoming requests.
        self.rcvr = CtrlMsgRcvr(self.channel)
        self.rcvr.addHandler(CMSG_NETIF_FE,
                             CMSG_NETIF_FE_DRIVER_STATUS,
                             self.recv_fe_driver_status)
        self.rcvr.addHandler(CMSG_NETIF_FE,
                             CMSG_NETIF_FE_INTERFACE_STATUS,
                             self.recv_fe_interface_status)
        self.rcvr.addHandler(CMSG_NETIF_FE,
                             CMSG_NETIF_FE_INTERFACE_CONNECT,
                             self.recv_fe_interface_connect)
        self.rcvr.registerChannel()
        if reboot:
            self.rebootDevices()

    def destroyController(self, reboot=False):
        """Destroy the controller and all devices.
        """
        self.destroyed = True
        log.debug("Destroying netif domain=%d", self.getDomain())
        self.destroyDevices(reboot=reboot)
        if self.rcvr:
            self.rcvr.deregisterChannel()

    def sxpr(self):
        val = ['netif', ['dom', self.getDomain()]]
        return val
    
    def newDevice(self, id, config, recreate=False):
        """Create a network device.

        @param id: interface id
        @param config: device configuration
        @param recreate: recreate flag (true after xend restart)
        """
        return NetDev(self, id, config, recreate=recreate)

    def limitDevice(self, vif, credit, period):        
        if vif not in self.devices:
            raise XendError('device does not exist for credit limit: vif'
                            + str(self.getDomain()) + '.' + str(vif))
        
        dev = self.devices[vif]
        return dev.setCreditLimit(credit, period)
    
    def recv_fe_driver_status(self, msg):
        msg = packMsg('netif_fe_driver_status_t',
                      { 'status'     : NETIF_DRIVER_STATUS_UP,
                        ## FIXME: max_handle should be max active interface id
                        'max_handle' : self.getDeviceCount()
                        #'max_handle' : self.getMaxDeviceId()
                        })
        # Two ways of doing it:
        # 1) front-end requests driver status, we reply with the interface count,
        #    front-end polls the interfaces,
        #    front-end checks they are all up
        # 2) front-end requests driver status, we reply (with anything),
        #    we notify the interfaces,
        #    we notify driver status up with the count
        #    front-end checks they are all up
        #
        # We really want to use 1), but at the moment the xenU kernel panics
        # in that mode, so we're sticking to 2) for now.
        resp = False
        if resp:
            self.channel.writeResponse(msg)
        else:
            for dev in self.devices.values():
                dev.reportStatus()
            self.channel.writeRequest(msg)
        return resp

    def recv_fe_interface_status(self, msg):
        val = unpackMsg('netif_fe_interface_status_t', msg)
        vif = val['handle']
        dev = self.findDevice(vif)
        if dev:
            dev.reportStatus(resp=True)
        else:
            log.error('Received netif_fe_interface_status for unknown vif: dom=%d vif=%d',
                      self.getDomain(), vif)
            msg = packMsg('netif_fe_interface_status_t',
                          { 'handle' : -1,
                            'status' : NETIF_INTERFACE_STATUS_CLOSED,
                            });
            self.channel.writeResponse(msg)
        return True
            
    def recv_fe_interface_connect(self, msg):
        val = unpackMsg('netif_fe_interface_connect_t', msg)
        vif = val['handle']
        dev = self.getDevice(vif)
        if dev:
            dev.recv_fe_interface_connect(val)
        else:
            log.error('Received netif_fe_interface_connect for unknown vif: dom=%d vif=%d',
                      self.getDomain(), vif)

