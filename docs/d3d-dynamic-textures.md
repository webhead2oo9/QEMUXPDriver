# Direct3D dynamic texture contract

This document defines XPDriver's dynamic-texture behavior through the stock
Windows XP Direct3D 8 runtime and the legacy Direct3D 9 path. It covers
capability discovery, video-memory locking, discard updates, upload generation,
and Reset ownership. It does not change the guest/host transport ABI.

## Capability and resource identity

XPDriver reports `D3DCAPS2_DYNAMICTEXTURES` in its D3D8 and legacy D3D9 caps
and `DDCAPS2_DYNAMICTEXTURES` in the DirectDraw HAL caps. The stock runtime may
therefore create a `D3DUSAGE_DYNAMIC`, `D3DPOOL_DEFAULT` texture. At the DDI
boundary that resource carries `DDSCAPS2_HINTDYNAMIC` and remains an ordinary
owner-scoped surface identity.

The driver does not advertise `D3DCAPS2_CANMANAGERESOURCE`; managed-resource
eviction remains the runtime's responsibility. The DDI-only
`D3DRENDERSTATE_EVICTMANAGEDTEXTURES` notification is consequently a recognized
no-op. `D3DRENDERSTATE_SCENECAPTURE` is also consumed as an informational
begin/end marker because XPDriver submits drawing eagerly.

## Lock, discard, and upload behavior

Dynamic default-pool textures use the existing linear guest video-memory
mapping. `QxpDdLock` returns the full-surface or validated subrectangle address
and records the DirectDraw lock flags. A successful writable `QxpDdUnlock`
increments the surface generation and invalidates the uploaded generation for
every member that shares the texture chain. A read-only unlock does neither.

`D3DLOCK_DISCARD` arrives as `DDLOCK_DISCARDCONTENTS`. XPDriver does not need to
preserve the prior contents and treats the completed write as a new generation.
The next sampled draw uploads that generation before rendering. The driver does
not currently rename storage on discard; correctness is guaranteed, while any
performance advantage beyond the direct linear mapping remains unknown.

Mip and cube members retain the established chain-wide invalidation and complete
face/level validation. Volume textures remain unadvertised.

## Reset and lifetime rules

- Dynamic textures remain default-pool resources and must be released before a
  stock-runtime Reset.
- Surface replacement through `CreateSurfaceEx` refreshes owner-scoped pointers
  without changing the resource handle contract.
- DestroySurface, DestroyDDLocal, context teardown, and process teardown retain
  the existing texture-name deletion and generation cleanup paths.
- Transport remains version 43. No request, packet, qemu-libretro, or host-core
  change is required.

## Deterministic coverage

The D3D8 and legacy D3D9 probes both require the dynamic-texture capability and
successful `CheckDeviceFormat` discovery for A8R8G8B8. Each creates a 16x16
dynamic default-pool texture, locks it with `D3DLOCK_DISCARD`, fills it with
`C02040`, and draws an exact sample. It then locks the same resource with
`D3DLOCK_DISCARD`, fills it with `20C080`, and draws a second exact sample.

The two colors occupy separate framebuffer locations. Requiring both exact
pixels proves the first upload was consumed before the second writable unlock,
the generation changed, the second upload replaced the sampled contents, and
the earlier framebuffer result remained intact. The D3D9 probe releases the
default-pool texture before its existing Reset gate.

Broad signed-install, D3D8, D3D9, native OpenGL, Reset, context, succession,
and applicable game validation remain mandatory because adding a capability can
change stock-runtime resource policy even for applications that do not create a
dynamic texture explicitly.

## XPDriver 1.0.0.227 validation

The signed `.227` package passed the portable T0 suites, authentic Windows XP
WDK build, PE/import checks, ChkINF 0/0, ICD dispatch coverage 336/336,
Inf2Cat, and catalog signing. A fresh offline stock-XP child then passed:

- exact D3D8 `C02040` and `20C080` dynamic-texture samples within the complete
  existing visual, HWTL, D24X8, state-block, Reset, and teardown gate;
- exact legacy-D3D9 `C02040` and `20C080` samples, default-pool release, Reset,
  `123456` post-reset output, and teardown;
- native OpenGL, focused VS1.1, simultaneous contexts, and two-process
  succession; and
- applicable Max Payne and UT2003 stock-D3D8 observations on a separate fresh
  games child, with zero defined driver-failure signatures and clean teardown.

Max Payne retained the established exact frame hash. UT2003 retained its known
pre-join presentation limitation: the DM-Antalus flyby alternates complete and
large-black-region phases while the HUD remains, and automated fire attempts do
not prove match entry. No dynamic-texture performance claim is made; measured
game-run counters are transport observations, not FPS.
