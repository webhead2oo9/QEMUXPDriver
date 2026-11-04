# Direct3D 8 depth-surface contract

This document defines the depth formats XPDriver exposes through the stock
Windows XP Direct3D runtimes and how they map to the existing guest/miniport
target transport. It describes format discovery, attachment semantics,
lifetime, and deterministic validation. It does not add or change a guest/host
packet layout.

## Advertised formats

XPDriver reports the following operation formats with
`D3DFORMAT_OP_ZSTENCIL` and
`D3DFORMAT_OP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`:

| Direct3D format | Storage | Host renderbuffer | Attachments |
| --- | --- | --- | --- |
| `D3DFMT_D16` | 16-bit depth | `GL_DEPTH_COMPONENT16` | depth |
| `D3DFMT_D24X8` | 24-bit depth plus eight unused bits | `GL_DEPTH_COMPONENT24` | depth |
| `D3DFMT_D24S8` | 24-bit depth plus 8-bit stencil | `GL_DEPTH24_STENCIL8` | depth and stencil |

`D24X8` is a depth-only format. The unused eight bits are not exposed as
stencil, and binding it explicitly detaches the framebuffer's stencil
attachment. `D24S8` remains the format for applications that request stencil.

The display driver accepts the runtime's `DDPF_D3DFORMAT` representation and
the equivalent legacy depth/stencil bit fields. A 24- or 32-bit depth surface
without stencil maps to `D24X8`; the same depth with an eight-bit stencil field
maps to `D24S8`.

## Transport and lifetime

Transport version remains 43. The existing target request already carries a
depth surface name and Direct3D format, so `D24X8` requires no request-size,
packet, host-core, or qemu-libretro ABI change.

The miniport validates the format before allocating storage. A renderbuffer is
keyed by process, guest depth-surface name, dimensions, and format. Rebinding
the same object reuses it; a mismatch fails instead of silently aliasing
storage. Color-target framebuffer-completeness results are cached for the exact
color/depth pair. Surface deletion invalidates matching cache entries and
deletes the host renderbuffer. Reset and process teardown retain the same
ownership rules as the established D16/D24S8 path.

Switching between `D24X8` and `D24S8` always updates both depth and stencil
attachments before completeness is checked. This prevents a prior packed
depth/stencil target from leaving stencil storage attached while a depth-only
surface is active.

## Deterministic coverage

The stock-D3D8 probe requires both `CheckDeviceFormat` and
`CheckDepthStencilMatch` to accept `D24X8`. It then:

1. keeps the normal D24S8 presentation depth surface alive;
2. creates a matching D24X8 surface and binds it to the real backbuffer;
3. clears a bounded region to `0x203040` with depth 1.0;
4. draws a nearer red rectangle, then a farther green rectangle over it;
5. restores the original D24S8 surface before Present; and
6. requires exact red, green, and untouched-clear pixels in the captured frame.

This one oracle covers discovery, runtime creation, target transport, true
depth rejection, color output, presentation preservation, attachment switching,
surface release, Reset, and teardown. The broad T2 matrix additionally retains
D24S8 stencil coverage, legacy D3D9 RTT/depth, native OpenGL, VS1.1 ownership,
simultaneous contexts, and process succession.

## Current limits

XPDriver does not advertise `D24X4S4`, `D32`, multisampled depth surfaces, or
depth textures. Adding any of those requires its own honest storage mapping and
stock-runtime oracle; format enumeration alone is not sufficient.

Diagnostic package `.228` specifically tested `D32` by requesting
`GL_DEPTH_COMPONENT32` and querying the allocated renderbuffer's
`GL_RENDERBUFFER_DEPTH_SIZE`. The current host path reported 24 bits, so the
miniport rejected the target and the public capability was removed before a
release checkpoint. The stock D3D8 call sequence nevertheless returned success,
which confirms that API-level creation alone is not a sufficient storage oracle.
`GL_DEPTH_COMPONENT32F` is not used as a fallback because floating-point depth
does not provide the normalized 32-bit storage contract required by
`D3DFMT_D32`. The failed diagnostic and clean teardown evidence are preserved in
`artifacts/runs/20260716-228-d32-host-substitution-t1/`.
