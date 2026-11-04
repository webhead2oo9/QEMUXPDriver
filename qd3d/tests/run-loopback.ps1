# SPDX-License-Identifier: GPL-2.0-only
<#
.SYNOPSIS
Runs a 32-bit Direct3D test program through the QD3D proxy and the loopback
host executor on this Windows machine (no VM).

.DESCRIPTION
Copies the program and the proxy d3d9.dll (and optionally d3d8.dll, and with
-WithDDraw the DirectDraw front end ddraw.dll) into a new
run directory, starts qd3d_loopback.exe on a channel of its own (so several
runs can share the machine) with -Slots channel slots, runs the program with
QD3D_TRANSPORT=loopback, %TEMP% in the run directory (the proxy's
persistent qd3d.log lands there) and -Arguments, stops the harness, and
verifies from the program's own output that the proxy was the d3d9.dll it
loaded. Exit code: the program's exit code, or 10 if the proxy was not
loaded, or 11 if the harness reported a fault.

The harness dumps the run's first 8 presented frames and every 1000th as
frame_NNNNNN.bmp into the run directory (frames counted from 1 over every
slot), the repeats of a dumped frame N (the cursor moved between presents,
a window shown again) as frame_NNNNNN_cKKK.bmp, and logs released frame
sources (source_released.txt) and its display policy's transitions
(display.txt); the program finds that directory in QD3D_FRAME_DUMPS
(qd3d9_smoke checks the cursor over its first frames there, qd3d9_display
the display policy).

-WithDDraw also sets QDDRAW_VIRTUAL_MODES=1 for the program, so the front
end never changes this machine's display mode (SetDisplayMode is emulated);
-DDrawDll <path> takes that ddraw.dll instead of qd3d\ddraw\build\ddraw.dll.
#>
param(
    [string]$Program = "$PSScriptRoot\build\qd3d9_smoke.exe",
    [string[]]$Arguments = @(),
    [string]$OutDir = (Join-Path $env:TEMP ("qd3d-loopback-" + (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + [guid]::NewGuid().ToString("N").Substring(0, 6))),
    [switch]$WithD3D8,
    [switch]$WithDDraw,
    [string]$DDrawDll = "",
    [ValidateRange(1, 4)][int]$Slots = 1,
    [int]$TimeoutSec = 120,
    [string]$Name = ("Local\QD3D_LB_" + [guid]::NewGuid().ToString("N").Substring(0, 12))
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$proxy = Join-Path $root "guest\build\d3d9.dll"
$d3d8 = Join-Path $root "d3d8\build\d3d8.dll"
$ddraw = if ($DDrawDll) { $DDrawDll } else { Join-Path $root "ddraw\build\ddraw.dll" }
$harness = Join-Path $root "host\build\qd3d_loopback.exe"

foreach ($f in @($Program, $proxy, $harness)) {
    if (-not (Test-Path $f)) { throw "missing $f (build it first)" }
}
if ((Test-Path $OutDir) -and (Get-ChildItem $OutDir | Select-Object -First 1)) {
    throw "$OutDir exists and is not empty"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Copy-Item $Program, $proxy $OutDir
if ($WithD3D8) {
    if (-not (Test-Path $d3d8)) { throw "missing $d3d8" }
    Copy-Item $d3d8 $OutDir
}
if ($WithDDraw) {
    if (-not (Test-Path $ddraw)) { throw "missing $ddraw" }
    Copy-Item $ddraw $OutDir
}
$exe = Join-Path $OutDir (Split-Path -Leaf $Program)

$hlog = Join-Path $OutDir "harness.log"
$h = Start-Process -FilePath $harness -ArgumentList @("--timeout", "$($TimeoutSec + 10)", "--dump", $OutDir, "--dump-first", "8", "--dump-every", "1000", "--name", $Name, "--slots", "$Slots") `
    -RedirectStandardOutput $hlog -RedirectStandardError (Join-Path $OutDir "harness.err") -PassThru -WindowStyle Minimized
Start-Sleep -Milliseconds 700
if ($h.HasExited) { throw "harness exited early: $(Get-Content $hlog -Raw)" }

$saved = @{ TEMP = $env:TEMP; TMP = $env:TMP }
$env:QD3D_TRANSPORT = "loopback"
$env:QD3D_LOOPBACK_NAME = $Name
$env:QD3D_FRAME_DUMPS = $OutDir
$env:TEMP = $OutDir
$env:TMP = $OutDir
if ($WithDDraw) { $env:QDDRAW_VIRTUAL_MODES = "1" }
$plog = Join-Path $OutDir "program.log"
$start = @{
    FilePath = $exe; WorkingDirectory = $OutDir; RedirectStandardOutput = $plog
    RedirectStandardError = (Join-Path $OutDir "program.err"); PassThru = $true; NoNewWindow = $true
}
if ($Arguments.Count) { $start.ArgumentList = $Arguments }
try {
    $p = Start-Process @start
} finally {
    Remove-Item Env:\QD3D_TRANSPORT
    Remove-Item Env:\QD3D_LOOPBACK_NAME
    Remove-Item Env:\QD3D_FRAME_DUMPS
    if ($WithDDraw) { Remove-Item Env:\QDDRAW_VIRTUAL_MODES }
    $env:TEMP = $saved.TEMP
    $env:TMP = $saved.TMP
}
# Touching Handle keeps it open; without it ExitCode is empty after a
# redirected Start-Process exits.
$null = $p.Handle
if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Stop-Process -Id $p.Id -Force
    Write-Output "program timed out after $TimeoutSec s"
}
$p.WaitForExit()
$rc = $p.ExitCode

Start-Sleep -Milliseconds 500
if (-not $h.HasExited) { Stop-Process -Id $h.Id }
$h.WaitForExit()

$out = Get-Content $plog -Raw
$hout = if (Test-Path $hlog) { Get-Content $hlog -Raw } else { "" }
$glog = Join-Path $OutDir "qd3d.log"
Write-Output "== program ($exe $($Arguments -join ' ')), exit $rc"
Write-Output $out
Write-Output "== harness"
Write-Output $hout
if (Test-Path $glog) {
    Write-Output "== proxy event log (qd3d.log)"
    Write-Output (Get-Content $glog -Raw)
}
Write-Output "== run directory: $OutDir"

# Both smoke tests print the path of the d3d9.dll their process loaded.
$expected = [regex]::Escape((Join-Path $OutDir "d3d9.dll"))
if ($out -notmatch "(?i)$expected") {
    Write-Output "proxy NOT loaded (expected $OutDir\d3d9.dll in the program output)"
    exit 10
}
if ($hout -notmatch "(?m)^qd3dx: HELLO") {
    Write-Output "harness saw no HELLO: the program never reached the executor"
    exit 10
}
if ($hout -match "(?i)fault") { exit 11 }
exit $rc
