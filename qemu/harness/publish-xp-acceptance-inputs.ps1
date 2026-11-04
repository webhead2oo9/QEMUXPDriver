param(
    [string]$PackageDirectory,
    [string]$IsoPath,
    [string]$RetroHostPath = $env:XPD_RETRO_HOST,
    [string]$CorePath,
    [string]$OutputPath,
    [string]$SnapshotDirectory,
    [string]$ObjectStorePath,
    [switch]$SkipBuild
)

# Produces one immutable frozen-input generation at the selected output path.
# Building happens HERE—never inside a validation window. Name a new ISO and
# manifest when guest tools intentionally change; do not overwrite a generation
# already referenced by historical evidence. Publication is append-only: an
# existing output may be regenerated only when the canonical content is
# byte-identical; any other change requires a new output path.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-identity.ps1')
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'acceptance-iso.ps1')

function ConvertTo-XpFrozenWslPath {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "expected a drive-letter Windows path, got '$Path'"
    }
    $drive = $Matches[1].ToLowerInvariant()
    $tail = $Matches[2] -replace '\\', '/'
    return "/mnt/$drive/$tail"
}


function Get-XpFrozenPackageDigest {
    param([object[]]$Files)

    $description = @($Files | Sort-Object name | ForEach-Object {
        "$($_.name):$($_.size):$($_.sha256)"
    }) -join ';'
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $algorithm.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($description))
    } finally {
        $algorithm.Dispose()
    }
    return ([BitConverter]::ToString($digest)).Replace('-', '')
}

