# Windows XP acceptance harness

## Local frontend and core paths

All checkout-relative defaults are derived from the script location, not the
current directory. Explicit executable parameters always take precedence.

- The Python diagnostic cycles take `--retroarch`, falling back to
  `XPD_RETROARCH`, or `--retro-host`, falling back to `XPD_RETRO_HOST`.
  The selected frontend requires one of those sources; no installation path
  is assumed.
- `--core` (including on `install-xpdm-cycle.py`) defaults to `XPD_CORE`, then
  the paired checkout's `build-win64/libqemu_libretro.dll`. As in
  `guest/paired-core.mk`, use `core/` when `core/hw/mesa/mglfunci.h` exists,
  otherwise use the sibling `../qemu-libretro-d3d9`.
- `publish-xp-acceptance-inputs.ps1` and `invoke-xp-candidate-cycle.ps1` take
  `-RetroHostPath`, falling back to `XPD_RETRO_HOST`; omitting both is an
  error. Their `-CorePath` falls back to `XPD_CORE`, then the same paired-core
  build path. `test-frozen-inputs.ps1` uses these environment/default paths
  for its optional local publisher checks.
- Non-dry `invoke-xp-test-cycle.ps1` and standalone XPDM validation retain
  their frozen-input contract: explicit host/core paths win, then the selected
  manifest's paths. Environment values never override a frozen manifest.
  Test-cycle `-DryRun`, or the standalone runner without frozen inputs, uses
  `XPD_RETRO_HOST` and `XPD_CORE`/the paired-core build path instead; it still
  requires an explicit host path or `XPD_RETRO_HOST`.
- `RETRO_HOST_SYSTEM_DIR` is inherited unchanged by the frontends. Set it to
  your libretro system/firmware directory when needed. If unset, the harness
  leaves the frontend's own behavior unchanged; it never searches another
  application's installation.

The public source snapshot does not include historical, machine-bound frozen
input manifests. Publish a new local generation with
`publish-xp-acceptance-inputs.ps1 -OutputPath <new-manifest.json>`, then pass that
path as `-FrozenInputsPath` to validation. Publishing requires the real signed
package, acceptance ISO, frontend, and paired core; it is not part of a
validation run. Historical byte-preservation tests still require their original
manifests and ISO assets and cannot be satisfied by a newly published generation.

## Legacy POP diagnostic cycles

The three Python cycles in this section predate the canonical
`invoke-xp-test-cycle.ps1` lease, preflight, frozen-input, and handback
contract. They remain checked in only because current diagnostic history cites
their exact behavior. Do not use them for new evidence or mutate a preserved
target through them; migrate the required oracle into the canonical lifecycle
before the next run.

For the dedicated POP:SoT games disk, `pop-menu-cycle.py` automates the manual
loop: wait for Explorer, launch the `Play Prince...` desktop shortcut, accept
the safe-mode prompt, send Space every two seconds for 170 seconds, and sample
the resulting frame burst. It refuses to run beside another RetroArch process,
uses QMP on port 49055 for input and shutdown, and writes frames, logs, and
`verdict.json` under a timestamped `artifacts/runs` directory:

```powershell
python .\qemu\harness\pop-menu-cycle.py --label pop-present-check
```

The newest `xpdriver-xpdm-games-pop-*-test.qemu_cmd_line` is selected unless
`--cmdline` names another file. Exit 0 means `ANIMATING`; exit 2 means `STATIC`
or `BLACK`; exit 3 means the desktop/game setup did not reach POP; exit 4 means
harness infrastructure failed. Because this libretro build does not expose QMP
`screendump`, the core provides visual samples through its environment-gated
`XPD_FRAME_RING` test-harness export. Normal runs leave the export disabled.
The default gated ring has eight slots and a 500 ms interval. Bounded visual
oracles can set `XPD_FRAME_RING_SLOTS` (1-4096) and
`XPD_FRAME_RING_INTERVAL_US` (0-10000000); all values require complete decimal
syntax and `XPD_FRAME_RING` must equal `1`. Consecutive Max Payne captures can
be checked for the proven isolated black-frame cadence with:

```powershell
python .\qemu\harness\frame-ring-black-cadence.py .\qemu\winxp `
  --max-isolated-black 0 --output frame-ring-cadence.json
```

The cadence gate requires at least 256 numeric ring frames spanning one second,
rejects ambiguous tied timestamps and malformed BMPs, and distinguishes a
contiguous loading transition from isolated `V-B-V` black publications.

`install-xpdm-cycle.py` installs the newest staged POP package from the
acceptance ISO without guest interaction. It runs `D:\XPDM\qxpsetup.exe`,
verifies the exact serial DriverVer, handles the libretro core unload used for
XP reboot by relaunching the install cmdline with `D:` still mounted, waits for
the post-reboot desktop, then shuts XP down:

```powershell
python .\qemu\harness\install-xpdm-cycle.py
```

`display-return-cycle.py` is the regression gate for display ownership. The
driver publishes frames to the host and the host shows whichever span was
published last, so a D3D session that outlives its owner leaves the screen
frozen on a dead flip-chain member: Windows keeps painting the desktop into
video memory and none of it is ever seen again. The cycle boots to the
desktop, starts the fullscreen game, waits for two consecutive frames of real
game output, logs off to destroy the game's DirectDraw objects, and requires
the display to leave the owner's last frame **and** land on a live shell
frame. `RETURNED` (exit 0) passes; `FROZEN` (exit 2) is the defect:

```powershell
python .\qemu\harness\display-return-cycle.py `
  --cmdline xpdriver-abi64-prince-r338.qemu_cmd_line `
  --frontend retro-host --retro-host <retro_host.exe>
```

It needs a freshly installed overlay: the cycle logs off, so the disk it
leaves behind boots to the logon screen rather than the desktop.

These scripts operate on the persistent games disk and are not canonical VM
owners. Never launch them while any other VM owns that disk.

## Canonical evidence-producing invocation

For reviewed acceptance, diagnostic, and performance runs, invoke the harness
through `invoke-xp-acceptance.ps1`. The launcher gives the run a stable identity,
passes every runner option as a separate argument, captures the complete child
console, and finalizes one self-describing artifact bundle after the child has
initialized and published its run result. Run it from the repository root in
Windows PowerShell 5.1:

```powershell
$runId = '20260717-247-copy-profile-t2-r1'
$artifactDirectory = Join-Path `
  (Resolve-Path '.\artifacts\runs').Path $runId
# Replace this with the fresh, offline content profile assigned by the handoff.
$contentPath = 'C:\absolute\path\to\fresh-xpdm-target.qemu_cmd_line'
$runnerParameters = @{
  Xpdm = $true
  BootOnly = $true
  D3dVs11 = $true
  ContentPath = (Resolve-Path $contentPath).Path
  TimeoutSeconds = 300
}

& .\qemu\harness\invoke-xp-acceptance.ps1 `
  -ArtifactDirectory $artifactDirectory `
  -RunId $runId `
  -TaskId 'xpdriver-247-copy-profile' `
  -Tier T2 `
  -Diagnostic `
  -RunnerParameters $runnerParameters
if ($LASTEXITCODE -ne 0) {
  throw "Acceptance run failed with exit code $LASTEXITCODE"
}
```

Use a run ID containing 1-64 letters, digits, dots, underscores, or hyphens.
The artifact parent must already exist. The run directory must be absent or
empty; reusing a nonempty directory fails closed. `RunnerParameters` accepts
only parameters declared by `run-xp-acceptance.ps1`. The launcher rejects
unknown keys and owns `ArtifactDirectory`, `RunId`, `TaskId`, `Tier`, and
`Diagnostic`, so those five keys must not appear in the hashtable. Boolean true
values become switches, false values are omitted, and all other non-null values
are emitted as separate name/value arguments in sorted name order.

Prefer a checked script or here-block for generated invocations containing a
PowerShell hashtable. If another process must generate a single command line,
use PowerShell's encoded-command mechanism instead of embedding a hashtable in
a multiply quoted raw `-Command` string.

Wait for the launcher process to exit. Its `AcceptanceLaunch` line reports the
lifecycle `phase`, whether a child started, its exit code when applicable, and
whether bundle finalization succeeded. For a finalized run, that line is written
after `console.log` and the manifest are finalized, so it is not part of
`console.log`. Failure evidence has three explicit phases:

- launcher validation before child launch (invalid run identity, unresolved or
  unparsable runner, unknown/reserved key, or missing artifact parent) throws
  after reporting `phase=pre-child` and before creating either a bundle or a
  staging console;
- a launched child that exits before initializing its artifact directory/result
  reports `phase=finalization-failed` and `finalized=False`; the launcher
  propagates the child exit and retains only the sibling staging console for
  recovery; and
- a child that publishes `harness-result.json` before failing can be finalized
  into the complete canonical bundle with `phase=finalized`, after which the
  launcher propagates its nonzero exit code. The result's `launcher` object
  records the same phase, child-started/finalized flags, exit code, console, and
  exact-command references.

A finalization failure is itself a failed run. Consumers must branch on the
launcher's thrown validation error or terminal `finalized` value instead of
waiting for a bundle that the reported phase cannot produce.

Each finalized run directory contains:

- `harness-result.json`: authoritative structured identity, configuration,
  outcome, guest/QMP/teardown state, failure information, and launcher exit;
- `console.log`: the complete ordered child stdout/stderr stream;
- `launcher-command.json`: the exact executable and argument array;
- `artifact-manifest.json`: size and SHA-256 for every other file in the bundle;
- `command.txt`: the child process command line as observed by the runner; and
- `package-identity.json` on installation runs: the exact staged INF
  `DriverVer`, catalog signer and bundled-certificate thumbprints, and SHA-256
  identity of every required package file;
- `d3d-driver-profile.json` when driver profiling records are present: the
  selected display/miniport session, reconstructed 64-bit counters, timer
  validation, and derived copy/submit attribution;
