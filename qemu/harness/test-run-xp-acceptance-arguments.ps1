$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'd3d9-multistream-order.ps1')
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')

function Assert-AcceptanceArgument {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$runner = Join-Path $PSScriptRoot 'run-xp-acceptance.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $runner, [ref]$tokens, [ref]$parseErrors)
Assert-AcceptanceArgument ($parseErrors.Count -eq 0) `
    "run-xp-acceptance.ps1 did not parse: $($parseErrors -join '; ')"
$runnerText = Get-Content -Raw -LiteralPath $runner
$observerText = Get-Content -Raw -LiteralPath `
    (Join-Path $PSScriptRoot '..\..\guest\acceptance\qxpgameobserve.c')
$visualHelperText = Get-Content -Raw -LiteralPath `
    (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
Assert-AcceptanceArgument `
    ($runnerText.Contains("'Warcraft3D3D8'") -and
     $runnerText.Contains("'XPD_FRAME_RING_SLOTS', '256'") -and
     $runnerText.Contains("'XPD_FRAME_RING_INTERVAL_US', '0'") -and
     $runnerText.Contains('Enter-XpWarcraft3D3D8RingSnapshot') -and
     $runnerText.Contains('New-Warcraft3D3D8RingVideo') -and
     $runnerText.Contains('--max-interior-black-runs 0') -and
     $runnerText.Contains('--min-visible-frames 192') -and
     $runnerText.Contains('Test-XpWarcraft3D3D8GuestContract') -and
     $runnerText.Contains('Add-XpWarcraft3D3D8UniqueMessage') -and
     $runnerText.Contains('$warcraft3D3D8ResultDrainUntil') -and
     $runnerText.Contains('$warcraft3D3D8GuestContractOk') -and
     ([regex]::Matches($runnerText,
        '-CaptureImages @\(\$captureImages\)').Count -eq 2) -and
     $runnerText.Contains('-count_frames') -and
     $runnerText.Contains('@(''w'', ''a'', ''r'', ''c'', ''r'', ''a'', ''f'', ''t'', ''3'',') -and
     $runnerText.Contains("'d', '3', 'd', '8')")) `
    'Warcraft D3D8 guest spelling or bounded frame-ring contract is missing'
Assert-AcceptanceArgument `
    ($runnerText.Contains('[string]$frozenInputs.retroHost.path') -and
     $runnerText.Contains('[string]$frozenInputs.core.path') -and
     $runnerText.Contains('Assert-XpFrozenExecutionInputs')) `
    'omitted standalone host/core paths are not derived from frozen inputs'
Assert-AcceptanceArgument `
    ($observerText.Contains('error=process-handoff-during-oracle') -and
     $observerText.Contains('oracle-warmup-reset=1') -and
     $observerText.Contains('warcraft_d3d8_oracle && measurement_started')) `
    'Warcraft D3D8 process handoff can inherit oracle qualification'

$parameterNames = @($ast.ParamBlock.Parameters | ForEach-Object {
    $_.Name.VariablePath.UserPath
})
foreach ($parameterName in @(
        'GamePerformance', 'D3dGameplay', 'D3dNormalLaunch',
        'D3d9Multistream7', 'ObserveGame', 'ObservePerformance', 'RecordVideo',
        'RunId', 'ProgressIntervalSeconds', 'GuestLaunchDelaySeconds',
        'PreparedAcceptanceIsoPath', 'PreparedAcceptanceIsoSha256',
        'FrozenInputsPath', 'PackageDirectory',
        'ArtifactDirectory', 'TaskId', 'Tier', 'Diagnostic')) {
    Assert-AcceptanceArgument ($parameterNames -contains $parameterName) `
        "missing acceptance parameter -$parameterName"
}

$canonicalEvents = @(
    'Ready', 'Draw', 'Present', 'SceneCaptureAck', 'Release',
    'Lifetime', 'Reset', 'ResetCaptureAck', 'Result'
)
$multistreamStage = 0
foreach ($event in $canonicalEvents) {
    $transition = Update-XpD3d9MultistreamOrder `
        -CurrentStage $multistreamStage -Event $event
    Assert-AcceptanceArgument $transition.ok `
        "canonical multistream event '$event' was rejected"
    $multistreamStage = $transition.stage
}
Assert-AcceptanceArgument ($multistreamStage -eq 9) `
    'canonical multistream marker sequence did not complete'

$transition = Update-XpD3d9MultistreamOrder -CurrentStage 0 -Event Ready
$transition = Update-XpD3d9MultistreamOrder `
    -CurrentStage $transition.stage -Event Draw
