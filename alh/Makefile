# Standalone Motif ALH build. The extensions build is in Makefile.epics.
.DEFAULT_GOAL := all
include ../Makefile.rules

ALH_SRCS = alCA.c acknowledge.c alAudio.c alarm.c alCaCommon.c \
  alConfig.c alFilter.c alLib.c alLog.c alView.c \
  alh.c awAct.c awAlh.c awEdit.c awView.c \
  axArea.c axRunW.c axSubW.c browser.c current.c \
  dialog.c file.c force.c guidance.c heartbeat.c \
  help.c line.c mask.c process.c property.c \
  scroll.c showmask.c sllLib.c beepSevr.c noAck.c
ALH_OBJS = $(addprefix $(OBJ_DIR)/,$(ALH_SRCS:.c=.o))
PROGRAMS = alh alh_printer alh_DB
OBJECTS = $(ALH_OBJS) $(OBJ_DIR)/printer.o $(OBJ_DIR)/alh_DB.o

.PHONY: all clean distclean
all: $(addprefix $(BIN_DIR)/,$(PROGRAMS))

$(OBJ_DIR)/alh: $(ALH_OBJS) $(EPICS_LIBS)
	$(CXX) $(LDFLAGS) -o $@ $(ALH_OBJS) $(EPICS_LDFLAGS) $(EPICS_LIBS) $(EPICS_SYS_LIBS) $(X_LIBS) $(LDLIBS)

$(OBJ_DIR)/alh_printer: $(OBJ_DIR)/printer.o
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OBJ_DIR)/alh_DB: $(OBJ_DIR)/alh_DB.o
	$(CC) $(LDFLAGS) -o $@ $< $(RPC_LIBS) $(LDLIBS)

$(OBJ_DIR)/awAlh.o: ALH_CPPFLAGS += -DALH_HELP_URL='"$(ALH_HELP_URL)"'

$(OBJ_DIR)/%.o: %.c Makefile ../Makefile.rules | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(ALH_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR)/%: $(OBJ_DIR)/% | $(BIN_DIR)
	cp -f $< $@

$(OBJ_DIR) $(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR)

distclean: clean
	rm -f $(addprefix $(BIN_DIR)/,$(PROGRAMS))

-include $(OBJECTS:.o=.d)