- `host-process-profile.json` for capture-free performance runs: boundary-only
  `retro_host` process user/kernel CPU deltas, average utilized cores, and
  stable per-thread CPU deltas keyed by thread ID plus start time. Thread
  coverage is reported separately because threads created or destroyed inside
  the measurement window cannot be differenced safely; and
- the available mode-specific host, guest, driver, trace, profile, and capture
  artifacts copied before the shared lease is released.

Host CPU profiling takes only one process/thread snapshot at each existing
performance-window boundary; it does not poll or alter the guest package.
`averageUtilizedCores` is process CPU seconds divided by host wall seconds, not
time that can be added to driver or doorbell wall timers. Per-thread results
identify stable thread identities and CPU concentration, not QEMU thread roles;
use `stableCoveragePercent` before drawing conclusions from the top-thread
ranking. Managed process counters are sampled separately, so the profile
records total-versus-user/kernel skew and labels disagreements beyond its
reported boundary-capture tolerance. A process snapshot failure leaves
incomplete evidence; partial thread enumeration leaves `valid-with-findings`.
Neither condition by itself fails the acceptance run.

The runner prints `HarnessConfig` before the VM launch, including requested,
effective, and recommended outer timeouts. It then prints `HarnessProgress`
records every 30 seconds by default and a terminal `HarnessRun`. Requested
timeouts have explicit safety floors: 180 seconds for ordinary XPDM runs,
300 seconds for long Direct3D/performance runs, 600 seconds for ordinary
capture-enabled game observation, and 7200 seconds for the synchronous UT2003
menu oracle. The UT floor conservatively scales the recorded 608.5 host seconds
per 46.625 observed seconds across its bounded startup+dwell interval. The
recommended outer timeout adds 240
seconds for finalization and teardown, or 360 seconds when video is requested.
An outer supervisor must allow at least that reported budget and must continue
waiting on the same process rather than starting a replacement run.

Only one acceptance runner may own the shared `qemu\winxp` runtime artifacts at
a time. Its byte-range lease records the run ID, harness and host PIDs, QMP port,
mode, content path, timeouts, failure, and cleanup status. A contender fails with
the readable owner metadata. Normal and exceptional exits stop the exact owned
host process when required, persist passed/failed lease status, and release the
lease; do not bypass the launcher by manually deleting the lease file.

For a quick interactive development run where a durable evidence bundle is not
needed, the runner can still be invoked directly from the repository root:

```bat
qemu\harness\run-xp-acceptance.cmd -TimeoutSeconds 150
```

Direct invocation is not the canonical path for handed-off validation because
it does not add the launcher's console and exact argument record.

Before a VM run, execute all harness parser and unit checks through the single
T0 entry point. It parses every harness PowerShell script, runs each suite in an
isolated Windows PowerShell process so one failure cannot hide later results,
and optionally writes a structured result, individual suite logs, and a hashed
manifest:

```powershell
$t0RunId = '20260717-harness-t0'
& .\qemu\harness\test-acceptance-harness.ps1 `
  -ArtifactDirectory ".\artifacts\runs\$t0RunId" `
  -RunId $t0RunId `
  -TaskId 'xpdriver-harness-review'
if ($LASTEXITCODE -ne 0) {
  throw "Harness T0 failed with exit code $LASTEXITCODE"
}
```

This entry point covers serial tailing, D3D driver profile session selection
and split-counter arithmetic, boundary-only host
process/thread CPU profile arithmetic and identity matching, package identity,
acceptance-matrix verification, run identity and lease behavior, atomic
artifact creation/replacement, runner arguments and exceptional cleanup,
launcher finalization, and test preflight/handback workspace auditing. On failure it continues through the remaining suites,
prints the failed suite's raw output between explicit markers, records the
complete per-suite stream when an artifact directory was requested, and exits
nonzero.

For an independent handoff, put the frozen source, repository, package/input,
and canonical bundle expectations in a JSON matrix spec and run the reviewed
verifier instead of writing one-off validation scripts:

```powershell
& .\qemu\harness\verify-xp-acceptance-matrix.ps1 `
  -SpecPath <frozen-matrix-spec.json> `
  -OutputPath <new-verification-result.json>
if ($LASTEXITCODE -ne 0) {
  throw "Acceptance matrix verification failed: $LASTEXITCODE"
}
```

The spec has schema `1`, one stable `taskId`, and these optional identity
collections plus one or more bundles:

- `frozenFiles`: exact absolute `path`, `bytes`, and `sha256` records;
- `inputFiles`: named exact host/core/tool inputs with SHA-256 and optional
  size;
- `repositories`: `windows` or `wsl` Git path plus exact branch, commit, and
  normalized `statusSha256`;
- `package`: staged directory, exact DriverVer/signer, and exact required file
  hashes; and
- `bundles`: `harness-self-test`, `install`, or `d3d-profile` directories,
  run IDs, optional expected suite names, and data-driven result-field
  assertions.

The verifier recomputes exact manifest membership, file sizes and hashes,
checks the result's artifact references, applies the type-specific pass and
teardown contract, compares package/profile sidecars semantically, reparses
the raw final driver profile, rejects capture artifacts, and converts the
frame-publication, game-performance, and transport audit console records into
structured observations. It refuses to overwrite its output and exits nonzero
with an explicit findings array on any stale identity or evidence mismatch.
The detailed handoff remains an audit/explanation document; the verifier is the
oracle. A tester must not synthesize replacement assertion code when the
verifier reports a failure.

Two checked helpers capture the workspace evidence around a handed-off matrix
so the tester does not reconstruct it by hand. Before building or launching,
record the start state and require the ownership, disk-policy, and prerequisite
checks to pass:

```powershell
# Stage 1, before creating a planned child (its name must be completely
# free; new-xpdm-overlay.ps1 refuses when either file exists):
& .\qemu\harness\verify-xp-test-preflight.ps1 `
  -TaskId <task-id> `
  -OutputPath <aggregate-dir>\start-state.json `
  -PlannedChildName <new-child-name> -HashBase `
  -RequireHostRepo
if ($LASTEXITCODE -ne 0) {
  throw "Test preflight failed with exit code $LASTEXITCODE"
}

# Stage 2, after new-xpdm-overlay.ps1 created the pair:
& .\qemu\harness\verify-xp-test-preflight.ps1 `
  -TaskId <task-id> `
  -OutputPath <aggregate-dir>\start-state-postcreate.json `
  -ContentPath qemu\winxp\xpdriver-<new-child-name>.qemu_cmd_line `
  -PlannedChildName <new-child-name> -ExpectChildCreated
```

Runs that boot an existing registered target need no planned child: one
preflight with the assigned `-ContentPath` is the whole gate. The stage-1
start state is the one the handback audits.

`invoke-xp-test-cycle.ps1` runs that whole contract as one command: stage-1
preflight (which audits the `-BasePath` the child will actually be backed
by), optional fresh-child creation with the stage-2 gate, and every
requested mode through the canonical `invoke-xp-acceptance.ps1` launcher so
each run leaves a finalized artifact bundle (manifest, harness result,
launcher command, console and driver logs) under the evidence directory.
Performance modes repeat `-PerformanceRuns` (at least 1) passes and report
a median; a run that exits 0 without publishing a fresh-FPS measurement is
a failed run. `MaxPayneVisual` is a separate, non-repeated tutorial companion:
it captures outside every timed run and publishes three evenly spaced,
decodable raw BMPs plus `maxpayne-visual-sequence.json`.
The closing handback always runs and audits every attempted run bundle.
`cycle-summary.json` records per-run verdicts, the SHA-256 identities of the
`retro_host`/core binaries, fresh-FPS medians, final miniport batching counters,
and the visual companion's image names, timestamps, sequences, and hashes;
`-DryRun` renders the full command plan without touching the VM.

The canonical initial guest launch delay is 40 seconds and can be tuned with
`-GuestLaunchDelaySeconds`; existing mode-specific retries remain the fallback
when Explorer is not ready. Every mode still receives a fresh cold VM. Before
preflight, non-dry cycles validate one externally published
`guest\acceptance\frozen-inputs-ut-oracle-r5.json` and require the selected `retro_host` and
core to match its exact paths, sizes, and SHA-256 values. Standalone XPDM runs
that omit `-RetroHostPath`/`-CorePath` derive both paths from that manifest.

Every cycle mode, including Install, copies the already-frozen acceptance ISO
once to a unique read-only file inside the cycle evidence directory and binds
every child to that private path and SHA-256; validation never builds or
restages the ISO. A unique temporary content profile remains beside the
original QEMU profile so relative disk/config paths retain their meaning; it is
removed after handback, while an exact evidence copy and content hash remain.
Each child receives the same frozen manifest and rejects a missing, changed, or
differently mounted ISO or a different host/core. Install additionally retains
the package-before/preparation/package-after identity proof and requires the
manifest's package directory.

Publish a new generation outside the validation window with
`publish-xp-acceptance-inputs.ps1`. Its `-PackageDirectory` and `-IsoPath`
are both passed to the acceptance Makefile, including non-default absolute
paths; publication then rejects an ISO whose embedded package differs from
the selected package and records the ABI parsed from those packaged bytes.
Publication is append-only: an existing manifest may be regenerated only
when the canonical content is byte-identical, and any other change requires
a new output path. Consumers fail closed unless the manifest ABI is present
and equals the ABI decoded independently from the frozen
`qxpmp.sys`/`qxpdisp.dll` bytes, and unless every install payload's digest
is a member of the signed `qxp.cat`, so a retained old catalog cannot
validate a payload replaced after signing.

Each run result publishes `acceptanceIsoSeconds`, `bootToFirstLaunchSeconds`,
`firstLaunchToResultSeconds`, and `shutdownSeconds`. These phase timings are
also copied into the cycle summary so timing changes can be evaluated without
parsing console progress lines.

`Install` mode always requires `-ChildName`: installs land only on a fresh
child created inside the cycle, never on an existing `-ContentPath` target
(a registered preserved profile passes preflight writable as its own
sanctioned frontend, so the orchestrator refuses the combination outright).
Install cycles always install and report the staged package at
`guest\xpdm\build\package`; a different `-PackageDirectory` is rejected.

Candidate packages that are not yet the accepted staged package must use
`invoke-xp-candidate-cycle.ps1`; do not copy an artifact-local
junction/restore script. The wrapper exclusively owns the staging path,
validates the complete accepted and candidate package identities, exposes the
candidate at the canonical staging path, invokes `invoke-xp-test-cycle.ps1`,
and restores and revalidates the accepted package in `finally`. A child
failure retains its exit code only when restoration succeeds; stale
`package.accepted-hold` state or any restoration mismatch fails closed.

Name an immutable frozen-input manifest. If it already exists, the wrapper
requires its package directory, DriverVer, signer, exact member hashes, ISO,
host, and core to match before the canonical cycle starts. If it does not
exist, `-AcceptanceIsoPath` is required and the wrapper runs
`publish-xp-acceptance-inputs.ps1 -SkipBuild` while the candidate occupies the
staging path, before canonical preflight opens the validation window. The new
manifest is not overwritten or deleted after a failed cycle.

```powershell
# Signed candidate package + already-built candidate acceptance ISO:
& .\qemu\harness\invoke-xp-candidate-cycle.ps1 `
  -TaskId <task-id> `
  -EvidenceDirectory artifacts\runs\<new-cycle-dir> `
  -CandidatePackageDirectory artifacts\temp\<candidate-package> `
  -AcceptanceIsoPath artifacts\temp\<candidate-acceptance>.iso `
  -FrozenInputsPath artifacts\temp\<new-frozen-inputs>.json `
  -ChildName <new-child-name> -QmpPort <port> `
  -BasePath .\qemu\winxp\<approved-base>.qcow2 `
  -Modes Install,D3dVisual,D3d9
