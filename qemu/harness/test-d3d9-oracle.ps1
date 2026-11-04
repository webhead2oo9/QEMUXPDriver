$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'd3d9-oracle.ps1')

function Assert-D3d9Oracle {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$legacyLifecycle = Get-XpD3d9ShaderLifecycle -DebugText @'
QXPDISP PS DEFINE+
QXPMP D3D SHADER DEFINE+
QXPMP D3D SHADER DELETE+
'@
Assert-D3d9Oracle ($legacyLifecycle.valid -and
    $legacyLifecycle.profile -eq 'legacy' -and
    $legacyLifecycle.define -and $legacyLifecycle.delete) `
    'complete legacy pixel-shader lifecycle was rejected'

$nativeLifecycle = Get-XpD3d9ShaderLifecycle -DebugText @'
QXPDISP PS20 DEFINE+
QXPMP D3D SM2 GLSL DEFINE+
QXPDISP PS20 DELETE+
QXPMP D3D SM2 GLSL DELETE+
'@
Assert-D3d9Oracle ($nativeLifecycle.valid -and
    $nativeLifecycle.profile -eq 'native' -and
    $nativeLifecycle.define -and $nativeLifecycle.delete) `
    'complete native pixel-shader lifecycle was rejected'

$mixedLifecycle = Get-XpD3d9ShaderLifecycle -DebugText @'
QXPDISP PS DEFINE+
QXPMP D3D SM2 GLSL DEFINE+
QXPMP D3D SM2 GLSL DELETE+
'@
Assert-D3d9Oracle (-not $mixedLifecycle.valid -and
    $mixedLifecycle.profile -eq 'incomplete') `
    'markers from different shader routes formed a false lifecycle'

$missingDelete = Get-XpD3d9ShaderLifecycle -DebugText @'
QXPDISP PS20 DEFINE+
QXPMP D3D SM2 GLSL DEFINE+
'@
Assert-D3d9Oracle (-not $missingDelete.valid -and
    $missingDelete.define -and -not $missingDelete.delete) `
    'native shader definition without deletion was accepted'

$declinedResult = 'QXP_D3D9_RESULT rc=0 hr=0x00000000 stage=complete runtime=system32 fixed-function=1 ps_1_1=1 ps_1_x-addressing=declined-unsupported a8-pattern=1 managed-mip=1 cubemap-mip=declined dynamic-cube=declined dynamic-texture=1 render-target=1 separate-fog=1 destination-alpha=1 depth-attachment=1 multistream7=0 resources-released=0 reset=1'
$declinedProfile = Get-XpD3d9VisualProfile -GuestResult $declinedResult
Assert-D3d9Oracle ($declinedProfile.name -eq 'declined-addressing' -and
    $declinedProfile.sceneExpected.Count -eq 14 -and
    $declinedProfile.resetExpected.cube -eq '123456') `
    'declined-addressing result did not select its conservative visual profile'

$r526Scene = [ordered]@{
    background = '144696'
    triangle = 'FF8000'
    texture = '00FF00'
    mip2 = '40A0E0'
    shader = 'FFFF00'
    bump = '144696'
    luminance = '144696'
    matrix = '144696'
    register = 'FF8000'
    fixedFourth = '00FFFF'
    fourth = '00FFFF'
    cube = '144696'
    pixelShader12 = '144696'
    pixelShader13Depth = 'FF0000'
    renderTargetNoDepth = '20C060'
    renderTargetDepth = 'E0A020'
    renderTargetClear = '400080'
    destinationAlpha = '400000'
    inverseDestinationAlpha = '00BF00'
    dynamicBefore = 'C02040'
    dynamicAfter = '20C080'
    multistream7 = '144696'
}
$r526Assessment = Get-XpD3d9SceneAssessment `
    -Profile $declinedProfile -Pixels $r526Scene
Assert-D3d9Oracle ($r526Assessment.valid -and
    $r526Assessment.score -eq 14 -and
    $r526Assessment.expectedScore -eq 14) `
    'exact supported r526 pixels failed declined-addressing coverage'
$r526Reset = Get-XpD3d9ResetAssessment -Profile $declinedProfile `
    -Pixels ([ordered]@{
        background = '123456'; mip2 = '40A0E0'; cube = '123456' })
Assert-D3d9Oracle $r526Reset.valid `
    'declined cubemap Reset background was rejected'

$badCoreScene = [ordered]@{}
foreach ($entry in $r526Scene.GetEnumerator()) {
    $badCoreScene[$entry.Key] = $entry.Value
}
$badCoreScene['dynamicAfter'] = '000000'
$badCoreAssessment = Get-XpD3d9SceneAssessment `
    -Profile $declinedProfile -Pixels $badCoreScene
Assert-D3d9Oracle (-not $badCoreAssessment.valid -and
    $badCoreAssessment.score -eq 13 -and
    $badCoreAssessment.mismatches.Count -eq 1) `
    'declined profile accepted a wrong supported pixel'

$fullResult = $declinedResult.Replace(
    'ps_1_x-addressing=declined-unsupported',
    'ps_1_x-addressing=unexpectedly-admitted').Replace(
    'cubemap-mip=declined', 'cubemap-mip=1').Replace(
    'dynamic-cube=declined', 'dynamic-cube=1')
$fullProfile = Get-XpD3d9VisualProfile -GuestResult $fullResult
$fullAssessment = Get-XpD3d9SceneAssessment `
    -Profile $fullProfile -Pixels $r526Scene
Assert-D3d9Oracle ($fullProfile.name -eq 'full-addressing' -and
    $fullProfile.sceneExpected.Count -eq 21 -and
    $fullProfile.resetExpected.cube -eq '8040FF' -and
    -not $fullAssessment.valid -and $fullAssessment.score -eq 14) `
    'full profile accepted declined-addressing background cells'

$multistreamProfile = Get-XpD3d9VisualProfile `
    -GuestResult $declinedResult -Multistream7 $true
$r526Scene['multistream7'] = '2AD46F'
$multistreamAssessment = Get-XpD3d9SceneAssessment `
    -Profile $multistreamProfile -Pixels $r526Scene
Assert-D3d9Oracle ($multistreamProfile.sceneExpected.Count -eq 15 -and
    $multistreamAssessment.valid -and
    $multistreamAssessment.score -eq 15) `
    'multistream pixel was not added to declined-addressing coverage'

Write-Output 'D3D9 oracle contract tests passed'
