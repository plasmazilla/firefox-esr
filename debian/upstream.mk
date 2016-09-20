# Generic rules to help download sources from archive.mozilla.org.
# Define the following variables before including this file:
# PRODUCT - product codename (e.g. browser)

# The VERSION_FILTER transforms upstream version patterns to versions
# used in debian/changelog. Versions are to be transformed as follows:
# 4.0      -> 4.0
# 4.0a1    -> 4.0~a1
# 4.0b5    -> 4.0~b5
# That should ensure the proper ordering
VERSION_FILTER := sed 's/\([0-9]\)\([ab]\)/\1~\2/g'
$(call lazy,UPSTREAM_VERSION,$$(shell cat $(PRODUCT)/config/version.txt))
GRE_SRCDIR := $(strip $(foreach dir,. mozilla,$(if $(wildcard $(dir)/python/mozbuild/mozbuild/milestone.py),$(dir))))
ifndef GRE_SRCDIR
$(error Could not determine the top directory for GRE codebase)
endif
GRE_MILESTONE := $(shell $(PYTHON) $(GRE_SRCDIR)/python/mozbuild/mozbuild/milestone.py --topsrcdir $(GRE_SRCDIR) --uaversion | $(VERSION_FILTER))

# Construct GRE_VERSION from the first digit in GRE_MILESTONE
GRE_VERSION := $(subst ~, ,$(subst ., ,$(GRE_MILESTONE)))
export JS_SO_VERSION := $(firstword $(GRE_VERSION))d
export GRE_VERSION := $(firstword $(GRE_VERSION))

# Last version in debian/changelog
DEBIAN_SRC_VERSION := $(shell dpkg-parsechangelog | sed -n 's/^\(Source\|Version\): *// p')
DEBIAN_SOURCE := $(firstword $(DEBIAN_SRC_VERSION))
DEBIAN_VERSION := $(word 2, $(DEBIAN_SRC_VERSION))
# Debian part of the above version (anything after the last dash)
DEBIAN_RELEASE := $(lastword $(subst -, ,$(DEBIAN_VERSION)))
# Upstream part of the debian/changelog version (anything before the last dash)
UPSTREAM_RELEASE := $(DEBIAN_VERSION:%-$(DEBIAN_RELEASE)=%)
# Aurora builds have the build id in the upstream part of the debian/changelog version
export MOZ_BUILD_DATE := $(word 2,$(subst +, ,$(UPSTREAM_RELEASE)))
ifndef MOZ_BUILD_DATE
export MOZ_BUILD_DATE := $(shell TZ=UTC date -d "$(shell dpkg-parsechangelog -S Date)" +%Y%m%d%H%M%S)
endif
UPSTREAM_RELEASE := $(firstword $(subst +, ,$(UPSTREAM_RELEASE)))
# If the debian part of the version contains ~bpo or ~deb, it's a backport
DEBIAN_RELEASE_EXTRA := $(word 2,$(subst ~, ,$(DEBIAN_RELEASE)))
ifneq (,$(filter bpo% deb%,$(DEBIAN_RELEASE_EXTRA)))
BACKPORT = 1
DEBIAN_TARGET := $(subst bpo,,$(subst deb,,$(DEBIAN_RELEASE_EXTRA)))
ifneq (,$(filter 7%,$(DEBIAN_TARGET)))
BACKPORT = wheezy
endif
ifneq (,$(filter 8%,$(DEBIAN_TARGET)))
BACKPORT = jessie
endif
ifneq (,$(filter 9%,$(DEBIAN_TARGET)))
BACKPORT = stretch
endif
endif

PRODUCT_NAME := $(DEBIAN_SOURCE)

# Check if the version in debian/changelog matches actual upstream version
# as VERSION_FILTER transforms it.
FILTERED_UPSTREAM_VERSION := $(shell echo $(UPSTREAM_VERSION) | $(VERSION_FILTER))
ifneq ($(FILTERED_UPSTREAM_VERSION),$(subst esr,,$(firstword $(subst ~b, ,$(UPSTREAM_RELEASE)))))
$(error Upstream version in debian/changelog ($(UPSTREAM_RELEASE)) does not match actual upstream version ($(FILTERED_UPSTREAM_VERSION)))
endif

VERSION = $(UPSTREAM_RELEASE)
$(call lazy,SOURCE_TARBALL_EXT,$$(shell sed -n '/^SOURCE_TAR/s/.*\.tar\.//p' toolkit/mozapps/installer/upload-files.mk))
SOURCE_TARBALL = $(DEBIAN_SOURCE)_$(VERSION)$(SOURCE_BUILD_DATE:%=+%).orig.tar.$(SOURCE_TARBALL_EXT)
SOURCE_TARBALL_LOCATION = ..

SOURCE_VERSION = $(subst ~,,$(VERSION))

# Find the right channel corresponding to the version number
ifneq (,$(filter suite mail calendar,$(PRODUCT)))
REPO_PREFIX = comm
else
REPO_PREFIX = mozilla
endif
ifneq (,$(findstring esr, $(VERSION)))
SOURCE_TYPE := releases
SHORT_SOURCE_CHANNEL := esr$(firstword $(subst ., ,$(VERSION)))
SHORT_L10N_CHANNEL := release
else
ifneq (,$(findstring ~b, $(VERSION)))
# Betas are under releases/
SOURCE_TYPE := releases
SHORT_SOURCE_CHANNEL := beta
else
ifneq (,$(filter %~a2, $(VERSION)))
# Aurora
SOURCE_TYPE := nightly
SHORT_SOURCE_CHANNEL := aurora
DOWNLOAD_SOURCE := aurora
else
ifneq (,$(filter %~a1, $(VERSION)))
# Nightly
SOURCE_TYPE := nightly
SHORT_SOURCE_CHANNEL := central
DOWNLOAD_SOURCE := nightly
L10N_REPO := https://hg.mozilla.org/l10n-central
else
# Release
SOURCE_TYPE := releases
SHORT_SOURCE_CHANNEL := release
endif
endif
endif
endif
SOURCE_CHANNEL = $(REPO_PREFIX)-$(SHORT_SOURCE_CHANNEL)
ifndef SHORT_L10N_CHANNEL
SHORT_L10N_CHANNEL := $(SHORT_SOURCE_CHANNEL)
endif

