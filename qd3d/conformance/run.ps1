# SPDX-License-Identifier: GPL-2.0-only
<#
.SYNOPSIS
Runs Wine's Direct3D 8/9 and DirectDraw/Direct3D 1-7 API tests against the
stock runtime and through QD3D (proxy d3d9.dll, Direct3D 8 and DirectDraw
front ends, loopback executor) and reports every behaviour that differs.

.DESCRIPTION
Build first: make -C qd3d/conformance (and qd3d/guest, qd3d/d3d8,
qd3d/ddraw, qd3d/host).

At start the runner snapshots qd3d/guest/build/d3d9.dll,
qd3d/d3d8/build/d3d8.dll, qd3d/ddraw/build/ddraw.dll,
qd3d/host/build/qd3d_loopback.exe and the test programs into
OutDir\snapshot and records their SHA-256 in manifest.json, so concurrent
rebuilds cannot change a run. Then, per test program, it runs:

  stock   the program alone in its directory (the system runtimes)
  qd3d    the program with the proxy d3d9.dll and its front end (d3d8.dll
          for Direct3D 8 tests, ddraw.dll for DirectDraw tests) beside it,
          on its own qd3d_loopback.exe channel (QD3D_TRANSPORT=loopback,
          QD3D_LOOPBACK_NAME); the executor runs with QD3DX_TRACE=1, the
          proxy with QD3D_LOG=stderr and the front end with its log
          (QD3D8_LOG, QDDRAW_LOG) in the run directory
  d3d8fe  Direct3D 8 tests only: the front end d3d8.dll on the system
          d3d9.dll, to tell front-end differences from proxy/executor ones

While qd3d/ddraw/build/ddraw.dll does not exist, the qd3d mode of the
DirectDraw tests is not run ("front end not built" in the output and in
summary.md); everything else runs.

Each program run (a segment) has a timeout, and so does each test function
in it (FunctionTimeoutSec). A segment that crashes, hangs, or (qd3d) whose
executor faults or whose proxy reports a dead channel or falls back to the
stock runtime is stopped, and the program resumes in a fresh process (and
executor) after the test function that was running
(QD3D_CONFORMANCE_AFTER, see qd3d_conformance.c).

The stock runtime cannot create devices while the input desktop is
elsewhere (locked workstation, secure desktop) although the executor's
Direct3D 9Ex can, so the runner checks with build\native_probe.exe before
and after every segment: it waits (WaitNativeSec) for the desktop to come
back and runs a segment again when the desktop went away during it. The
probe also lists the host's displays (mode and position of each); when the
list differs from the first one (a test changed the host's display
configuration) the run stops at once, and summary.md reports the lists of
the first and the last probe.

Outputs in OutDir: <test>\<mode>\segN\ (output.txt with the program's
stdout and stderr, executor.log, qd3d8.log or qddraw.log, segment.json),
results.json and summary.md: counts per test and mode, crashes and hangs,
failures and skips present only through QD3D, functions that ran fewer
checks, and failures present only natively.

