$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'host-process-profile.ps1')

function Assert-HostProcessProfile {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function New-HostProcessSnapshotFixture {
    param(
        [long]$Timestamp,
        [long]$TotalCpuTicks,
        [long]$UserCpuTicks,
        [long]$KernelCpuTicks,
        [object[]]$Threads,
        [int]$ProcessId = 42,
        [string]$ProcessStartAtUtc = '2026-07-18T12:00:00.0000000Z',
        [switch]$IncompleteThreads
    )

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-host-process-cpu-snapshot'
        valid = $true
        error = $null
        capturedAtUtc = if ($Timestamp -eq 1000) {
            '2026-07-18T12:01:00.0000000Z'
        } else {
            '2026-07-18T12:01:45.0000000Z'
        }
        monotonicTimestamp = $Timestamp
        monotonicFrequency = 100
        captureDurationMilliseconds = 1.25
        processId = $ProcessId
        processStartAtUtc = $ProcessStartAtUtc
        totalCpuTicks = $TotalCpuTicks
        userCpuTicks = $UserCpuTicks
        kernelCpuTicks = $KernelCpuTicks
        threadEnumerationComplete = -not $IncompleteThreads
        threadFailures = if ($IncompleteThreads) { 1 } else { 0 }
        threads = @($Threads)
    }
}

function New-HostThreadFixture {
    param(
        [int]$Id,
        [string]$StartAtUtc,
        [long]$TotalCpuTicks,
        [long]$UserCpuTicks,
        [long]$KernelCpuTicks
    )

    [pscustomobject][ordered]@{
        id = $Id
        startAtUtc = $StartAtUtc
        totalCpuTicks = $TotalCpuTicks
        userCpuTicks = $UserCpuTicks
        kernelCpuTicks = $KernelCpuTicks
    }
}