PRODUCT_DOWNLOAD_NAME := $(firstword $(subst -, ,$(PRODUCT_NAME)))

BASE_URL = https://archive.mozilla.org/pub/$(PRODUCT_DOWNLOAD_NAME)/$(SOURCE_TYPE)

L10N_FILTER = awk '(NF == 1 || /linux/) && $$1 != "en-US" { print $$1 }'
$(call lazy,L10N_LANGS,$$(shell $$(L10N_FILTER) $(PRODUCT)/locales/shipped-locales))
ifeq ($(SOURCE_TYPE),releases)
SOURCE_URL = $(BASE_URL)/$(SOURCE_VERSION)/source/$(PRODUCT_DOWNLOAD_NAME)-$(SOURCE_VERSION).source.tar.$(SOURCE_TARBALL_EXT)
SOURCE_REV = $(call uc,$(PRODUCT_DOWNLOAD_NAME))_$(subst .,_,$(SOURCE_VERSION))_RELEASE
CANDIDATE_BASE_URL = http://archive.mozilla.org/pub/$(PRODUCT_DOWNLOAD_NAME)/candidates/$(SOURCE_VERSION)-candidates
CANDIDATE = $(shell curl -s $(CANDIDATE_BASE_URL)/ | sed -n '/href.*build/s/.*>\(build[0-9]*\)\/<.*/\1/p' | tail -1)
$(call lazy,L10N_CHANGESETS,$$(shell curl -s $(CANDIDATE_BASE_URL)/$$(CANDIDATE)/l10n_changesets.txt | sed 's/ /:/'))
L10N_REV = $(subst $1:,,$(filter $1:%,$(L10N_CHANGESETS)))
SOURCE_REPO = https://hg.mozilla.org/releases/$(SOURCE_CHANNEL)
else
ifeq ($(SOURCE_TYPE),nightly)
SOURCE_TARBALL_EXT = bz2
$(call lazy,LATEST_NIGHTLY,$$(shell $$(PYTHON) debian/latest_nightly.py $(PRODUCT_DOWNLOAD_NAME)-$(DOWNLOAD_SOURCE)))
$(call lazy,SOURCE_BUILD_VERSION,$$(shell echo $$(firstword $$(LATEST_NIGHTLY)) | $$(VERSION_FILTER)))
SOURCE_BUILD_DATE = $(word 2, $(LATEST_NIGHTLY))
SOURCE_URL = $(subst /rev/,/archive/,$(word 3, $(LATEST_NIGHTLY))).tar.bz2
SOURCE_REV = $(patsubst %.tar.bz2,%,$(notdir $(SOURCE_URL)))
L10N_REV = tip
SOURCE_REPO = $(patsubst %/,%,$(dir $(patsubst %/,%,$(dir $(SOURCE_URL)))))
endif
endif

L10N_REPO ?= $(subst $(SOURCE_CHANNEL),l10n/mozilla-$(SHORT_L10N_CHANNEL),$(SOURCE_REPO))

ifneq (,$(filter dump dump-% import download,$(MAKECMDGOALS)))
ifneq (,$(filter-out $(VERSION),$(UPSTREAM_RELEASE))$(filter $(SOURCE_CHANNEL),aurora central))
$(call lazy,L10N_LANGS,$$(shell curl -s $(SOURCE_REPO)/raw-file/$(SOURCE_REV)/$(PRODUCT)/locales/shipped-locales | $$(L10N_FILTER)))
$(call lazy,SOURCE_TARBALL_EXT,$$(shell curl -s $(SOURCE_REPO)/raw-file/$(SOURCE_REV)/toolkit/mozapps/installer/upload-files.mk | sed -n '/^SOURCE_TAR/s/.*\.tar\.//p'))
endif
L10N_TARBALLS = $(foreach lang,$(L10N_LANGS),$(SOURCE_TARBALL_LOCATION)/$(SOURCE_TARBALL:%.orig.tar.$(SOURCE_TARBALL_EXT)=%.orig-l10n-$(lang).tar.bz2))

ALL_TARBALLS = $(SOURCE_TARBALL_LOCATION)/$(SOURCE_TARBALL) $(L10N_TARBALLS)

download: $(ALL_TARBALLS)

import: $(ALL_TARBALLS)
	debian/import-tar.py $(addprefix -H ,$(BRANCH)) $< | git fast-import

$(SOURCE_TARBALL_LOCATION)/$(SOURCE_TARBALL): debian/source.filter
	$(if $(filter-out $(VERSION),$(SOURCE_BUILD_VERSION)),$(error Downloaded version ($(SOURCE_BUILD_VERSION)) does not match requested version ($(VERSION))))
	debian/repack.py -o $@ $(SOURCE_URL)

$(L10N_TARBALLS): $(SOURCE_TARBALL_LOCATION)/$(SOURCE_TARBALL:%.orig.tar.$(SOURCE_TARBALL_EXT)=%.orig-l10n-%.tar.bz2): debian/l10n.filter
	debian/repack.py -o $@ -t $* -f debian/l10n.filter $(L10N_REPO)/$*/archive/$(call L10N_REV,$*).tar.bz2
endif
.PHONY: download
