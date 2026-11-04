param(
    [Parameter(Mandatory = $true)]
    [string]$ArtifactDirectory,
    [Parameter(Mandatory = $true)]
    [string]$RunId,
    [string]$TaskId,
    [ValidatePattern('^T[0-3](?:-diagnostic)?$')]
    [string]$Tier = 'T2',
    [switch]$Diagnostic,
    [hashtable]$RunnerParameters = @{},
    [string]$RunnerPath = (Join-Path $PSScriptRoot 'run-xp-acceptance.ps1')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')

try {
    if (-not (Test-XpAcceptanceRunId -RunId $RunId)) {
        throw "Invalid launcher run ID '$RunId'"
    }
    $runner = (Resolve-Path -LiteralPath $RunnerPath).Path
    $tokens = $null
    $parseErrors = $null
    $runnerAst = [Management.Automation.Language.Parser]::ParseFile(
        $runner, [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) {
        throw "Acceptance runner did not parse: $($parseErrors -join '; ')"
    }
    $runnerParameterNames = @($runnerAst.ParamBlock.Parameters |
        ForEach-Object { $_.Name.VariablePath.UserPath })
    $reserved = @(
        'ArtifactDirectory', 'RunId', 'TaskId', 'Tier', 'Diagnostic')
    foreach ($parameterName in $RunnerParameters.Keys) {
        if ($reserved -contains $parameterName) {
            throw "Runner parameter -$parameterName is owned by the launcher"
        }
        if ($runnerParameterNames -notcontains $parameterName) {
            throw "Unknown acceptance runner parameter -$parameterName"
        }
    }

    $fullArtifactDirectory = [IO.Path]::GetFullPath($ArtifactDirectory)
    $artifactParent = Split-Path -Parent $fullArtifactDirectory
    if (-not (Test-Path -LiteralPath $artifactParent -PathType Container)) {
        throw "Artifact parent directory does not exist: $artifactParent"
    }
} catch {
    $reportedRunId = if (Test-XpAcceptanceRunId -RunId $RunId) {
        $RunId
    } else { '<invalid>' }
    $failureType = $_.Exception.GetType().FullName
    $failureMessage = $_.Exception.Message.Replace("`r", ' ').Replace("`n", ' ')
    Write-Output "AcceptanceLaunch runId=$reportedRunId childExitCode=not-started finalized=False phase=pre-child childStarted=False failureType=$failureType failure=$failureMessage"
    throw
}
$consoleStagingPath = Join-Path $artifactParent `
    ('.{0}.console-{1}-{2}.log' -f $RunId, $PID,
        [Guid]::NewGuid().ToString('N'))
$powershell = Join-Path $PSHOME 'powershell.exe'
$childArguments = [Collections.Generic.List[string]]::new()
foreach ($argument in @(
        '-NoLogo', '-NoProfile', '-NonInteractive',
        '-ExecutionPolicy', 'Bypass', '-File', $runner,
        '-ArtifactDirectory', $fullArtifactDirectory,
        '-RunId', $RunId,
        '-TaskId', $(if ([string]::IsNullOrWhiteSpace($TaskId)) {
            $RunId
        } else { $TaskId }),
        '-Tier', $Tier)) {
    $childArguments.Add([string]$argument)
}
if ($Diagnostic) { $childArguments.Add('-Diagnostic') }
foreach ($parameterName in @($RunnerParameters.Keys | Sort-Object)) {
    $value = $RunnerParameters[$parameterName]
    if ($value -is [Management.Automation.SwitchParameter] -or
        $value -is [bool]) {
        if ([bool]$value) { $childArguments.Add("-$parameterName") }
    } elseif ($null -ne $value) {
        $childArguments.Add("-$parameterName")
        $childArguments.Add([string]$value)
    }
}
$launchRecord = [pscustomobject][ordered]@{
    schema = 1
    executable = $powershell
    arguments = @($childArguments)
    started = Get-Date -Format o
}

$childExitCode = $null
$savedErrorActionPreference = $ErrorActionPreference
try {
    # Native stderr is evidence, not a wrapper exception. Merge it into the
    # ordered console stream while retaining the child's process exit code.
    $ErrorActionPreference = 'Continue'
    & $powershell @childArguments 2>&1 |
        Tee-Object -FilePath $consoleStagingPath
    $childExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
if ($null -eq $childExitCode) { $childExitCode = 1 }

$finalized = $false
$finalizationError = $null
try {
    if (-not (Test-Path -LiteralPath $fullArtifactDirectory `
            -PathType Container)) {
        throw "Child did not create its artifact directory: $fullArtifactDirectory"
    }
    $context = [pscustomobject]@{
        Directory = $fullArtifactDirectory
        RunId = $RunId
        TaskId = if ([string]::IsNullOrWhiteSpace($TaskId)) {
            $RunId
        } else { $TaskId }
        Tier = $Tier
        StartedUtc = $launchRecord.started
    }
    [void](Complete-XpAcceptanceLauncherArtifacts `
        -Context $context -ConsoleStagingPath $consoleStagingPath `
        -LaunchRecord $launchRecord -ChildExitCode $childExitCode)
    $finalized = $true
} catch {
    $finalizationError = $_.Exception.Message.Replace("`r", ' ').Replace("`n", ' ')
}

if ($finalized) {
    Remove-Item -LiteralPath $consoleStagingPath -Force
    Write-Output "AcceptanceLaunch runId=$RunId childExitCode=$childExitCode finalized=True phase=finalized childStarted=True directory=$fullArtifactDirectory console=console.log result=harness-result.json manifest=artifact-manifest.json"
} else {
    Write-Warning "Acceptance launcher artifact finalization failed: $finalizationError"
    Write-Output "AcceptanceLaunch runId=$RunId childExitCode=$childExitCode finalized=False phase=finalization-failed childStarted=True stagingConsole=$consoleStagingPath failure=$finalizationError"
}
if (-not $finalized -and $childExitCode -eq 0) { exit 1 }
if ($childExitCode -ne 0) { exit $childExitCode }
