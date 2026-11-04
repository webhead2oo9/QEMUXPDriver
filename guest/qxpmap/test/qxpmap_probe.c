/* SPDX-License-Identifier: GPL-2.0-only */
/* Run only inside an idle XP VM. Never sends a QD3D command or Mesa write. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../qxpmap_ioctl.h"
#include "../qxpmap_layout.h"
#include "../../../qd3d/proto/qd3d_proto.h"

#define MATCH(field) _Static_assert(QXPMAP_QD3D_##field == QD3D_VM_##field, #field)
_Static_assert(QXPMAP_QD3D_MAGIC == QD3D_MAGIC, "MAGIC");
MATCH(SLOTS);
MATCH(CHANNEL_GPA);
MATCH(CHANNEL_SIZE);
MATCH(MMIO_GPA);
MATCH(MMIO_SIZE);
MATCH(REG_KICK);
MATCH(REG_ID);
MATCH(REG_SLOTS);
MATCH(REG_RELEASE);
_Static_assert(QD3D_ABI_VERSION == 4, "qxpmap requires the RELEASE-aware core");
_Static_assert(sizeof(QXPMAP_INFO) == 32, "QUERY layout");
_Static_assert(sizeof(QXPMAP_QD3D_SLOT) == 24, "CLAIM layout");
_Static_assert(sizeof(QXPMAP_WINDOW_REQUEST) == 8, "MAP input layout");
_Static_assert(sizeof(QXPMAP_MAPPING) == 16, "MAP output layout");
_Static_assert(sizeof(void *) == 4, "XP x86 probe");

static unsigned failures;

static int result(int okay, const char *name)
{
    printf("%s %s\n", okay ? "PASS" : "FAIL", name);
    if (!okay) ++failures;
    return okay;
}

static HANDLE open_driver(void)
{
    return CreateFileA(QXPMAP_USER_PATH, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL query(HANDLE handle, QXPMAP_INFO *info)
{
    DWORD bytes = 0;
    ZeroMemory(info, sizeof(*info));
    return DeviceIoControl(handle, IOCTL_QXPMAP_QUERY, NULL, 0,
        info, sizeof(*info), &bytes, NULL) && bytes == sizeof(*info);
}

static BOOL claim(HANDLE handle, QXPMAP_QD3D_SLOT *slot)
{
    DWORD bytes = 0;
    ZeroMemory(slot, sizeof(*slot));
    return DeviceIoControl(handle, IOCTL_QXPMAP_QD3D_CLAIM, NULL, 0,
        slot, sizeof(*slot), &bytes, NULL) && bytes == sizeof(*slot);
}

static BOOL release(HANDLE handle)
{
    DWORD bytes = 0;
    return DeviceIoControl(handle, IOCTL_QXPMAP_QD3D_RELEASE,
        NULL, 0, NULL, 0, &bytes, NULL) && bytes == 0;
}

static int region(ULONGLONG address, ULONG size, int writable)
{
    MEMORY_BASIC_INFORMATION memory;
    DWORD protection;
    if (address == 0 || (address >> 32) != 0 || (address & 4095) != 0)
        return 0;
    if (VirtualQuery((void *)(uintptr_t)address, &memory, sizeof(memory)) !=
        sizeof(memory) || memory.State != MEM_COMMIT ||
        memory.RegionSize < size || (memory.Protect & PAGE_GUARD) != 0)
        return 0;
    protection = memory.Protect & 0xff;
    return protection == PAGE_READWRITE ||
        (!writable && protection == PAGE_READONLY);
}

static int exercise_slot(const QXPMAP_QD3D_SLOT *slot)
{
    volatile ULONG *registers;
    volatile ULONG *scratch;
    volatile struct qd3d_ctrl *control;
    ULONG before;
    ULONG after;
    if (slot->Slot >= QD3D_VM_SLOTS ||
        slot->ChannelSize != QD3D_VM_CHANNEL_SIZE ||
        !region(slot->Registers, QD3D_VM_MMIO_SIZE, 0) ||
        !region(slot->Channel, slot->ChannelSize, 1)) return 0;
    registers = (volatile ULONG *)(uintptr_t)slot->Registers;
    if (registers[QD3D_VM_REG_ID / sizeof(ULONG)] != QD3D_MAGIC) return 0;
    control = (volatile struct qd3d_ctrl *)(uintptr_t)slot->Channel;
    if (control->magic != QD3D_MAGIC || control->abi != QD3D_ABI_VERSION)
        return 0;
    /* Only touch our newly claimed slot, without submitting any command,
     * and restore the original contents. Never touch a producer cursor. */
    scratch = (volatile ULONG *)(uintptr_t)(slot->Channel +
        slot->ChannelSize - sizeof(ULONG));
    before = *scratch;
    *scratch = before ^ 0x51d34d51UL;
    after = *scratch;
    *scratch = before;
    return after == (before ^ 0x51d34d51UL);
}

