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
# Copyright (C) 2005 XenSource Ltd
#============================================================================


import os, string
import re
import math
import signal

import xen.lowlevel.xc
from xen.xend.XendConstants import REVERSE_DOMAIN_SHUTDOWN_REASONS
from xen.xend.XendError import VmError, XendError
from xen.xend.XendLogging import log
from xen.xend.server.netif import randomMAC
from xen.xend.xenstore.xswatch import xswatch
from xen.xend import arch
from xen.xend import FlatDeviceTree

xc = xen.lowlevel.xc.xc()

MAX_GUEST_CMDLINE = 1024


def create(vm, vmConfig, imageConfig, deviceConfig):
    """Create an image handler for a vm.

    @return ImageHandler instance
    """
    return findImageHandlerClass(imageConfig)(vm, vmConfig, imageConfig,
                                              deviceConfig)


class ImageHandler:
    """Abstract base class for image handlers.

    createImage() is called to configure and build the domain from its
    kernel image and ramdisk etc.

    The method buildDomain() is used to build the domain, and must be
    defined in a subclass.  Usually this is the only method that needs
    defining in a subclass.

    The method createDeviceModel() is called to create the domain device
    model if it needs one.  The default is to do nothing.

    The method destroy() is called when the domain is destroyed.
    The default is to do nothing.
    """

    ostype = None


    def __init__(self, vm, vmConfig, imageConfig, deviceConfig):
        self.vm = vm

        self.kernel = None
        self.ramdisk = None
        self.cmdline = None

        self.configure(vmConfig, imageConfig, deviceConfig)

    def configure(self, vmConfig, imageConfig, _):
        """Config actions common to all unix-like domains."""
        self.kernel = vmConfig['kernel_kernel']
        self.cmdline = vmConfig['kernel_args']
        self.ramdisk = vmConfig['kernel_initrd']
        self.vm.storeVm(("image/ostype", self.ostype),
                        ("image/kernel", self.kernel),
                        ("image/cmdline", self.cmdline),
                        ("image/ramdisk", self.ramdisk))


    def cleanupBootloading(self):
        self.unlink(self.kernel)
        self.unlink(self.ramdisk)


    def unlink(self, f):
        if not f: return
        try:
            os.unlink(f)
        except OSError, ex:
            log.warning("error removing bootloader file '%s': %s", f, ex)


    def createImage(self):
        """Entry point to create domain memory image.
        Override in subclass  if needed.
        """
        return self.createDomain()


    def createDomain(self):
        """Build the domain boot image.
        """
        # Set params and call buildDomain().

        if not os.path.isfile(self.kernel):
            raise VmError('Kernel image does not exist: %s' % self.kernel)
        if self.ramdisk and not os.path.isfile(self.ramdisk):
            raise VmError('Kernel ramdisk does not exist: %s' % self.ramdisk)
        if len(self.cmdline) >= MAX_GUEST_CMDLINE:
            log.warning('kernel cmdline too long, domain %d',
                        self.vm.getDomid())
        
        log.info("buildDomain os=%s dom=%d vcpus=%d", self.ostype,
                 self.vm.getDomid(), self.vm.getVCpuCount())

        result = self.buildDomain()

        if isinstance(result, dict):
            return result
        else:
            raise VmError('Building domain failed: ostype=%s dom=%d err=%s'
                          % (self.ostype, self.vm.getDomid(), str(result)))

    def getRequiredAvailableMemory(self, mem_kb):
        """@param mem_kb The configured maxmem or memory, in KiB.
        @return The corresponding required amount of memory for the domain,
        also in KiB.  This is normally the given mem_kb, but architecture- or
        image-specific code may override this to add headroom where
        necessary."""
        return mem_kb

    def getRequiredInitialReservation(self):
        """@param mem_kb The configured memory, in KiB.
        @return The corresponding required amount of memory to be free, also
        in KiB. This is normally the same as getRequiredAvailableMemory, but
        architecture- or image-specific code may override this to
        add headroom where necessary."""
        return self.getRequiredAvailableMemory(self.vm.getMemoryTarget())

    def getRequiredShadowMemory(self, shadow_mem_kb, maxmem_kb):
        """@param shadow_mem_kb The configured shadow memory, in KiB.
        @param maxmem_kb The configured maxmem, in KiB.
        @return The corresponding required amount of shadow memory, also in
        KiB."""
        # PV domains don't need any shadow memory
        return 0

    def buildDomain(self):
        """Build the domain. Define in subclass."""
        raise NotImplementedError()

    def createDeviceModel(self):
        """Create device model for the domain (define in subclass if needed)."""
        pass
    
    def destroy(self):
        """Extra cleanup on domain destroy (define in subclass if needed)."""
        pass


    def recreate(self):
        pass


