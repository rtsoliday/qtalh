# ALH and QtALH standalone builds.
.DEFAULT_GOAL := all
include Makefile.rules
ifeq ($(OS),Windows)
HAVE_MOTIF :=
HAVE_QT := 1
else
HAVE_MOTIF := $(if $(wildcard $(MOTIF_INC)/Xm/Xm.h),1)
HAVE_QT := $(shell $(PKG_CONFIG) --exists Qt6Widgets Qt6Network Qt6PrintSupport Qt6Multimedia 2>/dev/null && echo 1 || ($(PKG_CONFIG) --exists Qt5Widgets Qt5Network Qt5PrintSupport Qt5Multimedia 2>/dev/null && echo 1))
endif
.PHONY: all alh qtalh test-qtalh clean distclean
all: $(if $(HAVE_MOTIF),alh) $(if $(HAVE_QT),qtalh)
	@$(if $(HAVE_MOTIF),:,echo "Motif unavailable: legacy ALH omitted.")
	@$(if $(HAVE_QT),:,echo "Qt unavailable: QtALH omitted.")
	@$(if $(or $(HAVE_MOTIF),$(HAVE_QT)),:,false)
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
clean distclean:
ifneq ($(OS),Windows)
	$(MAKE) -C alh $@
endif
	$(MAKE) -C qtalh $@
