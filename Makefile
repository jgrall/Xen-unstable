#
# Grand Unified Makefile for Xen.
#

# Default is to install to local 'dist' directory.
DISTDIR ?= $(CURDIR)/dist
DESTDIR ?= $(DISTDIR)/install

INSTALL		:= install
INSTALL_DIR	:= $(INSTALL) -d -m0755
INSTALL_DATA	:= $(INSTALL) -m0644
INSTALL_PROG	:= $(INSTALL) -m0755

KERNELS ?= linux-2.6-xen0 linux-2.6-xenU
# linux-2.4-xen0 linux-2.4-xenU netbsd-2.0-xenU
# You may use wildcards in the above e.g. KERNELS=*2.4*

XKERNELS := $(foreach kernel, $(KERNELS), $(patsubst buildconfigs/mk.%,%,$(wildcard buildconfigs/mk.$(kernel))) )

export DESTDIR

# Export target architecture overrides to Xen and Linux sub-trees.
ifneq ($(XEN_TARGET_ARCH),)
SUBARCH := $(subst x86_32,i386,$(XEN_TARGET_ARCH))
export XEN_TARGET_ARCH SUBARCH
endif

# Default target must appear before any include lines
all: dist

include Config.mk
include buildconfigs/Rules.mk

ifeq ($(XEN_TARGET_X86_PAE),y)
export pae=y
endif

.PHONY:	all dist install xen kernels tools dev-docs docs world clean
.PHONY:	mkpatches mrproper kbuild kdelete kclean

# build and install everything into the standard system directories
install: install-xen install-kernels install-tools install-docs

build: kernels
	$(MAKE) -C xen build
	$(MAKE) -C tools build
	$(MAKE) -C docs build

# build and install everything into local dist directory
dist: xen kernels tools docs
	$(INSTALL_DIR) $(DISTDIR)/check
	$(INSTALL_DATA) ./COPYING $(DISTDIR)
	$(INSTALL_DATA) ./README $(DISTDIR)
	$(INSTALL_PROG) ./install.sh $(DISTDIR)
	$(INSTALL_PROG) tools/check/chk tools/check/check_* $(DISTDIR)/check

xen:
	$(MAKE) -C xen install

tools:
	$(MAKE) -C tools install

kernels:
	for i in $(XKERNELS) ; do $(MAKE) $$i-build || exit 1; done

docs:
	sh ./docs/check_pkgs && $(MAKE) -C docs install || true

dev-docs:
	$(MAKE) -C docs dev-docs

# Build all the various kernels and modules
kbuild: kernels

# Delete the kernel build trees entirely
kdelete:
	for i in $(XKERNELS) ; do $(MAKE) $$i-delete ; done

# Clean the kernel build trees
kclean:
	for i in $(XKERNELS) ; do $(MAKE) $$i-clean ; done

# Make patches from kernel sparse trees
mkpatches:
	for i in $(ALLSPARSETREES) ; do $(MAKE) $$i-xen.patch; done

# build xen, the tools, and a domain 0 plus unprivileged linux-xen images,
# and place them in the install directory. 'make install' should then
# copy them to the normal system directories
world: 
	$(MAKE) clean
	$(MAKE) kdelete
	$(MAKE) dist

# clean doesn't do a kclean
clean:: 
	$(MAKE) -C xen clean
	$(MAKE) -C tools clean
	$(MAKE) -C docs clean

# clean, but blow away kernel build tree plus tar balls
mrproper: clean
	rm -rf dist patches/tmp
	for i in $(ALLKERNELS) ; do $(MAKE) $$i-delete ; done
	for i in $(ALLSPARSETREES) ; do $(MAKE) $$i-mrproper ; done

install-logging: LOGGING=logging-0.4.9.2
install-logging:
	[ -f $(LOGGING).tar.gz ] || wget http://www.red-dove.com/$(LOGGING).tar.gz
	tar -zxf $(LOGGING).tar.gz
	cd $(LOGGING) && python setup.py install

# handy target to upgrade iptables (use rpm or apt-get in preference)
install-iptables:
	wget http://www.netfilter.org/files/iptables-1.2.11.tar.bz2
	tar -jxf iptables-1.2.11.tar.bz2
	$(MAKE) -C iptables-1.2.11 PREFIX= KERNEL_DIR=../linux-$(LINUX_VER)-xen0 install

