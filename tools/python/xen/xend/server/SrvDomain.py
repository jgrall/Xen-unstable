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

from xen.web import http

from xen.xend import sxp
from xen.xend import XendDomain
from xen.xend import PrettyPrint
from xen.xend.Args import FormFn

from xen.web.SrvDir import SrvDir

class SrvDomain(SrvDir):
    """Service managing a single domain.
    """

    def __init__(self, dom):
        SrvDir.__init__(self)
        self.dom = dom
        self.xd = XendDomain.instance()

    def op_configure(self, op, req):
        """Configure an existing domain.
        Configure is unusual in that it requires a domain id,
        not a domain name.
        """
        fn = FormFn(self.xd.domain_configure,
                    [['dom',    'int'],
                     ['config', 'sxpr']])
        return fn(req.args, {'dom': self.dom.domid})

    def op_unpause(self, op, req):
        val = self.xd.domain_unpause(self.dom.domid)
        return val
        
    def op_pause(self, op, req):
        val = self.xd.domain_pause(self.dom.domid)
        return val

    def acceptCommand(self, req):
        req.setResponseCode(http.ACCEPTED)
        req.setHeader("Location", "%s/.." % req.prePathURL())

    def op_shutdown(self, op, req):
        self.acceptCommand(req)
        return self.dom.shutdown(req.args['reason'][0])

    def op_sysrq(self, op, req):
        self.acceptCommand(req)
        return self.dom.send_sysrq(int(req.args['key'][0]))

    def op_destroy(self, op, req):
        self.acceptCommand(req)
        return self.xd.domain_destroy(self.dom.domid)

    def op_save(self, op, req):
        self.acceptCommand(req)
        return req.threadRequest(self.do_save, op, req)

    def do_save(self, op, req):
        return self.xd.domain_save(self.dom.domid, req.args['file'][0])

    def op_migrate(self, op, req):
        return req.threadRequest(self.do_migrate, op, req)
    
    def do_migrate(self, op, req):
        fn = FormFn(self.xd.domain_migrate,
                    [['dom',         'int'],
                     ['destination', 'str'],
                     ['live',        'int'],
                     ['resource',    'int']])
        return fn(req.args, {'dom': self.dom.domid})

    def op_pincpu(self, op, req):
        fn = FormFn(self.xd.domain_pincpu,
                    [['dom', 'int'],
                     ['vcpu', 'int'],
                     ['cpumap', 'int']])
        val = fn(req.args, {'dom': self.dom.domid})
        return val

    def op_cpu_bvt_set(self, op, req):
        fn = FormFn(self.xd.domain_cpu_bvt_set,
                    [['dom',       'int'],
                     ['mcuadv',    'int'],
                     ['warpback',  'int'],
                     ['warpvalue', 'int'],
                     ['warpl',     'long'],
                     ['warpu',     'long']])
        val = fn(req.args, {'dom': self.dom.domid})
        return val
    
    
    def op_cpu_sedf_set(self, op, req):
        fn = FormFn(self.xd.domain_cpu_sedf_set,
                    [['dom', 'int'],
                     ['period', 'int'],
                     ['slice', 'int'],
		     ['latency', 'int'],
		     ['extratime', 'int'],
		     ['weight', 'int']])
        val = fn(req.args, {'dom': self.dom.domid})
        return val

    def op_maxmem_set(self, op, req):
        fn = FormFn(self.xd.domain_maxmem_set,
                    [['dom',    'int'],
                     ['memory', 'int']])
        val = fn(req.args, {'dom': self.dom.domid})
        return val

    
    def call(self, fn, args, req):
        return FormFn(fn, args)(req.args)


    def op_mem_target_set(self, op, req):
        return self.call(self.dom.setMemoryTarget
                         [['target', 'int']],
                         req)

    def op_devices(self, op, req):
        return self.call(self.dom.getDeviceSxprs,
                         [['deviceClass', 'str']],
                         req)

    def op_device_create(self, op, req):
        return self.call(self.dom.device_create,
                         [['dev_config', 'sxpr']],
                         req)

    def op_device_destroy(self, op, req):
        return self.call(self.dom.destroyDevice,
                         [['deviceClass', 'str'],
                          ['devid',       'int']],
                         req)
                
    def op_device_configure(self, op, req):
        return self.call(self.dom.device_configure,
                         [['dev_config', 'sxpr'],
                          ['devid',       'int']],
                         req)


    def op_vif_limit_set(self, op, req):
        fn = FormFn(self.xd.domain_vif_limit_set,
                    [['dom',    'int'],
                     ['vif',    'int'],
                     ['credit', 'int'],
                     ['period', 'int']])
        val = fn(req.args, {'dom': self.dom.domid})
        return val

    def op_vcpu_hotplug(self, op, req):
        return self.call(self.dom.vcpu_hotplug,
                         [['vcpu', 'int'],
                          ['state', 'int']],
                         req)

    def render_POST(self, req):
        return self.perform(req)
        
    def render_GET(self, req):
        op = req.args.get('op')
        #
        # XXX SMH: below may be useful once again if we ever try to get
        # the raw 'web' interface to xend working once more. But for now
        # is useless and out of date (i.e. no ops called 'v???' anymore).
        #
        # if op and op[0] in ['vifs', 'vif', 'vbds', 'vbd', 'mem_target_set']:
        #    return self.perform(req)
        if self.use_sxp(req):
            req.setHeader("Content-Type", sxp.mime_type)
            sxp.show(self.dom.sxpr(), out=req)
        else:
            req.write('<html><head></head><body>')
            self.print_path(req)
            #self.ls()
            req.write('<p>%s</p>' % self.dom)
            self.form(req)
            req.write('</body></html>')
        return ''

    def form(self, req):
        url = req.prePathURL()
        req.write('<form method="post" action="%s">' % url)
        req.write('<input type="submit" name="op" value="unpause">')
        req.write('<input type="submit" name="op" value="pause">')
        req.write('</form>')

        req.write('<form method="post" action="%s">' % url)
        req.write('<input type="submit" name="op" value="destroy">')
        req.write('</form>')

        req.write('<form method="post" action="%s">' % url)
        req.write('<input type="submit" name="op" value="shutdown">')
        req.write('<input type="radio" name="reason" value="poweroff" checked>Poweroff')
        req.write('<input type="radio" name="reason" value="halt">Halt')
        req.write('<input type="radio" name="reason" value="reboot">Reboot')
        req.write('</form>')
        
        req.write('<form method="post" action="%s">' % url)
        req.write('<br><input type="submit" name="op" value="save">')
        req.write(' To file: <input type="text" name="file">')
        req.write('</form>')
        
        req.write('<form method="post" action="%s">' % url)
        req.write('<br><input type="submit" name="op" value="migrate">')
        req.write(' To host: <input type="text" name="destination">')
        req.write('<input type="checkbox" name="live" value="1">Live')
        req.write('</form>')