$reordered = Update-XpD3d9MultistreamOrder `
    -CurrentStage $transition.stage -Event Release
Assert-AcceptanceArgument (-not $reordered.ok -and
        $reordered.stage -eq 2 -and $reordered.expected -ceq 'Present') `
    'reordered multistream release was not rejected'

$transition = Update-XpD3d9MultistreamOrder -CurrentStage 0 -Event Ready
$duplicate = Update-XpD3d9MultistreamOrder `
    -CurrentStage $transition.stage -Event Ready
Assert-AcceptanceArgument (-not $duplicate.ok -and
        $duplicate.stage -eq 1 -and $duplicate.expected -ceq 'Draw') `
    'duplicate multistream marker was not rejected'

function Assert-RejectedArguments {
    param(
        [string[]]$Arguments,
        [string]$ExpectedMessage
    )

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & (Join-Path $PSHOME 'powershell.exe') -NoLogo -NoProfile `
            -NonInteractive -ExecutionPolicy Bypass -File $runner `
            @Arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    Assert-AcceptanceArgument ($exitCode -ne 0) `
        "invalid arguments unexpectedly succeeded: $($Arguments -join ' ')"
    Assert-AcceptanceArgument ($output.Contains($ExpectedMessage)) `
        "invalid arguments did not report '$ExpectedMessage': $output"
}

Assert-RejectedArguments -Arguments @('-ObservePerformance') `
    -ExpectedMessage `
        '-ObservePerformance is a capture-free modifier for -ObserveGame'
Assert-RejectedArguments -Arguments @('-D3dGameplay') `
    -ExpectedMessage `
        '-D3dGameplay is a deterministic tutorial-scene modifier for -D3dGame'
Assert-RejectedArguments -Arguments @('-D3dNormalLaunch') `
    -ExpectedMessage '-D3dNormalLaunch is a no-skipstartup modifier for -D3dGame'
Assert-RejectedArguments -Arguments @(
        '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
        '-D3dGame', '-D3dNormalLaunch', '-D3dGameplay') `
    -ExpectedMessage `
        '-D3dNormalLaunch is incompatible with -D3dGameplay and -D3dPerformance'
Assert-RejectedArguments -Arguments @(
        '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
        '-D3dGame', '-D3dNormalLaunch', '-D3dPerformance') `
    -ExpectedMessage `
        '-D3dNormalLaunch is incompatible with -D3dGameplay and -D3dPerformance'
Assert-RejectedArguments -Arguments @('-GamePerformance') `
    -ExpectedMessage `
        '-GamePerformance is a capture-free modifier for -Game'
Assert-RejectedArguments -Arguments @('-D3d9Multistream7') `
    -ExpectedMessage `
        '-D3d9Multistream7 is a focused modifier for -D3d9'

$gamePerformancePrerequisites = @(
    '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
    '-Game', '-GamePerformance'
)
Assert-RejectedArguments `
    -Arguments ($gamePerformancePrerequisites + '-RecordVideo') `
    -ExpectedMessage '-GamePerformance is incompatible with -RecordVideo'
Assert-RejectedArguments -Arguments @('-RecordVideo') `
    -ExpectedMessage '-RecordVideo currently requires -ObserveGame or -Game'

