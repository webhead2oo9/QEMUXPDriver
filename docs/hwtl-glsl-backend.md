# Scalable fixed-function HWTL shader backend

Status: production miniport integration and seven real-XP coverage increments
validated on 2026-07-15 through signed package `.201`.

## Decision

Use one bounded, vertex-only GLSL 1.10 program as the scalable backend for D3D8
fixed-function HWTL combinations that exceed the portable 128-instruction
ARBvp1.0 limit. Keep the existing exact ARB programs as proven fallbacks while
the GLSL selector grows. This is an internal implementation detail: it does not
advertise Direct3D programmable vertex-shader caps.

Adding a fifth complete Range-aware light to the existing four-light ARB
program cannot preserve its material sources and four texture transforms within
the 128-instruction portability floor. More increasingly narrow ARB variants
would multiply selector and cache state without solving the eight-light,
multi-spot, and multi-specular cases. A GLSL backend removes that static
instruction ceiling while continuing to consume the standard D3D fixed-function
state packet.

## Deterministic host proof

`mingw32-make -C guest/xpdm glsl-compat-smoke` creates a legacy compatibility
context on the actual host renderer, compiles and links a vertex-only GLSL 1.10
program, renders into a 640x480 drawable, and reads deterministic pixels. It
proves:

- GLSL primary color reaches the fixed fragment pipeline (red sample);
- GLSL texture coordinates reach fixed texture processing (green sample); and
- an enabled ARB fragment program consumes the GLSL primary color (blue sample);
- the production program applies independent Range across five lights; and
- its MATERIAL Power=0 and seven-light local-viewer specular paths reach the
  separate-color sum, with one output LSB of fixed-function raster latitude in
  nonzero expected channels; and
- two simultaneous spotlights produce the exact linear and squared midpoint
  Falloff factors while remaining isolated by Range; and
- programmable point size reads the standard compatibility point state and
  turns a 16-pixel value with constant attenuation four into an exact 8-pixel
  raster boundary while preserving top-left point-sprite texturing and an
  exact linear-fog mix.

The 2026-07-15 run passed on OpenGL 4.6.0, NVIDIA 610.47, RTX 4090. The test is
an explicit target rather than part of generic `make test`, because it requires
a real Windows OpenGL display context.

## Existing FIFO ABI map

No host ABI change is needed. The current Mesa FIFO already exposes every call
needed by the miniport:

| Operation | FIFO ordinal | Completion/data rule |
| --- | ---: | --- |
| `glAttachShader` | `0x019` | ordinary arguments |
| `glCompileShader` | `0x113` | ordinary arguments, then query status |
| `glCreateProgram` | `0x171` | object name from serialized global return register `0xFA4` |
| `glCreateShader` | `0x177` | object name from serialized global return register `0xFA4` |
| `glDeleteProgram` | `0x1A2` | ordinary arguments |
| `glDeleteShader` | `0x1AD` | ordinary arguments |
| `glGetProgramInfoLog` | `0x3CB` | shared output length at byte 0, text at byte 8 |
| `glGetProgramiv` | `0x3CD` | shared output count at byte 0, scalar at byte 8 |
| `glGetShaderInfoLog` | `0x3FC` | shared output length at byte 0, text at byte 8 |
| `glGetShaderiv` | `0x3FD` | shared output count at byte 0, scalar at byte 8 |
| `glGetUniformLocation` | `0x444` | string input plus result from `0xFA4` |
| `glLinkProgram` | `0x4F8` | ordinary arguments, then query status |
| `glShaderSource` | `0x82E` | one NUL-terminated string plus one aligned host-pointer slot |
| `glUniform1i` | `0x944` | ordinary arguments |
| `glUniform4fv` | `0x984` | FIFO data input |
| `glUseProgram` | `0x9BC` | ordinary arguments |

Result-bearing calls must remain inside the miniport's existing
`VideoPortSynchronizeExecution` critical section and read `0xFA4`. That global
register exists specifically because XP may resume the caller on another vCPU
after the synchronous MMIO store. `glGetUniformLocation` needs a combined FIFO
input/result helper; the existing FBTM input helper is not correct for its Mesa
`hshm` string argument.

Use exactly one source string. The host's multiple-string reconstruction has
legacy complexity that the driver does not need, while count one has a simple
bounded layout: aligned source including NUL, followed by one aligned pointer
slot which the host fills before calling OpenGL.

## Fail-closed lifecycle

The miniport must not select the backend unless all of these steps succeed:

1. create the vertex shader;
2. upload the bounded source;
3. compile and require `GL_COMPILE_STATUS`;
4. create, attach, and link the program and require `GL_LINK_STATUS`;
5. resolve every required uniform location; and
6. delete the intermediate shader object.

