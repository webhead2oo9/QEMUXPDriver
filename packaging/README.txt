XPDriver @VERSION@
==================

XPDriver gives Windows XP guests of the qemu-libretro core (RetroArch) real
3D acceleration on your PC's graphics card:

  - Direct3D 1-9 and DirectDraw (QD3D): XPDriver's d3d9.dll, d3d8.dll and
    ddraw.dll replace Microsoft's in System32 and forward every call to the
    host, which replays it on Direct3D 9Ex. Microsoft's DLLs stay beside
    them, as the fallback and for uninstalling;
  - OpenGL: Microsoft's opengl32.dll loads XPDriver's OpenGL driver (the
    standard driver mechanism), which forwards to the host's OpenGL;
  - a Windows XP display driver for the desktop and its display modes.

Games need no patches and nothing copied into their folders.


Contents
--------

  retroarch\cores\qemu_xpdriver_libretro.dll   the core: QEMU with XPDriver's
                                               devices (does not replace an
                                               existing QEMU core)
  retroarch\system\qemu\                       firmware the core needs
  vm\xpdriver.qemu_cmd_line                    starts Windows XP
  vm\xpdriver-install.qemu_cmd_line            installs Windows XP from your CD
  vm\winxp.qcow2                               an empty 32 GB disk for it
  vm\xpdriver-tools.iso                        the XPDriver CD for inside XP
  xpdm\                                        the display driver package (also
                                               on the CD), xpdriver-test.cer
  MANIFEST.txt                                 versions, commits, file hashes
  LICENSE.txt, THIRD_PARTY_NOTICES.txt         XPDriver's licence and notices
  licenses\                                    the core's licences (QEMU) and
                                               its firmware notes (pc-bios)


Requirements
------------

  - Windows 10 or 11, 64-bit, with "Windows Hypervisor Platform" turned on
    (Control Panel > Programs > Turn Windows features on or off). Without it
    the core falls back to much slower software emulation.
  - RetroArch for Windows, 64-bit.
  - A graphics card with current Direct3D 9 and OpenGL drivers.
  - Your own Windows XP SP3 (32-bit) CD image. It is not included.


Setup
-----

  1. Copy retroarch\cores\qemu_xpdriver_libretro.dll into RetroArch's
     "cores" folder and retroarch\system\qemu\ into RetroArch's "system"
     folder (so it becomes system\qemu\).

  2. Copy your Windows XP CD image into the vm folder as winxp.iso.

  3. In RetroArch: Load Core > qemu_xpdriver, then Load Content >
     vm\xpdriver-install.qemu_cmd_line. Install Windows XP onto the empty
     disk as usual; the VM boots from the CD once, then from the disk.

  4. Shut Windows XP down (Start > Turn Off Computer); RetroArch closes with
     it. Closing RetroArch while Windows runs is like pulling the plug. From
     now on load vm\xpdriver.qemu_cmd_line instead: Windows XP starts with the
     XPDriver CD in its second CD drive.

  5. In Windows XP, open the XPDriver CD and run SETUP.CMD. It installs QD3D
     for every program and the XPDriver display package, then restarts
     Windows. Afterwards set the resolution you like in Display Properties.

Keyboard and mouse: RetroArch's "Game Focus" mode (Scroll Lock by default)
passes every key to Windows XP; press it again to reach RetroArch's menu.


Using it
--------

Install and run games as on a real PC.

On the XPDriver CD:

  INSTALL\status.cmd      what is installed (QD3D files, mapping driver)
  INSTALL\scan.cmd        lists per-game d3d8/d3d9/ddraw/opengl32 DLLs (old
                          wrappers) that bypass the system-wide install;
                          "scan.cmd --move-leftovers" renames them aside,
                          "scan.cmd --restore-leftovers" renames them back
  INSTALL\uninstall.cmd   restores Microsoft's DLLs exactly
  RUNTESTS.CMD            self-tests (results in C:\QD3D\results.txt)

Programs that cannot use XPDriver (no host connection) fall back to
Microsoft's own runtimes. C:\Documents and Settings\<user>\Local Settings\
Temp\qd3d.log records why.


Uninstalling
------------

Run INSTALL\uninstall.cmd on the XPDriver CD to restore Microsoft's
Direct3D and DirectDraw DLLs. To replace the display driver, open Device
Manager > Display adapters > the adapter > Update Driver, choose "Install
from a list", "Don't search", and pick Windows' "Standard VGA Graphics
Adapter".


Source code and licence
-----------------------

XPDriver is free software under the GNU General Public License version 2
(LICENSE.txt); THIRD_PARTY_NOTICES.txt lists the components it includes.
The exact source of this release: XPDriver commit @XPD_COMMIT@ and core
(qemu-libretro) commit @CORE_COMMIT@ (see MANIFEST.txt), at
@SOURCE_URL@.

The display driver package is signed with XPDriver's own test certificate
(xpdriver-test.cer), not by Microsoft; SETUP.CMD trusts it inside the VM.
