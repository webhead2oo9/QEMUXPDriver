$ErrorActionPreference = 'Stop'

function Assert-AcceptanceLauncher {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-AcceptanceLauncherExactNames {
    param(
        [object[]]$Actual,
        [string[]]$Expected,
        [string]$Label
    )

    $actualNames = @($Actual | ForEach-Object { [string]$_ } | Sort-Object)
    $expectedNames = @($Expected | Sort-Object)
    $actualText = $actualNames -join "`n"
    $expectedText = $expectedNames -join "`n"
    Assert-AcceptanceLauncher `
        ($actualNames.Count -eq $expectedNames.Count -and
         $actualText -ceq $expectedText) `
        "$Label names are wrong; expected=[$($expectedNames -join ',')] actual=[$($actualNames -join ',')]"
}

$launcher = Join-Path $PSScriptRoot 'invoke-xp-acceptance.ps1'

function Invoke-IsolatedAcceptanceLauncher {
    param([string[]]$Arguments)

    $powershell = Join-Path $PSHOME 'powershell.exe'
    $savedErrorActionPreference = $ErrorActionPreference
    $rawOutput = @()
    $exitCode = $null
    try {
        $ErrorActionPreference = 'Continue'
        $rawOutput = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $launcher @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($null -eq $exitCode) { $exitCode = 1 }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = @($rawOutput | ForEach-Object { $_.ToString() }) -join `
            [Environment]::NewLine
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-acceptance-launcher-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    $contractPath = (Resolve-Path (Join-Path $PSScriptRoot `
        'acceptance-run-contract.ps1')).Path.Replace("'", "''")
    $artifactPath = (Resolve-Path (Join-Path $PSScriptRoot `
        'acceptance-run-artifacts.ps1')).Path.Replace("'", "''")
    $fakeRunner = Join-Path $testRoot 'fake-runner.ps1'
    $fakeSource = @"
param(
    [string]`$ArtifactDirectory,
    [string]`$RunId,
    [string]`$TaskId,
    [string]`$Tier,
    [switch]`$Diagnostic,
    [switch]`$Synthetic
)
`$ErrorActionPreference = 'Stop'
. '$contractPath'
. '$artifactPath'
`$context = Initialize-XpAcceptanceRunArtifacts -Directory `$ArtifactDirectory ``
    -RunId `$RunId -TaskId `$TaskId -Tier `$Tier ``
    -CommandLine ([Environment]::CommandLine)
`$result = [pscustomobject][ordered]@{
    schema = 1
    kind = 'synthetic-acceptance-run'
    taskId = `$TaskId
    runId = `$RunId
    tier = `$Tier
    diagnostic = [bool]`$Diagnostic
    result = 'pass'
    harnessStatus = 'passed'
    exitCode = 0
}
[void](Write-XpAcceptanceRunResult -Context `$context -Result `$result)
[void](Write-XpAcceptanceRunManifest -Context `$context)
Write-Output "SyntheticRunner runId=`$RunId synthetic=`$Synthetic"
Write-Output "HarnessRun runId=`$RunId status=passed"
"@
    Set-Content -LiteralPath $fakeRunner -Value $fakeSource `
        -Encoding UTF8

    $bundle = Join-Path $testRoot 'bundle'
    $output = & $launcher -ArtifactDirectory $bundle `
        -RunId 'launcher-contract' -TaskId 'launcher-task' `
        -Tier T1 -Diagnostic -RunnerPath $fakeRunner `
        -RunnerParameters @{ Synthetic = $true } | Out-String
    Assert-AcceptanceLauncher `
        ($output.Contains(
            'SyntheticRunner runId=launcher-contract synthetic=True')) `
        "synthetic child output was not streamed: $output"
    Assert-AcceptanceLauncher `
        ($output.Contains(
            'AcceptanceLaunch runId=launcher-contract childExitCode=0 finalized=True phase=finalized childStarted=True')) `
        "launcher did not report finalization: $output"
    $canonicalBundleNames = @(
        'artifact-manifest.json', 'command.txt', 'console.log',
        'harness-result.json', 'launcher-command.json')
    $canonicalManifestNames = @(
        'command.txt', 'console.log', 'harness-result.json',
        'launcher-command.json')
    foreach ($name in $canonicalBundleNames) {
        Assert-AcceptanceLauncher `
            (Test-Path -LiteralPath (Join-Path $bundle $name) `
                -PathType Leaf) `
            "launcher bundle is missing $name"
    }
    Assert-AcceptanceLauncherExactNames `
        -Actual @(Get-ChildItem -LiteralPath $bundle -Force |
            ForEach-Object { $_.Name }) `
        -Expected $canonicalBundleNames -Label 'successful bundle directory'
    $result = Get-Content -Raw -LiteralPath `
        (Join-Path $bundle 'harness-result.json') | ConvertFrom-Json
    Assert-AcceptanceLauncher `
        ($result.kind -eq 'synthetic-acceptance-run' -and
         $result.taskId -eq 'launcher-task' -and
         $result.runId -eq 'launcher-contract' -and
         $result.tier -eq 'T1' -and
         $result.diagnostic -eq $true -and
         $result.result -eq 'pass' -and
         $result.harnessStatus -eq 'passed' -and
         $result.exitCode -eq 0 -and
         $result.launcher.phase -eq 'finalized' -and
         $result.launcher.childStarted -eq $true -and
         $result.launcher.finalized -eq $true -and
         $result.launcher.childExitCode -eq 0 -and
         $result.launcher.console -eq 'console.log' -and
         $result.launcher.command -eq 'launcher-command.json') `
        'launcher finalization did not preserve success identity'
    Assert-AcceptanceLauncherExactNames -Actual @($result.artifacts) `
        -Expected @('command.txt', 'console.log', 'launcher-command.json') `
        -Label 'successful result artifact references'
    $launch = Get-Content -Raw -LiteralPath `
        (Join-Path $bundle 'launcher-command.json') | ConvertFrom-Json
    Assert-AcceptanceLauncher `
        ($launch.executable.EndsWith('powershell.exe') -and
         $launch.arguments -contains '-Synthetic') `
        'launcher command record is incomplete'
    $console = Get-Content -Raw -LiteralPath `
        (Join-Path $bundle 'console.log')
    Assert-AcceptanceLauncher `
        ($console.Contains('SyntheticRunner runId=launcher-contract') -and
         $console.Contains('HarnessRun runId=launcher-contract') -and
         -not $console.Contains('AcceptanceLaunch runId=')) `
        'final console artifact is incomplete'
    $manifest = Get-Content -Raw -LiteralPath `
        (Join-Path $bundle 'artifact-manifest.json') | ConvertFrom-Json
    Assert-AcceptanceLauncher `
        ($manifest.schema -eq 1 -and
         $manifest.runId -eq 'launcher-contract') `
        'successful bundle manifest identity is wrong'
    Assert-AcceptanceLauncherExactNames `
        -Actual @($manifest.files | ForEach-Object { $_.name }) `
        -Expected $canonicalManifestNames -Label 'successful bundle manifest'
    foreach ($record in $manifest.files) {
        $path = Join-Path $bundle $record.name
        Assert-AcceptanceLauncher (Test-Path -LiteralPath $path) `
            "launcher manifest references missing $($record.name)"
        Assert-AcceptanceLauncher `
            ((Get-Item -LiteralPath $path).Length -eq $record.size -and
             (Get-FileHash -LiteralPath $path `
                -Algorithm SHA256).Hash -eq $record.sha256) `
            "launcher manifest identity is wrong for $($record.name)"
    }
    Assert-AcceptanceLauncher `
        (-not @(Get-ChildItem -LiteralPath $testRoot -Force |
            Where-Object { $_.Name -match '^\.launcher-contract\.console-' }).Count) `
        'launcher left its staging console behind'

    # A launched child can fail before creating its artifact context. The
    # launcher must propagate that exit and retain only its sibling staging
    # console; it cannot claim that a canonical bundle was finalized.
    $preBundleRunner = Join-Path $testRoot 'pre-bundle-runner.ps1'
    $preBundleSource = @'
param(
    [string]$ArtifactDirectory,
    [string]$RunId,
    [string]$TaskId,
    [string]$Tier,
    [switch]$Diagnostic
)
Write-Output "SyntheticPreBundle runId=$RunId stdout=present"
[Console]::Error.WriteLine(
    "SyntheticPreBundle runId=$RunId stderr=present")
exit 9
'@
    Set-Content -LiteralPath $preBundleRunner -Value $preBundleSource `
        -Encoding UTF8
    $preBundle = Join-Path $testRoot 'pre-bundle'
    $preBundleLaunch = Invoke-IsolatedAcceptanceLauncher -Arguments @(
        '-ArtifactDirectory', $preBundle,
        '-RunId', 'launcher-pre-bundle',
        '-TaskId', 'launcher-task',
        '-Tier', 'T0',
        '-RunnerPath', $preBundleRunner)
    Assert-AcceptanceLauncher ($preBundleLaunch.ExitCode -eq 9) `
        "pre-bundle child exit was not propagated: $($preBundleLaunch.Output)"
    Assert-AcceptanceLauncher `
        ($preBundleLaunch.Output.Contains(
            'AcceptanceLaunch runId=launcher-pre-bundle childExitCode=9 finalized=False phase=finalization-failed childStarted=True') -and
         $preBundleLaunch.Output.Contains('stagingConsole=')) `
        "pre-bundle failure did not report retained staging: $($preBundleLaunch.Output)"
    Assert-AcceptanceLauncher (-not (Test-Path -LiteralPath $preBundle)) `
        'pre-bundle failure unexpectedly created a canonical directory'
    $preBundleStaging = @(Get-ChildItem -LiteralPath $testRoot -Force |
        Where-Object {
            $_.Name -match '^\.launcher-pre-bundle\.console-' -and
            -not $_.PSIsContainer
        })
    Assert-AcceptanceLauncher ($preBundleStaging.Count -eq 1) `
        'pre-bundle failure did not retain exactly one staging console'
    $preBundleConsole = Get-Content -Raw -LiteralPath `
        $preBundleStaging[0].FullName
    Assert-AcceptanceLauncher `
        ($preBundleConsole.Contains(
            'SyntheticPreBundle runId=launcher-pre-bundle stdout=present') -and
         $preBundleConsole.Contains(
            'SyntheticPreBundle runId=launcher-pre-bundle stderr=present')) `
        'pre-bundle staging console lost child stdout or stderr'

    # A child that publishes its result before returning nonzero must retain a
    # complete canonical bundle while the launcher propagates the child exit.
    $postBundleRunner = Join-Path $testRoot 'post-bundle-runner.ps1'
    $postBundleSource = @"
param(
    [string]`$ArtifactDirectory,
    [string]`$RunId,
    [string]`$TaskId,
    [string]`$Tier,
    [switch]`$Diagnostic
)
`$ErrorActionPreference = 'Stop'
. '$contractPath'
. '$artifactPath'
`$context = Initialize-XpAcceptanceRunArtifacts -Directory `$ArtifactDirectory ``
    -RunId `$RunId -TaskId `$TaskId -Tier `$Tier ``
    -CommandLine ([Environment]::CommandLine)
`$result = [pscustomobject][ordered]@{
    schema = 1
    kind = 'synthetic-post-bundle-failure'
    taskId = `$TaskId
    runId = `$RunId
    tier = `$Tier
    result = 'fail'
    harnessStatus = 'failed'
    exitCode = 23
}
[void](Write-XpAcceptanceRunResult -Context `$context -Result `$result)
[void](Write-XpAcceptanceRunManifest -Context `$context)
Write-Output "SyntheticPostBundle runId=`$RunId stdout=present"
[Console]::Error.WriteLine(
    "SyntheticPostBundle runId=`$RunId stderr=present")
exit 23
"@
    Set-Content -LiteralPath $postBundleRunner -Value $postBundleSource `
        -Encoding UTF8
    $postBundle = Join-Path $testRoot 'post-bundle'
    $postBundleLaunch = Invoke-IsolatedAcceptanceLauncher -Arguments @(
        '-ArtifactDirectory', $postBundle,
        '-RunId', 'launcher-post-bundle',
        '-TaskId', 'launcher-task',
        '-Tier', 'T0',
        '-RunnerPath', $postBundleRunner)
    Assert-AcceptanceLauncher ($postBundleLaunch.ExitCode -eq 23) `
        "post-bundle child exit was not propagated: $($postBundleLaunch.Output)"
    Assert-AcceptanceLauncher `
        ($postBundleLaunch.Output.Contains(
            'AcceptanceLaunch runId=launcher-post-bundle childExitCode=23 finalized=True phase=finalized childStarted=True')) `
        "post-bundle failure was not finalized: $($postBundleLaunch.Output)"
    foreach ($name in $canonicalBundleNames) {
        Assert-AcceptanceLauncher `
            (Test-Path -LiteralPath (Join-Path $postBundle $name) `
                -PathType Leaf) `
            "post-bundle failure is missing canonical artifact $name"
    }
    Assert-AcceptanceLauncherExactNames `
        -Actual @(Get-ChildItem -LiteralPath $postBundle -Force |
            ForEach-Object { $_.Name }) `
        -Expected $canonicalBundleNames `
        -Label 'post-bundle failure directory'
    $postBundleResult = Get-Content -Raw -LiteralPath `
        (Join-Path $postBundle 'harness-result.json') | ConvertFrom-Json
    Assert-AcceptanceLauncher `
        ($postBundleResult.kind -eq 'synthetic-post-bundle-failure' -and
         $postBundleResult.taskId -eq 'launcher-task' -and
         $postBundleResult.runId -eq 'launcher-post-bundle' -and
         $postBundleResult.tier -eq 'T0' -and
         $postBundleResult.result -eq 'fail' -and
         $postBundleResult.harnessStatus -eq 'failed' -and
         $postBundleResult.exitCode -eq 23 -and
         $postBundleResult.launcher.phase -eq 'finalized' -and
         $postBundleResult.launcher.childStarted -eq $true -and
         $postBundleResult.launcher.finalized -eq $true -and
         $postBundleResult.launcher.childExitCode -eq 23 -and
         $postBundleResult.launcher.console -eq 'console.log' -and
         $postBundleResult.launcher.command -eq 'launcher-command.json') `
        'post-bundle structured failure identity is wrong'
    Assert-AcceptanceLauncherExactNames `
        -Actual @($postBundleResult.artifacts) `
        -Expected @('command.txt', 'console.log', 'launcher-command.json') `
        -Label 'post-bundle result artifact references'
    $postBundleConsole = Get-Content -Raw -LiteralPath `
        (Join-Path $postBundle 'console.log')
    Assert-AcceptanceLauncher `
        ($postBundleConsole.Contains(
            'SyntheticPostBundle runId=launcher-post-bundle stdout=present') -and
         $postBundleConsole.Contains(
            'SyntheticPostBundle runId=launcher-post-bundle stderr=present') -and
         -not $postBundleConsole.Contains('AcceptanceLaunch runId=')) `
        'post-bundle console content or final-line exclusion is wrong'
    $postBundleManifest = Get-Content -Raw -LiteralPath `
        (Join-Path $postBundle 'artifact-manifest.json') | ConvertFrom-Json
    Assert-AcceptanceLauncher `
        ($postBundleManifest.schema -eq 1 -and
         $postBundleManifest.runId -eq 'launcher-post-bundle') `
        'post-bundle manifest identity is wrong'
    Assert-AcceptanceLauncherExactNames `
        -Actual @($postBundleManifest.files | ForEach-Object { $_.name }) `
        -Expected $canonicalManifestNames `
        -Label 'post-bundle failure manifest'
    foreach ($record in $postBundleManifest.files) {
        $path = Join-Path $postBundle $record.name
        Assert-AcceptanceLauncher `
            ((Get-Item -LiteralPath $path).Length -eq $record.size -and
             (Get-FileHash -LiteralPath $path `
                -Algorithm SHA256).Hash -eq $record.sha256) `
            "post-bundle manifest identity is wrong for $($record.name)"
    }
    Assert-AcceptanceLauncher `
        (-not @(Get-ChildItem -LiteralPath $testRoot -Force |
            Where-Object {
                $_.Name -match '^\.launcher-post-bundle\.console-'
            }).Count) `
        'post-bundle finalization left its staging console behind'

    # Invalid identity is rejected before child launch. This isolated call
    # proves the machine-readable phase marker as well as the empty filesystem
    # outcome; the hashtable-specific checks below cover other validation types.
    $preChildParent = Join-Path $testRoot 'pre-child-parent'
    [void](New-Item -ItemType Directory -Path $preChildParent)
    $preChildDirectory = Join-Path $preChildParent 'bundle'
    $preChildLaunch = Invoke-IsolatedAcceptanceLauncher -Arguments @(
        '-ArtifactDirectory', $preChildDirectory,
        '-RunId', 'invalid run id',
        '-RunnerPath', $fakeRunner)
    Assert-AcceptanceLauncher ($preChildLaunch.ExitCode -eq 1) `
        "pre-child validation exit is wrong: $($preChildLaunch.ExitCode)"
    Assert-AcceptanceLauncher `
        ($preChildLaunch.Output.Contains(
            'AcceptanceLaunch runId=<invalid> childExitCode=not-started finalized=False phase=pre-child childStarted=False') -and
         $preChildLaunch.Output.Contains('Invalid launcher run ID')) `
        "pre-child lifecycle marker is incomplete: $($preChildLaunch.Output)"
    Assert-AcceptanceLauncher `
        (-not (Test-Path -LiteralPath $preChildDirectory) -and
         -not @(Get-ChildItem -LiteralPath $preChildParent -Force).Count) `
        'pre-child validation created bundle or staging evidence'

    $unknownRejected = $false
    $unknownDirectory = Join-Path $testRoot 'unknown'
    try {
        [void](& $launcher -ArtifactDirectory `
            $unknownDirectory -RunId 'launcher-unknown' `
            -RunnerPath $fakeRunner -RunnerParameters @{ Bogus = 1 })
    } catch {
        $unknownRejected = $_.Exception.Message.Contains(
            'Unknown acceptance runner parameter -Bogus')
    }
    Assert-AcceptanceLauncher $unknownRejected `
        'launcher did not reject an unknown runner parameter'
    Assert-AcceptanceLauncher `
        (-not (Test-Path -LiteralPath $unknownDirectory) -and
         -not @(Get-ChildItem -LiteralPath $testRoot -Force |
            Where-Object {
                $_.Name -match '^\.launcher-unknown\.console-'
            }).Count) `
        'pre-child validation created a bundle or staging console'

    $reservedRejected = $false
    $reservedDirectory = Join-Path $testRoot 'reserved'
    try {
        [void](& $launcher -ArtifactDirectory `
            $reservedDirectory -RunId 'launcher-reserved' `
            -RunnerPath $fakeRunner `
            -RunnerParameters @{ RunId = 'override' })
    } catch {
        $reservedRejected = $_.Exception.Message.Contains(
            'Runner parameter -RunId is owned by the launcher')
    }
    Assert-AcceptanceLauncher $reservedRejected `
        'launcher did not reject a reserved runner parameter'
    Assert-AcceptanceLauncher `
        (-not (Test-Path -LiteralPath $reservedDirectory) -and
         -not @(Get-ChildItem -LiteralPath $testRoot -Force |
            Where-Object {
                $_.Name -match '^\.launcher-reserved\.console-'
            }).Count) `
        'reserved-key validation created a bundle or staging console'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Output 'AcceptanceLauncherLifecycle success=pass preChild=pass preBundle=pass postBundle=pass'
Write-Output 'acceptance launcher tests passed'
