$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'maxpayne-visual.ps1')

function Assert-MaxPayneVisual {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function New-MaxPayneVisualFrame {
    param(
        [string]$Path,
        [int]$Seed,
        [DateTime]$CapturedUtc,
        [switch]$Desktop
    )

    Add-Type -AssemblyName System.Drawing
    $bitmap = [Drawing.Bitmap]::new(640, 480)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            for ($y = 0; $y -lt 12; ++$y) {
                for ($x = 0; $x -lt 16; ++$x) {
                    $red = 40 + (($Seed * 17 + $x * 11 + $y * 3) % 190)
                    $green = 35 + (($Seed * 7 + $x * 5 + $y * 13) % 190)
                    $blue = 30 + (($Seed * 3 + $x * 17 + $y * 7) % 190)
                    $brush = [Drawing.SolidBrush]::new(
                        [Drawing.Color]::FromArgb($red, $green, $blue))
                    try {
                        $graphics.FillRectangle($brush,
                            $x * 40, $y * 40, 40, 40)
                    } finally {
                        $brush.Dispose()
                    }
                }
            }
        } finally {
            $graphics.Dispose()
        }
        foreach ($probe in @(
                @(25, 465), @(300, 470), @(25, 14), @(300, 9))) {
            $bitmap.SetPixel($probe[0], $probe[1],
                [Drawing.Color]::FromArgb(90, 90, 90))
        }
        if ($Desktop) {
            $bitmap.SetPixel(25, 465,
                [Drawing.Color]::FromArgb(10, 180, 10))
            $bitmap.SetPixel(300, 470,
                [Drawing.Color]::FromArgb(20, 20, 240))
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Bmp)
    } finally {
        $bitmap.Dispose()
    }
    (Get-Item -LiteralPath $Path).LastWriteTimeUtc = $CapturedUtc
}

function Write-MaxPayneJson {
    param([string]$Path, [psobject]$Value)

    [IO.File]::WriteAllText($Path,
        (($Value | ConvertTo-Json -Depth 20) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
}

$boundary = New-XpMaxPayneVisualBoundaryState
$firstStart = [DateTime]::Parse('2026-07-18T11:59:00Z')
Assert-MaxPayneVisual `
    (-not (Set-XpMaxPayneVisualCompletionBoundary `
        -State $boundary -At $firstStart -Sequence 9)) `
    'completion before tutorial start was accepted'
Assert-MaxPayneVisual `
    (-not $boundary.completed -and
     $boundary.completedAt -eq [DateTime]::MaxValue -and
     $boundary.endSequence -eq -1) `
    'refused pre-start completion mutated the boundary state'
Assert-MaxPayneVisual `
    (Set-XpMaxPayneVisualStartBoundary `
        -State $boundary -At $firstStart -Sequence 10) `
    'first visual start boundary was not accepted'
Assert-MaxPayneVisual `
    (-not (Set-XpMaxPayneVisualStartBoundary `
        -State $boundary -At $firstStart.AddSeconds(1) -Sequence 11)) `
    'duplicate visual start boundary was accepted'
Assert-MaxPayneVisual `
    ($boundary.startedAt -eq $firstStart -and
     $boundary.startSequence -eq 10) `
    'duplicate start delivery changed the latched boundary'
$firstCompletion = $firstStart.AddSeconds(45)
Assert-MaxPayneVisual `
    (Set-XpMaxPayneVisualCompletionBoundary `
        -State $boundary -At $firstCompletion -Sequence 20) `
    'first visual completion boundary was not accepted'
Assert-MaxPayneVisual `
    (-not (Set-XpMaxPayneVisualCompletionBoundary `
        -State $boundary -At $firstCompletion.AddSeconds(1) -Sequence 21)) `
    'duplicate visual completion boundary was accepted'
Assert-MaxPayneVisual `
    ($boundary.completedAt -eq $firstCompletion -and
     $boundary.endSequence -eq 20) `
    'duplicate completion delivery changed the latched boundary'

$resultBoundary = New-XpMaxPayneVisualBoundaryState
[void](Set-XpMaxPayneVisualStartBoundary `
    -State $resultBoundary -At $firstStart -Sequence 30)
