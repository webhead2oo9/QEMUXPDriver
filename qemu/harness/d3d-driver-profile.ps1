$ErrorActionPreference = 'Stop'

function ConvertFrom-XpD3dProfileHex32 {
    param([string]$Text)

    if ($Text -notmatch '^0x([0-9A-Fa-f]{8})$') {
        throw "Invalid XPDriver profile word '$Text'"
    }
    return [Convert]::ToUInt32($Matches[1], 16)
}

function Join-XpD3dProfileWords {
    param([uint32[]]$Words)

    if ($Words.Count -eq 1) { return [uint64]$Words[0] }
    if ($Words.Count -ne 2) {
        throw "An XPDriver profile value must contain one or two words"
    }
    return ([uint64]$Words[0] * [uint64]4294967296) +
        [uint64]$Words[1]
}

function Get-XpD3dProfileBlock {
    param(
        [string[]]$Lines,
        [int]$StartIndex,
        [int]$EndIndex,
        [object[]]$Fields,
        [string]$Kind
    )

    $values = [ordered]@{}
    $present = [ordered]@{}
    $rawPresent = [ordered]@{}
    foreach ($field in $Fields) {
        $rawPresent[$field.Name] = $false
        for ($index = $StartIndex; $index -lt $EndIndex; ++$index) {
            if ($Lines[$index] -eq $field.Label) {
                $rawPresent[$field.Name] = $true
                break
            }
        }
    }
    $cursor = $StartIndex
    $failure = $null
    $counterFailure = $null
    foreach ($field in $Fields) {
        $labelIndex = -1
        for ($index = $cursor; $index -lt $EndIndex; ++$index) {
            if ($Lines[$index] -eq $field.Label) {
                $labelIndex = $index
                break
            }
        }
        if ($labelIndex -lt 0) {
            $present[$field.Name] = $false
            if ($field.Optional) {
                $values[$field.Name] = [uint64]0
                continue
            }
            $failure = "missing label '$($field.Label)'"
            if (-not $field.Profile) { $counterFailure = $failure }
            break
        }

        $words = [Collections.Generic.List[uint32]]::new()
        $index = $labelIndex + 1
        while ($index -lt $EndIndex -and $words.Count -lt $field.Words) {
            $line = $Lines[$index]
            if ($line -match '^0x[0-9A-Fa-f]{8}$') {
                $words.Add((ConvertFrom-XpD3dProfileHex32 -Text $line))
            } elseif ($line -ne 'QXPMP STATUS' -and
                      ($line.StartsWith('QXPMP D3D ') -or
                       $line.StartsWith('QXPDISP PERF '))) {
                break
            }
            ++$index
        }
        if ($words.Count -ne $field.Words) {
            $failure = "label '$($field.Label)' has $($words.Count) of $($field.Words) words"
            if (-not $field.Profile) { $counterFailure = $failure }
            break
        }
        $values[$field.Name] = Join-XpD3dProfileWords -Words $words.ToArray()
        $present[$field.Name] = $true
        $cursor = $index
    }

    # PERF tick labels exist only in QXP_D3D_PROFILE builds; their presence
    # anywhere in the block decides whether this session owes the full
    # profile contract or only the always-on counter dump.
    $hasProfileLabels = $false
    for ($index = $StartIndex; $index -lt $EndIndex; ++$index) {
        if ($Lines[$index].StartsWith('QXPMP D3D PERF ') -or
            $Lines[$index].StartsWith('QXPDISP PERF ')) {
            $hasProfileLabels = $true
            break
        }
    }

    return [pscustomobject][ordered]@{
        kind = $Kind
        startLine = $StartIndex + 1
        endLine = $EndIndex
        complete = $null -eq $failure
        counterComplete = $null -eq $counterFailure
        hasProfileLabels = $hasProfileLabels
        failure = $failure
        values = [pscustomobject]$values
        present = [pscustomobject]$present
        rawPresent = [pscustomobject]$rawPresent
    }
}

function Get-XpD3dProfileBlocks {
    param(
        [string[]]$Lines,
        [object[]]$Fields,
        [string]$Kind
    )

    $starts = [Collections.Generic.List[int]]::new()
    for ($index = 0; $index -lt $Lines.Count; ++$index) {
        if ($Lines[$index] -eq $Fields[0].Label) { $starts.Add($index) }
    }
    $blocks = [Collections.Generic.List[object]]::new()
    for ($blockIndex = 0; $blockIndex -lt $starts.Count; ++$blockIndex) {
        $end = if ($blockIndex + 1 -lt $starts.Count) {
            $starts[$blockIndex + 1]
        } else {
            $Lines.Count
        }
        $blocks.Add((Get-XpD3dProfileBlock -Lines $Lines `
            -StartIndex $starts[$blockIndex] -EndIndex $end `
            -Fields $Fields -Kind $Kind))
    }
    return @($blocks)
}