```

```powershell
# Full fresh-child install + T2 matrix in one invocation:
& .\qemu\harness\invoke-xp-test-cycle.ps1 `
  -TaskId <task-id> `
  -EvidenceDirectory artifacts\runs\<new-cycle-dir> `
  -ChildName <new-child-name> -QmpPort <port> `
  -BasePath .\qemu\winxp\<approved-base>.qcow2

# Three-run Max Payne + UT2003 comparison on an existing child:
& .\qemu\harness\invoke-xp-test-cycle.ps1 `
  -TaskId <task-id> `
  -EvidenceDirectory artifacts\runs\<new-perf-dir> `
  -ContentPath qemu\winxp\xpdriver-<child>.qemu_cmd_line -QmpPort <port> `
  -Modes MaxPayne,UT2003

# Capture-enabled visual companions require Install on a fresh same-cycle
# child; this prevents stale installed-package evidence:
& .\qemu\harness\invoke-xp-test-cycle.ps1 `
  -TaskId <task-id> `
  -EvidenceDirectory artifacts\runs\<new-visual-dir> `
  -ChildName <new-child-name> -QmpPort <port> `
  -BasePath .\qemu\winxp\<approved-base>.qcow2 `
  -Modes Install,MaxPayneVisual,UT2003Visual,UT2003Gameplay
```

The preflight writes one self-describing `start-state.json`: both repository
identities, running VM processes, the QMP port probe, the shared lease owner,
the base disk identity (size and last-write time, plus a content hash with
`-HashBase`), the content profile's parsed disk/network/QMP/VRAM settings,
planned child availability, and the pycdlib/WSL/encoder prerequisites. The target
registry is semantically validated through `tools\validate-test-targets.ps1`
before its protected set is trusted, so a registry that parses but carries
null targets, duplicate ids, or escaped paths fails closed. Profile policy
decisions are made over command-line tokens (a double-quoted value is one
token), never regex over the raw text, so option values cannot carry decoy
options. The base and every registry backing disk are reproducibility roots
that boot only through `-snapshot`, and a `preserve`/`child-only` registry
target never boots writable — not even through its own registered content
profile; boot a disposable `-snapshot` copy or create a fresh child
overlay. Anything else that would write a
protected disk fails with `DISK_POLICY_VIOLATION` — including
floppy/pflash/SD/mtdblock/`-blockdev`/`-fsdev`/`-virtfs` attachments, any
`-drive` without an exact `media=cdrom` segment, and a repeated `-hda`,
all of which fail closed because only the first `-hda` is audited. With
`-ExpectChildCreated` and a `-BasePath`, the overlay's stored qcow2
backing reference is verified with `qemu-img info`. An online profile fails
with `NETWORK_POLICY_VIOLATION`, and an ownership conflict quotes the holding
PID, port, or lease owner. Every standard-VGA profile must also declare exactly
one `-global VGA.vgamem_mb=128`: the driver advertises a 128 MB aperture, so a
smaller default backing store can silently remove presentation surfaces while
the guest still reports 128 MB. Missing, malformed, non-128, duplicate, and
quoted-decoy declarations fail before launch with `HANDOFF_INCOMPLETE`; the
separate non-XPDM Cirrus baseline is exempt. The output is never overwritten.

## Scope: what this harness defends against

This harness exists to prevent **accidents** and to preserve **honest
evidence**: it stops installs and writable boots from landing on preserved
checkpoints, catches leftover processes, mid-window workspace edits, and
mis-wired or tampered evidence bundles, and fails closed when its inputs
are malformed. **Adversarial-runner containment is explicitly out of
scope.** Delegating VM execution to a runner is a trust decision made
before the runner starts; a runner that authors its own profiles and runs
arbitrary commands can bypass any static parser, and no amount of argv
emulation changes that. Review findings that presuppose a malicious runner
(argv-exactness games, atomic publication against concurrent creators that
do not exist, cross-binding evidence a trusted local chain already binds)
are judged against this declared scope, not against a maximal threat
model.

After the matrix, audit the handback against that start state:

```powershell
& .\qemu\harness\verify-xp-test-handback.ps1 `
  -StartStatePath <aggregate-dir>\start-state.json `
  -OutputPath <aggregate-dir>\handback-audit.json `
  -RunDirectory <finalized-vm-run-dir>[, <finalized-vm-run-dir>...] `
  -T0Directory <aggregate-t0-dir> `
  -PackageDirectory <signed-package-dir>
if ($LASTEXITCODE -ne 0) {
  throw "Test handback audit failed with exit code $LASTEXITCODE"
}
```

The audit requires the recorded preflight to have passed (`PREFLIGHT_FAILED`),
the base disk identity to be unchanged (`BASE_IMAGE_MUTATED`), no
window-started VM process or bound QMP port to remain (`CLEANUP_INCOMPLETE`),
the shared lease to be free, both repositories to match the start state
exactly — branch, HEAD, status lines, and the per-file content identity of
every already-dirty path (`WORKSPACE_MUTATED`) — and every supplied bundle to
be complete evidence: a finalized VM bundle carries its result, manifest, and
launcher identity, while the launcher-less T0 self-test aggregate is audited
through `-T0Directory` (`EVIDENCE_INVALID`). Both scripts exit nonzero with
per-check stop-token classes and write their JSON atomically. Validate them with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\qemu\harness\test-xp-test-preflight.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\qemu\harness\test-xp-test-handback.ps1
```

Acceptance ISO generation uses the checked `guest/acceptance/make_iso.py`
helper instead of relying on a Unix `genisoimage` executable in the caller's
`PATH`. Install its pinned pure-Python dependency once into the ignored tooling
directory; the same copy is usable by the Windows and WSL launch paths:

```powershell
python -m pip install --target .tools/pycdlib pycdlib==1.14.0
```

The image is deterministic. If an interrupted runner leaves an existing ISO
mounted without delete sharing, the builder byte-compares the newly generated
temporary image and reuses the locked destination only when they are identical.
Changed contents still fail closed instead of running a stale guest payload.

Use `-Reclaim` for the owner-death case. It terminates the first ICD process
mid-context, waits beyond the heartbeat lease, then requires a second process to
acquire the reclaimed host and finish the normal render test.
Use `-Idle` for the false-positive guard: the owner performs no GL calls for 12
seconds while its heartbeat remains live, then must finish the render test on the
same session.
Use `-Wgl` to install QXPICD in the disposable snapshot, supply the missing
XPDM `OPENGL_GETINFO` signal through the proven narrow IAT shim, and require a
normal application linked to Microsoft `opengl32.dll` to select the ICD, create a
WGL context, report the host renderer, render, swap, and cleanly release it.
Use `-Xpdm` only with an ISO built by `make xpdm-iso`. It boots QEMU Standard
VGA, installs the signed development
`qxpmp.sys`/`qxpmap.sys`/`qxpdisp.dll`/`qxpicd.dll`/`qxpcoin2.dll` package,
and schedules the stock-opengl32 test with no replacement OpenGL DLL.

XP does not finish its in-guest display-driver reboot under the libretro host,
and QEMU cannot carry a `-snapshot` overlay across a host restart. Use a local
persistent QCOW2 overlay and content file for the deterministic two-stage test:

```powershell
qemu\harness\run-xp-acceptance.ps1 -Xpdm -InstallOnly `
  -ContentPath qemu\winxp\xpdriver-xpdm-persistent.qemu_cmd_line
qemu\harness\run-xp-acceptance.ps1 -Xpdm -BootOnly `
  -ContentPath qemu\winxp\xpdriver-xpdm-persistent.qemu_cmd_line
```

