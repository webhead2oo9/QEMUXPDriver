# WineD3D offscreen-surface coverage vs. the host (Phase 3 cheap-path question)

**Question:** *"pbuffer/FBO surface coverage needed by WineD3D if Phase 3 goes the
cheap route — current host side exposes `hPbuffer[16]`; enough?"*

**Paper research only** (2026-07-10). Host tree = qemu-libretro at `<core checkout>`
(hw/mesa, working tree). VBox refs = sparse clone at `refs/virtualbox`.

**TL;DR:** WineD3D defaults to **ORM_FBO** — offscreen render targets are **FBOs**, which
pass through the host as ordinary GL calls with **no host-side handle cap**. `hPbuffer[16]`
is only touched by `WGL_ARB_pbuffer`/render-to-texture, which the FBO path does not use.
So the pbuffer array is **not the constraint** — it is very nearly moot. The cheap path is
plausible; the real (small) risks are the 6-slot context stack and flat `wglShareLists`,
both of which the already-working per-game wine9x flow exercises today.

---

## 1. What VBox-WineD3D actually uses for offscreen rendering

- WineD3D's offscreen mode is `wined3d_settings.offscreen_rendering_mode`, values
  `ORM_BACKBUFFER=0` / `ORM_FBO=1` (VBox's WineD3D-derived shaderlib:
  `refs/virtualbox/src/VBox/Devices/Graphics/shaderlib/wined3d_private.h:293-294,318`).
- **Default is `ORM_FBO`.** Confirmed in the same-era WineD3D `wined3d_main.c`
  (`process_attach`, default `ORM_FBO`; registry `Direct3D\OffscreenRenderingMode` only
  downgrades to `"backbuffer"`). The old `ORM_PBUFFER` mode was dropped from WineD3D well
  before the VBox fork; later WineD3D `context.c` has **no `pbuffer` code at all**.
- In ORM_FBO, every D3D render target / depth-stencil that is not the window backbuffer is
  a **GL FBO** (`glGenFramebuffers` + `glFramebufferTexture2D`/renderbuffer). Multiple
  render targets (MRT) are FBO-only (VBox shaderlib `directx.c:2599-2603`: `limits.buffers=1`
  unless ORM_FBO). FBO count is **per-texture / per-render-target** and unbounded — it is
  not a fixed pool.
- GL **contexts**, by contrast, are bounded and few: WineD3D creates roughly one GL context
  per swapchain (bound to the window HDC, or a hidden window) plus extra contexts for extra
  threads. A typical single-swapchain, single-thread d3d8/d3d9 game ⇒ **1 GL context**;
  a handful for multi-threaded / multi-swapchain apps. Offscreen surfaces do **not** each
  get a context in FBO mode.
- VBox shipped this as `VBoxD3D9.dll` importing `wined3d.dll`, tunnelling GL to the host
  (Chromium/HGCM) — the same "D3D→WineD3D→GL, GL goes to host" shape this project's guest
  ICD + system-wide WineD3D would take.

## 2. What the host exposes today (hw/mesa)

WGL extension string handed to the guest (`hw/mesa/mglcntx_mingw.c:583-592`):
`WGL_ARB_create_context(_profile)`, `WGL_ARB_extensions_string`, **`WGL_ARB_multisample`**,
**`WGL_ARB_pixel_format`**, **`WGL_ARB_pbuffer`**, **`WGL_ARB_render_texture`**,
`WGL_EXT_swap_control`, `WGL_3DFX_gamma_control`. **Not present:**
`WGL_ARB_make_current_read` (`wglMakeContextCurrentARB`) and any pbuffer-float/multisample-pbuffer ext.

Host offscreen-surface pools (all file-scope statics, one set per VM —
`hw/mesa/mglcntx_mingw.c:144-147`, sizes in `hw/mesa/mglfuncs.h:34-36`):

| Pool | Size | Purpose | Overflow behavior |
|---|---|---|---|
| `hRC[MAX_LVLCNTX]` | **6** (`MESAGL_MAGIC&0xF +1`) | nested/stacked GL contexts | recycles: deletes `hRC[1]`, shifts down (`mglcntx_mingw.c:619-624`) |
| `hPbuffer/hPBDC/hPBRC[MAX_PBUFFER]` | **16** | WGL pbuffers + their DC/RC | `wglCreatePbufferARB` returns **0/fail** at 17th (`mglcntx_mingw.c:713-718`) |

- **FBOs are not in any of these tables.** `glGenFramebuffers(EXT)`,
  `glBindFramebuffer(EXT)`, `glFramebufferTexture2D(EXT)`, `glBlitFramebuffer(EXT)`,
  `glGenRenderbuffers(EXT)`, `glDelete*` are plain FEnum dispatch entries
  (`hw/mesa/mglfptbl.h:72-695`; dispatched `hw/mesa/mesagl_impl.c:458-489`), resolved on
  demand via `InitMesaGLExt` → `MesaGLGetProc` (`wglGetProcAddress`,
  `mesagl_impl.c:1790-1804`). FBO *names* are GL-side handles owned by the host driver and
  stored in guest GL objects — **the host keeps no FBO table and imposes no FBO count cap.**
