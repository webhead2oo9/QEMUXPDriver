$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')

function Assert-TestHandback {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$preflightScript = Join-Path $PSScriptRoot 'verify-xp-test-preflight.ps1'
$handbackScript = Join-Path $PSScriptRoot 'verify-xp-test-handback.ps1'

function Invoke-IsolatedHarnessScript {
    param([string]$ScriptPath, [string[]]$Arguments)

    $powershell = Join-Path $PSHOME 'powershell.exe'
    $savedErrorActionPreference = $ErrorActionPreference
    $rawOutput = @()
    $exitCode = $null
    try {
        $ErrorActionPreference = 'Continue'
        $rawOutput = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $ScriptPath @Arguments 2>&1)
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

function Get-FreeTcpPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function New-FakeWorkspace {
    param([string]$Root)

    $vmDir = Join-Path $Root 'qemu\winxp'
    $harnessDir = Join-Path $Root 'qemu\harness'
    [void](New-Item -ItemType Directory -Path $vmDir -Force)
    [void](New-Item -ItemType Directory -Path $harnessDir -Force)
    [IO.File]::WriteAllBytes((Join-Path $vmDir 'my_pc_disk.qcow2'),
        [byte[]](1..16))
    # The fixture registry must satisfy the canonical validator that the
    # preflight invokes, so every required target property is present.
    $registry = '{"schemaVersion":1,' +
        '"current":{"fullCheckpointTarget":"preserved-one",' +
        '"manualGamesTarget":"preserved-one",' +
        '"gamesBackingTarget":"preserved-one"},' +
        '"targets":[{"id":"preserved-one",' +
        '"purpose":"handback fixture preserved checkpoint",' +
        '"lifecycle":"preserved-checkpoint",' +
        '"mutationPolicy":"child-only",' +
        '"contentPath":"qemu/winxp/preserved-target.qemu_cmd_line",' +
        '"diskPath":"qemu/winxp/preserved-target.qcow2",' +
        '"backingDiskPath":"qemu/winxp/backing-base.qcow2",' +
        '"installedDriver":"1.0.0.1","network":"offline",' +
        '"qmpPort":49552,"launcher":"retro_host",' +
        '"allowedTiers":["T2"],"lastVerified":"2026-07-17",' +
        '"notes":"handback fixture"}]}'
    [IO.File]::WriteAllText((Join-Path $harnessDir 'test-targets.json'),
        $registry)
    [IO.File]::WriteAllText((Join-Path $Root 'tracked-notes.txt'), 'seed')
    [void](New-Item -ItemType Directory -Path `
        (Join-Path $Root '.tools\pycdlib\pycdlib') -Force)
    & git -C $Root init -q
    & git -C $Root config user.email 'test@example.invalid'
    & git -C $Root config user.name 'Harness Test'
    & git -C $Root add -A
    & git -C $Root -c commit.gpgsign=false commit -q -m 'fake workspace'
    Assert-TestHandback ($LASTEXITCODE -eq 0) "git setup failed for $Root"
}

function New-FakeBundle {
    param(
        [string]$Directory,
        [string]$TaskId = 'fake-task',
        [string]$RunId = 'fake',
        [int]$ChildExitCode = 0,
        [string]$RunResult = 'pass',
        [int]$ResultExitCode = 0,
        [switch]$NoLauncher,
        [switch]$NoManifest,
        [switch]$NoResult
    )

    [void](New-Item -ItemType Directory -Path $Directory -Force)
    Write-XpAcceptanceAtomicText -Path (Join-Path $Directory 'command.txt') `
        -Text "fake command`n"
    Write-XpAcceptanceAtomicText -Path (Join-Path $Directory 'console.log') `
        -Text "fake console`n"
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $Directory 'launcher-command.json') `
        -Text "{`"schema`":1}`n"
    if (-not $NoResult) {
        $result = [pscustomobject][ordered]@{
            schema = 1
            kind = 'xpdriver-acceptance-run'
            taskId = $TaskId
            runId = $RunId
            result = $RunResult
            exitCode = $ResultExitCode
        }
        if (-not $NoLauncher) {
            $result | Add-Member -NotePropertyName launcher `
                -NotePropertyValue ([pscustomobject][ordered]@{
                    phase = 'finalized'
                    childStarted = $true
                    finalized = $true
                    childExitCode = $ChildExitCode
                    console = 'console.log'
                    command = 'launcher-command.json'
                })
        }
        Write-XpAcceptanceAtomicText -Path `
            (Join-Path $Directory 'harness-result.json') `
            -Text (($result | ConvertTo-Json -Depth 6) + "`n")
    }
    if (-not $NoManifest) {
        [void](Write-XpAcceptanceRunManifest -Context (
            [pscustomobject]@{ Directory = $Directory; RunId = $RunId }))
    }
}