`-InstallOnly` succeeds after the real SetupAPI installation and installed-file
verification, including the co-installer that creates the native OpenGL ICD
registry tuple and, after successful installation, the shared `qxpmap` service.
The INF copies `system32\drivers\qxpmap.sys`; service creation is in the
co-installer because ChkINF reports W22.1.2027 for both `StartType=1` and
`StartType=2` in a display INF. The guest requires the `Services\qxpmap` DWORD
values `Type=1` and `Start=1` before its existing `files-verified` marker; it
accepts `Start=2` only when `PendingFileRenameOperations` names `qxpmap.sys`.
A shipped co-installer's filename must change when its behavior changes: XP can
defer replacement of an in-use co-installer until reboot and execute the old
DLL during an upgrade. `qxpcoin2.dll` ensures this install loads the mapping
service implementation immediately while retaining the `QxpCoInstaller` export.
`-InstallOnly` invokes the installer's stage-only mode,
flushes the registry, and requests that the VM power off before XP tries to
hot-restart the active display stack. Before boot, the host verifies and hashes
the exact eight-member staged package, requires all six INF/binary payloads to
be catalog-covered and the signer to match the bundled certificate, and records
both identities. After SetupAPI and file verification, the guest reports
the exact mounted INF `DriverVer` and SHA-1 certificate thumbprint. Installation
fails unless both values match the staged package without conflicting reports.
`-BootOnly` then cold-boots the same overlay, logs into XP, launches
`D:\XPDM\qxpwgltest.exe --native`, and requires the real XPDM escape, installed
ICD, accelerated owner, concurrent GDI Generic fallback, rendering, and clean
teardown to return `QXPICD_RESULT rc=0`.

For development, create a fresh copy-on-write QCOW2 overlay backed by the unchanged
known-good XP image for every package version. Reusing an overlay that already has
XPDriver installed is intentionally unsupported: XP's driver cache and catalog
ownership can restore an earlier miniport during raw file replacement, producing a
plausible render result from stale code. A new overlay is initially only about
200 KB, is instant to create, and makes each test a trustworthy clean SetupAPI
install without modifying the base image.

Use the checked helper to create both the ignored overlay and its offline
(`-nic none`) content file. The generated profile also fixes the standard VGA
backing store at 128 MB to match the driver's advertised aperture. The helper
stores a relative backing filename that works from both Windows QEMU and WSL,
refuses to overwrite an existing checkpoint, and uses the harness's standard
QMP port unless another one is requested:

```powershell
qemu\harness\new-xpdm-overlay.ps1 -Name xpdm-release-190-quad-point-range
```

Add `-Game` to launch the installed Quake II demo through stock
`opengl32.dll`. The guest rejects a private runtime, waits for the `demo1` map
and an accelerated renderer, then requires ten seconds of stable rendering.
The host also scores frames captured during that interval, rejects desktop-only
output, and retains the verified in-game frame as
`qemu\winxp\xpdm-quake2.bmp`.

