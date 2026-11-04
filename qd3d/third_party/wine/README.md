# Wine Direct3D 8/9 and DirectDraw API tests

Wine's conformance tests for Direct3D 8 and 9 and for DirectDraw with
Direct3D 1-7, used by `qd3d/conformance` to compare QD3D with the native
runtime (see "Using them" below).

## Source

- Project: Wine, <https://gitlab.winehq.org/wine/wine>
- Tag: `wine-11.0` (commit `db11d0fe6a169c457e23d007e20404643d067aa8`), the
  newest stable release tag in that repository on 2026-11-02
- Fetched 2026-11-02 from `https://gitlab.winehq.org/wine/wine/-/raw/wine-11.0/<path>`
- Licence: GNU Lesser General Public License 2.1 or later
  (LGPL-2.1-or-later); the licence text is `COPYING.LIB` from the same tag,
  and each file carries its own copyright notice.

Every file is unmodified; its git blob id matches the tag's tree:

| Path | Git blob | SHA-256 |
|---|---|---|
| `include/wine/test.h` | `0bc361ebf0ceec73e327c609cdcbac8842b64190` | `2a8b0cb6dcf26a4affc21ffa6423e2ae79f5c2b2fa15a0e016d5b73d663bb81d` |
| `include/wine/debug.h` | `8328524ddb9dc729bb8e1f8e9209470a8cbc9dea` | `6b511fb8c5cd6374fef611afc1ed42839fb303c946e05f9855a2e5f9e30bb36a` |
| `dlls/winecrt0/debug.c` | `a7bf864aee52cb38777b77b75b61bbe895a43a1c` | `0066aa351d4f60875a712a0ef82ec9b9ad0c8b3c6bfa07b2510f68d6f2fc675e` |
| `dlls/d3d9/tests/device.c` | `b782833c37d4a425efcad6f41a25a1520f8e70d3` | `6e440a45cd98923f3e4dab242ec5f63e7c84af341a19673d4a97f907919f7c66` |
| `dlls/d3d9/tests/stateblock.c` | `c238e5b9adc21d6b75c6923bd76798099855b9c5` | `aabd39dc88904bf5dd308c80f8bb93cf091ff157e4345b7231a6594962382661` |
| `dlls/d3d9/tests/visual.c` | `ade80fbe9a1de8e48f21a7d2bba6c24621bd843c` | `f695fb0b646b65a456f808656efa9944471636ef444972c161b4ae9d27797f4a` |
| `dlls/d3d9/tests/utils.h` | `248e07eb52150d8263bf5082921344509e9735d8` | `eb35c760222fb2ee29f4e04db3477db62e6eb6d86fdcd6eb6bf3745e7908fa78` |
| `dlls/d3d8/tests/device.c` | `7059e82b128454c25886deeaf1ea9fc26a6563a3` | `5b76a8c5d3f20fb69e460bb8cb550c1dd4e7793961fe3dd0cf7cfe072d74a5b3` |
| `dlls/d3d8/tests/stateblock.c` | `c99252edbcd2a16800bcebc3620d821bb24ba3f5` | `189fa424dce05e6f55fcaf720cf2bcc44be4e2fc84d30a3d677e74e1210422bb` |
| `dlls/d3d8/tests/visual.c` | `f1e648a9e0943a5cb6ceb689a11df3e987216d0c` | `a69e3a6492fe1ae1873cbac71f562b71f322a5b231be7f296bfa315651b44f66` |
| `dlls/ddraw/tests/d3d.c` | `ec536a837290ab8b53cb29098ab0e0492d5ba3f8` | `3c435d4a3b61b582857398596280bd0633e4b05dc2a47fa26b7edb185785e614` |
| `dlls/ddraw/tests/ddraw1.c` | `ab7ddf3be15b1583e67544172eb7c7b73e83a0ff` | `9f4b42ddc4ce3f4b95c7f2b6eb5ce70c33129bc79ef36e0fec79e449bce21555` |
| `dlls/ddraw/tests/ddraw2.c` | `36b7c8eeea8953d4e182037289b9186c23c646f6` | `ca67bab15bea5a3c2d09f62a702f165803d13d011e47415c4fe2ff2692b0d090` |
| `dlls/ddraw/tests/ddraw4.c` | `1e9a964d86d94d476bdc39f06bcfc45fab019521` | `38afd052ba2d064e844fcd63212db7ca4be51c78183b72fd09882700f2a2f5d0` |
| `dlls/ddraw/tests/ddraw7.c` | `7a3c96fa315117f2dcd300ec1c78ff355bccb570` | `fb358fc6f164da1ad546710a057d9fb3f53b2def9fba043f6cf5b776cd07f31a` |
| `dlls/ddraw/tests/ddrawmodes.c` | `7a6f1da229aebaaaa200d9746fc21e829ec0c465` | `72a17b29f54798977183d8e2cc18c1c985616ab92237cf45924a440a91f7615d` |
| `dlls/ddraw/tests/dsurface.c` | `e65160034f6eccd1aff525186c63efc893f5f0dd` | `d63b76d6b4685f806ff8d141d54c86e3a6e92e49ffdf8ef8d74d3f3400d6a8b6` |
| `dlls/ddraw/tests/refcount.c` | `b89058f94183b64005ce89356ea7cab34ae0de10` | `d17e719a7987444b9cce0275b596bd4d7ef672bb4cd5115b78b214ac7ebc2ec0` |
| `dlls/ddraw/tests/visual.c` | `f5c6e79f832b33588f76f026da6f15bff9431faf` | `76d24d3820bbccbf10a7866b7c0e2fb3411dd0250d5c336996d485b09aedaacf` |
| `COPYING.LIB` | `732811e4f1faf71870b7350f211b26e00d7bfbe9` | `e237fa56668030e928551ddd60f05df5fe957f75eab874bbd017e085ed722e7c` |

