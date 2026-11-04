param(
    [string]$RegistryPath,
    [switch]$RequireLocalAssets
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $RegistryPath) {
    $RegistryPath = Join-Path $workspace 'qemu\harness\test-targets.json'
}
$registryFile = (Resolve-Path -LiteralPath $RegistryPath).Path
$registry = Get-Content -LiteralPath $registryFile -Raw | ConvertFrom-Json

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-WorkspaceRelativePath {
    param(
        [string]$RelativePath,
        [string]$Field
    )
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) `
        "$Field must not be empty"
    Assert-Condition (-not [IO.Path]::IsPathRooted($RelativePath)) `
        "$Field must be relative to the workspace: $RelativePath"
    $native = $RelativePath -replace '/', '\'
    $full = [IO.Path]::GetFullPath((Join-Path $workspace $native))
    $prefix = $workspace.TrimEnd('\') + '\'
    Assert-Condition ($full.StartsWith(
            $prefix, [StringComparison]::OrdinalIgnoreCase)) `
        "$Field escapes the workspace: $RelativePath"
    return $full
}

function Require-Properties {
    param(
        [object]$Object,
        [string[]]$Names,
        [string]$Label
    )
    $present = @($Object.PSObject.Properties.Name)
    foreach ($name in $Names) {
        Assert-Condition ($present -contains $name) `
            "$Label is missing required property '$name'"
    }
}

Assert-Condition ($registry.schemaVersion -eq 1) `
    "Unsupported registry schemaVersion '$($registry.schemaVersion)'"
Assert-Condition ($null -ne $registry.current) `
    'Registry must define current target references'
Assert-Condition ($null -ne $registry.targets -and $registry.targets.Count -gt 0) `
    'Registry must contain at least one target'

$required = @(
    'id', 'purpose', 'lifecycle', 'mutationPolicy', 'contentPath',
    'diskPath', 'backingDiskPath', 'installedDriver', 'network', 'qmpPort',
    'launcher', 'allowedTiers', 'lastVerified', 'notes'
)
$ids = @{}
$ports = @{}
$validLifecycles = @('preserved-checkpoint', 'preserved-base', 'disposable')
$validPolicies = @('preserve', 'child-only', 'disposable')
$validNetworks = @('offline', 'user', 'unknown')
$validTiers = @('T0', 'T1', 'T2', 'T3')

foreach ($target in @($registry.targets)) {
    Require-Properties $target $required 'Target'
    Assert-Condition ($target.id -match '^[a-z0-9][a-z0-9._-]*$') `
        "Invalid target id '$($target.id)'"
    Assert-Condition (-not $ids.ContainsKey($target.id)) `
        "Duplicate target id '$($target.id)'"
    $ids[$target.id] = $true

    Assert-Condition ($validLifecycles -contains $target.lifecycle) `
        "Target '$($target.id)' has invalid lifecycle '$($target.lifecycle)'"
    Assert-Condition ($validPolicies -contains $target.mutationPolicy) `
        "Target '$($target.id)' has invalid mutationPolicy '$($target.mutationPolicy)'"
    Assert-Condition ($validNetworks -contains $target.network) `
        "Target '$($target.id)' has invalid network '$($target.network)'"
    Assert-Condition ($target.allowedTiers.Count -gt 0) `
        "Target '$($target.id)' must allow at least one validation tier"
    foreach ($tier in @($target.allowedTiers)) {
        Assert-Condition ($validTiers -contains $tier) `
            "Target '$($target.id)' has invalid validation tier '$tier'"
    }

    $disk = Resolve-WorkspaceRelativePath $target.diskPath `
        "Target '$($target.id)' diskPath"
    if ($RequireLocalAssets) {
        Assert-Condition (Test-Path -LiteralPath $disk -PathType Leaf) `
            "Target '$($target.id)' disk is missing: $disk"
    }

    if ($null -ne $target.backingDiskPath) {
        $backing = Resolve-WorkspaceRelativePath $target.backingDiskPath `
            "Target '$($target.id)' backingDiskPath"
        if ($RequireLocalAssets) {
            Assert-Condition (Test-Path -LiteralPath $backing -PathType Leaf) `
                "Target '$($target.id)' backing disk is missing: $backing"
        }
    }

    if ($null -eq $target.contentPath) {
        Assert-Condition ($null -eq $target.qmpPort) `
            "Target '$($target.id)' has no contentPath but declares a QMP port"
        continue
    }

    $content = Resolve-WorkspaceRelativePath $target.contentPath `
        "Target '$($target.id)' contentPath"
    if ($RequireLocalAssets) {
        Assert-Condition (Test-Path -LiteralPath $content -PathType Leaf) `
            "Target '$($target.id)' content file is missing: $content"
    }
    if (-not (Test-Path -LiteralPath $content -PathType Leaf)) {
        continue
    }

    Assert-Condition ($null -ne $target.qmpPort -and
        [int]$target.qmpPort -ge 1 -and [int]$target.qmpPort -le 65535) `
        "Target '$($target.id)' must declare a valid QMP port"
    Assert-Condition (-not $ports.ContainsKey([string]$target.qmpPort)) `
        "QMP port $($target.qmpPort) is assigned to multiple targets"
    $ports[[string]$target.qmpPort] = $target.id

    $line = Get-Content -LiteralPath $content -Raw
    Assert-Condition ($line -match
        '(?i)-qmp\s+tcp:(?:127\.0\.0\.1|localhost):(\d+)(?:[,\s]|$)') `
        "Target '$($target.id)' content file has no loopback TCP QMP port"
    $contentPort = [int]$Matches[1]
    Assert-Condition ($contentPort -eq [int]$target.qmpPort) `
        "Target '$($target.id)' registry QMP port $($target.qmpPort) does not match content port $contentPort"

    Assert-Condition ($line -match '(?i)-hda\s+(?:"([^"]+)"|([^\s]+))') `
        "Target '$($target.id)' content file has no -hda disk"
    $contentDisk = if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
    Assert-Condition ([StringComparer]::OrdinalIgnoreCase.Equals(
            (Split-Path -Leaf $contentDisk), (Split-Path -Leaf $disk))) `
        "Target '$($target.id)' content disk '$contentDisk' does not match registry disk '$($target.diskPath)'"

    if ($target.network -eq 'offline') {
        Assert-Condition ($line -match '(?i)(?:^|\s)-nic\s+none(?:\s|$)') `
            "Target '$($target.id)' is marked offline but content does not use -nic none"
    }
}

Require-Properties $registry.current @(
    'fullCheckpointTarget', 'manualGamesTarget', 'gamesBackingTarget'
) 'Current target references'
foreach ($property in @($registry.current.PSObject.Properties)) {
    Assert-Condition ($ids.ContainsKey([string]$property.Value)) `
        "Current target reference '$($property.Name)' points to unknown id '$($property.Value)'"
}

Write-Output ("Validated {0} test targets from {1}" -f `
    $registry.targets.Count, $registryFile)