$failedResult =
    'QXP_D3D_GAME_RESULT rc=9 stage=runtime error=no-system-d3d8'
if (Test-XpMaxPayneSuccessfulResult -Message $failedResult) {
    [void](Set-XpMaxPayneVisualCompletionBoundary `
        -State $resultBoundary -At $firstCompletion -Sequence 40)
}
Assert-MaxPayneVisual (-not $resultBoundary.completed) `
    'failed post-start guest result latched visual completion'
foreach ($malformedSuccess in @(
        'QXP_D3D_GAME_RESULT rc=00 stage=complete',
        'prefix QXP_D3D_GAME_RESULT rc=0 stage=complete',
        'QXP_D3D_GAME_RESULT rc=01 stage=complete')) {
    Assert-MaxPayneVisual `
        (-not (Test-XpMaxPayneSuccessfulResult `
            -Message $malformedSuccess)) `
        "non-exact success result was accepted: $malformedSuccess"
}
$successfulResult =
    'QXP_D3D_GAME_RESULT rc=0 stage=complete runtime=system32'
Assert-MaxPayneVisual `
    (Test-XpMaxPayneSuccessfulResult -Message $successfulResult) `
    'exact successful Max Payne result was rejected'
if (Test-XpMaxPayneSuccessfulResult -Message $successfulResult) {
    [void](Set-XpMaxPayneVisualCompletionBoundary `
        -State $resultBoundary -At $firstCompletion -Sequence 40)
}
Assert-MaxPayneVisual `
    ($resultBoundary.completed -and $resultBoundary.endSequence -eq 40) `
    'successful post-start result did not latch fallback completion'

$runnerText = Get-Content -Raw -LiteralPath `
    (Join-Path $PSScriptRoot 'run-xp-acceptance.ps1')
Assert-MaxPayneVisual `
    ($runnerText -match
        'if \(@\(\$captureImages\)\.Count\)\s*\{\s*Remove-Item') `
    'runner does not guard empty capture-image cleanup'
Assert-MaxPayneVisual `
    ($runnerText.Contains(
        'Test-XpMaxPayneSuccessfulResult -Message $message') -and
     $runnerText.Contains('$maxPayneVisualBoundary.started -and') -and
     $runnerText.Contains('-not $maxPayneVisualBoundary.completed')) `
    'failed guest result can still synthesize a tutorial completion boundary'
$maxPayneGuest = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot `
    '..\..\guest\acceptance\qxpd3dgame.c')
Assert-MaxPayneVisual `
    ($maxPayneGuest.Contains('source=live-stub') -and
     $maxPayneGuest.Contains('retained_process_id') -and
     $maxPayneGuest.Contains('close_game_pair(active_process, window, retained_process)')) `
    'live launcher-child adoption or paired teardown is incomplete'