`dlls/d3d9/tests/d3d9ex.c` is not included: Windows XP has no Direct3D 9Ex.
`dlls/ddraw/tests` holds the nine test files of the tag (all of them; the
directory's `Makefile.in` is not needed). `include/wine/debug.h` is needed by
`test.h`; `dlls/winecrt0/debug.c` holds Wine's fallbacks for the
`__wine_dbg_*` functions that only Wine's ntdll exports
(`wine_dbgstr_rect()` and friends call them).

## Using them

`make -C qd3d/conformance` builds one standalone 32-bit program per test
file with mingw-w64 (`STANDALONE` mode of `test.h`, one-entry test list):
`d3d9_device.exe`, `d3d9_stateblock.exe`, `d3d9_visual.exe`, the same for
d3d8, and `ddraw_<file>.exe` for the nine DirectDraw files.
`qd3d/conformance/run.ps1` runs each against the stock runtime and through
QD3D and diffs the results.

The vendored files stay pristine. The build compiles a copy from
`qd3d/conformance/build/src` made of:

1. the vendored file with `qd3d/conformance/patches/<dll>-<file>.patch`
   applied when one exists. Every patched line keeps its line number, so
   test output (`device.c:1234: Test failed: ...`) points into the vendored
   file;
2. every plain `name();` call inside `START_TEST` (any indentation)
   rewritten to `QD3D_RUN(name);`, and every
   `run_for_each_device_type(name);` to
   `QD3D_RUN_CALL(name, run_for_each_device_type(name));`
   (`qd3d/conformance/qd3d_conformance.h`): each prints a marker before the
   test function and lets the runner skip functions
   (`QD3D_CONFORMANCE_AFTER`, `QD3D_CONFORMANCE_ONLY`) to resume a program
   after a crash. Calls that create or release objects the following tests
   share stay plain calls (`SETUP_<test>` in the Makefile:
   `init_function_pointers`, `ReleaseDirect3D`, `D3D1_releaseObjects`,
   `createdirectdraw`, `releasedirectdraw`, `flushdisplaymodes`,
   `ReleaseDirectDraw`, `releaseObjects`).

Other build-time adaptations to the mingw-w64 headers:
`qd3d/conformance/include/d3d9on12.h` (the part of Wine's
`include/d3d9on12.idl` that `d3d9/tests/device.c` uses, LGPL-2.1-or-later;
mingw-w64 has no such header), and `-DDECLSPEC_EXPORT= -DCONST_VTABLE`,
which Wine's own headers would provide. The DirectDraw tests also get:

- `-D__WINESRC__`, with which Wine builds all of its sources, tests
  included: `wine/debug.h` defines `debugstr_a()` only then;
- `-include windef.h -include winbase.h -include wincon.h`: `ddraw1.c`,
  `ddraw2.c` and `ddraw4.c` include `wine/test.h` first, which calls
  `GetConsoleMode()`; Wine's `winbase.h` declares it, mingw-w64's needs
  `wincon.h`;
- `qd3d/conformance/ddraw_guids.c`: the GUIDs of `<ddraw.h>`, because
  mingw-w64's `libdxguid` lacks `IID_IDirectDraw3`;
- `-Wl,-u,_DirectDrawCreate@12`: Wine links all DirectDraw test files into
  one program that imports `ddraw.dll`; `ddraw7.c` finds
  `DirectDrawCreateEx` with `GetModuleHandleA("ddraw.dll")`, so every
  program imports `DirectDrawCreate` and loads `ddraw.dll` at start.

### Excluded test functions

These test functions create fullscreen (`Windowed = FALSE`) devices, change
display modes, take DirectDraw's exclusive fullscreen cooperative level,
move or clip the host cursor, or take the foreground. They would change the
monitor of the machine running the tests, so the patches replace each call
with a `skip()` that names it (`Tests skipped: qd3d conformance: <function>
excluded (...)`). They run in neither mode on the host; running them inside
the VM is left for later.

Direct3D 9 and 8 (`device.c`):

| Function | d3d9 device.c | d3d8 device.c | Reason |
|---|---|---|---|
| `test_cursor` | yes | yes | fullscreen device; `SetCursorPos` |
| `test_cursor_pos` | yes | yes | `SetCursorPos` and `SetCursorPosition` move the host cursor; needs input focus |
| `test_reset_fullscreen` | yes | - | Reset to fullscreen |
| `test_reset` | yes | yes | `ChangeDisplaySettings`; fullscreen devices |
| `test_scissor_size` | yes | - | fullscreen devices |
| `test_wndproc` | yes | yes | `ChangeDisplaySettings`; fullscreen devices; `SetForegroundWindow` focus loss |
| `test_wndproc_windowed` | yes | yes | Reset to fullscreen |
| `test_window_style` | yes | yes | fullscreen devices; `SetForegroundWindow` |
| `test_mode_change` | yes | yes | `ChangeDisplaySettings`; fullscreen devices |
| `test_device_window_reset` | yes | yes | fullscreen device |
| `test_occlusion_query` | yes | - | fullscreen device (registry-mode size) |
| `test_lost_device` | yes | yes | fullscreen device; `SetForegroundWindow` |
| `test_swapchain_parameters` | yes | yes | `Windowed = FALSE` devices |
| `test_get_display_mode` | yes | yes | fullscreen devices |
| `test_multi_adapter` | yes | yes | `ChangeDisplaySettingsEx` detaches secondary monitors |
| `test_cursor_clipping` | yes | yes | `ClipCursor`; fullscreen devices |
| `test_window_position` | yes | yes | fullscreen devices; `SetForegroundWindow` |

`test_swapchain` runs, except its fullscreen part: from
`device_desc.width = registry_mode.dmPelsWidth;` (the `reset_device()` to
fullscreen) through the four `CreateAdditionalSwapChain` checks after it
(d3d9 `device.c` lines 1468-1487, d3d8 `device.c` lines 516-535) is wrapped
in `if (0) { ... }` after a `skip()` that names it. The two
`Windowed = FALSE` `CreateAdditionalSwapChain` calls before it stay: the
stock runtime returns `D3DERR_INVALIDCALL` and creates nothing, and through
QD3D in loopback the proxy never changes the display mode and the executor
creates only windowed swap chains.

`d3d9/tests/visual.c`: `test_multisample_get_front_buffer_data` (fullscreen
640x480 device). `stateblock.c` and `d3d8/tests/visual.c` exclude nothing.

DirectDraw (`ddraw1.c`, `ddraw2.c`, `ddraw4.c`, `ddraw7.c`; "exclusive" is
`SetCooperativeLevel(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)`):

| Function | ddraw1 | ddraw2 | ddraw4 | ddraw7 | Reason |
|---|---|---|---|---|---|
| `start_foreground_window_thread` | yes | yes | yes | yes | set-up thread whose window calls `SetForegroundWindow`; only excluded tests use it |
| `test_coop_level_create_device_window` | yes | yes | yes | yes | exclusive; `DDSCL_CREATEDEVICEWINDOW`, `DDSCL_SETFOCUSWINDOW` |
| `test_coop_level_d3d_state` | yes | yes | yes | yes | exclusive |
| `test_coop_level_threaded` | yes | yes | yes | yes | exclusive |
| `test_ck_complex` | yes | yes | yes | yes | exclusive |
| `test_wndproc` | yes | yes | yes | yes | exclusive |
| `test_window_style` | yes | yes | yes | yes | exclusive; `SetForegroundWindow`, `SetActiveWindow` |
| `test_redundant_mode_set` | yes | yes | yes | yes | `SetDisplayMode`; exclusive |
| `test_coop_level_mode_set` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`, `ChangeDisplaySettings(Ex)`; exclusive; `SetForegroundWindow` |
| `test_coop_level_mode_set_multi` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`, `ChangeDisplaySettingsEx`; exclusive |
| `test_coop_level_versions` | - | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`; exclusive; `DDSCL_SETFOCUSWINDOW` |
| `test_coop_level_activateapp` | yes | yes | yes | yes | exclusive; `SetForegroundWindow` |
| `test_primary_caps` | yes | yes | yes | yes | exclusive |
| `test_flip` | yes | yes | yes | yes | exclusive |
| `test_primary_palette` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`; exclusive |
| `test_surface_attachment` | yes | yes | yes | yes | exclusive |
| `test_create_surface_pitch` | yes | yes | yes | yes | exclusive |
| `test_palette_gdi` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`; exclusive |
| `test_palette_alpha` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`; exclusive |
| `test_lost_device` | yes | yes | yes | yes | exclusive; `SetForegroundWindow` |
| `test_cross_device_blt` | yes | yes | yes | yes | exclusive |
| `test_display_mode_surface_pixel_format` | yes | yes | yes | yes | `SetDisplayMode`; exclusive |
| `test_killfocus` | yes | yes | yes | yes | exclusive; `SetForegroundWindow` |
| `test_gdi_surface` | yes | yes | yes | yes | exclusive |
| `test_cursor_clipping` | yes | yes | yes | yes | `SetDisplayMode`, `RestoreDisplayMode`; exclusive; `ClipCursor` |
| `test_window_position` | yes | yes | yes | yes | exclusive; `SetForegroundWindow` |
| `test_get_display_mode` | - | yes | yes | yes | `SetDisplayMode`; exclusive |
| `test_vb_desc` | - | - | yes | yes | exclusive |
| `test_d3d_state_reset` | - | yes | yes | yes | `SetDisplayMode`; exclusive |

The other DirectDraw files:

- `d3d.c`: `BackBuffer3DAttachmentTest` (exclusive).
- `dsurface.c`: `BackBufferAttachmentFlipTest` (exclusive).
- `refcount.c`: `test_iface_refcnt` (exclusive on the desktop window;
  `DDSCL_SETFOCUSWINDOW`).
- `ddrawmodes.c`: `testdisplaymodes` (`SetDisplayMode` through every mode;
  interactive runs only), `testcooperativelevels_normal` and
  `testcooperativelevels_exclusive` (exclusive; `SetForegroundWindow`).
- `visual.c`: everything. `START_TEST` skips `createObjects` (exclusive,
  `SetDisplayMode` to 640x480) and returns, since every other test draws on
  its device; `D3D3_ViewportClearTest` and `DX1_BackBufferFlipTest`
  (`SetDisplayMode`; exclusive) are excluded as well.

The lists come from a scan of every function that `START_TEST` calls, the
helpers they call and the function-like macros they use, for
`CREATE_DEVICE_FULLSCREEN`, `Windowed` values other than `TRUE`,
`ChangeDisplaySettings*`, `SetDisplayMode`, `RestoreDisplayMode`,
`StartModeTest`, `EvaluateMode`, `DDSCL_EXCLUSIVE`, `DDSCL_FULLSCREEN`,
`DDSCL_ALLOWMODEX`, `DDSCL_SETFOCUSWINDOW`, `DDSCL_SETDEVICEWINDOW`,
`DDSCL_CREATEDEVICEWINDOW`, `SetCursorPos`, `SetCursorPosition`,
`ClipCursor`, `SetForegroundWindow`, `SetActiveWindow`, `SetFocus`,
`BringWindowToTop`, input injection, gamma ramps and system settings.
Every `CreateDevice`, `Reset` and `CreateAdditionalSwapChain` call outside
the excluded code uses windowed parameters (except the two in
`test_swapchain` above), and every `SetCooperativeLevel` call outside it
passes `DDSCL_NORMAL`, directly or as the `coop_level` argument of
`create_device()`. Reviewed and kept: `ddrawmodes.c`'s set-up
`createwindow()` calls `SetFocus` on its own hidden window, which cannot
take the foreground from another program; `test_clipper_blt` shows its own
window and `ddraw1.c`'s `test_get_display_mode_cb` moves its own window;
the tests that create a primary surface at `DDSCL_NORMAL` (for example
`test_surface_lock`, `test_pixel_format`, `test_caps`, `SurfaceCapsTest`)
can draw on the desktop for an instant but never change the mode.

### Other patched lines

`d3d8-visual.patch` replaces the six-line `TEB64`/`PEB64` probe in
`START_TEST(visual)` with `new_wow64 = FALSE;`. The probe detects Wine's own
new WoW64 mode, needs Wine's `winternl.h`, and only selects a `todo_wine`,
which is inert on Windows.

`d3d9-device.patch`, `d3d8-device.patch` and the `ddraw1`, `ddraw2`,
`ddraw4` and `ddraw7` patches read `registry_mode` with
`ENUM_CURRENT_SETTINGS` instead of `ENUM_REGISTRY_SETTINGS`. `START_TEST`
skips every test when the two differ, because the mode tests must restore
the registry mode; on the development host they differ (the registry has
held 1024x768 while the desktop ran 3440x1440 or a remote-streaming
client's mode). Only excluded code uses `registry_mode`, so nothing else
changes.

`ddraw-d3d.patch` runs the `d3d2_tests[]` loop's call as
`QD3D_RUN_CALL(test_get_caps2, ...)`, so that its single test function gets
its own marker.

## Updating

Fetch the same paths from a newer tag, compare the git blob ids, update the
table above, and regenerate the patches: edit a copy of each patched file
(replace lines, never add or remove them) and write
`diff -u --label a/<path> --label b/<path> <vendored> <copy>` to
`qd3d/conformance/patches/<dll>-<file>.patch`. The build applies patches
with `--fuzz=0`, so a patch that no longer fits fails the build. Repeat the
scan above on the new sources.