Add `-D3d` to launch the stock-System32 Direct3D 8 probe and its low-level
DirectDraw companion instead. The harness records every `QXP_D3D_STAGE` message
and requires `QXP_D3D_RESULT rc=0` for a complete clear/present pass. The
current native-HAL checkpoint creates the device, selects the DX7 DP2 path
through `GetDriverState`, completes a target clear, maps DirectDraw surfaces
through bounds-checked Lock/Unlock callbacks, and completes windowed Present
through XP's stock software Blt fallback. The harness suppresses duplicate
launches and preserves a timeout framebuffer for later transport work.
The parent probe owns COM1 and the acceptance VMs deliberately have no network,
so the DirectDraw child writes exactly three transient records to
`C:\QXPDDQUERY.LOG`: the raw GDI1/GDI2 HAL table and format report, the
callbacks3 report, and the video-memory Blt result. After the child exits, the
parent re-emits those records through its normal result channel and requires all
three. It removes the transient file both before launch and after collection, so
a missing or crashed child cannot satisfy the gate with stale evidence.
Use `-D3dFullscreen` for the exclusive fullscreen/Flip path. Use `-D3dVisual`
for the same fullscreen test plus deterministic framebuffer assertions covering
an explicit reacquire/rebind of the device's own backbuffer and depth surface,
the checker texture, a dynamic default-pool texture with exact pre- and
post-`D3DLOCK_DISCARD` colors, managed mipmap selection, D24S8 stencil masking, a
separately created D24X8 depth-only surface with exact near/far/clear pixels and
restoration of the original D24S8 surface, coplanar
depth bias, color-write masking, common additive, signed-add, subtractive, and
scaled-modulate and alpha-interpolated texture combines, texture-factor
arguments, DOT3 bump-lighting combines, two-stage lightmap-style texturing
with independent or shared coordinates, a null stage-zero texture whose
TFACTOR-only equation feeds a textured stage-one ADD with exact `90A0B0`
output, RHW-correct perspective interpolation, three-input RGB/alpha LERP, and other argument
modifiers, depth rejection, alpha
blending/testing, blend equations, triangle strips, indexed fans, line
lists/strips, uniform 9-pixel points with an off-center radius assertion,
optional FVF fields, and specular
color-sum, including A4R4G4B4 color/alpha conversion and L8/A8L8 luminance and
alpha conversion. It next scores a distinct object-space reference frame with
131 exact samples covering nonidentity world/view/projection composition,
directional, point, and spot lights, emissive material, a no-`COLOR1`
point-lit fallback from a requested `COLOR1` diffuse source to material, separate
`COLOR1` ambient and emissive sources, a no-`COLOR2` fallback plus separate
`COLOR2` ambient/emissive sources and a blended COLOR2 diffuse-alpha oracle,
point-light specular with local and infinite viewers, material/COLOR1/COLOR2
sources, missing-COLOR2 fallback, a combined COLOR1 diffuse/specular state, and
spotlight specular, four two-point specular samples with independent hard Range
and isolated material/COLOR2 red/green sources,
four-point independent Range with isolated red/green/blue/magenta sources,
five-point independent Range with isolated red/green/blue/cyan/magenta sources,
separate six-, seven-, and eight-point rows that update the active count and both
packed Range vectors while keeping every neighbor outside the cutoff,
post-projection and post-divide table fog, separate EXP and EXP2 results, table fog
combined with point-light Range, and table fog combined with literal four-component
texture q,
inverse-transpose normal
handling, independent 5/11/17-pixel object points and their radii, both sides of a
user clip plane, an input-3D/output-2D texture transform, `COUNT3 | PROJECTED`
homogeneous division, an explicit four-component nonprojected discriminator and
its simultaneous ordinary/range vertex-fog paths, a hard point-light Range cutoff,
exact spotlight outer-cone samples with linear and squared Falloff, and a combined
point-light/nonprojected-4D-q draw,
absolute eye-Z vertex fog, a radial range-fog discriminator, and untouched
background. Four asymmetric point-sprite groups additionally prove stage-zero,
stage-one, stock-runtime `ps_1_1`, and offscreen render-to-texture coordinate
replacement/orientation, while a uniform cyan group proves explicit coordinates
are restored when sprite generation is disabled. Two multi-light samples use all
eight advertised slots at sparse logical indices and then disable the endpoint
contributors, proving additive accumulation and active-slot compaction. Two more
samples keep sparse point lights simultaneously enabled and prove that each light's
independent D3D `Range` removes only its out-of-range cross contribution. Four
additional normalization-off samples select diffuse from `COLOR1` and `COLOR2`,
proving both source controls and their independent cutoffs. Three final-draw
samples use sparse logical IDs for simultaneous red, green, and blue point lights,
proving three independent Range cutoffs without risking later scene geometry
overwriting the oracle. Two adjacent final-draw samples keep all three lights
beyond Range and prove absolute-eye-Z vertex fog plus post-divide table fog with
distinct exact blends. Together these form
the stock XP software-VP oracle and the exact
accelerated parity gate for hardware VP. XP's software VP deterministically uses
planar Z for the range discriminator and leaves it white; `-D3dHardware` instead
requires the advertised radial-distance result in purple. Encoding that one
mode-specific runtime baseline keeps both paths deterministic without weakening
the accelerated assertion. Before either object-space frame, the probe saves
the live viewport and installs an upper-left 640x480 viewport. This keeps NDC
conversion, local-viewer eye vectors, and point-scale coefficients invariant
when the backbuffer is 800x600 or larger; it restores the caller's viewport
before returning. The probe then presents a separate 54-sample
scalable-specular frame. Six isolated rows use three through eight sparse point
or spot lights with independent Range, alternating MATERIAL/COLOR2 sources,
local/infinite viewers, and Power zero/eight. Power-zero channel endpoints, two
background anchors, and the infinite-viewer black row are exact. The seven
low-intensity Power-eight local-viewer samples are exact under software VP and
allow one output LSB only in nonzero expected channels under hardware VP for
host raster rounding. Zero channels remain exact, and the complete half-vector
profile is mandatory. Two additional
samples use simultaneous spotlights to require exact linear (`604020`) and
squared (`302010`) outer-cone Falloff. A final three-spot triplet combines that
lighting with fog: Power-zero red specular plus half vertex fog is exact
`504040`, range fog is software-planar `604020` or hardware-radial `C040FF`, and
post-divide table fog is exact `6C381C`. The triplet is drawn last so its deliberate
dither and fog-coordinate transitions cannot contaminate the established
low-intensity raster oracle. Two more samples enable world-space `x >= 0` under a
translated world matrix: the left spotlight must remain background `16233B` and
the right must retain exact nonlinear Falloff-two `302010`. That pair rejects
both plane capture under world and fallback to fixed OpenGL spotlight semantics.
Five additional samples exercise point primitives under five simultaneous hard-
Range lights. Pure red/green centers prove scalable-program selection, while
inner/outer samples distinguish 10- and 20-pixel per-vertex sizes after D3D
viewport scaling, attenuation, and min/max clamping.
Five more samples keep the same five-light discriminator active while a
20-pixel point uses top-left point-sprite coordinates, the asymmetric reference
texture, and an exact half-distance linear fog mix. Four quadrants plus an
outer boundary reject coordinate, texture-stage, fog, sprite-size, and clamp
regressions in one standards-driven interaction.
Two samples add a mixed red directional and green point-light pair. Directional
ambient must affect both triangles, while point diffuse must affect only the
triangle inside hard Range, producing exact yellow/red centers. Fixed OpenGL's
missing D3D Range cutoff would produce yellow/yellow, so this also proves that
the scalable program owns the mixed set.
It then presents a separate white wireframe triangle on blue and
requires both its top edge and a blue interior, proving fill-mode translation and
upright rendering after the first hardware flip. Visual mode temporarily requests
synchronous host readback with `QEMU_FX_FORCE_SYNC_READBACK`; installed-game
observation and other modes retain the normal asynchronous PBO path. Because the
synchronous dump callback can fall to roughly 0.5 FPS, visual mode also passes the
probe's `--visual` flag and holds each distinct main, HWTL, wireframe, Reset, and
post-Reset HWTL frame for 2.5 seconds. The denser scalable-specular frame is held
for six such intervals so a sub-0.5 FPS callback cannot expose only an
incremental prefix. This makes capture deterministic
without slowing the
ordinary fullscreen smoke test. After the exact frames and fullscreen Reset, the
probe rebuilds the object FVF, transforms, texture-stage state, material, and point
light, then requires a separate material-lit `FF4000` triangle on an `0D2E4F`
background. This proves that object-space rendering and its bounded vertex program
recover after Reset; neither the earlier HWTL frame nor the uniform `123456` Reset
clear can satisfy the two-pixel identity gate. The triangle's NDC center is derived
from the live viewport so it remains at the scorer's absolute `(320,240)` canvas
coordinate when a clean XP base starts at 800x600 or another larger mode. The probe
also requires D3D8 custom
state-block record, apply, capture, re-apply, and
delete for render state, texture-stage state, world transform, material, light
definition/enable, and clip planes. A second contract verifies runtime-owned
`D3DSBT_PIXELSTATE`, `D3DSBT_VERTEXSTATE`, and `D3DSBT_ALL` membership, exclusions,
and Capture/Apply; the host does not report success until both completion markers
arrive. Keeping the destructive ALL oracle after the visual gates prevents its
legitimate binding restoration from contaminating the rendered reference. Its
bounded worker watchdog adds the nine intentional visual
holds to the fixed D3D execution budget, avoiding false timeouts under slow
synchronous capture without weakening hang detection. It keeps the matching
main, HWTL-reference, HWTL-specular, post-Reset HWTL, and wireframe frames at
`qemu\winxp\xpdm-d3d-visual.bmp`, `qemu\winxp\xpdm-d3d-hwtl-reference.bmp`,
`qemu\winxp\xpdm-d3d-hwtl-specular.bmp`,
`qemu\winxp\xpdm-d3d-post-reset-hwtl.bmp`, and
`qemu\winxp\xpdm-d3d-fillmode.bmp`, then removes
its temporary numbered dumps. Failures retain representative
`xpdm-d3d-visual-failed.bmp`, `xpdm-d3d-hwtl-reference-failed.bmp`,
`xpdm-d3d-hwtl-specular-failed.bmp`,
`xpdm-d3d-post-reset-hwtl-failed.bmp`, and `xpdm-d3d-fillmode-failed.bmp`
artifacts.
The scorer accepts captured geometries of at least 640x480 because a clean XP
base may retain a larger desktop mode. The probe explicitly renders its fixed
object-space canvas through an upper-left 640x480 viewport, and the scorer uses
that same upper-left canvas. Frame identity still requires every scene,
HWTL-reference, and HWTL-specular sample under its declared precision rule, so
accepting the larger container is deterministic and does not weaken the visual
gate.
Add `-D3dHardware` to `-D3dVisual` to require a hardware-vertex-processing
device for the same fullscreen scene. The guest reports the selected mode and
the harness fails unless it observes `vertex-processing hardware`, so a runtime
fallback cannot turn the accelerated parity gate into another software-VP
reference. Hardware runs retain separate `xpdm-d3d-hardware-*` logs and images.
The seven-light local-viewer Power=8 row has one nominal
`5,4,2,2,2,4,5` channel profile in both vertex-processing modes because the
canonical viewport keeps its eye vectors identical. Stock XP software VP is
exact: it quantizes each lit vertex to D3DCOLOR before submitting the compact
transformed FVF. Hardware VP evaluates the half vector before final color
conversion and retains one-LSB latitude for host rasterization. Both paths
remain strict, and zero channels remain exact.
Use `-D3dVs11` for the focused stock-System32 D3D8 programmable-vertex probe.
It creates a hardware-VP fullscreen device, submits an ordinary `vs_1_1`
declaration and function through the public runtime, uploads `c0`, and renders
the same quad first through user clip plane zero and then through the no-clip
ARB fast path. Serial stage markers bracket a shared bounded host-frame window;
the harness forces synchronous readback and requires exact
`0B1D2E/FF2080` clipped and `0B1D2E/20C060` fast-path samples. It also requires
six host vertex/clip definitions and deletions, at least five explicit display-
driver vertex deletes, seven host draws and presents, and clean teardown. The
expanded probe also records and captures custom state blocks containing shaders
with opposite declaration constants, draws after both replays, applies a state
block only while its referenced shader remains live, deletes the block before the
shader, then recreates/binds/verifies/draws/deletes a distinct shader. It also
resets the device, recreates/draws/deletes a shader, and rejects an unmarked
parameter token. Reset closes and reopens the miniport transport, so the gate
sums its session counters and requires the exact workload minimum of six pre-
Reset plus one post-Reset draw/present. The `.211` gate requires
the public 1.1/96-constant caps and proves the paired display/miniport transport
without a replacement Direct3D DLL.
Use `-D3dVsSuccession` for the real-process teardown/reacquisition variant. A
small wrapper launches the same stock-System32 VS1.1 probe in child process one,
waits for complete exit, then launches child process two with the same
runtime-local shader workload. Each child owns COM1 only during its lifetime.
The harness requires both child results, four context and four host opens,
twelve host/display/clip definitions, twelve host and clip deletions, at least
ten explicit display deletes, exactly four draw/present sessions with the
`6,1,6,1` pre/post-Reset split, fourteen total draws and presents, exact clipped
and fast-path pixels from child two, and clean VM teardown. This is a sequential
process-succession gate; it does not claim simultaneous multi-context coverage.
Use `-D3dVsMultiContext` for simultaneous context isolation. The stock-System32
probe creates two overlapping windowed hardware-VP devices in one process, and
requires the XP runtime to assign the same public shader handle to both. It
renders device A, device B, and device A again with opposite declaration state;
then it destroys A and renders B once more. The scorer requires the four ordered
exact signatures, two display/host/clip definitions, two host/clip deletions,
exactly two contexts over one reference-counted host session, exactly four draws
and presents, and clean teardown. Zero explicit display delete markers are valid
when stock D3D8 defers each final object to context teardown; the two host deletes
and exact B-survivor frame prove the owner-specific cleanup contract.
Add `-D3dHwtlOnly` to `-D3dVisual` for the focused object-space oracle. The guest
creates only the reference textures/program, 131-sample HWTL frame, and
54-sample scalable-specular frame, while the harness
scores only the frame interval bracketed by the probe's HWTL enter/leave markers.
It deliberately skips the transformed scene, wireframe, Reset, and state-block
contracts, so it is a development loop rather than a replacement for the full
release gate. Retained BMP evidence is accompanied by an opaque 24-bit
`*.review.png`, avoiding image-consumer disagreements about 32-bit BMP masks.
Out-of-bounds samples are reported with the capture dimensions instead of throwing
an unhelpful bitmap exception.
Use `-D3dCopyVisual` for a windowed `D3DSWAPEFFECT_COPY` regression. It draws
asymmetric red, green, blue, and yellow corner anchors once, presents successive
cyan, magenta, orange, and white center updates, then clears only the center to
purple. The scorer requires every center state while all four anchors remain in
their original quadrants. It scans only frames published before probe teardown and
uses a coarse exact-color prefilter before full-resolution scoring, keeping the
test deterministic without repeatedly decoding hundreds of irrelevant frames. Its
upper frame bound comes from the last host dump before the final WGL context
deletion, so serial-file polling latency cannot admit a post-teardown desktop
repaint as a false rendering failure.
Add `-D3d16` to `-D3dVisual` to create an R5G6B5 fullscreen device and repeat
the complete scene, wireframe, Present, and Reset assertions with one source
quantization step of RGB565 tolerance. It retains separate `xpdm-d3d16-*`
artifacts so 16- and 32-bit evidence cannot overwrite one another. It composes
with `-D3dHardware`, making the same hardware-VP creation, exact object-space
reference, Reset, and teardown requirements mandatory in RGB565.
Use `-D3d9SelfTest` for the out-of-band Shader Model 2 draw-source authority
test. It is independent of whether stock-runtime SM2 capabilities are exposed
and isolates lifetime-safe system-memory resolution. It runs
`qxpd3d9selftest.exe`, which creates a `D3DPOOL_SYSTEMMEM` vertex buffer, fills
it with a known pattern, and calls `ExtEscape` with escape `0x1102`, passing
the byte count and FNV-1a checksum it expects. The driver runs the guarded
transaction over the calling process's system-memory vertex buffers and
reports how many hashed to that checksum. It executes no draw and submits
nothing to the host. The terminal line is `QXP_D3D9_SELFTEST_RESULT rc=0`;
`readers=0` proves no lifetime reader leaked and `matches>=1` proves the
resolved base and extent really name the caller's buffer.

The same run carries three runtime-path stages that reach the driver through
the stock D3D9 runtime rather than through `DrvEscape`, covering the DDI
surface and declaration contracts an ordinary game uses:

