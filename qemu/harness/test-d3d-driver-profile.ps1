$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'd3d-driver-profile.ps1')

function Assert-D3dProfile {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Add-D3dProfileValue {
    param(
        [Collections.Generic.List[string]]$Lines,
        [string]$Label,
        [uint64]$Value,
        [int]$Words,
        [switch]$Miniport
    )

    [void]$Lines.Add($Label)
    $wordValues = if ($Words -eq 2) {
        @([uint32]($Value / [uint64]4294967296),
          [uint32]($Value % [uint64]4294967296))
    } else { @([uint32]$Value) }
    foreach ($word in $wordValues) {
        if ($Miniport) { [void]$Lines.Add('QXPMP STATUS') }
        [void]$Lines.Add(('0x{0:X8}' -f $word))
    }
}

function New-D3dProfileFixture {
    param(
        [uint32]$LogicalDraws,
        [uint32]$SingleIoctls,
        [uint32]$BatchIoctls,
        [uint64]$CopyTicks,
        [uint64]$CopyBytes,
        [uint32]$CompactIndexedDraws = 0,
        [switch]$IncludeCompactIndexed,
        [switch]$CombinedIndexedCopy,
        [switch]$SaturatedIndexedCopy,
        [switch]$OmitTimeline,
        [switch]$RawOnlyCopy,
        [switch]$IncludePresentSources,
        [uint32]$CpuPresents = 2,
        [uint32]$SurfaceTexturePresents = 5,
        [uint32]$EmptyPresents = 0
    )

    $lines = [Collections.Generic.List[string]]::new()
    [void]$lines.Add('unrelated display noise')
    Add-D3dProfileValue $lines 'QXPDISP PERF DP2 CALLS' 25 1
    Add-D3dProfileValue $lines 'QXPDISP PERF DP2 COMMAND BYTES' 65537 2
    Add-D3dProfileValue $lines 'QXPDISP PERF DP2 TICKS' 1500 2
    if (-not $OmitTimeline) {
        Add-D3dProfileValue $lines `
            'QXPDISP PERF DP2 TIMELINE VERSION' 1 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 SPAN TICKS' 5000 2
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP COUNT' 24 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP TICKS' 3500 2
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP MAX TICKS' 500 2
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 OVERLAP COUNT' 0 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP LT 1MS' 24 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP 1-10MS' 0 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP 10-50MS' 0 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP 50-100MS' 0 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP 100-250MS' 0 1
        Add-D3dProfileValue $lines 'QXPDISP PERF DP2 GAP GE 250MS' 0 1
    }
    Add-D3dProfileValue $lines 'QXPDISP PERF DRAW IOCTL CALLS' `
        ([uint64]$SingleIoctls + $BatchIoctls) 1
    Add-D3dProfileValue $lines 'QXPDISP PERF DRAW IOCTL BYTES' `
        ([uint64]4294967301) 2
    Add-D3dProfileValue $lines 'QXPDISP PERF DRAW IOCTL TICKS' 800 2
    Add-D3dProfileValue $lines 'QXPDISP PERF INDEXED CALLS' 17 1
    Add-D3dProfileValue $lines `
        'QXPDISP PERF INDEXED RAW PRELIT CALLS' 9 1
    Add-D3dProfileValue $lines `
        'QXPDISP PERF INDEXED RAW ELIGIBLE CALLS' 8 1
    Add-D3dProfileValue $lines 'QXPDISP PERF INDEXED RAW PACKETS' 11 1
    Add-D3dProfileValue $lines `
        'QXPDISP PERF INDEXED RAW FALLBACK CHUNKS' 1 1
    Add-D3dProfileValue $lines `
        'QXPDISP PERF INDEXED CANONICAL PACKETS' 3 1
    Add-D3dProfileValue $lines 'QXPDISP PERF FREQUENCY' 1000000 2
    Add-D3dProfileValue $lines 'QXPMP D3D DRAWS' $LogicalDraws 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D DRAW IOCTLS' $SingleIoctls 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D DRAW BATCH IOCTLS' $BatchIoctls 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D DRAW BATCH MAX' 20 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS' 7 1 -Miniport
    if ($IncludePresentSources) {
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS CPU' `
            $CpuPresents 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS SURFTEX' `
            $SurfaceTexturePresents 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS EMPTY' `
            $EmptyPresents 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D SURFACE COPIES' 3 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D SURFACE READBACKS' 4 1 -Miniport
    }
    Add-D3dProfileValue $lines 'QXPMP D3D CLEARS' 8 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D UPLOADS' 9 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D BATCHES' 5 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D BATCHMAX' 40 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D FIXEDREUSE' 10 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D TEXREUSE' 11 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D ARRAYREUSE' 12 1 -Miniport
    $interleavedDraws = if ($SaturatedIndexedCopy) {
        [uint64][uint32]::MaxValue
    } else { [uint64]$LogicalDraws - 2 }
    Add-D3dProfileValue $lines 'QXPMP D3D INTERLEAVED' `
        $interleavedDraws 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D COMPACT' `
        ([uint64]$LogicalDraws - 3) 1 -Miniport
    if ($IncludeCompactIndexed) {
        Add-D3dProfileValue $lines 'QXPMP D3D COMPACT INDEXED' `
            $CompactIndexedDraws 1 -Miniport
    }
    Add-D3dProfileValue $lines 'QXPMP D3D INDEXED CANONICAL' 3 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D INDEXED ELEMENTS' 14 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D RAW OBJECT' 11 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF DRAW TICKS' 1000 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF PACKET TICKS' 100 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF PREPARE TICKS' 50 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF STATE TICKS' 100 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF SUBMIT TICKS' 700 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF INTERLEAVED COPY TICKS' `
        $CopyTicks 2 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF INTERLEAVED COPY BYTES' `
        $CopyBytes 2 -Miniport
    $copyCalls = if ($RawOnlyCopy) {
        [uint64]11
    } elseif ($SaturatedIndexedCopy) {
        [uint64][uint32]::MaxValue
    } else { [uint64]$LogicalDraws - 2 }
    if (-not $RawOnlyCopy -and -not $CombinedIndexedCopy -and
            -not $SaturatedIndexedCopy) {
        $copyCalls += $CompactIndexedDraws
    }
    Add-D3dProfileValue $lines 'QXPMP D3D PERF INTERLEAVED COPY CALLS' `
        $copyCalls 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D PERF FREQUENCY' 1000000 2 -Miniport
    [void]$lines.Add('QXPMP D3D CLOSE+')
    return $lines -join "`r`n"
}

$first = New-D3dProfileFixture -LogicalDraws 20 -SingleIoctls 1 `
    -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000
$second = New-D3dProfileFixture -LogicalDraws 120 -SingleIoctls 2 `
    -BatchIoctls 10 -CopyTicks 600 -CopyBytes ([uint64]4294967303)
$profile = ConvertFrom-XpD3dDriverProfile -Text ($first + "`r`n" + $second)
Assert-D3dProfile $profile.valid 'two-session profile was not valid'
Assert-D3dProfile ($profile.status -eq 'complete') `
    'two-session profile was not complete'
Assert-D3dProfile ($profile.selection.displayBlocks -eq 2 -and
                   $profile.selection.miniportBlocks -eq 2) `
    'two-session block counts are wrong'
Assert-D3dProfile ($profile.miniport.logicalDraws -eq 120) `
    'parser did not select the final miniport session'
Assert-D3dProfile ($profile.display.drawIoctlCalls -eq 12) `
    'parser did not pair the final display session'
Assert-D3dProfile ($profile.miniport.interleavedCopyBytes -eq
                   [uint64]4294967303) `
    'split 64-bit value was reconstructed incorrectly'
Assert-D3dProfile ($profile.display.indexedCalls -eq 17 -and
                   $profile.display.indexedRawPrelitCalls -eq 9 -and
                   $profile.display.indexedRawEligibleCalls -eq 8 -and
                   $profile.display.indexedRawPackets -eq 11 -and
                   $profile.display.indexedRawFallbackChunks -eq 1 -and
                   $profile.display.indexedCanonicalPackets -eq 3 -and
                   $profile.display.aggregate.indexedRawPackets -eq 11) `
    'raw indexed display counters were not retained'
Assert-D3dProfile ($profile.miniport.rawIndexedObjectDraws -eq 11) `
    'raw indexed miniport counter was not retained'
Assert-D3dProfile ($profile.display.timelineVersion -eq 1 -and
                   $profile.display.timelineComplete -and
                   $profile.display.dp2GapCount -eq 24 -and
                   $profile.display.dp2OverlapCount -eq 0) `
    'DP2 timeline counters were not retained'
Assert-D3dProfile ($profile.derived.dp2SpanSeconds -eq 0.005 -and
                   $profile.derived.dp2GapSeconds -eq 0.0035 -and
                   $profile.derived.dp2ActivePercentOfSpan -eq 30.0 -and
                   $profile.derived.dp2GapPercentOfSpan -eq 70.0) `
    'DP2 timeline derivation is wrong'
Assert-D3dProfile ($profile.derived.interleavedCopySeconds -eq 0.0006) `
    'copy seconds are wrong'
Assert-D3dProfile ($profile.derived.interleavedCopyPercentOfSubmit -eq
                   85.714285714) `
    'nested copy share is wrong'
Assert-D3dProfile ($profile.derived.residualSubmitTicks -eq 100) `
    'submit residual is wrong'
Assert-D3dProfile ($profile.derived.unattributedDrawTicks -eq 50) `
    'unattributed total is wrong'
Assert-D3dProfile ($profile.miniport.compactIndexedDraws -eq 0) `
    'legacy profile did not default the optional indexed counter to zero'
Assert-D3dProfile (-not $profile.miniport.presentationSourceCountersPresent -and
                   $profile.miniport.cpuPresents -eq 0 -and
                   $profile.miniport.surfaceTexturePresents -eq 0) `
    'legacy profile did not preserve omitted presentation-source counters'
[void]($profile | ConvertTo-Json -Depth 12)

$modernPresentationProfile = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dProfileFixture -LogicalDraws 20 -SingleIoctls 1 `
        -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000 `
        -IncludePresentSources)
Assert-D3dProfile ($modernPresentationProfile.valid -and
                   $modernPresentationProfile.miniport.
                       presentationSourceCountersPresent -and
                   $modernPresentationProfile.miniport.cpuPresents -eq 2 -and
                   $modernPresentationProfile.miniport.
                       surfaceTexturePresents -eq 5 -and
                   $modernPresentationProfile.miniport.emptyPresents -eq 0 -and
                   $modernPresentationProfile.miniport.surfaceCopies -eq 3 -and
                   $modernPresentationProfile.miniport.surfaceReadbacks -eq 4) `
    'modern presentation-source counters were not retained'
$badPresentationSum = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dProfileFixture -LogicalDraws 20 -SingleIoctls 1 `
        -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000 `
        -IncludePresentSources -CpuPresents 3)
Assert-D3dProfile (-not $badPresentationSum.valid -and
                   $badPresentationSum.findings -contains
                       'miniport presentation-source counts do not sum to presents') `
    'invalid presentation-source sum was accepted'
$partialPresentation = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000 `
    -IncludePresentSources).Replace(
        "QXPMP D3D SURFACE READBACKS`r`nQXPMP STATUS`r`n0x00000004`r`n", '')
$partialPresentationProfile = ConvertFrom-XpD3dDriverProfile `
    -Text $partialPresentation
Assert-D3dProfile (-not $partialPresentationProfile.valid -and
                   $partialPresentationProfile.findings -contains
                       'final miniport presentation-source counter group is incomplete') `
    'partial presentation-source group was accepted'

$contextFixture = New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000
$miniportFixture = New-D3dProfileFixture -LogicalDraws 40 `
    -SingleIoctls 2 -BatchIoctls 2 -CopyTicks 500 -CopyBytes 2000
$contextDisplay = $contextFixture.Substring(0,
    $contextFixture.IndexOf('QXPMP D3D DRAWS'))
$firstContextDisplay = $contextDisplay.Replace(
    "QXPDISP PERF INDEXED CALLS`r`n0x00000011",
    "QXPDISP PERF INDEXED CALLS`r`n0x00000012")
$miniportOnly = $miniportFixture.Substring(
    $miniportFixture.IndexOf('QXPMP D3D DRAWS'))
$multiContextText =
    $firstContextDisplay + $contextDisplay + $miniportOnly
$multiContextProfile = ConvertFrom-XpD3dDriverProfile -Text $multiContextText
Assert-D3dProfile ($multiContextProfile.valid -and
                   $multiContextProfile.display.aggregate.contextCount -eq 2 -and
                   $multiContextProfile.display.aggregate.drawIoctlCalls -eq 4 -and
                   $multiContextProfile.display.timelines.Count -eq 2) `
    'multiple display contexts were not aggregated for one miniport session'
Assert-D3dProfile ($multiContextProfile.derived.dp2TimelineCaveat -match
                   'selected final display context') `
    'multi-context timeline derivation did not retain its scope caveat'
$cumulativeDisplay = $contextDisplay +
    $miniportFixture.Substring(0,
        $miniportFixture.IndexOf('QXPMP D3D DRAWS')) +
    $miniportOnly
$cumulativeProfile = ConvertFrom-XpD3dDriverProfile -Text $cumulativeDisplay
Assert-D3dProfile ($cumulativeProfile.valid -and
                   $cumulativeProfile.display.aggregate.contextCount -eq 1 -and
                   $cumulativeProfile.display.aggregate.drawIoctlCalls -eq 4 -and
                   $cumulativeProfile.display.timelines.Count -eq 1) `
    'cumulative display snapshots were counted as separate contexts'


$badFirstDisplay = $contextDisplay.Replace(
    "QXPDISP PERF DP2 GAP LT 1MS`r`n0x00000018",
    "QXPDISP PERF DP2 GAP LT 1MS`r`n0x00000017")
$badFirstContextProfile = ConvertFrom-XpD3dDriverProfile -Text (
    $badFirstDisplay + $contextDisplay + $miniportOnly)
Assert-D3dProfile (-not $badFirstContextProfile.valid -and
                   @($badFirstContextProfile.findings | Where-Object {
                       $_ -eq 'display DP2 gap buckets do not match gap count'
                   }).Count -eq 1) `
    'malformed non-final display context was not rejected'

$legacyDisplayProfile = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dProfileFixture -LogicalDraws 20 -SingleIoctls 1 `
        -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000 -OmitTimeline)
Assert-D3dProfile ($legacyDisplayProfile.valid -and
                   -not $legacyDisplayProfile.display.timelinePresent -and
                   $legacyDisplayProfile.display.timelineVersion -eq 0 -and
                   $null -eq $legacyDisplayProfile.display.timelineComplete -and
                   $legacyDisplayProfile.derived.dp2SpanSeconds -eq $null) `
    'legacy display profile without timeline counters was not preserved'

$overlapText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 CALLS`r`n0x00000019",
        "QXPDISP PERF DP2 CALLS`r`n0x0000001A").Replace(
        "QXPDISP PERF DP2 OVERLAP COUNT`r`n0x00000000",
        "QXPDISP PERF DP2 OVERLAP COUNT`r`n0x00000001")
$overlapProfile = ConvertFrom-XpD3dDriverProfile -Text $overlapText
Assert-D3dProfile ($overlapProfile.valid -and
                   $overlapProfile.display.dp2OverlapCount -eq 1 -and
                   $null -eq $overlapProfile.derived.dp2ActivePercentOfSpan) `
    'overlapping DP2 callbacks were not retained as an arithmetic caveat'

$badBucketsText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 GAP LT 1MS`r`n0x00000018",
        "QXPDISP PERF DP2 GAP LT 1MS`r`n0x00000017")
$badBucketsProfile = ConvertFrom-XpD3dDriverProfile -Text $badBucketsText
Assert-D3dProfile (-not $badBucketsProfile.valid -and
                   @($badBucketsProfile.findings | Where-Object {
                       $_ -eq 'display DP2 gap buckets do not match gap count'
                   }).Count -eq 1) `
    'inconsistent DP2 gap buckets were not rejected'

$zeroVersionText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 TIMELINE VERSION`r`n0x00000001",
        "QXPDISP PERF DP2 TIMELINE VERSION`r`n0x00000000")
