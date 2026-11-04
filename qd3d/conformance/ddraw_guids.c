/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Defines the GUIDs that <ddraw.h> declares. mingw-w64's libdxguid lacks
 * IID_IDirectDraw3, which Wine's dxguid has and the DirectDraw tests use;
 * the other definitions are DECLSPEC_SELECTANY and merge with libdxguid's.
 */
#include <initguid.h>
#include <ddraw.h>