- `dxt1` and `dxt5` create, lock, fill, read back and sample a compressed
  texture. The reported pitch must equal `blocks_across * block_bytes`, the
  written blocks must survive the round trip, and four quadrant samples must
  decode to their exact colours. Declining the format outright is legal and
  reports `declined`; accepting it and then getting any of that wrong is
  `BAD` and fails the run.
- `vertexdecl` creates its own device with `D3DCREATE_HARDWARE_VERTEXPROCESSING`
  - software vertex processing would report the runtime's emulated profile
  (`MaxStreams 16`, `vs_3_0`) and hide the driver completely - then asks for a
  single-stream and a multi-stream vertex declaration. Single-stream must
  succeed. Multi-stream is `declined` on the ordinary DDI-v3 profile, which is
  exactly the `E_FAIL` that kills Prince of Persia: The Sands of Time; the
  line also prints `max_streams`, `vs_version`, `ps_version` and `decl_types`
  so the profile a game sees is recorded with the verdict.

Use `-D3d9` for the ordinary stock-System32 DDI-v3 fallback gate. It requires
`QXPDISP GD9 FALLBACK`, rejects a diagnostic DDI-v4 marker, verifies
VS1.1/PS1.3, and expects PS2/SM2 to be skipped when they are not advertised.
The common fixed-function and SM1.x contract creates a fullscreen HAL device,
uploads a managed A8R8G8B8
texture, renders an orange untextured triangle and green textured strip, creates
a `ps_1_1` program through the stock runtime, samples texture stage zero, adds
a shader constant, and checks the exact yellow shader output. It also
advertises and uploads a signed `V8U8` bump map, runs a two-stage `texbem`
program with its bump matrix, and checks the displaced blue sample. It also
uploads `L6V5U5`, runs `texbeml` with luminance scale/offset state, and checks
the exact `127/255`-green result. Finally it supplies three coordinate sets,
samples a stage-two texture through `texm3x2pad`/`texm3x2tex`, and checks the
exact red result. A final magenta triangle combines `texreg2ar` and `texreg2gb`
dependent reads, proving both channel-coordinate paths. Ninth and tenth exact
cyan samples use four 2D coordinate sets to cover both a four-stage fixed-function
combine and a `tex t3` lookup from stock D3D9 through the miniport. The gate
also requires mipmapped cube-map caps and A8R8G8B8 format support, uploads three
levels for all six managed faces, supplies three-component matrix rows, and
checks an eleventh purple sample after forcing level two of the positive-X face
through `texm3x3tex`. A twelfth white sample runs a stock-runtime `ps_1_2`
program that samples a 2D direction texture, performs a cube `texreg2rgb`
dependent read, and evaluates `dp4`, `add`, and the zero-sensitive `cmp` path.
A thirteenth yellow sample runs `ps_1_3` `texm3x2depth` over a nearer red
primitive; its vertex depth would fail, so the exact yellow result proves the
shader depth output took effect. Samples fourteen through sixteen come from a
64x64 X8R8G8B8 render-target texture: a depth-disabled green draw, the nearer amber
member of a depth-tested amber/red pair using D24S8, and an untouched purple clear.
Together they prove offscreen rasterization, depth attachment, sampling, and
orientation. Samples seventeen and eighteen use one dynamic default-pool
A8R8G8B8 texture before and after a discard lock, requiring exact `C02040` and
`20C080` output. The dynamic resource is released before Reset. The gate
requires a successful cooperative-level check, Reset, exact `123456` post-reset
frame, clean draw and present transport, and teardown. The matching scene is
retained as `qemu\winxp\xpdm-d3d9.bmp`; failures retain
`xpdm-d3d9-failed.bmp`, and the exact Reset frame is retained as
`xpdm-d3d9-reset.bmp`. Automated `-D3d9` runs enable the opt-in capture
handshake: the guest holds the main scene until response-checked QMP delivery of
F12, then holds the Reset scene until F11. The harness records complete frames,
scores only the newest scene and Reset windows at 640x480 or larger, and keeps a
bounded timeout plus low-rate guest heartbeat. Manual probe launches retain the
fixed three-second holds unless `--capture-handshake` is supplied. D3D9 capture
forces synchronous readback just like the other deterministic visual modes, and
its transport gate consumes the shared miniport `QXPMP D3D SHADER
DEFINE+`/`DELETE+` lifecycle markers used by both vertex and pixel programs.
When VS2/PS2 are advertised, the probe additionally creates and binds
stock-runtime VS1.1, VS2, and PS2 objects, then checks direct, indexed managed,
dynamic default-pool, and managed textured draws. It samples s4 and s15
independently, then executes one `ps_2_0` program that declares and samples all
sixteen independently bound managed textures in one draw; the gate requires
`mask=ffff` and exact center pixel `ffff2040`.

`-D3d9Multistream7` is the integrated diagnostic profile. It requires a fresh
same-cycle install and passes `-Diagnostic`, so the harness requires
`QXPDISP GD9 DIAG4`, `QXPDISP MAXSTREAMS8 DIAG`, coherent VS2/PS2 caps, and
eight streams. In addition to the full SM2 contract above, it proves
independent stream-0+7 declaration offsets/strides, exact pixels, resource
release before Reset, Reset, and teardown. It is `T2-diagnostic`; it must not
satisfy the ordinary `-D3d9` fallback oracle.

For a legacy indexed-command regression, set `QXP_TEST_LEGACY_DP2=1` while
building/staging the package, then add `-RequireLegacyIndexedDp2` to a D3D probe
run. For representable commands, the test build converts real EX indexed draws
back to index units and routes them through the same checked index-to-byte
translation used by `D3DNTDP2OP_DRAWINDEXEDPRIMITIVE`; byte offsets that cannot
be expressed as whole vertex or index units remain on the EX path. The harness
requires proof that the legacy translation rendered successfully; normal builds
use the EX byte-offset protocol directly.
Use `-D3dClean` only to remove legacy per-game wrapper DLLs from the disposable
Max Payne demo directory.
Use `-D3dGame` against that cleaned, installed demo to launch Max Payne through
System32 `d3d8.dll`. The probe launches the game executable directly with
`-skipstartup`, selects the startup dialog's Play button, then requires a real
XPDM context/host session/draw path and stable game frames in the dominant
captured fullscreen geometry that are neither the XP desktop nor all-black. The
gate rejects draw-transport failures, failed or unknown DP2 commands, surface
association-table exhaustion, render-target failures, unintended host swap limits,
and any skipped indexed primitive. Teardown counters report elapsed time, draws,
presents, clears, uploads, batch count, average draws per batch, and maximum batch
size. The visual gate requires spatially rich output in every quadrant for at
least two frames, rejects a normal or vertically inverted XP taskbar, and uses the
Max Payne menu's red upper-left logo as a game-specific orientation anchor. It keeps the
strongest matching frame as `qemu\winxp\xpdm-maxpayne.bmp`, keeps a representative
`xpdm-maxpayne-failed.bmp` on visual failure, and removes the temporary per-frame
sequence in either case. The retained frame proves a stable, upright main menu. With `-D3dGameplay`, the
separate tutorial visual path does not reuse that menu-logo oracle. After the
15-second guest warmup it bounds capture by scene timestamps and frame sequence,
uses the dominant geometry, discards only undecodable files, and keeps three
evenly spaced frames. Each selected frame is published as an identity-bearing raw
BMP; one compact sidecar records timestamps, scene-relative offsets, source
sequence, dimensions, and full hashes.
The sidecar is embedded in `harness-result.json`, which remains the single source
for source, package, host/core, content, and launcher identity. The automated
gate proves only that three bounded images were captured and decoded, not that
the rendering is correct; human or AI review must inspect all three images.
`MaxPayneVisual` schedules this visual-only run once in a canonical test cycle.

Add `-D3dPerformance` to `-D3dGame` for a capture-free throughput baseline.
This omits the host dump flag and visual scorer but retains the same stock-runtime
launch, 45-second guest observation, driver counters, transport-failure audit, and
clean teardown requirement. It writes separate `xpdm-d3d-performance` logs so a
profiling run cannot overwrite visual evidence. Use this mode for timing; ordinary
`-D3dGame` remains the correctness gate. The acceptance host also brackets the
exact guest observation with named events and reports one aggregate
`FramePublicationProfile`: fresh core publications, NULL duplicate callbacks,
fresh/public callback rates, p50/p95/p99/max fresh-frame intervals, and stall
counts. No per-frame logging or pixel readback is involved.

Use `-D3dBenchmark` for a short, capture-free stock-System32 D3D8 phase benchmark.
After one warm-up batch it measures eight idle Present calls, sixteen one-draw
frames, sixteen 16-draw frames, and 64 frames containing sixteen identical
lit 252-vertex object-space draws through the device's transform path. The result
reports guest submission time, the three basic Present averages, transform cost
per draw/vertex, the transform batch size, and whether the device is using
software or hardware vertex processing. Idle Present has no dirty
accelerated frame and does not enter XPDriver; comparing one-draw against idle
estimates fixed driver/host publication, while comparing 16-draw against one-draw
estimates marginal cached replay. The `.157` basic baseline was 739 ns/draw,
22.566 ms idle, 23.127 ms one-draw, and 24.642 ms for 16 draws. The extended run
measured 225 us per 252-vertex software transform (893 ns/vertex) and a 23.242 ms
transform Present. It completed 304 draws in 49 clean transport batches with no
failed DP2 command or teardown error. Run it with:

```powershell
.\qemu\harness\run-xp-acceptance.ps1 -Xpdm -BootOnly -D3dBenchmark `
  -ContentPath .\qemu\winxp\xpdriver-xpdm-release-154.qemu_cmd_line