static BOOL map_window(HANDLE handle, ULONG window, QXPMAP_MAPPING *mapping)
{
    QXPMAP_WINDOW_REQUEST request;
    DWORD bytes = 0;
    request.Window = window;
    request.Reserved = 0;
    ZeroMemory(mapping, sizeof(*mapping));
    return DeviceIoControl(handle, IOCTL_QXPMAP_MAP_WINDOW, &request,
        sizeof(request), mapping, sizeof(*mapping), &bytes, NULL) &&
        bytes == sizeof(*mapping);
}

static BOOL unmap_window(HANDLE handle, QXPMAP_MAPPING *mapping)
{
    DWORD bytes = 0;
    return DeviceIoControl(handle, IOCTL_QXPMAP_UNMAP_WINDOW, mapping,
        sizeof(*mapping), NULL, 0, &bytes, NULL) && bytes == 0;
}

static void test_windows(HANDLE handle)
{
    static const ULONG invalid[] = {
        0, QXPMAP_WINDOW_COUNT + 1, 0xffffffffUL,
        QD3D_VM_CHANNEL_GPA,
        QD3D_VM_CHANNEL_GPA + QD3D_VM_CHANNEL_SIZE,
        QD3D_VM_CHANNEL_GPA + 2 * QD3D_VM_CHANNEL_SIZE,
        QD3D_VM_CHANNEL_GPA + 3 * QD3D_VM_CHANNEL_SIZE,
        QD3D_VM_MMIO_GPA
    };
    static const ULONG sizes[] = {
        QXPMAP_MESA_REGS_SIZE, QXPMAP_MESA_FIFO_SIZE,
        QXPMAP_MESA_FBTM_SIZE, QXPMAP_MESA_MBUFO_SIZE
    };
    QXPMAP_WINDOW_REQUEST request;
    QXPMAP_MAPPING mapping;
    QXPMAP_MAPPING wrong;
    DWORD bytes;
    ULONG window;
    ULONG probe = 0;
    unsigned index;
    int okay = 1;
    int present = 0;
    int identified = 0;
    BOOL accepted;
    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        request.Window = invalid[index];
        request.Reserved = 0;
        bytes = 0;
        accepted = DeviceIoControl(handle, IOCTL_QXPMAP_MAP_WINDOW,
            &request, sizeof(request), &mapping, sizeof(mapping), &bytes, NULL);
        if (accepted || GetLastError() != ERROR_INVALID_PARAMETER) okay = 0;
        if (accepted) (void)unmap_window(handle, &mapping);
    }
    result(okay, "MAP_WINDOW rejects unknown ids and all QD3D addresses");
    request.Window = QXPMAP_WINDOW_MESA_REGS;
    request.Reserved = 1;
    bytes = 0;
    accepted = DeviceIoControl(handle, IOCTL_QXPMAP_MAP_WINDOW,
        &request, sizeof(request), &mapping, sizeof(mapping), &bytes, NULL);
    result(!accepted && GetLastError() == ERROR_INVALID_PARAMETER,
        "MAP_WINDOW rejects nonzero Reserved");
    if (accepted) (void)unmap_window(handle, &mapping);
    for (window = 1; window <= QXPMAP_WINDOW_COUNT; ++window) {
        char name[96];
        okay = map_window(handle, window, &mapping);
        snprintf(name, sizeof(name), "MAP_WINDOW %lu returns the fixed window", window);
        if (!result(okay, name)) continue;
        okay = mapping.Window == window && mapping.Size == sizes[window - 1] &&
            region(mapping.Address, mapping.Size, 1);
        if (window == QXPMAP_WINDOW_MESA_REGS && okay) {
            volatile ULONG *registers = (volatile ULONG *)(uintptr_t)mapping.Address;
            probe = registers[0xfb4 / sizeof(ULONG)];
            present = (probe >> 20) == 0x3d1;
            identified = 1;
        }
        snprintf(name, sizeof(name), "MAP_WINDOW %lu address/size/protection", window);
        result(okay, name);
        wrong = mapping;
        wrong.Size += 4096;
        result(!unmap_window(handle, &wrong) && GetLastError() == ERROR_INVALID_PARAMETER,
            "UNMAP_WINDOW rejects a changed mapping identity");
        snprintf(name, sizeof(name), "UNMAP_WINDOW %lu releases the owned view", window);
        result(unmap_window(handle, &mapping), name);
        result(!unmap_window(handle, &mapping) && GetLastError() == ERROR_INVALID_PARAMETER,
            "UNMAP_WINDOW rejects an already released view");
    }
    if (identified && present) {
        result(1, "Mesa device present (read-only identity)");
    } else if (identified) {
        printf("SKIP Mesa device execution: identity 0x%08lx is absent/incompatible; "
            "fixed mapping/unmapping was tested without MMIO writes\n", probe);
    }
}

