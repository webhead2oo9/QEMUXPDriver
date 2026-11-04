# SPDX-License-Identifier: GPL-2.0-only
#
# qddraw: DirectDraw 1-7 and Direct3D 7 front end over QD3D
# (docs/design/ddraw-remoting.md).
#
#   make                 build build/ddraw.dll and every test in test/
#   make check-imports   list ddraw.dll imports; fail on non-XP-SP3 imports
#   make run-stock       run the smoke tests against the system ddraw.dll
#   make run-fallback    run qdd_smoke with build/ddraw.dll beside it and no
#                        QD3D session: it must hand everything to the stock
#                        runtime
#   make run-loopback    run qdd_smoke through the front end on the loopback
#                        executor (qd3d/tests/run-loopback.ps1 -WithDDraw)
#
# Every qdd_*.cpp is part of the DLL (the Direct3D 7 code is qdd_d3d7*.cpp);
# every test/qdd_*.cpp is a console test program.
CROSS    ?= i686-w64-mingw32-
CXX      := $(CROSS)g++
WINDRES  := $(CROSS)windres
OBJDUMP  := $(CROSS)objdump
PYTHON   ?= python3

# BUILD=<dir> builds elsewhere (parallel builds must not share a directory).
BUILD    ?= build
DLL      := $(BUILD)/ddraw.dll

WARN     := -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS := -O2 $(WARN) -march=pentium4 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fcheck-new
LDFLAGS  := -Wl,--subsystem,windows:5.01 -static-libgcc -static-libstdc++ \
            -Wl,--nxcompat -Wl,--dynamicbase
LIBS     := -ldxguid -luuid -lgdi32 -luser32

DLL_SRCS := $(sort $(wildcard qdd_*.cpp))
# D3D7=0: DirectDraw only (the Direct3D hooks stubbed), for testing the
# core on its own.
ifeq ($(D3D7),0)
DLL_SRCS := $(filter-out qdd_d3d7%,$(DLL_SRCS)) d3d7_stub.cpp
endif
DLL_OBJS := $(DLL_SRCS:%.cpp=$(BUILD)/%.o)
HDRS     := $(wildcard *.h) ../proto/qd3d_proto.h
TEST_SRCS := $(sort $(wildcard test/qdd_*.cpp))
TESTS    := $(TEST_SRCS:test/%.cpp=$(BUILD)/%.exe)

.PHONY: all clean check-imports run-stock run-fallback run-loopback

all: $(DLL) $(TESTS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

include ../../tools/version.mk
$(BUILD)/qddraw_rc.o: qddraw.rc $(XPD_VERSION_H) | $(BUILD)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(DLL): $(DLL_OBJS) $(BUILD)/qddraw_rc.o ddraw.def
	$(CXX) -shared -o $@ $(DLL_OBJS) $(BUILD)/qddraw_rc.o ddraw.def $(LDFLAGS) $(LIBS)

$(BUILD)/%.exe: test/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< -Wl,--subsystem,console:5.01 -static-libgcc -static-libstdc++ \
	    -lddraw -ldxguid -luser32 -lgdi32

check-imports: $(DLL)
	$(PYTHON) ../tools/check_imports.py --objdump $(OBJDUMP) $(DLL)

# The smoke tests in a directory without ddraw.dll: the system runtime.
run-stock: $(TESTS)
	rm -rf $(BUILD)/stock
	mkdir -p $(BUILD)/stock
	cp $(TESTS) $(BUILD)/stock/
	cd $(BUILD)/stock && ./qdd_smoke.exe --expect-stock

# build/ddraw.dll beside the test and only the system d3d9.dll: no QD3D
# session, so DirectDraw creation and every later export go to the stock
# runtime (System32\ddraw.dll for a per-game copy).  %TEMP% points at the
# run directory, so the qd3d.log event lands there.
run-fallback: $(DLL) $(TESTS)
	rm -rf $(BUILD)/fallback
	mkdir -p $(BUILD)/fallback
	cp $(DLL) $(BUILD)/qdd_smoke.exe $(BUILD)/fallback/
	cd $(BUILD)/fallback && W="$$(wslpath -w .)" && QDDRAW_LOG="$$W\\qddraw.log" TEMP="$$W" TMP="$$W" \
	    WSLENV=QDDRAW_LOG/w:TEMP/w:TMP/w ./qdd_smoke.exe --expect-fallback && \
	    grep -q "stock runtime" qddraw.log && grep -q "stock runtime" qd3d.log

run-loopback: $(DLL) $(TESTS)
	powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$(wslpath -w ../tests/run-loopback.ps1)" \
	    -Program "$$(wslpath -w $(BUILD)/qdd_smoke.exe)" -WithDDraw -Arguments --expect-local

clean:
	rm -rf $(BUILD)
