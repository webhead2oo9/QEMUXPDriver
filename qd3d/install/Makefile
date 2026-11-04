# SPDX-License-Identifier: GPL-2.0-only
#
# qd3d_install.exe: system-wide installer for QD3D and qxpmap on Windows XP SP3
# (x86). See README.md.
#
#   make                 build build/qd3d_install.exe
#   make check-imports   fail unless every import is on the XP SP3 allowlist
#   make test            host test against a temporary fake system root (needs
#                        ../guest/build/d3d9.dll, ../d3d8/build/d3d8.dll and
#                        ../ddraw/build/ddraw.dll)
#   make stage-driver    copy the WDK-built qxpmap.sys into build/
#   make stage           build/QD3D: installer, .CMD wrappers, qxpmap.sys and
#                        the three QD3D DLLs for the tools CD

CC      := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
OBJDUMP := i686-w64-mingw32-objdump
PYTHON  := python3

B := build

# No C runtime: the program has its own entry point and memset/memcpy/memmove/
# memcmp, so its imports are exactly the Win32 calls in qd3d_install.c.
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -march=i686 -ffreestanding -fno-stack-protector \
           -fno-asynchronous-unwind-tables -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DUNICODE -D_UNICODE
LDFLAGS := -nostdlib -Wl,-e,_qd3dinst_start -Wl,--subsystem,console:5.01 \
           -Wl,--major-os-version,5,--minor-os-version,1
LIBS    := -lkernel32 -ladvapi32 -lversion -lgcc

# The QD3D DLLs the installer puts into System32.
QD3D_DLLS := ../guest/build/d3d9.dll ../d3d8/build/d3d8.dll ../ddraw/build/ddraw.dll
QXPMAP_SYS := ../../guest/qxpmap/objfre_wxp_x86/i386/qxpmap.sys
WRAPPERS  := install.cmd uninstall.cmd status.cmd scan.cmd

all: $(B)/qd3d_install.exe

$(B):
	mkdir -p $(B)

$(B)/qd3d_install.o: qd3d_install.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

# The loops must stay loops, not calls to the functions they implement.
$(B)/qd3d_mem.o: qd3d_mem.c | $(B)
	$(CC) $(CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns -c -o $@ $<

# Manifest: asInvoker, so Windows Vista and later never elevate it by name.
include ../../tools/version.mk
$(B)/qd3d_install.res.o: qd3d_install.rc qd3d_install.manifest $(XPD_VERSION_H) | $(B)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(B)/qd3d_install.exe: $(B)/qd3d_install.o $(B)/qd3d_mem.o $(B)/qd3d_install.res.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

check-imports: $(B)/qd3d_install.exe
	$(PYTHON) ../tools/check_imports.py --objdump $(OBJDUMP) $<

# Test helper: holds a file the way a running game does (mapped image, or
# open without FILE_SHARE_DELETE).
$(B)/qd3d_hold.exe: test/qd3d_hold.c | $(B)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $< -Wl,--subsystem,console:5.01

# Test fixture: a ddraw.dll that names itself the way Wine's does.
$(B)/wine_ddraw.res.o: test/wine_ddraw.rc | $(B)
	$(WINDRES) -O coff -o $@ $<

$(B)/wine_ddraw.dll: test/wine_ddraw.c $(B)/wine_ddraw.res.o
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -shared -nostdlib -o $@ $^ \
	    -Wl,-e,_wine_ddraw_entry@12 -Wl,--kill-at

# Native PE/version fixture only: fake SCM never loads this image, and the
# real installer refuses its fixture identity outside --root.
$(B)/qxpmap_fixture.res.o: test/qxpmap_fixture.rc | $(B)
	$(WINDRES) -O coff -o $@ $<

$(B)/qxpmap_fixture_older.res.o: test/qxpmap_fixture.rc | $(B)
	$(WINDRES) -DQXPMAP_FIXTURE_OLDER -O coff -o $@ $<

$(B)/qxpmap_fixture_newer.res.o: test/qxpmap_fixture.rc | $(B)
	$(WINDRES) -DQXPMAP_FIXTURE_NEWER -O coff -o $@ $<

QXPMAP_FIXTURES := $(B)/qxpmap_fixture.sys $(B)/qxpmap_fixture_older.sys $(B)/qxpmap_fixture_newer.sys
$(QXPMAP_FIXTURES): $(B)/%.sys: test/qxpmap_fixture.c $(B)/%.res.o
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -nostdlib -o $@ $^ \
	    -Wl,-e,_fixture_entry@8 -Wl,--subsystem,native:5.01 \
	    -Wl,--major-os-version,5,--minor-os-version,1

test: $(B)/qd3d_install.exe $(B)/qd3d_hold.exe $(B)/wine_ddraw.dll $(QXPMAP_FIXTURES)
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$(wslpath -w test/run-host-test.ps1)"

stage-driver: | $(B)
	@test -f "$(QXPMAP_SYS)" || { \
	    echo "missing $(QXPMAP_SYS): build guest/qxpmap with the WDK XP x86 free build environment first"; \
	    exit 1; \
	}
	cp "$(QXPMAP_SYS)" $(B)/qxpmap.sys

stage: $(B)/qd3d_install.exe $(WRAPPERS) stage-driver
	@for f in $(QD3D_DLLS); do \
	    test -f $$f || { echo "missing $$f: run make -C $${f%/build/*} first"; exit 1; }; \
	done
	rm -rf $(B)/QD3D
	mkdir -p $(B)/QD3D
	cp $(B)/qd3d_install.exe $(B)/qxpmap.sys $(WRAPPERS) $(QD3D_DLLS) $(B)/QD3D/

clean:
	rm -rf $(B)

.PHONY: all check-imports test stage-driver stage clean
