$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')

function Assert-RunArtifacts {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-acceptance-artifacts-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    $runDirectory = Join-Path $testRoot 'run'
    $context = Initialize-XpAcceptanceRunArtifacts `
        -Directory $runDirectory -RunId 'artifact-contract' `
        -TaskId 'artifact-task' -Tier 'T1' `
        -CommandLine 'powershell.exe -File contract.ps1'
    Assert-RunArtifacts ($context.Directory -eq $runDirectory) `
        'artifact context directory is wrong'
    Assert-RunArtifacts `
        ((Get-Content -Raw -LiteralPath `
            (Join-Path $runDirectory 'command.txt')).Trim() -eq
            'powershell.exe -File contract.ps1') `
        'command record is wrong'
    Write-XpAcceptanceAtomicText `
        -Path (Join-Path $runDirectory 'command.txt') `
        -Text "powershell.exe -File replacement.ps1`r`n"
    Assert-RunArtifacts `
        ((Get-Content -Raw -LiteralPath `
            (Join-Path $runDirectory 'command.txt')).Trim() -eq
            'powershell.exe -File replacement.ps1') `
        'atomic text replacement is wrong'

    $source = Join-Path $testRoot 'source.log'
    Set-Content -LiteralPath $source -Value 'first artifact' `
        -Encoding ASCII
    $copied = Copy-XpAcceptanceRunArtifact -Context $context `
        -Source $source -Name 'host.stderr.log'
    Assert-RunArtifacts (Test-Path -LiteralPath $copied) `
        'required artifact was not copied'
    Set-Content -LiteralPath $source -Value 'replacement artifact' `
        -Encoding ASCII
    [void](Copy-XpAcceptanceRunArtifact -Context $context `
        -Source $source -Name 'host.stderr.log')
    Assert-RunArtifacts `
        ((Get-Content -Raw -LiteralPath $copied).Trim() -eq
            'replacement artifact') `
        'atomic copied-file replacement is wrong'
    Assert-RunArtifacts `
        ($null -eq (Copy-XpAcceptanceRunArtifact -Context $context `
            -Source (Join-Path $testRoot 'missing.log') `
            -Name 'missing.log' -Optional)) `
        'optional missing artifact returned a path'

    $unsafeRejected = $false
    try {
        [void](Copy-XpAcceptanceRunArtifact -Context $context `
            -Source $source -Name '..\escape.log')
    } catch {
        $unsafeRejected = $_.Exception.Message.Contains('safe file name')
    }
    Assert-RunArtifacts $unsafeRejected `
        'artifact destination traversal was not rejected'

    $result = [pscustomobject][ordered]@{
        schema = 1
        runId = $context.RunId
        taskId = $context.TaskId
        tier = $context.Tier
        result = 'pass'
        metrics = [pscustomobject]@{ copied = 1 }
    }
    $resultPath = Write-XpAcceptanceRunResult -Context $context `
        -Result $result
    $parsedResult = Get-Content -Raw -LiteralPath $resultPath |
        ConvertFrom-Json
    Assert-RunArtifacts ($parsedResult.runId -eq 'artifact-contract') `
        'structured run result is wrong'

    $manifestPath = Write-XpAcceptanceRunManifest -Context $context
    $manifest = Get-Content -Raw -LiteralPath $manifestPath |
        ConvertFrom-Json
    Assert-RunArtifacts ($manifest.runId -eq 'artifact-contract') `
        'artifact manifest run ID is wrong'
    Assert-RunArtifacts ($manifest.files.Count -eq 3) `
        "artifact manifest file count is wrong: $($manifest.files.Count)"
    foreach ($record in $manifest.files) {
        $recordPath = Join-Path $runDirectory $record.name
        Assert-RunArtifacts (Test-Path -LiteralPath $recordPath) `
            "manifest references missing artifact $($record.name)"
        Assert-RunArtifacts `
            ((Get-FileHash -LiteralPath $recordPath `
                -Algorithm SHA256).Hash -eq $record.sha256) `
            "manifest hash is wrong for $($record.name)"
    }

    # A captureMode=video run may pass only with one digest-bearing review MP4.
    # Exercise the exact records later written to artifact-manifest.json, not a
    # pre-rename destination or a loose VM-directory glob.
    $videoDirectory = Join-Path $testRoot 'video-run'
    $videoContext = Initialize-XpAcceptanceRunArtifacts `
        -Directory $videoDirectory -RunId 'artifact-video' `
        -TaskId 'artifact-task' -Tier 'T3' -CommandLine 'video.ps1'
    $videoSource = Join-Path $testRoot 'video-source.mp4'
    Set-Content -LiteralPath $videoSource -Value 'encoded video' `
        -Encoding ASCII
    [void](Copy-XpAcceptanceRunArtifact -Context $videoContext `
        -Source $videoSource `
        -Name 'xpdm-ut2003-review-012345abcdef.mp4')
    $videoRecords = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $videoContext)
    Assert-RunArtifacts (Test-XpAcceptanceReviewVideoRecords `
            -Records $videoRecords -NamePrefix 'xpdm-ut2003-review-') `
        'one digest-bearing review video was not accepted'
    Assert-RunArtifacts (-not (Test-XpAcceptanceReviewVideoRecords `
            -Records $videoRecords -NamePrefix 'xpdm-quake2-review-')) `
        'a review video with the wrong prefix was accepted'
    [void](Copy-XpAcceptanceRunArtifact -Context $videoContext `
        -Source $videoSource `
        -Name 'xpdm-ut2003-review-fedcba543210.mp4')
    $duplicateVideoRecords = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $videoContext)
    Assert-RunArtifacts (-not (Test-XpAcceptanceReviewVideoRecords `
            -Records $duplicateVideoRecords `
            -NamePrefix 'xpdm-ut2003-review-')) `
        'duplicate review videos were accepted'
    Remove-Item -LiteralPath (Join-Path $videoDirectory `
        'xpdm-ut2003-review-fedcba543210.mp4') -Force
    $videoManifestPath = Write-XpAcceptanceRunManifest `
        -Context $videoContext
    $videoManifest = Get-Content -Raw -LiteralPath $videoManifestPath |
        ConvertFrom-Json
    $manifestedVideos = @($videoManifest.files | Where-Object {
        $_.name -like 'xpdm-ut2003-review-*.mp4'
    })
    Assert-RunArtifacts ($manifestedVideos.Count -eq 1 -and
        (Test-XpAcceptanceReviewVideoRecords `
            -Records $videoManifest.files `
            -NamePrefix 'xpdm-ut2003-review-')) `
        'successful video manifest does not contain exactly one review MP4'

    # Reconstruct publication from the exact run-cleaned namespace. Mutable
    # capture arrays and pre-rename paths are deliberately irrelevant here.
    $observeDirectory = Join-Path $testRoot 'observe-namespace'
    [void](New-Item -ItemType Directory -Path $observeDirectory)
    $validObserveNames = @(
        'xpdm-ut2003-baseline.bmp',
        'xpdm-ut2003-sample-0-012345abcdef.bmp',
        'xpdm-ut2003-sample-1-012345abcdef.bmp',
        'xpdm-ut2003-sample-2-012345abcdef.bmp',
        'xpdm-ut2003-sample-3-012345abcdef.bmp',
        'xpdm-ut2003-sample-4-012345abcdef.bmp',
        'xpdm-ut2003-sample-5-012345abcdef.bmp',
        'xpdm-ut2003-sample-6-012345abcdef.bmp',
        'xpdm-ut2003-sample-7-fedcba543210.bmp',
        'xpdm-ut2003-review-012345abcdef.mp4')
    $invalidObserveNames = @(
        'xpdm-ut2003-review.mp4',
        'xpdm-ut2003-review-012345ABCDEf.mp4',
        'xpdm-ut2003-review-zzzzzzzzzzzz.mp4',
        'xpdm-ut2003-sample-8-012345abcdef.bmp',
        'xpdm-warcraft3d3d8-review-012345abcdef.mp4')
    foreach ($name in @($validObserveNames + $invalidObserveNames)) {
        Set-Content -LiteralPath (Join-Path $observeDirectory $name) `
            -Value $name -Encoding ASCII
    }
    $observePaths = @(Get-XpAcceptanceGameObserveCapturePaths `
        -Directory $observeDirectory -Game 'UT2003')
    $observeNames = @($observePaths | ForEach-Object {
        [IO.Path]::GetFileName($_)
    })
    Assert-RunArtifacts `
        (($observeNames -join ',') -eq
            (($validObserveNames | Sort-Object) -join ',')) `
        "game-observe namespace admitted stale/foreign files: $($observeNames -join ',')"
    $observeRecords = @($observeNames | ForEach-Object {
        [pscustomobject]@{ name = $_ }
    })
    Assert-RunArtifacts (Test-XpAcceptanceReviewVideoRecords `
            -Records $observeRecords -NamePrefix 'xpdm-ut2003-review-') `
        'reconstructed namespace did not retain exactly one digest video'
    Assert-RunArtifacts (Test-XpAcceptanceGameObserveCaptureNames `
            -Names $observeNames -Game 'UT2003') `
        'complete baseline/sample/video namespace was rejected'
    Assert-RunArtifacts (-not (Test-XpAcceptanceGameObserveCaptureNames `
            -Names @($observeNames | Where-Object {
                $_ -notlike 'xpdm-ut2003-sample-4-*'
            }) -Game 'UT2003')) `
        'namespace with a missing sample index was accepted'
    Set-Content -LiteralPath (Join-Path $observeDirectory `
        'xpdm-ut2003-review-fedcba543210.mp4') -Value 'duplicate' `
        -Encoding ASCII
    $duplicateObserveRecords = @(
        Get-XpAcceptanceGameObserveCapturePaths `
            -Directory $observeDirectory -Game 'UT2003' |
        ForEach-Object {
            [pscustomobject]@{ name = [IO.Path]::GetFileName($_) }
        })
    Assert-RunArtifacts (-not (Test-XpAcceptanceReviewVideoRecords `
            -Records $duplicateObserveRecords `
            -NamePrefix 'xpdm-ut2003-review-')) `
        'duplicate reconstructed review videos were accepted'
    Assert-RunArtifacts (-not (Test-XpAcceptanceGameObserveCaptureNames `
            -Names @($duplicateObserveRecords | ForEach-Object name) `
            -Game 'UT2003')) `
        'duplicate review video passed the complete capture-set gate'
    Remove-Item -LiteralPath (Join-Path $observeDirectory `
        'xpdm-ut2003-review-fedcba543210.mp4') -Force
    $duplicateSampleName =
        'xpdm-ut2003-sample-4-fedcba543210.bmp'
    Set-Content -LiteralPath (Join-Path $observeDirectory `
        $duplicateSampleName) -Value 'duplicate sample' -Encoding ASCII
    $duplicateSampleNames = @(
        Get-XpAcceptanceGameObserveCapturePaths `
            -Directory $observeDirectory -Game 'UT2003' |
        ForEach-Object { [IO.Path]::GetFileName($_) })
    Assert-RunArtifacts (-not (Test-XpAcceptanceGameObserveCaptureNames `
            -Names $duplicateSampleNames -Game 'UT2003')) `
        'duplicate sample index passed the complete capture-set gate'

    $consoleStaging = Join-Path $testRoot 'console-staging.log'
    Set-Content -LiteralPath $consoleStaging `
        -Value 'HarnessRun runId=artifact-contract status=passed' `
        -Encoding ASCII
    $launchRecord = [pscustomobject][ordered]@{
        executable = 'powershell.exe'
        arguments = @('-File', 'contract.ps1')
    }
    # A result whose identity does not match the launcher context must
    # refuse to finalize: a foreign or stale document cannot be adopted.
    $foreignDirectory = Join-Path $testRoot 'foreign-run'
    $foreignContext = Initialize-XpAcceptanceRunArtifacts `
        -Directory $foreignDirectory -RunId 'artifact-foreign' `
        -TaskId 'artifact-task' -Tier 'T0' -CommandLine 'foreign.ps1'
    [void](Write-XpAcceptanceRunResult -Context $foreignContext -Result (
        [pscustomobject][ordered]@{
            schema = 1
            runId = 'some-other-run'
            taskId = 'artifact-task'
            tier = 'T0'
            result = 'pass'
        }))
    $foreignStaging = Join-Path $testRoot 'foreign-console.log'
    Set-Content -LiteralPath $foreignStaging -Value 'console' -Encoding ASCII
    $foreignRejected = $false
    try {
        [void](Complete-XpAcceptanceLauncherArtifacts `
            -Context $foreignContext -ConsoleStagingPath $foreignStaging `
            -LaunchRecord $launchRecord -ChildExitCode 0)
    } catch {
        $foreignRejected = $_.Exception.Message.Contains(
            'does not match the launcher')
    }
    Assert-RunArtifacts $foreignRejected `
        'mismatched result identity was finalized'

    $manifestPath = Complete-XpAcceptanceLauncherArtifacts `
        -Context $context -ConsoleStagingPath $consoleStaging `
        -LaunchRecord $launchRecord -ChildExitCode 0
    $parsedResult = Get-Content -Raw -LiteralPath $resultPath |
        ConvertFrom-Json
    Assert-RunArtifacts `
        ($parsedResult.launcher.phase -eq 'finalized' -and
         $parsedResult.launcher.childStarted -eq $true -and
         $parsedResult.launcher.finalized -eq $true -and
         $parsedResult.launcher.childExitCode -eq 0) `
        'launcher lifecycle identity or child exit code is wrong'
    Assert-RunArtifacts ($parsedResult.launcher.console -eq 'console.log') `
        'launcher console reference is wrong'
    Assert-RunArtifacts ($parsedResult.artifacts -contains 'console.log') `
        'run result artifacts omit console.log'
    $manifest = Get-Content -Raw -LiteralPath $manifestPath |
        ConvertFrom-Json
    Assert-RunArtifacts ($manifest.files.Count -eq 5) `
        "finalized manifest file count is wrong: $($manifest.files.Count)"
    foreach ($record in $manifest.files) {
        $recordPath = Join-Path $runDirectory $record.name
        Assert-RunArtifacts `
            ((Get-FileHash -LiteralPath $recordPath `
                -Algorithm SHA256).Hash -eq $record.sha256) `
            "finalized manifest hash is wrong for $($record.name)"
    }
    Assert-RunArtifacts `
        (-not @(Get-ChildItem -LiteralPath $runDirectory -Force |
            Where-Object {
                $_.Name -match '^\..+\.(?:tmp|backup)-'
            }).Count) `
        'successful artifact replacement left temporary or backup files'

    $reuseRejected = $false
    try {
        [void](Initialize-XpAcceptanceRunArtifacts `
            -Directory $runDirectory -RunId 'artifact-reuse' `
            -TaskId 'artifact-task' -Tier 'T1' -CommandLine 'reuse')
    } catch {
        $reuseRejected = $_.Exception.Message.Contains('not empty')
    }
    Assert-RunArtifacts $reuseRejected `
        'nonempty artifact directory reuse was not rejected'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Output 'acceptance run artifact tests passed'
