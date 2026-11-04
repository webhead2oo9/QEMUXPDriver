param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$TaskId,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$EvidenceDirectory,
    [string]$ChildName,
    [string]$BasePath,
    [string]$ContentPath,
    [int]$QmpPort,
    # Diagnostic only. Forwarded to the overlay generator so the generated
    # command line opens a gdb stub. Leave unset for every ordinary run.
    [ValidateRange(0, 65535)]
    [int]$GdbPort = 0,
    [string]$RetroHostPath = $env:XPD_RETRO_HOST,
    [string]$CorePath,
    [string[]]$Modes = @('Install', 'Boot', 'D3dVisual', 'D3d9', 'D3dVs11',
        'D3dVsMultiContext', 'D3dVsSuccession'),
    [string]$FrozenInputsPath,
    [ValidateRange(1, 25)]
    [int]$PerformanceRuns = 3,
    [ValidateRange(15, 180)]
    [int]$GuestLaunchDelaySeconds = 40,
    [string]$PackageDirectory,
    [switch]$ContinueOnFailure,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'd3d-driver-profile.ps1')
. (Join-Path $PSScriptRoot 'maxpayne-visual.ps1')
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')
. (Join-Path $PSScriptRoot 'acceptance-iso.ps1')

# One invocation runs the whole delegated test contract: preflight opens the
# observation window, an optional fresh child is created through the
# two-stage flow, every requested mode runs through the canonical
# invoke-xp-acceptance launcher so each run leaves a finalized artifact
# bundle, performance modes repeat and report a median, and the handback
# always closes the window - success or failure - auditing every bundle.
$modeTable = @{
    'Install' = @{ Params = [ordered]@{ Xpdm = $true; InstallOnly = $true };
        Timeout = 300; Tier = 'T2'; Performance = $false }
    'Boot' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true };
        Timeout = 360; Tier = 'T2'; Performance = $false }
    'D3dVisual' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        D3dVisual = $true }; Timeout = 300; Tier = 'T2'; Performance = $false }
    'D3d9' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        D3d9 = $true }; Timeout = 300; Tier = 'T2'; Performance = $false }
    'D3d9Multistream7' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; D3d9 = $true; D3d9Multistream7 = $true };
        Timeout = 360; Tier = 'T2-diagnostic'; Performance = $false }
    'D3d9SelfTest' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        D3d9SelfTest = $true }; Timeout = 300; Tier = 'T2';
        Performance = $false }
    'D3dVs11' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        D3dVs11 = $true }; Timeout = 360; Tier = 'T2'; Performance = $false }
    'D3dVsMultiContext' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; D3dVsMultiContext = $true }; Timeout = 360;
        Tier = 'T2'; Performance = $false }
    'D3dVsSuccession' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; D3dVsSuccession = $true }; Timeout = 360;
        Tier = 'T2'; Performance = $false }
    'MaxPayne' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        D3dGame = $true; D3dGameplay = $true; D3dPerformance = $true };
        Timeout = 360; Tier = 'T3'; Performance = $true }
    'MaxPayneVisual' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; D3dGame = $true; D3dGameplay = $true };
        Timeout = 600; Tier = 'T3'; Performance = $false; Visual = $true }
    'MaxPayneNormalLaunch' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; D3dGame = $true; D3dNormalLaunch = $true };
        Timeout = 420; Tier = 'T3'; Performance = $false }
    'UT2003' = @{ Params = [ordered]@{ Xpdm = $true; BootOnly = $true;
        ObserveGame = 'UT2003'; ObservePerformance = $true }; Timeout = 300;
        Tier = 'T3'; Performance = $true }
    'UT2003Gameplay' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; ObserveGame = 'UT2003Gameplay';
        RecordVideo = $true }; Timeout = 300; Tier = 'T3';
        Performance = $false; Visual = $true }
    'UT2003GameplayPerformance' = @{ Params = [ordered]@{
        Xpdm = $true; BootOnly = $true; ObserveGame = 'UT2003Gameplay';
        ObservePerformance = $true }; Timeout = 360; Tier = 'T3';
        Performance = $true }
    'UT2003Visual' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; ObserveGame = 'UT2003'; RecordVideo = $true };
        Timeout = 900; Tier = 'T3'; Performance = $false; Visual = $true }
    'NFSU2Visual' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; ObserveGame = 'NFSU2'; RecordVideo = $true };
        Timeout = 600; Tier = 'T3'; Performance = $false; Visual = $true }
    'NFSU2Performance' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; ObserveGame = 'NFSU2';
        ObservePerformance = $true }; Timeout = 300; Tier = 'T3';
        Performance = $true }
    'Warcraft3D3D8Visual' = @{ Params = [ordered]@{ Xpdm = $true;
        BootOnly = $true; ObserveGame = 'Warcraft3D3D8';
        RecordVideo = $true }; Timeout = 600; Tier = 'T3';
        Performance = $false; Visual = $true }
    'Warcraft3D3D8Performance' = @{ Params = [ordered]@{
        Xpdm = $true; BootOnly = $true; ObserveGame = 'Warcraft3D3D8';
        ObservePerformance = $true }; Timeout = 360; Tier = 'T3';
        Performance = $true }
}
# powershell.exe -File binds a comma-joined list as one literal string, so
# the documented single-command invocation would otherwise see one unknown
# mega-mode; accept both spellings.
$Modes = @($Modes | ForEach-Object { $_ -split ',' } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ })
if (-not $Modes.Count) { throw 'No modes requested' }
foreach ($mode in $Modes) {
    if (-not $modeTable.ContainsKey($mode)) {
        throw "Unknown mode '$mode'; supported: $($modeTable.Keys -join ', ')"
    }
}
if ($Modes -contains 'D3d9Multistream7') {
    $installIndex = [Array]::IndexOf($Modes, 'Install')
    $multistreamIndex = [Array]::IndexOf($Modes, 'D3d9Multistream7')
    if ($installIndex -lt 0 -or -not $ChildName) {
        throw 'D3d9Multistream7 requires Install on a fresh -ChildName in the same cycle'
    }
    if ($installIndex -gt $multistreamIndex) {
        throw 'Install must precede D3d9Multistream7 in the same cycle'
    }
}
foreach ($visualMode in @(
        'MaxPayneVisual', 'UT2003Visual', 'UT2003Gameplay',
        'NFSU2Visual')) {
    if (@($Modes | Where-Object { $_ -eq $visualMode }).Count -gt 1) {
        throw "$visualMode may appear only once per cycle"
    }
    if ($Modes -contains $visualMode) {
        $installIndex = [Array]::IndexOf($Modes, 'Install')
        $visualIndex = [Array]::IndexOf($Modes, $visualMode)
        if ($installIndex -lt 0 -or -not $ChildName) {
            throw "$visualMode requires Install on a fresh -ChildName in the same cycle"
        }
        if ($installIndex -gt $visualIndex) {
            throw "Install must precede $visualMode in the same cycle"
        }
    }
}
if ($Modes -contains 'MaxPayneNormalLaunch') {
    $installIndex = [Array]::IndexOf($Modes, 'Install')
    $launchIndex = [Array]::IndexOf($Modes, 'MaxPayneNormalLaunch')
    if ($installIndex -lt 0 -or -not $ChildName) {
        throw 'MaxPayneNormalLaunch requires Install on a fresh -ChildName in the same cycle'
    }
    if ($installIndex -gt $launchIndex) {
        throw 'Install must precede MaxPayneNormalLaunch in the same cycle'
    }
}
if (@($Modes | Where-Object {
        $_ -eq 'MaxPayneNormalLaunch' }).Count -gt 1) {
    throw 'MaxPayneNormalLaunch may appear only once per cycle'
}
if ($Modes -contains 'Warcraft3D3D8Visual') {
    $installIndex = [Array]::IndexOf($Modes, 'Install')
    $oracleIndex = [Array]::IndexOf($Modes, 'Warcraft3D3D8Visual')
    if ($installIndex -lt 0 -or -not $ChildName) {
        throw 'Warcraft3D3D8Visual requires Install on a fresh -ChildName in the same cycle'
    }
    if ($installIndex -gt $oracleIndex) {
        throw 'Install must precede Warcraft3D3D8Visual in the same cycle'
    }
}
if (@($Modes | Where-Object {
        $_ -eq 'Warcraft3D3D8Visual' }).Count -gt 1) {
    throw 'Warcraft3D3D8Visual may appear only once per cycle'
}

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $FrozenInputsPath) {
    $FrozenInputsPath = Join-Path $workspace `
        'guest\acceptance\frozen-inputs-ut-oracle-r5.json'
}
$frozenInputs = $null
$frozenExecutionInputs = $null
if (-not $DryRun) {
    # Validation consumes one frozen input set produced outside the cycle;
    # nothing rebuilds the package or ISO inside a validation window. Use its
    # immutable object paths unless the caller explicitly selects paths, in
    # which case the strict execution-input check below still binds them.
    $frozenInputs = Get-XpFrozenInputs -Path $FrozenInputsPath
    if (-not $PSBoundParameters.ContainsKey('RetroHostPath')) {
        $RetroHostPath = [string]$frozenInputs.retroHost.path
    }
    if (-not $PSBoundParameters.ContainsKey('CorePath')) {
        $CorePath = [string]$frozenInputs.core.path
    }
    $frozenExecutionInputs = Assert-XpFrozenExecutionInputs `
        -Manifest $frozenInputs -RetroHostPath $RetroHostPath `
        -CorePath $CorePath
}
if (-not $RetroHostPath) {
    throw 'Pass -RetroHostPath or set XPD_RETRO_HOST to retro_host.exe'
}
if (-not $CorePath) {
    $CorePath = Get-XpDefaultCorePath -Workspace $workspace
}
$vmDir = Join-Path $workspace 'qemu\winxp'
$harness = $PSScriptRoot
$powershell = Join-Path $PSHOME 'powershell.exe'

if ($ChildName) {
    if (-not $BasePath -or -not $QmpPort) {
        throw '-ChildName requires -BasePath and -QmpPort'
    }
    if ($ContentPath) {
        throw '-ChildName derives the content profile; do not pass -ContentPath'
    }
    $ContentPath = Join-Path $vmDir "xpdriver-$ChildName.qemu_cmd_line"
} elseif (-not $ContentPath) {
    throw 'Pass -ContentPath for an existing target or -ChildName for a fresh child'
}
$contentFull = [IO.Path]::GetFullPath($ContentPath)

# Install writes the target disk. A registered preserved target passes
# preflight writable through its own profile, so the guard lives here: an
# install lands only on a fresh child created inside this cycle.
if (($Modes -contains 'Install') -and -not $ChildName) {
    throw 'Install mode requires -ChildName (a fresh child overlay); it never installs over an existing -ContentPath target'
}
# Install defaults to the staging package, but a separately frozen diagnostic
# generation may name a different immutable package directory. The selected
# directory is forwarded to every runner and must match the frozen manifest.
$stagingPackage = Join-Path $workspace 'guest\xpdm\build\package'
if (($Modes -contains 'Install') -and -not $PackageDirectory) {
    $PackageDirectory = if (-not $DryRun -and $frozenInputs.package.directory) {
        [string]$frozenInputs.package.directory
    } else {
        $stagingPackage
    }
}
if ($PackageDirectory -and -not $DryRun) {
    $selectedPackage = [IO.Path]::GetFullPath($PackageDirectory)
    $frozenPackage = [IO.Path]::GetFullPath(
        [string]$frozenInputs.package.directory)
    if ($selectedPackage -ine $frozenPackage) {
        throw 'selected package directory differs from frozen inputs'
    }
}

$packageIdentityBefore = $null
$packageFingerprintBefore = $null
if ($PackageDirectory -and -not $DryRun) {
    $packageIdentityBefore = Get-XpAcceptancePackageIdentity `
        -Directory $PackageDirectory
    if (-not $packageIdentityBefore.valid) {
        throw 'Staged package identity is invalid before the test cycle'
    }
    $packageFingerprintBefore = @(
        "valid=$($packageIdentityBefore.valid)",
        "findings=$(@($packageIdentityBefore.findings) -join ',')",
        "driverVer=$($packageIdentityBefore.driverVer)",
        "signer=$($packageIdentityBefore.certificate.thumbprint)",
        @($packageIdentityBefore.files | Sort-Object name |
            ForEach-Object { "$($_.name):$($_.size):$($_.sha256)" })
    ) -join ';'
}

