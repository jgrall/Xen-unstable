
verbose     ?= n
debug       ?= n
debugger    ?= n
perfc       ?= n
trace       ?= n
optimize    ?= y

# Currently supported architectures: x86_32, x86_64
COMPILE_ARCH    ?= $(shell uname -m | sed -e s/i.86/x86_32/)
TARGET_ARCH     ?= $(COMPILE_ARCH)

# Set ARCH/SUBARCH appropriately.
COMPILE_SUBARCH := $(COMPILE_ARCH)
TARGET_SUBARCH  := $(COMPILE_ARCH)
COMPILE_ARCH    := $(patsubst x86%,x86,$(COMPILE_ARCH))
TARGET_ARCH     := $(patsubst x86%,x86,$(TARGET_ARCH))

TARGET  := $(BASEDIR)/xen
HDRS    := $(wildcard $(BASEDIR)/include/xen/*.h)
HDRS    += $(wildcard $(BASEDIR)/include/scsi/*.h)
HDRS    += $(wildcard $(BASEDIR)/include/public/*.h)
HDRS    += $(wildcard $(BASEDIR)/include/asm-$(TARGET_ARCH)/*.h)
HDRS    += $(wildcard $(BASEDIR)/include/asm-$(TARGET_ARCH)/$(TARGET_SUBARCH)/*.h)
# compile.h is always regenerated, but other files shouldn't be rebuilt
HDRS    := $(subst $(BASEDIR)/include/xen/banner.h,,$(HDRS))
HDRS    := $(subst $(BASEDIR)/include/xen/compile.h,,$(HDRS))

C_SRCS  := $(wildcard *.c)
S_SRCS  := $(wildcard *.S)
OBJS    := $(patsubst %.S,%.o,$(S_SRCS))
OBJS    += $(patsubst %.c,%.o,$(C_SRCS))

# Note that link order matters!
ALL_OBJS := $(BASEDIR)/common/common.o
ALL_OBJS += $(BASEDIR)/drivers/char/driver.o
ALL_OBJS += $(BASEDIR)/drivers/acpi/driver.o
ALL_OBJS += $(BASEDIR)/drivers/pci/driver.o
ALL_OBJS += $(BASEDIR)/arch/$(TARGET_ARCH)/arch.o

HOSTCC     = gcc
HOSTCFLAGS = -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer 

test-gcc-flag = $(shell gcc -v --help 2>&1 | grep -q " $(1) " && echo $(1))

include $(BASEDIR)/arch/$(TARGET_ARCH)/Rules.mk

ifneq ($(debug),y)
CFLAGS += -DNDEBUG
ifeq ($(verbose),y)
CFLAGS += -DVERBOSE
endif
else
CFLAGS += -DVERBOSE
endif

ifeq ($(debugger),y)
CFLAGS += -DXEN_DEBUGGER
endif

ifeq ($(perfc),y)
CFLAGS += -DPERF_COUNTERS
endif

ifeq ($(trace),y)
CFLAGS += -DTRACE_BUFFER
endif

CFLAGS := $(strip $(CFLAGS))

%.o: %.c $(HDRS) Makefile
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HDRS) Makefile
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -c $< -o $@

