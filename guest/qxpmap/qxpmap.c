/* SPDX-License-Identifier: GPL-2.0-only */
/* XP x86 legacy control device. No worker queues: mappings are made only
 * by a top-level user IRP in its issuing process at PASSIVE_LEVEL. */
#include <ntifs.h>
#include "qxpmap_ioctl.h"
#include "qxpmap_layout.h"

#define QXPMAP_POOL_TAG 0x6d707851UL /* "Qxpm" */
#define QXPMAP_NO_SLOT 0xffffffffUL

C_ASSERT(sizeof(PVOID) == 4);
C_ASSERT(sizeof(QXPMAP_INFO) == 32);
C_ASSERT(sizeof(QXPMAP_QD3D_SLOT) == 24);
C_ASSERT(sizeof(QXPMAP_WINDOW_REQUEST) == 8);
C_ASSERT(sizeof(QXPMAP_MAPPING) == 16);
C_ASSERT(FIELD_OFFSET(QXPMAP_QD3D_SLOT, Registers) == 8);
C_ASSERT(FIELD_OFFSET(QXPMAP_QD3D_SLOT, Channel) == 16);

typedef struct QXP_WINDOW {
    ULONG Physical;
    ULONG Size;
    ULONG Protect;
} QXP_WINDOW;

static const QXP_WINDOW QxpWindows[QXPMAP_WINDOW_COUNT] = {
    { QXPMAP_MESA_REGS_GPA, QXPMAP_MESA_REGS_SIZE,
      PAGE_READWRITE | PAGE_NOCACHE },
    { QXPMAP_MESA_FIFO_GPA, QXPMAP_MESA_FIFO_SIZE, PAGE_READWRITE },
    { QXPMAP_MESA_FBTM_GPA, QXPMAP_MESA_FBTM_SIZE, PAGE_READWRITE },
    { QXPMAP_MESA_MBUFO_GPA, QXPMAP_MESA_MBUFO_SIZE, PAGE_READWRITE }
};

typedef struct QXP_FILE {
    PEPROCESS Process;
    PEPROCESS SlotOwner;
    ULONG Slot;
    PVOID Registers;
    PVOID Channel;
    /* A bounded mapping list: at most one mapping per fixed window. */
    PVOID Windows[QXPMAP_WINDOW_COUNT];
    BOOLEAN Cleaned;
} QXP_FILE;

typedef struct QXP_DEVICE {
    /* KMUTEX, not FAST_MUTEX: ZwMap/Unmap must run at PASSIVE_LEVEL. */
    KMUTEX Mutex;
    HANDLE PhysicalMemory;
    PUCHAR Registers;
    QXP_FILE *Slots[QXPMAP_QD3D_SLOTS];
} QXP_DEVICE;

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD QxpUnload;
DRIVER_DISPATCH QxpCreate;
DRIVER_DISPATCH QxpClose;
DRIVER_DISPATCH QxpCleanup;
DRIVER_DISPATCH QxpDeviceControl;
DRIVER_DISPATCH QxpUnsupported;

