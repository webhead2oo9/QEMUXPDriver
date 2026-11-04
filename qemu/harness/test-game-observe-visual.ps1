$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
. (Join-Path $PSScriptRoot 'test-visual-bundle-fixtures.ps1')

function Assert-GameObserveVisual {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

Add-Type -AssemblyName System.Drawing

function New-GameObserveTestBitmap {
    param(
        [switch]$Black,
        [switch]$DesktopBottom,
        [switch]$DesktopTop,
        [int]$Variant = 0
    )

    $bitmap = [Drawing.Bitmap]::new(320, 240)
    for ($y = 0; $y -lt $bitmap.Height; ++$y) {
        for ($x = 0; $x -lt $bitmap.Width; ++$x) {
            $color = if ($Black) {
                [Drawing.Color]::Black
            } else {
                [Drawing.Color]::FromArgb(
                    24 + (($x * 7 + $y * 3 + $Variant * 17) % 220),
                    24 + (($x * 5 + $y * 11 + $Variant * 29) % 220),
                    24 + (($x * 13 + $y * 2 + $Variant * 37) % 220))
            }
            $bitmap.SetPixel($x, $y, $color)
        }
    }
    foreach ($probe in @(
            @(25, 225), @(300, 230), @(25, 14), @(300, 9))) {
        $bitmap.SetPixel($probe[0], $probe[1],
            [Drawing.Color]::FromArgb(90, 90, 90))
    }
    if ($DesktopBottom) {
        $bitmap.SetPixel(25, 225,
            [Drawing.Color]::FromArgb(10, 180, 10))
        $bitmap.SetPixel(300, 230,
            [Drawing.Color]::FromArgb(20, 20, 240))
    }
    if ($DesktopTop) {
        $bitmap.SetPixel(25, 14,
            [Drawing.Color]::FromArgb(10, 180, 10))
        $bitmap.SetPixel(300, 9,
            [Drawing.Color]::FromArgb(20, 20, 240))
    }
    return $bitmap
}

$rich = New-GameObserveTestBitmap
try {
    $assessment = Get-XpGameObserveBitmapAssessment -Bitmap $rich
    Assert-GameObserveVisual `
        ($assessment.valid -and $assessment.rich -and
         -not $assessment.desktop -and $assessment.colors -ge 8 -and
         $assessment.blackPercent -lt 90) `
        'rich non-desktop frame was rejected'
} finally {
    $rich.Dispose()
}

$black = New-GameObserveTestBitmap -Black
try {
    $assessment = Get-XpGameObserveBitmapAssessment -Bitmap $black
    Assert-GameObserveVisual `
        (-not $assessment.rich -and
         $assessment.blackPercent -eq 100) `
        'solid black frame was accepted'
} finally {
    $black.Dispose()
}

foreach ($edge in @('bottom', 'top')) {
    $desktop = if ($edge -eq 'bottom') {
        New-GameObserveTestBitmap -DesktopBottom
    } else {
        New-GameObserveTestBitmap -DesktopTop
    }
    try {
        $assessment = Get-XpGameObserveBitmapAssessment -Bitmap $desktop
        Assert-GameObserveVisual `
            (-not $assessment.rich -and $assessment.desktop) `
            "XP desktop at $edge edge was accepted as renderer output"
    } finally {
        $desktop.Dispose()
    }
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-game-observe-samples-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $root)
try {
    $residue = Join-Path $root 'unpublished-review.mp4'
    [IO.File]::WriteAllText($residue, 'partial')
    Remove-XpUnpublishedReviewVideo -Path $residue -Published $false
    Assert-GameObserveVisual `
        (-not (Test-Path -LiteralPath $residue)) `
        'failed video left an unhashed destination'
    $published = Join-Path $root 'published-review.mp4'
    [IO.File]::WriteAllText($published, 'complete')
    Remove-XpUnpublishedReviewVideo -Path $published -Published $true
    Assert-GameObserveVisual (Test-Path -LiteralPath $published) `
        'published video cleanup removed a valid destination'

    $publishSource = Join-Path $root 'publish-success.mp4'
    [IO.File]::WriteAllText($publishSource, 'publish-success')
    $publishResult = Publish-XpValidatedReviewVideo `
        -Destination $publishSource
    Assert-GameObserveVisual `
        (-not (Test-Path -LiteralPath $publishSource) -and
         (Test-Path -LiteralPath $publishResult.path) -and
         [IO.Path]::GetFileName($publishResult.path) -match
            '^publish-success-[0-9a-f]{12}\.mp4$') `
        'validated video was not digest-published'

    $hashFailureSource = Join-Path $root 'publish-hash-failure.mp4'
    [IO.File]::WriteAllText($hashFailureSource, 'partial-hash')
    $hashFailureCaught = $false
    try {
        [void](Publish-XpValidatedReviewVideo `
            -Destination $hashFailureSource `
            -HashAction { param($Path) throw "hash fault $Path" })
    } catch { $hashFailureCaught = $true }
    Assert-GameObserveVisual `
        ($hashFailureCaught -and
         -not (Test-Path -LiteralPath $hashFailureSource)) `
        'hash failure retained an unhashed video'

    $moveFailureSource = Join-Path $root 'publish-move-failure.mp4'
    [IO.File]::WriteAllText($moveFailureSource, 'partial-move')
    $moveFailureCaught = $false
    try {
        [void](Publish-XpValidatedReviewVideo `
            -Destination $moveFailureSource `
            -MoveAction {
                param($Source, $Target)
                throw "move fault $Source $Target"
            })
    } catch { $moveFailureCaught = $true }
    Assert-GameObserveVisual `
        ($moveFailureCaught -and
         -not (Test-Path -LiteralPath $moveFailureSource)) `
        'rename failure retained an unhashed video'

    $frames = [Collections.Generic.List[IO.FileInfo]]::new()
    for ($index = 0; $index -lt 9; ++$index) {
        $path = Join-Path $root ('rich-{0:D2}.bmp' -f $index)
        $bitmap = New-GameObserveTestBitmap -Variant $index
        try { $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Bmp) }
        finally { $bitmap.Dispose() }
        $item = Get-Item -LiteralPath $path
        $item.LastWriteTimeUtc = [DateTime]::Parse(
            '2026-07-28T12:00:00Z').AddSeconds($index)
        $frames.Add($item)
    }
    $desktopPath = Join-Path $root 'desktop.bmp'
    $desktop = New-GameObserveTestBitmap -DesktopTop
    try { $desktop.Save($desktopPath, [Drawing.Imaging.ImageFormat]::Bmp) }
    finally { $desktop.Dispose() }
    $desktopItem = Get-Item -LiteralPath $desktopPath
    $desktopItem.LastWriteTimeUtc = [DateTime]::Parse(
        '2026-07-28T12:00:09Z')
    $frames.Add($desktopItem)

    $selected = @(Select-XpGameObserveSampleFrames `
        -Frames @($frames) -Required 8)
    Assert-GameObserveVisual ($selected.Count -eq 8) `
        'qualified sequence did not select exactly eight frames'
    Assert-GameObserveVisual `
        (@($selected | Where-Object { $_.Name -eq 'desktop.bmp' }).Count -eq 0) `
        'desktop frame entered the retained sample set'
    $insufficient = @(Select-XpGameObserveSampleFrames `
        -Frames @($frames | Where-Object {
            $_.Name -like 'rich-0[0-6].bmp'
        }) -Required 8)
    Assert-GameObserveVisual ($insufficient.Count -eq 0) `
        'fewer than eight qualified frames did not fail closed'
    $earlyCoverage = Test-XpUT2003VisualFrameCoverage `
        -SelectedFrames $selected -RequiredObservedMs 180000
    Assert-GameObserveVisual (-not $earlyCoverage.valid) `
        'eight early rich frames passed 180-second visual coverage'
    $coverageStart = [DateTime]::Parse(
        '2026-07-28T13:00:00Z').ToUniversalTime()
    for ($index = 0; $index -lt $selected.Count; ++$index) {
        $selected[$index].LastWriteTimeUtc =
            $coverageStart.AddSeconds(25 * $index)
    }
    $fullCoverage = Test-XpUT2003VisualFrameCoverage `
        -SelectedFrames $selected -RequiredObservedMs 180000
    Assert-GameObserveVisual `
        ($fullCoverage.valid -and $fullCoverage.spanSeconds -eq 175 -and
         $fullCoverage.maximumGapSeconds -eq 25 -and
         $fullCoverage.canonicalDistinctHashes -ge 4 -and
         $fullCoverage.meaningfulTransitions -ge 3) `
        'distributed diverse rich frames failed 180-second visual coverage'
    $frozenFrames = [Collections.Generic.List[IO.FileInfo]]::new()
    for ($index = 0; $index -lt 8; ++$index) {
        $frozenPath = Join-Path $root ('frozen-{0:D2}.bmp' -f $index)
        Copy-Item -LiteralPath $selected[0].FullName `
            -Destination $frozenPath
        # Change ignored BMP header metadata so every file-byte hash differs
        # while the decoded visible pixels remain exactly identical.
        $frozenBytes = [IO.File]::ReadAllBytes($frozenPath)
        $frozenBytes[6] = [byte]($index + 1)
        [IO.File]::WriteAllBytes($frozenPath, $frozenBytes)
        $frozenItem = Get-Item -LiteralPath $frozenPath
        $frozenItem.LastWriteTimeUtc = $coverageStart.AddSeconds(25 * $index)
        $frozenFrames.Add($frozenItem)
    }
    $frozenCoverage = Test-XpUT2003VisualFrameCoverage `
        -SelectedFrames @($frozenFrames) -RequiredObservedMs 180000
    $frozenRawHashes = @($frozenFrames | ForEach-Object {
        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    } | Select-Object -Unique).Count
    Assert-GameObserveVisual `
        (-not $frozenCoverage.valid -and
         $frozenRawHashes -eq 8 -and
         $frozenCoverage.canonicalDistinctHashes -eq 1 -and
         $frozenCoverage.meaningfulTransitions -eq 0) `
        'metadata-diverse but visibly frozen frames passed coverage'

    $semanticBoundary = [DateTime]::Now.AddMinutes(-1)
    $writerGuardPath = Join-Path $root 'frame_640x480_000101.bmp'
    $pendingGuardPath = Join-Path $root 'frame_640x480_000102.bmp'
    $firstPostBoundaryPath = Join-Path $root `
        'frame_640x480_000103.bmp'
    $staleTimestampPath = Join-Path $root `
        'frame_640x480_000104.bmp'
    [IO.File]::WriteAllText($writerGuardPath, 'writer-guard')
    [IO.File]::WriteAllText($pendingGuardPath, 'pending-guard')
    [IO.File]::WriteAllText($firstPostBoundaryPath, 'post')
    [IO.File]::WriteAllText($staleTimestampPath, 'stale')
    $writerGuard = Get-Item -LiteralPath $writerGuardPath
    $pendingGuard = Get-Item -LiteralPath $pendingGuardPath
    $firstPostBoundary = Get-Item -LiteralPath $firstPostBoundaryPath
    $staleTimestamp = Get-Item -LiteralPath $staleTimestampPath
    $writerGuard.LastWriteTime = $semanticBoundary.AddSeconds(1)
    $pendingGuard.LastWriteTime = $semanticBoundary.AddSeconds(2)
    $firstPostBoundary.LastWriteTime = $semanticBoundary.AddSeconds(3)
    $staleTimestamp.LastWriteTime = $semanticBoundary.AddSeconds(-1)
    $boundaryArguments = @{
        StartedAt = $semanticBoundary
        CompletedAt = $semanticBoundary.AddMinutes(3)
        StartSequence = 102
        EndSequence = 300
        StrictStartTime = $true
    }
    Assert-GameObserveVisual `
        (-not (Test-XpGameObserveFrameInterval `
            -Frame $writerGuard @boundaryArguments) -and
         -not (Test-XpGameObserveFrameInterval `
            -Frame $pendingGuard @boundaryArguments) -and
         (Test-XpGameObserveFrameInterval `
            -Frame $firstPostBoundary @boundaryArguments) -and
         -not (Test-XpGameObserveFrameInterval `
            -Frame $staleTimestamp @boundaryArguments)) `
        'writer/pending guard or stale-time frame crossed semantic boundary'

    for ($index = 0; $index -lt $selected.Count - 1; ++$index) {
        $selected[$index].LastWriteTimeUtc =
            $coverageStart.AddSeconds($index)
    }
    $selected[-1].LastWriteTimeUtc = $coverageStart.AddSeconds(175)
    $clusteredCoverage = Test-XpUT2003VisualFrameCoverage `
        -SelectedFrames $selected -RequiredObservedMs 180000
    Assert-GameObserveVisual `
        (-not $clusteredCoverage.valid -and
         $clusteredCoverage.spanSeconds -eq 175 -and
         $clusteredCoverage.maximumGapSeconds -gt 45) `
        'clustered rich frames with one late witness passed coverage'
    Assert-GameObserveVisual `
        (Test-XpGameObserveEncodedVideoTiming `
            -ExpectedFrames 675 -EncodedFrames 675 -FrameRate 15 `
            -NominalFrameRate '15/1' -AverageFrameRate '15/1' `
            -DurationSeconds 45.0) `
        'exact 15-fps review timing was rejected'
    Assert-GameObserveVisual `
        (-not (Test-XpGameObserveEncodedVideoTiming `
            -ExpectedFrames 675 -EncodedFrames 676 -FrameRate 15 `
            -NominalFrameRate '15/1' -AverageFrameRate '15/1' `
            -DurationSeconds (676 / 15.0)) -and
         -not (Test-XpGameObserveEncodedVideoTiming `
            -ExpectedFrames 675 -EncodedFrames 675 -FrameRate 15 `
            -NominalFrameRate '15/1' -AverageFrameRate '15/1' `
            -DurationSeconds (676 / 15.0))) `
        'extra frame or duration was accepted'

    $semanticCompiler = Get-Command 'i686-w64-mingw32-gcc.exe' `
        -ErrorAction SilentlyContinue
    Assert-GameObserveVisual ($null -ne $semanticCompiler) `
        'MinGW compiler is unavailable for UT2003 semantic mutations'
    $semanticTest = Join-Path $root 'qxp_ut2003_log_test.exe'
    $semanticSources = @(
        (Join-Path $PSScriptRoot `
            '..\..\guest\acceptance\qxp_ut2003_log_test.c'),
        (Join-Path $PSScriptRoot `
            '..\..\guest\acceptance\qxp_ut2003_log.c'))
    $semanticCompileOutput = & $semanticCompiler.Source `
        '-O2' '-Wall' '-Wextra' '-mconsole' '-static-libgcc' `
        '-o' $semanticTest @semanticSources 2>&1
    Assert-GameObserveVisual ($LASTEXITCODE -eq 0) `
        "UT2003 semantic test compile failed: $($semanticCompileOutput -join ' ')"
    $semanticTestOutput = & $semanticTest 2>&1
    Assert-GameObserveVisual `
        ($LASTEXITCODE -eq 0 -and
         ($semanticTestOutput -join "`n").Contains(
            'UT2003 Demo 2206 gameplay-log and visual-timing tests passed')) `
        "UT2003 parser mutations failed: $($semanticTestOutput -join ' ')"

    $utReady = 'QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-ready boundary=foreground-d3d8-window-stable warmup-ms=30000'
    $utStage = 'QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-complete observed-ms=180001 required-ms=180000 renderer=d3d8 system32=1 boundary=post-visual-ready'
    $utResult = 'QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003 stage=complete outcome=observed exit=259 modules=05 observed-ms=180001'
    $utContract = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady) -StageMessages @($utStage) `
        -ResultMessages @($utResult)
    Assert-GameObserveVisual `
        ($utContract.valid -and $utContract.observedMs -eq 180001 -and
         $utContract.visualBoundary -ceq
            'foreground-d3d8-window-stable') `
        'exact UT2003 visual-ready guest contract was rejected'
    $missingReady = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @() -StageMessages @($utStage) `
        -ResultMessages @($utResult)
    $wrongRc = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady.Replace('rc=0', 'rc=9')) `
        -StageMessages @($utStage) -ResultMessages @($utResult)
    $wrongGame = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady) -StageMessages @($utStage) `
        -ResultMessages @($utResult.Replace('game=ut2003', 'game=wrong'))
    $obsoleteReady = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady.Replace(
            'foreground-d3d8-window-stable',
            'log-main-menu-assumed')) `
        -StageMessages @($utStage) -ResultMessages @($utResult)
    $missingBoundary = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady) `
        -StageMessages @($utStage.Replace(
            ' boundary=post-visual-ready', '')) `
        -ResultMessages @($utResult)
    Assert-GameObserveVisual `
        (-not $missingReady.valid -and -not $wrongRc.valid -and
         -not $wrongGame.valid -and -not $obsoleteReady.valid -and
         -not $missingBoundary.valid) `
        'missing, obsolete, wrong, or pre-boundary UT2003 evidence was accepted'
    $uniqueStages = [Collections.Generic.List[string]]::new()
    Assert-GameObserveVisual `
        ((Add-XpGameObserveUniqueMessage `
            -Messages $uniqueStages -Message $utReady) -and
         -not (Add-XpGameObserveUniqueMessage `
            -Messages $uniqueStages -Message $utReady) -and
         $uniqueStages.Count -eq 1) `
        'exact visual-ready retransmission was not deduplicated'
    $conflicting = Test-XpUT2003VisualGuestContract `
        -ReadyMessages @($utReady, $utReady.Replace(
            'warmup-ms=30000', 'warmup-ms=30001')) `
        -StageMessages @($utStage, $utStage.Replace(
            'observed-ms=180001', 'observed-ms=180002')) `
        -ResultMessages @($utResult)
    Assert-GameObserveVisual (-not $conflicting.valid) `
        'conflicting UT2003 ready or visual records were accepted'

    $gameplayConsole = @(
        'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=semantic transition=dm-antalus-ready evidence=browse-load-deathmatch-level-play',
        'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=module name=d3d8.dll path=C:\WINDOWS\system32\d3d8.dll system32=1',
        'GuestInput=mouse-left ok=True',
        'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=1 injected=2 foreground=1 ready=1',
        'GuestInput=mouse-left ok=True',
        'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=2 injected=2 foreground=1 ready=1',
        'GuestInput=mouse-left ok=True',
        'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=3 injected=2 foreground=1 ready=1',
        'GuestResult=QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003gameplay stage=complete outcome=observed exit=259 modules=05 observed-ms=49078') -join "`n"
    $gameplayContract =
        Test-XpUT2003GameplayGuestContract -Console $gameplayConsole
    Assert-GameObserveVisual `
        ($gameplayContract.valid -and
         $gameplayContract.observedMs -eq 49078 -and
         $gameplayContract.semanticTransition -ceq 'dm-antalus-ready') `
        'exact UT2003 joined-gameplay guest contract was rejected'
    $badGameplayInput = Test-XpUT2003GameplayGuestContract `
        -Console $gameplayConsole.Replace(
            'attempt=2 injected=2', 'attempt=2 injected=1')
    $badGameplaySemantic = Test-XpUT2003GameplayGuestContract `
        -Console $gameplayConsole.Replace(
            'browse-load-deathmatch-level-play',
            'browse-load-level-play')
    $shortGameplay = Test-XpUT2003GameplayGuestContract `
        -Console $gameplayConsole.Replace(
            'observed-ms=49078', 'observed-ms=44999')
    Assert-GameObserveVisual `
        (-not $badGameplayInput.valid -and
         -not $badGameplaySemantic.valid -and
         -not $shortGameplay.valid) `
        'missing input, semantic, or duration passed gameplay contract'


    foreach ($mode in @(
            'UT2003Visual', 'UT2003Gameplay', 'Warcraft3D3D8Visual')) {
        $bundle = Join-Path $root $mode
        $videoName = New-GameObserveBundleFixture `
            -Directory $bundle -Mode $mode
        $check = Test-XpGameObserveVisualBundle `
            -Directory $bundle -Mode $mode
        Assert-GameObserveVisual $check.valid `
            "$mode fixture was rejected: $(@($check.findings) -join '; ')"
        if ($mode -eq 'UT2003Visual') {
            $harnessPath = Join-Path $bundle 'harness-result.json'
            $harnessText = Get-Content -Raw -LiteralPath $harnessPath
            $harnessDocument = $harnessText | ConvertFrom-Json
            $harnessDocument.result = 'fail'
            [IO.File]::WriteAllText($harnessPath,
                ($harnessDocument | ConvertTo-Json))
            $tamperedHarness = Test-XpGameObserveVisualBundle `
                -Directory $bundle -Mode $mode
            Assert-GameObserveVisual `
                (-not $tamperedHarness.valid -and
                 @($tamperedHarness.findings | Where-Object {
                    $_ -eq 'harness result manifest size/hash mismatch'
                 }).Count -eq 1) `
                'UT2003 bundle trusted an unbound harness result'
            [IO.File]::WriteAllText($harnessPath, $harnessText)

            $consolePath = Join-Path $bundle 'console.log'
            $consoleText = Get-Content -Raw -LiteralPath $consolePath
            [IO.File]::WriteAllText($consolePath,
                $consoleText.Replace(
                    'UT2003MenuVisual valid=True',
                    'UT2003MenuVisual valid=False'),
                [Text.Encoding]::Unicode)
            $tamperedMenu = Test-XpGameObserveVisualBundle `
                -Directory $bundle -Mode $mode
            Assert-GameObserveVisual (-not $tamperedMenu.valid -and
                @($tamperedMenu.findings | Where-Object {
                    $_ -like '*menu semantic is missing or non-unique*'
                }).Count -eq 1 -and
                @($tamperedMenu.findings | Where-Object {
                    $_ -eq 'console log manifest size/hash mismatch'
                }).Count -eq 1) `
                'UT2003 bundle accepted tampered menu-layout console proof'
            [IO.File]::WriteAllText(
                $consolePath, $consoleText, [Text.Encoding]::Unicode)
        }
        Add-Content -LiteralPath (Join-Path $bundle $videoName) `
            -Value 'tampered'
        $tampered = Test-XpGameObserveVisualBundle `
            -Directory $bundle -Mode $mode
        Assert-GameObserveVisual (-not $tampered.valid -and
            @($tampered.findings | Where-Object {
                $_ -like '*video hash mismatch*'
            }).Count -eq 1) `
            "$mode did not fail closed on video tampering"
    }

    $invalidVideoBundle = Join-Path $root 'manifest-bound-invalid-video'
    $invalidVideoName = New-GameObserveBundleFixture `
        -Directory $invalidVideoBundle -Mode 'UT2003Gameplay'
    $invalidVideoPath = Join-Path $invalidVideoBundle $invalidVideoName
    [IO.File]::WriteAllText($invalidVideoPath, 'not-an-mp4')
    $invalidVideoHash = (Get-FileHash -LiteralPath $invalidVideoPath `
        -Algorithm SHA256).Hash
    $invalidVideoRenamed =
        "xpdm-ut2003gameplay-review-$($invalidVideoHash.Substring(0, 12).ToLowerInvariant()).mp4"
    Move-Item -LiteralPath $invalidVideoPath `
        -Destination (Join-Path $invalidVideoBundle $invalidVideoRenamed)
    $invalidVideoManifestPath = Join-Path $invalidVideoBundle `
        'artifact-manifest.json'
    $invalidVideoManifest = Get-Content -Raw -LiteralPath `
        $invalidVideoManifestPath | ConvertFrom-Json
    $invalidVideoRecords = @($invalidVideoManifest.files |
        Where-Object { [string]$_.name -ceq $invalidVideoName })
    Assert-GameObserveVisual ($invalidVideoRecords.Count -eq 1) `
        'invalid-video fixture lacks one original manifest record'
    $invalidVideoRecords[0].name = $invalidVideoRenamed
    $invalidVideoRecords[0].size = (
        Get-Item -LiteralPath (
            Join-Path $invalidVideoBundle $invalidVideoRenamed)).Length
    $invalidVideoRecords[0].sha256 = $invalidVideoHash
    [IO.File]::WriteAllText(
        $invalidVideoManifestPath,
        (($invalidVideoManifest | ConvertTo-Json -Depth 5) + "`n"))
    $invalidVideo = Test-XpGameObserveVisualBundle `
        -Directory $invalidVideoBundle -Mode 'UT2003Gameplay'
    Assert-GameObserveVisual (-not $invalidVideo.valid -and
        @($invalidVideo.findings | Where-Object {
            $_ -like '*review video is not decodable*'
        }).Count -eq 1) `
        'manifest-bound non-video bytes passed the decoded-video gate'

    foreach ($permutedMode in @('UT2003Gameplay', 'Warcraft3D3D8Visual')) {
        $permutedBundle = Join-Path $root "permuted-video-$permutedMode"
        [void](New-GameObserveBundleFixture `
            -Directory $permutedBundle -Mode $permutedMode -PermutedVideo)
        $permuted = Test-XpGameObserveVisualBundle `
            -Directory $permutedBundle -Mode $permutedMode
        $pixelFinding = if ($permutedMode -eq 'Warcraft3D3D8Visual') {
            '*manifested sample*pixels*'
        } else { '*out of sequence order*' }
        Assert-GameObserveVisual (-not $permuted.valid -and
            @($permuted.findings | Where-Object {
                $_ -like $pixelFinding
            }).Count -ge 1) `
            "$permutedMode content-correct metadata with wrong video pixels passed: $(@($permuted.findings) -join '; ')"
    }

    $confinedRoot = Join-Path $root 'confinement'
    [void](New-Item -ItemType Directory -Path $confinedRoot)
    [IO.File]::WriteAllText((Join-Path $confinedRoot 'real.txt'), 'real')
    $confinedExpectation = [IO.Path]::GetFullPath(
        (Join-Path $confinedRoot 'real.txt'))
    Assert-GameObserveVisual `
        ((Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name 'real.txt') -ceq $confinedExpectation) `
        'confined in-bundle artifact was rejected'
    Assert-GameObserveVisual `
        (-not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name '..\real.txt') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name 'sub\real.txt') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name 'sub/real.txt') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name 'C:\real.txt') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name 'real.txt:stream') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name ' real.txt') -and
         -not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
            -Name ' ')) `
        'traversal, separator, drive, stream, or padded names were confined'
    $junctionPath = Join-Path $confinedRoot 'junction.txt'
    [void](New-Item -ItemType Junction -Path $junctionPath `
        -Target $confinedRoot)
    try {
        Assert-GameObserveVisual `
            (-not (Resolve-XpBundleConfinedPath -BundleRoot $confinedRoot `
                -Name 'junction.txt')) `
            'reparse-point descendant was accepted as a bundle artifact'
    } finally {
        [IO.Directory]::Delete($junctionPath)
    }

    $missingSemanticBundle = Join-Path $root 'missing-ut-semantic'
    [void](New-GameObserveBundleFixture `
        -Directory $missingSemanticBundle -Mode 'UT2003Visual')
    $missingSemanticConsole = Join-Path $missingSemanticBundle 'console.log'
    $missingSemanticText = Get-Content -Raw -LiteralPath `
        $missingSemanticConsole
    $missingSemanticText = $missingSemanticText.Replace(
        'UT2003Semantic valid=True transition=main-menu ' +
        'evidence=manifest-bound-menu-layout qualifyingFrames=8 ' +
        'requiredFrames=6 boundary=post-visual-ready' + "`n", '')
    [IO.File]::WriteAllText($missingSemanticConsole,
        $missingSemanticText, [Text.Encoding]::Unicode)
    $missingSemanticManifestPath = Join-Path $missingSemanticBundle `
        'artifact-manifest.json'
    $missingSemanticManifest = Get-Content -Raw -LiteralPath `
        $missingSemanticManifestPath | ConvertFrom-Json
    $missingSemanticConsoleRecord = @(
        $missingSemanticManifest.files | Where-Object {
            $_.name -ceq 'console.log'
        })
    Assert-GameObserveVisual ($missingSemanticConsoleRecord.Count -eq 1) `
        'semantic fixture lacks one console manifest record'
    $missingSemanticConsoleRecord[0].size =
        (Get-Item -LiteralPath $missingSemanticConsole).Length
    $missingSemanticConsoleRecord[0].sha256 =
        (Get-FileHash -LiteralPath $missingSemanticConsole `
            -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($missingSemanticManifestPath,
        (($missingSemanticManifest | ConvertTo-Json -Depth 5) + "`n"))
    $missingSemanticBundleCheck = Test-XpGameObserveVisualBundle `
        -Directory $missingSemanticBundle -Mode 'UT2003Visual'
    Assert-GameObserveVisual `
        (-not $missingSemanticBundleCheck.valid -and
         @($missingSemanticBundleCheck.findings | Where-Object {
            $_ -eq 'UT2003 manifest-bound menu semantic is missing or non-unique'
         }).Count -eq 1 -and
         @($missingSemanticBundleCheck.findings | Where-Object {
            $_ -eq 'console log manifest size/hash mismatch'
         }).Count -eq 0) `
        'manifest-consistent UT2003 bundle passed without semantic evidence'

    $missingMenuBundle = Join-Path $root 'missing-ut-menu-highlight'
    [void](New-GameObserveBundleFixture `
        -Directory $missingMenuBundle -Mode 'UT2003Visual' `
        -MissingUTMenuHighlight)
    $missingMenuCheck = Test-XpGameObserveVisualBundle `
        -Directory $missingMenuBundle -Mode 'UT2003Visual'
    Assert-GameObserveVisual `
        (-not $missingMenuCheck.valid -and
         $missingMenuCheck.evidence.recomputedMenuQualifyingFrames -eq 0 -and
         @($missingMenuCheck.findings | Where-Object {
            $_ -eq ('manifest-bound UT2003 samples do not prove ' +
                'the expected main-menu visual layout')
         }).Count -eq 1) `
        'manifest-bound samples without the menu highlight passed'

    $badCoverageBundle = Join-Path $root 'bad-ut-coverage'
    [void](New-GameObserveBundleFixture `
        -Directory $badCoverageBundle -Mode 'UT2003Visual')
    $badCoverageConsole = Join-Path $badCoverageBundle 'console.log'
    $badCoverageText = Get-Content -Raw -LiteralPath $badCoverageConsole
    [IO.File]::WriteAllText($badCoverageConsole,
        $badCoverageText.Replace(
            'spanSeconds=180 requiredSpanSeconds=150',
            'spanSeconds=10 requiredSpanSeconds=150'),
        [Text.Encoding]::Unicode)
    $badCoverageManifestPath = Join-Path $badCoverageBundle `
        'artifact-manifest.json'
    $badCoverageManifest = Get-Content -Raw -LiteralPath `
        $badCoverageManifestPath | ConvertFrom-Json
    $badCoverageConsoleRecord = @(
        $badCoverageManifest.files | Where-Object {
            $_.name -ceq 'console.log'
        })
    $badCoverageConsoleRecord[0].size =
        (Get-Item -LiteralPath $badCoverageConsole).Length
    $badCoverageConsoleRecord[0].sha256 =
        (Get-FileHash -LiteralPath $badCoverageConsole `
            -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($badCoverageManifestPath,
        (($badCoverageManifest | ConvertTo-Json -Depth 5) + "`n"))
    $badCoverageCheck = Test-XpGameObserveVisualBundle `
        -Directory $badCoverageBundle -Mode 'UT2003Visual'
    Assert-GameObserveVisual `
        (-not $badCoverageCheck.valid -and
         @($badCoverageCheck.findings | Where-Object {
            $_ -eq 'UT2003 sustained rich-frame coverage is missing or invalid'
         }).Count -eq 1 -and
         @($badCoverageCheck.findings | Where-Object {
            $_ -eq 'console log manifest size/hash mismatch'
         }).Count -eq 0) `
        'manifest-consistent UT2003 bundle passed invalid frame coverage'

    $missingConsoleBundle = Join-Path $root 'missing-console-manifest'
    [void](New-GameObserveBundleFixture `
        -Directory $missingConsoleBundle -Mode 'UT2003Visual')
    $missingConsoleManifestPath = Join-Path $missingConsoleBundle `
        'artifact-manifest.json'
    $missingConsoleManifest = Get-Content -Raw -LiteralPath `
        $missingConsoleManifestPath | ConvertFrom-Json
    $missingConsoleManifest.files = @(
        $missingConsoleManifest.files | Where-Object {
            $_.name -cne 'console.log'
        })
    [IO.File]::WriteAllText($missingConsoleManifestPath,
        ($missingConsoleManifest | ConvertTo-Json -Depth 5))
    $missingConsole = Test-XpGameObserveVisualBundle `
        -Directory $missingConsoleBundle -Mode 'UT2003Visual'
    Assert-GameObserveVisual (-not $missingConsole.valid -and
        @($missingConsole.findings | Where-Object {
            $_ -eq 'console log is not uniquely manifested'
        }).Count -eq 1) `
        'UT2003 bundle accepted an unmanifested console proof'

    $malformedBundle = Join-Path $root 'malformed-bundle'
    [void](New-GameObserveBundleFixture `
        -Directory $malformedBundle -Mode 'UT2003Visual')
    $malformedHarnessPath = Join-Path $malformedBundle 'harness-result.json'
    $malformedHarness = Get-Content -Raw -LiteralPath `
        $malformedHarnessPath | ConvertFrom-Json
    $malformedHarness.exitCode = 'not-an-integer'
    [IO.File]::WriteAllText($malformedHarnessPath,
        ($malformedHarness | ConvertTo-Json))
    $malformedManifestPath = Join-Path $malformedBundle `
        'artifact-manifest.json'
    $malformedManifest = Get-Content -Raw -LiteralPath `
        $malformedManifestPath | ConvertFrom-Json
    $malformedHarnessRecord = @($malformedManifest.files |
        Where-Object { $_.name -ceq 'harness-result.json' })
    $malformedHarnessRecord[0].size =
        (Get-Item -LiteralPath $malformedHarnessPath).Length
    $malformedHarnessRecord[0].sha256 =
        (Get-FileHash -LiteralPath $malformedHarnessPath `
            -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($malformedManifestPath,
        ($malformedManifest | ConvertTo-Json -Depth 5))
    $malformed = Test-XpGameObserveVisualBundle `
        -Directory $malformedBundle -Mode 'UT2003Visual'
    Assert-GameObserveVisual (-not $malformed.valid -and
        $null -ne $malformed.evidence -and
        @($malformed.findings | Where-Object {
            $_ -like 'visual bundle verification failed closed:*'
        }).Count -eq 1) `
        'malformed visual metadata threw or lacked canonical invalid evidence'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force `
        -ErrorAction SilentlyContinue
}

Write-Output 'game-observe visual classification tests passed'
