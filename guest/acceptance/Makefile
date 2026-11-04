include ../mingw-shell.mk

CC = i686-w64-mingw32-gcc
PYTHON ?= python3
CFLAGS ?= -O2 -Wall -Wextra -mwindows -march=i686
LDFLAGS ?= -static-libgcc -Wl,--subsystem,windows:5.01 \
	-Wl,--major-os-version,5,--minor-os-version,1
BUILD := build
XPDM_PACKAGE := ../xpdm/build/package
ISO_OUTPUT ?= xpdriver-acceptance.iso

.PHONY: all clean iso xpdm-iso test

all: $(BUILD)/glowner.exe $(BUILD)/qxpwgltest.exe $(BUILD)/qxpsetup.exe \
	$(BUILD)/qxpgametest.exe $(BUILD)/qxpd3dprobe.exe \
	$(BUILD)/qxpd3dvsprobe.exe $(BUILD)/qxpd3dvsbench.exe \
	$(BUILD)/qxpd3dvssuccession.exe \
	$(BUILD)/qxpd3dvsmultictx.exe \
	$(BUILD)/qxp_process_lineage_test.exe \
	$(BUILD)/qxpd3dgame.exe $(BUILD)/qxpgameobserve.exe \
	$(BUILD)/qxpd3d9probe.exe \
	$(BUILD)/qxpd3d9selftest.exe \
	$(BUILD)/qxpddquery.exe

$(BUILD):
	mkdir -p $@

$(BUILD)/glowner.exe: glowner.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -lopengl32 -lgdi32 -luser32

$(BUILD)/qxpwgltest.exe: qxpwgltest.c ../qxpresult.h ../xpdm/qxp_escape.c \
		../xpdm/qxp_escape.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole -I../xpdm \
		$(LDFLAGS) -o $@ qxpwgltest.c ../xpdm/qxp_escape.c \
		-lopengl32 -lgdi32 -luser32 -ladvapi32 -lws2_32

$(BUILD)/qxpsetup.exe: qxpsetup.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
		-ladvapi32 -lcrypt32 -luser32 -lws2_32

$(BUILD)/qxpgametest.exe: qxpgametest.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
		-luser32 -lws2_32

$(BUILD)/qxpd3dprobe.exe: qxpd3dprobe.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32 -lws2_32

$(BUILD)/qxpd3dvsprobe.exe: qxpd3dvsprobe.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32

$(BUILD)/qxpd3dvsbench.exe: qxpd3dvsbench.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32

$(BUILD)/qxpd3dvssuccession.exe: qxpd3dvssuccession.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $<

$(BUILD)/qxpd3dvsmultictx.exe: qxpd3dvsmultictx.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32

$(BUILD)/qxp_process_lineage_test.exe: qxp_process_lineage_test.c \
		qxp_process_lineage.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $<

$(BUILD)/qxpd3dgame.exe: qxpd3dgame.c qxp_process_lineage.h \
		../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32 -lws2_32

$(BUILD)/qxpgameobserve.exe: qxpgameobserve.c qxp_ut2003_log.c \
		qxp_ut2003_log.h qxp_process_lineage.h ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ \
		qxpgameobserve.c qxp_ut2003_log.c \
		-ladvapi32 -luser32 -lws2_32

$(BUILD)/qxp_ut2003_log_test.exe: qxp_ut2003_log_test.c \
		qxp_ut2003_log.c qxp_ut2003_log.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ \
		qxp_ut2003_log_test.c qxp_ut2003_log.c

test: $(BUILD)/qxp_process_lineage_test.exe \
		$(BUILD)/qxp_ut2003_log_test.exe
	$(BUILD)/qxp_process_lineage_test.exe
	$(BUILD)/qxp_ut2003_log_test.exe

$(BUILD)/qxpd3d9probe.exe: qxpd3d9probe.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-luser32 -lws2_32

