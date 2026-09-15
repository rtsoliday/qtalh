.DEFAULT_GOAL := all
QTALH_GUI := 1
include ../Makefile.rules
ifeq ($(OS),Windows)
CORE = core/analytics.cc core/model.cc core/config.cc core/engine.cc core/notifications.cc
SERVICES = services/analytics.cc services/notifications.cc services/log_browser.cc services/channel_access.cc services/options.cc services/logging.cc services/ipc.cc
GUI = ui/analytics.cc ui/notifications.cc ui/appearance.cc ui/workflows.cc ui/window.cc ui/alarm_view.cc ui/dialogs.cc
OBJECTS = $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.obj) $(SERVICES:.cc=.obj) $(GUI:.cc=.obj)) $(OBJ_DIR)/resources.obj
# MSVC does not emit GCC-style .d files. Rebuild on project header changes.
HEADERS = $(wildcard core/*.h services/*.h ui/*.h)
LIBS = advapi32.lib $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(QT_LIBS) $(LDLIBS)

.PHONY: all clean distclean test test-core test-ui test-ioc test-helpers test-visual benchmark-cpu FORCE
PROD = qtalh
$(OBJ_DIR)/qtalh.exe: $(OBJECTS) $(OBJ_DIR)/main.obj
	$(LINKEXE) $(LDFLAGS) /OUT:$@ $^ $(LIBS)
$(OBJ_DIR)/tests/%.obj: tests/%.cc $(HEADERS) $(BUILD_FILES)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(QT_CFLAGS) $(QT_TEST_CFLAGS) $(CXXFLAGS) /c $< /Fo$@
$(OBJ_DIR)/resources.cc: resources/qtalh.qrc resources/alh.xbm
	@mkdir -p $(OBJ_DIR)
	$(RCC) $< -o $@
$(OBJ_DIR)/resources.obj: $(OBJ_DIR)/resources.cc $(BUILD_FILES)
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(QT_CFLAGS) $(CXXFLAGS) /c $< /Fo$@
$(OBJ_DIR)/test_%.moc: tests/test_%.cc $(HEADERS) $(BUILD_FILES)
	@mkdir -p $(OBJ_DIR)
	$(MOC) -D_WIN32 $(CPPFLAGS) $(INCLUDES) $(QT_CFLAGS) $< -o $@

TESTS = core ui ioc helpers notifications analytics
define test_rules
$(OBJ_DIR)/tests/test_$(1).obj: $(OBJ_DIR)/test_$(1).moc
$(OBJ_DIR)/test_$(1).exe: $(OBJECTS) $(OBJ_DIR)/tests/test_$(1).obj
	$$(LINKEXE) $$(LDFLAGS) /OUT:$$@ $$^ $$(LIBS) $$(QT_TEST_LIBS)
test-$(1): $(OBJ_DIR)/test_$(1).exe
	@mkdir -p $(OBJ_DIR)/test-artifacts
	@QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="$(QT_QPA_FONTDIR)" $(OBJ_DIR)/test_$(1).exe -o $(OBJ_DIR)/test-artifacts/$(1).txt,txt; result=$$$$?; cat $(OBJ_DIR)/test-artifacts/$(1).txt; exit $$$$result
endef
$(foreach test,$(TESTS),$(eval $(call test_rules,$(test))))
$(addprefix $(OBJ_DIR)/tests/test_,$(addsuffix .obj,$(TESTS))): CPPFLAGS += -DTEST_OUTPUT='"$(abspath $(OBJ_DIR)/test-artifacts)"' -DEPICS_TEST_BIN='"$(EPICS_BASE)/bin/$(EPICS_HOST_ARCH)"' -DTEST_BIN_DIR='"$(abspath $(BIN_DIR))"'
test: all $(addprefix test-,$(TESTS))
test-visual:
	@echo "Visual comparisons require Motif ALH and X11 on Linux/macOS."
	@exit 1
$(OBJ_DIR)/benchmark_cpu.exe: $(OBJECTS) $(OBJ_DIR)/tests/benchmark_cpu.obj
	$(LINKEXE) $(LDFLAGS) /OUT:$@ $^ $(LIBS)
benchmark-cpu: $(OBJ_DIR)/benchmark_cpu.exe
	QT_QPA_PLATFORM=offscreen $< 10000 idle 10
$(OBJ_DIR)/tests/test_visual.obj: $(OBJ_DIR)/test_visual.moc
$(OBJ_DIR)/tests/test_visual.obj: CPPFLAGS += -DTEST_OUTPUT='"$(abspath $(OBJ_DIR)/test-artifacts)"'
$(OBJ_DIR)/test_visual.exe: $(OBJECTS) $(OBJ_DIR)/tests/test_visual.obj
	$(LINKEXE) $(LDFLAGS) /OUT:$@ $^ $(LIBS) $(QT_TEST_LIBS)
.PHONY: test-gallery test-ui-fusion
test-gallery: $(OBJ_DIR)/test_visual.exe
	QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="$(QT_QPA_FONTDIR)" $< dialogGallery
	QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="$(QT_QPA_FONTDIR)" QTALH_TEST_STYLE=fusion $< dialogGallery
test-ui test-ui-fusion: $(OBJ_DIR)/qtalh.exe
test-ui-fusion: $(OBJ_DIR)/test_ui.exe
	QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR="$(QT_QPA_FONTDIR)" QTALH_TEST_STYLE=fusion $(OBJ_DIR)/test_ui.exe
else
OBJ_DIR = O.$(OS)-$(ARCH)-qt$(QT_VERSION)
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17 -fPIC
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),all)),)
ifeq ($(filter $(QT_VERSION),5 6),)
$(error QT_VERSION must be 5 or 6)
endif
ifeq ($(QT_VERSION),5)
ifeq ($(shell $(PKG_CONFIG) --atleast-version=5.15 Qt5Core && echo yes),)
$(error Qt 5.15 or newer is required)
endif
endif
ifeq ($(HAVE_QT),)
$(error Qt $(QT_VERSION) Widgets, Network, PrintSupport, and Multimedia development packages are required)
endif
endif
INCLUDES = -I. -I$(OBJ_DIR) -I$(EPICS_BASE)/include -I$(EPICS_BASE)/include/compiler/$(EPICS_COMPILER) -I$(EPICS_BASE)/include/os/$(OS)
CORE = core/analytics.cc core/model.cc core/config.cc core/engine.cc core/notifications.cc
SERVICES = services/analytics.cc services/notifications.cc services/log_browser.cc services/channel_access.cc services/options.cc services/logging.cc services/ipc.cc
GUI = ui/analytics.cc ui/notifications.cc ui/appearance.cc ui/workflows.cc ui/window.cc ui/alarm_view.cc ui/dialogs.cc main.cc
OBJECTS = $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o) $(SERVICES:.cc=.o) $(GUI:.cc=.o)) $(OBJ_DIR)/resources.o
PROD = qtalh qtalh_printer qtalh_DB
.PHONY: all test test-core test-ui test-ioc test-helpers clean distclean
$(OBJ_DIR)/qtalh: $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(QT_LIBS) $(LDLIBS)
# Keep the test rule first for macOS GNU Make 3.81, which uses rule order.
$(OBJ_DIR)/tests/%.o: tests/%.cc $(BUILD_FILES)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(QT_CFLAGS) $(call qt_cflags,Test) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(OBJ_DIR)/qtalh_printer: $(OBJ_DIR)/printer_main.o $(OBJ_DIR)/services/ipc.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(call qt_libs,Core Network)
$(OBJ_DIR)/qtalh_DB: $(OBJ_DIR)/database_main.o $(OBJ_DIR)/services/ipc.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(call qt_libs,Core) $(RPC_LIBS)
$(OBJ_DIR)/test_core.moc: tests/test_core.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_core.o: $(OBJ_DIR)/test_core.moc
$(OBJ_DIR)/test_core: $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o)) $(OBJ_DIR)/services/options.o $(OBJ_DIR)/services/ipc.o $(OBJ_DIR)/tests/test_core.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(call qt_libs,Test Core)
test-core: $(OBJ_DIR)/test_core
	$(OBJ_DIR)/test_core
$(OBJ_DIR)/test_ui.moc: tests/test_ui.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_ui.o: $(OBJ_DIR)/test_ui.moc
$(OBJ_DIR)/tests/test_ui.o: CPPFLAGS += -DTEST_OUTPUT='"$(abspath $(OBJ_DIR)/test-artifacts)"' -DEPICS_TEST_BIN='"$(EPICS_BASE)/bin/$(EPICS_HOST_ARCH)"'
$(OBJ_DIR)/test_ui: $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS)) $(OBJ_DIR)/tests/test_ui.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(QT_LIBS) $(call qt_libs,Test)
test-ui: $(OBJ_DIR)/qtalh $(OBJ_DIR)/test_ui
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/test_ui
$(OBJ_DIR)/test_ioc.moc: tests/test_ioc.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_ioc.o: $(OBJ_DIR)/test_ioc.moc
$(OBJ_DIR)/tests/test_ioc.o: CPPFLAGS += -DEPICS_TEST_BIN='"$(EPICS_BASE)/bin/$(EPICS_HOST_ARCH)"' -DTEST_OUTPUT='"$(abspath $(OBJ_DIR)/test-artifacts)"'
$(OBJ_DIR)/test_ioc: $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o)) $(OBJ_DIR)/services/channel_access.o $(OBJ_DIR)/tests/test_ioc.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(call qt_libs,Test Core)
test-ioc: $(OBJ_DIR)/test_ioc
	@mkdir -p $(OBJ_DIR)/test-artifacts
	$(OBJ_DIR)/test_ioc
$(OBJ_DIR)/test_helpers.moc: tests/test_helpers.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_helpers.o: $(OBJ_DIR)/test_helpers.moc
$(OBJ_DIR)/tests/test_helpers.o: CPPFLAGS += $(RPC_CFLAGS) -DTEST_BIN_DIR='"$(abspath $(BIN_DIR))"'
$(OBJ_DIR)/test_helpers: $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o)) $(OBJ_DIR)/services/options.o $(OBJ_DIR)/services/logging.o $(OBJ_DIR)/services/ipc.o $(OBJ_DIR)/tests/test_helpers.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(call qt_libs,Test Core Network) $(RPC_LIBS)
test-helpers: all $(OBJ_DIR)/test_helpers
	$(OBJ_DIR)/test_helpers

test: test-core test-ui test-ioc test-helpers test-notifications test-analytics

$(OBJ_DIR)/test_analytics.moc: tests/test_analytics.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_analytics.o: $(OBJ_DIR)/test_analytics.moc
$(OBJ_DIR)/test_analytics: $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o)) $(OBJ_DIR)/services/analytics.o $(OBJ_DIR)/services/notifications.o $(OBJ_DIR)/tests/test_analytics.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(call qt_libs,Test Core Network)
.PHONY: test-analytics
test-analytics: $(OBJ_DIR)/test_analytics
	$(OBJ_DIR)/test_analytics

$(OBJ_DIR)/test_notifications.moc: tests/test_notifications.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_notifications.o: $(OBJ_DIR)/test_notifications.moc
$(OBJ_DIR)/test_notifications: $(addprefix $(OBJ_DIR)/,$(CORE:.cc=.o)) $(OBJ_DIR)/services/notifications.o $(OBJ_DIR)/tests/test_notifications.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(call qt_libs,Test Core Network)
.PHONY: test-notifications
test-notifications: $(OBJ_DIR)/test_notifications
	$(OBJ_DIR)/test_notifications

$(OBJ_DIR)/resources.cc: resources/qtalh.qrc resources/alh.xbm
	@mkdir -p $(OBJ_DIR)
	$(RCC) $< -o $@
$(OBJ_DIR)/resources.o: $(OBJ_DIR)/resources.cc $(BUILD_FILES)
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(QT_CFLAGS) $(CXXFLAGS) -c $< -o $@
$(OBJ_DIR)/test_visual.moc: tests/test_visual.cc
	@mkdir -p $(OBJ_DIR)
	$(MOC) $(INCLUDES) $(QT_CFLAGS) $< -o $@
$(OBJ_DIR)/tests/test_visual.o: $(OBJ_DIR)/test_visual.moc
$(OBJ_DIR)/tests/test_visual.o: CPPFLAGS += -DTEST_BIN_DIR='"$(abspath $(BIN_DIR))"' -DEPICS_TEST_BIN='"$(EPICS_BASE)/bin/$(EPICS_HOST_ARCH)"' -DTEST_OUTPUT='"$(abspath $(OBJ_DIR)/test-artifacts)"'
$(OBJ_DIR)/test_visual: $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS)) $(OBJ_DIR)/tests/test_visual.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(QT_LIBS) $(call qt_libs,Test)
.PHONY: test-visual test-gallery test-ui-fusion
test-gallery: $(OBJ_DIR)/test_visual
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/test_visual dialogGallery
	QT_QPA_PLATFORM=offscreen QTALH_TEST_STYLE=fusion $(OBJ_DIR)/test_visual dialogGallery
test-ui-fusion: $(OBJ_DIR)/qtalh $(OBJ_DIR)/test_ui
	QT_QPA_PLATFORM=offscreen QTALH_TEST_STYLE=fusion $(OBJ_DIR)/test_ui
test-visual: $(OBJ_DIR)/test_visual
	$(OBJ_DIR)/test_visual


$(OBJ_DIR)/benchmark_cpu: $(filter-out $(OBJ_DIR)/main.o,$(OBJECTS)) $(OBJ_DIR)/tests/benchmark_cpu.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(QT_LIBS)
.PHONY: benchmark-cpu
benchmark-cpu: $(OBJ_DIR)/benchmark_cpu
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/benchmark_cpu 10000 idle 10
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/benchmark_cpu 10000 updates 10
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/benchmark_cpu 10000 filtered 10
	QT_QPA_PLATFORM=offscreen $(OBJ_DIR)/benchmark_cpu 10000 runtime 10
endif

CLEAN_DIRS = O.$(OS)-$(ARCH)-qt5 O.$(OS)-$(ARCH)-qt6
EXTRA_CXX_OBJ_DEPS = $(HEADERS)
CCFLAGS = $(CXXFLAGS) $(INCLUDES) $(QT_CFLAGS) $(RPC_CFLAGS)
include ../Makefile.build
