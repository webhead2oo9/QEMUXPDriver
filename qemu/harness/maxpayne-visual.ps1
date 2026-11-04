# Test-XpMaxPayneVisualBundle confines every artifact read through
# Resolve-XpBundleConfinedPath, which lives in game-observe-visual.ps1.
# Dot-source idempotently: loading this file directly (its dedicated tests)
# must yield the same functions as loading it after game-observe-visual.ps1.
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')

function ConvertTo-XpMaxPayneCanonicalJson {
    param([psobject]$Value)

    if ($null -eq $Value) { return 'null' }
    return ($Value | ConvertTo-Json -Depth 16 -Compress)
}

function Test-XpMaxPayneSuccessfulResult {
    param([string]$Message)

    return -not [string]::IsNullOrWhiteSpace($Message) -and
        $Message -cmatch '^QXP_D3D_GAME_RESULT rc=0(?:\s|$)'
}

function New-XpMaxPayneVisualBoundaryState {
    [pscustomobject][ordered]@{
        started = $false
        startedAt = [DateTime]::MinValue
        startSequence = -1
        completed = $false
        completedAt = [DateTime]::MaxValue
        endSequence = -1
    }
}

function Set-XpMaxPayneVisualStartBoundary {
    param([psobject]$State, [DateTime]$At, [int]$Sequence)

    if (-not $State -or $State.started) { return $false }
    $State.started = $true
    $State.startedAt = $At
    $State.startSequence = $Sequence
    return $true
}

function Set-XpMaxPayneVisualCompletionBoundary {
    param([psobject]$State, [DateTime]$At, [int]$Sequence)

    if (-not $State -or -not $State.started -or $State.completed) {
        return $false
    }
    $State.completed = $true
    $State.completedAt = $At
    $State.endSequence = $Sequence
    return $true
}

function Get-XpMaxPayneFrameIdentity {
    param([IO.FileInfo]$Frame)

    if (-not $Frame -or
        $Frame.Name -notmatch '^frame_(\d+)x(\d+)_(\d{6})\.bmp$') {
        return $null
    }
    [pscustomobject][ordered]@{
        frame = $Frame
        width = [int]$Matches[1]
        height = [int]$Matches[2]
        sequence = [int]$Matches[3]
        capturedUtc = $Frame.LastWriteTimeUtc
    }
}

function Test-XpMaxPayneFrameDecodable {
    param([psobject]$Identity)

    try {
        Add-Type -AssemblyName System.Drawing
        $bitmap = [Drawing.Bitmap]::new($Identity.frame.FullName)
        try {
            return $bitmap.Width -eq $Identity.width -and
                $bitmap.Height -eq $Identity.height
        } finally {
            $bitmap.Dispose()
        }
    } catch {
        return $false
    }
}

