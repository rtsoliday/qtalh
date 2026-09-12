# ALH and QtALH builds.
.DEFAULT_GOAL := all
QTALH_TOP_LEVEL := 1
include Makefile.rules
ifeq ($(OS),Windows)
HAVE_MOTIF :=
HAVE_QT := 1
else
HAVE_MOTIF := $(if $(and $(XM_LIB),$(wildcard $(MOTIF_INC)/Xm/Xm.h)),1)
include Makefile.qt
endif
.PHONY: all alh qtalh test-qtalh clean distclean check-dependencies
all: check-dependencies $(if $(HAVE_MOTIF),alh) $(if $(HAVE_QT),qtalh)
	@$(if $(or $(HAVE_MOTIF),$(HAVE_QT)),:,false)

# Complete notices before either recursive build, including with make -j.
ifneq ($(filter all install,$(or $(MAKECMDGOALS),all)),)
alh qtalh: | check-dependencies
endif

check-dependencies:
ifneq ($(OS),Windows)
ifeq ($(HAVE_MOTIF),)
	@echo ""
	@echo "=========================================="
	@echo "NOTE: Motif development libraries not found."
ifeq ($(XM_LIB),)
	@echo "  Missing library: libXm"
endif
ifeq ($(wildcard $(MOTIF_INC)/Xm/Xm.h),)
	@echo "  Missing headers: Xm/Xm.h"
endif
	@echo "Skipping build of alh (Motif-based ALH)."
ifneq ($(HAVE_QT),)
	@echo "Only building qtalh (Qt-based ALH)."
endif
	@echo ""
	@echo "To build alh, install Motif development packages:"
	@echo "  Debian/Ubuntu: sudo apt-get install libmotif-dev libxmu-dev"
	@echo "  RHEL/CentOS:   sudo yum install motif-devel libXmu-devel"
	@echo "  macOS:         brew install openmotif"
	@echo "=========================================="
	@echo ""
endif
ifeq ($(HAVE_QT),)
	@echo ""
	@echo "=========================================="
	@echo "NOTE: Qt development libraries not found."
	@echo "QtALH requires Qt 5.15 or newer, or Qt 6, including"
	@echo "Widgets, Network, PrintSupport, and Multimedia."
	@echo "Skipping build of qtalh (Qt-based ALH)."
	@echo ""
	@echo "To build qtalh, install Qt development packages:"
	@echo "  Debian/Ubuntu: sudo apt-get install qtbase5-dev qtmultimedia5-dev"
	@echo "                 or sudo apt-get install qt6-base-dev qt6-base-dev-tools qt6-multimedia-dev"
	@echo "  RHEL/CentOS:   sudo yum install qt5-qtbase-devel qt5-qtmultimedia-devel"
	@echo "  macOS:         brew install qt"
	@echo "=========================================="
	@echo ""
endif
endif
ifeq ($(or $(HAVE_MOTIF),$(HAVE_QT)),)
	@echo "ERROR: No available ALH variant to build. Install Motif or Qt development packages."
endif

alh:
ifeq ($(OS),Windows)
	@echo "Legacy Motif ALH requires Linux or macOS; use make qtalh on Windows."
	@exit 1
else
	$(MAKE) -C alh
endif
qtalh:
	$(MAKE) -C qtalh
test-qtalh: $(if $(HAVE_MOTIF),alh)
	$(MAKE) -C qtalh test
clean: docs-clean
distclean: docs-distclean
clean distclean:
ifneq ($(OS),Windows)
	$(MAKE) -C alh $@
endif
	$(MAKE) -C qtalh $@

# Build the standalone documentation site without Qt or EPICS.
.PHONY: docs
DOCS_BASE ?= /
docs:
	$(PYTHON) scripts/build-docs.py --base "$(DOCS_BASE)"

# Remove generated docs while preserving authored pages and original archives.
.PHONY: docs-clean docs-distclean
docs-clean:
	$(PYTHON) scripts/sync-docs.py --clean
docs-distclean:
	$(PYTHON) scripts/sync-docs.py --distclean
