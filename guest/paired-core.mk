# SPDX-License-Identifier: GPL-2.0-only
#
# QEMU_DIR: the paired qemu-libretro core tree. The OpenGL ICD compiles its
# Mesa pass-through function table (hw/mesa/mglfunci.h) and size table
# (hw/mesa/szgldata.c) from it, so it must be the core the guest will run
# on. Default: the core submodule (core/), else the qemu-libretro-d3d9
# worktree beside this checkout. Override: make QEMU_DIR=/path/to/core.

XPDRIVER_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
QEMU_DIR ?= $(if $(wildcard $(XPDRIVER_ROOT)/core/hw/mesa/mglfunci.h),$(XPDRIVER_ROOT)/core,$(abspath $(XPDRIVER_ROOT)/../qemu-libretro-d3d9))

ifeq ($(wildcard $(QEMU_DIR)/hw/mesa/mglfunci.h),)
$(error QEMU_DIR=$(QEMU_DIR) is not a qemu-libretro core tree (no hw/mesa/mglfunci.h); set QEMU_DIR to the paired core)
endif
