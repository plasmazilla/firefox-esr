include debian/make.mk

PYTHON := python -B
PRODUCT := browser

include debian/upstream.mk

ARCHES := amd64 i386
DPKG_ARCHES := $(shell dpkg --print-architecture) $(shell dpkg --print-foreign-architectures)

ifneq ($(ARCHES),$(filter $(ARCHES),$(DPKG_ARCHES)))
$(foreach arch,$(filter-out $(filter $(ARCHES),$(DPKG_ARCHES)),$(ARCHES)),$(error Please run `dpkg --add-architecture $(arch)`))
endif

.DEFAULT_GOAL = symbols

PACKAGE_NAME = $(PRODUCT_NAME)
PACKAGE_VERSION = $(DEBIAN_VERSION)

define download_package
$1_$2_$3.deb: PACKAGE=$1
$1_$2_$3.deb: ARCH=$3
PACKAGES += $1_$2_$3.deb

endef

define define_package
$(foreach arch,$(ARCHES),$(call download_package,$1,$(lastword $(subst :,%3a,$(PACKAGE_VERSION))),$(arch)))
endef

$(eval $(call define_package,$(PACKAGE_NAME)))
ifeq (,$(filter $(BACKPORT),wheezy jessie))
DBG=dbgsym
DBGTYPE=buildid
else
DBG=dbg
DBGTYPE=dbg
endif
$(eval $(call define_package,$(PACKAGE_NAME)-$(DBG)))

export APT_CONFIG=$(CURDIR)/debian/symbols.apt.conf
apt-tmp:
	mkdir -p apt-tmp/config/apt.conf.d apt-tmp/config/preferences.d apt-tmp/dpkg apt-tmp/lists/lists/partial
	touch apt-tmp/dpkg/status
	apt-get update

$(PACKAGES): apt-tmp
	apt-get download $(PACKAGE):$(ARCH)=$(PACKAGE_VERSION)

NON_DEBUG_PACKAGES := $(filter $(PACKAGE_NAME)_%,$(PACKAGES))

define CR


endef

$(NON_DEBUG_PACKAGES:%=%.x): $(PACKAGE_NAME)_%.x: $(PACKAGE_NAME)_% $(PACKAGE_NAME)-$(DBG)_%
	$(foreach deb,$^,dpkg-deb -x $(deb) $@$(CR))
	@touch $@

MOZ_OBJDIR = build-breakpad
export MOZCONFIG=$(CURDIR)/$(MOZ_OBJDIR)/mozconfig

$(MOZ_OBJDIR)/mozconfig:
	mkdir -p $(MOZ_OBJDIR)
	@echo mk_add_options MOZ_OBJDIR=$(MOZ_OBJDIR) > $@

$(MOZ_OBJDIR)/config.status: $(MOZ_OBJDIR)/mozconfig
	$(CURDIR)/mach configure
	$(CURDIR)/mach build pre-export export

$(MOZ_OBJDIR)/dist/host/bin/dump_syms: $(MOZ_OBJDIR)/config.status
	$(CURDIR)/mach build -C . toolkit/crashreporter/google-breakpad/src/tools/linux/dump_syms/host

$(MOZ_OBJDIR)/dist/bin/fileid: $(MOZ_OBJDIR)/config.status
	$(CURDIR)/mach build -C . testing/tools/fileid/target

ifneq (undefined,$(origin SYMBOL_FILE))
$(MAKECMDGOALS): syms/%: $(SYMBOL_FILE) $(MOZ_OBJDIR)/dist/host/bin/dump_syms
	@mkdir -p $(@D)
	$(MOZ_OBJDIR)/dist/host/bin/dump_syms $< > $@
endif

ifneq (undefined,$(origin SYMBOL_FILES))
FILE_ID = $(shell $(MOZ_OBJDIR)/dist/bin/fileid $1)
BUILD_ID = $(shell LANG=C readelf --notes $1 | awk '/Build ID/ {print substr($$3, 1, 2) "/" substr($$3, 3)}')
ifeq ($(DBGTYPE),buildid)
DBG_FILE = .build-id/$(call BUILD_ID, $1).debug
else
DBG_FILE = $(subst $(NULL) $(NULL),/,$(wordlist 2,$(words $(subst /, ,$1)),$(subst /, ,$1)))
endif

$(SYMBOL_FILES:%=%.dbg): %.dbg: %
	objcopy --decompress-debug-sections $(firstword $(subst /, ,$<))/usr/lib/debug/$(call DBG_FILE, $*) $@

$(SYMBOL_FILES:%=dump-%.dbg): dump-%.dbg: %.dbg $(MOZ_OBJDIR)/dist/bin/fileid
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) syms/$(notdir $*)/$(call FILE_ID,$*.dbg)/$(notdir $*).sym SYMBOL_FILE="$<"

symbols: $(SYMBOL_FILES:%=dump-%.dbg)

else
symbols: $(NON_DEBUG_PACKAGES:%=%.x)
	find $(^:%=%/usr/lib) -path '*/usr/lib/debug' -prune -o \( -type f \( -name firefox -o -name plugin-container -o -name lib\*.so \) -exec sh -c "file -b --mime-type {} | grep -q application/" \; -print \)
	$(MAKE) -f $(firstword $(MAKEFILE_LIST)) symbols SYMBOL_FILES="$(shell find $(^:%=%/usr/lib) -path '*/usr/lib/debug' -prune -o \( -type f \( -name firefox -o -name plugin-container -o -name lib\*.so \) -exec sh -c "file -b --mime-type {} | grep -q application/" \; -print \))"
endif

syms.zip:
	(cd syms; zip -rmD ../syms.zip .)

upload: syms.zip
	curl -X POST -H 'Auth-Token: $(API_TOKEN)' --form syms.zip=@syms.zip https://crash-stats.mozilla.com/symbols/upload
