param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$TaskId,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputPath,
    [string]$WorkspaceRoot,
    [string]$HostRepoPath,
    [ValidateSet('windows', 'wsl')]
    [string]$HostRepoKind = 'windows',
    [switch]$RequireHostRepo,
    [string]$BasePath,
    [string]$ContentPath,
    [ValidateRange(1, 65535)]
    [int]$QmpPort,
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$PlannedChildName,
    [switch]$ExpectChildCreated,
    [string]$LockPath,
    [switch]$RequireVideo,
    [switch]$HashBase
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'acceptance-matrix-verifier.ps1')

# Records one named preflight check. A failed check carries the handoff
# protocol's stop-token class so the caller can classify without parsing prose.
$script:checks = [Collections.Generic.List[object]]::new()
function Add-PreflightCheck {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$FailureClass,
        [string]$Detail
    )
    $script:checks.Add([pscustomobject][ordered]@{
        name = $Name
        ok = $Ok
        failureClass = if ($Ok) { $null } else { $FailureClass }
        detail = $Detail
    })
}

# Tokenizes a profile command line the way the frontend does: a
# double-quoted run is one token, everything else splits on whitespace.
# Policy decisions must only ever look at tokens; regex over the raw text
# lets any quoted option value smuggle decoy options past the audit.
function Get-XpTestProfileTokens {
    param([string]$Text)

    $tokens = [Collections.Generic.List[string]]::new()
    foreach ($match in [regex]::Matches($Text, '"([^"]*)"|(\S+)')) {
        if ($match.Groups[1].Success) {
            $tokens.Add($match.Groups[1].Value)
        } else {
            $tokens.Add($match.Groups[2].Value)
        }
    }
    return @($tokens)
}

function ConvertTo-XpTestWslPath {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Expected a drive-letter Windows path, got '$Path'"
    }
    return "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2] -replace '\\', '/')"
}

# The observation window opens before any snapshot so a process started
# mid-preflight cannot escape the handback's new-process audit.
$observationStarted = Get-Date

$outputFile = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $outputFile
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw "Preflight output parent does not exist: $outputParent"
}
if (Test-Path -LiteralPath $outputFile) {
    throw "Refusing to overwrite preflight output: $outputFile"
}

