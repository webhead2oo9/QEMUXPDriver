# SPDX-License-Identifier: GPL-2.0-only
<#
Builds an XPDriver release from clean XPDriver and paired-core checkouts:

  dist\XPDriver-<version>\      the bundle (packaging\README.txt describes it)
  dist\XPDriver-<version>.zip   the same bundle, zipped

  tools\make-release.ps1 -SourceUrl https://example.org/XPDriver

1. Identity. Refuses uncommitted changes in either checkout, a core whose
   hw\qd3d\proto and hw\qd3d\host differ from this checkout's qd3d\proto and
   qd3d\host (the files scripts/qd3d-sync.sh copies), a missing XPD_SIGN_PFX
   (the persistent project certificate, tools\new-signing-cert.ps1) and a
   missing -SourceUrl. -Rehearsal builds anyway, as
   XPDriver-<version>-rehearsal.
2. Build. guest\xpdm\stage-package.cmd (builds, stamps and signs the XPDM
   package), the tools CD with that package (make -C qd3d/cd iso XPDM=1) and
   the core (ninja in -CoreBuildDir, default build-release, which is
   configured first when new). -SkipBuild assembles the current outputs.
3. Assembly. The core, which must carry the core commit recorded for it,
   its firmware (pc-bios), an empty 32 GB disk, the example content files,
   the CD, the package, licences and notices and README.txt; then
   MANIFEST.txt (version, commits, QD3D ABI, DriverVer, signer, SHA-256 of
   every file) and the zip.

