/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Force-included into every Wine test source (-include). The build rewrites
 * each plain `name();` call in START_TEST to `QD3D_RUN(name);` (and
 * `run_for_each_device_type(name);` to `QD3D_RUN_CALL(name, ...)`), so the
 * runner can see which test function is running and resume a test program
 * after a crash or hang. No system headers here: the Wine sources must see
 * their own include order (COBJMACROS, initguid.h).
 */
#ifndef QD3D_CONFORMANCE_H
#define QD3D_CONFORMANCE_H

int qd3d_conformance_begin(const char *name);

/* Runs `call` as the test function `name`. */
#define QD3D_RUN_CALL(name, call) do { if (qd3d_conformance_begin(#name)) call; } while (0)
#define QD3D_RUN(fn) QD3D_RUN_CALL(fn, fn())

#endif
