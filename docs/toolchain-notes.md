# ICD toolchain notes (Step 3)

Written 2026-07-10, against `refs/qemu-3dfx` @ `b47f30a` (shallow clone). Verdict up
front: **modern MinGW cross-build from WSL works out of the box; no fork-vs-vendor
problem; derived ICD is GPLv2.**

## Verified by test build

Built `wrappers/mesa` → `opengl32.dll` from WSL (Ubuntu, `i686-w64-mingw32-gcc` GCC
13-win32, `gendef` from mingw-w64-tools) using the upstream flow:

```
cd refs/qemu-3dfx/wrappers/mesa && mkdir build && cd build
bash ../../../scripts/conf_wrapper   # see caveats below
make
```

Result: clean build, upstream's own `exports-check` (2977 exports) passed. PE header
of the produced DLL:

- `MajorSubsystemVersion 4.0` / `MajorOSystemVersion 4.0` — loads on XP (and 9x-era
  loaders); the modern-binutils default is *not* 6.0 here, so no
  `-Wl,--subsystem,windows:5.01` needed. Harmless to add for explicitness.
- Imports: `KERNEL32.dll`, `msvcrt.dll`, `USER32.dll` only. The GCC "-win32" flavor +
  `-static-libgcc` keeps it off ucrt/winpthreads. msvcrt linkage is XP-native.
- ~1.06 MB, `--nxcompat --dynamicbase --no-seh` — fine on XP SP2+.

## Build-system shape (fork-vs-vendor call)

There is no real build system to fork: `wrappers/mesa/src/Makefile.in` is ~70 lines;
`scripts/conf_wrapper` just substitutes `CROSS=` and strips the MSYS2 guard for
Linux/Darwin cross builds (explicitly supported, `conf_wrapper:55-58`). The ICD gets
its own small Makefile borrowing the flags; nothing to vendor beyond the source files
we're deriving from anyway.

Headers: the wrapper compiles against `qemu-1/hw/mesa` inside the qemu-3dfx repo
(`-I$(QEMU_SRC_DIR)/hw/mesa`, for `mglfuncs.h` etc.). The ICD must instead compile
against **qemu-libretro's** `hw/mesa` headers so guest and host agree on the ABI the
core actually implements.

Export mechanics worth knowing: upstream generates the .def with `gendef` and a sed
that renames internal `mgl*` symbols to `wgl*` exports (`Makefile.in`, `$(TARGET)`
rule). The ICD exports ~40 `Drv*` symbols instead — a hand-written .def is simpler
than porting that machinery.

## Caveats hit during the test build

1. **CRLF checkout breaks the scripts under WSL.** This clone was checked out with
   CRLF; `conf_wrapper`'s shebang line fails from /mnt/c (`not found`). Fix:
   `tr -d '\r' < scripts/conf_wrapper > /tmp/cw && bash /tmp/cw`. Also it uses `==`
   inside `[ ]`, so it needs bash, not dash — invoke with `bash`, not `sh`.
2. **`-march=x86-64-v2`** (`Makefile.in` CFLAGS) is accepted for 32-bit output but
   raises the guest-CPU ISA floor to SSE4.2/POPCNT era. Fine when the VM runs with a
   modern CPU model; the ICD build should consider `-march=pentium4` (SSE2) or lower
   so a conservative XP guest CPU model can't fault. Note `wrapgl32.c` already has
   `COMPACT_FRAME` paths compiled `no-sse2` for specific functions — SSE assumptions
   are deliberate and per-function upstream.
3. `stamp.o` rule runs `git rev-parse --short HEAD` — builds must happen inside a git
   clone (or the rule needs replacing in the ICD Makefile).

## License

`refs/qemu-3dfx/LICENSE` is plain GPLv2 ("Version 2, June 1991"; no "or later"
wording in the grant — treat as GPLv2). Source files carry no per-file license
headers (`wrapgl32.c` header block is bare), so the repo LICENSE governs. The ICD
derived from this code is therefore **GPLv2**; ship sources alongside binaries in the
guest driver pack. No conflict with the GPLv2 qemu-libretro core; note mesa9x/ReactOS
code (MIT/GPL respectively) can be referenced for interface shape, but avoid copying
ReactOS GPL code into non-GPL contexts (moot here — everything ends up GPLv2).

## Real-XP compatibility check

- Use `stubicd.dll` and its console probes (`guest/stubicd/README.txt`) to check
  modern-MinGW binaries on XP SP3. Stock `opengl32.dll` must resolve the full ICD
  export surface, call validation/callback/pixel-format/context functions, and
  safely use the 336-entry dispatch table.

## Not yet verified

- windres path untested (no .rc in the mesa wrapper today; the ICD may want a VERSIONINFO
  resource — `i686-w64-mingw32-windres` is present in WSL).
