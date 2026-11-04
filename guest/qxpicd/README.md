# QXPICD bring-up slice

This ICD implements the XP loader export surface, Mesa window mapping through
XPDriver's `qxpmap.sys` (`\\.\QXPMAP`, shared; see `docs/design/qxpmap.md`),
versioned `0xFB4` probe/reservation, guest-side pixel-format synthesis, and the
tokened `0xFE4` ownership bind. The first accelerated rendering slice is live:
host context create/make-current/delete, FIFO batching, renderer-string readback,
swap, viewport/clear, immediate-mode geometry, textures, scalar/vector state,
counted queries, and client arrays. Separate color/normal/texture-coordinate/
vertex arrays, indexed draws, and packed interleaved arrays now use the original
host vertex-cache protocol. Pixel draw/read and texture readback also use bounded
bulk mappings. All 336 OpenGL 1.1 table slots have non-no-op dispatch bodies; `make
coverage` enforces that floor. This is static table coverage, while the real-XP
suite exercises representative generated entries and every custom pointer/bulk path.

The exact implemented register, ownership, lease, refusal, threat-model, and SMP
pairing contract is documented in
[`../../docs/mesa-qxpicd-protocol.md`](../../docs/mesa-qxpicd-protocol.md).

MMIO write/read acknowledgement pairs temporarily pin the calling thread to one
available vCPU, then restore its prior affinity. Host replies are per-vCPU; without
this narrow guard XP can migrate between the two instructions and read another
vCPU's latch. Unlike kit v12, QXPICD does not pin the entire application process.
If the temporary thread pin cannot be established, the paired operation fails
before issuing its doorbell.

After a tokened bind, a small worker renews the host lease through `0xFB0` once
per second. A killed process stops renewing, allowing the host to reclaim its WGL
objects and stale ring after five seconds; an idle live process remains owner.

The current host exposes one ordinary window context. QXPICD now models that limit
honestly: one live `DHGLRC`, clean refusal of a second create, stale/invalid-handle
rejection, a real host WGL unbind from `DrvReleaseContext`, and rebind support.
Deleting the context releases the session, while a later create on the same
pixel-formatted DC reserves, binds, and creates a fresh session. The acceptance
test exercises that entire sequence before rendering.

`guest/acceptance/qxpwgltest.exe` also exercises QXPICD through Microsoft's stock
XP `opengl32.dll`, rather than loading `Drv*` exports directly. The loader performs
the real registry lookup, DLL/export validation, WGL context lifecycle, and dispatch
table calls; a narrow IAT shim supplies only the display driver's missing
`OPENGL_GETINFO` response. This exposed and fixed a state bug where a post-bind
`DrvDescribePixelFormat` query demoted `GRANTED` back to `ADVERTISING`. The test now
renders accelerated, verifies a concurrent process falls back to `GDI Generic`,
then confirms the incumbent remains accelerated.

`qxptest.exe` renders a moving checker-textured triangle for 180 frames, rotating
among immediate mode, separate arrays, indexed arrays, and `GL_T2F_C3F_V3F`
interleaved records. It also validates texture/state/query round trips and reports
a stage-coded UDP result to the host harness. Real XP SP3/WHPX/`-smp 2` currently
passes with the RTX 4090 renderer, ~32 FPS, and clean teardown.

`PixelMap*` setters are bounded to the shared aperture, and `GetPixelMap*` obtains
the actual native map size before a counted copy. `GetPixelMap*` and
`GetPolygonStipple` now use explicit host pointer signatures instead of forwarding
raw guest virtual addresses. Float/uint/ushort PixelMap round trips and the exact
128-byte polygon-stipple round trip pass on real XP.

Build in WSL with `make`; run `make coverage` for the reproducible dispatch audit.
The ICD compiles the paired core's Mesa pass-through tables
(`hw/mesa/mglfunci.h`, `hw/mesa/szgldata.c`): `QEMU_DIR` names that core
(`guest/paired-core.mk`: the `core/` submodule, else the `qemu-libretro-d3d9`
worktree beside this checkout; `make QEMU_DIR=...` overrides). ReactOS's
OpenGL 1.1 table is vendored in `third_party/reactos/`.
The output is `build/qxpicd.dll` and targets Windows XP SP3 with a Pentium 4/SSE2
ISA floor.
