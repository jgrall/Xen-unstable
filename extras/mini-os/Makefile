debug ?= y

include $(CURDIR)/../../Config.mk

# Set TARGET_ARCH
override TARGET_ARCH     := $(XEN_TARGET_ARCH)

# NB. '-Wcast-qual' is nasty, so I omitted it.
CFLAGS := -fno-builtin -Wall -Werror -Iinclude/ -Wredundant-decls -Wno-format
CFLAGS += -Wstrict-prototypes -Wnested-externs -Wpointer-arith -Winline

ifeq ($(TARGET_ARCH),x86_32)
CFLAGS += -m32 -march=i686
LDFLAGS := -m elf_i386
endif

ifeq ($(TARGET_ARCH),x86_64)
CFLAGS += -m64 -mno-red-zone -fpic -fno-reorder-blocks
CFLAGS += -fno-asynchronous-unwind-tables
LDFLAGS := -m elf_x86_64
endif

ifeq ($(debug),y)
CFLAGS += -g
else
CFLAGS += -O3
endif

TARGET := mini-os

OBJS := $(TARGET_ARCH).o
OBJS += $(patsubst %.c,%.o,$(wildcard *.c))
OBJS += $(patsubst %.c,%.o,$(wildcard lib/*.c))

HDRS := $(wildcard include/*.h)
HDRS += $(wildcard include/xen/*.h)

default: $(TARGET)

xen-public:
	[ -e include/xen ] || ln -sf ../../../xen/include/public include/xen

$(TARGET): xen-public $(OBJS)
	$(LD) -N -T minios-$(TARGET_ARCH).lds $(OBJS) -o $@.elf
	gzip -f -9 -c $@.elf >$@.gz

clean:
	find . -type f -name '*.o' | xargs rm -f
	rm -f *.o *~ core $(TARGET).elf $(TARGET).raw $(TARGET) $(TARGET).gz
	find . -type l | xargs rm -f

%.o: %.c $(HDRS) Makefile
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HDRS) Makefile
	$(CC) $(CFLAGS) -D__ASSEMBLY__ -c $< -o $@

