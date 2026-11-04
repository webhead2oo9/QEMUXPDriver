$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'warcraft3-d3d8-oracle.ps1')

function Assert-Warcraft3D3D8Oracle {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-warcraft3-d3d8-oracle-$PID-$([Guid]::NewGuid().ToString('N'))"
$source = Join-Path $root 'source'
$destination = Join-Path $root 'snapshot'
$snapshot = $null
try {
    [void](New-Item -ItemType Directory -Path $source)
    $expected = [ordered]@{}
    $boundary = [DateTime]::UtcNow.AddSeconds(-5)
    foreach ($index in 0..2) {
        $path = Join-Path $source "frame-ring-$index.bmp"
        [IO.File]::WriteAllBytes(
            $path, [byte[]]@(($index + 1), 7, 9))
        (Get-Item -LiteralPath $path).LastWriteTimeUtc =
            $boundary.AddSeconds($index + 1)
        $expected[(Split-Path -Leaf $path)] =
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $stale = Join-Path $source 'frame-ring-99.bmp'
    [IO.File]::WriteAllBytes($stale, [byte[]]@(91, 92, 93))
    (Get-Item -LiteralPath $stale).LastWriteTimeUtc =
        $boundary.AddSeconds(-10)
    [IO.File]::WriteAllText((Join-Path $source 'unrelated.txt'), 'keep')

    $snapshot = Enter-XpWarcraft3D3D8RingSnapshot `
        -SourceDirectory $source -DestinationDirectory $destination `
        -NotBeforeUtc $boundary
    Assert-Warcraft3D3D8Oracle ($snapshot.frameCount -eq 3) `
        'snapshot did not move every numeric ring frame'
    $remainingRing = @(Get-ChildItem -LiteralPath $source -File |
        Where-Object { $_.Name -match '^frame-ring-[0-9]+\.bmp$' })
    Assert-Warcraft3D3D8Oracle `
        ($remainingRing.Count -eq 1 -and
         $remainingRing[0].Name -eq 'frame-ring-99.bmp') `
        'snapshot did not exclude the pre-oracle ring generation'
    Assert-Warcraft3D3D8Oracle `
        (Test-Path -LiteralPath (Join-Path $source 'unrelated.txt')) `
        'snapshot moved an unrelated source file'

    $writerWasBlocked = $false
    try {
        $writer = [IO.File]::Open(
            (Join-Path $source 'frame-ring-tmp.bmp'),
            [IO.FileMode]::Create, [IO.FileAccess]::Write,
            [IO.FileShare]::ReadWrite)
        $writer.Dispose()
    } catch [IO.IOException] {
        $writerWasBlocked = $true
    }
    Assert-Warcraft3D3D8Oracle $writerWasBlocked `
        'exclusive snapshot lock did not quiesce the producer temp file'

    # Simulate arbitrary post-boundary publication under a new source slot.
    # The immutable snapshot must retain exactly the boundary hashes.
    [IO.File]::WriteAllBytes(
        (Join-Path $source 'frame-ring-0.bmp'), [byte[]]@(99, 98, 97))
    foreach ($name in $expected.Keys) {
        $snapshotPath = Join-Path $destination $name
        Assert-Warcraft3D3D8Oracle `
            ((Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash `
                -ceq $expected[$name]) `
            "post-boundary publication changed snapshot $name"
    }
    Assert-Warcraft3D3D8Oracle `
        (@(Get-ChildItem -LiteralPath $destination -File).Count -eq 3) `
        'post-boundary publication changed snapshot cardinality'

    Exit-XpWarcraft3D3D8RingSnapshot -Snapshot $snapshot
    $snapshot = $null
    $writer = [IO.File]::Open(
        (Join-Path $source 'frame-ring-tmp.bmp'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write,
        [IO.FileShare]::ReadWrite)
    $writer.Dispose()

    $doubleBlackCadence = [pscustomobject]@{
        classes = 'VBBV'
        runs = @(
            [pscustomobject]@{ start = 0; end = 0; class = 'V' },
            [pscustomobject]@{ start = 1; end = 2; class = 'B' },
            [pscustomobject]@{ start = 3; end = 3; class = 'V' })
        frameDetails = @(0..3 | ForEach-Object {
            [pscustomobject]@{ file = "frame-ring-$_.bmp" }
        })
    }
    $doubleWitness = @(Get-XpWarcraft3D3D8BlackRunWitness `
        -Cadence $doubleBlackCadence)
    Assert-Warcraft3D3D8Oracle `
        ($doubleWitness.Count -eq 4 -and
         ($doubleWitness.label -join ',') -eq
            'before,black-first,black-last,after' -and
         ($doubleWitness.index -join ',') -eq '0,1,2,3') `
        'visible-bounded two-frame black run did not retain four witnesses'
    $singleBlackCadence = [pscustomobject]@{
        classes = 'VBV'
        runs = @(
            [pscustomobject]@{ start = 0; end = 0; class = 'V' },
            [pscustomobject]@{ start = 1; end = 1; class = 'B' },
            [pscustomobject]@{ start = 2; end = 2; class = 'V' })
        frameDetails = @(0..2 | ForEach-Object {
            [pscustomobject]@{ file = "frame-ring-$_.bmp" }
        })
    }
    $singleWitness = @(Get-XpWarcraft3D3D8BlackRunWitness `
        -Cadence $singleBlackCadence)
    Assert-Warcraft3D3D8Oracle `
        ($singleWitness.Count -eq 3 -and
         ($singleWitness.label -join ',') -eq 'before,black,after') `
        'isolated black frame did not retain a three-frame witness'

    Assert-Warcraft3D3D8Oracle `
        (Test-XpWarcraft3D3D8EncodedFrameCount `
            -SnapshotFrames 256 -EncodedFrames 256) `
        'exact 256-frame video count was rejected'
    Assert-Warcraft3D3D8Oracle `
        (-not (Test-XpWarcraft3D3D8EncodedFrameCount `
            -SnapshotFrames 256 -EncodedFrames 255) -and
         -not (Test-XpWarcraft3D3D8EncodedFrameCount `
            -SnapshotFrames 255 -EncodedFrames 255)) `
        'decimated or insufficient video frame counts were accepted'
    Assert-Warcraft3D3D8Oracle `
        (Test-XpWarcraft3D3D8EncodedVideoTiming `
            -SnapshotFrames 256 -EncodedFrames 256 `
            -NominalFrameRate '30/1' -AverageFrameRate '30/1' `
            -DurationSeconds (256 / 30.0)) `
        'exact 30-fps video timing was rejected'
    Assert-Warcraft3D3D8Oracle `
        (-not (Test-XpWarcraft3D3D8EncodedVideoTiming `
            -SnapshotFrames 256 -EncodedFrames 256 `
            -NominalFrameRate '25/1' -AverageFrameRate '30/1' `
            -DurationSeconds (256 / 30.0)) -and
         -not (Test-XpWarcraft3D3D8EncodedVideoTiming `
            -SnapshotFrames 256 -EncodedFrames 256 `
            -NominalFrameRate '30/1' -AverageFrameRate '30/1' `
            -DurationSeconds 8.48)) `
        'wrong frame rate or duration was accepted'

    $receivedStart = [DateTime]::UtcNow.AddSeconds(-25.2)
    $receivedComplete = [DateTime]::UtcNow
    $startMessage = 'QXP_GAME_OBSERVE_STAGE rc=0 game=warcraft3d3d8 stage=oracle-start warmup-ms=15000 renderer=d3d8 system32=1'
    $completeMessage = 'QXP_GAME_OBSERVE_STAGE rc=0 game=warcraft3d3d8 stage=oracle-complete observed-ms=25079 renderer=d3d8 system32=1'
    $resultMessage = 'QXP_GAME_OBSERVE_RESULT rc=0 game=warcraft3d3d8 stage=complete outcome=observed exit=259 modules=0d observed-ms=25079 renderer=d3d8 system32=1'
    $guestCheck = Test-XpWarcraft3D3D8GuestContract `
        -OracleStartMessages @($startMessage) `
        -OracleCompleteMessages @($completeMessage) `
        -ResultMessages @($resultMessage) `
        -OracleStartReceivedAt $receivedStart `
        -OracleCompleteReceivedAt $receivedComplete
    Assert-Warcraft3D3D8Oracle $guestCheck.valid `
        'exact 25-second System32 D3D8 guest contract was rejected'
    $shortCheck = Test-XpWarcraft3D3D8GuestContract `
        -OracleStartMessages @($startMessage) `
        -OracleCompleteMessages @($completeMessage.Replace(
            '25079', '24999')) `
        -ResultMessages @($resultMessage.Replace('25079', '24999')) `
        -OracleStartReceivedAt $receivedComplete.AddSeconds(-20) `
        -OracleCompleteReceivedAt $receivedComplete
    Assert-Warcraft3D3D8Oracle (-not $shortCheck.valid -and
        $shortCheck.findings -contains
            'oracle-complete observed-ms is shorter than 25000' -and
        $shortCheck.findings -contains
            'host oracle boundary is shorter than 24500 ms') `
        'short Warcraft oracle timing was accepted'
    $handoffCheck = Test-XpWarcraft3D3D8GuestContract `
        -OracleStartMessages @($startMessage) `
        -OracleCompleteMessages @() `
        -ResultMessages @('QXP_GAME_OBSERVE_RESULT rc=8 game=warcraft3d3d8 stage=oracle error=process-handoff-during-oracle successor-pid=9 observed-ms=12000') `
        -OracleStartReceivedAt $receivedStart `
        -OracleCompleteReceivedAt ([DateTime]::MaxValue)
    Assert-Warcraft3D3D8Oracle (-not $handoffCheck.valid -and
        $handoffCheck.findings -contains 'result fields are malformed') `
        'post-start process handoff inherited oracle qualification'
    $duplicateCheck = Test-XpWarcraft3D3D8GuestContract `
        -OracleStartMessages @($startMessage, $startMessage) `
        -OracleCompleteMessages @($completeMessage) `
        -ResultMessages @($resultMessage) `
        -OracleStartReceivedAt $receivedStart `
        -OracleCompleteReceivedAt $receivedComplete
    Assert-Warcraft3D3D8Oracle (-not $duplicateCheck.valid) `
        'duplicate oracle-start qualification was accepted'
    $collectedResults = [Collections.Generic.List[string]]::new()
    Assert-Warcraft3D3D8Oracle `
        (Add-XpWarcraft3D3D8UniqueMessage `
            -Messages $collectedResults -Message $resultMessage) `
        'first result record was not collected'
    Assert-Warcraft3D3D8Oracle `
        (-not (Add-XpWarcraft3D3D8UniqueMessage `
            -Messages $collectedResults -Message $resultMessage) -and
         $collectedResults.Count -eq 1) `
        'exact terminal retransmission was not deduplicated'
    $conflictingResult = $resultMessage.Replace('25079', '25080')
    Assert-Warcraft3D3D8Oracle `
        (Add-XpWarcraft3D3D8UniqueMessage `
            -Messages $collectedResults -Message $conflictingResult) `
        'conflicting result record was discarded'
    $conflictingCheck = Test-XpWarcraft3D3D8GuestContract `
        -OracleStartMessages @($startMessage) `
        -OracleCompleteMessages @($completeMessage) `
        -ResultMessages @($collectedResults) `
        -OracleStartReceivedAt $receivedStart `
        -OracleCompleteReceivedAt $receivedComplete
    Assert-Warcraft3D3D8Oracle (-not $conflictingCheck.valid -and
        $conflictingCheck.findings -contains
            'result count is 2, expected 1') `
        'two distinct received result records passed qualification'

    $fullProfile = [pscustomobject]@{
        valid = $true
        miniport = [pscustomobject]@{
            presentationSourceCountersPresent = $true
            logicalDraws = 100
            presents = 10
            cpuPresents = 0
            surfaceTexturePresents = 10
            emptyPresents = 0
            surfaceReadbacks = 10
        }
    }
    $counterCheck = Test-XpWarcraft3D3D8CounterProfile -Profile $fullProfile
    Assert-Warcraft3D3D8Oracle $counterCheck.valid `
        'complete Warcraft D3D8 counters were rejected'
    $missingCheck = Test-XpWarcraft3D3D8CounterProfile -Profile $null
    Assert-Warcraft3D3D8Oracle (-not $missingCheck.valid -and
        $missingCheck.findings -contains 'D3D counter profile is missing') `
        'missing Warcraft D3D8 counters were accepted'
    $partialProfile = [pscustomobject]@{
        valid = $true
        miniport = [pscustomobject]@{
            presentationSourceCountersPresent = $false
            logicalDraws = 100
            presents = 10
            cpuPresents = 0
            surfaceTexturePresents = 10
            emptyPresents = 0
            surfaceReadbacks = 10
        }
    }
    $partialCheck = Test-XpWarcraft3D3D8CounterProfile `
        -Profile $partialProfile
    Assert-Warcraft3D3D8Oracle (-not $partialCheck.valid -and
        $partialCheck.findings -contains
            'presentation-source counter group is missing') `
        'partial Warcraft D3D8 counters were accepted'
    $redProfile = [pscustomobject]@{
        valid = $true
        miniport = [pscustomobject]@{
            presentationSourceCountersPresent = $true
            logicalDraws = 100
            presents = 10
            cpuPresents = 5
            surfaceTexturePresents = 5
            emptyPresents = 0
            surfaceReadbacks = 10
        }
    }
    $redCheck = Test-XpWarcraft3D3D8CounterProfile -Profile $redProfile
    Assert-Warcraft3D3D8Oracle (-not $redCheck.valid -and
        $redCheck.findings -contains
            'CPU presents remain in the Warcraft D3D8 path') `
        'known red CPU/surface-texture split passed the repair gate'
    $zeroReadbackProfile = [pscustomobject]@{
        valid = $true
        miniport = [pscustomobject]@{
            presentationSourceCountersPresent = $true
            logicalDraws = 100
            presents = 10
            cpuPresents = 0
            surfaceTexturePresents = 10
            emptyPresents = 0
            surfaceReadbacks = 0
        }
    }
    $zeroReadbackCheck = Test-XpWarcraft3D3D8CounterProfile `
        -Profile $zeroReadbackProfile
    Assert-Warcraft3D3D8Oracle (-not $zeroReadbackCheck.valid -and
        $zeroReadbackCheck.findings -contains
            'surface readback count is zero') `
        'zero-readback run passed the repair-path gate'
    $badSumProfile = [pscustomobject]@{
        valid = $true
        miniport = [pscustomobject]@{
            presentationSourceCountersPresent = $true
            logicalDraws = 100
            presents = 10
            cpuPresents = 0
            surfaceTexturePresents = 9
            emptyPresents = 0
            surfaceReadbacks = 10
        }
    }
    $badSumCheck = Test-XpWarcraft3D3D8CounterProfile `
        -Profile $badSumProfile
    Assert-Warcraft3D3D8Oracle (-not $badSumCheck.valid -and
        $badSumCheck.findings -contains
            'presentation-source counts do not sum to presents') `
        'inconsistent Warcraft D3D8 counters were accepted'

    Write-Output 'Warcraft D3D8 oracle contract tests passed'
} finally {
    if ($snapshot) {
        Exit-XpWarcraft3D3D8RingSnapshot -Snapshot $snapshot
    }
    Remove-Item -LiteralPath $root -Recurse -Force `
        -ErrorAction SilentlyContinue
}
