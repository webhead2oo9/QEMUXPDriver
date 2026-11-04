param(
    [ValidateRange(1, 86400)]
    [int]$TimeoutSeconds = 150,
    [int]$QmpPort = 44573,
    [int]$ResultPort = 44574,
    [string]$RunId,
    [ValidateRange(5, 300)]
    [int]$ProgressIntervalSeconds = 30,
    [ValidateRange(15, 180)]
    [int]$GuestLaunchDelaySeconds = 40,
    [string]$PreparedAcceptanceIsoPath,
    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string]$PreparedAcceptanceIsoSha256,
    [string]$FrozenInputsPath,
    [string]$PackageDirectory,
    [string]$ArtifactDirectory,
    [string]$TaskId,
    [ValidatePattern('^T[0-3](?:-diagnostic)?$')]
    [string]$Tier = 'T2',
    [switch]$Diagnostic,
    [string]$ContentPath,
    [string]$RetroHostPath,
    [string]$CorePath,
    [switch]$Reclaim,
    [switch]$Idle,
    [switch]$Wgl,
    [switch]$Xpdm,
    [switch]$InstallOnly,
    [switch]$BootOnly,
    [switch]$Game,
    [switch]$GamePerformance,
    [switch]$D3d,
    [switch]$D3dFullscreen,
    [switch]$D3dVisual,
    [switch]$D3dCopyVisual,
    [switch]$D3dBenchmark,
    [switch]$D3dHardware,
    [switch]$D3d16,
    [switch]$D3dHwtlOnly,
    [switch]$D3dVs11,
    [switch]$D3dVsSuccession,
    [switch]$D3dVsMultiContext,
    [switch]$D3dVsBenchmark,
    [switch]$D3d9,
    [switch]$D3d9Multistream7,
    [switch]$D3d9SelfTest,
    [switch]$D3dClean,
    [switch]$D3dGame,
    [switch]$D3dGameplay,
    [switch]$D3dNormalLaunch,
    [switch]$D3dPerformance,
    [ValidateSet('UT2003', 'UT2003Gameplay', 'UT2003GameplaySWVP',
                 'Warcraft3', 'Warcraft3D3D8', 'NFSU2')]
    [string]$ObserveGame,
    [switch]$ObservePerformance,
    [switch]$RecordVideo,
    [switch]$RequireLegacyIndexedDp2
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'serial-tail.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'host-process-profile.ps1')
. (Join-Path $PSScriptRoot 'doorbell-profile.ps1')
. (Join-Path $PSScriptRoot 'maxpayne-visual.ps1')
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
. (Join-Path $PSScriptRoot 'd3d-driver-profile.ps1')
. (Join-Path $PSScriptRoot 'd3d9-multistream-order.ps1')
. (Join-Path $PSScriptRoot 'd3d9-oracle.ps1')
. (Join-Path $PSScriptRoot 'warcraft3-d3d8-oracle.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')
. (Join-Path $PSScriptRoot 'acceptance-iso.ps1')

function Publish-XpAcceptanceCurrentRunArtifacts {
    param(
        [psobject]$Context,
        [string]$Status,
        [int]$ExitCode,
        [string]$FailureType,
        [string]$FailureMessage,
        [Nullable[bool]]$CleanupHostStopped,
        [object[]]$CaptureImages = @()
    )

    if (-not $Context) { return }
    $copySpecs = @(
        @('stdout', 'host.stdout.log'),
        @('stderr', 'host.stderr.log'),
        @('driverLog', 'driver.log'),
        @('debugconSource', 'guest-debug.log'),
        @('serialResultPath', 'guest-result.log'),
        @('stageLog', 'guest-stages.log'),
        @('d3dCrashImage', 'd3d-crash.ppm')
    )
    foreach ($copySpec in $copySpecs) {
        $sourceVariable = Get-Variable -Name $copySpec[0] -Scope 1 `
            -ErrorAction SilentlyContinue
        if ($sourceVariable) {
            [void](Copy-XpAcceptanceRunArtifact -Context $Context `
                -Source ([string]$sourceVariable.Value) `
                -Name $copySpec[1] -Optional)
        }
    }
    foreach ($capturePath in @($CaptureImages)) {
        if ($capturePath) {
            [void](Copy-XpAcceptanceRunArtifact -Context $Context `
                -Source ([string]$capturePath) `
                -Name ([IO.Path]::GetFileName([string]$capturePath)) `
                -Optional)
        }
    }
    if ($packageIdentity) {
        Write-XpAcceptanceAtomicText `
            -Path (Join-Path $Context.Directory 'package-identity.json') `
            -Text (($packageIdentity | ConvertTo-Json -Depth 8) +
                [Environment]::NewLine)
    }
    if ($d3dDriverProfile -and $d3dDriverProfile.status -ne 'absent') {
        Write-XpAcceptanceAtomicText `
            -Path (Join-Path $Context.Directory 'd3d-driver-profile.json') `
            -Text (($d3dDriverProfile | ConvertTo-Json -Depth 12) +
                [Environment]::NewLine)
    }
    $publishedHostProcessProfile =
        Complete-XpHostProcessCpuProfileState `
            -State $hostProcessProfileState
    if ($publishedHostProcessProfile) {
        Write-XpAcceptanceAtomicText `
            -Path (Join-Path $Context.Directory `
                'host-process-profile.json') `
            -Text (($publishedHostProcessProfile |
                    ConvertTo-Json -Depth 12) + [Environment]::NewLine)
    }

    $branchLines = @(& git -C $workspace branch --show-current 2>$null)
    $commitLines = @(& git -C $workspace rev-parse HEAD 2>$null)
    $statusLines = @(& git -C $workspace status --short 2>$null)
    $sourceBranch = if ($branchLines.Count) {
        ([string]$branchLines[0]).Trim()
    } else { $null }
    $sourceCommit = if ($commitLines.Count) {
        ([string]$commitLines[0]).Trim()
    } else { $null }
    $dirtyScope = @($statusLines | ForEach-Object {
        $line = [string]$_
        if ($line.Length -gt 3) { $line.Substring(3) } else { $line }
    })
    $infText = Get-Content -Raw -LiteralPath `
        (Join-Path $workspace 'guest\xpdm\qxp.inf')
    $versionMatch = [regex]::Match(
        $infText, '(?m)^DriverVer\s*=\s*[^,]+,([^\r\n]+)')
    $sourceVersion = if ($versionMatch.Success) {
        $versionMatch.Groups[1].Value.Trim()
    } else { $null }
    $transportText = Get-Content -Raw -LiteralPath `
        (Join-Path $workspace 'guest\xpdm\qxp_d3d_transport.h')
    $transportMatch = [regex]::Match(
        $transportText,
        '(?m)^#define\s+QXP_D3D_TRANSPORT_VERSION\s+(\d+)')
    $transportVersion = if ($transportMatch.Success) {
        [int]$transportMatch.Groups[1].Value
    } else { $null }
    $coreHash = if ($coreDll -and
                    (Test-Path -LiteralPath $coreDll -PathType Leaf)) {
        (Get-FileHash -LiteralPath $coreDll -Algorithm SHA256).Hash
    } else { $null }
    $hostHash = if ($hostExe -and
                    (Test-Path -LiteralPath $hostExe -PathType Leaf)) {
        (Get-FileHash -LiteralPath $hostExe -Algorithm SHA256).Hash
    } else { $null }
    $contentHash = if ($content -and
                       (Test-Path -LiteralPath $content -PathType Leaf)) {
        (Get-FileHash -LiteralPath $content -Algorithm SHA256).Hash
    } else { $null }
    $publishedVisualEvidence = $null
    if ($maxPayneVisualSelection) {
        # The sidecar is embedded in this result, so source, package, host,
        # content, and launcher identities remain single-sourced by the
        # existing harness result rather than duplicated in screenshot JSON.
        $publishedVisualEvidence = New-XpMaxPayneVisualEvidence `
            -Selection $maxPayneVisualSelection -Context $Context `
            -Mode $variant
        Write-XpAcceptanceAtomicText `
            -Path (Join-Path $Context.Directory `
                'maxpayne-visual-sequence.json') `
            -Text (($publishedVisualEvidence |
                    ConvertTo-Json -Depth 20) + [Environment]::NewLine)
    }
    $artifactRecords = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $Context)
    $durationSeconds = [Math]::Round(
        ((Get-Date) - $harnessStartedAt).TotalSeconds, 3)
    $resultDocument = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-run'
        taskId = $Context.TaskId
        runId = $Context.RunId
        timestamp = Get-Date -Format o
        tier = $Context.Tier
        mode = $variant
        diagnostic = [bool]$Diagnostic
        capture = -not [string]::IsNullOrWhiteSpace($captureMode) -and
            $captureMode -ne 'none' -and $captureMode -ne 'capture-free'
        captureMode = $captureMode
        result = if ($Status -eq 'passed') { 'pass' } else { 'fail' }
        harnessStatus = $Status
        exitCode = $ExitCode
        durationSeconds = $durationSeconds
        xpdriver = [pscustomobject][ordered]@{
            branch = $sourceBranch
            commit = $sourceCommit
            dirtyScope = $dirtyScope
            version = $sourceVersion
            transportAbi = $transportVersion
        }
        host = [pscustomobject][ordered]@{
            executable = $hostExe
            executableSha256 = $hostHash
            core = $coreDll
            coreSha256 = $coreHash
            processProfile = $publishedHostProcessProfile
        }
        package = $packageIdentity
        install = if ($InstallOnly) {
            [pscustomobject][ordered]@{
                guestReported = -not [string]::IsNullOrWhiteSpace(
                    $installDriverVer)
                driverVer = $installDriverVer
                signerThumbprint = $installSignerThumbprint
                conflictingReports = [bool]$installIdentityConflict
                matchesStagedPackage = [bool]$installIdentityOk
            }
        } else { $null }
        d3dDriverProfile = $d3dDriverProfile
        visualEvidence = $publishedVisualEvidence
        vm = [pscustomobject][ordered]@{
            content = $content
            contentSha256 = $contentHash
            qmpPort = $QmpPort
            acceptanceIso = $acceptanceIsoPath
            acceptanceIsoSha256 = $acceptanceIsoSha256
            preparedAcceptanceIso =
                -not [string]::IsNullOrWhiteSpace(
                    $PreparedAcceptanceIsoSha256)
        }
        timing = [pscustomobject][ordered]@{
            acceptanceIsoSeconds = [Math]::Round(
                ($acceptanceIsoReadyAt -
                 $acceptanceIsoPreparationStartedAt).TotalSeconds, 3)
            bootToFirstLaunchSeconds = if (
                $firstGuestLaunchAt -gt [DateTime]::MinValue) {
                [Math]::Round(
                    ($firstGuestLaunchAt - $runStartedAt).TotalSeconds, 3)
            } else { $null }
            firstLaunchToResultSeconds = if (
                $firstGuestLaunchAt -gt [DateTime]::MinValue -and
                $resultObservedAt -gt [DateTime]::MinValue) {
                [Math]::Round(
                    ($resultObservedAt -
                     $firstGuestLaunchAt).TotalSeconds, 3)
            } else { $null }
            shutdownSeconds = if (
                $shutdownStartedAt -gt [DateTime]::MinValue -and
                $shutdownCompletedAt -gt [DateTime]::MinValue) {
                [Math]::Round(
                    ($shutdownCompletedAt -
                     $shutdownStartedAt).TotalSeconds, 3)
            } else { $null }
        }
        timeout = [pscustomobject][ordered]@{
            requestedSeconds = $TimeoutSeconds
            effectiveSeconds = $effectiveTimeout
            recommendedOuterSeconds =
                $timeoutBudget.RecommendedOuterSeconds
            progressIntervalSeconds = $ProgressIntervalSeconds
            progressRecords = $progressRecordCount
            guestLaunchDelaySeconds = $GuestLaunchDelaySeconds
        }
        guestResult = $result
        lastGuestMessage = $lastGuestMessage
        guestTestStarted = [bool]$guestTestStarted
        guestCompleted = -not [string]::IsNullOrWhiteSpace($result)
        qmpStatus = $qmpStatus
        teardownOk = if ($null -eq $teardown) {
            $null
        } else {
            [bool]$teardown
        }
        lease = [pscustomobject][ordered]@{
            path = $artifactLeasePath
            status = $Status
            cleanupHostStopped = $CleanupHostStopped
        }
        failure = if ($FailureType -or $FailureMessage) {
            [pscustomobject][ordered]@{
                type = $FailureType
                message = $FailureMessage
            }
        } else { $null }
        artifacts = @($artifactRecords | ForEach-Object { $_.name })
    }
    [void](Write-XpAcceptanceRunResult -Context $Context `
        -Result $resultDocument)
    [void](Write-XpAcceptanceRunManifest -Context $Context)
    Write-Output "ArtifactBundle runId=$($Context.RunId) directory=$($Context.Directory) result=harness-result.json manifest=artifact-manifest.json"
}
$D3dProbe = $D3d -or $D3dFullscreen -or $D3dVisual -or $D3dCopyVisual -or
            $D3dBenchmark
$D3dVsMode = $D3dVs11 -or $D3dVsSuccession -or $D3dVsMultiContext
$D3dAnyProbe = $D3dProbe -or $D3dVsMode -or $D3dVsBenchmark -or $D3d9 -or
               $D3d9SelfTest
$GameObserve = -not [string]::IsNullOrEmpty($ObserveGame)
$UT2003VisualOracle = $ObserveGame -eq 'UT2003' -and $RecordVideo
$NFSU2VisualOracle = $ObserveGame -eq 'NFSU2' -and $RecordVideo
$Warcraft3D3D8Oracle = $ObserveGame -eq 'Warcraft3D3D8' -and
    -not $ObservePerformance
if (($InstallOnly -or $BootOnly) -and
    (-not $Xpdm -or -not $ContentPath)) {
    throw '-InstallOnly/-BootOnly require -Xpdm and a persistent -ContentPath'
}
if ($InstallOnly -and $BootOnly) {
    throw '-InstallOnly and -BootOnly are mutually exclusive'
}
if ($Game -and (-not $Xpdm -or -not $BootOnly -or -not $ContentPath)) {
    throw '-Game requires -Xpdm, -BootOnly, and a persistent -ContentPath'
}
if ($GamePerformance -and -not $Game) {
    throw '-GamePerformance is a capture-free modifier for -Game'
}
if ($GamePerformance -and $RecordVideo) {
    throw '-GamePerformance is incompatible with -RecordVideo'
}
if (($D3dAnyProbe -or $D3dClean -or $D3dGame) -and
    (-not $Xpdm -or -not $BootOnly -or -not $ContentPath)) {
    throw '-D3d/-D3dFullscreen/-D3dVisual/-D3dCopyVisual/-D3dBenchmark/-D3dVs11/-D3dVsSuccession/-D3dVsMultiContext/-D3dVsBenchmark/-D3d9/-D3d9SelfTest/-D3dClean/-D3dGame require -Xpdm, -BootOnly, and a persistent -ContentPath'
}
if ($GameObserve -and (-not $Xpdm -or -not $BootOnly -or -not $ContentPath)) {
    throw '-ObserveGame requires -Xpdm, -BootOnly, and a persistent -ContentPath'
}
if ($ObservePerformance -and -not $GameObserve) {
    throw '-ObservePerformance is a capture-free modifier for -ObserveGame'
}
if ($ObservePerformance -and $RecordVideo) {
    throw '-ObservePerformance is incompatible with -RecordVideo'
}
if ($Warcraft3D3D8Oracle -and -not $RecordVideo) {
    throw 'Warcraft3D3D8 requires -RecordVideo to retain temporal evidence'
}
if ($RecordVideo -and -not ($GameObserve -or $Game)) {
    throw '-RecordVideo currently requires -ObserveGame or -Game'
}
if ($RequireLegacyIndexedDp2 -and -not $D3dProbe) {
    throw '-RequireLegacyIndexedDp2 requires a Direct3D probe mode'
}
if ($D3d16 -and -not $D3dVisual) {
    throw '-D3d16 is a modifier for -D3dVisual'
}
if ($D3dHwtlOnly -and -not $D3dVisual) {
    throw '-D3dHwtlOnly is a focused modifier for -D3dVisual'
}
if ($D3d9Multistream7 -and -not $D3d9) {
    throw '-D3d9Multistream7 is a focused modifier for -D3d9'
}
if ($D3dHardware -and -not ($D3dVisual -or $D3dBenchmark)) {
    throw '-D3dHardware is a modifier for -D3dVisual or -D3dBenchmark'
}
if ($D3dPerformance -and -not $D3dGame) {
    throw '-D3dPerformance is a capture-free modifier for -D3dGame'
}
if ($D3dGameplay -and -not $D3dGame) {
    throw '-D3dGameplay is a deterministic tutorial-scene modifier for -D3dGame'
}
if ($D3dNormalLaunch -and -not $D3dGame) {
    throw '-D3dNormalLaunch is a no-skipstartup modifier for -D3dGame'
}
if ($D3dNormalLaunch -and ($D3dGameplay -or $D3dPerformance)) {
    throw '-D3dNormalLaunch is incompatible with -D3dGameplay and -D3dPerformance'
}
$exclusiveModeCount = @(@(
    $Game, $D3d, $D3dFullscreen, $D3dVisual, $D3dCopyVisual, $D3dBenchmark,
    $D3dVs11, $D3dVsSuccession, $D3dVsMultiContext, $D3dVsBenchmark,
    $D3d9, $D3dClean,
    $D3dGame, $GameObserve) | Where-Object { $_ }).Count
if ($exclusiveModeCount -gt 1) {
    throw '-Game, Direct3D modes, and -ObserveGame are mutually exclusive'
}
$timeoutBudget = Get-XpAcceptanceTimeoutBudget `
    -RequestedSeconds $TimeoutSeconds -Xpdm ([bool]$Xpdm) `
    -LongRunning ([bool]($D3dAnyProbe -or $D3dGame -or
        $GamePerformance -or ($GameObserve -and $ObservePerformance))) `
    -CaptureGameObserve ([bool]($GameObserve -and -not $ObservePerformance)) `
    -RecordVideo ([bool]$RecordVideo) `
    -UT2003VisualOracle ([bool]$UT2003VisualOracle)
$effectiveTimeout = $timeoutBudget.EffectiveSeconds
if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = New-XpAcceptanceRunId
} elseif (-not (Test-XpAcceptanceRunId -RunId $RunId)) {
    throw "Invalid acceptance run ID '$RunId'; use 1-64 letters, digits, dots, underscores, or hyphens"
}
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vmDir = Join-Path $workspace 'qemu\winxp'
$artifactLeasePath = Join-Path $vmDir `
    '.xpdriver-acceptance-artifacts.lock'
$artifactLease = Enter-XpAcceptanceArtifactLease `
    -LockPath $artifactLeasePath -RunId $RunId -QmpPort $QmpPort `
    -RequestedTimeoutSeconds $TimeoutSeconds `
    -EffectiveTimeoutSeconds $effectiveTimeout
$harnessStartedAt = Get-Date
$acceptanceIsoPreparationStartedAt = $harnessStartedAt
$acceptanceIsoReadyAt = $harnessStartedAt
$acceptanceIsoPath = $null
$acceptanceIsoSha256 = $null
$firstGuestLaunchAt = [DateTime]::MinValue
$gameProfileTextStart = -1
$resultObservedAt = [DateTime]::MinValue
$shutdownStartedAt = [DateTime]::MinValue
$shutdownCompletedAt = [DateTime]::MinValue
$harnessExitCode = 0
$harnessTerminatingError = $null
$process = $null
$lastGuestMessage = '<none>'
$guestTestStarted = $false
$artifactContext = $null
$artifactPublished = $false
$progressRecordCount = 0
$udp = $null
$serialStream = $null
$frameProfileStartEvent = $null
$frameProfileStopEvent = $null
$hostProcessProfileState = New-XpHostProcessCpuProfileState
$packageIdentity = $null
$installDriverVer = $null
$installSignerThumbprint = $null
$installIdentityConflict = $false
$installIdentityOk = -not $InstallOnly
$d3dDriverProfile = $null
$d3dDriverProfileOk = $true
$captureImages = @()
# A canonical video run is not allowed to pass until exactly one digest-named
# review MP4 has been copied into its artifact directory. Standalone runs with
# no artifact context retain their legacy behavior.
$reviewVideoArtifactOk = -not ($RecordVideo -and $ArtifactDirectory)
$gameObserveArtifactSetRequired = $GameObserve -and $RecordVideo -and
    -not $ObservePerformance -and -not $Warcraft3D3D8Oracle -and
    -not [string]::IsNullOrWhiteSpace($ArtifactDirectory)
$gameObserveSourceSetOk = -not $gameObserveArtifactSetRequired
$gameObserveArtifactSetOk = -not $gameObserveArtifactSetRequired
$warcraft3D3D8CountersOk = -not $Warcraft3D3D8Oracle

function Start-XpPerformanceProfileWindow {
    Start-XpHostProcessCpuProfileWindow `
        -State $hostProcessProfileState -Process $process `
        -StartEvent $frameProfileStartEvent
}

function Stop-XpPerformanceProfileWindow {
    Stop-XpHostProcessCpuProfileWindow `
        -State $hostProcessProfileState -Process $process `
        -StopEvent $frameProfileStopEvent
}

try {
$artifactContext = if ($ArtifactDirectory) {
    Initialize-XpAcceptanceRunArtifacts -Directory $ArtifactDirectory `
        -RunId $RunId -TaskId $TaskId -Tier $Tier `
        -CommandLine ([Environment]::CommandLine)
} else { $null }
# Standalone XPDM runs consume the frozen input manifest produced outside
# any validation window (publish-xp-acceptance-inputs.ps1); a cycle passes the
# same manifest plus one private ISO copy. Resolve omitted host/core paths from
# that manifest so the defaults cannot drift to another build tree.
if ([string]::IsNullOrWhiteSpace($PreparedAcceptanceIsoPath) -ne
    [string]::IsNullOrWhiteSpace($PreparedAcceptanceIsoSha256)) {
    throw '-PreparedAcceptanceIsoPath and -PreparedAcceptanceIsoSha256 must be supplied together'
}
$frozenInputs = $null
$frozenExecutionInputs = $null
if ($Xpdm -and
    [string]::IsNullOrWhiteSpace($FrozenInputsPath) -and
    [string]::IsNullOrWhiteSpace($PreparedAcceptanceIsoPath)) {
    $FrozenInputsPath = Join-Path $workspace `
        'guest\acceptance\frozen-inputs-ut-oracle-r5.json'
}
if ($Xpdm -and
    -not [string]::IsNullOrWhiteSpace($FrozenInputsPath)) {
    $frozenInputs = Get-XpFrozenInputs -Path $FrozenInputsPath
}
$hostExe = if ($RetroHostPath) {
    (Resolve-Path -LiteralPath $RetroHostPath).Path
} elseif ($frozenInputs) {
    (Resolve-Path -LiteralPath (
        [string]$frozenInputs.retroHost.path)).Path
} elseif ($env:XPD_RETRO_HOST) {
    (Resolve-Path -LiteralPath $env:XPD_RETRO_HOST).Path
} else {
    throw 'Pass -RetroHostPath or set XPD_RETRO_HOST to retro_host.exe'
}
$coreDll = if ($CorePath) {
    (Resolve-Path -LiteralPath $CorePath).Path
} elseif ($frozenInputs) {
    (Resolve-Path -LiteralPath ([string]$frozenInputs.core.path)).Path
} else {
    (Resolve-Path -LiteralPath (
        Get-XpDefaultCorePath -Workspace $workspace)).Path
}
# Only the legacy non-XPDM WGL flow still rebuilds its own acceptance ISO.
# XPDM defaults to the staging package, while a frozen diagnostic generation
# may select another immutable package directory.
$stagedPackageDirectory = if ($PackageDirectory) {
    [IO.Path]::GetFullPath($PackageDirectory)
} else {
    Join-Path $workspace 'guest\xpdm\build\package'
}
if ($Xpdm -and $InstallOnly) {
    $packageIdentity = Get-XpAcceptancePackageIdentity `
        -Directory $stagedPackageDirectory
    Write-Output "PackageIdentity valid=$($packageIdentity.valid) driverVer=$($packageIdentity.driverVer) signerThumbprint=$($packageIdentity.certificate.thumbprint) files=$($packageIdentity.files.Count)"
    if (-not $packageIdentity.valid) {
        throw "Selected XPDM package identity is invalid: $($packageIdentity.findings -join '; ')"
    }
}
$acceptanceIsoPreparationStartedAt = Get-Date
if ($PreparedAcceptanceIsoPath) {
    $acceptanceIsoPath = [IO.Path]::GetFullPath($PreparedAcceptanceIsoPath)
    $preparedIsoIdentity = Assert-XpPreparedAcceptanceIso `
        -Path $acceptanceIsoPath `
        -ExpectedSha256 $PreparedAcceptanceIsoSha256
    $acceptanceIsoSha256 = $preparedIsoIdentity.sha256
    Write-Output "AcceptanceIso prepared=True sha256=$acceptanceIsoSha256 path=$acceptanceIsoPath"
} elseif ($Xpdm) {
    if ([string]::IsNullOrWhiteSpace($FrozenInputsPath)) {
        $FrozenInputsPath = Join-Path $workspace `
            'guest\acceptance\frozen-inputs-ut-oracle-r5.json'
    }
    if (-not $frozenInputs) {
        $frozenInputs = Get-XpFrozenInputs -Path $FrozenInputsPath
    }
    $acceptanceIsoPath = [IO.Path]::GetFullPath(
        [string]$frozenInputs.acceptanceIso.path)
    $PreparedAcceptanceIsoSha256 = [string]$frozenInputs.acceptanceIso.sha256
    $preparedIsoIdentity = Assert-XpPreparedAcceptanceIso `
        -Path $acceptanceIsoPath `
        -ExpectedSha256 $PreparedAcceptanceIsoSha256
    $acceptanceIsoSha256 = $preparedIsoIdentity.sha256
    # Treat the frozen ISO exactly like an explicitly prepared one: the
    # content profile must mount these same validated guest bytes.
    $PreparedAcceptanceIsoPath = $acceptanceIsoPath
    Write-Output "AcceptanceIso prepared=True frozen=True sha256=$acceptanceIsoSha256 path=$acceptanceIsoPath"
} else {
    $acceptanceIsoPath = Join-Path $workspace `
        'guest\acceptance\xpdriver-acceptance.iso'
    Push-Location $workspace
    try {
        & wsl.exe make -C guest/acceptance iso
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Acceptance ISO rebuild failed with exit code $LASTEXITCODE"
    }
    $acceptanceIsoSha256 = (Get-FileHash -LiteralPath $acceptanceIsoPath `
        -Algorithm SHA256).Hash
    Write-Output "AcceptanceIso prepared=False sha256=$acceptanceIsoSha256"
}
if ($frozenInputs) {
    if ($acceptanceIsoSha256 -cne
            [string]$frozenInputs.acceptanceIso.sha256) {
        throw 'selected acceptance ISO differs from frozen inputs'
    }
    if ($InstallOnly -and
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath($stagedPackageDirectory),
            [IO.Path]::GetFullPath(
                [string]$frozenInputs.package.directory))) {
        throw 'selected package directory differs from frozen inputs'
    }
    $frozenExecutionInputs = Assert-XpFrozenExecutionInputs `
        -Manifest $frozenInputs -RetroHostPath $hostExe `
        -CorePath $coreDll
    Write-Output "FrozenExecutionInputs valid=True host=$($frozenExecutionInputs.retroHost.sha256) core=$($frozenExecutionInputs.core.sha256)"
}
$acceptanceIsoReadyAt = Get-Date
if ($Xpdm -and $InstallOnly) {
    $packageRecheck = Get-XpAcceptancePackageIdentity `
        -Directory $stagedPackageDirectory
    $packageBefore = $packageIdentity.files | ConvertTo-Json -Depth 4
    $packageAfter = $packageRecheck.files | ConvertTo-Json -Depth 4
    if ($packageBefore -cne $packageAfter) {
        throw 'Selected XPDM package changed during acceptance ISO preparation; the recorded identity would not match the installed bytes'
    }
}
$retroSystemDirectory = [Environment]::GetEnvironmentVariable(
    'RETRO_HOST_SYSTEM_DIR', 'Process')
$contentName = if ($Xpdm) { 'xpdriver-xpdm.qemu_cmd_line' } else { 'xpdriver-accept.qemu_cmd_line' }
$content = if ($ContentPath) {
    (Resolve-Path $ContentPath).Path
} else {
    Join-Path $PSScriptRoot $contentName
}
$contentDir = Split-Path -Parent $content
$contentCommandLine = Get-Content -LiteralPath $content -Raw
if ($PreparedAcceptanceIsoPath) {
    # Parse the profile the way the frontend does (acceptance-iso.ps1): a
    # double-quoted run is one token, everything else splits on whitespace.
    # Only the single actual CD-ROM argument may name the validated ISO; a
    # mention inside any other token, a missing CD-ROM drive, or a second
    # CD-ROM attachment fails closed.
    $contentTokens = @(Get-XpContentProfileTokenSpans `
        -Text $contentCommandLine)
    $mountedIsoArguments = [Collections.Generic.List[object]]::new()
    for ($tokenIndex = 0; $tokenIndex -lt $contentTokens.Count; ++$tokenIndex) {
        $optionValue = [string]$contentTokens[$tokenIndex].value
        if ($optionValue -ieq '-cdrom') {
            $mountedIsoArguments.Add(
                $(if ($tokenIndex + 1 -lt $contentTokens.Count) {
                    [string]$contentTokens[$tokenIndex + 1].value
                } else { $null }))
            continue
        }
        if ($optionValue -ine '-drive' -or
                $tokenIndex + 1 -ge $contentTokens.Count) { continue }
        $driveSegments = @(
            ([string]$contentTokens[$tokenIndex + 1].value) -split ',')
        if (-not @($driveSegments | Where-Object {
                $_ -ieq 'media=cdrom' }).Count) { continue }
        $fileSegments = @($driveSegments | Where-Object {
            $_.StartsWith('file=', [StringComparison]::OrdinalIgnoreCase) })
        $mountedIsoArguments.Add(
            $(if ($fileSegments.Count -eq 1) {
                $fileSegments[0].Substring('file='.Length)
            } else { $null }))
    }
    if ($mountedIsoArguments.Count -ne 1) {
        throw ("Prepared content must mount the validated acceptance ISO " +
            "through exactly one CD-ROM argument, found " +
            "$($mountedIsoArguments.Count): $content")
    }
    $mountedIsoArgument = [string]$mountedIsoArguments[0]
    if ([string]::IsNullOrWhiteSpace($mountedIsoArgument)) {
        throw ("Prepared content CD-ROM argument does not name an ISO " +
            "file: $content")
    }
    $mountedIsoWindows = $mountedIsoArgument.Replace('/', '\')
    if (-not [IO.Path]::IsPathRooted($mountedIsoWindows)) {
        $mountedIsoWindows = Join-Path $contentDir $mountedIsoWindows
    }
    $mountedIsoFull = [IO.Path]::GetFullPath($mountedIsoWindows)
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
            $mountedIsoFull, $acceptanceIsoPath)) {
        throw "Prepared content does not mount the validated acceptance ISO: $acceptanceIsoPath"
    }
}
if ($contentCommandLine -match
    '(?i)-qmp\s+tcp:(?:127\.0\.0\.1|localhost):(\d+)(?:[,\s]|$)') {
    $contentQmpPort = [int]$Matches[1]
    if ($PSBoundParameters.ContainsKey('QmpPort') -and
        $QmpPort -ne $contentQmpPort) {
        throw "-QmpPort $QmpPort does not match the content QMP port $contentQmpPort"
    }
    $QmpPort = $contentQmpPort
}
# Reject an orphaned or concurrent frontend before clearing artifacts and
# scoring frames that belong to a different VM. The QMP listener is the
# authoritative per-config ownership point and closes the common case where a
# terminated harness leaves its retro_host child alive.
$qmpPortProbe = [Net.Sockets.TcpListener]::new(
    [Net.IPAddress]::Loopback, $QmpPort)
