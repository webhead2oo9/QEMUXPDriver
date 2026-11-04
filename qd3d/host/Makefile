# SPDX-License-Identifier: GPL-2.0-only
# QD3D host executor (x86_64 MinGW, C11): libqd3dx.a, the loopback harness
# and the executor self-test. Direct3D 9 is loaded at run time from the
# system directory; nothing links against libd3d9.

CC      := x86_64-w64-mingw32-gcc
AR      := x86_64-w64-mingw32-ar

CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter
LDFLAGS := -static
LDLIBS  := -lgdi32 -luser32

B := build

LIB_SRCS := qd3dx_gen.c qd3dx_core.c qd3dx_ext.c qd3dx_xfer.c qd3dx_over.c qd3dx_display.c
LIB_OBJS := $(LIB_SRCS:%.c=$(B)/%.o)
HDRS := qd3dx.h qd3dx_rt.h qd3dx_gen.h qd3dx_priv.h \
        ../proto/qd3d_proto.h ../proto/qd3d_ops.h ../proto/qd3d_sizes.h

all: $(B)/libqd3dx.a $(B)/qd3d_loopback.exe $(B)/qd3dx_selftest.exe

$(B):
	mkdir -p $(B)

$(B)/%.o: %.c $(HDRS) | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/libqd3dx.a: $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(B)/qd3d_loopback.exe: loopback/qd3d_loopback.c $(B)/libqd3dx.a $(HDRS)
	$(CC) $(CFLAGS) -I. $< -o $@ $(LDFLAGS) -L$(B) -lqd3dx $(LDLIBS)

$(B)/qd3dx_selftest.exe: test/qd3dx_selftest.c $(B)/libqd3dx.a $(HDRS)
	$(CC) $(CFLAGS) -I. $< -o $@ $(LDFLAGS) -L$(B) -lqd3dx $(LDLIBS)

clean:
	rm -rf $(B)

.PHONY: all clean