- Pbuffer slot is `handle & (MAX_PBUFFER-1)` — the guest handle's low 4 bits index the pool
  (`mglcntx_mingw.c:329,352,534,678,713-722`), so the 16 cap is baked into the addressing.
- MSAA: `WGL_ARB_multisample` advertised; window pixel format picks samples via
  `iattribs_fb(GetContextMSAA())` and `WGL_SAMPLE_BUFFERS/SAMPLES_ARB`
  (`mglcntx_mingw.c:391-398,450-461`). `GetContextMSAA()` comes from the **host `ContextMSAA`
  config toggle** (`mesagl_impl.c:1649-1650,1705`), not from the guest's per-format request.
- sRGB: `WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB` queried on the window format, gated by
  `ContextUseSRGB()`/`wrContextSRGB()` (`mglcntx_mingw.c:455-459,356`; `mesagl_impl.c:178,1679`).
- FIFO/data budget per (single) client is large: `MAX_FIFO=0xc0000`, `MGLSHM_SIZE≈64 MB`,
  `MGLFBT_SIZE=32 MB` (`mglfuncs.h:29-33`). FBO-heavy command streams are many tiny GL
  calls; the per-flush data window is ~16M dwords — not a practical limit.

## 3. Gap analysis — is `hPbuffer[16]` a real constraint?

**No — the FBO path makes it very nearly moot.**

- WineD3D in its default ORM_FBO mode allocates offscreen render targets as **FBOs**, not
  pbuffers. Pbuffers (`WGL_ARB_pbuffer` + `WGL_ARB_render_texture`) are the old ORM_PBUFFER
  render-to-texture mechanism, which WineD3D removed; the ORM_FBO path calls
  `wglCreatePbufferARB` **zero times** for render targets.
- FBOs go through the host as ordinary GL with **no host-side count limit** (§2). A d3d9
  game with dozens of simultaneous render targets / shadow maps / post-process buffers ⇒
  dozens of FBOs ⇒ fine.
- The `hPbuffer[16]` pool is exercised only if some guest component still uses WGL pbuffer
  RTT. For WineD3D that is essentially never. 16 concurrent pbuffers would already be a lot
  even for code that does use them.
- **Corroborating evidence already on this stack:** the per-game **wine9x** flow (a Wine
  D3D→GL layer) already renders correctly against *this exact host* — see README lines
  98-99. That is a live existence proof that
  WineD3D-class offscreen rendering fits inside `hPbuffer[16]`+FBO passthrough today.

**Verdict for the specific question: `hPbuffer[16]` is not the bottleneck.** If anything is
low, it is not the pbuffer count.

## 4. Other host-surface gaps WineD3D could hit (ranked by real risk)

1. **`hRC[MAX_LVLCNTX]=6` context ceiling (low-moderate risk).** WineD3D makes ~1 context
   per swapchain/thread. Single-swapchain games are fine. A heavily multi-threaded D3D app
   or one with many swapchains that needs >6 live GL contexts would trip the recycler
   (`mglcntx_mingw.c:619-624`), which *deletes* `hRC[1]` — pulling GL objects out from a
   still-live WineD3D context. wine9x works today ⇒ common cases stay ≤6.
2. **Flat `wglShareLists(A,B)` between two window contexts is unsupported (moderate risk,
   the sharpest edge).** The host handler only recognizes `wglShareLists(hRC[0], hPBRC[i])`
   — main context ↔ a **pbuffer** RC (`mglcntx_mingw.c:532-542`); anything else logs
   *"unknown contexts"* and returns 0. WineD3D shares GL objects across all its contexts.
   It gets that sharing **for free** only if it creates secondary contexts via
   `wglCreateContextAttribsARB`, which the host auto-shares off `hRC[i-1]`
   (`mglcntx_mingw.c:625`). If WineD3D instead does `wglCreateContext` + flat `wglShareLists`
   between two *window* contexts, sharing silently fails. Which path WineD3D takes depends on
   its version/context-creation code — **only testing settles this.**
3. **`WGL_ARB_make_current_read` absent (low risk).** No `wglMakeContextCurrentARB`. WineD3D
   normally uses plain `wglMakeCurrent`; read/draw separation is not required. Unlikely to
   bite.
4. **MSAA (low risk).** D3D multisampled render targets in ORM_FBO are multisampled FBO
   renderbuffers (`GL_EXT_framebuffer_multisample`) — plain GL passthrough, independent of
   the window pixel format. The window's own MSAA is a host config toggle (§2); it does not
   gate D3D MSAA content.