$zeroVersionProfile = ConvertFrom-XpD3dDriverProfile -Text $zeroVersionText
Assert-D3dProfile (-not $zeroVersionProfile.valid -and
                   $zeroVersionProfile.display.timelinePresent -and
                   @($zeroVersionProfile.findings | Where-Object {
                       $_ -eq 'display DP2 timeline version is unsupported'
                   }).Count -eq 1) `
    'explicit timeline version zero was silently treated as legacy'

$unversionedText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 TIMELINE VERSION`r`n0x00000001`r`n", '')
$unversionedProfile = ConvertFrom-XpD3dDriverProfile -Text $unversionedText
Assert-D3dProfile (-not $unversionedProfile.valid -and
                   $unversionedProfile.display.timelinePresent -and
                   -not $unversionedProfile.display.timelineComplete -and
                   @($unversionedProfile.findings | Where-Object {
                       $_ -eq 'display DP2 timeline version label is missing'
                   }).Count -eq 1) `
    'unversioned timeline fields were silently treated as legacy'

$zeroGapCountText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 GAP COUNT`r`n0x00000018",
        "QXPDISP PERF DP2 GAP COUNT`r`n0x00000000")
$zeroGapCountProfile = ConvertFrom-XpD3dDriverProfile -Text $zeroGapCountText
Assert-D3dProfile (-not $zeroGapCountProfile.valid -and
                   @($zeroGapCountProfile.findings | Where-Object {
                       $_ -eq 'display DP2 zero gap count has nonzero gap ticks'
                   }).Count -eq 1) `
    'nonzero gap timing with zero gap count was not rejected'

$badMaximumText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 GAP MAX TICKS`r`n0x00000000`r`n0x000001F4",
        "QXPDISP PERF DP2 GAP MAX TICKS`r`n0x00000000`r`n0x00000064")
$badMaximumProfile = ConvertFrom-XpD3dDriverProfile -Text $badMaximumText
Assert-D3dProfile (-not $badMaximumProfile.valid -and
                   @($badMaximumProfile.findings | Where-Object {
                       $_ -eq 'display DP2 maximum gap is infeasible for total and count'
                   }).Count -eq 1) `
    'infeasible DP2 maximum gap was not rejected'

