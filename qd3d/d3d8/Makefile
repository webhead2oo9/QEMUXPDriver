# SPDX-License-Identifier: GPL-2.0-only
#
# qd3d8: Direct3D 8 front end implemented on Direct3D 9.
#
#   make                 build build/d3d8.dll and build/qd3d8_smoke.exe
#   make check-imports   list d3d8.dll imports; fail on non-XP-SP3 imports
#   make run-stock       run the smoke test against the system d3d8.dll
#   make run-wrapped     run the smoke test with build/d3d8.dll beside it
#   make run-fallback    run it with build/d3d8.dll "installed" in a test
#                        directory (QD3D8_TEST_SYSTEM_DIR) without a QD3D
#                        session: it must hand the application to the stock
#                        d3d8.dll copied there as qd3d8_orig.dll
CROSS    ?= i686-w64-mingw32-
CXX      := $(CROSS)g++
WINDRES  := $(CROSS)windres
OBJDUMP  := $(CROSS)objdump
PYTHON   ?= python3

BUILD    := build
DLL      := $(BUILD)/d3d8.dll
SMOKE    := $(BUILD)/qd3d8_smoke.exe

WARN     := -Wall -Wextra -Werror -Wno-unused-parameter
CXXFLAGS := -O2 $(WARN) -march=pentium4 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fcheck-new
LDFLAGS  := -Wl,--subsystem,windows:5.01 -static-libgcc -static-libstdc++ \
            -Wl,--nxcompat -Wl,--dynamicbase

DLL_SRCS := qd3d8_main.cpp qd3d8_d3d.cpp qd3d8_device.cpp qd3d8_resource.cpp qd3d8_shader.cpp
DLL_OBJS := $(DLL_SRCS:%.cpp=$(BUILD)/%.o)

.PHONY: all clean check-imports run-stock run-wrapped run-fallback

all: $(DLL) $(SMOKE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp qd3d8.h ../proto/qd3d_proto.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

include ../../tools/version.mk
$(BUILD)/qd3d8_rc.o: qd3d8.rc $(XPD_VERSION_H) | $(BUILD)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(DLL): $(DLL_OBJS) $(BUILD)/qd3d8_rc.o d3d8.def
	$(CXX) -shared -o $@ $(DLL_OBJS) $(BUILD)/qd3d8_rc.o d3d8.def $(LDFLAGS)

$(SMOKE): test/qd3d8_smoke.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< -Wl,--subsystem,console:5.01 -static-libgcc -static-libstdc++ \
	    -ld3d8 -luser32 -lgdi32

check-imports: $(DLL)
	$(PYTHON) ../tools/check_imports.py --objdump $(OBJDUMP) $(DLL)

run-stock: $(SMOKE)
	mkdir -p $(BUILD)/stock
	cp $(SMOKE) $(BUILD)/stock/
	cd $(BUILD)/stock && ./qd3d8_smoke.exe --expect-stock

run-wrapped: $(DLL) $(SMOKE)
	cd $(BUILD) && ./qd3d8_smoke.exe --expect-local

# The stock 32-bit d3d8.dll: SysWOW64 on 64-bit Windows, else System32.
STOCK_D3D8 := $(firstword $(wildcard /mnt/c/Windows/SysWOW64/d3d8.dll /mnt/c/Windows/System32/d3d8.dll))

run-fallback: $(DLL) $(SMOKE)
	rm -rf $(BUILD)/fallback
	mkdir -p $(BUILD)/fallback
	cp $(DLL) $(SMOKE) $(BUILD)/fallback/
	cp $(STOCK_D3D8) $(BUILD)/fallback/qd3d8_orig.dll
	cd $(BUILD)/fallback && QD3D8_TEST_SYSTEM_DIR="$$(wslpath -w .)" WSLENV=QD3D8_TEST_SYSTEM_DIR \
	    ./qd3d8_smoke.exe --expect-fallback

clean:
	rm -rf $(BUILD)
