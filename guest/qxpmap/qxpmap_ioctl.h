/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qxpmap: XPDriver's guest memory-window driver, user interface.
 *
 * Replaces the qemu-3dfx MapMem driver (fxptl.sys, \\.\MAPMEM) for XPDriver's
 * own clients.  Unlike MapMem it is not exclusive, maps only the fixed
 * windows of the emulated devices (never a caller-supplied physical address),
 * offers no CPU register access, and ties every mapping to the handle that
 * made it: closing the handle, process exit included, undoes them.
 *
 * QD3D channel slots are never mapped by address: QD3D_CLAIM picks a free
 * slot for the calling handle (one per handle) and maps it with the QD3D
 * register page.  When the handle releases the slot (QD3D_RELEASE or close),
 * the driver unmaps both views, writes the slot number to the register page's
 * QD3D_VM_REG_RELEASE so the host ends that slot's session at once, and frees
 * the slot.  No process can map another process's slot.
 *
 * Shared by the driver (WDK 7.1, C89) and user-mode clients: the includer
 * provides CTL_CODE and the ULONG/ULONGLONG types (windows.h and winioctl.h,
 * or ntddk.h).  Every structure is fixed-size and 8-byte aligned so 32-bit
 * clients and the driver agree byte for byte.
 */
#ifndef QXPMAP_IOCTL_H
#define QXPMAP_IOCTL_H

#define QXPMAP_DEVICE_NAME  L"\\Device\\QxpMap"
#define QXPMAP_DOS_NAME     L"\\DosDevices\\QXPMAP"
#define QXPMAP_USER_PATH    "\\\\.\\QXPMAP"
#define QXPMAP_SERVICE_NAME "qxpmap"
#define QXPMAP_FILE_NAME    "qxpmap.sys"
#define QXPMAP_VERSION      1u

#define QXPMAP_DEVICE_TYPE 0x8317u
#define IOCTL_QXPMAP_QUERY        CTL_CODE(QXPMAP_DEVICE_TYPE, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QXPMAP_QD3D_CLAIM   CTL_CODE(QXPMAP_DEVICE_TYPE, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QXPMAP_QD3D_RELEASE CTL_CODE(QXPMAP_DEVICE_TYPE, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QXPMAP_MAP_WINDOW   CTL_CODE(QXPMAP_DEVICE_TYPE, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QXPMAP_UNMAP_WINDOW CTL_CODE(QXPMAP_DEVICE_TYPE, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Fixed windows MAP_WINDOW serves (qemu-3dfx Mesa pass-through, used by the
 * OpenGL ICD guest/qxpicd).  Guest-physical base and size, in this order. */
#define QXPMAP_WINDOW_MESA_REGS  1u /* 0xefffe000, 0x00001000 */
#define QXPMAP_WINDOW_MESA_FIFO  2u /* 0xec000000, 0x03ffc000 */
#define QXPMAP_WINDOW_MESA_FBTM  3u /* 0xea000000, 0x02000000 */
#define QXPMAP_WINDOW_MESA_MBUFO 4u /* 0xe0000000, 0x08000000 */
#define QXPMAP_WINDOW_COUNT      4u

/* QUERY output: what the machine offers right now. */
typedef struct QXPMAP_INFO {
    ULONG Version;       /* QXPMAP_VERSION */
    ULONG Qd3dSlots;     /* channel slots the QD3D device serves; 0 = no QD3D device */
    ULONG Qd3dSlotSize;  /* bytes per slot */
    ULONG Qd3dSlotsFree; /* slots no handle has claimed */
    ULONG Qd3dAbi;       /* reserved for the host's QD3D ABI if the register page reports it; 0 otherwise */
    ULONG Windows;       /* bit (1u << id) set for each QXPMAP_WINDOW_* this driver serves */
    ULONG Reserved[2];
} QXPMAP_INFO;

/* QD3D_CLAIM output (no input).  Fails with STATUS_DEVICE_BUSY when every
 * slot is claimed, STATUS_NO_SUCH_DEVICE without a QD3D device, and
 * STATUS_INVALID_DEVICE_REQUEST when this handle already holds a slot. */
typedef struct QXPMAP_QD3D_SLOT {
    ULONG Slot;            /* 0 .. Qd3dSlots - 1 */
    ULONG ChannelSize;     /* bytes mapped at Channel */
    ULONGLONG Registers;   /* user address of the QD3D register page (4096 bytes) */
    ULONGLONG Channel;     /* user address of the slot's channel */
} QXPMAP_QD3D_SLOT;

/* MAP_WINDOW input; output QXPMAP_MAPPING.  UNMAP_WINDOW input: the
 * QXPMAP_MAPPING returned (Address and Window must match one this handle
 * made). */
typedef struct QXPMAP_WINDOW_REQUEST {
    ULONG Window;          /* QXPMAP_WINDOW_* */
    ULONG Reserved;        /* zero */
} QXPMAP_WINDOW_REQUEST;

typedef struct QXPMAP_MAPPING {
    ULONGLONG Address;     /* user address in the calling process */
    ULONG Size;            /* bytes */
    ULONG Window;          /* QXPMAP_WINDOW_* */
} QXPMAP_MAPPING;

#endif /* QXPMAP_IOCTL_H */
