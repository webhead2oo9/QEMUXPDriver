# SPDX-License-Identifier: GPL-2.0-only
# Dot-sourced by the release and publication scripts: locating the paired
# core and checking that it matches this checkout.

function ConvertTo-WslPath([string]$Path) {
    $result = & wsl.exe -e wslpath -a $Path
    if ($LASTEXITCODE -ne 0 -or -not $result) { throw "wslpath failed for $Path" }
    return ([string]$result).Trim()
}

# Commands keep to single quotes: Windows PowerShell does not escape double
# quotes inside native arguments.
function Get-WslOutput([string]$Command) {
    $output = & wsl.exe -e bash -lc $Command
    if ($LASTEXITCODE -ne 0) { throw "WSL command failed (exit $LASTEXITCODE): $Command" }
    return (@($output) -join "`n").Trim()
}

# The order guest\paired-core.mk uses: core\ inside the checkout, else the
# sibling qemu-libretro-d3d9.
function Resolve-PairedCore([string]$Root, [string]$QemuDir) {
    if (-not $QemuDir) {
        $candidates = @((Join-Path $Root 'core'),
                        (Join-Path (Split-Path -Parent $Root) 'qemu-libretro-d3d9'))
        $QemuDir = $candidates | Where-Object {
            Test-Path -LiteralPath (Join-Path $_ 'hw\qd3d\SYNCED_FROM')
        } | Select-Object -First 1
        if (-not $QemuDir) { throw "no paired core at $($candidates -join ' or '): pass -QemuDir" }
    }
    return [IO.Path]::GetFullPath($QemuDir).TrimEnd('\')
}

function Get-NormalizedText([string]$Path) {
    return [IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
}

# What keeps the core from being this checkout's pair: uncommitted core
# changes (the core checkout belongs to WSL Git) and any difference between
# its hw\qd3d\proto and hw\qd3d\host and the files scripts/qd3d-sync.sh copies
# from qd3d\proto and qd3d\host.
function Get-CorePairingProblems([string]$Root, [string]$QemuDir) {
    $problems = New-Object Collections.Generic.List[string]
    $coreWsl = ConvertTo-WslPath $QemuDir
    if (Get-WslOutput "git -C '$coreWsl' status --porcelain --untracked-files=no") {
        $problems.Add('the core has uncommitted changes')
    }
    $synced = @(Get-ChildItem -LiteralPath (Join-Path $Root 'qd3d\proto') -Filter '*.h' -File |
                ForEach-Object { "proto\$($_.Name)" }) +
              @(Get-ChildItem -LiteralPath (Join-Path $Root 'qd3d\host') -File | Where-Object {
                  ($_.Extension -eq '.c' -or $_.Extension -eq '.h') -and
                  $_.Name -notmatch 'selftest|loopback'
              } | ForEach-Object { "host\$($_.Name)" })
    foreach ($relative in $synced) {
        $copy = Join-Path $QemuDir "hw\qd3d\$relative"
        if (-not (Test-Path -LiteralPath $copy -PathType Leaf)) {
            $problems.Add("the core lacks hw\qd3d\$relative (run scripts/qd3d-sync.sh)")
        } elseif ((Get-NormalizedText (Join-Path $Root "qd3d\$relative")) -cne (Get-NormalizedText $copy)) {
            $problems.Add("the core's hw\qd3d\$relative differs (run scripts/qd3d-sync.sh)")
        }
    }
    foreach ($directory in 'proto', 'host') {
        Get-ChildItem -LiteralPath (Join-Path $QemuDir "hw\qd3d\$directory") -File | Where-Object {
            $_.Name -ne 'meson.build' -and $synced -notcontains "$directory\$($_.Name)"
        } | ForEach-Object {
            $problems.Add("the core has hw\qd3d\$directory\$($_.Name), which XPDriver lacks")
        }
    }
    return , $problems
}