class LinuxImageHandler(ImageHandler):

    ostype = "linux"

    def buildDomain(self):
        store_evtchn = self.vm.getStorePort()
        console_evtchn = self.vm.getConsolePort()

        mem_mb = self.getRequiredInitialReservation() / 1024

        log.debug("domid          = %d", self.vm.getDomid())
        log.debug("memsize        = %d", mem_mb)
        log.debug("image          = %s", self.kernel)
        log.debug("store_evtchn   = %d", store_evtchn)
        log.debug("console_evtchn = %d", console_evtchn)
        log.debug("cmdline        = %s", self.cmdline)
        log.debug("ramdisk        = %s", self.ramdisk)
        log.debug("vcpus          = %d", self.vm.getVCpuCount())
        log.debug("features       = %s", self.vm.getFeatures())

        return xc.linux_build(domid          = self.vm.getDomid(),
                              memsize        = mem_mb,
                              image          = self.kernel,
                              store_evtchn   = store_evtchn,
                              console_evtchn = console_evtchn,
                              cmdline        = self.cmdline,
                              ramdisk        = self.ramdisk,
                              features       = self.vm.getFeatures())

    def destroy(self):
        if not self.pid:
            return
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)
        self.pid = 0

class PPC_LinuxImageHandler(LinuxImageHandler):

    ostype = "linux"

    def configure(self, vmConfig, imageConfig, deviceConfig):
        LinuxImageHandler.configure(self, vmConfig, imageConfig, deviceConfig)
        self.imageConfig = imageConfig

    def buildDomain(self):
        store_evtchn = self.vm.getStorePort()
        console_evtchn = self.vm.getConsolePort()

        mem_mb = self.getRequiredInitialReservation() / 1024

        log.debug("domid          = %d", self.vm.getDomid())
        log.debug("memsize        = %d", mem_mb)
        log.debug("image          = %s", self.kernel)
        log.debug("store_evtchn   = %d", store_evtchn)
        log.debug("console_evtchn = %d", console_evtchn)
        log.debug("cmdline        = %s", self.cmdline)
        log.debug("ramdisk        = %s", self.ramdisk)
        log.debug("vcpus          = %d", self.vm.getVCpuCount())
        log.debug("features       = %s", self.vm.getFeatures())

        devtree = FlatDeviceTree.build(self)

        return xc.linux_build(domid          = self.vm.getDomid(),
                              memsize        = mem_mb,
                              image          = self.kernel,
                              store_evtchn   = store_evtchn,
                              console_evtchn = console_evtchn,
                              cmdline        = self.cmdline,
                              ramdisk        = self.ramdisk,
                              features       = self.vm.getFeatures(),
                              arch_args      = devtree.to_bin())