static int close_inherited(const char *text, const char *event_name)
{
    char *end;
    ULONG value = strtoul(text, &end, 16);
    HANDLE handle = (HANDLE)(uintptr_t)value;
    HANDLE event;
    QXPMAP_INFO info;
    int okay;
    if (*end != '\0' || value == 0 || handle == INVALID_HANDLE_VALUE) return 1;
    event = OpenEventA(SYNCHRONIZE, FALSE, event_name);
    if (event == NULL) {
        CloseHandle(handle);
        return 1;
    }
    okay = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0 &&
        !query(handle, &info) && GetLastError() == ERROR_ACCESS_DENIED;
    if (!CloseHandle(handle)) okay = 0;
    CloseHandle(event);
    return okay ? 0 : 1;
}

static int test_foreign_close(HANDLE *handle, QXPMAP_QD3D_SLOT *slot)
{
    char path[MAX_PATH];
    char command[512];
    char event_name[80];
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    QXPMAP_MAPPING window;
    QXPMAP_QD3D_SLOT old = *slot;
    HANDLE event = NULL;
    HANDLE copy = INVALID_HANDLE_VALUE;
    DWORD length;
    DWORD exit_code;
    int started = 0;
    int exited = 0;
    int okay = 0;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (!map_window(*handle, QXPMAP_WINDOW_MESA_REGS, &window)) goto done;
    length = GetModuleFileNameA(NULL, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) goto done;
    snprintf(event_name, sizeof(event_name), "Local\\QxpMapProbe_%lu_close", GetCurrentProcessId());
    event = CreateEventA(NULL, TRUE, FALSE, event_name);
    if (event == NULL || GetLastError() == ERROR_ALREADY_EXISTS) goto done;
    if (!DuplicateHandle(GetCurrentProcess(), *handle, GetCurrentProcess(),
        &copy, 0, TRUE, DUPLICATE_SAME_ACCESS)) goto done;
    snprintf(command, sizeof(command), "\"%s\" --close-inherited %lx %s",
        path, (ULONG)(uintptr_t)copy, event_name);
    if (!CreateProcessA(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, &process)) goto done;
    started = 1;
    CloseHandle(*handle);
    *handle = INVALID_HANDLE_VALUE;
    CloseHandle(copy);
    copy = INVALID_HANDLE_VALUE;
    if (!SetEvent(event) ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) goto done;
    exited = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0 ||
        region(old.Registers, QD3D_VM_MMIO_SIZE, 0) ||
        region(old.Channel, old.ChannelSize, 1) ||
        region(window.Address, window.Size, 1)) goto done;
    *handle = open_driver();
    okay = *handle != INVALID_HANDLE_VALUE && claim(*handle, slot) &&
        slot->Slot == old.Slot && exercise_slot(slot);
 done:
    if (copy != INVALID_HANDLE_VALUE) CloseHandle(copy);
    if (started) {
        if (!exited) {
            (void)TerminateProcess(process.hProcess, 39);
            (void)WaitForSingleObject(process.hProcess, 10000);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (event != NULL) CloseHandle(event);
    return okay;
}

typedef struct CHILD_STATE {
    volatile DWORD Error;
    volatile ULONG Slot;
} CHILD_STATE;

static int child_main(const char *event_name, const char *state_name)
{
    HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name);
    HANDLE section = OpenFileMappingA(FILE_MAP_WRITE, FALSE, state_name);
    CHILD_STATE *state = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    QXPMAP_QD3D_SLOT slot;
    DWORD error = ERROR_INVALID_HANDLE;
    if (event == NULL || section == NULL) goto done;
    state = (CHILD_STATE *)MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(*state));
    if (state == NULL) goto done;
    handle = open_driver();
    if (handle != INVALID_HANDLE_VALUE && claim(handle, &slot)) {
        error = ERROR_SUCCESS;
        state->Slot = slot.Slot;
    } else {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_INVALID_DATA;
    }
    state->Error = error;
    if (!SetEvent(event)) goto done;
    if (error == ERROR_SUCCESS) Sleep(INFINITE);
 done:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (state != NULL) UnmapViewOfFile(state);
    if (section != NULL) CloseHandle(section);
    if (event != NULL) CloseHandle(event);
    return error == ERROR_SUCCESS ? 0 : 1;
}