Needs the XP WDK (-WdkRoot), WSL with the mingw-w64 toolchains, genisoimage,
ninja and qemu-img, Python 3 as `py -3`, and the core's libretro-common and
zlib submodules (BUILDING.md).
#>
param(
    [string]$SourceUrl,
    [string]$QemuDir,
    [string]$OutDir,
    [string]$WdkRoot = 'C:\WinDDK\7600.16385.1',
    [string]$CoreBuildDir = 'build-release',
    [switch]$Rehearsal,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
. (Join-Path $PSScriptRoot 'core-pairing.ps1')

# Commands keep to single quotes: Windows PowerShell does not escape double
# quotes inside native arguments.
function Invoke-Wsl([string]$What, [string]$Command) {
    & wsl.exe -e bash -lc $Command
    if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
}

function Write-CrlfText([string]$Path, [string]$Text) {
    $crlf = $Text.Replace("`r`n", "`n").Replace("`n", "`r`n")
    [IO.File]::WriteAllText($Path, $crlf, (New-Object Text.UTF8Encoding $false))
}

# --- 1. Identity -----------------------------------------------------------

$QemuDir = Resolve-PairedCore $root $QemuDir
$rootWsl = ConvertTo-WslPath $root
$coreWsl = ConvertTo-WslPath $QemuDir

$problems = New-Object Collections.Generic.List[string]
if (& git -C $root status --porcelain --untracked-files=no) {
    $problems.Add('XPDriver has uncommitted changes')
}
foreach ($problem in (Get-CorePairingProblems $root $QemuDir)) { $problems.Add($problem) }
if (-not $env:XPD_SIGN_PFX) {
    $problems.Add('XPD_SIGN_PFX is not set: releases are signed with the project certificate')
}
if (-not $SourceUrl) { $problems.Add('-SourceUrl is missing: where this release''s source is published') }
if ($problems.Count) {
    if (-not $Rehearsal) {
        throw ("not releasable:`n  " + ($problems -join "`n  ") +
               "`n-Rehearsal builds anyway, named as a rehearsal")
    }
    foreach ($problem in $problems) { Write-Warning "rehearsal: $problem" }
}
if (-not $SourceUrl) { $SourceUrl = '(rehearsal: no source URL)' }

# --- 2. Build --------------------------------------------------------------

if (-not $SkipBuild) {
    if ($WdkRoot -match '\s') { throw "-WdkRoot must not contain spaces: $WdkRoot" }
    if (-not $env:ALLUSERSPROFILE) { $env:ALLUSERSPROFILE = 'C:\ProgramData' }
    Push-Location -LiteralPath $root
    try {
        & cmd.exe /d /c "guest\xpdm\stage-package.cmd $WdkRoot"
        if ($LASTEXITCODE -ne 0) { throw "stage-package.cmd failed (exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
    Invoke-Wsl 'the tools CD' "make -C '$rootWsl/qd3d/cd' iso XPDM=1"
    # The release core gets its own build directory, configured here as the
    # core's README configures the Windows cross build; a developer's build
    # directory (and the DLL a running RetroArch holds) stays untouched.
    if (-not (Test-Path -LiteralPath (Join-Path $QemuDir "$CoreBuildDir\build.ninja"))) {
        Invoke-Wsl 'configuring the core' (
            "mkdir -p '$coreWsl/$CoreBuildDir' && cd '$coreWsl/$CoreBuildDir' && " +
            "CFLAGS='-O2 -Wno-error -Wno-nested-externs -Wno-redundant-decls' ../configure " +
            '--target-list=x86_64-softmmu --without-default-features --glib=internal ' +
            '--zlib=internal --disable-pie --enable-fdt=internal --disable-modules ' +
            '--disable-plugins --enable-libretro --audio-drv-list=libretro --disable-sdl ' +
            '--enable-slirp -Dwrap_mode=forcefallback -Db_staticpic=true ' +
            '--cross-prefix=x86_64-w64-mingw32- --extra-ldflags=-static --enable-whpx ' +
            '--enable-qemu-3dfx')
    }
    Invoke-Wsl 'the core' "ninja -C '$coreWsl/$CoreBuildDir' libqemu_libretro.dll"
}

# Read after the build: staging and the core build regenerate both.
$version = ([string](& py -3 (Join-Path $root 'tools\version.py'))).Trim()
$versionFull = ([string](& py -3 (Join-Path $root 'tools\version.py') --full)).Trim()
if ($LASTEXITCODE -ne 0 -or -not $version) { throw 'tools\version.py failed' }
$xpdCommit = ([string](& git -C $root rev-parse HEAD)).Trim()
$coreCommit = Get-WslOutput "git -C '$coreWsl' rev-parse HEAD"
# The core embeds what scripts/qemu-version.sh records at build time.
$coreDescribe = Get-WslOutput ("cd '$coreWsl' && { git describe --match 'v*' --dirty 2>/dev/null || " +
                               "git describe --always --dirty; }")
$coreDll = Join-Path $QemuDir "$CoreBuildDir\libqemu_libretro.dll"
& wsl.exe -e grep -qF "($coreDescribe)" (ConvertTo-WslPath $coreDll)
if ($LASTEXITCODE -ne 0) {
    throw "$coreDll does not carry the core's identity ($coreDescribe): rebuild the core"
}

# --- 3. Assembly -----------------------------------------------------------

$name = if ($problems.Count) { "XPDriver-$version-rehearsal" } else { "XPDriver-$version" }
$bundle = Join-Path $OutDir $name
$zip = "$bundle.zip"
foreach ($old in $bundle, $zip) {
    if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Recurse -Force }
}
$cores = Join-Path $bundle 'retroarch\cores'
$firmware = Join-Path $bundle 'retroarch\system\qemu'
$vm = Join-Path $bundle 'vm'
$xpdm = Join-Path $bundle 'xpdm'
$licenses = Join-Path $bundle 'licenses'
foreach ($directory in $cores, $firmware, $vm, $xpdm, $licenses) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

Copy-Item -LiteralPath $coreDll -Destination (Join-Path $cores 'qemu_xpdriver_libretro.dll')
# The firmware set the VM was tested with.
foreach ($file in 'bios-256k.bin', 'bios.bin', 'efi-e1000.rom', 'efi-e1000e.rom',
                  'kvmvapic.bin', 'linuxboot.bin', 'linuxboot_dma.bin', 'multiboot.bin',
                  'multiboot_dma.bin', 'pvh.bin', 'vgabios-ati.bin',
                  'vgabios-bochs-display.bin', 'vgabios-cirrus.bin', 'vgabios-qxl.bin',
                  'vgabios-ramfb.bin', 'vgabios-stdvga.bin', 'vgabios-virtio.bin',
                  'vgabios-vmware.bin', 'vgabios.bin') {
    Copy-Item -LiteralPath (Join-Path $QemuDir "pc-bios\$file") -Destination $firmware
}
# Content files are written LF whatever the checkout holds: the core splits
# them with g_shell_parse_argv, which would keep a CR in the last argument.
foreach ($file in 'xpdriver.qemu_cmd_line', 'xpdriver-install.qemu_cmd_line') {
    $text = [IO.File]::ReadAllText((Join-Path $root "packaging\$file")).Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $vm $file), $text, (New-Object Text.UTF8Encoding $false))
}
Copy-Item -LiteralPath (Join-Path $root 'qd3d\cd\build\qd3d-tools.iso') `
    -Destination (Join-Path $vm 'xpdriver-tools.iso')
Invoke-Wsl 'the empty disk' ("qemu-img create -q -f qcow2 '" +
                             (ConvertTo-WslPath (Join-Path $vm 'winxp.qcow2')) + "' 32G")
Get-ChildItem -LiteralPath (Join-Path $root 'guest\xpdm\build\package') -File |
    Copy-Item -Destination $xpdm

$readme = [IO.File]::ReadAllText((Join-Path $root 'packaging\README.txt'))
$readme = $readme.Replace('@VERSION@', $version).Replace('@XPD_COMMIT@', $xpdCommit.Substring(0, 10))
$readme = $readme.Replace('@CORE_COMMIT@', $coreCommit.Substring(0, 10)).Replace('@SOURCE_URL@', $SourceUrl)
Write-CrlfText (Join-Path $bundle 'README.txt') $readme
Write-CrlfText (Join-Path $bundle 'LICENSE.txt') ([IO.File]::ReadAllText((Join-Path $root 'LICENSE')))
Write-CrlfText (Join-Path $bundle 'THIRD_PARTY_NOTICES.txt') `
    ([IO.File]::ReadAllText((Join-Path $root 'THIRD_PARTY_NOTICES.md')))
