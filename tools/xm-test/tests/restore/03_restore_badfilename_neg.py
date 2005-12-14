#!/usr/bin/python

# Copyright (C) International Business Machines Corp., 2005
# Author: Li Ge <lge@us.ibm.com> 

# Test description:
# Negative Test:
# Test for restoring domain with non existent file.
# Verify fail.

import re

from XmTestLib import *

if ENABLE_VMX_SUPPORT:
    SKIP("Restore currently not supported for VMX domains")

status, output = traceCommand("xm restore /tmp/NON_EXIST")
eyecatcher1 = "Error:"
eyecatcher2 = "Traceback"
where1 = output.find(eyecatcher1)
where2 = output.find(eyecatcher2)
if status == 0:
    FAIL("xm restore returned bad status, expected non 0, status is: %i" % status)
elif where2 == 0:
    FAIL("xm restore returned a stack dump, expected nice error message") 
elif where1 == -1:
    FAIL("xm restore returned bad output, expected Error:, output is: %s" % output)
