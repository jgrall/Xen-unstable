#!/usr/bin/env python

import Xc, XenoUtil, string, sys, os, time, socket, getopt

config_dir  = '/etc/xc/'
config_file = xc_config_file = config_dir + 'defaults'

def main_usage ():
    print >>sys.stderr,"""
Usage: %s <args>

This tool is used to create and start new domains. It reads defaults
from a file written in Python, having allowed variables to be set and
passed into the file. Further command line arguments allow the
defaults to be overridden. The defaults for each parameter are listed
in [] brackets. Arguments are as follows:

Arguments to control the parsing of the defaults file:
 -f config_file   -- Use the specified defaults script. 
                     Default: ['%s']
 -D foo=bar       -- Set variable foo=bar before parsing config
                     E.g. '-D vmid=3:ip=1.2.3.4'
 -h               -- Print extended help message, including all arguments
 -n               -- Dry run only, don't actually create domain
""" % (sys.argv[0], xc_config_file)

def extra_usage ():
    print >>sys.stderr,"""
Arguments to override current config read from '%s':
 -k image         -- Path to kernel image ['%s']
 -r ramdisk       -- Path to ramdisk (or empty) ['%s']
 -b builder_fn    -- Function to use to build domain ['%s']
 -m mem_size      -- Initial memory allocation in MB [%dMB]
 -N domain_name   -- Set textual name of domain ['%s']
 -a auto_restart  -- Restart domain on exit, yes/no ['%d']
 -e vbd_expert    -- Saftey catch to avoid some disk accidents ['%d'] 
 -d udisk,dev,rw  -- Add disk, partition, or virtual disk to domain. E.g. to 
                     make partion sda4 available to the domain as hda1 with 
                     read-write access: '-b phy:sda4,hda1,rw' To add 
                     multiple disks use multiple -d flags or seperate with ':'
                     Default: ['%s']
 -i vfr_ipaddr    -- Add IP address to the list which Xen will route to
                     the domain. Use multiple times to add more IP addrs.
		     Default: ['%s']

Args to override the kernel command line, which is concatenated from these:
 -I cmdline_ip    -- Override 'ip=ipaddr:nfsserv:gateway:netmask::eth0:off'
                     Default: ['%s']
 -R cmdline_root  -- Override root device parameters.
                     Default: ['%s']
 -E cmdline_extra -- Override extra kernel args and rc script env vars.
                     Default: ['%s']

""" % (config_file,
       image, ramdisk, builder_fn, mem_size, domain_name, auto_restart,
       vbd_expert, 
       printvbds( vbd_list ), 
       reduce ( (lambda a,b: a+':'+b), vfr_ipaddr,'' )[1:],
       cmdline_ip, cmdline_root, cmdline_extra)

def config_usage (): pass

def answer ( s ):
    s = string.lower(s)
    if s == 'yes' or s == 'true' or s == '1': return 1
    return 0

def printvbds ( v ):
    s=''
    for (a,b,c) in v:
	s = s + ':%s,%s,%s' % (a,b,c)
    return s[1:]


bail=False; dryrun=False; extrahelp=False
image=''; ramdisk=''; builder_fn=''; 
mem_size=0; domain_name=''; vfr_ipaddr=[]; 
vbd_expert=0; auto_restart=0;
vbd_list = []; cmdline_ip = ''; cmdline_root=''; cmdline_extra=''

##### Determine location of defautls file
#####

try:
    opts, args = getopt.getopt(sys.argv[1:], "h?nf:D:k:r:b:m:N:a:e:d:i:I:R:E:" )

    for opt in opts:
	if opt[0] == '-f': config_file= opt[1]
	if opt[0] == '-h' or opt[0] == '-?' : bail=True; extrahelp=True
	if opt[0] == '-n': dryrun=True
	if opt[0] == '-D': 
	    for o in string.split( opt[1], ':' ):
		(l,r) = string.split( o, '=' )
		exec "%s='%s'" % (l,r)


except getopt.GetoptError:
    bail=True


try:
    os.stat( config_file )
except:
    try:
	d = config_dir + config_file
	os.stat( d )
	config_file = d
    except:
	print >> sys.stderr, "Unable to open config file '%s'" % config_file
	bail = True


##### Parse the config file
#####

print "Parsing config file '%s'" % config_file

try:
    execfile ( config_file )
except (AssertionError,IOError):
    print >>sys.stderr,"Exiting %s" % sys.argv[0]
    bail = True

##### Print out config if necessary 
##### 

if bail:
    main_usage()
    config_usage()
    if extrahelp: extra_usage()
    sys.exit(1)

##### Parse any command line overrides 
##### 

x_vbd_list = []
x_vfr_ipaddr  = []

