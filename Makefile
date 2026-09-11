# ALH and QtALH standalone builds.
.DEFAULT_GOAL := all
include Makefile.rules
HAVE_MOTIF := $(if $(wildcard $(MOTIF_INC)/Xm/Xm.h),1)
HAVE_QT := $(shell $(PKG_CONFIG) --exists Qt6Widgets Qt6Network Qt6PrintSupport Qt6Multimedia 2>/dev/null && echo 1 || ($(PKG_CONFIG) --exists Qt5Widgets Qt5Network Qt5PrintSupport Qt5Multimedia 2>/dev/null && echo 1))
.PHONY: all alh qtalh test-qtalh clean distclean
all: $(if $(HAVE_MOTIF),alh) $(if $(HAVE_QT),qtalh)
	@$(if $(HAVE_MOTIF),:,echo "Motif unavailable: legacy ALH omitted.")
	@$(if $(HAVE_QT),:,echo "Qt unavailable: QtALH omitted.")
	@$(if $(or $(HAVE_MOTIF),$(HAVE_QT)),:,false)
alh:
	$(MAKE) -C alh
qtalh:
	$(MAKE) -C qtalh
test-qtalh: $(if $(HAVE_MOTIF),alh)
	$(MAKE) -C qtalh test
clean distclean:
	$(MAKE) -C alh $@
	$(MAKE) -C qtalh $@
