# Shared synthetic visual-bundle fixtures for harness self-tests.
# Dot-sourced by test-game-observe-visual.ps1 and
# test-verify-xp-evidence-bundle.ps1; contains no test logic of its own.

function Get-GameObserveFixtureVideoBytes {
    param(
        [string]$Mode,
        [string[]]$SamplePaths,
        [switch]$PermutedOrder
    )

    if ($null -eq $script:GameObserveFixtureVideoBytes) {
        $script:GameObserveFixtureVideoBytes = @{}
    }
    $sampleHashes = @($SamplePaths | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    })
    $cacheKey = "{0}|{1}|{2}" -f $Mode, ($sampleHashes -join ','),
        [bool]$PermutedOrder
    if ($script:GameObserveFixtureVideoBytes.ContainsKey($cacheKey)) {
        return [byte[]]$script:GameObserveFixtureVideoBytes[$cacheKey]
    }

    # Fixture videos are encoded from the fixture sample bitmaps themselves
    # through the same concat/encoder pipeline as production, so the decoded
    # pixels provably bind to the manifested source frames.  -PermutedOrder
    # rotates the sample sequence to produce content-correct metadata with
    # wrong pixels for negative tests.
    $frames = if ($Mode -eq 'UT2003Visual') {
        675
    } elseif ($Mode -eq 'UT2003Gameplay') {
        525
    } else { 256 }
    $order = @(0, 1, 2, 3, 4, 5, 6, 7)
    if ($PermutedOrder) {
        $order = @(1, 2, 3, 4, 5, 6, 7, 0)
    }
    $starts = @(for ($index = 0; $index -lt 8; ++$index) {
        [int][Math]::Round($index * ($frames - 1) / 7.0)
    })
    $workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $ffmpeg = Join-Path $workspace `
        '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe'
    if (-not (Test-Path -LiteralPath $ffmpeg -PathType Leaf)) {
        throw 'Pinned FFmpeg is required for visual-bundle fixtures'
    }
    $sampleDirectory = Split-Path -Parent $SamplePaths[0]
    $manifest = Join-Path $sampleDirectory `
        "fixture-video-$PID-$([Guid]::NewGuid().ToString('N')).ffconcat"
    $temporary = Join-Path ([IO.Path]::GetTempPath()) `
        "xpdriver-fixture-$PID-$([Guid]::NewGuid().ToString('N')).mp4"
    try {
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add('ffconcat version 1.0')
        for ($frame = 0; $frame -lt $frames; ++$frame) {
            $block = 0
            for ($index = 1; $index -lt 8; ++$index) {
                if ($starts[$index] -le $frame) { $block = $index }
            }
            $name = [IO.Path]::GetFileName($SamplePaths[$order[$block]])
            $lines.Add("file '$($name.Replace("'", "''"))'")
            if ($Mode -ne 'Warcraft3D3D8Visual') {
                $lines.Add('duration 0.0666667')
            }
        }
        if ($Mode -ne 'Warcraft3D3D8Visual') {
            # The concat demuxer ignores the final duration unless another
            # file follows.
            $name = [IO.Path]::GetFileName($SamplePaths[$order[7]])
            $lines.Add("file '$($name.Replace("'", "''"))'")
        }
        [IO.File]::WriteAllLines($manifest, [string[]]$lines,
            [Text.Encoding]::ASCII)
        if ($Mode -eq 'Warcraft3D3D8Visual') {
            & $ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 `
                -i $manifest -an `
                -vf 'settb=expr=1/30,setpts=N,scale=trunc(iw/2)*2:trunc(ih/2)*2:flags=neighbor' `
                -fps_mode passthrough -enc_time_base '1:30' `
                -c:v libx264 -preset veryfast -crf 22 `
                -pix_fmt yuv420p -movflags +faststart $temporary
        } else {
            & $ffmpeg -hide_banner -loglevel error -y -f concat -safe 0 `
                -i $manifest -an `
                -vf 'scale=trunc(iw/2)*2:trunc(ih/2)*2:flags=neighbor' `
                -c:v libx264 -preset veryfast -crf 22 -pix_fmt yuv420p `
                -movflags +faststart -r 15 -frames:v $frames $temporary
        }
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $temporary -PathType Leaf)) {
            throw "fixture FFmpeg failed with exit code $LASTEXITCODE"
        }
        $bytes = [IO.File]::ReadAllBytes($temporary)
        $script:GameObserveFixtureVideoBytes[$cacheKey] = $bytes
        return [byte[]]$bytes
    } finally {
        Remove-Item -LiteralPath $manifest -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
    }
}