$minimumMaximumText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        "QXPDISP PERF DP2 GAP MAX TICKS`r`n0x00000000`r`n0x000001F4",
        "QXPDISP PERF DP2 GAP MAX TICKS`r`n0x00000000`r`n0x00000092")
$minimumMaximumProfile = ConvertFrom-XpD3dDriverProfile `
    -Text $minimumMaximumText
Assert-D3dProfile $minimumMaximumProfile.valid `
    'feasible ceiling-average maximum gap was rejected'

$versionRecord = "QXPDISP PERF DP2 TIMELINE VERSION`r`n0x00000001`r`n"
$reorderedTimelineText = (New-D3dProfileFixture -LogicalDraws 20 `
    -SingleIoctls 1 -BatchIoctls 1 -CopyTicks 500 -CopyBytes 2000).Replace(
        $versionRecord, '').Replace(
        'QXPDISP PERF DP2 COMMAND BYTES',
        $versionRecord + 'QXPDISP PERF DP2 COMMAND BYTES')
$reorderedTimelineProfile = ConvertFrom-XpD3dDriverProfile `
    -Text $reorderedTimelineText
Assert-D3dProfile (-not $reorderedTimelineProfile.valid -and
                   $reorderedTimelineProfile.display.timelinePresent -and
                   -not $reorderedTimelineProfile.display.timelineComplete -and
                   @($reorderedTimelineProfile.findings | Where-Object {
                       $_ -eq 'display DP2 timeline block is incomplete'
                   }).Count -eq 1) `
    'reordered timeline label was silently treated as legacy'

$indexed = New-D3dProfileFixture -LogicalDraws 120 -SingleIoctls 2 `
    -BatchIoctls 10 -CopyTicks 600 -CopyBytes 3000 `
    -CompactIndexedDraws 30 -IncludeCompactIndexed
$indexedProfile = ConvertFrom-XpD3dDriverProfile -Text $indexed
Assert-D3dProfile $indexedProfile.valid `
    'compact indexed profile was not valid'
Assert-D3dProfile ($indexedProfile.miniport.compactIndexedDraws -eq 30 -and
                   $indexedProfile.miniport.interleavedCopyCalls -eq 148 -and
                   $indexedProfile.derived.interleavedCopyContract -eq
                       'split-index-and-vertex-copy') `
    'legacy split compact indexed counters were not retained'
$combinedIndexed = New-D3dProfileFixture -LogicalDraws 120 `
    -SingleIoctls 2 -BatchIoctls 10 -CopyTicks 600 -CopyBytes 3000 `
    -CompactIndexedDraws 30 -IncludeCompactIndexed -CombinedIndexedCopy
$combinedIndexedProfile = ConvertFrom-XpD3dDriverProfile `
    -Text $combinedIndexed
Assert-D3dProfile ($combinedIndexedProfile.valid -and
                   $combinedIndexedProfile.miniport.interleavedCopyCalls -eq
                       118 -and
                   $combinedIndexedProfile.derived.interleavedCopyContract -eq
                       'combined-index-and-vertex-copy') `
    'combined compact indexed copy contract was not retained'
$saturatedIndexed = New-D3dProfileFixture `
    -LogicalDraws ([uint32]::MaxValue) -SingleIoctls 2 -BatchIoctls 10 `
    -CopyTicks 600 -CopyBytes 3000 -CompactIndexedDraws 30 `
    -IncludeCompactIndexed -SaturatedIndexedCopy
$saturatedIndexedProfile = ConvertFrom-XpD3dDriverProfile `
    -Text $saturatedIndexed
Assert-D3dProfile ($saturatedIndexedProfile.valid -and
                   $saturatedIndexedProfile.derived.interleavedCopyContract -eq
                       'ambiguous-saturated-copy-counter') `
    'saturated compact indexed copy contract was not marked ambiguous'
$rawOnlyCopy = New-D3dProfileFixture -LogicalDraws 120 `
    -SingleIoctls 2 -BatchIoctls 10 -CopyTicks 600 -CopyBytes 3000 `
    -RawOnlyCopy
$rawOnlyCopyProfile = ConvertFrom-XpD3dDriverProfile -Text $rawOnlyCopy
Assert-D3dProfile ($rawOnlyCopyProfile.valid -and
                   $rawOnlyCopyProfile.miniport.interleavedCopyCalls -eq 11 -and
                   $rawOnlyCopyProfile.derived.interleavedCopyContract -eq
                       'raw-indexed-object-only-copy') `
    'raw indexed object copy contract was not retained'
Assert-D3dProfile ($indexedProfile.derived.compactIndexedPercent -eq
                   25.641025641) `
    'compact indexed draw percentage is wrong'
Assert-D3dProfile ($indexedProfile.derived.interleavedCopyBytesPerCompactDraw `
                   -eq 25.641025641) `
    'copy bytes per compact draw are wrong'

$truncated = $first + "`r`n" + $second + "`r`n" + @'
QXPMP D3D DRAWS
QXPMP STATUS
0x00000001
QXPMP D3D DRAW IOCTLS
'@
$truncatedProfile = ConvertFrom-XpD3dDriverProfile -Text $truncated
Assert-D3dProfile (-not $truncatedProfile.valid) `
    'truncated final session silently reused earlier evidence'
Assert-D3dProfile ($truncatedProfile.status -eq 'incomplete') `
    'truncated final session did not report incomplete status'

$displayOnlyTail = $first + "`r`n" + $second + "`r`n" + @'
QXPDISP PERF DP2 CALLS
0x00000001
'@
$displayOnlyProfile = ConvertFrom-XpD3dDriverProfile -Text $displayOnlyTail
Assert-D3dProfile (-not $displayOnlyProfile.valid -and
                   $displayOnlyProfile.status -eq 'incomplete') `
    'unpaired trailing display session silently reused earlier miniport evidence'

$invalid = New-D3dProfileFixture -LogicalDraws 120 -SingleIoctls 2 `
    -BatchIoctls 10 -CopyTicks 701 -CopyBytes 1000
$invalidProfile = ConvertFrom-XpD3dDriverProfile -Text $invalid
Assert-D3dProfile (-not $invalidProfile.valid -and
                   $invalidProfile.status -eq 'invalid') `
    'nested-timer violation was not rejected'
Assert-D3dProfile (@($invalidProfile.findings | Where-Object {
    $_ -eq 'nested interleaved copy ticks exceed submit ticks'
}).Count -eq 1) 'nested-timer finding is missing'

# An ordinary (non-QXP_D3D_PROFILE) build dumps only the always-on miniport
# counters: no display block and no PERF tick labels. That is a valid
# counters-only session, never an incomplete profile.
function New-D3dCounterFixture {
    param(
        [uint32]$LogicalDraws,
        [uint32]$SingleIoctls,
        [uint32]$BatchIoctls,
        [switch]$IncludeCompactIndexed,
        [switch]$OmitCompact,
        [switch]$OmitDrawIoctls,
        [switch]$IncludePresentSources
    )

    $lines = [Collections.Generic.List[string]]::new()
    [void]$lines.Add('QXPMP D3D OPEN+')
    Add-D3dProfileValue $lines 'QXPMP D3D DRAWS' $LogicalDraws 1 -Miniport
    if (-not $OmitDrawIoctls) {
        Add-D3dProfileValue $lines 'QXPMP D3D DRAW IOCTLS' $SingleIoctls 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D DRAW BATCH IOCTLS' $BatchIoctls 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D DRAW BATCH MAX' 20 1 -Miniport
    }
    Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS' 7 1 -Miniport
    if ($IncludePresentSources) {
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS CPU' 2 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS SURFTEX' 5 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D PRESENTS EMPTY' 0 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D SURFACE COPIES' 3 1 -Miniport
        Add-D3dProfileValue $lines 'QXPMP D3D SURFACE READBACKS' 4 1 -Miniport
    }
    Add-D3dProfileValue $lines 'QXPMP D3D CLEARS' 8 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D UPLOADS' 9 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D BATCHES' 5 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D BATCHMAX' 40 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D FIXEDREUSE' 10 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D TEXREUSE' 11 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D ARRAYREUSE' 12 1 -Miniport
    Add-D3dProfileValue $lines 'QXPMP D3D INTERLEAVED' `
        ([uint64]$LogicalDraws - 2) 1 -Miniport
    if (-not $OmitCompact) {
        Add-D3dProfileValue $lines 'QXPMP D3D COMPACT' `
            ([uint64]$LogicalDraws - 3) 1 -Miniport
    }
    if ($IncludeCompactIndexed) {
        Add-D3dProfileValue $lines 'QXPMP D3D COMPACT INDEXED' 30 1 -Miniport
    }
    [void]$lines.Add('QXPMP D3D CLOSE+')
    return $lines -join "`r`n"
}

$counters = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dCounterFixture -LogicalDraws 120 -SingleIoctls 2 -BatchIoctls 10 `
        -IncludeCompactIndexed)
Assert-D3dProfile ($counters.status -eq 'counters' -and $counters.valid) `
    'ordinary counter dump was not accepted as a counters-only session'
Assert-D3dProfile ($counters.selection.rule -eq 'final-counter-session' -and
                   $null -eq $counters.display) `
    'counters-only selection is wrong'
Assert-D3dProfile ($counters.miniport.logicalDraws -eq 120 -and
                   $counters.miniport.compactIndexedDraws -eq 30 -and
                   $counters.miniport.drawBatchIoctls -eq 10) `
    'counters-only miniport values are wrong'
Assert-D3dProfile ($counters.derived.totalDrawIoctls -eq 12 -and
                   $counters.derived.averageDrawsPerIoctl -eq 10.0) `
    'counters-only derived batching stats are wrong'

$presentationCounters = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dCounterFixture -LogicalDraws 120 -SingleIoctls 2 `
        -BatchIoctls 10 -IncludeCompactIndexed -IncludePresentSources)
Assert-D3dProfile ($presentationCounters.status -eq 'counters' -and
                   $presentationCounters.valid -and
                   $presentationCounters.miniport.
                       presentationSourceCountersPresent -and
                   $presentationCounters.miniport.cpuPresents -eq 2 -and
                   $presentationCounters.miniport.
                       surfaceTexturePresents -eq 5 -and
                   $presentationCounters.miniport.surfaceReadbacks -eq 4) `
    'counters-only presentation-source fields were not retained'

# A genuine .240-era log lacks the draw-IOCTL labels and the COMPACT
# INDEXED label entirely; it must still parse as a valid counter session.
$legacyCounters = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dCounterFixture -LogicalDraws 20 -SingleIoctls 0 -BatchIoctls 0 `
        -OmitDrawIoctls)
Assert-D3dProfile ($legacyCounters.status -eq 'counters' -and
                   $legacyCounters.valid -and
                   $legacyCounters.miniport.compactIndexedDraws -eq 0 -and
                   $legacyCounters.miniport.drawBatchIoctls -eq 0) `
    'legacy counter dump was not accepted'

# A .250-era counter dump with the draw-IOCTL labels still parses fully.
$modernCounters = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dCounterFixture -LogicalDraws 20 -SingleIoctls 5 -BatchIoctls 2)
Assert-D3dProfile ($modernCounters.status -eq 'counters' -and
                   $modernCounters.valid -and
                   $modernCounters.miniport.singleDrawIoctls -eq 5) `
    'modern counter dump was not accepted'

# A counter dump missing a required counter label is still incomplete.
$brokenCounters = ConvertFrom-XpD3dDriverProfile -Text (
    New-D3dCounterFixture -LogicalDraws 20 -SingleIoctls 5 -BatchIoctls 0 `
        -OmitCompact)
Assert-D3dProfile ($brokenCounters.status -eq 'incomplete' -and
                   -not $brokenCounters.valid) `
    'truncated counter dump was not rejected'

$absent = ConvertFrom-XpD3dDriverProfile -Text 'ordinary non-profile log'
Assert-D3dProfile ($absent.status -eq 'absent' -and -not $absent.valid) `
    'non-profile log did not report absent status'

Write-Output 'D3D driver profile parser tests passed'
