# SPDX-License-Identifier: GPL-2.0-only
#
# The QD3D tools CD for the XP test VM, built from the repository:
#   QD3D\      d3d9.dll, d3d8.dll, ddraw.dll and the smoke tests/benchmark;
#              UPDATE.CMD copies the DLLs beside games (per-game test install)
#   INSTALL\   the system-wide installer kit, including qxpmap.sys
#              (qd3d/install, README.TXT)
#   QXPICD\    the OpenGL ICD (qxpicd.dll) beside qxptest.exe, its direct test
#   XPDM\      only with XPDM=1: the signed display-driver package (staged on
#              Windows by guest\xpdm\stage-package.cmd) with qxpsetup.exe,
#              which installs it, restarts XP and then runs qxpwgltest.exe
#              --native once
#   SETUP.CMD (users: QD3D system-wide, then the XPDM package, then restart),
#   UPDATE.CMD, RUNTESTS.CMD, SYSWIDE.CMD, TRACE.CMD, BENCH.CMD (CRLF, see
#   .gitattributes)
#   LICENSE.TXT, NOTICES.TXT (the GPL and the third-party notices)
#
#   make        build every component, check its imports, stage build/iso/
#   make iso    build/qd3d-tools.iso
#   make vm     copy the ISO to qemu/winxp/qd3d-tools.iso, the CD the
#               interactive VM (qemu/winxp/qd3d-boot.ps1) mounts; refuses while
#               RetroArch runs
#   make clean

B     := build
STAGE := $(B)/iso
ISO   := $(B)/qd3d-tools.iso
VMISO := ../../qemu/winxp/qd3d-tools.iso

QD3D_FILES := ../guest/build/d3d9.dll \
	../d3d8/build/d3d8.dll ../d3d8/build/qd3d8_smoke.exe \
	../ddraw/build/ddraw.dll ../ddraw/build/qdd_smoke.exe \
	../ddraw/build/qdd_d3d7_smoke.exe ../ddraw/build/qdd_d3d7_legacy_smoke.exe \
	../tests/build/qd3d9_smoke.exe ../tests/build/qd3d9_bench.exe \
	../../guest/qxpmap/test/build/qxpmap_probe.exe
QXPICD_FILES := ../../guest/qxpicd/build/qxpicd.dll \
	../../guest/qxpicd/build/qxptest.exe
SCRIPTS := SETUP.CMD UPDATE.CMD RUNTESTS.CMD SYSWIDE.CMD TRACE.CMD BENCH.CMD
XPDM_PACKAGE := ../../guest/xpdm/build/package
XPDM_MEMBERS := qxp.inf qxp.cat xpdriver-test.cer qxpmp.sys qxpmap.sys \
	qxpdisp.dll qxpicd.dll qxpcoin2.dll
XPDM_TOOLS := ../../guest/acceptance/build/qxpsetup.exe \
	../../guest/acceptance/build/qxpwgltest.exe

.PHONY: all components stage iso vm clean

all: stage

components:
	$(MAKE) -C ../guest
	$(MAKE) -C ../guest check-imports
	$(MAKE) -C ../d3d8
	$(MAKE) -C ../d3d8 check-imports
	$(MAKE) -C ../ddraw
	$(MAKE) -C ../ddraw check-imports
	$(MAKE) -C ../tests
	$(MAKE) -C ../../guest/qxpmap/test
	$(MAKE) -C ../../guest/qxpicd
	python3 ../tools/check_imports.py ../../guest/qxpicd/build/qxpicd.dll
	$(MAKE) -C ../install check-imports stage
ifeq ($(XPDM),1)
	$(MAKE) -C ../../guest/acceptance build/qxpsetup.exe build/qxpwgltest.exe
	@for f in $(XPDM_MEMBERS); do test -f $(XPDM_PACKAGE)/$$f || \
		{ echo "$(XPDM_PACKAGE)/$$f is missing: run guest\\xpdm\\stage-package.cmd" >&2; exit 1; }; done
	@cmp -s $(XPDM_PACKAGE)/qxpicd.dll ../../guest/qxpicd/build/qxpicd.dll && \
		cmp -s $(XPDM_PACKAGE)/qxpmap.sys ../../guest/qxpmap/objfre_wxp_x86/i386/qxpmap.sys || \
		{ echo "the staged XPDM package predates the current ICD/qxpmap builds: rerun stage-package.cmd" >&2; exit 1; }
endif

stage: components
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/QD3D $(STAGE)/INSTALL $(STAGE)/QXPICD
	cp $(QD3D_FILES) $(STAGE)/QD3D/
	cp $(QXPICD_FILES) $(STAGE)/QXPICD/
	cp ../install/build/QD3D/* $(STAGE)/INSTALL/
	sed 's/\r*$$/\r/' ../install/README.md > $(STAGE)/INSTALL/README.TXT
	cp $(SCRIPTS) $(STAGE)/
ifeq ($(XPDM),1)
	mkdir -p $(STAGE)/XPDM
	cp $(addprefix $(XPDM_PACKAGE)/,$(XPDM_MEMBERS)) $(XPDM_TOOLS) $(STAGE)/XPDM/
endif
	sed 's/\r*$$/\r/' ../../LICENSE > $(STAGE)/LICENSE.TXT
	sed 's/\r*$$/\r/' ../../THIRD_PARTY_NOTICES.md > $(STAGE)/NOTICES.TXT
	cd $(STAGE) && find . -type f | sort | xargs sha256sum > ../iso-manifest.txt

iso: stage
	genisoimage -quiet -J -R -V XPDRIVER -o $(ISO) $(STAGE)
	sha256sum $(ISO)

vm: iso
	powershell.exe -NoProfile -Command "if (Get-Process retroarch -ErrorAction SilentlyContinue) { Write-Error 'RetroArch is running: close the VM first'; exit 1 }"
	cp $(ISO) $(VMISO)
	sha256sum $(VMISO)

clean:
	rm -rf $(B)
