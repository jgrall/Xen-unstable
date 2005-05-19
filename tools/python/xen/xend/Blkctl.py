"""Xend interface to block control scripts.
"""
import os
import os.path
import sys
import string
import xen.util.process

from xen.xend import XendRoot

xroot = XendRoot.instance()

"""Where network control scripts live."""
SCRIPT_DIR = xroot.block_script_dir

def block(op, type, dets, script=None):
    """Call a block control script.
    Xend calls this with op 'bind' when it is about to export a block device
    (other than a raw partition).  The script is called with unbind when a
    device is no longer in use and should be removed.

    @param op:        operation (start, stop, status)
    @param type:      type of block device (determines the script used)
    @param dets:      arguments to the control script
    @param script:    block script name
    """
    
    if op not in ['bind', 'unbind']:
        raise ValueError('Invalid operation:' + op)

    # Special case phy devices - they don't require any (un)binding
    # Parallax also doesn't need script-based binding.
    if (type == 'phy') or (type == 'parallax'):
        return dets
    
    if script is None:
        script = xroot.get_block_script(type)
    script = os.path.join(SCRIPT_DIR, script)
    args = [op] + string.split(dets, ':')
    args = ' '.join(args)
    ret = xen.util.process.runscript(script + ' ' + args)
    if len(ret):
        return ret.splitlines()[0]
