/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Test-function selection for the Wine conformance programs (see
 * qd3d_conformance.h). Environment:
 *
 *   QD3D_CONFORMANCE_AFTER=fn     skip the test functions up to and
 *                                 including fn (resume after a crash or hang)
 *   QD3D_CONFORMANCE_ONLY=fn,...  run only the listed test functions
 *
 * Before each test function it runs, prints
 * "qd3d-conformance: run <fn> at <ms> ms, <executed> executed, <failed> failed"
 * with the time since the first function and wine/test.h's running counts
 * (as in its summary line), so a program that dies still reports how far
 * it got.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "qd3d_conformance.h"

/* Defined by wine/test.h in the test program (STANDALONE). */
extern LONG winetest_successes, winetest_failures, winetest_flaky_failures;
extern LONG winetest_todo_successes, winetest_todo_failures;

static int in_list(const char *list, const char *name)
{
    size_t n = strlen(name);

    for (;;) {
        const char *end = strchr(list, ',');
        size_t len = end ? (size_t)(end - list) : strlen(list);

        if (len == n && !strncmp(list, name, n))
            return 1;
        if (!end)
            return 0;
        list = end + 1;
    }
}

int qd3d_conformance_begin(const char *name)
{
    static int started, resumed;
    static DWORD t0;
    const char *after = getenv("QD3D_CONFORMANCE_AFTER");
    const char *only = getenv("QD3D_CONFORMANCE_ONLY");

    if (!started) {
        started = 1;
        t0 = GetTickCount();
    }
    if (after && *after && !resumed) {
        if (!strcmp(after, name))
            resumed = 1;
        return 0;
    }
    if (only && *only && !in_list(only, name))
        return 0;
    printf("qd3d-conformance: run %s at %lu ms, %ld executed, %ld failed\n", name,
           (unsigned long)(GetTickCount() - t0),
           winetest_successes + winetest_failures + winetest_flaky_failures + winetest_todo_successes +
               winetest_todo_failures,
           winetest_failures + winetest_todo_failures);
    fflush(stdout);
    return 1;
}
