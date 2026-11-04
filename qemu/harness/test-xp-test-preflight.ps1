$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')

function Assert-TestPreflight {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$preflightScript = Join-Path $PSScriptRoot 'verify-xp-test-preflight.ps1'

function Invoke-IsolatedPreflight {
    param([string[]]$Arguments)

    $powershell = Join-Path $PSHOME 'powershell.exe'
    $savedErrorActionPreference = $ErrorActionPreference
    $rawOutput = @()
    $exitCode = $null
    try {
        $ErrorActionPreference = 'Continue'
        $rawOutput = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass -File $preflightScript @Arguments 2>&1)
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

function ConvertTo-TestWslPath {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "Expected a drive-letter Windows path, got '$Path'"
    }
    return "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2] -replace '\\', '/')"
}

function New-FakeWorkspace {
    param(
        [string]$Root,
        [switch]$NoPycdlib,
        [switch]$NoGit,
        [switch]$NoRegistry,
        [switch]$BrokenRegistry,
        [switch]$EmptyRegistry
    )

    $vmDir = Join-Path $Root 'qemu\winxp'
    $harnessDir = Join-Path $Root 'qemu\harness'
    [void](New-Item -ItemType Directory -Path $vmDir -Force)
    [void](New-Item -ItemType Directory -Path $harnessDir -Force)
    [IO.File]::WriteAllBytes((Join-Path $vmDir 'my_pc_disk.qcow2'),
        [byte[]](1..16))
    if ($BrokenRegistry) {
        [IO.File]::WriteAllText((Join-Path $harnessDir 'test-targets.json'),
            '{ not json')
    } elseif ($EmptyRegistry) {
        # Parses cleanly with the right schemaVersion but provides no
        # protected set; the canonical validator must reject it.
        [IO.File]::WriteAllText((Join-Path $harnessDir 'test-targets.json'),
            '{"schemaVersion":1,"targets":null}')
    } elseif (-not $NoRegistry) {
        # The fixture registry must satisfy the canonical validator that the
        # preflight now invokes, so every required target property is present.
        $registry = '{"schemaVersion":1,' +
            '"current":{"fullCheckpointTarget":"preserved-one",' +
            '"manualGamesTarget":"preserved-one",' +
            '"gamesBackingTarget":"preserved-one"},' +
            '"targets":[{"id":"preserved-one",' +
            '"purpose":"preflight fixture preserved checkpoint",' +
            '"lifecycle":"preserved-checkpoint",' +
            '"mutationPolicy":"child-only",' +
            '"contentPath":"qemu/winxp/preserved-target.qemu_cmd_line",' +
            '"diskPath":"qemu/winxp/preserved-target.qcow2",' +
            '"backingDiskPath":"qemu/winxp/backing-base.qcow2",' +
            '"installedDriver":"1.0.0.1","network":"offline",' +
            '"qmpPort":49551,"launcher":"retro_host",' +
            '"allowedTiers":["T2"],"lastVerified":"2026-07-17",' +
            '"notes":"preflight fixture"}]}'
        [IO.File]::WriteAllText((Join-Path $harnessDir 'test-targets.json'),
            $registry)
    }
    if (-not $NoPycdlib) {
        [void](New-Item -ItemType Directory -Path `
            (Join-Path $Root '.tools\pycdlib\pycdlib') -Force)
    }
    $ffmpegDir = Join-Path $Root `
        '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin'
    [void](New-Item -ItemType Directory -Path $ffmpegDir -Force)
    [IO.File]::WriteAllBytes((Join-Path $ffmpegDir 'ffmpeg.exe'),
        [byte[]](1..4))
    if (-not $NoGit) {
        & git -C $Root init -q
        & git -C $Root config user.email 'test@example.invalid'
        & git -C $Root config user.name 'Harness Test'
        & git -C $Root add -A
        & git -C $Root -c commit.gpgsign=false commit -q -m 'fake workspace'
        Assert-TestPreflight ($LASTEXITCODE -eq 0) `
            "git setup failed for $Root"
    }
}

function Write-FakeProfile {
    param(
        [string]$Path,
        [string]$Hda,
        [int]$Port,
        [switch]$Snapshot,
        [switch]$NoNic,
        [switch]$NoVram,
        [string]$Vga = 'std',
        [string]$VramMb = '128',
        [string]$ExtraTokens
    )

    $line = "qemu-system-x86_64 -m 1024M -vga $Vga "
    if (-not $NoVram) {
        $line += "-global VGA.vgamem_mb=$VramMb "
    }
    if ($Snapshot) { $line += '-snapshot ' }
    $line += "-hda $Hda "
    if (-not $NoNic) { $line += '-nic none ' }
    if ($ExtraTokens) { $line += "$ExtraTokens " }
    $line += "-qmp tcp:127.0.0.1:$Port,server,nowait -boot c"
    [IO.File]::WriteAllText($Path, $line + "`n",
        [Text.UTF8Encoding]::new($false))
}

function Get-PreflightCheck {
    param([object]$Record, [string]$Name)
    return @($Record.checks | Where-Object { $_.name -eq $Name })[0]
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-test-preflight-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
$heldLease = $null
try {
    # A complete workspace with a created and bound child/profile pair passes
    # every check as the post-creation stage.
    $caseRoot = Join-Path $testRoot 'case-pass'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace `
        'qemu\winxp\xpdriver-child-unique.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'child-unique.qcow2' -Port $port
    [IO.File]::WriteAllBytes((Join-Path $workspace `
        'qemu\winxp\child-unique.qcow2'), [byte[]](1..8))
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-pass', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath, '-QmpPort', "$port",
        '-PlannedChildName', 'child-unique', '-ExpectChildCreated',
        '-RequireVideo')
    Assert-TestPreflight ($run.ExitCode -eq 0) `
        "pass case failed: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    Assert-TestPreflight ($record.kind -eq 'xpdriver-test-preflight' -and
        $record.schema -eq 1 -and $record.result -eq 'pass' -and
        $record.taskId -eq 'preflight-pass' -and
        $record.observationStarted) `
        'pass case record identity is wrong'
    Assert-TestPreflight (@($record.checks | Where-Object {
        -not $_.ok }).Count -eq 0) 'pass case recorded a failed check'
    Assert-TestPreflight ($record.environment.contentProfile.hdaLeaf -eq `
        'child-unique.qcow2' -and
        $record.environment.contentProfile.vgaVramMb -eq 128 -and
        -not $record.environment.contentProfile.disposable) `
        'pass case content profile parse is wrong'
    Assert-TestPreflight ($record.environment.protectedRoots -contains `
        'my_pc_disk.qcow2' -and $record.environment.protectedRoots -contains `
        'backing-base.qcow2') 'reproducibility roots were not protected'
    Assert-TestPreflight (@($record.environment.registeredTargets)[0].diskLeaf `
        -eq 'preserved-target.qcow2') `
        'registered target profile was not recorded'
    Assert-TestPreflight `
        ($null -ne $record.environment.repositories[0].statusContentSha256) `
        'repository content identity was not recorded'

    # The non-XPDM baseline intentionally uses Cirrus VGA. The standard-VGA
    # aperture requirement must not reject that separate device model.
    $caseRoot = Join-Path $testRoot 'case-cirrus-vram-exempt'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace 'qemu\winxp\cirrus.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'child.qcow2' -Port $port `
        -Vga 'cirrus' -NoVram
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-cirrus', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace,
        '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath)
    Assert-TestPreflight ($run.ExitCode -eq 0) `
        "Cirrus profile was rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'contentProfileVram'
    Assert-TestPreflight ($check.ok -and
        $null -eq $record.environment.contentProfile.vgaVramMb -and
        @($record.environment.contentProfile.vga)[0] -eq 'cirrus') `
        'Cirrus VRAM exemption was not recorded'

    # A planned child and an unrelated profile are a packet defect: the
    # profile must be the generated xpdriver-<name> file booting <name>.qcow2.
    $caseRoot = Join-Path $testRoot 'case-binding'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace 'qemu\winxp\unrelated.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'some-other.qcow2' -Port $port
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-binding', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath, '-PlannedChildName', 'child-unique')
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "decoy child/profile pair was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'plannedChildBinding'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'HANDOFF_INCOMPLETE') `
        'child/profile binding check is wrong'

    # Post-creation, the generated child/profile pair must exist and bind.
    $caseRoot = Join-Path $testRoot 'case-created'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace `
        'qemu\winxp\xpdriver-child-new.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'child-new.qcow2' -Port $port
    [IO.File]::WriteAllBytes((Join-Path $workspace 'qemu\winxp\child-new.qcow2'),
        [byte[]](1..8))
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-created', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath, '-PlannedChildName', 'child-new',
        '-ExpectChildCreated')
    Assert-TestPreflight ($run.ExitCode -eq 0) `
        "post-creation case failed: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    Assert-TestPreflight ($record.environment.plannedChild.exists -and
        $record.environment.plannedChild.stage -eq 'post-creation') `
        'post-creation stage was not recorded'

    # Block devices outside the audited -hda fail closed; the cdrom -drive
    # that every registered profile carries stays legal.
    foreach ($variant in @(
            @{ Tokens = '-hdb second.qcow2'; Expect = 1; Label = 'hdb' },
            @{ Tokens = '-fda floppy.img'; Expect = 1; Label = 'fda' },
            @{ Tokens = '-pflash firmware.bin'; Expect = 1; Label = 'pflash' },
            @{ Tokens = '-virtfs local,path=/tmp,mount_tag=host'; Expect = 1;
               Label = 'virtfs' },
            @{ Tokens = '-hda plain-child.qcow2'; Expect = 1;
               Label = 'repeated-hda' },
            @{ Tokens = '-drive file=x.qcow2,format=qcow2'; Expect = 1;
               Label = 'drive-disk' },
            @{ Tokens = '-drive file=a.iso,media=cdrom,index=2'; Expect = 0;
               Label = 'drive-cdrom' },
            @{ Tokens = '-drive file=media=cdrom.qcow2,format=qcow2';
               Expect = 1; Label = 'drive-decoy-filename' },
            @{ Tokens = '-name "quoted -hdb -blockdev decoy"'; Expect = 0;
               Label = 'quoted-decoy' })) {
        $caseRoot = Join-Path $testRoot "case-extradisk-$($variant.Label)"
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        $port = Get-FreeTcpPort
        $profilePath = Join-Path $workspace 'qemu\winxp\extradisk.qemu_cmd_line'
        Write-FakeProfile -Path $profilePath -Hda 'plain-child.qcow2' `
            -Port $port -ExtraTokens $variant.Tokens
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-extradisk', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath)
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'contentProfileExtraDisks'
        if ($variant.Expect -eq 0) {
            Assert-TestPreflight ($check.ok) `
                "$($variant.Label) was wrongly rejected: $($run.Output)"
        } else {
            Assert-TestPreflight ($run.ExitCode -eq 1 -and -not $check.ok -and
                $check.failureClass -eq 'DISK_POLICY_VIOLATION') `
                "$($variant.Label) attachment was not rejected: $($run.Output)"
        }
    }

    # Pre-creation, the planned name must be completely free: the overlay
    # helper refuses when either the overlay or the content file exists, so
    # a half-taken name must fail before the helper is ever invoked.
    foreach ($variant in @(
            @{ Label = 'free'; Overlay = $false; Content = $false;
               ExpectPass = $true },
            @{ Label = 'content-taken'; Overlay = $false; Content = $true;
               ExpectPass = $false },
            @{ Label = 'overlay-taken'; Overlay = $true; Content = $false;
               ExpectPass = $false })) {
        $caseRoot = Join-Path $testRoot "case-precreation-$($variant.Label)"
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        if ($variant.Overlay) {
            [IO.File]::WriteAllBytes((Join-Path $workspace `
                'qemu\winxp\child-planned.qcow2'), [byte[]](1..8))
        }
        if ($variant.Content) {
            Write-FakeProfile -Path (Join-Path $workspace `
                'qemu\winxp\xpdriver-child-planned.qemu_cmd_line') `
                -Hda 'child-planned.qcow2' -Port (Get-FreeTcpPort)
        }
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-precreation', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-PlannedChildName', 'child-planned')
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'plannedChild'
        if ($variant.ExpectPass) {
            Assert-TestPreflight ($run.ExitCode -eq 0 -and $check.ok -and
                $record.environment.plannedChild.stage -eq 'pre-creation') `
                "free planned name was wrongly rejected: $($run.Output)"
        } else {
            Assert-TestPreflight ($run.ExitCode -eq 1 -and -not $check.ok -and
                $check.failureClass -eq 'INFRASTRUCTURE_FAILURE') `
                "$($variant.Label) name was not rejected: $($run.Output)"
        }
    }

    # A held artifact lease is an ownership conflict quoting the owner.
    $caseRoot = Join-Path $testRoot 'case-lease'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $lockPath = Join-Path $workspace `
        'qemu\winxp\.xpdriver-acceptance-artifacts.lock'
    $heldLease = Enter-XpAcceptanceArtifactLease -LockPath $lockPath `
        -RunId 'preflight-test-holder' -QmpPort 49100 `
        -RequestedTimeoutSeconds 150 -EffectiveTimeoutSeconds 180
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-lease', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "held lease was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'artifactLease'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'VM_OWNERSHIP_CONFLICT') `
        'held lease check is wrong'
    Assert-TestPreflight ($record.environment.lease.owner -like `
        '*preflight-test-holder*') 'held lease owner was not recorded'
    Exit-XpAcceptanceArtifactLease -Lease $heldLease -Status passed
    $heldLease = $null

    # A bound QMP port is an ownership conflict.
    $caseRoot = Join-Path $testRoot 'case-port'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $port)
    $listener.Start()
    try {
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-port', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-QmpPort', "$port")
        Assert-TestPreflight ($run.ExitCode -eq 1) `
            "bound port was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'qmpPortFree'
        Assert-TestPreflight (-not $check.ok -and
            $check.failureClass -eq 'VM_OWNERSHIP_CONFLICT') `
            'bound port check is wrong'
    } finally {
        $listener.Stop()
    }

    # Booting a reproducibility root without -snapshot violates the disk
    # policy; a non-registered profile may never boot a preserved disk;
    # -snapshot makes a root boot disposable and legal.
    foreach ($variant in @(
            @{ Hda = '../winxp/my_pc_disk.qcow2'; Snapshot = $false;
               Expect = 'DISK_POLICY_VIOLATION'; Label = 'base' },
            @{ Hda = 'preserved-target.qcow2'; Snapshot = $false;
               Expect = 'DISK_POLICY_VIOLATION'; Label = 'non-registered disk' },
            @{ Hda = 'backing-base.qcow2'; Snapshot = $false;
               Expect = 'DISK_POLICY_VIOLATION'; Label = 'registry backing' },
            @{ Hda = 'preserved-target.qcow2'; Snapshot = $false;
               Extra = '-name "decoy -snapshot payload"';
               Expect = 'DISK_POLICY_VIOLATION';
               Label = 'quoted snapshot decoy' },
            @{ Hda = '../winxp/my_pc_disk.qcow2'; Snapshot = $true;
               Expect = $null; Label = 'base with snapshot' })) {
        $caseRoot = Join-Path $testRoot ('case-disk-' +
            $variant.Label.Replace(' ', '-'))
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        $port = Get-FreeTcpPort
        $profilePath = Join-Path $workspace 'qemu\winxp\run.qemu_cmd_line'
        $profileArgs = @{ Path = $profilePath; Hda = $variant.Hda;
            Port = $port }
        if ($variant.Snapshot) { $profileArgs.Snapshot = $true }
        if ($variant.Extra) { $profileArgs.ExtraTokens = $variant.Extra }
        Write-FakeProfile @profileArgs
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-disk', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath)
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'contentProfileDisk'
        if ($variant.Expect) {
            Assert-TestPreflight ($run.ExitCode -eq 1 -and -not $check.ok `
                -and $check.failureClass -eq $variant.Expect) `
                "$($variant.Label) disk policy was not enforced: $($run.Output)"
        } else {
            Assert-TestPreflight ($check.ok) `
                "$($variant.Label) was wrongly rejected: $($run.Output)"
        }
    }

    # A registered child-only target's own profile is not an exemption from
    # its mutation policy: a writable boot fails the disk policy, and a
    # -snapshot boot of the same registered profile passes.
    $caseRoot = Join-Path $testRoot 'case-registered'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace `
        'qemu\winxp\preserved-target.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'preserved-target.qcow2' `
        -Port $port
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-registered', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath)
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "registered child-only profile booted writable: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'contentProfileDisk'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'DISK_POLICY_VIOLATION' -and
        $record.environment.contentProfile.hdaRegisteredTarget -eq `
        'preserved-one') 'registered writable-boot check is wrong'

    $caseRoot = Join-Path $testRoot 'case-registered-snapshot'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $profilePath = Join-Path $workspace `
        'qemu\winxp\preserved-target.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'preserved-target.qcow2' `
        -Port $port -Snapshot
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-registered-snapshot', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath)
    Assert-TestPreflight ($run.ExitCode -eq 0) `
        "registered -snapshot profile was wrongly rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'contentProfileDisk'
    Assert-TestPreflight ($check.ok -and
        $record.environment.contentProfile.hdaRegisteredTarget -eq `
        'preserved-one') 'registered snapshot ownership is wrong'

    $caseRoot = Join-Path $testRoot 'case-profile-copy'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $port = Get-FreeTcpPort
    $copyPath = Join-Path $caseRoot 'copied-profile.qemu_cmd_line'
    Write-FakeProfile -Path $copyPath -Hda `
        'qemu/winxp/preserved-target.qcow2' -Port $port
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-profile-copy', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $copyPath)
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "copied preserved-disk profile was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'contentProfileDisk'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'DISK_POLICY_VIOLATION') `
        'copied profile check is wrong'

    # Offline means exactly -nic none: a second -nic or a legacy -net option
    # enables networking and must fail.
    foreach ($variant in @(
            @{ Extra = '-nic user'; Label = 'mixed nic' },
            @{ Extra = $null; NoNic = $true; Label = 'missing nic' },
            @{ Extra = $null; NoNic = $true; Legacy = '-net nic';
               Label = 'legacy net' })) {
        $caseRoot = Join-Path $testRoot ('case-network-' +
            $variant.Label.Replace(' ', '-'))
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        $port = Get-FreeTcpPort
        $profilePath = Join-Path $workspace 'qemu\winxp\run.qemu_cmd_line'
        $extra = $variant.Extra
        if ($variant.Legacy) { $extra = $variant.Legacy }
        $profileArgs = @{ Path = $profilePath; Hda = 'child.qcow2';
            Port = $port }
        if ($variant.NoNic) { $profileArgs.NoNic = $true }
        if ($extra) { $profileArgs.ExtraTokens = $extra }
        Write-FakeProfile @profileArgs
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-network', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath)
        Assert-TestPreflight ($run.ExitCode -eq 1) `
            "$($variant.Label) profile was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'contentProfileNetwork'
        Assert-TestPreflight (-not $check.ok -and
            $check.failureClass -eq 'NETWORK_POLICY_VIOLATION') `
            "$($variant.Label) network check is wrong"
    }

    # The driver advertises 128 MB of VRAM. The VM profile must back that
    # contract exactly once; missing, malformed, smaller, duplicate, and
    # quoted-decoy declarations all fail closed before the VM starts.
    foreach ($variant in @(
            @{ NoVram = $true; Label = 'missing' },
            @{ VramMb = '64'; Label = 'smaller' },
            @{ VramMb = 'garbage'; Label = 'malformed' },
            @{ Extra = '-global VGA.vgamem_mb=128'; Label = 'duplicate' },
            @{ NoVram = $true;
               Extra = '-name "decoy -global VGA.vgamem_mb=128"';
               Label = 'quoted decoy' })) {
        $caseRoot = Join-Path $testRoot "case-vram-$($variant.Label)"
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        $port = Get-FreeTcpPort
        $profilePath = Join-Path $workspace 'qemu\winxp\run.qemu_cmd_line'
        $profileArgs = @{
            Path = $profilePath
            Hda = 'child.qcow2'
            Port = $port
        }
        if ($variant.NoVram) { $profileArgs.NoVram = $true }
        if ($variant.VramMb) { $profileArgs.VramMb = $variant.VramMb }
        if ($variant.Extra) { $profileArgs.ExtraTokens = $variant.Extra }
        Write-FakeProfile @profileArgs
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-vram', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath)
        Assert-TestPreflight ($run.ExitCode -eq 1) `
            "$($variant.Label) VRAM profile was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'contentProfileVram'
        Assert-TestPreflight (-not $check.ok -and
            $check.failureClass -eq 'HANDOFF_INCOMPLETE') `
            "$($variant.Label) VRAM check is wrong"
    }

    # The disk policy cannot fail open: a missing or malformed registry stops
    # the task instead of silently protecting only the base.
    foreach ($variant in @(
            @{ Switch = 'NoRegistry'; Label = 'missing' },
            @{ Switch = 'BrokenRegistry'; Label = 'malformed' },
            @{ Switch = 'EmptyRegistry'; Label = 'semantically-empty' })) {
        $caseRoot = Join-Path $testRoot "case-registry-$($variant.Label)"
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        $workspaceArgs = @{ Root = $workspace }
        if ($variant.Switch -eq 'NoRegistry') { $workspaceArgs.NoRegistry = $true }
        if ($variant.Switch -eq 'BrokenRegistry') {
            $workspaceArgs.BrokenRegistry = $true
        }
        if ($variant.Switch -eq 'EmptyRegistry') {
            $workspaceArgs.EmptyRegistry = $true
        }
        New-FakeWorkspace @workspaceArgs
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-registry', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
        Assert-TestPreflight ($run.ExitCode -eq 1) `
            "$($variant.Label) registry did not fail closed: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'targetRegistry'
        Assert-TestPreflight (-not $check.ok -and
            $check.failureClass -eq 'INFRASTRUCTURE_FAILURE') `
            "$($variant.Label) registry check is wrong"
    }

    # An explicit -QmpPort that disagrees with the profile is a packet defect.
    $caseRoot = Join-Path $testRoot 'case-port-disagree'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $profilePort = Get-FreeTcpPort
    $otherPort = Get-FreeTcpPort
    $profilePath = Join-Path $workspace 'qemu\winxp\run.qemu_cmd_line'
    Write-FakeProfile -Path $profilePath -Hda 'child.qcow2' -Port $profilePort
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-disagree', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-ContentPath', $profilePath, '-QmpPort', "$otherPort")
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "port disagreement was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'qmpPortAgreement'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'HANDOFF_INCOMPLETE') `
        'port disagreement check is wrong'

    # A handoff that requires the paired host must fail when it is absent.
    $caseRoot = Join-Path $testRoot 'case-host-required'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-host', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-RequireHostRepo')
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "absent required host was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'repository:qemu-libretro'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'HANDOFF_INCOMPLETE') `
        'required host check is wrong'

    # A WSL host path must reach wsl git verbatim, not through Windows path
    # normalization. Guarded on a working WSL.
    $wslAvailable = $false
    & wsl.exe -e true 2>$null
    if ($LASTEXITCODE -eq 0) { $wslAvailable = $true }
    if ($wslAvailable) {
        $wslRepo = "/tmp/xpdriver-preflight-wsl-$PID"
        $wslSetup = "rm -rf '$wslRepo' && mkdir -p '$wslRepo' && " +
            "git -C '$wslRepo' init -q && " +
            "git -C '$wslRepo' config user.email 'test@example.invalid' && " +
            "git -C '$wslRepo' config user.name 'Harness Test' && " +
            "echo seed > '$wslRepo/seed.txt' && " +
            "git -C '$wslRepo' add -A && " +
            "git -C '$wslRepo' -c commit.gpgsign=false commit -q -m seed"
        & wsl.exe bash -c $wslSetup
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'wsl repo setup failed'
        try {
            $caseRoot = Join-Path $testRoot 'case-wsl-host'
            $workspace = Join-Path $caseRoot 'workspace'
            [void](New-Item -ItemType Directory -Path $workspace -Force)
            New-FakeWorkspace -Root $workspace
            $outputPath = Join-Path $caseRoot 'start-state.json'
            $run = Invoke-IsolatedPreflight -Arguments @(
                '-TaskId', 'preflight-wsl', '-OutputPath', $outputPath,
                '-WorkspaceRoot', $workspace, '-HostRepoPath', $wslRepo,
                '-HostRepoKind', 'wsl', '-RequireHostRepo')
            Assert-TestPreflight ($run.ExitCode -eq 0) `
                "wsl host case failed: $($run.Output)"
            $record = Get-Content -Raw -LiteralPath $outputPath |
                ConvertFrom-Json
            $hostRepo = @($record.environment.repositories |
                Where-Object { $_.name -eq 'qemu-libretro' })[0]
            Assert-TestPreflight ($hostRepo.branch -and $hostRepo.commit -and
                $hostRepo.statusContentSha256) `
                'wsl host identity was not recorded'
        } finally {
            & wsl.exe rm -rf $wslRepo
        }
    } else {
        Write-Output 'SKIP: wsl host case (wsl.exe unavailable)'
    }

    # Post-creation, the overlay's stored qcow2 backing reference must
    # resolve to the assigned base; a rebased overlay pointing at another
    # disk fails even though every filename convention still matches.
    if ($wslAvailable) {
        $caseRoot = Join-Path $testRoot 'case-backing'
        $workspace = Join-Path $caseRoot 'workspace'
        [void](New-Item -ItemType Directory -Path $workspace -Force)
        New-FakeWorkspace -Root $workspace
        $vmDirFixture = Join-Path $workspace 'qemu\winxp'
        $approvedBase = Join-Path $vmDirFixture 'approved-base.qcow2'
        & wsl.exe qemu-img create -f qcow2 (
            ConvertTo-TestWslPath $approvedBase) 1M
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'base creation failed'
        $otherBase = Join-Path $vmDirFixture 'other-base.qcow2'
        & wsl.exe qemu-img create -f qcow2 (
            ConvertTo-TestWslPath $otherBase) 1M
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'other base creation failed'
        $overlay = Join-Path $vmDirFixture 'backchild.qcow2'
        & wsl.exe qemu-img create -f qcow2 -F qcow2 -b (
            ConvertTo-TestWslPath $approvedBase) (
            ConvertTo-TestWslPath $overlay)
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'overlay creation failed'
        & wsl.exe qemu-img rebase -u -f qcow2 -F qcow2 `
            -b 'approved-base.qcow2' (ConvertTo-TestWslPath $overlay)
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'overlay rebase failed'
        $port = Get-FreeTcpPort
        $profilePath = Join-Path $vmDirFixture `
            'xpdriver-backchild.qemu_cmd_line'
        Write-FakeProfile -Path $profilePath -Hda 'backchild.qcow2' -Port $port
        $outputPath = Join-Path $caseRoot 'start-state.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-backing', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath, '-PlannedChildName', 'backchild',
            '-ExpectChildCreated', '-BasePath', $approvedBase)
        Assert-TestPreflight ($run.ExitCode -eq 0) `
            "matching backing chain failed: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'plannedChildBacking'
        Assert-TestPreflight ($check.ok) `
            'approved backing chain did not pass'

        & wsl.exe qemu-img rebase -u -f qcow2 -F qcow2 `
            -b 'other-base.qcow2' (ConvertTo-TestWslPath $overlay)
        Assert-TestPreflight ($LASTEXITCODE -eq 0) 'overlay re-point failed'
        $outputPath = Join-Path $caseRoot 'start-state-wrong.json'
        $run = Invoke-IsolatedPreflight -Arguments @(
            '-TaskId', 'preflight-backing-wrong', '-OutputPath', $outputPath,
            '-WorkspaceRoot', $workspace,
            '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
            '-ContentPath', $profilePath, '-PlannedChildName', 'backchild',
            '-ExpectChildCreated', '-BasePath', $approvedBase)
        Assert-TestPreflight ($run.ExitCode -eq 1) `
            "re-pointed backing chain was not rejected: $($run.Output)"
        $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
        $check = Get-PreflightCheck $record 'plannedChildBacking'
        Assert-TestPreflight (-not $check.ok -and
            $check.failureClass -eq 'HANDOFF_INCOMPLETE') `
            're-pointed backing check is wrong'
    } else {
        Write-Output 'SKIP: backing chain case (wsl.exe unavailable)'
    }

    # Missing prerequisites and missing base are infrastructure failures.
    $caseRoot = Join-Path $testRoot 'case-nopycdlib'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace -NoPycdlib
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-nopycdlib', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "missing pycdlib was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'prerequisitePycdlib'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'INFRASTRUCTURE_FAILURE') `
        'pycdlib check is wrong'

    $caseRoot = Join-Path $testRoot 'case-nobase'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-nobase', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-BasePath', (Join-Path $caseRoot 'missing.qcow2'))
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "missing base was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'baseImage'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'INFRASTRUCTURE_FAILURE') `
        'missing base check is wrong'

    # An existing planned child blocks name reuse in the pre-creation stage.
    $caseRoot = Join-Path $testRoot 'case-taken'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    [IO.File]::WriteAllBytes((Join-Path $workspace 'qemu\winxp\taken.qcow2'),
        [byte[]](1..4))
    [IO.File]::WriteAllText((Join-Path $workspace `
        'qemu\winxp\xpdriver-taken.qemu_cmd_line'), 'occupied')
    $outputPath = Join-Path $caseRoot 'start-state.json'
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-taken', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'),
        '-PlannedChildName', 'taken')
    Assert-TestPreflight ($run.ExitCode -eq 1) `
        "taken child name was not rejected: $($run.Output)"
    $record = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
    $check = Get-PreflightCheck $record 'plannedChild'
    Assert-TestPreflight (-not $check.ok -and
        $check.failureClass -eq 'INFRASTRUCTURE_FAILURE') `
        'taken child check is wrong'

    # The output is never overwritten.
    $caseRoot = Join-Path $testRoot 'case-overwrite'
    $workspace = Join-Path $caseRoot 'workspace'
    [void](New-Item -ItemType Directory -Path $workspace -Force)
    New-FakeWorkspace -Root $workspace
    $outputPath = Join-Path $caseRoot 'start-state.json'
    [IO.File]::WriteAllText($outputPath, 'prior evidence')
    $run = Invoke-IsolatedPreflight -Arguments @(
        '-TaskId', 'preflight-overwrite', '-OutputPath', $outputPath,
        '-WorkspaceRoot', $workspace, '-HostRepoPath', (Join-Path $caseRoot 'absent-host'))
    Assert-TestPreflight ($run.ExitCode -ne 0) `
        'existing output did not fail closed'
    Assert-TestPreflight ((Get-Content -Raw -LiteralPath $outputPath) -eq `
        'prior evidence') 'existing output was overwritten'
} finally {
    if ($heldLease) {
        Exit-XpAcceptanceArtifactLease -Lease $heldLease -Status failed
    }
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}

Write-Output 'test preflight tests passed'
