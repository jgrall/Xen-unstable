# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>

from xen.xend import sxp
from xen.xend import XendDomain
from xen.xend import XendConsole
from xen.xend import PrettyPrint
from xen.xend.Args import FormFn

from SrvDir import SrvDir

class SrvDomain(SrvDir):
    """Service managing a single domain.
    """

    def __init__(self, dom):
        SrvDir.__init__(self)
        self.dom = dom
        self.xd = XendDomain.instance()
        self.xconsole = XendConsole.instance()

    def op_unpause(self, op, req):
        val = self.xd.domain_unpause(self.dom.id)
        return val
        
    def op_pause(self, op, req):
        val = self.xd.domain_pause(self.dom.id)
        return val

    def op_shutdown(self, op, req):
        val = self.xd.domain_shutdown(self.dom.id)
        req.setResponseCode(202)
        req.setHeader("Location", "%s/.." % req.prePathURL())
        return val

    def op_destroy(self, op, req):
        val = self.xd.domain_destroy(self.dom.id)
        req.setHeader("Location", "%s/.." % req.prePathURL())
        return val

    def op_save(self, op, req):
        fn = FormFn(self.xd.domain_save,
                    [['dom', 'int'],
                     ['file', 'str']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_migrate(self, op, req):
        fn = FormFn(self.xd.domain_migrate,
                    [['dom', 'int'],
                     ['destination', 'str']])
        val = fn(req.args, {'dom': self.dom.id})
        val = 0 # Some migrate id.
        req.setResponseCode(202)
        #req.send_header("Location", "%s/.." % self.path) # Some migrate url.
        return val

    def op_pincpu(self, op, req):
        fn = FormFn(self.xd.domain_migrate,
                    [['dom', 'int'],
                     ['cpu', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_cpu_bvt_set(self, op, req):
        fn = FormFn(self.xd.domain_cpu_bvt_set,
                    [['dom', 'int'],
                     ['mcuadv', 'int'],
                     ['warp', 'int'],
                     ['warpl', 'int'],
                     ['warpu', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_cpu_atropos_set(self, op, req):
        fn = FormFn(self.xd.domain_cpu_atropos_set,
                    [['dom', 'int'],
                     ['period', 'int'],
                     ['slice', 'int'],
                     ['latency', 'int'],
                     ['xtratime', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vifs(self, op, req):
        return self.xd.domain_vif_ls(self.dom.id)

    def op_vif(self, op, req):
        fn = FormFn(self.xd.domain_vif_get,
                    [['dom', 'int'],
                     ['vif', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vif_stats(self, op, req):
        #todo
        fn = FormFn(self.xd.domain_vif_stats,
                    [['dom', 'int'],
                     ['vif', 'int']])
        #val = fn(req.args, {'dom': self.dom.id})
        val = 999
        #return val
        return val

    def op_vif_ip_add(self, op, req):
        fn = FormFn(self.xd.domain_vif_ip_add,
                    [['dom', 'int'],
                     ['vif', 'int'],
                     ['ip', 'str']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vif_scheduler_set(self, op, req):
        fn = FormFn(self.xd.domain_vif_scheduler_set,
                    [['dom', 'int'],
                     ['vif', 'int'],
                     ['bytes', 'int'],
                     ['usecs', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vif_scheduler_get(self, op, req):
        fn = FormFn(self.xd.domain_vif_scheduler_set,
                    [['dom', 'int'],
                     ['vif', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vbds(self, op, req):
        return self.xd.domain_vbd_ls(self.dom.id)

    def op_vbd(self, op, req):
        fn = FormFn(self.xd.domain_vbd_get,
                    [['dom', 'int'],
                     ['vbd', 'int']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vbd_add(self, op, req):
        fn = FormFn(self.xd.domain_vbd_add,
                    [['dom', 'int'],
                     ['uname', 'str'],
                     ['dev', 'str'],
                     ['mode', 'str']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def op_vbd_remove(self, op, req):
        fn = FormFn(self.xd.domain_vbd_remove,
                    [['dom', 'int'],
                     ['dev', 'str']])
        val = fn(req.args, {'dom': self.dom.id})
        return val

    def render_POST(self, req):
        return self.perform(req)
        
    def render_GET(self, req):
        op = req.args.get('op')
        if op and op[0] in ['vifs', 'vif', 'vif_stats', 'vbds', 'vbd']:
            return self.perform(req)
        if self.use_sxp(req):
            req.setHeader("Content-Type", sxp.mime_type)
            sxp.show(self.dom.sxpr(), out=req)
        else:
            req.write('<html><head></head><body>')
            self.print_path(req)
            #self.ls()
            req.write('<p>%s</p>' % self.dom)
            if self.dom.console:
                cinfo = self.dom.console
                cid = cinfo.id
                #todo: Local xref: need to know server prefix.
                req.write('<p><a href="/xend/console/%s">Console %s</a></p>'
                          % (cid, cid))
                req.write('<p><a href="%s">Connect to console</a></p>'
                          % cinfo.uri())
            if self.dom.config:
                req.write("<code><pre>")
                PrettyPrint.prettyprint(self.dom.config, out=req)
                req.write("</pre></code>")
            req.write('<a href="%s?op=vif_stats&vif=0">vif 0 stats</a>'
                      % req.prePathURL())
            self.form(req)
            req.write('</body></html>')
        return ''

    def form(self, req):
        req.write('<form method="post" action="%s">' % req.prePathURL())
        req.write('<input type="submit" name="op" value="unpause">')
        req.write('<input type="submit" name="op" value="pause">')
        req.write('<input type="submit" name="op" value="shutdown">')
        req.write('<input type="submit" name="op" value="destroy">')
        req.write('<br><input type="submit" name="op" value="migrate">')
        req.write('To: <input type="text" name="destination">')
        req.write('</form>')