function Publish-XpFrozenObject {
    param(
        [string]$SourcePath,
        [string]$StorePath
    )

    $source = Get-XpFrozenFileIdentity -Path $SourcePath
    [void][IO.Directory]::CreateDirectory($StorePath)
    $extension = [IO.Path]::GetExtension([string]$source.path).ToLowerInvariant()
    $destination = Join-Path $StorePath `
        (([string]$source.sha256) + $extension)
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
            throw "frozen object destination is not a file: $destination"
        }
        $existing = Get-XpFrozenFileIdentity -Path $destination
        if ($existing.sha256 -cne $source.sha256 -or
                $existing.bytes -ne $source.bytes) {
            throw "frozen object identity collision: $destination"
        }
        return $existing
    }

    $temporary = "$destination.tmp-$PID-$([Guid]::NewGuid().ToString('N'))"
    try {
        [IO.File]::Copy([string]$source.path, $temporary, $false)
        $copied = Get-XpFrozenFileIdentity -Path $temporary
        if ($copied.sha256 -cne $source.sha256 -or
                $copied.bytes -ne $source.bytes) {
            throw "frozen object copy differs from source: $SourcePath"
        }
        try {
            [IO.File]::Move($temporary, $destination)
        } catch {
            if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                throw
            }
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
    }
    $published = Get-XpFrozenFileIdentity -Path $destination
    if ($published.sha256 -cne $source.sha256 -or
            $published.bytes -ne $source.bytes) {
        throw "published frozen object differs from source: $destination"
    }
    return $published
}


$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $RetroHostPath) {
    throw 'Pass -RetroHostPath or set XPD_RETRO_HOST to retro_host.exe'
}
if (-not $CorePath) {
    $CorePath = Get-XpDefaultCorePath -Workspace $workspace
}
if (-not $PackageDirectory) {
    $PackageDirectory = Join-Path $workspace 'guest\xpdm\build\package'
}
if (-not $IsoPath) {
    $IsoPath = Join-Path $workspace 'guest\acceptance\xpdriver-acceptance.iso'
}
if (-not $OutputPath) {
    $OutputPath = Join-Path $workspace 'guest\acceptance\frozen-inputs.json'
}
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$IsoPath = [IO.Path]::GetFullPath($IsoPath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $OutputPath
[void][IO.Directory]::CreateDirectory($outputParent)

if (-not $SkipBuild) {
    Push-Location $workspace
    try {
        & wsl.exe make -C guest/acceptance xpdm-iso `
            "XPDM_PACKAGE=$(ConvertTo-XpFrozenWslPath -Path $PackageDirectory)" `
            "ISO_OUTPUT=$(ConvertTo-XpFrozenWslPath -Path $IsoPath)"
        $buildExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if ($buildExit -ne 0) {
        throw "acceptance ISO build failed with exit code $buildExit"
    }
}

$sourcePackage = Get-XpAcceptancePackageIdentity `
    -Directory $PackageDirectory
if (-not $sourcePackage.valid) {
    throw "staged package identity is invalid: $($sourcePackage.findings -join '; ')"
}
$transportAbi = Get-XpPackagedTransportAbi -Directory $PackageDirectory
$sourceTransportAbi = Get-XpTransportAbiVersion -Workspace $workspace
if ($transportAbi -ne $sourceTransportAbi) {
    throw ("staged package transport ABI {0} differs from source ABI {1}" -f
        $transportAbi, $sourceTransportAbi)
}

$sourceIso = Get-XpFrozenFileIdentity -Path $IsoPath
$sourceHost = Get-XpFrozenFileIdentity -Path $RetroHostPath
$isoPackageFiles = @(Get-XpAcceptanceIsoPackageFiles -Path $IsoPath)
if (-not (Compare-XpFrozenPackageFiles `
        -ManifestFiles @($sourcePackage.files) `
        -CurrentFiles $isoPackageFiles)) {
    throw "acceptance ISO does not embed the selected package"
}

$sourceCore = Get-XpFrozenFileIdentity -Path $CorePath
$packageDigest = Get-XpFrozenPackageDigest -Files @($sourcePackage.files)

if (-not $SnapshotDirectory) {
    $snapshotName = ('{0}.inputs-{1}-{2}' -f
        [IO.Path]::GetFileNameWithoutExtension($OutputPath),
        $packageDigest.Substring(0, 16),
        ([string]$sourceIso.sha256).Substring(0, 16))
    $SnapshotDirectory = Join-Path $outputParent $snapshotName
}
$SnapshotDirectory = [IO.Path]::GetFullPath($SnapshotDirectory)
$snapshotPackageDirectory = Join-Path $SnapshotDirectory 'package'
$snapshotIsoPath = Join-Path $SnapshotDirectory 'acceptance.iso'

if (-not (Test-Path -LiteralPath $SnapshotDirectory)) {
    $temporarySnapshot = "$SnapshotDirectory.tmp-$PID-$([Guid]::NewGuid().ToString('N'))"
    try {
        [void][IO.Directory]::CreateDirectory(
            (Join-Path $temporarySnapshot 'package'))
        foreach ($file in @($sourcePackage.files)) {
            [IO.File]::Copy(
                (Join-Path $PackageDirectory ([string]$file.name)),
                (Join-Path (Join-Path $temporarySnapshot 'package') `
                    ([string]$file.name)),
                $false)
        }
        [IO.File]::Copy(
            [string]$sourceIso.path,
            (Join-Path $temporarySnapshot 'acceptance.iso'),
            $false)
        $temporaryPackage = Get-XpAcceptancePackageIdentity `
            -Directory (Join-Path $temporarySnapshot 'package')
        $temporaryIso = Get-XpFrozenFileIdentity `
            -Path (Join-Path $temporarySnapshot 'acceptance.iso')
        if (-not $temporaryPackage.valid -or
                -not (Compare-XpFrozenPackageFiles `
                    -ManifestFiles @($sourcePackage.files) `
                    -CurrentFiles @($temporaryPackage.files)) -or
                $temporaryIso.sha256 -cne $sourceIso.sha256 -or
                $temporaryIso.bytes -ne $sourceIso.bytes) {
            throw 'frozen package or ISO snapshot differs from its source'
        }
        try {
            [IO.Directory]::Move($temporarySnapshot, $SnapshotDirectory)
        } catch {
            if (-not (Test-Path -LiteralPath $SnapshotDirectory `
                    -PathType Container)) {
                throw
            }
        }
    } finally {
        Remove-Item -LiteralPath $temporarySnapshot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
if (-not (Test-Path -LiteralPath $SnapshotDirectory -PathType Container)) {
    throw "frozen snapshot destination is not a directory: $SnapshotDirectory"
}

$package = Get-XpAcceptancePackageIdentity `
    -Directory $snapshotPackageDirectory
$isoIdentity = Get-XpFrozenFileIdentity -Path $snapshotIsoPath
if (-not $package.valid -or
        -not (Compare-XpFrozenPackageFiles `
            -ManifestFiles @($sourcePackage.files) `
            -CurrentFiles @($package.files)) -or
        $isoIdentity.sha256 -cne $sourceIso.sha256 -or
        $isoIdentity.bytes -ne $sourceIso.bytes) {
    throw "existing frozen snapshot differs from selected inputs: $SnapshotDirectory"
}

if (-not $ObjectStorePath) {
    $ObjectStorePath = Join-Path $workspace `
        'artifacts\frozen-inputs\objects\sha256'
}
$ObjectStorePath = [IO.Path]::GetFullPath($ObjectStorePath)
$hostIdentity = Publish-XpFrozenObject `
    -SourcePath ([string]$sourceHost.path) -StorePath $ObjectStorePath
$coreIdentity = Publish-XpFrozenObject `
    -SourcePath ([string]$sourceCore.path) -StorePath $ObjectStorePath

$manifest = [pscustomobject][ordered]@{
    schemaVersion = 1
    kind = 'xpdriver-frozen-inputs'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    transportAbi = $transportAbi
    snapshot = [pscustomobject][ordered]@{
        directory = $SnapshotDirectory
        packageDigest = $packageDigest
    }
    package = [pscustomobject][ordered]@{
        directory = $snapshotPackageDirectory
        driverVer = $package.driverVer
        signerThumbprint = $package.certificate.thumbprint
        files = @($package.files)
    }
    acceptanceIso = $isoIdentity
    retroHost = $hostIdentity
    core = $coreIdentity
}
$manifestText = ($manifest | ConvertTo-Json -Depth 10) +
    [Environment]::NewLine
if (Test-Path -LiteralPath $OutputPath) {
    if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "frozen inputs manifest destination is not a file: $OutputPath"
    }
    # Append-only: an existing generation may be republished only when the
    # canonical content is unchanged. createdUtc is the one field derived
    # from the clock, so adopt the recorded timestamp before comparing bytes.
    $existingText = [IO.File]::ReadAllText($OutputPath)
    $existingCreatedUtc = $null
    try {
        $existingCreatedUtc = [string](
            ($existingText | ConvertFrom-Json).createdUtc)
    } catch {
        $existingCreatedUtc = $null
    }
    if ($existingCreatedUtc) {
        $manifest.createdUtc = $existingCreatedUtc
        $manifestText = ($manifest | ConvertTo-Json -Depth 10) +
            [Environment]::NewLine
    }
    if ($existingText -cne $manifestText) {
        throw ("frozen inputs manifest already exists and would change " +
            "(frozen generations are append-only; choose a new output " +
            "path): $OutputPath")
    }
} else {
    Write-XpAcceptanceAtomicText -Path $OutputPath -Text $manifestText
}

# The record just written must validate against independent snapshots, never
# against the mutable build outputs that produced them.
[void](Get-XpFrozenInputs -Path $OutputPath)
Write-Output (("FrozenInputs driverVer={0} signer={1} iso={2} core={3} " +
    "abi={4} output={5}") -f
    $package.driverVer, $package.certificate.thumbprint,
    $isoIdentity.sha256, $coreIdentity.sha256, $transportAbi, $OutputPath)
