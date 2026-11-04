# SPDX-License-Identifier: GPL-2.0-only
# Guest d3d9.dll proxy (i686, Windows XP) and its framing self-test.

CC      := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
OBJDUMP := i686-w64-mingw32-objdump
PYTHON  := python3

B := build

CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -march=pentium4 \
          -D_WIN32_WINNT=0x0501 -DWINVER=0x0501
LDFLAGS := -static-libgcc
LIBS := -luser32 -lgdi32 -lkernel32

HDRS := qd3d9_rt.h qd3d9_gen.h qd3d9_priv.h \
        ../proto/qd3d_proto.h ../proto/qd3d_ops.h ../proto/qd3d_sizes.h \
        ../../guest/qxpmap/qxpmap_ioctl.h

PROXY_OBJS := $(B)/qd3d9_rt.o $(B)/qd3d9_main.o $(B)/qd3d9_res.o $(B)/qd3d9_gen.o

all: $(B)/d3d9.dll $(B)/qd3d9_ringtest.exe

$(B):
	mkdir -p $(B)

$(B)/%.o: %.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

include ../../tools/version.mk
$(B)/qd3d9_rc.o: qd3d9.rc $(XPD_VERSION_H) | $(B)
	$(WINDRES) $(XPD_VERSION_INC) -O coff -o $@ $<

$(B)/d3d9.dll: $(PROXY_OBJS) $(B)/qd3d9_rc.o qd3d9.def
	$(CC) -shared -o $@ $(PROXY_OBJS) $(B)/qd3d9_rc.o qd3d9.def $(LDFLAGS) \
	    -Wl,--subsystem,windows:5.01 -Wl,--kill-at $(LIBS)

$(B)/qd3d9_ringtest.exe: $(B)/qd3d9_ringtest.o $(PROXY_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--subsystem,console:5.01 $(LIBS)

# Fails unless XP SP3 exports every import of d3d9.dll (../tools/xp_sp3_exports.txt).
check-imports: $(B)/d3d9.dll
	$(PYTHON) ../tools/check_imports.py --objdump $(OBJDUMP) $(B)/d3d9.dll

clean:
	rm -rf $(B)

.PHONY: all check-imports clean