static int test_killed_child(HANDLE handle, ULONG expected,
                             QXPMAP_QD3D_SLOT *reclaimed)
{
    char path[MAX_PATH];
    char command[512];
    char event_name[80];
    char state_name[80];
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    QXPMAP_INFO info;
    CHILD_STATE *state = NULL;
    HANDLE event = NULL;
    HANDLE section = NULL;
    HANDLE waiters[2];
    DWORD length;
    DWORD wait_result;
    int okay = 0;
    int started = 0;
    int terminated = 0;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    length = GetModuleFileNameA(NULL, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) goto done;
    snprintf(event_name, sizeof(event_name), "Local\\QxpMapProbe_%lu_ready", GetCurrentProcessId());
    snprintf(state_name, sizeof(state_name), "Local\\QxpMapProbe_%lu_state", GetCurrentProcessId());
    event = CreateEventA(NULL, TRUE, FALSE, event_name);
    if (event == NULL || GetLastError() == ERROR_ALREADY_EXISTS) goto done;
    section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        0, sizeof(*state), state_name);
    if (section == NULL || GetLastError() == ERROR_ALREADY_EXISTS) goto done;
    state = (CHILD_STATE *)MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0, sizeof(*state));
    if (state == NULL) goto done;
    state->Error = ERROR_IO_PENDING;
    state->Slot = 0xffffffffUL;
    snprintf(command, sizeof(command), "\"%s\" --child %s %s", path, event_name, state_name);
    if (!CreateProcessA(path, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, &process)) goto done;
    started = 1;
    waiters[0] = event;
    waiters[1] = process.hProcess;
    wait_result = WaitForMultipleObjects(2, waiters, FALSE, 10000);
    if (wait_result != WAIT_OBJECT_0 || state->Error != ERROR_SUCCESS ||
        state->Slot != expected || !query(handle, &info) || info.Qd3dSlotsFree != 0)
        goto done;
    if (!TerminateProcess(process.hProcess, 37)) goto done;
    terminated = 1;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0) goto done;
    okay = query(handle, &info) && info.Qd3dSlotsFree == 1 &&
        claim(handle, reclaimed) && reclaimed->Slot == expected && exercise_slot(reclaimed);
 done:
    if (started) {
        if (!terminated) (void)TerminateProcess(process.hProcess, 38);
        (void)WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    if (state != NULL) UnmapViewOfFile(state);
    if (section != NULL) CloseHandle(section);
    if (event != NULL) CloseHandle(event);
    return okay;
}

