# Design: asynchronous / pipelined Direct3D draw submission

Status: proposed (performance-led milestone, high risk). Correctness is a hard
gate; this note is the reviewable plan before any code.

## Problem (measured)

Every mesa MMIO doorbell blocks the guest vCPU until the QEMU main thread runs
the GL work. Concretely, `mesapt_write` (vCPU thread) calls
`qemu_fx_run_on_main_thread`, which enqueues a bottom-half on the main thread and
`qemu_cond_wait_bql`s until it completes (`ui/libretro-3dfx.c`). The 0xFC0
draw-flush doorbell therefore serializes: the vCPU is idle while the main thread
executes `doMesaFunc` (the real `glDrawElements` etc.), and the main thread is
idle while the vCPU builds the next batch.

Evidence (2026-07-19): Max Payne and UT2003 both run the emulator at only
~1.1-1.3 of 32 host cores, split across a vCPU thread (~55%) and the main/GL
thread (~40%) that ping-pong. See
`artifacts/runs/20260719-d3d-submission-serialization-finding/summary.json`.

## Goal

Guiding principle: use as much native host CPU and native host GPU as possible.
Today the guest vCPU and the host GL/main thread ALTERNATE, so at any instant
only one is working -- the host CPU is under-utilized (~1.1/32 cores) and the GPU
is starved during vCPU-only phases. Overlap the vCPU and main/GL thread so the
guest builds batch N+1 on one core while the host drives the GPU on another,
without changing observable rendering, ordering, Reset, teardown, multi-context
behavior, or the single-GL-context model.

Complementary lever (already implemented, not part of this milestone): native
GPU vertex transform via the hardware-T&L path (we advertise
`D3DDEVCAPS_HWTRANSFORMANDLIGHT`; hardware-VP and VS1.1 titles transform on the
host GPU). Software-VP titles (e.g. Max Payne sends pre-transformed XYZRHW) keep
their transform on the emulated CPU by their own device choice, which the driver
cannot change without a per-game shim; for those, this CPU-parallelism milestone
is the honest lever.

## Core invariant (must hold)

All queued asynchronous draw work must be fully executed on the host, in
submission order, before ANY observation of GL results or presentation:
- result reads (0xFA4): glGetError, capability/readback calls;
- present/swap (0xFF0 wglSwapBuffers);
- context ops (0xFD4/0xFFC/0xFF8), Reset, mode change, teardown, session
  release, DLL load/unload;
- any readback (glReadPixels) or output-returning call.

## Why the existing dispatch queue makes this safe

Every doorbell routes through the SAME single main-thread BH queue in FIFO
order. If draw-only flushes are posted asynchronously (enqueue, do not wait) and
every other doorbell stays synchronous (enqueue and wait), then a synchronous
op cannot complete until all previously-posted async draws ahead of it in the
FIFO queue have run. The queue itself is the ordering barrier; no extra fences
are required for correctness of ordering.

## The one race to solve: shared FIFO reuse

The guest writes commands into the shared `s->fifo_ptr` RAM; the host reads it
during processing. Today the vCPU waits for the host to drain and reset the FIFO
indices before refilling. If the draw doorbell returns immediately, the vCPU
would overwrite the FIFO while the host still needs it.

Resolution: snapshot-on-submit. On an async draw flush, copy the live FIFO
command/data region into a heap-owned snapshot buffer on the vCPU thread, reset
the shared FIFO indices so the guest may refill immediately, then post the
snapshot to the main thread for GL execution without waiting. The main thread
executes from the snapshot, never from the live FIFO. The snapshot copy (<=~88
KiB) is far cheaper than GL execution, so the vCPU unblocks early and overlaps.

## Backpressure (bounded memory, bounded latency)

Allow at most N in-flight async snapshots (start N=2, i.e. one rendering while
one is being built). If a new async flush would exceed N, it blocks (waits for
the main thread to retire the oldest snapshot) — degrading gracefully to the
current synchronous behavior under pressure. This bounds host memory to N
snapshots and prevents unbounded queue growth or latency.

## Threading reality (scope correction, then de-risk)

The host GL context is thread-affine (`wglMakeCurrent` binds it to one thread),
and all GL runs on the main-loop dispatch thread via
`qemu_fx_run_on_main_thread` (a bottom-half + cond-wait on which the vCPU
blocks). A first read suggested a dedicated render thread with context
migration; on inspection that is NOT required. The main-loop dispatch thread is
already separate from the vCPU thread and already owns the context, so overlap
is available between the vCPU (which only touches the shared FIFO RAM, never GL)
and the main loop (which does the GL). The vCPU can snapshot the FIFO and let the
main loop render, without moving the context.

The one real constraint: the GL processing reads the live shared region through a
single base pointer (`s->hshm`, with `s->fifo_ptr`/data/arg pointers derived from
it). Async draw processing must therefore rebase that base to a per-item SNAPSHOT
buffer so the guest can refill the live region concurrently. Because only the
main-loop thread touches `s->FEnum/arg/parg` and it processes one queued item at a
time, rebasing is safe without new locks. Only pure draw-flush commands (which
reference FIFO+data, never the FBTM/output regions) are eligible for async; every
result/output/present/context command stays synchronous and drains the queue.

## Host changes (`hw/mesa/mesapt_mm.c`, `ui/libretro-3dfx.c`)

1. Add `qemu_fx_post_to_main_thread(fn, heap_opaque)`: enqueue a heap-owned
   request the main thread runs and then frees; caller does not wait. Reuses the
   existing dispatch queue/BH so async and sync work stay FIFO-ordered.
2. Add an async draw-flush entry point: snapshot FIFO+data, reset indices,
   enforce the N-in-flight bound, post the snapshot for GL execution.
3. Draw-only GL_FLUSH from the guest becomes async; every other doorbell stays
   synchronous and therefore drains the async queue first.
4. On session release/reset/teardown/owner-timer reclaim, drain (join) all
   in-flight snapshots before mutating session/context state.

## Guest changes (`guest/xpdm/qxpmp/qxpmp.c`, transport header)

1. Draw-only FIFO flushes (`qxp_d3d_reserve_draw_fifo`) use the async doorbell.
2. Every result-returning call, present, Reset, target/context change, and
   teardown continues to use the synchronous doorbell (natural drain).
3. Bump the transport ABI to /4; the paired host accepts /1../4, with /4 adding
   the async draw-flush doorbell. Older hosts and non-async paths are unchanged.

## Validation plan

- Paired host C contract test: async ordering (draws before a following result
  read/present execute in order), snapshot isolation (guest refill during
  in-flight render does not corrupt output), backpressure bound, and drain on
  teardown/reset.
- Guest miniport: unchanged rendering counters; drain-before-sync unit coverage.
- Full seven-mode T2 with clean handback (exact D3D8/D3D9 visual, Reset,
  multi-context, succession) — correctness gate.
- Matched capture-free performance: Max Payne and UT2003 (and an OpenGL title as
  a non-regression check), profiled active-vs-gap and host-core utilization
  before/after, promoted only on a real, matched improvement with no regression.
- Bounded read-only concurrency/ordering review of the frozen diff.

## Risk and rollback

Highest-risk change in the project: host concurrency, single GL context,
observable-state ordering. Rollback is the signed `.254` package and the
paired host at `ab240aecc2`; the async doorbell is additive (ABI /4) so reverting
the guest to synchronous flushes restores exact prior behavior. Promote only if
correctness gates pass and the matched performance gain is real; otherwise reject
and keep `.254`.

## Explicit non-goals

No per-client rings, no multi-context GPU rework, no change to the exclusive-
owner model, no change to rendering semantics, and no game-specific behavior.
