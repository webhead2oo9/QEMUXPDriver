# SPDX-License-Identifier: GPL-2.0-only
<#
.SYNOPSIS
Host test for qd3d_install.exe against a temporary fake system root (no VM).

.DESCRIPTION
Builds a fake Windows XP layout under a new temporary directory:
WINDOWS\system32 with this host's 32-bit stock d3d9.dll, d3d8.dll and
ddraw.dll, their copies in system32\dllcache and ServicePackFiles\i386, and
game folders with wrapper DLLs (a stock copy, QD3D copies, a ddraw.dll that
names itself the way WineD3D's does, placeholders). Then runs the installer
with --root against it: source checks, third-party DLLs in System32, install,
re-install, update, an upgrade of a d3d9/d3d8-only install that adds
ddraw.dll, replacement of DLLs mapped by a running program and of DLLs held
open without FILE_SHARE_DELETE (finished by applying the pending restart-time
renames the way smss does), WFP refusing the exception, conflicts, a missing
saved original, rollback after a failure at every step (--fail-after),
uninstall back to the exact original files, and the leftover scan with
--move-leftovers and --restore-leftovers. A native .sys fixture and a
persisted fake SCM exercise qxpmap creation, start/stop, configuration,
in-use replacement, pending deletion, rollback and the driver-only command.
Versioned native fixtures and a directory-backed OpenGL registry key cover
coexistence with the XPDM package. No test uses the host SCM or registry.

Exit code 0 when every check passes; the root is kept on failure or -Keep.
#>
param(
    [string]$Installer = (Join-Path $PSScriptRoot "..\build\qd3d_install.exe"),
    [string]$Hold = (Join-Path $PSScriptRoot "..\build\qd3d_hold.exe"),
    [string]$WineDDraw = (Join-Path $PSScriptRoot "..\build\wine_ddraw.dll"),
    [string]$Driver = (Join-Path $PSScriptRoot "..\build\qxpmap_fixture.sys"),
    [string]$DriverOlder = (Join-Path $PSScriptRoot "..\build\qxpmap_fixture_older.sys"),
    [string]$DriverNewer = (Join-Path $PSScriptRoot "..\build\qxpmap_fixture_newer.sys"),
    [string]$Proxy = (Join-Path $PSScriptRoot "..\..\guest\build\d3d9.dll"),
    [string]$FrontEnd = (Join-Path $PSScriptRoot "..\..\d3d8\build\d3d8.dll"),
    [string]$DDraw = (Join-Path $PSScriptRoot "..\..\ddraw\build\ddraw.dll"),
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2
# Never a UAC prompt, even if the manifest were missing.
$env:__COMPAT_LAYER = "RunAsInvoker"

foreach ($f in @($Installer, $Hold, $WineDDraw, $Driver, $DriverOlder, $DriverNewer, $Proxy, $FrontEnd, $DDraw)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "missing $f (build it first)" }
}
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$Hold = (Resolve-Path -LiteralPath $Hold).Path
$stockDir = Join-Path $env:SystemRoot "SysWOW64"
if (-not (Test-Path -LiteralPath (Join-Path $stockDir "d3d9.dll"))) { $stockDir = Join-Path $env:SystemRoot "System32" }
$Stock9 = Join-Path $stockDir "d3d9.dll"
$Stock8 = Join-Path $stockDir "d3d8.dll"
$StockDD = Join-Path $stockDir "ddraw.dll"

$Base = Join-Path ([IO.Path]::GetTempPath()) ("qd3d-install-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$R = Join-Path $Base "root"
$Win = Join-Path $R "WINDOWS"
$Sys = Join-Path $Win "system32"
$Cache = Join-Path $Sys "dllcache"
$SpDir = Join-Path $Win "ServicePackFiles\i386"
$Pending = Join-Path $R "PendingFileRenameOperations.bin"
$Drivers = Join-Path $Sys "drivers"
$tDriver = Join-Path $Drivers "qxpmap.sys"
$Scm = Join-Path $R "qxpmap-scm.bin"
$OpenGLKey = Join-Path $R "registry\HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\qxpicd"
$Src1 = Join-Path $Base "src1"
$Src2 = Join-Path $Base "src2"
$t9 = Join-Path $Sys "d3d9.dll"; $t8 = Join-Path $Sys "d3d8.dll"; $tdd = Join-Path $Sys "ddraw.dll"
$b9 = Join-Path $Sys "qd3d9_orig.dll"; $b8 = Join-Path $Sys "qd3d8_orig.dll"; $bdd = Join-Path $Sys "qddraw_orig.dll"

$script:checks = 0
$script:failures = 0
$script:holds = @()
$script:bootDriverHash = "absent"

function Check([bool]$Condition, [string]$What) {
    $script:checks++
    if ($Condition) {
        Write-Host "  ok   $What"
    } else {
        $script:failures++
        Write-Host "  FAIL $What"
    }
}

function New-Dir([string]$Path) {
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    return $Path
}

function Write-Bytes([string]$Path, [string]$Text) {
    [IO.File]::WriteAllBytes($Path, [Text.Encoding]::ASCII.GetBytes($Text))
}

function HashOf([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return "absent" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

# Content, size, last write time and attributes: what "restored exactly" means.
function Sig([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return "absent" }
    $i = Get-Item -LiteralPath $Path -Force
    return "{0}|{1}|{2}|{3}" -f $i.Length, (HashOf $Path), $i.LastWriteTimeUtc.Ticks, [int]$i.Attributes
}

function Snap([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir)) { return "" }
    $lines = Get-ChildItem -LiteralPath $Dir -Recurse -Force -File | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($Dir.Length) + "=" + (Sig $_.FullName)
    }
    return ($lines -join "`n")
}

function Dll-Snap {
    return ((Get-ChildItem -LiteralPath $Win -Recurse -Force -File -Filter "*.dll" | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($Win.Length) + "=" + (Sig $_.FullName)
    }) -join "`n")
}

# svc_state: seven DWORDs (magic, present, state, type, start, error, pending)
# followed by a 520-WCHAR image path. These helpers touch only this file.
function Scm-Field([int]$Index) {
    if (-not (Test-Path -LiteralPath $Scm)) { return 0 }
    return [BitConverter]::ToUInt32([IO.File]::ReadAllBytes($Scm), $Index * 4)
}

function Set-ScmField([int]$Index, [uint32]$Value) {
    $bytes = [IO.File]::ReadAllBytes($Scm)
    [BitConverter]::GetBytes($Value).CopyTo($bytes, $Index * 4)
    [IO.File]::WriteAllBytes($Scm, $bytes)
}

function Scm-Image {
    return [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($Scm), 28, 1040).TrimEnd([char]0)
}

function Set-ScmImage([string]$Image) {
    $bytes = [IO.File]::ReadAllBytes($Scm)
    $imageBytes = [Text.Encoding]::Unicode.GetBytes($Image)
    if ($imageBytes.Length -ge 1040) { throw "fake SCM image path too long" }
    [Array]::Clear($bytes, 28, 1040)
    $imageBytes.CopyTo($bytes, 28)
    [IO.File]::WriteAllBytes($Scm, $bytes)
}

# Truncate the fixed value before FILEVERSION, keeping its signature/header:
# version.dll rejects the entire resource if that signature is removed.
# Shift the identity-string children to their new aligned offset.
function Clear-DriverVersion([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $offsets = @()
    for ($i = 0; $i + 4 -le $bytes.Length; $i++) {
        if ($bytes[$i] -eq 0xbd -and $bytes[$i + 1] -eq 0x04 -and
            $bytes[$i + 2] -eq 0xef -and $bytes[$i + 3] -eq 0xfe) {
            $offsets += $i
        }
    }
    if ($offsets.Count -ne 1 -or $offsets[0] -lt 40) { throw "expected one VS_FIXEDFILEINFO signature in $Path" }
    $fixed = $offsets[0]
    $root = $fixed - 40   # header, "VS_VERSION_INFO" and DWORD padding
    $length = [BitConverter]::ToUInt16($bytes, $root)
    if ([BitConverter]::ToUInt16($bytes, $root + 2) -ne 52 -or
        [Text.Encoding]::Unicode.GetString($bytes, $root + 6, 32).TrimEnd([char]0) -ne "VS_VERSION_INFO" -or
        $length -lt 92 -or $root + $length -gt $bytes.Length) {
        throw "unexpected VERSIONINFO layout in $Path"
    }
    [Array]::Copy($bytes, $fixed + 52, $bytes, $fixed + 8, $length - 92)
    [Array]::Clear($bytes, $root + $length - 44, 44)
    [BitConverter]::GetBytes([uint16]($length - 44)).CopyTo($bytes, $root)
    [BitConverter]::GetBytes([uint16]8).CopyTo($bytes, $root + 2)
    [IO.File]::WriteAllBytes($Path, $bytes)
}

# Temporary files the installer left (NAME.qd3d-new*, NAME.qd3d-old*).
function Temps([string]$Dir) {
    return @(Get-ChildItem -LiteralPath $Dir -Force -File -Filter "*.qd3d-*" -ErrorAction SilentlyContinue).Count
}

function Inst([int]$Expect, [string[]]$A) {
    $out = (& $Installer @A --root $R | Out-String)
    $rc = $LASTEXITCODE
    Write-Host ("> qd3d_install {0}  (exit {1})" -f ($A -join " "), $rc)
    if ($rc -ne $Expect) { Write-Host $out }
    Check ($rc -eq $Expect) ("exit {0}" -f $Expect)
    return $out
}

function Has([string]$Out, [string]$Text) {
    Check ($Out.Contains($Text)) "output has: $Text"
}

# XP loads SYSTEM_START drivers before smss renames files, AUTO_START after.
# Record the bytes that would actually load, not just the post-reboot file.
function Restart-Sim {
    $script:bootDriverHash = "absent"
    $present = (Scm-Field 1) -ne 0
    $deleting = (Scm-Field 6) -ne 0
    $start = Scm-Field 4
    if ($present -and -not $deleting -and $start -eq 1) {
        $script:bootDriverHash = HashOf $tDriver
    }
    $done = 0
    if (Test-Path -LiteralPath $Pending) {
        $parts = [IO.File]::ReadAllText($Pending, [Text.Encoding]::Unicode).Split([char]0)
        for ($i = 0; $i + 1 -lt $parts.Length; $i += 2) {
            $src = $parts[$i] -replace '^\\\?\?\\', ''
            $dst = $parts[$i + 1]
            if (-not $src -or -not (Test-Path -LiteralPath $src)) { continue }
            if ($dst -eq "") {
                Remove-Item -LiteralPath $src -Force
            } else {
                $replace = $dst.StartsWith("!")
                $dst = $dst.TrimStart("!") -replace '^\\\?\?\\', ''
                if (Test-Path -LiteralPath $dst) {
                    if (-not $replace) { continue }
                    Remove-Item -LiteralPath $dst -Force
                }
                Move-Item -LiteralPath $src -Destination $dst
            }
            $done++
        }
        Remove-Item -LiteralPath $Pending
    }
    if ($present) {
        if ($deleting) {
            Remove-Item -LiteralPath $Scm
        } else {
            if ($start -eq 2) { $script:bootDriverHash = HashOf $tDriver }
            Set-ScmField 2 $(if ($script:bootDriverHash -eq "absent") { 1 } else { 4 })
        }
    }
    Write-Host "> restart (simulated): $done pending operation(s) applied"
    return $done
}

function Pending-Text {
    if (-not (Test-Path -LiteralPath $Pending)) { return "" }
    return [IO.File]::ReadAllText($Pending, [Text.Encoding]::Unicode).Replace([char]0, "|")
}

function Start-Hold([string]$Mode, [string]$File) {
    $ready = Join-Path $Base ("ready-" + [guid]::NewGuid().ToString("N") + ".txt")
    $p = Start-Process -FilePath $Hold -ArgumentList @($Mode, "`"$File`"", "`"$ready`"") -PassThru -WindowStyle Hidden
    $script:holds += $p
    $deadline = (Get-Date).AddSeconds(20)
    while (-not (Test-Path -LiteralPath $ready)) {
        if ($p.HasExited) { throw "qd3d_hold $Mode $File exited early" }
        if ((Get-Date) -gt $deadline) { throw "qd3d_hold $Mode $File not ready" }
        Start-Sleep -Milliseconds 50
    }
    Write-Host "> holding ($Mode) $File"
    return $p
}

function Stop-Hold($P) {
    if (-not $P.HasExited) { Stop-Process -Id $P.Id -Force }
    $P.WaitForExit()
}

function New-Root {
    if (Test-Path -LiteralPath $R) { Remove-Item -LiteralPath $R -Recurse -Force }
    New-Dir $Sys | Out-Null
    New-Dir $Drivers | Out-Null
    New-Dir $Cache | Out-Null
    New-Dir $SpDir | Out-Null
    Copy-Item -LiteralPath $Stock9, $Stock8, $StockDD -Destination $Sys
    Copy-Item -LiteralPath $Stock9, $Stock8, $StockDD -Destination $Cache
    # Windows XP SP3 replaced all three; this ServicePackFiles lacks d3d8.dll.
    Copy-Item -LiteralPath $Stock9, $StockDD -Destination $SpDir
    $g = New-Dir (Join-Path $R "Program Files\GameA")
    Copy-Item -LiteralPath $Stock9 -Destination (Join-Path $g "d3d9.dll")
    $g = New-Dir (Join-Path $R "Program Files\GameB\bin")
    Copy-Item -LiteralPath (Join-Path $Src1 "d3d8.dll") -Destination (Join-Path $g "d3d8.dll")
    Write-Bytes (Join-Path $g "d3dx9_30.dll") "not a wrapper"
    $g = New-Dir (Join-Path $R "Program Files\GameE")
    Write-Bytes (Join-Path $g "wined3d.dll") "wined3d"
    Write-Bytes (Join-Path $g "dxgi.dll") "dxgi"
    $g = New-Dir (Join-Path $R "Program Files\GameF")
    Copy-Item -LiteralPath (Join-Path $Src1 "ddraw.dll") -Destination (Join-Path $g "ddraw.dll")
    $g = New-Dir (Join-Path $R "Games\GameC")
    Write-Bytes (Join-Path $g "ddraw.dll") "ddraw wrapper"
    $g = New-Dir (Join-Path $R "Games\WineGame")
    Copy-Item -LiteralPath $WineDDraw -Destination (Join-Path $g "ddraw.dll")
    $g = New-Dir (Join-Path $R "GOG Games\GameD")
    Write-Bytes (Join-Path $g "opengl32.dll") "opengl32 wrapper"
    $g = New-Dir (Join-Path $R "Documents and Settings\User\Desktop\GameX")
    Write-Bytes (Join-Path $g "d3d9.dll") "outside the default game roots"
}

# What the d3d9/d3d8-only installer left after an install: everything of an
# install except ddraw.dll, which stays stock in System32 and the WFP caches.
function Undo-DDrawInstall {
    Remove-Item -LiteralPath $tdd -Force
    Move-Item -LiteralPath $bdd -Destination $tdd
    Move-Item -LiteralPath (Join-Path $Cache "qddraw_orig.dll") -Destination (Join-Path $Cache "ddraw.dll")
    Move-Item -LiteralPath (Join-Path $SpDir "qddraw_orig.dll") -Destination (Join-Path $SpDir "ddraw.dll")
}

$t0 = Get-Date
try {
    Write-Host "== setup: $Base"
    # Never execute an install/setup/update/patch-named image on this host,
    # even for read-only commands. The production/staged name stays intact.
    New-Dir $Base | Out-Null
    $runner = Join-Path $Base "qd3d_ctl.exe"
    Copy-Item -LiteralPath $Installer -Destination $runner
    $Installer = $runner
    # One copy of the build outputs for the whole run (sibling builds may
    # rewrite them meanwhile).
    New-Dir $Src1 | Out-Null
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $Src1 "d3d9.dll")
    Copy-Item -LiteralPath $FrontEnd -Destination (Join-Path $Src1 "d3d8.dll")
    Copy-Item -LiteralPath $DDraw -Destination (Join-Path $Src1 "ddraw.dll")
    Copy-Item -LiteralPath $Driver -Destination (Join-Path $Src1 "qxpmap.sys")
    $Proxy = Join-Path $Src1 "d3d9.dll"; $FrontEnd = Join-Path $Src1 "d3d8.dll"; $DDraw = Join-Path $Src1 "ddraw.dll"
    New-Dir $Src2 | Out-Null
    foreach ($n in @("d3d9.dll", "d3d8.dll", "ddraw.dll", "qxpmap.sys")) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $Src1 $n)) + [Text.Encoding]::ASCII.GetBytes("QD3D-TEST-V2")
        [IO.File]::WriteAllBytes((Join-Path $Src2 $n), $bytes)
    }
    $olderDriver = New-Dir (Join-Path $Base "driver-older")
    $newerDriver = New-Dir (Join-Path $Base "driver-newer")
    Copy-Item -LiteralPath $DriverOlder -Destination (Join-Path $olderDriver "qxpmap.sys")
    Copy-Item -LiteralPath $DriverNewer -Destination (Join-Path $newerDriver "qxpmap.sys")
    $p1 = HashOf (Join-Path $Src1 "d3d9.dll"); $f1 = HashOf (Join-Path $Src1 "d3d8.dll"); $d1 = HashOf (Join-Path $Src1 "ddraw.dll")
    $p2 = HashOf (Join-Path $Src2 "d3d9.dll"); $f2 = HashOf (Join-Path $Src2 "d3d8.dll"); $d2 = HashOf (Join-Path $Src2 "ddraw.dll")
    $k1 = HashOf (Join-Path $Src1 "qxpmap.sys")
    $k2 = HashOf (Join-Path $Src2 "qxpmap.sys")
    $s9 = HashOf $Stock9; $s8 = HashOf $Stock8; $sdd = HashOf $StockDD
    New-Root
    $before = Snap $Win
    $sig = @{}
    foreach ($rel in @("system32\d3d9.dll", "system32\d3d8.dll", "system32\ddraw.dll",
                       "system32\dllcache\d3d9.dll", "system32\dllcache\d3d8.dll", "system32\dllcache\ddraw.dll",
                       "ServicePackFiles\i386\d3d9.dll", "ServicePackFiles\i386\ddraw.dll")) {
        $sig[$rel] = Sig (Join-Path $Win $rel)
    }

    Write-Host "== sources are checked"
    $bad = New-Dir (Join-Path $Base "bad")
    Copy-Item -LiteralPath $Stock9, $Stock8, $StockDD -Destination $bad
    $o = Inst 4 @("install", "--from", $bad)
    Has $o "is not the QD3D Direct3D 9 proxy"
    Has $o "is not the QD3D DirectDraw front end"
    $swap = New-Dir (Join-Path $Base "swapped")
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $swap "ddraw.dll")
    Copy-Item -LiteralPath $FrontEnd -Destination (Join-Path $swap "d3d9.dll")
    Copy-Item -LiteralPath $DDraw -Destination (Join-Path $swap "d3d8.dll")
    $o = Inst 4 @("install", "--from", $swap)
    Has $o "$(Join-Path $swap 'd3d9.dll') is not the QD3D Direct3D 9 proxy (QD3D"
    Has $o "$(Join-Path $swap 'd3d8.dll') is not the QD3D Direct3D 8 front end (QD3D"
    Has $o "$(Join-Path $swap 'ddraw.dll') is not the QD3D DirectDraw front end (QD3D"
    # A tools CD staged before there was a QD3D ddraw.dll.
    $oldcd = New-Dir (Join-Path $Base "old-cd")
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $oldcd "d3d9.dll")
    Copy-Item -LiteralPath $FrontEnd -Destination (Join-Path $oldcd "d3d8.dll")
    $o = Inst 4 @("install", "--from", $oldcd)
    Has $o "$(Join-Path $oldcd 'ddraw.dll') is not the QD3D DirectDraw front end (absent)"
    Check ((Snap $Win) -eq $before) "refused installs change nothing"
    $out = (& $Installer install --fail-after 1 | Out-String)
    Check ($LASTEXITCODE -eq 1) "--fail-after without --root is a usage error"

    Write-Host "== status, fresh system"
    $o = Inst 0 @("status")
    Has $o "qd3d: state=not-installed restart-pending=no"
    Has $o "${tdd}: Microsoft: "

    Write-Host "== third-party DLLs in System32 never become saved originals"
    $stash = New-Dir (Join-Path $Base "stash")
    Copy-Item -LiteralPath $t9, $tdd -Destination $stash
    Write-Bytes $t9 "another product's d3d9.dll"
    $snap = Snap $Win
    $o = Inst 4 @("install", "--from", $Src1)
    Has $o "d3d9.dll: $t9 is neither QD3D nor a Microsoft DLL (foreign: no version information"
    Check ((Snap $Win) -eq $snap) "an unknown d3d9.dll: refused, nothing changed"
    Move-Item -LiteralPath (Join-Path $stash "d3d9.dll") -Destination $t9 -Force
    # WineD3D's ddraw.dll names Microsoft Corporation as its company.
    Copy-Item -LiteralPath $WineDDraw -Destination $tdd -Force
    $snap = Snap $Win
    $o = Inst 4 @("install", "--from", $Src1)
    Has $o "ddraw.dll: $tdd is neither QD3D nor a Microsoft DLL (foreign: Microsoft Corporation, Wine, Wine DirectDraw 5.3.1.904"
    $o = Inst 0 @("status")
    Has $o "${tdd}: foreign: Microsoft Corporation, Wine, Wine DirectDraw 5.3.1.904"
    Check ((Snap $Win) -eq $snap) "WineD3D's ddraw.dll: refused, nothing changed"
    Move-Item -LiteralPath (Join-Path $stash "ddraw.dll") -Destination $tdd -Force
    Check ((Snap $Win) -eq $before) "stock DLLs back for the next steps"

    Write-Host "== install"
    $o = Inst 0 @("install", "--from", $Src1)
    Has $o "qd3d: install ok restart-required=no"
    foreach ($t in @($t9, $t8, $tdd)) { Has $o "WFP exception for $t (simulated)" }
    Has $o "ddraw.dll: installed, stock DLL saved as qddraw_orig.dll"
    Has $o "qd3d: leftovers=8 "
    Check ((HashOf $t9) -eq $p1) "system32\d3d9.dll is the QD3D proxy"
    Check ((HashOf $t8) -eq $f1) "system32\d3d8.dll is the QD3D front end"
    Check ((HashOf $tdd) -eq $d1) "system32\ddraw.dll is the QD3D DirectDraw front end"
    Check ((Sig $b9) -eq $sig["system32\d3d9.dll"]) "stock d3d9.dll kept unchanged as qd3d9_orig.dll"
    Check ((Sig $b8) -eq $sig["system32\d3d8.dll"]) "stock d3d8.dll kept unchanged as qd3d8_orig.dll"
    Check ((Sig $bdd) -eq $sig["system32\ddraw.dll"]) "stock ddraw.dll kept unchanged as qddraw_orig.dll"
    Check (-not (Test-Path (Join-Path $Cache "d3d9.dll"))) "dllcache\d3d9.dll moved aside"
    Check ((Sig (Join-Path $Cache "qd3d9_orig.dll")) -eq $sig["system32\dllcache\d3d9.dll"]) "dllcache keeps it as qd3d9_orig.dll"
    Check ((Sig (Join-Path $Cache "qd3d8_orig.dll")) -eq $sig["system32\dllcache\d3d8.dll"]) "dllcache keeps d3d8 as qd3d8_orig.dll"
    Check (-not (Test-Path (Join-Path $Cache "ddraw.dll"))) "dllcache\ddraw.dll moved aside"
    Check ((Sig (Join-Path $Cache "qddraw_orig.dll")) -eq $sig["system32\dllcache\ddraw.dll"]) "dllcache keeps it as qddraw_orig.dll"
    Check (-not (Test-Path (Join-Path $SpDir "d3d9.dll"))) "ServicePackFiles\i386\d3d9.dll moved aside"
    Check ((Sig (Join-Path $SpDir "qd3d9_orig.dll")) -eq $sig["ServicePackFiles\i386\d3d9.dll"]) "ServicePackFiles keeps it as qd3d9_orig.dll"
    Check (-not (Test-Path (Join-Path $SpDir "ddraw.dll"))) "ServicePackFiles\i386\ddraw.dll moved aside"
    Check ((Sig (Join-Path $SpDir "qddraw_orig.dll")) -eq $sig["ServicePackFiles\i386\ddraw.dll"]) "ServicePackFiles keeps it as qddraw_orig.dll"
    Check (-not (Test-Path (Join-Path $SpDir "qd3d8_orig.dll"))) "no ServicePackFiles d3d8 copy, nothing made up"
    Check ((Temps $Sys) -eq 0 -and (Temps $Cache) -eq 0 -and (Temps $SpDir) -eq 0) "no temporary files left"
    Check (-not (Test-Path -LiteralPath $Pending)) "nothing pending for a restart"
    $o = Inst 0 @("status", "--from", $Src1)
    Has $o "qd3d: state=installed restart-pending=no"
    Has $o "d3d9.dll: installed, same as"
    Has $o "ddraw.dll: installed, same as"
    Has $o "${tdd}: QD3D: "
    Has $o "${bdd}: Microsoft: "

    Write-Host "== install again: nothing changes"
    $snap = Snap $Win
    $o = Inst 0 @("install", "--from", $Src1)
    Has $o "d3d9.dll: up to date"
    Has $o "d3d8.dll: up to date"
    Has $o "ddraw.dll: up to date"
    Check ((Snap $Win) -eq $snap) "second install leaves every file as it was"

    Write-Host "== update"
    $o = Inst 0 @("install", "--from", $Src2)
    Has $o "d3d9.dll: updated"
    Has $o "ddraw.dll: updated"
    Check ((HashOf $t9) -eq $p2 -and (HashOf $t8) -eq $f2 -and (HashOf $tdd) -eq $d2) "all three DLLs updated"
    Check ((Sig $b9) -eq $sig["system32\d3d9.dll"] -and (Sig $b8) -eq $sig["system32\d3d8.dll"] -and
           (Sig $bdd) -eq $sig["system32\ddraw.dll"]) "saved originals untouched by the update"

    Write-Host "== update while a program has d3d9.dll mapped"
    $h = Start-Hold "load" $t9
    $o = Inst 0 @("install", "--from", $Src1)
    $oldcopies = @(Get-ChildItem -LiteralPath $Sys -Filter "d3d9.dll.qd3d-old*")
    Check ((HashOf $t9) -eq $p1) "d3d9.dll replaced at once (new programs get the new proxy)"
    Check ($oldcopies.Count -eq 1) "the mapped old copy waits under a temporary name"
    Has $o "is in use; it is deleted at the next restart"
    Check ($oldcopies.Count -eq 1 -and (Pending-Text).Contains("\??\" + $oldcopies[0].FullName + "||")) "its deletion is scheduled for the restart"
    $o = Inst 0 @("status")
    Has $o "restart-pending=no"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((Temps $Sys) -eq 0) "the old copy is gone after the restart"

    Write-Host "== update while a program holds ddraw.dll open without FILE_SHARE_DELETE"
    $h = Start-Hold "open" $tdd
    $o = Inst 3 @("install", "--from", $Src2)
    Has $o "qd3d: install ok restart-required=yes"
    Has $o "$tdd is locked by a running program"
    Has $o "ddraw.dll: updated (at the next restart)"
    Check ((HashOf $tdd) -eq $d1) "the locked ddraw.dll is unchanged until the restart"
    Check ((HashOf $t9) -eq $p2 -and (HashOf $t8) -eq $f2) "d3d9.dll and d3d8.dll, not locked, are updated at once"
    Check ((Pending-Text).Contains("!\??\$tdd")) "a restart-time replacement of ddraw.dll is scheduled"
    $o = Inst 0 @("status")
    Has $o "restart-pending=yes"
    $o = Inst 4 @("uninstall")
    Has $o "A restart is pending"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((HashOf $tdd) -eq $d2) "after the restart ddraw.dll is the new front end"
    Check ((Temps $Sys) -eq 0) "no temporary files left after the restart"

    Write-Host "== update while a program has ddraw.dll mapped"
    $h = Start-Hold "load" $tdd
    $o = Inst 0 @("install", "--from", $Src1)
    $oldcopies = @(Get-ChildItem -LiteralPath $Sys -Filter "ddraw.dll.qd3d-old*")
    Check ((HashOf $tdd) -eq $d1) "ddraw.dll replaced at once (new programs get the new front end)"
    Check ($oldcopies.Count -eq 1 -and (Pending-Text).Contains("\??\" + $oldcopies[0].FullName + "||")) "the mapped old copy is deleted at the restart"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((Temps $Sys) -eq 0) "the old copy is gone after the restart"

    Write-Host "== update while a program holds d3d9.dll open without FILE_SHARE_DELETE"
    $h = Start-Hold "open" $t9
    $o = Inst 3 @("install", "--from", $Src2)
    Has $o "qd3d: install ok restart-required=yes"
    Has $o "is locked by a running program"
    Check ((HashOf $t9) -eq $p1) "the locked d3d9.dll is unchanged until the restart"
    Check ((HashOf $t8) -eq $f2 -and (HashOf $tdd) -eq $d2) "d3d8.dll and ddraw.dll, not locked, are updated at once"
    Check ((Pending-Text).Contains("!\??\$t9")) "a restart-time replacement of d3d9.dll is scheduled"
    $o = Inst 0 @("status")
    Has $o "restart-pending=yes"
    $o = Inst 4 @("install", "--from", $Src2)
    Has $o "A restart is pending"
    $o = Inst 4 @("uninstall")
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((HashOf $t9) -eq $p2) "after the restart d3d9.dll is the new proxy"
    $o = Inst 0 @("status", "--from", $Src2)
    Has $o "qd3d: state=installed restart-pending=no"

    Write-Host "== uninstall restores the original files exactly"
    $o = Inst 0 @("uninstall")
    Has $o "qd3d: uninstall ok restart-required=no"
    Has $o "ddraw.dll: stock DLL restored"
    Check ((Snap $Win) -eq $before) "WINDOWS tree identical to before the install"
    $o = Inst 0 @("uninstall")
    Has $o "d3d9.dll: not installed"
    Has $o "ddraw.dll: not installed"
    Check ((Snap $Win) -eq $before) "second uninstall changes nothing"

    Write-Host "== uninstall while programs have d3d9.dll and ddraw.dll mapped"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    $h = Start-Hold "load" $t9
    $h2 = Start-Hold "load" $tdd
    $o = Inst 0 @("uninstall")
    Check ((Sig $t9) -eq $sig["system32\d3d9.dll"] -and (Sig $tdd) -eq $sig["system32\ddraw.dll"]) "stock d3d9.dll and ddraw.dll back at once"
    Stop-Hold $h
    Stop-Hold $h2
    Restart-Sim | Out-Null
    Check ((Snap $Win) -eq $before) "exact original tree after the restart"

    Write-Host "== uninstall while a program holds d3d8.dll open without FILE_SHARE_DELETE"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    $h = Start-Hold "open" $t8
    $o = Inst 3 @("uninstall")
    Check ((Sig $t9) -eq $sig["system32\d3d9.dll"] -and (Sig $tdd) -eq $sig["system32\ddraw.dll"]) "d3d9.dll and ddraw.dll restored at once"
    Check ((HashOf $t8) -eq $f1) "locked d3d8.dll stays QD3D until the restart"
    Check ((Pending-Text).Contains("\??\$t8.qd3d-new|!\??\$t8|")) "a verified copy of qd3d8_orig.dll replaces d3d8.dll at the restart"
    Check ((Pending-Text).Contains("\??\$b8||")) "qd3d8_orig.dll itself is deleted after that"
    Check ((HashOf $b8) -eq $s8) "qd3d8_orig.dll stays until the restart"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((Snap $Win) -eq $before) "exact original tree after the restart"

    Write-Host "== uninstall while a program holds ddraw.dll open without FILE_SHARE_DELETE"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    $h = Start-Hold "open" $tdd
    $o = Inst 3 @("uninstall")
    Has $o "ddraw.dll: stock DLL restored at the next restart"
    Check ((Sig $t9) -eq $sig["system32\d3d9.dll"] -and (Sig $t8) -eq $sig["system32\d3d8.dll"]) "d3d9.dll and d3d8.dll restored at once"
    Check ((HashOf $tdd) -eq $d1) "locked ddraw.dll stays QD3D until the restart"
    Check ((Sig (Join-Path $Cache "ddraw.dll")) -eq $sig["system32\dllcache\ddraw.dll"] -and
           (Sig (Join-Path $SpDir "ddraw.dll")) -eq $sig["ServicePackFiles\i386\ddraw.dll"]) "its WFP cache copies are back at once"
    Check ((Pending-Text).Contains("\??\$tdd.qd3d-new|!\??\$tdd|\??\$bdd||")) "a verified copy of qddraw_orig.dll replaces it at the restart, then qddraw_orig.dll goes"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((Snap $Win) -eq $before) "exact original tree after the restart"

    Write-Host "== install while programs hold the stock d3d9.dll and ddraw.dll open"
    $h = Start-Hold "open" $t9
    $h2 = Start-Hold "open" $tdd
    $o = Inst 3 @("install", "--from", $Src1)
    Has $o "saved a copy of the stock d3d9.dll"
    Has $o "saved a copy of the stock ddraw.dll"
    Check ((Sig $b9) -eq $sig["system32\d3d9.dll"] -and (Sig $bdd) -eq $sig["system32\ddraw.dll"]) "qd3d9_orig.dll and qddraw_orig.dll are verified copies (bytes, time, attributes)"
    Check ((HashOf $t9) -eq $s9 -and (HashOf $tdd) -eq $sdd) "d3d9.dll and ddraw.dll stay stock until the restart"
    Check ((HashOf $t8) -eq $f1) "d3d8.dll, not held, is installed at once"
    Stop-Hold $h
    Stop-Hold $h2
    Restart-Sim | Out-Null
    Check ((HashOf $t9) -eq $p1 -and (HashOf $tdd) -eq $d1) "after the restart both are QD3D"
    $o = Inst 0 @("uninstall")
    Check ((Snap $Win) -eq $before) "uninstall restores bytes, times and attributes"

    Write-Host "== WFP refuses the exception: everything waits for the restart"
    $o = Inst 3 @("install", "--from", $Src1, "--sim-wfp-deny")
    Has $o "WFP exception for $tdd (simulated, refused)"
    Check ((HashOf $t9) -eq $s9 -and (HashOf $t8) -eq $s8 -and (HashOf $tdd) -eq $sdd) "System32 DLLs untouched before the restart"
    Check ((HashOf $b9) -eq $s9 -and (HashOf $b8) -eq $s8 -and (HashOf $bdd) -eq $sdd) "copies of the originals saved"
    Check (-not (Test-Path (Join-Path $Cache "d3d9.dll")) -and -not (Test-Path (Join-Path $Cache "ddraw.dll")) -and
           -not (Test-Path (Join-Path $SpDir "ddraw.dll"))) "cache copies moved aside at once"
    Check ((Pending-Text).Contains("!\??\$tdd|")) "ddraw.dll is replaced at the restart"
    Restart-Sim | Out-Null
    Check ((HashOf $t9) -eq $p1 -and (HashOf $t8) -eq $f1 -and (HashOf $tdd) -eq $d1) "installed after the restart"
    $o = Inst 3 @("uninstall", "--sim-wfp-deny")
    Check ((Test-Path (Join-Path $Cache "d3d9.dll")) -and (Test-Path (Join-Path $Cache "ddraw.dll")) -and
           (Test-Path (Join-Path $SpDir "ddraw.dll"))) "cache copies restored at once"
    Check ((HashOf $tdd) -eq $d1 -and (HashOf $bdd) -eq $sdd) "System32 untouched until the restart"
    Restart-Sim | Out-Null
    Check ((Snap $Win) -eq $before) "exact original tree after the restart"

    Write-Host "== conflicts are refused without changes"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    Copy-Item -LiteralPath $Stock8 -Destination $t9 -Force
    Copy-Item -LiteralPath $Stock9 -Destination $tdd -Force
    $snap = Snap $Win
    $o = Inst 4 @("install", "--from", $Src1)
    Has $o "d3d9.dll: $t9 is not QD3D and differs from the saved original"
    Has $o "ddraw.dll: $tdd is not QD3D and differs from the saved original"
    $o = Inst 4 @("uninstall")
    Check ((Snap $Win) -eq $snap) "refusals change nothing"
    Copy-Item -LiteralPath $b9 -Destination $t9 -Force
    Copy-Item -LiteralPath $bdd -Destination $tdd -Force
    $o = Inst 0 @("status")
    Has $o "d3d9.dll: the stock DLL is back in place"
    Has $o "ddraw.dll: the stock DLL is back in place"
    $o = Inst 0 @("install", "--from", $Src1)
    Check ((HashOf $t9) -eq $p1 -and (HashOf $tdd) -eq $d1) "copies of the saved originals in place (as WFP restores them) are replaced"
    $o = Inst 0 @("uninstall")
    Check ((Snap $Win) -eq $before) "the saved original objects are what comes back"

    Write-Host "== a missing saved original is recovered from the WFP caches"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    Remove-Item -LiteralPath $b9, $bdd
    $o = Inst 0 @("status")
    Has $o "qd3d: state=inconsistent"
    Has $o "ddraw.dll: QD3D without a saved stock original"
    $o = Inst 0 @("install", "--from", $Src1)
    Has $o "qd3d9_orig.dll as $b9"
    Has $o "qddraw_orig.dll as $bdd"
    Check ((HashOf $b9) -eq $s9 -and (HashOf $bdd) -eq $sdd) "qd3d9_orig.dll and qddraw_orig.dll recreated from the dllcache copies"
    Remove-Item -LiteralPath $b9, $bdd
    $o = Inst 0 @("uninstall")
    Check ((HashOf $t9) -eq $s9 -and (HashOf (Join-Path $Cache "d3d9.dll")) -eq $s9) "uninstall without it restores d3d9.dll from the cache copy"
    Check ((HashOf $tdd) -eq $sdd -and (HashOf (Join-Path $Cache "ddraw.dll")) -eq $sdd -and
           (HashOf (Join-Path $SpDir "ddraw.dll")) -eq $sdd) "and ddraw.dll, with its cache copies back"
    Check ((Temps $Sys) -eq 0) "no temporary files left"
    Inst 0 @("install", "--from", $Src1) | Out-Null
    Remove-Item -LiteralPath $b9, (Join-Path $Cache "qd3d9_orig.dll"), (Join-Path $SpDir "qd3d9_orig.dll")
    $snap = Snap $Win
    $o = Inst 4 @("install", "--from", $Src1)
    Has $o "no stock copy is saved"
    $o = Inst 4 @("uninstall")
    Check ((Snap $Win) -eq $snap) "no stock copy anywhere: refused, nothing changed"

    Write-Host "== upgrade of a d3d9/d3d8-only install adds ddraw.dll"
    New-Root
    Inst 0 @("install", "--from", $Src1) | Out-Null
    Undo-DDrawInstall
    $o = Inst 0 @("status", "--from", $Src1)
    Has $o "ddraw.dll: not installed"
    Has $o "qd3d: state=partial restart-pending=no"
    $o = Inst 0 @("install", "--from", $Src1)
    Has $o "d3d9.dll: up to date"
    Has $o "d3d8.dll: up to date"
    Has $o "ddraw.dll: installed, stock DLL saved as qddraw_orig.dll"
    Check ((HashOf $tdd) -eq $d1 -and (Sig $bdd) -eq $sig["system32\ddraw.dll"]) "ddraw.dll installed, the stock one kept as qddraw_orig.dll"
    Check ((Sig (Join-Path $Cache "qddraw_orig.dll")) -eq $sig["system32\dllcache\ddraw.dll"] -and
           (Sig (Join-Path $SpDir "qddraw_orig.dll")) -eq $sig["ServicePackFiles\i386\ddraw.dll"]) "its WFP cache copies moved aside"
    $o = Inst 0 @("uninstall")
    Check ((Snap $Win) -eq $before) "uninstall restores all three exactly"

    Write-Host "== rollback after a failure at every step"
    $sweeps = @(
        @{ Name = "install"; Prep = {}; Cmd = @("install", "--from", $Src1) },
        @{ Name = "update"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("install", "--from", $Src2) },
        @{ Name = "uninstall"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("uninstall") },
        @{ Name = "install, WFP refusing"; Prep = {}; Cmd = @("install", "--from", $Src1, "--sim-wfp-deny") },
        @{ Name = "uninstall, WFP refusing"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("uninstall", "--sim-wfp-deny") },
        @{ Name = "upgrade adding ddraw.dll"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null; Undo-DDrawInstall }; Cmd = @("install", "--from", $Src1) }
    )
    foreach ($s in $sweeps) {
        $hits = 0
        $ddhits = 0
        for ($n = 1; $n -le 60; $n++) {
            New-Root
            & $s.Prep
            $snap = Snap $Win
            $scmBefore = HashOf $Scm
            $a = $s.Cmd + @("--fail-after", "$n")
            $out = (& $Installer @a --root $R | Out-String)
            $rc = $LASTEXITCODE
            if ($rc -ne 2) {
                Check ($rc -eq 0 -or $rc -eq 3) ("{0}: completes when step {1} is past the last change (exit {2})" -f $s.Name, $n, $rc)
                break
            }
            $hits++
            # The change that failed is named on the line after the injection.
            $m = [regex]::Match($out, "injected failure at step $n\r?\n([^\r\n]*)")
            if ($m.Success -and $m.Groups[1].Value -match "ddraw") { $ddhits++ }
            Restart-Sim | Out-Null   # an operation scheduled before the failure must be harmless
            $same = (Snap $Win) -eq $snap -and (HashOf $Scm) -eq $scmBefore
            $reported = $m.Success -and ($out -match "qd3d: (un)?install failed")
            if (-not ($same -and $reported)) { Write-Host $out }
            Check ($same -and $reported) ("{0}: failure at step {1} rolls back exactly" -f $s.Name, $n)
        }
        Check ($hits -ge 4 -and $ddhits -ge 1) ("{0}: {1} failure points exercised, {2} of them in ddraw.dll's changes" -f $s.Name, $hits, $ddhits)
    }

    Write-Host "== qxpmap source checks and driver-only install"
    New-Root
    $driverOnly = New-Dir (Join-Path $Base "driver-only")
    Copy-Item -LiteralPath (Join-Path $Src1 "qxpmap.sys") -Destination $driverOnly
    $dlls = Dll-Snap
    $pristine = Snap $Win
    $o = Inst 4 @("driver", "--from", $oldcd)
    Has $o "is not the XPDriver qxpmap native i386 driver"
    $withoutDriver = New-Dir (Join-Path $Base "without-driver")
    Copy-Item -LiteralPath $Proxy, $FrontEnd, $DDraw -Destination $withoutDriver
    $o = Inst 4 @("install", "--from", $withoutDriver)
    Has $o "is not the XPDriver qxpmap native i386 driver"
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $bad "qxpmap.sys")
    $o = Inst 4 @("driver", "--from", $bad)
    Check ((Snap $Win) -eq $pristine -and -not (Test-Path -LiteralPath $Scm)) "wrong/missing .sys cannot create a service or change files"
    foreach ($option in @("--sim-scm-busy", "--sim-scm-delete-pending")) {
        $out = (& $Installer driver $option | Out-String)
        Check ($LASTEXITCODE -eq 1) "$option cannot reach the host SCM without --root"
    }
    $out = (& $Installer driver --sim-scm-fail start | Out-String)
    Check ($LASTEXITCODE -eq 1) "SCM fault injection requires --root"
    $o = Inst 0 @("driver", "--from", $driverOnly)
    Has $o "SCM (simulated): create qxpmap"
    Has $o "SCM (simulated): start qxpmap"
    Has $o "qd3d: driver ok restart-required=no"
    Check ((HashOf $tDriver) -eq $k1) "driver-only installs the fixture in system32\drivers"
    Check ((Scm-Field 1) -eq 1 -and (Scm-Field 2) -eq 4 -and (Scm-Field 3) -eq 1 -and (Scm-Field 4) -eq 1) "fake service is running, KERNEL_DRIVER, SYSTEM_START"
    $image = Scm-Image
    Check ($image -eq "\SystemRoot\system32\drivers\qxpmap.sys") "service image is the system driver path"
    Check ((Dll-Snap) -eq $dlls -and -not $o.Contains("leftovers=")) "driver-only neither changes DLLs/caches nor scans games"
    $o = Inst 0 @("status")
    Has $o "qxpmap: service=running start=SYSTEM_START"
    Has $o "${tDriver}: file-version=1.0.0.0"
    Has $o "qd3d: state=partial restart-pending=no"
    $snapshot = Snap $Win
    $scmBefore = Sig $Scm
    $o = Inst 0 @("driver", "--from", $driverOnly)
    Check ((Snap $Win) -eq $snapshot -and (Sig $Scm) -eq $scmBefore) "driver-only reinstall changes neither image nor SCM file"
    $o = Inst 0 @("driver", "--from", $Src2)
    Has $o "SCM (simulated): stop qxpmap"
    Has $o "SCM (simulated): start qxpmap"
    Check ((HashOf $tDriver) -eq $k2 -and (Dll-Snap) -eq $dlls) "driver-only update replaces only the driver"
    Check ([Diagnostics.FileVersionInfo]::GetVersionInfo($tDriver).FileVersion -eq
           [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $Src1 "qxpmap.sys")).FileVersion) "equal FILEVERSION with different bytes is still replaced"
    Check ((Temps $Drivers) -eq 0) "normal driver update leaves no temporary files"

    Write-Host "== busy qxpmap updates load the replacement on the first reboot"
    $o = Inst 3 @("driver", "--from", $Src1, "--sim-scm-busy")
    Has $o "clients prevent stopping the driver"
    Has $o "temporary AUTO_START"
    Check ((HashOf $tDriver) -eq $k2 -and (Scm-Field 2) -eq 4 -and (Scm-Field 4) -eq 2) "busy update keeps old image running with replacement-boot AUTO_START"
    Check ((Pending-Text).Contains("!\??\$tDriver|")) "busy update schedules the verified .sys replacement"
    $o = Inst 0 @("status")
    Has $o "qxpmap: service=running start=AUTO_START"
    Has $o "restart-pending=yes"
    Inst 4 @("driver", "--from", $Src1) | Out-Null
    Restart-Sim | Out-Null
    Check ((HashOf $tDriver) -eq $k1 -and $script:bootDriverHash -eq $k1) "SCM starts the new image after smss, not the old SYSTEM_START image"
    $o = Inst 0 @("driver", "--from", $Src1)
    Check ((Scm-Field 4) -eq 1 -and (Scm-Field 2) -eq 4) "next driver command restores SYSTEM_START without stopping the running driver"
    Check ((Dll-Snap) -eq $dlls -and (Temps $Drivers) -eq 0) "deferred driver update leaves DLLs and caches untouched"

    Write-Host "== stopped service and locked .sys"
    Set-ScmField 2 1
    $o = Inst 0 @("driver", "--from", $Src1)
    Has $o "SCM (simulated): start qxpmap"
    $h = Start-Hold "open" $tDriver
    $o = Inst 3 @("driver", "--from", $Src2)
    Check ((HashOf $tDriver) -eq $k1 -and (Scm-Field 2) -eq 1 -and (Scm-Field 4) -eq 2) "a locked .sys waits for reboot even after service stop"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ($script:bootDriverHash -eq $k2 -and (HashOf $tDriver) -eq $k2) "locked .sys replacement is loaded at the simulated reboot"
    Inst 0 @("driver", "--from", $Src2) | Out-Null

    Write-Host "== driver uninstall and pending service deletion"
    $o = Inst 0 @("uninstall")
    Check ((Snap $Win) -eq $pristine -and -not (Test-Path -LiteralPath $Scm)) "driver-only uninstall restores the exact original tree and removes service"
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    $h = Start-Hold "open" $tDriver
    $o = Inst 3 @("uninstall", "--sim-scm-busy")
    Check ((HashOf $tDriver) -eq $k1 -and (Scm-Field 6) -eq 1 -and (Scm-Field 4) -eq 4) "busy uninstall disables boot loading and accepts service pending deletion"
    Check ((Pending-Text).Contains("\??\$tDriver||")) "busy uninstall schedules driver-file deletion"
    $o = Inst 0 @("status")
    Has $o "qxpmap: service=delete-pending"
    Has $o "restart-pending=yes"
    Stop-Hold $h
    Restart-Sim | Out-Null
    Check ((Snap $Win) -eq $pristine -and -not (Test-Path -LiteralPath $Scm)) "reboot completes busy service/file removal"
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    $o = Inst 3 @("uninstall", "--sim-scm-delete-pending")
    Check (-not (Test-Path -LiteralPath $tDriver) -and (Scm-Field 6) -eq 1) "another SCM handle may delay deletion after file removal"
    $o = Inst 0 @("status")
    Has $o "restart-pending=yes"
    $o = Inst 4 @("driver", "--from", $Src1)
    Has $o "service deletion is pending"
    $o = Inst 3 @("uninstall")
    Restart-Sim | Out-Null
    Check (-not (Test-Path -LiteralPath $Scm)) "pending service deletion completes without a file-rename record"

    Write-Host "== service image spellings shared with SetupAPI"
    $imagePaths = @(
        "\sYsTeMrOoT\System32\DRIVERS\QXPMAP.SYS",
        "system32\drivers\qxpmap.sys",
        "SYSTEM32\DRIVERS\QXPMAP.SYS",
        "%SYSTEMROOT%\SYSTEM32\DRIVERS\QXPMAP.SYS",
        ("\??\" + $tDriver.ToUpperInvariant()),
        $tDriver.ToUpperInvariant()
    )
    foreach ($image in $imagePaths) {
        New-Root
        Inst 0 @("driver", "--from", $Src1) | Out-Null
        Set-ScmImage $image
        $o = Inst 0 @("status")
        Has $o "qd3d: state=partial restart-pending=no"
        $o = Inst 0 @("driver", "--from", $Src2)
        Check ((HashOf $tDriver) -eq $k2 -and (Scm-Image) -ceq $image) "update accepts and retains service ImagePath $image"
        $o = Inst 0 @("uninstall")
        Check ((Snap $Win) -eq $pristine -and -not (Test-Path -LiteralPath $Scm)) "uninstall accepts service ImagePath $image"
    }

    Write-Host "== foreign image paths are refused, even with OpenGL registered"
    New-Root
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    New-Dir $OpenGLKey | Out-Null
    foreach ($image in @("system32\drivers\foreign.sys", ($tDriver + ".other"),
                         "\??\system32\drivers\qxpmap.sys", "%windir%\system32\drivers\qxpmap.sys")) {
        Set-ScmImage $image
        $snapshot = Snap $R
        foreach ($cmd in @("driver", "install", "uninstall")) {
            $o = Inst 4 @($cmd, "--from", $Src2)
            Has $o "qxpmap: conflicting service type or image path; nothing will be changed."
            Check ((Snap $R) -eq $snapshot) "$cmd refuses foreign ImagePath $image without changes"
        }
        $o = Inst 0 @("status")
        Has $o "qd3d: state=inconsistent restart-pending=no"
    }

    Write-Host "== OpenGL ownership preserves service and file through QD3D uninstall"
    New-Root
    $dlls = Dll-Snap
    Inst 0 @("install", "--from", $Src1) | Out-Null
    Set-ScmImage "system32\drivers\qxpmap.sys"
    New-Dir $OpenGLKey | Out-Null
    $driverBefore = Sig $tDriver
    $scmBefore = Sig $Scm
    $h = Start-Hold "open" $tDriver
    $o = Inst 0 @("uninstall", "--sim-scm-busy", "--sim-scm-fail", "delete")
    $kept = "qxpmap: kept because the XPDriver OpenGL driver (qxpicd) uses it."
    Has $o $kept
    Check ([regex]::Matches($o, [regex]::Escape($kept)).Count -eq 1) "shared-driver retention is reported once"
    Check ((Sig $tDriver) -eq $driverBefore -and (Sig $Scm) -eq $scmBefore) "shared driver file and service are untouched, even when busy"
    Check ($o -notmatch "SCM \(simulated\): (stop|config|delete) qxpmap") "shared uninstall neither stops, configures nor deletes the service"
    Check ((Dll-Snap) -eq $dlls) "shared-driver uninstall still restores every stock DLL and WFP cache"
    Check ((Test-Path -LiteralPath $OpenGLKey) -and -not (Test-Path -LiteralPath $Pending) -and
           (Temps $Drivers) -eq 0) "OpenGL registration remains and no driver removal is staged"
    Stop-Hold $h
    Set-ScmField 2 1
    Set-ScmField 4 3
    $snapshot = Snap $R
    $o = Inst 0 @("uninstall", "--sim-scm-fail", "config")
    Has $o $kept
    Check ((Snap $R) -eq $snapshot) "repeat shared uninstall also preserves a stopped DEMAND_START service"
    Remove-Item -LiteralPath $OpenGLKey
    $o = Inst 0 @("uninstall")
    Has $o "qxpmap: removed"
    Check ((Snap $Win) -eq $pristine -and -not (Test-Path -LiteralPath $Scm)) "without the OpenGL key, uninstall removes service and driver as before"

    Write-Host "== installed FILEVERSION orders driver replacement"
    New-Root
    Inst 0 @("driver", "--from", $newerDriver) | Out-Null
    Set-ScmImage "system32\drivers\qxpmap.sys"
    $snapshot = Snap $R
    $o = Inst 0 @("driver", "--from", $Src1, "--sim-scm-busy", "--sim-scm-fail", "stop")
    Has $o "qxpmap.sys: newer version 1.0.0.1 already installed"
    Check ((Snap $R) -eq $snapshot) "newer installed revision is kept without stopping or rewriting it"
    $driverBefore = Sig $tDriver
    Set-ScmField 2 1
    Set-ScmField 4 3
    $o = Inst 0 @("install", "--from", $Src1)
    Has $o "qxpmap.sys: newer version 1.0.0.1 already installed"
    Has $o "SCM (simulated): config qxpmap"
    Has $o "SCM (simulated): start qxpmap"
    Check ((Sig $tDriver) -eq $driverBefore -and (Scm-Field 2) -eq 4 -and (Scm-Field 4) -eq 1) "full install keeps newer bytes while normalizing and starting its service"
    Check ((HashOf $t9) -eq $p1 -and (HashOf $t8) -eq $f1 -and (HashOf $tdd) -eq $d1) "newer shared driver does not prevent the QD3D DLL install"
    New-Root
    Copy-Item -LiteralPath (Join-Path $newerDriver "qxpmap.sys") -Destination $tDriver
    $driverBefore = Sig $tDriver
    $o = Inst 0 @("driver", "--from", $Src1)
    Has $o "qxpmap.sys: newer version 1.0.0.1 already installed"
    Has $o "SCM (simulated): create qxpmap"
    Check ((Sig $tDriver) -eq $driverBefore -and (Scm-Field 2) -eq 4 -and (Scm-Field 4) -eq 1) "newer installed file still gets a running service when absent"
    New-Root
    Inst 0 @("driver", "--from", $olderDriver) | Out-Null
    $o = Inst 0 @("driver", "--from", $Src1)
    Has $o "qxpmap.sys: installed/updated"
    Has $o "SCM (simulated): stop qxpmap"
    Has $o "SCM (simulated): start qxpmap"
    Check ((HashOf $tDriver) -eq $k1) "1.0.0.0 replaces 0.65535.65535.65535: major version takes precedence"
    $snapshot = Snap $R
    $o = Inst 0 @("driver", "--from", $olderDriver)
    Has $o "qxpmap.sys: newer version 1.0.0.0 already installed"
    Check ((Snap $R) -eq $snapshot) "a lower major version never replaces the installed driver"

    Write-Host "== unreadable numeric FILEVERSION uses the installed-file fallback"
    Clear-DriverVersion $tDriver
    $o = Inst 0 @("driver", "--from", $Src1)
    Has $o "qxpmap.sys: installed FILEVERSION unreadable; using byte comparison"
    Check ((HashOf $tDriver) -eq $k1) "a driver with identity strings but unreadable fixed version is replaced by byte comparison"
    New-Root
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    $unversioned = New-Dir (Join-Path $Base "driver-unversioned")
    $unversionedFile = Join-Path $unversioned "qxpmap.sys"
    Copy-Item -LiteralPath (Join-Path $Src1 "qxpmap.sys") -Destination $unversionedFile
    Clear-DriverVersion $unversionedFile
    $snapshot = Snap $R
    $o = Inst 4 @("driver", "--from", $unversioned)
    Has $o "has no readable FILEVERSION; refusing an update without version ordering."
    Check ((Snap $R) -eq $snapshot) "a carried driver without version ordering cannot downgrade the installed image"

    Write-Host "== qxpmap conflicts and named SCM failures"
    New-Root
    Write-Bytes $tDriver "foreign driver"
    $snapshot = Snap $Win
    Inst 4 @("driver", "--from", $Src1) | Out-Null
    Inst 4 @("uninstall") | Out-Null
    Check ((Snap $Win) -eq $snapshot -and -not (Test-Path -LiteralPath $Scm)) "foreign driver bytes are never replaced or removed"
    New-Root
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    Set-ScmField 3 16
    $snapshot = Snap $Win
    $scmBefore = HashOf $Scm
    Inst 4 @("driver", "--from", $Src2) | Out-Null
    Check ((Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore) "a non-kernel qxpmap service is refused without changes"
    $faults = @(
        @{ Op = "query"; Prep = {}; Cmd = @("driver", "--from", $Src1); Exit = 4 },
        @{ Op = "create"; Prep = {}; Cmd = @("install", "--from", $Src1); Exit = 2 },
        @{ Op = "start"; Prep = {}; Cmd = @("install", "--from", $Src1); Exit = 2 },
        @{ Op = "stop"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("install", "--from", $Src2); Exit = 2 },
        @{ Op = "config"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("install", "--from", $Src2, "--sim-scm-busy"); Exit = 2 },
        @{ Op = "delete"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("uninstall"); Exit = 2 }
    )
    foreach ($f in $faults) {
        New-Root
        & $f.Prep
        $snapshot = Snap $Win
        $scmBefore = HashOf $Scm
        $o = Inst $f.Exit ($f.Cmd + @("--sim-scm-fail", $f.Op))
        Check ((Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore) ("SCM {0} failure restores files and service state" -f $f.Op)
        Check ((Temps $Drivers) -eq 0) ("SCM {0} failure leaves no staged driver" -f $f.Op)
    }

    New-Root
    Inst 0 @("driver", "--from", $Src1) | Out-Null
    Set-ScmField 2 1
    Set-ScmField 4 3
    $snapshot = Snap $Win
    $scmBefore = HashOf $Scm
    Inst 2 @("driver", "--from", $Src2, "--sim-scm-fail", "start") | Out-Null
    Check ((Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore) "failed start restores the old image, stopped state and original DEMAND_START policy"
    Restart-Sim | Out-Null
    Check ((Scm-Field 2) -eq 1 -and (Scm-Field 4) -eq 3 -and $script:bootDriverHash -eq "absent") "rollback does not boot-start a formerly demand-start stopped service"

    Write-Host "== a failed DeleteService withdraws only our pending deletion"
    New-Root
    Inst 0 @("install", "--from", $Src1) | Out-Null
    $snapshot = Snap $Win
    $scmBefore = HashOf $Scm
    $external = Join-Path $R "unrelated.tmp"
    Write-Bytes $external "unrelated"
    $record = "\??\$external" + [char]0 + [char]0
    [IO.File]::WriteAllText($Pending, $record, [Text.UnicodeEncoding]::new($false, $false))
    $pendingBefore = HashOf $Pending
    $o = Inst 2 @("uninstall", "--sim-scm-busy", "--sim-scm-fail", "delete")
    Check ((HashOf $Pending) -eq $pendingBefore) "rollback retains unrelated pending records and withdraws driver deletion"
    Check ((Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore) "failed busy uninstall restores the running service and original start type"
    Restart-Sim | Out-Null
    Check ((HashOf $tDriver) -eq $k1 -and -not (Test-Path -LiteralPath $external)) "reboot after rollback keeps qxpmap and still applies unrelated operation"

    Write-Host "== every new driver transaction failure point"
    $driverSweeps = @(
        @{ Name = "driver install"; Prep = {}; Cmd = @("driver", "--from", $Src1); Min = 4 },
        @{ Name = "driver update"; Prep = { Inst 0 @("driver", "--from", $Src1) | Out-Null }; Cmd = @("driver", "--from", $Src2); Min = 5 },
        @{ Name = "busy driver update"; Prep = { Inst 0 @("driver", "--from", $Src1) | Out-Null }; Cmd = @("driver", "--from", $Src2, "--sim-scm-busy"); Min = 4 },
        @{ Name = "busy full update"; Prep = { Inst 0 @("install", "--from", $Src1) | Out-Null }; Cmd = @("install", "--from", $Src2, "--sim-scm-busy", "--sim-wfp-deny"); Min = 10 },
        @{ Name = "driver uninstall"; Prep = { Inst 0 @("driver", "--from", $Src1) | Out-Null }; Cmd = @("uninstall"); Min = 3 },
        @{ Name = "busy driver uninstall"; Prep = { Inst 0 @("driver", "--from", $Src1) | Out-Null }; Cmd = @("uninstall", "--sim-scm-busy"); Min = 4 },
        @{ Name = "start-type normalization"; Prep = { Inst 0 @("driver", "--from", $Src1) | Out-Null; Set-ScmField 4 2 }; Cmd = @("driver", "--from", $Src1); Min = 1 }
    )
    foreach ($s in $driverSweeps) {
        $hits = 0
        $completed = $false
        for ($n = 1; $n -le 80; $n++) {
            New-Root
            & $s.Prep
            $snapshot = Snap $Win
            $scmBefore = HashOf $Scm
            $a = $s.Cmd + @("--fail-after", "$n")
            $out = (& $Installer @a --root $R | Out-String)
            $rc = $LASTEXITCODE
            if ($rc -ne 2) {
                $completed = $rc -eq 0 -or $rc -eq 3
                Check $completed ("{0}: completes after last failure point (exit {1})" -f $s.Name, $rc)
                break
            }
            $hits++
            $exact = (Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore
            Restart-Sim | Out-Null
            $exact = $exact -and (Snap $Win) -eq $snapshot -and (HashOf $Scm) -eq $scmBefore
            if (-not $exact) { Write-Host $out }
            Check ($exact -and $out.Contains("injected failure at step $n")) ("{0}: step {1} rolls back before and after reboot" -f $s.Name, $n)
        }
        Check ($completed -and $hits -ge $s.Min) ("{0}: all {1} failure stages covered" -f $s.Name, $hits)
    }

    Write-Host "== leftover wrapper DLLs"
    New-Root
    $games = Snap $R
    $o = Inst 0 @("scan")
    Has $o "qd3d: leftovers=8 moved=0 restored=0"
    foreach ($rel in @("Program Files\GameA\d3d9.dll", "Program Files\GameB\bin\d3d8.dll", "Program Files\GameE\wined3d.dll",
                       "Program Files\GameE\dxgi.dll", "Program Files\GameF\ddraw.dll", "Games\GameC\ddraw.dll",
                       "Games\WineGame\ddraw.dll", "GOG Games\GameD\opengl32.dll")) {
        Has $o (Join-Path $R $rel)
    }
    Has $o "GameB\bin\d3d8.dll [QD3D: "
    Has $o "GameF\ddraw.dll [QD3D: "
    Has $o "GameA\d3d9.dll [Microsoft: "
    Has $o "WineGame\ddraw.dll [foreign: Microsoft Corporation, Wine, Wine DirectDraw 5.3.1.904, "
    Has $o "GameC\ddraw.dll [foreign: no version information, 13 bytes]"
    Check (-not $o.Contains("d3dx9_30.dll")) "d3dx9_30.dll is not a wrapper"
    Check (-not $o.Contains("GameX")) "Documents and Settings is not a default game root"
    $o = Inst 0 @("scan", "--scan-dir", (Join-Path $R "Documents and Settings\User\Desktop"))
    Has $o "qd3d: leftovers=9 "
    $q = New-Dir (Join-Path $R "QD3D")
    Copy-Item -LiteralPath (Join-Path $Src1 "d3d9.dll"), (Join-Path $Src1 "d3d8.dll"), (Join-Path $Src1 "ddraw.dll") -Destination $q
    $o = Inst 0 @("scan")
    Has $o "qd3d: leftovers=11 "
    $o = Inst 0 @("scan", "--from", $q)
    Has $o "qd3d: leftovers=8 "
    Remove-Item -LiteralPath $q -Recurse
    $o = Inst 0 @("scan", "--move-leftovers")
    Has $o "qd3d: leftovers=8 moved=8 restored=0"
    Check (-not (Test-Path (Join-Path $R "Games\WineGame\ddraw.dll")) -and
           (Test-Path (Join-Path $R "Games\WineGame\ddraw.dll.qd3d-bak"))) "WineD3D's ddraw.dll renamed to ddraw.dll.qd3d-bak"
    Check (-not (Test-Path (Join-Path $R "Program Files\GameF\ddraw.dll")) -and
           (Test-Path (Join-Path $R "Program Files\GameF\ddraw.dll.qd3d-bak"))) "the per-game QD3D ddraw.dll renamed to ddraw.dll.qd3d-bak"
    $o = Inst 0 @("scan")
    Has $o "qd3d: leftovers=0 "
    Has $o "Moved aside earlier (*.qd3d-bak): 8"
    Write-Bytes (Join-Path $R "Games\GameC\ddraw.dll") "a new wrapper"
    $o = Inst 0 @("scan", "--move-leftovers")
    Has $o "not moved: $(Join-Path $R 'Games\GameC\ddraw.dll.qd3d-bak') exists"
    Has $o "qd3d: leftovers=1 moved=0"
    $o = Inst 0 @("scan", "--restore-leftovers")
    Has $o "not restored: $(Join-Path $R 'Games\GameC\ddraw.dll') exists"
    Has $o "restored=7"
    Remove-Item -LiteralPath (Join-Path $R "Games\GameC\ddraw.dll")
    $o = Inst 0 @("scan", "--restore-leftovers")
    Has $o "restored=1"
    Check ((Snap $R) -eq $games) "every wrapper DLL back unchanged"
    $o = Inst 1 @("install", "--move-leftovers")
} finally {
    foreach ($p in $script:holds) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
}

$secs = [int]((Get-Date) - $t0).TotalSeconds
if ($script:failures -eq 0) {
    Write-Host ("PASS: {0} checks in {1} s" -f $script:checks, $secs)
    if (-not $Keep) { Remove-Item -LiteralPath $Base -Recurse -Force }
    exit 0
}
Write-Host ("FAIL: {0} of {1} checks failed; root kept at {2}" -f $script:failures, $script:checks, $Base)
exit 1