function Select-XpMaxPayneTutorialVisualSequence {
    param(
        [IO.FileInfo[]]$Frames,
        [DateTime]$StartedAt,
        [DateTime]$CompletedAt,
        [int]$StartSequenceExclusive,
        [int]$EndSequenceInclusive,
        [string]$OutputDirectory
    )

    $findings = [Collections.Generic.List[string]]::new()
    $sceneObserved = $StartedAt -gt [DateTime]::MinValue -and
        $CompletedAt -lt [DateTime]::MaxValue -and
        $CompletedAt -gt $StartedAt
    $startedUtc = $StartedAt.ToUniversalTime()
    $completedUtc = $CompletedAt.ToUniversalTime()
    $durationMs = ($completedUtc - $startedUtc).TotalMilliseconds
    if (-not $sceneObserved) {
        $findings.Add('tutorial scene boundaries were not observed')
    }
    if ($durationMs -le 0) {
        $findings.Add('tutorial capture interval is empty or reversed')
    }
    if ($EndSequenceInclusive -lt 0) {
        $findings.Add('tutorial capture end sequence is unavailable')
    }
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Visual output directory does not exist: $OutputDirectory"
    }

    $identified = if ($sceneObserved) {
        @($Frames | ForEach-Object {
            $identity = Get-XpMaxPayneFrameIdentity -Frame $_
            if ($identity -and
                $identity.sequence -gt $StartSequenceExclusive -and
                ($EndSequenceInclusive -lt 0 -or
                 $identity.sequence -le $EndSequenceInclusive) -and
                $identity.capturedUtc -ge $startedUtc.AddSeconds(-1) -and
                $identity.capturedUtc -le $completedUtc.AddSeconds(2)) {
                $identity
            }
        })
    } else { @() }
    $geometryGroups = @($identified | Group-Object {
        '{0}x{1}' -f $_.width, $_.height
    } | ForEach-Object {
        $parts = $_.Name -split 'x'
        [pscustomobject]@{
            name = $_.Name
            count = $_.Count
            width = [int]$parts[0]
            height = [int]$parts[1]
            area = [int64]$parts[0] * [int64]$parts[1]
            frames = @($_.Group)
        }
    } | Sort-Object -Property `
        @{ Expression = 'count'; Descending = $true },
        @{ Expression = 'area'; Descending = $true },
        @{ Expression = 'name'; Descending = $false })
    $dominant = @($geometryGroups | Select-Object -First 1)
    if (-not $dominant.Count) {
        $findings.Add('no tutorial frames matched the scene window')
    }

    $decodable = if ($dominant.Count) {
        @($dominant[0].frames | Where-Object {
            Test-XpMaxPayneFrameDecodable -Identity $_
        } | Sort-Object capturedUtc, sequence)
    } else { @() }
    if ($decodable.Count -lt 3) {
        $findings.Add(
            "visual sequence has $($decodable.Count) decodable frames; 3 are required")
    }

    $selected = [Collections.Generic.List[object]]::new()
    if ($decodable.Count -ge 3 -and $durationMs -gt 0) {
        foreach ($sample in @(
                @{ Index = 1; Fraction = 1.0 / 6.0 },
                @{ Index = 2; Fraction = 1.0 / 2.0 },
                @{ Index = 3; Fraction = 5.0 / 6.0 })) {
            $sourceIndex = [int][Math]::Round(
                ($decodable.Count - 1) * $sample.Fraction)
            $chosen = $decodable[$sourceIndex]
            $offsetMs = [int64][Math]::Round(
                ($chosen.capturedUtc - $startedUtc).TotalMilliseconds)
            # Keep overload resolution on Int64 when a failed guest leaves
            # DateTime sentinel boundaries in place. The Int32 overload cannot
            # represent the resulting diagnostic-scale offset.
            $offsetMs = [Math]::Max(
                [int64]0,
                [Math]::Min([int64][Math]::Round($durationMs), $offsetMs))
            $digest = (Get-FileHash -LiteralPath $chosen.frame.FullName `
                -Algorithm SHA256).Hash
            $rawName = ('maxpayne-tutorial-{0:D2}-t{1}-h{2:D9}-q{3:D6}-{4}.bmp' -f
                $sample.Index,
                $chosen.capturedUtc.ToString('yyyyMMddTHHmmssfffZ'),
                $offsetMs, $chosen.sequence,
                $digest.Substring(0, 12).ToLowerInvariant())
            $rawPath = Join-Path $OutputDirectory $rawName
            Copy-Item -LiteralPath $chosen.frame.FullName `
                -Destination $rawPath -Force
            $item = Get-Item -LiteralPath $rawPath
            $selected.Add([pscustomobject][ordered]@{
                index = $sample.Index
                sourceSequence = $chosen.sequence
                capturedUtc = $chosen.capturedUtc.ToString('o')
                hostOffsetMs = $offsetMs
                bytes = $item.Length
                sha256 = (Get-FileHash -LiteralPath $rawPath `
                    -Algorithm SHA256).Hash
                width = $chosen.width
                height = $chosen.height
                rawPath = $rawPath
                fileName = $rawName
            })
        }
    }

    $valid = $findings.Count -eq 0 -and $selected.Count -eq 3
    [pscustomobject][ordered]@{
        valid = $valid
        status = if ($valid) { 'pass' } else { 'fail' }
        findings = @($findings)
        startedUtc = $startedUtc.ToString('o')
        completedUtc = $completedUtc.ToString('o')
        hostObservedDurationMs = [int64][Math]::Round($durationMs)
        startSequenceExclusive = $StartSequenceExclusive
        endSequenceInclusive = $EndSequenceInclusive
        dominantGeometry = if ($dominant.Count) {
            [pscustomobject][ordered]@{
                width = $dominant[0].width
                height = $dominant[0].height
                candidateCount = $dominant[0].count
            }
        } else { $null }
        decodableFrameCount = $decodable.Count
        requiredImages = 3
        selectedCount = $selected.Count
        images = @($selected)
    }
}