foreach ($pair in @(@('LICENSE', 'qemu-LICENSE.txt'), @('COPYING', 'qemu-COPYING.txt'),
                    @('COPYING.LIB', 'qemu-COPYING.LIB.txt'), @('pc-bios\README', 'pc-bios-README.txt'))) {
    Write-CrlfText (Join-Path $licenses $pair[1]) ([IO.File]::ReadAllText((Join-Path $QemuDir $pair[0])))
}

$abiMatch = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'qd3d\proto\qd3d_proto.h')),
                           '#define QD3D_ABI_VERSION (\d+)u')
if (-not $abiMatch.Success) { throw 'QD3D_ABI_VERSION not found in qd3d\proto\qd3d_proto.h' }
$driverVerMatch = [regex]::Match([IO.File]::ReadAllText((Join-Path $xpdm 'qxp.inf')),
                                 '(?m)^\s*DriverVer\s*=\s*(\S+)')
if (-not $driverVerMatch.Success) { throw 'the staged qxp.inf has no DriverVer' }
$signer = (Get-AuthenticodeSignature -LiteralPath (Join-Path $xpdm 'qxp.cat')).SignerCertificate
if (-not $signer) { throw 'the staged qxp.cat has no signer' }
$bundledCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $xpdm 'xpdriver-test.cer')
if ($bundledCertificate.Thumbprint -ne $signer.Thumbprint) {
    throw 'the catalog signer is not the bundled xpdriver-test.cer'
}
$syncedFrom = ([IO.File]::ReadAllText((Join-Path $QemuDir 'hw\qd3d\SYNCED_FROM'))).Trim()

$manifest = New-Object Collections.Generic.List[string]
$manifest.Add("XPDriver $versionFull")
$manifest.Add('')
$manifest.Add("version           $version")
$manifest.Add("xpdriver_commit   $xpdCommit")
$manifest.Add("core_commit       $coreCommit")
$manifest.Add("core_identity     $coreDescribe")
$manifest.Add("core_synced_from  $syncedFrom")
$manifest.Add("qd3d_abi          $($abiMatch.Groups[1].Value)")
$manifest.Add("driverver         $($driverVerMatch.Groups[1].Value)")
$manifest.Add("package_signer    $($signer.Thumbprint) $($signer.Subject)")
$manifest.Add("source            $SourceUrl")
if ($problems.Count) {
    foreach ($problem in $problems) { $manifest.Add("rehearsal         $problem") }
}
$manifest.Add('')
$manifest.Add('sha256                                                            bytes  file')
$prefix = $bundle.Length + 1
Get-ChildItem -LiteralPath $bundle -Recurse -File | Sort-Object { $_.FullName.Substring($prefix) } |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $manifest.Add(('{0}  {1,10}  {2}' -f $hash, $_.Length, $_.FullName.Substring($prefix)))
    }
Write-CrlfText (Join-Path $bundle 'MANIFEST.txt') (($manifest -join "`n") + "`n")

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($bundle, $zip,
    [IO.Compression.CompressionLevel]::Optimal, $true)
Write-Host "Release: $bundle"
Write-Host "         $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB)"
