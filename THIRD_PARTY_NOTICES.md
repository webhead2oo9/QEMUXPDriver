# Third-party notices

XPDriver is licensed under the GNU General Public License version 2
(`LICENSE`). It includes or derives from the following components, whose
licences are compatible with GPLv2. Their notices are reproduced or
referenced here as their licences require.

| Component | Used in | Licence | Notice |
|---|---|---|---|
| apitrace API specifications (commit `c3829c8fc94332f46f7616d9c9ee995b63d9bfc1`) | `qd3d/third_party/apitrace/specs/`, and the code `qd3d/gen/qd3d_gen.py` generates from them | MIT | `qd3d/third_party/apitrace/LICENSE` |
| qemu-3dfx MESA GL wrapper | `guest/qxpicd/` (derived ICD) | GPL-2.0 | see `docs/toolchain-notes.md` |
| ReactOS OpenGL 1.1 function table (`dll/opengl/opengl32/glfuncs.h`, commit `c296dcfa29e7f32b623d8ed3e70604a06472fda2`; the file has no header, its directory's files say "See COPYING in the top level directory") | `guest/qxpicd/third_party/reactos/` (unmodified), compiled into `guest/qxpicd/` (dispatch table, no-op stubs) and read by `generate_scalar_stubs.py` | GPL-2.0 | `guest/qxpicd/third_party/reactos/COPYING` |
| Wine Direct3D 8/9 and DirectDraw API tests (`dlls/{d3d8,d3d9}/tests`, `dlls/ddraw/tests/{d3d,ddraw1,ddraw2,ddraw4,ddraw7,ddrawmodes,dsurface,refcount,visual}.c`), `include/wine/{test,debug}.h`, `dlls/winecrt0/debug.c` (tag `wine-11.0`, commit `db11d0fe6a169c457e23d007e20404643d067aa8`) | `qd3d/third_party/wine/` (unmodified; build-time patches in `qd3d/conformance/patches/`) and Wine-derived `qd3d/conformance/include/d3d9on12.h`, built by `qd3d/conformance/` as host test programs only, not shipped in guest DLLs | LGPL-2.1-or-later | `qd3d/third_party/wine/COPYING.LIB`, `qd3d/third_party/wine/README.md` |