function New-XpMaxPayneVisualEvidence {
    param([psobject]$Selection, [psobject]$Context, [string]$Mode)

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-maxpayne-visual-sequence'
        status = $Selection.status
        valid = [bool]$Selection.valid
        findings = @($Selection.findings)
        bindings = [pscustomobject][ordered]@{
            harnessResult = 'harness-result.json'
            taskId = $Context.TaskId
            runId = $Context.RunId
            tier = $Context.Tier
            mode = $Mode
        }
        scene = [pscustomobject][ordered]@{
            game = 'MaxPayne'
            name = 'tutorial'
            warmupMs = 15000
            guestDurationMs = 45000
            startedUtc = $Selection.startedUtc
            completedUtc = $Selection.completedUtc
            hostObservedDurationMs = $Selection.hostObservedDurationMs
            startSequenceExclusive = $Selection.startSequenceExclusive
            endSequenceInclusive = $Selection.endSequenceInclusive
            dominantGeometry = $Selection.dominantGeometry
        }
        selection = [pscustomobject][ordered]@{
            method = 'three-evenly-spaced-decodable-frames'
            automatedScope = 'capture-presence-only'
            requiredImages = 3
            decodableFrameCount = $Selection.decodableFrameCount
            selectedCount = $Selection.selectedCount
        }
        images = @($Selection.images | ForEach-Object {
            [pscustomobject][ordered]@{
                index = $_.index
                fileName = $_.fileName
                sourceSequence = $_.sourceSequence
                capturedUtc = $_.capturedUtc
                hostOffsetMs = $_.hostOffsetMs
                bytes = $_.bytes
                sha256 = $_.sha256
                width = $_.width
                height = $_.height
            }
        })
        assessment = [pscustomobject][ordered]@{
            automated = [pscustomobject][ordered]@{
                status = $Selection.status
                scope = 'capture-presence-only'
                renderingCorrectness = 'not-assessed'
            }
            review = [pscustomobject][ordered]@{
                status = 'pending'
                verdict = $null
            }
        }
    }
}