```

Use `-D3dVsBenchmark` for the paired public-VS1.1 measurement fixture. It omits
frame capture and runs four fresh hardware-VP devices in ABBA order: fixed
pretransformed control, shader, shader, fixed control. Every phase has one
warm-up frame plus sixteen measured frames containing sixteen public
`DrawPrimitiveUP` calls. The guest reports QPC-derived device creation, shader
creation, API submission, and CPU-visible `Present` timing. The harness keeps
API calls distinct from transport requests: stock XP D3D8 coalesces the sixteen
calls in each frame into one 48-vertex DP2 request, so each phase must close with
272 public calls, seventeen transport draws/presents/batches, and a maximum of
one transport draw per batch. Four ordered host profile blocks, exact shader
lifecycle, failure-marker absence, and clean teardown are mandatory. This mode
is a measurement diagnostic, not a pixel oracle or an FPS benchmark; run
`-D3dVs11` for exact programmable correctness.

```powershell
.\qemu\harness\run-xp-acceptance.ps1 -Xpdm -BootOnly -D3dVsBenchmark `
  -ContentPath .\qemu\winxp\xpdriver-xpdm-release-214-context-publish.qemu_cmd_line
```

Add `-D3dHardware` to make hardware VP mandatory instead of accepting the
ordinary diagnostic fallback to software VP. Both modes receive the same fully
specified windowed presentation tuple; this avoids an XP D3D8
`D3DERR_INVALIDCALL` from incomplete defaults being misreported as a vertex
processing failure. Hardware benchmark mode additionally derives the expected
fixed-HWTL-state and vertex-array reuse count from the reported batch/frame shape
and requires both counters to meet it. That proves every repeated object-space
draw exercised the cache whose timing is being measured.

The profiled host build prints cumulative pass-through doorbell totals only when
the GL context closes, avoiding per-call log distortion. `-D3dPerformance` and
`-D3dBenchmark` parse the profile block belonging to the `QXPD3D` session into a
`D3dHostProfile` result, including the most expensive synchronous function.
Capture-free game observation similarly reports `GameObserveHostProfile`, using
the final accelerated `QXPICD` or `QXPD3D` session. Native OpenGL applications
can identify as `QXPD3D` when the installed XPDM wrapper owns the session. The
session marker prevents the earlier native-OpenGL preflight profile from being
mistaken for the game workload. A profiled 45.48-second Max Payne menu run
measured 152.190 ms across 157 swaps and 274.275 ms across all 178 direct calls;
158 framebuffer checks accounted for 274.192 ms. Package `.158` reduced the same
class to two distinct attachment checks totaling 8.529 ms; its 161 swaps took
141.414 ms and all 22 direct calls took 8.681 ms. Because all measured host work
was under one percent of the observation and this fixture does not enter gameplay,
its Present cadence is a compatibility observation rather than an FPS benchmark.

Profile-enabled XPDM builds also emit paired display and miniport teardown
blocks. The harness selects the final paired session, reconstructs high/low
64-bit counters, validates IOCTL agreement and nested-timer bounds, and derives
seconds, per-call cost, copy share, and residual submit time. A truncated final
block, an unpaired later display block, inconsistent counts, or impossible
nested timing fails the run instead of silently reusing an earlier session.
Ordinary non-profile builds remain valid and simply omit the structured driver
profile artifact.

Use `-ObserveGame UT2003`, `-ObserveGame UT2003Gameplay`,
`-ObserveGame UT2003GameplaySWVP`, or `-ObserveGame Warcraft3` with
`-Xpdm -BootOnly` against a registered games target while the XPDriver
acceptance ISO is mounted. `-ObserveGame UT2003 -RecordVideo` is the fail-closed
normal-menu oracle and the cycle-level `UT2003Visual` mode. It launches the
normal executable with default audio and no direct map, removes only the stale
`UT2003.log` on the disposable child, and requires canonical System32
`d3d8.dll` plus the launched process's foreground window after a 30-second
warmup. The guest emits exactly one `foreground-d3d8-window-stable` visual-ready
marker and then observes a further 180 seconds. It does not require a live
`Index.ut2` transition: the official Demo 2206 configures that nonexistent map
as a placeholder and does not reliably log a normal-menu boundary.

UT alone forces synchronous producer readback. When the visual-ready marker
arrives, the host records the latest published sequence and quarantines two
callbacks: one can already be inside retro_host's inline BMP writer and another
can occupy the frontend pending slot. It records the first later sequence
admitted and requires every selected timestamp to follow the marker. Eight
manifest-bound samples must span at least 150 seconds with no adjacent gap
above 45 seconds, at least four distinct canonical decoded-RGB hashes, and at
least three adjacent changes of 0.05% or more under an RGB-distance threshold
of 48. This threshold admits real low-area menu highlight animation while
rejecting identical/frozen samples.

The read-only verifier independently decodes all eight bound BMPs, recomputes
canonical diversity and adjacent change, and rejects disagreement with the
console claim. Its menu-layout classifier requires 640x480 geometry plus
expected title-logo, highlight, and four menu-row occupancy in at least six
frames. Desktop, blank/opaque, wrong-geometry, logo-only, and missing-menu-row
samples fail closed. Only that combined result emits
`UT2003Semantic valid=True transition=main-menu` with
`evidence=manifest-bound-menu-layout`; process lifetime, image richness, raw
hashes, or the visual-ready marker alone cannot substitute. The final passing
exact-package proof is
`artifacts/runs/20260801-abi63-ut-menu-r206/`, bound to frozen generation
`artifacts/frozen-inputs/generations/20260801-abi63-ut-menu-r206/`.
The automated cycle pass does not replace the release-review gate.
`verify-xp-evidence-bundle.ps1` remains `OPEN` until a schema-1 direct review
binds the harness result, manifest, eight raw/canonical sample hashes, and the
video hash at `scope=post-visual-ready`. A bound reviewer must inspect all eight
samples and the companion video and report visible/legible menus, no logo-only
frame, and no opaque glyph rectangles; a bound failing or malformed pass
returns `FAIL`.

`UT2003Gameplay` is the separate cycle-level joined-match gate. It launches
`DM-Antalus?game=XGame.xDeathMatch` with audio disabled and hardware-VP
settings after deleting only the stale title log. The guest requires the fresh
ordered `Browse DM-Antalus` -> `LoadMap DM-Antalus` -> `xDeathMatch` game class
-> `DM-Antalus.myLevel` -> `Bringing Level ... up for play` sequence,
canonical System32 `d3d8.dll`, and three successful foreground `SendInput`
fire attempts. The host also sends response-checked QMP left-clicks and anchors
capture at the first fire marker. The fixed 45-second observation must yield
eight manifest-bound rich samples, a visible post-input transition, and a
digest-named review video of at least 30 seconds; the bundle verifier
independently re-decodes the samples and checks semantic, input, D3D8,
duration, video, and artifact records. The mode is T3 visual evidence and
requires `Install` on a fresh child in the same cycle.
The final frozen-input proof is the r210 `UT2003Gameplay` bundle under
`artifacts/runs/20260801-abi63-s1-titles-r210/`; its independent verification
record is
`artifacts/temp/20260801-abi63-s1-titles-r210-utgame-verification-r2.json`
and returns `PASS`.

`UT2003GameplaySWVP` applies the same direct-map guest checks after the
software-VP INI override but remains a diagnostic comparison. Neither
direct-map mode substitutes for the independent normal-startup/main-menu
oracle. Ordinary observation retains every eighth frontend callback and uses a
600-second minimum host deadline. The synchronous UT menu oracle raises that
bounded deadline to 7200 seconds, allowing up to 300 seconds plus one second of
polling slack to reach the boundary and then the full 180-second post-boundary
dwell under the recorded capture slowdown.
Add `-ObservePerformance` for an opt-in capture-free measurement of the same
selected title path. It writes separate `xpdm-<game>-performance` logs, omits
`--dump`, does not set `RETRO_HOST_DUMP_ALL`, and skips frame scanning,
scoring, and video encoding. The mode still requires the guest result,
observed wall interval, matching aggregate host profile, transport-failure
audit, and clean teardown. It is incompatible with `-RecordVideo`; ordinary
observation remains the separate visual diagnostic. The aggregate profile and
observer interval are timing evidence. `FramePublicationProfile` reports
actual fresh core publications and semantic repeat callbacks; it does not
claim that separately published pixel buffers have different content.

`Warcraft3 -ObservePerformance` retains the forced native-OpenGL path, while
`UT2003 -ObservePerformance` retains the configured stock D3D8 menu path.
The exact-path variants are `UT2003Gameplay -ObservePerformance`, which waits
for the fresh DM-Antalus semantic and accepted foreground input before starting
its sample, and `Warcraft3D3D8 -ObservePerformance`, which uses the ordinary
default launch and requires canonical System32 `d3d8.dll`. Their cycle modes
are `UT2003GameplayPerformance` and `Warcraft3D3D8Performance`.
`-ObserveGame Warcraft3D3D8 -RecordVideo` remains a separate fail-closed
temporal oracle: it launches Warcraft with no renderer
override, requires canonical System32 `d3d8.dll`, warms the visible game for 15
seconds, then retains a 256-slot every-publication ring for a further 25 seconds.
At the completion marker the harness exclusively locks the producer's atomic
temporary file, quiesces further publication, and moves the numeric slots into
an immutable same-volume snapshot before guest shutdown can overwrite them.
The existing `frame-ring-black-cadence.py` gate requires all 256 valid frames,
at least one second of chronology, at least 192 visible frames, zero isolated
`V-B-V` black inserts, and zero visible-bounded black runs of any length. The
visibility floor rejects sustained black/dark output while retaining a bounded
edge transition; the run gate catches repeated `VVBB` flicker without mistaking
a short contiguous loading transition for an insert. Exact guest fields prove
one 15-second warmup and one 25-second System32-D3D8 observation; a process
handoff after oracle start fails rather than inheriting qualification. Teardown
must also report nonzero readbacks and surface-texture presents with zero CPU or
empty presents, proving the repaired path ran. Its cadence JSON, eight hashed
samples, first bounded black-run witness when present, and ring video containing
each of the 256 publications exactly once enter the run manifest; FFprobe
rejects an encoded frame-count mismatch. The capture-enabled form cannot be
combined with `-ObservePerformance`; the cycle-level `Warcraft3D3D8Visual`
mode requires a same-cycle `Install` on a fresh child. The separate
`Warcraft3D3D8Performance` mode is capture-free and does not run the cadence
oracle.