for opt in opts:
    if opt[0] == '-k': image = opt[1]
    if opt[0] == '-r': ramdisk = opt[1]
    if opt[0] == '-b': builder_fn = opt[1]  #XXXX
    if opt[0] == '-m': mem_size = int(opt[1])
    if opt[0] == '-N': domain_name = opt[1]
    if opt[0] == '-a': auto_restart = answer(opt[1])
    if opt[0] == '-e': vbd_expert = answer(opt[1])
    if opt[0] == '-I': cmdline_ip = opt[1]
    if opt[0] == '-R': cmdline_root = opt[1]
    if opt[0] == '-E': cmdline_extra = opt[1]
    if opt[0] == '-i': x_vfr_ipaddr.append(opt[1])
    if opt[0] == '-d':
	try:
	    vv = string.split(opt[1],':')	    
	    for v in vv:
		(udisk,dev,mode) = string.split(v,',')
		x_vbd_list.append( (udisk,dev,mode) )
	except:
	    print >>sys.stderr, "Invalid block device specification : %s" % opt[1]
	    sys.exit(1)

if x_vbd_list: vbd_list = x_vbd_list
if x_vfr_ipaddr: vfr_ipaddr = x_vfr_ipaddr

cmdline = cmdline_ip +' '+ cmdline_root +' '+ cmdline_extra

##### Print some debug info just incase things don't work out...
##### 

print
print 'VM image           : "%s"' % image
print 'VM ramdisk         : "%s"' % ramdisk
print 'VM memory (MB)     : "%d"' % mem_size
print 'VM IP address(es)  : "%s"' % reduce((lambda a,b: a+':'+b),vfr_ipaddr,'' )[1:]
print 'VM block device(s) : "%s"' % printvbds( vbd_list )
print 'VM cmdline         : "%s"' % cmdline
print

if dryrun:
    sys.exit(1)

##### Code beyond this point is actually used to manage the mechanics of
##### starting (and watching if necessary) guest virtual machines.

# Obtain an instance of the Xen control interface
xc = Xc.new()

# This function creates, builds and starts a domain, using the values
# in the global variables, set above.  It is used in the subsequent
# code for starting the new domain and rebooting it if appropriate.
def make_domain():
    """Create, build and start a domain.
    Returns: [int] the ID of the new domain.
    """

    # set up access to the global variables declared above
    global image, ramdisk, mem_size, domain_name, vfr_ipaddr, netmask
    global vbd_list, cmdline, xc, vbd_expert
    	
    if not os.path.isfile( image ):
        print "Image file '" + image + "' does not exist"
        sys.exit()

    if ramdisk and not os.path.isfile( ramdisk ):
        print "Ramdisk file '" + ramdisk + "' does not exist"
        sys.exit()

    id = xc.domain_create( mem_kb=mem_size*1024, name=domain_name )
    if id <= 0:
        print "Error creating domain"
        sys.exit()

    ret = xc.linux_build( dom=id, image=image, ramdisk=ramdisk, 
			  cmdline=cmdline )
    if ret < 0:
        print "Error building Linux guest OS: "
        print "Return code from linux_build = " + str(ret)
        xc.domain_destroy ( dom=id )
        sys.exit()

    # setup the virtual block devices

    # set the expertise level appropriately
    XenoUtil.VBD_EXPERT_MODE = vbd_expert
    
    for ( uname, virt_name, rw ) in vbd_list:
	virt_dev = XenoUtil.blkdev_name_to_number( virt_name )

	segments = XenoUtil.lookup_disk_uname( uname )
	if not segments:
	    print "Error looking up %s\n" % uname
	    xc.domain_destroy ( dom=id )
	    sys.exit()

        # check that setting up this VBD won't violate the sharing
        # allowed by the current VBD expertise level
        if XenoUtil.vd_extents_validate(segments, rw=='w' or rw=='rw') < 0:
            xc.domain_destroy( dom = id )
            sys.exit()
            
	if xc.vbd_create( dom=id, vbd=virt_dev, writeable= rw=='w' or rw=='rw' ):
	    print "Error creating VBD vbd=%d writeable=%d\n" % (virt_dev,rw)
	    xc.domain_destroy ( dom=id )
	    sys.exit()
	
        if xc.vbd_setextents( dom=id,
                              vbd=virt_dev,
                              extents=segments):
            print "Error populating VBD vbd=%d\n" % virt_dev
            xc.domain_destroy ( dom=id )
            sys.exit()

    # setup virtual firewall rules for all aliases
    for ip in vfr_ipaddr:
	XenoUtil.setup_vfr_rules_for_vif( id, 0, ip )

    if xc.domain_start( dom=id ) < 0:
        print "Error starting domain"
        xc.domain_destroy ( dom=id )
        sys.exit()

    return id
# end of make_domain()



# The starting / monitoring of the domain actually happens here...

# start the domain and record its ID number
current_id = make_domain()
print "VM started in domain %d" % current_id

# if the auto_restart flag is set then keep polling to see if the domain is
# alive - restart if it is not by calling make_domain() again (it's necessary
# to update the id variable, since the new domain may have a new ID)

if auto_restart:
    # turn ourselves into a background daemon
    try:
	pid = os.fork()
	if pid > 0:
	    sys.exit(0)
	os.setsid()
	pid = os.fork()
	if pid > 0:
	    print >> sys.stderr, 'Auto-restart daemon PID = %d' % pid
	    sys.exit(0)
    except:
	print >> sys.stderr, 'Problem starting daemon'
	sys.exit(1)

    while auto_restart:
	time.sleep(1)
	if not xc.domain_getinfo(current_id):
	    print "Domain %d has terminated, restarting VM in new domain" % current_id
	    current_id = make_domain()
	    print "VM started in domain %d" % current_id