-Report rebuilds results.json and summary.md from an existing OutDir.
Exit code: 0 when every run was valid, 1 when a run was invalid (proxy or
front end not loaded, executor did not start), 2 on setup errors or when
the stock runtime could not create devices.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File qd3d\conformance\run.ps1

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File qd3d\conformance\run.ps1 -Tests d3d9_device -Modes stock,qd3d -Only test_refcount,test_lights

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File qd3d\conformance\run.ps1 -Tests ddraw_ddraw7,ddraw_refcount -Modes stock,qd3d

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File qd3d\conformance\run.ps1 -Tests ddraw_ddraw4 -Modes stock,qd3d -DDrawDll qd3d\ddraw\build-core\ddraw.dll
#>
param(
    # Default: every test, in the order of $testOrder below.
    [string[]]$Tests = @(),
    # stock, qd3d, d3d8fe
    [string[]]$Modes = @('stock', 'qd3d', 'd3d8fe'),
    [string]$OutDir = (Join-Path $env:TEMP ("qd3d-conformance-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),
    # Seconds per segment; 0 selects 300 (stateblock, small DirectDraw
    # tests), 1200 (device, ddraw1-7), 2400 (visual).
    [int]$TimeoutSec = 0,
    # Seconds one test function may run before it counts as hung.
    [int]$FunctionTimeoutSec = 180,
    [int]$MaxSegments = 30,
    # Seconds to wait for the stock runtime to create devices again (see
    # native_probe.c) before giving up.
    [int]$WaitNativeSec = 600,
    # Comma-separated test functions to run (QD3D_CONFORMANCE_ONLY).
    [string]$Only = "",
    # Take the binaries from an earlier run's snapshot directory instead of
    # the build directories (to rerun exactly what that run tested).
    [string]$FromSnapshot = "",
    # DirectDraw front end to test instead of qd3d\ddraw\build\ddraw.dll
    # (another build directory's ddraw.dll).
    [string]$DDrawDll = "",
    [switch]$Report
)
$ErrorActionPreference = "Stop"
$conf = $PSScriptRoot
$qd3d = Split-Path -Parent $conf
$script:live = New-Object System.Collections.Generic.List[System.Diagnostics.Process]
$allModes = @('stock', 'qd3d', 'd3d8fe')
$testOrder = @('d3d9_stateblock', 'd3d8_stateblock', 'd3d9_device', 'd3d8_device', 'd3d9_visual', 'd3d8_visual',
    'ddraw_refcount', 'ddraw_dsurface', 'ddraw_d3d', 'ddraw_ddrawmodes', 'ddraw_visual',
    'ddraw_ddraw1', 'ddraw_ddraw2', 'ddraw_ddraw4', 'ddraw_ddraw7')
# Front ends the qd3d mode puts beside a test program, by test prefix: the
# DLL, its build product, the variable naming its log file, the log file in
# the run directory and the prefix of its log lines. "<prefix>: loaded
# <path>" names the d3d9.dll it loaded.
$frontEnds = @{
    d3d8  = @{ dll = 'd3d8.dll'; build = 'd3d8\build\d3d8.dll'; logVar = 'QD3D8_LOG'; log = 'qd3d8.log'; prefix = 'qd3d8' }
    ddraw = @{ dll = 'ddraw.dll'; build = 'ddraw\build\ddraw.dll'; logVar = 'QDDRAW_LOG'; log = 'qddraw.log'; prefix = 'qddraw' }
}
# powershell -File passes "a,b" as one string.
$Tests = @($Tests | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Tests.Count) { $Tests = $testOrder }
if ($DDrawDll -and (Test-Path -LiteralPath $DDrawDll)) { $DDrawDll = (Resolve-Path -LiteralPath $DDrawDll).ProviderPath }
$Modes = @($Modes | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($m in $Modes) {
    if ($allModes -notcontains $m) { [Console]::Out.WriteLine("unknown mode $m"); exit 2 }
}
foreach ($t in $Tests) {
    if ($testOrder -notcontains $t) { [Console]::Out.WriteLine("unknown test $t"); exit 2 }
}

# Progress goes straight to stdout so that it never mixes into function results.
function Write-Step([string]$msg) {
    [Console]::Out.WriteLine(("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg))
}

function Get-Sha256([string]$path) {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

# Copies a build product that another agent may be relinking right now: the
# copy is kept only when the source is unchanged across the copy.
function Copy-Stable([string]$src, [string]$dstDir) {
    if (-not (Test-Path -LiteralPath $src)) { throw "missing $src (build it first)" }
    $dst = Join-Path $dstDir (Split-Path -Leaf $src)
    for ($i = 0; $i -lt 20; $i++) {
        try {
            $before = Get-Sha256 $src
            Copy-Item -LiteralPath $src -Destination $dst -Force
            Start-Sleep -Milliseconds 500
            if ((Get-Sha256 $src) -eq $before -and (Get-Sha256 $dst) -eq $before) {
                $item = Get-Item -LiteralPath $src
                return [ordered]@{
                    name = (Split-Path -Leaf $src); source = $src; sha256 = $before
                    size = $item.Length; modified = $item.LastWriteTime.ToString("o")
                }
            }
        } catch {
            Start-Sleep -Seconds 1
        }
    }
    throw "cannot take a stable copy of $src"
}

# Complete new lines of a file another process is still writing.
$script:tailPos = @{}
$script:tailRest = @{}
function Read-NewLines([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $pos = 0L
    if ($script:tailPos.ContainsKey($path)) { $pos = $script:tailPos[$path] }
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        if ($fs.Length -le $pos) { return "" }
        $null = $fs.Seek($pos, 'Begin')
        $buf = New-Object byte[] ($fs.Length - $pos)
        $n = $fs.Read($buf, 0, $buf.Length)
        $script:tailPos[$path] = $pos + $n
        $text = [System.Text.Encoding]::Default.GetString($buf, 0, $n)
    } finally {
        $fs.Dispose()
    }
    if ($script:tailRest.ContainsKey($path)) { $text = $script:tailRest[$path] + $text }
    $cut = $text.LastIndexOf("`n")
    $script:tailRest[$path] = $text.Substring($cut + 1)
    return $text.Substring(0, $cut + 1)
}

function Stop-Tree([System.Diagnostics.Process]$p) {
    if ($p -and -not $p.HasExited) {
        & taskkill.exe /T /F /PID $p.Id 2>&1 | Out-Null
        $null = $p.WaitForExit(15000)
    }
}

function Get-DefaultTimeout([string]$test) {
    if ($test -like "*_stateblock") { return 300 }
    if ($test -like "*_device" -or $test -like "ddraw_ddraw*") { return 1200 }
    if ($test -like "ddraw_*") { return 300 }
    return 2400
}

# d3d9, d3d8 or ddraw: the test program's API.
function Get-Kind([string]$test) {
    return $test.Split('_')[0]
}

$reDeath = [regex]'(?m)^qd3d9: (channel dead|HELLO failed|stock runtime .* not loadable)[^\r\n]*'
# A front end that sent a call to the stock runtime (QD3D8_LOG, QDDRAW_LOG).
$reFallback = [regex]'(?m)^\w+: [^\r\n]*(no QD3D session|-> stock runtime)[^\r\n]*'

# ---------------------------------------------------------------------------
# Running

function Invoke-Segment($snap, [string]$test, [string]$mode, [int]$index, [string]$after, [int]$timeout, [string]$dir) {
    $kind = Get-Kind $test
    # The front end this run puts beside the program (qd3d, d3d8fe), if any.
    $front = if ($mode -ne "stock" -and $frontEnds.ContainsKey($kind)) { $frontEnds[$kind] } else { $null }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item -LiteralPath (Join-Path $snap "$test.exe") -Destination $dir
    if ($mode -eq "qd3d") { Copy-Item -LiteralPath (Join-Path $snap "d3d9.dll") -Destination $dir }
    if ($front) { Copy-Item -LiteralPath (Join-Path $snap $front.dll) -Destination $dir }
    $out = Join-Path $dir "output.txt"
    $hlog = Join-Path $dir "executor.log"
    $fe = if ($front) { Join-Path $dir $front.log } else { "" }
    $seg = [ordered]@{
        test = $test; mode = $mode; index = $index; after = $after; status = ""; reason = ""
        broken_fn = ""; exit_code = $null; seconds = 0.0; channel = ""
    }

    $harness = $null
    if ($mode -eq "qd3d") {
        $seg.channel = "Local\QD3D_CONF_" + [guid]::NewGuid().ToString("N").Substring(0, 12)
        $env:QD3DX_TRACE = "1"
        try {
            $harness = Start-Process -FilePath (Join-Path $snap "qd3d_loopback.exe") `
                -ArgumentList @("--name", $seg.channel, "--timeout", "$($timeout + 120)") `
                -RedirectStandardOutput $hlog -RedirectStandardError (Join-Path $dir "executor.err") `
                -PassThru -WindowStyle Minimized
        } finally {
            Remove-Item Env:\QD3DX_TRACE -ErrorAction SilentlyContinue
        }
        $null = $harness.Handle
        $script:live.Add($harness)
        $deadline = (Get-Date).AddSeconds(30)
        while (-not ((Test-Path -LiteralPath $hlog) -and
                     (Select-String -LiteralPath $hlog -SimpleMatch "waiting for a guest" -Quiet))) {
            if ($harness.HasExited -or (Get-Date) -gt $deadline) {
                Stop-Tree $harness
                $seg.status = "invalid"
                $seg.reason = "executor did not start"
                $seg | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dir "segment.json") -Encoding UTF8
                return $seg
            }
            Start-Sleep -Milliseconds 100
        }
    }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /s /c ""' + (Join-Path $dir "$test.exe") + '" > "' + $out + '" 2>&1"'
    $psi.WorkingDirectory = $dir
    $psi.UseShellExecute = $false
    foreach ($k in @("QD3D_TRANSPORT", "QD3D_LOOPBACK_NAME", "QD3D_LOG", "QD3D_TIMEOUT_MS", "QD3D8_LOG",
                     "QDDRAW_LOG", "QDDRAW_VIRTUAL_MODES",
                     "QD3D_CONFORMANCE_AFTER", "QD3D_CONFORMANCE_ONLY", "QD3DX_TRACE",
                     "WINETEST_PLATFORM", "WINETEST_DEBUG", "WINETEST_INTERACTIVE", "WINETEST_REPORT_SUCCESS",
                     "WINETEST_REPORT_FLAKY", "WINETEST_TIME", "WINETEST_MUTE_THRESHOLD", "WINETEST_COLOR")) {
        $psi.EnvironmentVariables.Remove($k)
    }
    $psi.EnvironmentVariables["WINETEST_COLOR"] = "0"
    if ($after) { $psi.EnvironmentVariables["QD3D_CONFORMANCE_AFTER"] = $after }
    if ($Only) { $psi.EnvironmentVariables["QD3D_CONFORMANCE_ONLY"] = $Only }
    if ($mode -eq "qd3d") {
        $psi.EnvironmentVariables["QD3D_TRANSPORT"] = "loopback"
        $psi.EnvironmentVariables["QD3D_LOOPBACK_NAME"] = $seg.channel
        $psi.EnvironmentVariables["QD3D_LOG"] = "stderr"
    }
    if ($front) { $psi.EnvironmentVariables[$front.logVar] = $fe }
    # The DirectDraw tests that change display modes are compiled out (see
    # ../third_party/wine/README.md); this keeps the front end's
    # SetDisplayMode off the real display should one still be reached.
    if ($mode -eq "qd3d" -and $kind -eq "ddraw") { $psi.EnvironmentVariables["QDDRAW_VIRTUAL_MODES"] = "1" }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = [System.Diagnostics.Process]::Start($psi)
    $null = $p.Handle
    $script:live.Add($p)
    $reason = ""
    $lastPoll = 0L
    $fnStart = 0.0
    $curFn = ""
    while (-not $p.WaitForExit(250)) {
        $now = $sw.Elapsed.TotalSeconds
        if ($now -ge $timeout) { $reason = "timeout after $timeout s"; break }
        if ($sw.ElapsedMilliseconds - $lastPoll -lt 1000) { continue }
        $lastPoll = $sw.ElapsedMilliseconds
        $o = Read-NewLines $out
        $marks = [regex]::Matches($o, "(?m)^qd3d-conformance: run (\w+)")
        if ($marks.Count) { $curFn = $marks[$marks.Count - 1].Groups[1].Value; $fnStart = $now }
        if ($curFn -and $now - $fnStart -ge $FunctionTimeoutSec) {
            $reason = "timeout: $curFn ran longer than $FunctionTimeoutSec s"; break
        }
        if ($mode -ne "qd3d") { continue }
        $m = [regex]::Match((Read-NewLines $hlog), "qd3dx: FAULT[^\r\n]*")
        if ($m.Success) { $reason = "executor " + $m.Value.Substring(7); break }
        $m = $reDeath.Match($o)
        if ($m.Success) { $reason = $m.Value.Substring(7); break }
        if ($front) {
            $m = $reFallback.Match((Read-NewLines $fe))
            if ($m.Success) { $reason = "front end: " + $m.Value; break }
        }
        if ($harness.HasExited) { $reason = "executor exited with $($harness.ExitCode)"; break }
    }
    if ($reason) { Stop-Tree $p }
    $p.WaitForExit()
    $seg.seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $seg.exit_code = $p.ExitCode
    if ($harness) {
        Start-Sleep -Milliseconds 300
        if (-not $harness.HasExited) { Stop-Process -Id $harness.Id -Force -ErrorAction SilentlyContinue }
        $harness.WaitForExit()
    }

    # Where the program was: the last test function it started, or the one
    # in which the proxy lost the executor.
    $lines = if (Test-Path -LiteralPath $out) { [System.IO.File]::ReadAllLines($out, [System.Text.Encoding]::Default) } else { @() }
    $fn = ""
    $deathFn = ""
    $death = ""
    $completed = $false
    $crash = ""
    $connected = $false
    foreach ($line in $lines) {
        if ($line.StartsWith("qd3d-conformance: run ")) { $fn = $line.Split(" ")[2]; continue }
        if (-not $death -and $reDeath.IsMatch($line)) { $death = $line.Substring(7); $deathFn = $fn; continue }
        if ($line -match '^[0-9a-f]+:\w+:[\d.]* \d+ tests executed') { $completed = $true; continue }
        if ($line -match '^[0-9a-f]+:\w+:[\d.]* unhandled exception ([0-9a-f]{8})') { $crash = $Matches[1]; continue }
        if ($line.StartsWith("qd3d9: connected (loopback)")) { $connected = $true }
    }
    $fault = ""
    if (Test-Path -LiteralPath $hlog) {
        $f = Select-String -LiteralPath $hlog -Pattern "^qd3dx: FAULT.*" | Select-Object -First 1
        if ($f) { $fault = $f.Line.Substring(7) }
    }
    if ($reason -like "timeout*") {
        $seg.status = "timeout"; $seg.reason = $reason; $seg.broken_fn = $fn
    } elseif ($reason -or $death -or $fault) {
        # Stopped here, or it ran on with a dead channel: resume after the
        # function where the channel died.
        $seg.status = "aborted"
        $seg.reason = if ($reason) { $reason } elseif ($fault) { "executor $fault" } else { $death }
        $seg.broken_fn = if ($deathFn) { $deathFn } else { $fn }
    } elseif ($crash) {
        $seg.status = "crash"; $seg.reason = "unhandled exception $crash"; $seg.broken_fn = $fn
    } elseif ($completed) {
        $seg.status = "completed"
    } else {
        $seg.status = "crash"; $seg.reason = "exit code 0x{0:x8} without a summary line" -f $p.ExitCode
        $seg.broken_fn = $fn
    }

    # Proof that the run used what it was meant to use.
    $ran = $lines -match '^qd3d-conformance: run '
    if ($mode -eq "qd3d") {
        $hello = (Test-Path -LiteralPath $hlog) -and (Select-String -LiteralPath $hlog -Pattern "^qd3dx: HELLO pid" -Quiet)
        $seg.proxy_connected = $connected
        $seg.executor_hello = [bool]$hello
        if ($ran -and -not ($connected -and $hello)) {
            $seg.status = "invalid"; $seg.reason = "proxy not connected to the executor"
        }
    }
    if ($front) {
        $pat = "^" + [regex]::Escape($front.prefix) + ": loaded (.*)$"
        $loaded = if (Test-Path -LiteralPath $fe) { Select-String -LiteralPath $fe -Pattern $pat | Select-Object -First 1 } else { $null }
        $seg.front_end_d3d9 = if ($loaded) { $loaded.Matches[0].Groups[1].Value.Trim() } else { "" }
        $local = [string]::Equals($seg.front_end_d3d9, (Join-Path $dir "d3d9.dll"), [StringComparison]::OrdinalIgnoreCase)
        if ($ran -and (-not $seg.front_end_d3d9 -or $local -ne ($mode -eq "qd3d"))) {
            $seg.status = "invalid"; $seg.reason = "front end loaded d3d9 '$($seg.front_end_d3d9)'"
        }
        # A fallback the polling above did not see before the program ended.
        $fb = if ($mode -eq "qd3d" -and (Test-Path -LiteralPath $fe)) { $reFallback.Match([System.IO.File]::ReadAllText($fe)) } else { $null }
        if ($fb -and $fb.Success -and $seg.status -ne "aborted") {
            $seg.status = "invalid"; $seg.reason = "front end: $($fb.Value)"
        }
    }
    $seg | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dir "segment.json") -Encoding UTF8
    $script:live.Remove($p) | Out-Null
    if ($harness) { $script:live.Remove($harness) | Out-Null }
    return $seg
}

# Whether the stock runtime can create devices now (native_probe.c): it
# cannot while the input desktop is elsewhere (locked workstation, secure
# desktop), where the QD3D executor's Direct3D 9Ex still can.
#
# The probe also lists the host's displays with their modes and positions;
# the first list is kept in displays.txt, and any later difference (a test
# changed the host's display configuration) stops the run.
function Test-Native($snap) {
    $txt = Join-Path $OutDir "native_probe.txt"
    $p = Start-Process -FilePath (Join-Path $snap "native_probe.exe") -WorkingDirectory $snap -PassThru `
        -NoNewWindow -Wait -RedirectStandardOutput $txt
    $m = [regex]::Match([System.IO.File]::ReadAllText($txt), '(?m)^native_probe: displays(.*?)\r?$')
    $now = if ($m.Success) { $m.Groups[1].Value.Trim() } else { "(no display list)" }
    $first = Join-Path $OutDir "displays.txt"
    if (-not (Test-Path -LiteralPath $first)) {
        Set-Content -LiteralPath $first -Value $now -Encoding UTF8
    } else {
        $was = (Get-Content -LiteralPath $first -Raw).Trim()
        if ($now -ne $was) { throw "the host display configuration changed: '$was' -> '$now'; run stopped" }
    }
    return $p.ExitCode -eq 0
}

function Wait-Native($snap) {
    if (Test-Native $snap) { return $true }
    Write-Step "the stock runtime cannot create devices ($((Get-Content -LiteralPath (Join-Path $OutDir 'native_probe.txt') -Raw).Trim())); input desktop unavailable? waiting up to $WaitNativeSec s"
    $deadline = (Get-Date).AddSeconds($WaitNativeSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 15
        if (Test-Native $snap) { Write-Step "the stock runtime creates devices again"; return $true }
    }
    return $false
}

function Invoke-TestMode($snap, [string]$test, [string]$mode) {
    $timeout = if ($TimeoutSec -gt 0) { $TimeoutSec } else { Get-DefaultTimeout $test }
    $after = ""
    $lost = 0
    for ($i = 0; $i -lt $MaxSegments; $i++) {
        if (-not (Wait-Native $snap)) { throw "the stock runtime cannot create devices; run stopped" }
        $dir = Join-Path $OutDir (Join-Path $test (Join-Path $mode "seg$i"))
        $seg = Invoke-Segment $snap $test $mode $i $after $timeout $dir
        if (-not (Test-Native $snap)) {
            # The desktop went away during the segment: discard it and run it again.
            $lost++
            Rename-Item -LiteralPath $dir -NewName "seg$i.desktop-lost$lost"
            Write-Step ("{0} {1} seg{2}: discarded, the stock runtime lost the desktop during it" -f $test, $mode, $i)
            if ($lost -ge 3) { throw "the desktop went away three times; run stopped" }
            $i--
            continue
        }
        $note = if ($seg.reason) { " ($($seg.reason))" } else { "" }
        $where = if ($seg.broken_fn) { " in $($seg.broken_fn)" } else { "" }
        Write-Step ("{0} {1} seg{2}: {3}{4}{5}, {6} s" -f $test, $mode, $i, $seg.status, $where, $note, $seg.seconds)
        if ($seg.status -eq "completed" -or $seg.status -eq "invalid" -or -not $seg.broken_fn) { return }
        $after = $seg.broken_fn
    }
}

# ---------------------------------------------------------------------------
# Parsing and comparison

$reResult = [regex]'^([\w.\-]+):(\d+):[\d.]* (Test failed|Test succeeded inside todo block|Test succeeded inside flaky todo block|Test marked flaky|Tests skipped): (.*)$'
$reMarker = [regex]'^qd3d-conformance: run (\w+) at (\d+) ms, (\d+) executed, (\d+) failed$'
$reSummary = [regex]'^[0-9a-f]+:\w+:[\d.]* (\d+) tests executed \((\d+) marked as todo, (\d+) as flaky, (\d+) failures?\), (\d+) skipped\.$'
$reCrash = [regex]'^[0-9a-f]+:\w+:[\d.]* unhandled exception ([0-9a-f]{8}) at (\S+)$'
$reLast = [regex]'^([\w.\-]+):(\d+): this is the last test seen before the exception$'
# %p output (8 hex digits, upper case, at least one letter); colours and
# HRESULTs are printed in lower case.
$rePtr = [regex]'\b(?=[0-9A-F]*[A-F])[0-9A-F]{8}\b'

function Read-Segment([string]$dir) {
    $seg = Get-Content -LiteralPath (Join-Path $dir "segment.json") -Raw | ConvertFrom-Json
    $results = New-Object System.Collections.Generic.List[object]
    $functions = New-Object System.Collections.Generic.List[string]
    # Checks each test function executed (-1: it did not finish).
    $checks = @{}
    $mark = 0
    $guest = New-Object System.Collections.Generic.List[string]
    $summary = $null
    $crash = $null
    $lastLoc = ""
    $fn = "(prologue)"
    $out = Join-Path $dir "output.txt"
    $lines = if (Test-Path -LiteralPath $out) { [System.IO.File]::ReadAllLines($out, [System.Text.Encoding]::Default) } else { @() }
    foreach ($line in $lines) {
        $m = $reResult.Match($line)
        if ($m.Success) {
            $k = switch ($m.Groups[3].Value) {
                "Test failed" { "fail" }
                "Test succeeded inside todo block" { "todo-pass" }
                "Tests skipped" { "skip" }
                default { "flaky" }
            }
            $msg = $m.Groups[4].Value.Trim()
            if ($k -eq "skip" -and $msg.StartsWith("qd3d conformance: ")) { $k = "excluded" }
            $results.Add([pscustomobject]@{
                fn = $fn; file = $m.Groups[1].Value; line = [int]$m.Groups[2].Value; kind = $k
                msg = $msg; norm = $rePtr.Replace($msg, "<ptr>"); segment = [int]$seg.index
            })
            continue
        }
        $m = $reMarker.Match($line)
        if ($m.Success) {
            $n = [int]$m.Groups[3].Value
            if ($functions.Count) { $checks[$fn] = $n - $mark }
            $fn = $m.Groups[1].Value
            $functions.Add($fn)
            $checks[$fn] = -1
            $mark = $n
            continue
        }
        $m = $reSummary.Match($line)
        if ($m.Success) {
            $summary = [pscustomobject]@{
                executed = [int]$m.Groups[1].Value; todo = [int]$m.Groups[2].Value; flaky = [int]$m.Groups[3].Value
                failures = [int]$m.Groups[4].Value; skipped = [int]$m.Groups[5].Value
            }
            if ($functions.Count) { $checks[$fn] = $summary.executed - $mark }
            continue
        }
        $m = $reLast.Match($line)
        if ($m.Success) { $lastLoc = "$($m.Groups[1].Value):$($m.Groups[2].Value)"; continue }
        $m = $reCrash.Match($line)
        if ($m.Success) {
            $crash = [pscustomobject]@{ code = $m.Groups[1].Value; address = $m.Groups[2].Value; last_check = $lastLoc; fn = $fn }
            continue
        }
        if ($line.StartsWith("qd3d9: ") -and -not $line.StartsWith("qd3d9: connected")) { $guest.Add("[$fn] $($line.Substring(7))") }
    }
    $executor = New-Object System.Collections.Generic.List[string]
    $hlog = Join-Path $dir "executor.log"
    if (Test-Path -LiteralPath $hlog) {
        foreach ($line in [System.IO.File]::ReadAllLines($hlog)) {
            if ($line -match "^qd3dx: (FAULT|[\w:]+ on 0x[0-9a-f]+ returned|.*fail)") { $executor.Add($line.Substring(7)) }
        }
    }
    $front = New-Object System.Collections.Generic.List[string]
    foreach ($fe in $frontEnds.Values) {
        $felog = Join-Path $dir $fe.log
        if (-not (Test-Path -LiteralPath $felog)) { continue }
        foreach ($line in [System.IO.File]::ReadAllLines($felog)) {
            if ($line -notmatch "^(qd3d8|qddraw): (loaded|Direct3DCreate8)") { $front.Add($line) }
        }
    }
    [pscustomobject]@{
        info = $seg; dir = $dir; results = $results; functions = $functions; checks = $checks
        executed = $(if ($summary) { $summary.executed } else { $mark })
        summary = $summary; crash = $crash; guest = $guest; executor = $executor; front = $front
    }
}

function Read-Run([string]$test, [string]$mode) {
    $root = Join-Path $OutDir (Join-Path $test $mode)
    if (-not (Test-Path -LiteralPath $root)) { return $null }
    $segs = @(Get-ChildItem -LiteralPath $root -Directory | Where-Object { $_.Name -match '^seg\d+$' } |
        Sort-Object { [int]$_.Name.Substring(3) } | ForEach-Object { Read-Segment $_.FullName })
    if (-not $segs.Count) { return $null }
    $results = New-Object System.Collections.Generic.List[object]
    $ran = @{}
    $checks = @{}
    $executed = 0
    $partial = $false
    $seconds = 0.0
    foreach ($s in $segs) {
        foreach ($r in $s.results) { $results.Add($r) }
        foreach ($f in $s.functions) { $ran[$f] = $true; $checks[$f] = $s.checks[$f] }
        $executed += $s.executed
        if (-not $s.summary) { $partial = $true }
        $seconds += [double]$s.info.seconds
    }
    $count = @{ fail = 0; skip = 0; flaky = 0 }
    # Each exclusion once: a resumed segment prints the ones before it again.
    $excluded = @{}
    foreach ($r in $results) {
        if ($r.kind -eq "fail" -or $r.kind -eq "todo-pass") { $count.fail++ }
        elseif ($r.kind -eq "skip") { $count.skip++ }
        elseif ($r.kind -eq "flaky") { $count.flaky++ }
        elseif ($r.kind -eq "excluded") { $excluded["$($r.file):$($r.line)"] = $true }
    }
    [pscustomobject]@{
        test = $test; mode = $mode; segments = $segs; results = $results; ran = $ran; checks = $checks
        executed = $executed; executed_partial = $partial; seconds = $seconds
        failures = $count.fail; skips = $count.skip; flaky = $count.flaky; excluded = $excluded.Count
        broken = @($segs | Where-Object { $_.info.status -ne "completed" })
        invalid = @($segs | Where-Object { $_.info.status -eq "invalid" }).Count -gt 0
    }
}

function Group-Results($run, [string[]]$kinds) {
    $g = @{}
    if (-not $run) { return $g }
    foreach ($r in $run.results) {
        if ($kinds -notcontains $r.kind) { continue }
        $key = "$($r.file):$($r.line)"
        if (-not $g.ContainsKey($key)) {
            $g[$key] = [pscustomobject]@{ key = $key; file = $r.file; line = $r.line; fn = $r.fn
                count = 0; norms = @{}; example = $r.msg }
        }
        $e = $g[$key]
        $e.count++
        $e.norms[$r.norm] = $true
    }
    return $g
}

# Entries of $other that $base lacks: a new line, a new message on a line,
# or more occurrences of the same messages.
function Compare-Groups($base, $other) {
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($e in $other.Values) {
        $b = if ($base.ContainsKey($e.key)) { $base[$e.key] } else { $null }
        $kind = ""
        if (-not $b) {
            $kind = "new"
        } else {
            $newNorms = @($e.norms.Keys | Where-Object { -not $b.norms.ContainsKey($_) })
            if ($newNorms.Count) { $kind = "changed" } elseif ($e.count -gt $b.count) { $kind = "more" }
        }
        if (-not $kind) { continue }
        $list.Add([pscustomobject]@{
            key = $e.key; file = $e.file; line = $e.line; fn = $e.fn; kind = $kind; count = $e.count
            base_count = $(if ($b) { $b.count } else { 0 }); example = $e.example
            base_example = $(if ($b) { $b.example } else { "" })
        })
    }
    return @($list | Sort-Object fn, file, line)
}

function Format-Md([string]$s) {
    $s = $s.Trim() -replace '\|', '\|'
    if ($s.Length -gt 220) { $s = $s.Substring(0, 217) + "..." }
    return $s
}

function Write-Report {
    $runs = @{}
    $found = @(Get-ChildItem -LiteralPath $OutDir -Directory | Where-Object { $_.Name -match '^(d3d[89]|ddraw)_\w+$' } |
        ForEach-Object { $_.Name })
    $tests = @($testOrder | Where-Object { $found -contains $_ }) + @($found | Where-Object { $testOrder -notcontains $_ })
    foreach ($t in $tests) {
        foreach ($m in $allModes) {
            $r = Read-Run $t $m
            if ($r) { $runs["$t/$m"] = $r }
        }
    }
    $md = New-Object System.Collections.Generic.List[string]
    $json = [ordered]@{ out_dir = $OutDir; tests = [ordered]@{} }
    $manifest = Join-Path $OutDir "manifest.json"
    $md.Add("# QD3D Wine conformance run")
    $md.Add("")
    $md.Add("Run directory: ``$OutDir``")
    if (Test-Path -LiteralPath $manifest) {
        $man = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        $md.Add("")
        $md.Add("Started $($man.started); $($man.os); display adapters: $($man.gpus); Wine tests $($man.wine_tag).")
        if ($man.only) { $md.Add(""); $md.Add("Only: ``$($man.only)``") }
        $md.Add("")
        foreach ($f in $man.files) { $md.Add("- ``$($f.name)`` sha256 ``$($f.sha256)`` ($($f.modified)) from ``$($f.source)``") }
        # Runs the runner could not make, e.g. "ddraw_ddraw7 qd3d: front end not built".
        $notRun = @($man.not_run | Where-Object { $_ })
        if ($notRun.Count) {
            $md.Add("")
            foreach ($n in $notRun) { $md.Add("Not run: $n") }
            $json["not_run"] = $notRun
        }
    }
    # The host's displays at the first and the last probe (see Test-Native).
    $first = Join-Path $OutDir "displays.txt"
    if (Test-Path -LiteralPath $first) {
        $was = (Get-Content -LiteralPath $first -Raw).Trim()
        $last = Join-Path $OutDir "native_probe.txt"
        $m = if (Test-Path -LiteralPath $last) { [regex]::Match([System.IO.File]::ReadAllText($last), '(?m)^native_probe: displays(.*?)\r?$') } else { $null }
        $now = if ($m -and $m.Success) { $m.Groups[1].Value.Trim() } else { "(no display list)" }
        $state = if ($now -eq $was) { "unchanged" } else { "CHANGED to ``$now``" }
        $md.Add("")
        $md.Add("Host displays at the first probe: ``$was``; at the last probe: $state.")
        $json["displays"] = [ordered]@{ first = $was; last = $now }
    }
    $md.Add("")
    $md.Add("## Counts")
    $md.Add("")
    $md.Add("Failures: ``Test failed`` and ``Test succeeded inside todo block`` lines. Skips: ``Tests skipped`` lines other than the build's exclusions (Excluded). Executed: sum of the summary lines (``+``: a segment ended without one).")
    $md.Add("")
    $md.Add("| Test | Mode | Executed | Failures | Skips | Flaky | Excluded | Segments | Problems | Seconds |")
    $md.Add("|---|---|---:|---:|---:|---:|---:|---:|---|---:|")
    foreach ($t in $tests) {
        foreach ($m in $allModes) {
            if (-not $runs.ContainsKey("$t/$m")) { continue }
            $r = $runs["$t/$m"]
            $probs = @($r.broken | ForEach-Object {
                $where = if ($_.info.broken_fn) { " in $($_.info.broken_fn)" } else { "" }
                "$($_.info.status)$where" + $(if ($_.info.reason) { " ($($_.info.reason))" } else { "" }) })
            $exec = "$($r.executed)" + $(if ($r.executed_partial) { "+" } else { "" })
            $md.Add("| $t | $m | $exec | $($r.failures) | $($r.skips) | $($r.flaky) | $($r.excluded) | $($r.segments.Count) | $(Format-Md ($probs -join '; ')) | $([math]::Round($r.seconds)) |")
        }
    }

    $md.Add("")
    $md.Add("## Crashes, hangs, executor faults")
    $md.Add("")
    $any = $false
    foreach ($t in $tests) {
        foreach ($m in $allModes) {
            if (-not $runs.ContainsKey("$t/$m")) { continue }
            foreach ($s in $runs["$t/$m"].broken) {
                $any = $true
                $c = if ($s.crash) { "; code $($s.crash.code) at $($s.crash.address), last check $($s.crash.last_check)" } else { "" }
                $md.Add("- **$t $m** seg$($s.info.index) ``$($s.info.broken_fn)``: $($s.info.status), $($s.info.reason)$c")
                foreach ($g in ($s.guest | Select-Object -First 5)) { $md.Add("  - proxy: ``$(Format-Md $g)``") }
                foreach ($x in ($s.executor | Where-Object { $_ -match 'FAULT' } | Select-Object -First 3)) { $md.Add("  - executor: ``$(Format-Md $x)``") }
            }
        }
    }
    if (-not $any) { $md.Add("None.") }

    foreach ($t in $tests) {
        $stock = if ($runs.ContainsKey("$t/stock")) { $runs["$t/stock"] } else { $null }
        $tj = [ordered]@{}
        foreach ($m in @('qd3d', 'd3d8fe')) {
            if (-not $runs.ContainsKey("$t/$m") -or -not $stock) { continue }
            $other = $runs["$t/$m"]
            $fe = $null
            if ($m -eq "qd3d" -and $runs.ContainsKey("$t/d3d8fe")) { $fe = Group-Results $runs["$t/d3d8fe"] @('fail', 'todo-pass') }
            $stockFail = Group-Results $stock @('fail', 'todo-pass')
            $otherFail = Group-Results $other @('fail', 'todo-pass')
            $diff = Compare-Groups $stockFail $otherFail
            $skipDiff = Compare-Groups (Group-Results $stock @('skip')) (Group-Results $other @('skip'))
            $lost = Compare-Groups $otherFail $stockFail
            $notRun = @($stock.ran.Keys | Where-Object { -not $other.ran.ContainsKey($_) } | Sort-Object)
            # Functions that stopped early (a skip or return after a failure)
            # or did not finish.
            $short = New-Object System.Collections.Generic.List[object]
            foreach ($f in $stock.checks.Keys) {
                if (-not $other.checks.ContainsKey($f)) { continue }
                $a = [int]$stock.checks[$f]
                $b = [int]$other.checks[$f]
                if ($a -lt 0) { continue }
                if ($b -lt 0 -or ($b -le $a - 5 -and $b -lt 0.9 * $a)) {
                    $short.Add([pscustomobject]@{ fn = $f; stock = $a; other = $b })
                }
            }
            $short = @($short | Sort-Object fn)
            $tj[$m] = [ordered]@{ failures = $diff; skips = $skipDiff; stock_only = $lost; not_run = $notRun; fewer_checks = $short }

            $md.Add("")
            $md.Add("## $t`: $m vs stock")
            $md.Add("")
            if ($notRun.Count) {
                $md.Add("Not run in $m (crash or abort before them): " + (($notRun | ForEach-Object { "``$_``" }) -join ", "))
                $md.Add("")
            }
            $md.Add("### Failures only in $m ($($diff.Count) lines)")
            $md.Add("")
            if ($diff.Count) {
                $feHead = if ($fe) { " Front end on stock d3d9 |" } else { "" }
                $md.Add("| Function | Line | Kind | $m | stock | $m message | stock message |$feHead")
                $md.Add("|---|---|---|---:|---:|---|---|" + $(if ($fe) { "---|" } else { "" }))
                foreach ($d in $diff) {
                    $feCol = ""
                    if ($fe) { $feCol = if ($fe.ContainsKey($d.key)) { " fails ($($fe[$d.key].count)) |" } else { " passes |" } }
                    $md.Add("| ``$($d.fn)`` | $($d.key) | $($d.kind) | $($d.count) | $($d.base_count) | $(Format-Md $d.example) | $(Format-Md $d.base_example) |$feCol")
                }
            } else { $md.Add("None.") }
            $md.Add("")
            $md.Add("### Functions with fewer checks in $m ($($short.Count))")
            $md.Add("")
            if ($short.Count) {
                $md.Add(($short | ForEach-Object {
                    "``$($_.fn)`` " + $(if ($_.other -lt 0) { "did not finish" } else { "$($_.other)" }) + " vs $($_.stock)" }) -join "; ")
            } else { $md.Add("None.") }
            $md.Add("")
            $md.Add("### Skips only in $m ($($skipDiff.Count) lines)")
            $md.Add("")
            if ($skipDiff.Count) {
                foreach ($d in $skipDiff) { $md.Add("- ``$($d.fn)`` $($d.key) ($($d.kind), $($d.count)x): $(Format-Md $d.example)") }
            } else { $md.Add("None.") }
            $md.Add("")
            $md.Add("### Failures only in stock ($($lost.Count) lines, not findings)")
            $md.Add("")
            if ($lost.Count) {
                foreach ($d in $lost) { $md.Add("- ``$($d.fn)`` $($d.key) ($($d.kind), $($d.count)x): $(Format-Md $d.example)") }
            } else { $md.Add("None.") }
            $diag = New-Object System.Collections.Generic.List[string]
            foreach ($s in $other.segments) {
                foreach ($x in $s.guest) { if (-not $diag.Contains("proxy: $x")) { $diag.Add("proxy: $x") } }
                foreach ($x in $s.front) { if (-not $diag.Contains("front end: $x")) { $diag.Add("front end: $x") } }
                foreach ($x in $s.executor) { if (-not $diag.Contains("executor: $x")) { $diag.Add("executor: $x") } }
            }
            if ($diag.Count) {
                $md.Add("")
                $md.Add("### Proxy, front end and executor diagnostics ($m, first 80)")
                $md.Add("")
                foreach ($x in ($diag | Select-Object -First 80)) { $md.Add("- ``$(Format-Md $x)``") }
            }
        }
        if ($stock) {
            $tj["stock_failures"] = @((Group-Results $stock @('fail', 'todo-pass')).Values | Sort-Object fn, file, line |
                ForEach-Object { [ordered]@{ key = $_.key; fn = $_.fn; count = $_.count; example = $_.example } })
        }
        $tj["runs"] = [ordered]@{}
        foreach ($m in $allModes) {
            if (-not $runs.ContainsKey("$t/$m")) { continue }
            $r = $runs["$t/$m"]
            $tj["runs"][$m] = [ordered]@{
                executed = $r.executed; executed_partial = $r.executed_partial; failures = $r.failures
                skips = $r.skips; flaky = $r.flaky; excluded = $r.excluded; seconds = $r.seconds
                segments = @($r.segments | ForEach-Object { $_.info })
                crashes = @($r.segments | Where-Object { $_.crash } | ForEach-Object { $_.crash })
            }
        }
        $json.tests[$t] = $tj
    }
    $md | Set-Content -LiteralPath (Join-Path $OutDir "summary.md") -Encoding UTF8
    $json | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutDir "results.json") -Encoding UTF8
    Write-Step "summary: $(Join-Path $OutDir 'summary.md')"
    return @($runs.Values | Where-Object { $_.invalid }).Count
}

# ---------------------------------------------------------------------------

if ($Report) {
    if (-not (Test-Path -LiteralPath $OutDir)) { Write-Step "no such run directory: $OutDir"; exit 2 }
    $bad = Write-Report
    if ($bad) { exit 1 }
    exit 0
}

if ((Test-Path -LiteralPath $OutDir) -and (Get-ChildItem -LiteralPath $OutDir | Select-Object -First 1)) {
    Write-Step "$OutDir exists and is not empty"
    exit 2
}
$snap = Join-Path $OutDir "snapshot"
New-Item -ItemType Directory -Force -Path $snap | Out-Null
try {
    # Source of each binary: its build directory, or -FromSnapshot.
    function Get-Source([string]$built) {
        if ($FromSnapshot) { return Join-Path $FromSnapshot (Split-Path -Leaf $built) }
        return $built
    }
    $files = New-Object System.Collections.Generic.List[object]
    if ($Modes -contains "qd3d") {
        $files.Add((Copy-Stable (Get-Source (Join-Path $qd3d "guest\build\d3d9.dll")) $snap))
        $files.Add((Copy-Stable (Get-Source (Join-Path $qd3d "host\build\qd3d_loopback.exe")) $snap))
    }
    # The front ends the tests and modes need; a front end that is not built
    # (the DirectDraw one while it is being written) leaves its runs out.
    $missingFront = @{}
    $notRun = New-Object System.Collections.Generic.List[string]
    foreach ($kind in $frontEnds.Keys) {
        $pairs = @(foreach ($t in $Tests) {
            foreach ($m in $Modes) {
                if ((Get-Kind $t) -eq $kind -and ($m -eq "qd3d" -or ($m -eq "d3d8fe" -and $kind -eq "d3d8"))) { "$t $m" }
            }
        })
        if (-not $pairs.Count) { continue }
        $src = if ($kind -eq "ddraw" -and $DDrawDll) { $DDrawDll } else { Get-Source (Join-Path $qd3d $frontEnds[$kind].build) }
        if (Test-Path -LiteralPath $src) {
            $files.Add((Copy-Stable $src $snap))
        } else {
            $missingFront[$kind] = $src
            foreach ($pair in $pairs) { $notRun.Add("$pair`: front end not built ($src missing)") }
        }
    }
    foreach ($t in $Tests) { $files.Add((Copy-Stable (Get-Source (Join-Path $conf "build\$t.exe")) $snap)) }
    $files.Add((Copy-Stable (Join-Path $conf "build\native_probe.exe") $snap))
} catch {
    Write-Step "snapshot failed: $_"
    exit 2
}
$os = Get-CimInstance Win32_OperatingSystem
$gpus = (@(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) -join "; ")
$readme = Join-Path $qd3d "third_party\wine\README.md"
$tag = ""
if (Test-Path -LiteralPath $readme) {
    $hit = Select-String -LiteralPath $readme -Pattern '\bwine-\d+\.\d+(\.\d+)?\b' | Select-Object -First 1
    if ($hit) { $tag = $hit.Matches[0].Value }
}
[ordered]@{
    started = (Get-Date).ToString("o"); os = "$($os.Caption) $($os.Version)"; gpus = $gpus
    wine_tag = $tag; tests = $Tests; modes = $Modes; only = $Only; from_snapshot = $FromSnapshot; ddraw_dll = $DDrawDll
    files = $files; not_run = $notRun
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir "manifest.json") -Encoding UTF8
Write-Step "run directory $OutDir"
foreach ($n in $notRun) { Write-Step "not run: $n" }
if (-not (Wait-Native $snap)) {
    Write-Step "the stock runtime cannot create devices; nothing run"
    exit 2
}

$stopped = ""
try {
    foreach ($t in $Tests) {
        foreach ($m in $Modes) {
            if ($m -eq "d3d8fe" -and (Get-Kind $t) -ne "d3d8") { continue }
            if ($m -ne "stock" -and $missingFront.ContainsKey((Get-Kind $t))) { continue }
            Invoke-TestMode $snap $t $m
        }
    }
} catch {
    $stopped = "$_"
    Write-Step $stopped
} finally {
    foreach ($p in @($script:live)) { Stop-Tree $p }
}
$bad = Write-Report
if ($stopped) { exit 2 }
if ($bad) { exit 1 }
exit 0