On any failure, print the bounded shader/program log, delete all objects already
created, leave the backend undefined, and reject only the unsupported draw. A
session close must first select program zero and delete the linked program;
session reopen/Reset starts with no cached object names or uniform locations.

Switching from an ARB vertex program must disable and unbind it before
`glUseProgram`. Switching back must call `glUseProgram(0)` before binding the
ARB program. The host smoke proves that either the fixed fragment path or the
existing ARBfp pixel-shader translator can remain downstream of the GLSL vertex
stage.

## First coverage increment

The first selector is deliberately standards-driven and bounded: five through
eight simultaneous D3D point lights, independent hard Range and A/B/C
attenuation, diffuse/ambient/emissive MATERIAL/COLOR1/COLOR2 sources, optional
normal normalization, and all four texture matrix/projected-q paths. It excludes
fog, specular, spotlights, point primitives, and user clip planes until their
equations have separate exact oracles.

The real-XP gate first recorded five isolated stock software-VP samples at `y=55`
in a previously verified background-only strip. Three more verified-empty bands
add separate rows of six, seven, and eight lights. Their center spacing is 100,
90, and 80 pixels: at a 640-pixel viewport the adjacent eye-space distances are
approximately 0.590, 0.574, and 0.559 after the common 0.5 eye-Z offset. A 0.52
Range keeps each triangle's own light active and every neighbor inactive. The
consecutive draws exercise live active-count and both packed Range-vector updates
without recompiling the program. All 26 scalable samples use nonwhite channel
endpoints so the previous fixed path cannot accidentally satisfy one.

Unchanged `.190` hardware passed the prior 105 samples and produced white for
all 26 scalable samples, a clean 105/131 negative with successful guest completion and
teardown. A fresh network-disabled `.191` SetupAPI install then passed file and
registry verification. Mandatory X8R8G8B8 and RGB565 hardware VP passed all 131
samples. The full X8 gate passed its 70 transformed samples, the 131-sample HWTL
frame, wireframe, fullscreen Reset, post-Reset HWTL, custom and predefined state
blocks, and clean teardown. The QXPD3D host profile records one compile, one link,
one shader/program status query, and eight uniform lookups; the driver records one
scalable-program definition with no FIFO/program/draw/transport error marker.

## Scalable specular increment

Signed `.196` extends the same program to three through eight simultaneous point
lights when specular is enabled. Each light keeps its own hard Range and A/B/C
attenuation attached to diffuse and half-vector specular terms. MATERIAL,
COLOR1, and COLOR2 specular sources retain the standard missing-FVF fallback;
local and infinite viewers, optional normal normalization, material Power, and
all four texture transforms remain independent. The selector still excludes
fog, spotlights, point primitives, and user clip planes from this scalable path.

Power zero is handled explicitly. GLSL leaves `pow(0, 0)` undefined, while the
Direct3D fixed-function equation requires a unit exponent result for a
front-facing vertex. Computing one before the positive-Power branch prevents
NaN from leaking through a zero Range mask. The fixed fragment pipeline also
requires `GL_COLOR_SUM` for GLSL secondary-color output; the shared state
predicate therefore names the scalable program even when the input FVF has no
COLOR2 field.

The real-XP probe presents a separate 35-sample frame after the mature
131-sample HWTL frame. Six isolated rows exercise active counts three through
eight, sparse logical light IDs, both Range vectors, MATERIAL/COLOR2 sources,
local/infinite viewers, and Power zero/eight. Power-zero channel endpoints and
the infinite-viewer black row remain byte-exact. The seven low-intensity
Power-eight samples allow one output LSB in nonzero expected channels because
stock XP software VP and the host fixed-function rasterizer can round the same
per-vertex result differently. Zero channels remain exact, and the complete
spatial half-vector profile is mandatory.

A fresh network-disabled `.196` SetupAPI install passed file and registry
verification. Stock software VP passed both frames. Mandatory hardware VP in
X8R8G8B8 and RGB565 passed the 131- and 35-sample frames, and the full X8 gate
also passed its transformed scene, wireframe, fullscreen Reset, post-Reset HWTL,
custom/predefined state blocks, guest completion, and clean teardown. Each
hardware log contains exactly one scalable-program definition and no FIFO,
program, draw-stage, packet, rejection, or transport error marker.

## Scalable spotlight increment

Signed `.197` extends the selector to mixed point/spot configurations with two
through eight active positional lights. Pure-point configurations continue to
use their already-proven bounded ARB paths when those paths cover the requested
state. A selected spotlight requires ordered `Theta <= Phi <= pi`; unsupported
state still fails closed instead of approximating the equation.

