# Guest wrapper notes: qemu-3dfx mesa fifo client (2026-07-10)

**Question (Step 1b):** how does the guest GL wrapper attach, how do host failures
propagate to the app, what does it cache, and how far is it from the ~16 `Drv*` entry
points a real XP ICD needs?

**Audited tree:** `refs/qemu-3dfx` @ `b47f30a396` ("Retain tick hooking for Win9x/ME
QPC", 2026-05-27). Main file `wrappers/mesa/src/wrapgl32.c` (17,601 lines: ~250 lines
fifo core, ~16k generated GL stubs, ~1k WGL/DllMain). Helper lib `wrappers/fxlib`.
Host cross-refs are to the qemu-libretro working tree at `<core checkout>`.

**Bottom line:** the wrapper fails **cleanly** exactly twice — DLL-load activation
(`0xFBC` readback) and `wglSetPixelFormat` (`0xFE4` readback). Everything downstream
of a successful SetPixelFormat **assumes success**: `wglCreateContext`/`wglMakeCurrent`
are fire-and-forget and fabricate the handle locally. There is **one spin-wait**
(`wglChoosePixelFormat` polling `0xFB8`) that hangs forever if the host never publishes
a window. So: a host that refuses **at or before SetPixelFormat, and keeps `0xFB8`
reads nonzero**, is survivable today with zero wrapper changes; refusal any later is
invisible to the client.

---

## 0. Transport primer (guest side)

Four fixed guest-physical ranges, mapped at DLL load via the guest kernel helper
(`\\.\MAPMEM` driver on NT — `fxlib/fxlibnt.c:19-57`; `FXMEMMAP.VXD` on 9x —
`fxlib/fxlib9x.c:19-55`; selected by `DllMain`, `wrapgl32.c:17528-17533`):

| ptr | GPA | size | use |
|---|---|---|---|
| `ptm` | `MESAPT_MM_BASE 0xefffe000` (`wrapgl32.c:40`) | 4 KB | MMIO register page |
| `mfifo` | `MESA_FIFO_BASE 0xec000000` | `MGLSHM_SIZE 0x3ffc000` | fifo ring + arg regs + scratch |
| `fbtm` | `MESA_FBTM_BASE 0xea000000` | `MGLFBT_SIZE 0x2000000` | bulk texture/pixel staging |
| `mbufo` | `MBUFO_BASE 0xe0000000` | 128 MB | zero-copy mapped buffer objects |

(constants: `qemu-1/hw/mesa/mglfuncs.h:16-38`, mirrored by the host's
`hw/mesa/mglfuncs.h` — this header pair plus the `FEnum_*` ordinals in `mglfunci.h`
and `szgldata.c` are the guest↔host ABI; the ICD must build against a copy that
matches the qemu-libretro host build.)

Ring layout (`InitMesaPTMMBase`, `wrapgl32.c:58-79`): `mfifo[0]` = shared write cursor
(reset to `FIRST_FIFO`=24), `mdata = &mfifo[MAX_FIFO]`, `mdata[0]` = shared data
cursor, `mdata[1]` = cross-process DLL refcount, `pt = &mfifo[1]` = per-call argument
registers, and **`pt[0]` = the owner's guest-virtual address of `ptm+0xFC0`** — the
ring-claim word. Top of the shm doubles as scratch: last page = WGL call marshalling
area (`WGL_FUNCP`, `:16632-16640`), last 3 pages = GL readback area (`fifoOutData`,
`:105-114`), `MGLSHM_SIZE-ALIGNED(1)` = SwapBuffers ret slot (`:17340`).

Two submission modes for the ~2900 GL stubs:
- **queued:** `FIFO_GLFUNC` appends `FEnum`+args to the ring if it fits (`:540-544`);
  drained by the host on the next `MESAGL_MAGIC` poke to any register
  (`mesapt_mm.c:2264-2272`).
- **sync:** `*pt0 = FEnum` — an MMIO write *through `pt[0]`* to `ptm+0xFC0`; host
  drains the ring, then executes that call immediately (`mesapt_mm.c:2243-2257`);
  return value read back as `*pt0` (host `s->FRet`, `mesapt_mm.c:384-386`) — see
  `glMapBuffer` (`wrapgl32.c:6957-6963`). Out-params come from the 3-page readback
  area via `fifoOutData` (e.g. `glGetIntegerv`, `:4602-4608`; `glGetString`,
  `:5490-5492`).