# Prepared-ISO mount contract: only the single parsed CD-ROM argument may
# name the validated ISO. A mention inside any other token, a missing CD-ROM
# drive, or a second CD-ROM attachment fails closed. Positive profiles run
# past the mount check and stop at the deliberate QMP port mismatch, which
# proves the mount gate accepted them without launching a VM.
$contentContractRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-content-contract-$PID"
[void](New-Item -ItemType Directory -Force -Path $contentContractRoot)
try {
    $contractHost = Join-Path $contentContractRoot 'retro_host.exe'
    $contractCore = Join-Path $contentContractRoot 'libqemu_libretro.dll'
    $contractIso = Join-Path $contentContractRoot 'prepared-acceptance.iso'
    $contractIsoSpacedParent = Join-Path $contentContractRoot 'prepared iso'
    [void](New-Item -ItemType Directory -Force -Path $contractIsoSpacedParent)
    $contractIsoSpaced = Join-Path $contractIsoSpacedParent `
        'prepared-acceptance.iso'
    [IO.File]::WriteAllText($contractHost, 'contract host')
    [IO.File]::WriteAllText($contractCore, 'contract core')
    [IO.File]::WriteAllText($contractIso, 'contract iso bytes')
    [IO.File]::WriteAllText($contractIsoSpaced, 'contract iso bytes')
    $contractIsoSha256 = (Get-FileHash -LiteralPath $contractIso `
        -Algorithm SHA256).Hash
    $contractIsoArgument = $contractIso.Replace('\', '/')
    $contractIsoSpacedArgument = $contractIsoSpaced.Replace('\', '/')
    $otherIsoArgument = ($contentContractRoot + '\other.iso').Replace('\', '/')

    function Assert-ContentContract {
        param(
            [string]$Label,
            [string]$ContentText,
            [string]$IsoPath,
            [string[]]$ExtraArguments,
            [string]$ExpectedMessage,
            [string]$ForbiddenMessage
        )

        $contentFile = Join-Path $contentContractRoot "$Label.qemu_cmd_line"
        [IO.File]::WriteAllText($contentFile, $ContentText)
        $savedErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $output = & (Join-Path $PSHOME 'powershell.exe') -NoLogo `
                -NoProfile -NonInteractive -ExecutionPolicy Bypass `
                -File $runner `
                -ContentPath $contentFile `
                -PreparedAcceptanceIsoPath $IsoPath `
                -PreparedAcceptanceIsoSha256 $contractIsoSha256 `
                -RetroHostPath $contractHost `
                -CorePath $contractCore `
                @ExtraArguments 2>&1 | Out-String -Width 4096
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedErrorActionPreference
        }
        Assert-AcceptanceArgument ($exitCode -ne 0) `
            "content contract '$Label' unexpectedly succeeded: $output"
        Assert-AcceptanceArgument ($output.Contains($ExpectedMessage)) `
            "content contract '$Label' did not report '$ExpectedMessage': $output"
        if ($ForbiddenMessage) {
            Assert-AcceptanceArgument `
                (-not $output.Contains($ForbiddenMessage)) `
                "content contract '$Label' reported '$ForbiddenMessage': $output"
        }
    }

    $qmpMismatchArguments = @('-QmpPort', '45679')
    $qmpMismatchMessage = `
        '-QmpPort 45679 does not match the content QMP port 45671'
    $mountRejectMessage = 'does not mount the validated acceptance ISO'
    $countRejectMessage = 'exactly one CD-ROM argument'

    Assert-ContentContract -Label 'drive-unquoted' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-qmp tcp:127.0.0.1:45671,server,nowait " +
            "-drive file=$contractIsoArgument,format=raw,media=cdrom,index=2,id=acceptcd") `
        -ExtraArguments $qmpMismatchArguments `
        -ExpectedMessage $qmpMismatchMessage `
        -ForbiddenMessage $mountRejectMessage
    Assert-ContentContract -Label 'drive-quoted-spaced' `
        -IsoPath $contractIsoSpaced `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-qmp tcp:127.0.0.1:45671,server,nowait " +
            "-drive `"file=$contractIsoSpacedArgument,format=raw,media=cdrom`"") `
        -ExtraArguments $qmpMismatchArguments `
        -ExpectedMessage $qmpMismatchMessage `
        -ForbiddenMessage $mountRejectMessage
    Assert-ContentContract -Label 'cdrom-legacy-unquoted' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-qmp tcp:127.0.0.1:45671,server,nowait " +
            "-cdrom $($contractIsoArgument.ToUpperInvariant())") `
        -ExtraArguments $qmpMismatchArguments `
        -ExpectedMessage $qmpMismatchMessage `
        -ForbiddenMessage $mountRejectMessage
    Assert-ContentContract -Label 'cdrom-legacy-quoted-spaced' `
        -IsoPath $contractIsoSpaced `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-qmp tcp:127.0.0.1:45671,server,nowait " +
            "-cdrom `"$contractIsoSpacedArgument`"") `
        -ExtraArguments $qmpMismatchArguments `
        -ExpectedMessage $qmpMismatchMessage `
        -ForbiddenMessage $mountRejectMessage

    Assert-ContentContract -Label 'mention-only' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-D $contractIsoArgument") `
        -ExtraArguments @() `
        -ExpectedMessage $countRejectMessage `
        -ForbiddenMessage $null
    Assert-ContentContract -Label 'mounts-another-mentions-approved' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-drive file=$otherIsoArgument,format=raw,media=cdrom,index=2,id=acceptcd " +
            "-D $contractIsoArgument") `
        -ExtraArguments @() `
        -ExpectedMessage $mountRejectMessage `
        -ForbiddenMessage $null
    Assert-ContentContract -Label 'two-cdrom-arguments' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-cdrom `"$contractIsoArgument`" " +
            "-drive file=$otherIsoArgument,media=cdrom") `
        -ExtraArguments @() `
        -ExpectedMessage $countRejectMessage `
        -ForbiddenMessage $null
    Assert-ContentContract -Label 'quoted-decoy-mention' `
        -IsoPath $contractIso `
        -ContentText ("qemu-system-x86_64 -hda disk.qcow2 -nic none " +
            "-name `"quoted -drive file=$contractIsoArgument,media=cdrom`" " +
            "-drive file=$otherIsoArgument,media=cdrom,format=raw") `
        -ExtraArguments @() `
        -ExpectedMessage $mountRejectMessage `
        -ForbiddenMessage $null
} finally {
    Remove-Item -LiteralPath $contentContractRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}

$workspaceForFrozenMutation = (Resolve-Path (
    Join-Path $PSScriptRoot '..\..')).Path
$frozenManifestPath = Join-Path $workspaceForFrozenMutation `
    'guest\acceptance\frozen-inputs-ut-oracle-r5.json'