The program normalizes the spotlight direction, computes `rho` against the
light-to-vertex direction, returns zero at and outside the outer cone, returns
one inside the inner cone, and raises the normalized interval between them to
Direct3D `Falloff`. That factor multiplies the existing Range/A/B/C attenuation
before ambient, diffuse, and specular accumulation. Fixed OpenGL exposes only a
single spot exponent, so the driver transports the D3D-only quantities through
otherwise-unused light-color alpha while the scalable backend is active:
ambient alpha carries Falloff and diffuse alpha carries the inner half-angle in
degrees. Light RGB and the material/output alpha contract are unchanged, and
point lights remain distinguishable through OpenGL's `-1` spot cutoff.

The real-XP frame grows from 35 to 37 samples. It keeps all six active-count
rows and changes selected rows to spotlights wholly inside their inner cones,
then adds two simultaneous outer-cone spotlights at the same vertices: Falloff
one must produce `604020`, and Falloff two must produce `302010`. Stock XP
software VP and mandatory hardware VP in X8R8G8B8 and RGB565 pass the paired
131- and 37-sample frames. The full X8 gate also passes the transformed scene,
wireframe, fullscreen Reset, post-Reset HWTL, custom/predefined state blocks,
guest completion, and clean teardown. Each audited hardware log defines the
scalable program exactly once and has no GLSL, uniform, FIFO, VP, staging, packet,
rejection, or transport failure marker.

## Scalable fog increment

Signed `.198` removes fog from the scalable selector's conservative exclusions.
A tenth uniform selects planar absolute eye Z or radial eye distance for vertex
fog and selects the existing per-vertex fog-coordinate input for table fog. The
fixed fragment pipeline continues to own the standard fog mode, start/end,
density, and color state, so this remains a vertex-only extension with no new
guest/host ABI and no advertised capability change. The uniform array capacity
is shared between the device structure and all indexing constants so extending
the program cannot overwrite adjacent presentation state.

The real-XP scalable frame grows from 37 to 40 samples. Its final triplet keeps
three simultaneous spotlights enabled and deliberately runs after the established
low-intensity specular rows. The first sample adds exact Power-zero red specular
to Falloff-one diffuse, producing raw `804020`, then applies half linear vertex
fog over `204060` for exact `504040`. The second sample distinguishes XP's stock
software-VP planar result (`604020`) from hardware VP's required radial result
(`C040FF`). The third selects post-divide table fog and produces exact `6C381C`.

Stock XP software VP and mandatory hardware VP in X8R8G8B8 and RGB565 pass the
paired 131- and 40-sample frames. The full X8 gate also passes the transformed
scene, wireframe, fullscreen Reset, post-Reset HWTL, custom/predefined state
blocks, ignored-state handling, guest completion, and clean teardown. Each
hardware log defines the scalable program exactly once; the software log does not
instantiate it. All four logs contain no GLSL, uniform, FIFO, VP, staging, packet,
rejection, transport, or flip failure marker.

## Scalable user clip-plane increment

Signed `.199` removes enabled user clip planes from the scalable selector's
remaining exclusions. No shader or transport field is added: the program already
emits eye-space `gl_ClipVertex`, packet validation already bounds all six enable
bits and finite plane equations, and the shared object-matrix path defines D3D's
world-space planes while only the view matrix is current. The world transform is
restored afterward for vertex processing, so object translation cannot move a
world-space clipping boundary.

The scalable frame grows from 40 to 42 samples. A second two-spot pair applies a
`+0.5` world-X translation under the world-space plane `x >= 0`. The translated
left primitive must remain the exact `16233B` clear color; the translated right
primitive must retain the exact nonlinear Falloff-two result `302010`. Defining
the plane while world is current would move the boundary to `x = 0.5` and reject
the right sample, while falling back to fixed OpenGL spotlight exponent semantics
cannot produce `302010`. The pair therefore proves both plane space and scalable
program selection without a private diagnostic.

Stock XP software VP and mandatory hardware VP in X8R8G8B8 and RGB565 pass the
paired 131- and 42-sample frames. The full X8 lifecycle also passes wireframe,
fullscreen Reset, post-Reset HWTL, custom/predefined state blocks (including clip
plane membership), ignored-state handling, guest completion, and clean teardown.
Each hardware log defines the scalable program once, the software log defines
none, and all four logs contain no GLSL, uniform, FIFO, VP, staging, packet,
rejection, transport, or flip failure marker.

## Scalable point-primitive increment

