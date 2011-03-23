#  -*- mode: Makefile; -*-

# `all' is the default target
all:

include $(XEN_ROOT)/Config.mk

export _INSTALL := $(INSTALL)
INSTALL = $(XEN_ROOT)/tools/cross-install

XEN_INCLUDE        = $(XEN_ROOT)/tools/include
XEN_LIBXC          = $(XEN_ROOT)/tools/libxc
XEN_XENLIGHT       = $(XEN_ROOT)/tools/libxl
XEN_XENSTORE       = $(XEN_ROOT)/tools/xenstore
XEN_LIBXENSTAT     = $(XEN_ROOT)/tools/xenstat/libxenstat/src
XEN_BLKTAP2        = $(XEN_ROOT)/tools/blktap2

CFLAGS_xeninclude = -I$(XEN_INCLUDE)

CFLAGS_libxenctrl = -I$(XEN_LIBXC) $(CFLAGS_xeninclude)
LDLIBS_libxenctrl = -L$(XEN_LIBXC) -lxenctrl
SHLIB_libxenctrl  = -Wl,-rpath-link=$(XEN_LIBXC)

CFLAGS_libxenguest = -I$(XEN_LIBXC) $(CFLAGS_xeninclude)
LDLIBS_libxenguest = -L$(XEN_LIBXC) -lxenguest
SHLIB_libxenguest  = -Wl,-rpath-link=L$(XEN_LIBXC)

CFLAGS_libxenstore = -I$(XEN_XENSTORE) $(CFLAGS_xeninclude)
LDLIBS_libxenstore = -L$(XEN_XENSTORE) -lxenstore
SHLIB_libxenstore  = -Wl,-rpath-link=$(XEN_XENSTORE)

CFLAGS_libxenstat  = -I$(XEN_LIBXENSTAT)
LDLIBS_libxenstat  = $(SHLIB_libxenctrl) $(SHLIB_libxenstore) -L$(XEN_LIBXENSTAT) -lxenstat
SHLIB_libxenstat  = -Wl,-rpath-link=$(XEN_LIBXENSTAT)

ifeq ($(CONFIG_Linux),y)
LIBXL_BLKTAP = y
else
LIBXL_BLKTAP = n
endif

ifeq ($(LIBXL_BLKTAP),y)
CFLAGS_libblktapctl = -I$(XEN_BLKTAP2)/control -I$(XEN_BLKTAP2)/include $(CFLAGS_xeninclude)
LDLIBS_libblktapctl = -L$(XEN_BLKTAP2)/control -lblktapctl
SHLIB_libblktapctl  = -Wl,-rpath-link=$(XEN_BLKTAP2)/control
else
CFLAGS_libblktapctl =
LDLIBS_libblktapctl =
SHLIB_libblktapctl  =
endif

CFLAGS_libxenlight = -I$(XEN_XENLIGHT) $(CFLAGS_xeninclude)
LDLIBS_libxenlight = -L$(XEN_XENLIGHT) $(SHLIB_libxenctrl) $(SHLIB_libxenstore) $(SHLIB_libblktapctl) -lxenlight
SHLIB_libxenlight  = -Wl,-rpath-link=$(XEN_XENLIGHT)

CFLAGS += -D__XEN_TOOLS__

# Get gcc to generate the dependencies for us.
CFLAGS += -MMD -MF .$(@F).d
DEPS = .*.d

ifneq ($(XEN_OS),NetBSD)
# Enable implicit LFS support *and* explicit LFS names.
CFLAGS  += $(shell getconf LFS_CFLAGS)
CFLAGS  += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
LDFLAGS += $(shell getconf LFS_LDFLAGS)
endif

# 32-bit x86 does not perform well with -ve segment accesses on Xen.
CFLAGS-$(CONFIG_X86_32) += $(call cc-option,$(CC),-mno-tls-direct-seg-refs)
CFLAGS += $(CFLAGS-y)

# Require GCC v3.4+ (to avoid issues with alignment constraints in Xen headers)
check-$(CONFIG_X86) = $(call cc-ver-check,CC,0x030400,\
                        "Xen requires at least gcc-3.4")
$(eval $(check-y))

_PYTHON_PATH := $(shell which $(PYTHON))
PYTHON_PATH ?= $(_PYTHON_PATH)
INSTALL_PYTHON_PROG = \
	$(XEN_ROOT)/tools/python/install-wrap "$(PYTHON_PATH)" $(INSTALL_PROG)

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) $(CFLAGS_$*.opic) -fPIC -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_$*.o) -c -o $@ $<

%.o: %.cc
	$(CC) $(CPPFLAGS) $(CXXFLAGS) $(CXXFLAGS_$*.o) -c -o $@ $<

subdirs-all subdirs-clean subdirs-install subdirs-distclean: .phony
	@set -e; for subdir in $(SUBDIRS) $(SUBDIRS-y); do \
		$(MAKE) subdir-$(patsubst subdirs-%,%,$@)-$$subdir; \
	done

subdir-all-% subdir-clean-% subdir-install-%: .phony
	$(MAKE) -C $* $(patsubst subdir-%-$*,%,$@)

subdir-distclean-%: .phony
	$(MAKE) -C $* clean
