/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Test helper for run-host-test.ps1: holds a file the way a running program
 * does until it is killed (at most two minutes), after writing READY.
 *
 *   qd3d_hold load FILE READY   map FILE as an image (a game using the DLL)
 *   qd3d_hold open FILE READY   open FILE for reading without FILE_SHARE_DELETE
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    HANDLE ready;
    DWORD written;

    if (argc != 4) {
        fprintf(stderr, "usage: qd3d_hold load|open FILE READY\n");
        return 1;
    }
    if (!strcmp(argv[1], "load")) {
        if (!LoadLibraryExA(argv[2], NULL, DONT_RESOLVE_DLL_REFERENCES)) {
            fprintf(stderr, "LoadLibraryEx(%s) failed: %lu\n", argv[2], GetLastError());
            return 2;
        }
    } else if (!strcmp(argv[1], "open")) {
        if (CreateFileA(argv[2], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) ==
            INVALID_HANDLE_VALUE) {
            fprintf(stderr, "CreateFile(%s) failed: %lu\n", argv[2], GetLastError());
            return 2;
        }
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 1;
    }
    ready = CreateFileA(argv[3], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (ready == INVALID_HANDLE_VALUE)
        return 3;
    WriteFile(ready, "held\r\n", 6, &written, NULL);
    CloseHandle(ready);
    Sleep(120000);
    return 0;
}
