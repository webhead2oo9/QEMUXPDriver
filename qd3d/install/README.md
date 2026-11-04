# QD3D system-wide installer (Windows XP SP3 x86)

`qd3d_install.exe` installs the `qxpmap` kernel service and puts the three
QD3D DLLs into `%SystemRoot%\system32`, so every game uses QD3D without
per-game copies. The driver image is
`%SystemRoot%\system32\drivers\qxpmap.sys`, shared with the signed XPDM
package's OpenGL ICD (`qxpicd`); no MapMem service is used.

| DLL | QD3D component | Stock DLL kept as | Stock fallback |
|---|---|---|---|
| `d3d9.dll` | Direct3D 9 proxy (`qd3d/guest`) | `qd3d9_orig.dll` | `load_stock` in `qd3d/guest/qd3d9_main.c` |
| `d3d8.dll` | Direct3D 8 front end (`qd3d/d3d8`) | `qd3d8_orig.dll` | `q8_orig_module` in `qd3d/d3d8/qd3d8_main.cpp` |
| `ddraw.dll` | DirectDraw and Direct3D 1-7 front end (`qd3d/ddraw`) | `qddraw_orig.dll` | `qdd_stock_module` in `qd3d/ddraw/qdd_main.cpp` |

Each component hands a process to its stock DLL in System32 when no QD3D
channel is available. The installer also lists per-game wrapper DLLs that
would shadow the system-wide install.

## Tools CD

`make -C qd3d/install stage` (after building `qd3d/guest`, `qd3d/d3d8`,
`qd3d/ddraw`, and the WDK XP x86 free build of `guest/qxpmap`) fills
`qd3d/install/build/QD3D/`. `stage-driver` copies the real driver from
`guest/qxpmap/objfre_wxp_x86/i386/qxpmap.sys` into `qd3d/install/build/`;
`stage` includes that copy and fails with a missing-driver diagnostic if
the WDK output is absent. It never substitutes the host-test fixture.

| File | Purpose |
|---|---|
| `qd3d_install.exe` | the installer (no C runtime; kernel32, advapi32 and version imports only) |
| `d3d9.dll`, `d3d8.dll`, `ddraw.dll` | the QD3D DLLs it installs |
| `qxpmap.sys` | the WDK-built guest memory-window driver |
| `install.cmd` | install or update the DLLs and driver |
| `uninstall.cmd` | restore the stock DLLs; remove the driver/service only when the OpenGL ICD does not use it |
| `status.cmd` | show every file's state and the qxpmap service/start policy |
| `scan.cmd` | list per-game wrapper DLLs (`scan.cmd --move-leftovers`, `scan.cmd --restore-leftovers`) |

Run them as an administrator. The wrappers pass extra arguments through,
return the program's exit code, and `pause` at the end unless `QD3D_NOPAUSE`
is set (for unattended runs, or call `qd3d_install.exe` directly).

## Usage

```
qd3d_install [status|install|driver|uninstall|scan] [options]
  --from DIR           QD3D DLLs and qxpmap.sys (default: the program's directory)
  --scan-dir DIR       also scan DIR for wrapper DLLs (repeatable)
  --move-leftovers     scan: rename each wrapper DLL found to NAME.qd3d-bak
  --restore-leftovers  scan: rename NAME.qd3d-bak files back
  --root DIR           test mode, see "Testing on a development host"
```

For per-game testing, `qd3d_install driver --from DIR` installs or updates
only `qxpmap.sys`, creates/configures the kernel service and starts it.
`DIR` needs only the driver; System32 DLLs, saved originals, WFP caches and
game directories are untouched. Install the chosen QD3D DLLs beside the
game separately. The ordinary `uninstall` also removes a driver-only
installation without changing already-stock DLLs, unless the XPDriver
OpenGL ICD still needs the shared driver.

| Exit code | Meaning |
|---:|---|
| 0 | done (or nothing to do) |
| 1 | usage error |
| 2 | failed; rollback attempted (any incomplete restoration is reported explicitly) |
| 3 | done, but the change completes at the next restart ("Restart Windows to finish.") |
| 4 | refused, nothing changed (conflict, missing original, wrong or incomplete source, pending restart, not Windows XP) |

