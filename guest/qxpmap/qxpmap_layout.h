/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QXPMAP_LAYOUT_H
#define QXPMAP_LAYOUT_H

/* WDK 7.1 has no stdint.h. The probe compile checks these against the
 * authoritative qd3d/proto/qd3d_proto.h before it can be built. */
#define QXPMAP_QD3D_MAGIC        0x44334451UL
#define QXPMAP_QD3D_SLOTS        4UL
#define QXPMAP_QD3D_CHANNEL_GPA  0xd0000000UL
#define QXPMAP_QD3D_CHANNEL_SIZE (32UL * 1024UL * 1024UL)
#define QXPMAP_QD3D_MMIO_GPA     0xd8000000UL
#define QXPMAP_QD3D_MMIO_SIZE    4096UL
#define QXPMAP_QD3D_REG_KICK     0x000UL
#define QXPMAP_QD3D_REG_ID       0x004UL
#define QXPMAP_QD3D_REG_SLOTS    0x008UL
#define QXPMAP_QD3D_REG_RELEASE  0x00cUL

#define QXPMAP_MESA_REGS_GPA     0xefffe000UL
#define QXPMAP_MESA_REGS_SIZE    0x00001000UL
#define QXPMAP_MESA_FIFO_GPA     0xec000000UL
#define QXPMAP_MESA_FIFO_SIZE    0x03ffc000UL
#define QXPMAP_MESA_FBTM_GPA     0xea000000UL
#define QXPMAP_MESA_FBTM_SIZE    0x02000000UL
#define QXPMAP_MESA_MBUFO_GPA    0xe0000000UL
#define QXPMAP_MESA_MBUFO_SIZE   0x08000000UL
#define QXPMAP_WINDOWS_MASK     0x0000001eUL

#endif