int main(int argc, char **argv)
{
    HANDLE handles[5];
    QXPMAP_QD3D_SLOT slots[5];
    QXPMAP_QD3D_SLOT duplicate;
    QXPMAP_INFO info;
    DWORD bytes;
    ULONG mask = 0;
    ULONG released;
    unsigned index;
    BOOL accepted;
    int okay;
    if (argc == 4 && strcmp(argv[1], "--child") == 0)
        return child_main(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--close-inherited") == 0)
        return close_inherited(argv[2], argv[3]);
    if (argc != 1) {
        fprintf(stderr, "Usage: qxpmap_probe.exe (inside an idle XP VM only)\n");
        return 2;
    }
    printf("qxpmap_probe: idle XP VM only; no QD3D commands or Mesa MMIO writes\n");
    for (index = 0; index < 5; ++index) handles[index] = INVALID_HANDLE_VALUE;
    handles[0] = open_driver();
    if (!result(handles[0] != INVALID_HANDLE_VALUE, "open QXPMAP")) {
        printf("open error=%lu\n", GetLastError());
        goto done;
    }
    okay = query(handles[0], &info) && info.Version == QXPMAP_VERSION &&
        info.Qd3dSlots == QD3D_VM_SLOTS &&
        info.Qd3dSlotSize == QD3D_VM_CHANNEL_SIZE &&
        info.Qd3dSlotsFree <= info.Qd3dSlots && info.Qd3dAbi == 0 &&
        info.Windows == QXPMAP_WINDOWS_MASK &&
        info.Reserved[0] == 0 && info.Reserved[1] == 0;
    if (!result(okay, "QUERY sane (four-slot RELEASE-aware VM required)")) goto done;
    if (!result(info.Qd3dSlotsFree == QD3D_VM_SLOTS,
        "idle VM: all slots free before probe (otherwise stop without claiming)")) goto done;
    bytes = 0;
    result(!DeviceIoControl(handles[0], IOCTL_QXPMAP_QUERY, NULL, 0,
        &info, sizeof(info) - 1, &bytes, NULL), "QUERY rejects a short output buffer");
    for (index = 1; index < 5; ++index) {
        handles[index] = open_driver();
        if (!result(handles[index] != INVALID_HANDLE_VALUE, "nonexclusive handle open")) goto done;
    }
    for (index = 0; index < 4; ++index) {
        char name[96];
        okay = claim(handles[index], &slots[index]);
        snprintf(name, sizeof(name), "CLAIM %u: readable QD3D ID and writable private channel", index);
        if (!result(okay && exercise_slot(&slots[index]), name)) goto done;
        if (!result((mask & (1UL << slots[index].Slot)) == 0, "CLAIM slot is distinct")) goto done;
        mask |= 1UL << slots[index].Slot;
    }
    accepted = claim(handles[4], &duplicate);
    if (!result(!accepted && GetLastError() == ERROR_BUSY, "fifth CLAIM is busy")) goto done;
    accepted = claim(handles[0], &duplicate);
    if (!result(!accepted && GetLastError() == ERROR_INVALID_FUNCTION,
        "second CLAIM on the same handle is refused")) goto done;
    released = slots[0].Slot;
    if (!result(release(handles[0]) && claim(handles[0], &slots[0]) &&
        slots[0].Slot == released && exercise_slot(&slots[0]),
        "RELEASE then re-CLAIM reuses the free slot")) goto done;
    released = slots[1].Slot;
    CloseHandle(handles[1]);
    handles[1] = INVALID_HANDLE_VALUE;
    if (!result(claim(handles[4], &slots[4]) && slots[4].Slot == released &&
        exercise_slot(&slots[4]), "closing a handle frees its slot")) goto done;
    if (!result(test_foreign_close(&handles[0], &slots[0]),
        "foreign last-close revokes live owner's QD3D/Mesa views before reuse")) goto done;
    released = slots[0].Slot;
    if (!result(release(handles[0]), "release exactly one slot for the child")) goto done;
    if (!result(test_killed_child(handles[0], released, &slots[0]),
        "TerminateProcess frees the child slot; parent re-CLAIM succeeds")) goto done;
    test_windows(handles[0]);
 done:
    for (index = 0; index < 5; ++index) {
        if (handles[index] != INVALID_HANDLE_VALUE) CloseHandle(handles[index]);
    }
    if (failures == 0) {
        HANDLE final_handle = open_driver();
        result(final_handle != INVALID_HANDLE_VALUE && query(final_handle, &info) &&
            info.Qd3dSlotsFree == QD3D_VM_SLOTS, "all probe slots freed on close");
        if (final_handle != INVALID_HANDLE_VALUE) CloseHandle(final_handle);
    }
    printf("RESULT: %s qxpmap_probe (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
