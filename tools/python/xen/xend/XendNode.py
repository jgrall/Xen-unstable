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
# Copyright (c) 2006 Xensource Inc.
#============================================================================

import os
import socket
import xen.lowlevel.xc

from xen.util import Brctl

from xen.xend import uuid
from xen.xend.XendError import XendError, NetworkAlreadyConnected
from xen.xend.XendOptions import instance as xendoptions
from xen.xend.XendQCoWStorageRepo import XendQCoWStorageRepo
from xen.xend.XendLocalStorageRepo import XendLocalStorageRepo
from xen.xend.XendLogging import log
from xen.xend.XendPIF import *
from xen.xend.XendNetwork import *
from xen.xend.XendStateStore import XendStateStore
from xen.xend.XendMonitor import XendMonitor

class XendNode:
    """XendNode - Represents a Domain 0 Host."""
    
    def __init__(self):
        """Initalises the state of all host specific objects such as

        * Host
        * Host_CPU
        * PIF
        * Network
        * Storage Repository
        """
        
        self.xc = xen.lowlevel.xc.xc()
        self.state_store = XendStateStore(xendoptions().get_xend_state_path())
        self.monitor = XendMonitor()
        self.monitor.start()

        # load host state from XML file
        saved_host = self.state_store.load_state('host')
        if saved_host and len(saved_host.keys()) == 1:
            self.uuid = saved_host.keys()[0]
            host = saved_host[self.uuid]
            self.name = host.get('name_label', socket.gethostname())
            self.desc = host.get('name_description', '')
            self.cpus = {}
        else:
            self.uuid = uuid.createString()
            self.name = socket.gethostname()
            self.desc = ''
            self.cpus = {}
            
        # load CPU UUIDs
        saved_cpus = self.state_store.load_state('cpu')
        for cpu_uuid, cpu in saved_cpus.items():
            self.cpus[cpu_uuid] = cpu

        # verify we have enough cpus here
        physinfo = self.physinfo_dict()
        cpu_count = physinfo['nr_cpus']
        cpu_features = physinfo['hw_caps']

        # If the number of CPUs don't match, we should just reinitialise 
        # the CPU UUIDs.
        if cpu_count != len(self.cpus):
            self.cpus = {}
            for i in range(cpu_count):
                cpu_uuid = uuid.createString()
                cpu_info = {'uuid': cpu_uuid,
                            'host': self.uuid,
                            'number': i,
                            'features': cpu_features}
                self.cpus[cpu_uuid] = cpu_info

        self.pifs = {}
        self.networks = {}
        self.srs = {}
        
        # initialise networks
        saved_networks = self.state_store.load_state('network')
        if saved_networks:
            for net_uuid, network in saved_networks.items():
                self.network_create(network.get('name_label'),
                                    network.get('name_description', ''),
                                    network.get('default_gateway', ''),
                                    network.get('default_netmask', ''),
                                    False, net_uuid)
        else:
            gateway, netmask = linux_get_default_network()
            self.network_create('net0', '', gateway, netmask, False)

        # initialise PIFs
        saved_pifs = self.state_store.load_state('pif')
        if saved_pifs:
            for pif_uuid, pif in saved_pifs.items():
                if pif.get('network') in self.networks:
                    network = self.networks[pif['network']]
                    try:
                        self.PIF_create(pif['name'], pif['MTU'], pif['VLAN'],
                                        pif['MAC'], network, False, pif_uuid)
                    except NetworkAlreadyConnected, exn:
                        log.error('Cannot load saved PIF %s, as network %s ' +
                                  'is already connected to PIF %s',
                                  pif_uuid, pif['network'], exn.pif_uuid)
        else:
            for name, mtu, mac in linux_get_phy_ifaces():
                network = self.networks.values()[0]
                self.PIF_create(name, mtu, '', mac, network, False)

        # initialise storage
        saved_srs = self.state_store.load_state('sr')
        if saved_srs:
            for sr_uuid, sr_cfg in saved_srs.items():
                if sr_cfg['type'] == 'qcow_file':
                    self.srs[sr_uuid] = XendQCoWStorageRepo(sr_uuid)
                elif sr_cfg['type'] == 'local_image':
                    self.srs[sr_uuid] = XendLocalStorageRepo(sr_uuid)

        # Create missing SRs if they don't exist
        if not self.get_sr_by_type('local_image'):
            image_sr_uuid = uuid.createString()
            self.srs[image_sr_uuid] = XendLocalStorageRepo(image_sr_uuid)
            
        if not self.get_sr_by_type('qcow_file'):
            qcow_sr_uuid = uuid.createString()
            self.srs[qcow_sr_uuid] = XendQCoWStorageRepo(qcow_sr_uuid)



    def network_create(self, name_label, name_description,
                       default_gateway, default_netmask, persist = True,
                       net_uuid = None):
        if net_uuid is None:
            net_uuid = uuid.createString()
        self.networks[net_uuid] = XendNetwork(net_uuid, name_label,
                                              name_description,
                                              default_gateway,
                                              default_netmask)
        if persist:
            self.save_networks()
        return net_uuid


    def network_destroy(self, net_uuid):
        del self.networks[net_uuid]
        self.save_networks()


    def PIF_create(self, name, mtu, vlan, mac, network, persist = True,
                   pif_uuid = None):
        for pif in self.pifs.values():
            if pif.network == network:
                raise NetworkAlreadyConnected(pif.uuid)

        if pif_uuid is None:
            pif_uuid = uuid.createString()
        self.pifs[pif_uuid] = XendPIF(pif_uuid, name, mtu, vlan, mac, network,
                                      self)
        if persist:
            self.save_PIFs()
            self.refreshBridges()
        return pif_uuid


    def PIF_create_VLAN(self, pif_uuid, network_uuid, vlan):
        pif = self.pifs[pif_uuid]
        network = self.networks[network_uuid]
        return self.PIF_create(pif.name, pif.mtu, vlan, pif.mac, network)


    def PIF_destroy(self, pif_uuid):
        del self.pifs[pif_uuid]
        self.save_PIFs()


    def save(self):
        # save state
        host_record = {self.uuid: {'name_label':self.name,
                                   'name_description':self.desc}}
        self.state_store.save_state('host',host_record)
        self.state_store.save_state('cpu', self.cpus)
        self.save_PIFs()
        self.save_networks()
        self.save_SRs()

    def save_PIFs(self):
        pif_records = dict([(k, v.get_record(transient = False))
                            for k, v in self.pifs.items()])
        self.state_store.save_state('pif', pif_records)

    def save_networks(self):
        net_records = dict([(k, v.get_record(transient = False))
                            for k, v in self.networks.items()])
        self.state_store.save_state('network', net_records)

    def save_SRs(self):
        sr_records = dict([(k, v.get_record(transient = False))
                            for k, v in self.srs.items()])
        self.state_store.save_state('sr', sr_records)

    def shutdown(self):
        return 0

    def reboot(self):
        return 0

    def notify(self, _):
        return 0
        
    #
    # Ref validation
    #
    
    def is_valid_host(self, host_ref):
        return (host_ref == self.uuid)

    def is_valid_cpu(self, cpu_ref):
        return (cpu_ref in self.cpus)

    def is_valid_network(self, network_ref):
        return (network_ref in self.networks)

    def is_valid_sr(self, sr_ref):
        return (sr_ref in self.srs)

    def is_valid_vdi(self, vdi_ref):
        for sr in self.srs.values():
            if sr.is_valid_vdi(vdi_ref):
                return True
        return False

    #
    # Storage Repositories
    #

    def get_sr(self, sr_uuid):
        return self.srs.get(sr_uuid)

    def get_sr_by_type(self, sr_type):
        return [sr.uuid for sr in self.srs.values() if sr.type == sr_type]

    def get_sr_by_name(self, name):
        return [sr.uuid for sr in self.srs.values() if sr.name_label == name]

    def get_all_sr_uuid(self):
        return self.srs.keys()

    def get_vdi_by_uuid(self, vdi_uuid):
        for sr in self.srs.values():
            if sr.is_valid_vdi(vdi_uuid):
                return sr.get_vdi_by_uuid(vdi_uuid)
        return None

    def get_vdi_by_name_label(self, name):
        for sr in self.srs.values():
            vdi = sr.get_vdi_by_name_label(name)
            if vdi:
                return vdi
        return None

    def get_sr_containing_vdi(self, vdi_uuid):
        for sr in self.srs.values():
            if sr.is_valid_vdi(vdi_uuid):
                return sr
        return None
    

    #
    # Host Functions
    #

    def xen_version(self):
        info = self.xc.xeninfo()
        try:
            from xen import VERSION
            return {'Xen': '%(xen_major)d.%(xen_minor)d' % info,
                    'Xend': VERSION}
        except (ImportError, AttributeError):
            return {'Xen': '%(xen_major)d.%(xen_minor)d' % info,
                    'Xend': '3.0.3'}

    def get_name(self):
        return self.name

    def set_name(self, new_name):
        self.name = new_name

    def get_description(self):
        return self.desc

    def set_description(self, new_desc):
        self.desc = new_desc

    def get_uuid(self):
        return self.uuid

    #
    # Host CPU Functions
    #

    def get_host_cpu_by_uuid(self, host_cpu_uuid):
        if host_cpu_uuid in self.cpus:
            return host_cpu_uuid
        raise XendError('Invalid CPU UUID')

    def get_host_cpu_refs(self):
        return self.cpus.keys()

    def get_host_cpu_uuid(self, host_cpu_ref):
        if host_cpu_ref in self.cpus:
            return host_cpu_ref
        else:
            raise XendError('Invalid CPU Reference')

    def get_host_cpu_features(self, host_cpu_ref):
        try:
            return self.cpus[host_cpu_ref]['features']
        except KeyError:
            raise XendError('Invalid CPU Reference')

    def get_host_cpu_number(self, host_cpu_ref):
        try:
            return self.cpus[host_cpu_ref]['number']
        except KeyError:
            raise XendError('Invalid CPU Reference')        
            
    def get_host_cpu_load(self, host_cpu_ref):
        host_cpu = self.cpus.get(host_cpu_ref)
        if not host_cpu:
            return 0.0

        vcpu = int(host_cpu['number'])
        cpu_loads = self.monitor.get_domain_vcpus_util()
        if 0 in cpu_loads and vcpu in cpu_loads[0]:
            return cpu_loads[0][vcpu]

        return 0.0

    #
    # Network Functions
    #
    
    def get_network_refs(self):
        return self.networks.keys()

    def get_network(self, network_ref):
        return self.networks[network_ref]

    def bridge_to_network(self, bridge):
        """
        Determine which network a particular bridge is attached to.

        @param bridge The name of the bridge.  If empty, the default bridge
        will be used instead (the first one in the list returned by brctl
        show); this is the behaviour of the vif-bridge script.
        @return The XendNetwork instance to which this bridge is attached.
        @raise Exception if the interface is not connected to a network.
        """
        if not bridge:
            rc, bridge = commands.getstatusoutput(
                'brctl show | cut -d "\n" -f 2 | cut -f 1')
            if rc != 0 or not bridge:
                raise Exception(
                    'Could not find default bridge, and none was specified')

        bridges = Brctl.get_state()
        if bridge not in bridges:
            raise Exception('Bridge %s is not up' % bridge)
        for pif in self.pifs.values():
            if pif.interface_name() in bridges[bridge]:
                return pif.network
        raise Exception('Bridge %s is not connected to a network' % bridge)


    #
    # Getting host information.
    #

    def info(self):
        return (self.nodeinfo() + self.physinfo() + self.xeninfo() +
                self.xendinfo())

    def nodeinfo(self):
        (sys, host, rel, ver, mch) = os.uname()
        return [['system',  sys],
                ['host',    host],
                ['release', rel],
                ['version', ver],
                ['machine', mch]]

    def physinfo(self):
        info = self.xc.physinfo()

        info['nr_cpus'] = (info['nr_nodes'] *
                           info['sockets_per_node'] *
                           info['cores_per_socket'] *
                           info['threads_per_core'])
        info['cpu_mhz'] = info['cpu_khz'] / 1000
        # physinfo is in KiB
        info['total_memory'] = info['total_memory'] / 1024
        info['free_memory']  = info['free_memory'] / 1024

        ITEM_ORDER = ['nr_cpus',
                      'nr_nodes',
                      'sockets_per_node',
                      'cores_per_socket',
                      'threads_per_core',
                      'cpu_mhz',
                      'hw_caps',
                      'total_memory',
                      'free_memory',
                      ]

        return [[k, info[k]] for k in ITEM_ORDER]

    def xenschedinfo(self):
        sched_id = self.xc.sched_id_get()
        if sched_id == xen.lowlevel.xc.XEN_SCHEDULER_SEDF:
            return 'sedf'
        elif sched_id == xen.lowlevel.xc.XEN_SCHEDULER_CREDIT:
            return 'credit'
        else:
            return 'unknown'

    def xeninfo(self):
        info = self.xc.xeninfo()
        info['xen_scheduler'] = self.xenschedinfo()

        ITEM_ORDER = ['xen_major',
                      'xen_minor',
                      'xen_extra',
                      'xen_caps',
                      'xen_scheduler',
                      'xen_pagesize',
                      'platform_params',
                      'xen_changeset',
                      'cc_compiler',
                      'cc_compile_by',
                      'cc_compile_domain',
                      'cc_compile_date',
                      ]

        return [[k, info[k]] for k in ITEM_ORDER]

    def xendinfo(self):
        return [['xend_config_format', 3]]

    #
    # utilisation tracking
    #

    def get_vcpu_util(self, domid, vcpuid):
        cpu_loads = self.monitor.get_domain_vcpus_util()
        if domid in cpu_loads:
            return cpu_loads[domid].get(vcpuid, 0.0)
        return 0.0

    def get_vif_util(self, domid, vifid):
        vif_loads = self.monitor.get_domain_vifs_util()
        if domid in vif_loads:
            return vif_loads[domid].get(vifid, (0.0, 0.0))
        return (0.0, 0.0)

    def get_vbd_util(self, domid, vbdid):
        vbd_loads = self.monitor.get_domain_vbds_util()
        if domid in vbd_loads:
            return vbd_loads[domid].get(vbdid, (0.0, 0.0))
        return (0.0, 0.0)

    def get_pif_util(self, pifname):
        pifs_util = self.monitor.get_pifs_util()
        if pifname in pifs_util:
            return pifs_util[pifname]
        return (0.0, 0.0)

    # dictionary version of *info() functions to get rid of
    # SXPisms.
    def nodeinfo_dict(self):
        return dict(self.nodeinfo())
    def xendinfo_dict(self):
        return dict(self.xendinfo())
    def xeninfo_dict(self):
        return dict(self.xeninfo())
    def physinfo_dict(self):
        return dict(self.physinfo())
    def info_dict(self):
        return dict(self.info())


    def refreshBridges(self):
        for pif in self.pifs.values():
            pif.refresh(Brctl.get_state())


def instance():
    global inst
    try:
        inst
    except:
        inst = XendNode()
        inst.save()
    return inst