class HVMImageHandler(ImageHandler):

    ostype = "hvm"

    def __init__(self, vm, vmConfig, imageConfig, deviceConfig):
        ImageHandler.__init__(self, vm, vmConfig, imageConfig, deviceConfig)
        self.shutdownWatch = None
        self.rebootFeatureWatch = None

    def configure(self, vmConfig, imageConfig, deviceConfig):
        ImageHandler.configure(self, vmConfig, imageConfig, deviceConfig)

        info = xc.xeninfo()
        if 'hvm' not in info['xen_caps']:
            raise VmError("HVM guest support is unavailable: is VT/AMD-V "
                          "supported by your CPU and enabled in your BIOS?")

        self.dmargs = self.parseDeviceModelArgs(imageConfig, deviceConfig)
        self.device_model = imageConfig['hvm'].get('device_model')
        if not self.device_model:
            raise VmError("hvm: missing device model")
        
        self.display = imageConfig['hvm'].get('display')
        self.xauthority = imageConfig['hvm'].get('xauthority')
        self.vncconsole = imageConfig['hvm'].get('vncconsole')

        self.vm.storeVm(("image/dmargs", " ".join(self.dmargs)),
                        ("image/device-model", self.device_model),
                        ("image/display", self.display))

        self.pid = None

        self.dmargs += self.configVNC(imageConfig)

        self.pae  = imageConfig['hvm'].get('pae', 0)
        self.apic  = imageConfig['hvm'].get('apic', 0)
        self.acpi  = imageConfig['hvm']['devices'].get('acpi', 0)
        

    def buildDomain(self):
        store_evtchn = self.vm.getStorePort()

        mem_mb = self.getRequiredInitialReservation() / 1024

        log.debug("domid          = %d", self.vm.getDomid())
        log.debug("image          = %s", self.kernel)
        log.debug("store_evtchn   = %d", store_evtchn)
        log.debug("memsize        = %d", mem_mb)
        log.debug("vcpus          = %d", self.vm.getVCpuCount())
        log.debug("pae            = %d", self.pae)
        log.debug("acpi           = %d", self.acpi)
        log.debug("apic           = %d", self.apic)

        self.register_shutdown_watch()
        self.register_reboot_feature_watch()

        return xc.hvm_build(domid          = self.vm.getDomid(),
                            image          = self.kernel,
                            store_evtchn   = store_evtchn,
                            memsize        = mem_mb,
                            vcpus          = self.vm.getVCpuCount(),
                            pae            = self.pae,
                            acpi           = self.acpi,
                            apic           = self.apic)

    # Return a list of cmd line args to the device models based on the
    # xm config file
    def parseDeviceModelArgs(self, imageConfig, deviceConfig):
        dmargs = [ 'boot', 'fda', 'fdb', 'soundhw',
                   'localtime', 'serial', 'stdvga', 'isa', 'vcpus',
                   'acpi', 'usb', 'usbdevice', 'keymap' ]
        ret = []
        hvmDeviceConfig = imageConfig['hvm']['devices']
        
        for a in dmargs:
            v = hvmDeviceConfig.get(a)

            # python doesn't allow '-' in variable names
            if a == 'stdvga': a = 'std-vga'
            if a == 'keymap': a = 'k'

            # Handle booleans gracefully
            if a in ['localtime', 'std-vga', 'isa', 'usb', 'acpi']:
                if v != None: v = int(v)
                if v: ret.append("-%s" % a)
            else:
                if v:
                    ret.append("-%s" % a)
                    ret.append("%s" % v)

            if a in ['fda', 'fdb']:
                if v:
                    if not os.path.isabs(v):
                        raise VmError("Floppy file %s does not exist." % v)
            log.debug("args: %s, val: %s" % (a,v))

        # Handle disk/network related options
        mac = None
        ret = ret + ["-domain-name", str(self.vm.info['name_label'])]
        nics = 0
        
        for devuuid, (devtype, devinfo) in deviceConfig.items():
            if devtype == 'vbd':
                uname = devinfo['uname']
                if uname is not None and 'file:' in uname:
                    (_, vbdparam) = string.split(uname, ':', 1)
                    if not os.path.isfile(vbdparam):
                        raise VmError('Disk image does not exist: %s' %
                                      vbdparam)
            if devtype == 'vif':
                dtype = devinfo.get('type', 'ioemu')
                if dtype != 'ioemu':
                    continue
                nics += 1
                mac = devinfo.get('mac')
                if mac == None:
                    mac = randomMAC()
                bridge = devinfo.get('bridge', 'xenbr0')
                model = devinfo.get('model', 'rtl8139')
                ret.append("-net")
                ret.append("nic,vlan=%d,macaddr=%s,model=%s" %
                           (nics, mac, model))
                ret.append("-net")
                ret.append("tap,vlan=%d,bridge=%s" % (nics, bridge))
        return ret

    def configVNC(self, imageConfig):
        # Handle graphics library related options
        vnc = imageConfig.get('vnc')
        sdl = imageConfig.get('sdl')
        ret = []
        nographic = imageConfig.get('nographic')

        # get password from VM config (if password omitted, None)
        vncpasswd_vmconfig = imageConfig.get('vncpasswd')

        if nographic:
            ret.append('-nographic')
            return ret

        if vnc:
            vncdisplay = imageConfig.get('vncdisplay',
                                         int(self.vm.getDomid()))
            vncunused = imageConfig.get('vncunused')

            if vncunused:
                ret += ['-vncunused']
            else:
                ret += ['-vnc', '%d' % vncdisplay]

            vnclisten = imageConfig.get('vnclisten')

            if not(vnclisten):
                vnclisten = (xen.xend.XendRoot.instance().
                             get_vnclisten_address())
            if vnclisten:
                ret += ['-vnclisten', vnclisten]

            vncpasswd = vncpasswd_vmconfig
            if vncpasswd is None:
                vncpasswd = (xen.xend.XendRoot.instance().
                             get_vncpasswd_default())
                if vncpasswd is None:
                    raise VmError('vncpasswd is not set up in ' +
                                  'VMconfig and xend-config.')
            if vncpasswd != '':
                self.vm.storeVm("vncpasswd", vncpasswd)

        return ret

    def createDeviceModel(self):
        if self.pid:
            return
        # Execute device model.
        #todo: Error handling
        args = [self.device_model]
        args = args + ([ "-d",  "%d" % self.vm.getDomid(),
                  "-m", "%s" % (self.getRequiredInitialReservation() / 1024)])
        args = args + self.dmargs
        env = dict(os.environ)
        if self.display:
            env['DISPLAY'] = self.display
        if self.xauthority:
            env['XAUTHORITY'] = self.xauthority
        if self.vncconsole:
            args = args + ([ "-vncviewer" ])
        log.info("spawning device models: %s %s", self.device_model, args)
        # keep track of pid and spawned options to kill it later
        self.pid = os.spawnve(os.P_NOWAIT, self.device_model, args, env)
        self.vm.storeDom("image/device-model-pid", self.pid)
        log.info("device model pid: %d", self.pid)

    def recreate(self):
        self.register_shutdown_watch()
        self.register_reboot_feature_watch()
        self.pid = self.vm.gatherDom(('image/device-model-pid', int))

    def destroy(self):
        self.unregister_shutdown_watch()
        self.unregister_reboot_feature_watch();
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except OSError, exn:
                log.exception(exn)
            try:
                os.waitpid(self.pid, 0)
            except OSError, exn:
                # This is expected if Xend has been restarted within the
                # life of this domain.  In this case, we can kill the process,
                # but we can't wait for it because it's not our child.
                pass
            self.pid = None

    def register_shutdown_watch(self):
        """ add xen store watch on control/shutdown """
        self.shutdownWatch = xswatch(self.vm.dompath + "/control/shutdown",
                                     self.hvm_shutdown)
        log.debug("hvm shutdown watch registered")

    def unregister_shutdown_watch(self):
        """Remove the watch on the control/shutdown, if any. Nothrow
        guarantee."""

        try:
            if self.shutdownWatch:
                self.shutdownWatch.unwatch()
        except:
            log.exception("Unwatching hvm shutdown watch failed.")
        self.shutdownWatch = None
        log.debug("hvm shutdown watch unregistered")

    def hvm_shutdown(self, _):
        """ watch call back on node control/shutdown,
            if node changed, this function will be called
        """
        xd = xen.xend.XendDomain.instance()
        try:
            vm = xd.domain_lookup( self.vm.getDomid() )
        except XendError:
            # domain isn't registered, no need to clean it up.
            return False

        reason = vm.getShutdownReason()
        log.debug("hvm_shutdown fired, shutdown reason=%s", reason)
        if reason in REVERSE_DOMAIN_SHUTDOWN_REASONS:
            vm.info['shutdown'] = 1
            vm.info['shutdown_reason'] = \
                REVERSE_DOMAIN_SHUTDOWN_REASONS[reason]
            vm.refreshShutdown(vm.info)

        return True # Keep watching

    def register_reboot_feature_watch(self):
        """ add xen store watch on control/feature-reboot """
        self.rebootFeatureWatch = xswatch(self.vm.dompath + "/control/feature-reboot", \
                                         self.hvm_reboot_feature)
        log.debug("hvm reboot feature watch registered")

    def unregister_reboot_feature_watch(self):
        """Remove the watch on the control/feature-reboot, if any. Nothrow
        guarantee."""

        try:
            if self.rebootFeatureWatch:
                self.rebootFeatureWatch.unwatch()
        except:
            log.exception("Unwatching hvm reboot feature watch failed.")
        self.rebootFeatureWatch = None
        log.debug("hvm reboot feature watch unregistered")

    def hvm_reboot_feature(self, _):
        """ watch call back on node control/feature-reboot,
            if node changed, this function will be called
        """
        status = self.vm.readDom('control/feature-reboot')
        log.debug("hvm_reboot_feature fired, module status=%s", status)
        if status == '1':
            self.unregister_shutdown_watch()

        return True # Keep watching