install-%: DESTDIR=
install-%: %
	@: # do nothing

help:
	@echo 'Installation targets:'
	@echo '  install          - build and install everything'
	@echo '  install-xen      - build and install the Xen hypervisor'
	@echo '  install-tools    - build and install the control tools'
	@echo '  install-kernels  - build and install guest kernels'
	@echo '  install-docs     - build and install documentation'
	@echo ''
	@echo 'Building targets:'
	@echo '  dist             - build and install everything into local dist directory'
	@echo '  world            - clean everything, delete guest kernel build'
	@echo '                     trees then make dist'
	@echo '  xen              - build and install Xen hypervisor'
	@echo '  tools            - build and install tools'
	@echo '  kernels          - build and install guest kernels'
	@echo '  kbuild           - synonym for make kernels'
	@echo '  docs             - build and install docs'
	@echo ''
	@echo 'Cleaning targets:'
	@echo '  clean            - clean the Xen, tools and docs (but not'
	@echo '                     guest kernel) trees'
	@echo '  mrproper         - clean plus delete kernel tarballs and kernel'
	@echo '                     build trees'
	@echo '  kdelete          - delete guest kernel build trees'
	@echo '  kclean           - clean guest kernel build trees'
	@echo ''
	@echo 'Dependency installation targets:'
	@echo '  install-logging  - install the Python Logging package'
	@echo '  install-iptables - install iptables tools'
	@echo ''
	@echo 'Miscellaneous targets:'
	@echo '  mkpatches        - make patches against vanilla kernels from'
	@echo '                     sparse trees'
	@echo '  uninstall        - attempt to remove installed Xen tools (use'
	@echo '                     with extreme care!)'

# Use this target with extreme care!
uninstall: DESTDIR=
uninstall: D=$(DESTDIR)
uninstall:
	[ -d $(D)/etc/xen ] && mv -f $(D)/etc/xen $(D)/etc/xen.old-`date +%s`
	rm -rf $(D)/etc/init.d/xend*
	rm -rf $(D)/usr/$(LIBDIR)/libxc* $(D)/usr/$(LIBDIR)/libxutil*
	rm -rf $(D)/usr/$(LIBDIR)/python/xen $(D)/usr/include/xen
	rm -rf $(D)/usr/$(LIBDIR)/share/xen $(D)/usr/$(LIBDIR)/libxenstore*
	rm -rf $(D)/var/run/xen* $(D)/var/lib/xen*
	rm -rf $(D)/usr/include/xcs_proto.h $(D)/usr/include/xc.h
	rm -rf $(D)/usr/include/xs_lib.h $(D)/usr/include/xs.h
	rm -rf $(D)/usr/sbin/xcs $(D)/usr/sbin/xcsdump $(D)/usr/sbin/xen*
	rm -rf $(D)/usr/sbin/netfix
	rm -rf $(D)/usr/sbin/xfrd $(D)/usr/sbin/xm
	rm -rf $(D)/usr/share/doc/xen  $(D)/usr/man/man*/xentrace*
	rm -rf $(D)/usr/bin/xen* $(D)/usr/bin/miniterm
	rm -rf $(D)/boot/*xen*
	rm -rf $(D)/lib/modules/*xen*
	rm -rf $(D)/usr/bin/cpuperf-perfcntr $(D)/usr/bin/cpuperf-xen
	rm -rf $(D)/usr/bin/xc_shadow
	rm -rf $(D)/usr/share/xen $(D)/usr/libexec/xen
	rm -rf $(D)/usr/share/man/man1/xen*
	rm -rf $(D)/usr/share/man/man8/xen*
	rm -rf $(D)/usr/lib/xen
	rm -rf $(D)/etc/hotplug.d/xen-backend
	rm -rf $(D)/etc/hotplug/xen-backend.agent

# Legacy targets for compatibility
linux24:
	$(MAKE) 'KERNELS=linux-2.4*' kernels

linux26:
	$(MAKE) 'KERNELS=linux-2.6*' kernels

netbsd20:
	$(MAKE) netbsd-2.0-xenU-build