$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
if ((Test-Path -LiteralPath $evidence) -and
        @(Get-ChildItem -LiteralPath $evidence -Force).Count) {
    throw "Refusing to reuse a non-empty evidence directory: $evidence"
}
[void](New-Item -ItemType Directory -Force -Path $evidence)

$commandLog = [Collections.Generic.List[string]]::new()
$results = [Collections.Generic.List[object]]::new()
$runDirectories = [Collections.Generic.List[string]]::new()
$preparedAcceptanceIso = $null
$preparedRuntimeContentPath = $null
$cycleOk = $true

function Invoke-XpCycleChild {
    param([string]$Label, [string[]]$ChildArguments, [string]$OutputLog)

    $rendered = "$powershell -NoLogo -NoProfile -NonInteractive " +
        "-ExecutionPolicy Bypass $($ChildArguments -join ' ')"
    $script:commandLog.Add($rendered)
    # Direct console writes keep the function's pipeline output to exactly
    # the returned exit code.
    [Console]::Out.WriteLine("CycleStep taskId=$TaskId step=$Label")
    if ($script:DryRunActive) {
        [Console]::Out.WriteLine("CycleDryRun taskId=$TaskId command=$rendered")
        return 0
    }
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $powershell -NoLogo -NoProfile -NonInteractive `
            -ExecutionPolicy Bypass @ChildArguments 2>&1)
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $saved
    }
    if ($null -eq $exit) { $exit = 1 }
    if ($OutputLog) {
        $text = (@($output | ForEach-Object { $_.ToString() }) -join
            [Environment]::NewLine) + [Environment]::NewLine
        [IO.File]::WriteAllText($OutputLog, $text)
    }
    return $exit
}
$script:DryRunActive = [bool]$DryRun

function ConvertTo-XpCycleQuoted {
    param([string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

# invoke-xp-acceptance takes -RunnerParameters as a hashtable, which cannot
# cross a powershell.exe -File boundary; runs and the handback (whose
# -RunDirectory is an array) are launched through -Command instead.
function ConvertTo-XpCycleRunnerParameters {
    param($Parameters)

    $pairs = foreach ($key in $Parameters.Keys) {
        $value = $Parameters[$key]
        if ($value -is [bool]) {
            '{0} = ${1}' -f $key, $value.ToString().ToLower()
        } elseif ($value -is [int]) {
            "$key = $value"
        } else {
            "$key = " + (ConvertTo-XpCycleQuoted "$value")
        }
    }
    return '@{ ' + ($pairs -join '; ') + ' }'
}

function Get-XpCycleRunId {
    param([string]$Seed)

    $candidate = $Seed -replace '[^A-Za-z0-9._-]', '-'
    if ($candidate.Length -gt 64) {
        $candidate = $candidate.Substring($candidate.Length - 64)
    }
    return ($candidate -replace '^[^A-Za-z0-9]+', '')
}

function Get-XpCycleFileIdentity {
    param([string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $item = Get-Item -LiteralPath $full
        return [pscustomobject][ordered]@{
            path = $full
            bytes = $item.Length
            sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
        }
    }
    return [pscustomobject][ordered]@{
        path = $full
        bytes = $null
        sha256 = $null
    }
}

# The final non-empty miniport counter session carries the run's batching
# evidence. The observer teardown appends an empty session with its own
# header, so each candidate window must end at the next session header, not
# at a fixed line count.
function Get-XpCycleRunCounters {
    param([string]$DriverLog)

    if (-not (Test-Path -LiteralPath $DriverLog -PathType Leaf)) { return $null }
    $parsed = ConvertFrom-XpD3dDriverProfile -Text (
        Get-Content -Raw -LiteralPath $DriverLog)
    if ($parsed.miniport -and $parsed.miniport.logicalDraws) {
        return $parsed
    }
    $lines = @((Get-Content -Raw -LiteralPath $DriverLog) -split "`r?`n" |
        ForEach-Object { $_.Trim() })
    for ($index = $lines.Count - 1; $index -ge 0; --$index) {
        if ($lines[$index] -ne 'QXPMP D3D DRAWS') { continue }
        $end = $lines.Count - 1
        for ($next = $index + 1; $next -lt $lines.Count; ++$next) {
            if ($lines[$next] -eq 'QXPMP D3D DRAWS') {
                $end = $next - 1
                break
            }
        }
        $candidate = ConvertFrom-XpD3dDriverProfile -Text (
            ($lines[$index..$end] -join "`n"))
        if ($candidate.miniport -and $candidate.miniport.logicalDraws) {
            return $candidate
        }
    }
    return $null
}