Bulk data: `fifoAddData` copies into the ring data area (`:92-103`); larger payloads
stage through `fbtm` top-down (`glBufferData` `:1152-1157`, chunked
`glBufferSubData` `:1194-1200`, `glCompressedTexImage*` `:1843+`) or bottom-up for
readback (`glGetCompressedTexImage` reads size from `fbtm[0]`, `:4301-4307`).

Guest-side MMIO register census (all pokes write `MESAGL_MAGIC` unless noted):

| reg | dir | guest site | host handler |
|---|---|---|---|
| `0xFBC` | W+R | DllMain attach `0xA0320` / detach `0xD0320`; read expects `0x320a0` (`:17568-17573,17587`) | `mesapt_mm.c:2218-2241`; read = `MesaVer` `:381-383` |
| `0xFC0` | W+R | sync GL dispatch via `pt[0]`; read = FRet | `:2243-2262`; read `:384-386` |
| `0x0FC` | W | `FIFO_FLUSH` (`:116-117`) | drain-only path `:2264-2272` |
| `0xFB8` | R | window-ready poll (`:17405-17406`) — **the only spin** | `glwnd_ready()` `:378-380`; `mglcntx_mingw.c:167` |
| `0xFEC` | W+R | `wglChoosePixelFormat` (`:17398-17409`) | `:2362-2380`; read = `pixfmt` |
| `0xFE8` | W+R | `wglDescribePixelFormat` (`:17414-17429`) | `:2382-2389`; read = `pixfmtMax` |
| `0xFE4` | W+R | `wglSetPixelFormat` (`:17448-17473`) | `:2391-2408` (+3 s `crashRC` ring-warp `:2399-2407`); read = `procRet` |
| `0xFE0` | W+R | GL-ext presence query in `wglGetProcAddress` (`:17181-17183`) | `:2410-2416`; read = `procRet` |
| `0xFDC` | W | 23 named-WGL-func calls (`WGL_FUNCP`); results read back in-place from scratch | `MGLFuncHandler` `mglcntx_mingw.c:523-771`; special-cased in `mesapt_mm.c:2418-2445` |
| `0xFD8` | W | WM_ACTIVATE forward from a `WH_CALLWNDPROC` hook (`:17499-17513`) | `:2447-2456` |
| `0xFF0` | W | `wglSwapBuffers` (`:17373`) | `:2350-2360` |
| `0xFF4` | W (val=RC) | `wglDeleteContext` (`:17263`) | `:2331-2348` |
| `0xFF8` | W | `wglMakeCurrent` (`:17226`) | `:2289-2329` |
| `0xFFC` | W | `wglCreateContext` (`:17200`) | `:2275-2287` |

---

## 1. Attach sequence: DLL load → first frame

**A. `DLL_PROCESS_ATTACH`** (`wrapgl32.c:17540-17575`):
1. `drv.Init()` — open `\\.\MAPMEM`. Fail → `DllMain` returns FALSE (`:17552-17555`).
2. `InitMesaPTMMBase()` — map the four ranges (`:63-66`). `!ptm || !mfifo` → fail
   (`:68-69`). **Guest-side collision sniff** (`:72-73`): if `mfifo[1]`'s low 12 bits
   == `0xFC0`, another live process has claimed the ring → "fail", but see §2.4.
