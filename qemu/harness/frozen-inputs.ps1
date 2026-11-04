$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-identity.ps1')

# Frozen validation inputs: one manifest, produced outside any validation
# window, binds the staged package, acceptance ISO, retro host, and core by
# hash. Validation cycles consume it and never rebuild.

function Get-XpDefaultCorePath {
    param([string]$Workspace)

    if ($env:XPD_CORE) { return $env:XPD_CORE }
    # Match guest/paired-core.mk, without requiring build outputs for -DryRun.
    $coreRoot = Join-Path $Workspace 'core'
    if (-not (Test-Path -LiteralPath `
            (Join-Path $coreRoot 'hw\mesa\mglfunci.h') -PathType Leaf)) {
        $coreRoot = Join-Path (Split-Path -Parent $Workspace) `
            'qemu-libretro-d3d9'
    }
    return (Join-Path $coreRoot 'build-win64\libqemu_libretro.dll')
}

function Get-XpFrozenFileIdentity {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "frozen input file is missing: $full"
    }
    return [pscustomobject][ordered]@{
        path = $full
        bytes = (Get-Item -LiteralPath $full).Length
        sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    }
}

function Assert-XpFrozenExecutionInputs {
    param(
        [psobject]$Manifest,
        [string]$RetroHostPath,
        [string]$CorePath
    )

    if (-not $Manifest) {
        throw 'frozen execution input validation requires a manifest'
    }
    $result = [ordered]@{}
    foreach ($spec in @(
            @('retroHost', 'retro host', $RetroHostPath),
            @('core', 'paired core', $CorePath))) {
        $entry = $Manifest.($spec[0])
        if ($null -eq $entry -or -not $entry.path -or
            -not $entry.sha256) {
            throw "frozen inputs manifest lacks a $($spec[1]) record"
        }
        $expectedPath = [IO.Path]::GetFullPath([string]$entry.path)
        $selectedPath = [IO.Path]::GetFullPath([string]$spec[2])
        if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
                $selectedPath, $expectedPath)) {
            throw "selected $($spec[1]) path differs from frozen inputs"
        }
        $identity = Get-XpFrozenFileIdentity -Path $selectedPath
        if ($identity.sha256 -cne [string]$entry.sha256 -or
            ($entry.bytes -and $identity.bytes -ne [int64]$entry.bytes)) {
            throw "selected $($spec[1]) identity differs from frozen inputs"
        }
        $result[$spec[0]] = $identity
    }
    return [pscustomobject]$result
}

function Get-XpTransportAbiVersion {
    param([string]$Workspace)

    $header = Join-Path $Workspace 'guest\xpdm\qxp_d3d_transport.h'
    if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
        throw "transport header is missing: $header"
    }
    $match = [regex]::Match((Get-Content -Raw -LiteralPath $header),
        '(?m)^#define\s+QXP_D3D_TRANSPORT_VERSION\s+(\d+)')
    if (-not $match.Success) {
        throw 'QXP_D3D_TRANSPORT_VERSION is not defined in the transport header'
    }
    return [int]$match.Groups[1].Value
}

function Compare-XpFrozenPackageFiles {
    param(
        [object[]]$ManifestFiles,
        [object[]]$CurrentFiles
    )

    $left = @($ManifestFiles | Sort-Object name | ForEach-Object {
        "$($_.name):$($_.size):$($_.sha256)" }) -join ';'
    $right = @($CurrentFiles | Sort-Object name | ForEach-Object {
        "$($_.name):$($_.size):$($_.sha256)" }) -join ';'
    return ($left -ceq $right)
}

function Get-XpFrozenInputs {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw ('frozen inputs manifest is missing: {0}; run ' +
            'qemu/harness/publish-xp-acceptance-inputs.ps1 first') -f $full
    }
    try {
        $manifest = Get-Content -Raw -LiteralPath $full | ConvertFrom-Json
    } catch {
        throw "frozen inputs manifest is invalid JSON: $($_.Exception.Message)"
    }
    $findings = [Collections.Generic.List[string]]::new()
    if ([int]$manifest.schemaVersion -ne 1 -or
            [string]$manifest.kind -cne 'xpdriver-frozen-inputs') {
        $findings.Add('frozen inputs manifest has an unsupported schema')
    }
    $manifestTransportAbi = $null
    if ($null -ne $manifest.transportAbi) {
        try {
            $manifestTransportAbi = [uint32]$manifest.transportAbi
        } catch {
            $manifestTransportAbi = $null
        }
    }
    if ($null -eq $manifestTransportAbi) {
        $findings.Add('frozen inputs manifest lacks a transport ABI record')
    }
    foreach ($section in @(
            @('acceptanceIso', 'acceptance ISO'),
            @('retroHost', 'retro host'),
            @('core', 'paired core'))) {
        $entry = $manifest.($section[0])
        if ($null -eq $entry -or -not $entry.path -or -not $entry.sha256) {
            $findings.Add("frozen inputs manifest lacks a $($section[1]) record")
            continue
        }
        if (-not (Test-Path -LiteralPath ([string]$entry.path) -PathType Leaf)) {
            $findings.Add("frozen $($section[1]) is missing: $($entry.path)")
            continue
        }
        $identity = Get-XpFrozenFileIdentity -Path ([string]$entry.path)
        if ($identity.sha256 -cne [string]$entry.sha256 -or
                ($entry.bytes -and
                    $identity.bytes -ne [int64]$entry.bytes)) {
            $findings.Add("frozen $($section[1]) identity differs on disk")
        }
    }
    $packageDirectory = [string]$manifest.package.directory
    if (-not $packageDirectory) {
        $findings.Add('frozen inputs manifest lacks a package directory')
    } else {
        $package = Get-XpAcceptancePackageIdentity `
            -Directory $packageDirectory
        if (-not $package.valid) {
            $findings.Add('frozen package identity is invalid: ' +
                ($package.findings -join '; '))
        } elseif (-not (Compare-XpFrozenPackageFiles `
                -ManifestFiles @($manifest.package.files) `
                -CurrentFiles @($package.files))) {
            $findings.Add('frozen package files differ on disk')
        } else {
            # The bytes above now match the frozen hashes, so their embedded
            # ABI markers are the frozen ABI; the manifest must agree with
            # them, never only with a live source header.
            try {
                $packagedTransportAbi = Get-XpPackagedTransportAbi `
                    -Directory $packageDirectory
                if ($null -ne $manifestTransportAbi -and
                        $packagedTransportAbi -ne $manifestTransportAbi) {
                    $finding = ("frozen package transport ABI {0} " +
                        "differs from manifest ABI {1}") -f @(
                            $packagedTransportAbi, $manifestTransportAbi)
                    $findings.Add($finding)
                }
            } catch {
                $findings.Add(("frozen package transport ABI could not " +
                    "be decoded: " + $_.Exception.Message))
            }
        }
    }
    if ($findings.Count) {
        throw ('frozen inputs validation failed: ' +
            ($findings -join '; '))
    }
    return $manifest
}