function Test-XpMaxPayneVisualBundle {
    param([string]$Directory)

    $findings = [Collections.Generic.List[string]]::new()
    $fixedPaths = @{}
    foreach ($fixedName in @(
            'harness-result.json',
            'maxpayne-visual-sequence.json',
            'artifact-manifest.json')) {
        $resolved = Resolve-XpBundleConfinedPath `
            -BundleRoot $Directory -Name $fixedName
        if (-not $resolved) {
            $findings.Add("$fixedName is a reparse point or escapes the canonical bundle")
            continue
        }
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            $findings.Add("visual bundle is missing $fixedName")
            continue
        }
        $fixedPaths[$fixedName] = $resolved
    }
    $result = $sidecar = $manifest = $null
    if (-not $findings.Count) {
        try { $result = Get-Content -Raw `
            -LiteralPath $fixedPaths['harness-result.json'] | ConvertFrom-Json }
        catch { $findings.Add('harness-result.json is invalid') }
        try { $sidecar = Get-Content -Raw `
            -LiteralPath $fixedPaths['maxpayne-visual-sequence.json'] |
            ConvertFrom-Json }
        catch { $findings.Add('maxpayne-visual-sequence.json is invalid') }
        try { $manifest = Get-Content -Raw `
            -LiteralPath $fixedPaths['artifact-manifest.json'] | ConvertFrom-Json }
        catch { $findings.Add('artifact-manifest.json is invalid') }
    }
    if ($result -and $sidecar) {
        if ((ConvertTo-XpMaxPayneCanonicalJson $result.visualEvidence) -cne
            (ConvertTo-XpMaxPayneCanonicalJson $sidecar)) {
            $findings.Add('visual sidecar differs from harness result evidence')
        }
        if ([string]$result.result -cne 'pass' -or
            [string]$result.harnessStatus -cne 'passed' -or
            -not [bool]$result.teardownOk -or -not [bool]$result.capture -or
            [string]$result.captureMode -cne 'visual' -or
            -not [bool]$sidecar.valid -or
            @($sidecar.images).Count -ne 3) {
            $findings.Add('visual result, capture policy, teardown, or image count did not pass')
        }
        if ([string]$sidecar.bindings.taskId -cne [string]$result.taskId -or
            [string]$sidecar.bindings.runId -cne [string]$result.runId -or
            [string]$sidecar.bindings.tier -cne [string]$result.tier -or
            [string]$sidecar.bindings.mode -cne [string]$result.mode) {
            $findings.Add('visual evidence identity differs from harness result')
        }
    }
    if ($sidecar) {
        $sidecarImages = @($sidecar.images)
        for ($imageIndex = 0; $imageIndex -lt $sidecarImages.Count; ++$imageIndex) {
            if ([int]$sidecarImages[$imageIndex].index -ne ($imageIndex + 1)) {
                $findings.Add('visual image indices are not exactly 1 through 3 in order')
                break
            }
        }
        $uniqueNames = @{}
        $uniqueHashes = @{}
        $previousSequence = -1
        foreach ($image in $sidecarImages) {
            $nameKey = [string]$image.fileName
            if ($uniqueNames.ContainsKey($nameKey.ToLowerInvariant())) {
                $findings.Add("visual image file name is not unique: $($image.fileName)")
            }
            $uniqueNames[$nameKey.ToLowerInvariant()] = $true
            $hashKey = [string]$image.sha256
            if ($uniqueHashes.ContainsKey($hashKey)) {
                $findings.Add("visual image content is not unique: $($image.fileName)")
            }
            $uniqueHashes[$hashKey] = $true
            if ([int]$image.sourceSequence -le $previousSequence) {
                $findings.Add('visual image source sequences are not strictly increasing')
            }
            $previousSequence = [int]$image.sourceSequence
        }
    }
    if ($sidecar -and $manifest) {
        $expectedGeometry = $null
        foreach ($image in @($sidecar.images)) {
            $path = Resolve-XpBundleConfinedPath `
                -BundleRoot $Directory -Name ([string]$image.fileName)
            if (-not $path) {
                $findings.Add("visual image is a reparse point or escapes the canonical bundle: $($image.fileName)")
                continue
            }
            $record = @($manifest.files | Where-Object {
                [string]$_.name -ceq [string]$image.fileName
            })
            if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
                $record.Count -ne 1) {
                $findings.Add("visual image is missing: $($image.fileName)")
                continue
            }
            $item = Get-Item -LiteralPath $path
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($item.Length -ne [int64]$image.bytes -or
                $hash -cne [string]$image.sha256 -or
                [int64]$record[0].size -ne [int64]$image.bytes -or
                [string]$record[0].sha256 -cne [string]$image.sha256) {
                $findings.Add("visual image identity mismatch: $($image.fileName)")
            }
            $decodedWidth = 0
            $decodedHeight = 0
            try {
                Add-Type -AssemblyName System.Drawing
                $bitmap = [Drawing.Bitmap]::new($path)
                try {
                    $decodedWidth = $bitmap.Width
                    $decodedHeight = $bitmap.Height
                } finally {
                    $bitmap.Dispose()
                }
            } catch {
                $findings.Add("visual image is not a decodable bitmap: $($image.fileName)")
                continue
            }
            if ($decodedWidth -le 0 -or $decodedHeight -le 0 -or
                [int]$image.width -ne $decodedWidth -or
                [int]$image.height -ne $decodedHeight) {
                $findings.Add("visual image geometry does not match its decoded bitmap: $($image.fileName)")
                continue
            }
            $geometry = "${decodedWidth}x${decodedHeight}"
            if ($null -eq $expectedGeometry) {
                $expectedGeometry = $geometry
            } elseif ($expectedGeometry -cne $geometry) {
                $findings.Add('visual images do not share one decoded geometry')
            }
        }
    }
    [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        findings = @($findings)
        evidence = $sidecar
    }
}
