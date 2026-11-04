$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'doorbell-profile.ps1')

function Assert-DoorbellProfile {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$boot = @'
mesapt: QXPICD/1 vertical-sliceWRAPGL32
mesapt: DoorbellProfile publish calls 0 total 0 us avg 0 us
mesapt: DoorbellProfile swap calls 90 total 59000 us avg 655 us
mesapt: DoorbellProfile direct calls 2 total 3500 us
'@
$game = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: DoorbellProfile publish calls 115 total 120741 us avg 1049 us
mesapt: DoorbellProfile swap calls 1 total 1944 us avg 1944 us
mesapt: DoorbellProfile direct calls 473 total 475967 us
mesapt: DoorbellProfile direct _glBindFramebuffer@8(0x03c) calls 117 total 184836 us avg 1579 us
'@
$teardown = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: VertexArrayCache 32MB
FrameProfile durationUs=45141391 callbacks=2919 fresh=89
'@
$text = $boot + $game + $teardown
$gameMarkerIndex = $text.IndexOf('QXPD3D/')
$profile = Get-XpLastCompleteDoorbellProfile `
    -Text $text -StartIndex $boot.Length
Assert-DoorbellProfile ($null -ne $profile) `
    'complete game profile was lost behind an incomplete teardown session'
Assert-DoorbellProfile ($profile.markerName -ceq 'QXPD3D/' -and
        $profile.markerIndex -eq $gameMarkerIndex -and
        $profile.publishCalls -eq 115 -and
        $profile.publishTotalUs -eq 120741 -and
        $profile.publishAverageUs -eq 1049 -and
        $profile.swapCalls -eq 1 -and
        $profile.swapTotalUs -eq 1944 -and
        $profile.directCalls -eq 473 -and
        $profile.directTotalUs -eq 475967) `
    'wrong launch-bounded session or doorbell values were selected'
Assert-DoorbellProfile ($profile.topDirect -and
        $profile.topDirect.name -ceq '_glBindFramebuffer@8(0x03c)' -and
        $profile.topDirect.calls -eq 117 -and
        $profile.topDirect.totalUs -eq 184836 -and
        $profile.topDirect.averageUs -eq 1579) `
    'top direct-call profile was not preserved'

$profile = Get-XpLastCompleteDoorbellProfile `
    -Text ($boot + $game) -StartIndex $boot.Length
Assert-DoorbellProfile ($profile.publishCalls -eq 115) `
    'last complete game session was not selected directly'

$partialTail = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: DoorbellProfile publish calls 999 total 999 us avg 1 us
'@
$profile = Get-XpLastCompleteDoorbellProfile `
    -Text ($boot + $game + $partialTail) -StartIndex $boot.Length
Assert-DoorbellProfile ($profile.publishCalls -eq 115) `
    'incomplete trailing session shadowed the launch-bounded game profile'

$incompleteGame = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: DoorbellProfile publish calls 17 total 1700 us avg 100 us
'@
Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile `
        -Text ($boot + $incompleteGame + $teardown) `
        -StartIndex $boot.Length)) `
    'incomplete game fell back across the launch boundary to boot preflight'

$bootSameMarker = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: DoorbellProfile publish calls 7 total 700 us avg 100 us
mesapt: DoorbellProfile swap calls 1 total 100 us avg 100 us
mesapt: DoorbellProfile direct calls 9 total 900 us
'@
Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile `
        -Text ($bootSameMarker + $incompleteGame + $teardown) `
        -StartIndex $bootSameMarker.Length)) `
    'same-marker preflight crossed the explicit launch boundary'

$incompleteLastGame = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
FrameProfile durationUs=45141391 callbacks=2919 fresh=0
'@
Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile `
        -Text ($boot + $incompleteLastGame) -StartIndex $boot.Length)) `
    'incomplete final game produced a profile'

$zero = @'
mesapt: QXPD3D/3 XPDMWRAPGL32
mesapt: DoorbellProfile publish calls 0 total 0 us avg 0 us
mesapt: DoorbellProfile swap calls 0 total 0 us avg 0 us
mesapt: DoorbellProfile direct calls 0 total 0 us
'@
$profile = Get-XpLastCompleteDoorbellProfile `
    -Text ($boot + $game + $zero) -StartIndex $boot.Length
Assert-DoorbellProfile ($profile.markerIndex -gt $boot.Length -and
        $profile.publishCalls -eq 0 -and
        $profile.swapCalls -eq 0 -and
        $profile.directCalls -eq 0) `
    'complete zero-call session was silently replaced by an older workload'
Assert-DoorbellProfile ($null -eq $profile.topDirect) `
    'missing optional top-direct record produced fabricated data'

Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile `
        -Text $teardown -StartIndex 0)) `
    'incomplete-only session produced a profile'
Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile -Text '' -StartIndex 0)) `
    'empty input produced a profile'
Assert-DoorbellProfile ($null -eq
    (Get-XpLastCompleteDoorbellProfile -Text $game -StartIndex 999999)) `
    'out-of-range launch boundary produced a profile'

Write-Output 'doorbell profile session-selection tests passed'
