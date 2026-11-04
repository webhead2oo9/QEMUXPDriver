# SPDX-License-Identifier: GPL-2.0-only
<#
.SYNOPSIS
Runs qd3d9_multiproc.exe through run-loopback.ps1 with two channel slots and
with one, and checks the executor's side of the crashed-owner scenario.

.DESCRIPTION
"--slots 2" on a two-slot harness: the launcher pattern and the crashed
owner (qd3d9_multiproc.c). The harness log must also show the reusing
child's HELLO releasing the crashed child's session: "qd3dx: HELLO pid
<reuse> ends the session of pid <dead>, releasing N objects" with N >= 4 (its
IDirect3D9, device, texture and vertex buffer at least).
"--slots 1" on a one-slot harness: a second process gets the stock runtime
and the proxy's qd3d.log says why; FreeLibrary of the proxy gives the slot
back at once.
Exit code: 0 when everything passed; otherwise the first failing
run-loopback.ps1 exit code, or 12 when the release was not observed.
#>
param(
    [string]$Program = "$PSScriptRoot\build\qd3d9_multiproc.exe",
    [string]$OutDir = (Join-Path $env:TEMP ("qd3d-multiproc-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 6))),
    [int]$TimeoutSec = 120
)
$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "run-loopback.ps1"
$two = Join-Path $OutDir "slots2"
$one = Join-Path $OutDir "slots1"

& $runner -Program $Program -Arguments "--slots", "2" -Slots 2 -OutDir $two -TimeoutSec $TimeoutSec
$rc2 = $LASTEXITCODE
& $runner -Program $Program -Arguments "--slots", "1" -Slots 1 -OutDir $one -TimeoutSec $TimeoutSec
$rc1 = $LASTEXITCODE

$released = $false
$plog = Get-Content (Join-Path $two "program.log") -Raw
$m = [regex]::Match($plog, "(?m)^INFO crashed-owner: dead pid (\d+), reuse pid (\d+)")
if ($m.Success) {
    $dead = $m.Groups[1].Value
    $reuse = $m.Groups[2].Value
    $hlog = Get-Content (Join-Path $two "harness.log") -Raw
    $r = [regex]::Match($hlog, "(?m)^qd3dx: HELLO pid $reuse ends the session of pid $dead, releasing (\d+) objects")
    $released = $r.Success -and [int]$r.Groups[1].Value -ge 4
    if ($r.Success) { Write-Output "== executor: $($r.Value)" }
    if (-not $released) { Write-Output "== executor: pid $reuse's HELLO did not release pid $dead's session" }
} else {
    Write-Output "== the program printed no crashed-owner pids"
}

Write-Output "== slots 2: exit $rc2; slots 1: exit $rc1; crashed session released at the next HELLO: $released"
Write-Output "== run directories: $two, $one"
if ($rc2) { exit $rc2 }
if ($rc1) { exit $rc1 }
if (-not $released) { exit 12 }
exit 0
