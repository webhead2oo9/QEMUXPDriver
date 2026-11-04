# Building XPDriver

Everything builds on a Windows 10 or 11 x64 host: the XP kernel-mode and
display driver parts with the Windows Driver Kit 7.1, everything else in WSL
with mingw-w64. Commands below run from the repository root, in `cmd` or
PowerShell unless marked as WSL.

## Prerequisites

Windows:

- **WDK 7.1.0** (7600.16385.1), the last Microsoft kit that targets Windows
  XP, installed at `C:\WinDDK\7600.16385.1` (or pass its root to the
  scripts). Microsoft distributes it as `GRMWDK_EN_7600_1.ISO`.
- **Python 3** with the `py` launcher (standard library only): the build
  wrappers generate the version header, validate the PE images and stamp the
  INF.
- **Git for Windows** for this checkout; Windows PowerShell 5.1 (built in).

WSL (Ubuntu; GCC 13 tested). The default `win32` thread-model alternatives
keep the guest binaries free of `winpthreads`:

```sh
sudo apt-get install build-essential gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-mingw-w64-x86-64 genisoimage qemu-utils python3 python3-venv \
    python3-pip ninja-build flex bison pkgconf git
```

## Checkout

Clone from WSL onto a Windows drive, so the Windows tools and WSL share one
tree and the core keeps the LF line endings QEMU's scripts need. The host
side is the `core/` submodule (the qemu-libretro fork, branch
`d3d9-remoting`), which needs two of its own submodules:

```sh
cd /mnt/c/src
git clone https://github.com/webhead2oo9/QEMUXPDriver.git
cd QEMUXPDriver
git submodule update --init core
git -C core submodule update --init libretro-common zlib
```

The core's `--zlib=internal` build compiles zlib's sources directly; do not
run zlib's own `./configure`, which rewrites tracked files and would mark
the core, and every release built from it, as modified.

The guest builds find the core at `core/`, else at a sibling
`../qemu-libretro-d3d9` checkout (`guest/paired-core.mk`). The core's
`hw/qd3d/proto` and `hw/qd3d/host` are copies of this repository's
`qd3d/proto` and `qd3d/host`; after changing those, run the core's
`scripts/qd3d-sync.sh <XPDriver checkout>` (WSL) and commit both.

## Build

1. **Display driver package** (WDK, then WSL for the user-mode parts):

   ```bat
   guest\xpdm\stage-package.cmd C:\WinDDK\7600.16385.1
   ```

   Builds `qxpmap.sys`, the miniport and display driver, the co-installer
   and the OpenGL ICD, stamps `DriverVer` into the staged INF, catalogs and
   signs the package into `guest\xpdm\build\package`. Warnings fail the
   driver builds (`/W4 /WX`).

2. **QD3D and the tools CD** (WSL):

   ```sh
   make -C qd3d/cd iso XPDM=1
   ```

   Builds the QD3D DLLs, the installer and the test programs, checks every
   guest binary's imports against Windows XP SP3, and writes
   `qd3d/cd/build/qd3d-tools.iso` with the staged package and `SETUP.CMD`.
   Without `XPDM=1` the CD carries QD3D only.

3. **The core** (WSL): configure `core/build-win64` with the Windows
   cross-build options from the core's README, then

   ```sh
   ninja -C core/build-win64 libqemu_libretro.dll
   ```

4. **Release bundle**:

   ```powershell
   tools\make-release.ps1 -SourceUrl https://github.com/webhead2oo9/QEMUXPDriver
   ```

   Refuses uncommitted changes, a core whose `hw/qd3d` copies differ from
   this checkout, and a missing project certificate (below); then builds
   steps 1–3 (the core in its own `build-release` directory, configured on
   first use), verifies the core DLL carries the core commit it is recorded
   with, and writes `dist\XPDriver-<version>\` with `MANIFEST.txt` and the
   zip. `-Rehearsal` builds anyway under a `-rehearsal` name.

## Versions

`VERSION` holds MAJOR.MINOR.PATCH; the build number is the commit count.
`tools/version.py` stamps it, with the commit, into every guest binary's
version resource and the package's `DriverVer` (dated by the commit), so
equal commits build equal versions.

## Signing

Without configuration, staging signs the package with a throwaway
self-signed certificate made for that run, which suits development. Releases
use one persistent project certificate, so users who trusted one release's
`xpdriver-test.cer` keep trusting the next. Create it once, outside the
repository, and keep the PFX safe:

```powershell
tools\new-signing-cert.ps1 -Path D:\keys\xpdriver-signing.pfx
$env:XPD_SIGN_PFX = 'D:\keys\xpdriver-signing.pfx'
$env:XPD_SIGN_PFX_PASSWORD = '...'   # optional; prompted for otherwise
```

Neither certificate is trusted by Windows until imported; inside the VM,
`SETUP.CMD` imports the bundled one.

## Tests

Host-side checks (WSL):

| Command | Covers |
|---|---|
| `make -C guest/xpdm test` | XPDM unit tests, co-installer with a fake registry and SCM, INF validation |
| `make -C guest/qxpicd coverage` | OpenGL ICD dispatch coverage |
| `make -C qd3d/install test` | The system-wide installer against fixtures |
| `make -C qd3d/tests run-one-proxy` | One QD3D proxy per process across front ends |
| `make -C qd3d/d3d8 run-stock run-fallback`, `make -C qd3d/ddraw run-loopback` | Front-end scenes on the Windows host through the host executor |

VM acceptance runs use the harness under `qemu/harness/`; its targets are
registered in `qemu/harness/test-targets.json`.
