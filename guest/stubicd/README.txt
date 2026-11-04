XPDriver real-XP ICD fallback battery
=====================================

Run all commands from an Administrator command prompt.

1. Run icdinfo.exe. A usable VirtualBox/VMware display driver must report:
     QUERYESCSUPPORT(OPENGL_GETINFO) != 0
     OPENGL_GETINFO > 0
     DriverName=<name>

2. Save the displayed DriverName and install the diagnostic ICD:
     icdhijack.exe install <DriverName>

   This copies stubicd.dll to system32, preserves the existing Dll value in
   XPDriverOriginalDll, and changes only Dll. Version and DriverVersion are
   deliberately left untouched so they still match the display-driver escape.

   If the installed display driver does not answer OPENGL_GETINFO (including
   VirtualBox 7.2's XP driver), install the narrow user-mode test substitute:

     icdhijack.exe shim-install
     glprobe.exe --mock-icd-escape

   This patches only XP opengl32.dll's imported GDI32!ExtEscape pointer inside
   glprobe, emulating the ICD-discovery signal that an XPDM display driver
   provides. The escape returns {Version=2, DriverVersion=1,
   DriverName=XPDriverStub}; the matching registry key contains
   Dll=stubicd.dll, Version=2, DriverVersion=1, and Flags=1. The Microsoft ICD
   loader and every subsequent loader/Drv call remain real. Add --trace-loader
   for verbose IAT diagnostics.

3. Select a mode and run the probe. Delete C:\stubicd.log and glprobe.log
   between numbered tests when convenient.

     setmode.cmd 1   ValidateVersion fails (T3)
     setmode.cmd 2   zero ICD formats (T4, design-critical)
     setmode.cmd 3   SetPixelFormat fails (T5)
     setmode.cmd 4   context creation fails (T6)
     setmode.cmd 5   SetContext fails (T7)
     setmode.cmd 6   inert successful context (T8)

     glprobe.exe
     glprobe.exe --rounds 2

   glprobe logs format flags, every WGL return, GetLastError, renderer strings,
   a same-window generic retry, and a fresh-window generic attempt. The ICD
   writes its call order to C:\stubicd.log and OutputDebugString.

   To test per-window re-querying in one process (T11), start in mode 2 and flip
   to mode 6 before a second window:
     setmode.cmd 2
     glprobe.exe --mock-icd-escape --flip-mode 6

4. Restore the original additions ICD before removing the VM snapshot:
     icdhijack.exe restore <DriverName>

   Or remove the user-mode test key:
     icdhijack.exe shim-remove

Expected load-bearing results:

  Mode 1 and mode 2 silently reach GDI Generic. Mode 2 must not call any
  context entry point. Mode 3 is app-visible but permits the same window to
  accept a generic format. Mode 4 poisons the first window, while the fresh
  window accepts a generic format. Mode 5 fails wglMakeCurrent without a
  crash. Mode 6 reports StubICD and SwapBuffers fails without a crash.