function New-GameObserveBundleFixture {
    param(
        [string]$Directory,
        [string]$Mode,
        [switch]$MetadataDiverseFrozenSamples,
        [switch]$PermutedVideo,
        [switch]$MissingUTMenuHighlight
    )

    [void](New-Item -ItemType Directory -Force -Path $Directory)
    $prefix = if ($Mode -eq 'UT2003Visual') {
        'xpdm-ut2003'
    } elseif ($Mode -eq 'UT2003Gameplay') {
        'xpdm-ut2003gameplay'
    } else { 'xpdm-warcraft3d3d8' }
    $modeFlag = if ($Mode -eq 'UT2003Visual') {
        '-xpdm-ut2003'
    } elseif ($Mode -eq 'UT2003Gameplay') {
        '-xpdm-ut2003gameplay'
    } else { '-xpdm-warcraft3d3d8' }
    $runId = "fixture-$Mode"
    $harness = [pscustomobject][ordered]@{
        result = 'pass'
        harnessStatus = 'passed'
        exitCode = 0
        capture = $true
        captureMode = 'video'
        taskId = $runId
        runId = $runId
        mode = $modeFlag
    }
    [IO.File]::WriteAllText(
        (Join-Path $Directory 'harness-result.json'),
        ($harness | ConvertTo-Json))
    $records = [Collections.Generic.List[object]]::new()
    $harnessPath = Join-Path $Directory 'harness-result.json'
    $records.Add([pscustomobject][ordered]@{
        name = 'harness-result.json'
        size = (Get-Item -LiteralPath $harnessPath).Length
        sha256 = (Get-FileHash -LiteralPath $harnessPath `
            -Algorithm SHA256).Hash
    })
    Add-Type -AssemblyName System.Drawing
    $sampleWidth = if ($Mode -eq 'Warcraft3D3D8Visual') { 800 } else { 640 }
    $sampleHeight = if ($Mode -eq 'Warcraft3D3D8Visual') { 600 } else { 480 }
    $blockWidth = [int]($sampleWidth / 16)
    $blockHeight = [int]($sampleHeight / 12)
    # Sample capture times must sit inside the encoded review-video window
    # asserted by the console GameObserveFrames line below so the pixel
    # binding is fully exercised for every manifested sample.
    $sampleTimelineStart = if ($Mode -eq 'UT2003Visual') {
        [DateTime]::SpecifyKind([DateTime]::Parse('2026-07-29T00:00:00'),
            [DateTimeKind]::Utc)
    } else {
        [DateTime]::SpecifyKind([DateTime]::Parse('2026-08-01T00:00:00'),
            [DateTimeKind]::Utc)
    }
    $samplePaths = [Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt 8; ++$index) {
        $temporary = Join-Path $Directory "sample-$index.bmp"
        $bitmap = [Drawing.Bitmap]::new($sampleWidth, $sampleHeight)
        $visualIndex = if ($MetadataDiverseFrozenSamples) { 0 } else { $index }
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            for ($by = 0; $by -lt 12; ++$by) {
                for ($bx = 0; $bx -lt 16; ++$bx) {
                    $brush = if ($Mode -eq 'UT2003Visual') {
                        [Drawing.SolidBrush]::new(
                            [Drawing.Color]::FromArgb(
                                18 + (($visualIndex * 3 + $bx) % 24),
                                28 + (($visualIndex * 5 + $by) % 28),
                                48 + (($visualIndex * 7 + $bx + $by) % 40)))
                    } else {
                        [Drawing.SolidBrush]::new(
                            [Drawing.Color]::FromArgb(
                                40 + (($visualIndex * 17 + $bx * 11 +
                                       $by * 3) % 190),
                                35 + (($visualIndex * 7 + $bx * 5 +
                                       $by * 13) % 190),
                                30 + (($visualIndex * 3 + $bx * 17 +
                                       $by * 7) % 190)))
                    }
                    try {
                        $graphics.FillRectangle($brush,
                            $bx * $blockWidth, $by * $blockHeight,
                            $blockWidth, $blockHeight)
                    } finally {
                        $brush.Dispose()
                    }
                }
            }
            if ($Mode -eq 'UT2003Visual') {
                $white = [Drawing.SolidBrush]::new(
                    [Drawing.Color]::FromArgb(240, 240, 240))
                $gold = [Drawing.SolidBrush]::new(
                    [Drawing.Color]::FromArgb(190, 150, 40))
                try {
                    # Synthetic high-contrast regions occupy the same
                    # normalized title/highlight/menu-row bands that the
                    # production oracle samples. They are shapes, not copied
                    # game artwork, and retain a varying dark-blue backdrop.
                    $graphics.FillRectangle($white, 100, 32, 360, 24)
                    $graphics.FillRectangle($gold, 248, 120, 144, 16)
                    if (-not $MissingUTMenuHighlight) {
                        $graphics.FillRectangle($gold, 220, 248, 200, 20)
                    }
                    $graphics.FillRectangle($white, 200, 280, 240, 20)
                    $graphics.FillRectangle($white, 230, 320, 180, 20)
                    $graphics.FillRectangle($white, 260, 352, 120, 20)
                } finally {
                    $white.Dispose()
                    $gold.Dispose()
                }
            }
            $bitmap.Save($temporary, [Drawing.Imaging.ImageFormat]::Bmp)
        } finally {
            $graphics.Dispose()
            $bitmap.Dispose()
        }
        if ($MetadataDiverseFrozenSamples) {
            $bytes = [IO.File]::ReadAllBytes($temporary)
            $bytes[6] = [byte]($index + 1)
            [IO.File]::WriteAllBytes($temporary, $bytes)
        }
        $hash = (Get-FileHash -LiteralPath $temporary `
            -Algorithm SHA256).Hash
        $name = "$prefix-sample-$index-$($hash.Substring(0, 12).ToLowerInvariant()).bmp"
        Move-Item -LiteralPath $temporary `
            -Destination (Join-Path $Directory $name)
        (Get-Item -LiteralPath (Join-Path $Directory $name)).LastWriteTimeUtc =
            $sampleTimelineStart.AddSeconds(4 * $index)
        $samplePaths.Add((Join-Path $Directory $name))
        $records.Add([pscustomobject][ordered]@{
            name = $name
            size = (Get-Item -LiteralPath (
                Join-Path $Directory $name)).Length
            sha256 = $hash
        })
    }
    $temporary = Join-Path $Directory 'video.mp4'
    [IO.File]::WriteAllBytes($temporary,
        (Get-GameObserveFixtureVideoBytes -Mode $Mode `
            -SamplePaths @($samplePaths) -PermutedOrder:$PermutedVideo))
    $hash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash
    $videoName = "$prefix-review-$($hash.Substring(0, 12).ToLowerInvariant()).mp4"
    Move-Item -LiteralPath $temporary `
        -Destination (Join-Path $Directory $videoName)
    $records.Add([pscustomobject][ordered]@{
        name = $videoName
        size = (Get-Item -LiteralPath (
            Join-Path $Directory $videoName)).Length
        sha256 = $hash
    })

    if ($Mode -eq 'UT2003Visual') {
        $console = @(
            'UT2003VisualGuestContract valid=True ready=1 stages=1 results=1 findings=0',
            'UT2003VisualBoundary valid=True boundary=foreground-d3d8-window-stable messages=1',
            'UT2003VisualTiming valid=True observedMs=180001 requiredMs=180000 boundary=post-visual-ready',
            'UT2003MenuVisual valid=True selected=8 qualifyingFrames=8 requiredFrames=6 signatureStep=8 geometry=640x480',
            'UT2003Semantic valid=True transition=main-menu evidence=manifest-bound-menu-layout qualifyingFrames=8 requiredFrames=6 boundary=post-visual-ready',
            'UT2003CaptureBoundary readySequence=100 lastGuardSequence=102 firstAcceptedSequence=103 guardCount=2 forceSyncReadback=True',
            'GameObserveFrames game=UT2003 count=198 sequence=102-300 time=2026-07-29T00:00:00.0000000Z-2026-07-29T00:03:00.0000000Z',
            'UT2003VisualCoverage valid=True selected=8 decoded=8 canonicalDistinctHashes=8 requiredCanonicalDistinctHashes=4 meaningfulTransitions=4 requiredMeaningfulTransitions=3 minimumChangedPercent=0.05 maximumChangedPercent=90 spanSeconds=180 requiredSpanSeconds=150 maximumGapSeconds=30 maximumAllowedGapSeconds=45',
            'GameObserveVisual=recorded game=UT2003 frame=f geometry=640x480 geometryFrames=8 distance=0 score=1 blackPercent=1 colors=9 desktop=False',
            'ReviewVideo=x frames=675 fps=15 seconds=45 sourceSize=640x480',
            'ReviewVideoArtifact valid=True sources=1 artifacts=1 prefix=x') -join "`n"
    } elseif ($Mode -eq 'UT2003Gameplay') {
        $console = @(
            'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=semantic transition=dm-antalus-ready evidence=browse-load-deathmatch-level-play',
            'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=module name=d3d8.dll path=C:\WINDOWS\system32\d3d8.dll system32=1',
            'GuestInput=mouse-left ok=True',
            'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=1 injected=2 foreground=1 ready=1',
            'GuestInput=mouse-left ok=True',
            'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=2 injected=2 foreground=1 ready=1',
            'GuestInput=mouse-left ok=True',
            'GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=3 injected=2 foreground=1 ready=1',
            'GameObserveFrames game=UT2003Gameplay count=68 sequence=72-140 time=2026-08-01T00:00:00.0000000Z-2026-08-01T00:00:35.0000000Z',
            'GameObserveVisual=recorded game=UT2003Gameplay frame=f geometry=640x480 geometryFrames=68 distance=1 score=2 blackPercent=1 colors=9 desktop=False',
            'ReviewVideo=x frames=525 fps=15 seconds=35 sourceSize=640x480',
            'ReviewVideoArtifact valid=True sources=1 artifacts=1 prefix=x',
            'GuestResult=QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003gameplay stage=complete outcome=observed exit=259 modules=05 observed-ms=49078') -join "`n"
    } else {
        $cadenceName = "$prefix-black-cadence.json"
        $cadencePath = Join-Path $Directory $cadenceName
        [IO.File]::WriteAllText($cadencePath,
            (([pscustomobject][ordered]@{
                frames = 256
                classes = 'V' * 256
                durationSeconds = 8.6
            } | ConvertTo-Json) + "`n"))
        $hash = (Get-FileHash -LiteralPath $cadencePath `
            -Algorithm SHA256).Hash
        $records.Add([pscustomobject][ordered]@{
            name = $cadenceName
            size = (Get-Item -LiteralPath $cadencePath).Length
            sha256 = $hash
        })
        $console = @(
            'Warcraft3D3D8FrameRing result=pass frames=256 durationSeconds=8.6 visible=256 dark=0 black=0 isolatedBlack=0 interiorBlackRuns=0 vvb=0',
            'GameObserveVisual=recorded game=Warcraft3D3D8 frame=f geometry=unknown geometryFrames=256 distance=1 score=2 blackPercent=1 colors=9 desktop=False',
            'ReviewVideo=x sourceFrames=256 encodedFrames=256 fps=30 seconds=8.533 sourceSize=800x600 nonDecimated=True',
            'ReviewVideoArtifact valid=True sources=1 artifacts=1 prefix=x') -join "`n"
    }
    $consolePath = Join-Path $Directory 'console.log'
    [IO.File]::WriteAllText(
        $consolePath, $console, [Text.Encoding]::Unicode)
    $records.Add([pscustomobject][ordered]@{
        name = 'console.log'
        size = (Get-Item -LiteralPath $consolePath).Length
        sha256 = (Get-FileHash -LiteralPath $consolePath `
            -Algorithm SHA256).Hash
    })
    [IO.File]::WriteAllText(
        (Join-Path $Directory 'artifact-manifest.json'),
        (([pscustomobject][ordered]@{
            runId = $runId
            files = @($records)
        } | ConvertTo-Json -Depth 5) + "`n"))
    return $videoName
}

function New-MaxPayneVisualBundleFixture {
    param(
        [string]$Directory,
        [string]$ImagePrefix = 'fixture-image',
        [switch]$NonBitmapBytes
    )

    [void](New-Item -ItemType Directory -Force -Path $Directory)
    $taskId = 'fixture-MaxPayneVisual'
    $records = [Collections.Generic.List[object]]::new()
    $images = [Collections.Generic.List[object]]::new()
    Add-Type -AssemblyName System.Drawing
    for ($index = 1; $index -le 3; ++$index) {
        $name = "$ImagePrefix-0$index.bmp"
        $path = Join-Path $Directory $name
        $width = 0
        $height = 0
        if ($NonBitmapBytes) {
            [IO.File]::WriteAllText($path, "maxpayne-image-$index")
        } else {
            $bitmap = [Drawing.Bitmap]::new(640, 480)
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                for ($by = 0; $by -lt 12; ++$by) {
                    for ($bx = 0; $bx -lt 16; ++$bx) {
                        $brush = [Drawing.SolidBrush]::new(
                            [Drawing.Color]::FromArgb(
                                40 + (($index * 17 + $bx * 11 + $by * 3) % 190),
                                35 + (($index * 7 + $bx * 5 + $by * 13) % 190),
                                30 + (($index * 3 + $bx * 17 + $by * 7) % 190)))
                        try {
                            $graphics.FillRectangle($brush,
                                $bx * 40, $by * 40, 40, 40)
                        } finally {
                            $brush.Dispose()
                        }
                    }
                }
                $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Bmp)
                $width = $bitmap.Width
                $height = $bitmap.Height
            } finally {
                $graphics.Dispose()
                $bitmap.Dispose()
            }
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $size = (Get-Item -LiteralPath $path).Length
        $records.Add([pscustomobject][ordered]@{
            name = $name
            size = $size
            sha256 = $hash
        })
        $images.Add([pscustomobject][ordered]@{
            index = $index
            fileName = $name
            sourceSequence = 100 + $index
            capturedUtc = '2026-07-29T00:00:00Z'
            hostOffsetMs = 1000 * $index
            bytes = $size
            sha256 = $hash
            width = $width
            height = $height
        })
    }
    $sidecar = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-maxpayne-visual-sequence'
        status = 'pass'
        valid = $true
        findings = @()
        bindings = [pscustomobject][ordered]@{
            harnessResult = 'harness-result.json'
            taskId = $taskId
            runId = $taskId
            tier = 'T3'
            mode = '-xpdm-d3d-gameplay'
        }
        selection = [pscustomobject][ordered]@{
            method = 'three-evenly-spaced-decodable-frames'
            automatedScope = 'capture-presence-only'
            requiredImages = 3
            decodableFrameCount = 3
            selectedCount = 3
        }
        images = @($images)
        assessment = [pscustomobject][ordered]@{
            automated = [pscustomobject][ordered]@{
                status = 'pass'
                scope = 'capture-presence-only'
                renderingCorrectness = 'not-assessed'
            }
            review = [pscustomobject][ordered]@{
                status = 'pending'
                verdict = $null
            }
        }
    }
    [IO.File]::WriteAllText(
        (Join-Path $Directory 'maxpayne-visual-sequence.json'),
        ($sidecar | ConvertTo-Json -Depth 12))
    $result = [pscustomobject][ordered]@{
        taskId = $taskId
        runId = $taskId
        tier = 'T3'
        mode = '-xpdm-d3d-gameplay'
        result = 'pass'
        harnessStatus = 'passed'
        exitCode = 0
        teardownOk = $true
        capture = $true
        captureMode = 'visual'
        visualEvidence = $sidecar
    }
    $harnessPath = Join-Path $Directory 'harness-result.json'
    [IO.File]::WriteAllText(
        $harnessPath, ($result | ConvertTo-Json -Depth 12))
    $records.Add([pscustomobject][ordered]@{
        name = 'harness-result.json'
        size = (Get-Item -LiteralPath $harnessPath).Length
        sha256 = (Get-FileHash -LiteralPath $harnessPath `
            -Algorithm SHA256).Hash
    })
    [IO.File]::WriteAllText(
        (Join-Path $Directory 'artifact-manifest.json'),
        (([pscustomobject][ordered]@{
            runId = $taskId
            files = @($records)
        } | ConvertTo-Json -Depth 5) + "`n"))
    return $sidecar
}