class IA64_HVM_ImageHandler(HVMImageHandler):

    def getRequiredAvailableMemory(self, mem_kb):
        page_kb = 16
        # ROM size for guest firmware, ioreq page and xenstore page
        extra_pages = 1024 + 3
        return mem_kb + extra_pages * page_kb

    def getRequiredShadowMemory(self, shadow_mem_kb, maxmem_kb):
        # Explicit shadow memory is not a concept 
        return 0

class X86_HVM_ImageHandler(HVMImageHandler):

    def getRequiredAvailableMemory(self, mem_kb):
        # Add 8 MiB overhead for QEMU's video RAM.
        return mem_kb + 8192

    def getRequiredInitialReservation(self):
        return self.vm.getMemoryTarget()

    def getRequiredShadowMemory(self, shadow_mem_kb, maxmem_kb):
        # 256 pages (1MB) per vcpu,
        # plus 1 page per MiB of RAM for the P2M map,
        # plus 1 page per MiB of RAM to shadow the resident processes.  
        # This is higher than the minimum that Xen would allocate if no value 
        # were given (but the Xen minimum is for safety, not performance).
        return max(4 * (256 * self.vm.getVCpuCount() + 2 * (maxmem_kb / 1024)),
                   shadow_mem_kb)


_handlers = {
    "powerpc": {
        "linux": PPC_LinuxImageHandler,
    },
    "ia64": {
        "linux": LinuxImageHandler,
        "hvm": IA64_HVM_ImageHandler,
    },
    "x86": {
        "linux": LinuxImageHandler,
        "hvm": X86_HVM_ImageHandler,
    },
}

def findImageHandlerClass(image):
    """Find the image handler class for an image config.

    @param image config
    @return ImageHandler subclass or None
    """
    image_type = image['type']
    if image_type is None:
        raise VmError('missing image type')
    try:
        return _handlers[arch.type][image_type]
    except KeyError:
        raise VmError('unknown image type: ' + image_type)