function Get-XpD3dTimelineRecord {
    param([object]$Block)

    $names = @(
        'timelineVersion', 'dp2SpanTicks', 'dp2GapCount',
        'dp2GapTicks', 'dp2GapMaxTicks', 'dp2OverlapCount',
        'dp2GapLt1ms', 'dp2Gap1To10ms', 'dp2Gap10To50ms',
        'dp2Gap50To100ms', 'dp2Gap100To250ms', 'dp2GapGe250ms')
    $value = $Block.values
    $present = @($names | Where-Object {
        [bool]$Block.rawPresent.$_
    }).Count -gt 0
    $complete = if ($present) {
        -not @($names | Where-Object {
            -not [bool]$Block.present.$_
        }).Count
    } else { $null }
    $versionLabelPresent = [bool]$Block.rawPresent.timelineVersion
    $versionPresent = [bool]$Block.present.timelineVersion
    $usable = $present -and $complete -and $versionPresent -and
        $value.timelineVersion -eq 1
    $buckets = [pscustomobject][ordered]@{
        lt1ms = $value.dp2GapLt1ms
        from1To10ms = $value.dp2Gap1To10ms
        from10To50ms = $value.dp2Gap10To50ms
        from50To100ms = $value.dp2Gap50To100ms
        from100To250ms = $value.dp2Gap100To250ms
        ge250ms = $value.dp2GapGe250ms
    }
    $timelineFindings = [Collections.Generic.List[string]]::new()
    if ($present) {
        if (-not $versionLabelPresent) {
            $timelineFindings.Add('display DP2 timeline version label is missing')
        } elseif ($versionPresent -and $value.timelineVersion -ne 1) {
            $timelineFindings.Add('display DP2 timeline version is unsupported')
        }
        if (-not $complete) {
            $timelineFindings.Add('display DP2 timeline block is incomplete')
        }
    }
    if ($usable) {
        $bucketCount = [uint64]$buckets.lt1ms +
            [uint64]$buckets.from1To10ms +
            [uint64]$buckets.from10To50ms +
            [uint64]$buckets.from50To100ms +
            [uint64]$buckets.from100To250ms +
            [uint64]$buckets.ge250ms
        $maximumBoundaries = if ($value.dp2Calls) {
            [uint64]$value.dp2Calls - 1
        } else { [uint64]0 }
        if ($bucketCount -ne $value.dp2GapCount) {
            $timelineFindings.Add(
                'display DP2 gap buckets do not match gap count')
        }
        if ($value.dp2GapCount -gt $maximumBoundaries) {
            $timelineFindings.Add(
                'display DP2 gap count exceeds call boundaries')
        }
        if ($value.dp2OverlapCount -gt $maximumBoundaries -or
            [uint64]$value.dp2GapCount +
                [uint64]$value.dp2OverlapCount -gt $maximumBoundaries) {
            $timelineFindings.Add(
                'display DP2 overlap and gap counts exceed call boundaries')
        }
        if (-not $value.dp2GapCount) {
            if ($value.dp2GapTicks -or $value.dp2GapMaxTicks) {
                $timelineFindings.Add(
                    'display DP2 zero gap count has nonzero gap ticks')
            }
        } else {
            if (-not $value.dp2GapTicks -or -not $value.dp2GapMaxTicks) {
                $timelineFindings.Add(
                    'display DP2 positive gap count has zero gap ticks')
            }
            if ($value.dp2GapTicks -lt $value.dp2GapCount) {
                $timelineFindings.Add(
                    'display DP2 gap ticks cannot contain every positive gap')
            } else {
                $minimumMaximum = [uint64][decimal]::Floor(
                    [decimal]$value.dp2GapTicks /
                        [decimal]$value.dp2GapCount)
                if ($value.dp2GapTicks % $value.dp2GapCount) {
                    ++$minimumMaximum
                }
                $maximumMaximum = $value.dp2GapTicks -
                    $value.dp2GapCount + 1
                if ($value.dp2GapMaxTicks -lt $minimumMaximum -or
                    $value.dp2GapMaxTicks -gt $maximumMaximum) {
                    $timelineFindings.Add(
                        'display DP2 maximum gap is infeasible for total and count')
                }
            }
        }
        if ($value.dp2GapTicks -gt $value.dp2SpanTicks) {
            $timelineFindings.Add(
                'display DP2 gap ticks exceed callback span')
        }
        if (-not $value.dp2Calls -and
            ($value.dp2Ticks -or $value.dp2SpanTicks -or
             $value.dp2GapCount -or $value.dp2OverlapCount)) {
            $timelineFindings.Add(
                'display DP2 timeline has activity with zero calls')
        }
        if (-not $value.dp2OverlapCount -and
            $value.dp2SpanTicks -ne
                $value.dp2Ticks + $value.dp2GapTicks) {
            $timelineFindings.Add(
                'display DP2 span does not equal active plus gap ticks')
        }
    }
    return [pscustomobject][ordered]@{
        startLine = $Block.startLine
        present = $present
        versionLabelPresent = $versionLabelPresent
        versionPresent = $versionPresent
        version = $value.timelineVersion
        complete = $complete
        usable = $usable
        calls = $value.dp2Calls
        spanTicks = $value.dp2SpanTicks
        gapCount = $value.dp2GapCount
        gapTicks = $value.dp2GapTicks
        gapMaxTicks = $value.dp2GapMaxTicks
        overlapCount = $value.dp2OverlapCount
        buckets = $buckets
        findings = @($timelineFindings)
    }
}