try {
    $qmpPortProbe.Start()
} catch {
    throw "QMP port $QmpPort is already in use; another acceptance VM may still be running"
} finally {
    $qmpPortProbe.Stop()
}
$variant = if ($Reclaim) { '-reclaim' } elseif ($Idle) { '-idle' } elseif ($Wgl) { '-wgl' } elseif ($InstallOnly) { '-xpdm-install' } elseif ($Game -and $GamePerformance) { '-xpdm-game-performance' } elseif ($Game) { '-xpdm-game' } elseif ($D3dGame -and $D3dGameplay -and $D3dPerformance) { '-xpdm-d3d-gameplay-performance' } elseif ($D3dGame -and $D3dGameplay) { '-xpdm-d3d-gameplay' } elseif ($D3dGame -and $D3dPerformance) { '-xpdm-d3d-performance' } elseif ($D3dGame -and $D3dNormalLaunch) { '-xpdm-d3d-game-normal' } elseif ($D3dGame) { '-xpdm-d3d-game' } elseif ($GameObserve -and $ObservePerformance) { "-xpdm-$($ObserveGame.ToLowerInvariant())-performance" } elseif ($GameObserve) { "-xpdm-$($ObserveGame.ToLowerInvariant())" } elseif ($D3dClean) { '-xpdm-d3d-clean' } elseif ($D3dVsBenchmark) { '-xpdm-d3d-vs-benchmark' } elseif ($D3dVsMultiContext) { '-xpdm-d3d-vs-multictx' } elseif ($D3dVsSuccession) { '-xpdm-d3d-vs-succession' } elseif ($D3dVs11) { '-xpdm-d3d-vs11' } elseif ($D3d9SelfTest) { '-xpdm-d3d9-selftest' } elseif ($D3d9 -and $D3d9Multistream7) { '-xpdm-d3d9-multistream7' } elseif ($D3d9) { '-xpdm-d3d9' } elseif ($D3dBenchmark -and $D3dHardware) { '-xpdm-d3d-hardware-benchmark' } elseif ($D3dBenchmark) { '-xpdm-d3d-benchmark' } elseif ($D3dCopyVisual) { '-xpdm-d3d-copy-visual' } elseif ($D3dVisual -and $D3dHwtlOnly -and $D3dHardware -and $D3d16) { '-xpdm-d3d16-hardware-hwtl' } elseif ($D3dVisual -and $D3dHwtlOnly -and $D3dHardware) { '-xpdm-d3d-hardware-hwtl' } elseif ($D3dVisual -and $D3dHwtlOnly -and $D3d16) { '-xpdm-d3d16-hwtl' } elseif ($D3dVisual -and $D3dHwtlOnly) { '-xpdm-d3d-hwtl' } elseif ($D3dVisual -and $D3dHardware -and $D3d16) { '-xpdm-d3d16-hardware-visual' } elseif ($D3dVisual -and $D3dHardware) { '-xpdm-d3d-hardware-visual' } elseif ($D3dVisual -and $D3d16) { '-xpdm-d3d16-visual' } elseif ($D3dVisual) { '-xpdm-d3d-visual' } elseif ($D3dFullscreen) { '-xpdm-d3d-fullscreen' } elseif ($D3d) { '-xpdm-d3d' } elseif ($BootOnly) { '-xpdm-boot' } elseif ($Xpdm) { '-xpdm' } else { '' }
Update-XpAcceptanceArtifactLease -Lease $artifactLease `
    -QmpPort $QmpPort -Mode $variant -ContentPath $content
$captureMode = if ($ObservePerformance -or $D3dPerformance -or
                    $GamePerformance) {
    'capture-free'
} elseif ($RecordVideo) {
    'video'
} elseif ($D3dVisual -or $D3dCopyVisual -or $D3dVsMode -or $D3d9 -or
          $Game -or $D3dGame -or $GameObserve) {
    'visual'
} else {
    'none'
}
Write-Output "HarnessConfig runId=$RunId harnessPid=$PID mode=$variant qmpPort=$QmpPort requestedTimeoutSeconds=$TimeoutSeconds effectiveTimeoutSeconds=$effectiveTimeout recommendedOuterTimeoutSeconds=$($timeoutBudget.RecommendedOuterSeconds) progressIntervalSeconds=$ProgressIntervalSeconds captureMode=$captureMode content=$content"
$stdout = Join-Path $vmDir "automated-host$variant.stdout.log"
$stderr = Join-Path $vmDir "automated-host$variant.stderr.log"
$stageLog = Join-Path $contentDir 'xpdm-stages.log'
$debugconName = if ($contentCommandLine -match
    '(?i)-debugcon\s+file:(?:"([^"]+)"|([^\s]+))') {
    if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
} else {
    'xpdm-debug.log'
}
$debugconSource = Join-Path $contentDir $debugconName
$serialResultName = if ($contentCommandLine -match
    '(?i)-serial\s+file:(?:"([^"]+)"|([^\s]+))') {
    if ($Matches[1]) { $Matches[1] } else { $Matches[2] }
} else {
    $null
}
$serialResultPath = if ($serialResultName) {
    Join-Path $contentDir $serialResultName
} else {
    $null
}
$driverLog = Join-Path $vmDir "automated-host$variant.driver.log"
$d3dCrashImage = Join-Path $vmDir 'xpdm-d3d-crash.ppm'
$d3dVisualStem = if ($D3dHardware -and $D3d16) {
    'xpdm-d3d16-hardware'
} elseif ($D3dHardware) {
    'xpdm-d3d-hardware'
} elseif ($D3d16) {
    'xpdm-d3d16'
} else { 'xpdm-d3d' }
$d3dVisualImage = Join-Path $vmDir "$d3dVisualStem-visual.bmp"
$d3dVisualFailureImage = Join-Path $vmDir "$d3dVisualStem-visual-failed.bmp"
$d3dHwtlReferenceImage = Join-Path $vmDir "$d3dVisualStem-hwtl-reference.bmp"
$d3dHwtlReferenceFailureImage = Join-Path $vmDir "$d3dVisualStem-hwtl-reference-failed.bmp"
$d3dHwtlSpecularImage = Join-Path $vmDir "$d3dVisualStem-hwtl-specular.bmp"
$d3dHwtlSpecularFailureImage = Join-Path $vmDir "$d3dVisualStem-hwtl-specular-failed.bmp"
$d3dPostResetHwtlImage = Join-Path $vmDir "$d3dVisualStem-post-reset-hwtl.bmp"
$d3dPostResetHwtlFailureImage = Join-Path $vmDir "$d3dVisualStem-post-reset-hwtl-failed.bmp"
$d3dFillModeImage = Join-Path $vmDir "$d3dVisualStem-fillmode.bmp"
$d3dFillModeFailureImage = Join-Path $vmDir "$d3dVisualStem-fillmode-failed.bmp"
$d3dCopyImage = Join-Path $vmDir 'xpdm-d3d-copy-visual.bmp'
$d3dCopyFailureImage = Join-Path $vmDir 'xpdm-d3d-copy-visual-failed.bmp'
$d3dVs11Stem = if ($D3dVsMultiContext) {
    'xpdm-d3d-vs-multictx'
} elseif ($D3dVsSuccession) {
    'xpdm-d3d-vs-succession'
} else { 'xpdm-d3d-vs11' }
$d3dVs11ClipImage = Join-Path $vmDir "$d3dVs11Stem-clip.bmp"
$d3dVs11FastImage = Join-Path $vmDir "$d3dVs11Stem-fast.bmp"
$d3dVs11FailureImage = Join-Path $vmDir "$d3dVs11Stem-failed.bmp"
$d3dVsMultiAImage = Join-Path $vmDir 'xpdm-d3d-vs-multictx-a.bmp'
$d3dVsMultiBImage = Join-Path $vmDir 'xpdm-d3d-vs-multictx-b.bmp'
$d3dVsMultiReturnImage = Join-Path $vmDir `
    'xpdm-d3d-vs-multictx-a-return.bmp'
$d3dVsMultiSurvivorImage = Join-Path $vmDir `
    'xpdm-d3d-vs-multictx-b-survivor.bmp'
$d3d9VisualImage = Join-Path $vmDir 'xpdm-d3d9.bmp'
$d3d9ResetImage = Join-Path $vmDir 'xpdm-d3d9-reset.bmp'
$d3d9VisualFailureImage = Join-Path $vmDir 'xpdm-d3d9-failed.bmp'
$gameVisualImage = Join-Path $vmDir 'xpdm-quake2.bmp'
$gameVisualFailureImage = Join-Path $vmDir 'xpdm-quake2-failed.bmp'
$d3dGameVisualImage = Join-Path $vmDir 'xpdm-maxpayne.bmp'
$d3dGameVisualFailureImage = Join-Path $vmDir 'xpdm-maxpayne-failed.bmp'
$gameObserveImage = if ($GameObserve) {
    Join-Path $vmDir "xpdm-$($ObserveGame.ToLowerInvariant())-baseline.bmp"
} else { $null }
$gameObserveSamplePrefix = if ($GameObserve) {
    "xpdm-$($ObserveGame.ToLowerInvariant())-sample"
} else { $null }
$gameObserveVideo = if ($GameObserve) {
    Join-Path $vmDir "xpdm-$($ObserveGame.ToLowerInvariant())-review.mp4"
} elseif ($Game) {
    Join-Path $vmDir 'xpdm-quake2-review.mp4'
} else { $null }
$warcraft3D3D8Cadence = if ($Warcraft3D3D8Oracle) {
    Join-Path $vmDir 'xpdm-warcraft3d3d8-black-cadence.json'
} else { $null }
$warcraft3D3D8RingDirectory = if ($Warcraft3D3D8Oracle) {
    Join-Path $vmDir ".xpdriver-$RunId-warcraft3d3d8-ring"
} else { $null }
$frameRingOracle = Join-Path $PSScriptRoot 'frame-ring-black-cadence.py'
$ffmpeg = Join-Path $workspace `
    '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe'
$ffprobe = Join-Path (Split-Path -Parent $ffmpeg) 'ffprobe.exe'

