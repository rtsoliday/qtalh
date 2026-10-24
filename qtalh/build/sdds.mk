# Optional SDDS: try complete installations in priority order. Explicit include
# or library overrides are authoritative, including nonexistent paths (useful
# for building/testing without SDDS). Never borrow a core library from another
# installation. The link probe is compiled, never executed, and stays quiet.
SDDS_REPO ?= $(abspath $(RULES_DIR)../SDDS)
ifneq ($(strip $(SDDS_INCLUDE_DIR) $(SDDS_LIB_DIR)),)
SDDS_CANDIDATES := explicit
else
SDDS_CANDIDATES := sibling extensions aps
endif
sdds_include_explicit = $(SDDS_INCLUDE_DIR)
sdds_library_explicit = $(SDDS_LIB_DIR)
sdds_include_sibling = $(SDDS_REPO)/include
sdds_library_sibling = $(SDDS_REPO)/lib/$(OS)-$(ARCH)
sdds_include_extensions = $(or $(EXTENSIONS_INCLUDE_DIR),$(EPICS_INC_DIR))
sdds_library_extensions = $(or $(EXTENSIONS_LIB_DIR),$(EPICS_LIB_DIR))
sdds_include_aps = /usr/local/epics/extensions/include
sdds_library_aps = /usr/local/epics/extensions/lib/$(EPICS_HOST_ARCH)
sdds_lib = $(if $(strip $(2)),$(if $(filter Windows,$(OS)),$(wildcard $(2)/$(1).lib),$(call find_lib,$(1),a dylib so,$(2))))
sdds_files = $(foreach lib,SDDS1 rpnlib mdbmth mdblib,$(call sdds_lib,$(lib),$(sdds_library_$(1)))) \
             $(or $(call sdds_lib,lzma,$(sdds_library_$(1))),$(LZMA_LIB)) \
             $(or $(call sdds_lib,z,$(sdds_library_$(1))),$(Z_LIB))
sdds_includes = -I"$(sdds_include_$(1))" $(if $(filter Windows,$(OS)),-I"$(sdds_include_$(1))/../lzma")
sdds_windows_gsl = $(foreach lib,gsl gslcblas,$(or $(call sdds_lib,$(lib),$(sdds_library_$(1))),$(call sdds_lib,$(lib),$(WINDOWS_GSL_LIB_DIR))))
sdds_links = $(foreach lib,$(call sdds_files,$(1)),"$(lib)") \
             $(if $(filter Windows,$(OS)),$(foreach lib,$(call sdds_windows_gsl,$(1)),"$(lib)") /LIBPATH:"$(sdds_library_$(1))" $(LIB_LINK_DIRS),$(GSL_LINK_LIBS) -lm $(call epics_rpath,$(filter $(sdds_library_$(1))/%,$(call sdds_files,$(1))),$(sdds_library_$(1))))
ifeq ($(OS),Windows)
sdds_probe_command = $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(call sdds_includes,$(1)) build/sdds_link_probe.cc /Fo"$(OBJ_DIR)/sdds-probe.obj" /Fe"$(OBJ_DIR)/sdds-probe.exe" /link $(LDFLAGS) $(call sdds_links,$(1))
else
sdds_probe_command = $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(call sdds_includes,$(1)) build/sdds_link_probe.cc $(LDFLAGS) -o "$(OBJ_DIR)/sdds-probe" $(call sdds_links,$(1))
endif
sdds_probe = $(if $(and $(wildcard $(sdds_include_$(1))/SDDS.h),$(call sdds_lib,SDDS1,$(sdds_library_$(1)))),$(shell mkdir -p "$(OBJ_DIR)"; $(call sdds_probe_command,$(1)) > "$(OBJ_DIR)/sdds-probe-$(1).log" 2>&1 && echo yes))
sdds_first_usable = $(if $(strip $(1)),$(if $(call sdds_probe,$(firstword $(1))),$(firstword $(1)),$(call sdds_first_usable,$(wordlist 2,$(words $(1)),$(1)))))
# Probe on each invocation so replaced headers/libraries and toolchain changes
# are recognized without a clean build, including recovery from a failed probe.
# Cleanup alone needs no probe; `make clean all` still builds with SDDS.
ifneq ($(BUILD_GOALS),)
SDDS_SELECTED := $(call sdds_first_usable,$(SDDS_CANDIDATES))
endif
ifneq ($(SDDS_SELECTED),)
SDDS_LINK_LIBS := $(call sdds_links,$(SDDS_SELECTED))
SDDS_DEPENDENCIES := $(call sdds_files,$(SDDS_SELECTED)) $(if $(filter Windows,$(OS)),$(call sdds_windows_gsl,$(SDDS_SELECTED)),$(wildcard $(filter-out -%,$(GSL_LINK_LIBS))))
SDDS_HEADERS := $(wildcard $(sdds_include_$(SDDS_SELECTED))/*.h)
CPPFLAGS += -DQTALH_HAVE_SDDS
CPPFLAGS += $(call sdds_includes,$(SDDS_SELECTED))
endif
