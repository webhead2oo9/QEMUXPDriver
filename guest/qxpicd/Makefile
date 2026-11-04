include ../mingw-shell.mk
include ../paired-core.mk

CC := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
CFLAGS := -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	-Wno-cast-function-type -std=c99 -march=pentium4 \
	-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
	-I$(QEMU_DIR)/hw/mesa
LDFLAGS := -Wl,--subsystem,windows:5.01 -Wl,--kill-at -static-libgcc
BUILD := build
GENERATED := generated_gl_scalar.c
SIZE_SOURCE := $(QEMU_DIR)/hw/mesa/szgldata.c
GLFUNCS := third_party/reactos/glfuncs.h

.PHONY: all clean coverage

all: $(BUILD)/qxpicd.dll $(BUILD)/qxptest.exe

coverage: $(GENERATED)
	python3 check_dispatch_coverage.py --minimum 336

$(BUILD):
	mkdir -p $@

$(GENERATED): generate_scalar_stubs.py $(GLFUNCS) \
	$(QEMU_DIR)/hw/mesa/mglfunci.h
	QEMU_DIR="$(QEMU_DIR)" python3 generate_scalar_stubs.py

include ../../tools/version.mk
$(BUILD)/qxpicd_rc.o: qxpicd.rc $(XPD_VERSION_H) | $(BUILD)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(BUILD)/qxpicd.dll: qxpicd.c transport.c transport.h $(GENERATED) $(GLFUNCS) \
	$(SIZE_SOURCE) qxpicd.def ../qxpmap/qxpmap_ioctl.h $(BUILD)/qxpicd_rc.o | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ qxpicd.c transport.c $(GENERATED) \
		$(SIZE_SOURCE) $(BUILD)/qxpicd_rc.o \
		qxpicd.def $(LDFLAGS)

$(BUILD)/qxptest.exe: qxptest.c ../qxpresult.h | $(BUILD)
	$(CC) $(CFLAGS) -mconsole -o $@ $< \
		-Wl,--subsystem,console:5.01 -static-libgcc -lgdi32 -luser32 -lws2_32

clean:
	rm -rf $(BUILD)
