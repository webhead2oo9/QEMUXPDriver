/* SPDX-License-Identifier: GPL-2.0-only */
/* A native PE fixture, never loaded as a driver. Only --root accepts its
 * deliberately different product identity. Production staging uses the WDK
 * qxpmap.sys, never this file. */
unsigned long __attribute__((stdcall)) fixture_entry(void *driver, void *registry)
{
    (void)driver;
    (void)registry;
    return 0;
}