function Get-XpCycleFreshFps {
    param([string]$HarnessLog)

    if (-not (Test-Path -LiteralPath $HarnessLog -PathType Leaf)) { return $null }
    $match = [regex]::Match((Get-Content -Raw -LiteralPath $HarnessLog),
        'FramePublicationProfile workload=\S+ durationUs=\d+ callbacks=\d+ ' +
        'fresh=\d+ duped=\d+ dupedPercent=[\d.]+ freshFps=([\d.]+)')
    if ($match.Success) { return [double]$match.Groups[1].Value }
    return $null
}

function Get-XpCycleVisualEvidence {
    param(
        [string]$RunDirectory,
        [string]$Mode
    )

    if ($Mode -eq 'MaxPayneVisual') {
        $check = Test-XpMaxPayneVisualBundle -Directory $RunDirectory
        $evidence = $check.evidence
        return [pscustomobject][ordered]@{
            valid = [bool]$check.valid
            findings = @($check.findings)
            kind = 'xpdriver-maxpayne-visual'
            sidecar = 'maxpayne-visual-sequence.json'
            selectedCount = if ($evidence) {
                [int]$evidence.selection.selectedCount
            } else { 0 }
            decodableFrameCount = if ($evidence) {
                [int]$evidence.selection.decodableFrameCount
            } else { 0 }
            images = if ($evidence) {
                @($evidence.images | ForEach-Object {
                    [pscustomobject][ordered]@{
                        index = $_.index
                        fileName = $_.fileName
                        sourceSequence = $_.sourceSequence
                        capturedUtc = $_.capturedUtc
                        hostOffsetMs = $_.hostOffsetMs
                        sha256 = $_.sha256
                    }
                })
            } else { @() }
        }
    }

    $check = Test-XpGameObserveVisualBundle `
        -Directory $RunDirectory -Mode $Mode
    $evidence = $check.evidence
    return [pscustomobject][ordered]@{
        valid = [bool]$check.valid
        findings = @($check.findings)
        kind = if ($evidence) { $evidence.kind } else { $null }
        mode = $Mode
        observedMs = if ($evidence) { $evidence.observedMs } else { $null }
        semanticTransition = if ($evidence) {
            $evidence.semanticTransition
        } else { $null }
        visualSpanSeconds = if ($evidence) {
            $evidence.visualSpanSeconds
        } else { $null }
        visualMaximumGapSeconds = if ($evidence) {
            $evidence.visualMaximumGapSeconds
        } else { $null }
        selectedCount = if ($evidence) {
            [int]$evidence.selectedCount
        } else { 0 }
        samples = if ($evidence) { @($evidence.samples) } else { @() }
        video = if ($evidence) { $evidence.video } else { $null }
        cadence = if ($evidence) { $evidence.cadence } else { $null }
    }
}

function Get-XpCycleMedian {
    param([double[]]$Values)

    if (-not $Values.Count) { return $null }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return $sorted[$middle] }
    return [Math]::Round(($sorted[$middle - 1] + $sorted[$middle]) / 2.0, 6)
}

# The binaries that will execute every run are identified up front; each
# canonical bundle additionally records the hashes its own run used.
$hostIdentity = if ($frozenExecutionInputs) {
    $frozenExecutionInputs.retroHost
} else { Get-XpCycleFileIdentity -Path $RetroHostPath }
$coreIdentity = if ($frozenExecutionInputs) {
    $frozenExecutionInputs.core
} else { Get-XpCycleFileIdentity -Path $CorePath }

$failedStep = $null
try {
    # Stage-1 preflight opens the observation window; with a planned child it
    # also proves the name is completely free before the overlay helper runs.
    $stage1 = @('-File', (Join-Path $harness 'verify-xp-test-preflight.ps1'),
        '-TaskId', $TaskId,
        '-OutputPath', (Join-Path $evidence 'start-state.json'))
    if ($ChildName) {
        $stage1 += @('-PlannedChildName', $ChildName, '-QmpPort', "$QmpPort")
    } else {
        $stage1 += @('-ContentPath', $contentFull)
    }
    if ($BasePath) { $stage1 += @('-BasePath', $BasePath) }
    if ((Invoke-XpCycleChild -Label 'preflight' -ChildArguments $stage1) -ne 0) {
        throw 'stage-1 preflight failed'
    }

    if ($ChildName) {
        $overlay = @('-File', (Join-Path $harness 'new-xpdm-overlay.ps1'),
            '-Name', $ChildName, '-QmpPort', "$QmpPort",
            '-BasePath', $BasePath)
        if ($GdbPort -gt 0) {
            $overlay += @('-GdbPort', "$GdbPort")
        }
        if ((Invoke-XpCycleChild -Label 'overlay' -ChildArguments $overlay) -ne 0) {
            throw 'child overlay creation failed'
        }
        $stage2 = @('-File', (Join-Path $harness 'verify-xp-test-preflight.ps1'),
            '-TaskId', $TaskId,
            '-OutputPath', (Join-Path $evidence 'start-state-postcreate.json'),
            '-ContentPath', $contentFull,
            '-PlannedChildName', $ChildName, '-ExpectChildCreated')
        if ($BasePath) { $stage2 += @('-BasePath', $BasePath) }
        if ((Invoke-XpCycleChild -Label 'preflight-postcreate' `
                -ChildArguments $stage2) -ne 0) {
            throw 'stage-2 preflight failed'
        }
    }

    # Every mode, Install included, mounts one cycle-private copy of the
    # frozen acceptance ISO. The ISO is produced outside the validation
    # window (publish-xp-acceptance-inputs.ps1); nothing rebuilds it here.
    $privateIsoPath = Join-Path $evidence `
        'prepared-xpdriver-acceptance.iso'
    $privateContentPath = Join-Path $evidence `
        'prepared-content.qemu_cmd_line'
    $runtimeContentName = if ($DryRun) {
        '.xpdriver-prepared-dryrun.qemu_cmd_line'
    } else {
        '.xpdriver-prepared-' + [Guid]::NewGuid().ToString('N') +
            '.qemu_cmd_line'
    }
    $preparedRuntimeContentPath = Join-Path `
        (Split-Path -Parent $contentFull) $runtimeContentName
    $isoCommand = "frozen-inputs $FrozenInputsPath"
    $commandLog.Add($isoCommand)
    [Console]::Out.WriteLine(
        "CycleStep taskId=$TaskId step=prepare-acceptance-iso")
    if ($DryRun) {
        [Console]::Out.WriteLine(
            "CycleDryRun taskId=$TaskId command=$isoCommand")
        $isoHash = '0' * 64
    } else {
        Copy-Item -LiteralPath `
            ([string]$frozenInputs.acceptanceIso.path) `
            -Destination $privateIsoPath
        $contentText = Get-Content -Raw -LiteralPath $contentFull
        $contentText = Set-XpContentAcceptanceIsoPath `
            -ContentText $contentText -IsoPath $privateIsoPath
        [IO.File]::WriteAllText($privateContentPath, $contentText)
        [IO.File]::WriteAllText(
            $preparedRuntimeContentPath, $contentText)
        (Get-Item -LiteralPath $privateIsoPath).IsReadOnly = $true
        $isoIdentity = Get-XpAcceptanceIsoIdentity `
            -Path $privateIsoPath
        if ($isoIdentity.sha256 -cne
                [string]$frozenInputs.acceptanceIso.sha256) {
            throw 'cycle-private frozen ISO copy does not match the manifest'
        }
        $isoHash = $isoIdentity.sha256
    }
    $preparedAcceptanceIso = [pscustomobject][ordered]@{
        schema = 1
        path = $privateIsoPath
        sha256 = $isoHash
        length = if ($DryRun) { $null } else { $isoIdentity.length }
        contentPath = $preparedRuntimeContentPath
        contentEvidencePath = $privateContentPath
        preparedOnce = $true
    }
    if (-not $DryRun) {
        [IO.File]::WriteAllText(
            (Join-Path $evidence 'acceptance-iso-identity.json'),
            (($preparedAcceptanceIso | ConvertTo-Json -Depth 4) +
             [Environment]::NewLine))
    }

    $invokeAcceptance = Join-Path $harness 'invoke-xp-acceptance.ps1'
    foreach ($mode in $Modes) {
        $spec = $modeTable[$mode]
        $runCount = if ($spec.Performance) { $PerformanceRuns } else { 1 }
        $fpsValues = [Collections.Generic.List[double]]::new()
        for ($run = 1; $run -le $runCount; ++$run) {
            $runName = if ($runCount -gt 1) { "$mode-r$run" } else { $mode }
            $runDir = Join-Path $evidence $runName
            $harnessLog = Join-Path $evidence "$runName.harness-output.log"
            $runnerParameters = [ordered]@{}
            foreach ($key in $spec.Params.Keys) {
                $runnerParameters[$key] = $spec.Params[$key]
            }
            $runnerParameters['ContentPath'] = if ($preparedAcceptanceIso) {
                $preparedAcceptanceIso.contentPath
            } else {
                $contentFull
            }
            $runnerParameters['RetroHostPath'] = $RetroHostPath
            $runnerParameters['CorePath'] = $CorePath
            $runnerParameters['FrozenInputsPath'] = $FrozenInputsPath
            if ($PackageDirectory) {
                $runnerParameters['PackageDirectory'] = $PackageDirectory
            }
            $runnerParameters['TimeoutSeconds'] = $spec.Timeout
            $runnerParameters['GuestLaunchDelaySeconds'] =
                $GuestLaunchDelaySeconds
            if ($preparedAcceptanceIso) {
                $runnerParameters['PreparedAcceptanceIsoPath'] =
                    $preparedAcceptanceIso.path
                $runnerParameters['PreparedAcceptanceIsoSha256'] =
                    $preparedAcceptanceIso.sha256
            }
            if ($QmpPort) { $runnerParameters['QmpPort'] = $QmpPort }
            $launcherDiagnostic = if ($spec.Tier -like '*-diagnostic') {
                ' -Diagnostic'
            } else {
                ''
            }
            $runCommand = ('& {0} -ArtifactDirectory {1} -RunId {2} ' +
                '-TaskId {3} -Tier {4}{5} -RunnerParameters {6}') -f
                (ConvertTo-XpCycleQuoted $invokeAcceptance),
                (ConvertTo-XpCycleQuoted $runDir),
                (ConvertTo-XpCycleQuoted (
                    Get-XpCycleRunId -Seed "$TaskId-$runName")),
                (ConvertTo-XpCycleQuoted $TaskId),
                (ConvertTo-XpCycleQuoted $spec.Tier),
                $launcherDiagnostic,
                (ConvertTo-XpCycleRunnerParameters $runnerParameters)
            $runDirectories.Add($runDir)
            $exit = Invoke-XpCycleChild -Label $runName `
                -ChildArguments @('-Command', $runCommand) `
                -OutputLog $harnessLog
            $runTiming = $null
            $runResultPath = Join-Path $runDir 'harness-result.json'
            if (-not $DryRun -and
                (Test-Path -LiteralPath $runResultPath -PathType Leaf)) {
                try {
                    $runTiming = (Get-Content -Raw -LiteralPath `
                        $runResultPath | ConvertFrom-Json).timing
                } catch {
                    $runTiming = $null
                }
            }
            $record = [ordered]@{
                mode = $mode
                run = $runName
                artifactDirectory = $runDir
                exitCode = $exit
                result = if ($exit -eq 0) { 'pass' } else { 'fail' }
                freshFps = $null
                counters = $null
                visualEvidence = $null
                timing = $runTiming
            }
            if (-not $DryRun) {
                if ($spec.Performance) {
                    $fps = Get-XpCycleFreshFps -HarnessLog $harnessLog
                    $record.freshFps = $fps
                    if ($exit -eq 0 -and $null -eq $fps) {
                        # A passing performance run must publish its
                        # measurement; a missing profile line is a failed
                        # run, not a pass with fewer samples.
                        $exit = 1
                        $record.result = 'fail'
                        $record.failure = 'run exited 0 without a ' +
                            'FramePublicationProfile freshFps measurement'
                    } elseif ($exit -eq 0) {
                        $fpsValues.Add($fps)
                    }
                }
                $counters = Get-XpCycleRunCounters -DriverLog (
                    Join-Path $runDir 'driver.log')
                if ($counters -and $counters.miniport) {
                    $record.counters = [ordered]@{
                        logicalDraws = $counters.miniport.logicalDraws
                        singleDrawIoctls = $counters.miniport.singleDrawIoctls
                        drawBatchIoctls = $counters.miniport.drawBatchIoctls
                        presents = $counters.miniport.presents
                        presentationSourceCountersPresent =
                            $counters.miniport.presentationSourceCountersPresent
                        cpuPresents = $counters.miniport.cpuPresents
                        surfaceTexturePresents =
                            $counters.miniport.surfaceTexturePresents
                        emptyPresents = $counters.miniport.emptyPresents
                        surfaceCopies = $counters.miniport.surfaceCopies
                        surfaceReadbacks = $counters.miniport.surfaceReadbacks
                        compactDraws = $counters.miniport.compactDraws
                        compactIndexedDraws = $counters.miniport.compactIndexedDraws
                    }
                }
                if ([bool]$spec.Visual) {
                    try {
                        $visual = Get-XpCycleVisualEvidence `
                            -RunDirectory $runDir -Mode $mode
                    } catch {
                        $visual = [pscustomobject][ordered]@{
                            valid = $false
                            findings = @(
                                "cycle visual verification failed closed: $($_.Exception.Message)")
                            kind = 'xpdriver-cycle-visual-invalid'
                            mode = $mode
                            observedMs = $null
                            semanticTransition = $null
                            visualSpanSeconds = $null
                            visualMaximumGapSeconds = $null
                            selectedCount = 0
                            samples = @()
                            video = $null
                            cadence = $null
                        }
                    }
                    $record.visualEvidence = $visual
                    if ($exit -eq 0 -and -not $visual.valid) {
                        $exit = 1
                        $record.exitCode = 1
                        $record.result = 'fail'
                        $record.failure = 'visual companion evidence failed: ' +
                            (@($visual.findings) -join '; ')
                    }
                }
            }
            $results.Add([pscustomobject]$record)
            Write-Output "CycleRun taskId=$TaskId run=$runName result=$($record.result)"
            if ($exit -ne 0) {
                $script:cycleOk = $false
                if (-not $ContinueOnFailure) {
                    $script:failedStep = $runName
                    break
                }
            }
        }
        if ($spec.Performance -and $fpsValues.Count) {
            $median = Get-XpCycleMedian -Values $fpsValues.ToArray()
            $results.Add([pscustomobject][ordered]@{
                mode = $mode
                run = "$mode-median"
                artifactDirectory = $null
                exitCode = $null
                result = 'summary'
                freshFps = $median
                counters = $null
                visualEvidence = $null
            })
            Write-Output "CycleMedian taskId=$TaskId mode=$mode freshFps=$median runs=$($fpsValues.Count)"
        }
        if ($script:failedStep) { break }
    }
} catch {
    $cycleOk = $false
    if (-not $failedStep) { $failedStep = $_.Exception.Message }
    Write-Output "CycleError taskId=$TaskId detail=$($_.Exception.Message)"
} finally {
    # The window always closes: a failed matrix still gets an environment
    # audit, every attempted run's bundle is audited, and the audit result
    # never overwrites the matrix verdict.
    $handbackCommand = '& {0} -StartStatePath {1} -OutputPath {2}' -f
        (ConvertTo-XpCycleQuoted (
            Join-Path $harness 'verify-xp-test-handback.ps1')),
        (ConvertTo-XpCycleQuoted (Join-Path $evidence 'start-state.json')),
        (ConvertTo-XpCycleQuoted (Join-Path $evidence 'handback-audit.json'))
    if ($runDirectories.Count) {
        $handbackCommand += ' -RunDirectory ' + ((
            $runDirectories | ForEach-Object {
                ConvertTo-XpCycleQuoted $_ }) -join ',')
    }
    if ($PackageDirectory) {
        $handbackCommand += ' -PackageDirectory ' + (
            ConvertTo-XpCycleQuoted $PackageDirectory)
    }
    $handbackExit = Invoke-XpCycleChild -Label 'handback' `
        -ChildArguments @('-Command', $handbackCommand)
    if ($handbackExit -ne 0) { $cycleOk = $false }
    if ($preparedRuntimeContentPath -and
        (Test-Path -LiteralPath $preparedRuntimeContentPath)) {
        Remove-Item -LiteralPath $preparedRuntimeContentPath -Force `
            -ErrorAction SilentlyContinue
    }
}

if (-not $DryRun) {
    $packageIdentity = $null
    if ($PackageDirectory) {
        try {
            $identity = Get-XpAcceptancePackageIdentity `
                -Directory $PackageDirectory
            $packageFingerprintAfter = @(
                "valid=$($identity.valid)",
                "findings=$(@($identity.findings) -join ',')",
                "driverVer=$($identity.driverVer)",
                "signer=$($identity.certificate.thumbprint)",
                @($identity.files | Sort-Object name |
                    ForEach-Object {
                        "$($_.name):$($_.size):$($_.sha256)" })
            ) -join ';'
            if (-not $identity.valid) {
                $cycleOk = $false
                if (-not $failedStep) {
                    $failedStep = 'final staged package identity is invalid'
                }
            } elseif ($packageFingerprintBefore -and
                $packageFingerprintAfter -cne $packageFingerprintBefore) {
                $cycleOk = $false
                if (-not $failedStep) {
                    $failedStep = 'staged package identity changed during cycle'
                }
            }
            $packageIdentity = [ordered]@{
                directory = $identity.directory
                valid = $identity.valid
                driverVer = $identity.driverVer
                signerThumbprint = $identity.certificate.thumbprint
                unchangedFromStart = $identity.valid -and
                    (-not $packageFingerprintBefore -or
                     $packageFingerprintAfter -ceq $packageFingerprintBefore)
                findings = @($identity.findings)
                files = @($identity.files | Sort-Object name |
                    ForEach-Object {
                        [ordered]@{
                            name = $_.name
                            size = $_.size
                            sha256 = $_.sha256
                        }
                    })
            }
        } catch {
            $cycleOk = $false
            if (-not $failedStep) {
                $failedStep = 'final staged package identity check failed'
            }
            $packageIdentity = [ordered]@{ valid = $false; error = "$_" }
        }
    }
    $summary = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-test-cycle'
        taskId = $TaskId
        result = if ($cycleOk) { 'pass' } else { 'fail' }
        failedStep = $failedStep
        contentPath = $contentFull
        acceptanceIso = $preparedAcceptanceIso
        guestLaunchDelaySeconds = $GuestLaunchDelaySeconds
        childName = $ChildName
        qmpPort = $QmpPort
        host = [ordered]@{
            retroHost = $hostIdentity
            core = $coreIdentity
        }
        package = $packageIdentity
        runs = @($results)
    }
    [IO.File]::WriteAllText((Join-Path $evidence 'cycle-summary.json'),
        (($summary | ConvertTo-Json -Depth 12) + [Environment]::NewLine))
}
[IO.File]::WriteAllText((Join-Path $evidence 'command.txt'),
    (($commandLog -join [Environment]::NewLine) + [Environment]::NewLine))

Write-Output "TestCycle taskId=$TaskId result=$(if ($cycleOk) { 'pass' } else { 'fail' }) modes=$($Modes -join ',') evidence=$evidence"
if (-not $cycleOk) { exit 1 }