$root = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-maxpayne-visual-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $root)
try {
    $lineageCompiler = Get-Command 'i686-w64-mingw32-gcc.exe' `
        -ErrorAction SilentlyContinue
    Assert-MaxPayneVisual ($null -ne $lineageCompiler) `
        'MinGW compiler is unavailable for process-lineage mutation tests'
    $lineageSource = Join-Path $PSScriptRoot `
        '..\..\guest\acceptance\qxp_process_lineage_test.c'
    $lineageExe = Join-Path $root 'qxp_process_lineage_test.exe'
    $lineageCompileOutput = & $lineageCompiler.Source `
        '-O2' '-Wall' '-Wextra' '-mconsole' '-static-libgcc' `
        '-o' $lineageExe $lineageSource 2>&1
    $lineageCompileExit = $LASTEXITCODE
    Assert-MaxPayneVisual ($lineageCompileExit -eq 0) `
        "process-lineage test compile failed: $($lineageCompileOutput -join ' ')"
    $lineageOutput = & $lineageExe 2>&1
    $lineageExit = $LASTEXITCODE
    Assert-MaxPayneVisual `
        ($lineageExit -eq 0 -and
         ($lineageOutput -join "`n").Contains(
            'Max Payne process lineage tests passed')) `
        "process-lineage mutations failed: $($lineageOutput -join ' ')"

    $source = Join-Path $root 'source'
    $selected = Join-Path $root 'selected'
    [void](New-Item -ItemType Directory -Path $source)
    [void](New-Item -ItemType Directory -Path $selected)
    $start = [DateTime]::Parse(
        '2026-07-18T12:00:00Z').ToUniversalTime()
    $offsets = @(2500, 7500, 12500, 17500, 22500,
                 27500, 32500, 37500, 42500)
    for ($index = 0; $index -lt $offsets.Count; ++$index) {
        $path = Join-Path $source (
            'frame_640x480_{0:D6}.bmp' -f ($index + 1))
        New-MaxPayneVisualFrame -Path $path -Seed ($index + 1) `
            -CapturedUtc $start.AddMilliseconds($offsets[$index])
    }
    $frames = @(Get-ChildItem -LiteralPath $source -File)
    $selection = Select-XpMaxPayneTutorialVisualSequence `
        -Frames $frames -StartedAt $start `
        -CompletedAt $start.AddSeconds(45) `
        -StartSequenceExclusive 0 -EndSequenceInclusive 10 `
        -OutputDirectory $selected
    Assert-MaxPayneVisual $selection.valid `
        "valid visual sequence failed: $($selection.findings -join '; ')"
    Assert-MaxPayneVisual ($selection.selectedCount -eq 3) `
        'visual sequence did not select exactly three screenshots'
    Assert-MaxPayneVisual `
        ((@($selection.images | ForEach-Object sourceSequence) -join ',') -eq
            '2,5,8') `
        'visual sequence did not choose temporal-third midpoints'
    foreach ($image in @($selection.images)) {
        Assert-MaxPayneVisual `
            (Test-Path -LiteralPath $image.rawPath -PathType Leaf) `
            "raw screenshot is missing: $($image.fileName)"
        Assert-MaxPayneVisual `
            ($image.fileName -match
                '^maxpayne-tutorial-\d{2}-t\d{8}T\d{9}Z-h\d{9}-q\d{6}-[0-9a-f]{12}\.bmp$') `
            "raw screenshot name is not identity-bearing: $($image.fileName)"
    }

    # A guest failure can leave the caller's DateTime sentinels intact. Keep
    # diagnostic frame selection fail-safe instead of throwing when that
    # interval exceeds Int32 milliseconds.
    $sentinelOutput = Join-Path $root 'sentinel'
    [void](New-Item -ItemType Directory -Path $sentinelOutput)
    $sentinelSelection = Select-XpMaxPayneTutorialVisualSequence `
        -Frames $frames -StartedAt ([DateTime]::MinValue) `
        -CompletedAt $start.AddSeconds(45) `
        -StartSequenceExclusive 0 -EndSequenceInclusive 10 `
        -OutputDirectory $sentinelOutput
    Assert-MaxPayneVisual (-not $sentinelSelection.valid) `
        'missing scene start boundary was accepted as visual evidence'
    Assert-MaxPayneVisual `
        (@($sentinelSelection.findings | Where-Object {
            $_ -eq 'tutorial scene boundaries were not observed'
        }).Count -eq 1) `
        'missing scene start boundary was not reported explicitly'
    Assert-MaxPayneVisual `
        ($sentinelSelection.selectedCount -eq 0 -and
         @($sentinelSelection.images).Count -eq 0) `
        'frames outside a proven tutorial scene were selected'
    Assert-MaxPayneVisual `
        ([Math]::Max([int64]0, [int64]63920798067194) -eq
            [int64]63920798067194) `
        'Int64 offset clamp regressed to the Int32 overload'

    $missingMiddleOutput = Join-Path $root 'missing-middle'
    [void](New-Item -ItemType Directory -Path $missingMiddleOutput)
    $edgeFrames = @($frames | Where-Object {
        $_.Name -match '_(000001|000009)\.bmp$'
    })
    $missingMiddle = Select-XpMaxPayneTutorialVisualSequence `
        -Frames $edgeFrames -StartedAt $start `
        -CompletedAt $start.AddSeconds(45) `
        -StartSequenceExclusive 0 -EndSequenceInclusive 9 `
        -OutputDirectory $missingMiddleOutput
    Assert-MaxPayneVisual (-not $missingMiddle.valid) `
        'sequence with no middle-third frame was accepted'
    Assert-MaxPayneVisual `
        (@($missingMiddle.findings | Where-Object {
            $_ -like '*decodable frames*'
        }).Count -eq 1) `
        'insufficient-frame failure was not explicit'

    $boundedOutput = Join-Path $root 'bounded'
    [void](New-Item -ItemType Directory -Path $boundedOutput)
    $bounded = Select-XpMaxPayneTutorialVisualSequence `
        -Frames $frames -StartedAt $start `
        -CompletedAt $start.AddSeconds(45) `
        -StartSequenceExclusive 6 -EndSequenceInclusive 7 `
        -OutputDirectory $boundedOutput
    Assert-MaxPayneVisual (-not $bounded.valid) `
        'sequence bounds that remove outer thirds were ignored'

    $context = [pscustomobject]@{
        TaskId = 'visual-task'
        RunId = 'visual-run'
        Tier = 'T3'
    }
    $evidence = New-XpMaxPayneVisualEvidence `
        -Selection $selection -Context $context `
        -Mode '-xpdm-d3d-gameplay'
    Assert-MaxPayneVisual $evidence.valid `
        'valid selection did not produce valid visual evidence'
    Assert-MaxPayneVisual `
        ($evidence.assessment.automated.renderingCorrectness -eq
            'not-assessed') `
        'capture qualification was misrepresented as rendering correctness'

    $bundle = Join-Path $root 'bundle'
    [void](New-Item -ItemType Directory -Path $bundle)
    foreach ($image in @($selection.images)) {
        Copy-Item -LiteralPath $image.rawPath -Destination `
            (Join-Path $bundle $image.fileName)
    }
    Write-MaxPayneJson -Path (Join-Path $bundle `
        'maxpayne-visual-sequence.json') -Value $evidence
    $result = [pscustomobject][ordered]@{
        taskId = 'visual-task'
        runId = 'visual-run'
        tier = 'T3'
        mode = '-xpdm-d3d-gameplay'
        result = 'pass'
        harnessStatus = 'passed'
        capture = $true
        captureMode = 'visual'
        teardownOk = $true
        visualEvidence = $evidence
    }
    Write-MaxPayneJson -Path (Join-Path $bundle 'harness-result.json') `
        -Value $result
    $manifest = [pscustomobject][ordered]@{
        schema = 1
        runId = 'visual-run'
        files = @(Get-ChildItem -LiteralPath $bundle -File |
            Sort-Object Name | ForEach-Object {
                [pscustomobject][ordered]@{
                    name = $_.Name
                    size = $_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName `
                        -Algorithm SHA256).Hash
                }
            })
    }
    Write-MaxPayneJson -Path (Join-Path $bundle 'artifact-manifest.json') `
        -Value $manifest
    $bundleCheck = Test-XpMaxPayneVisualBundle -Directory $bundle
    Assert-MaxPayneVisual $bundleCheck.valid `
        "valid visual bundle failed: $($bundleCheck.findings -join '; ')"

    Add-Content -LiteralPath (Join-Path $bundle `
        $selection.images[0].fileName) -Value 'tamper'
    $tamperedCheck = Test-XpMaxPayneVisualBundle -Directory $bundle
    Assert-MaxPayneVisual (-not $tamperedCheck.valid) `
        'tampered visual screenshot was accepted'

    Write-Output 'Max Payne visual sequence tests passed'
} finally {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
