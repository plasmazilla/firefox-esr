
ifeq ($(USE_SHORT_LIBNAME), 1)
PROGRAM = $(MOZ_APP_NAME)$(BIN_SUFFIX)
else
PROGRAM = $(MOZ_APP_NAME)-bin$(BIN_SUFFIX)
endif

TARGET_DIST = $(TARGET_DEPTH)/dist

ifeq ($(MOZ_BUILD_APP),camino)
browser_path = \"$(TARGET_DIST)/Camino.app/Contents/MacOS/Camino\"
else
ifeq ($(OS_ARCH),Darwin)
ifdef MOZ_DEBUG
browser_path = \"$(TARGET_DIST)/$(MOZ_APP_DISPLAYNAME)Debug.app/Contents/MacOS/$(PROGRAM)\"
else
browser_path = \"$(TARGET_DIST)/$(MOZ_APP_DISPLAYNAME).app/Contents/MacOS/$(PROGRAM)\"
endif
else
browser_path = \"$(TARGET_DIST)/bin/$(PROGRAM)\"
endif
endif

_PROFILE_DIR = $(TARGET_DEPTH)/_profile/pgo
_SYMBOLS_PATH = $(TARGET_DIST)/crashreporter-symbols

ifneq (,$(filter /%,$(MOZILLA_DIR)))
# $(MOZILLA_DIR) is already an absolute pathname.
ABSOLUTE_TOPSRCDIR = $(MOZILLA_DIR)
else
# $(MOZILLA_DIR) is a relative pathname: prepend the current directory.
ABSOLUTE_TOPSRCDIR = $(CURDIR)/$(MOZILLA_DIR)
endif
_CERTS_SRC_DIR = $(ABSOLUTE_TOPSRCDIR)/build/pgo/certs

AUTOMATION_PPARGS = 	\
			-DBROWSER_PATH=$(browser_path) \
			-DXPC_BIN_PATH=\"$(LIBXUL_DIST)/bin\" \
			-DBIN_SUFFIX=\"$(BIN_SUFFIX)\" \
			-DPROFILE_DIR=\"$(_PROFILE_DIR)\" \
			-DCERTS_SRC_DIR=\"$(_CERTS_SRC_DIR)\" \
			-DSYMBOLS_PATH=\"$(_SYMBOLS_PATH)\" \
			$(NULL)

ifeq ($(OS_ARCH),Darwin)
AUTOMATION_PPARGS += -DIS_MAC=1
else
AUTOMATION_PPARGS += -DIS_MAC=0
endif

ifeq ($(MOZ_BUILD_APP),camino)
AUTOMATION_PPARGS += -DIS_CAMINO=1
else
AUTOMATION_PPARGS += -DIS_CAMINO=0
endif

ifeq ($(host_os), cygwin)
AUTOMATION_PPARGS += -DIS_CYGWIN=1
endif

ifeq ($(ENABLE_TESTS), 1)
AUTOMATION_PPARGS += -DIS_TEST_BUILD=1
else
AUTOMATION_PPARGS += -DIS_TEST_BUILD=0
endif

ifeq ($(MOZ_DEBUG), 1)
AUTOMATION_PPARGS += -DIS_DEBUG_BUILD=1
else
AUTOMATION_PPARGS += -DIS_DEBUG_BUILD=0
endif

automation.py: $(MOZILLA_DIR)/build/automation.py.in $(MOZILLA_DIR)/build/automation-build.mk
	$(PYTHON) $(MOZILLA_DIR)/config/Preprocessor.py \
	$(AUTOMATION_PPARGS) $(DEFINES) $(ACDEFINES) $< > $@
