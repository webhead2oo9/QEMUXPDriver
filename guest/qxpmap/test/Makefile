# SPDX-License-Identifier: GPL-2.0-only
include ../../mingw-shell.mk

CC = i686-w64-mingw32-gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Werror -march=pentium4 \
	-D_WIN32_WINNT=0x0501 -DWINVER=0x0501
LDFLAGS = -mconsole -static-libgcc -Wl,--subsystem,console:5.01 \
	-Wl,--major-os-version,5,--minor-os-version,1
BUILD := build
PROBE := $(BUILD)/qxpmap_probe.exe

.PHONY: all check-imports clean
all: $(PROBE) check-imports

$(BUILD):
	mkdir -p $@

$(PROBE): qxpmap_probe.c ../qxpmap_ioctl.h ../qxpmap_layout.h \
	../../../qd3d/proto/qd3d_proto.h | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ qxpmap_probe.c

check-imports: $(PROBE)
	python3 ../../../qd3d/tools/check_imports.py $(PROBE)

clean:
	rm -rf $(BUILD)
