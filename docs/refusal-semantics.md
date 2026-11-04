# Refusal-semantics contract: host ↔ guest (2026-07-14, v0.8 — NOT FROZEN, see §6)

**What this is:** the normative contract between the hardened qemu-libretro host
(Step 2a/2b) and its guest GL clients — the legacy qemu-3dfx wrapper (unmodified) and
the `qxpicd.dll` ICD (Step 4). Both implementations are built against this
document. Clauses are numbered R1..R26; each has a statement, rationale, code
citations, and the acceptance test that exercises it.

**Inputs:** `docs/guest-wrapper-notes.md`, `docs/icd-fallback-analysis.md`,
`docs/icd-fallback-external-evidence.md`. Code lines cite the qemu-libretro
working tree @ `869b6ca16a`
(`hw/mesa/mesapt_mm.c`, `hw/mesa/mglcntx_mingw.c`, `ui/libretro-3dfx.c`), the wrapper
@ `b47f30a396` (`refs/qemu-3dfx/wrappers/mesa/src/wrapgl32.c`), and ReactOS opengl32
(`refs/reactos/dll/opengl/opengl32`).

**Acceptance-test keys:** `2a-acc` = two shim'd GL apps: first renders, second
fails cleanly, order-independent and repeatable; `2b-acc` = kill owner → reclaim,
with an idle owner not reclaimed; `2d-acc` = EmuVR kit-v12 regression coverage;
`T0`..`T12` = the stub-ICD battery in `docs/icd-fallback-analysis.md` §5.
The stock-XP refusal outcomes and owner-isolation results are summarized in §6.

---

## 0. Definitions and new protocol constants (normative)

**Session states** (host-side, one state machine in `MesaPTState`):

| State | Meaning |
|---|---|
| `FREE` | No owner, no live reservation. Any client may proceed to bind. |
| `RESERVED(T)` | Soft-reserved by probe token `T`; expires `MESA_PROBE_TTL` ms after the last reserving write (lazy evaluation — no timer). |
| `OWNED` | A session holds the passthrough: bound at its first granted `0xFE4` (R12), released per R17. |

**New constants** (to be added to both copies of `mglfuncs.h` — the header pair is the
ABI, `docs/guest-wrapper-notes.md` §0):

| Constant | Value | Use |
|---|---|---|
| `MESA_BUSY` | `0xB5B5B5B5` | busy sentinel returned on `0xFBC` and `0xFB8` reads (R1, R3). Nonzero; ≠ `0x320a0` (activation success, wrapgl32.c:17570); ≠ `MESAGL_MAGIC` `0x5b5eb5e5` (mglfuncs.h:18). |
| `MESA_PROBE` | MMIO offset `0xFB4` | read-only busy-probe + soft-reservation doorbell (R10, R11). Free on both sides today: absent from `mesapt_read`/`mesapt_write` (mesapt_mm.c:372-403, 2213-2464) and from the wrapper's register census (`docs/guest-wrapper-notes.md` §0 table; no `0xFB4` in wrapgl32.c). |
| `MESA_PROBE_TTL` | 2000 ms | soft-reservation lifetime (R11). |
| `MESA_HEARTBEAT` | MMIO offset `0xFB0` | QXPICD-only lease renewal; accepted only while OWNED and only when the 16-bit write matches the holder token. |
| `MESA_HEARTBEAT_TTL` | 5000 ms, virtual time | host-driven QXPICD owner-death cleanup; virtual time prevents expiry while the VM is paused. |
| probe tag | `0x3D` in bits 31-24 | distinguishes a probe-aware host from an old host, whose unhandled read returns 0 (mesapt_mm.c:398-400) (R10, R24). |

**Identity evidence.** The legacy ABI carries a claimant VA in the requester's
guest-VA of its MMIO mapping (`ptm`), written into the
`wglSetPixelFormat` scratch payload (wrapgl32.c:17465) and read by the host at
mesapt_mm.c:2396; the current claimant's VA also sits in the ring-claim word
`mfifo[1]` (wrapgl32.c:76). Page-aligned, so bits 11-0 are always `0xFC0`
(host masks `& 0xFFFFF000`, mesapt_mm.c:2402). Real-XP `2a-acc` proved that this
VA is not process-unique: two processes used `0097e000`. The legacy wrapper's
shared `mdata[1]` ring reference count supplies the missing evidence: DllMain
increments it when a second process ring-sniff shares. Owned-state attribution
therefore combines session state, `ptm`, and `mdata[1] <= 1`; R7 and R8 record
the remaining unattributable writes.

**Guest-visible touchpoints covered** (from the wrapper census plus QXPICD,
`docs/guest-wrapper-notes.md` §0): `0xFBC` (activation/teardown + version readback),
`0xFB0` (QXPICD heartbeat), `0xFB8` (window-ready poll), `0xFEC` (ChoosePixelFormat), `0xFE8`
(DescribePixelFormat), `0xFE4` (SetPixelFormat), `0xFE0` (GL-ext query), `0xFDC`
(named WGL funcs incl. ARB create/choose), `0xFD8` (WM_ACTIVATE), `0xFC0` (sync
dispatch + FRet), `0xFF0/0xFF4/0xFF8/0xFFC` (swap/delete/make-current/create), plus
the new `0xFB4`.

---

## 1. Host behavior when BUSY (state = OWNED, request not from the owner)

**R1 — Activation is refused without side effects; readback is `MESA_BUSY`.**
While `OWNED`, a `0xFBC=0xA0320` write MUST NOT run `InitMesaGL()`, MUST NOT run
`MGLTmpContext()` (which overwrites the file-scope `hwnd` with a throwaway window,
mglcntx_mingw.c:224-267 at :265), and MUST NOT reset `s->mglCntxAtt`
(today all three run unconditionally, mesapt_mm.c:2218-2228 at :2223-2226). The
following `0xFBC` read MUST return `MESA_BUSY` (`0xB5B5B5B5`). Guest result: the
wrapper compares the readback against `0x320a0` and returns FALSE from `DllMain`
(wrapgl32.c:17568-17573) — the app's `LoadLibrary` of opengl32 fails: app-fatal but
clean, no owner state touched. Non-busy activation failure (signature mismatch,
`InitMesaGL` error) keeps today's `MesaVer=0` readback (mesapt_mm.c:2221,
:381-383) so the two refusal causes are distinguishable in guest logs.
While `OWNED`, a `0xFBC=0xD0320` write is treated per R17.
*Tests:* 2a-acc (second app started while first renders); 2d-acc (single client:
readback still `0x320a0`).

**R2 — reserved.** (Folded into R1.)

