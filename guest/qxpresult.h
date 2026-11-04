#ifndef QXP_ACCEPTANCE_RESULT_H
#define QXP_ACCEPTANCE_RESULT_H

#include <windows.h>

/* Keep acceptance reporting available when the VM has no network adapter.
 * QEMU connects COM1 to a host-side file; UDP remains as a compatibility
 * fallback for older content profiles. */
static void qxp_report_serial(const char *message)
{
    static HANDLE serial = INVALID_HANDLE_VALUE;
    DCB state;
    DWORD bytes;
    DWORD length;

    if (!message || !message[0]) return;
    if (serial == INVALID_HANDLE_VALUE) {
        serial = CreateFileA("\\\\.\\COM1", GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
        if (serial == INVALID_HANDLE_VALUE) return;
        ZeroMemory(&state, sizeof(state));
        state.DCBlength = sizeof(state);
        if (GetCommState(serial, &state)) {
            state.BaudRate = CBR_115200;
            state.ByteSize = 8;
            state.Parity = NOPARITY;
            state.StopBits = ONESTOPBIT;
            SetCommState(serial, &state);
        }
    }
    length = (DWORD)lstrlenA(message);
    WriteFile(serial, message, length, &bytes, NULL);
    WriteFile(serial, "\r\n", 2, &bytes, NULL);
}

#endif