function ConvertFrom-XpD3dDriverProfile {
    param([AllowEmptyString()][string]$Text)

    $displayFields = @(
        [pscustomobject]@{ Name = 'dp2Calls'; Label = 'QXPDISP PERF DP2 CALLS'; Words = 1 },
        [pscustomobject]@{ Name = 'dp2CommandBytes'; Label = 'QXPDISP PERF DP2 COMMAND BYTES'; Words = 2 },
        [pscustomobject]@{ Name = 'dp2Ticks'; Label = 'QXPDISP PERF DP2 TICKS'; Words = 2 },
        [pscustomobject]@{ Name = 'timelineVersion'; Label = 'QXPDISP PERF DP2 TIMELINE VERSION'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2SpanTicks'; Label = 'QXPDISP PERF DP2 SPAN TICKS'; Words = 2; Optional = $true },
        [pscustomobject]@{ Name = 'dp2GapCount'; Label = 'QXPDISP PERF DP2 GAP COUNT'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2GapTicks'; Label = 'QXPDISP PERF DP2 GAP TICKS'; Words = 2; Optional = $true },
        [pscustomobject]@{ Name = 'dp2GapMaxTicks'; Label = 'QXPDISP PERF DP2 GAP MAX TICKS'; Words = 2; Optional = $true },
        [pscustomobject]@{ Name = 'dp2OverlapCount'; Label = 'QXPDISP PERF DP2 OVERLAP COUNT'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2GapLt1ms'; Label = 'QXPDISP PERF DP2 GAP LT 1MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2Gap1To10ms'; Label = 'QXPDISP PERF DP2 GAP 1-10MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2Gap10To50ms'; Label = 'QXPDISP PERF DP2 GAP 10-50MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2Gap50To100ms'; Label = 'QXPDISP PERF DP2 GAP 50-100MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2Gap100To250ms'; Label = 'QXPDISP PERF DP2 GAP 100-250MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'dp2GapGe250ms'; Label = 'QXPDISP PERF DP2 GAP GE 250MS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'drawIoctlCalls'; Label = 'QXPDISP PERF DRAW IOCTL CALLS'; Words = 1 },
        [pscustomobject]@{ Name = 'drawIoctlBytes'; Label = 'QXPDISP PERF DRAW IOCTL BYTES'; Words = 2 },
        [pscustomobject]@{ Name = 'drawIoctlTicks'; Label = 'QXPDISP PERF DRAW IOCTL TICKS'; Words = 2 },
        [pscustomobject]@{ Name = 'indexedCalls'; Label = 'QXPDISP PERF INDEXED CALLS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedRawPrelitCalls'; Label = 'QXPDISP PERF INDEXED RAW PRELIT CALLS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedRawEligibleCalls'; Label = 'QXPDISP PERF INDEXED RAW ELIGIBLE CALLS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedRawPackets'; Label = 'QXPDISP PERF INDEXED RAW PACKETS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedRawFallbackChunks'; Label = 'QXPDISP PERF INDEXED RAW FALLBACK CHUNKS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedCanonicalPackets'; Label = 'QXPDISP PERF INDEXED CANONICAL PACKETS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'frequency'; Label = 'QXPDISP PERF FREQUENCY'; Words = 2 }
    )
    $miniportFields = @(
        [pscustomobject]@{ Name = 'logicalDraws'; Label = 'QXPMP D3D DRAWS'; Words = 1 },
        # The three draw-IOCTL labels first appeared in the .250 batching
        # line; .240-era release dumps lack them, so they default to zero
        # instead of failing a legacy counter session.
        [pscustomobject]@{ Name = 'singleDrawIoctls'; Label = 'QXPMP D3D DRAW IOCTLS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'drawBatchIoctls'; Label = 'QXPMP D3D DRAW BATCH IOCTLS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'maximumDrawsPerIoctl'; Label = 'QXPMP D3D DRAW BATCH MAX'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'presents'; Label = 'QXPMP D3D PRESENTS'; Words = 1 },
        # Presentation-source counters were added after the original always-on
        # counter contract.  Legacy sessions may omit the complete group; a
        # modern session that emits any member owes all five and their sum.
        [pscustomobject]@{ Name = 'cpuPresents'; Label = 'QXPMP D3D PRESENTS CPU'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'surfaceTexturePresents'; Label = 'QXPMP D3D PRESENTS SURFTEX'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'emptyPresents'; Label = 'QXPMP D3D PRESENTS EMPTY'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'surfaceCopies'; Label = 'QXPMP D3D SURFACE COPIES'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'surfaceReadbacks'; Label = 'QXPMP D3D SURFACE READBACKS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'clears'; Label = 'QXPMP D3D CLEARS'; Words = 1 },
        [pscustomobject]@{ Name = 'uploads'; Label = 'QXPMP D3D UPLOADS'; Words = 1 },
        [pscustomobject]@{ Name = 'hostBatches'; Label = 'QXPMP D3D BATCHES'; Words = 1 },
        [pscustomobject]@{ Name = 'maximumDrawsPerHostBatch'; Label = 'QXPMP D3D BATCHMAX'; Words = 1 },
        [pscustomobject]@{ Name = 'fixedStateReuse'; Label = 'QXPMP D3D FIXEDREUSE'; Words = 1 },
        [pscustomobject]@{ Name = 'textureReuse'; Label = 'QXPMP D3D TEXREUSE'; Words = 1 },
        [pscustomobject]@{ Name = 'arrayReuse'; Label = 'QXPMP D3D ARRAYREUSE'; Words = 1 },
        [pscustomobject]@{ Name = 'interleavedDraws'; Label = 'QXPMP D3D INTERLEAVED'; Words = 1 },
        [pscustomobject]@{ Name = 'compactDraws'; Label = 'QXPMP D3D COMPACT'; Words = 1 },
        [pscustomobject]@{ Name = 'compactIndexedDraws'; Label = 'QXPMP D3D COMPACT INDEXED'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'canonicalIndexedDraws'; Label = 'QXPMP D3D INDEXED CANONICAL'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'indexedElementDraws'; Label = 'QXPMP D3D INDEXED ELEMENTS'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'rawIndexedObjectDraws'; Label = 'QXPMP D3D RAW OBJECT'; Words = 1; Optional = $true },
        [pscustomobject]@{ Name = 'drawTicks'; Label = 'QXPMP D3D PERF DRAW TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'packetTicks'; Label = 'QXPMP D3D PERF PACKET TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'prepareTicks'; Label = 'QXPMP D3D PERF PREPARE TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'stateTicks'; Label = 'QXPMP D3D PERF STATE TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'submitTicks'; Label = 'QXPMP D3D PERF SUBMIT TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'interleavedCopyTicks'; Label = 'QXPMP D3D PERF INTERLEAVED COPY TICKS'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'interleavedCopyBytes'; Label = 'QXPMP D3D PERF INTERLEAVED COPY BYTES'; Words = 2; Profile = $true },
        [pscustomobject]@{ Name = 'interleavedCopyCalls'; Label = 'QXPMP D3D PERF INTERLEAVED COPY CALLS'; Words = 1; Profile = $true },
        [pscustomobject]@{ Name = 'frequency'; Label = 'QXPMP D3D PERF FREQUENCY'; Words = 2; Profile = $true }
    )

    $lines = @($Text -split '\r?\n' | ForEach-Object { $_.Trim() })
    $displayBlocks = @(Get-XpD3dProfileBlocks -Lines $lines `
        -Fields $displayFields -Kind display)
    $miniportBlocks = @(Get-XpD3dProfileBlocks -Lines $lines `
        -Fields $miniportFields -Kind miniport)
    if (-not $displayBlocks.Count -and -not $miniportBlocks.Count) {
        return [pscustomobject][ordered]@{
            schema = 1
            kind = 'xpdriver-d3d-driver-profile'
            status = 'absent'
            valid = $false
            selection = [pscustomobject][ordered]@{
                rule = 'last-complete-paired-session'
                displayBlocks = 0
                miniportBlocks = 0
                selectedDisplayLine = $null
                selectedMiniportLine = $null
            }
            display = $null
            miniport = $null
            derived = $null
            findings = @('no XPDriver D3D profile blocks were present')
        }
    }

    # An ordinary (non-QXP_D3D_PROFILE) build dumps the always-on miniport
    # counters at transport release with no display block and no PERF tick
    # labels. That is a counters-only session, not an incomplete profile;
    # only a build that emitted any PERF label owes the paired-session
    # contract below.
    $lastMiniport = if ($miniportBlocks.Count) { $miniportBlocks[-1] } else { $null }
    if ($lastMiniport -and -not $displayBlocks.Count -and
            -not $lastMiniport.hasProfileLabels) {
        $counterFindings = [Collections.Generic.List[string]]::new()
        if (-not $lastMiniport.counterComplete) {
            $counterFindings.Add(
                "final miniport counter block is incomplete: $($lastMiniport.failure)")
        }
        $counterMiniport = $null
        $counterDerived = $null
        if ($lastMiniport.counterComplete) {
            $value = $lastMiniport.values
            $presentSourceNames = @(
                'cpuPresents', 'surfaceTexturePresents', 'emptyPresents',
                'surfaceCopies', 'surfaceReadbacks')
            $presentSourceCount = @($presentSourceNames | Where-Object {
                [bool]$lastMiniport.rawPresent.$_
            }).Count
            $presentSourceComplete =
                $presentSourceCount -eq $presentSourceNames.Count
            if ($presentSourceCount -and -not $presentSourceComplete) {
                $counterFindings.Add(
                    'final miniport presentation-source counter group is incomplete')
            } elseif ($presentSourceComplete -and
                      [uint64]$value.cpuPresents +
                          [uint64]$value.surfaceTexturePresents +
                          [uint64]$value.emptyPresents -ne
                              [uint64]$value.presents) {
                $counterFindings.Add(
                    'miniport presentation-source counts do not sum to presents')
            }
            $counterMiniport = [pscustomobject][ordered]@{
                startLine = $lastMiniport.startLine
                logicalDraws = $value.logicalDraws
                singleDrawIoctls = $value.singleDrawIoctls
                drawBatchIoctls = $value.drawBatchIoctls
                maximumDrawsPerIoctl = $value.maximumDrawsPerIoctl
                presents = $value.presents
                presentationSourceCountersPresent = $presentSourceComplete
                cpuPresents = $value.cpuPresents
                surfaceTexturePresents = $value.surfaceTexturePresents
                emptyPresents = $value.emptyPresents
                surfaceCopies = $value.surfaceCopies
                surfaceReadbacks = $value.surfaceReadbacks
                clears = $value.clears
                uploads = $value.uploads
                hostBatches = $value.hostBatches
                maximumDrawsPerHostBatch = $value.maximumDrawsPerHostBatch
                fixedStateReuse = $value.fixedStateReuse
                textureReuse = $value.textureReuse
                arrayReuse = $value.arrayReuse
                interleavedDraws = $value.interleavedDraws
                compactDraws = $value.compactDraws
                compactIndexedDraws = $value.compactIndexedDraws
                canonicalIndexedDraws = $value.canonicalIndexedDraws
                indexedElementDraws = $value.indexedElementDraws
                rawIndexedObjectDraws = $value.rawIndexedObjectDraws
            }
            if ($counterMiniport.interleavedDraws -gt
                    $counterMiniport.logicalDraws) {
                $counterFindings.Add('interleaved draw count exceeds logical draw count')
            }
            if ($counterMiniport.compactDraws -gt
                    $counterMiniport.interleavedDraws) {
                $counterFindings.Add('compact draw count exceeds interleaved draw count')
            }
            if ($counterMiniport.compactIndexedDraws -gt
                    $counterMiniport.compactDraws) {
                $counterFindings.Add('compact indexed draw count exceeds compact draw count')
            }
            if ($counterMiniport.canonicalIndexedDraws -gt
                    $counterMiniport.logicalDraws) {
                $counterFindings.Add(
                    'canonical indexed draw count exceeds logical draw count')
            }
            if ([uint64]$counterMiniport.indexedElementDraws -gt
                    [uint64]$counterMiniport.canonicalIndexedDraws +
                    [uint64]$counterMiniport.rawIndexedObjectDraws) {
                $counterFindings.Add(
                    'indexed element draw count exceeds canonical plus raw indexed draw count')
            }
            $counterIoctls = $counterMiniport.singleDrawIoctls +
                $counterMiniport.drawBatchIoctls
            $counterDerived = [pscustomobject][ordered]@{
                totalDrawIoctls = $counterIoctls
                averageDrawsPerIoctl = if ($counterIoctls) {
                    [Math]::Round($counterMiniport.logicalDraws /
                        [double]$counterIoctls, 9)
                } else { $null }
                interleavedPercent = if ($counterMiniport.logicalDraws) {
                    [Math]::Round(100.0 * $counterMiniport.interleavedDraws /
                        $counterMiniport.logicalDraws, 9)
                } else { $null }
                compactIndexedPercent = if ($counterMiniport.compactDraws) {
                    [Math]::Round(100.0 * $counterMiniport.compactIndexedDraws /
                        $counterMiniport.compactDraws, 9)
                } else { $null }
                canonicalIndexedPercent = if ($counterMiniport.logicalDraws) {
                    [Math]::Round(
                        100.0 * $counterMiniport.canonicalIndexedDraws /
                        $counterMiniport.logicalDraws, 9)
                } else { $null }
                rawIndexedObjectPercent = if ($counterMiniport.logicalDraws) {
                    [Math]::Round(
                        100.0 * $counterMiniport.rawIndexedObjectDraws /
                        $counterMiniport.logicalDraws, 9)
                } else { $null }
            }
        }
        return [pscustomobject][ordered]@{
            schema = 1
            kind = 'xpdriver-d3d-driver-profile'
            status = if ($counterFindings.Count) { 'incomplete' } else { 'counters' }
            valid = $counterFindings.Count -eq 0
            selection = [pscustomobject][ordered]@{
                rule = 'final-counter-session'
                displayBlocks = 0
                miniportBlocks = $miniportBlocks.Count
                selectedDisplayLine = $null
                selectedMiniportLine = $lastMiniport.startLine
            }
            display = $null
            miniport = $counterMiniport
            derived = $counterDerived
            findings = @($counterFindings)
        }
    }

    $findings = [Collections.Generic.List[string]]::new()
    $trailingDisplayBlocks = @()
    if ($lastMiniport) {
        $trailingDisplayBlocks = @($displayBlocks | Where-Object {
            $_.startLine -gt $lastMiniport.startLine
        })
        if ($trailingDisplayBlocks.Count) {
            $findings.Add('an unpaired display profile block follows the final miniport session')
        }
    }
    $selectedDisplay = $null
    $displayCandidates = @()
    if ($lastMiniport) {
        $previousMiniportLine = if ($miniportBlocks.Count -gt 1) {
            $miniportBlocks[-2].startLine
        } else { 0 }
        $displayCandidates = @($displayBlocks | Where-Object {
            $_.startLine -gt $previousMiniportLine -and
            $_.startLine -lt $lastMiniport.startLine
        })
        if ($displayCandidates.Count) { $selectedDisplay = $displayCandidates[-1] }
    }
    if (-not $lastMiniport) {
        $findings.Add('no miniport profile block was present')
    } elseif (-not $lastMiniport.complete) {
        $findings.Add("final miniport profile block is incomplete: $($lastMiniport.failure)")
    }
    if (-not $selectedDisplay) {
        $findings.Add('no display profile block pairs with the final miniport session')
    } else {
        foreach ($candidate in $displayCandidates) {
            if (-not $candidate.complete) {
                $findings.Add(
                    "paired display profile block at line $($candidate.startLine) is incomplete: $($candidate.failure)")
            }
        }
    }

    $completeDisplayCandidates = @($displayCandidates | Where-Object {
        $_.complete
    })
    # Profile builds emit cumulative snapshots from a live display context.
    # Collapse each nondecreasing run, including an identical terminal dump,
    # to its terminal snapshot; a counter reset starts a separate context.
    $terminalDisplayCandidates =
        [Collections.Generic.List[object]]::new()
    for ($candidateIndex = 0;
         $candidateIndex -lt $completeDisplayCandidates.Count;
         ++$candidateIndex) {
        $candidate = $completeDisplayCandidates[$candidateIndex]
        $continuation = $false
        if ($candidateIndex + 1 -lt $completeDisplayCandidates.Count) {
            $nextCandidate =
                $completeDisplayCandidates[$candidateIndex + 1]
            $continuation = $true
            foreach ($counterName in @(
                    'dp2Calls', 'drawIoctlCalls', 'indexedCalls')) {
                if ($nextCandidate.values.$counterName -lt
                        $candidate.values.$counterName) {
                    $continuation = $false
                    break
                }
            }
        }
        if (-not $continuation) {
            $terminalDisplayCandidates.Add($candidate)
        }
    }

    $validatedDisplayTimelines =
        @($completeDisplayCandidates | ForEach-Object {
            Get-XpD3dTimelineRecord -Block $_
        })
    $displayTimelines = @($terminalDisplayCandidates | ForEach-Object {
        Get-XpD3dTimelineRecord -Block $_
    })
    foreach ($timeline in $validatedDisplayTimelines) {
        foreach ($timelineFinding in $timeline.findings) {
            $findings.Add($timelineFinding)
        }
    }
    $displayAggregate = $null
    if ($completeDisplayCandidates.Count -eq $displayCandidates.Count -and
        $terminalDisplayCandidates.Count) {
        $aggregateDp2CommandBytes = [uint64]0
        $aggregateDp2Ticks = [uint64]0
        $aggregateDrawIoctlCalls = [uint64]0
        $aggregateDrawIoctlBytes = [uint64]0
        $aggregateDrawIoctlTicks = [uint64]0
        $aggregateIndexedCalls = [uint64]0
        $aggregateIndexedRawPrelitCalls = [uint64]0
        $aggregateIndexedRawEligibleCalls = [uint64]0
        $aggregateIndexedRawPackets = [uint64]0
        $aggregateIndexedRawFallbackChunks = [uint64]0
        $aggregateIndexedCanonicalPackets = [uint64]0
        foreach ($candidate in $terminalDisplayCandidates) {
            $aggregateDp2Calls += $candidate.values.dp2Calls
            $aggregateDp2CommandBytes += $candidate.values.dp2CommandBytes
            $aggregateDp2Ticks += $candidate.values.dp2Ticks
            $aggregateDrawIoctlCalls += $candidate.values.drawIoctlCalls
            $aggregateDrawIoctlBytes += $candidate.values.drawIoctlBytes
            $aggregateDrawIoctlTicks += $candidate.values.drawIoctlTicks
            $aggregateIndexedCalls += $candidate.values.indexedCalls
            $aggregateIndexedRawPrelitCalls +=
                $candidate.values.indexedRawPrelitCalls
            $aggregateIndexedRawEligibleCalls +=
                $candidate.values.indexedRawEligibleCalls
            $aggregateIndexedRawPackets +=
                $candidate.values.indexedRawPackets
            $aggregateIndexedRawFallbackChunks +=
                $candidate.values.indexedRawFallbackChunks
            $aggregateIndexedCanonicalPackets +=
                $candidate.values.indexedCanonicalPackets
        }
        $displayAggregate = [pscustomobject][ordered]@{
            contextCount = $terminalDisplayCandidates.Count
            startLines = @($terminalDisplayCandidates.startLine)
            dp2Calls = $aggregateDp2Calls
            dp2CommandBytes = $aggregateDp2CommandBytes
            dp2Ticks = $aggregateDp2Ticks
            drawIoctlCalls = $aggregateDrawIoctlCalls
            drawIoctlBytes = $aggregateDrawIoctlBytes
            drawIoctlTicks = $aggregateDrawIoctlTicks
            indexedCalls = $aggregateIndexedCalls
            indexedRawPrelitCalls = $aggregateIndexedRawPrelitCalls
            indexedRawEligibleCalls = $aggregateIndexedRawEligibleCalls
            indexedRawPackets = $aggregateIndexedRawPackets
            indexedRawFallbackChunks = $aggregateIndexedRawFallbackChunks
            indexedCanonicalPackets = $aggregateIndexedCanonicalPackets
        }
    }

    $display = if ($selectedDisplay -and $selectedDisplay.complete) {
        $value = $selectedDisplay.values
        $selectedTimeline = Get-XpD3dTimelineRecord -Block $selectedDisplay
        [pscustomobject][ordered]@{
            startLine = $selectedDisplay.startLine
            dp2Calls = $value.dp2Calls
            dp2CommandBytes = $value.dp2CommandBytes
            dp2Ticks = $value.dp2Ticks
            dp2Seconds = if ($value.frequency) {
                [Math]::Round($value.dp2Ticks / [double]$value.frequency, 9)
            } else { $null }
            aggregate = $displayAggregate
            timelines = @($displayTimelines)
            timelinePresent = $selectedTimeline.present
            timelineVersionLabelPresent = $selectedTimeline.versionLabelPresent
            timelineVersionPresent = $selectedTimeline.versionPresent
            timelineVersion = $selectedTimeline.version
            timelineComplete = $selectedTimeline.complete
            dp2SpanTicks = $selectedTimeline.spanTicks
            dp2GapCount = $selectedTimeline.gapCount
            dp2GapTicks = $selectedTimeline.gapTicks
            dp2GapMaxTicks = $selectedTimeline.gapMaxTicks
            dp2OverlapCount = $selectedTimeline.overlapCount
            dp2GapBuckets = $selectedTimeline.buckets
            drawIoctlCalls = $value.drawIoctlCalls
            drawIoctlBytes = $value.drawIoctlBytes
            drawIoctlTicks = $value.drawIoctlTicks
            indexedCalls = $value.indexedCalls
            indexedRawPrelitCalls = $value.indexedRawPrelitCalls
            indexedRawEligibleCalls = $value.indexedRawEligibleCalls
            indexedRawPackets = $value.indexedRawPackets
            indexedRawFallbackChunks = $value.indexedRawFallbackChunks
            indexedCanonicalPackets = $value.indexedCanonicalPackets
            drawIoctlSeconds = if ($value.frequency) {
                [Math]::Round($value.drawIoctlTicks / [double]$value.frequency, 9)
            } else { $null }
            frequency = $value.frequency
        }
    } else { $null }

    $presentSourceComplete = $false
    if ($lastMiniport -and $lastMiniport.complete) {
        $presentSourceNames = @(
            'cpuPresents', 'surfaceTexturePresents', 'emptyPresents',
            'surfaceCopies', 'surfaceReadbacks')
        $presentSourceCount = @($presentSourceNames | Where-Object {
            [bool]$lastMiniport.rawPresent.$_
        }).Count
        $presentSourceComplete =
            $presentSourceCount -eq $presentSourceNames.Count
        if ($presentSourceCount -and -not $presentSourceComplete) {
            $findings.Add(
                'final miniport presentation-source counter group is incomplete')
        } elseif ($presentSourceComplete -and
                  [uint64]$lastMiniport.values.cpuPresents +
                      [uint64]$lastMiniport.values.surfaceTexturePresents +
                      [uint64]$lastMiniport.values.emptyPresents -ne
                          [uint64]$lastMiniport.values.presents) {
            $findings.Add(
                'miniport presentation-source counts do not sum to presents')
        }
    }

    $miniport = if ($lastMiniport -and $lastMiniport.complete) {
        $value = $lastMiniport.values
        [pscustomobject][ordered]@{
            startLine = $lastMiniport.startLine
            logicalDraws = $value.logicalDraws
            singleDrawIoctls = $value.singleDrawIoctls
            drawBatchIoctls = $value.drawBatchIoctls
            maximumDrawsPerIoctl = $value.maximumDrawsPerIoctl
            presents = $value.presents
            presentationSourceCountersPresent = $presentSourceComplete
            cpuPresents = $value.cpuPresents
            surfaceTexturePresents = $value.surfaceTexturePresents
            emptyPresents = $value.emptyPresents
            surfaceCopies = $value.surfaceCopies
            surfaceReadbacks = $value.surfaceReadbacks
            clears = $value.clears
            uploads = $value.uploads
            hostBatches = $value.hostBatches
            maximumDrawsPerHostBatch = $value.maximumDrawsPerHostBatch
            fixedStateReuse = $value.fixedStateReuse
            textureReuse = $value.textureReuse
            arrayReuse = $value.arrayReuse
            interleavedDraws = $value.interleavedDraws
            compactDraws = $value.compactDraws
            compactIndexedDraws = $value.compactIndexedDraws
            canonicalIndexedDraws = $value.canonicalIndexedDraws
            indexedElementDraws = $value.indexedElementDraws
            rawIndexedObjectDraws = $value.rawIndexedObjectDraws
            drawTicks = $value.drawTicks
            packetTicks = $value.packetTicks
            prepareTicks = $value.prepareTicks
            stateTicks = $value.stateTicks
            submitTicks = $value.submitTicks
            interleavedCopyTicks = $value.interleavedCopyTicks
            interleavedCopyBytes = $value.interleavedCopyBytes
            interleavedCopyCalls = $value.interleavedCopyCalls
            frequency = $value.frequency
        }
    } else { $null }

    $derived = $null
    if ($display -and $miniport) {
        $timelineUsable = $selectedTimeline.usable
        $displayFrequencies = @($completeDisplayCandidates |
            ForEach-Object { $_.values.frequency })
        if (-not $miniport.frequency -or
            @($displayFrequencies | Where-Object { -not $_ }).Count) {
            $findings.Add('profile frequency is zero')
        } elseif (@($displayFrequencies | Where-Object {
            $_ -ne $miniport.frequency
        }).Count) {
            $findings.Add('display and miniport profile frequencies differ')
        }
        $ioctlCount = $miniport.singleDrawIoctls + $miniport.drawBatchIoctls
        $displayIoctlCount = if ($display.aggregate) {
            $display.aggregate.drawIoctlCalls
        } else { $display.drawIoctlCalls }
        if ($displayIoctlCount -ne $ioctlCount) {
            $findings.Add('display and miniport draw IOCTL counts differ')
        }
        if ($miniport.interleavedDraws -gt $miniport.logicalDraws) {
            $findings.Add('interleaved draw count exceeds logical draw count')
        }
        if ($miniport.compactDraws -gt $miniport.interleavedDraws) {
            $findings.Add('compact draw count exceeds interleaved draw count')
        }
        if ($miniport.compactIndexedDraws -gt $miniport.compactDraws) {
            $findings.Add('compact indexed draw count exceeds compact draw count')
        }
        if ($miniport.canonicalIndexedDraws -gt $miniport.logicalDraws) {
            $findings.Add(
                'canonical indexed draw count exceeds logical draw count')
        }
        if ([uint64]$miniport.indexedElementDraws -gt
                [uint64]$miniport.canonicalIndexedDraws +
                [uint64]$miniport.rawIndexedObjectDraws) {
            $findings.Add(
                'indexed element draw count exceeds canonical plus raw indexed draw count')
        }
        $combinedCopyCalls = [uint64]$miniport.interleavedDraws
        $splitCopyCalls = $combinedCopyCalls +
            [uint64]$miniport.compactIndexedDraws
        $rawCopyCalls = [uint64]$miniport.rawIndexedObjectDraws
        if ($splitCopyCalls -gt [uint32]::MaxValue) {
            $splitCopyCalls = [uint64][uint32]::MaxValue
        }
        $copyContract = if ($rawCopyCalls -and
            $miniport.interleavedCopyCalls -eq $rawCopyCalls) {
            'raw-indexed-object-only-copy'
        } elseif (-not $miniport.compactIndexedDraws -and
            $miniport.interleavedCopyCalls -eq $combinedCopyCalls) {
            'undifferentiated-no-indexed-draws'
        } elseif ($combinedCopyCalls -eq $splitCopyCalls -and
            $miniport.interleavedCopyCalls -eq $combinedCopyCalls) {
            'ambiguous-saturated-copy-counter'
        } elseif ($miniport.interleavedCopyCalls -eq $combinedCopyCalls) {
            'combined-index-and-vertex-copy'
        } elseif ($miniport.interleavedCopyCalls -eq $splitCopyCalls) {
            'split-index-and-vertex-copy'
        } else { 'invalid' }
        if ($copyContract -eq 'invalid') {
            $findings.Add(
                'interleaved copy calls match no supported transport copy contract')
        }
        if ($miniport.interleavedCopyTicks -gt $miniport.submitTicks) {
            $findings.Add('nested interleaved copy ticks exceed submit ticks')
        }
        if ($miniport.submitTicks -gt $miniport.drawTicks) {
            $findings.Add('submit ticks exceed total miniport draw ticks')
        }
        $componentTicks = $miniport.packetTicks + $miniport.prepareTicks +
            $miniport.stateTicks + $miniport.submitTicks
        if ($componentTicks -gt $miniport.drawTicks) {
            $findings.Add('miniport component ticks exceed total draw ticks')
        }
        if ($miniport.interleavedCopyCalls -and
            -not $miniport.interleavedCopyBytes) {
            $findings.Add('interleaved copy calls are nonzero but bytes are zero')
        }

        $frequency = [double]$miniport.frequency
        $residualSubmitTicks = if ($miniport.submitTicks -ge
                                      $miniport.interleavedCopyTicks) {
            $miniport.submitTicks - $miniport.interleavedCopyTicks
        } else { [uint64]0 }
        $unattributedDrawTicks = if ($miniport.drawTicks -ge $componentTicks) {
            $miniport.drawTicks - $componentTicks
        } else { [uint64]0 }
        $timelineResidualTicks = $null
        if ($timelineUsable -and -not $display.dp2OverlapCount -and
            $display.dp2SpanTicks -ge
                $display.dp2Ticks + $display.dp2GapTicks) {
            $timelineResidualTicks = $display.dp2SpanTicks -
                $display.dp2Ticks - $display.dp2GapTicks
        }
        $derived = [pscustomobject][ordered]@{
            dp2SpanSeconds = if ($display.frequency -and $timelineUsable) {
                [Math]::Round($display.dp2SpanTicks /
                    [double]$display.frequency, 9)
            } else { $null }
            dp2GapSeconds = if ($display.frequency -and $timelineUsable) {
                [Math]::Round($display.dp2GapTicks /
                    [double]$display.frequency, 9)
            } else { $null }
            dp2MaximumGapMilliseconds = if ($display.frequency -and
                                               $timelineUsable) {
                [Math]::Round(1000.0 * $display.dp2GapMaxTicks /
                    $display.frequency, 9)
            } else { $null }
            dp2ActivePercentOfSpan = if ($timelineUsable -and
                                            -not $display.dp2OverlapCount -and
                                            $display.dp2SpanTicks) {
                [Math]::Round(100.0 * $display.dp2Ticks /
                    $display.dp2SpanTicks, 9)
            } else { $null }
            dp2GapPercentOfSpan = if ($timelineUsable -and
                                         -not $display.dp2OverlapCount -and
                                         $display.dp2SpanTicks) {
                [Math]::Round(100.0 * $display.dp2GapTicks /
                    $display.dp2SpanTicks, 9)
            } else { $null }
            dp2TimelineResidualTicks = $timelineResidualTicks
            dp2TimelineCaveat = if (-not $display.timelinePresent) {
                'legacy display profile has no DP2 timeline counters'
            } elseif (-not $timelineUsable) {
                'invalid DP2 timeline record has no derived timing claims'
            } elseif ($display.aggregate.contextCount -gt 1) {
                'timeline derivation describes only the selected final display context; context spans may overlap and are not summed'
            } elseif ($display.dp2OverlapCount) {
                'overlapping DP2 callbacks invalidate active-plus-gap arithmetic'
            } else {
                'DP2 span equals callback-active plus positive inter-call gap time'
            }
            totalDrawIoctls = $ioctlCount
            averageDrawsPerIoctl = if ($ioctlCount) {
                [Math]::Round($miniport.logicalDraws / [double]$ioctlCount, 9)
            } else { $null }
            interleavedPercent = if ($miniport.logicalDraws) {
                [Math]::Round(100.0 * $miniport.interleavedDraws /
                    $miniport.logicalDraws, 9)
            } else { $null }
            compactIndexedPercent = if ($miniport.compactDraws) {
                [Math]::Round(100.0 * $miniport.compactIndexedDraws /
                    $miniport.compactDraws, 9)
            } else { $null }
            canonicalIndexedPercent = if ($miniport.logicalDraws) {
                [Math]::Round(100.0 * $miniport.canonicalIndexedDraws /
                    $miniport.logicalDraws, 9)
            } else { $null }
            rawIndexedObjectPercent = if ($miniport.logicalDraws) {
                [Math]::Round(100.0 * $miniport.rawIndexedObjectDraws /
                    $miniport.logicalDraws, 9)
            } else { $null }
            indexedElementPercent = if (
                    [uint64]$miniport.canonicalIndexedDraws +
                    [uint64]$miniport.rawIndexedObjectDraws) {
                [Math]::Round(100.0 * $miniport.indexedElementDraws /
                    ([uint64]$miniport.canonicalIndexedDraws +
                     [uint64]$miniport.rawIndexedObjectDraws), 9)
            } else { $null }
            miniportDrawSeconds = if ($frequency) {
                [Math]::Round($miniport.drawTicks / $frequency, 9)
            } else { $null }
            packetSeconds = if ($frequency) {
                [Math]::Round($miniport.packetTicks / $frequency, 9)
            } else { $null }
            prepareSeconds = if ($frequency) {
                [Math]::Round($miniport.prepareTicks / $frequency, 9)
            } else { $null }
            stateSeconds = if ($frequency) {
                [Math]::Round($miniport.stateTicks / $frequency, 9)
            } else { $null }
            submitSeconds = if ($frequency) {
                [Math]::Round($miniport.submitTicks / $frequency, 9)
            } else { $null }
            interleavedCopyContract = $copyContract
            interleavedCopySeconds = if ($frequency) {
                [Math]::Round($miniport.interleavedCopyTicks / $frequency, 9)
            } else { $null }
            interleavedCopyMicrosecondsPerCall =
                if ($frequency -and $miniport.interleavedCopyCalls) {
                    [Math]::Round(1000000.0 * $miniport.interleavedCopyTicks /
                        $frequency / $miniport.interleavedCopyCalls, 9)
                } else { $null }
            interleavedCopyBytesPerCall =
                if ($miniport.interleavedCopyCalls) {
                    [Math]::Round($miniport.interleavedCopyBytes /
                        [double]$miniport.interleavedCopyCalls, 9)
                } else { $null }
            interleavedCopyMicrosecondsPerCompactDraw =
                if ($frequency -and $miniport.compactDraws) {
                    [Math]::Round(1000000.0 * $miniport.interleavedCopyTicks /
                        $frequency / $miniport.compactDraws, 9)
                } else { $null }
            interleavedCopyBytesPerCompactDraw =
                if ($miniport.compactDraws) {
                    [Math]::Round($miniport.interleavedCopyBytes /
                        [double]$miniport.compactDraws, 9)
                } else { $null }
            interleavedCopyCallsPerCompactDraw =
                if ($miniport.compactDraws) {
                    [Math]::Round($miniport.interleavedCopyCalls /
                        [double]$miniport.compactDraws, 9)
                } else { $null }
            interleavedCopyPercentOfSubmit = if ($miniport.submitTicks) {
                [Math]::Round(100.0 * $miniport.interleavedCopyTicks /
                    $miniport.submitTicks, 9)
            } else { $null }
            interleavedCopyPercentOfTotalDraw = if ($miniport.drawTicks) {
                [Math]::Round(100.0 * $miniport.interleavedCopyTicks /
                    $miniport.drawTicks, 9)
            } else { $null }
            residualSubmitTicks = $residualSubmitTicks
            residualSubmitSeconds = if ($frequency) {
                [Math]::Round($residualSubmitTicks / $frequency, 9)
            } else { $null }
            residualSubmitMicrosecondsPerLogicalDraw =
                if ($frequency -and $miniport.logicalDraws) {
                    [Math]::Round(1000000.0 * $residualSubmitTicks /
                        $frequency / $miniport.logicalDraws, 9)
                } else { $null }
            unattributedDrawTicks = $unattributedDrawTicks
            unattributedDrawSeconds = if ($frequency) {
                [Math]::Round($unattributedDrawTicks / $frequency, 9)
            } else { $null }
            nestedTimerCaveat = 'interleavedCopyTicks is nested within submitTicks and is not additive with submit or total draw time'
        }
    }

    $status = if ($findings.Count) {
        if (($lastMiniport -and -not $lastMiniport.complete) -or
            ($selectedDisplay -and -not $selectedDisplay.complete) -or
            @($displayTimelines | Where-Object {
                $_.present -and -not $_.complete
            }).Count -or $trailingDisplayBlocks.Count) {
            'incomplete'
        } else { 'invalid' }
    } else { 'complete' }
    return [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-d3d-driver-profile'
        status = $status
        valid = $findings.Count -eq 0
        selection = [pscustomobject][ordered]@{
            rule = 'last-complete-paired-session'
            displayBlocks = $displayBlocks.Count
            miniportBlocks = $miniportBlocks.Count
            selectedDisplayLine = if ($selectedDisplay) {
                $selectedDisplay.startLine
            } else { $null }
            pairedDisplayLines = @($displayCandidates.startLine)
            selectedMiniportLine = if ($lastMiniport) {
                $lastMiniport.startLine
            } else { $null }
        }
        display = $display
        miniport = $miniport
        derived = $derived
        findings = @($findings)
    }
}
