#  -*- mode: Makefile; -*-

include $(XEN_ROOT)/Config.mk

XEN_XC             = $(XEN_ROOT)/tools/python/xen/lowlevel/xc
XEN_LIBXC          = $(XEN_ROOT)/tools/libxc
XEN_XENSTORE       = $(XEN_ROOT)/tools/xenstore
XEN_LIBXENSTAT     = $(XEN_ROOT)/tools/xenstat/libxenstat/src

ifeq ($(XEN_TARGET_ARCH),x86_32)
CFLAGS  += -m32 -march=i686
LDFLAGS += -m32
endif

ifeq ($(XEN_TARGET_ARCH),x86_64)
CFLAGS  += -m64
LDFLAGS += -m64
endif

X11_LDPATH = -L/usr/X11R6/$(LIBDIR)

%.opic: %.c
	$(CC) $(CPPFLAGS) -DPIC $(CFLAGS) -fPIC -c -o $@ $<

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

%.o: %.cc
	$(CC) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<