static NTSTATUS QxpComplete(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static VOID QxpLock(QXP_DEVICE *Device)
{
    (VOID)KeWaitForSingleObject(&Device->Mutex, Executive, KernelMode,
                                FALSE, NULL);
}

static VOID QxpUnlock(QXP_DEVICE *Device)
{
    (VOID)KeReleaseMutex(&Device->Mutex, FALSE);
}

/* One-off lines into QEMU's debug console (I/O port 0xe9, the channel the
 * XPDM drivers also use): load progress and rare failures, never per call. */
#define QXPMAP_DEBUGCON_PORT ((PUCHAR)(ULONG_PTR)0xe9)

static VOID QxpTrace(const char *Text, ULONG Value)
{
    static const char hex[] = "0123456789abcdef";
    const char *p;
    int shift;

    for (p = "QXPMAP "; *p != '\0'; ++p) {
        WRITE_PORT_UCHAR(QXPMAP_DEBUGCON_PORT, (UCHAR)*p);
    }
    for (p = Text; *p != '\0'; ++p) {
        WRITE_PORT_UCHAR(QXPMAP_DEBUGCON_PORT, (UCHAR)*p);
    }
    WRITE_PORT_UCHAR(QXPMAP_DEBUGCON_PORT, (UCHAR)' ');
    for (shift = 28; shift >= 0; shift -= 4) {
        WRITE_PORT_UCHAR(QXPMAP_DEBUGCON_PORT, (UCHAR)hex[(Value >> shift) & 0xfUL]);
    }
    WRITE_PORT_UCHAR(QXPMAP_DEBUGCON_PORT, (UCHAR)'\n');
}

static BOOLEAN QxpCallerContext(PIRP Irp)
{
    /* METHOD_BUFFERED alone does not guarantee a process context. Reject
     * kernel-generated/forwarded/queued requests and attached processes. */
    return (BOOLEAN)(KeGetCurrentIrql() == PASSIVE_LEVEL &&
        Irp->RequestorMode == UserMode &&
        Irp->Tail.Overlay.Thread == PsGetCurrentThread() &&
        IoGetRequestorProcess(Irp) == PsGetCurrentProcess() &&
        IoGetTopLevelIrp() == NULL);
}

static ULONG QxpSlotCount(QXP_DEVICE *Device)
{
    ULONG count;

    if (READ_REGISTER_ULONG((PULONG)(Device->Registers +
            QXPMAP_QD3D_REG_ID)) != QXPMAP_QD3D_MAGIC) {
        return 0;
    }
    count = READ_REGISTER_ULONG((PULONG)(Device->Registers +
        QXPMAP_QD3D_REG_SLOTS));
    if (count == 0 || count > QXPMAP_QD3D_SLOTS) {
        return 0;
    }
    return count;
}

/* winnt.h's MEM_DOS_LIM (x86: views at page instead of 64-KiB granularity);
 * the WDK 7.1 XP kernel headers do not define it. */
#ifndef MEM_DOS_LIM
#define MEM_DOS_LIM 0x40000000
#endif
#define QXPMAP_ALLOCATION_GRANULARITY 0x10000UL

static NTSTATUS QxpMap(QXP_DEVICE *Device, ULONG Physical, ULONG Size,
                      ULONG Protect, PVOID *Address)
{
    PHYSICAL_ADDRESS offset;
    SIZE_T size;
    PVOID base;
    ULONG allocation;
    NTSTATUS status;

    *Address = NULL;
    if ((Physical & (PAGE_SIZE - 1)) != 0 || Size == 0 ||
        (Size & (PAGE_SIZE - 1)) != 0 || Physical > MAXULONG - Size) {
        return STATUS_INVALID_PARAMETER;
    }
    offset.QuadPart = (LONGLONG)(ULONGLONG)Physical;
    size = (SIZE_T)Size;
    base = NULL;
    /* A window off the 64-KiB allocation granularity (the Mesa register page
     * at 0xefffe000) is mapped with MEM_DOS_LIM so its neighbouring physical
     * pages are never exposed; aligned windows (every QD3D window) take the
     * ordinary path. All addresses are driver constants. */
    allocation = (Physical & (QXPMAP_ALLOCATION_GRANULARITY - 1)) != 0 ?
        MEM_DOS_LIM : 0;
    status = ZwMapViewOfSection(Device->PhysicalMemory, ZwCurrentProcess(),
        &base, 0, 0, &offset, &size, ViewUnmap, allocation, Protect);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (base == NULL || ((ULONG_PTR)base & (PAGE_SIZE - 1)) != 0 ||
        offset.QuadPart != (LONGLONG)(ULONGLONG)Physical || size != Size ||
        (ULONG_PTR)base > (ULONG_PTR)MmHighestUserAddress ||
        size - 1 > (ULONG_PTR)MmHighestUserAddress - (ULONG_PTR)base) {
        (VOID)ZwUnmapViewOfSection(ZwCurrentProcess(), base);
        return STATUS_INVALID_ADDRESS;
    }
    *Address = base;
    return STATUS_SUCCESS;
}

static NTSTATUS QxpUnmap(PVOID *Address)
{
    NTSTATUS status;

    if (*Address == NULL) {
        return STATUS_SUCCESS;
    }
    status = ZwUnmapViewOfSection(ZwCurrentProcess(), *Address);
    /* A caller may already have used UnmapViewOfFile, and process exit may
     * have started address-space teardown before IRP_MJ_CLEANUP arrives. */
    if (NT_SUCCESS(status) || status == STATUS_NOT_MAPPED_VIEW ||
        status == STATUS_PROCESS_IS_TERMINATING) {
        *Address = NULL;
        return STATUS_SUCCESS;
    }
    return status;
}

static NTSTATUS QxpUnmapOwned(QXP_FILE *File, BOOLEAN AllWindows)
{
    KAPC_STATE apcState;
    BOOLEAN attached;
    NTSTATUS status;
    NTSTATUS viewStatus;
    ULONG index;

    if (!AllWindows && File->Channel == NULL && File->Registers == NULL) {
        return STATUS_SUCCESS;
    }
    attached = (BOOLEAN)(File->Process != PsGetCurrentProcess());
    if (attached) {
        KeStackAttachProcess((PKPROCESS)File->Process, &apcState);
    }
    /* Only these section operations execute while attached: no waiting on
     * application work, completion, MMIO notification, or object release. */
    status = QxpUnmap(&File->Channel);
    viewStatus = QxpUnmap(&File->Registers);
    if (NT_SUCCESS(status)) {
        status = viewStatus;
    }
    if (AllWindows) {
        for (index = 0; index < QXPMAP_WINDOW_COUNT; ++index) {
            viewStatus = QxpUnmap(&File->Windows[index]);
            if (NT_SUCCESS(status)) {
                status = viewStatus;
            }
        }
    }
    if (attached) {
        KeUnstackDetachProcess(&apcState);
    }
    return status;
}

static NTSTATUS QxpReleaseSlot(QXP_DEVICE *Device, QXP_FILE *File)
{
    NTSTATUS status;
    ULONG slot;

    status = QxpUnmapOwned(File, FALSE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    slot = File->Slot;
    if (slot == QXPMAP_NO_SLOT) {
        return STATUS_SUCCESS;
    }
    /* The host snapshots the old producer's head in this write. Keep the
     * mutex/slot ownership until after the MMIO write has returned. */
    WRITE_REGISTER_ULONG((PULONG)(Device->Registers +
        QXPMAP_QD3D_REG_RELEASE), slot);
    Device->Slots[slot] = NULL;
    File->Slot = QXPMAP_NO_SLOT;
    ObDereferenceObject(File->SlotOwner);
    File->SlotOwner = NULL;
    return status;
}

static NTSTATUS QxpCleanFile(QXP_DEVICE *Device, QXP_FILE *File)
{
    NTSTATUS status;

    File->Cleaned = TRUE;
    status = QxpUnmapOwned(File, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return QxpReleaseSlot(Device, File);
}

NTSTATUS NTAPI QxpUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return QxpComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

NTSTATUS NTAPI QxpCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    QXP_FILE *file;

    UNREFERENCED_PARAMETER(DeviceObject);
    stack = IoGetCurrentIrpStackLocation(Irp);
    if (!QxpCallerContext(Irp)) {
        return QxpComplete(Irp, STATUS_ACCESS_DENIED, 0);
    }
    if (stack->FileObject->FileName.Length != 0) {
        return QxpComplete(Irp, STATUS_OBJECT_NAME_NOT_FOUND, 0);
    }
    file = (QXP_FILE *)ExAllocatePoolWithTag(NonPagedPool,
        sizeof(*file), QXPMAP_POOL_TAG);
    if (file == NULL) {
        return QxpComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlZeroMemory(file, sizeof(*file));
    file->Process = PsGetCurrentProcess();
    ObReferenceObject(file->Process);
    file->Slot = QXPMAP_NO_SLOT;
    stack->FileObject->FsContext = file;
    return QxpComplete(Irp, STATUS_SUCCESS, FILE_OPENED);
}

NTSTATUS NTAPI QxpCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    QXP_DEVICE *device;
    QXP_FILE *file;
    NTSTATUS status;

    device = (QXP_DEVICE *)DeviceObject->DeviceExtension;
    file = (QXP_FILE *)IoGetCurrentIrpStackLocation(Irp)->FileObject->FsContext;
    status = STATUS_SUCCESS;
    if (file != NULL) {
        QxpLock(device);
        status = QxpCleanFile(device, file);
        QxpUnlock(device);
    }
    return QxpComplete(Irp, status, 0);
}

NTSTATUS NTAPI QxpClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    QXP_DEVICE *device;
    PFILE_OBJECT fileObject;
    QXP_FILE *file;
    NTSTATUS status;

    device = (QXP_DEVICE *)DeviceObject->DeviceExtension;
    fileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    file = (QXP_FILE *)fileObject->FsContext;
    status = STATUS_SUCCESS;
    if (file != NULL) {
        QxpLock(device);
        status = QxpCleanFile(device, file);
        fileObject->FsContext = NULL;
        if (!NT_SUCCESS(status)) {
            /* Fail closed: never free/reassign a still-writable slot or
             * unload code while an orphaned context owns live mappings.
             * Only reboot recovers this unexpected MM failure. */
            DeviceObject->DriverObject->DriverUnload = NULL;
            QxpUnlock(device);
            return QxpComplete(Irp, status, 0);
        }
        QxpUnlock(device);
        ObDereferenceObject(file->Process);
        ExFreePoolWithTag(file, QXPMAP_POOL_TAG);
    }
    return QxpComplete(Irp, status, 0);
}

static NTSTATUS QxpClaim(QXP_DEVICE *Device, QXP_FILE *File,
                        QXPMAP_QD3D_SLOT *Result)
{
    NTSTATUS status;
    ULONG count;
    ULONG slot;

    if (File->Slot != QXPMAP_NO_SLOT) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    /* Retain and retry any view left by an unsuccessful mapping rollback. */
    status = QxpReleaseSlot(Device, File);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    count = QxpSlotCount(Device);
    if (count == 0) {
        return STATUS_NO_SUCH_DEVICE;
    }
    for (slot = 0; slot < count; ++slot) {
        if (Device->Slots[slot] == NULL) {
            break;
        }
    }
    if (slot == count) {
        return STATUS_DEVICE_BUSY;
    }
    status = QxpMap(Device, QXPMAP_QD3D_MMIO_GPA, QXPMAP_QD3D_MMIO_SIZE,
        PAGE_READWRITE | PAGE_NOCACHE, &File->Registers);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* fxptl.sys uses Win32Protect=4 (PAGE_READWRITE), not PAGE_NOCACHE.
     * Preserve that cache policy for the RAM-backed command channel. */
    status = QxpMap(Device, QXPMAP_QD3D_CHANNEL_GPA +
        slot * QXPMAP_QD3D_CHANNEL_SIZE, QXPMAP_QD3D_CHANNEL_SIZE,
        PAGE_READWRITE, &File->Channel);
    if (!NT_SUCCESS(status)) {
        (VOID)QxpUnmap(&File->Registers);
        return status;
    }
    File->SlotOwner = PsGetCurrentProcess();
    ObReferenceObject(File->SlotOwner);
    Device->Slots[slot] = File;
    File->Slot = slot;
    Result->Slot = slot;
    Result->ChannelSize = QXPMAP_QD3D_CHANNEL_SIZE;
    Result->Registers = (ULONGLONG)(ULONG_PTR)File->Registers;
    Result->Channel = (ULONGLONG)(ULONG_PTR)File->Channel;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI QxpDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    QXP_DEVICE *device;
    QXP_FILE *file;
    PIO_STACK_LOCATION stack;
    PVOID buffer;
    ULONG input;
    ULONG output;
    ULONG code;
    ULONG index;
    ULONG_PTR information;
    NTSTATUS status;
    QXPMAP_INFO *info;
    QXPMAP_WINDOW_REQUEST request;
    QXPMAP_MAPPING mapping;

    stack = IoGetCurrentIrpStackLocation(Irp);
    device = (QXP_DEVICE *)DeviceObject->DeviceExtension;
    file = (QXP_FILE *)stack->FileObject->FsContext;
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    if (!QxpCallerContext(Irp) || file == NULL ||
        file->Process != PsGetCurrentProcess()) {
        return QxpComplete(Irp, STATUS_ACCESS_DENIED, 0);
    }
    if ((code & 3) != METHOD_BUFFERED) {
        return QxpComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    input = stack->Parameters.DeviceIoControl.InputBufferLength;
    output = stack->Parameters.DeviceIoControl.OutputBufferLength;
    buffer = Irp->AssociatedIrp.SystemBuffer;
    if ((input != 0 || output != 0) &&
        (buffer == NULL || ((ULONG_PTR)buffer & 7) != 0)) {
        return QxpComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    information = 0;
    status = STATUS_INVALID_DEVICE_REQUEST;
    QxpLock(device);
    if (file->Cleaned) {
        QxpUnlock(device);
        return QxpComplete(Irp, STATUS_FILE_CLOSED, 0);
    }
    switch (code) {
    case IOCTL_QXPMAP_QUERY:
        if (input != 0 || output < sizeof(QXPMAP_INFO)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        info = (QXPMAP_INFO *)buffer;
        RtlZeroMemory(info, sizeof(*info));
        info->Version = QXPMAP_VERSION;
        info->Qd3dSlots = QxpSlotCount(device);
        info->Qd3dSlotSize = QXPMAP_QD3D_CHANNEL_SIZE;
        info->Windows = QXPMAP_WINDOWS_MASK;
        for (index = 0; index < info->Qd3dSlots; ++index) {
            if (device->Slots[index] == NULL) {
                ++info->Qd3dSlotsFree;
            }
        }
        information = sizeof(*info);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_QXPMAP_QD3D_CLAIM:
        if (input != 0 || output < sizeof(QXPMAP_QD3D_SLOT)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        status = QxpClaim(device, file, (QXPMAP_QD3D_SLOT *)buffer);
        if (NT_SUCCESS(status)) {
            information = sizeof(QXPMAP_QD3D_SLOT);
        } else {
            QxpTrace("claim-failed", (ULONG)status);
        }
        break;
    case IOCTL_QXPMAP_QD3D_RELEASE:
        if (input != 0 || output != 0) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        status = QxpReleaseSlot(device, file);
        break;
    case IOCTL_QXPMAP_MAP_WINDOW:
        if (input != sizeof(request) || output < sizeof(mapping)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        RtlCopyMemory(&request, buffer, sizeof(request));
        if (request.Reserved != 0 || request.Window == 0 ||
            request.Window > QXPMAP_WINDOW_COUNT) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        index = request.Window - 1;
        if (file->Windows[index] != NULL) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        status = QxpMap(device, QxpWindows[index].Physical,
            QxpWindows[index].Size, QxpWindows[index].Protect,
            &file->Windows[index]);
        if (NT_SUCCESS(status)) {
            mapping.Address = (ULONGLONG)(ULONG_PTR)file->Windows[index];
            mapping.Size = QxpWindows[index].Size;
            mapping.Window = request.Window;
            RtlCopyMemory(buffer, &mapping, sizeof(mapping));
            information = sizeof(mapping);
        }
        break;
    case IOCTL_QXPMAP_UNMAP_WINDOW:
        if (input != sizeof(mapping) || output != 0) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        RtlCopyMemory(&mapping, buffer, sizeof(mapping));
        if (mapping.Window == 0 || mapping.Window > QXPMAP_WINDOW_COUNT ||
            mapping.Address == 0 || (mapping.Address & (PAGE_SIZE - 1)) != 0 ||
            mapping.Address > (ULONGLONG)(ULONG_PTR)MmHighestUserAddress) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        index = mapping.Window - 1;
        if (mapping.Size != QxpWindows[index].Size ||
            mapping.Address != (ULONGLONG)(ULONG_PTR)file->Windows[index]) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = QxpUnmap(&file->Windows[index]);
        break;
    default:
        break;
    }
    QxpUnlock(device);
    return QxpComplete(Irp, status, information);
}

VOID NTAPI QxpUnload(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT deviceObject;
    QXP_DEVICE *device;
    UNICODE_STRING link;

    /* The I/O manager does not unload while open file objects reference
     * this device; every context has already reached CLEANUP/CLOSE. */
    deviceObject = DriverObject->DeviceObject;
    device = (QXP_DEVICE *)deviceObject->DeviceExtension;
    RtlInitUnicodeString(&link, QXPMAP_DOS_NAME);
    (VOID)IoDeleteSymbolicLink(&link);
    MmUnmapIoSpace(device->Registers, QXPMAP_QD3D_MMIO_SIZE);
    (VOID)ZwClose(device->PhysicalMemory);
    IoDeleteDevice(deviceObject);
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath)
{
    PDEVICE_OBJECT deviceObject;
    QXP_DEVICE *device;
    UNICODE_STRING name;
    UNICODE_STRING link;
    OBJECT_ATTRIBUTES attributes;
    PHYSICAL_ADDRESS physical;
    NTSTATUS status;
    ULONG index;

    UNREFERENCED_PARAMETER(RegistryPath);
    QxpTrace("load", 0);
    RtlInitUnicodeString(&name, QXPMAP_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, sizeof(QXP_DEVICE), &name,
        QXPMAP_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN, FALSE, &deviceObject);
    QxpTrace("create-device", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    device = (QXP_DEVICE *)deviceObject->DeviceExtension;
    RtlZeroMemory(device, sizeof(*device));
    KeInitializeMutex(&device->Mutex, 0);
    RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
    InitializeObjectAttributes(&attributes, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenSection(&device->PhysicalMemory,
        SECTION_MAP_READ | SECTION_MAP_WRITE, &attributes);
    QxpTrace("open-physical-memory", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }
    physical.QuadPart = (LONGLONG)(ULONGLONG)QXPMAP_QD3D_MMIO_GPA;
    device->Registers = (PUCHAR)MmMapIoSpace(physical,
        QXPMAP_QD3D_MMIO_SIZE, MmNonCached);
    QxpTrace("map-registers", (ULONG)(ULONG_PTR)device->Registers);
    if (device->Registers == NULL) {
        (VOID)ZwClose(device->PhysicalMemory);
        IoDeleteDevice(deviceObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    QxpTrace("qd3d-id", READ_REGISTER_ULONG((PULONG)(device->Registers +
        QXPMAP_QD3D_REG_ID)));
    QxpTrace("qd3d-slots", QxpSlotCount(device));
    for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index) {
        DriverObject->MajorFunction[index] = QxpUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QxpCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QxpClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = QxpCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QxpDeviceControl;
    DriverObject->DriverUnload = QxpUnload;
    RtlInitUnicodeString(&name, QXPMAP_DEVICE_NAME);
    RtlInitUnicodeString(&link, QXPMAP_DOS_NAME);
    status = IoCreateSymbolicLink(&link, &name);
    QxpTrace("create-link", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        MmUnmapIoSpace(device->Registers, QXPMAP_QD3D_MMIO_SIZE);
        (VOID)ZwClose(device->PhysicalMemory);
        IoDeleteDevice(deviceObject);
        return status;
    }
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    QxpTrace("ready", 0);
    return STATUS_SUCCESS;
}
