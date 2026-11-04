# SPDX-License-Identifier: GPL-2.0-only
#
# qd3d9_smoke.exe: 32-bit, Windows XP compatible Direct3D 9 end-to-end oracle.
# Run it with the stock runtime, or with the QD3D proxy d3d9.dll beside it.
# qd3d9_multiproc.exe: several Direct3D 9 processes on QD3D channel slots
# (run-multiproc.ps1).
# qd3d9_display.exe: the display policy end to end (static screen,
# minimized and hidden windows, a killed process; run-loopback.ps1 -Slots 2).
# qd3d_ddraw_cursor.exe: the cursor over a DirectDraw exclusive-mode frame
# between flips (run-loopback.ps1 -WithDDraw).
# qd3d9_bench.exe: Direct3D 9 per-draw throughput (a measurement, not a test;
# the tools CD's BENCH.CMD runs it on the stock runtime and through QD3D).
# qd3d_one_proxy.exe: one QD3D proxy per process (run-one-proxy, host only):
# a front end must reuse the QD3D d3d9.dll a game loaded from its own folder.

include ../../guest/mingw-shell.mk

CC = i686-w64-mingw32-gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	-march=pentium4 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501
LDFLAGS = -mconsole -static-libgcc -Wl,--subsystem,console:5.01 \
	-Wl,--major-os-version,5,--minor-os-version,1
LDLIBS = -ld3d9 -luser32 -lgdi32
BUILD := build

.PHONY: all clean run-one-proxy

all: $(BUILD)/qd3d9_smoke.exe $(BUILD)/qd3d9_multiproc.exe $(BUILD)/qd3d9_display.exe \
	$(BUILD)/qd3d_ddraw_cursor.exe $(BUILD)/qd3d9_bench.exe $(BUILD)/qd3d_one_proxy.exe

$(BUILD):
	mkdir -p $@

$(BUILD)/qd3d9_smoke.exe: qd3d9_smoke.c qd3d9_smoke_shaders.h ../proto/qd3d_proto.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d9_smoke.c $(LDLIBS)

$(BUILD)/qd3d9_multiproc.exe: qd3d9_multiproc.c ../proto/qd3d_proto.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d9_multiproc.c $(LDLIBS)

$(BUILD)/qd3d9_display.exe: qd3d9_display.c ../proto/qd3d_proto.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d9_display.c $(LDLIBS)

$(BUILD)/qd3d_ddraw_cursor.exe: qd3d_ddraw_cursor.c ../proto/qd3d_proto.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d_ddraw_cursor.c -lddraw $(LDLIBS)

$(BUILD)/qd3d9_bench.exe: qd3d9_bench.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d9_bench.c $(LDLIBS)

$(BUILD)/qd3d_one_proxy.exe: qd3d_one_proxy.c | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qd3d_one_proxy.c -luser32

# game\ holds the QD3D d3d9.dll a game loads first; sys\ stands for System32
# after a system-wide install: both front ends beside a second proxy, and the
# host's 32-bit stock d3d9.dll kept as qd3d9_orig.dll.  No QD3D session is
# needed; %TEMP% points at the run directory for qd3d.log.
ONE_PROXY := $(BUILD)/one-proxy
STOCK_D3D9 := $(firstword $(wildcard /mnt/c/Windows/SysWOW64/d3d9.dll /mnt/c/Windows/System32/d3d9.dll))
run-one-proxy: $(BUILD)/qd3d_one_proxy.exe ../guest/build/d3d9.dll \
		../ddraw/build/ddraw.dll ../d3d8/build/d3d8.dll
	rm -rf $(ONE_PROXY)
	mkdir -p $(ONE_PROXY)/game $(ONE_PROXY)/sys
	cp $(BUILD)/qd3d_one_proxy.exe $(ONE_PROXY)/
	cp ../guest/build/d3d9.dll $(ONE_PROXY)/game/
	cp ../guest/build/d3d9.dll ../ddraw/build/ddraw.dll ../d3d8/build/d3d8.dll $(ONE_PROXY)/sys/
	cp $(STOCK_D3D9) $(ONE_PROXY)/sys/qd3d9_orig.dll
	cd $(ONE_PROXY) && W="$$(wslpath -w .)" && export TEMP="$$W" TMP="$$W" WSLENV=TEMP/w:TMP/w && \
	    ./qd3d_one_proxy.exe "$$W\\game\\d3d9.dll" "$$W\\sys\\ddraw.dll" ddraw && \
	    ./qd3d_one_proxy.exe "$$W\\game\\d3d9.dll" "$$W\\sys\\d3d8.dll" d3d8 && \
	    QD3D9_TEST_SYSTEM_DIR="$$W\\sys" WSLENV=TEMP/w:TMP/w:QD3D9_TEST_SYSTEM_DIR \
	    ./qd3d_one_proxy.exe "$$W\\game\\d3d9.dll" "$$W\\sys\\d3d9.dll" stock

clean:
	rm -rf $(BUILD)