if (Test-Path -LiteralPath $frozenManifestPath -PathType Leaf) {
    $frozenManifest = Get-Content -Raw -LiteralPath $frozenManifestPath |
        ConvertFrom-Json
    $executionMutationRoot = Join-Path ([IO.Path]::GetTempPath()) `
        "xpdriver-execution-input-mutation-$PID"
    [void](New-Item -ItemType Directory -Force -Path $executionMutationRoot)
    try {
        $alternateHost = Join-Path $executionMutationRoot 'retro_host.exe'
        $alternateCore = Join-Path $executionMutationRoot `
            'libqemu_libretro.dll'
        [IO.File]::WriteAllText($alternateHost, 'alternate host')
        [IO.File]::WriteAllText($alternateCore, 'alternate core')

        # This test exercises selected host/core path precedence, not a dated
        # package snapshot. Bind a private manifest to the package bytes that
        # are on disk now so unrelated driver rebuilds cannot mask that error.
        $packageDirectory = Join-Path $workspaceForFrozenMutation `
            'guest\xpdm\build\package'
        $packageIdentity = Get-XpAcceptancePackageIdentity `
            -Directory $packageDirectory
        Assert-AcceptanceArgument $packageIdentity.valid `
            'current package identity is invalid'
        $frozenManifest.package.directory =
            [IO.Path]::GetFullPath($packageDirectory)
        $frozenManifest.package.files = @($packageIdentity.files)
        $frozenManifest.transportAbi = Get-XpPackagedTransportAbi `
            -Directory $packageDirectory
        foreach ($section in @('acceptanceIso', 'retroHost', 'core')) {
            $frozenManifest.$section = Get-XpFrozenFileIdentity `
                -Path ([string]$frozenManifest.$section.path)
        }
        $argumentManifestPath = Join-Path $executionMutationRoot `
            'frozen-inputs.json'
        $frozenManifest | ConvertTo-Json -Depth 10 |
            Set-Content -LiteralPath $argumentManifestPath -Encoding UTF8

        foreach ($mutation in @(
                @('retro host', $alternateHost,
                    [string]$frozenManifest.core.path),
                @('paired core', [string]$frozenManifest.retroHost.path,
                    $alternateCore))) {
            Assert-RejectedArguments -Arguments @(
                    '-Xpdm', '-BootOnly',
                    '-ContentPath', '__argument_validation_only__',
                    '-FrozenInputsPath', $argumentManifestPath,
                    '-RetroHostPath', $mutation[1],
                    '-CorePath', $mutation[2]) `
                -ExpectedMessage `
                    "selected $($mutation[0]) path differs from frozen inputs"
        }
    } finally {
        Remove-Item -LiteralPath $executionMutationRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
Assert-RejectedArguments -Arguments @('-RunId', 'has space') `
    -ExpectedMessage 'Invalid acceptance run ID'
Assert-RejectedArguments -Arguments @(
    '-PreparedAcceptanceIsoSha256', 'not-a-sha256') `
    -ExpectedMessage 'does not match the'

$observePrerequisites = @(
    '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
    '-ObserveGame', 'Warcraft3', '-ObservePerformance'
)
Assert-RejectedArguments -Arguments ($observePrerequisites + '-RecordVideo') `
    -ExpectedMessage '-ObservePerformance is incompatible with -RecordVideo'
Assert-RejectedArguments -Arguments @(
        '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
        '-ObserveGame', 'Warcraft3D3D8') `
    -ExpectedMessage `
        'Warcraft3D3D8 requires -RecordVideo to retain temporal evidence'

# NFSU2 observe modes: behavioral parser/argument coverage. Accepted
# invocations must clear every argument-validation gate and reach execution
# finalization (the sentinel content path can never resolve, so each run
# stops inside the guarded execution block), while invalid combinations stay
# fail-closed on their exact validation messages.
function Assert-AcceptedObserveArguments {
    param(
        [string[]]$Arguments,
        [string]$RunId
    )

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & (Join-Path $PSHOME 'powershell.exe') -NoLogo -NoProfile `
            -NonInteractive -ExecutionPolicy Bypass -File $runner `
            @Arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    Assert-AcceptanceArgument ($exitCode -ne 0) `
        "observe arguments unexpectedly completed: $($Arguments -join ' ')"
    foreach ($rejection in @(
            'Cannot validate argument',
            '-ObserveGame requires',
            '-ObservePerformance is a capture-free modifier for -ObserveGame',
            '-ObservePerformance is incompatible with -RecordVideo',
            '-RecordVideo currently requires -ObserveGame or -Game',
            'mutually exclusive')) {
        Assert-AcceptanceArgument (-not $output.Contains($rejection)) `
            "observe arguments hit validation rejection '$rejection': $output"
    }
    Assert-AcceptanceArgument `
        ($output.Contains("HarnessRun runId=$RunId status=failed")) `
        "observe arguments did not reach execution finalization: $output"
}

$nfsu2ObservePrerequisites = @(
    '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
    '-ObserveGame', 'NFSU2'
)
Assert-AcceptedObserveArguments `
    -Arguments ($nfsu2ObservePrerequisites + @(
        '-RunId', "nfsu2-normal-$PID")) `
    -RunId "nfsu2-normal-$PID"
Assert-AcceptedObserveArguments `
    -Arguments ($nfsu2ObservePrerequisites + @(
        '-RecordVideo', '-RunId', "nfsu2-visual-$PID")) `
    -RunId "nfsu2-visual-$PID"
Assert-AcceptedObserveArguments `
    -Arguments ($nfsu2ObservePrerequisites + @(
        '-ObservePerformance', '-RunId', "nfsu2-performance-$PID")) `
    -RunId "nfsu2-performance-$PID"
foreach ($performanceGame in @(
        'UT2003Gameplay', 'Warcraft3D3D8')) {
    $performanceRunId =
        "$($performanceGame.ToLowerInvariant())-performance-$PID"
    Assert-AcceptedObserveArguments -Arguments @(
            '-Xpdm', '-BootOnly', '-ContentPath',
            '__argument_validation_only__', '-ObserveGame',
            $performanceGame, '-ObservePerformance', '-RunId',
            $performanceRunId) -RunId $performanceRunId
}
Assert-RejectedArguments -Arguments ($nfsu2ObservePrerequisites + @(
        '-ObservePerformance', '-RecordVideo')) `
    -ExpectedMessage '-ObservePerformance is incompatible with -RecordVideo'
Assert-RejectedArguments -Arguments @(
        '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
        '-Game', '-ObserveGame', 'NFSU2') `
    -ExpectedMessage `
        '-Game, Direct3D modes, and -ObserveGame are mutually exclusive'
Assert-RejectedArguments -Arguments @(
        '-Xpdm', '-BootOnly', '-ContentPath', '__argument_validation_only__',
        '-ObserveGame', 'NFSU2Cycle') `
    -ExpectedMessage 'The argument "NFSU2Cycle" does not belong to the set'

# Execute the runner's real guest-launch key construction, lifted from the
# parsed AST, against a fake QMP endpoint. Each accepted NFSU2 mode must
# type qxpgameobserve.exe plus exactly the contracted guest argument.
$guestLaunchAst = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Invoke-GuestLaunch'
    }, $true)
Assert-AcceptanceArgument ($null -ne $guestLaunchAst) `
    'Invoke-GuestLaunch is missing from the acceptance runner'
. ([scriptblock]::Create($guestLaunchAst.Extent.Text))

function ConvertTo-GuestProgramText {
    param([string[]]$Keys)

    $text = [Text.StringBuilder]::new()
    foreach ($key in $Keys) {
        switch ($key) {
            'dot' { [void]$text.Append('.') }
            'spc' { [void]$text.Append(' ') }
            'minus' { [void]$text.Append('-') }
            default {
                Assert-AcceptanceArgument ($key.Length -eq 1) `
                    "unexpected guest program key '$key'"
                [void]$text.Append($key)
            }
        }
    }
    $text.ToString()
}

function Capture-ObserveGuestCommand {
    param(
        [bool]$Video,
        [bool]$Performance
    )

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
        $accept = $listener.AcceptTcpClientAsync()
        $launched = Invoke-GuestLaunch -Port $port
        Assert-AcceptanceArgument $launched `
            "guest launch failed (video=$Video performance=$Performance)"
        $client = $accept.GetAwaiter().GetResult()
        try {
            $reader = [IO.StreamReader]::new($client.GetStream())
            $sendKeys = @()
            while (($line = $reader.ReadLine()) -ne $null) {
                if ($line.Contains('sendkey')) {
                    $commandLine = [string]($line | ConvertFrom-Json).arguments.'command-line'
                    $sendKeys += $commandLine.Substring('sendkey '.Length)
                }
            }
        } finally {
            $client.Dispose()
        }
    } finally {
        $listener.Stop()
    }

    $expectedPrefix = @(
        'esc', 'ctrl-esc', 'r', 'ctrl-a', 'backspace',
        'd', 'shift-semicolon', 'backslash',
        'x', 'p', 'd', 'm', 'backslash')
    Assert-AcceptanceArgument `
        ($sendKeys.Count -gt $expectedPrefix.Count + 1 -and
         $sendKeys[$sendKeys.Count - 1] -eq 'ret' -and
         ((@($sendKeys[0..($expectedPrefix.Count - 1)]) -join ',') -ceq
          ($expectedPrefix -join ','))) `
        "guest command did not open the XPDM directory first: $($sendKeys -join ' ')"
    ConvertTo-GuestProgramText `
        -Keys @($sendKeys[$expectedPrefix.Count..($sendKeys.Count - 2)])
}

$GameObserve = $true
$ObserveGame = 'NFSU2'
$Xpdm = $true
$Warcraft3D3D8Oracle = $false
try {
    foreach ($nfsu2Mode in @(
            @($false, $false, 'qxpgameobserve.exe nfsu2'),
            @($true, $false, 'qxpgameobserve.exe nfsu2visual'),
            @($false, $true, 'qxpgameobserve.exe nfsu2performance'))) {
        $RecordVideo = $nfsu2Mode[0]
        $ObservePerformance = $nfsu2Mode[1]
        $typedCommand = Capture-ObserveGuestCommand `
            -Video $nfsu2Mode[0] -Performance $nfsu2Mode[1]
        Assert-AcceptanceArgument ($typedCommand -ceq $nfsu2Mode[2]) `
            "NFSU2 guest command typed '$typedCommand', expected '$($nfsu2Mode[2])'"
    }
    foreach ($performanceMode in @(
            @('UT2003Gameplay',
              'qxpgameobserve.exe ut2003gameplayperformance'),
            @('Warcraft3D3D8',
              'qxpgameobserve.exe warcraft3d3d8performance'))) {
        $ObserveGame = $performanceMode[0]
        $RecordVideo = $false
        $ObservePerformance = $true
        $typedCommand = Capture-ObserveGuestCommand `
            -Video $false -Performance $true
        Assert-AcceptanceArgument `
            ($typedCommand -ceq $performanceMode[1]) `
            "$($performanceMode[0]) guest command typed '$typedCommand', expected '$($performanceMode[1])'"
    }
} finally {
    Remove-Variable -Name GameObserve, ObserveGame, Xpdm, RecordVideo, `
        ObservePerformance, Warcraft3D3D8Oracle -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath Function:\Invoke-GuestLaunch `
        -ErrorAction SilentlyContinue
}

# Force a terminating error after lease acquisition but before ISO refresh or
# host launch. The runner must publish a failed final record and release the
# artifact lease even though its ordinary result path was never reached.
$exceptionRunId = "contract-exception-$PID"
$missingHost = Join-Path $PSScriptRoot `
    "__missing-retro-host-$([Guid]::NewGuid().ToString('N')).exe"
$savedErrorActionPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $exceptionOutput = & (Join-Path $PSHOME 'powershell.exe') `
        -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass `
        -File $runner -RunId $exceptionRunId `
        -RetroHostPath $missingHost 2>&1 | Out-String
    $exceptionExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $savedErrorActionPreference
}
Assert-AcceptanceArgument ($exceptionExitCode -ne 0) `
    'post-acquisition exception unexpectedly succeeded'
Assert-AcceptanceArgument `
    ($exceptionOutput.Contains("HarnessRun runId=$exceptionRunId status=failed") -and
     $exceptionOutput.Contains('cleanupHostStopped=True')) `
    "post-acquisition exception did not emit a failed final record: $exceptionOutput"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$leasePath = Join-Path $workspace `
    'qemu\winxp\.xpdriver-acceptance-artifacts.lock'
$exceptionLeaseText = Get-Content -Raw -LiteralPath $leasePath
$exceptionLease = $exceptionLeaseText | ConvertFrom-Json
Assert-AcceptanceArgument ($exceptionLease.runId -eq $exceptionRunId) `
    'exception lease run ID is wrong'
Assert-AcceptanceArgument ($exceptionLease.status -eq 'failed') `
    'exception lease did not persist failed status'
Assert-AcceptanceArgument ($exceptionLease.cleanupHostStopped -eq $true) `
    'exception lease did not persist successful host cleanup'
Assert-AcceptanceArgument `
    (-not [string]::IsNullOrWhiteSpace($exceptionLease.failureType) -and
     -not [string]::IsNullOrWhiteSpace($exceptionLease.failureMessage)) `
    'exception lease did not persist failure identity'
Write-Output 'AcceptanceExceptionChildOutputBegin'
Write-Output $exceptionOutput.TrimEnd()
Write-Output 'AcceptanceExceptionChildOutputEnd'

# The failed JSON is evidence and must remain intact. Probe the exact lease
# byte from this parent PowerShell process without calling Enter, which would
# replace the child's metadata with a new owner record.
$leaseProbe = $null
$leaseProbeAcquired = $false
$leaseProbeError = $null
try {
    $leaseProbe = [IO.File]::Open(
        $leasePath, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    $leaseProbe.Lock($script:XpAcceptanceLeaseOffset, 1)
    $leaseProbeAcquired = $true
} catch {
    $leaseProbeError = $_.Exception.Message
} finally {
    if ($leaseProbe) {
        if ($leaseProbeAcquired) {
            $leaseProbe.Unlock($script:XpAcceptanceLeaseOffset, 1)
        }
        $leaseProbe.Dispose()
    }
}
Assert-AcceptanceArgument $leaseProbeAcquired `
    "exception lease byte could not be reacquired by the parent process: $leaseProbeError"
$exceptionLeaseAfterProbe = Get-Content -Raw -LiteralPath $leasePath
Assert-AcceptanceArgument `
    ($exceptionLeaseAfterProbe -eq $exceptionLeaseText) `
    'lease reacquisition changed the failed owner metadata'
Write-Output "AcceptanceExceptionFinalization=passed runId=$exceptionRunId childExitCode=$exceptionExitCode status=$($exceptionLease.status) cleanupHostStopped=$($exceptionLease.cleanupHostStopped) failureType=$($exceptionLease.failureType) leaseReacquired=$leaseProbeAcquired metadataPreserved=True"

$runnerText = Get-Content -Raw -LiteralPath $runner
$artifactHelperText = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot `
    'acceptance-run-artifacts.ps1')
foreach ($runContractMarker in @(
        'HarnessConfig runId=', 'HarnessProgress runId=',
        'HarnessRun runId=', 'ArtifactBundle runId=',
        'Enter-XpAcceptanceArtifactLease')) {
    Assert-AcceptanceArgument ($runnerText.Contains($runContractMarker)) `
        "acceptance runner is missing run-contract marker '$runContractMarker'"
}
Assert-AcceptanceArgument `
    ($runnerText.Contains('stage=measurement-start') -and
     $runnerText.Contains('stage=measurement-complete')) `
    'capture-free frame profiling is not routed through warm-up markers'
Assert-AcceptanceArgument `
    ($runnerText.Contains('$gameProfileTextStart = -1') -and
     $runnerText.Contains('GameObserveHostProfileBoundary characters=') -and
     $runnerText.Contains('-StartIndex $gameProfileTextStart')) `
    'game doorbell profile is not bounded to post-launch host output'
Assert-AcceptanceArgument `
    (([regex]::Matches($runnerText,
        [regex]::Escape('$captureImages += $reviewVideo.path')).Count -eq 2) -and
     -not $runnerText.Contains('$captureImages += $gameObserveVideo') -and
     $runnerText.Contains('Test-XpAcceptanceReviewVideoRecords') -and
     $runnerText.Contains('$reviewVideoSources.Count -eq 1') -and
     $runnerText.Contains('GameObserveArtifactNamespace stem=') -and
     $runnerText.Contains('Get-XpAcceptanceGameObserveCapturePaths') -and
     $artifactHelperText.Contains('-review-[0-9a-f]{{12}}\.mp4') -and
     $artifactHelperText.Contains(
        'Test-XpAcceptanceGameObserveCaptureNames') -and
     $runnerText.Contains('$reviewVideoArtifactOk -and') -and
     $runnerText.Contains('$gameObserveArtifactSetOk -and') -and
     $runnerText.Contains(
        '$gameObserveArtifactSetRequired = $GameObserve -and $RecordVideo -and')) `
    'digest-renamed review video is not reconstructed and fail-closed in artifacts'
Assert-AcceptanceArgument `
    ($runnerText.Contains('AcceptanceIso prepared=') -and
     $runnerText.Contains('acceptanceIsoSha256') -and
     $runnerText.Contains('bootToFirstLaunchSeconds')) `
    'prepared-ISO attribution or phase timing is missing'
Assert-AcceptanceArgument `
    ($runnerText.Contains("elseif (`$RecordVideo)") -and
     $runnerText.Contains("`$keys += @('v', 'i', 's', 'u', 'a', 'l')") -and
     $runnerText.Contains(
        '$gameObserveVisualOk = $representativeScore -ge 0 -and') -and
     $runnerText.Contains('Get-XpGameObserveBitmapAssessment') -and
     $runnerText.Contains('$geometryGroups[0].frames') -and
     $runnerText.Contains('desktop=$($assessment.desktop)') -and
     $runnerText.Contains('Test-XpUT2003VisualGuestContract') -and
     $runnerText.Contains('$ut2003VisualReadyMessages') -and
     $runnerText.Contains('$ut2003VisualStageMessages') -and
     $runnerText.Contains('$ut2003VisualResultMessages') -and
     $runnerText.Contains('$ut2003VisualGuestContractOk') -and
     $runnerText.Contains('$ut2003VisualCoverageOk') -and
     $visualHelperText.Contains('Test-XpUT2003VisualFrameCoverage') -and
     $visualHelperText.Contains('Test-XpUT2003MainMenuCoverage') -and
     $visualHelperText.Contains('AnalyzeUT2003Menu') -and
     $runnerText.Contains('$ut2003MenuVisualOk -and') -and
     $visualHelperText.Contains(
        '^QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-ready') -and
     $visualHelperText.Contains(
        '^QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-complete') -and
     $visualHelperText.Contains(
        '^QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003') -and
     $runnerText.Contains('UT2003Semantic valid=') -and
     $runnerText.Contains('UT2003VisualTiming valid=') -and
     $runnerText.Contains('UT2003VisualCoverage valid=') -and
     $runnerText.Contains('UT2003MenuVisual valid=') -and
     $visualHelperText.Contains('$minimumChangedPercent = 0.05') -and
     -not $runnerText.Contains('UT2003TexgenHost valid=')) `
    'UT2003 video is not routed through the extended fail-closed visual gate'
$observer = Join-Path $PSScriptRoot `
    '..\..\guest\acceptance\qxpgameobserve.c'
