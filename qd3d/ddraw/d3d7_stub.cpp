/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * qdd: the Direct3D hooks of qdd.h without Direct3D ("make D3D7=0"): a
 * DirectDraw-only build for testing the core on its own.  The product
 * build links qdd_d3d7*.cpp instead.
 */
#include "qdd.h"

HRESULT qdd_d3d7_query(QddDraw *dd, REFIID riid, void **out)
{
    *out = NULL;
    return E_NOINTERFACE;
}

void qdd_d3d7_draw_destroy(QddDraw *dd)
{
}

void qdd_d3d7_surface_destroy(QddSurface *s)
{
}

void qdd_d3d7_before_reset(QddDraw *dd)
{
}

void qdd_d3d7_after_reset(QddDraw *dd)
{
}

void qdd_d3d7_flush(QddDraw *dd)
{
}
