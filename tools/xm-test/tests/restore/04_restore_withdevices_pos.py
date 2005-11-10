#!/usr/bin/python

# Copyright (C) International Business Machines Corp., 2005
# Author: Dan Smith <danms@us.ibm.com>

from XmTestLib import *

import re

domain = XmTestDomain(extraOpts={"nics":2})

domain.configAddDisk("phy:/dev/ram0", "hda1", "w")
domain.configAddDisk("phy:/dev/ram1", "hdb2", "w")

s, o = traceCommand("mke2fs -q /dev/ram0")
if s != 0:
    FAIL("Unable to mke2fs /dev/ram0 in dom0")

s, o = traceCommand("mke2fs -q /dev/ram1")
if s != 0:
    FAIL("Unable to mke2fs /dev/ram1 in dom0")

try:
    domain.start()
except DomainError, e:
    FAIL(str(e))

try:
    console = XmConsole(domain.getName())
    console.sendInput("foo")

    run = console.runCmd("mkdir /mnt/a /mnt/b")
    if run["return"] != 0:
        FAIL("Unable to mkdir /mnt/a /mnt/b")

    run = console.runCmd("mount /dev/hda1 /mnt/a")
    if run["return"] != 0:
        FAIL("Unable to mount /dev/hda1")

    run = console.runCmd("mount /dev/hdb2 /mnt/b")
    if run["return"] != 0:
        FAIL("Unable to mount /dev/hdb2")

    run = console.runCmd("echo hda1 > /mnt/a/foo")
    if run["return"] != 0:
        FAIL("Unable to write to block device hda1!")

    run = console.runCmd("echo hdb2 > /mnt/b/foo")
    if run["return"] != 0:
        FAIL("Unable to write to block device hdb2!")

    run = console.runCmd("ifconfig eth0 169.254.0.1 netmask 255.255.255.0")
    if run["return"] != 0:
        FAIL("Unable to configure DomU's eth0")

    run = console.runCmd("ifconfig eth1 169.254.1.1 netmask 255.255.255.0")
    if run["return"] != 0:
        FAIL("Unable to configure DomU's eth1")

    run = console.runCmd("ifconfig lo 127.0.0.1")
    if run["return"] != 0:
        FAIL("Unable to configure DomU's lo")


except ConsoleError, e:
    FAIL(str(e))

console.closeConsole()

try:
    s, o = traceCommand("xm save %s /tmp/test.state" % domain.getName(),
                        timeout=30)
except TimeoutError, e:
    FAIL(str(e))

if s != 0:
    FAIL("xm save exited with %i != 0" % s)

# Let things settle
time.sleep(15)

try:
    s, o = traceCommand("xm restore /tmp/test.state",
                        timeout=30)
except TimeoutError, e:
    FAIL(str(e))

if s != 0:
    FAIL("xm restore exited with %i != 0" % s)

try:
    console = XmConsole(domain.getName())

    run = console.runCmd("ls | grep proc")
    if run["return"] != 0:
        FAIL("ls failed on restored domain")
    
    run = console.runCmd("cat /mnt/a/foo")
    if run["return"] != 0:
        FAIL("Unable to read from block device hda1")
    if not re.search("hda1", run["output"]):
        FAIL("Failed to read correct data from hda1")

    run = console.runCmd("cat /mnt/b/foo")
    if run["return"] != 0:
        FAIL("Unable to read from block device hdb2")
    if not re.search("hdb2", run["output"]):
        FAIL("Failed to read correct data from hdb2")

    run = console.runCmd("ifconfig")
    if not re.search("eth0", run["output"]):
        FAIL("DomU's eth0 disappeared")
    if not re.search("169.254.0.1", run["output"]):
        FAIL("DomU's eth0 lost its IP")
    if not re.search("eth1", run["output"]):
        FAIL("DomU's eth1 disappeared")
    if not re.search("169.254.1.1", run["output"]):
        FAIL("DomU's eth1 lost its IP")
    if not re.search("Loopback", run["output"]):
        FAIL("DomU's lo disappeared")
    if not re.search("127.0.0.1", run["output"]):
        FAIL("DomU's lo lost its IP")

except ConsoleError, e:
    FAIL(str(e))
        
