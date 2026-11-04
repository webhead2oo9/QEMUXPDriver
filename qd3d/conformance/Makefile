# SPDX-License-Identifier: GPL-2.0-only
#
# Wine's Direct3D 8/9 and DirectDraw/Direct3D 1-7 API tests (vendored in
# ../third_party/wine, see its README.md) built as standalone 32-bit
# programs, one per test file:
#
#   build/d3d9_device.exe   build/d3d9_stateblock.exe   build/d3d9_visual.exe
#   build/d3d8_device.exe   build/d3d8_stateblock.exe   build/d3d8_visual.exe
#   build/ddraw_ddraw1.exe  build/ddraw_ddraw2.exe      build/ddraw_ddraw4.exe
#   build/ddraw_ddraw7.exe  build/ddraw_d3d.exe         build/ddraw_dsurface.exe
#   build/ddraw_refcount.exe  build/ddraw_visual.exe    build/ddraw_ddrawmodes.exe
#
# run.ps1 runs each one against the stock runtime and through QD3D and diffs
# the results; build/native_probe.exe tells it whether the stock runtime can
# create devices at the moment.
#
# Each source is built from a copy in build/src: the vendored file, with
# patches/<dll>-<file>.patch applied when there is one (excluded test
# functions, compile fixes; line numbers stay those of the vendored file),
# and every plain `name();` and `run_for_each_device_type(name);` call in
# START_TEST rewritten to `QD3D_RUN(name);` and
# `QD3D_RUN_CALL(name, run_for_each_device_type(name));`
# (qd3d_conformance.h), except the set-up calls listed in SETUP_<test>.

CC   := i686-w64-mingw32-gcc
WINE := ../third_party/wine
B    := build

DLLS        := d3d9 d3d8
FILES       := device stateblock visual
DDRAW_FILES := ddraw1 ddraw2 ddraw4 ddraw7 d3d dsurface refcount visual ddrawmodes
TESTS := $(foreach d,$(DLLS),$(foreach f,$(FILES),$(d)_$(f))) $(DDRAW_FILES:%=ddraw_%)

# Calls in START_TEST that create or release objects the following test
# functions share; they stay plain calls, so that a program resumed with
# QD3D_CONFORMANCE_AFTER or run with QD3D_CONFORMANCE_ONLY still makes them.
SETUP_ddraw_d3d        := init_function_pointers|ReleaseDirect3D|D3D1_releaseObjects
SETUP_ddraw_ddrawmodes := init_function_pointers|createdirectdraw|releasedirectdraw|flushdisplaymodes
SETUP_ddraw_dsurface   := ReleaseDirectDraw
SETUP_ddraw_refcount   := init_function_pointers
SETUP_ddraw_visual     := releaseObjects

# sed -E commands that wrap the test function calls of START_TEST.
RUN_CALLS := s/^( +)run_for_each_device_type\(([A-Za-z_][A-Za-z0-9_]*)\);$$/\1QD3D_RUN_CALL(\2, run_for_each_device_type(\2));/; t; \
	s/^( +)([A-Za-z_][A-Za-z0-9_]*)\(\);$$/\1QD3D_RUN(\2);/

# Wine's flags for its tests: -O2, and -fno-strict-aliasing because the tests
# pun floats through pointers. -Wno-format: mingw-w64 declares the BITMAP
# fields LONG where Wine's headers use INT, and device.c prints them with %d.
# DECLSPEC_EXPORT and CONST_VTABLE come from Wine's own headers.
WINE_CFLAGS := -O2 -Wall -Werror -Wno-format -fno-strict-aliasing \
	-DSTANDALONE -DDECLSPEC_EXPORT= -DCONST_VTABLE \
	-include qd3d_conformance.h -Iinclude -I$(WINE)/include
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror
LDFLAGS := -mconsole -static-libgcc
LIBS    := -luser32 -lgdi32 -ldxguid -luuid
# The DirectDraw tests include wine/test.h before any header that declares
# the console API (Wine's winbase.h does, mingw-w64's needs wincon.h), and
# they use debugstr_a(), which wine/debug.h defines for __WINESRC__ (Wine
# builds all of its sources, tests included, with it).
CFLAGS_ddraw := -D__WINESRC__ -include windef.h -include winbase.h -include wincon.h
# Wine links all DirectDraw test files into one program that imports
# ddraw.dll, and ddraw7.c finds DirectDrawCreateEx through
# GetModuleHandleA("ddraw.dll"): import DirectDrawCreate in every program so
# that ddraw.dll is loaded at start. ole32: CoCreateInstance(CLSID_DirectDraw).
LIBS_ddraw := -lole32 -Wl,-u,_DirectDrawCreate@12
# Objects linked into the tests of one DLL.
OBJS_ddraw := $(B)/ddraw_guids.o

HDRS := qd3d_conformance.h include/d3d9on12.h $(WINE)/include/wine/test.h \
	$(WINE)/include/wine/debug.h $(WINE)/dlls/d3d9/tests/utils.h

.PHONY: all clean

all: $(TESTS:%=$(B)/%.exe) $(B)/native_probe.exe

$(B)/native_probe.exe: native_probe.c Makefile | $(B)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -luser32

$(B):
	mkdir -p $@

# Wine's fallbacks for the __wine_dbg_* functions that ntdll exports only
# under Wine (wine_dbgstr_rect() and friends use them).
$(B)/winecrt0_debug.o: $(WINE)/dlls/winecrt0/debug.c $(WINE)/include/wine/debug.h Makefile | $(B)
	$(CC) $(WINE_CFLAGS) -D__WINE_PE_BUILD -D__WINESRC__ '-DARRAY_SIZE(x)=(sizeof(x) / sizeof((x)[0]))' \
		-c -o $@ $<

$(B)/qd3d_conformance.o: qd3d_conformance.c qd3d_conformance.h Makefile | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/ddraw_guids.o: ddraw_guids.c Makefile | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

# $(1): d3d9, d3d8 or ddraw, $(2): test file
define test_rules
$(B)/src/$(1)/$(2).c: $(WINE)/dlls/$(1)/tests/$(2).c $(wildcard patches/$(1)-$(2).patch) Makefile
	mkdir -p $$(@D)
	$(if $(wildcard patches/$(1)-$(2).patch),patch -s --fuzz=0 -r - -o $$@.in $$< patches/$(1)-$(2).patch,cp $$< $$@.in)
	sed -E '/^START_TEST\(/,/^\}/ { $(if $(SETUP_$(1)_$(2)),/^ +($(SETUP_$(1)_$(2)))\(\);$$$$/b; )$$(RUN_CALLS); }' $$@.in > $$@.tmp
	rm -f $$@.in
	mv $$@.tmp $$@

$(B)/$(1)_$(2).exe: $(B)/src/$(1)/$(2).c $(B)/qd3d_conformance.o $(B)/winecrt0_debug.o $(OBJS_$(1)) $(HDRS)
	$(CC) $(WINE_CFLAGS) $(CFLAGS_$(1)) -I$(WINE)/dlls/$(1)/tests -o $$@ $$< $(B)/qd3d_conformance.o \
		$(B)/winecrt0_debug.o $(OBJS_$(1)) $(LDFLAGS) -l$(1) $(LIBS_$(1)) $(LIBS)
endef
$(foreach d,$(DLLS),$(foreach f,$(FILES),$(eval $(call test_rules,$(d),$(f)))))
$(foreach f,$(DDRAW_FILES),$(eval $(call test_rules,ddraw,$(f))))

clean:
	rm -rf $(B)