3. On clean claim: `mfifo[0]=FIRST_FIFO`, `mdata[0]` reset, **`pt[0] = &ptm[0xFC0>>2]`
   (this process's virtual address — the claim word)** (`:74-76`), shared refcount
   `mdata[1]=1` (`:17549`).
4. Install the `WH_CALLWNDPROC` hook for WM_ACTIVATE forwarding (`:17563`).
5. Write the 8-byte git-rev build signature `rev_` to the top of `fbtm`
   (`:17567`) — the host's activation gate compares this against its own compiled-in
   `rev_` (`mesapt_mm.c:2222`). Guest and host **must be built from the same commit**.
6. Poke `0xFBC = 0xA0320` (`(0xA0<<12)|MESAVER`, `:17568`). Host: verify signature,
   `InitMesaGL()` (dlopen host GL), `MGLTmpContext()` (discovery window + throwaway
   context to harvest `wglFuncs`/WGL-ext pointers, `mglcntx_mingw.c:224-267`), reset
   the global attestation flag, set `MesaVer` (`mesapt_mm.c:2220-2228`).
7. Read `0xFBC` back; anything ≠ `(MESAVER<<8)|0xA0` = `0x320a0` → **`DllMain`
   returns FALSE** (`:17569-17573`) → process's `LoadLibrary`/static import of
   opengl32 fails.

**B. Pixel format.** Apps arrive via `ChoosePixelFormat` and/or `DescribePixelFormat`,
then `SetPixelFormat`:
- `wglChoosePixelFormat` (`:17398-17409`): copies cfg words (from per-EXE
  `wrapgl32.ext`, `PPFD_CONFIG` `:17392-17396`) + PFD into scratch, pokes `0xFEC`.
  Host: applies cfg, `MGLChoosePixelFormat()` → `MGLWindowInit()` →
  `mesa_prepare_window()` — **destroys any existing render window**
  (`libretro-3dfx.c:712`), checks `fx_3d_activation_allowed()` (SMP → `vm_stop`,
  `:268-285`), creates the render window on the main thread; `cwnd_mesagl` publishes
  `hwnd/hDC` and sets `wnd_ready` (`mglcntx_mingw.c:188-195,269-282`). Guest
  **spins reading `0xFB8` until nonzero** (`:17405-17406`), then reads the format
  from `0xFEC`.
- `wglDescribePixelFormat` (`:17414-17429`): pokes `0xFE8`, reads count back; 0 = fail.
- `wglSetPixelFormat` (`:17448-17473`): first walks the caller's stack frame to find a
  return address and IAT-patches `Get/SetDeviceGammaRamp` (`HookDeviceGammaRamp`,
  `:17046-17073`) and `timeGetTime`/QPC (`HookTimeGetTime`, `fxlib/fxhook.c`) — the
  per-game timing shims. Then scratch = `{format, (uint32_t)ptm, PFD}`, poke `0xFE4`,
  read back: `== MESAGL_MAGIC` → success, cache `currPixFmt = format` (`:17470-17471`).
  Host side also runs the 3 s `crashRC` reclaim here: if the previous owner has been
  silent >3 s it re-points the ring-claim word `mfifo[1]` at *this* client's `ptm`
  mapping and decrements the shared refcount (`mesapt_mm.c:2399-2407`).

**C. Context.** `wglCreateContext` = `mglCreateContext` (`:17191-17212`): bail locally
if `currPixFmt == 0` (`:17196-17197`); else write the DC to scratch, poke `0xFFC`
(host: `MGLCreateContext` → `hRC[0]`, gated on the attestation flag,
`mesapt_mm.c:2275-2287`), and **return the arithmetic handle `MESAGL_MAGIC`
unconditionally** — no status readback. `wglMakeCurrent` = `mglMakeCurrent`
(`:17214-17246`): scratch = `{RC, rev_, build-string}`, poke `0xFF8` (host:
`wglMakeCurrent(hDC,hRC[n])`, `InitMesaGLExt`, forced swap-interval 0, overlay
activation, `disp`/`crashRC` timers — `mesapt_mm.c:2289-2329`), then guest applies
`wrapgl32.ext` options (SRGB, swap interval) and sets `currGLRC` — again **no
readback, returns TRUE**. Nested/shared contexts go through
`wglCreateContextAttribsARB` (`:16749-16779`), which *does* check the host's answer
(scratch `argsp[0]`, host `mesapt_mm.c:2418-2432`) and maintains the guest `level`
counter mirroring the host's `hRC[]` stack; handles are `MESAGL_MAGIC - level`.

**Attestation** is passive and rides on `glDebugMessageInsertARB` — the only call the
host executes without a current context (`mesapt_mm.c:2244-2245`). The wrapper emits
messages via `OHST_DMESG` (`:386-393`) from extension filtering (`fltrxstr`,
`:430,446`) and the hook patchers (`:17031,17037`; `fxhook.c`). Host SHA-1s each
message and compares against the `ATTEST_IV` denylist (`mesagl_pfn.h:27-40`,
`mesagl_blit.c:27-35`, dispatch at `mesapt_mm.c:1612-1623`); a match sets the global
`mglCntxAtt`, which silently zeroes the *next* `wglCreateContext` result host-side
(`:2280,2426`). The guest never learns about it on the legacy path (see §2.6).

**D. Frame.** GL calls queue/sync per §0. `wglSwapBuffers` (`:17336-17386`): cursor
sync via `wglSetDeviceCursor3DFX` over `0xFDC` (`:16956-17014`), write `swapFps` to
the ret slot, poke `0xFF0`. Host: `MGLSwapBuffers()` → `mesa_swap_notify` readback →
`fx.sink->publish` → libretro frame (`mglcntx_mingw.c:372-383`). Guest reads the slot
back for the FPS-limit pacing loop (`:17375-17384`) and returns bit 0.

---

## 2. Failure propagation — what the wrapper does when the host refuses

Per step, if the host refuses/returns garbage **today**:

1. **Device absent / MAPMEM missing** — `DllMain` FALSE (`:17546,17555`). App fails to
   load opengl32 ("DLL initialization failed"). Clean-ish but app-fatal; fine for a
   per-game shim, **unacceptable for a system-wide ICD** (would kill every GL process
   on a host without the device). The ICD must move all of this out of `DllMain`.
2. **Activation refused** (`0xFBC` readback ≠ `0x320a0`, i.e. signature mismatch or
   `InitMesaGL` failure ⇒ host `MesaVer`=0) — `DllMain` FALSE (`:17569-17573`). Same
   app-fatal-but-clean behavior. **This is the only pre-pixel-format refusal the
   wrapper already handles.**
3. **`wglChoosePixelFormat` with no window** — guest **spins forever** on `0xFB8`
   (`:17405-17406`). The host comment even documents it: "wnd_ready stays 0; the
   guest keeps polling" (`libretro-3dfx.c:718`). Today this is masked because the
   only deny path (`fx_3d_activation_allowed`) stops the whole VM first
   (`:268-285`); under Step 2a refusal semantics this becomes a **live app hang**
   unless the host breaks the loop. Answer to the open question: **the fifo client
   does *not* tolerate a busy host at this point — it spins.** Note
   `wglDescribePixelFormat`/`wglSetPixelFormat` have no such spin; an app whose first
   pixel-format call is Describe/Set fails cleanly, one that calls Choose first hangs.
4. **Ring already claimed by a live owner** (`mfifo[1]` sniff, `:72-73`) — this is
   *detected* but **not refused**: `DllMain` still returns TRUE, merely skipping
   activation/ring-reset and bumping the shared refcount (`:17542-17547`). The second
   process then shares the owner's ring: its `wglChoosePixelFormat` succeeds (window
   exists ⇒ `0xFB8`=1, `0xFEC` returns the owner's format), its `wglSetPixelFormat`
   succeeds (host `MGLSetPixelFormat` returns 1 on the already-set window and
   `procRet` is only gated on `mfifo[1]` being nonzero — the *owner's* claim,
   `mesapt_mm.c:2397-2398`), its `wglCreateContext` clobbers the owner's `hRC[]`
   (`mglcntx_mingw.c:335-341`), and its sync GL stubs dereference `pt[0]` — **the
   owner's virtual address of the MMIO page** — in the wrong address space
   (`:544,551`…): AV or silent corruption. Both processes believe they own the
   passthrough.
5. **Refused `wglSetPixelFormat`** (`0xFE4` read ≠ MAGIC) — returns FALSE,
   `currPixFmt=0` (`:17470-17471`); the subsequent `wglCreateContext` fails **locally,
   without touching the host** (`:17196-17197`); `wglGetPixelFormat` returns 0
   (`:17434-17444`). **This is the one clean, complete, already-wired refusal chain**:
   SetPixelFormat=FALSE → CreateContext=NULL → app sees a normal WGL failure.
6. **Refused/failed `wglCreateContext` or `wglMakeCurrent` after a successful
   SetPixelFormat** (e.g. attestation-denied, or host refuses at `0xFFC`) — invisible.
   Guest fabricates `MESAGL_MAGIC` (`:17204-17211`) and `TRUE` (`:17245`). App then
   runs against an inert host: queued calls are drained-and-dropped, sync calls hit
   the `!mglContext` branch which only zeroes the first 8 bytes of the readback area
   (`mesapt_mm.c:2259-2262`), so:
   - `glGetString` copies zeros → empty strings (guest gate `:5467-5468` passes since
     `currGLRC` is set);
   - `glGetIntegerv`-style queries return 0/stale (`:4602-4608`);
   - `glMapBuffer` returns a pointer built from a **stale `FRet`** (`:6961-6962`) —
     app writes through garbage → crash;
   - `glGetCompressedTexImage` memcpys `fbtm[0]` (stale, up to 32 MB) into the app's
     buffer (`:4301-4307`, capped only by `FBTMMCPY` `:118-119`) → heap smash;
   - queued stubs keep advancing `mfifo[0]`/`mdata[0]` unchecked past `MAX_DATA`
     (`fifoAddData` warns but writes anyway, `:98-102`) → eventually runs off the
     mapping → AV.
   Verdict: **refusal must never be signalled later than SetPixelFormat** on the
   legacy path. (The ARB path is safe: `wglCreateContextAttribsARB` checks `argsp[0]`
   and returns NULL, `:16764-16778`.)
7. **`wglSwapBuffers` with inert host** — reads back its own `swapFps` write; returns
   FALSE, no hang (`:17371-17385`). Benign.
8. **`0xFDC` WGL-ext calls with inert host** — return values are whatever inputs were
   left in scratch (`WGL_FUNCP_RET`, `:16638-16640`); unknown funcs host-side just log
   (`mglcntx_mingw.c:769`). Garbage-but-bounded: no hang, wrong booleans.
9. **`wglGetProcAddress`** — WGL-ext names are answered **locally, unconditionally**
   (`:17122-17170`); GL-ext names ask the host (`0xFE0`) and NULL out cleanly on 0
   (`:17180-17184`). So an app can hold live function pointers into the wrapper even
   after a refused attach — the pointers themselves are safe (they hit the paths
   above), but the ICD must not treat GetProcAddress success as "host granted".

---

## 3. Cached state

**Per-process statics in `wrapgl32.c` (all file-scope, no TLS — the wrapper is
single-context-per-process by construction):**
- Transport pointers: `ptm/pt0/pt/mfifo/mdata/fbtm/mbufo` (`:43-49`) — per-process
  mappings of shared physical memory.
- WGL state: `currPixFmt` (`:124`), `currDC/currGLRC` (`:125`), `currPB[16]` pbuffer
  handles (`:126`), `level` context-stack depth (`:16749`), `GLwnd` (`:122`), `hHook`
  (`:123`), `wglGetPixelFormat`'s `static int fmt` (`:17436`).
- GL client state mirror: `vtxArry`/`Interleaved` vertex-array descriptors, pack/unpack
  buffer bindings, `queryBuf` (`:133-145`) — reset by `InitClientStates()` on context
  create/recreate (`:376-384`, called `:17208,16774`).
- String caches: `vendstr/rendstr/vernstr/extnstr/glslstr` (`:127-131`) — refilled on
  every `glGetString` call, filtered per `wrapgl32.ext` (`fltrxstr`, `:411-456`).
- Options/behavior: `swapCur/swapFps/texClampFix` (`:469`), parsed per call from
  `wrapgl32.ext` (`parse_options`, `:498-538`); `logpname` heap block (`:17238-17240`);
  one-shot `once` in `mglGetProcAddress` (`:17174-17176`); hook state in fxlib.

**Shared (cross-process, lives in the physical ring):**
- `mfifo[0]` fifo cursor, `mdata[0]` data cursor — reset only by the claiming process
  at DLL load (`:74-75`) and by the host after each drain (`mesapt_mm.c:2187,2199`).
- `mfifo[1]` (= `pt[0]`) ring-claim word — set at load (`:76`), cleared at last-out
  detach (`:17589`), force-warped by the host's 3 s reclaim (`mesapt_mm.c:2402-2405`).
- `mdata[1]` cross-process DLL refcount (`:17544-17550,17577`).
- `fbtm` top: build signature (written `:17567`, cleared `:17588`).

**What a refused attach must reset:** on the clean path (§2.5) nothing leaks —
`currPixFmt` stays 0 and gates everything. The dangerous residue is: (a) a fabricated
`currGLRC/currDC` if refusal came after create/make-current (§2.6) — the ICD must not
set these without a host ack; (b) the shared ring claim: a client that failed *after*
claiming (`DllMain` succeeded, then refused later) leaves `mfifo[1]`/`mdata[1]`
populated until process detach — a refused ICD attach must either never claim the ring
or explicitly release (`mfifo[1]=0`, refcount decrement) so the next client's load
isn't misdetected as "busy" (§2.4). (c) `wglSetPixelFormat`'s IAT patches
(gamma/timer hooks) are installed *before* the host is consulted (`:17455-17456`) and
are not unwound on refusal — harmless functionally (they wrap GDI/winmm), but the ICD
should defer them until after a granted attach.

---

## 4. Entry point inventory vs. the ICD `Drv*` surface

**What the wrapper exports today** (no `.def` in-tree; generated at build time by
`gendef` + sed over the built DLL — `Makefile.in:38-40` — with `mgl*` renamed to
`wgl*` and the `wgd` prefix stripped to produce GDI-alias names; export count pinned
to 2977 by `exports-check`, `Makefile.in:29-32`):
- ~2900 `gl*` stubs (generated, `:549-16630`);
- `wgl*`: ChoosePixelFormat, CopyContext, CreateContext, CreateLayerContext,
  DeleteContext, DescribeLayerPlane, DescribePixelFormat, GetCurrentContext,
  GetCurrentDC, GetDefaultProcAddress, GetLayerPaletteEntries, GetPixelFormat,
  GetProcAddress, MakeCurrent, RealizeLayerPalette, SetLayerPaletteEntries,
  SetPixelFormat, ShareLists, SwapBuffers, SwapLayerBuffers, UseFontBitmapsA/W,
  UseFontOutlinesA/W;
- GDI aliases (from `wgd*`): ChoosePixelFormat, DescribePixelFormat, GetPixelFormat,
  SetPixelFormat, SwapBuffers (`:17387-17388,17410-17412,17430-17432,17445-17446,17474-17476`);
- stray fxlib publics (HookDeviceGammaRamp etc.). **Zero `Drv*` exports.**

**What the loader needs** — ReactOS opengl32 (`refs/reactos/dll/opengl/opengl32/
icdload.c:311-327`) hard-requires 16 exports (missing any one ⇒ ICD load fails ⇒
software fallback) plus two optional (`DrvValidateVersion` `:281-289`,
`DrvSetCallbackProcs` `:291-300`); interface types in `icd.h:365-392`. Mapping:

| Drv* entry | existing wrapper logic | gap notes |
|---|---|---|
| `DrvSetPixelFormat(HDC,int)` | `wglSetPixelFormat` `:17448-17473` | closest-to-done; already tolerates NULL PFD (`:17466-17468`); drop the stack-walk hook trigger or move it |
| `DrvDescribePixelFormat` | `wglDescribePixelFormat` `:17414-17429` | must return the *max format count* even for `ppfd==NULL` probes — host `pixfmtMax` provides it; beware: host side creates the render window on first call (`MGLDescribePixelFormat`→`MGLWindowInit`, `mglcntx_mingw.c:471`) — needs the Step 2a gate so dxdiag probing formats doesn't seize the window |
| `DrvCreateContext(HDC)` | `mglCreateContext` `:17191-17212` | **needs a host ack** (today fire-and-forget, fabricates handle — §2.6); return `DHGLRC` (can keep the MAGIC scheme internally) |
| `DrvCreateLayerContext(HDC,int)` | `mglCreateLayerContext` `:17286-17291` (stub TRUE) | implement as plane==0 → DrvCreateContext, else fail |
| `DrvSetContext(HDC,DHGLRC,callback)` | `mglMakeCurrent` `:17214-17246` | **biggest structural gap**: must return the 336-entry `GLCLTPROCTABLE` (`icd.h:10,354-358`) — a static table pointing at the existing `gl*` stubs; wrapper has all the functions, no table |
| `DrvReleaseContext(DHGLRC)` | none — approx. `mglMakeCurrent(0,0)` (see `:17460`) | new, trivial (flush + clear current) |
| `DrvDeleteContext(DHGLRC)` | `mglDeleteContext` `:17248-17265` | direct |
| `DrvCopyContext` | `mglCopyContext` `:17279-17284` (fire-and-forget TRUE; host warns unhandled) | honest-failure stub is acceptable |
| `DrvShareLists` | `mglShareLists` `:17293-17302` (real, via `0xFDC`) | direct |
| `DrvGetProcAddress` | `mglGetProcAddress` `:17114-17186` | direct; ensure extension fns use ICD calling expectations |
| `DrvSwapBuffers(HDC)` | `wglSwapBuffers` `:17336-17386` | direct |
| `DrvSwapLayerBuffers` | `mglSwapLayerBuffers` `:17390` | direct |
| `DrvDescribeLayerPlane` | `mglDescribeLayerPlane` `:17083-17089` (FALSE) | fine as-is |
| `DrvGetLayerPaletteEntries` | `mglGetLayerPaletteEntries` `:17091-17097` (FALSE) | fine |
| `DrvRealizeLayerPalette` | `mglRealizeLayerPalette` `:17099-17104` (FALSE) | fine |
| `DrvSetLayerPaletteEntries` | `mglSetLayerPaletteEntries` `:17106-17112` (FALSE) | fine |
| `DrvValidateVersion(DWORD)` *(optional but load-bearing for us)* | none — logic exists as DllMain steps 5-7 (`:17567-17573`) | **relocate the whole attach handshake here** (or first DrvSetPixelFormat): mapping, signature write, `0xA0320`, readback; return FALSE on refusal ⇒ loader falls back to software without killing the process |
| `DrvSetCallbackProcs` | none | store `wglSetCurrentValue/wglGetCurrentValue/wglGetDHGLRC`; trivial |

Also missing vs. a real ICD: per-`DHGLRC` bookkeeping (the loader hands handles back;
wrapper only tracks one implicit current context), and `wglMakeContextCurrentARB`-style
pbuffer plumbing already half-exists (`:16925-16940`, `currPB[]`).

Everything below the WGL layer — the fifo client of §0, the `gl*` stub army, fxlib —
carries over unmodified; the ICD is essentially a new ~1k-line front replacing
`wrapgl32.c:16632-17601` plus a proc-table.

---

## 5. Required host refusal semantics (input to Step 2a)

For the existing client code (and the ICD derived from it) to fail out cleanly instead
of hanging or corrupting, a "busy/denied" host must:

1. **Refuse at activation:** on a denied `0xFBC=0xA0320`, leave/force `MesaVer=0` so
   the `0xFBC` read returns ≠`0x320a0` (`mesapt_mm.c:2220-2228,381-383`). Current
   wrapper: DLL load fails (app-fatal but clean). Future ICD: DrvValidateVersion
   returns FALSE → software GL. Denied activation must also skip `MGLTmpContext`/
   `InitMesaGL` side effects on the owner (audit item: `mesa_prepare_window` must not
   `fx_window_destroy()` the incumbent, `libretro-3dfx.c:712`).
2. **Never leave `0xFB8` reads at 0 for a refused client.** The guest spin
   (`wrapgl32.c:17405-17406`) accepts *any* nonzero value. Recommendation: on a
   refused/denied session, `0xFEC` handling must not run `MGLWindowInit` for the
   denier, and the `0xFB8` read should return a nonzero "refused" sentinel (guest then
   reads `0xFEC`=0 and `wglChoosePixelFormat` returns 0 — a failure the app handles).
   This is the **one place the wrapper needs the host's help to avoid a hang.**
3. **Refuse at SetPixelFormat:** `0xFE4` read must return ≠`MESAGL_MAGIC` (return 0;
   guest maps any non-MAGIC to FALSE, `:17470`). This alone makes the *entire* legacy
   context path fail cleanly (`currPixFmt=0` gates `wglCreateContext` locally,
   `:17196`). **This is the primary refusal point — everything later is too late**
   (§2.6: `0xFFC`/`0xFF8` have no readback).
4. **Refuse `0xFE8` (Describe) with 0** — guest returns 0 cleanly (`:17423-17428`).
   Same for `0xFE0` ext queries (`procRet=0` → GetProcAddress NULL, `:17183`).
5. **ARB paths:** for a denied client, `0xFDC:wglCreateContextAttribsARB` must set
   `argsp[0]=0` (guest checks it, `:16764-16778`) and `wglChoosePixelFormatARB`
   should report `argsp[0]=0`/format 0 (`:16740-16744`).
6. **Do not let a non-owner mutate shared state:** the `0xFE4` crashRC warp
   (`mesapt_mm.c:2399-2407`) must be conditioned on real owner-death, not wall-clock
   silence, or a denied client's SetPixelFormat steals the ring claim from an idle
   owner (§2.4). Likewise attestation (`mglCntxAtt`) and the `0xFEC/0xFE8`
   config dispatch (`PPFD_CONFIG_DISPATCH`, `:2363-2379`) are global today — a denied
   client's pokes currently reconfigure the owner's MSAA/scaler/timer options.
7. **Drops must stay bounded:** keep the current behavior of draining-and-discarding
   fifo pokes from non-owners *and* resetting the shared cursors (`mesapt_mm.c:2187,
   2199,2256`), otherwise a misbehaving refused client that ignores WGL failures will
   walk `mdata[0]` off the end of the mapping (§2.6 last bullet).
8. **Ring-claim hygiene:** if refusal happens after a client claimed `mfifo[1]`
   (first-loader loses arbitration later), the host's grant to the next owner must
   re-point/clear the claim word — the DllMain sniff (`:72-73`) otherwise reports
   "busy" to every subsequent loader until reboot.

Summary: **the wrapper survives refusal at
DLL-load and at SetPixelFormat; it hangs only on the `0xFB8` window-ready poll; it
silently fabricates success at CreateContext/MakeCurrent.** Refusal design: deny at
activation and SetPixelFormat, keep `0xFB8` nonzero, and the existing client needs no
patch; the ICD then converts those two failures into DrvValidateVersion/
DrvSetPixelFormat=FALSE for MS-opengl32 software fallback.

---

## Build-system notes (feeds Step 3)

- `Makefile.in` insists on MSYS2 `MINGW32` (`:54`), uses `gendef` for the export map
  (`:38-40`), pins export count 2977, embeds `git rev-parse --short HEAD` as `rev_`
  (`stamp.o`, `:48-51`) — which is also the **attach signature**, so the ICD's
  signature scheme must either reuse the host's commit hash or the Step 2a host must
  adopt a version handshake decoupled from git revs.
- `CFLAGS -march=x86-64-v2 -mfpmath=sse` (`:8`) — SSE4-era codegen inside the guest;
  fine for QEMU's default CPU models but worth noting for XP-era `-cpu` choices.
  `wglSetPixelFormat` relies on `-fno-omit-frame-pointer`/no-sse2 codegen for its
  stack-walk (`COMPACT_FRAME`, `wrapgl32.c:16-18,17452-17454`) — drop the stack-walk
  in the ICD rather than inherit that fragility.
- Guest kernel dependency: `\\.\MAPMEM` physical-memory mapper (fxlibnt). XPDriver
  replaced it with its own fixed-window driver `qxpmap.sys` (`docs/design/qxpmap.md`);
  XPDriver never shipped the qemu-3dfx MAPMEM `.sys`.
- fxlib extras the ICD probably should *not* inherit system-wide: per-EXE binary
  patches (`fxhpat.c`, md5-keyed page patches), timeGetTime/QPC IAT hooks
  (`fxhook.c`, `fxtime.c`) — per-game shims by nature; make them opt-in via
  `wrapgl32.ext` at most.
