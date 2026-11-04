# SPDX-License-Identifier: GPL-2.0-only
#
# The generated version header (tools/version.py, from VERSION and Git) for
# resource files: list $(XPD_VERSION_H) among a resource object's
# prerequisites and pass $(XPD_VERSION_INC) to windres. The header is
# regenerated on every make run but rewritten only when the version
# changes, so nothing rebuilds needlessly.

XPD_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))..)
XPD_VERSION_H := $(XPD_ROOT)/build/version/xpdriver_version.h
XPD_VERSION_INC := -I$(XPD_ROOT)/build/version

.PHONY: xpd-version-check
$(XPD_VERSION_H): xpd-version-check
	@python3 $(XPD_ROOT)/tools/version.py --header $@