$(BUILD)/qxpd3d9selftest.exe: qxpd3d9selftest.c ../qxpresult.h \
	../xpdm/qxp_escape.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-lgdi32 -luser32 -lws2_32

$(BUILD)/qxpddquery.exe: qxpddquery.c ../qxpresult.h | $(BUILD)
	$(CC) $(filter-out -mwindows,$(CFLAGS)) -mconsole $(LDFLAGS) -o $@ $< \
	-ladvapi32 -lddraw -lgdi32 -luser32 -lws2_32

iso: all
	$(MAKE) -C ../qxpicd
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/GLTEST $(BUILD)/iso/QXPICD
	cp $(BUILD)/glowner.exe $(BUILD)/iso/GLTEST/
	cp ../qxpicd/build/qxpicd.dll ../qxpicd/build/qxptest.exe \
		$(BUILD)/iso/QXPICD/
	cp $(BUILD)/qxpwgltest.exe $(BUILD)/iso/QXPICD/
	cp qxpreclaim.cmd qxpidle.cmd $(BUILD)/iso/QXPICD/
	cp autorun.inf $(BUILD)/iso/
	$(PYTHON) make_iso.py $(BUILD)/iso $(ISO_OUTPUT)
	@echo $(ISO_OUTPUT)

xpdm-iso: all $(XPDM_PACKAGE)/qxpmp.sys $(XPDM_PACKAGE)/qxpmap.sys \
	$(XPDM_PACKAGE)/qxpdisp.dll $(XPDM_PACKAGE)/qxpicd.dll \
	$(XPDM_PACKAGE)/qxpcoin2.dll $(XPDM_PACKAGE)/qxp.inf \
	$(XPDM_PACKAGE)/qxp.cat $(XPDM_PACKAGE)/xpdriver-test.cer
	$(MAKE) -C ../qxpicd
	rm -rf $(BUILD)/iso
	mkdir -p $(BUILD)/iso/GLTEST $(BUILD)/iso/QXPICD $(BUILD)/iso/XPDM
	cp $(BUILD)/glowner.exe $(BUILD)/iso/GLTEST/
	cp ../qxpicd/build/qxpicd.dll ../qxpicd/build/qxptest.exe \
		$(BUILD)/qxpwgltest.exe $(BUILD)/iso/QXPICD/
	cp $(XPDM_PACKAGE)/qxpmp.sys $(XPDM_PACKAGE)/qxpmap.sys \
		$(XPDM_PACKAGE)/qxpdisp.dll \
		$(XPDM_PACKAGE)/qxpicd.dll $(XPDM_PACKAGE)/qxpcoin2.dll \
		$(XPDM_PACKAGE)/qxp.inf \
		$(XPDM_PACKAGE)/qxp.cat $(XPDM_PACKAGE)/xpdriver-test.cer \
		$(BUILD)/qxpwgltest.exe $(BUILD)/qxpsetup.exe \
		$(BUILD)/qxpgametest.exe $(BUILD)/qxpd3dprobe.exe \
		$(BUILD)/qxpd3dvsprobe.exe $(BUILD)/qxpd3dvsbench.exe \
		$(BUILD)/qxpd3dvssuccession.exe \
		$(BUILD)/qxpd3dvsmultictx.exe \
		$(BUILD)/qxpd3dgame.exe $(BUILD)/qxpgameobserve.exe \
		$(BUILD)/qxpd3d9probe.exe \
		$(BUILD)/qxpd3d9selftest.exe \
		$(BUILD)/qxpddquery.exe \
		$(BUILD)/iso/XPDM/
	cp qxpreclaim.cmd qxpidle.cmd $(BUILD)/iso/QXPICD/
	cp autorun.inf $(BUILD)/iso/
	$(PYTHON) make_iso.py $(BUILD)/iso $(ISO_OUTPUT)
	@echo $(ISO_OUTPUT)

clean:
	rm -rf $(BUILD)
