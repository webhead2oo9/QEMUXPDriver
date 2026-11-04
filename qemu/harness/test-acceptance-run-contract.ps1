$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')

function Assert-RunContract {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$generatedRunId = New-XpAcceptanceRunId
Assert-RunContract (Test-XpAcceptanceRunId -RunId $generatedRunId) `
    "generated run ID is invalid: $generatedRunId"
Assert-RunContract (Test-XpAcceptanceRunId -RunId 'manual.run_1-ok') `
    'expected manual run ID to be accepted'
foreach ($invalidRunId in @('', 'has space', '../escape', ('x' * 65))) {
    Assert-RunContract (-not (Test-XpAcceptanceRunId -RunId $invalidRunId)) `
        "invalid run ID was accepted: $invalidRunId"
}

$ordinary = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 150 `
    -Xpdm $false -LongRunning $false -CaptureGameObserve $false `
    -RecordVideo $false
Assert-RunContract ($ordinary.EffectiveSeconds -eq 150) `
    'ordinary timeout was unexpectedly raised'
Assert-RunContract ($ordinary.RecommendedOuterSeconds -eq 390) `
    'ordinary outer timeout allowance is wrong'
$xpdm = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 150 `
    -Xpdm $true -LongRunning $false -CaptureGameObserve $false `
    -RecordVideo $false
Assert-RunContract ($xpdm.EffectiveSeconds -eq 180) `
    'XPDM timeout floor is wrong'
$long = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 150 `
    -Xpdm $true -LongRunning $true -CaptureGameObserve $false `
    -RecordVideo $false
Assert-RunContract ($long.EffectiveSeconds -eq 300) `
    'long-running timeout floor is wrong'
$capture = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 300 `
    -Xpdm $true -LongRunning $true -CaptureGameObserve $true `
    -RecordVideo $true
Assert-RunContract ($capture.EffectiveSeconds -eq 600) `
    'capture observation timeout floor is wrong'
Assert-RunContract ($capture.RecommendedOuterSeconds -eq 960) `
    'video outer timeout allowance is wrong'
$utMenu = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 900 `
    -Xpdm $true -LongRunning $false -CaptureGameObserve $true `
    -RecordVideo $true -UT2003VisualOracle $true
Assert-RunContract ($utMenu.EffectiveSeconds -eq 7200) `
    'UT2003 post-menu oracle timeout floor is not conservative'
Assert-RunContract ($utMenu.RecommendedOuterSeconds -eq 7560) `
    'UT2003 outer timeout allowance is wrong'
$explicit = Get-XpAcceptanceTimeoutBudget -RequestedSeconds 700 `
    -Xpdm $true -LongRunning $true -CaptureGameObserve $true `
    -RecordVideo $false
Assert-RunContract ($explicit.EffectiveSeconds -eq 700) `
    'explicit timeout above the floor was changed'

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-acceptance-contract-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
$lockPath = Join-Path $testRoot 'artifacts.lock'
$firstLease = $null
$secondLease = $null
try {
    $firstLease = Enter-XpAcceptanceArtifactLease -LockPath $lockPath `
        -RunId 'contract-first' -QmpPort 49000 `
        -RequestedTimeoutSeconds 150 -EffectiveTimeoutSeconds 300
    $metadata = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
    Assert-RunContract ($metadata.status -eq 'active') `
        'new lease is not active'
    Assert-RunContract ($metadata.runId -eq 'contract-first') `
        'new lease run ID is wrong'
    Assert-RunContract ($metadata.qmpPort -eq 49000) `
        'new lease QMP port is wrong'

    $collision = $null
    try {
        $secondLease = Enter-XpAcceptanceArtifactLease -LockPath $lockPath `
            -RunId 'contract-collision' -QmpPort 49001 `
            -RequestedTimeoutSeconds 150 -EffectiveTimeoutSeconds 300
    } catch {
        $collision = $_.Exception.Message
    }
    Assert-RunContract ($null -eq $secondLease) `
        'a second lease acquired the shared artifact lock'
    Assert-RunContract ($collision -and
        $collision.Contains('owned by another run') -and
        $collision.Contains('contract-first')) `
        "lease collision did not report its owner: $collision"

    Update-XpAcceptanceArtifactLease -Lease $firstLease -HostPid 4242 `
        -QmpPort 49002 -Mode '-xpdm-contract' -ContentPath 'contract-content'
    $metadata = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
    Assert-RunContract ($metadata.hostPid -eq 4242) `
        'lease host PID update was not persisted'
    Assert-RunContract ($metadata.qmpPort -eq 49002) `
        'lease QMP port update was not persisted'
    Assert-RunContract ($metadata.mode -eq '-xpdm-contract') `
        'lease mode update was not persisted'
    Exit-XpAcceptanceArtifactLease -Lease $firstLease -Status passed
    Assert-RunContract ($null -eq $firstLease.Stream) `
        'completed lease retained a live stream reference'
    $firstLease = $null

    $metadata = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
    Assert-RunContract ($metadata.status -eq 'passed') `
        'completed lease status was not persisted'
    $secondLease = Enter-XpAcceptanceArtifactLease -LockPath $lockPath `
        -RunId 'contract-second' -QmpPort 49003 `
        -RequestedTimeoutSeconds 150 -EffectiveTimeoutSeconds 180
    Assert-RunContract ($secondLease.Metadata.runId -eq 'contract-second') `
        'released artifact lock could not be reacquired'
} finally {
    if ($firstLease) {
        Exit-XpAcceptanceArtifactLease -Lease $firstLease -Status failed
    }
    if ($secondLease) {
        Exit-XpAcceptanceArtifactLease -Lease $secondLease -Status passed
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Output 'acceptance run contract tests passed'