Signed `.200` permits point lists in the scalable selector and replaces the
program's placeholder one-pixel output with the standard Direct3D scaled-point
equation. The shader reads GLSL 1.10 compatibility state `gl_Point`: current
size, minimum/maximum clamps, and constant/linear/quadratic distance
attenuation. The display driver and miniport already transport those values,
including the viewport-height factor required by D3D8. Existing
`D3DFVF_PSIZE` replay splits adjacent unequal sizes into bounded point runs;
each run's ordinary `glPointSize` update is therefore visible to the same
program without another vertex attribute, uniform, packet field, or ABI
version.

The scalable frame grows from 42 to 52 samples. Five simultaneous point lights
use independent hard Range: the two points lie inside only their matching red
or green light, while the other colored lights remain outside. Fixed OpenGL
ignores D3D Range and cannot produce the two pure centers, so this also proves
scalable-program selection. Per-vertex sizes 0.5 and 2.0 use a viewport-derived
constant attenuation and clamps of 10 and 20 pixels. Exact center, inner, and
outer samples reject a one-pixel shader result, a shared size, missing
attenuation, or missing min/max clamping. A third 20-pixel point retains all
five Range-isolated lights and adds the asymmetric reference texture through
top-left point-sprite coordinate replacement. Four quadrant samples require
the exact half-distance linear-fog blends, and an outer sample rejects a sprite
or clamp overrun. This directly closes the scalable point-sprite/fog
interaction instead of extrapolating it from the older isolated tests.

The host compatibility smoke independently requires a 16-pixel point with
constant attenuation four to cover its 8-pixel textured/fogged quadrants and
reject its outer sample. The bounded production source is 9,866 bytes, its
NUL-terminated payload is 9,867 bytes, and FIFO alignment plus the pointer slot
consumes 9,880 bytes. It still resolves the same ten uniforms and stays below
the audited internal 16 KiB ceiling.

A fresh network-disabled `.200` SetupAPI install passed signed-file and registry
verification. Stock software VP and mandatory hardware VP in X8R8G8B8 and
RGB565 passed the paired 131- and 52-sample frames. The full X8 lifecycle passed
the transformed scene, both HWTL frames, wireframe, fullscreen Reset,
post-Reset HWTL, ignored-state handling, custom/predefined state blocks, guest
completion, and clean teardown. Each hardware log defines the scalable program
once, the software log defines none, and all four audited logs contain no GLSL,
uniform, FIFO, VP, draw, packet, transport, or flip failure marker.

## Mixed directional/positional increment

Signed `.201` permits directional lights in a scalable set that also contains a
point or spot light. The generated GLSL uses the standard compatibility light
`position.w` to form either an invariant directional vector or an eye-to-light
positional vector. Distance, hard D3D Range, and A/B/C attenuation are evaluated
only for positional lights. All-directional sets stay on the mature fixed-GL
path, and the existing point/spot selection thresholds are unchanged. This is a
selector and shader-semantic extension only: it adds no cap, uniform, packet
field, or guest/host ABI.

The scalable frame grows from 52 to 54 samples. One red directional light
contributes ambient to two triangles. A green point light contributes diffuse
only to the left triangle inside its 0.6-unit Range, so the exact result is
yellow on the left and red on the right. Treating the directional light as
positional would attenuate or reject it; ignoring the point light's D3D Range
would make both triangles yellow. The host compatibility smoke independently
renders the same production-shader discriminator.

A fresh network-disabled `.201` SetupAPI install passed signed-file and registry
verification. Stock software VP and mandatory X8R8G8B8/RGB565 hardware VP passed
the paired 131- and 54-sample frames. The full X8 lifecycle passed the transformed
scene, both HWTL frames, wireframe, fullscreen Reset, post-Reset HWTL,
ignored-state handling, custom/predefined state blocks, guest completion, and
clean teardown. Each hardware log defines the scalable program once, the
software log defines none, and all audited logs contain no shader, uniform,
FIFO, VP, draw, packet, transport, or flip failure marker.

`extract-glsl-source.py` reconstructs the exact production C string for the host
smoke and rejects internal source-payload overflow. The public D3D/ARB source
contract remains limited to 8 KiB; only this audited internal fixed-function
source has a 16 KiB ceiling. The validated source is 9,953 bytes, its
NUL-terminated payload is 9,954 bytes, and alignment plus the count-one
host-pointer slot consumes 9,968 FIFO bytes. The host smoke compiles that
production shader, resolves all ten uniforms, and checks five-point Range,
three- and seven-light specular rendering, simultaneous spotlight Falloff,
vertex fog, translated-world clip rejection/retention, and exact scaled-point
sprite/fog raster boundaries, plus mixed directional/point Range semantics, in
addition to the fixed and ARB fragment bridges.