$ticksPerSecond = [TimeSpan]::TicksPerSecond
$threadOneStart = '2026-07-18T12:00:01.0000000Z'
$threadTwoStart = '2026-07-18T12:00:02.0000000Z'
$threadThreeStart = '2026-07-18T12:00:03.0000000Z'
$start = New-HostProcessSnapshotFixture -Timestamp 1000 `
    -TotalCpuTicks (10 * $ticksPerSecond) `
    -UserCpuTicks (8 * $ticksPerSecond) `
    -KernelCpuTicks (2 * $ticksPerSecond) -Threads @(
        (New-HostThreadFixture -Id 101 -StartAtUtc $threadOneStart `
            -TotalCpuTicks (4 * $ticksPerSecond) `
            -UserCpuTicks (3 * $ticksPerSecond) `
            -KernelCpuTicks (1 * $ticksPerSecond)),
        (New-HostThreadFixture -Id 102 -StartAtUtc $threadTwoStart `
            -TotalCpuTicks (3 * $ticksPerSecond) `
            -UserCpuTicks (2 * $ticksPerSecond) `
            -KernelCpuTicks (1 * $ticksPerSecond)),
        (New-HostThreadFixture -Id 103 -StartAtUtc $threadThreeStart `
            -TotalCpuTicks (1 * $ticksPerSecond) `
            -UserCpuTicks (1 * $ticksPerSecond) -KernelCpuTicks 0)
    )
$stop = New-HostProcessSnapshotFixture -Timestamp 5500 `
    -TotalCpuTicks (70 * $ticksPerSecond) `
    -UserCpuTicks (48 * $ticksPerSecond) `
    -KernelCpuTicks (22 * $ticksPerSecond) -Threads @(
        (New-HostThreadFixture -Id 101 -StartAtUtc $threadOneStart `
            -TotalCpuTicks (34 * $ticksPerSecond) `
            -UserCpuTicks (23 * $ticksPerSecond) `
            -KernelCpuTicks (11 * $ticksPerSecond)),
        (New-HostThreadFixture -Id 102 -StartAtUtc $threadTwoStart `
            -TotalCpuTicks (23 * $ticksPerSecond) `
            -UserCpuTicks (16 * $ticksPerSecond) `
            -KernelCpuTicks (7 * $ticksPerSecond)),
        (New-HostThreadFixture -Id 104 `
            -StartAtUtc '2026-07-18T12:01:10.0000000Z' `
            -TotalCpuTicks (5 * $ticksPerSecond) `
            -UserCpuTicks (4 * $ticksPerSecond) `
            -KernelCpuTicks (1 * $ticksPerSecond))
    )
$profile = New-XpHostProcessCpuProfile -Start $start -Stop $stop `
    -ProcessorCount 8
Assert-HostProcessProfile $profile.valid 'matched fixture must be valid'
Assert-HostProcessProfile ($profile.status -eq 'valid') `
    'complete fixture must have valid status'
Assert-HostProcessProfile ($profile.window.wallSeconds -eq 45) `
    'wall time must use monotonic timestamps'
Assert-HostProcessProfile ($profile.process.totalCpuSeconds -eq 60) `
    'process total CPU delta must be exact'
Assert-HostProcessProfile ($profile.process.userCpuSeconds -eq 40) `
    'process user CPU delta must be exact'
Assert-HostProcessProfile ($profile.process.kernelCpuSeconds -eq 20) `
    'process kernel CPU delta must be exact'
Assert-HostProcessProfile `
    ([Math]::Abs($profile.process.averageUtilizedCores - 1.333333) -lt
        0.000001) 'average utilized cores must be CPU/wall'
Assert-HostProcessProfile `
    ([Math]::Abs($profile.process.hostCapacityPercent - 16.667) -lt 0.001) `
    'host capacity percent must normalize by logical processor count'
Assert-HostProcessProfile ($profile.threads.stableCount -eq 2) `
    'only stable thread identities must have deltas'
Assert-HostProcessProfile ($profile.threads.startedCount -eq 1) `
    'new threads must be reported'
Assert-HostProcessProfile ($profile.threads.exitedCount -eq 1) `
    'exited threads must be reported'
Assert-HostProcessProfile ($profile.threads.stableCpuSeconds -eq 50) `
    'stable thread CPU sum must be exact'
Assert-HostProcessProfile ($profile.threads.stableCoveragePercent -eq 83.333) `
    'stable thread coverage must be relative to process CPU'
Assert-HostProcessProfile ($profile.threads.top[0].id -eq 101) `
    'top stable thread must be sorted by CPU delta'
Assert-HostProcessProfile `
    ($profile.threads.top[0].shareOfProcessCpuPercent -eq 50) `
    'top thread share must use total process CPU'

$reusedStop = New-HostProcessSnapshotFixture -Timestamp 5500 `
    -TotalCpuTicks (12 * $ticksPerSecond) `
    -UserCpuTicks (9 * $ticksPerSecond) `
    -KernelCpuTicks (3 * $ticksPerSecond) -Threads @(
        (New-HostThreadFixture -Id 101 `
            -StartAtUtc '2026-07-18T12:01:15.0000000Z' `
            -TotalCpuTicks (1 * $ticksPerSecond) `
            -UserCpuTicks (1 * $ticksPerSecond) -KernelCpuTicks 0)
    )
$reuseProfile = New-XpHostProcessCpuProfile -Start $start -Stop $reusedStop
Assert-HostProcessProfile ($reuseProfile.threads.stableCount -eq 0) `
    'a reused thread ID with a different start time must not match'
Assert-HostProcessProfile ($reuseProfile.threads.startedCount -eq 1) `
    'a reused thread ID must be a started identity'
Assert-HostProcessProfile ($reuseProfile.threads.exitedCount -eq 3) `
    'all old identities must be exited when an ID is reused'

$wrongProcess = New-HostProcessSnapshotFixture -Timestamp 5500 `
    -TotalCpuTicks (70 * $ticksPerSecond) `
    -UserCpuTicks (48 * $ticksPerSecond) `
    -KernelCpuTicks (22 * $ticksPerSecond) -Threads @() -ProcessId 43
$wrongProcessProfile = New-XpHostProcessCpuProfile `
    -Start $start -Stop $wrongProcess
Assert-HostProcessProfile (-not $wrongProcessProfile.valid) `
    'a changed process identity must invalidate the profile'
Assert-HostProcessProfile `
    ($wrongProcessProfile.findings -contains
        'host process identity changed during the measurement window') `
    'identity failure must be explicit'

$incompleteStop = New-HostProcessSnapshotFixture -Timestamp 5500 `
    -TotalCpuTicks (70 * $ticksPerSecond) `
    -UserCpuTicks (48 * $ticksPerSecond) `
    -KernelCpuTicks (22 * $ticksPerSecond) -Threads @() -IncompleteThreads
$incompleteProfile = New-XpHostProcessCpuProfile `
    -Start $start -Stop $incompleteStop
Assert-HostProcessProfile $incompleteProfile.valid `
    'thread enumeration failure must not invalidate process CPU evidence'
Assert-HostProcessProfile `
    ($incompleteProfile.status -eq 'valid-with-findings') `
    'incomplete thread evidence must be labeled'

$missingStopProfile = New-XpHostProcessCpuProfile -Start $start -Stop $null
Assert-HostProcessProfile (-not $missingStopProfile.valid) `
    'a missing stop snapshot must produce incomplete evidence'
Assert-HostProcessProfile ($missingStopProfile.status -eq 'incomplete') `
    'a missing stop snapshot must be labeled incomplete'

$inconsistentStop = New-HostProcessSnapshotFixture -Timestamp 5500 `
    -TotalCpuTicks (11 * $ticksPerSecond) `
    -UserCpuTicks (88 * $ticksPerSecond / 10) `
    -KernelCpuTicks (28 * $ticksPerSecond / 10) -Threads @(
        (New-HostThreadFixture -Id 101 -StartAtUtc $threadOneStart `
            -TotalCpuTicks (6 * $ticksPerSecond) `
            -UserCpuTicks (45 * $ticksPerSecond / 10) `
            -KernelCpuTicks (15 * $ticksPerSecond / 10))
    )
$inconsistentProfile = New-XpHostProcessCpuProfile `
    -Start $start -Stop $inconsistentStop
Assert-HostProcessProfile $inconsistentProfile.valid `
    'counter inconsistency must label evidence without gating acceptance'
Assert-HostProcessProfile `
    ($inconsistentProfile.status -eq 'valid-with-findings') `
    'counter inconsistency must change profile status'
Assert-HostProcessProfile `
    ($inconsistentProfile.findings -contains
        'process total CPU differs from user plus kernel beyond sampling tolerance') `
    'process component inconsistency must be explicit'
Assert-HostProcessProfile `
    ($inconsistentProfile.findings -contains
        'stable thread CPU exceeds process CPU beyond sampling tolerance') `
    'thread/process inconsistency must be explicit'
Assert-HostProcessProfile `
    ($inconsistentProfile.process.breakdownSkewSeconds -eq -0.6) `
    'process component skew must be quantified'
Assert-HostProcessProfile `
    ($inconsistentProfile.threads.stableCoveragePercent -eq 200) `
    'impossible stable coverage must remain visible rather than be clamped'

function New-ProfileEventFixture {
    $fixture = [pscustomobject]@{ setCalls = 0 }
    $fixture | Add-Member -MemberType ScriptMethod -Name Set -Value {
        ++$this.setCalls
        $true
    }
    $fixture
}

$failureState = New-XpHostProcessCpuProfileState
$failureStartEvent = New-ProfileEventFixture
$failureStopEvent = New-ProfileEventFixture
Start-XpHostProcessCpuProfileWindow -State $failureState -Process $null `
    -StartEvent $failureStartEvent
$firstFailedSnapshot = $failureState.start
Start-XpHostProcessCpuProfileWindow -State $failureState -Process $null `
    -StartEvent $failureStartEvent
Stop-XpHostProcessCpuProfileWindow -State $failureState -Process $null `
    -StopEvent $failureStopEvent
$firstFailedStopSnapshot = $failureState.stop
Stop-XpHostProcessCpuProfileWindow -State $failureState -Process $null `
    -StopEvent $failureStopEvent
Assert-HostProcessProfile $failureState.startAttempted `
    'start snapshot attempt must be recorded'
Assert-HostProcessProfile $failureState.stopAttempted `
    'stop snapshot attempt must be recorded'
Assert-HostProcessProfile `
    ([object]::ReferenceEquals($firstFailedSnapshot, $failureState.start)) `
    'repeated start notification must not retry the snapshot'
Assert-HostProcessProfile `
    ([object]::ReferenceEquals($firstFailedStopSnapshot, $failureState.stop)) `
    'repeated stop notification must not retry the snapshot'
Assert-HostProcessProfile ($failureStartEvent.setCalls -eq 2) `
    'every start notification must preserve event signaling'
Assert-HostProcessProfile ($failureStopEvent.setCalls -eq 2) `
    'every stop notification must preserve event signaling'
Assert-HostProcessProfile (-not $failureState.profile.valid) `
    'snapshot failure must leave labeled incomplete evidence'
Assert-HostProcessProfile ($failureState.profile.status -eq 'incomplete') `
    'snapshot failure must not throw or masquerade as valid evidence'
$publishedFailureProfile = Complete-XpHostProcessCpuProfileState `
    -State $failureState
Assert-HostProcessProfile `
    ([object]::ReferenceEquals(
        $publishedFailureProfile, $failureState.profile)) `
    'publication must reuse the same profile object embedded in the result'

$liveProcess = [Diagnostics.Process]::GetCurrentProcess()
try {
    $liveStart = Get-XpHostProcessCpuSnapshot -Process $liveProcess
    $sum = 0
    for ($index = 0; $index -lt 100000; ++$index) { $sum += $index % 17 }
    $liveStop = Get-XpHostProcessCpuSnapshot -Process $liveProcess
    $liveProfile = New-XpHostProcessCpuProfile `
        -Start $liveStart -Stop $liveStop
    Assert-HostProcessProfile $liveStart.valid `
        'live start snapshot must be valid'
    Assert-HostProcessProfile $liveStop.valid `
        'live stop snapshot must be valid'
    Assert-HostProcessProfile $liveProfile.valid `
        'two live snapshots of the same process must compare'
    Assert-HostProcessProfile ($liveProfile.window.wallSeconds -gt 0) `
        'live profile wall time must advance'
} finally {
    $liveProcess.Dispose()
}

Write-Output 'host process profile tests passed'
