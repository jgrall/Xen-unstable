CONFIG_X86 := y
CONFIG_HVM := y
CONFIG_MIGRATE := y
CONFIG_XCUTILS := y
CONFIG_IOEMU := y
CONFIG_MBOOTPACK := y

CFLAGS += -m64
LDFLAGS += -m64
LIBDIR = lib64