function Get-HandbackCheck {
    param([object]$Record, [string]$Name)
    return @($Record.checks | Where-Object { $_.name -eq $Name })[0]
}

# Builds a fresh workspace and captures a genuine preflight start state.
function New-PreflightedCase {
    param(
        [string]$CaseRoot,
        [switch]$HashBase,
        [switch]$WithPort
    )

    $workspace = Join-Path $CaseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $outputPath = Join-Path $CaseRoot 'start-state.json'
    $arguments = @(
        '-TaskId', ('handback-' + (Split-Path -Leaf $CaseRoot)),
        '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace,
        '-HostRepoPath', (Join-Path $CaseRoot 'absent-host'))
    if ($HashBase) { $arguments += '-HashBase' }
    if ($WithPort) {
        $script:casePort = Get-FreeTcpPort
        $arguments += @('-QmpPort', "$($script:casePort)")
    }
    $run = Invoke-IsolatedHarnessScript -ScriptPath $preflightScript `
        -Arguments $arguments
    Assert-TestHandback ($run.ExitCode -eq 0) `
        "case preflight failed: $($run.Output)"
    return $outputPath
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-test-handback-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
$heldLease = $null
$leftoverProcess = $null
try {
    # A quiet window hands back cleanly.
    $caseRoot = Join-Path $testRoot 'case-pass'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 0) `
        "pass case failed: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    Assert-TestHandback ($record.kind -eq 'xpdriver-test-handback' -and
        $record.schema -eq 1 -and $record.result -eq 'pass' -and
        $record.taskId -eq 'handback-case-pass') `
        'pass case record identity is wrong'

    # A touched base write time is a mutated base.
    $caseRoot = Join-Path $testRoot 'case-mtime'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $basePath = Join-Path $caseRoot 'workspace\qemu\winxp\my_pc_disk.qcow2'
    (Get-Item -LiteralPath $basePath).LastWriteTimeUtc = `
        (Get-Item -LiteralPath $basePath).LastWriteTimeUtc.AddHours(1)
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "touched base was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'baseImageUnchanged'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'BASE_IMAGE_MUTATED') `
        'base write-time check is wrong'

    # Same-size content with a restored write time is caught only by the hash.
    $caseRoot = Join-Path $testRoot 'case-hash'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot -HashBase
    $basePath = Join-Path $caseRoot 'workspace\qemu\winxp\my_pc_disk.qcow2'
    $originalWriteTime = (Get-Item -LiteralPath $basePath).LastWriteTimeUtc
    [IO.File]::WriteAllBytes($basePath, [byte[]](16..31))
    (Get-Item -LiteralPath $basePath).LastWriteTimeUtc = $originalWriteTime
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "rewritten base was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'baseImageUnchanged'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'BASE_IMAGE_MUTATED') `
        'base hash check is wrong'

    # A new dirty file in the repository is workspace mutation.
    $caseRoot = Join-Path $testRoot 'case-drift'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    [IO.File]::WriteAllText((Join-Path $caseRoot 'workspace\dirty-note.txt'),
        'drift')
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "repository drift was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'repositoryUnchanged:xpdriver'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'WORKSPACE_MUTATED' -and
        $check.detail -like '*dirty-note.txt*') `
        'repository drift check is wrong'

    # Editing an already-dirty tracked file keeps its status line; only the
    # per-file content identity can catch it.
    $caseRoot = Join-Path $testRoot 'case-dirty-content'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $trackedPath = Join-Path $workspace 'tracked-notes.txt'
    [IO.File]::WriteAllText($trackedPath, 'version one')
    $startState = Join-Path $caseRoot 'start-state.json'
    $preRun = Invoke-IsolatedHarnessScript -ScriptPath $preflightScript `
        -Arguments @('-TaskId', 'handback-case-dirty-content',
            '-OutputPath', $startState, '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
    Assert-TestHandback ($preRun.ExitCode -eq 0) `
        "case preflight failed: $($preRun.Output)"
    [IO.File]::WriteAllText($trackedPath, 'version two')
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "edited dirty file was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'repositoryUnchanged:xpdriver'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'WORKSPACE_MUTATED' -and
        $check.detail -like '*tracked-notes.txt*' -and
        $check.detail -notlike '*status added*') `
        "dirty content check is wrong: $($check.detail)"

    # Editing an already-untracked file keeps its ?? status line; the content
    # identity must catch it too.
    $caseRoot = Join-Path $testRoot 'case-untracked-content'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $untrackedPath = Join-Path $workspace 'scratch.md'
    [IO.File]::WriteAllText($untrackedPath, 'first')
    $startState = Join-Path $caseRoot 'start-state.json'
    $preRun = Invoke-IsolatedHarnessScript -ScriptPath $preflightScript `
        -Arguments @('-TaskId', 'handback-case-untracked-content',
            '-OutputPath', $startState, '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
    Assert-TestHandback ($preRun.ExitCode -eq 0) `
        "case preflight failed: $($preRun.Output)"
    [IO.File]::WriteAllText($untrackedPath, 'second')
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "edited untracked file was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'repositoryUnchanged:xpdriver'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'WORKSPACE_MUTATED' -and
        $check.detail -like '*scratch.md*' -and
        $check.detail -notlike '*status added*') `
        "untracked content check is wrong: $($check.detail)"

    # A failed preflight poisons the whole window.
    $caseRoot = Join-Path $testRoot 'case-failed-preflight'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $failedState = Get-Content -Raw -LiteralPath $startState | ConvertFrom-Json
    $failedState.result = 'fail'
    $failedPath = Join-Path $caseRoot 'start-state-failed.json'
    [IO.File]::WriteAllText($failedPath,
        (($failedState | ConvertTo-Json -Depth 12) + "`n"))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $failedPath, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "failed preflight was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'preflightPassed'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'PREFLIGHT_FAILED') `
        'failed preflight check is wrong'

    # A well-formed bundle and package audit cleanly.
    $caseRoot = Join-Path $testRoot 'case-bundle'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'handback-case-bundle'
    $packageDir = Join-Path $caseRoot 'package'
    [void](New-Item -ItemType Directory -Path $packageDir)
    [IO.File]::WriteAllText((Join-Path $packageDir 'qxp.inf'), 'inf')
    [IO.File]::WriteAllBytes((Join-Path $packageDir 'qxpmp.sys'),
        [byte[]](1..8))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 0) `
        "bundle case failed: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    Assert-TestHandback (@($record.audit.runBundles)[0].ok) `
        'well-formed bundle did not audit cleanly'

    # A stub package directory is recorded but fails the identity gate:
    # existence alone must never bless the audited package.
    $outputPath = Join-Path $caseRoot 'handback-package.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle, '-PackageDirectory', $packageDir)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "stub package directory was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'packageDirectory'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID') `
        'stub package check is wrong'
    Assert-TestHandback (@($record.audit.package.files).Count -eq 2 -and
        $record.audit.package.files[0].sha256) `
        'package membership was not recorded'

    # A bundle from a different task, or whose manifest names a different
    # run than its result, is mis-wired evidence.
    $caseRoot = Join-Path $testRoot 'case-foreign-task'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'some-other-task'
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "foreign-task bundle was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID' -and
        $check.detail -like '*does not match the preflight taskId*') `
        'foreign-task bundle check is wrong'

    $caseRoot = Join-Path $testRoot 'case-runid-mismatch'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'handback-case-runid-mismatch' `
        -RunId 'run-one'
    [void](Write-XpAcceptanceRunManifest -Context (
        [pscustomobject]@{ Directory = $bundle; RunId = 'run-two' }))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "runId-mismatched bundle was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID' -and
        $check.detail -like '*does not match result runId*') `
        'runId-mismatch bundle check is wrong'

    # A T0 aggregate bundle has no launcher identity by design: it passes as
    # -T0Directory and must fail as -RunDirectory.
    $caseRoot = Join-Path $testRoot 'case-t0'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $t0Dir = Join-Path $caseRoot 'runs\fake-t0'
    [void](New-Item -ItemType Directory -Path $t0Dir)
    Write-XpAcceptanceAtomicText -Path (Join-Path $t0Dir 'command.txt') `
        -Text "fake t0`n"
    $t0Result = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-harness-self-test'
        result = 'pass'
        exitCode = 0
        suites = @([pscustomobject][ordered]@{ name = 'a'; result = 'pass' })
    }
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $t0Dir 'harness-result.json') `
        -Text (($t0Result | ConvertTo-Json -Depth 6) + "`n")
    [void](Write-XpAcceptanceRunManifest -Context (
        [pscustomobject]@{ Directory = $t0Dir; RunId = 'fake-t0' }))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-T0Directory', $t0Dir)
    Assert-TestHandback ($run.ExitCode -eq 0) `
        "T0 aggregate was not accepted: $($run.Output)"
    $outputPath2 = Join-Path $caseRoot 'handback-two.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath2,
            '-RunDirectory', $t0Dir)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "T0 aggregate wrongly passed as a VM bundle: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath2 | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-t0'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID') `
        'T0-as-VM-bundle check is wrong'

    $caseRoot = Join-Path $testRoot 'case-t0-nomanifest'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $t0Dir = Join-Path $caseRoot 'runs\fake-t0'
    [void](New-Item -ItemType Directory -Path $t0Dir)
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $t0Dir 'harness-result.json') `
        -Text (($t0Result | ConvertTo-Json -Depth 6) + "`n")
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-T0Directory', $t0Dir)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "manifestless T0 aggregate was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 't0Bundle:fake-t0'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID') `
        'manifestless T0 check is wrong'

    # Incomplete bundles are invalid evidence, never quiet passes.
    foreach ($variant in @(
            @{ Mode = 'empty'; Expect = 'missing harness-result.json' },
            @{ Mode = 'nomanifest'; Expect = 'missing artifact-manifest.json' },
            @{ Mode = 'noresult'; Expect = 'missing harness-result.json' },
            @{ Mode = 'nolauncher'; Expect = 'lacks finalized launcher' })) {
        $caseRoot = Join-Path $testRoot "case-bundle-$($variant.Mode)"
        [void](New-Item -ItemType Directory -Path $caseRoot)
        $startState = New-PreflightedCase -CaseRoot $caseRoot
        $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
        $variantTaskId = "handback-case-bundle-$($variant.Mode)"
        switch ($variant.Mode) {
            'empty' { [void](New-Item -ItemType Directory -Path $bundle) }
            'nomanifest' { New-FakeBundle -Directory $bundle -NoManifest `
                -TaskId $variantTaskId }
            'noresult' { New-FakeBundle -Directory $bundle -NoResult `
                -TaskId $variantTaskId }
            'nolauncher' { New-FakeBundle -Directory $bundle -NoLauncher `
                -TaskId $variantTaskId }
        }
        $outputPath = Join-Path $caseRoot 'handback.json'
        $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
            -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
                '-RunDirectory', $bundle)
        Assert-TestHandback ($run.ExitCode -eq 1) `
            "$($variant.Mode) bundle was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
        Assert-TestHandback (-not $check.ok -and
            $check.failureClass -eq 'EVIDENCE_INVALID' -and
            $check.detail -like "*$($variant.Expect)*") `
            "$($variant.Mode) bundle check is wrong: $($check.detail)"
    }

    # A post-manifest bundle edit is invalid evidence.
    $caseRoot = Join-Path $testRoot 'case-corrupt'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'handback-case-corrupt'
    Add-Content -LiteralPath (Join-Path $bundle 'console.log') 'tampered'
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "corrupted bundle was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID' -and
        $check.detail -like '*console.log*') `
        'corrupted bundle check is wrong'

    # The launcher child exit must agree with the run verdict.
    $caseRoot = Join-Path $testRoot 'case-exit'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'handback-case-exit' `
        -ChildExitCode 1 -RunResult 'pass' -ResultExitCode 0
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "exit disagreement was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID') `
        'exit disagreement check is wrong'

    # Unparsable bundle JSON is invalid evidence.
    $caseRoot = Join-Path $testRoot 'case-badjson'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    New-FakeBundle -Directory $bundle -TaskId 'handback-case-badjson'
    [IO.File]::WriteAllText((Join-Path $bundle 'broken.json'), '{ broken')
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "unparsable bundle JSON was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID') `
        'unparsable JSON check is wrong'

    # Valid but falsey JSON (null, [], false) parses cleanly; a required
    # document that is not a non-null object must fail, never silently skip
    # its schema and contract checks. The bundle is built manifest-last so
    # the byte identity matches and only the object guard can catch it.
    $caseRoot = Join-Path $testRoot 'case-falsey-run'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $bundle = Join-Path $caseRoot 'runs\fake-run-r1'
    [void](New-Item -ItemType Directory -Path $bundle -Force)
    Write-XpAcceptanceAtomicText -Path (Join-Path $bundle 'console.log') `
        -Text "fake console`n"
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $bundle 'launcher-command.json') -Text "{`"schema`":1}`n"
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $bundle 'harness-result.json') -Text "null`n"
    [void](Write-XpAcceptanceRunManifest -Context (
        [pscustomobject]@{ Directory = $bundle; RunId = 'fake' }))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-RunDirectory', $bundle)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "null harness result was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'runBundle:fake-run-r1'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID' -and
        $check.detail -like '*not a JSON object*') `
        "null harness result check is wrong: $($check.detail)"

    $caseRoot = Join-Path $testRoot 'case-falsey-t0'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $t0Falsey = Join-Path $caseRoot 'runs\fake-t0'
    [void](New-Item -ItemType Directory -Path $t0Falsey -Force)
    Write-XpAcceptanceAtomicText -Path `
        (Join-Path $t0Falsey 'harness-result.json') -Text "[]`n"
    [void](Write-XpAcceptanceRunManifest -Context (
        [pscustomobject]@{ Directory = $t0Falsey; RunId = 'fake-t0' }))
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath,
            '-T0Directory', $t0Falsey)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "empty-array T0 result was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 't0Bundle:fake-t0'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'EVIDENCE_INVALID' -and
        $check.detail -like '*not a JSON object*') `
        "empty-array T0 result check is wrong: $($check.detail)"

    # A lease held by a pre-existing foreign owner is an ownership conflict.
    $caseRoot = Join-Path $testRoot 'case-lease'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $lockPath = Join-Path $caseRoot `
        'workspace\qemu\winxp\.xpdriver-acceptance-artifacts.lock'
    $heldLease = Enter-XpAcceptanceArtifactLease -LockPath $lockPath `
        -RunId 'handback-foreign-holder' -QmpPort 49101 `
        -RequestedTimeoutSeconds 150 -EffectiveTimeoutSeconds 180
    $outputPath = Join-Path $caseRoot 'handback.json'
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -eq 1) `
        "held lease was not rejected at handback: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-HandbackCheck $record 'leaseReleased'
    Assert-TestHandback (-not $check.ok -and
        $check.failureClass -eq 'VM_OWNERSHIP_CONFLICT') `
        'held lease check is wrong'
    Exit-XpAcceptanceArtifactLease -Lease $heldLease -Status passed
    $heldLease = $null

    # A still-bound assigned port is incomplete cleanup.
    $caseRoot = Join-Path $testRoot 'case-port'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $script:casePort = $null
    $startState = New-PreflightedCase -CaseRoot $caseRoot -WithPort
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,
        $script:casePort)
    $listener.Start()
    try {
        $outputPath = Join-Path $caseRoot 'handback.json'
        $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
            -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
        Assert-TestHandback ($run.ExitCode -eq 1) `
            "bound port was not rejected at handback: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-HandbackCheck $record 'qmpPortFree'
        Assert-TestHandback (-not $check.ok -and
            $check.failureClass -eq 'CLEANUP_INCOMPLETE') `
            'bound port check is wrong'
    } finally {
        $listener.Stop()
    }

    # A VM process started inside the window must not survive it.
    $caseRoot = Join-Path $testRoot 'case-process'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $fakeHost = Join-Path $caseRoot 'retro_host.exe'
    Copy-Item -LiteralPath (Join-Path $PSHOME 'powershell.exe') `
        -Destination $fakeHost
    $leftoverProcess = Start-Process -FilePath $fakeHost `
        -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 600' `
        -PassThru -WindowStyle Hidden
    try {
        $outputPath = Join-Path $caseRoot 'handback.json'
        $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
            -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
        Assert-TestHandback ($run.ExitCode -eq 1) `
            "leftover process was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-HandbackCheck $record 'processesReleased'
        Assert-TestHandback (-not $check.ok -and
            $check.failureClass -eq 'CLEANUP_INCOMPLETE' -and
            $check.detail -like "*$($leftoverProcess.Id)*") `
            'leftover process check is wrong'
    } finally {
        if ($leftoverProcess -and -not $leftoverProcess.HasExited) {
            Stop-Process -Id $leftoverProcess.Id -Force
        }
        $leftoverProcess = $null
    }

    # The output is never overwritten.
    $caseRoot = Join-Path $testRoot 'case-overwrite'
    [void](New-Item -ItemType Directory -Path $caseRoot)
    $startState = New-PreflightedCase -CaseRoot $caseRoot
    $outputPath = Join-Path $caseRoot 'handback.json'
    [IO.File]::WriteAllText($outputPath, 'prior evidence')
    $run = Invoke-IsolatedHarnessScript -ScriptPath $handbackScript `
        -Arguments @('-StartStatePath', $startState, '-OutputPath', $outputPath)
    Assert-TestHandback ($run.ExitCode -ne 0) `
        'existing handback output did not fail closed'
    Assert-TestHandback ((Get-Content -Raw -LiteralPath $outputPath) -eq `
        'prior evidence') 'existing handback output was overwritten'
} finally {
    if ($heldLease) {
        Exit-XpAcceptanceArtifactLease -Lease $heldLease -Status failed
    }
    if ($leftoverProcess -and -not $leftoverProcess.HasExited) {
        Stop-Process -Id $leftoverProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Output 'test handback tests passed'