5. **sRGB (low risk).** D3D sRGB surfaces/textures use `GL_EXT_texture_sRGB` /
   `GL_ARB_framebuffer_sRGB` on FBO attachments — plain GL passthrough. Window-level sRGB
   only affects the final present.
6. **Single output surface / exclusive owner (by design, not a gap).** The whole Phase-0
   design refuses all but one GL owner (`docs/refusal-semantics.md`). So at most **one
   WineD3D process** is ever active against the passthrough; its surface counts are the
   per-process numbers above, never summed across the desktop.

## 5. What system-wide XP-era WineD3D exercises that per-game wine9x does not

The per-game wine9x flow already proves the *translation and surface model* work here. A
system-wide WineD3D differs **not** in per-process surface usage (identical: same WineD3D,
same FBO/context pattern) but in **who else is on the machine**:

- **More concurrent D3D/GL clients** — dxdiag at login, the logon screensaver, installer
  splash screens, a second game — all now hit the *same* system32 `d3d9.dll`. The exclusive-
  owner host (Phase 0) must refuse the non-owners cleanly; that is a *session-arbitration*
  problem, **not** a surface-count problem. It does not change how many FBOs/pbuffers/contexts
  the one active game allocates.
- **SMP desktop** — a general XP desktop is multi-vCPU; today that hard-stops the VM
  (`fx_3d_activation_allowed`, Phase-0 item 2c). Orthogonal to surfaces.
- **Newer D3D feature surface** — system-wide WineD3D may be a newer build than the wine9x
  snapshot, so it could probe GL extensions/context-creation paths wine9x never used (the
  `wglCreateContextAttribsARB` vs flat-`wglShareLists` choice in §4.2 is the one that
  actually touches host surface code). This is the only surface-relevant delta, and it is
  small.

None of these are pbuffer-count issues.

## 6. Verdict

**The cheap Phase-3 path (system-wide WineD3D in system32, no host surface work) is
plausible.** `hPbuffer[16]` is not a real constraint: WineD3D's default ORM_FBO renders
offscreen into FBOs, which the host passes through uncapped, and the pbuffer pool is left
essentially unused. The load-bearing evidence is that per-game wine9x — a Wine D3D→GL layer —
already renders correctly on this exact host/pbuffer/FBO plumbing.

**Confidence: moderate-high on paper.** The numbers line up (needs: 0 pbuffers, ~1-few
contexts, N FBOs uncapped; host has: 16 pbuffers, 6 contexts, unbounded FBOs) and a
WineD3D-class layer already works here.

**What only testing settles:**
- Whether the specific WineD3D build shipped uses `wglCreateContextAttribsARB` (host
  auto-shares) vs. flat `wglCreateContext`+`wglShareLists` between window contexts (host
  returns 0) — §4.2, the one plausible hard failure.
- Whether any real d3d9 title needs >6 simultaneous GL contexts (§4.1).
- That D3D MSAA/sRGB render targets actually come through as FBO-attachment GL and not via
  an unadvertised WGL path (expected fine, §4.4-4.5).

If a gap does appear, the cheapest mitigations are host-side and localized: raise
`MAX_PBUFFER`/`MAX_LVLCNTX` (both are just array sizes + a mask), and broaden the
`wglShareLists` handler to share arbitrary `hRC[]`/pbuffer contexts — none of which touch
the guest ABI. No multi-surface or transport rework is implied by Phase 3.

---

### Source anchors
- Host pools/caps: `hw/mesa/mglfuncs.h:18,20,34-36`; `hw/mesa/mglcntx_mingw.c:144-147`.
- Pbuffer create/cap/fail: `hw/mesa/mglcntx_mingw.c:706-724` (cap at :713-718).
- WGL ext string to guest: `hw/mesa/mglcntx_mingw.c:583-592`.
- `wglShareLists` limited handler: `hw/mesa/mglcntx_mingw.c:532-542`.
- `wglCreateContextAttribsARB` auto-share + 6-slot recycle: `hw/mesa/mglcntx_mingw.c:599-627`.
- FBO funcs are plain dispatch: `hw/mesa/mglfptbl.h:72-695`; `hw/mesa/mesagl_impl.c:458-489`;
  resolve `mesagl_impl.c:1790-1804`.
- MSAA/sRGB pixel-format handling: `hw/mesa/mglcntx_mingw.c:385-465`; `mesagl_impl.c:178,1649-1705`.
- WineD3D ORM: `refs/virtualbox/.../shaderlib/wined3d_private.h:293-294,318`;
  `.../shaderlib/directx.c:530,2582-2603`; default ORM_FBO per same-era WineD3D
  `wined3d_main.c` (`process_attach`).
- wine9x already works on this stack: `README.md:98-99`.
