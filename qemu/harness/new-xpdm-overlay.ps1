param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Name,
    [ValidateRange(1, 65535)]
    [int]$QmpPort = 44605,
    [string]$BasePath,
    # Diagnostic only. When set, the generated command line opens a gdb stub on
    # this port. The guest still boots freely because -S is deliberately not
    # passed; a debugger may attach at any time. Leave unset for every ordinary
    # run so the shipping command line is unchanged.
    [ValidateRange(0, 65535)]
    [int]$GdbPort = 0
)

$ErrorActionPreference = 'Stop'
$vmDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\winxp'))
if (-not $BasePath) {
    $BasePath = Join-Path $vmDir 'my_pc_disk.qcow2'
}
$base = (Resolve-Path -LiteralPath $BasePath).Path
if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
        (Split-Path -Parent $base), $vmDir)) {
    throw 'The XP base and generated overlay must share qemu\winxp so the backing filename remains portable'
}

$overlay = Join-Path $vmDir "$Name.qcow2"
$content = Join-Path $vmDir "xpdriver-$Name.qemu_cmd_line"
if ((Test-Path -LiteralPath $overlay) -or
    (Test-Path -LiteralPath $content)) {
    throw "Refusing to overwrite an existing overlay or content file for '$Name'"
}

function ConvertTo-WslPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Expected a drive-letter Windows path, got '$Path'"
    }
    $drive = $Matches[1].ToLowerInvariant()
    $tail = $Matches[2] -replace '\\', '/'
    return "/mnt/$drive/$tail"
}

$baseWsl = ConvertTo-WslPath $base
$overlayWsl = ConvertTo-WslPath $overlay
try {
    & wsl.exe qemu-img create -f qcow2 -F qcow2 -b $baseWsl $overlayWsl
    if ($LASTEXITCODE -ne 0) {
        throw "qemu-img create failed with exit code $LASTEXITCODE"
    }
    # The absolute WSL path is useful while creating the image but is not
    # meaningful to the Windows QEMU frontend. Store only the sibling base
    # filename so both Windows QEMU and WSL qemu-img resolve the same chain.
    & wsl.exe qemu-img rebase -u -f qcow2 -F qcow2 `
        -b (Split-Path -Leaf $base) $overlayWsl
    if ($LASTEXITCODE -ne 0) {
        throw "qemu-img rebase failed with exit code $LASTEXITCODE"
    }

    $overlayName = Split-Path -Leaf $overlay
    $line = "qemu-system-x86_64 -accel whpx,ssd=off -accel tcg " +
        "-audiodev libretro,id=snd0 -machine pcspk-audiodev=snd0 " +
        "-device AC97,audiodev=snd0 -vga std -global VGA.vgamem_mb=128 " +
        "-debugcon file:$Name-debug.log -global isa-debugcon.iobase=0xe9 " +
        "-m 1024M -smp 2 -hda $overlayName -nic none " +
        "-serial file:$Name-result.log -boot c " +
        "-qmp tcp:127.0.0.1:$QmpPort,server,nowait " +
        "-drive file=../../guest/acceptance/xpdriver-acceptance.iso," +
        "format=raw,media=cdrom,index=2,id=acceptcd"
    if ($GdbPort -gt 0) {
        $line += " -gdb tcp::$GdbPort"
    }
    $utf8 = New-Object Text.UTF8Encoding($false)
    # The libretro content parser treats a trailing CR as part of the final
    # option value. Keep this command-line format LF-only on Windows.
    [IO.File]::WriteAllText($content, $line + "`n", $utf8)
} catch {
    # Both paths were proven absent above, so anything present here belongs to
    # this failed creation attempt and must not poison a deterministic retry.
    if (Test-Path -LiteralPath $overlay) {
        Remove-Item -LiteralPath $overlay -Force
    }
    if (Test-Path -LiteralPath $content) {
        Remove-Item -LiteralPath $content -Force
    }
    throw
}

Write-Output "Overlay=$overlay"
Write-Output "Content=$content"