$observerText = Get-Content -Raw -LiteralPath $observer
foreach ($performanceMode in @(
        'ut2003performance', 'ut2003gameplayperformance',
        'warcraft3performance', 'warcraft3d3d8performance')) {
    Assert-AcceptanceArgument ($observerText.Contains($performanceMode)) `
        "capture-free observer mode is missing '$performanceMode'"
}
Assert-AcceptanceArgument `
    ($observerText.Contains('ut2003visual') -and
     -not $observerText.Contains('ut2003gameplayvisual') -and
     $observerText.Contains('prepare_fresh_log') -and
     -not $observerText.Contains('read_ut_main_menu_semantic') -and
     $observerText.Contains('stage(config.mode, "visual-ready"') -and
     $observerText.Contains(
        'boundary=foreground-%s-window-stable') -and
     $observerText.Contains('audio=default visual=extended') -and
     $observerText.Contains('launch=normal') -and
     -not $observerText.Contains('QXP_UT2003_VISUAL_TIMEOUT_MS') -and
     $observerText.Contains('qxp_ut2003_visual_should_stop') -and
     $observerText.Contains('elapsed >= 30000UL') -and
     $observerText.Contains('for (;;)') -and
     $observerText.Contains('visual_observation && !exited') -and
     $observerText.Contains('error=observation-incomplete') -and
     $observerText.Contains('stage(config.mode, "visual-complete"') -and
     $observerText.Contains(
        'error=foreground-system32-%s-window-not-stable') -and
     $observerText.Contains('error=process-handoff-not-allowed') -and
     $observerText.Contains(
        'child_process_id && visual_observation && !ut_gameplay') -and
     $observerText.Contains('error=system32-%s-not-ready') -and
     $observerText.Contains('boundary=post-visual-ready') -and
     $runnerText.Contains(
        'excludes NvidiaLogo or other rich splash frames') -and
     $runnerText.Contains('$gameObserveStartFrame =') -and
     $runnerText.Contains('Get-LatestDumpFrameSequence') -and
     $runnerText.Contains('$ut2003GuardFrame =') -and
     $runnerText.Contains(
        '[Math]::Max(0, $ut2003VisualReadyFrame) + 2') -and
     $runnerText.Contains('forceSyncReadback=True') -and
     $runnerText.Contains('Test-XpGameObserveFrameInterval') -and
     $runnerText.Contains(
        'guest\acceptance\frozen-inputs-ut-oracle-r5.json') -and
     $visualHelperText.Contains('CanonicalRgbSha256') -and
     $visualHelperText.Contains('requiredMeaningfulTransitions') -and
     $visualHelperText.Contains('manifest-bound-menu-layout')) `
    'semantic UT2003 normal-menu visual observer contract is incomplete'

Write-Output 'acceptance argument parser tests passed'
