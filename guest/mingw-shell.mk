# Native Windows GNU Make otherwise falls back to cmd.exe even when Codex or
# PowerShell exports SHELL. These guest Makefiles intentionally use POSIX rm,
# cp, mkdir, and continuation syntax, so select the Git-for-Windows shell from
# its stable short path and fail with a useful message when it is unavailable.
ifeq ($(OS),Windows_NT)
qxp_git_sh := $(firstword \
	$(wildcard C:/Progra~1/Git/bin/sh.exe) \
	$(wildcard C:/Progra~2/Git/bin/sh.exe))
ifeq ($(qxp_git_sh),)
$(error Git for Windows sh.exe is required for native Windows guest builds)
endif
SHELL := $(qxp_git_sh)
qxp_git_root := $(patsubst %/bin/sh.exe,%,$(qxp_git_sh))
# GNU Make may directly execute simple recipes without starting SHELL. Put the
# matching POSIX utilities on its own process search path as well.
export PATH := $(qxp_git_root)/usr/bin;$(PATH)
endif

# Keep an explicit clean ordered ahead of a rebuild even with -j. Otherwise
# independent top-level goals may delete outputs while another goal creates
# them.
ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif
