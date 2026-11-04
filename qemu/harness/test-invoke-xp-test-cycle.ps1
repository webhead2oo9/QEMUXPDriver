$ErrorActionPreference = 'Stop'

function Assert-TestCycle {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$cycleScript = Join-Path $PSScriptRoot 'invoke-xp-test-cycle.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'
$cycleSource = Get-Content -Raw -LiteralPath $cycleScript
Assert-TestCycle `
    ($cycleSource.Contains('Assert-XpFrozenExecutionInputs') -and
     $cycleSource.IndexOf('Assert-XpFrozenExecutionInputs') -lt
        $cycleSource.IndexOf("Invoke-XpCycleChild -Label 'preflight'")) `
    'cycle does not reject alternate frozen host/core inputs before preflight'
foreach ($visualMode in @(
        'UT2003Visual', 'UT2003Gameplay', 'Warcraft3D3D8Visual')) {
    $modePattern = "'$visualMode'\s*=\s*@\{[\s\S]{1,400}?Visual\s*=\s*\`$true"
    Assert-TestCycle ([regex]::IsMatch($cycleSource, $modePattern)) `
        "$visualMode is not fail-closed through cycle visual evidence"
}

function Invoke-IsolatedCycle {
    param([string[]]$Arguments)

    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $cycleScript `
            -RetroHostPath (Join-Path $testRoot 'retro_host.exe') `
            -CorePath (Join-Path $testRoot 'libqemu_libretro.dll') `
            @Arguments 2>&1)
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    if ($null -eq $exit) { $exit = 1 }
    return [pscustomobject]@{
        ExitCode = $exit
        Output = @($output | ForEach-Object { $_.ToString() }) -join
            [Environment]::NewLine
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-test-cycle-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    # A dry run renders the full fresh-child plan without touching anything:
    # two-stage preflight, overlay creation, every mode through the canonical
    # launcher, handback auditing every run bundle.
    $evidence = Join-Path $testRoot 'dryrun-child'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-dryrun', '-EvidenceDirectory', $evidence,
        '-ChildName', 'cycle-test-child', '-BasePath', 'fake-base.qcow2',
        '-QmpPort', '49999', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*verify-xp-test-preflight.ps1*-PlannedChildName cycle-test-child*') `
        'stage-1 preflight plan is missing the planned child'
    Assert-TestCycle ($commands -like '*-PlannedChildName cycle-test-child*-BasePath fake-base.qcow2*') `
        'stage-1 preflight plan does not audit the child base disk'
    Assert-TestCycle ($commands -like '*new-xpdm-overlay.ps1*-Name cycle-test-child*') `
        'overlay creation plan is missing'
    Assert-TestCycle ($commands -like '*-ExpectChildCreated*') `
        'stage-2 preflight plan is missing'
    Assert-TestCycle ($commands -like '*invoke-xp-acceptance.ps1*') `
        'runs are not routed through the canonical launcher'
    Assert-TestCycle ($commands -like '*FrozenInputsPath*frozen-inputs-ut-oracle-r5.json*') `
        'runs are not bound to the cycle frozen host/core manifest'
    Assert-TestCycle ($commands -like '*InstallOnly = $true*' -and
        $commands -like '*D3dVsSuccession = $true*') `
        'default matrix plan is incomplete'
    Assert-TestCycle ($commands -like "*-Tier 'T2'*") `
        'acceptance runs do not declare their tier'
    Assert-TestCycle ($commands -like '*verify-xp-test-handback.ps1*') `
        'handback plan is missing'
    Assert-TestCycle ($commands -like '*verify-xp-test-handback.ps1*-RunDirectory*Install*') `
        'handback plan does not audit the run bundles'
    Assert-TestCycle ($commands -like '*-PackageDirectory*guest?xpdm?build?package*') `
        'install plan is not bound to the staged package'
    $installIndex = $commands.IndexOf('InstallOnly')
    $successionIndex = $commands.IndexOf('D3dVsSuccession')
    $handbackIndex = $commands.IndexOf('verify-xp-test-handback.ps1')
    Assert-TestCycle ($installIndex -lt $successionIndex -and
        $successionIndex -lt $handbackIndex) `
        'plan steps are out of order'

    # A performance mode renders one canonical run per requested pass at T3,
    # and the handback audits each of the three bundles.
    $evidence = Join-Path $testRoot 'dryrun-perf'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-perf', '-EvidenceDirectory', $evidence,
        '-ContentPath', (Join-Path $testRoot 'fake.qemu_cmd_line'),
        '-Modes', 'MaxPayne', '-PerformanceRuns', '3', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "performance dry run failed: $($run.Output)"
    $commands = Get-Content (Join-Path $evidence 'command.txt')
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like 'frozen-inputs *frozen-inputs-ut-oracle-r5.json' }).Count -eq 1) `
        'non-install cycle did not consume the frozen acceptance ISO exactly once'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*PreparedAcceptanceIsoSha256*' -and
        $_ -like '*PreparedAcceptanceIsoPath*prepared-xpdriver-acceptance.iso*' -and
        $_ -like '*ContentPath*.xpdriver-prepared-dryrun.qemu_cmd_line*'
    }).Count -eq 3) `
        'performance runs are not bound to a cycle-private ISO and content path'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*GuestLaunchDelaySeconds = 40*' }).Count -eq 3) `
        'canonical 40-second guest launch delay was not propagated'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*D3dPerformance = $true*' }).Count -eq 3) `
        'performance mode did not plan three runs'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like "*-Tier 'T3'*" }).Count -eq 3) `
        'performance runs are not declared as T3'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*-RunDirectory*MaxPayne-r1*MaxPayne-r2*MaxPayne-r3*' }).Count -eq 1) `
        'handback does not audit every performance run bundle'

    # Exact joined-UT and default-renderer Warcraft performance modes remain
    # capture-free and repeat independently from their visual gates.
    $evidence = Join-Path $testRoot 'dryrun-exact-title-perf'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-exact-title-perf',
        '-EvidenceDirectory', $evidence,
        '-ContentPath', (Join-Path $testRoot 'fake.qemu_cmd_line'),
        '-Modes',
        'UT2003GameplayPerformance,Warcraft3D3D8Performance',
        '-PerformanceRuns', '2', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "exact title performance dry run failed: $($run.Output)"
    $commands = Get-Content (Join-Path $evidence 'command.txt')
    $utGameplayPerformanceCommands = @($commands | Where-Object {
        $_ -like '*UT2003GameplayPerformance*' -and
        $_ -like "*ObserveGame = 'UT2003Gameplay'*" -and
        $_ -like '*ObservePerformance = $true*' -and
        $_ -notlike '*RecordVideo = $true*'
    })
    Assert-TestCycle ($utGameplayPerformanceCommands.Count -eq 2) `
        'joined UT performance mode did not plan two capture-free runs'
    $warcraftD3D8PerformanceCommands = @($commands | Where-Object {
        $_ -like '*Warcraft3D3D8Performance*' -and
        $_ -like "*ObserveGame = 'Warcraft3D3D8'*" -and
        $_ -like '*ObservePerformance = $true*' -and
        $_ -notlike '*RecordVideo = $true*'
    })
    Assert-TestCycle ($warcraftD3D8PerformanceCommands.Count -eq 2) `
        'Warcraft D3D8 performance mode did not plan two capture-free runs'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*-RunDirectory*UT2003GameplayPerformance-r1*UT2003GameplayPerformance-r2*Warcraft3D3D8Performance-r1*Warcraft3D3D8Performance-r2*'
    }).Count -eq 1) `
        'handback does not audit every exact-title performance bundle'

    # A visual companion is one separate capture-enabled tutorial run after
    # the requested capture-free repetitions; it is never multiplied by
    # PerformanceRuns and every bundle reaches handback.
    $evidence = Join-Path $testRoot 'dryrun-perf-visual'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-perf-visual', '-EvidenceDirectory', $evidence,
        '-ChildName', 'cycle-perf-visual-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49995',
        '-Modes', 'Install,MaxPayne,MaxPayneVisual',
        '-PerformanceRuns', '3', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "performance plus visual dry run failed: $($run.Output)"
    $commands = Get-Content (Join-Path $evidence 'command.txt')
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*D3dPerformance = $true*' }).Count -eq 3) `
        'visual companion contaminated performance-run planning'
    $visualCommands = @($commands | Where-Object {
        $_ -like '*MaxPayneVisual*' -and
        $_ -like '*D3dGameplay = $true*' -and
        $_ -notlike '*D3dPerformance = $true*'
    })
    Assert-TestCycle ($visualCommands.Count -eq 1) `
        'visual companion was not planned exactly once and separately'
    Assert-TestCycle ($visualCommands[0] -like "*-Tier 'T3'*") `
        'visual companion is not declared as T3'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*-RunDirectory*MaxPayne-r1*MaxPayne-r2*MaxPayne-r3*MaxPayneVisual*'
    }).Count -eq 1) `
        'handback does not audit performance and visual companion bundles'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-duplicate-visual',
        '-EvidenceDirectory', (Join-Path $testRoot 'duplicate-visual'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'MaxPayneVisual,MaxPayneVisual', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*only once*') `
        'duplicate visual companion modes were not rejected'

    # UT2003's ordinary cycle mode is capture-free performance evidence. The
    # normal-menu and joined-DM modes retain fail-closed visual evidence from
    # exact-package fresh installs.
    $evidence = Join-Path $testRoot 'dryrun-game-visuals'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-game-visuals', '-EvidenceDirectory', $evidence,
        '-ChildName', 'cycle-game-visuals-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49994',
        '-Modes',
        'Install,MaxPayneVisual,UT2003Visual,UT2003Gameplay', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "game visuals dry run failed: $($run.Output)"
    $commands = Get-Content (Join-Path $evidence 'command.txt')
    $utVisualCommands = @($commands | Where-Object {
        $_ -like '*UT2003Visual*' -and
        $_ -like "*ObserveGame = 'UT2003'*" -and
        $_ -like '*RecordVideo = $true*' -and
        $_ -notlike '*ObservePerformance = $true*'
    })
    Assert-TestCycle ($utVisualCommands.Count -eq 1) `
        'UT2003 normal-menu visual companion was not planned once'
    Assert-TestCycle ($utVisualCommands[0] -like "*-Tier 'T3'*" -and
        $utVisualCommands[0] -like '*TimeoutSeconds = 900*') `
        'UT2003 visual companion lacks its bounded T3 timeout'
    $utGameplayCommands = @($commands | Where-Object {
        $_ -like '*UT2003Gameplay*' -and
        $_ -like "*ObserveGame = 'UT2003Gameplay'*" -and
        $_ -like '*RecordVideo = $true*' -and
        $_ -notlike '*ObservePerformance = $true*'
    })
    Assert-TestCycle ($utGameplayCommands.Count -eq 1) `
        'UT2003 joined-gameplay visual mode was not planned once'
    Assert-TestCycle ($utGameplayCommands[0] -like "*-Tier 'T3'*" -and
        $utGameplayCommands[0] -like '*TimeoutSeconds = 300*') `
        'UT2003 gameplay mode lacks its bounded T3 timeout'
    Assert-TestCycle (@($commands | Where-Object {
        $_ -like '*-RunDirectory*MaxPayneVisual*UT2003Visual*UT2003Gameplay*'
    }).Count -eq 1) `
        'handback does not audit all capture-enabled game bundles'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-duplicate-ut-visual',
        '-EvidenceDirectory', (Join-Path $testRoot 'duplicate-ut-visual'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'UT2003Visual,UT2003Visual', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*only once*') `
        'duplicate UT2003 visual companion modes were not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-duplicate-ut-gameplay',
        '-EvidenceDirectory', (Join-Path $testRoot 'duplicate-ut-gameplay'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'UT2003Gameplay,UT2003Gameplay', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*only once*') `
        'duplicate UT2003 gameplay modes were not rejected'

    foreach ($visualMode in @(
            'MaxPayneVisual', 'UT2003Visual', 'UT2003Gameplay')) {
        $run = Invoke-IsolatedCycle -Arguments @(
            '-TaskId', "cycle-stale-$visualMode",
            '-EvidenceDirectory', (Join-Path $testRoot "stale-$visualMode"),
            '-ContentPath', 'existing-target.qemu_cmd_line',
            '-Modes', $visualMode, '-DryRun')
        Assert-TestCycle ($run.ExitCode -ne 0 -and
            $run.Output -like '*requires Install on a fresh -ChildName*') `
            "$visualMode accepted an existing target without exact-package install"
    }

    # Install writes the target disk and must never land on an existing
    # content profile - including a registered preserved target.
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-install-existing',
        '-EvidenceDirectory', (Join-Path $testRoot 'install-existing'),
        '-ContentPath', 'existing-target.qemu_cmd_line',
        '-Modes', 'Install', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*requires -ChildName*') `
        'Install against an existing content profile was not rejected'

    # A separately frozen diagnostic generation may select a non-staging
    # package. The cycle must forward that exact directory to both the runner
    # and handback rather than silently reporting the staging package.
    $evidence = Join-Path $testRoot 'install-package'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-install-package',
        '-EvidenceDirectory', $evidence,
        '-ChildName', 'pkg-child', '-BasePath', 'fake-base.qcow2',
        '-QmpPort', '49997', '-Modes', 'Install',
        '-PackageDirectory', $testRoot, '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "diagnostic package dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle (
        @([regex]::Matches(
            $commands, [regex]::Escape($testRoot))).Count -ge 2) `
        'diagnostic package was not bound to runner and handback'

    # Zero performance runs would let a performance mode pass with no
    # measurements at all.
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-zero-perf',
        '-EvidenceDirectory', (Join-Path $testRoot 'zero-perf'),
        '-ContentPath', 'fake.qemu_cmd_line', '-Modes', 'MaxPayne',
        '-PerformanceRuns', '0', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*PerformanceRuns*') `
        'zero performance runs were not rejected'

    # A comma-joined -Modes list arrives as one literal string through
    # powershell.exe -File and must still plan every listed mode.
    $evidence = Join-Path $testRoot 'comma-modes'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-comma', '-EvidenceDirectory', $evidence,
        '-ContentPath', (Join-Path $testRoot 'fake.qemu_cmd_line'),
        '-Modes', 'Boot,D3d9', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "comma-joined modes dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*BootOnly = $true*' -and
        $commands -like '*D3d9 = $true*') `
        'comma-joined modes did not plan every listed mode'
    Assert-TestCycle (@($commands -split [Environment]::NewLine |
        Where-Object {
            $_ -like 'frozen-inputs *frozen-inputs-ut-oracle-r5.json'
        }).Count -eq 1) `
        'multi-mode cycle did not consume one frozen acceptance ISO'

    # Stream seven is the integrated diagnostic DDI-v4/SM2 profile. It is
    # valid only after this same cycle installs its separately frozen package
    # on a fresh child, and must never satisfy the ordinary fallback oracle.
    $evidence = Join-Path $testRoot 'multistream7-mode'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-multistream7', '-EvidenceDirectory', $evidence,
        '-ChildName', 'multistream7-child', '-BasePath', 'fake-base.qcow2',
        '-QmpPort', '49996', '-Modes', 'Install,D3d9Multistream7',
        '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "stream-seven dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*InstallOnly = $true*' -and
        $commands -like '*D3d9 = $true*' -and
        $commands -like '*D3d9Multistream7 = $true*' -and
        $commands -like "*Tier 'T2-diagnostic'*" -and
        $commands -like '*-Diagnostic*') `
        'stream-seven mode did not retain install-bound diagnostic parameters'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-multistream7-no-install',
        '-EvidenceDirectory', (Join-Path $testRoot 'multistream7-no-install'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'D3d9Multistream7', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*requires Install*same cycle*') `
        'stream-seven mode without a same-cycle install was not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-multistream7-order',
        '-EvidenceDirectory', (Join-Path $testRoot 'multistream7-order'),
        '-ChildName', 'multistream7-order-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49995',
        '-Modes', 'D3d9Multistream7,Install', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*Install must precede*') `
        'stream-seven mode before same-cycle install was not rejected'

    # The no-skipstartup Max Payne launch is attributable only when the staged
    # package is installed first on the same fresh child.  It stays separate
    # from tutorial automation and performance modes.
    $evidence = Join-Path $testRoot 'maxpayne-normal-launch'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-maxpayne-normal', '-EvidenceDirectory', $evidence,
        '-ChildName', 'maxpayne-normal-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49994',
        '-Modes', 'Install,MaxPayneNormalLaunch', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "normal-launch dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*InstallOnly = $true*' -and
        $commands -like '*D3dGame = $true*' -and
        $commands -like '*D3dNormalLaunch = $true*' -and
        $commands -like "*Tier 'T3'*") `
        'normal-launch mode did not retain install-bound T3 parameters'
    Assert-TestCycle ($commands.IndexOf('InstallOnly') -lt
        $commands.IndexOf('D3dNormalLaunch')) `
        'normal-launch mode was planned before installation'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-maxpayne-normal-no-install',
        '-EvidenceDirectory', (Join-Path $testRoot 'normal-no-install'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'MaxPayneNormalLaunch', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*requires Install*same cycle*') `
        'normal-launch mode without a same-cycle install was not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-maxpayne-normal-order',
        '-EvidenceDirectory', (Join-Path $testRoot 'normal-order'),
        '-ChildName', 'maxpayne-normal-order-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49993',
        '-Modes', 'MaxPayneNormalLaunch,Install', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*Install must precede*') `
        'normal-launch mode before same-cycle install was not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-maxpayne-normal-duplicate',
        '-EvidenceDirectory', (Join-Path $testRoot 'normal-duplicate'),
        '-ChildName', 'maxpayne-normal-duplicate-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49992',
        '-Modes', 'Install,MaxPayneNormalLaunch,MaxPayneNormalLaunch',
        '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*only once*') `
        'duplicate normal-launch modes were not rejected'

    # The Warcraft default-D3D8 cadence gate is temporal T3 evidence, not the
    # existing forced-OpenGL observer.  Attribute it to one fresh installation
    # and retain a review video alongside the zero-isolated-black gate.
    $evidence = Join-Path $testRoot 'warcraft3-d3d8-visual'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-warcraft3-d3d8',
        '-EvidenceDirectory', $evidence,
        '-ChildName', 'warcraft3-d3d8-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49991',
        '-Modes', 'Install,Warcraft3D3D8Visual', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "Warcraft D3D8 dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*InstallOnly = $true*' -and
        $commands -like "*ObserveGame = 'Warcraft3D3D8'*" -and
        $commands -like '*RecordVideo = $true*' -and
        $commands -like "*Tier 'T3'*") `
        'Warcraft D3D8 mode did not retain install-bound temporal parameters'
    Assert-TestCycle ($commands.IndexOf('InstallOnly') -lt
        $commands.IndexOf("ObserveGame = 'Warcraft3D3D8'")) `
        'Warcraft D3D8 oracle was planned before installation'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-warcraft3-d3d8-no-install',
        '-EvidenceDirectory', (Join-Path $testRoot 'warcraft-no-install'),
        '-ContentPath', 'fake.qemu_cmd_line',
        '-Modes', 'Warcraft3D3D8Visual', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*requires Install*same cycle*') `
        'Warcraft D3D8 mode without a same-cycle install was not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-warcraft3-d3d8-order',
        '-EvidenceDirectory', (Join-Path $testRoot 'warcraft-order'),
        '-ChildName', 'warcraft3-d3d8-order-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49990',
        '-Modes', 'Warcraft3D3D8Visual,Install', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*Install must precede*') `
        'Warcraft D3D8 mode before same-cycle install was not rejected'

    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-warcraft3-d3d8-duplicate',
        '-EvidenceDirectory', (Join-Path $testRoot 'warcraft-duplicate'),
        '-ChildName', 'warcraft3-d3d8-duplicate-child',
        '-BasePath', 'fake-base.qcow2', '-QmpPort', '49989',
        '-Modes',
        'Install,Warcraft3D3D8Visual,Warcraft3D3D8Visual', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*only once*') `
        'duplicate Warcraft D3D8 modes were not rejected'

    # A caller can tune the readiness delay without weakening retry logic.
    $evidence = Join-Path $testRoot 'custom-launch-delay'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-delay', '-EvidenceDirectory', $evidence,
        '-ContentPath', (Join-Path $testRoot 'fake.qemu_cmd_line'),
        '-Modes', 'Boot', '-GuestLaunchDelaySeconds', '50', '-DryRun')
    Assert-TestCycle ($run.ExitCode -eq 0) `
        "custom launch delay dry run failed: $($run.Output)"
    $commands = Get-Content -Raw (Join-Path $evidence 'command.txt')
    Assert-TestCycle ($commands -like '*GuestLaunchDelaySeconds = 50*') `
        'custom guest launch delay was not propagated'

    # An unknown mode is rejected before anything runs.
    $evidence = Join-Path $testRoot 'unknown-mode'
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-unknown', '-EvidenceDirectory', $evidence,
        '-ContentPath', 'fake.qemu_cmd_line', '-Modes', 'NotAMode', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*Unknown mode*') `
        'unknown mode was not rejected'

    # A non-empty evidence directory is never reused.
    $evidence = Join-Path $testRoot 'taken'
    [void](New-Item -ItemType Directory -Path $evidence)
    [IO.File]::WriteAllText((Join-Path $evidence 'existing.txt'), 'evidence')
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-taken', '-EvidenceDirectory', $evidence,
        '-ContentPath', 'fake.qemu_cmd_line', '-Modes', 'Boot', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*Refusing to reuse*') `
        'non-empty evidence directory was not refused'

    # -ChildName and -ContentPath are mutually exclusive by contract.
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-conflict',
        '-EvidenceDirectory', (Join-Path $testRoot 'conflict'),
        '-ChildName', 'child', '-BasePath', 'base.qcow2', '-QmpPort', '49998',
        '-ContentPath', 'other.qemu_cmd_line', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*do not pass -ContentPath*') `
        'conflicting child/content arguments were not rejected'

    # -ChildName without its required companions is rejected.
    $run = Invoke-IsolatedCycle -Arguments @(
        '-TaskId', 'cycle-partial',
        '-EvidenceDirectory', (Join-Path $testRoot 'partial'),
        '-ChildName', 'child', '-DryRun')
    Assert-TestCycle ($run.ExitCode -ne 0 -and
        $run.Output -like '*requires -BasePath and -QmpPort*') `
        'partial child arguments were not rejected'

    Write-Output 'XP test cycle orchestrator tests passed'
} finally {
    Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
}
