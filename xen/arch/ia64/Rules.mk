########################################
# ia64-specific definitions

HAS_ACPI := y
HAS_VGA  := y
VALIDATE_VT	?= n
no_warns ?= n

ifneq ($(COMPILE_ARCH),$(TARGET_ARCH))
CROSS_COMPILE ?= /usr/local/sp_env/v2.2.5/i686/bin/ia64-unknown-linux-
endif

# Used only by linux/Makefile.
AFLAGS_KERNEL  += -mconstant-gp -nostdinc $(CPPFLAGS)

CFLAGS	+= -nostdinc -fno-builtin -fno-common -fno-strict-aliasing
CFLAGS	+= -mconstant-gp
#CFLAGS  += -O3		# -O3 over-inlines making debugging tough!
CFLAGS	+= -O2		# but no optimization causes compile errors!
CFLAGS	+= -fomit-frame-pointer -D__KERNEL__
CFLAGS	+= -iwithprefix include
CPPFLAGS+= -I$(BASEDIR)/include						\
	   -I$(BASEDIR)/include/asm-ia64				\
	   -I$(BASEDIR)/include/asm-ia64/linux 				\
	   -I$(BASEDIR)/include/asm-ia64/linux-xen 			\
	   -I$(BASEDIR)/include/asm-ia64/linux-null 			\
	   -I$(BASEDIR)/arch/ia64/linux -I$(BASEDIR)/arch/ia64/linux-xen
CFLAGS	+= $(CPPFLAGS)
#CFLAGS  += -Wno-pointer-arith -Wredundant-decls
CFLAGS	+= -DIA64 -DXEN -DLINUX_2_6 -DV_IOSAPIC_READY
CFLAGS	+= -ffixed-r13 -mfixed-range=f2-f5,f12-f127
CFLAGS	+= -g
#CFLAGS  += -DVTI_DEBUG
ifeq ($(VALIDATE_VT),y)
CFLAGS  += -DVALIDATE_VT
endif
ifeq ($(no_warns),y)
CFLAGS	+= -Wa,--fatal-warnings -Werror -Wno-uninitialized
endif

LDFLAGS := -g

# Additionnal IA64 include dirs.
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/asm/sn/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-null/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/asm/sn/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux-xen/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/asm-generic/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/asm/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/linux/byteorder/*.h)
HDRS += $(wildcard $(BASEDIR)/include/asm-ia64/hvm/*.h)