function New-GameObserveVideo {
    param(
        [IO.FileInfo[]]$Frames,
        [string]$Destination
    )

    if (-not $Frames.Count) { throw 'No observed frames are available for video' }
    if (-not (Test-Path -LiteralPath $ffmpeg) -or
        -not (Test-Path -LiteralPath $ffprobe)) {
        throw "Video encoder/probe missing. Run qemu\harness\install-video-tools.ps1 once."
    }

    # The frontend creates each bitmap when it publishes that frame.  Resample
    # those timestamps onto a fixed 15 fps timeline so slow and repeated game
    # frames retain their wall-clock duration.  Bound review clips to the full
    # 45-second guest observation and use only the dominant frame size.
    Add-Type -AssemblyName System.Drawing
    $dominantSize = $Frames | Group-Object {
        if ($_.Name -match '^frame_(\d+x\d+)_\d{6}\.bmp$') {
            $Matches[1]
        } else {
            $frameBitmap = [Drawing.Bitmap]::new($_.FullName)
            try { "$($frameBitmap.Width)x$($frameBitmap.Height)" }
            finally { $frameBitmap.Dispose() }
        }
    } | Sort-Object Count -Descending | Select-Object -First 1
    $usable = @($Frames | Where-Object {
        if ($_.Name -match '^frame_(\d+x\d+)_\d{6}\.bmp$') {
            return $Matches[1] -eq $dominantSize.Name
        }
        $frameBitmap = [Drawing.Bitmap]::new($_.FullName)
        try {
            return "$($frameBitmap.Width)x$($frameBitmap.Height)" -eq
                $dominantSize.Name
        } finally { $frameBitmap.Dispose() }
    } | Sort-Object LastWriteTime, Name)
    if (-not $usable.Count) { throw 'No consistently sized frames are available for video' }

    $frameRate = 15
    $maximumSeconds = 45
    $startTime = $usable[0].LastWriteTime
    $availableSeconds = [Math]::Max(
        1.0, ($usable[-1].LastWriteTime - $startTime).TotalSeconds)
    $durationSeconds = [Math]::Min($maximumSeconds, $availableSeconds)
    $sampleCount = [Math]::Max(
        2, [int][Math]::Ceiling($durationSeconds * $frameRate))
    $manifest = Join-Path (Split-Path -Parent $usable[0].FullName) `
        'xpdm-review-video.ffconcat'
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('ffconcat version 1.0')
    $sourceIndex = 0
    for ($sampleIndex = 0; $sampleIndex -lt $sampleCount; ++$sampleIndex) {
        $sampleTime = $startTime.AddSeconds($sampleIndex / $frameRate)
        while ($sourceIndex + 1 -lt $usable.Count -and
            $usable[$sourceIndex + 1].LastWriteTime -le $sampleTime) {
            ++$sourceIndex
        }
        $lines.Add("file '$($usable[$sourceIndex].Name.Replace("'", "''"))'")
        $lines.Add(('duration {0:F7}' -f (1.0 / $frameRate)))
    }
    # The concat demuxer ignores the final duration unless another file follows.
    $lines.Add("file '$($usable[$sourceIndex].Name.Replace("'", "''"))'")
    Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII

    Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    $videoValidated = $false
    try {
        & $ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 `
            -i $manifest -an -vf 'scale=trunc(iw/2)*2:trunc(ih/2)*2:flags=neighbor' `
            -c:v libx264 -preset veryfast -crf 22 -pix_fmt yuv420p `
            -movflags +faststart -r $frameRate -frames:v $sampleCount `
            $Destination
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Destination)) {
            throw "FFmpeg failed with exit code $LASTEXITCODE"
        }
        $probeOutput = @(& $ffprobe -v error -count_frames `
            -select_streams 'v:0' `
            -show_entries 'stream=nb_read_frames,r_frame_rate,avg_frame_rate:format=duration' `
            -of json $Destination 2>&1)
        if ($LASTEXITCODE -ne 0 -or -not $probeOutput.Count) {
            throw "FFprobe failed with exit code $LASTEXITCODE"
        }
        try {
            $probe = ($probeOutput -join [Environment]::NewLine) |
                ConvertFrom-Json
        } catch {
            throw "FFprobe returned invalid JSON: $($_.Exception.Message)"
        }
        $probeStreams = @($probe.streams)
        $encodedFrames = 0
        $encodedDuration = 0.0
        $durationParsed = $probe.format -and [double]::TryParse(
            [string]$probe.format.duration,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$encodedDuration)
        if ($probeStreams.Count -ne 1 -or
            -not [int]::TryParse(
                [string]$probeStreams[0].nb_read_frames,
                [ref]$encodedFrames) -or -not $durationParsed -or
            -not (Test-XpGameObserveEncodedVideoTiming `
                -ExpectedFrames $sampleCount `
                -EncodedFrames $encodedFrames -FrameRate $frameRate `
                -NominalFrameRate ([string]$probeStreams[0].r_frame_rate) `
                -AverageFrameRate ([string]$probeStreams[0].avg_frame_rate) `
                -DurationSeconds $encodedDuration)) {
            throw "Encoded review video does not match exact frame timing"
        }
        $videoValidated = $true
    } finally {
        Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
        Remove-XpUnpublishedReviewVideo `
            -Path $Destination -Published $videoValidated
    }
    $publishedVideo = Publish-XpValidatedReviewVideo `
        -Destination $Destination
    return [pscustomobject][ordered]@{
        path = $publishedVideo.path
        frames = $encodedFrames
        fps = $frameRate
        durationSeconds = [Math]::Round($encodedDuration, 2)
        sourceSize = $dominantSize.Name
    }
}

function New-Warcraft3D3D8RingVideo {
    param(
        [IO.FileInfo[]]$Frames,
        [string]$Destination
    )

    $ordered = @($Frames | Sort-Object LastWriteTimeUtc, Name)
    if ($ordered.Count -ne 256) {
        throw "Warcraft D3D8 video requires 256 ring frames; got $($ordered.Count)"
    }
    if (-not (Test-Path -LiteralPath $ffmpeg -PathType Leaf) -or
        -not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
        throw 'Pinned FFmpeg/FFprobe tools are missing'
    }
    Add-Type -AssemblyName System.Drawing
    $dimensions = @($ordered | ForEach-Object {
        $bitmap = [Drawing.Bitmap]::new($_.FullName)
        try { "$($bitmap.Width)x$($bitmap.Height)" }
        finally { $bitmap.Dispose() }
    } | Select-Object -Unique)
    if ($dimensions.Count -ne 1) {
        throw "Warcraft D3D8 ring dimensions changed: $($dimensions -join ',')"
    }

    $manifest = Join-Path (Split-Path -Parent $ordered[0].FullName) `
        'xpdm-warcraft3d3d8-video.ffconcat'
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('ffconcat version 1.0')
    foreach ($frame in $ordered) {
        $lines.Add("file '$($frame.Name.Replace("'", "''"))'")
    }
    Set-Content -LiteralPath $manifest -Value $lines -Encoding ASCII
    Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
    $videoValidated = $false
    try {
        # One concat entry and one output frame per frozen ring publication.
        # Give setpts an exact 1/30 time base and bind the encoder to it; the
        # concat demuxer's default 1/25 base otherwise quantizes every sixth
        # timestamp into a duplicate while still reporting 256 frames.
        & $ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 `
            -i $manifest -an `
            -vf 'settb=expr=1/30,setpts=N,scale=trunc(iw/2)*2:trunc(ih/2)*2:flags=neighbor' `
            -fps_mode passthrough -enc_time_base '1:30' `
            -c:v libx264 -preset veryfast -crf 22 `
            -pix_fmt yuv420p -movflags +faststart $Destination
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
            throw "FFmpeg failed with exit code $LASTEXITCODE"
        }
        $probeOutput = @(& $ffprobe -v error -count_frames `
            -select_streams 'v:0' `
            -show_entries 'stream=nb_read_frames,r_frame_rate,avg_frame_rate:format=duration' `
            -of json $Destination 2>&1)
        if ($LASTEXITCODE -ne 0 -or -not $probeOutput.Count) {
            throw "FFprobe failed with exit code $LASTEXITCODE"
        }
        try {
            $probe = ($probeOutput -join [Environment]::NewLine) |
                ConvertFrom-Json
        } catch {
            throw "FFprobe returned invalid JSON: $($_.Exception.Message)"
        }
        $probeStreams = @($probe.streams)
        $encodedFrames = 0
        $encodedDuration = 0.0
        $durationParsed = $probe.format -and [double]::TryParse(
            [string]$probe.format.duration,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$encodedDuration)
        if ($probeStreams.Count -ne 1 -or
            -not [int]::TryParse(
                [string]$probeStreams[0].nb_read_frames,
                [ref]$encodedFrames) -or -not $durationParsed -or
            -not (Test-XpWarcraft3D3D8EncodedVideoTiming `
                -SnapshotFrames $ordered.Count `
                -EncodedFrames $encodedFrames `
                -NominalFrameRate ([string]$probeStreams[0].r_frame_rate) `
                -AverageFrameRate ([string]$probeStreams[0].avg_frame_rate) `
                -DurationSeconds $encodedDuration)) {
            throw "Encoded video does not match 256 frames at exact 30 fps"
        }
        $videoValidated = $true
    } finally {
        Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
        Remove-XpUnpublishedReviewVideo `
            -Path $Destination -Published $videoValidated
    }
    $publishedVideo = Publish-XpValidatedReviewVideo `
        -Destination $Destination
    return [pscustomobject][ordered]@{
        path = $publishedVideo.path
        sourceFrames = $ordered.Count
        encodedFrames = $encodedFrames
        fps = 30
        durationSeconds = [Math]::Round($encodedDuration, 3)
        sourceSize = $dimensions[0]
    }
}

function Get-LatestDumpFrameSequence {
    $sequences = @(Get-ChildItem -LiteralPath $vmDir -File |
        ForEach-Object {
            if ($_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$') {
                [int]$Matches[1]
            }
        })
    if ($sequences.Count) {
        return [int](($sequences | Measure-Object -Maximum).Maximum)
    }
    return -1
}

function Invoke-GuestLaunch {
    param([int]$Port)

    $programKeys = if ($Reclaim) {
        @('q', 'x', 'p', 'r', 'e', 'c', 'l', 'a', 'i', 'm',
          'dot', 'c', 'm', 'd')
    } elseif ($Idle) {
        @('q', 'x', 'p', 'i', 'd', 'l', 'e', 'dot', 'c', 'm', 'd')
    } elseif ($Wgl) {
        @('q', 'x', 'p', 'w', 'g', 'l', 't', 'e', 's', 't',
          'dot', 'e', 'x', 'e')
    } elseif ($D3dGame) {
        $d3dGameKeys = @('q', 'x', 'p', 'd', '3', 'd', 'g', 'a', 'm', 'e',
          'dot', 'e', 'x', 'e')
        if ($D3dGameplay) {
            $d3dGameKeys += @('spc', 'minus', 'minus', 't', 'u', 't', 'o',
                              'r', 'i', 'a', 'l')
        } elseif ($D3dNormalLaunch) {
            $d3dGameKeys += @('spc', 'minus', 'minus', 'n', 'o', 'r', 'm',
                              'a', 'l', 'minus', 'l', 'a', 'u', 'n', 'c', 'h')
        }
        $d3dGameKeys
    } elseif ($GameObserve) {
        $gameKeys = if ($ObserveGame -eq 'UT2003') {
            $keys = @('u', 't', '2', '0', '0', '3')
            if ($ObservePerformance) {
                $keys += @('p', 'e', 'r', 'f', 'o', 'r', 'm', 'a', 'n', 'c', 'e')
            } elseif ($RecordVideo) {
                $keys += @('v', 'i', 's', 'u', 'a', 'l')
            }
            $keys
        } elseif ($ObserveGame -eq 'UT2003Gameplay') {
            $keys = @('u', 't', '2', '0', '0', '3', 'g', 'a', 'm', 'e',
              'p', 'l', 'a', 'y')
            if ($ObservePerformance) {
                $keys += @('p', 'e', 'r', 'f', 'o', 'r', 'm', 'a', 'n',
                           'c', 'e')
            }
            $keys
        } elseif ($ObserveGame -eq 'UT2003GameplaySWVP') {
            @('u', 't', '2', '0', '0', '3', 'g', 'a', 'm', 'e', 'p', 'l', 'a', 'y',
              's', 'w', 'v', 'p')
        } elseif ($ObserveGame -eq 'NFSU2') {
            $keys = @('n', 'f', 's', 'u', '2')
            if ($ObservePerformance) {
                $keys += @('p', 'e', 'r', 'f', 'o', 'r', 'm', 'a', 'n', 'c', 'e')
            } elseif ($RecordVideo) {
                $keys += @('v', 'i', 's', 'u', 'a', 'l')
            }
            $keys
        } elseif ($ObserveGame -eq 'Warcraft3D3D8') {
            $keys = @('w', 'a', 'r', 'c', 'r', 'a', 'f', 't', '3',
              'd', '3', 'd', '8')
            if ($ObservePerformance) {
                $keys += @('p', 'e', 'r', 'f', 'o', 'r', 'm', 'a', 'n',
                           'c', 'e')
            }
            $keys
        } else {
            $keys = @('w', 'a', 'r', 'c', 'r', 'a', 'f', 't', '3')
            if ($ObservePerformance) {
                $keys += @('p', 'e', 'r', 'f', 'o', 'r', 'm', 'a', 'n', 'c', 'e')
            }
            $keys
        }
        @('q', 'x', 'p', 'g', 'a', 'm', 'e', 'o', 'b', 's', 'e', 'r', 'v', 'e',
          'dot', 'e', 'x', 'e', 'spc') + $gameKeys
    } elseif ($D3dClean) {
        @('q', 'x', 'p', 'd', '3', 'd', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'c', 'l', 'e', 'a', 'n', 'minus',
          'm', 'a', 'x', 'p', 'a', 'y', 'n', 'e')
    } elseif ($D3dBenchmark) {
        $probeKeys = @('q', 'x', 'p', 'd', '3', 'd', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'b', 'e', 'n', 'c', 'h', 'm', 'a', 'r', 'k')
        if ($D3dHardware) {
            $probeKeys += @('spc', 'minus', 'minus', 'h', 'a', 'r', 'd',
                            'w', 'a', 'r', 'e', 'minus', 'v', 'p')
        }
        $probeKeys
    } elseif ($D3dVsBenchmark) {
        @('q', 'x', 'p', 'd', '3', 'd', 'v', 's', 'b', 'e', 'n', 'c', 'h',
          'dot', 'e', 'x', 'e')
    } elseif ($D3dVsMultiContext) {
        @('q', 'x', 'p', 'd', '3', 'd', 'v', 's', 'm', 'u', 'l', 't', 'i',
          'c', 't', 'x', 'dot', 'e', 'x', 'e')
    } elseif ($D3dVsSuccession) {
        @('q', 'x', 'p', 'd', '3', 'd', 'v', 's', 's', 'u', 'c', 'c', 'e',
          's', 's', 'i', 'o', 'n', 'dot', 'e', 'x', 'e')
    } elseif ($D3dVs11) {
        @('q', 'x', 'p', 'd', '3', 'd', 'v', 's', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e')
    } elseif ($D3d9SelfTest) {
        @('q', 'x', 'p', 'd', '3', 'd', '9', 's', 'e', 'l', 'f',
          't', 'e', 's', 't', 'dot', 'e', 'x', 'e')
    } elseif ($D3d9) {
        $d3d9Keys = @('q', 'x', 'p', 'd', '3', 'd', '9', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'c', 'a', 'p', 't', 'u', 'r', 'e', 'minus',
          'h', 'a', 'n', 'd', 's', 'h', 'a', 'k', 'e')
        if ($D3d9Multistream7) {
            $d3d9Keys += @('spc', 'minus', 'minus', 'm', 'u', 'l', 't', 'i',
                           's', 't', 'r', 'e', 'a', 'm', '7')
        }
        $d3d9Keys
    } elseif ($D3dCopyVisual) {
        @('q', 'x', 'p', 'd', '3', 'd', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'c', 'o', 'p', 'y', 'minus', 'p', 'r', 'e', 's', 'e', 'r', 'v', 'e')
    } elseif ($D3dFullscreen -or $D3dVisual) {
        $probeKeys = @('q', 'x', 'p', 'd', '3', 'd', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'f', 'u', 'l', 'l', 's', 'c', 'r', 'e', 'e', 'n')
        if ($D3dVisual) {
            # Forced synchronous readback can reduce the dump callback to
            # roughly 0.5 FPS. Ask the probe to hold each unique frame long
            # enough for deterministic capture without slowing nonvisual
            # fullscreen smoke tests.
            $probeKeys += @('spc', 'minus', 'minus', 'v', 'i', 's', 'u', 'a', 'l')
        }
        if ($D3d16) {
            $probeKeys += @('spc', 'minus', 'minus', 'r', 'g', 'b', '5', '6', '5')
        }
        if ($D3dHardware) {
            $probeKeys += @('spc', 'minus', 'minus', 'h', 'a', 'r', 'd',
                            'w', 'a', 'r', 'e', 'minus', 'v', 'p')
        }
        if ($D3dHwtlOnly) {
            $probeKeys += @('spc', 'minus', 'minus', 'h', 'w', 't', 'l',
                            'minus', 'o', 'n', 'l', 'y')
        }
        $probeKeys
    } elseif ($D3d) {
        @('q', 'x', 'p', 'd', '3', 'd', 'p', 'r', 'o', 'b', 'e',
          'dot', 'e', 'x', 'e')
    } elseif ($Game) {
        $gameTestKeys = @('q', 'x', 'p', 'g', 'a', 'm', 'e', 't', 'e', 's', 't',
          'dot', 'e', 'x', 'e')
        if ($GamePerformance) {
            $gameTestKeys += @('spc', 'minus', 'minus', 'p', 'e', 'r', 'f',
                               'o', 'r', 'm', 'a', 'n', 'c', 'e')
        }
        $gameTestKeys
    } elseif ($BootOnly) {
        @('q', 'x', 'p', 'w', 'g', 'l', 't', 'e', 's', 't',
          'dot', 'e', 'x', 'e', 'spc', 'minus', 'minus',
          'n', 'a', 't', 'i', 'v', 'e')
    } elseif ($InstallOnly) {
        @('q', 'x', 'p', 's', 'e', 't', 'u', 'p', 'dot', 'e', 'x', 'e',
          'spc', 'minus', 'minus', 's', 't', 'a', 'g', 'e', 'minus',
          'o', 'n', 'l', 'y')
    } elseif ($Xpdm) {
        @('q', 'x', 'p', 's', 'e', 't', 'u', 'p', 'dot', 'e', 'x', 'e')
    } else {
        @('q', 'x', 'p', 't', 'e', 's', 't', 'dot', 'e', 'x', 'e')
    }
    $directoryKeys = if ($Xpdm) {
        @('x', 'p', 'd', 'm')
    } else {
        @('q', 'x', 'p', 'i', 'c', 'd')
    }
    $keys = @('esc', 'ctrl-esc', 'r', 'ctrl-a', 'backspace',
              'd', 'shift-semicolon', 'backslash') +
            $directoryKeys + @('backslash') + $programKeys + @('ret')
    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        Start-Sleep -Milliseconds 100
        for ($keyIndex = 0; $keyIndex -lt $keys.Count; ++$keyIndex) {
            $key = $keys[$keyIndex]
            $command = @{
                execute = 'human-monitor-command'
                arguments = @{ 'command-line' = "sendkey $key" }
            } | ConvertTo-Json -Compress -Depth 4
            $writer.WriteLine($command)
            $delay = if ($keyIndex -eq 0) {
                300
            } elseif ($keyIndex -eq 1 -or $keyIndex -eq 2) {
                500
            } else {
                90
            }
            Start-Sleep -Milliseconds $delay
        }
        $writer.Dispose()
        $client.Dispose()
        return $true
    } catch {
        return $false
    }
}

function Invoke-GuestLogin {
    param([int]$Port)

    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        Start-Sleep -Milliseconds 100
        $command = @{
            execute = 'human-monitor-command'
            arguments = @{ 'command-line' = 'sendkey ret' }
        } | ConvertTo-Json -Compress -Depth 4
        $writer.WriteLine($command)
        $writer.Dispose()
        $client.Dispose()
        return $true
    } catch {
        return $false
    }
}

function Invoke-GuestKey {
    param(
        [int]$Port,
        [string]$Key,
        [int]$HoldMilliseconds = 0
    )

    $client = $null
    $reader = $null
    $writer = $null
    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 3000
        $stream.WriteTimeout = 3000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        if (-not $reader.ReadLine()) { return $false }
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        $capabilitiesReply = $reader.ReadLine()
        if (-not $capabilitiesReply -or
            $capabilitiesReply.Contains('"error"')) {
            return $false
        }
        $sendKeyCommand = if ($HoldMilliseconds -gt 0) {
            "sendkey $Key $HoldMilliseconds"
        } else {
            "sendkey $Key"
        }
        $command = @{
            execute = 'human-monitor-command'
            arguments = @{ 'command-line' = $sendKeyCommand }
        } | ConvertTo-Json -Compress -Depth 4
        $writer.WriteLine($command)
        $commandReply = $reader.ReadLine()
        return $commandReply -and -not $commandReply.Contains('"error"')
    } catch {
        return $false
    } finally {
        if ($writer) { $writer.Dispose() }
        if ($reader) { $reader.Dispose() }
        if ($client) { $client.Dispose() }
    }
}

function Invoke-GuestMouseClick {
    param([int]$Port)

    $client = $null
    $reader = $null
    $writer = $null
    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 3000
        $stream.WriteTimeout = 3000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        if (-not $reader.ReadLine()) { return $false }
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        $capabilitiesReply = $reader.ReadLine()
        if (-not $capabilitiesReply -or
            $capabilitiesReply.Contains('"error"')) {
            return $false
        }
        $command = @{
            execute = 'input-send-event'
            arguments = @{
                events = @(
                    @{ type = 'btn'; data = @{ down = $true; button = 'left' } },
                    @{ type = 'btn'; data = @{ down = $false; button = 'left' } }
                )
            }
        } | ConvertTo-Json -Compress -Depth 6
        $writer.WriteLine($command)
        $commandReply = $reader.ReadLine()
        return $commandReply -and -not $commandReply.Contains('"error"')
    } catch {
        return $false
    } finally {
        if ($writer) { $writer.Dispose() }
        if ($reader) { $reader.Dispose() }
        if ($client) { $client.Dispose() }
    }
}

function Test-D3dVisualPixel {
    param($Pixel, [string]$ExpectedRgb, [int]$Tolerance = 0)

    $red = [Convert]::ToInt32($ExpectedRgb.Substring(0, 2), 16)
    $green = [Convert]::ToInt32($ExpectedRgb.Substring(2, 2), 16)
    $blue = [Convert]::ToInt32($ExpectedRgb.Substring(4, 2), 16)
    if (-not $D3d16) {
        $redTolerance = if ($red -ne 0) { $Tolerance } else { 0 }
        $greenTolerance = if ($green -ne 0) { $Tolerance } else { 0 }
        $blueTolerance = if ($blue -ne 0) { $Tolerance } else { 0 }
        return [Math]::Abs([int]$Pixel.R - $red) -le $redTolerance -and
            [Math]::Abs([int]$Pixel.G - $green) -le $greenTolerance -and
            [Math]::Abs([int]$Pixel.B - $blue) -le $blueTolerance
    }
    # RGB565 scanout expands five-bit red/blue and six-bit green back to
    # eight-bit channels. Allow one source quantization step for nonzero
    # expectations, but keep exactly representable zero channels strict.
    $redQuantization = if ($red -ne 0) { 7 } else { 0 }
    $greenQuantization = if ($green -ne 0) { 3 } else { 0 }
    $blueQuantization = if ($blue -ne 0) { 7 } else { 0 }
    return [Math]::Abs([int]$Pixel.R - $red) -le $redQuantization -and
        [Math]::Abs([int]$Pixel.G - $green) -le $greenQuantization -and
        [Math]::Abs([int]$Pixel.B - $blue) -le $blueQuantization
}

function Get-D3dVisualSamplePixel {
    param($Bitmap, $Sample)

    if ($Sample.X -lt 0 -or $Sample.Y -lt 0 -or
        $Sample.X -ge $Bitmap.Width -or $Sample.Y -ge $Bitmap.Height) {
        return $null
    }
    return $Bitmap.GetPixel($Sample.X, $Sample.Y)
}

function Export-OpaqueReviewPng {
    param([string]$SourcePath)

    if (-not (Test-Path -LiteralPath $SourcePath)) { return $null }
    $source = [Drawing.Bitmap]::new($SourcePath)
    try {
        # libretro frame dumps are useful raw evidence, but some image
        # consumers misinterpret their 32-bit BMP channel masks and display a
        # valid RGB565 capture as solid black.  A 24-bit clone preserves the
        # classified RGB values in a portable, reviewable PNG.
        $rectangle = [Drawing.Rectangle]::new(
            0, 0, $source.Width, $source.Height)
        $review = $source.Clone(
            $rectangle, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $destination = [IO.Path]::ChangeExtension(
                $SourcePath, 'review.png')
            $review.Save($destination, [Drawing.Imaging.ImageFormat]::Png)
            return $destination
        } finally {
            $review.Dispose()
        }
    } finally {
        $source.Dispose()
    }
}

Remove-Item $stdout, $stderr, $driverLog -ErrorAction SilentlyContinue
if ($D3dAnyProbe) {
    Remove-Item $d3dCrashImage -ErrorAction SilentlyContinue
}
if (($Game -and -not $GamePerformance) -or
    $D3dVisual -or $D3dCopyVisual -or $D3dVsMode -or
    ($D3dGame -and -not $D3dPerformance) -or
    $D3d9 -or ($GameObserve -and -not $ObservePerformance)) {
    $captureImages = if ($Game) {
        @($gameVisualImage, $gameVisualFailureImage)
    } elseif ($D3dGame) {
        @($d3dGameVisualImage, $d3dGameVisualFailureImage)
    } elseif ($D3dVsMultiContext) {
        @($d3dVsMultiAImage, $d3dVsMultiBImage,
          $d3dVsMultiReturnImage, $d3dVsMultiSurvivorImage,
          $d3dVs11FailureImage)
    } elseif ($D3dVsMode) {
        @($d3dVs11ClipImage, $d3dVs11FastImage,
          $d3dVs11FailureImage)
    } elseif ($D3d9) {
        @($d3d9VisualImage, $d3d9ResetImage, $d3d9VisualFailureImage)
    } elseif ($D3dVisual) {
        @($d3dVisualImage, $d3dVisualFailureImage,
          $d3dHwtlReferenceImage, $d3dHwtlReferenceFailureImage,
          $d3dHwtlSpecularImage, $d3dHwtlSpecularFailureImage,
          $d3dPostResetHwtlImage, $d3dPostResetHwtlFailureImage,
          $d3dFillModeImage, $d3dFillModeFailureImage)
    } elseif ($D3dCopyVisual) {
        @($d3dCopyImage, $d3dCopyFailureImage)
    } else {
        @($gameObserveImage)
    }
    if ($Warcraft3D3D8Oracle) {
        $captureImages += $warcraft3D3D8Cadence
        Remove-Item -LiteralPath $warcraft3D3D8RingDirectory `
            -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $warcraft3D3D8Cadence `
            -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -match '^frame-ring-[0-9]+\.bmp$' -or
                $_.Name -eq 'frame-ring-tmp.bmp' -or
                $_.Name -match '^xpdm-warcraft3d3d8-(?:sample|flicker)-'
            } | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    if ($D3dGame -and $D3dGameplay) {
        # Tutorial capture publishes three identity-bearing raw BMPs selected
        # after the guest's warmup, not the menu-only singleton.
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -like 'maxpayne-tutorial-*' -or
                $_.Name -eq 'maxpayne-visual-sequence.json'
            } | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    if ($D3dVisual) {
        $captureImages += @($captureImages | ForEach-Object {
            [IO.Path]::ChangeExtension($_, 'review.png')
        })
    }
    if ($RecordVideo) {
        # New-GameObserveVideo renames the temporary destination with a digest.
        # Do not retain this pre-rename path; each encoder caller appends the
        # returned final path after the rename succeeds.
        Remove-Item -LiteralPath $gameObserveVideo -Force `
            -ErrorAction SilentlyContinue
        $reviewPattern = if ($GameObserve) {
            "xpdm-$($ObserveGame.ToLowerInvariant())-review-*.mp4"
        } else {
            'xpdm-quake2-review-*.mp4'
        }
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -like $reviewPattern } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
    if (@($captureImages).Count) {
        Remove-Item $captureImages -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
        Remove-Item -Force
}
if ($Xpdm) {
    $xpdmArtifacts = @(
        $debugconSource,
        $stageLog,
        $serialResultPath
    )
    Remove-Item ($xpdmArtifacts | Where-Object { $_ }) `
        -ErrorAction SilentlyContinue
}
$udp = $null
$serialStream = $null
$serialReadBuffer = [byte[]]::new(4096)
$serialPending = ''
$serialMessages = [Collections.Generic.Queue[string]]::new()
if (-not $serialResultPath) {
    $listenPorts = if ($ResultPort -eq 44574) {
        $ResultPort..($ResultPort + 16)
    } else {
        @($ResultPort)
    }
    foreach ($candidatePort in $listenPorts) {
        try {
            $udp = [Net.Sockets.UdpClient]::new()
            $udp.Client.ExclusiveAddressUse = $false
            $udp.Client.SetSocketOption(
                [Net.Sockets.SocketOptionLevel]::Socket,
                [Net.Sockets.SocketOptionName]::ReuseAddress, $true)
            $udp.Client.Bind([Net.IPEndPoint]::new(
                [Net.IPAddress]::Any, $candidatePort))
            $listenPort = $candidatePort
            break
        } catch [Net.Sockets.SocketException] {
            if ($udp) {
                $udp.Dispose()
                $udp = $null
            }
            if ($_.Exception.SocketErrorCode -ne
                [Net.Sockets.SocketError]::AddressAlreadyInUse -and
                $_.Exception.SocketErrorCode -ne
                [Net.Sockets.SocketError]::AccessDenied) {
                throw
            }
        }
    }
    if (-not $udp) {
        throw "No free acceptance UDP port in $($listenPorts[0])-$($listenPorts[-1])"
    }
    $udp.Client.Blocking = $false
}
$hostArguments = @($coreDll, $content, $QmpPort, '--wait',
                   ($effectiveTimeout * 1000), '--no-quit')
$frameProfileStartEvent = $null
$frameProfileStopEvent = $null
if ($ObservePerformance -or $D3dPerformance -or $GamePerformance) {
    $frameProfileRunId =
        "$PID-$([Guid]::NewGuid().ToString('N'))"
    $frameProfileStartEventName =
        "Local\XPDriverFrameProfileStart-$frameProfileRunId"
    $frameProfileStopEventName =
        "Local\XPDriverFrameProfileStop-$frameProfileRunId"
    $frameProfileStartEvent = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::AutoReset,
        $frameProfileStartEventName)
    $frameProfileStopEvent = [Threading.EventWaitHandle]::new(
        $false, [Threading.EventResetMode]::AutoReset,
        $frameProfileStopEventName)
    $hostArguments += @(
        '--frame-profile', $frameProfileStartEventName,
        $frameProfileStopEventName)
}
if (-not $D3dPerformance -and -not $D3dBenchmark -and
    -not $D3dVsBenchmark -and -not $ObservePerformance -and
    -not $GamePerformance) {
    $hostArguments += '--dump'
}
$start = @{
    FilePath = $hostExe
    ArgumentList = $hostArguments
    WorkingDirectory = $vmDir
    RedirectStandardOutput = $stdout
    RedirectStandardError = $stderr
    WindowStyle = 'Hidden'
    PassThru = $true
}
$oldDumpAll = [Environment]::GetEnvironmentVariable(
    'RETRO_HOST_DUMP_ALL', 'Process')
$oldDumpEvery = [Environment]::GetEnvironmentVariable(
    'RETRO_HOST_DUMP_EVERY', 'Process')
$oldSystemDirectory = [Environment]::GetEnvironmentVariable(
    'RETRO_HOST_SYSTEM_DIR', 'Process')
$oldForceSyncReadback = [Environment]::GetEnvironmentVariable(
    'QEMU_FX_FORCE_SYNC_READBACK', 'Process')
$oldFrameRing = [Environment]::GetEnvironmentVariable(
    'XPD_FRAME_RING', 'Process')
$oldFrameRingSlots = [Environment]::GetEnvironmentVariable(
    'XPD_FRAME_RING_SLOTS', 'Process')
$oldFrameRingInterval = [Environment]::GetEnvironmentVariable(
    'XPD_FRAME_RING_INTERVAL_US', 'Process')
try {
    if ($retroSystemDirectory) {
        [Environment]::SetEnvironmentVariable(
            'RETRO_HOST_SYSTEM_DIR', $retroSystemDirectory, 'Process')
    }
    if (($Game -and -not $GamePerformance) -or
        $D3dVisual -or $D3dCopyVisual -or $D3dVsMode -or
        ($D3dGame -and -not $D3dPerformance) -or
        $D3d9 -or ($GameObserve -and -not $ObservePerformance)) {
        [Environment]::SetEnvironmentVariable(
            'RETRO_HOST_DUMP_ALL', '1', 'Process')
    }
    if (($GameObserve -and -not $ObservePerformance) -or
        ($D3dGame -and $D3dGameplay -and -not $D3dPerformance)) {
        # Retain every eighth callback. This leaves several frames per scene
        # segment while bounding bitmap I/O in a separate visual-only run.
        [Environment]::SetEnvironmentVariable(
            'RETRO_HOST_DUMP_EVERY', '8', 'Process')
    } elseif ($Game -and $RecordVideo) {
        # Quake's ordinary visual gate is ten seconds. Retaining every fourth
        # callback keeps motion review smooth while bounding temporary BMP IO.
        [Environment]::SetEnvironmentVariable(
            'RETRO_HOST_DUMP_EVERY', '4', 'Process')
    }
    if ($Warcraft3D3D8Oracle) {
        # The existing cadence oracle consumes every published frontend frame.
        # A bounded 256-slot ring overwrites boot/loading history during the
        # guest's post-warmup observation without unbounded bitmap growth.
        [Environment]::SetEnvironmentVariable(
            'XPD_FRAME_RING', '1', 'Process')
        [Environment]::SetEnvironmentVariable(
            'XPD_FRAME_RING_SLOTS', '256', 'Process')
        [Environment]::SetEnvironmentVariable(
            'XPD_FRAME_RING_INTERVAL_US', '0', 'Process')
    }
    # Exact pixel verdicts must describe the just-completed back buffer. Most
    # game observations retain the normal asynchronous PBO stream. UT's menu
    # oracle is the exception: producer synchrony plus one quarantined frontend
    # callback gives its semantic capture boundary a closed ownership rule.
    if ($D3dVisual -or $D3dCopyVisual -or $D3dVsMode -or $D3d9 -or
        $UT2003VisualOracle) {
        [Environment]::SetEnvironmentVariable(
            'QEMU_FX_FORCE_SYNC_READBACK', '1', 'Process')
    }
    $process = Start-Process @start
    Update-XpAcceptanceArtifactLease -Lease $artifactLease `
        -HostPid $process.Id
} finally {
    [Environment]::SetEnvironmentVariable(
        'RETRO_HOST_DUMP_ALL', $oldDumpAll, 'Process')
    [Environment]::SetEnvironmentVariable(
        'RETRO_HOST_DUMP_EVERY', $oldDumpEvery, 'Process')
    [Environment]::SetEnvironmentVariable(
        'RETRO_HOST_SYSTEM_DIR', $oldSystemDirectory, 'Process')
    [Environment]::SetEnvironmentVariable(
        'QEMU_FX_FORCE_SYNC_READBACK', $oldForceSyncReadback, 'Process')
    [Environment]::SetEnvironmentVariable(
        'XPD_FRAME_RING', $oldFrameRing, 'Process')
    [Environment]::SetEnvironmentVariable(
        'XPD_FRAME_RING_SLOTS', $oldFrameRingSlots, 'Process')
    [Environment]::SetEnvironmentVariable(
        'XPD_FRAME_RING_INTERVAL_US', $oldFrameRingInterval, 'Process')
}

$runStartedAt = Get-Date
$deadline = $runStartedAt.AddSeconds($effectiveTimeout)
$nextProgressAt = $runStartedAt.AddSeconds($ProgressIntervalSeconds)
$nextLaunch = (Get-Date).AddSeconds($GuestLaunchDelaySeconds)
$launchRetrySeconds = if ($Game -or $D3dAnyProbe -or $D3dClean -or $D3dGame -or $GameObserve) { 60 } elseif ($Xpdm) { 30 } elseif ($Reclaim -or $Idle) { 30 } else { 12 }
$result = $null
$lastGuestFailure = $null
$installerStarted = $false
$hardwareVpOk = -not $D3dHardware
$customStateBlockOk = -not $D3dVisual -or $D3dHwtlOnly
$predefinedStateBlockOk = -not $D3dVisual -or $D3dHwtlOnly
$stateBlockOk = -not $D3dVisual -or $D3dHwtlOnly
$gamePlayAt = [DateTime]::MinValue
$gameObservationAt = [DateTime]::MaxValue
$gamePerformanceWarmupAt = [DateTime]::MaxValue
$gamePerformanceStopAt = [DateTime]::MaxValue
$gamePerformanceResult = $null
$d3dGamePlayAt = [DateTime]::MinValue
$d3dGameResultAt = [DateTime]::MaxValue
$d3dGameObservationAt = [DateTime]::MaxValue
$d3dGameStartFrame = -1
$d3dGameEndFrame = -1
$maxPayneVisualSelection = $null
$maxPayneVisualBoundary = New-XpMaxPayneVisualBoundaryState
$gameObserveStartedAt = [DateTime]::MinValue
$gameObserveCompletedAt = [DateTime]::MaxValue
$gameObserveStartFrame = -1
$gameObserveEndFrame = -1
$gameObserveInteractionStarted = $false
$nfsu2VisualReadySeen = $false
$ut2003VisualObservedMs = [int64]-1
$ut2003VisualBoundary = $null
$ut2003VisualReadyMessages = [Collections.Generic.List[string]]::new()
$ut2003VisualStageMessages = [Collections.Generic.List[string]]::new()
$ut2003VisualResultMessages = [Collections.Generic.List[string]]::new()
$ut2003VisualResultDrainUntil = [DateTime]::MaxValue
$ut2003VisualReadyFrame = -1
$ut2003GuardFrame = -1
$ut2003FirstAcceptedFrame = -1
$ut2003VisualGuestContract = $null
$ut2003VisualGuestContractOk = -not $UT2003VisualOracle
$ut2003VisualCoverage = $null
$ut2003VisualCoverageOk = -not $UT2003VisualOracle
$ut2003MenuVisual = $null
$ut2003MenuVisualOk = -not $UT2003VisualOracle
$warcraft3D3D8OracleStartedAt = [DateTime]::MinValue
$warcraft3D3D8OracleCompletedAt = [DateTime]::MaxValue
$warcraft3D3D8OracleStartMessages =
    [Collections.Generic.List[string]]::new()
$warcraft3D3D8OracleCompleteMessages =
    [Collections.Generic.List[string]]::new()
$warcraft3D3D8ResultMessages =
    [Collections.Generic.List[string]]::new()
$warcraft3D3D8ResultDrainUntil = [DateTime]::MaxValue
$warcraft3D3D8GuestContract = $null
$warcraft3D3D8GuestContractOk = -not $Warcraft3D3D8Oracle
$warcraft3D3D8RingFrames = @()
$warcraft3D3D8RingSnapshot = $null
$warcraft3D3D8CadenceResult = $null
$warcraft3D3D8CadenceOk = -not $Warcraft3D3D8Oracle
$warcraft3D3D8VideoOk = -not $Warcraft3D3D8Oracle
$d3dCopyEndFrame = -1
$d3dHwtlStartFrame = -1
$d3dHwtlEndFrame = -1
$d3dVs11ClipStartFrame = -1
$d3dVs11ClipEndFrame = -1
$d3dVs11FastStartFrame = -1
$d3dVs11FastEndFrame = -1
$d3dVsMultiAStartFrame = -1
$d3dVsMultiAEndFrame = -1
$d3dVsMultiBStartFrame = -1
$d3dVsMultiBEndFrame = -1
$d3dVsMultiReturnStartFrame = -1
$d3dVsMultiReturnEndFrame = -1
$d3dVsMultiSurvivorStartFrame = -1
$d3dVsMultiSurvivorEndFrame = -1
$d3d9SceneStartFrame = -1
$d3d9SceneEndFrame = -1
$d3d9ResetStartFrame = -1
$d3d9ResetEndFrame = -1
$d3d9SceneCaptureAck = $false
$d3d9ResetCaptureAck = $false
$d3d9SceneCaptureDispatchOk = $false
$d3d9ResetCaptureDispatchOk = $false
$d3d9MultistreamReady = -not $D3d9Multistream7
$d3d9MultistreamDraw = -not $D3d9Multistream7
$d3d9MultistreamDrawReleased = -not $D3d9Multistream7
$d3d9MultistreamLifetime = -not $D3d9Multistream7
$d3d9MultistreamStage = if ($D3d9Multistream7) { 0 } else { 9 }
$d3d9MultistreamOrderOk = $true
$d3d9NextSceneCaptureDispatch = [DateTime]::MaxValue
$d3d9NextResetCaptureDispatch = [DateTime]::MaxValue
$d3dVsSuccessionLeg1Ok = -not $D3dVsSuccession
$d3dVsSuccessionLeg2Ok = -not $D3dVsSuccession
$d3dVsBenchmarkPhases = [Collections.Generic.List[string]]::new()
$installReadyAt = [DateTime]::MaxValue
$rebooting = $BootOnly
$nextLogin = if ($BootOnly) {
    (Get-Date).AddSeconds(35)
} else {
    [DateTime]::MaxValue
}
try {
    while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
        $message = $null
        if ($serialResultPath) {
            if (-not $serialStream -and
                (Test-Path -LiteralPath $serialResultPath)) {
                try {
                    $serialStream = [IO.File]::Open(
                        $serialResultPath, [IO.FileMode]::Open,
                        [IO.FileAccess]::Read,
                        [IO.FileShare]::ReadWrite)
                } catch [IO.IOException] {
                    if ($serialStream) {
                        $serialStream.Dispose()
                        $serialStream = $null
                    }
                }
            }
            if ($serialStream) {
                Receive-SerialLines -Stream $serialStream `
                    -Buffer $serialReadBuffer -Pending ([ref]$serialPending) `
                    -Messages $serialMessages
                if ($serialMessages.Count) {
                    $message = $serialMessages.Dequeue()
                }
            }
        }
        if (-not $message -and $udp -and $udp.Available -gt 0) {
            $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
            $bytes = $udp.Receive([ref]$remote)
            $message = [Text.Encoding]::ASCII.GetString($bytes)
        }
        if ($message) {
            $lastGuestMessage = $message.Replace("`r", ' ').Replace("`n", ' ')
            if ($message.StartsWith('QXPICD_RESULT ')) {
                if ($Game -or $D3dAnyProbe -or $D3dClean -or $D3dGame -or
                    $GameObserve) {
                    Write-Output "GuestIgnored=$message"
                    if ($message -eq 'QXPICD_RESULT rc=0' -and
                        ($D3dAnyProbe -or $D3dClean -or $D3dGame)) {
                        # The native RunOnce smoke test returns only after its
                        # accelerated context has been released. That is a
                        # stronger readiness signal than the conservative
                        # fixed boot delay, so start D3D promptly while keeping
                        # the original deadline as a fallback when RunOnce is
                        # absent or reports a transient failure.
                        $readyLaunch = (Get-Date).AddSeconds(2)
                        if ($readyLaunch -lt $nextLaunch) {
                            $nextLaunch = $readyLaunch
                        }
                    }
                } elseif ($BootOnly -and $message -ne 'QXPICD_RESULT rc=0') {
                    $lastGuestFailure = $message
                    Write-Output "GuestRetry=$message"
                } elseif (-not $InstallOnly -or $installerStarted) {
                    $result = $message
                    break
                }
            }
            if ($message.StartsWith('QXP_WGL_STAGE ')) {
                Write-Output "GuestStage=$message"
            }
            if ($Game -and $message.StartsWith('QXP_GAME_RESULT ')) {
                if ($frameProfileStopEvent -and -not $GamePerformance) {
                    Stop-XpPerformanceProfileWindow
                }
                if ($message.StartsWith('QXP_GAME_RESULT rc=0 ')) {
                    if ($GamePerformance) {
                        $gamePerformanceResult = $message
                        $guestTestStarted = $true
                        $nextLaunch = [DateTime]::MaxValue
                        if ($gamePerformanceWarmupAt -eq
                            [DateTime]::MaxValue) {
                            # The result repeats benchmark-ready=1 so a lost
                            # adjacent UDP stage cannot strand the host before
                            # its deterministic warm-up window.
                            $gamePerformanceWarmupAt =
                                (Get-Date).AddSeconds(15)
                        }
                    } else {
                        $result = $message
                        break
                    }
                } elseif ($GamePerformance -and $gamePerformanceResult) {
                    $result = $message
                    break
                } else {
                    $lastGuestFailure = $message
                    Write-Output "GuestRetry=$message"
                }
            }
            if ($Game -and $message.StartsWith('QXP_GAME_STAGE ')) {
                if ($message.Contains('stage=rendering') -and
                    -not $GamePerformance) {
                    $gamePlayAt = Get-Date
                }
                if ($message.Contains('stage=benchmark-ready') -and
                    $GamePerformance) {
                    $guestTestStarted = $true
                    $nextLaunch = [DateTime]::MaxValue
                    if ($gamePerformanceWarmupAt -eq
                        [DateTime]::MaxValue) {
                        $gamePerformanceWarmupAt = (Get-Date).AddSeconds(15)
                    }
                } elseif ($message.Contains('stage=observation-complete') -and
                          -not $GamePerformance) {
                    $gameObservationAt = Get-Date
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3dProbe -and $message.StartsWith('QXP_D3D_RESULT ')) {
                if ($D3dCopyVisual -and $d3dCopyEndFrame -lt 0) {
                    Start-Sleep -Milliseconds 500
                    $d3dCopyEndFrame = Get-LatestDumpFrameSequence
                }
                $result = $message
                break
            }
            if ($D3dVsBenchmark -and
                $message.StartsWith('QXP_D3DVS_BENCH_PHASE ')) {
                $guestTestStarted = $true
                $d3dVsBenchmarkPhases.Add($message)
                Write-Output "GuestStage=$message"
            }
            if ($D3dVsBenchmark -and
                $message.StartsWith('QXP_D3DVS_BENCH_RESULT ')) {
                $guestTestStarted = $true
                $result = $message
                break
            }
            if ($D3dVsMultiContext -and
                $message.StartsWith('QXP_D3DVS_MULTI_RESULT ')) {
                $guestTestStarted = $true
                $result = $message
                break
            }
            if ($D3dVsMultiContext -and
                $message.StartsWith('QXP_D3DVS_MULTI_STAGE ')) {
                $guestTestStarted = $true
                if ($message.Contains('stage=device-a-frame-enter')) {
                    $d3dVsMultiAStartFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains('stage=device-a-frame-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVsMultiAEndFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains('stage=device-b-frame-enter')) {
                    $d3dVsMultiBStartFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains('stage=device-b-frame-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVsMultiBEndFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains('stage=device-a-return-enter')) {
                    $d3dVsMultiReturnStartFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains('stage=device-a-return-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVsMultiReturnEndFrame = Get-LatestDumpFrameSequence
                } elseif ($message.Contains(
                        'stage=device-b-survivor-enter')) {
                    $d3dVsMultiSurvivorStartFrame =
                        Get-LatestDumpFrameSequence
                } elseif ($message.Contains(
                        'stage=device-b-survivor-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVsMultiSurvivorEndFrame =
                        Get-LatestDumpFrameSequence
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3dVsSuccession -and
                $message.StartsWith('QXP_D3DVS_SUCCESSION_CHILD_RESULT ')) {
                $guestTestStarted = $true
                if ($message.Contains('leg=1 rc=0 ')) {
                    $d3dVsSuccessionLeg1Ok = $true
                } elseif ($message.Contains('leg=2 rc=0 ')) {
                    $d3dVsSuccessionLeg2Ok = $true
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3dVsSuccession -and
                $message.StartsWith('QXP_D3DVS_SUCCESSION_RESULT ')) {
                $guestTestStarted = $true
                $result = $message
                break
            }
            if ($D3dVs11 -and
                $message.StartsWith('QXP_D3DVS_RESULT ')) {
                $guestTestStarted = $true
                $result = $message
                break
            }
            if ($D3dVsMode -and
                (($D3dVs11 -and
                  $message.StartsWith('QXP_D3DVS_STAGE ')) -or
                 ($D3dVsSuccession -and
                  $message.StartsWith('QXP_D3DVS_SUCCESSION_STAGE ')))) {
                $guestTestStarted = $true
                $scoreVsStage = -not $D3dVsSuccession -or
                    $message.Contains('leg=2 ')
                if ($scoreVsStage -and
                    $message.Contains('stage=clip-frame-enter')) {
                    $d3dVs11ClipStartFrame = Get-LatestDumpFrameSequence
                } elseif ($scoreVsStage -and
                          $message.Contains('stage=clip-frame-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVs11ClipEndFrame = Get-LatestDumpFrameSequence
                } elseif ($scoreVsStage -and
                          $message.Contains('stage=fast-frame-enter')) {
                    $d3dVs11FastStartFrame = Get-LatestDumpFrameSequence
                } elseif ($scoreVsStage -and
                          $message.Contains('stage=fast-frame-leave')) {
                    Start-Sleep -Milliseconds 250
                    $d3dVs11FastEndFrame = Get-LatestDumpFrameSequence
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3dProbe -and $message.StartsWith('QXP_D3D_STAGE ')) {
                $guestTestStarted = $true
                if ($D3dHwtlOnly -and
                    $message.Contains('stage=hwtl-reference-enter')) {
                    $d3dHwtlStartFrame = Get-LatestDumpFrameSequence
                }
                if ($D3dHwtlOnly -and
                    $message.Contains('stage=hwtl-reference-leave')) {
                    # The guest reports leave after the held Present. Allow
                    # the asynchronous dump writer to publish the last copy.
                    # Poll a bounded interval instead of assuming one host's
                    # fixed readback latency, then cap focused scoring to this
                    # exact scene interval before the guest changes mode.
                    $hwtlFrameDeadline = (Get-Date).AddSeconds(2)
                    do {
                        Start-Sleep -Milliseconds 100
                        $latestHwtlFrame = Get-LatestDumpFrameSequence
                    } while ($latestHwtlFrame -le $d3dHwtlStartFrame -and
                             (Get-Date) -lt $hwtlFrameDeadline)
                    $d3dHwtlEndFrame = if ($latestHwtlFrame -gt
                        $d3dHwtlStartFrame) {
                        $latestHwtlFrame
                    } else {
                        # Some hosts finish publishing the held Present only
                        # after the serial leave marker. The lower bound still
                        # rejects boot frames; leave the upper bound open.
                        -1
                    }
                }
                if ($D3dHardware -and
                    $message.Contains('stage=vertex-processing hardware')) {
                    $hardwareVpOk = $true
                }
                if ($D3dVisual -and $message.Contains(
                        'hr=0x00000000 stage=state-block-leave render-texture-transform-material-light-clip')) {
                    $customStateBlockOk = $true
                    $stateBlockOk = $customStateBlockOk -and
                        $predefinedStateBlockOk
                }
                if ($D3dVisual -and $message.Contains(
                        'hr=0x00000000 stage=predefined-state-block-leave pixel-vertex-all-membership')) {
                    $predefinedStateBlockOk = $true
                    $stateBlockOk = $customStateBlockOk -and
                        $predefinedStateBlockOk
                }
                if ($D3dCopyVisual -and
                    $message.Contains('stage=copy-present-leave') -and
                    $message.Contains('frame=rect-clear')) {
                    # Present has completed synchronously by the time the
                    # guest emits this stage. Snapshot the sequence now: a
                    # later device/context release can repaint the client
                    # area with stale system-memory pixels that are outside
                    # the COPY-preservation interval under test.
                    $d3dCopyEndFrame = Get-LatestDumpFrameSequence
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3d9 -and $message.StartsWith('QXP_D3D9_RESULT ')) {
                if ($D3d9Multistream7) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Result
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                $result = $message
                break
            }
            if ($D3d9SelfTest -and
                $message.StartsWith('QXP_D3D9_SELFTEST_RESULT ')) {
                $result = $message
                break
            }
            if ($D3d9SelfTest -and
                $message.StartsWith('QXP_D3D9_SELFTEST_STAGE ')) {
                $guestTestStarted = $true
                Write-Output "GuestStage=$message"
            }
            if ($D3d9 -and $message.StartsWith('QXP_D3D9_STAGE ')) {
                $guestTestStarted = $true
                if ($message.Contains('stage=frame-capture-wait') -and
                    $d3d9SceneStartFrame -lt 0) {
                    $d3d9SceneStartFrame = Get-LatestDumpFrameSequence
                    $d3d9SceneEndFrame = $d3d9SceneStartFrame
                    if (Invoke-GuestKey -Port $QmpPort -Key 'f12' `
                            -HoldMilliseconds 2000) {
                        $d3d9SceneCaptureDispatchOk = $true
                    }
                    $d3d9NextSceneCaptureDispatch =
                        (Get-Date).AddSeconds(3)
                } elseif ($message.Contains('stage=frame-capture-ack')) {
                    $d3d9SceneCaptureAck = $true
                    if ($D3d9Multistream7) {
                        $transition = Update-XpD3d9MultistreamOrder `
                            -CurrentStage $d3d9MultistreamStage `
                            -Event SceneCaptureAck
                        if ($transition.ok) {
                            $d3d9MultistreamStage = $transition.stage
                        } else {
                            $d3d9MultistreamOrderOk = $false
                        }
                    }
                } elseif ($message.Contains('stage=reset-capture-wait') -and
                          $d3d9ResetStartFrame -lt 0) {
                    $d3d9ResetStartFrame = Get-LatestDumpFrameSequence
                    $d3d9ResetEndFrame = $d3d9ResetStartFrame
                    if (Invoke-GuestKey -Port $QmpPort -Key 'f11' `
                            -HoldMilliseconds 2000) {
                        $d3d9ResetCaptureDispatchOk = $true
                    }
                    $d3d9NextResetCaptureDispatch =
                        (Get-Date).AddSeconds(3)
                } elseif ($message.Contains('stage=reset-capture-ack')) {
                    $d3d9ResetCaptureAck = $true
                    if ($D3d9Multistream7) {
                        $transition = Update-XpD3d9MultistreamOrder `
                            -CurrentStage $d3d9MultistreamStage `
                            -Event ResetCaptureAck
                        if ($transition.ok) {
                            $d3d9MultistreamStage = $transition.stage
                        } else {
                            $d3d9MultistreamOrderOk = $false
                        }
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=multistream7-ready') -and
                    -not $message.Contains('skipped')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Ready
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                        $d3d9MultistreamReady = $true
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=multistream7-draw') -and
                    $message.Contains('stream0+7-draw=1')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Draw
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                        $d3d9MultistreamDraw = $true
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=clear-draw-present')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Present
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=multistream7-release') -and
                    $message.Contains('resources-released')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Release
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                        $d3d9MultistreamDrawReleased = $true
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=multistream7-lifetime') -and
                    $message.Contains('released-before-reset')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Lifetime
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                        $d3d9MultistreamLifetime = $true
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                if ($D3d9Multistream7 -and
                    $message.Contains('hr=0x00000000 stage=reset ')) {
                    $transition = Update-XpD3d9MultistreamOrder `
                        -CurrentStage $d3d9MultistreamStage -Event Reset
                    if ($transition.ok) {
                        $d3d9MultistreamStage = $transition.stage
                    } else {
                        $d3d9MultistreamOrderOk = $false
                    }
                }
                Write-Output "GuestStage=$message"
            }
            if ($D3dClean -and $message.StartsWith('QXP_D3D_CLEAN ')) {
                $result = $message
                break
            }
            if ($D3dGame -and $message.StartsWith('QXP_D3D_GAME_RESULT ')) {
                $d3dGameResultAt = Get-Date
                if ($D3dGameplay -and -not $D3dPerformance -and
                    (Test-XpMaxPayneSuccessfulResult -Message $message) -and
                    $maxPayneVisualBoundary.started -and
                    -not $maxPayneVisualBoundary.completed) {
                    Start-Sleep -Milliseconds 750
                    [void](Set-XpMaxPayneVisualCompletionBoundary `
                        -State $maxPayneVisualBoundary `
                        -At $d3dGameResultAt `
                        -Sequence (Get-LatestDumpFrameSequence))
                    $d3dGameObservationAt =
                        $maxPayneVisualBoundary.completedAt
                    $d3dGameEndFrame =
                        $maxPayneVisualBoundary.endSequence
                }
                if ($frameProfileStopEvent) {
                    Stop-XpPerformanceProfileWindow
                }
                $result = $message
                break
            }
            if ($D3dGame -and $message.StartsWith('QXP_D3D_GAME_STAGE ')) {
                $guestTestStarted = $true
                if ($message.Contains('stage=play-started') -and
                    -not $D3dGameplay) {
                    $d3dGamePlayAt = Get-Date
                    if ($frameProfileStartEvent) {
                        Start-XpPerformanceProfileWindow
                    }
                }
                if ($message.Contains('stage=scene-started') -and
                    $D3dGameplay) {
                    if ($D3dPerformance) {
                        # Keep the established capture-free timing path.
                        $d3dGamePlayAt = Get-Date
                    } elseif (-not $maxPayneVisualBoundary.started) {
                        [void](Set-XpMaxPayneVisualStartBoundary `
                            -State $maxPayneVisualBoundary -At (Get-Date) `
                            -Sequence (Get-LatestDumpFrameSequence))
                        $d3dGamePlayAt = $maxPayneVisualBoundary.startedAt
                        $d3dGameStartFrame =
                            $maxPayneVisualBoundary.startSequence
                    }
                    if ($frameProfileStartEvent) {
                        Start-XpPerformanceProfileWindow
                    }
                }
                if ($message.Contains('stage=observation-complete')) {
                    if ($D3dGameplay -and -not $D3dPerformance) {
                        if ($maxPayneVisualBoundary.started -and
                            -not $maxPayneVisualBoundary.completed) {
                            $completionAt = Get-Date
                            # The dump writer is asynchronous. Include its
                            # final bounded scene frame before latching once.
                            Start-Sleep -Milliseconds 750
                            [void](Set-XpMaxPayneVisualCompletionBoundary `
                                -State $maxPayneVisualBoundary `
                                -At $completionAt `
                                -Sequence (Get-LatestDumpFrameSequence))
                            $d3dGameObservationAt =
                                $maxPayneVisualBoundary.completedAt
                            $d3dGameEndFrame =
                                $maxPayneVisualBoundary.endSequence
                        }
                    } else {
                        $d3dGameObservationAt = Get-Date
                    }
                    if ($frameProfileStopEvent) {
                        Stop-XpPerformanceProfileWindow
                    }
                }
                if ($message.Contains('stage=activate action=escape')) {
                    [void](Invoke-GuestKey -Port $QmpPort -Key 'esc')
                } elseif ($message.Contains('stage=activate action=enter')) {
                    [void](Invoke-GuestKey -Port $QmpPort -Key 'ret')
                }
                Write-Output "GuestStage=$message"
            }
            if ($GameObserve -and
                $message.StartsWith('QXP_GAME_OBSERVE_RESULT ')) {
                $firstUtResult = $UT2003VisualOracle -and
                    $ut2003VisualResultMessages.Count -eq 0
                if ($UT2003VisualOracle) {
                    [void](Add-XpGameObserveUniqueMessage `
                        -Messages $ut2003VisualResultMessages `
                        -Message $message)
                    if (-not $firstUtResult) { continue }
                }
                $firstWarcraftResult = $Warcraft3D3D8Oracle -and
                    $warcraft3D3D8ResultMessages.Count -eq 0
                if ($Warcraft3D3D8Oracle) {
                    [void](Add-XpWarcraft3D3D8UniqueMessage `
                        -Messages $warcraft3D3D8ResultMessages `
                        -Message $message)
                    if (-not $firstWarcraftResult) {
                        # The helper deliberately retransmits one terminal
                        # record. Drain exact copies, but retain every distinct
                        # record so the contract can reject conflicting results.
                        continue
                    }
                }
                if ($gameObserveCompletedAt -eq [DateTime]::MaxValue) {
                    $gameObserveCompletedAt = Get-Date
                }
                if ($frameProfileStopEvent) {
                    Stop-XpPerformanceProfileWindow
                }
                if (-not $ObservePerformance) {
                    # Dump-all frames are written by the host asynchronously.
                    # Give the final observed frame a short opportunity to
                    # appear before recording the inclusive end of the
                    # observation interval.
                    Start-Sleep -Milliseconds 750
                    $latestSequence = Get-LatestDumpFrameSequence
                    if ($latestSequence -ge 0) {
                        $gameObserveEndFrame = $latestSequence
                    }
                }
                $result = $message
                if ($Warcraft3D3D8Oracle) {
                    $guestTestStarted = $true
                    $nextLaunch = [DateTime]::MaxValue
                    $warcraft3D3D8ResultDrainUntil =
                        (Get-Date).AddMilliseconds(1000)
                } elseif ($UT2003VisualOracle) {
                    $guestTestStarted = $true
                    $nextLaunch = [DateTime]::MaxValue
                    $ut2003VisualResultDrainUntil =
                        (Get-Date).AddMilliseconds(1000)
                } else {
                    break
                }
            }
            if ($GameObserve -and
                $message.StartsWith('QXP_GAME_OBSERVE_STAGE ')) {
                $guestTestStarted = $true
                if ($Warcraft3D3D8Oracle -and
                    $message.Contains('stage=oracle-start ')) {
                    $newOracleStart = Add-XpWarcraft3D3D8UniqueMessage `
                        -Messages $warcraft3D3D8OracleStartMessages `
                        -Message $message
                    if ($newOracleStart -and
                        $warcraft3D3D8OracleStartMessages.Count -eq 1) {
                        $warcraft3D3D8OracleStartedAt = Get-Date
                    }
                }
                if ($Warcraft3D3D8Oracle -and
                    $message.Contains('stage=oracle-complete ')) {
                    $newOracleComplete = Add-XpWarcraft3D3D8UniqueMessage `
                        -Messages $warcraft3D3D8OracleCompleteMessages `
                        -Message $message
                    if ($newOracleComplete -and
                        $warcraft3D3D8OracleCompleteMessages.Count -eq 1) {
                        $warcraft3D3D8OracleCompletedAt = Get-Date
                        $ringNotBeforeUtc = if (
                            $warcraft3D3D8OracleStartedAt -ne
                                [DateTime]::MinValue) {
                            $warcraft3D3D8OracleStartedAt.ToUniversalTime().AddSeconds(-1)
                        } else { [DateTime]::MinValue }
                        $warcraft3D3D8RingSnapshot =
                            Enter-XpWarcraft3D3D8RingSnapshot `
                                -SourceDirectory $vmDir `
                                -DestinationDirectory `
                                    $warcraft3D3D8RingDirectory `
                                -NotBeforeUtc $ringNotBeforeUtc
                        $warcraft3D3D8RingFrames = @(
                            $warcraft3D3D8RingSnapshot.frames)
                        Write-Output "Warcraft3D3D8FrameRingSnapshot frames=$($warcraft3D3D8RingSnapshot.frameCount) boundary=$($warcraft3D3D8OracleCompletedAt.ToString('o')) quiesced=True"
                    }
                }
                if ($UT2003VisualOracle -and
                    $message.Contains(
                        'stage=visual-ready ' +
                        'boundary=foreground-d3d8-window-stable ')) {
                    $newUtReady = Add-XpGameObserveUniqueMessage `
                        -Messages $ut2003VisualReadyMessages `
                        -Message $message
                    if ($newUtReady -and
                        $ut2003VisualReadyMessages.Count -eq 1) {
                        # Start only after the ordinary title has maintained
                        # its foreground System32-D3D8 window through warmup.
                        # Manifest-bound menu-layout classification below,
                        # not this liveness boundary, proves the main menu and
                        # excludes NvidiaLogo or other rich splash frames.
                        $gameObserveStartedAt = Get-Date
                        $ut2003VisualReadyFrame =
                            Get-LatestDumpFrameSequence
                        # UT uses synchronous producer readback, but one
                        # callback can be inside retro_host's inline writer
                        # while another frame occupies the frontend pending
                        # slot. Quarantine both ambiguous callbacks.
                        $ut2003GuardFrame =
                            [Math]::Max(0, $ut2003VisualReadyFrame) + 2
                        $gameObserveStartFrame = $ut2003GuardFrame
                    }
                }
                if ($NFSU2VisualOracle -and
                    -not $nfsu2VisualReadySeen -and
                    $message.Contains(
                        'stage=visual-ready ' +
                        'boundary=foreground-d3d9-window-stable ')) {
                    $nfsu2VisualReadySeen = $true
                    $gameObserveStartedAt = Get-Date
                    # The normal asynchronous readback stream may still
                    # publish the two callbacks straddling this guest event.
                    $gameObserveStartFrame =
                        [Math]::Max(0, (Get-LatestDumpFrameSequence)) + 2
                    Write-Output "NFSU2VisualBoundary valid=True boundary=foreground-d3d9-window-stable messages=1"
                }
                if ($UT2003VisualOracle -and
                    $message.Contains('stage=visual-complete ')) {
                    [void](Add-XpGameObserveUniqueMessage `
                        -Messages $ut2003VisualStageMessages `
                        -Message $message)
                }
                # UDP can lose the initial stage=started datagram.  Anchor the
                # capture interval to the first observer stage we actually
                # receive so boot frames cannot win visual selection.
                $performanceMeasurementStart = $ObservePerformance -and
                    $message.Contains('stage=measurement-start')
                if ($gameObserveStartedAt -eq [DateTime]::MinValue -and
                    (-not $ObservePerformance -or
                     $performanceMeasurementStart)) {
                    $gameObserveStartedAt = Get-Date
                    if ($frameProfileStartEvent) {
                        Start-XpPerformanceProfileWindow
                    }
                    if (-not $ObservePerformance) {
                        $gameObserveStartFrame = Get-LatestDumpFrameSequence
                    }
                }
                $performanceMeasurementComplete = $ObservePerformance -and
                    $message.Contains('stage=measurement-complete')
                if ($performanceMeasurementComplete -or
                    (-not $ObservePerformance -and
                     $message.Contains('stage=observation-complete'))) {
                    $gameObserveCompletedAt = Get-Date
                    if ($frameProfileStopEvent) {
                        Stop-XpPerformanceProfileWindow
                    }
                    if (-not $ObservePerformance) {
                        $latestSequence = Get-LatestDumpFrameSequence
                        if ($latestSequence -ge 0) {
                            $gameObserveEndFrame = $latestSequence
                        }
                    }
                }
                if ($message.Contains('stage=input action=fire')) {
                    $inputOk = Invoke-GuestMouseClick -Port $QmpPort
                    Write-Output "GuestInput=mouse-left ok=$inputOk"
                    if (-not $gameObserveInteractionStarted) {
                        $gameObserveInteractionStarted = $true
                        $gameObserveStartedAt = Get-Date
                        if ($frameProfileStartEvent) {
                            Start-XpPerformanceProfileWindow
                        }
                        if (-not $ObservePerformance) {
                            $gameObserveStartFrame =
                                Get-LatestDumpFrameSequence
                        }
                    }
                }
                Write-Output "GuestStage=$message"
            }
            if ($Xpdm -and $message.StartsWith('QXPSETUP ')) {
                $installerStarted = $true
                Write-Output "GuestStage=$message"
                Add-Content -Path $stageLog -Value $message
                if ($message -match
                    '^QXPSETUP identity driverVer=([^\s]+) signerThumbprint=([0-9A-Fa-f]{40})$') {
                    $reportedDriverVer = $Matches[1]
                    $reportedSignerThumbprint = $Matches[2].ToUpperInvariant()
                    if (($installDriverVer -and
                         $installDriverVer -ne $reportedDriverVer) -or
                        ($installSignerThumbprint -and
                         $installSignerThumbprint -ne
                            $reportedSignerThumbprint)) {
                        $installIdentityConflict = $true
                    }
                    $installDriverVer = $reportedDriverVer
                    $installSignerThumbprint = $reportedSignerThumbprint
                }
                if ($message -eq 'QXPSETUP stage=poweroff-ready' -or
                    $message -eq 'QXPSETUP stage=rebooting') {
                    if ($InstallOnly) {
                        $installReadyAt = (Get-Date).AddSeconds(
                            $(if ($message.EndsWith('poweroff-ready')) { 2 } else { 15 }))
                    } else {
                        $rebooting = $true
                        $nextLogin = (Get-Date).AddSeconds(35)
                    }
                }
            }
        }
        if ($InstallOnly -and (Get-Date) -ge $installReadyAt) {
            $result = 'QXPICD_INSTALL_READY'
            break
        }
        if ($GamePerformance -and $gamePerformanceResult -and
            $gamePlayAt -eq [DateTime]::MinValue -and
            (Get-Date) -ge $gamePerformanceWarmupAt) {
            $gamePlayAt = Get-Date
            $gamePerformanceStopAt = $gamePlayAt.AddSeconds(45)
            if ($frameProfileStartEvent) {
                Start-XpPerformanceProfileWindow
            }
            Write-Output 'GamePerformanceStage game=Quake2 stage=measurement-start warmupSeconds=15 clock=host-wall'
        }
        if ($GamePerformance -and
            $gamePlayAt -gt [DateTime]::MinValue -and
            (Get-Date) -ge $gamePerformanceStopAt) {
            $gameObservationAt = Get-Date
            if ($frameProfileStopEvent) {
                Stop-XpPerformanceProfileWindow
            }
            Write-Output 'GamePerformanceStage game=Quake2 stage=measurement-complete observedSeconds=45 clock=host-wall'
            $result = $gamePerformanceResult
            break
        }
        if (($Warcraft3D3D8Oracle -and $result -and
             (Get-Date) -ge $warcraft3D3D8ResultDrainUntil) -or
            ($UT2003VisualOracle -and $result -and
             (Get-Date) -ge $ut2003VisualResultDrainUntil)) {
            break
        }
        if ($rebooting -and (Get-Date) -ge $nextLogin) {
            [void](Invoke-GuestLogin -Port $QmpPort)
            $nextLogin = (Get-Date).AddSeconds(20)
        }
        if ((Get-Date) -ge $nextLaunch -and
            -not ($Xpdm -and $installerStarted) -and
            -not (($D3dAnyProbe -or $D3dGame -or $GameObserve -or
                  $GamePerformance) -and
                  $guestTestStarted)) {
            if ($firstGuestLaunchAt -eq [DateTime]::MinValue) {
                $firstGuestLaunchAt = Get-Date
                if (($ObservePerformance -or $GamePerformance) -and
                    (Test-Path -LiteralPath $stderr -PathType Leaf)) {
                    $profilePrefix = Get-Content -Raw -LiteralPath $stderr
                    $gameProfileTextStart = if ($profilePrefix) {
                        $profilePrefix.Length
                    } else { 0 }
                    Write-Output "GameObserveHostProfileBoundary characters=$gameProfileTextStart"
                }
            }
            [void](Invoke-GuestLaunch -Port $QmpPort)
            $nextLaunch = (Get-Date).AddSeconds($launchRetrySeconds)
        }
        if ($D3d9 -and $d3d9SceneStartFrame -ge 0 -and
            -not $d3d9SceneCaptureAck -and
            (Get-Date) -ge $d3d9NextSceneCaptureDispatch) {
            if (Invoke-GuestKey -Port $QmpPort -Key 'f12' `
                    -HoldMilliseconds 2000) {
                $d3d9SceneCaptureDispatchOk = $true
            }
            $d3d9NextSceneCaptureDispatch = (Get-Date).AddSeconds(3)
        }
        if ($D3d9 -and $d3d9ResetStartFrame -ge 0 -and
            -not $d3d9ResetCaptureAck -and
            (Get-Date) -ge $d3d9NextResetCaptureDispatch) {
            if (Invoke-GuestKey -Port $QmpPort -Key 'f11' `
                    -HoldMilliseconds 2000) {
                $d3d9ResetCaptureDispatchOk = $true
            }
            $d3d9NextResetCaptureDispatch = (Get-Date).AddSeconds(3)
        }
        $progressNow = Get-Date
        if ($progressNow -ge $nextProgressAt) {
            $elapsedSeconds = [int][Math]::Floor(
                ($progressNow - $runStartedAt).TotalSeconds)
            $remainingSeconds = [int][Math]::Max(
                0, [Math]::Ceiling(($deadline - $progressNow).TotalSeconds))
            Write-Output "HarnessProgress runId=$RunId elapsedSeconds=$elapsedSeconds remainingSeconds=$remainingSeconds hostPid=$($process.Id) qmpPort=$QmpPort guestTestStarted=$guestTestStarted lastGuestMessage=$lastGuestMessage"
            ++$progressRecordCount
            $nextProgressAt = $progressNow.AddSeconds(
                $ProgressIntervalSeconds)
        }
        Start-Sleep -Milliseconds 50
        $process.Refresh()
    }
} finally {
    if ($serialStream) { $serialStream.Dispose() }
    if ($udp) { $udp.Dispose() }
}

if ($result) { $resultObservedAt = Get-Date }
$shutdownStartedAt = Get-Date
$qmpStatus = $null
$powerdownRequested = $false
if ($result -or $Xpdm) {
    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $QmpPort)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 3000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        [void]$reader.ReadLine()
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        [void]$reader.ReadLine()
        if ($D3dAnyProbe -and -not $result) {
            $capture = @{
                execute = 'screendump'
                arguments = @{ filename = [IO.Path]::GetFileName($d3dCrashImage) }
            } | ConvertTo-Json -Compress -Depth 4
            $writer.WriteLine($capture)
            [void]$reader.ReadLine()
        }
        $writer.WriteLine('{"execute":"query-status"}')
        $qmpStatus = $reader.ReadLine()
        if ($Xpdm) {
            $writer.WriteLine('{"execute":"system_powerdown"}')
            $powerdownRequested = $true
        } else {
            $writer.WriteLine('{"execute":"quit"}')
        }
        [void]$reader.ReadLine()
        $writer.Dispose()
        $reader.Dispose()
        $client.Dispose()
    } catch {
        Write-Warning "QMP status/capture/shutdown failed: $_"
    }
}

if (-not $process.HasExited) {
    Wait-Process -Id $process.Id -Timeout 45 -ErrorAction SilentlyContinue
    $process.Refresh()
}
if (-not $process.HasExited -and $powerdownRequested) {
    try {
        $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $QmpPort)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 3000
        $reader = [IO.StreamReader]::new($stream)
        $writer = [IO.StreamWriter]::new($stream)
        $writer.AutoFlush = $true
        [void]$reader.ReadLine()
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        [void]$reader.ReadLine()
        $writer.WriteLine('{"execute":"quit"}')
        [void]$reader.ReadLine()
        $writer.Dispose()
        $reader.Dispose()
        $client.Dispose()
    } catch {
        Write-Warning "QMP quit fallback failed: $_"
    }
    # A successful quit can tear down QMP before its reply reaches this reader,
    # and a busy dump callback can delay that reply past ReadTimeout. Always
    # give the frontend the same bounded graceful-exit window after sending the
    # fallback command, even when reading the acknowledgement failed.
    if (-not $process.HasExited) {
        Wait-Process -Id $process.Id -Timeout 20 -ErrorAction SilentlyContinue
        $process.Refresh()
    }
}
if (-not $process.HasExited) {
    Stop-Process -Id $process.Id
}
$shutdownCompletedAt = Get-Date
if ($frameProfileStartEvent) {
    $frameProfileStartEvent.Dispose()
}
if ($frameProfileStopEvent) {
    $frameProfileStopEvent.Dispose()
}
$teardown = (Test-Path $stdout) -and
            [bool](Select-String -Path $stdout -SimpleMatch 'TEARDOWN_OK' -Quiet)
$swapLimitFailure = (Test-Path $stderr) -and [bool](
    Select-String -Path $stderr -SimpleMatch 'Guest GL Swap limit' -Quiet)
$hostProfileText = if (($D3dPerformance -or $D3dBenchmark -or
                        $D3dVsBenchmark -or $ObservePerformance -or
                        $GamePerformance) -and
                       (Test-Path -LiteralPath $stderr)) {
    Get-Content -LiteralPath $stderr -Raw
} else { '' }
$performanceFrameProfileOk = -not ($ObservePerformance -or $D3dPerformance -or
                                    $GamePerformance)
if (($ObservePerformance -or $D3dPerformance -or $GamePerformance) -and
    $hostProfileText) {
    $frameProfileWorkload = if ($ObservePerformance) {
        $ObserveGame
    } elseif ($GamePerformance) {
        'Quake2'
    } else {
        'MaxPayne'
    }
    $frameProfile = [regex]::Match($hostProfileText,
        'FrameProfile durationUs=(\d+) callbacks=(\d+) fresh=(\d+) ' +
        'duped=(\d+) freshFpsMilli=(\d+) callbackFpsMilli=(\d+) ' +
        'intervals=(\d+) overflow=(\d+) p50Us=(\d+) p95Us=(\d+) ' +
        'p99Us=(\d+) maxUs=(\d+) stalls50ms=(\d+) ' +
        'stalls100ms=(\d+) stalls250ms=(\d+) stalls1000ms=(\d+)')
    if ($frameProfile.Success) {
        $frameDurationUs = [long]$frameProfile.Groups[1].Value
        $frameCallbacks = [long]$frameProfile.Groups[2].Value
        $freshFrames = [long]$frameProfile.Groups[3].Value
        $dupedFrames = [long]$frameProfile.Groups[4].Value
        $frameIntervals = [long]$frameProfile.Groups[7].Value
        $frameIntervalOverflow = [long]$frameProfile.Groups[8].Value
        $performanceFrameProfileOk =
            $frameDurationUs -gt 0 -and $freshFrames -gt 1 -and
            $frameCallbacks -eq $freshFrames + $dupedFrames -and
            $frameIntervals -eq $freshFrames - 1 -and
            $frameIntervalOverflow -eq 0
        $freshFps = [Math]::Round(
            [long]$frameProfile.Groups[5].Value / 1000.0, 3)
        $callbackFps = [Math]::Round(
            [long]$frameProfile.Groups[6].Value / 1000.0, 3)
        $dupedPercent = if ($frameCallbacks) {
            [Math]::Round(100.0 * $dupedFrames / $frameCallbacks, 2)
        } else { 0 }
        Write-Output "FramePublicationProfile workload=$frameProfileWorkload durationUs=$frameDurationUs callbacks=$frameCallbacks fresh=$freshFrames duped=$dupedFrames dupedPercent=$dupedPercent freshFps=$freshFps callbackFps=$callbackFps intervals=$frameIntervals p50Us=$($frameProfile.Groups[9].Value) p95Us=$($frameProfile.Groups[10].Value) p99Us=$($frameProfile.Groups[11].Value) maxUs=$($frameProfile.Groups[12].Value) stalls50ms=$($frameProfile.Groups[13].Value) stalls100ms=$($frameProfile.Groups[14].Value) stalls250ms=$($frameProfile.Groups[15].Value) stalls1000ms=$($frameProfile.Groups[16].Value) valid=$performanceFrameProfileOk"
    } else {
        Write-Output "FramePublicationProfile unavailable workload=$frameProfileWorkload"
    }
}
if ($ObservePerformance -or $D3dPerformance -or $GamePerformance) {
    $hostProcessProfile = Complete-XpHostProcessCpuProfileState `
        -State $hostProcessProfileState
    if ($hostProcessProfile -and $hostProcessProfile.valid) {
        $topHostThread = @($hostProcessProfile.threads.top |
            Select-Object -First 1)
        $topThreadFields = if ($topHostThread.Count) {
            " topThreadId=$($topHostThread[0].id)" +
            " topThreadCpuSeconds=$($topHostThread[0].totalCpuSeconds)" +
            " topThreadSharePercent=$($topHostThread[0].shareOfProcessCpuPercent)"
        } else { '' }
        Write-Output "HostProcessProfile status=$($hostProcessProfile.status) wallSeconds=$($hostProcessProfile.window.wallSeconds) cpuSeconds=$($hostProcessProfile.process.totalCpuSeconds) userSeconds=$($hostProcessProfile.process.userCpuSeconds) kernelSeconds=$($hostProcessProfile.process.kernelCpuSeconds) averageUtilizedCores=$($hostProcessProfile.process.averageUtilizedCores) hostCapacityPercent=$($hostProcessProfile.process.hostCapacityPercent) stableThreads=$($hostProcessProfile.threads.stableCount) stableCoveragePercent=$($hostProcessProfile.threads.stableCoveragePercent)$topThreadFields"
    } elseif ($hostProcessProfile) {
        Write-Output "HostProcessProfile unavailable status=$($hostProcessProfile.status) findings=$(@($hostProcessProfile.findings).Count)"
    } else {
        Write-Output 'HostProcessProfile unavailable status=not-captured findings=0'
    }
}
if (($D3dPerformance -or $D3dBenchmark) -and $hostProfileText) {
    # The native-ICD preflight and the Direct3D workload create separate host
    # GL sessions, each with its own DoorbellProfile block. Restrict parsing to
    # the QXPD3D session so the preflight profile cannot be reported as the
    # benchmark/game result merely because it appears first in the log.
    $d3dProfileMarker = $hostProfileText.LastIndexOf('QXPD3D/')
    $hostProfileScope = if ($d3dProfileMarker -ge 0) {
        $hostProfileText.Substring($d3dProfileMarker)
    } else {
        ''
    }
    $publishProfile = [regex]::Match($hostProfileScope,
        'DoorbellProfile publish calls (\d+) total (\d+) us avg (\d+) us')
    $swapProfile = [regex]::Match($hostProfileScope,
        'DoorbellProfile swap calls (\d+) total (\d+) us avg (\d+) us')
    $directProfile = [regex]::Match($hostProfileScope,
        'DoorbellProfile direct calls (\d+) total (\d+) us')
    $topDirectProfile = [regex]::Match($hostProfileScope,
        'DoorbellProfile direct ([^\r\n ]+) calls (\d+) total (\d+) us avg (\d+) us')
    if ($publishProfile.Success -and $swapProfile.Success -and
        $directProfile.Success) {
        $topDirect = if ($topDirectProfile.Success) {
            " topDirect=$($topDirectProfile.Groups[1].Value) topCalls=$($topDirectProfile.Groups[2].Value) topTotalUs=$($topDirectProfile.Groups[3].Value) topAverageUs=$($topDirectProfile.Groups[4].Value)"
        } else { '' }
        Write-Output "D3dHostProfile publishCalls=$($publishProfile.Groups[1].Value) publishTotalUs=$($publishProfile.Groups[2].Value) publishAverageUs=$($publishProfile.Groups[3].Value) swapCalls=$($swapProfile.Groups[1].Value) swapTotalUs=$($swapProfile.Groups[2].Value) swapAverageUs=$($swapProfile.Groups[3].Value) directCalls=$($directProfile.Groups[1].Value) directTotalUs=$($directProfile.Groups[2].Value)$topDirect"
    } else {
        Write-Output 'D3dHostProfile unavailable d3d-profiler-session-not-present'
    }
}
$gameObserveHostProfileOk = -not ($ObservePerformance -or $GamePerformance)
if (($ObservePerformance -or $GamePerformance) -and $hostProfileText) {
    # Attribute only text appended after this run's first guest launch, which
    # excludes the completed boot-time native-ICD preflight. The observer
    # teardown can append an incomplete final accelerated session; selecting
    # the last complete session inside the launch-bounded suffix recovers the
    # measured game without ever walking back into preflight evidence. Native
    # OpenGL applications may still identify as QXPD3D when the installed XPDM
    # wrapper owns the session.
    $gameProfile = Get-XpLastCompleteDoorbellProfile `
        -Text $hostProfileText -StartIndex $gameProfileTextStart
    $gameProfileMarkerName = if ($gameProfile) {
        $gameProfile.markerName
    } else { '' }
    if ($gameProfile) {
        $gameObserveHostProfileOk = $true
        $gameTopDirect = if ($gameProfile.topDirect) {
            " topDirect=$($gameProfile.topDirect.name) topCalls=$($gameProfile.topDirect.calls) topTotalUs=$($gameProfile.topDirect.totalUs) topAverageUs=$($gameProfile.topDirect.averageUs)"
        } else { '' }
        $profileGame = if ($GamePerformance) { 'Quake2' } else { $ObserveGame }
        Write-Output "GameObserveHostProfile game=$profileGame session=$gameProfileMarkerName publishCalls=$($gameProfile.publishCalls) publishTotalUs=$($gameProfile.publishTotalUs) publishAverageUs=$($gameProfile.publishAverageUs) swapCalls=$($gameProfile.swapCalls) swapTotalUs=$($gameProfile.swapTotalUs) swapAverageUs=$($gameProfile.swapAverageUs) directCalls=$($gameProfile.directCalls) directTotalUs=$($gameProfile.directTotalUs)$gameTopDirect"
    } else {
        $profileGame = if ($GamePerformance) { 'Quake2' } else { $ObserveGame }
        Write-Output "GameObserveHostProfile unavailable game=$profileGame session=$gameProfileMarkerName"
    }
}
$d3dVsBenchmarkHostOk = -not $D3dVsBenchmark
if ($D3dVsBenchmark -and $hostProfileText) {
    $profileModes = @('fixed', 'shader', 'shader', 'fixed')
    $profileDirectCalls = @(2, 32, 32, 2)
    $profilePattern =
        '(?ms)QXPD3D/[^\r\n]*.*?' +
        'DoorbellProfile publish calls (\d+) total (\d+) us avg (\d+) us.*?' +
        'DoorbellProfile swap calls (\d+) total (\d+) us avg (\d+) us.*?' +
        'DoorbellProfile direct calls (\d+) total (\d+) us'
    $profileMatches = [regex]::Matches($hostProfileText, $profilePattern)
    $d3dVsBenchmarkHostOk = $profileMatches.Count -eq $profileModes.Count
    for ($profileIndex = 0;
         $profileIndex -lt [Math]::Min(
             $profileMatches.Count, $profileModes.Count);
        ++$profileIndex) {
        $profile = $profileMatches[$profileIndex]
        $d3dVsBenchmarkHostOk = $d3dVsBenchmarkHostOk -and
            [int]$profile.Groups[1].Value -eq 17 -and
            [int]$profile.Groups[2].Value -gt 0 -and
            [int]$profile.Groups[4].Value -eq 0 -and
            [int]$profile.Groups[7].Value -eq
                $profileDirectCalls[$profileIndex] -and
            [int]$profile.Groups[8].Value -gt 0
        Write-Output "D3dVsBenchmarkHost order=$($profileIndex + 1) mode=$($profileModes[$profileIndex]) publishCalls=$($profile.Groups[1].Value) publishTotalUs=$($profile.Groups[2].Value) publishAverageUs=$($profile.Groups[3].Value) swapCalls=$($profile.Groups[4].Value) swapTotalUs=$($profile.Groups[5].Value) swapAverageUs=$($profile.Groups[6].Value) directCalls=$($profile.Groups[7].Value) directTotalUs=$($profile.Groups[8].Value)"
    }
    Write-Output "D3dVsBenchmarkHostOk=$d3dVsBenchmarkHostOk sessions=$($profileMatches.Count) expected=$($profileModes.Count)"
}
$legacyIndexedDp2Ok = -not $RequireLegacyIndexedDp2
if ($RequireLegacyIndexedDp2) {
    $xpdmDebugLog = $debugconSource
    $legacyIndexedDp2Ok = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP LEGACY INDEXED TRANSLATION' -Quiet)
    Write-Output "LegacyIndexedDp2Ok=$legacyIndexedDp2Ok"
}
$ignoredStateChangeOk = -not $D3dVisual -or $D3dHwtlOnly
if ($D3dVisual -and -not $D3dHwtlOnly) {
    $debugText = if (Test-Path -LiteralPath $debugconSource) {
        Get-Content -LiteralPath $debugconSource -Raw
    } else {
        ''
    }
    $renderStateChanged = $debugText -match `
        '(?m)^QXPDISP D3D RSTATE CHANGED\r?\n0x00000010\r?$'
    $textureStateChanged = $debugText -match `
        '(?m)^QXPDISP D3D TSTATE CHANGED\r?\n0x00000000\r?\n0x0000000F\r?$'
    $ignoredStateChangeOk = $renderStateChanged -and $textureStateChanged
    Write-Output "IgnoredStateChangeOk=$ignoredStateChangeOk render=$renderStateChanged texture=$textureStateChanged"
}
$hwtlReuseOk = -not ($D3dBenchmark -and $D3dHardware)
if ($D3dBenchmark -and $D3dHardware) {
    $debugText = if (Test-Path -LiteralPath $debugconSource) {
        Get-Content -LiteralPath $debugconSource -Raw
    } else {
        ''
    }
    $reuseValues = @{}
    foreach ($counterName in @('FIXEDREUSE', 'ARRAYREUSE')) {
        $counterMatches = [regex]::Matches(
            $debugText,
            "(?m)^QXPMP D3D $counterName`r?`nQXPMP STATUS`r?`n0x([0-9A-Fa-f]{8})`r?$")
        if ($counterMatches.Count) {
            $reuseValues[$counterName] = [Convert]::ToUInt64(
                $counterMatches[$counterMatches.Count - 1].Groups[1].Value,
                16)
        }
    }
    $fixedReuse = if ($reuseValues.ContainsKey('FIXEDREUSE')) {
        $reuseValues['FIXEDREUSE']
    } else { 0 }
    $arrayReuse = if ($reuseValues.ContainsKey('ARRAYREUSE')) {
        $reuseValues['ARRAYREUSE']
    } else { 0 }
    $batchShape = [regex]::Match(
        [string]$result, 'transform-batch=(\d+) transform-frames=(\d+)')
    $expectedReuse = if ($batchShape.Success) {
        ([Convert]::ToUInt64($batchShape.Groups[1].Value) - 1) *
            [Convert]::ToUInt64($batchShape.Groups[2].Value)
    } else { 0 }
    $hwtlReuseOk = $expectedReuse -gt 0 -and
                   $fixedReuse -ge $expectedReuse -and
                   $arrayReuse -ge $expectedReuse
    Write-Output "D3dHwtlReuseOk=$hwtlReuseOk expected=$expectedReuse fixedStateReuse=$fixedReuse arrayStateReuse=$arrayReuse"
}
$d3dGameTransportOk = -not $D3dGame
if ($D3dGame) {
    $xpdmDebugLog = $debugconSource
    $hasContext = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPDISP D3C+' -Quiet)
    $hasHostSession = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPMP D3D OPEN+' -Quiet)
    $hasDraw = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D DRAW+' -Quiet)
    $hasPresent = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP FLIP PRESENT' -Quiet)
    $drawFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog `
            -Pattern 'QXPMP D3D DRAW .*-$' -Quiet)
    $dp2Failure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -Pattern `
            'QXPDISP DP2 (BAD|.*FAILED|CLEAR SHORT|UNKNOWN|SHORT)$' -Quiet)
    $surfaceFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP CSE TABLE-' -Quiet)
    $targetFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D TARGET-' -Quiet)
    $primitiveSkip = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP IDX PRIMITIVE SKIP' -Quiet)
    $debugText = if (Test-Path $xpdmDebugLog) {
        Get-Content -LiteralPath $xpdmDebugLog -Raw
    } else {
        ''
    }
    $counterValues = @{}
    foreach ($counterName in @(
            'DRAWS', 'PRESENTS', 'CLEARS', 'UPLOADS', 'BATCHES', 'BATCHMAX',
            'FIXEDREUSE', 'TEXREUSE', 'ARRAYREUSE')) {
        $counterMatches = [regex]::Matches(
            $debugText,
            "(?m)^QXPMP D3D $counterName`r?`nQXPMP STATUS`r?`n0x([0-9A-Fa-f]{8})`r?$")
        if ($counterMatches.Count) {
            $counterValues[$counterName] = [Convert]::ToUInt64(
                $counterMatches[$counterMatches.Count - 1].Groups[1].Value,
                16)
        }
    }
    $counterOk = $counterValues.ContainsKey('DRAWS') -and
                 $counterValues.ContainsKey('PRESENTS') -and
                 $counterValues.ContainsKey('BATCHES') -and
                 $counterValues.ContainsKey('BATCHMAX') -and
                 $counterValues['DRAWS'] -gt 0 -and
                 $counterValues['PRESENTS'] -gt 0 -and
                 $counterValues['BATCHES'] -gt 0 -and
                 $counterValues['BATCHMAX'] -gt 0
    $observedSeconds = if ($d3dGamePlayAt -gt [DateTime]::MinValue -and
                           $d3dGameObservationAt -lt [DateTime]::MaxValue) {
        ($d3dGameObservationAt - $d3dGamePlayAt).TotalSeconds
    } else {
        0
    }
    if ($counterOk -and $observedSeconds -gt 0) {
        $drawRate = [Math]::Round(
            $counterValues['DRAWS'] / $observedSeconds, 2)
        $presentRate = [Math]::Round(
            $counterValues['PRESENTS'] / $observedSeconds, 2)
        $drawsPerBatch = [Math]::Round(
            $counterValues['DRAWS'] / $counterValues['BATCHES'], 2)
        Write-Output "D3dGamePerformance observedSeconds=$([Math]::Round($observedSeconds, 2)) draws=$($counterValues['DRAWS']) presents=$($counterValues['PRESENTS']) clears=$($counterValues['CLEARS']) uploads=$($counterValues['UPLOADS']) batches=$($counterValues['BATCHES']) maxDrawsPerBatch=$($counterValues['BATCHMAX']) averageDrawsPerBatch=$drawsPerBatch fixedStateReuse=$($counterValues['FIXEDREUSE']) textureStageReuse=$($counterValues['TEXREUSE']) arrayStateReuse=$($counterValues['ARRAYREUSE']) drawsPerSecond=$drawRate transportPresentsPerSecond=$presentRate"
    } else {
        Write-Output "D3dGamePerformance unavailable counters=$counterOk observedSeconds=$observedSeconds"
    }
    $d3dGameTransportOk = $hasContext -and $hasHostSession -and $hasDraw -and
                          $hasPresent -and $counterOk -and -not $drawFailure -and
                          -not $dp2Failure -and -not $surfaceFailure -and
                          -not $targetFailure -and
                          -not $primitiveSkip -and -not $swapLimitFailure
    Write-Output "D3dGameTransportOk=$d3dGameTransportOk context=$hasContext host=$hasHostSession draw=$hasDraw present=$hasPresent drawFailure=$drawFailure dp2Failure=$dp2Failure surfaceFailure=$surfaceFailure targetFailure=$targetFailure primitiveSkip=$primitiveSkip swapLimitFailure=$swapLimitFailure"
}
$d3dVsBenchmarkTransportOk = -not $D3dVsBenchmark
if ($D3dVsBenchmark) {
    $xpdmDebugLog = $debugconSource
    $debugText = if (Test-Path -LiteralPath $xpdmDebugLog) {
        Get-Content -LiteralPath $xpdmDebugLog -Raw
    } else { '' }
    $phaseTokens = @(
        'order=1 mode=fixed ',
        'order=2 mode=shader ',
        'order=3 mode=shader ',
        'order=4 mode=fixed '
    )
    $timedApiDrawsPerPhase = 256
    $totalApiDrawsPerPhase = 272
    $measuredTransportDrawsPerPhase = 16
    $transportDrawsPerPhase = 17
    $apiDrawsPerTransport = 16
    $phaseOk = $d3dVsBenchmarkPhases.Count -eq $phaseTokens.Count
    for ($phaseIndex = 0;
         $phaseOk -and $phaseIndex -lt $phaseTokens.Count;
         ++$phaseIndex) {
        $phaseOk = $d3dVsBenchmarkPhases[$phaseIndex].Contains('rc=0 ') -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains(
                $phaseTokens[$phaseIndex]) -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains(
                'warmup-frames=1 ') -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains('frames=16 ') -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains(
                'draws-per-frame=16 ') -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains(
                "timed-api-draws=$timedApiDrawsPerPhase ") -and
            $d3dVsBenchmarkPhases[$phaseIndex].Contains(
                "total-api-draws=$totalApiDrawsPerPhase")
    }
    $resultShapeOk = $result -and $result.Contains('rounds=2 ') -and
        $result.Contains('order=fixed,shader,shader,fixed ') -and
        $result.Contains('warmup-frames=1 ') -and
        $result.Contains('frames-per-phase=16 ') -and
        $result.Contains('draws-per-frame=16 ') -and
        $result.Contains("timed-api-draws=$timedApiDrawsPerPhase ") -and
        $result.Contains("total-api-draws=$totalApiDrawsPerPhase ") -and
        $result.Contains('fixed-submit-ns=') -and
        $result.Contains('shader-submit-ns=') -and
        $result.Contains('shader-create-us=')
    $contextCount = [regex]::Matches(
        $debugText, '(?m)^QXPDISP D3C\+$').Count
    $hostOpenCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D OPEN\+$').Count
    $hostCloseCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D CLOSE\+$').Count
    $shaderDefineCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D SHADER DEFINE\+$').Count
    $displayDefineCount = [regex]::Matches(
        $debugText, '(?m)^QXPDISP VS DEFINE\+$').Count
    $clipDefineCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D CLIP GLSL DEFINE\+$').Count
    $shaderDeleteCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D SHADER DELETE\+$').Count
    $clipDeleteCount = [regex]::Matches(
        $debugText, '(?m)^QXPMP D3D CLIP GLSL DELETE\+$').Count
    $counterSeries = @{}
    foreach ($counterName in @('DRAWS', 'PRESENTS', 'BATCHES', 'BATCHMAX')) {
        $counterMatches = [regex]::Matches(
            $debugText,
            "(?m)^QXPMP D3D $counterName`r?`nQXPMP STATUS`r?`n0x([0-9A-Fa-f]{8})`r?$")
        $counterSeries[$counterName] = @($counterMatches | ForEach-Object {
            [Convert]::ToUInt64($_.Groups[1].Value, 16)
        })
    }
    # XP's stock D3D8 runtime coalesces the sixteen DrawPrimitiveUP calls in
    # each frame into one DP2/transport draw. Keep the public API-call count
    # and transport-request count as separate exact contracts; a transport
    # DRAWS value of 272 would mean batching regressed. Do not depend on the
    # miniport's optional verbose packet trace for this production gate.
    $drawPatternOk = $counterSeries.DRAWS.Count -eq 4 -and
        @($counterSeries.DRAWS | Where-Object {
            $_ -ne $transportDrawsPerPhase
        }).Count -eq 0
    $presentPatternOk = $counterSeries.PRESENTS.Count -eq 4 -and
        @($counterSeries.PRESENTS | Where-Object { $_ -ne 17 }).Count -eq 0
    $batchPatternOk = $counterSeries.BATCHES.Count -eq 4 -and
        @($counterSeries.BATCHES | Where-Object { $_ -ne 17 }).Count -eq 0 -and
        $counterSeries.BATCHMAX.Count -eq 4 -and
        @($counterSeries.BATCHMAX | Where-Object { $_ -ne 1 }).Count -eq 0
    $lifecycleOk = $contextCount -eq 4 -and $hostOpenCount -eq 4 -and
        $hostCloseCount -eq 4 -and $shaderDefineCount -eq 2 -and
        $displayDefineCount -eq 2 -and $clipDefineCount -eq 2 -and
        $shaderDeleteCount -eq 2 -and $clipDeleteCount -eq 2
    $drawFailure = $debugText -match '(?m)^QXPMP D3D DRAW .*-$'
    $dp2Failure = $debugText -match
        '(?m)^QXPDISP DP2 (BAD|.*FAILED|CLEAR SHORT|UNKNOWN|SHORT)$'
    $surfaceFailure = $debugText -match '(?m)^QXPDISP D3D SURFACE .*-$'
    $targetFailure = $debugText -match '(?m)^QXPMP D3D TARGET .*-$'
    $d3dVsBenchmarkTransportOk = $phaseOk -and $resultShapeOk -and
        $lifecycleOk -and $drawPatternOk -and $presentPatternOk -and
        $batchPatternOk -and $d3dVsBenchmarkHostOk -and
        -not $drawFailure -and -not $dp2Failure -and
        -not $surfaceFailure -and -not $targetFailure -and
        -not $swapLimitFailure
    Write-Output "D3dVsBenchmarkTransportOk=$d3dVsBenchmarkTransportOk phases=$phaseOk($($d3dVsBenchmarkPhases.Count)) resultShape=$resultShapeOk contexts=$contextCount opens=$hostOpenCount closes=$hostCloseCount shaderDefines=$shaderDefineCount/$displayDefineCount/$clipDefineCount shaderDeletes=$shaderDeleteCount/$clipDeleteCount timedApiDrawsPerPhase=$timedApiDrawsPerPhase totalApiDrawsPerPhase=$totalApiDrawsPerPhase measuredTransportDrawsPerPhase=$measuredTransportDrawsPerPhase apiDrawsPerTransport=$apiDrawsPerTransport transportDraws=$($counterSeries.DRAWS -join ',') presents=$($counterSeries.PRESENTS -join ',') batches=$($counterSeries.BATCHES -join ',') batchMax=$($counterSeries.BATCHMAX -join ',') hostProfiles=$d3dVsBenchmarkHostOk drawFailure=$drawFailure dp2Failure=$dp2Failure surfaceFailure=$surfaceFailure targetFailure=$targetFailure swapLimitFailure=$swapLimitFailure"
}
$d3dVs11TransportOk = -not $D3dVsMode
if ($D3dVsMode) {
    $xpdmDebugLog = $debugconSource
    $ownerFactor = if ($D3dVsSuccession) { 2 } else { 1 }
    $contextCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPDISP D3C+').Count
    } else { 0 }
    $hostSessionCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPMP D3D OPEN+').Count
    } else { 0 }
    $hasContext = if ($D3dVsMultiContext) {
        $contextCount -eq 2
    } else {
        $contextCount -ge (2 * $ownerFactor)
    }
    $hasHostSession = if ($D3dVsMultiContext) {
        $hostSessionCount -eq 1
    } else {
        $hostSessionCount -ge (2 * $ownerFactor)
    }
    $shaderDefineCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D SHADER DEFINE+').Count } else { 0 }
    $displayDefineCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP VS DEFINE+').Count } else { 0 }
    $clipDefineCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D CLIP GLSL DEFINE+').Count } else { 0 }
    $shaderDeleteCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D SHADER DELETE+').Count } else { 0 }
    $displayDeleteCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP VS DELETE+').Count } else { 0 }
    $clipDeleteCount = if (Test-Path $xpdmDebugLog) { @(
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D CLIP GLSL DELETE+').Count } else { 0 }
    $shaderDefinitionMinimum = if ($D3dVsMultiContext) {
        2
    } else {
        # The bytecode cache retains identical programs across public
        # delete/recreate operations.  The probe creates six public handles
        # per owner but exactly four unique shader+clip programs reach the
        # display/host boundary; require every unique program.
        4 * $ownerFactor
    }
    $displayDeleteMinimum = if ($D3dVsMultiContext) {
        2
    } else { 5 * $ownerFactor }
    $hasShaderDefine = if ($D3dVsMultiContext) {
        $shaderDefineCount -eq 2 -and $displayDefineCount -eq 2
    } else {
        $shaderDefineCount -ge $shaderDefinitionMinimum -and
        $displayDefineCount -ge $shaderDefinitionMinimum
    }
    $hasClipDefine = if ($D3dVsMultiContext) {
        $clipDefineCount -eq 2
    } else { $clipDefineCount -ge $shaderDefinitionMinimum }
    # XP can batch the final public DeleteVertexShader until context teardown.
    # Require every host program to disappear, while allowing that final
    # cleanup to bypass the explicit display-driver delete marker.
    $hasShaderDelete = if ($D3dVsMultiContext) {
        # Each shader is the final programmable object on its D3D8 device.
        # Stock XP may defer both public deletes to context teardown, so the
        # paired host deletes and surviving owner's exact frame are the
        # authoritative isolation evidence. No extra display delete is legal.
        $shaderDeleteCount -eq 2 -and $displayDeleteCount -le 2
    } else {
        $shaderDeleteCount -ge $shaderDefinitionMinimum -and
        $displayDeleteCount -ge $displayDeleteMinimum
    }
    $hasClipDelete = if ($D3dVsMultiContext) {
        $clipDeleteCount -eq 2
    } else { $clipDeleteCount -ge $shaderDefinitionMinimum }
    $debugText = if (Test-Path $xpdmDebugLog) {
        Get-Content -LiteralPath $xpdmDebugLog -Raw
    } else { '' }
    $drawMatches = [regex]::Matches(
        $debugText,
        '(?m)^QXPMP D3D DRAWS\r?\nQXPMP STATUS\r?\n0x([0-9A-Fa-f]{8})\r?$')
    $presentMatches = [regex]::Matches(
        $debugText,
        '(?m)^QXPMP D3D PRESENTS\r?\nQXPMP STATUS\r?\n0x([0-9A-Fa-f]{8})\r?$')
    $drawCounts = @($drawMatches | ForEach-Object {
        [Convert]::ToUInt64($_.Groups[1].Value, 16)
    })
    $presentCounts = @($presentMatches | ForEach-Object {
        [Convert]::ToUInt64($_.Groups[1].Value, 16)
    })
    $drawCount = if ($drawCounts.Count) {
        ($drawCounts | Measure-Object -Sum).Sum
    } else { 0 }
    $presentCount = if ($presentCounts.Count) {
        ($presentCounts | Measure-Object -Sum).Sum
    } else { 0 }
    $drawFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog `
            -Pattern 'QXPMP D3D DRAW .*-$' -Quiet)
    $dp2Failure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -Pattern `
            'QXPDISP DP2 (BAD|.*FAILED|CLEAR SHORT|UNKNOWN|SHORT)$' -Quiet)
    if ($D3dVsMultiContext) {
        $drawPatternOk = $drawCounts.Count -eq 1 -and
            $drawCounts[0] -eq 4
        $presentPatternOk = $presentCounts.Count -eq 1 -and
            $presentCounts[0] -eq 4
        $multiContextOk = $result -and
            $result.Contains('devices=2 ') -and
            $result.Contains('simultaneous=1 ') -and
            $result.Contains('a-destroyed=1 ') -and
            $result.Contains('survivor=1 ') -and
            $result.Contains('handles-equal=1 ')
        $d3dVs11TransportOk = $hasContext -and $hasHostSession -and
            $hasShaderDefine -and $hasClipDefine -and $hasShaderDelete -and
            $hasClipDelete -and $drawPatternOk -and $presentPatternOk -and
            $multiContextOk -and -not $drawFailure -and -not $dp2Failure
        Write-Output "D3dVsMultiContextTransportOk=$d3dVs11TransportOk context=$hasContext($contextCount) host=$hasHostSession($hostSessionCount) multiContext=$multiContextOk shaderDefine=$hasShaderDefine($shaderDefineCount/$displayDefineCount) clipDefine=$hasClipDefine($clipDefineCount) shaderDelete=$hasShaderDelete($shaderDeleteCount/$displayDeleteCount) clipDelete=$hasClipDelete($clipDeleteCount) drawSessions=$($drawCounts -join ',') presentSessions=$($presentCounts -join ',') drawPattern=$drawPatternOk presentPattern=$presentPatternOk drawFailure=$drawFailure dp2Failure=$dp2Failure"
    } elseif ($D3dVsSuccession) {
        $preResetDrawCount = if ($drawCounts.Count -ge 4) {
            $drawCounts[0] + $drawCounts[2]
        } else { 0 }
        $preResetPresentCount = if ($presentCounts.Count -ge 4) {
            $presentCounts[0] + $presentCounts[2]
        } else { 0 }
        $preResetDrawOk = $drawCounts.Count -eq 4 -and
            $drawCounts[0] -ge 6 -and $drawCounts[2] -ge 6
        $preResetPresentOk = $presentCounts.Count -eq 4 -and
            $presentCounts[0] -ge 6 -and $presentCounts[2] -ge 6
        $resetDrawOk = $drawCounts.Count -eq 4 -and
            $drawCounts[1] -ge 1 -and $drawCounts[3] -ge 1
        $resetPresentOk = $presentCounts.Count -eq 4 -and
            $presentCounts[1] -ge 1 -and $presentCounts[3] -ge 1
    } else {
        $preResetDrawCount = if ($drawCounts.Count -ge 2) {
            ($drawCounts[0..($drawCounts.Count - 2)] |
                Measure-Object -Sum).Sum
        } else { 0 }
        $preResetPresentCount = if ($presentCounts.Count -ge 2) {
            ($presentCounts[0..($presentCounts.Count - 2)] |
                Measure-Object -Sum).Sum
        } else { 0 }
        $preResetDrawOk = $preResetDrawCount -ge 6
        $preResetPresentOk = $preResetPresentCount -ge 6
        $resetDrawOk = $drawCounts.Count -ge 2 -and $drawCounts[-1] -ge 1
        $resetPresentOk = $presentCounts.Count -ge 2 -and
            $presentCounts[-1] -ge 1
    }
    if (-not $D3dVsMultiContext) {
        $lifetimeOk = $result -and
            $result.Contains('lifetime=delete-recreate')
        $successionOk = $d3dVsSuccessionLeg1Ok -and
            $d3dVsSuccessionLeg2Ok
        $d3dVs11TransportOk = $hasContext -and $hasHostSession -and
            $hasShaderDefine -and $hasClipDefine -and $hasShaderDelete -and
            $hasClipDelete -and $drawCount -ge (7 * $ownerFactor) -and
            $presentCount -ge (7 * $ownerFactor) -and
            $preResetDrawOk -and $preResetPresentOk -and
            $resetDrawOk -and $resetPresentOk -and $lifetimeOk -and
            $successionOk -and -not $drawFailure -and -not $dp2Failure
        $vsTransportLabel = if ($D3dVsSuccession) {
            'D3dVsSuccessionTransportOk'
        } else { 'D3dVs11TransportOk' }
        $successionStatus = if ($D3dVsSuccession) {
            $successionOk
        } else { 'n/a' }
        Write-Output "$vsTransportLabel=$d3dVs11TransportOk context=$hasContext($contextCount) host=$hasHostSession($hostSessionCount) succession=$successionStatus shaderDefine=$hasShaderDefine($shaderDefineCount/$displayDefineCount) clipDefine=$hasClipDefine($clipDefineCount) shaderDelete=$hasShaderDelete($shaderDeleteCount/$displayDeleteCount) clipDelete=$hasClipDelete($clipDeleteCount) drawSessions=$($drawCounts -join ',') presentSessions=$($presentCounts -join ',') draws=$drawCount presents=$presentCount preResetDraw=$preResetDrawOk($preResetDrawCount) preResetPresent=$preResetPresentOk($preResetPresentCount) resetDraw=$resetDrawOk resetPresent=$resetPresentOk lifetime=$lifetimeOk drawFailure=$drawFailure dp2Failure=$dp2Failure"
    }
}
$d3d9TransportOk = -not $D3d9
if ($D3d9) {
    $xpdmDebugLog = $debugconSource
    $hasContext = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPDISP D3C+' -Quiet)
    $hasCaps9 = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPDISP G9' -Quiet)
    $hasDdiFallback = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP GD9 FALLBACK' -Quiet)
    $hasDdi4Diagnostic = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP GD9 DIAG4' -Quiet)
    $hasExpectedDdiMarker = if ($Diagnostic) {
        $hasDdi4Diagnostic
    } else {
        $hasDdiFallback
    }
    $hasHostSession = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch 'QXPMP D3D OPEN+' -Quiet)
    $shaderLifecycle = if (Test-Path $xpdmDebugLog) {
        Get-XpD3d9ShaderLifecycle -DebugText ([string](
            Get-Content -Raw -LiteralPath $xpdmDebugLog))
    } else {
        Get-XpD3d9ShaderLifecycle
    }
    $hasShaderDefine = $shaderLifecycle.define
    $hasShaderDelete = $shaderLifecycle.delete
    $hasDraw = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D DRAW+' -Quiet)
    $hasPresent = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP FLIP PRESENT' -Quiet)
    $hasMaxStreams8Diagnostic = -not $D3d9Multistream7 -or (
        (Test-Path $xpdmDebugLog) -and [bool](
            Select-String -Path $xpdmDebugLog -SimpleMatch `
                'QXPDISP MAXSTREAMS8 DIAG' -Quiet))
    $hasCapturedMultistream = -not $D3d9Multistream7 -or (
        (Test-Path $xpdmDebugLog) -and [bool](
            Select-String -Path $xpdmDebugLog -SimpleMatch `
                'QXPDISP D3D9 CAPTURED MULTISTREAM+' -Quiet))
    $capturedMultistreamFailure = $D3d9Multistream7 -and
        (Test-Path $xpdmDebugLog) -and [bool](
            Select-String -Path $xpdmDebugLog -SimpleMatch `
                'QXPDISP D3D9 CAPTURED MULTISTREAM-' -Quiet)
    $multistreamResultOk = -not $D3d9Multistream7 -or
        ($result -and $result.Contains('multistream7=1') -and
         $result.Contains('resources-released=1') -and
         $result.Contains('reset=1'))
    $drawFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog `
            -Pattern 'QXPMP D3D DRAW .*-$' -Quiet)
    $dp2Failure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -Pattern `
            'QXPDISP DP2 (BAD|.*FAILED|CLEAR SHORT|UNKNOWN|SHORT)$' -Quiet)
    $surfaceFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP CSE TABLE-' -Quiet)
    $targetFailure = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPMP D3D TARGET-' -Quiet)
    $primitiveSkip = (Test-Path $xpdmDebugLog) -and [bool](
        Select-String -Path $xpdmDebugLog -SimpleMatch `
            'QXPDISP IDX PRIMITIVE SKIP' -Quiet)
    $d3d9TransportOk = $hasContext -and $hasCaps9 -and
                       $hasExpectedDdiMarker -and $hasHostSession -and
                       $hasShaderDefine -and
                       $hasShaderDelete -and $hasDraw -and $hasPresent -and
                       $hasMaxStreams8Diagnostic -and
                       $hasCapturedMultistream -and
                       -not $capturedMultistreamFailure -and
                       $d3d9MultistreamReady -and
                       $d3d9MultistreamDrawReleased -and
                       $d3d9MultistreamLifetime -and
                       $d3d9MultistreamOrderOk -and
                       $d3d9MultistreamStage -eq 9 -and
                       $multistreamResultOk -and
                       -not $drawFailure -and
                       -not $dp2Failure -and -not $surfaceFailure -and
                       -not $targetFailure -and
                       -not $primitiveSkip -and -not $swapLimitFailure
    Write-Output "D3d9TransportOk=$d3d9TransportOk context=$hasContext caps9=$hasCaps9 ddiExpected=$hasExpectedDdiMarker ddiFallback=$hasDdiFallback ddi4=$hasDdi4Diagnostic diagnostic=$([bool]$Diagnostic) host=$hasHostSession shaderDefine=$hasShaderDefine shaderDelete=$hasShaderDelete draw=$hasDraw present=$hasPresent multistream7=$([bool]$D3d9Multistream7),$hasMaxStreams8Diagnostic,$hasCapturedMultistream,$d3d9MultistreamReady,$d3d9MultistreamDrawReleased,$d3d9MultistreamLifetime,$multistreamResultOk order=$d3d9MultistreamOrderOk,$d3d9MultistreamStage/9 capturedFailure=$capturedMultistreamFailure drawFailure=$drawFailure dp2Failure=$dp2Failure surfaceFailure=$surfaceFailure targetFailure=$targetFailure primitiveSkip=$primitiveSkip swapLimitFailure=$swapLimitFailure"
    Write-Output "D3d9ShaderLifecycle profile=$($shaderLifecycle.profile) valid=$($shaderLifecycle.valid) legacy=$($shaderLifecycle.legacy.complete) native=$($shaderLifecycle.native.complete)"
}
$visualOk = -not $D3dVisual
if ($D3dVisual) {
    if ($D3dHwtlOnly) {
        Write-Output "D3dHwtlFrameWindow start=$d3dHwtlStartFrame end=$d3dHwtlEndFrame"
    }
    $visualFrames = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object {
            if ($_.Name -notmatch
                    '^frame_(\d+)x(\d+)_(\d{6})\.bmp$') {
                return $false
            }
            # The probe's exact sample canvas occupies the first 640x480
            # pixels even when a clean XP base starts in a larger desktop
            # mode. Keep every geometry large enough to contain that canvas;
            # the exact sample sets remain the actual frame identity gate.
            if ([int]$Matches[1] -lt 640 -or [int]$Matches[2] -lt 480) {
                return $false
            }
            $sequence = [int]$Matches[3]
            if ($D3dHwtlOnly -and $d3dHwtlStartFrame -ge 0 -and
                $sequence -lt $d3dHwtlStartFrame) {
                # Serial stage delivery can trail the synchronous frame dump,
                # so the sequence current at ENTER may already be the oracle.
                return $false
            }
            if ($D3dHwtlOnly -and $d3dHwtlEndFrame -ge 0 -and
                $sequence -gt $d3dHwtlEndFrame) {
                return $false
            }
            return $true
        } |
        Sort-Object Name)
    $visualSamples = @(
        @{ Name = 'no-texture-stage-chain'; X = 10; Y = 10; Rgb = '90A0B0' },
        @{ Name = 'texture-yellow'; X = 50; Y = 50; Rgb = 'FFFF00' },
        @{ Name = 'texture-cyan'; X = 150; Y = 50; Rgb = '00FFFF' },
        @{ Name = 'cull-ccw'; X = 415; Y = 40; Rgb = 'FF4000' },
        @{ Name = 'cull-cw'; X = 415; Y = 80; Rgb = '40FF00' },
        @{ Name = 'texture-magenta'; X = 50; Y = 150; Rgb = 'FF00FF' },
        @{ Name = 'texture-white'; X = 150; Y = 150; Rgb = 'FFFFFF' },
        @{ Name = 'mipmap-level-two'; X = 210; Y = 45; Rgb = '0000FF' },
        @{ Name = 'mipmap-lod-bias'; X = 205; Y = 240; Rgb = '0000FF' },
        @{ Name = 'stencil-pass'; X = 202; Y = 80; Rgb = 'FF8000' },
        @{ Name = 'stencil-reject'; X = 218; Y = 80; Rgb = '144696' },
        @{ Name = 'color-write-mask'; X = 202; Y = 95; Rgb = '144696' },
        @{ Name = 'z-bias-overlay'; X = 202; Y = 130; Rgb = '00FF00' },
        @{ Name = 'depth'; X = 310; Y = 80; Rgb = 'FF0000' },
        @{ Name = 'd24x8-near'; X = 85; Y = 115; Rgb = 'FF0000' },
        @{ Name = 'd24x8-far'; X = 120; Y = 115; Rgb = '00FF00' },
        @{ Name = 'd24x8-clear'; X = 140; Y = 145; Rgb = '203040' },
        @{ Name = 'blend'; X = 310; Y = 240; Rgb = '8A234B' },
        @{ Name = 'blend-subtract'; X = 270; Y = 305; Rgb = '505064' },
        @{ Name = 'alpha-reject'; X = 65; Y = 245; Rgb = '144696' },
        @{ Name = 'alpha-pass'; X = 155; Y = 245; Rgb = '00FF00' },
        @{ Name = 'texture-add-complement'; X = 50; Y = 305; Rgb = '0080FF' },
        @{ Name = 'texture-modulate2x'; X = 130; Y = 305; Rgb = '008080' },
        @{ Name = 'texture-alpha-replicate'; X = 200; Y = 305; Rgb = '404040' },
        @{ Name = 'texture-subtract'; X = 290; Y = 455; Rgb = '00BF7F' },
        @{ Name = 'texture-modulate4x'; X = 50; Y = 455; Rgb = '804000' },
        @{ Name = 'texture-addsigned'; X = 130; Y = 455; Rgb = '7F7F7F' },
        @{ Name = 'texture-addsigned2x'; X = 210; Y = 455; Rgb = 'FFFFFF' },
        @{ Name = 'texture-factor'; X = 370; Y = 455; Rgb = '336699' },
        @{ Name = 'texture-blend-diffuse'; X = 250; Y = 170; Rgb = '80807F' },
        @{ Name = 'texture-blend-texture'; X = 340; Y = 170; Rgb = '4040BF' },
        @{ Name = 'texture-blend-factor'; X = 430; Y = 170; Rgb = '80807F' },
        @{ Name = 'texture-blend-current'; X = 520; Y = 170; Rgb = 'BF4040' },
        @{ Name = 'texture-stage1'; X = 205; Y = 170; Rgb = '808080' },
        @{ Name = 'texture-stage1-shared'; X = 205; Y = 205; Rgb = '808080' },
        @{ Name = 'texture-projected'; X = 240; Y = 200; Rgb = 'FFFF00' },
        @{ Name = 'texture-perspective'; X = 347; Y = 217; Rgb = 'FFFF00' },
        @{ Name = 'texture-dot3'; X = 408; Y = 305; Rgb = '414141' },
        @{ Name = 'texture-lerp'; X = 408; Y = 335; Rgb = '40803F' },
        @{ Name = 'texture-lerp-alpha'; X = 408; Y = 275; Rgb = '8AA3CB' },
        @{ Name = 'texture-a4r4g4b4'; X = 408; Y = 245; Rgb = '914546' },
        @{ Name = 'texture-x4r4g4b4'; X = 438; Y = 245; Rgb = '0000FF' },
        @{ Name = 'texture-r8g8b8'; X = 438; Y = 275; Rgb = '563412' },
        @{ Name = 'texture-r3g3b2'; X = 438; Y = 305; Rgb = 'FF0000' },
        @{ Name = 'texture-p8-alpha'; X = 438; Y = 335; Rgb = '8A634B' },
        @{ Name = 'texture-a8p8-alpha'; X = 438; Y = 365; Rgb = '4F5570' },
        @{ Name = 'texture-p8-palette-update'; X = 468; Y = 335; Rgb = '20E040' },
        @{ Name = 'dynamic-texture-before-discard'; X = 473; Y = 308; Rgb = 'C02040' },
        @{ Name = 'dynamic-texture-after-discard'; X = 508; Y = 308; Rgb = '20C080' },
        @{ Name = 'texture-a8r3g3b2-alpha'; X = 438; Y = 430; Rgb = '6548CB' },
        @{ Name = 'texture-a4l4'; X = 468; Y = 430; Rgb = '999999' },
        @{ Name = 'texture-l8'; X = 608; Y = 215; Rgb = 'A0A0A0' },
        @{ Name = 'texture-a8l8'; X = 608; Y = 245; Rgb = '2A436B' },
        @{ Name = 'texture-dxt1-opaque'; X = 608; Y = 275; Rgb = 'FF0000' },
        @{ Name = 'texture-dxt1-alpha'; X = 608; Y = 305; Rgb = '144696' },
        @{ Name = 'texture-dxt3-alpha'; X = 608; Y = 335; Rgb = '144696' },
        @{ Name = 'texture-dxt5-alpha'; X = 608; Y = 365; Rgb = '030AF0' },
        @{ Name = 'texture-dxt5-special'; X = 608; Y = 395; Rgb = '144696' },
        @{ Name = 'texture-a8'; X = 608; Y = 425; Rgb = '8AA3CB' },
        @{ Name = 'texture-x1r5g5b5'; X = 608; Y = 455; Rgb = '00FF00' },
        @{ Name = 'strip-first'; X = 450; Y = 60; Rgb = 'FF8000' },
        @{ Name = 'strip-second'; X = 570; Y = 120; Rgb = '00FFFF' },
        @{ Name = 'fan-first'; X = 510; Y = 220; Rgb = 'FF00FF' },
        @{ Name = 'fan-second'; X = 570; Y = 270; Rgb = '00FFFF' },
        @{ Name = 'line-strip-first'; X = 470; Y = 369; Rgb = 'FF8000' },
        @{ Name = 'line-strip-second'; X = 550; Y = 370; Rgb = '00FFFF' },
        @{ Name = 'line-list-first'; X = 470; Y = 409; Rgb = 'FFFF00' },
        @{ Name = 'line-list-second'; X = 550; Y = 410; Rgb = '00FF00' },
        @{ Name = 'point-red'; X = 450; Y = 449; Rgb = 'FF0000' },
        @{ Name = 'point-green'; X = 510; Y = 449; Rgb = '00FF00' },
        @{ Name = 'point-green-radius'; X = 506; Y = 449; Rgb = '00FF00' },
        @{ Name = 'point-white'; X = 570; Y = 449; Rgb = 'FFFFFF' },
        @{ Name = 'specular'; X = 310; Y = 380; Rgb = 'FF0080' },
        @{ Name = 'minimal-fvf'; X = 110; Y = 380; Rgb = 'FFFFFF' }
        @{ Name = 'table-fog'; X = 110; Y = 360; Rgb = '2468AC' }
        @{ Name = 'vertex-fog'; X = 310; Y = 360; Rgb = 'AC6824' }
    )
    # This frame is rendered from object-space vertices. Before XPDriver
    # advertises HWTL it is XP's software-VP reference; afterwards the same
    # samples are the exact accelerated parity gate.
    $hwtlReferenceSamples = @(
        @{ Name = 'matrix'; X = 80; Y = 90; Rgb = 'CC2030' },
        @{ Name = 'directional'; X = 200; Y = 90; Rgb = '20C040' },
        @{ Name = 'material-emissive'; X = 320; Y = 90; Rgb = '00FFFF' },
        @{ Name = 'point'; X = 440; Y = 90; Rgb = '8020C0' },
        @{ Name = 'spot'; X = 560; Y = 90; Rgb = 'C06020' },
        @{ Name = 'spot-falloff'; X = 80; Y = 330; Rgb = '604020' },
        @{ Name = 'spot-falloff-power'; X = 200; Y = 330; Rgb = '302010' },
        @{ Name = 'point-material-diffuse-color2-fallback'; X = 260; Y = 330; Rgb = '0040FF' },
        @{ Name = 'point-material-diffuse-fallback'; X = 320; Y = 330; Rgb = 'FF4000' },
        @{ Name = 'point-color1-ambient'; X = 400; Y = 330; Rgb = '402010' },
        @{ Name = 'point-color1-emissive'; X = 440; Y = 330; Rgb = '2050A0' },
        @{ Name = 'point-color2-ambient'; X = 400; Y = 170; Rgb = '104060' },
        @{ Name = 'point-color2-emissive'; X = 460; Y = 170; Rgb = '6090C0' },
        @{ Name = 'point-color2-diffuse-alpha'; X = 520; Y = 170; Rgb = '383633' },
        # Stock XP D3D8 software-VP oracle. These include the intentional
        # per-vertex lighting/interpolation rounding at the sample pixels.
        @{ Name = 'point-specular-local-viewer'; X = 80; Y = 170; Rgb = '3F7D1F' },
        @{ Name = 'point-specular-infinite-viewer'; X = 160; Y = 170; Rgb = '7FFE40' },
        @{ Name = 'point-specular-color1'; X = 240; Y = 170; Rgb = '4080C0' },
        @{ Name = 'point-specular-color2'; X = 300; Y = 170; Rgb = 'C06020' },
        @{ Name = 'point-specular-color2-fallback'; X = 360; Y = 170; Rgb = '407FFF' },
        @{ Name = 'spot-specular-material'; X = 600; Y = 170; Rgb = 'FF407F' },
        @{ Name = 'object-table-fog-postproject'; X = 615; Y = 90; Rgb = '204060' },
        @{ Name = 'object-table-fog-postdivide'; X = 80; Y = 215; Rgb = 'FFFFFF' },
        @{ Name = 'object-table-fog-exp'; X = 200; Y = 215; Rgb = '9B9B9B' },
        @{ Name = 'object-table-fog-exp2'; X = 320; Y = 215; Rgb = 'C7C7C7' },
        @{ Name = 'multi-light-eight'; X = 440; Y = 215; Rgb = '999966' },
        @{ Name = 'multi-light-compact'; X = 520; Y = 215; Rgb = '666666' },
        @{ Name = 'dual-point-range-red'; X = 400; Y = 315; Rgb = 'FF0000' },
        @{ Name = 'dual-point-range-green'; X = 560; Y = 315; Rgb = '00FF00' },
        @{ Name = 'dual-point-color1-red'; X = 360; Y = 365; Rgb = 'FF0000' },
        @{ Name = 'dual-point-color1-green'; X = 440; Y = 365; Rgb = '00FF00' },
        @{ Name = 'dual-point-color2-red'; X = 520; Y = 365; Rgb = 'FF0000' },
        @{ Name = 'dual-point-color2-green'; X = 600; Y = 365; Rgb = '00FF00' },
        @{ Name = 'triple-point-range-red'; X = 200; Y = 40; Rgb = 'FF0000' },
        @{ Name = 'triple-point-range-green'; X = 320; Y = 40; Rgb = '00FF00' },
        @{ Name = 'triple-point-range-blue'; X = 440; Y = 40; Rgb = '0000FF' },
        @{ Name = 'triple-point-vertex-fog'; X = 80; Y = 40; Rgb = '123456' },
        @{ Name = 'triple-point-table-fog'; X = 560; Y = 40; Rgb = '603018' },
        @{ Name = 'dual-point-specular-material-red'; X = 140; Y = 130; Rgb = 'FF0000' },
        @{ Name = 'dual-point-specular-color2-red'; X = 260; Y = 130; Rgb = 'FF0000' },
        @{ Name = 'dual-point-specular-color2-green'; X = 380; Y = 130; Rgb = '00FF00' },
        @{ Name = 'dual-point-specular-material-green'; X = 500; Y = 130; Rgb = '00FF00' },
        @{ Name = 'quad-point-range-red'; X = 80; Y = 20; Rgb = 'FF0000' },
        @{ Name = 'quad-point-range-green'; X = 240; Y = 20; Rgb = '00FF00' },
        @{ Name = 'quad-point-range-blue'; X = 400; Y = 20; Rgb = '0000FF' },
        @{ Name = 'quad-point-range-magenta'; X = 560; Y = 20; Rgb = 'FF00FF' },
        @{ Name = 'five-point-range-red'; X = 80; Y = 55; Rgb = 'FF0000' },
        @{ Name = 'five-point-range-green'; X = 200; Y = 55; Rgb = '00FF00' },
        @{ Name = 'five-point-range-blue'; X = 320; Y = 55; Rgb = '0000FF' },
        @{ Name = 'five-point-range-cyan'; X = 440; Y = 55; Rgb = '00FFFF' },
        @{ Name = 'five-point-range-magenta'; X = 560; Y = 55; Rgb = 'FF00FF' },
        @{ Name = 'six-point-range-red'; X = 70; Y = 30; Rgb = 'FF0000' },
        @{ Name = 'six-point-range-green'; X = 170; Y = 30; Rgb = '00FF00' },
        @{ Name = 'six-point-range-blue'; X = 270; Y = 30; Rgb = '0000FF' },
        @{ Name = 'six-point-range-cyan'; X = 370; Y = 30; Rgb = '00FFFF' },
        @{ Name = 'six-point-range-magenta'; X = 470; Y = 30; Rgb = 'FF00FF' },
        @{ Name = 'six-point-range-yellow'; X = 570; Y = 30; Rgb = 'FFFF00' },
        @{ Name = 'seven-point-range-red'; X = 50; Y = 64; Rgb = 'FF0000' },
        @{ Name = 'seven-point-range-green'; X = 140; Y = 64; Rgb = '00FF00' },
        @{ Name = 'seven-point-range-blue'; X = 230; Y = 64; Rgb = '0000FF' },
        @{ Name = 'seven-point-range-cyan'; X = 320; Y = 64; Rgb = '00FFFF' },
        @{ Name = 'seven-point-range-magenta'; X = 410; Y = 64; Rgb = 'FF00FF' },
        @{ Name = 'seven-point-range-yellow'; X = 500; Y = 64; Rgb = 'FFFF00' },
        @{ Name = 'seven-point-range-final-red'; X = 590; Y = 64; Rgb = 'FF0000' },
        @{ Name = 'eight-point-range-red'; X = 40; Y = 468; Rgb = 'FF0000' },
        @{ Name = 'eight-point-range-green'; X = 120; Y = 468; Rgb = '00FF00' },
        @{ Name = 'eight-point-range-blue'; X = 200; Y = 468; Rgb = '0000FF' },
        @{ Name = 'eight-point-range-cyan'; X = 280; Y = 468; Rgb = '00FFFF' },
        @{ Name = 'eight-point-range-magenta'; X = 360; Y = 468; Rgb = 'FF00FF' },
        @{ Name = 'eight-point-range-yellow'; X = 440; Y = 468; Rgb = 'FFFF00' },
        @{ Name = 'eight-point-range-repeat-red'; X = 520; Y = 468; Rgb = 'FF0000' },
        @{ Name = 'eight-point-range-final-green'; X = 600; Y = 468; Rgb = '00FF00' },
        @{ Name = 'object-table-fog-point-range'; X = 600; Y = 260; Rgb = '123456' },
        @{ Name = 'object-table-fog-nonprojected4'; X = 620; Y = 400; Rgb = '007F7F' },
        @{ Name = 'normal-matrix'; X = 120; Y = 260; Rgb = '40A0FF' },
        @{ Name = 'object-point-5'; X = 200; Y = 260; Rgb = 'FF0000' },
        @{ Name = 'object-point-11'; X = 260; Y = 260; Rgb = '00FF00' },
        @{ Name = 'object-point-11-inner'; X = 264; Y = 260; Rgb = '00FF00' },
        @{ Name = 'object-point-11-outer'; X = 267; Y = 260; Rgb = '0B1D2E' },
        @{ Name = 'object-point-17'; X = 320; Y = 260; Rgb = '0000FF' },
        @{ Name = 'object-point-17-inner'; X = 327; Y = 260; Rgb = '0000FF' },
        @{ Name = 'object-point-17-outer'; X = 330; Y = 260; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-a-min'; X = 80; Y = 450; Rgb = 'FF0000' },
        @{ Name = 'object-point-scale-a-min-inner'; X = 83; Y = 450; Rgb = 'FF0000' },
        @{ Name = 'object-point-scale-a-min-outer'; X = 87; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-a'; X = 200; Y = 450; Rgb = '00FF00' },
        @{ Name = 'object-point-scale-a-inner'; X = 206; Y = 450; Rgb = '00FF00' },
        @{ Name = 'object-point-scale-a-outer'; X = 209; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-a-max'; X = 320; Y = 450; Rgb = '0000FF' },
        @{ Name = 'object-point-scale-a-max-inner'; X = 328; Y = 450; Rgb = '0000FF' },
        @{ Name = 'object-point-scale-a-max-outer'; X = 332; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-b'; X = 440; Y = 450; Rgb = 'FF00FF' },
        @{ Name = 'object-point-scale-b-inner'; X = 446; Y = 450; Rgb = 'FF00FF' },
        @{ Name = 'object-point-scale-b-outer'; X = 449; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-c'; X = 560; Y = 450; Rgb = '00FFFF' },
        @{ Name = 'object-point-scale-c-inner'; X = 566; Y = 450; Rgb = '00FFFF' },
        @{ Name = 'object-point-scale-c-outer'; X = 569; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'object-point-scale-lit'; X = 620; Y = 450; Rgb = 'FF4000' },
        @{ Name = 'object-point-scale-lit-inner'; X = 626; Y = 450; Rgb = 'FF4000' },
        @{ Name = 'object-point-scale-lit-outer'; X = 629; Y = 450; Rgb = '0B1D2E' },
        @{ Name = 'point-sprite-stage0-tl'; X = 20; Y = 120; Rgb = 'FFFF00' },
        @{ Name = 'point-sprite-stage0-tr'; X = 40; Y = 120; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-stage0-bl'; X = 20; Y = 140; Rgb = 'FF00FF' },
        @{ Name = 'point-sprite-stage0-br'; X = 40; Y = 140; Rgb = 'FFFFFF' },
        @{ Name = 'point-sprite-stage1-tl'; X = 20; Y = 195; Rgb = 'FFFF00' },
        @{ Name = 'point-sprite-stage1-tr'; X = 40; Y = 195; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-stage1-bl'; X = 20; Y = 215; Rgb = 'FF00FF' },
        @{ Name = 'point-sprite-stage1-br'; X = 40; Y = 215; Rgb = 'FFFFFF' },
        @{ Name = 'point-sprite-disabled-tl'; X = 20; Y = 270; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-disabled-tr'; X = 40; Y = 270; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-disabled-bl'; X = 20; Y = 290; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-disabled-br'; X = 40; Y = 290; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-pixel-shader-tl'; X = 20; Y = 345; Rgb = 'FFFF00' },
        @{ Name = 'point-sprite-pixel-shader-tr'; X = 40; Y = 345; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-pixel-shader-bl'; X = 20; Y = 365; Rgb = 'FF00FF' },
        @{ Name = 'point-sprite-pixel-shader-br'; X = 40; Y = 365; Rgb = 'FFFFFF' },
        @{ Name = 'point-sprite-offscreen-tl'; X = 20; Y = 420; Rgb = 'FFFF00' },
        @{ Name = 'point-sprite-offscreen-tr'; X = 40; Y = 420; Rgb = '00FFFF' },
        @{ Name = 'point-sprite-offscreen-bl'; X = 20; Y = 440; Rgb = 'FF00FF' },
        @{ Name = 'point-sprite-offscreen-br'; X = 40; Y = 440; Rgb = 'FFFFFF' },
        @{ Name = 'clip-reject'; X = 370; Y = 260; Rgb = '0B1D2E' },
        @{ Name = 'clip-keep'; X = 410; Y = 260; Rgb = 'FF2080' },
        @{ Name = 'point-range-reject'; X = 520; Y = 260; Rgb = '000000' },
        @{ Name = 'texture-nonprojected4-fog'; X = 160; Y = 400; Rgb = '8040C0' },
        @{ Name = 'texture-nonprojected4-range-fog'; X = 500; Y = 330; Rgb = $(
            if ($D3dHardware) { 'C04020' } else { '00FFFF' }
        ) },
        @{ Name = 'texture-nonprojected4-point-range'; X = 600; Y = 330; Rgb = '00FF80' },
        @{ Name = 'texture-transform'; X = 250; Y = 400; Rgb = '00FFFF' },
        @{ Name = 'texture-nonprojected4'; X = 350; Y = 400; Rgb = '00FFFF' },
        @{ Name = 'texture-project'; X = 450; Y = 400; Rgb = 'FFFF00' },
        @{ Name = 'object-vertex-fog'; X = 80; Y = 400; Rgb = '20C060' },
        # XP's stock D3D8 software vertex processor uses planar eye Z even
        # when RANGEFOGENABLE is set. The accelerated path must instead use
        # the advertised radial range-fog semantics.
        @{ Name = 'object-range-fog'; X = 560; Y = 400; Rgb = $(
            if ($D3dHardware) { 'C040FF' } else { 'FFFFFF' }
        ) },
        @{ Name = 'background'; X = 620; Y = 460; Rgb = '0B1D2E' }
    )
    # A separate frame keeps scalable positional-light additions independent
    # from the mature 131-sample diffuse/fog/texture oracle. Alternating rows
    # use pure point or pure spot lights; the two-spot pair isolates exact
    # linear/squared outer-cone Falloff. A translated-world pair combines the
    # same nonlinear lighting with a user clip plane. A scaled point then
    # combines hard Range, point-sprite replacement, texturing, and linear fog.
    # The next triplet combines scalable spot lighting with planar/radial
    # vertex fog and post-divide table fog.
    # Rows three through six use Power=0
    # exact channel endpoints. The seven/eight-light Power=8 values below are
    # the stock XP software-VP oracle. The infinite-viewer row is black because
    # its (0,0,-1) viewer cancels each direct +Z light vector.
    $hwtlSpecularSamples = @(
        @{ Name = 'background-top'; X = 10; Y = 10; Rgb = '16233B' },
        @{ Name = 'background-bottom'; X = 630; Y = 470; Rgb = '16233B' },
        @{ Name = 'three-material-red'; X = 120; Y = 55; Rgb = 'FF0000' },
        @{ Name = 'three-material-green'; X = 320; Y = 55; Rgb = '00FF00' },
        @{ Name = 'three-material-blue'; X = 520; Y = 55; Rgb = '0000FF' },
        @{ Name = 'two-spot-falloff-linear'; X = 220; Y = 55; Rgb = '604020' },
        @{ Name = 'two-spot-falloff-squared'; X = 420; Y = 55; Rgb = '302010' },
        @{ Name = 'scalable-spot-world-clip-reject'; X = 220; Y = 170; Rgb = '16233B' },
        @{ Name = 'scalable-spot-world-clip-keep'; X = 420; Y = 170; Rgb = '302010' },
        @{ Name = 'scalable-point-small-center'; X = 40; Y = 170; Rgb = 'FF0000' },
        @{ Name = 'scalable-point-small-outer'; X = 47; Y = 170; Rgb = '16233B' },
        @{ Name = 'scalable-point-large-center'; X = 600; Y = 170; Rgb = '00FF00' },
        @{ Name = 'scalable-point-large-inner'; X = 607; Y = 170; Rgb = '00FF00' },
        @{ Name = 'scalable-point-large-outer'; X = 612; Y = 170; Rgb = '16233B' },
        @{ Name = 'scalable-point-sprite-tl-fog'; X = 194; Y = 164; Rgb = '90A030'; Tolerance = 1 },
        @{ Name = 'scalable-point-sprite-tr-fog'; X = 206; Y = 164; Rgb = '10A0B0'; Tolerance = 1 },
        @{ Name = 'scalable-point-sprite-bl-fog'; X = 194; Y = 176; Rgb = '9020B0'; Tolerance = 1 },
        @{ Name = 'scalable-point-sprite-br-fog'; X = 206; Y = 176; Rgb = '90A0B0'; Tolerance = 1 },
        @{ Name = 'scalable-point-sprite-outer'; X = 212; Y = 170; Rgb = '16233B' },
        @{ Name = 'scalable-directional-point-inside'; X = 160; Y = 245; Rgb = 'FFFF00' },
        @{ Name = 'scalable-directional-point-outside'; X = 480; Y = 245; Rgb = 'FF0000' },
        @{ Name = 'scalable-spot-specular-vertex-fog'; X = 220; Y = 95; Rgb = '504040' },
        @{ Name = 'scalable-spot-range-fog'; X = 320; Y = 95; Rgb = $(
            if ($D3dHardware) { 'C040FF' } else { '604020' }
        ) },
        @{ Name = 'scalable-spot-table-fog'; X = 420; Y = 95; Rgb = '6C381C' },
        @{ Name = 'four-color2-red'; X = 80; Y = 130; Rgb = 'FF0000' },
        @{ Name = 'four-color2-green'; X = 240; Y = 130; Rgb = '00FF00' },
        @{ Name = 'four-color2-blue'; X = 400; Y = 130; Rgb = '0000FF' },
        @{ Name = 'four-color2-cyan'; X = 560; Y = 130; Rgb = '00FFFF' },
        @{ Name = 'five-material-red'; X = 80; Y = 205; Rgb = 'FF0000' },
        @{ Name = 'five-material-green'; X = 200; Y = 205; Rgb = '00FF00' },
        @{ Name = 'five-material-blue'; X = 320; Y = 205; Rgb = '0000FF' },
        @{ Name = 'five-material-cyan'; X = 440; Y = 205; Rgb = '00FFFF' },
        @{ Name = 'five-material-magenta'; X = 560; Y = 205; Rgb = 'FF00FF' },
        @{ Name = 'six-color2-red'; X = 70; Y = 280; Rgb = 'FF0000' },
        @{ Name = 'six-color2-green'; X = 170; Y = 280; Rgb = '00FF00' },
        @{ Name = 'six-color2-blue'; X = 270; Y = 280; Rgb = '0000FF' },
        @{ Name = 'six-color2-cyan'; X = 370; Y = 280; Rgb = '00FFFF' },
        @{ Name = 'six-color2-magenta'; X = 470; Y = 280; Rgb = 'FF00FF' },
        @{ Name = 'six-color2-yellow'; X = 570; Y = 280; Rgb = 'FFFF00' },
        # Hardware VP evaluates the Power=8 half-vector at full precision and
        # quantizes after interpolation. Stock XP D3D8 software VP quantizes
        # each lit vertex to D3DCOLOR before the driver receives the
        # transformed FVF. Once the driver's circular execute-buffer extent is
        # resolved from lPitch rather than the current draw length, both paths
        # have the same nominal sample values here. Keep software VP exact and
        # retain one-LSB hardware latitude for host rasterization.
        @{ Name = 'seven-local-red'; X = 50; Y = 355; Rgb = '050000';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-green'; X = 140; Y = 355; Rgb = '000400';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-blue'; X = 230; Y = 355; Rgb = '000002';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-cyan'; X = 320; Y = 355; Rgb = '000202';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-magenta'; X = 410; Y = 355; Rgb = '020002';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-yellow'; X = 500; Y = 355; Rgb = '040400';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'seven-local-final-red'; X = 590; Y = 355; Rgb = '050000';
           Tolerance = $(if ($D3dHardware) { 1 } else { 0 }) },
        @{ Name = 'eight-infinite-red'; X = 40; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-green'; X = 120; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-blue'; X = 200; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-cyan'; X = 280; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-magenta'; X = 360; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-yellow'; X = 440; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-repeat-red'; X = 520; Y = 430; Rgb = '000000' },
        @{ Name = 'eight-infinite-final-green'; X = 600; Y = 430; Rgb = '000000' }
    )
    # Reset gets its own object-space render. Its distinct background makes
    # this impossible to satisfy with either the pre-Reset HWTL frame or the
    # uniform reset-clear frame.
    $postResetHwtlSamples = @(
        @{ Name = 'triangle'; X = 320; Y = 240; Rgb = 'FF4000' },
        @{ Name = 'background'; X = 20; Y = 20; Rgb = '0D2E4F' }
    )
    $visualMatch = $null
    $hwtlReferenceMatch = $null
    $hwtlReferenceCandidate = $null
    $hwtlReferencePixels = $null
    $hwtlReferenceBestCount = -1
    $hwtlSpecularMatch = $null
    $hwtlSpecularCandidate = $null
    $hwtlSpecularPixels = $null
    $hwtlSpecularBestCount = -1
    $postResetHwtlMatch = $null
    $postResetHwtlCandidate = $null
    $postResetHwtlPixels = $null
    $postResetHwtlBestCount = -1
    $fillModeMatch = $null
    $fillModeCandidate = $null
    $fillModeBestWhite = 0
    $fillModeBestInterior = 0
    $resetMatch = $null
    $sceneCandidate = $null
    $scenePixels = $null
    $bestMatchCount = -1
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $visualFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                if (-not $D3dHwtlOnly) {
                    $resetPixel = $bitmap.GetPixel(320, 240)
                    $resetRgb = '{0:X2}{1:X2}{2:X2}' -f `
                        $resetPixel.R, $resetPixel.G, $resetPixel.B
                    if (-not $resetMatch -and
                        (Test-D3dVisualPixel $resetPixel '123456')) {
                        $resetMatch = $frame
                    }
                    $postResetPixels = @{}
                    $postResetMatchCount = 0
                    foreach ($sample in $postResetHwtlSamples) {
                        $pixel = Get-D3dVisualSamplePixel $bitmap $sample
                        if ($null -eq $pixel) {
                            $postResetPixels[$sample.Name] =
                                "out-of-bounds:$($bitmap.Width)x$($bitmap.Height)"
                            continue
                        }
                        $rgb = '{0:X2}{1:X2}{2:X2}' -f `
                            $pixel.R, $pixel.G, $pixel.B
                        $postResetPixels[$sample.Name] = $rgb
                        if (Test-D3dVisualPixel $pixel $sample.Rgb) {
                            ++$postResetMatchCount
                        }
                    }
                    if ($postResetMatchCount -gt $postResetHwtlBestCount) {
                        $postResetHwtlBestCount = $postResetMatchCount
                        $postResetHwtlCandidate = $frame
                        $postResetHwtlPixels = $postResetPixels
                    }
                    if ($postResetMatchCount -eq $postResetHwtlSamples.Count) {
                        if (-not $postResetHwtlMatch) {
                            $postResetHwtlMatch = $frame
                        }
                        $postResetHwtlPixels = $postResetPixels
                    }
                    $wireWhite = 0
                    foreach ($y in 98..102) {
                        foreach ($x in 200..440) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            if (Test-D3dVisualPixel $pixel 'FFFFFF') {
                                ++$wireWhite
                            }
                        }
                    }
                    $wireInterior = 0
                    foreach ($y in 180..220) {
                        foreach ($x in 300..340) {
                            $pixel = $bitmap.GetPixel($x, $y)
                            if (Test-D3dVisualPixel $pixel '144696') {
                                ++$wireInterior
                            }
                        }
                    }
                    if ($wireWhite -gt $fillModeBestWhite) {
                        $fillModeBestWhite = $wireWhite
                        $fillModeBestInterior = $wireInterior
                        $fillModeCandidate = $frame
                    }
                    if (-not $fillModeMatch -and $wireWhite -ge 100 -and
                        $wireInterior -ge 1500) {
                        $fillModeMatch = $frame
                    }
                    $pixels = @{}
                    $matches = $true
                    $matchCount = 0
                    foreach ($sample in $visualSamples) {
                        $pixel = Get-D3dVisualSamplePixel $bitmap $sample
                        if ($null -eq $pixel) {
                            $pixels[$sample.Name] =
                                "out-of-bounds:$($bitmap.Width)x$($bitmap.Height)"
                            $matches = $false
                            continue
                        }
                        $rgb = '{0:X2}{1:X2}{2:X2}' -f `
                            $pixel.R, $pixel.G, $pixel.B
                        $pixels[$sample.Name] = $rgb
                        if (Test-D3dVisualPixel $pixel $sample.Rgb) {
                            ++$matchCount
                        } else {
                            $matches = $false
                        }
                    }
                    if ($matchCount -gt $bestMatchCount) {
                        $bestMatchCount = $matchCount
                        $sceneCandidate = $frame
                        $scenePixels = $pixels
                    }
                    if ($matches) {
                        if (-not $visualMatch) { $visualMatch = $frame }
                        $scenePixels = $pixels
                    }
                }
                $hwtlPixels = @{}
                $hwtlMatches = $true
                $hwtlMatchCount = 0
                foreach ($sample in $hwtlReferenceSamples) {
                    $pixel = Get-D3dVisualSamplePixel $bitmap $sample
                    if ($null -eq $pixel) {
                        $hwtlPixels[$sample.Name] =
                            "out-of-bounds:$($bitmap.Width)x$($bitmap.Height)"
                        $hwtlMatches = $false
                        continue
                    }
                    $rgb = '{0:X2}{1:X2}{2:X2}' -f `
                        $pixel.R, $pixel.G, $pixel.B
                    $hwtlPixels[$sample.Name] = $rgb
                    if (Test-D3dVisualPixel $pixel $sample.Rgb) {
                        ++$hwtlMatchCount
                    } else {
                        $hwtlMatches = $false
                    }
                }
                if ($hwtlMatchCount -gt $hwtlReferenceBestCount) {
                    $hwtlReferenceBestCount = $hwtlMatchCount
                    $hwtlReferenceCandidate = $frame
                    $hwtlReferencePixels = $hwtlPixels
                }
                if ($hwtlMatches) {
                    if (-not $hwtlReferenceMatch) {
                        $hwtlReferenceMatch = $frame
                    }
                    $hwtlReferencePixels = $hwtlPixels
                }
                $specularPixels = @{}
                $specularMatches = $true
                $specularMatchCount = 0
                foreach ($sample in $hwtlSpecularSamples) {
                    $pixel = Get-D3dVisualSamplePixel $bitmap $sample
                    if ($null -eq $pixel) {
                        $specularPixels[$sample.Name] =
                            "out-of-bounds:$($bitmap.Width)x$($bitmap.Height)"
                        $specularMatches = $false
                        continue
                    }
                    $rgb = '{0:X2}{1:X2}{2:X2}' -f `
                        $pixel.R, $pixel.G, $pixel.B
                    $specularPixels[$sample.Name] = $rgb
                    if (Test-D3dVisualPixel $pixel $sample.Rgb `
                            $sample.Tolerance) {
                        ++$specularMatchCount
                    } else {
                        $specularMatches = $false
                    }
                }
                if ($specularMatchCount -gt $hwtlSpecularBestCount) {
                    $hwtlSpecularBestCount = $specularMatchCount
                    $hwtlSpecularCandidate = $frame
                    $hwtlSpecularPixels = $specularPixels
                }
                if ($specularMatches) {
                    if (-not $hwtlSpecularMatch) {
                        $hwtlSpecularMatch = $frame
                    }
                    $hwtlSpecularPixels = $specularPixels
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        $completeVisualMatch = if ($D3dHwtlOnly) {
            [bool]$hwtlReferenceMatch -and [bool]$hwtlSpecularMatch
        } else {
            $visualMatch -and $hwtlReferenceMatch -and
                $hwtlSpecularMatch -and $resetMatch -and
                $postResetHwtlMatch -and $fillModeMatch
        }
        if ($completeVisualMatch) {
            if ($D3dHwtlOnly) {
                Copy-Item -LiteralPath $hwtlReferenceMatch.FullName `
                    -Destination $d3dHwtlReferenceImage -Force
                Copy-Item -LiteralPath $hwtlSpecularMatch.FullName `
                    -Destination $d3dHwtlSpecularImage -Force
                [void](Export-OpaqueReviewPng $d3dHwtlReferenceImage)
                [void](Export-OpaqueReviewPng $d3dHwtlSpecularImage)
                $visualOk = $true
                Write-Output "D3dVisual=ok focused=hwtl hwtlReference=$($hwtlReferenceMatch.Name) hwtlSpecular=$($hwtlSpecularMatch.Name)"
            } else {
                Copy-Item -LiteralPath $visualMatch.FullName `
                    -Destination $d3dVisualImage -Force
                Copy-Item -LiteralPath $hwtlReferenceMatch.FullName `
                    -Destination $d3dHwtlReferenceImage -Force
                Copy-Item -LiteralPath $hwtlSpecularMatch.FullName `
                    -Destination $d3dHwtlSpecularImage -Force
                Copy-Item -LiteralPath $postResetHwtlMatch.FullName `
                    -Destination $d3dPostResetHwtlImage -Force
                Copy-Item -LiteralPath $fillModeMatch.FullName `
                    -Destination $d3dFillModeImage -Force
                foreach ($retainedImage in @(
                        $d3dVisualImage, $d3dHwtlReferenceImage,
                        $d3dHwtlSpecularImage,
                        $d3dPostResetHwtlImage, $d3dFillModeImage)) {
                    [void](Export-OpaqueReviewPng $retainedImage)
                }
                Remove-Item -LiteralPath $d3dVisualFailureImage,
                    $d3dHwtlReferenceFailureImage,
                    $d3dHwtlSpecularFailureImage,
                    $d3dPostResetHwtlFailureImage, $d3dFillModeFailureImage `
                    -Force -ErrorAction SilentlyContinue
                $visualOk = $true
                Write-Output "D3dVisual=ok frame=$($visualMatch.Name) hwtlReference=$($hwtlReferenceMatch.Name) hwtlSpecular=$($hwtlSpecularMatch.Name) reset=$($resetMatch.Name) postResetHwtl=$($postResetHwtlMatch.Name) fillmode=$($fillModeMatch.Name)"
            }
        } else {
            $failureSource = if ($sceneCandidate) {
                $sceneCandidate
            } elseif ($visualFrames.Count) {
                $visualFrames[-1]
            }
            if ($failureSource) {
                Copy-Item -LiteralPath $failureSource.FullName `
                    -Destination $d3dVisualFailureImage -Force
                [void](Export-OpaqueReviewPng $d3dVisualFailureImage)
            }
            if ($fillModeCandidate) {
                Copy-Item -LiteralPath $fillModeCandidate.FullName `
                    -Destination $d3dFillModeFailureImage -Force
                [void](Export-OpaqueReviewPng $d3dFillModeFailureImage)
            }
            if ($hwtlReferenceCandidate) {
                Copy-Item -LiteralPath $hwtlReferenceCandidate.FullName `
                    -Destination $d3dHwtlReferenceFailureImage -Force
                [void](Export-OpaqueReviewPng $d3dHwtlReferenceFailureImage)
            }
            if ($hwtlSpecularCandidate) {
                Copy-Item -LiteralPath $hwtlSpecularCandidate.FullName `
                    -Destination $d3dHwtlSpecularFailureImage -Force
                [void](Export-OpaqueReviewPng $d3dHwtlSpecularFailureImage)
            }
            if ($postResetHwtlCandidate) {
                Copy-Item -LiteralPath $postResetHwtlCandidate.FullName `
                    -Destination $d3dPostResetHwtlFailureImage -Force
                [void](Export-OpaqueReviewPng $d3dPostResetHwtlFailureImage)
            }
            $pixelDetail = if ($scenePixels) {
                ($visualSamples | ForEach-Object {
                    "$($_.Name)=$($scenePixels[$_.Name])"
                }) -join ','
            } else {
                'no-scene-frame'
            }
            $resetDetail = if ($resetMatch) { $resetMatch.Name } else { 'missing' }
            $postResetHwtlDetail = if ($postResetHwtlMatch) {
                $postResetHwtlMatch.Name
            } elseif ($postResetHwtlPixels) {
                $postResetPixelDetail = ($postResetHwtlSamples |
                    ForEach-Object {
                        "$($_.Name)=$($postResetHwtlPixels[$_.Name])"
                    }) -join ','
                "missing $postResetPixelDetail"
            } else {
                'missing no-post-reset-frame'
            }
            $fillModeDetail = if ($fillModeMatch) { $fillModeMatch.Name } else { "missing white=$fillModeBestWhite interior=$fillModeBestInterior" }
            $hwtlDetail = if ($hwtlReferencePixels) {
                ($hwtlReferenceSamples | ForEach-Object {
                    "$($_.Name)=$($hwtlReferencePixels[$_.Name])"
                }) -join ','
            } else {
                'no-reference-frame'
            }
            $specularDetail = if ($hwtlSpecularPixels) {
                ($hwtlSpecularSamples | ForEach-Object {
                    "$($_.Name)=$($hwtlSpecularPixels[$_.Name])"
                }) -join ','
            } else {
                'no-specular-frame'
            }
            Write-Output "D3dVisual=failed frames=$($visualFrames.Count) matches=$bestMatchCount/$($visualSamples.Count) hwtlReference=$hwtlReferenceBestCount/$($hwtlReferenceSamples.Count) hwtlSpecular=$hwtlSpecularBestCount/$($hwtlSpecularSamples.Count) reset=$resetDetail postResetHwtl=$postResetHwtlDetail fillmode=$fillModeDetail pixels=$pixelDetail hwtlPixels=$hwtlDetail specularPixels=$specularDetail"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
$d3dCopyVisualOk = -not $D3dCopyVisual
if ($D3dCopyVisual) {
    # Serial-file polling can observe the final Present stage only after the
    # guest has already released its GL context. The host trace records frame
    # dumps and context deletion synchronously, so cap scoring at the final
    # frame captured while the last test context was still alive.
    $latestDumpBeforeDelete = -1
    $latestDumpSeen = -1
    if (Test-Path -LiteralPath $stderr) {
        foreach ($line in Get-Content -LiteralPath $stderr) {
            if ($line -match
                '\[dump\] frame_\d+x\d+_(\d{6})\.bmp') {
                $latestDumpSeen = [int]$Matches[1]
            } elseif ($line -match 'mesapt: wglDeleteContext' -and
                      $latestDumpSeen -ge 0) {
                $latestDumpBeforeDelete = $latestDumpSeen
            }
        }
    }
    if ($latestDumpBeforeDelete -ge 0 -and
        ($d3dCopyEndFrame -lt 0 -or
         $latestDumpBeforeDelete -lt $d3dCopyEndFrame)) {
        $d3dCopyEndFrame = $latestDumpBeforeDelete
    }
    $copyFrames = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object {
            if ($_.Name -notmatch '^frame_\d+x\d+_(\d{6})\.bmp$') {
                return $false
            }
            $d3dCopyEndFrame -lt 0 -or [int]$Matches[1] -le $d3dCopyEndFrame
        } |
        Sort-Object Name)
    $expectedCenters = @{
        0x00FFFF = '00FFFF'
        0xFF00FF = 'FF00FF'
        0xFF8000 = 'FF8000'
        0xFFFFFF = 'FFFFFF'
        0x8040C0 = '8040C0'
    }
    $expectedAnchors = @{
        0xFF0000 = 'red-top-left'
        0x00FF00 = 'green-top-right'
        0x0000FF = 'blue-bottom-left'
        0xFFFF00 = 'yellow-bottom-right'
    }
    $seenCenters = @{}
    $badFrames = [Collections.Generic.List[string]]::new()
    $lastGood = $null
    $failureCandidate = $null
    try {
        Add-Type -AssemblyName System.Drawing
        if (-not ('QxpCopyFrameScanner' -as [type])) {
            Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class QxpCopyFrameScanner
{
    // Returns count, sum(x), and sum(y) for each requested exact RGB color.
    public static long[] Scan(string path, int[] colors, int step)
    {
        if (step < 1) step = 1;
        using (Bitmap bitmap = new Bitmap(path)) {
            Rectangle bounds = new Rectangle(0, 0, bitmap.Width,
                                             bitmap.Height);
            BitmapData locked = bitmap.LockBits(
                bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                int stride = Math.Abs(locked.Stride);
                byte[] pixels = new byte[stride * bitmap.Height];
                Marshal.Copy(locked.Scan0, pixels, 0, pixels.Length);
                Dictionary<int, int> slots = new Dictionary<int, int>();
                long[] result = new long[colors.Length * 3];
                int index;

                for (index = 0; index < colors.Length; ++index)
                    slots[colors[index]] = index;
                for (int y = 0; y < bitmap.Height; y += step) {
                    int row = locked.Stride >= 0 ? y * stride :
                              (bitmap.Height - 1 - y) * stride;
                    for (int x = 0; x < bitmap.Width; x += step) {
                        int offset = row + x * 4;
                        int rgb = (pixels[offset + 2] << 16) |
                                  (pixels[offset + 1] << 8) |
                                  pixels[offset];
                        if (!slots.TryGetValue(rgb, out index)) continue;
                        int baseIndex = index * 3;
                        ++result[baseIndex];
                        result[baseIndex + 1] += x;
                        result[baseIndex + 2] += y;
                    }
                }
                return result;
            } finally {
                bitmap.UnlockBits(locked);
            }
        }
    }
}
'@
        }
        $scanCodes = [int[]](@($expectedCenters.Keys) +
                             @($expectedAnchors.Keys))
        $candidateFrames = [Collections.Generic.List[IO.FileInfo]]::new()
        foreach ($frame in $copyFrames) {
            $sample = [QxpCopyFrameScanner]::Scan(
                $frame.FullName, $scanCodes, 8)
            $sampledAnchors = 0
            $sampledCenter = $false
            for ($index = 0; $index -lt $scanCodes.Count; ++$index) {
                if ($expectedAnchors.ContainsKey($scanCodes[$index]) -and
                    $sample[$index * 3] -gt 0) {
                    ++$sampledAnchors
                }
                if ($expectedCenters.ContainsKey($scanCodes[$index]) -and
                    $scanCodes[$index] -ne 0xFFFFFF -and
                    $sample[$index * 3] -gt 0) {
                    $sampledCenter = $true
                }
            }
            if ($sampledAnchors -ge 3 -or $sampledCenter) {
                $candidateFrames.Add($frame)
            }
        }
        foreach ($frame in $candidateFrames) {
            $scan = [QxpCopyFrameScanner]::Scan(
                $frame.FullName, $scanCodes, 1)
            $regions = @{}
            for ($index = 0; $index -lt $scanCodes.Count; ++$index) {
                $regions[$scanCodes[$index]] = @{
                    Count = $scan[$index * 3]
                    X = $scan[$index * 3 + 1]
                    Y = $scan[$index * 3 + 2]
                }
            }
                $centerCode = 0
                $centerCount = 0
                foreach ($code in $expectedCenters.Keys) {
                    if ($regions[[int]$code].Count -gt $centerCount) {
                        $centerCode = [int]$code
                        $centerCount = $regions[[int]$code].Count
                    }
                }
                if ($centerCount -lt 100) { continue }
                $centerRgb = $expectedCenters[$centerCode]
                $seenCenters[$centerRgb] = $true
                $centerX = $regions[$centerCode].X / $centerCount
                $centerY = $regions[$centerCode].Y / $centerCount
                $anchorDetail = [Collections.Generic.List[string]]::new()
                $anchorsOk = $true
                foreach ($code in $expectedAnchors.Keys) {
                    $region = $regions[[int]$code]
                    if ($region.Count -lt 100) {
                        $anchorsOk = $false
                        $anchorDetail.Add(
                            "$($expectedAnchors[$code])=missing:$($region.Count)")
                        continue
                    }
                    $anchorX = $region.X / $region.Count
                    $anchorY = $region.Y / $region.Count
                    $anchorDetail.Add(
                        "$($expectedAnchors[$code])=$($region.Count)@$([Math]::Round($anchorX,1)),$([Math]::Round($anchorY,1))")
                    if (($code -eq 0xFF0000 -or $code -eq 0x0000FF) -and
                        $anchorX -ge $centerX) { $anchorsOk = $false }
                    if (($code -eq 0x00FF00 -or $code -eq 0xFFFF00) -and
                        $anchorX -le $centerX) { $anchorsOk = $false }
                    if (($code -eq 0xFF0000 -or $code -eq 0x00FF00) -and
                        $anchorY -ge $centerY) { $anchorsOk = $false }
                    if (($code -eq 0x0000FF -or $code -eq 0xFFFF00) -and
                        $anchorY -le $centerY) { $anchorsOk = $false }
                }
                if ($anchorsOk) {
                    $lastGood = $frame
                } else {
                    $failureCandidate = $frame
                    $badFrames.Add(
                        "$($frame.Name):center=${centerRgb}:$centerCount@$([Math]::Round($centerX,1)),$([Math]::Round($centerY,1));$($anchorDetail -join ',')")
                }
        }
        if ($seenCenters.Count -eq $expectedCenters.Count -and
            $badFrames.Count -eq 0 -and $lastGood) {
            Copy-Item -LiteralPath $lastGood.FullName `
                -Destination $d3dCopyImage -Force
            $d3dCopyVisualOk = $true
            Write-Output "D3dCopyVisual=ok frames=$($copyFrames.Count) end=$d3dCopyEndFrame candidates=$($candidateFrames.Count) centers=$($seenCenters.Keys -join ',') retained=$($lastGood.Name)"
        } else {
            if (-not $failureCandidate -and $copyFrames.Count) {
                $failureCandidate = $copyFrames[-1]
            }
            if ($failureCandidate) {
                Copy-Item -LiteralPath $failureCandidate.FullName `
                    -Destination $d3dCopyFailureImage -Force
            }
            Write-Output "D3dCopyVisual=failed frames=$($copyFrames.Count) end=$d3dCopyEndFrame candidates=$($candidateFrames.Count) centers=$($seenCenters.Keys -join ',') bad=$($badFrames.Count) details=$($badFrames -join '|')"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
$d3dVs11VisualOk = -not $D3dVsMode
if ($D3dVsMultiContext) {
    $allVsFrames = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object { $_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$' } |
        Sort-Object Name)
    $sceneFrames = @($allVsFrames | Where-Object {
        $_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$' -and
        $d3dVsMultiAStartFrame -ge 0 -and
        [int]$Matches[1] -ge $d3dVsMultiAStartFrame -and
        ($d3dVsMultiSurvivorEndFrame -lt 0 -or
         [int]$Matches[1] -le $d3dVsMultiSurvivorEndFrame)
    })
    $deviceAMatch = $null
    $deviceBMatch = $null
    $deviceAReturnMatch = $null
    $deviceBSurvivorMatch = $null
    $bestFailure = $null
    $bestDetail = 'no-candidate-frame'
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $sceneFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $left = $bitmap.GetPixel(
                    [int][Math]::Floor($bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $right = $bitmap.GetPixel(
                    [int][Math]::Floor(3.0 * $bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $leftRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $left.R, $left.G, $left.B
                $rightRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $right.R, $right.G, $right.B
                $bestFailure = $frame
                $bestDetail = "left=$leftRgb right=$rightRgb"
                if (-not $deviceAMatch -and
                    $leftRgb -eq '0B1D2E' -and $rightRgb -eq 'FF2080') {
                    $deviceAMatch = $frame
                } elseif ($deviceAMatch -and -not $deviceBMatch -and
                          $leftRgb -eq '4060E0' -and
                          $rightRgb -eq '0B1D2E') {
                    $deviceBMatch = $frame
                } elseif ($deviceBMatch -and -not $deviceAReturnMatch -and
                          $leftRgb -eq '0B1D2E' -and
                          $rightRgb -eq 'E0A020') {
                    $deviceAReturnMatch = $frame
                } elseif ($deviceAReturnMatch -and
                          -not $deviceBSurvivorMatch -and
                          $leftRgb -eq '20C060' -and
                          $rightRgb -eq '0B1D2E') {
                    $deviceBSurvivorMatch = $frame
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        if ($deviceAMatch -and $deviceBMatch -and $deviceAReturnMatch -and
            $deviceBSurvivorMatch) {
            Copy-Item -LiteralPath $deviceAMatch.FullName `
                -Destination $d3dVsMultiAImage -Force
            Copy-Item -LiteralPath $deviceBMatch.FullName `
                -Destination $d3dVsMultiBImage -Force
            Copy-Item -LiteralPath $deviceAReturnMatch.FullName `
                -Destination $d3dVsMultiReturnImage -Force
            Copy-Item -LiteralPath $deviceBSurvivorMatch.FullName `
                -Destination $d3dVsMultiSurvivorImage -Force
            $d3dVs11VisualOk = $true
            Write-Output "D3dVsMultiContextVisual=ok a=$($deviceAMatch.Name) b=$($deviceBMatch.Name) aReturn=$($deviceAReturnMatch.Name) bSurvivor=$($deviceBSurvivorMatch.Name) windows=$d3dVsMultiAStartFrame-$d3dVsMultiAEndFrame,$d3dVsMultiBStartFrame-$d3dVsMultiBEndFrame,$d3dVsMultiReturnStartFrame-$d3dVsMultiReturnEndFrame,$d3dVsMultiSurvivorStartFrame-$d3dVsMultiSurvivorEndFrame"
        } else {
            if ($bestFailure) {
                Copy-Item -LiteralPath $bestFailure.FullName `
                    -Destination $d3dVs11FailureImage -Force
            }
            Write-Output "D3dVsMultiContextVisual=failed frames=$($sceneFrames.Count) a=$([bool]$deviceAMatch) b=$([bool]$deviceBMatch) aReturn=$([bool]$deviceAReturnMatch) bSurvivor=$([bool]$deviceBSurvivorMatch) windows=$d3dVsMultiAStartFrame-$d3dVsMultiAEndFrame,$d3dVsMultiBStartFrame-$d3dVsMultiBEndFrame,$d3dVsMultiReturnStartFrame-$d3dVsMultiReturnEndFrame,$d3dVsMultiSurvivorStartFrame-$d3dVsMultiSurvivorEndFrame $bestDetail"
        }
    } finally {
        $allVsFrames | Remove-Item -Force -ErrorAction SilentlyContinue
    }
} elseif ($D3dVsMode) {
    $allVsFrames = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object { $_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$' } |
        Sort-Object Name)
    $sceneFrames = @($allVsFrames | Where-Object {
        $_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$' -and
        $d3dVs11ClipStartFrame -ge 0 -and
        [int]$Matches[1] -ge $d3dVs11ClipStartFrame -and
        ($d3dVs11FastEndFrame -lt 0 -or
         [int]$Matches[1] -le $d3dVs11FastEndFrame)
    })
    # Serial delivery can trail an already-published synchronous frame. Both
    # signatures are unique, so score the shared bounded scene window instead
    # of losing a boundary frame to an asynchronous marker.
    $clipFrames = $sceneFrames
    $fastFrames = $sceneFrames
    $clipMatch = $null
    $fastMatch = $null
    $bestFailure = $null
    $bestDetail = 'no-candidate-frame'
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $clipFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $left = $bitmap.GetPixel(
                    [int][Math]::Floor($bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $right = $bitmap.GetPixel(
                    [int][Math]::Floor(3.0 * $bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $leftRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $left.R, $left.G, $left.B
                $rightRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $right.R, $right.G, $right.B
                $bestFailure = $frame
                $bestDetail = "clip-left=$leftRgb clip-right=$rightRgb"
                if ($leftRgb -eq '0B1D2E' -and $rightRgb -eq 'FF2080') {
                    $clipMatch = $frame
                    break
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        foreach ($frame in $fastFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $left = $bitmap.GetPixel(
                    [int][Math]::Floor($bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $right = $bitmap.GetPixel(
                    [int][Math]::Floor(3.0 * $bitmap.Width / 4.0),
                    [int][Math]::Floor($bitmap.Height / 2.0))
                $leftRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $left.R, $left.G, $left.B
                $rightRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $right.R, $right.G, $right.B
                $bestFailure = $frame
                $bestDetail += " fast-left=$leftRgb fast-right=$rightRgb"
                if ($leftRgb -eq '0B1D2E' -and $rightRgb -eq '20C060') {
                    $fastMatch = $frame
                    break
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        if ($clipMatch -and $fastMatch) {
            Copy-Item -LiteralPath $clipMatch.FullName `
                -Destination $d3dVs11ClipImage -Force
            Copy-Item -LiteralPath $fastMatch.FullName `
                -Destination $d3dVs11FastImage -Force
            $d3dVs11VisualOk = $true
            $vsVisualLabel = if ($D3dVsSuccession) {
                'D3dVsSuccessionVisual'
            } else { 'D3dVs11Visual' }
            Write-Output "$vsVisualLabel=ok clip=$($clipMatch.Name) fast=$($fastMatch.Name) windows=$d3dVs11ClipStartFrame-$d3dVs11ClipEndFrame,$d3dVs11FastStartFrame-$d3dVs11FastEndFrame"
        } else {
            if ($bestFailure) {
                Copy-Item -LiteralPath $bestFailure.FullName `
                    -Destination $d3dVs11FailureImage -Force
            }
            $vsVisualLabel = if ($D3dVsSuccession) {
                'D3dVsSuccessionVisual'
            } else { 'D3dVs11Visual' }
            Write-Output "$vsVisualLabel=failed clipFrames=$($clipFrames.Count) fastFrames=$($fastFrames.Count) windows=$d3dVs11ClipStartFrame-$d3dVs11ClipEndFrame,$d3dVs11FastStartFrame-$d3dVs11FastEndFrame $bestDetail"
        }
    } finally {
        $allVsFrames | Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
$d3d9VisualOk = -not $D3d9
if ($D3d9) {
    $allD3d9Frames = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object {
            $_.Name -match '^frame_(\d+)x(\d+)_\d{6}\.bmp$' -and
                [int]$Matches[1] -ge 640 -and [int]$Matches[2] -ge 480
        } |
        Sort-Object Name)
    $d3d9SceneFrames = @($allD3d9Frames | Where-Object {
        if ($_.Name -notmatch '^frame_\d+x\d+_(\d{6})\.bmp$') {
            return $false
        }
        $sequence = [int]$Matches[1]
        $sequence -ge $d3d9SceneStartFrame -and
            $sequence -le $d3d9SceneEndFrame
    })
    $d3d9ResetFrames = @($allD3d9Frames | Where-Object {
        if ($_.Name -notmatch '^frame_\d+x\d+_(\d{6})\.bmp$') {
            return $false
        }
        $sequence = [int]$Matches[1]
        $sequence -ge $d3d9ResetStartFrame -and
            $sequence -le $d3d9ResetEndFrame
    })
    $d3d9CaptureDispatchOk = $d3d9SceneCaptureDispatchOk -and
        $d3d9ResetCaptureDispatchOk
    $d3d9CaptureHandshakeOk = $d3d9CaptureDispatchOk -and
        $d3d9SceneCaptureAck -and $d3d9ResetCaptureAck -and
        $d3d9SceneStartFrame -ge 0 -and
        $d3d9SceneEndFrame -ge $d3d9SceneStartFrame -and
        $d3d9ResetStartFrame -ge 0 -and
        $d3d9ResetEndFrame -ge $d3d9ResetStartFrame
    $d3d9VisualMatch = $null
    $d3d9ResetMatch = $null
    $d3d9VisualBest = $null
    $d3d9VisualBestScore = -1
    $d3d9VisualBestDetail = 'no-candidate-frame'
    $d3d9ResetBestDetail = 'no-candidate-frame'
    $d3d9VisualProfile = Get-XpD3d9VisualProfile `
        -GuestResult ([string]$result) `
        -Multistream7 ([bool]$D3d9Multistream7)
    $d3d9ExpectedScore = $d3d9VisualProfile.sceneExpected.Count
    Write-Output "D3d9VisualProfile=$($d3d9VisualProfile.name) expectedSceneScore=$d3d9ExpectedScore resetCube=$($d3d9VisualProfile.resetExpected.cube)"
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $d3d9ResetFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $reset = $bitmap.GetPixel(320, 240)
                $resetMip2 = $bitmap.GetPixel(75, 215)
                $resetCube = $bitmap.GetPixel(80, 40)
                $resetRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $reset.R, $reset.G, $reset.B
                $resetMip2Rgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $resetMip2.R, $resetMip2.G, $resetMip2.B
                $resetCubeRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $resetCube.R, $resetCube.G, $resetCube.B
                $d3d9ResetBestDetail = `
                    "background=$resetRgb mip2=$resetMip2Rgb cube=$resetCubeRgb"
                $resetAssessment = Get-XpD3d9ResetAssessment `
                    -Profile $d3d9VisualProfile -Pixels ([ordered]@{
                        background = $resetRgb
                        mip2 = $resetMip2Rgb
                        cube = $resetCubeRgb
                    })
                if ($resetAssessment.valid) {
                    $d3d9ResetMatch = $frame
                    break
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        foreach ($frame in $d3d9SceneFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $background = $bitmap.GetPixel(30, 30)
                $triangle = $bitmap.GetPixel(200, 180)
                $texture = $bitmap.GetPixel(500, 300)
                $mip2 = $bitmap.GetPixel(75, 215)
                $shader = $bitmap.GetPixel(320, 440)
                $bump = $bitmap.GetPixel(510, 80)
                $luminance = $bitmap.GetPixel(510, 440)
                $matrix = $bitmap.GetPixel(130, 440)
                $register = $bitmap.GetPixel(120, 120)
                $fixedFourth = $bitmap.GetPixel(200, 40)
                $fourth = $bitmap.GetPixel(320, 40)
                $cube = $bitmap.GetPixel(80, 40)
                $pixelShader12 = $bitmap.GetPixel(620, 35)
                $pixelShader13Depth = $bitmap.GetPixel(620, 115)
                $renderTargetNoDepth = $bitmap.GetPixel(610, 175)
                $renderTargetDepth = $bitmap.GetPixel(625, 205)
                $renderTargetClear = $bitmap.GetPixel(630, 175)
                $destinationAlpha = $bitmap.GetPixel(625, 190)
                $inverseDestinationAlpha = $bitmap.GetPixel(610, 205)
                $dynamicBefore = $bitmap.GetPixel(28, 208)
                $dynamicAfter = $bitmap.GetPixel(28, 248)
                $multistream7Pixel = $bitmap.GetPixel(380, 180)
                $reset = $bitmap.GetPixel(320, 240)
                $backgroundRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $background.R, $background.G, $background.B
                $triangleRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $triangle.R, $triangle.G, $triangle.B
                $textureRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $texture.R, $texture.G, $texture.B
                $mip2Rgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $mip2.R, $mip2.G, $mip2.B
                $shaderRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $shader.R, $shader.G, $shader.B
                $bumpRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $bump.R, $bump.G, $bump.B
                $luminanceRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $luminance.R, $luminance.G, $luminance.B
                $matrixRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $matrix.R, $matrix.G, $matrix.B
                $registerRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $register.R, $register.G, $register.B
                $fixedFourthRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $fixedFourth.R, $fixedFourth.G, $fixedFourth.B
                $fourthRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $fourth.R, $fourth.G, $fourth.B
                $cubeRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $cube.R, $cube.G, $cube.B
                $pixelShader12Rgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $pixelShader12.R, $pixelShader12.G, $pixelShader12.B
                $pixelShader13DepthRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $pixelShader13Depth.R, $pixelShader13Depth.G, `
                    $pixelShader13Depth.B
                $renderTargetNoDepthRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $renderTargetNoDepth.R, $renderTargetNoDepth.G, `
                    $renderTargetNoDepth.B
                $renderTargetDepthRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $renderTargetDepth.R, $renderTargetDepth.G, `
                    $renderTargetDepth.B
                $renderTargetClearRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $renderTargetClear.R, $renderTargetClear.G, `
                    $renderTargetClear.B
                $destinationAlphaRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $destinationAlpha.R, $destinationAlpha.G, `
                    $destinationAlpha.B
                $inverseDestinationAlphaRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $inverseDestinationAlpha.R, `
                    $inverseDestinationAlpha.G, `
                    $inverseDestinationAlpha.B
                $dynamicBeforeRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $dynamicBefore.R, $dynamicBefore.G, $dynamicBefore.B
                $dynamicAfterRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $dynamicAfter.R, $dynamicAfter.G, $dynamicAfter.B
                $multistream7Rgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $multistream7Pixel.R, $multistream7Pixel.G, `
                    $multistream7Pixel.B
                $resetRgb = '{0:X2}{1:X2}{2:X2}' -f `
                    $reset.R, $reset.G, $reset.B
                $sceneAssessment = Get-XpD3d9SceneAssessment `
                    -Profile $d3d9VisualProfile -Pixels ([ordered]@{
                        background = $backgroundRgb
                        triangle = $triangleRgb
                        texture = $textureRgb
                        mip2 = $mip2Rgb
                        shader = $shaderRgb
                        bump = $bumpRgb
                        luminance = $luminanceRgb
                        matrix = $matrixRgb
                        register = $registerRgb
                        fixedFourth = $fixedFourthRgb
                        fourth = $fourthRgb
                        cube = $cubeRgb
                        pixelShader12 = $pixelShader12Rgb
                        pixelShader13Depth = $pixelShader13DepthRgb
                        renderTargetNoDepth = $renderTargetNoDepthRgb
                        renderTargetDepth = $renderTargetDepthRgb
                        renderTargetClear = $renderTargetClearRgb
                        destinationAlpha = $destinationAlphaRgb
                        inverseDestinationAlpha = $inverseDestinationAlphaRgb
                        dynamicBefore = $dynamicBeforeRgb
                        dynamicAfter = $dynamicAfterRgb
                        multistream7 = $multistream7Rgb
                    })
                $score = $sceneAssessment.score
                if ($score -gt $d3d9VisualBestScore) {
                    $d3d9VisualBest = $frame
                    $d3d9VisualBestScore = $score
                    $d3d9VisualBestDetail = `
                        "background=$backgroundRgb triangle=$triangleRgb texture=$textureRgb mip2=$mip2Rgb shader=$shaderRgb bump=$bumpRgb luminance=$luminanceRgb matrix=$matrixRgb register=$registerRgb fixedFourth=$fixedFourthRgb fourth=$fourthRgb cube=$cubeRgb ps12=$pixelShader12Rgb ps13depth=$pixelShader13DepthRgb rttNoDepth=$renderTargetNoDepthRgb rttDepth=$renderTargetDepthRgb rttClear=$renderTargetClearRgb destAlpha=$destinationAlphaRgb invDestAlpha=$inverseDestinationAlphaRgb dynamicBefore=$dynamicBeforeRgb dynamicAfter=$dynamicAfterRgb multistream7=$multistream7Rgb reset=$resetRgb"
                }
                if (-not $d3d9VisualMatch -and $sceneAssessment.valid) {
                    $d3d9VisualMatch = $frame
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        if ($d3d9VisualMatch -and $d3d9ResetMatch -and
            $d3d9CaptureHandshakeOk) {
            Copy-Item -LiteralPath $d3d9VisualMatch.FullName `
                -Destination $d3d9VisualImage -Force
            Copy-Item -LiteralPath $d3d9ResetMatch.FullName `
                -Destination $d3d9ResetImage -Force
            $d3d9VisualOk = $true
            Write-Output "D3d9Visual=ok frame=$($d3d9VisualMatch.Name) reset=$($d3d9ResetMatch.Name) score=$d3d9ExpectedScore/$d3d9ExpectedScore multistream7=$([bool]$D3d9Multistream7) windows=$d3d9SceneStartFrame-$d3d9SceneEndFrame,$d3d9ResetStartFrame-$d3d9ResetEndFrame handshake=$d3d9CaptureHandshakeOk"
        } else {
            if ($d3d9VisualBest) {
                Copy-Item -LiteralPath $d3d9VisualBest.FullName `
                    -Destination $d3d9VisualFailureImage -Force
            }
            $resetDetail = if ($d3d9ResetMatch) {
                $d3d9ResetMatch.Name
            } else {
                "missing($d3d9ResetBestDetail)"
            }
            Write-Output "D3d9Visual=failed sceneFrames=$($d3d9SceneFrames.Count) resetFrames=$($d3d9ResetFrames.Count) matches=$d3d9VisualBestScore/$d3d9ExpectedScore reset=$resetDetail multistream7=$([bool]$D3d9Multistream7) windows=$d3d9SceneStartFrame-$d3d9SceneEndFrame,$d3d9ResetStartFrame-$d3d9ResetEndFrame handshake=$d3d9CaptureHandshakeOk dispatch=$d3d9SceneCaptureDispatchOk,$d3d9ResetCaptureDispatchOk acks=$d3d9SceneCaptureAck,$d3d9ResetCaptureAck $d3d9VisualBestDetail"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
$gamePerformanceTimingOk = -not $GamePerformance
if ($GamePerformance) {
    if ($gamePlayAt -gt [DateTime]::MinValue -and
        $gameObservationAt -lt [DateTime]::MaxValue -and
        $gameObservationAt -ge $gamePlayAt) {
        $gamePerformanceTimingOk = $true
        $observedSeconds = [Math]::Round(
            ($gameObservationAt - $gamePlayAt).TotalSeconds, 2)
        Write-Output "GamePerformance game=Quake2 capture=false observedSeconds=$observedSeconds"
    } else {
        Write-Output 'GamePerformance unavailable game=Quake2 capture=false'
    }
}
$gameVisualOk = -not $Game -or $GamePerformance
if ($GamePerformance) {
    Write-Output 'GameVisual=skipped capture-free-performance-mode'
}
if ($Game -and -not $GamePerformance) {
    $gameVisualEnd = if ($gameObservationAt -lt [DateTime]::MaxValue) {
        $gameObservationAt
    } else {
        Get-Date
    }
    $quakeFrames = if ($gamePlayAt -gt [DateTime]::MinValue) {
        @(Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -match '^frame_640x480_\d{6}\.bmp$' -and
                $_.LastWriteTime -ge $gamePlayAt.AddSeconds(1) -and
                $_.LastWriteTime -le $gameVisualEnd
            } | Sort-Object Name)
    } else {
        @()
    }
    $gameVisualMatch = $null
    $gameVisualBest = $null
    $gameVisualBestScore = -1
    $gameVisualBestDetail = 'no-candidate-frame'
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $quakeFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $colors = @{}
                $nonBlack = 0
                for ($y = 20; $y -lt 460; $y += 40) {
                    for ($x = 20; $x -lt 640; $x += 40) {
                        $pixel = $bitmap.GetPixel($x, $y)
                        $colors[$pixel.ToArgb()] = $true
                        if (($pixel.R + $pixel.G + $pixel.B) -gt 45) {
                            ++$nonBlack
                        }
                    }
                }
                $startPixel = $bitmap.GetPixel(25, 465)
                $taskPixel = $bitmap.GetPixel(300, 470)
                $desktop = ($startPixel.G -gt 100 -and
                            $startPixel.G -gt $startPixel.R) -or
                           ($taskPixel.B -gt 150 -and
                            $taskPixel.B -gt ($taskPixel.R + 80))
                $score = $colors.Count + $nonBlack
                if (-not $desktop -and $score -gt $gameVisualBestScore) {
                    $gameVisualBest = $frame
                    $gameVisualBestScore = $score
                    $gameVisualBestDetail =
                        "colors=$($colors.Count) nonBlack=$nonBlack desktop=$desktop"
                }
                if (-not $desktop -and $colors.Count -ge 6 -and
                    $nonBlack -ge 5 -and
                    (-not $gameVisualMatch -or
                     $score -gt $gameVisualMatch.Score)) {
                    $gameVisualMatch = [pscustomobject]@{
                        Frame = $frame
                        Score = $score
                    }
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        if ($gameVisualMatch) {
            Copy-Item -LiteralPath $gameVisualMatch.Frame.FullName `
                -Destination $gameVisualImage -Force
            $gameVisualOk = $true
            Write-Output "GameVisual=ok frame=$($gameVisualMatch.Frame.Name)"
        } else {
            if ($gameVisualBest) {
                Copy-Item -LiteralPath $gameVisualBest.FullName `
                    -Destination $gameVisualFailureImage -Force
            }
            Write-Output "GameVisual=failed frames=$($quakeFrames.Count) best=$($gameVisualBest.Name) $gameVisualBestDetail"
        }
        if ($RecordVideo -and $quakeFrames.Count) {
            $reviewVideo = New-GameObserveVideo -Frames $quakeFrames `
                -Destination $gameObserveVideo
            $captureImages += $reviewVideo.path
            Write-Output "ReviewVideo=$($reviewVideo.path) frames=$($reviewVideo.frames) fps=$($reviewVideo.fps) seconds=$($reviewVideo.durationSeconds) sourceSize=$($reviewVideo.sourceSize)"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
$d3dGameVisualOk = -not $D3dGame -or $D3dPerformance
if ($D3dPerformance) {
    Write-Output 'D3dGameVisual=skipped capture-free-performance-mode'
}
if ($D3dGame -and $D3dGameplay -and -not $D3dPerformance) {
    try {
        $tutorialFrames = @(Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$'
            })
        $maxPayneVisualSelection =
            Select-XpMaxPayneTutorialVisualSequence `
                -Frames $tutorialFrames -StartedAt $d3dGamePlayAt `
                -CompletedAt $d3dGameObservationAt `
                -StartSequenceExclusive $d3dGameStartFrame `
                -EndSequenceInclusive $d3dGameEndFrame `
                -OutputDirectory $vmDir
        foreach ($image in @($maxPayneVisualSelection.images)) {
            $captureImages += $image.rawPath
        }
        $d3dGameVisualOk = $maxPayneVisualSelection.valid
        Write-Output "MaxPayneVisualSequence valid=$d3dGameVisualOk selected=$($maxPayneVisualSelection.selectedCount) decodable=$($maxPayneVisualSelection.decodableFrameCount) sequence=$d3dGameStartFrame-$d3dGameEndFrame findings=$(@($maxPayneVisualSelection.findings).Count)"
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$'
            } | Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
if ($D3dGame -and -not $D3dGameplay -and -not $D3dPerformance) {
    $d3dGameVisualEnd = if ($d3dGameObservationAt -lt [DateTime]::MaxValue) {
        $d3dGameObservationAt
    } else {
        $d3dGameResultAt
    }
    $gameFrameCandidates = if ($d3dGamePlayAt -gt [DateTime]::MinValue -and
                               $d3dGameVisualEnd -lt [DateTime]::MaxValue) {
        @(Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' -and
                $_.LastWriteTime -ge $d3dGamePlayAt.AddSeconds(5) -and
                $_.LastWriteTime -le $d3dGameVisualEnd
            } | Sort-Object Name)
    } else {
        @()
    }
    # A fullscreen game is free to select a mode different from the desktop or
    # launcher.  Score the dominant captured geometry so a short-lived mode
    # transition cannot either hide the game or masquerade as stable output.
    $dominantGameFrameSize = $gameFrameCandidates | Group-Object {
        if ($_.Name -match '^frame_(\d+x\d+)_\d{6}\.bmp$') {
            $Matches[1]
        } else { 'unknown' }
    } | Sort-Object Count -Descending | Select-Object -First 1
    $gameFrames = if ($dominantGameFrameSize) {
        @($gameFrameCandidates | Where-Object {
            $_.Name -match "^frame_$([regex]::Escape($dominantGameFrameSize.Name))_\d{6}\.bmp$"
        })
    } else {
        @()
    }
    $gameVisualMatch = $null
    $gameVisualMatchScore = -1
    $gameVisualPassCount = 0
    $gameVisualBest = $null
    $gameVisualBestScore = -1
    $gameVisualBestDetail = 'no-candidate-frame'
    try {
        Add-Type -AssemblyName System.Drawing
        foreach ($frame in $gameFrames) {
            $bitmap = [Drawing.Bitmap]::new($frame.FullName)
            try {
                $colors = @{}
                $nonBlack = 0
                $quadrantNonBlack = @(0, 0, 0, 0)
                $menuRedTop = 0
                $menuRedBottom = 0
                $sampleColumns = 16
                $sampleRows = 12
                for ($sampleY = 0; $sampleY -lt $sampleRows; ++$sampleY) {
                    $y = [Math]::Min($bitmap.Height - 1,
                        [int][Math]::Floor((($sampleY + 0.5) * $bitmap.Height) / $sampleRows))
                    for ($sampleX = 0; $sampleX -lt $sampleColumns; ++$sampleX) {
                        $x = [Math]::Min($bitmap.Width - 1,
                            [int][Math]::Floor((($sampleX + 0.5) * $bitmap.Width) / $sampleColumns))
                        $pixel = $bitmap.GetPixel($x, $y)
                        $colors[$pixel.ToArgb()] = $true
                        if (($pixel.R + $pixel.G + $pixel.B) -gt 45) {
                            ++$nonBlack
                            $quadrant = 0
                            if ($x -ge ($bitmap.Width / 2)) { ++$quadrant }
                            if ($y -ge ($bitmap.Height / 2)) { $quadrant += 2 }
                            ++$quadrantNonBlack[$quadrant]
                        }
                    }
                }
                # This fixture stops at the Max Payne main menu.  Its red logo
                # is an orientation anchor in the upper-left quadrant; generic
                # color/entropy checks cannot distinguish a rich frame from
                # the same frame turned vertically upside down.
                $menuSampleColumns = 80
                $menuSampleRows = 120
                $leftWidth = [Math]::Max(1, [int][Math]::Floor($bitmap.Width / 2))
                for ($sampleY = 0; $sampleY -lt $menuSampleRows; ++$sampleY) {
                    $y = [Math]::Min($bitmap.Height - 1,
                        [int][Math]::Floor((($sampleY + 0.5) * $bitmap.Height) / $menuSampleRows))
                    for ($sampleX = 0; $sampleX -lt $menuSampleColumns; ++$sampleX) {
                        $x = [Math]::Min($leftWidth - 1,
                            [int][Math]::Floor((($sampleX + 0.5) * $leftWidth) / $menuSampleColumns))
                        $pixel = $bitmap.GetPixel($x, $y)
                        if ($pixel.R -ge 80 -and
                            $pixel.R -ge ($pixel.G * 1.35) -and
                            $pixel.R -ge ($pixel.B * 1.35)) {
                            if ($y -lt ($bitmap.Height / 2)) {
                                ++$menuRedTop
                            } else {
                                ++$menuRedBottom
                            }
                        }
                    }
                }
                $startX = [Math]::Min(25, $bitmap.Width - 1)
                $taskX = [Math]::Min(300, $bitmap.Width - 1)
                $startBottom = $bitmap.GetPixel($startX,
                    [Math]::Max(0, $bitmap.Height - 15))
                $taskBottom = $bitmap.GetPixel($taskX,
                    [Math]::Max(0, $bitmap.Height - 10))
                $startTop = $bitmap.GetPixel($startX,
                    [Math]::Min(14, $bitmap.Height - 1))
                $taskTop = $bitmap.GetPixel($taskX,
                    [Math]::Min(9, $bitmap.Height - 1))
                $desktopBottom =
                    ($startBottom.G -gt 100 -and
                     $startBottom.G -gt $startBottom.R) -or
                    ($taskBottom.B -gt 150 -and
                     $taskBottom.B -gt ($taskBottom.R + 80))
                # A vertically inverted XP desktop moves the Start button and
                # blue taskbar to the top edge.  The old one-edge check let
                # exactly that failure mode masquerade as game output.
                $desktopTop =
                    ($startTop.G -gt 100 -and
                     $startTop.G -gt $startTop.R) -or
                    ($taskTop.B -gt 150 -and
                     $taskTop.B -gt ($taskTop.R + 80))
                $desktop = $desktopBottom -or $desktopTop
                $minimumQuadrant = ($quadrantNonBlack |
                    Measure-Object -Minimum).Minimum
                $menuOrientationOk = $menuRedTop -ge 100 -and
                    $menuRedTop -ge (4 * [Math]::Max($menuRedBottom, 25))
                $score = $colors.Count + $nonBlack + 4 * $minimumQuadrant
                $detail = "size=$($bitmap.Width)x$($bitmap.Height) colors=$($colors.Count) nonBlack=$nonBlack quadrants=$($quadrantNonBlack -join ',') menuRedTop=$menuRedTop menuRedBottom=$menuRedBottom menuOrientationOk=$menuOrientationOk desktopBottom=$desktopBottom desktopTop=$desktopTop"
                if ($score -gt $gameVisualBestScore) {
                    $gameVisualBest = $frame
                    $gameVisualBestScore = $score
                    $gameVisualBestDetail = $detail
                }
                # Require rich, spatially coherent output in every quadrant
                # and see it in at least two frames.  This rejects a transient
                # launcher fragment or stale desktop while allowing menus and
                # gameplay with dark letterboxing.
                if (-not $desktop -and $colors.Count -ge 12 -and
                    $nonBlack -ge 20 -and $minimumQuadrant -ge 4 -and
                    $menuOrientationOk) {
                    ++$gameVisualPassCount
                }
                if (-not $desktop -and $colors.Count -ge 12 -and
                    $nonBlack -ge 20 -and $minimumQuadrant -ge 4 -and
                    $menuOrientationOk -and
                    $score -gt $gameVisualMatchScore) {
                    $gameVisualMatch = $frame
                    $gameVisualMatchScore = $score
                }
            } finally {
                $bitmap.Dispose()
            }
        }
        if ($gameVisualMatch -and $gameVisualPassCount -ge 2) {
            Copy-Item -LiteralPath $gameVisualMatch.FullName `
                -Destination $d3dGameVisualImage -Force
            $d3dGameVisualOk = $true
            Write-Output "D3dGameVisual=ok frame=$($gameVisualMatch.Name) stableFrames=$gameVisualPassCount"
        } else {
            if ($gameVisualBest) {
                Copy-Item -LiteralPath $gameVisualBest.FullName `
                    -Destination $d3dGameVisualFailureImage -Force
            }
            Write-Output "D3dGameVisual=failed frames=$($gameFrames.Count) stableFrames=$gameVisualPassCount best=$($gameVisualBest.Name) $gameVisualBestDetail"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}
if ($Warcraft3D3D8Oracle) {
    $warcraft3D3D8GuestContract =
        Test-XpWarcraft3D3D8GuestContract `
            -OracleStartMessages @($warcraft3D3D8OracleStartMessages) `
            -OracleCompleteMessages @($warcraft3D3D8OracleCompleteMessages) `
            -ResultMessages @($warcraft3D3D8ResultMessages) `
            -OracleStartReceivedAt $warcraft3D3D8OracleStartedAt `
            -OracleCompleteReceivedAt $warcraft3D3D8OracleCompletedAt
    $warcraft3D3D8GuestContractOk =
        $warcraft3D3D8GuestContract.valid
    Write-Output "Warcraft3D3D8GuestContract valid=$warcraft3D3D8GuestContractOk observedMs=$($warcraft3D3D8GuestContract.completeMilliseconds) hostMs=$($warcraft3D3D8GuestContract.hostMilliseconds) findings=$(@($warcraft3D3D8GuestContract.findings).Count)"
}
if ($Warcraft3D3D8Oracle) {
    if (-not $warcraft3D3D8GuestContractOk) {
        Write-Output 'Warcraft3D3D8FrameRing result=insufficient reason=invalid-guest-contract'
    } elseif ($warcraft3D3D8OracleStartedAt -eq [DateTime]::MinValue -or
        $warcraft3D3D8OracleCompletedAt -eq [DateTime]::MaxValue -or
        $warcraft3D3D8OracleCompletedAt -le
            $warcraft3D3D8OracleStartedAt) {
        Write-Output 'Warcraft3D3D8FrameRing result=insufficient reason=missing-oracle-boundary'
    } elseif (-not $warcraft3D3D8RingSnapshot) {
        Write-Output 'Warcraft3D3D8FrameRing result=insufficient reason=missing-boundary-snapshot'
    } elseif (-not (Test-Path -LiteralPath $frameRingOracle -PathType Leaf)) {
        Write-Output 'Warcraft3D3D8FrameRing result=insufficient reason=missing-analyzer'
    } else {
        $cadenceOutput = & python $frameRingOracle `
            $warcraft3D3D8RingDirectory --max-isolated-black 0 `
            --max-interior-black-runs 0 `
            --min-visible-frames 192 `
            --min-frames 256 --min-duration-seconds 1 `
            --output $warcraft3D3D8Cadence 2>&1
        $cadenceExit = $LASTEXITCODE
        if ($cadenceOutput) { Write-Output $cadenceOutput }
        if (Test-Path -LiteralPath $warcraft3D3D8Cadence -PathType Leaf) {
            $warcraft3D3D8CadenceResult = Get-Content -Raw `
                -LiteralPath $warcraft3D3D8Cadence | ConvertFrom-Json
            $warcraft3D3D8CadenceOk = $cadenceExit -eq 0
            $cadenceDisposition = if ($cadenceExit -eq 0) {
                'pass'
            } elseif ($cadenceExit -eq 1) {
                'flicker'
            } else { 'insufficient' }
            Write-Output "Warcraft3D3D8FrameRing result=$cadenceDisposition frames=$($warcraft3D3D8CadenceResult.frames) durationSeconds=$($warcraft3D3D8CadenceResult.durationSeconds) visible=$($warcraft3D3D8CadenceResult.visibleFrames) dark=$($warcraft3D3D8CadenceResult.darkFrames) black=$($warcraft3D3D8CadenceResult.blackFrames) isolatedBlack=$($warcraft3D3D8CadenceResult.isolatedBlackFrames) interiorBlackRuns=$($warcraft3D3D8CadenceResult.interiorBlackRuns) vvb=$($warcraft3D3D8CadenceResult.vvbWindows)"
            $blackWitness = @(Get-XpWarcraft3D3D8BlackRunWitness `
                -Cadence $warcraft3D3D8CadenceResult)
            foreach ($witnessFrame in $blackWitness) {
                $source = Join-Path $warcraft3D3D8RingDirectory `
                    $witnessFrame.frame.file
                $digest = (Get-FileHash -LiteralPath $source `
                    -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
                $destination = Join-Path $vmDir `
                    "xpdm-warcraft3d3d8-flicker-$($witnessFrame.label)-$digest.bmp"
                Copy-Item -LiteralPath $source -Destination $destination `
                    -Force
                $captureImages += $destination
            }
        } else {
            Write-Output "Warcraft3D3D8FrameRing result=insufficient reason=analyzer-exit-$cadenceExit"
        }
    }
}
if ($UT2003VisualOracle) {
    $ut2003VisualGuestContract = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($ut2003VisualReadyMessages) `
        -StageMessages @($ut2003VisualStageMessages) `
        -ResultMessages @($ut2003VisualResultMessages)
    $ut2003VisualGuestContractOk = $ut2003VisualGuestContract.valid
    $ut2003VisualObservedMs = $ut2003VisualGuestContract.observedMs
    $ut2003VisualBoundary = $ut2003VisualGuestContract.visualBoundary
    $boundaryLabel = if ($ut2003VisualBoundary) {
        $ut2003VisualBoundary
    } else { 'none' }
    Write-Output "UT2003VisualGuestContract valid=$ut2003VisualGuestContractOk ready=$($ut2003VisualReadyMessages.Count) stages=$($ut2003VisualStageMessages.Count) results=$($ut2003VisualResultMessages.Count) findings=$(@($ut2003VisualGuestContract.findings).Count)"
    Write-Output "UT2003VisualBoundary valid=$ut2003VisualGuestContractOk boundary=$boundaryLabel messages=$($ut2003VisualReadyMessages.Count)"
}
$gameObserveVisualOk = -not $GameObserve -or $ObservePerformance
$gameObserveTimingOk = if ($Warcraft3D3D8Oracle) {
    $warcraft3D3D8GuestContractOk
} elseif ($UT2003VisualOracle) {
    $ut2003VisualGuestContractOk
} else {
    -not $ObservePerformance
}
if ($UT2003VisualOracle) {
    Write-Output "UT2003VisualTiming valid=$gameObserveTimingOk observedMs=$ut2003VisualObservedMs requiredMs=180000 boundary=post-visual-ready"
}
if ($GameObserve -and $ObservePerformance) {
    if ($gameObserveStartedAt -ne [DateTime]::MinValue -and
        $gameObserveCompletedAt -ne [DateTime]::MaxValue -and
        $gameObserveCompletedAt -ge $gameObserveStartedAt) {
        $gameObserveTimingOk = $true
        $observedSeconds = [Math]::Round(
            ($gameObserveCompletedAt - $gameObserveStartedAt).TotalSeconds, 2)
        Write-Output "GameObservePerformance game=$ObserveGame capture=false observedSeconds=$observedSeconds"
    } else {
        Write-Output "GameObservePerformance unavailable game=$ObserveGame capture=false"
    }
    Write-Output "GameObserveVisual=skipped game=$ObserveGame mode=capture-free-performance"
}
if ($GameObserve -and -not $ObservePerformance) {
    $observeFrames = if ($Warcraft3D3D8Oracle) {
        @($warcraft3D3D8RingFrames)
    } else {
        @(Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object {
                if ($gameObserveStartedAt -eq [DateTime]::MinValue -and
                    $gameObserveStartFrame -lt 0) {
                    return $false
                }
                return Test-XpGameObserveFrameInterval -Frame $_ `
                    -StartedAt $gameObserveStartedAt `
                    -CompletedAt $gameObserveCompletedAt `
                    -StartSequence $gameObserveStartFrame `
                    -EndSequence $gameObserveEndFrame `
                    -StrictStartTime:$UT2003VisualOracle
            } | Sort-Object LastWriteTime, Name)
    }
    if ($UT2003VisualOracle -and $observeFrames.Count) {
        $ut2003FirstAcceptedFrame = [int](($observeFrames |
            ForEach-Object {
                if ($_.Name -match '^frame_\d+x\d+_(\d{6})\.bmp$') {
                    [int]$Matches[1]
                }
            } | Measure-Object -Minimum).Minimum)
    }
    if ($UT2003VisualOracle) {
        Write-Output "UT2003CaptureBoundary readySequence=$ut2003VisualReadyFrame lastGuardSequence=$ut2003GuardFrame firstAcceptedSequence=$ut2003FirstAcceptedFrame guardCount=2 forceSyncReadback=True"
    }
    Write-Output "GameObserveFrames game=$ObserveGame count=$($observeFrames.Count) sequence=$gameObserveStartFrame-$gameObserveEndFrame time=$($gameObserveStartedAt.ToString('o'))-$($gameObserveCompletedAt.ToString('o'))"
    try {
        if ($observeFrames.Count) {
            Get-ChildItem -LiteralPath $vmDir -File |
                Where-Object {
                    $_.Name -like "$gameObserveSamplePrefix-*.bmp"
                } | Remove-Item -Force -ErrorAction SilentlyContinue
            Add-Type -AssemblyName System.Drawing
            # A one-off splash or post-close desktop must not outweigh the
            # sustained renderer output. Select only the most frequent frame
            # geometry, then require non-black, non-desktop complexity before
            # RGB distance can choose a representative.
            $geometryGroups = @($observeFrames | Group-Object {
                if ($_.Name -match '^frame_(\d+)x(\d+)_\d{6}\.bmp$') {
                    "$($Matches[1])x$($Matches[2])"
                } else { 'unknown' }
            } | ForEach-Object {
                $parts = $_.Name -split 'x'
                [pscustomobject]@{
                    name = $_.Name
                    count = $_.Count
                    area = if ($parts.Count -eq 2) {
                        [int64]$parts[0] * [int64]$parts[1]
                    } else { 0L }
                    frames = @($_.Group)
                }
            } | Sort-Object -Property `
                @{ Expression = 'count'; Descending = $true },
                @{ Expression = 'area'; Descending = $true },
                @{ Expression = 'name'; Descending = $false })
            $visualFrames = @($geometryGroups[0].frames |
                Sort-Object LastWriteTime, Name)
            $sampleFrames = @(Select-XpGameObserveSampleFrames `
                -Frames $visualFrames -Required 8)
            if ($UT2003VisualOracle) {
                $ut2003VisualCoverage =
                    Test-XpUT2003VisualFrameCoverage `
                        -SelectedFrames $sampleFrames `
                        -RequiredObservedMs 180000
                $ut2003VisualCoverageOk =
                    [bool]$ut2003VisualCoverage.valid
                $coverageCulture =
                    [Globalization.CultureInfo]::InvariantCulture
                $coverageSpan =
                    ([double]$ut2003VisualCoverage.spanSeconds).ToString(
                        '0.###', $coverageCulture)
                $coverageRequired =
                    ([double]$ut2003VisualCoverage.requiredSpanSeconds).ToString(
                        '0.###', $coverageCulture)
                $coverageMaximumGap =
                    ([double]$ut2003VisualCoverage.maximumGapSeconds).ToString(
                        '0.###', $coverageCulture)
                $coverageAllowedGap =
                    ([double]$ut2003VisualCoverage.maximumAllowedGapSeconds).ToString(
                        '0.###', $coverageCulture)
                $coverageMinimumChanged =
                    ([double]$ut2003VisualCoverage.minimumChangedPercent).ToString(
                        '0.###', $coverageCulture)
                $coverageMaximumChanged =
                    ([double]$ut2003VisualCoverage.maximumChangedPercent).ToString(
                        '0.###', $coverageCulture)
                Write-Output "UT2003VisualCoverage valid=$ut2003VisualCoverageOk selected=$($ut2003VisualCoverage.selectedCount) decoded=$($ut2003VisualCoverage.decodedCount) canonicalDistinctHashes=$($ut2003VisualCoverage.canonicalDistinctHashes) requiredCanonicalDistinctHashes=$($ut2003VisualCoverage.requiredCanonicalDistinctHashes) meaningfulTransitions=$($ut2003VisualCoverage.meaningfulTransitions) requiredMeaningfulTransitions=$($ut2003VisualCoverage.requiredMeaningfulTransitions) minimumChangedPercent=$coverageMinimumChanged maximumChangedPercent=$coverageMaximumChanged spanSeconds=$coverageSpan requiredSpanSeconds=$coverageRequired maximumGapSeconds=$coverageMaximumGap maximumAllowedGapSeconds=$coverageAllowedGap"
                $ut2003MenuVisual = Test-XpUT2003MainMenuCoverage `
                    -VisualEvidence @(
                        $ut2003VisualCoverage.visualEvidence)
                $ut2003MenuVisualOk = [bool]$ut2003MenuVisual.valid
                Write-Output "UT2003MenuVisual valid=$ut2003MenuVisualOk selected=$($ut2003MenuVisual.selectedCount) qualifyingFrames=$($ut2003MenuVisual.qualifyingFrames) requiredFrames=$($ut2003MenuVisual.requiredFrames) signatureStep=8 geometry=640x480"
                $menuTransition = if ($ut2003MenuVisualOk) {
                    'main-menu'
                } else { 'none' }
                Write-Output "UT2003Semantic valid=$ut2003MenuVisualOk transition=$menuTransition evidence=manifest-bound-menu-layout qualifyingFrames=$($ut2003MenuVisual.qualifyingFrames) requiredFrames=$($ut2003MenuVisual.requiredFrames) boundary=post-visual-ready"
            }
            $sampleNames = @()
            if ($sampleFrames.Count -eq 8) {
                for ($sampleIndex = 0; $sampleIndex -lt 8; ++$sampleIndex) {
                    $sampleDigest = (Get-FileHash `
                        -LiteralPath $sampleFrames[$sampleIndex].FullName `
                        -Algorithm SHA256).Hash.Substring(0, 12).ToLowerInvariant()
                    $sampleName =
                        "$gameObserveSamplePrefix-$sampleIndex-$sampleDigest.bmp"
                    $samplePath = Join-Path $vmDir $sampleName
                    Copy-Item -LiteralPath `
                        $sampleFrames[$sampleIndex].FullName `
                        -Destination $samplePath -Force
                    $sampleNames += $sampleName
                    if ($Warcraft3D3D8Oracle) {
                        $captureImages += $samplePath
                    }
                }
                Write-Output "GameObserveSamples=$($sampleNames -join ',')"
            } else {
                Write-Output "GameObserveSamples=insufficient qualified=$($sampleFrames.Count) required=8"
            }
            $representative = $visualFrames[-1]
            $representativeDistance = 0L
            $representativeScore = -1L
            $referenceBitmap = [Drawing.Bitmap]::new(
                $visualFrames[-1].FullName)
            try {
                foreach ($candidate in $visualFrames) {
                    $candidateBitmap = [Drawing.Bitmap]::new(
                        $candidate.FullName)
                    try {
                        $distance = 0L
                        for ($y = 0; $y -lt $candidateBitmap.Height; $y += 32) {
                            for ($x = 0; $x -lt $candidateBitmap.Width; $x += 32) {
                                $pixel = $candidateBitmap.GetPixel($x, $y)
                                $referencePixel = $referenceBitmap.GetPixel($x, $y)
                                $distance += [Math]::Abs(
                                    [int]$pixel.R - [int]$referencePixel.R)
                                $distance += [Math]::Abs(
                                    [int]$pixel.G - [int]$referencePixel.G)
                                $distance += [Math]::Abs(
                                    [int]$pixel.B - [int]$referencePixel.B)
                            }
                        }
                        $assessment = Get-XpGameObserveBitmapAssessment `
                            -Bitmap $candidateBitmap
                        $score = if ($assessment.rich) {
                            $distance + 64L * $assessment.colors
                        } else { -1L }
                        if ($score -gt $representativeScore) {
                            $representativeScore = $score
                            $representativeDistance = $distance
                            $representative = $candidate
                        }
                    } finally {
                        $candidateBitmap.Dispose()
                    }
                }
            } finally {
                $referenceBitmap.Dispose()
            }
            $bitmap = [Drawing.Bitmap]::new($representative.FullName)
            try {
                $assessment = Get-XpGameObserveBitmapAssessment `
                    -Bitmap $bitmap
                Copy-Item -LiteralPath $representative.FullName `
                    -Destination $gameObserveImage -Force
                $gameObserveVisualOk = $representativeScore -ge 0 -and
                    $assessment.rich -and $sampleFrames.Count -eq 8 -and
                    $ut2003VisualCoverageOk -and $ut2003MenuVisualOk
                $visualDisposition = if ($gameObserveVisualOk) {
                    'recorded'
                } else { 'failed' }
                Write-Output "GameObserveVisual=$visualDisposition game=$ObserveGame frame=$($representative.Name) geometry=$($geometryGroups[0].name) geometryFrames=$($visualFrames.Count) distance=$representativeDistance score=$representativeScore blackPercent=$($assessment.blackPercent) colors=$($assessment.colors) desktop=$($assessment.desktop)"
            } finally {
                $bitmap.Dispose()
            }
            if ($RecordVideo) {
                if ($Warcraft3D3D8Oracle) {
                    try {
                        $ringVideo = New-Warcraft3D3D8RingVideo `
                            -Frames $observeFrames `
                            -Destination $gameObserveVideo
                        $captureImages += $ringVideo.path
                        $warcraft3D3D8VideoOk = $true
                        Write-Output "ReviewVideo=$($ringVideo.path) sourceFrames=$($ringVideo.sourceFrames) encodedFrames=$($ringVideo.encodedFrames) fps=$($ringVideo.fps) seconds=$($ringVideo.durationSeconds) sourceSize=$($ringVideo.sourceSize) nonDecimated=True"
                    } catch {
                        $warcraft3D3D8VideoOk = $false
                        Write-Output "ReviewVideo=failed nonDecimated=True reason=$($_.Exception.Message.Replace(' ', '-'))"
                    }
                } else {
                    $reviewVideo = New-GameObserveVideo `
                        -Frames $observeFrames `
                        -Destination $gameObserveVideo
                    $captureImages += $reviewVideo.path
                    Write-Output "ReviewVideo=$($reviewVideo.path) frames=$($reviewVideo.frames) fps=$($reviewVideo.fps) seconds=$($reviewVideo.durationSeconds) sourceSize=$($reviewVideo.sourceSize)"
                }
            }
        } else {
            Write-Output "GameObserveVisual=missing game=$ObserveGame"
        }
    } finally {
        Get-ChildItem -LiteralPath $vmDir -File |
            Where-Object { $_.Name -match '^frame_\d+x\d+_\d{6}\.bmp$' } |
            Remove-Item -Force -ErrorAction SilentlyContinue
        if ($Warcraft3D3D8Oracle) {
            Exit-XpWarcraft3D3D8RingSnapshot `
                -Snapshot $warcraft3D3D8RingSnapshot
            $warcraft3D3D8RingSnapshot = $null
            Get-ChildItem -LiteralPath $vmDir -File |
                Where-Object {
                    $_.Name -match '^frame-ring-[0-9]+\.bmp$'
                } | Remove-Item -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $warcraft3D3D8RingDirectory `
                -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
# Freeze visual artifacts as soon as their mode-specific processing finishes.
# Final publication repeats this idempotently, but this explicit copy prevents
# a later profile/result failure from stranding already-proven evidence in the
# shared VM directory. Reconstruct each capture set from its exact run-cleaned
# namespace instead of trusting mutable-array additions across PowerShell
# pipeline scopes.
if ($GameObserve -and -not $ObservePerformance -and
    -not $Warcraft3D3D8Oracle) {
    $observeArtifactStem =
        "xpdm-$($ObserveGame.ToLowerInvariant())"
    $captureImages = @(Get-XpAcceptanceGameObserveCapturePaths `
        -Directory $vmDir -Game $ObserveGame)
    $gameObserveSourceSetOk =
        Test-XpAcceptanceGameObserveCaptureNames `
            -Names @($captureImages | ForEach-Object {
                [IO.Path]::GetFileName([string]$_)
            }) -Game $ObserveGame
    Write-Output "GameObserveArtifactNamespace stem=$observeArtifactStem files=$($captureImages.Count) valid=$gameObserveSourceSetOk"
} elseif ($Game -and $RecordVideo) {
    $captureImages = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object {
            $_.Name -in @('xpdm-quake2.bmp', 'xpdm-quake2-failed.bmp') -or
            $_.Name -cmatch '^xpdm-quake2-review-[0-9a-f]{12}\.mp4$'
        } | Select-Object -ExpandProperty FullName)
    Write-Output "GameVisualArtifactNamespace files=$($captureImages.Count)"
}
if ($Warcraft3D3D8Oracle) {
    $captureImages = @(Get-ChildItem -LiteralPath $vmDir -File |
        Where-Object {
            $_.Name -eq 'xpdm-warcraft3d3d8-baseline.bmp' -or
            $_.Name -eq 'xpdm-warcraft3d3d8-black-cadence.json' -or
            $_.Name -like 'xpdm-warcraft3d3d8-sample-*.bmp' -or
            $_.Name -like 'xpdm-warcraft3d3d8-flicker-*.bmp' -or
            $_.Name -like 'xpdm-warcraft3d3d8-review-*.mp4'
        } | Select-Object -ExpandProperty FullName)
}
if ($artifactContext -and @($captureImages).Count) {
    $captureArtifactsCopied = 0
    foreach ($capturePath in @($captureImages)) {
        if ($capturePath -and
            (Test-Path -LiteralPath $capturePath -PathType Leaf)) {
            [void](Copy-XpAcceptanceRunArtifact -Context $artifactContext `
                -Source ([string]$capturePath) `
                -Name ([IO.Path]::GetFileName([string]$capturePath)))
            ++$captureArtifactsCopied
        }
    }
    Write-Output "CaptureArtifacts copied=$captureArtifactsCopied planned=$(@($captureImages).Count)"
}
if ($gameObserveArtifactSetRequired -and $artifactContext) {
    $gameObserveArtifactNames = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $artifactContext | ForEach-Object { [string]$_.name })
    $gameObserveArtifactSetOk = $gameObserveSourceSetOk -and
        (Test-XpAcceptanceGameObserveCaptureNames `
            -Names $gameObserveArtifactNames -Game $ObserveGame)
    Write-Output "GameObserveArtifactSet source=$gameObserveSourceSetOk artifacts=$gameObserveArtifactSetOk"
}
if ($RecordVideo -and $artifactContext) {
    $reviewPrefix = if ($GameObserve) {
        "xpdm-$($ObserveGame.ToLowerInvariant())-review-"
    } else {
        'xpdm-quake2-review-'
    }
    $reviewVideoSources = @($captureImages | Where-Object {
        $_ -and
        [IO.Path]::GetFileName([string]$_) -cmatch
            ('^{0}[0-9a-f]{{12}}\.mp4$' -f
                [regex]::Escape($reviewPrefix)) -and
        (Test-Path -LiteralPath ([string]$_) -PathType Leaf)
    })
    $reviewVideoRecords = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $artifactContext)
    $reviewVideoArtifactOk = $reviewVideoSources.Count -eq 1 -and
        (Test-XpAcceptanceReviewVideoRecords `
            -Records $reviewVideoRecords -NamePrefix $reviewPrefix)
    $reviewVideoArtifactCount = @($reviewVideoRecords | Where-Object {
        ([string]$_.name) -like "$reviewPrefix*.mp4"
    }).Count
    Write-Output "ReviewVideoArtifact valid=$reviewVideoArtifactOk sources=$($reviewVideoSources.Count) artifacts=$reviewVideoArtifactCount prefix=$reviewPrefix"
}
if (Test-Path -LiteralPath $debugconSource) {
    Copy-Item -LiteralPath $debugconSource -Destination $driverLog -Force
}
$profileSource = if (Test-Path -LiteralPath $driverLog -PathType Leaf) {
    $driverLog
} elseif (Test-Path -LiteralPath $debugconSource -PathType Leaf) {
    $debugconSource
} else { $null }
if ($profileSource) {
    $d3dDriverProfile = ConvertFrom-XpD3dDriverProfile `
        -Text (Get-Content -Raw -LiteralPath $profileSource)
    $d3dDriverProfileOk = $d3dDriverProfile.status -eq 'absent' -or
        $d3dDriverProfile.valid
    if ($d3dDriverProfile.status -ne 'absent') {
        Write-Output "D3dDriverProfile status=$($d3dDriverProfile.status) valid=$($d3dDriverProfile.valid) displayBlocks=$($d3dDriverProfile.selection.displayBlocks) miniportBlocks=$($d3dDriverProfile.selection.miniportBlocks) logicalDraws=$($d3dDriverProfile.miniport.logicalDraws) copySeconds=$($d3dDriverProfile.derived.interleavedCopySeconds) copyPercentSubmit=$($d3dDriverProfile.derived.interleavedCopyPercentOfSubmit)"
    }
    if ($Warcraft3D3D8Oracle) {
        $warcraftCounterCheck = Test-XpWarcraft3D3D8CounterProfile `
            -Profile $d3dDriverProfile
        $warcraft3D3D8CountersOk = $warcraftCounterCheck.valid
        $miniportCounters = $d3dDriverProfile.miniport
        Write-Output "Warcraft3D3D8Counters valid=$warcraft3D3D8CountersOk presents=$($miniportCounters.presents) sourceCounters=$($miniportCounters.presentationSourceCountersPresent) cpu=$($miniportCounters.cpuPresents) surfaceTexture=$($miniportCounters.surfaceTexturePresents) empty=$($miniportCounters.emptyPresents) copies=$($miniportCounters.surfaceCopies) readbacks=$($miniportCounters.surfaceReadbacks) findings=$(@($warcraftCounterCheck.findings).Count)"
    }
}
if ($Warcraft3D3D8Oracle) {
    $warcraftModuleOk = $warcraft3D3D8GuestContractOk -and
        (($warcraft3D3D8GuestContract.moduleMask -band 1) -ne 0)
    Write-Output "Warcraft3D3D8Module=$(if ($warcraftModuleOk) { 'ok' } else { 'failed' }) renderer=d3d8 system32=$([int]$warcraftModuleOk)"
}
$installIdentityOk = -not $InstallOnly -or
    ($packageIdentity -and $packageIdentity.valid -and
     -not $installIdentityConflict -and
     $installDriverVer -eq $packageIdentity.driverVer -and
     $installSignerThumbprint -eq $packageIdentity.certificate.thumbprint)
if ($InstallOnly) {
    Write-Output "InstallIdentity valid=$installIdentityOk driverVer=$installDriverVer signerThumbprint=$installSignerThumbprint packageDriverVer=$($packageIdentity.driverVer) packageSignerThumbprint=$($packageIdentity.certificate.thumbprint) conflictingReports=$installIdentityConflict"
}
if (-not $result -and $lastGuestFailure) { $result = $lastGuestFailure }
if ($D3dHardware) {
    Write-Output "D3dHardwareVpOk=$hardwareVpOk"
}
if ($D3dVisual) {
    Write-Output "D3dStateBlockOk=$stateBlockOk"
}
Write-Output "GuestResult=$result"
Write-Output "QmpStatus=$qmpStatus"
Write-Output "TeardownOk=$teardown"
Write-Output "HostLog=$stderr"
if (Test-Path -LiteralPath $driverLog) {
    Write-Output "DriverLog=$driverLog"
}

$expectedResult = if ($InstallOnly) {
    'QXPICD_INSTALL_READY'
} elseif ($Game) {
    $null
} elseif ($D3dAnyProbe -or $D3dClean -or $D3dGame -or $GameObserve) {
    $null
} else {
    'QXPICD_RESULT rc=0'
}
$resultOk = if ($Game) {
    $result -and $result.StartsWith('QXP_GAME_RESULT rc=0 ')
} elseif ($D3dProbe) {
    $result -and $result.StartsWith('QXP_D3D_RESULT rc=0 ')
} elseif ($D3dVsBenchmark) {
    $result -and $result.StartsWith('QXP_D3DVS_BENCH_RESULT rc=0 ')
} elseif ($D3dVsMultiContext) {
    $result -and $result.StartsWith('QXP_D3DVS_MULTI_RESULT rc=0 ')
} elseif ($D3dVsSuccession) {
    $result -and
        $result.StartsWith('QXP_D3DVS_SUCCESSION_RESULT rc=0 ')
} elseif ($D3dVs11) {
    $result -and $result.StartsWith('QXP_D3DVS_RESULT rc=0 ')
} elseif ($D3d9SelfTest) {
    $result -and
        $result.StartsWith('QXP_D3D9_SELFTEST_RESULT rc=0 ')
} elseif ($D3d9) {
    $result -and $result.StartsWith('QXP_D3D9_RESULT rc=0 ')
} elseif ($D3dClean) {
    $result -and $result.StartsWith('QXP_D3D_CLEAN rc=0 ')
} elseif ($D3dGame) {
    $result -and $result.StartsWith('QXP_D3D_GAME_RESULT rc=0 ') -and
        (-not $D3dNormalLaunch -or $result.Contains('launch=normal'))
} elseif ($GameObserve) {
    $result -and $result.StartsWith('QXP_GAME_OBSERVE_RESULT rc=0 ') -and
        (-not $UT2003VisualOracle -or $ut2003VisualGuestContractOk) -and
        (-not $Warcraft3D3D8Oracle -or
         $warcraft3D3D8GuestContractOk)
} else {
    $result -eq $expectedResult
}
$acceptanceOk = $resultOk -and $teardown -and $visualOk -and
    $d3dCopyVisualOk -and $hardwareVpOk -and $stateBlockOk -and
    $hwtlReuseOk -and $legacyIndexedDp2Ok -and $ignoredStateChangeOk -and
    $d3dGameTransportOk -and $gameVisualOk -and $d3dGameVisualOk -and
    $gameObserveVisualOk -and $gameObserveTimingOk -and
    $reviewVideoArtifactOk -and $gameObserveArtifactSetOk -and
    $ut2003VisualCoverageOk -and $ut2003MenuVisualOk -and
    $warcraft3D3D8GuestContractOk -and
    $warcraft3D3D8CadenceOk -and $warcraft3D3D8VideoOk -and
    $warcraft3D3D8CountersOk -and
    $gameObserveHostProfileOk -and
    $gamePerformanceTimingOk -and
    $performanceFrameProfileOk -and $d3dVsBenchmarkTransportOk -and
    $d3dVs11TransportOk -and $d3dVs11VisualOk -and
    $d3d9TransportOk -and $d3d9VisualOk -and $installIdentityOk -and
    $d3dDriverProfileOk
$harnessStatus = if ($acceptanceOk) { 'passed' } else { 'failed' }
$guestCompleted = -not [string]::IsNullOrWhiteSpace($result)
if ($artifactContext) {
    Publish-XpAcceptanceCurrentRunArtifacts -Context $artifactContext `
        -Status $harnessStatus `
        -ExitCode $(if ($acceptanceOk) { 0 } else { 1 }) `
        -CaptureImages @($captureImages)
    $artifactPublished = $true
}
Exit-XpAcceptanceArtifactLease -Lease $artifactLease -Status $harnessStatus
$totalSeconds = [Math]::Round(((Get-Date) - $runStartedAt).TotalSeconds, 2)
Write-Output "HarnessRun runId=$RunId status=$harnessStatus totalSeconds=$totalSeconds guestTestStarted=$guestTestStarted guestCompleted=$guestCompleted lastGuestMessage=$lastGuestMessage lease=$artifactLeasePath"
if (-not $acceptanceOk) {
    $harnessExitCode = 1
}
} catch {
    $harnessTerminatingError = $_
    throw
} finally {
    if ($artifactLease -and $artifactLease.Stream) {
        $hostStopped = $true
        if ($process) {
            try {
                $process.Refresh()
                if (-not $process.HasExited) {
                    Stop-Process -Id $process.Id -Force -ErrorAction Stop
                    Wait-Process -Id $process.Id -Timeout 10 `
                        -ErrorAction SilentlyContinue
                    $process.Refresh()
                }
                $hostStopped = $process.HasExited
            } catch {
                $hostStopped = $false
                Write-Warning "Exceptional host cleanup failed: $_"
            }
        }
        if ($warcraft3D3D8RingSnapshot) {
            try {
                Exit-XpWarcraft3D3D8RingSnapshot `
                    -Snapshot $warcraft3D3D8RingSnapshot
                $warcraft3D3D8RingSnapshot = $null
            } catch {
                Write-Warning "Exceptional frame-ring cleanup failed: $_"
            }
        }
        foreach ($resource in @(
                $serialStream, $udp,
                $frameProfileStartEvent, $frameProfileStopEvent)) {
            if ($resource) {
                try { $resource.Dispose() } catch {
                    Write-Warning "Exceptional resource cleanup failed: $_"
                }
            }
        }
        $failureType = if ($harnessTerminatingError) {
            $harnessTerminatingError.Exception.GetType().FullName
        } else {
            'XPDriver.AcceptanceIncomplete'
        }
        $failureMessage = if ($harnessTerminatingError) {
            $harnessTerminatingError.Exception.Message.Replace("`r", ' ').Replace("`n", ' ')
        } else {
            'Acceptance runner ended before normal finalization'
        }
        $artifactLease.Metadata.failureType = $failureType
        $artifactLease.Metadata.failureMessage = $failureMessage
        $artifactLease.Metadata.cleanupHostStopped = $hostStopped
        if ($artifactContext) {
            try {
                Publish-XpAcceptanceCurrentRunArtifacts `
                    -Context $artifactContext -Status failed -ExitCode 1 `
                    -FailureType $failureType `
                    -FailureMessage $failureMessage `
                    -CleanupHostStopped $hostStopped `
                    -CaptureImages @($captureImages)
                $artifactPublished = $true
            } catch {
                Write-Warning "Exceptional artifact publication failed: $_"
            }
        }
        try {
            Exit-XpAcceptanceArtifactLease -Lease $artifactLease `
                -Status failed
            $totalSeconds = [Math]::Round(
                ((Get-Date) - $harnessStartedAt).TotalSeconds, 2)
            $guestCompleted = -not [string]::IsNullOrWhiteSpace($result)
            Write-Output "HarnessRun runId=$RunId status=failed totalSeconds=$totalSeconds guestTestStarted=$guestTestStarted guestCompleted=$guestCompleted lastGuestMessage=$lastGuestMessage cleanupHostStopped=$hostStopped failureType=$failureType failureMessage=$failureMessage lease=$artifactLeasePath"
        } catch {
            Write-Warning "Exceptional lease finalization failed: $_"
        }
    }
}
if ($harnessExitCode -ne 0) {
    exit $harnessExitCode
}
