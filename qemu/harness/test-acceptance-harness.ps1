param(
    [string]$ArtifactDirectory,
    [string]$RunId,
    [string]$TaskId
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = New-XpAcceptanceRunId
} elseif (-not (Test-XpAcceptanceRunId -RunId $RunId)) {
    throw "Invalid harness self-test run ID '$RunId'"
}
if ([string]::IsNullOrWhiteSpace($TaskId)) { $TaskId = $RunId }

$started = Get-Date
$artifactContext = if ($ArtifactDirectory) {
    Initialize-XpAcceptanceRunArtifacts -Directory $ArtifactDirectory `
        -RunId $RunId -TaskId $TaskId -Tier 'T0' `
        -CommandLine ([Environment]::CommandLine)
} else { $null }

$parseFindings = [Collections.Generic.List[object]]::new()
$harnessScripts = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' `
    -File | Sort-Object Name)
foreach ($script in $harnessScripts) {
    $tokens = $null
    $parseErrors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script.FullName, [ref]$tokens, [ref]$parseErrors)
    foreach ($parseError in @($parseErrors)) {
        $parseFindings.Add([pscustomobject][ordered]@{
            script = $script.Name
            message = $parseError.Message
            line = $parseError.Extent.StartLineNumber
            column = $parseError.Extent.StartColumnNumber
        })
    }
}
$parseText = if ($parseFindings.Count) {
    @($parseFindings | ForEach-Object {
        '{0}:{1}:{2}: {3}' -f $_.script, $_.line, $_.column, $_.message
    }) -join [Environment]::NewLine
} else {
    "parsed $($harnessScripts.Count) harness PowerShell scripts with no errors"
}
if ($artifactContext) {
    Write-XpAcceptanceAtomicText `
        -Path (Join-Path $artifactContext.Directory 'powershell-parse.log') `
        -Text ($parseText + [Environment]::NewLine)
}
Write-Output "HarnessSelfTestParse runId=$RunId scripts=$($harnessScripts.Count) errors=$($parseFindings.Count)"

$suiteFiles = @(
    'test-serial-tail.ps1',
    'test-d3d-driver-profile.ps1',
    'test-host-process-profile.ps1',
    'test-doorbell-profile.ps1',
    'test-maxpayne-visual.ps1',
    'test-game-observe-visual.ps1',
    'test-visual-bundle-fixtures.ps1',
    'test-verify-xp-evidence-bundle.ps1',
    'test-frame-ring-black-cadence.ps1',
    'test-warcraft3-d3d8-oracle.ps1',
    'test-d3d9-oracle.ps1',
    'test-acceptance-iso.ps1',
    'test-frozen-inputs.ps1',
    'test-package-identity.ps1',
    'test-acceptance-matrix-verifier.ps1',
    'test-acceptance-run-contract.ps1',
    'test-acceptance-run-artifacts.ps1',
    'test-run-xp-acceptance-arguments.ps1',
    'test-invoke-xp-acceptance.ps1',
    'test-invoke-xp-test-cycle.ps1',
    'test-invoke-xp-candidate-cycle.ps1',
    'test-xp-test-preflight.ps1',
    'test-xp-test-handback.ps1'
)
$powershell = Join-Path $PSHOME 'powershell.exe'
$suiteResults = [Collections.Generic.List[object]]::new()
foreach ($suiteFile in $suiteFiles) {
    $suitePath = Join-Path $PSScriptRoot $suiteFile
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $savedErrorActionPreference = $ErrorActionPreference
    $output = @()
    $exitCode = $null
    try {
        # Native stderr is test evidence. Keep it in the per-suite stream and
        # decide success from the isolated child process exit code.
        $ErrorActionPreference = 'Continue'
        $output = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $suitePath 2>&1)
        $exitCode = $LASTEXITCODE
    } catch {
        $output = @($_)
        $exitCode = 1
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
        $stopwatch.Stop()
    }
    if ($null -eq $exitCode) { $exitCode = 1 }
    $outputLines = @($output | ForEach-Object { $_.ToString() })
    $outputText = $outputLines -join [Environment]::NewLine
    if ($outputLines.Count) { $outputText += [Environment]::NewLine }
    $logName = ([IO.Path]::GetFileNameWithoutExtension($suiteFile) + '.log')
    if ($artifactContext) {
        Write-XpAcceptanceAtomicText `
            -Path (Join-Path $artifactContext.Directory $logName) `
            -Text $outputText
    }
    $passed = $exitCode -eq 0
    $suiteResults.Add([pscustomobject][ordered]@{
        name = [IO.Path]::GetFileNameWithoutExtension($suiteFile)
        script = $suiteFile
        result = if ($passed) { 'pass' } else { 'fail' }
        exitCode = $exitCode
        durationSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        log = if ($artifactContext) { $logName } else { $null }
    })
    Write-Output "HarnessSelfTestSuite runId=$RunId name=$suiteFile result=$(if ($passed) { 'pass' } else { 'fail' }) exitCode=$exitCode durationSeconds=$([Math]::Round($stopwatch.Elapsed.TotalSeconds, 3))"
    if (-not $passed) {
        Write-Output "HarnessSelfTestFailureOutputBegin name=$suiteFile"
        $outputLines | ForEach-Object { Write-Output $_ }
        Write-Output "HarnessSelfTestFailureOutputEnd name=$suiteFile"
    }
}

$failedSuiteCount = @($suiteResults | Where-Object {
    $_.result -ne 'pass'
}).Count
$passedAll = $parseFindings.Count -eq 0 -and $failedSuiteCount -eq 0
$durationSeconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 3)
if ($artifactContext) {
    $artifactRecords = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $artifactContext)
    $result = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-harness-self-test'
        taskId = $TaskId
        runId = $RunId
        timestamp = Get-Date -Format o
        tier = 'T0'
        result = if ($passedAll) { 'pass' } else { 'fail' }
        exitCode = if ($passedAll) { 0 } else { 1 }
        durationSeconds = $durationSeconds
        powershell = $powershell
        scriptsParsed = $harnessScripts.Count
        parseErrors = @($parseFindings)
        suites = @($suiteResults)
        artifacts = @($artifactRecords | ForEach-Object { $_.name })
    }
    [void](Write-XpAcceptanceRunResult -Context $artifactContext `
        -Result $result)
    [void](Write-XpAcceptanceRunManifest -Context $artifactContext)
}

$directory = if ($artifactContext) { $artifactContext.Directory } else { '<none>' }
Write-Output "HarnessSelfTest runId=$RunId result=$(if ($passedAll) { 'pass' } else { 'fail' }) scripts=$($harnessScripts.Count) suites=$($suiteResults.Count) durationSeconds=$durationSeconds directory=$directory"
if (-not $passedAll) { exit 1 }