**R3 — `0xFB8` MUST NOT read 0 once a request has been refused or while `OWNED`.**
Rule: `0xFB8` read returns 1 when `wnd_ready` (unchanged, mesapt_mm.c:378-380,
mglcntx_mingw.c:167); otherwise, if state is `OWNED` or the most recent
`0xFEC/0xFE8/0xFE4` write was refused, it returns `MESA_BUSY`; only an un-owned,
un-refused, window-pending state may return 0. Rationale: `wglChoosePixelFormat`
spins `while (!ready) ready = ptm[0xFB8>>2];` and then discards the value
(wrapgl32.c:17400-17407 — `ready` is dead after the loop), so ANY nonzero value
terminates the spin and is safe; a permanent 0 is a live app hang (the deny path
today never reaches this state only because `fx_3d_activation_allowed()` stops the
whole VM first, ui/libretro-3dfx.c:268-285, :718 "wnd_ready stays 0; the guest keeps
polling"). `MESA_BUSY` (not 1) is used so a real ready window and a refusal are
distinguishable to the future ICD and in traces.
*Tests:* 2a-acc (second app calling ChoosePixelFormat first must fail, not hang);
2d-acc (owner sees 0→1 exactly as today).

**R4 — Refused `0xFEC` (ChoosePixelFormat): no side effects, next read returns 0.**
While `OWNED`, a foreign `0xFEC` write MUST NOT run `MGLChoosePixelFormat` →
`MGLWindowInit` → `mesa_prepare_window` (mesapt_mm.c:2380, mglcntx_mingw.c:421-432,
ui/libretro-3dfx.c:707-721) and MUST NOT execute `PPFD_CONFIG_DISPATCH` (R16). The
host latches `s->pixfmt = 0` so the requester's `0xFEC` read returns 0
(mesapt_mm.c:387-389). Guest result: `wglChoosePixelFormat` returns the read
verbatim (wrapgl32.c:17407-17408) → 0 → normal WGL failure. Attribution caveat: the
`0xFEC` payload carries no identity, so "foreign" here means "any `0xFEC` while
`OWNED` and the owner's format is already set" — an owner app that re-calls
ChoosePixelFormat after binding MUST still be answered with its established format
(host returns the current `GetPixelFormat(hDC)` value without re-running
window/config side effects, cf. mglcntx_mingw.c:426-429). The residual race (B's
refused poke latching 0, then A's poke re-latching between B's write and B's read)
resolves harmlessly: B proceeds to SetPixelFormat and is refused by R6.
*Tests:* 2a-acc; 2d-acc.

**R5 — Refused `0xFE8` (DescribePixelFormat): next read returns 0.**
Same gating as R4; the host answers `s->pixfmtMax = 0` (mesapt_mm.c:2382-2389,
read :390-392). The existing claim-word gate — describe already returns 0 when
`fifoptr[1]` is zero (mesapt_mm.c:2388) — is retained and extended to the OWNED
case. Guest result: `wglDescribePixelFormat` returns 0 cleanly
(wrapgl32.c:17421-17428). No `MGLWindowInit` may run for a refused describe
(today `MGLDescribePixelFormat` → `MGLWindowInit` creates the render window on
first touch, mglcntx_mingw.c:467-472 — the "dxdiag seizes the window" hazard,
`docs/guest-wrapper-notes.md` §4 DrvDescribePixelFormat row).
*Tests:* 2a-acc; T4-analog on the wrapper path.

**R6 — Refused `0xFE4` (SetPixelFormat) is the PRIMARY refusal point: readback ≠ `MESAGL_MAGIC`, and it MUST hold regardless of earlier races.**
While `OWNED`, a `0xFE4` request whose payload `ptm` (mesapt_mm.c:2396) does not
match the owner's bound VA, or whose shared ring reference count is greater than
one, is refused: `s->procRet = 0`, no `MGLSetPixelFormat`,
no `MGLWindowInit`, no crashRC warp (R14). While `RESERVED(T)`, a `0xFE4` request
is refused unless it presents `T` (R12). The wrapper maps any non-MAGIC readback to
FALSE and sets `currPixFmt = 0` (wrapgl32.c:17469-17471), which locally gates
`wglCreateContext` (`if (currPixFmt == 0) return 0;`, wrapgl32.c:17196-17197) —
the one complete, already-wired clean refusal chain (`docs/guest-wrapper-notes.md`
§2.5). Everything later is fire-and-forget on the legacy path — `0xFFC`/`0xFF8`
have no status readback and the wrapper fabricates `MESAGL_MAGIC`/TRUE
(wrapgl32.c:17200-17211, :17226-17245) — so refusal signalled after `0xFE4` is
invisible and corrupting (`docs/guest-wrapper-notes.md` §2.6). The existing grant
gate on the claim word (`fifoptr[1]? MESAGL_MAGIC:0`, mesapt_mm.c:2397-2398) is
retained.
*Tests:* 2a-acc (both orders: second app refused whether it Chooses first or Sets
first); 2d-acc (owner readback still `MESAGL_MAGIC`).

**R7 — `0xFDC` named-WGL refusals: ARB create answers 0, ARB choose answers 0.**
For a request in a refused context (state `OWNED`, requester not owner):
`wglCreateContextAttribsARB` MUST answer `argsp[0] = 0` — the guest checks it and
returns NULL (wrapgl32.c:16764-16778; host side mesapt_mm.c:2418-2432) — and
`wglChoosePixelFormatARB` MUST answer ret 0 (guest checks, wrapgl32.c:16740-16745).
Attribution residual (accepted): `0xFDC` payloads carry no identity, so the host
cannot distinguish the owner's own nested-context ARB calls from a ring-sharing
interloper's; the gate is therefore "no `0xFDC` context mutation unless a bound
owner exists and its context stack is consistent," and the interloper case is
covered by R21/R18 (it is out-of-contract before it reaches `0xFDC`). Foreign
`0xFDC` MUST NOT mutate `s->mglCntxWGL`/`s->mglContext` (today
`wglChoosePixelFormatARB` unconditionally overwrites `mglCntxWGL`,
mesapt_mm.c:2434-2437).
*Tests:* 2a-acc (app using ARB pixel-format path, e.g. wine9x flow).

**R8 — Attestation is per-session; a refused client cannot reset or poison the owner's flag.**
`s->mglCntxAtt` becomes a field of the session record. It is reset only when a NEW
pending session activates (replacing the global reset at mesapt_mm.c:2225), and it
is snapshotted into the owner record at binding (R12). `glDebugMessageInsertARB`
remains the only call accepted without a context (mesapt_mm.c:2244-2245, dispatch
:1612-1623, `ASSERT_ATTEST` mesagl_pfn.h:27-40) and continues to set the flag of
the session it is attributed to: pre-binding → the pending session; post-binding →
the owner (the owner's own denylist match must still zero its next create,
mesapt_mm.c:2280, :2426). Accepted residual: a ring-sharing interloper's attest
string post-binding is indistinguishable from the owner's (no identity on `0xFC0`);
the ICD closes this by never emitting attest strings before it is granted (R19).
*Tests:* 2a-acc repeatability (owner's attestation state survives a refused second
attach; denylisted title still denied after a refused interloper).

**R9 — Foreign/refused FIFO traffic: cursor containment holds while an owner context is live; in the no-context state the host guarantees owner safety, not the sender's survival.**
The host keeps today's containment exactly — and this contract scopes it honestly.
Both drain paths are gated on a live granted context (`s->mglContext &&
s->mglCntxCurrent`, mesapt_mm.c:2244-2245, :2264-2265). While that context exists
(the 2a-acc scenario: owner rendering, refused client queueing), MAGIC-poke drains
reset `fifoptr[0]` to `FIRST_FIFO` and clamp `dataptr[0]` (mesapt_mm.c:2187,
:2197-2200, :2264-2273), so cursors are re-based on every flush and `mdata[0]`
cannot walk off the mapping (`docs/guest-wrapper-notes.md` §5.7). With NO granted
context, a `0xFC0` sync only zeroes the first 8 bytes of the readback area
(mesapt_mm.c:2259-2262) and a MAGIC poke drains nothing — neither path resets
`fifoptr[0]`/`dataptr[0]`. Because the guest's `fifoAddData` appends unchecked
(wrapgl32.c:92-103 — DPRINTF-only overbound warning), a client that keeps queueing
in the no-context state is NOT bounded by the host: it can walk its own cursors
off the mapping and AV. That is owner-safe (entries are never executed without a
granted context) and consistent with R18's posture — the host guarantees the
owner's safety, not the queueing client's survival. The guest-side stop-appending
rule (R20 for the ICD) is therefore the PRIMARY belt; this host clamp is only the
second, and only while the host is alive with a granted context. A hardened host
MAY additionally clamp cursors on the no-context sync path as belt-and-suspenders;
the contract does not rely on it. The host MUST NOT execute drained entries
against the owner's context when the drain was triggered by a non-owner poke it
can identify as such (activation-refused session); for unattributable pokes the
drain-and-reset behavior stands.
*Tests:* 2a-acc ("no ring corruption warning" criterion); 2d-acc.

---

## 2. The read-only busy-probe (`0xFB4`) — required by the ICD at first GL touch

**R10 — Probe read: side-effect-free, repeatable, self-identifying.**
A read of `0xFB4` returns:
`(0x3D << 24) | (version << 20) | (state << 16) | (holder_token & 0xFFFF)`
with `version = 1`, `state ∈ {0 = FREE, 1 = RESERVED, 2 = OWNED}`, and
`holder_token` = the reserving/owning token (0 when FREE; 0 when owned by a legacy
client, which has no token). FREE on a v1 host therefore reads `0x3D100000`. The
read MUST have zero side effects — no `MGLWindowInit`, no fifo drain, no timer, no
TTL extension — and MUST be answerable in every device state including before any
activation (it consults only `MesaPTState` session fields). An old host returns 0
(unhandled read, mesapt_mm.c:398-400), which no v1 response can equal (tag byte
`0x3D` is nonzero) — this is the old-host detector (R24). This satisfies the
design requirement of a busy verdict answerable at `DrvDescribePixelFormat` time,
before any pixel format is handed out (`docs/icd-fallback-analysis.md` §3;
README.md "Phase 1").
*Tests:* T4 (probe consulted before any format granted — stub-log criterion);
2a-acc via ICD-era rerun; retro-host register assertions.

**R11 — Soft-reservation: probe write with token, 2000 ms lazy TTL, re-armable, cannot wedge the host.**
A write of token `T` (`0x0001 ≤ T ≤ 0xFFFF`; values ≥ `0x10000` are ignored — this
keeps `T` unequal to `MESAGL_MAGIC` and to the `0xFBC` command words, and an old
host ignores the write entirely with only a DPRINTF (mesapt_mm.c:2458-2463)) to
`0xFB4` behaves:
- state FREE → state := RESERVED(T), `expiry := now + MESA_PROBE_TTL`;
- state RESERVED(T) (same token) → re-arm `expiry`;
- state RESERVED(U≠T) unexpired, or OWNED → no-op (requester sees the holder via R10);
- state RESERVED(U≠T) expired → replace: RESERVED(T), fresh expiry.
Expiry is evaluated lazily at the next probe read/write or binding attempt — no
host timer, no allocation, no GL/window side effects. Therefore an ICD that probes,
reserves, and dies wedges nothing: after 2000 ms the next reader sees FREE. The
reservation exists to close the describe→create race identified in
`docs/icd-fallback-analysis.md` §3 ("host may become busy between 'advertised N
formats' and the owner's DrvCreateContext"); the recommended ICD cadence is: write
`T` in `DrvDescribePixelFormat` when the probe reads FREE, re-write on each
subsequent describe, and convert at `DrvSetPixelFormat` (R12). Token
recommendation: `T = PID & 0xFFFF`, substituting 1 for 0.
*Tests:* T4/T9 pattern (owner + latecomer); 2b-acc analog (reserve-then-kill →
next client binds after ≤2 s); T5 backstop (losing app of a near-simultaneous
start gets SetPixelFormat FALSE, not corruption — the accepted §3(b) outcome).

---

## 3. Ownership binding point

**R12 — Ownership binds at the first GRANTED `0xFE4` (SetPixelFormat), and nowhere else.**
Not at activation (R13), not at attestation (R8 — attestation only accumulates
into a session record), not at context creation (too late for both client flows:
the wrapper stops checking after `0xFE4` (wrapgl32.c:17200-17211), and an ICD that
advertised formats then failed create poisons the window — set-once ICD pixel
format, ReactOS wgl.c:836-840/405-422 as read in `docs/icd-fallback-analysis.md`
§3.3). On the granted `0xFE4` the host records: owner `ptm` VA (payload word 1,
mesapt_mm.c:2396), owner token (see below), attestation snapshot (R8), and a
generation counter. Grant rules by state:
- FREE → grant (legacy compatibility: an unreserved legacy client binds directly);
- RESERVED(T) → grant only if the request carries `T` in scratch word 12
  (byte offset 48 — the first word after `{format, ptm, PFD(40 bytes)}` that the
  legacy wrapper writes at wrapgl32.c:17463-17468); otherwise refuse per R6. The
  ICD always writes its token there; the legacy wrapper never writes word 12, so a
  legacy client racing a live reservation is refused (stale-scratch collision with
  a 16-bit token is possible and accepted: the failure direction is a refused ICD
  SetPixelFormat → app error box, the rare §3(b) outcome, never corruption);
- OWNED → grant only if payload `ptm` == owner `ptm` and the shared ring
  reference count is at most one (owner re-Set, today answered
  `ret=1` via the already-set format path, mglcntx_mingw.c:440-447); else refuse.
Interaction with the flows: the legacy wrapper reaches `0xFE4` via
DLL-load-activation → Choose/Describe → Set (`docs/guest-wrapper-notes.md` §1);
the ICD reaches it via probe(FREE) → reserve → `DrvDescribePixelFormat` advertising
its one synthesized format → `DrvSetPixelFormat` → activation-if-needed → `0xFE4`
with token (R23). Everything before the grant allocates no per-client host GL
state.
*Tests:* 2a-acc (order-independence: binding follows the first granted Set, not
process start order); T4/T6 (ICD: refusal at describe, binding never reached when
busy).

**R13 — Activation (`0xA0320`) does NOT bind ownership; it is a refcounted device-open.**
While FREE (or RESERVED), activation behaves exactly as today — signature check,
`InitMesaGL`, `MGLTmpContext`, readback `0x320a0` (mesapt_mm.c:2218-2228) — and may
run for multiple sequential clients (pre-binding state is only the discovery
context). While OWNED, a process that REACHES activation is refused per R1 — but a
concurrent secondary legacy loader never reaches it: its DllMain ring-sniff
(claim-word low-12-bit match, wrapgl32.c:72-73) takes the share path and returns
TRUE without ever poking `0xA0320` (wrapgl32.c:17542-17547), making it the R18
interloper, not an R1 refusal; activation runs only on a fresh mapping
(wrapgl32.c:17568). Rationale: binding at activation would
hand the passthrough to every process that merely loads the DLL (with a
system-wide ICD: dxdiag, screensavers at login — README.md "What a system-wide
guest ICD trips over"), and refusal-at-activation is app-fatal on the legacy path,
acceptable only as the busy answer, not as the arbitration point.
*Tests:* 2a-acc repeatability (owner exits → next app activates and binds without
VM restart); 2d-acc.

**R14 — The 3 s crashRC warp at `0xFE4` is owner-gated: a refused request never warps the ring claim or decrements the refcount.**
The historical warp ran on ANY `0xFE4` write after 3 s of owner silence, re-pointing
`fifoptr[1]` at the requester's `ptm` and decrementing the shared refcount
(mesapt_mm.c:2399-2407; `MESAGL_CRASH_RC` mglfuncs.h:38) — so a denied client's
SetPixelFormat can steal an idle-but-alive owner's ring claim
(`docs/guest-wrapper-notes.md` §5.6, §2.4: an owner is "silent" merely by not
swapping for 3 s — `crashRC` is bumped only on make-current (mesapt_mm.c:2318,
:2326), swap (:2359), glFinish/glFlush (:1922), and foreground `WM_ACTIVATE` via
`0xFD8` (:2454); an owner rendering without swap/flush and without a focus change
bumps nothing). Contract: the claim-word warp MAY execute only as part of a
successful GRANT under R12 while the state is FREE. Refcount patch-down is more
strict: it MAY execute only when the host carries an unrepaired legacy-crash
release recorded when the previous untokened owner expired. That pending repair
survives a failed succession attempt, is consumed by the next successful FREE
grant, and is cleared by a full ring reset. An ordinary FREE grant MUST NOT reduce
a refcount above one: those counts may represent live kit-v12 ring sharers, as the
reverse-launch-order regression proved. A refused request MUST leave `fifoptr[1]`,
`dataptr[1]`, the pending-repair state, and the owner record untouched. The current
implementation is at mesapt_mm.c:2904-3002 in host commit `9188331372`.
*Tests:* committed host fixtures `LSBO2RFAT` (reverse order) and `LBRD1WT`
(forward refusal plus crash succession); real-XP 2a-acc in both launch orders;
2b-acc ("an owner idle 10+ s is not reclaimed"; kill owner → next app attaches).

**R15 — `fx_window_destroy()` runs only for an owner (re)bind or owner teardown/reclaim — never as a side effect of a refused or foreign request.**
`mesa_prepare_window` destroys the existing window unconditionally on entry
(ui/libretro-3dfx.c:707-721 at :712 — before the activation-allowed check at
:713). Its only entry points are the three pixel-format pokes: `MGLWindowInit`
(mglcntx_mingw.c:269-282), called from Choose/Set/Describe (`0xFEC`/`0xFE4`/
`0xFE8`), which R4/R5/R6 gate in the OWNED state — so once an owner exists, no
refused/foreign request reaches the destroy. Precision: `MGLWindowInit` IS still
reachable pre-grant during FREE/RESERVED discovery (grant binds later, at the
first granted `0xFE4`, R12); that is harmless — no owner window exists to destroy
then. The load-bearing belt is therefore the destroy call itself, which MUST be
conditioned on "the caller is (re)binding or tearing down the owner".
Same rule for `mesa_release_window`
(ui/libretro-3dfx.c:723-727) via `MGLWndRelease`/`0xD0320` (R17).
*Tests:* 2a-acc ("owner's window untouched" criterion).

**R16 — Pixel-format config (`PPFD_CONFIG_DISPATCH`) is applied only while un-OWNED and frozen while OWNED.**
Today every `0xFEC`/`0xFE8` write re-runs the config dispatch (MSAA, scaler,
blit-flip, disp-timer — mesapt_mm.c:2362-2379, :2387), letting any process
reconfigure the owner's options (`docs/guest-wrapper-notes.md` §5.6). Contract:
config words are applied only while un-OWNED (they come from the binding client's
`wrapgl32.ext` and are identical across the owner's own repeated calls, so
freezing is invisible to the owner) and are re-armed on release. *Tests:* 2d-acc
(single client: options apply exactly as today); 2a-acc (second app with a
different `wrapgl32.ext` cannot flip the owner's scaler/MSAA).

**R17 — Release returns the state machine to FREE and re-arms claim-word hygiene.**
Ownership is released by: (a) owner's level-0 `wglDeleteContext`
(`0xFF4` with `val == MESAGL_MAGIC`, mesapt_mm.c:2331-2348); (b) owner's DLL-detach
teardown `0xFBC=0xD0320` (mesapt_mm.c:2230-2240; guest side wrapgl32.c:17587 —
poked only by the last-out process, refcount at :17577); (c) Step 2b host-driven
reclaim on owner death. On release the host MUST clear the owner record, return
the probe state to FREE, and — because a client that died after claiming leaves
`mfifo[1]` populated, which makes every subsequent loader's DllMain sniff report
"busy" until reboot (wrapgl32.c:72-73; `docs/guest-wrapper-notes.md` §5.8) — the
next GRANT must re-point/clear the claim word (the R14 warp, now owner-gated, is
that mechanism). Additionally, Step 2b reclaim MUST clear `mfifo[1]` (and re-base
the shared refcount `mdata[1]`) EAGERLY at reclaim time — the claim word lives in
host-mapped memory (`s->fifo_ptr`+4), so this is a plain host write; the `0xFF4`
level-0 delete never touches it (mesapt_mm.c:2331-2348) and the guest-side clear
runs only on a clean last-out detach (wrapgl32.c:17587-17589). Eager clearing
closes the owner-death → next-grant window in which a fresh legacy loader's sniff
would share against a dead owner's VA (the dying-granted-owner case of R21
depends on this); the next-GRANT warp remains the backstop for deaths 2b has not
yet detected. A `0xD0320` arriving while OWNED is honored as owner teardown
(the only in-protocol sender is the owner's own last-out detach); the hardened
host additionally logs it with the session generation for post-mortem.
*Tests:* 2b-acc; 2a-acc repeatability ("repeatable without VM restart").

---

## 4. Refused-client obligations

**R18 — A refused LEGACY wrapper client behaves as already wired; the host relies on exactly these reactions.**
(Verified behavior, normative for any shipped kit:) activation readback ≠
`0x320a0` → `DllMain` FALSE (wrapgl32.c:17568-17573); `0xFEC` read 0 →
`wglChoosePixelFormat` returns 0 (:17407-17408); `0xFE8` read 0 →
`wglDescribePixelFormat` returns 0 (:17421-17428); `0xFE4` read ≠ MAGIC →
`wglSetPixelFormat` FALSE and `currPixFmt = 0` (:17469-17471) → `wglCreateContext`
fails locally without touching the host (:17196-17197) → `wglGetPixelFormat` 0
(:17434-17443); ARB create/choose return NULL/0 on host answer 0
(:16764-16778, :16740-16745). A second
legacy process whose DllMain takes the ring-sniff share path (claim-word match →
TRUE without activation, wrapgl32.c:72-73, 17542-17547) executes with `pt[0]`
holding the OWNER's guest VA; every sync GL stub — including the
`glDebugMessageInsertARB` behind `OHST_DMESG`, which can fire inside
`wglSetPixelFormat`'s hook patchers before the `0xFE4` readback
(wrapgl32.c:17031, 17037, 17455-17456, 386-393; stub pattern :544, :551) —
dereferences that foreign VA. Real-XP `2a-acc` showed that kit v12 reaches
`0xFE4` without AV; its incremented `mdata[1]` refcount lets R6 distinguish it
even though both processes have the same `ptm`. The refusal returned before
context creation and the incumbent continued swapping. A wrapper-vNext hard
FALSE at the sniff remains a possible simplification, but is no longer required
for this observed kit-v12 path.
*Tests:* 2a-acc (both start orders); 2d-acc.

**R19 — A refused ICD reports ZERO formats from `DrvDescribePixelFormat` and touches nothing.**
When the probe (R10) reads anything other than FREE-or-mine (`state=FREE`, or
`state=RESERVED`/`OWNED` with `holder_token == own T`), or reads without the `0x3D`
tag (old host, R24), `DrvDescribePixelFormat` MUST return 0. The MS/ReactOS loader
then treats the DC as having no ICD formats and silently lands the app on generic
software GL — the only fully silent fallback path (ReactOS wgl.c:76-82: count
cached from the first describe; `docs/icd-fallback-analysis.md` §2 row 1, §3.1;
Mesa's shipping ICD refuses exactly there, `stw_pixelformat.c:357-358` as cited
in that doc). While refused the ICD MUST NOT: poke `0xA0320`, `0xFEC`, `0xFE8`,
`0xFE4`, or any MAGIC doorbell; claim the ring or write `mfifo[1]`/cursors; emit
attest strings (R8). Backstop: if the loader still reaches `DrvSetPixelFormat`
with an ICD-range index (hardcoded-index apps, or the R11 race lost),
`DrvSetPixelFormat` returns FALSE — app-visible but recoverable-by-retry, never
poisoning (ReactOS wgl.c:842-851; `docs/icd-fallback-analysis.md` §2 row 3, test
T5). `DrvValidateVersion` MUST NOT be used as the busy gate (only as the
device-present/ABI gate). Stock XP retries a FALSE validation on later GL touches,
but validation is still too early to participate in R10/R11 reservation and causes
repeated load/unload churn. *Tests:* T4 **passed** (zero formats → GDI Generic, no
context entry in the stub log); T3/T11 **passed** (validation retry + new-window
re-query); T5 **passed** (backstop leaves the window recoverable); T9 remains part
of 2a-acc.

**R20 — An ICD whose host dies mid-session goes INERT — it never fails, never falls back, never crashes.**
Once a window is bound to an ICD format, no software context is reachable on it
(set-once, ReactOS wgl.c:836-840; `docs/icd-fallback-analysis.md` §3 "Mid-session
refusal ... can never fall back"). Therefore on host death/refusal after grant
(activation readback lost, MMIO reads returning 0/garbage, device removed):
`gl*` entry points become no-ops; queries return zeros/cached strings (never
read stale `FRet`/`fbtm` — the wrapper's live-context stale-readback hazards,
`glMapBuffer` pointer from stale `FRet` and `glGetCompressedTexImage` memcpy of
up to 32 MB stale `fbtm`, are exactly what the ICD must NOT reproduce,
`docs/guest-wrapper-notes.md` §2.6); `DrvSwapBuffers` returns FALSE (frozen
window, crash-free — the T8/Mode-F documented UX); the DLL never unloads itself
and never raises. The ICD MUST also stop appending to the fifo the moment it is
inert so cursors cannot run off the mapping — this is the PRIMARY belt: R9's host
clamp is only the second, requires a live host with a granted context, and on a
truly dead host services nothing, so stop-appending is then the only protection.
*Tests:* T7 (no-op dispatch safety), T8 (mid-session death UX: black-but-alive,
clean exit).

**R21 — The ICD never uses the ring-sniff share path; a REFUSED (never-granted) ICD leaves no claim; a granted ICD's stale claim is cleaned host-side (R17).**
The ICD does not claim the ring at DLL load at all (attach work moves out of
`DllMain` — `docs/guest-wrapper-notes.md` §2.1): mapping happens lazily and the
claim word is written only as part of the R12 grant (host-side warp,
mesapt_mm.c:2404). Refused/pre-grant case: the ICD leaves `mfifo[1]`/`mdata[1]`
untouched — the warp never ran for it — so it can never wedge the DllMain sniff
of a later legacy client (`docs/guest-wrapper-notes.md` §3 "What a refused attach
must reset"). Granted case: the guarantee is NOT "leaves no stale claim" — at
grant the HOST wrote `mfifo[1]` = the ICD's VA; that word persists in host-backed
memory across process death, and no legacy path clears it on death (`0xFF4`
level-0 delete never touches `fifoptr[1]`, mesapt_mm.c:2331-2348), so a granted
ICD that dies mid-session leaves a stale claim exactly like a dead legacy owner
(`docs/guest-wrapper-notes.md` §5.8). That case is closed host-side, not ICD-side:
on clean teardown the ICD mirrors the wrapper's last-out sequence (`0xD0320` +
`mfifo[1] = 0`, wrapgl32.c:17587-17589); on death, Step 2b reclaim clears the
claim word EAGERLY per R17, with the owner-gated R14 warp on the next grant as
backstop. Accepted residual: a legacy loader starting inside the death →
2b-reclaim window still sniff-shares a dead VA (wrapgl32.c:72-73) — the same
hazard class, and the same answer, as R18's interloper.
*Tests:* T4 stub-log (no ring writes when refused); 2b-acc analog with an ICD
owner (kill ICD owner → later legacy client attaches cleanly, no reboot); mixed
wrapper+ICD guest smoke in the Step 4 acceptance.

---

## 5. Compatibility matrix

| | legacy wrapper (kit v12) | new ICD (qxpicd) |
|---|---|---|
| **hardened host** | R22: single-client flow byte-identical; multi-client → R1-R18 | R23: probe/reserve/bind flow |
| **old host** (≤ `73a050b026`) | today's behavior (out of scope) | R24: fail-safe inert |

**R22 — Hardened host × legacy wrapper, single client: the wire behavior is unchanged.**
For one client from activation through teardown the host emits exactly today's
values: `0xFBC` → `0x320a0`; `0xFB8` 0→1 via `cwnd_mesagl`
(mglcntx_mingw.c:188-195); `0xFEC/0xFE8` real format/count; `0xFE4` →
`MESAGL_MAGIC`; `0xFF0/0xFF4/0xFF8/0xFFC/0xFDC/0xFD8/0xFC0` semantics untouched.
New refusal behavior triggers only when the requester is not the owner, or on the
new `0xFB4` register the wrapper never touches. The R14 claim warp remains available
on a successful FREE grant and is a no-op when the claim already names that owner's
normalized `ptm`. Refcount patch-down is no longer a general FREE-grant side effect:
it requires `legacy_repair_pending`, set only by a crash-expired legacy-owner
release. A normal one-client activation/bind therefore cannot change its refcount;
crash succession retains the historical one-count repair for the dead claimant.
*Tests:* 2d-acc (full EmuVR checklist — the regression guard for this clause).

**R23 — Hardened host × new ICD: the granted flow.**
`DrvValidateVersion` (or first describe): map ranges; read `0xFB4`; require tag
`0x3D`, version 1 → device usable, return TRUE (device absent / no tag → R24).
`DrvDescribePixelFormat`: probe; FREE-or-mine → write token T (reserve/re-arm,
R11) and advertise exactly one accelerated format (32-bit RGBA double-buffered,
no `PFD_GENERIC_FORMAT`), synthesized guest-side — the host is NOT asked, so
enumeration allocates nothing and cannot seize the window (fixes the
`MGLDescribePixelFormat`→`MGLWindowInit` hazard, mglcntx_mingw.c:471); busy →
0 formats (R19). `DrvSetPixelFormat`: poke `0xA0320` if this process has not
activated (allowed: state RESERVED(T) is not OWNED, R13); then `0xFE4` with token
in scratch word 12 → grant binds ownership (R12). `DrvCreateContext`/
`DrvSetContext`: proceed on the fifo client exactly as the wrapper does, but
honest — create requires a host ack (the `0xFDC` ARB-create path, which returns a
real status, is the template, mesapt_mm.c:2418-2432) and never fabricates handles
(`docs/guest-wrapper-notes.md` §4 DrvCreateContext row). The 16 mandatory
exports + `DrvValidateVersion`/`DrvSetCallbackProcs`/`DrvPresentBuffers` are all
present (ReactOS icdload.c:303-327, :281-300; mesa3d.def per
`docs/icd-fallback-analysis.md` §2). *Tests:* granted-flow acceptance for qxpicd,
including the T1-analog happy path.

**R24 — Old host × new ICD fails SAFE: no probe tag ⇒ permanently inert for that process.**
Detection: `0xFB4` read returns 0 on any host predating this contract (unhandled
MMIO read, mesapt_mm.c:398-400); reads also return 0 when the device is absent
but the mapping succeeded, and the mapping itself fails when the device or the
mapping driver (`qxpmap.sys`; MAPMEM before the `qxpmap` cutover) is
missing. In every no-tag case the ICD reports zero formats forever
(process-lifetime latch is permitted here — there is nothing to re-query) and
goes inert per R20 discipline. It MUST NOT fall back to the legacy attach
sequence: poking `0xA0320` on a host with no arbitration bypasses the ownership,
window-destruction, and ring-claim protections specified above.
Probe writes on an old host are harmless (default write path only logs,
mesapt_mm.c:2458-2463) but the ICD has no reason to issue them after a no-tag
read. *Tests:* T0-analog on R1 rig with qxpicd against an unpatched core: GDI
Generic everywhere, no error UI, no VM instability.

**R25 — Attach signature: the git-hash check is retained for the wrapper and bypassed by a versioned handshake for the ICD.**
The legacy gate — guest writes the 8-byte `rev_` commit stamp to the top of
`fbtm` (wrapgl32.c:17567), host memcmps its own `rev_` (mesapt_mm.c:2222,
mglfuncs.h:49-50) — stays verbatim for kit clients (2d-acc). It couples guest
and host builds per-commit (`docs/guest-wrapper-notes.md` build notes), which is
untenable for a system-wide ICD serviced independently of the core. Contract: the
hardened host's activation gate accepts EITHER the exact `rev_` OR the fixed
ICD signature `"QXPICD\0" + abi` where `abi` (1 byte) must equal the version
field the host advertises in the probe tag (R10). The ICD learns compatibility
from the probe before activating; a host that answers version N only accepts
`abi == N`. Protocol changes bump the probe version, never the string.
*Tests:* 2d-acc (wrapper signature unchanged); Step 4 acceptance (ICD binds on a
host built from a different commit than the ICD).

---

## 6. Open items — the contract is NOT FROZEN until these close

**R26 — Freeze condition.** O1 is closed by the Step 2c implementation: every Mesa
and Glide MMIO write synchronously completes on the main-loop thread, and Mesa stores
the `0xFBC`, `0xFC0`, `0xFEC`, `0xFE8`, `0xFE4`, and `0xFE0` replies in per-vCPU
latches. A poke→readback instruction pair is not globally atomic—another vCPU may run
between the instructions—but its result can no longer be overwritten or consumed by
that other vCPU. The submitting guest thread MUST also remain on one vCPU across
the pair: real XP migrated QXPICD between `0xFE4` write/read after the host had
granted it, causing a read of the other vCPU's empty latch. Kit v12 pins its whole
process; QXPICD instead temporarily pins each acknowledgement pair and restores
thread affinity immediately. All future synchronous FIFO readbacks use the same
helper. The deliberately concurrent two-vCPU submitter fixture checks reply
isolation under both `tcg,thread=multi` and WHPX with golden `LICWPFT`:
per-vCPU replies remain isolated, every contention resolves as a grant or clean
refusal, and the session returns to FREE. O6/2a-acc is now closed in both launch
orders on real XP:
the kit-v12 ring-sniff interloper reached `0xFE4`, was refused using the honest
shared-ring refcount, and the owner continued rendering for more than 60 seconds.
The reverse-order run used `glowner.exe revfirst`, reported the second-loaded
process owning at 32 FPS and the first-loaded process refused at refcount 2, then
tore down cleanly (`9311ee7`, 2026-07-14). Host fixtures at `869b6ca16a` preserve
both reverse refusal and forward crash-expiry succession; the reverse fixture's
release/reacquire tail and the real-XP run also close owner-release/reacquisition
repetition. R1-R25 remain NOT FROZEN until the R21 owner-death → 2b-reclaim window
items and the EmuVR kit-v12 regression close.

Closed on stock XP in v0.3: (O2/T3) validation FALSE is retried rather than cached;
(O3/T4/T11) zero-format state is re-queried and a second window in the same process
can become ICD-backed after mode changes; (O4/T6) context-time failure poisons the
window; (O5/T5) failed `DrvSetPixelFormat` before commitment leaves it recoverable.

---

## 7. Verification (adversarial pass, 2026-07-10 — guest-behavior + host-implementability lenses; disputed code re-read and arbitrated)

Verdicts: **confirmed** = both lenses positively verified against code, no doc change;
**corrected** = clause substance held, citation/rationale fixed; **refuted→rewritten** =
claim overstated, clause re-scoped; **amended** = changed downstream of another verdict.

| id | verdict | correction applied (arbitrated against code) |
|---|---|---|
| R1 | confirmed | — (0xA0320/0x320a0 path wrapgl32.c:17568-17573; side effects mesapt_mm.c:2221-2226; single-shot readback, no spin) |
| R3 | confirmed | — (single reader of `0xFB8`, wrapgl32.c:17405-17406; `ready` dead after loop; `ptm` volatile) |
| R4 | confirmed | — (verbatim `0xFEC` readback wrapgl32.c:17407-17408; dispatch chain mesapt_mm.c:2376-2380) |
| R5 | confirmed | — (no spin in describe; `fifoptr[1]` gate mesapt_mm.c:2388 retained) |
| R6 | confirmed | — (ptm payload mesapt_mm.c:2396; currPixFmt gate wrapgl32.c:17196-17197; 0xFFC/0xFF8 have no readback) |
| R7 | confirmed | — (WGL_FUNCP_RET=argsp[0] wrapgl32.c:16640; unconditional mglCntxWGL overwrite mesapt_mm.c:2434-2437 as stated) |
| R8 | confirmed | — (attest exception mesapt_mm.c:2244-2245; global reset :2225; residual honestly scoped) |
| R9 | **refuted→rewritten** | Containment claim was overstated: both drain paths are context-gated (mesapt_mm.c:2244-2245, :2264-2265); the no-context sync path (:2259-2262) zeroes 8 readback bytes and does NOT reset `fifoptr[0]`/`dataptr[0]`; guest `fifoAddData` appends unchecked (wrapgl32.c:92-103). Re-scoped: cursor bound holds only while a granted owner context is live; no-context queueing is unbounded by the host but owner-safe (entries never executed) — sender's own AV risk per R18 posture; R20 stop-appending promoted to primary belt; host MAY clamp no-context path (not relied on). |
| R10 | confirmed | — (0xFB4 absent from mesapt_read/write; default read 0 mesapt_mm.c:375, :398-400; 0x3D tag ≠ 0) |
| R11 | confirmed | — (token < MESAGL_MAGIC 0x5b5eb5e5; old-host write falls to warn-DPRINTF mesapt_mm.c:2462-2463) |
| R12 | confirmed | — (word 12 = byte 48 free: guest writes {fmt,ptm,PFD(40)} wrapgl32.c:17463-17468; ALIGNED(4)=8; owner re-Set `ret=1` citation mglcntx_mingw.c:440-447 verified correct in the qemu-libretro tree — `GetPixelFormat` :440, `ret = 1` :446-447; the disputing :430 reference was the other tree's numbering) |
| R13 | **corrected** | "While OWNED it is refused per R1" was imprecise: a CONCURRENT secondary legacy loader never reaches activation — the DllMain ring-sniff (wrapgl32.c:72-73) returns TRUE via the share path (:17542-17547) without poking 0xA0320; it is the R18 interloper. Refusal-per-R1 applies only to a process that reaches activation (fresh mapping, :17568). Clause text now says so. |
| R14 | **corrected and tightened** | The claim warp remains successful-FREE-grant-only, but refcount patch-down now additionally requires a pending crash-expired legacy-owner repair. Host fixtures prove reverse-order live sharers retain refcount 2 (`LSBO2RFAT`) while forward crash succession repairs exactly once and warps the claim (`LBRD1WT`). Refusal leaves the claim, refcount, pending repair, and owner record untouched. |
| R15 | **corrected** | "reachable only on behalf of a session being granted" was loose: R4/R5/R6 gate only in the OWNED state; `MGLWindowInit` remains reachable pre-grant in FREE/RESERVED discovery (grant binds at first granted 0xFE4, R12) — harmless, no owner window exists. Rationale rewritten; the owner-conditioned destroy call (libretro-3dfx.c:712, before the :713 check) is stated as the load-bearing belt. Call-site census (3× GLWINDOW_INIT, 1× GLWINDOW_FINI) confirmed exhaustive. |
| R16 | confirmed (header tightened) | Header "latched at the owner's binding" → "applied only while un-OWNED, frozen while OWNED" (dispatch physically runs pre-0xFE4, mesapt_mm.c:2376-2379, :2387; body already said so). |
| R17 | **amended** (downstream of R21) | Added: Step 2b reclaim MUST clear `mfifo[1]`/re-base `mdata[1]` EAGERLY at reclaim time (plain host write to `s->fifo_ptr`; 0xFF4 delete never touches fifoptr[1], mesapt_mm.c:2331-2348; guest clear only on clean last-out detach, wrapgl32.c:17587-17589). Next-GRANT warp demoted to backstop for undetected deaths. |
| R18 | confirmed | — (full refusal chain and ring-sniff/OHST_DMESG pre-readback limitation verified at cited lines) |
| R19 | confirmed | — ("only fully silent fallback path" read as: only per-window silent refusal point available to a loaded ICD; load-stage escapes are a different phase) |
| R20 | **amended** (downstream of R9) | "R9's host clamp is the second belt" qualified: the clamp requires a live host with a granted context; on a dead host stop-appending is the ONLY protection. |
| R21 | **refuted→rewritten** | "a refused or dying ICD leaves `mfifo[1]`/`mdata[1]` untouched" was false for the granted-then-dying case: the R12 grant warp HOST-writes `mfifo[1]` = ICD VA (mesapt_mm.c:2404); it persists across process death (0xFF4 delete leaves it, :2331-2348) and its low-12-bit 0xFC0 signature always matches the legacy sniff (wrapgl32.c:72-73) — the §5.8 wedge. Re-scoped: untouched-claim guarantee holds only refused/pre-grant; granted case closed by clean-teardown clear (mirroring wrapgl32.c:17587-17589), eager 2b-reclaim clear (R17, new), next-grant warp backstop; death→reclaim window accepted as R18-class residual. |
| R22 | confirmed | — (`0xFB4` is absent from the wrapper register census; an ordinary single-client FREE grant has no pending legacy repair and cannot patch its refcount; crash succession retains the established repair path) |
| R23 | confirmed | — (0xFDC ARB-create real-ack template mesapt_mm.c:2418-2432; word-12 token slot free; synthesized describe never pokes 0xFE8) |
| R24 | confirmed | — (old-host 0xFB4 read = 0; probe writes fall to warn-DPRINTF; raw 0 must not be parsed as FREE — tag gate first) |
| R25 | confirmed | — (rev_ stamp write wrapgl32.c:17567 / memcmp mesapt_mm.c:2222; "QXPICD\0"+abi is exactly 8 bytes = the ALIGNED(1) window) |
| R26 | confirmed | — (procRet clear-on-read mesapt_mm.c:393-397 applies to 0xFE4/0xFE0 only; -smp 1 hard-stop libretro-3dfx.c:268-285) |

---

## Load-bearing clauses index

| id | one-line summary |
|---|---|
| R1 | Busy activation: `0xA0320` side-effect-free, `0xFBC` reads `MESA_BUSY`, wrapper DllMain fails clean |
| R3 | `0xFB8` never reads 0 for a refused/owned state — kills the ChoosePixelFormat infinite spin |
| R4 | Refused `0xFEC`: no window/config side effects, reads back 0 → ChoosePixelFormat fails clean |
| R5 | Refused `0xFE8`: describe answers 0 formats, no window creation |
| R6 | Refused `0xFE4` ≠ MAGIC is the PRIMARY refusal point; must hold even when earlier registers raced |
| R7 | Refused `0xFDC`: ARB create answers 0, ARB choose answers 0, no owner-state mutation |
| R8 | Attestation is per-session; refused clients can't reset or poison the owner's flag |
| R9 | Foreign fifo traffic: cursor containment while an owner context is live; no-context queueing is the sender's own AV risk (owner-safe), guest stop-appending (R20) is the primary belt |
| R10 | `0xFB4` read-only busy-probe: `0x3D`-tagged {version,state,token}, zero side effects, repeatable |
| R11 | `0xFB4` write = soft-reservation, token 1..0xFFFF, 2000 ms lazy TTL, re-armable, can't wedge host |
| R12 | Ownership binds at first GRANTED `0xFE4`; RESERVED converts via token in scratch word 12 |
| R13 | Activation never binds ownership — refcounted device-open only |
| R14 | crashRC claim warp is FREE-grant-gated; refcount repair additionally requires a pending crash-expired legacy owner |
| R15 | `fx_window_destroy` only on owner (re)bind/teardown — never from a refused request |
| R16 | `PPFD_CONFIG_DISPATCH` frozen while OWNED — non-owners can't reconfigure the owner |
| R17 | Release (`0xFF4` lvl-0 / `0xD0320` / 2b reclaim) → FREE, owner record cleared, claim-word hygiene incl. EAGER `mfifo[1]` clear at 2b reclaim |
| R18 | Refused legacy wrapper: DllMain FALSE / SPF FALSE chain as wired; ring-sniff interloper best-effort |
| R19 | Refused ICD: ZERO formats from DrvDescribePixelFormat, touches nothing, SPF-FALSE backstop |
| R20 | ICD with dead host goes INERT (no-op GL, FALSE swaps) — never fails, falls back, or crashes |
| R21 | ICD never ring-sniff-shares; refused ICD leaves no claim; dying GRANTED ICD's stale claim is cleaned by R17 host hygiene |
| R22 | Hardened host × single legacy client: wire-identical (2d regression guard) |
| R23 | Hardened host × ICD: probe → reserve → synthesized-format describe → tokened `0xFE4` bind |
| R24 | Old host × ICD: no `0x3D` tag ⇒ permanently inert; never legacy-attach on an unarbitrated host |
| R25 | Signature: wrapper keeps git-hash memcmp; ICD uses `"QXPICD\0"+abi` matched to probe version |
| R26 | Real-XP T3/T4/T5/T6/T11, concurrent two-vCPU reply isolation, both kit-v12 launch-order refusals, release/reacquire, affinity, and QXPICD reclaim/idle pass; R21 owner-death-window items and EmuVR remain |