Capture-free Warcraft III and UT2003 observations warm the selected running
scene for at least 15 guest seconds and then emit explicit
`measurement-start` and `measurement-complete` markers around a 45-second
guest sample. Joined UT additionally requires its live-level semantic and
accepted input before the start marker. The host frame profiler follows those
markers, excluding process launch, renderer initialization, and
post-observation log collection from the distribution. Capture-enabled
observation keeps its existing launch and timing.
`-Game -GamePerformance` applies the same capture-free publication profiler to
the Quake II `demo1` map. The guest first proves native ICD renderer
initialization and map load, warms the live map for 15 seconds, then brackets a
45-second sample with the same measurement markers. Ordinary `-Game` retains
its ten-second capture-enabled visual gate.
The `.215` UT2003 gate needed 608.5 wall seconds including boot, capture,
encoding, and teardown to accumulate 46.625 guest-observed seconds. This is
harness throughput under synchronous capture, not game FPS.
The guest observer locates and launches the installed game, reports its visible
windows and graphics runtime modules, captures selected configuration/log evidence,
and returns a bounded result even when the game exits early. The harness keeps a
representative frame as `xpdm-ut2003-baseline.bmp` or
`xpdm-warcraft3-baseline.bmp`, archives the matching debug console as
`automated-host-xpdm-<game>.driver.log`, and removes the temporary frame sequence.
This is a diagnostic mode: a successfully observed broken baseline returns success;
the retained frame and driver trace determine whether compatibility improved.
The `.224` `UT2003Gameplay` gate retained nine distinct phase samples and a
33-second review clip through stock System32 D3D8 and `qxpdisp.dll`. An initial
multi-image review appeared to alternate complete and large-black-region frames,
but the captured RGB data, single-file review, and MP4 did not contain that
dropout. The black regions were a review-client delta artifact, not a driver
presentation failure. That `.224` scene remains explicitly pre-join; the newer
joined-gameplay gate does not relabel or rewrite old evidence. The same `.224`
games target retained a complete upright Warcraft III main-menu clip.
The read-only verifier now admits `-xpdm-ut2003gameplay` only through the
complete joined-gameplay contract: fresh ordered semantics, System32 D3D8,
three accepted inputs, post-input frame/video bindings, and terminal duration.
Preserved `.224` pre-join bundles lack that contract and still return `FAIL`;
the software-VP direct-map mode remains
`UT2003DirectMapHistorical` and cannot prove ordinary joined gameplay.
The verifier requires an explicit `-OutputPath` outside the input bundle and rejects
an omitted or in-bundle destination.
Current visual-bundle verification also runs pinned FFprobe against the
manifest-bound MP4 bytes. The decoded stream must have the exact frame count,
frame rate, duration, and 640x480 or 800x600 geometry recorded by the mode;
a filename, extension, console claim, and matching hash cannot substitute for
a decodable video container.
The verifier rejects bundle or output paths that traverse filesystem reparse
points, so a junction/symlink alias cannot redirect its nominally external
record back into the preserved bundle.

Review retained images one file at a time. A client may optimize a batch of
closely related images as deltas and display unchanged regions as black in later
items. Do not classify those regions as driver corruption unless they are also
present in an independently decoded RGB image or the deterministic MP4.

Validate acceptance argument parsing and the capture-free observer constraints
with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\qemu\harness\test-run-xp-acceptance-arguments.ps1
```

Add `-RecordVideo` to a qualifying `-ObserveGame` mode or ordinary Quake II
`-Game` run when animation or intermittent presentation needs human review.
Direct-map UT2003 modes are deliberately excluded. The harness
deterministically resamples the published frame timestamps to a 15 fps timeline.
Ordinary observation retains every eighth frontend callback, leaving several
source frames per second while avoiding redundant bitmap writes; the final clip
still preserves each retained frame's wall-clock duration. The dedicated Warcraft
D3D8 cadence mode instead encodes its every-publication ring so sampling cannot
alias away an alternating black frame. It records at most the full observation,
writes `xpdm-<game>-review-<content-hash>.mp4`, and then removes the numbered raw
frame dumps as usual. Retained sample bitmaps likewise include a short content hash
so image viewers cannot reuse stale pixels from an earlier run under the same
filename. `-GamePerformance` rejects recording; capture-enabled timing is not
performance evidence. Install the pinned, checksum-verified local encoder once with:

```powershell
qemu\harness\install-video-tools.ps1
```

The encoder is stored under the ignored `.tools` directory and is never packaged
with the guest driver.

Persistent content profiles often choose a non-default QMP port. The harness reads
that port from the selected `.qemu_cmd_line` file automatically; if an explicit
`-QmpPort` disagrees, it fails immediately instead of waiting through a VM timeout.
The checked-in harness profiles and the persistent games profile are intentionally
offline: they use `-nic none` and route acceptance results over COM1 to a host
serial log. Custom profiles without a serial result file retain the older UDP
result transport as a fallback.

The D3D8 visual gate also changes and restores one unsupported render state and one
unsupported texture-stage state. It requires the driver log to contain both bounded
`RSTATE CHANGED` and `TSTATE CHANGED` records, proving that later game values cannot
be hidden by Direct3D's initial default-state burst.

An immediate RunOnce launch can safely select GDI Generic while the display and
host transport settle; boot-only mode records that result and retries the native
test until it succeeds or the timeout expires.
For Direct3D modes, a successful native RunOnce smoke test is also the readiness
signal for the requested probe: the harness launches it two seconds later instead
of waiting out the conservative 90-second fallback.

The runner expects the local XP base disk at `qemu\winxp\my_pc_disk.qcow2`
and the qemu-libretro builds named by the selected frozen manifest. Current
XPDM validation defaults to
`guest\acceptance\frozen-inputs-ut-oracle-r5.json`, whose separately named
`xpdriver-acceptance-ut-oracle-r5.iso` contains the corrected UT observer while
retaining the exact signed `.305` package and paired core. Neither ordinary nor
`-InstallOnly` XPDM validation rebuilds that ISO; publish a new manifest outside
the validation window when guest tools intentionally change. The old
`xpdriver-acceptance.iso` and `frozen-inputs.json` remain byte-preserved for the
historical `.305` evidence. Only the legacy non-XPDM flow may build `make iso`.
Ordinary disposable runs use QEMU `-snapshot`.
Persistent XPDM runs write only to the explicitly selected copy-on-write QCOW2
overlay, so both paths leave the base disk unchanged.

After 60 seconds the runner retries the guest launch every 12 seconds until the
XP test reports `QXPICD_RESULT rc=<stage>` over the configured serial transport
or UDP fallback. XPDM runs then request an ACPI
powerdown and wait at most 45 seconds. If XP or QEMU does not honor it, the runner
uses QMP `quit`, waits another 20 seconds, and finally stops the host process if
needed. That bounded wait also applies when a busy dump callback delays or drops
the QMP acknowledgement, since the quit command may still have been accepted.
ACPI shutdown is therefore best-effort and cannot wedge the harness. The
runner requires both `rc=0` and `TEARDOWN_OK`. Host diagnostics are left in
mode-specific `qemu\winxp\automated-host[-reclaim|-idle].*.log` files so one
acceptance run does not erase another mode's evidence. XPDM debug-console output is
also copied to the corresponding `automated-host*.driver.log`, preserving the driver
trace after a later mode replaces `xpdm-debug.log`. XPDM installation is launched once.
Stage messages are retained in `xpdm-stages.log`; boot-only mode can be rerun
against the same ignored local overlay without reinstalling the package.

## Operational gotcha: WSL2 mirrored networking can wedge QMP ports

With `networkingMode=mirrored` in `.wslconfig`, a running WSL2 VM can leave
whole ranges of loopback TCP ports reserved in HNS with **no visible owner**:
`netstat -anoq`, `Get-NetTCPConnection`, Linux-side `ss`, portproxy, and the
`netsh` exclusion list all show nothing, yet binding fails with
`WSAEADDRINUSE`. The preflight's `Test-XpTestPortFree` bind probe is the only
check that sees this state, so it fails closed with
`VM_OWNERSHIP_CONFLICT` ("already bound by another frontend") even though no
frontend exists. Observed 2026-07-19: ports 44655, 44881, and 45333 all
wedged while 44605 and 49042 bound fine; `wsl --shutdown` released every one
instantly.

The wedge is transient and range-based: it (re)appears around WSL VM state
transitions and can release minutes later, and the affected ranges move
between WSL boots. Critically, **the cycle restarts WSL itself** —
`new-xpdm-overlay.ps1` calls `wsl.exe qemu-img` between the stage-1 and
stage-2 preflights — so a port that passed stage-1 can be wedged by stage-2
(observed as r1/r2/r3 of task 20260719-fear-257-install: three different
failure points, one cause).

Practical rules:

- Choose QMP ports from the **49000–49151 class** (e.g. 49042, 49058):
  below the dynamic range, outside observed wedge ranges, and proven across
  dozens of cycles that each restart WSL mid-cycle. Mid-40k ports are
  repeatedly wedged.
- Verify the planned port with a **bind probe**, not netstat — wedged ports
  are invisible to `netstat -anoq`, `Get-NetTCPConnection`, Linux `ss`,
  portproxy, and the netsh exclusion list.
- If preflight reports `VM_OWNERSHIP_CONFLICT` on a port with no visible
  owner, suspect this wedge before suspecting a leaked frontend;
  `wsl --shutdown` clears it immediately (until the next WSL start).