$workspace = if ($WorkspaceRoot) {
    [IO.Path]::GetFullPath($WorkspaceRoot)
} else {
    (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$vmDir = Join-Path $workspace 'qemu\winxp'
$baseFull = if ($BasePath) {
    [IO.Path]::GetFullPath($BasePath)
} else {
    Join-Path $vmDir 'my_pc_disk.qcow2'
}
$lockFull = if ($LockPath) {
    [IO.Path]::GetFullPath($LockPath)
} else {
    Join-Path $vmDir '.xpdriver-acceptance-artifacts.lock'
}

# The paired host path is kind-sensitive: Windows paths normalize locally,
# WSL paths pass through verbatim because Windows path APIs would mangle them.
$hostRepoFull = $null
if ($HostRepoKind -ceq 'wsl') {
    if ($HostRepoPath) { $hostRepoFull = [string]$HostRepoPath }
} elseif ($HostRepoPath) {
    $hostRepoFull = [IO.Path]::GetFullPath($HostRepoPath)
} else {
    $hostRepoFull = [IO.Path]::GetFullPath(
        (Join-Path $workspace '..\qemu-libretro'))
}

Add-PreflightCheck 'workspace' `
    (Test-Path -LiteralPath $workspace -PathType Container) `
    'INFRASTRUCTURE_FAILURE' "workspace root not found: $workspace"

# Repository identities. The paired host is recorded when present; a handoff
# that exercises the VM must name the exact paired host explicitly.
$repositories = [Collections.Generic.List[object]]::new()
$xpdIdentity = Get-XpMatrixRepositoryIdentity -Repository (
    [pscustomobject]@{ name = 'xpdriver'; kind = 'windows'; path = $workspace })
$repositories.Add($xpdIdentity)
Add-PreflightCheck 'repository:xpdriver' ($xpdIdentity.exitCode -eq 0) `
    'INFRASTRUCTURE_FAILURE' `
    "git identity failed for $workspace (exit $($xpdIdentity.exitCode))"
$hostExists = $false
if ($hostRepoFull) {
    if ($HostRepoKind -ceq 'wsl') {
        & wsl.exe test -d $hostRepoFull 2>$null
        $hostExists = ($LASTEXITCODE -eq 0)
    } else {
        $hostExists = Test-Path -LiteralPath $hostRepoFull -PathType Container
    }
}
if ($hostExists) {
    $hostIdentity = Get-XpMatrixRepositoryIdentity -Repository (
        [pscustomobject]@{ name = 'qemu-libretro'; kind = $HostRepoKind;
            path = $hostRepoFull })
    $repositories.Add($hostIdentity)
    Add-PreflightCheck 'repository:qemu-libretro' `
        ($hostIdentity.exitCode -eq 0) 'INFRASTRUCTURE_FAILURE' `
        "git identity failed for $hostRepoFull (exit $($hostIdentity.exitCode))"
} else {
    $repositories.Add([pscustomobject][ordered]@{
        name = 'qemu-libretro'
        kind = $HostRepoKind
        path = $hostRepoFull
        exitCode = $null
        branch = $null
        commit = $null
        statusLines = @()
        statusSha256 = $null
        statusFiles = @()
        statusContentSha256 = $null
        exists = $false
    })
    $hostRequiredDetail = "paired host repository not found: $hostRepoFull; " +
        'a VM or package handoff must name the exact paired host path and kind'
    Add-PreflightCheck 'repository:qemu-libretro' (-not $RequireHostRepo) `
        'HANDOFF_INCOMPLETE' $hostRequiredDetail
}

# Running VM processes are evidence; only a process holding the assigned
# content profile or planned child is a conflict. Disposable runs of the
# shared base through -snapshot profiles are ordinary parallel work.
$processes = @(Get-XpTestVmProcesses)
$processConflict = $null
$contentLeaf = if ($ContentPath) { Split-Path -Leaf $ContentPath } else { $null }
foreach ($proc in $processes) {
    if ([string]::IsNullOrWhiteSpace($proc.commandLine)) { continue }
    if ($contentLeaf -and $proc.commandLine.IndexOf(
            $contentLeaf, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        $processConflict = "PID $($proc.pid) ($($proc.name)) references " +
            "the assigned content profile '$contentLeaf'"
        break
    }
    if ($PlannedChildName -and $proc.commandLine.IndexOf(
            $PlannedChildName, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        $processConflict = "PID $($proc.pid) ($($proc.name)) references " +
            "the planned child '$PlannedChildName'"
        break
    }
}
Add-PreflightCheck 'processes' ($null -eq $processConflict) `
    'VM_OWNERSHIP_CONFLICT' $processConflict

# Shared artifact lease. The byte-range lock is OS-held, so a held lease
# always names a live owner; never delete the file to defeat it.
$lease = Test-XpAcceptanceArtifactLease -LockPath $lockFull
Add-PreflightCheck 'artifactLease' ([bool]$lease.free) `
    'VM_OWNERSHIP_CONFLICT' "lease held by another run: $($lease.owner)"

# Base disk identity. The base is read-only evidence: QEMU opens it only as a
# read-only backing file, so its size and last-write time are a reliable
# mutation tripwire; -HashBase adds a content hash for the strongest check.
$baseInfo = [pscustomobject][ordered]@{
    path = $baseFull
    exists = $false
    bytes = $null
    lastWriteTimeUtc = $null
    sha256 = $null
}
if (Test-Path -LiteralPath $baseFull -PathType Leaf) {
    $baseItem = Get-Item -LiteralPath $baseFull
    $baseInfo.exists = $true
    $baseInfo.bytes = $baseItem.Length
    $baseInfo.lastWriteTimeUtc = $baseItem.LastWriteTimeUtc.ToString('o')
    if ($HashBase) {
        $baseInfo.sha256 = (Get-FileHash -LiteralPath $baseFull `
            -Algorithm SHA256).Hash
    }
}
Add-PreflightCheck 'baseImage' $baseInfo.exists 'INFRASTRUCTURE_FAILURE' `
    "base disk not found: $baseFull"

# The target registry defines the protected-disk set. The disk policy cannot
# fail open: without a parsed registry there is no protected set, so a
# missing or malformed registry stops the task as an infrastructure failure.
$protectedRoots = [Collections.Generic.List[string]]::new()
$protectedRoots.Add((Split-Path -Leaf $baseFull))
$protectedRootPaths = [Collections.Generic.List[string]]::new()
$protectedRootPaths.Add($baseFull)
$registeredTargets = [Collections.Generic.List[object]]::new()
$registryPath = Join-Path $workspace 'qemu\harness\test-targets.json'
$registryOk = Test-Path -LiteralPath $registryPath -PathType Leaf
$registryDetail = if ($registryOk) { $null } else {
    "target registry not found: $registryPath"
}
if ($registryOk) {
    try {
        $registry = Get-Content -Raw -LiteralPath $registryPath |
            ConvertFrom-Json
        if ($registry.schemaVersion -ne 1) {
            throw "schemaVersion is $($registry.schemaVersion), expected 1"
        }
        # Schema-valid JSON can still be semantic garbage - null targets,
        # duplicate ids, paths escaping the workspace - that silently empties
        # the protected set. The canonical validator is the authority; its
        # rejection is an infrastructure failure, not a weaker local reparse.
        [void](& (Join-Path $PSScriptRoot '..\..\tools\validate-test-targets.ps1') `
            -RegistryPath $registryPath)
        foreach ($target in @($registry.targets)) {
            $targetId = [string]$target.id
            if ($target.PSObject.Properties['backingDiskPath'] -and
                    $target.backingDiskPath) {
                $backingRelative = [string]$target.backingDiskPath
                $backingLeaf = Split-Path -Leaf (
                    $backingRelative -replace '/', '\')
                $backingFull = [IO.Path]::GetFullPath(
                    (Join-Path $workspace ($backingRelative -replace '/', '\')))
                if ($backingLeaf -and -not @($protectedRoots |
                        Where-Object { $_ -ieq $backingLeaf })) {
                    $protectedRoots.Add($backingLeaf)
                }
                if (-not @($protectedRootPaths | Where-Object {
                        $_ -ieq $backingFull })) {
                    $protectedRootPaths.Add($backingFull)
                }
            }
            if ($target.PSObject.Properties['diskPath'] -and $target.diskPath) {
                $diskRelative = [string]$target.diskPath
                $diskLeaf = Split-Path -Leaf ($diskRelative -replace '/', '\')
                $diskFull = [IO.Path]::GetFullPath(
                    (Join-Path $workspace ($diskRelative -replace '/', '\')))
                $contentFullRegistered = $null
                if ($target.PSObject.Properties['contentPath'] -and
                        $target.contentPath) {
                    $contentFullRegistered = [IO.Path]::GetFullPath(
                        (Join-Path $workspace (
                            [string]$target.contentPath -replace '/', '\')))
                }
                if ($diskLeaf) {
                    $registeredTargets.Add([pscustomobject][ordered]@{
                        id = $targetId
                        diskLeaf = $diskLeaf
                        diskPath = $diskFull
                        contentPath = $contentFullRegistered
                        mutationPolicy = [string]$target.mutationPolicy
                    })
                }
            }
        }
    } catch {
        $registryOk = $false
        $registryDetail = "target registry did not parse: $($_.Exception.Message)"
    }
}
Add-PreflightCheck 'targetRegistry' $registryOk 'INFRASTRUCTURE_FAILURE' `
    "$registryDetail; the protected-disk set cannot be established"

# Content profile: authoritative for QMP port, disk, and network policy.
$profileInfo = $null
$profilePort = $null
if ($ContentPath) {
    $contentFull = [IO.Path]::GetFullPath($ContentPath)
    $profileExists = Test-Path -LiteralPath $contentFull -PathType Leaf
    Add-PreflightCheck 'contentProfile' $profileExists `
        'INFRASTRUCTURE_FAILURE' "content profile not found: $contentFull"
    if ($profileExists) {
        $profileText = [IO.File]::ReadAllText($contentFull)
        # One token walk feeds every policy decision. Offline means exactly
        # -nic none everywhere: any other -nic value or any legacy
        # -net/-netdev option enables a network device. Every writable
        # block-device attachment outside the audited -hda fails closed:
        # -hdb/-hdc/-hdd, floppies, pflash, SD, mtdblock, -blockdev, host
        # shares (-fsdev/-virtfs), any -drive that is not a cdrom, and a
        # repeated -hda (only the first is audited).
        $profileTokens = @(Get-XpTestProfileTokens -Text $profileText)
        $snapshot = $false
        $legacyNet = $false
        $nicValues = @()
        $hdaValues = @()
        $vgaValues = @()
        $vgaVramValues = @()
        $extraDiskOptions = [Collections.Generic.List[string]]::new()
        $extraDiskNames = @('-hdb', '-hdc', '-hdd', '-fda', '-fdb',
            '-pflash', '-sd', '-mtdblock', '-blockdev', '-fsdev', '-virtfs')
        for ($tokenIndex = 0; $tokenIndex -lt $profileTokens.Count;
                ++$tokenIndex) {
            $token = $profileTokens[$tokenIndex]
            $value = if ($tokenIndex + 1 -lt $profileTokens.Count) {
                $profileTokens[$tokenIndex + 1]
            } else { '' }
            if ($token -ieq '-snapshot') {
                $snapshot = $true
            } elseif ($token -ieq '-hda') {
                $hdaValues += $value
            } elseif ($token -ieq '-nic') {
                $nicValues += @(($value -split ',')[0])
            } elseif ($token -ieq '-net' -or $token -ieq '-netdev') {
                $legacyNet = $true
            } elseif ($token -ieq '-vga') {
                $vgaValues += @(($value -split ',')[0])
            } elseif ($token -ieq '-global' -and
                    $value -imatch '^VGA\.vgamem_mb=(.*)$') {
                $vgaVramValues += $Matches[1]
            } elseif ($token -ieq '-qmp') {
                if ($value -match '(?i)^tcp:(?:127\.0\.0\.1|localhost):(\d+)') {
                    $profilePort = [int]$Matches[1]
                }
            } elseif ($extraDiskNames -contains $token) {
                $extraDiskOptions.Add($token)
            } elseif ($token -ieq '-drive') {
                # Exact comma-segment match: a filename merely containing
                # the text media=cdrom is still a writable disk.
                $driveSegments = @($value -split ',')
                if (-not @($driveSegments | Where-Object {
                        $_ -ieq 'media=cdrom' }).Count) {
                    $extraDiskOptions.Add("-drive $value")
                }
            }
        }
        if ($hdaValues.Count -gt 1) {
            $extraDiskOptions.Add(
                "-hda repeated $($hdaValues.Count) times; only the first is audited")
        }
        $nicNone = ($nicValues.Count -ge 1) -and
            (@($nicValues | Where-Object { $_ -ine 'none' }).Count -eq 0) -and
            (-not $legacyNet)
        $vgaVramMb = $null
        if ($vgaVramValues.Count -eq 1) {
            $parsedVramMb = 0
            if ([int]::TryParse(
                    [string]$vgaVramValues[0], [ref]$parsedVramMb)) {
                $vgaVramMb = $parsedVramMb
            }
        }
        $standardVga = ($vgaValues.Count -eq 0) -or
            (@($vgaValues | Where-Object { $_ -ieq 'std' }).Count -gt 0)
        $vgaVramOk = (-not $standardVga) -or
            (($vgaVramValues.Count -eq 1) -and ($vgaVramMb -eq 128))
        $vgaVramDetail = if ($standardVga) {
            "standard-VGA profiles must declare exactly one " +
                "-global VGA.vgamem_mb=128; found: " +
                "$($vgaVramValues -join ', ')"
        } else {
            "non-standard VGA profile '$($vgaValues -join ', ')' does not " +
                'use the standard-VGA backing aperture'
        }
        $hda = if ($hdaValues.Count -and $hdaValues[0]) {
            $hdaValues[0]
        } else { $null }
        $hdaLeaf = if ($hda) {
            Split-Path -Leaf ($hda -replace '/', '\')
        } else { $null }
        # Disk identity is compared by canonical path where resolvable and by
        # leaf name everywhere; a profile outside the disk directory cannot
        # dodge the policy with a relative spelling.
        $hdaCandidates = @()
        if ($hda) {
            $hdaCandidates = @(
                [IO.Path]::GetFullPath(
                    (Join-Path (Split-Path -Parent $contentFull) $hda)),
                [IO.Path]::GetFullPath(
                    (Join-Path $vmDir (Split-Path -Leaf ($hda -replace '/', '\'))))
            ) | Select-Object -Unique
        }
        $hdaProtectedRoot = ($null -ne $hdaLeaf) -and (
            (@($protectedRoots | Where-Object { $_ -ieq $hdaLeaf }).Count -gt 0) -or
            (@($hdaCandidates | Where-Object {
                $candidate = $_
                @($protectedRootPaths | Where-Object {
                    $_ -ieq $candidate }).Count -gt 0
            }).Count -gt 0))
        $registeredOwner = $null
        $registeredOwnerPolicy = $null
        $hdaRegisteredDisk = $false
        if ($hdaLeaf) {
            foreach ($registered in $registeredTargets) {
                $diskMatches = [string]::Equals($registered.diskLeaf, $hdaLeaf,
                    [StringComparison]::OrdinalIgnoreCase) -or
                    (@($hdaCandidates | Where-Object {
                        $_ -ieq $registered.diskPath }).Count -gt 0)
                if ($diskMatches) {
                    $hdaRegisteredDisk = $true
                    if ($registered.contentPath -and [string]::Equals(
                            $registered.contentPath, $contentFull,
                            [StringComparison]::OrdinalIgnoreCase)) {
                        $registeredOwner = $registered.id
                        $registeredOwnerPolicy = [string]$registered.mutationPolicy
                    }
                    break
                }
            }
        }
        $hdaProtected = $hdaProtectedRoot -or $hdaRegisteredDisk
        $profileInfo = [pscustomobject][ordered]@{
            path = $contentFull
            disposable = [bool]$snapshot
            networkOffline = [bool]$nicNone
            qmpPort = $profilePort
            vga = @($vgaValues)
            vgaVramMb = $vgaVramMb
            hda = $hda
            hdaLeaf = $hdaLeaf
            hdaProtected = [bool]$hdaProtected
            hdaRegisteredTarget = $registeredOwner
        }
        if (-not $hdaLeaf) {
            Add-PreflightCheck 'contentProfileDisk' $false `
                'DISK_POLICY_VIOLATION' `
                'content profile declares no -hda disk'
        } elseif ($registeredOwner -and -not $snapshot -and
                ($registeredOwnerPolicy -ieq 'preserve' -or
                 $registeredOwnerPolicy -ieq 'child-only')) {
            # Never write directly to a preserve/child-only target.
            # A writable boot mutates the qcow2
            # even without an install, so the registered profile is not an
            # exemption from its own target's mutation policy.
            $diskDetail = "registered target '$registeredOwner' has " +
                "mutationPolicy '$registeredOwnerPolicy' and its profile " +
                'does not carry -snapshot; boot a disposable -snapshot ' +
                'copy or create a fresh child overlay instead'
            Add-PreflightCheck 'contentProfileDisk' $false `
                'DISK_POLICY_VIOLATION' $diskDetail
        } elseif ($registeredOwner) {
            Add-PreflightCheck 'contentProfileDisk' $true $null `
                ("registered content profile for target '$registeredOwner' " +
                 "(disposable=$snapshot)")
        } elseif ($hdaProtected -and -not $snapshot) {
            $diskDetail = "content profile boots protected disk '$hdaLeaf' " +
                'without -snapshot and is not its registered target profile; ' +
                'create a fresh child overlay instead'
            Add-PreflightCheck 'contentProfileDisk' $false `
                'DISK_POLICY_VIOLATION' $diskDetail
        } else {
            Add-PreflightCheck 'contentProfileDisk' $true $null `
                "hda=$hdaLeaf disposable=$snapshot protected=$hdaProtected"
        }
        Add-PreflightCheck 'contentProfileNetwork' $nicNone `
            'NETWORK_POLICY_VIOLATION' `
            'content profile enables networking; only -nic none is permitted'
        Add-PreflightCheck 'contentProfileVram' $vgaVramOk `
            'HANDOFF_INCOMPLETE' $vgaVramDetail

        Add-PreflightCheck 'contentProfileExtraDisks' `
            ($extraDiskOptions.Count -eq 0) 'DISK_POLICY_VIOLATION' `
            ("content profile attaches block devices outside the audited " +
             "-hda: $($extraDiskOptions -join '; ')")

        # A planned child and its profile must be the same disk: the profile
        # must be the generated xpdriver-<name> file booting <name>.qcow2.
        if ($PlannedChildName) {
            $expectedContentLeaf = "xpdriver-$PlannedChildName.qemu_cmd_line"
            $expectedOverlayLeaf = "$PlannedChildName.qcow2"
            $bindingOk = ([string]::Equals((Split-Path -Leaf $contentFull),
                $expectedContentLeaf,
                [StringComparison]::OrdinalIgnoreCase)) -and
                ([string]::Equals($hdaLeaf, $expectedOverlayLeaf,
                    [StringComparison]::OrdinalIgnoreCase))
            $bindingDetail = "planned child '$PlannedChildName' expects " +
                "profile $expectedContentLeaf booting $expectedOverlayLeaf; " +
                "got $(Split-Path -Leaf $contentFull) booting $hdaLeaf"
            Add-PreflightCheck 'plannedChildBinding' $bindingOk `
                'HANDOFF_INCOMPLETE' $bindingDetail
        }
    }
}

# QMP port: the profile is authoritative, and the runner fails closed when an
# explicit value disagrees, so catch the disagreement before launch.
$effectivePort = $profilePort
if ($PSBoundParameters.ContainsKey('QmpPort')) {
    if ($profilePort -and $QmpPort -ne $profilePort) {
        $portDetail = "-QmpPort $QmpPort disagrees with the content profile " +
            "port $profilePort; the profile is authoritative"
        Add-PreflightCheck 'qmpPortAgreement' $false 'HANDOFF_INCOMPLETE' `
            $portDetail
    }
    if (-not $effectivePort) { $effectivePort = $QmpPort }
}
if ($effectivePort) {
    $portFree = Test-XpTestPortFree -Port $effectivePort
    Add-PreflightCheck 'qmpPortFree' $portFree 'VM_OWNERSHIP_CONFLICT' `
        "QMP port $effectivePort is already bound by another frontend"
}

# Planned fresh child, two stages: pre-creation BOTH the overlay and the
# content file must be absent, because new-xpdm-overlay.ps1 refuses when
# either exists and existing evidence is never deleted to free a name;
# post-creation the generated pair must exist.
$plannedChild = $null
if ($PlannedChildName) {
    $childOverlay = Join-Path $vmDir "$PlannedChildName.qcow2"
    $childContent = Join-Path $vmDir `
        "xpdriver-$PlannedChildName.qemu_cmd_line"
    $overlayExists = [bool](Test-Path -LiteralPath $childOverlay)
    $contentExists = [bool](Test-Path -LiteralPath $childContent)
    $plannedChild = [pscustomobject][ordered]@{
        name = $PlannedChildName
        overlayPath = $childOverlay
        contentPath = $childContent
        overlayExists = $overlayExists
        contentExists = $contentExists
        exists = ($overlayExists -and $contentExists)
        stage = if ($ExpectChildCreated) { 'post-creation' } `
            else { 'pre-creation' }
    }
    if ($ExpectChildCreated) {
        $childDetail = "planned child '$PlannedChildName' was not created; " +
            'run new-xpdm-overlay.ps1 before the post-creation preflight'
        Add-PreflightCheck 'plannedChild' `
            ($overlayExists -and $contentExists) `
            'INFRASTRUCTURE_FAILURE' $childDetail
        # Filename conventions prove nothing about the qcow2 chain: the
        # overlay's stored backing reference must resolve to the approved
        # base. Only an explicitly assigned -BasePath can be verified.
        if ($PSBoundParameters.ContainsKey('BasePath') -and $overlayExists) {
            $backingOk = $false
            $backingDetail = $null
            try {
                $infoLines = @(& wsl.exe qemu-img info --output=json (
                    ConvertTo-XpTestWslPath -Path $childOverlay) 2>$null)
                if ($LASTEXITCODE -ne 0) {
                    throw "qemu-img info failed with exit code $LASTEXITCODE"
                }
                $overlayInfo = ($infoLines -join "`n") | ConvertFrom-Json
                $backingRaw = [string]$overlayInfo.'backing-filename'
                if (-not $backingRaw) {
                    throw 'overlay stores no backing reference'
                }
                $backingResolved = $null
                $backingFull = [string]$overlayInfo.'full-backing-filename'
                if ($backingFull -match '^/mnt/([A-Za-z])/(.*)$') {
                    $backingResolved = [IO.Path]::GetFullPath(
                        "$($Matches[1].ToUpperInvariant()):\" +
                        ($Matches[2] -replace '/', '\'))
                } elseif ($backingRaw -notmatch '^([A-Za-z]:|[/\\])') {
                    $backingResolved = [IO.Path]::GetFullPath(
                        (Join-Path $vmDir ($backingRaw -replace '/', '\')))
                } else {
                    throw ("overlay backing '$backingRaw' is not the " +
                        'sibling-leaf form new-xpdm-overlay.ps1 writes')
                }
                $backingOk = [string]::Equals($backingResolved, $baseFull,
                    [StringComparison]::OrdinalIgnoreCase)
                $backingDetail = "overlay backing '$backingRaw' resolves " +
                    "to $backingResolved; approved base is $baseFull"
            } catch {
                $backingDetail = 'overlay backing chain could not be ' +
                    "verified: $($_.Exception.Message)"
            }
            $plannedChild | Add-Member -NotePropertyName backing `
                -NotePropertyValue $backingDetail
            Add-PreflightCheck 'plannedChildBacking' $backingOk `
                'HANDOFF_INCOMPLETE' $backingDetail
        }
    } else {
        $childDetail = "overlay or content file already exists for " +
            "'$PlannedChildName'; new-xpdm-overlay.ps1 refuses to reuse " +
            'either; choose a new unique child name and never delete ' +
            'existing evidence'
        Add-PreflightCheck 'plannedChild' `
            (-not $overlayExists -and -not $contentExists) `
            'INFRASTRUCTURE_FAILURE' $childDetail
    }
}

# Host prerequisites that sit outside the harness's own gates. Every VM
# launch rebuilds the acceptance ISO through WSL make and make_iso.py.
$pycdlibPath = Join-Path $workspace '.tools\pycdlib\pycdlib'
$pycdlibPresent = Test-Path -LiteralPath $pycdlibPath -PathType Container
Add-PreflightCheck 'prerequisitePycdlib' $pycdlibPresent `
    'INFRASTRUCTURE_FAILURE' `
    'pycdlib missing; run: python -m pip install --target .tools/pycdlib pycdlib==1.14.0'
$wslCommand = Get-Command wsl.exe -ErrorAction SilentlyContinue
Add-PreflightCheck 'prerequisiteWsl' ($null -ne $wslCommand) `
    'INFRASTRUCTURE_FAILURE' `
    'wsl.exe not found; every VM launch rebuilds the acceptance ISO through WSL make'
$ffmpegPath = $null
if ($RequireVideo) {
    $ffmpegExact = Join-Path $workspace `
        '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe'
    if (Test-Path -LiteralPath $ffmpegExact -PathType Leaf) {
        $ffmpegPath = $ffmpegExact
    } else {
        $toolsDir = Join-Path $workspace '.tools'
        $candidate = @(Get-ChildItem -LiteralPath $toolsDir -Directory `
            -Filter 'ffmpeg-*' -ErrorAction SilentlyContinue |
            ForEach-Object {
                Get-ChildItem -LiteralPath $_.FullName -Recurse `
                    -Filter 'ffmpeg.exe' -File -ErrorAction SilentlyContinue
            } | Select-Object -First 1)
        if ($candidate.Count) { $ffmpegPath = $candidate[0].FullName }
    }
    Add-PreflightCheck 'prerequisiteFfmpeg' ($null -ne $ffmpegPath) `
        'INFRASTRUCTURE_FAILURE' `
        'pinned encoder missing; run: qemu\harness\install-video-tools.ps1'
}

$failureCount = @($script:checks | Where-Object { -not $_.ok }).Count
$preflight = [pscustomobject][ordered]@{
    schema = 1
    kind = 'xpdriver-test-preflight'
    taskId = $TaskId
    observationStarted = $observationStarted.ToString('o')
    timestamp = Get-Date -Format o
    result = if ($failureCount) { 'fail' } else { 'pass' }
    checks = @($script:checks)
    environment = [pscustomobject][ordered]@{
        workspace = $workspace
        vmDir = $vmDir
        repositories = @($repositories)
        processes = $processes
        qmpPort = [pscustomobject][ordered]@{
            requested = if ($PSBoundParameters.ContainsKey('QmpPort')) {
                $QmpPort
            } else { $null }
            profile = $profilePort
            effective = $effectivePort
        }
        lease = $lease
        baseImage = $baseInfo
        protectedRoots = @($protectedRoots)
        registeredTargets = @($registeredTargets)
        contentProfile = $profileInfo
        plannedChild = $plannedChild
        prerequisites = [pscustomobject][ordered]@{
            pycdlib = [pscustomobject][ordered]@{
                path = $pycdlibPath
                present = $pycdlibPresent
            }
            wsl = [pscustomobject][ordered]@{
                present = ($null -ne $wslCommand)
            }
            ffmpeg = [pscustomobject][ordered]@{
                required = [bool]$RequireVideo
                path = $ffmpegPath
            }
        }
    }
}
Write-XpAcceptanceAtomicText -Path $outputFile `
    -Text (($preflight | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
Write-Output "TestPreflight taskId=$TaskId result=$($preflight.result) checks=$($script:checks.Count) failures=$failureCount output=$outputFile"
foreach ($failed in @($script:checks | Where-Object { -not $_.ok })) {
    $detail = [string]$failed.detail
    $detail = $detail.Replace("`r", ' ').Replace("`n", ' ')
    Write-Output "TestPreflightFailure name=$($failed.name) class=$($failed.failureClass) detail=$detail"
}
if ($failureCount) { exit 1 }
