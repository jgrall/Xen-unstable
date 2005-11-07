#!/usr/bin/python

# Copyright (C) International Business Machines Corp., 2005
# Authors: Dan Smith <danms@us.ibm.com>

from XmTestLib import *

import time

DOMS=2
MEM=32
DUR=60

domains = []

for i in range(0,DOMS):
    dom = XmTestDomain(extraOpts={"memory" : str(MEM)})

    try:
        dom.start()
    except DomainError, e:
        if verbose:
            print str(e)
        FAIL("Failed to start %s" % dom.getName())

    try:
        cons = XmConsole(dom.getName())
        cons.sendInput("foo")
    except ConsoleError, e:
        FAIL(str(e))

    if verbose:
        print "[%i/%i] Started %s" % (i, DOMS, dom.getName())

    domains.append([dom, cons])

# Started DOMS domains, now we put them to work

for d, c in domains:
    if verbose:
        print "Starting task on %s" % d.getName()
    c.sendInput("gzip -c </dev/zero >/dev/null &\n")

if verbose:
    print "Waiting %i seconds..." % DUR

time.sleep(DUR)

for d, c in domains:

    if verbose:
        print "Testing domain %s..." % d.getName()
    
    run = c.runCmd("ls")

    if run["return"] != 0:
        FAIL("Domain %s didn't survive!" % d.getName())

        