Each command ends with one machine-readable line, for example
`qd3d: install ok restart-required=no`, `qd3d: state=installed restart-pending=no`
or `qd3d: leftovers=3 moved=0 restored=0`. `status` also prints
`qxpmap: service=running start=SYSTEM_START` (or absent, stopped,
transitioning or delete-pending) and the installed `.sys` file version.
The overall state is `installed` only when all three DLLs and the running
driver are installed; `partial` includes a driver-only install or missing
DLL components; stopped, conflicting or missing-driver states are
`inconsistent`. A pending service deletion sets `restart-pending=yes`
even when no file operation remains.

Outside `--root`, changing commands run only on Windows XP x86 (5.1, not
WOW64); `status` and a plain `scan` are read-only and run anywhere.

## Which file is which

Every file is reported as one of:

- **QD3D**: an i386 PE DLL that contains the ASCII marker `QD3D` (the
  `QD3D_MARKER` RCDATA resource all three DLLs carry, which the front ends'
  stock-runtime checks also look for) and is not a Microsoft DLL.
- **Microsoft**: its version resource names Microsoft as the company and a
  Microsoft product ("Microsoft(R) Windows(R) Operating System"; localized XP
  builds translate the words around the name).
- **foreign**: anything else. Wine's DLLs, and so WineD3D's, give
  "Microsoft Corporation" as the company (Wine's `wine_common_ver.rc`: "GameGuard
  depends on this") but "Wine" as the product, so a WineD3D `ddraw.dll` is
  foreign, not Microsoft.

No stock file contains the marker (checked on the Windows XP SP3 `ddraw.dll`,
`d3dim.dll`, `d3dim700.dll`, `ddrawex.dll` and this host's stock DLLs). Install
sources must also export `Direct3DCreate9` (`d3d9.dll`), `Direct3DCreate8`
(`d3d8.dll`) or `DirectDrawCreate` (`ddraw.dll`), which catches swapped files,
and all three plus `qxpmap.sys` must be present: an incomplete tools CD is
refused rather than installed in part. Driver validation requires a native
i386 PE32 image with `OriginalFilename=qxpmap.sys` and
`ProductName=XPDriver`. This is identity checking, not signature
authentication. A foreign/unreadable driver file or a `qxpmap` service
whose type/image path conflicts is refused without changes.

## What install and uninstall do

For each of `d3d9.dll`, `d3d8.dll` and `ddraw.dll` (*T* = the DLL in
`system32`, *B* = its saved original there):

| State found | install | uninstall |
|---|---|---|
| T stock, no B (not installed) | T renamed to B, QD3D placed at T; refused if T is not a Microsoft DLL (another product's wrapper, WineD3D's included, must not become the fallback) | nothing to do |
| T QD3D, B stock (installed) | T replaced if it differs from the source | B renamed back to T, QD3D copy deleted |
| T stock and byte-identical to B (WFP or someone restored it) | the duplicate is replaced | B replaces the duplicate |
| T not QD3D and different from B | refused: keep the one you want as B, remove the other | refused |
| T QD3D or missing, no stock B | B recovered from a Microsoft copy in the WFP caches, else refused | T restored from that copy, else refused |
| B holds a QD3D file | the stray copy is removed and B saved properly | the stray copy is removed |

Only a Microsoft DLL becomes a saved original, a saved original is never
overwritten, and install warns when an existing one is not a Microsoft DLL. A
stock file is deleted only when a byte-identical saved original exists. Each
new file is copied next to its target (`NAME.qd3d-new`) and verified byte for
byte before it is renamed into place, so a program loading the DLL never sees
a partial file. A replaced QD3D file is renamed to `NAME.qd3d-old` and deleted
at the end. An install over a `d3d9.dll`/`d3d8.dll`-only install leaves those
two as they are and adds `ddraw.dll`.

### qxpmap service and boot ordering

The service uses `CreateServiceW("qxpmap", SERVICE_KERNEL_DRIVER, ...)`,
`SERVICE_SYSTEM_START`, `SERVICE_ERROR_NORMAL`, and the image path
`\SystemRoot\system32\drivers\qxpmap.sys`. `StartServiceW` makes it available
immediately. SYSTEM_START loads the mapping driver during kernel startup,
before ordinary applications need its device; BOOT_START is unnecessary
because it is not needed to access the boot disk.

The signed XPDM package installs this same service for the OpenGL ICD.
Service identity checks (including `status`) accept these ImagePath forms,
case-insensitively, and no others:

- `\SystemRoot\system32\drivers\qxpmap.sys`
- `system32\drivers\qxpmap.sys` (SetupAPI's `%12%\qxpmap.sys`)
- `%SystemRoot%\system32\drivers\qxpmap.sys`
- `\??\<windir>\system32\drivers\qxpmap.sys`
- `<windir>\system32\drivers\qxpmap.sys`

A different image path or non-kernel service remains a refusal without
changes, even when OpenGL is registered.

Before replacing a driver, the installer compares the numeric VERSIONINFO
**FILEVERSION**, not its display string or PRODUCTVERSION. A higher installed
version is preserved and reports
`qxpmap.sys: newer version a.b.c.d already installed`. The service is still
created if absent, normalized to SYSTEM_START and started if stopped.
Equal or lower versions use the usual byte comparison: different bytes are
replaced even when their FILEVERSIONs match. An installed image whose fixed
FILEVERSION cannot be read reports
`qxpmap.sys: installed FILEVERSION unreadable; using byte comparison` and
uses that same byte-comparison logic; existing image-identity checks still
apply. A carried image without a readable FILEVERSION is refused because
its update ordering cannot be established.

An ordinary update verifies a new copy beside the driver, stops the
service, renames the old image aside, replaces it and starts the service
again. An identical image is not stopped or rewritten. A stopped service
is started, and a successful `driver` or `install` normalizes its startup
policy to SYSTEM_START.

If clients prevent stopping the driver, or the image cannot be renamed
because a file handle denies delete sharing, the verified new copy is
scheduled with `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` and exit 3
reports that the replacement is not active yet. **That transaction sets
AUTO_START temporarily:** SYSTEM_START drivers load *before* smss applies
pending file renames; leaving SYSTEM_START would load the old driver again
on the replacement boot. AUTO_START instead lets SCM load the new file
*after* smss. `status` explicitly reports this replacement-boot policy.
The next successful `driver` or `install` restores SYSTEM_START. There is
no RunOnce command, helper service or copied bootstrap executable.

Uninstall first checks
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd`.
When that key exists, the service and image are left untouched: no stop,
startup-policy change, deletion or reboot-time removal. It reports
`qxpmap: kept because the XPDriver OpenGL driver (qxpicd) uses it.`
and succeeds while still restoring the stock QD3D-replaced DLLs. Remove
the OpenGL registration through the XPDM package before asking QD3D to
remove the shared driver. A failed registry lookup other than an absent
key refuses removal without changes.

Without that key, uninstall stops the driver, removes the service with
`DeleteService`, and removes the image. A service still referenced by another SCM handle
may remain **marked for deletion**, which is success with exit 3, not a
reason to recreate it. If clients prevent stopping, startup is disabled
before deletion and the file is scheduled for removal at reboot; the
running clients retain the old driver until then. The driver is not
WFP-protected and has no stock-original backup.


## Windows File Protection

On Windows XP, `d3d8.dll`, `d3d9.dll` and `ddraw.dll` are WFP-protected. WFP
reacts to a directory change notification: when a protected file no longer
matches its catalog signature it restores it silently from
`system32\dllcache`, or from the installation source (`ServicePackFiles\i386`
for files a service pack updated, then the network path or CD), and only asks
for the CD when no source is found
([KB 222193](https://ftp.zx.net.nz/pub/mirror/ftp.microsoft.com/MISC/KB/en-us/222/193.HTM)).

`ddraw.dll` is protected the way `d3d8.dll` and `d3d9.dll` are. The Windows XP
SP3 reference copy (`%TEMP%\qd3d-xp-ref\ddraw.dll`) is "Microsoft DirectDraw"
5.03.2600.5512 (xpsp.080413-0845), 279,552 bytes, product "Microsoft(R)
Windows(R) Operating System", with no embedded signature (empty security
directory): it is signed through the system catalogs, so any other file under
its name fails WFP's check. SP3 updated it, so on a system upgraded to SP3 a
copy is also in `ServicePackFiles\i386`, besides `dllcache`. At run time
`status` shows what `SfcIsFileProtected` says for each DLL.

The installer therefore:

1. **Suspends WFP per file.** Immediately before it changes a System32 DLL it
   calls `SfcFileException(0, path, -1)`, the unnamed export 5 of the Windows XP
   `sfc_os.dll`, which makes WFP ignore that file for about a minute
   ([bitsum, "Mod Method 3"](https://web.archive.org/web/20160321062438/bitsum.com/aboutwfp.asp)).
   VirtualBox's Guest Additions installer replaced `d3d8.dll` and `d3d9.dll`
   on XP with the same call (`DisableWFP` in its NSIS helper,
   `src/VBox/Additions/WINNT/Installer/InstallHelper/VBoxGuestInstallHelper.cpp`).
   `SfcIsFileProtected` (named export) decides whether the call is needed.
2. **Takes away WFP's silent stock sources.** The stock copies in the cache
   (`SFCDllCacheDir`, policy value first, default `system32\dllcache`) and in
   `ServicePackFiles\i386` (`ServicePackSourcePath`, and
   `%SystemRoot%\ServicePackFiles`) are renamed in place to their saved-original
   names (`qd3d9_orig.dll`, `qd3d8_orig.dll`, `qddraw_orig.dll`), so they stay
   byte-exact and uninstall renames them back. Nothing non-Microsoft is
   written into these directories. If WFP is ever triggered for these files
   (an exception that did not take, `sfc /scannow`), it finds no stock copy on
   disk and asks for the Windows XP CD instead of silently undoing the
   install; cancelling keeps QD3D. VirtualBox instead copied its own DLLs into
   `dllcache` and left `ServicePackFiles` alone; with WFP checking signatures,
   that `ServicePackFiles` copy would still be a silent stock source.
3. **Leaves the installation source alone.** `SourcePath` (CD or local
   `I386`) and the `Driver Cache` cabinets are never modified; `status` shows
   `SourcePath`.
4. **Falls back to the restart.** When the exception is unavailable or refused,
   every System32 change is scheduled with `MoveFileEx(...,
   MOVEFILE_DELAY_UNTIL_REBOOT)`; the session manager performs it before WFP
   starts. Exit code 3.

A service pack or hotfix that updates these DLLs, or `sfc /scannow` with the
CD inserted, puts the stock DLL back; `status` then reports "the stock DLL is
back in place" and running `install` again replaces it.

## ddraw.dll in a running Windows

More of Windows loads `ddraw.dll` than `d3d8.dll` or `d3d9.dll`. In the XP SP3
reference set, `d3dim.dll` and `d3dim700.dll` (the stock Direct3D 1-7 runtime
the stock DirectDraw loads) and `ddrawex.dll` import `ddraw.dll` by name, and
DirectShow's video renderers use DirectDraw. After the install all of them get
the QD3D front end; `qd3d/ddraw` forwards the stock runtime's internal exports
(`DDInternalLock`, `AcquireDDThreadLock`, `DSoundHelp`...) to
`qddraw_orig.dll` whenever the stock runtime is the one in use. Because
`ddraw.dll` is more often in use while installing, the in-use paths below
matter more for it.

## Files in use

- **Mapped by a running program** (a game or player using the DLL): renaming a
  loaded DLL is allowed, so the old file is renamed aside and the new one
  takes its name at once; new programs get it immediately, running ones keep
  the old file until they exit. The old file cannot be deleted while mapped
  and is deleted at the next restart (`MoveFileEx` with a NULL target). No
  restart is required.
- **Held open without `FILE_SHARE_DELETE`**: it cannot be renamed, so the new
  file waits as `NAME.qd3d-new` and replaces it at the next restart (exit 3).
  When the stock file is held like this during a first install, the saved
  original is a verified byte copy (content, last-write time and attributes)
  instead of the original file object. When an uninstall has to wait, a
  verified copy of the saved original replaces the QD3D file at the restart
  and the saved original is deleted after that; until then it stays, so the
  stock fallback keeps working.
- **Unreadable** (opened without read sharing): refused, close the program.
- While a restart-time operation on any managed DLL or driver is pending,
  `install`, `driver` and `uninstall` refuse to run (`status` shows it):
  restart first. A service marked for deletion also refuses installation;
  repeating uninstall accepts its pending deletion.

## Rollback

- **Automatic.** File changes are journaled until commit. Service type/image
  identity and the original running/startup state are captured before any
  mutation. Rollback stops a newly started driver before restoring files,
  deletes a newly created service, or restores an existing service's prior
  start type and running/stopped state after file restoration. This includes
  restoring SYSTEM_START after a failed deferred update.
  A restart-time replacement always takes a new file that rollback deletes:
  one scheduled before a later failure has no effect. Deletions of discarded
  temporary files and saved originals happen only after the transaction
  succeeds. `DeleteService` is the final fallible transactional step; if it
  fails after scheduling driver-file deletion, rollback withdraws exactly
  that deletion pair while retaining unrelated current pending records.
  File/service rollback errors are reported explicitly for manual recovery.
  The host test injects failures through every DLL and driver transaction
  stage, including SCM create/configure/start/stop/delete and reboot
  scheduling, checking exact file restoration and service state both before
  and after simulated reboot.
- **Uninstall** returns every stock file exactly; `scan --restore-leftovers`
  renames moved wrapper DLLs back.
- **Manual recovery** (for example from the Recovery Console or Safe Mode, if
  the installer cannot run): in `system32`, delete the QD3D `d3d9.dll`,
  `d3d8.dll` and `ddraw.dll` and rename `qd3d9_orig.dll`, `qd3d8_orig.dll`
  and `qddraw_orig.dll` back to them; in `system32\dllcache` and
  `ServicePackFiles\i386` rename the same saved-original names back. Files
  named `*.qd3d-new*` / `*.qd3d-old*` are temporaries and can be deleted. If
  no saved original is left, `expand` the stock DLL from the Windows XP SP3 CD
  (`I386\D3D9.DL_`, `I386\D3D8.DL_`, `I386\DDRAW.DL_`).
  Leave qxpmap in place while the XPDriver OpenGL ICD is registered. Otherwise
  stop/delete the service (`sc stop qxpmap`, `sc delete qxpmap`) and remove
  `system32\drivers\qxpmap.sys` after it is no longer in use; from the Recovery
  Console, disable `qxpmap` before removing its image.
  Do not delete a staged `.sys` needed by an intentionally pending update.

## Leftover wrapper DLLs

A game loads `d3d8.dll`, `d3d9.dll`, `ddraw.dll`, `opengl32.dll`... from its
own folder before System32 (they are not KnownDLLs), so such files bypass
QD3D, the OpenGL ICD or the stock runtime. `scan`, and every `install`, lists
`d3d8.dll`, `d3d9.dll`, `ddraw.dll`, `opengl32.dll`, `wined3d.dll`,
`libwine.dll`, `dxgi.dll`, `d3d10.dll`, `d3d10_1.dll`, `d3d10core.dll`,
`d3d11.dll`, `d3dim.dll`, `d3dim700.dll` and `d3dimm.dll` found under every
top-level directory of every fixed drive (Program Files, `C:\Games`,
`C:\GOG Games`, `C:\Sierra`...; not the Windows directory,
`Documents and Settings`, `RECYCLER` or `System Volume Information`), up to 12
levels deep without following junctions, plus each `--scan-dir`. The
installer's own directory and `--from` are skipped. Each entry is labelled as
above, for example:

```
  C:\Games\Old\ddraw.dll [QD3D: QD3D DirectDraw and Direct3D 7 front end 0.1.0.0, 814492 bytes]
  C:\Games\Wine\ddraw.dll [foreign: Microsoft Corporation, Wine, Wine DirectDraw 5.3.1.904, ... bytes]
  C:\Games\Copy\ddraw.dll [Microsoft: Microsoft DirectDraw 5.03.2600.5512 (xpsp.080413-0845), 279552 bytes]
  C:\Games\Patch\ddraw.dll [foreign: no version information, 13 bytes]
```

A QD3D copy is an old per-game test install; a foreign one is another
wrapper (WineD3D, dgVoodoo, DDrawCompat...). Nothing is renamed without
`--move-leftovers` (`NAME` to `NAME.qd3d-bak`, skipped if that name exists);
`--restore-leftovers` renames them back unless `NAME` exists again.

A per-game QD3D copy beside a system-wide install should go (or at least
match the installed build): a process then holds two QD3D proxies whenever
something loads `System32\d3d9.dll` by path, and each proxy takes a channel
slot of the four. Current builds avoid it from their side (a front end reuses
the QD3D `d3d9.dll` the process already has; a proxy never takes another QD3D
copy as its stock runtime); copies from before that fix load the system-wide
proxy as their "stock" runtime. `qd3d.log` names the file of every proxy that
connects, so a second one shows up there.

## Testing on a development host

Replace `<XPDriver checkout>` with the WSL path to your checkout:

```
wsl -e bash -lc "cd '<XPDriver checkout>' && make -C qd3d/install all check-imports test"
```

`--root DIR` makes `DIR` stand for the system drive: `DIR\WINDOWS\system32`,
`DIR\WINDOWS\system32\drivers`, the WFP cache directories, and the top-level
game directories. **Every SCM operation, including queries and rollback,
uses the fake backend under `--root` and cannot open the host SCM.** Its
service state is persisted only in `DIR\qxpmap-scm.bin`.
The OpenGL registration check also stays inside the fake root: a directory
at `DIR\registry\HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd`
represents the key's presence. No host registry key is read or written.

WFP is simulated (the System32 DLLs count as protected;
`--sim-wfp-deny` refuses the exception), and restart-time operations go to
`DIR\PendingFileRenameOperations.bin` instead of the registry, using its
pair format (`\??\source`, `!\??\target` or an empty target).
`--fail-after N` fails the Nth transactional change, including service
mutations and reboot scheduling. `--sim-scm-busy` models open driver
clients, `--sim-scm-delete-pending` models an outstanding SCM handle, and
`--sim-scm-fail query|create|config|start|stop|delete` fails the selected
SCM operation (not rollback). All fault/simulation flags require `--root`.

`test/run-host-test.ps1` builds such a root from this host's 32-bit stock
`d3d9.dll`, `d3d8.dll` and `ddraw.dll`, uses one copy of
`../guest/build/d3d9.dll`, `../d3d8/build/d3d8.dll` and
`../ddraw/build/ddraw.dll`, plus the native-PE
`build/qxpmap_fixture.sys` (1.0.0.0), `build/qxpmap_fixture_older.sys`
(0.65535.65535.65535), and `build/qxpmap_fixture_newer.sys` (1.0.0.1).
Modified copies cover equal-version/different-byte replacement and an
unreadable fixed version with intact identity strings.
The fixtures' `ProductName=XPDriver test fixture` is accepted only under
`--root`; they are never loaded, staged on the CD or substituted for a real
driver. `test/qd3d_hold.c` holds files like a running program (`load`:
mapped DLL; `open`: no `FILE_SHARE_DELETE`), and `test/wine_ddraw.c` / `.rc`
provides a DLL whose version resource reads like Wine's. It covers:
source validation (stock, swapped and incomplete sources); refusal to save an
unknown or WineD3D System32 DLL as an original; install, re-install and
update of all three DLLs with their WFP caches; `d3d9.dll` and `ddraw.dll`
mapped, and `d3d9.dll`, `d3d8.dll` and `ddraw.dll` held open, during update,
install and uninstall (applying the pending operations as the session
manager does); WFP refusing for install and uninstall; conflicts; recovery
of missing originals from the caches; the upgrade of a
`d3d9.dll`/`d3d8.dll`-only install; exact uninstall; and the leftover scan.
The pre-existing host scenarios remain, extended with service and driver
state restoration, driver-only install/update, source/service conflicts,
stopped-service startup, busy and locked-image updates, pending service
deletion, preserving unrelated reboot records on rollback, and failure
sweeps through each new transaction stage. Coexistence cases cover every
accepted ImagePath spelling and foreign-path refusals, shared-driver
retention/removal as the OpenGL key appears/disappears, and older/equal/newer
FILEVERSION ordering with service creation, startup and policy normalization.
The reboot simulation models SYSTEM_START **before** file renames and
AUTO_START **after**, checking the image that would actually be loaded.

The script copies the supplied executable to a temporary **`qd3d_ctl.exe`**
before any invocation, never executing an install/setup/update/patch-named
image on the development host. The production name stays
`qd3d_install.exe`. Its `asInvoker` manifest and the test's
`__COMPAT_LAYER=RunAsInvoker` setting are additional safeguards. The safe-name
runner is removed with the test directory after success (unless `-Keep`).

## Not verified until the run inside Windows XP

Host tests exercise file transactions and a fake SCM, not kernel loading,
real service-control behavior or actual XP boot ordering. In the XP VM, check:

- `status` shows `WFP: protected` for all three DLLs, and `install` reports
  `WFP exception for C:\WINDOWS\system32\ddraw.dll` (and `d3d9.dll`,
  `d3d8.dll`) without "failed", no WFP dialog appears, and the System event
  log has no Windows File Protection event 64001/64002 for them.
- After a restart, all three System32 DLLs are still QD3D (`status`);
  Direct3D 9/8 and DirectDraw/Direct3D 7 games use QD3D; with no QD3D channel
  they run on `qd3d9_orig.dll`, `qd3d8_orig.dll` and `qddraw_orig.dll`.
- Windows components that load `ddraw.dll` still work after the install, with
  and without a QD3D channel (media playback, DxDiag's DirectDraw test).
- The cache paths `status` lists match the VM (`SFCDllCacheDir`,
  `ServicePackSourcePath`; whether `ServicePackFiles\i386` exists and holds
  `ddraw.dll`).
- Whether `Driver Cache\i386\driver.cab` or `sp3.cab` contains `d3d8.dll`,
  `d3d9.dll` or `ddraw.dll` (`expand -D`); if so, WFP could restore stock
  silently from there when triggered.
- Optional: `sfc /scannow` should ask for the CD for these three files, and
  cancelling should keep QD3D.
- The in-use paths with a running game (mapped: immediate; restart path:
  replaced after the restart), `uninstall` back to the stock DLLs, and the
  exit codes and console output under `cmd.exe`.
- The executable starts on XP SP3 with its manifest (`trustInfo`, asm.v3).
- `driver` installs a running `SERVICE_KERNEL_DRIVER` named `qxpmap` with
  SYSTEM_START and the expected `.sys` version, without replacing DLLs.
- An open QXPMAP client prevents stopping safely; update returns 3, reports
  temporary AUTO_START, and the first reboot actually loads the replacement.
  A subsequent successful `driver` restores SYSTEM_START.
- With the XPDM OpenGL ICD registered, QD3D uninstall keeps the service/image
  and OpenGL still works. Without that registration, uninstall releases
  the service/image immediately when idle, or reports deletion/reboot pending
  while clients or SCM handles retain it.
- Installing the XPDM package and QD3D tools in either order preserves a higher
  installed qxpmap FILEVERSION and leaves both graphics paths usable.
