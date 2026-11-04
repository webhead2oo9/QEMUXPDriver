$ErrorActionPreference = 'Stop'

function Get-XpD3d9ShaderLifecycle {
    param(
        [AllowEmptyString()]
        [string]$DebugText = ''
    )

    $legacyDisplayDefine = [regex]::IsMatch(
        $DebugText, '(?m)^QXPDISP PS DEFINE\+\r?$')
    $legacyHostDefine = [regex]::IsMatch(
        $DebugText, '(?m)^QXPMP D3D SHADER DEFINE\+\r?$')
    $legacyHostDelete = [regex]::IsMatch(
        $DebugText, '(?m)^QXPMP D3D SHADER DELETE\+\r?$')
    $nativeDisplayDefine = [regex]::IsMatch(
        $DebugText, '(?m)^QXPDISP PS20 DEFINE\+\r?$')
    $nativeHostDefine = [regex]::IsMatch(
        $DebugText, '(?m)^QXPMP D3D SM2 GLSL DEFINE\+\r?$')
    $nativeHostDelete = [regex]::IsMatch(
        $DebugText, '(?m)^QXPMP D3D SM2 GLSL DELETE\+\r?$')
    $legacyDefine = $legacyDisplayDefine -and $legacyHostDefine
    $nativeDefine = $nativeDisplayDefine -and $nativeHostDefine
    $legacyComplete = $legacyDefine -and $legacyHostDelete
    $nativeComplete = $nativeDefine -and $nativeHostDelete
    $profile = if ($legacyComplete -and $nativeComplete) {
        'legacy+native'
    } elseif ($nativeComplete) {
        'native'
    } elseif ($legacyComplete) {
        'legacy'
    } else {
        'incomplete'
    }

    return [pscustomobject][ordered]@{
        valid = $legacyComplete -or $nativeComplete
        profile = $profile
        define = $legacyDefine -or $nativeDefine
        delete = $legacyComplete -or $nativeComplete
        legacy = [pscustomobject][ordered]@{
            displayDefine = $legacyDisplayDefine
            hostDefine = $legacyHostDefine
            hostDelete = $legacyHostDelete
            complete = $legacyComplete
        }
        native = [pscustomobject][ordered]@{
            displayDefine = $nativeDisplayDefine
            hostDefine = $nativeHostDefine
            hostDelete = $nativeHostDelete
            complete = $nativeComplete
        }
    }
}

function Get-XpD3d9VisualProfile {
    param(
        [AllowEmptyString()]
        [string]$GuestResult = '',
        [bool]$Multistream7 = $false
    )

    $addressingDeclined = $GuestResult -match
        '(?:^| )ps_1_x-addressing=declined-unsupported(?: |$)'
    $cubeDeclined = $GuestResult -match
        '(?:^| )cubemap-mip=declined(?: |$)'
    $sceneExpected = [ordered]@{
        background = '144696'
        triangle = 'FF8000'
        texture = '00FF00'
        mip2 = '40A0E0'
        shader = 'FFFF00'
        fixedFourth = '00FFFF'
        fourth = '00FFFF'
        renderTargetNoDepth = '20C060'
        renderTargetDepth = 'E0A020'
        renderTargetClear = '400080'
        destinationAlpha = '400000'
        inverseDestinationAlpha = '00BF00'
        dynamicBefore = 'C02040'
        dynamicAfter = '20C080'
    }
    if (-not $addressingDeclined) {
        $sceneExpected['bump'] = '0000FF'
        $sceneExpected['luminance'] = '007F00'
        $sceneExpected['matrix'] = 'FF0000'
        $sceneExpected['register'] = 'FF00FF'
        $sceneExpected['cube'] = '8040FF'
        $sceneExpected['pixelShader12'] = 'FFFFFF'
        $sceneExpected['pixelShader13Depth'] = 'FFFF00'
    }
    if ($Multistream7) {
        $sceneExpected['multistream7'] = '2AD46F'
    }
    $profile = if ($addressingDeclined -and $cubeDeclined) {
        'declined-addressing'
    } elseif ($addressingDeclined) {
        'declined-addressing-cube-admitted'
    } elseif ($cubeDeclined) {
        'full-addressing-cube-declined'
    } else {
        'full-addressing'
    }

    return [pscustomobject][ordered]@{
        name = $profile
        addressingDeclined = $addressingDeclined
        cubeDeclined = $cubeDeclined
        sceneExpected = $sceneExpected
        resetExpected = [ordered]@{
            background = '123456'
            mip2 = '40A0E0'
            cube = if ($cubeDeclined) { '123456' } else { '8040FF' }
        }
    }
}

function Get-XpD3d9SceneAssessment {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Profile,
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Pixels
    )

    if (-not $Profile.sceneExpected) {
        throw 'D3D9 visual profile has no scene expectations'
    }
    $score = 0
    $mismatches = [Collections.Generic.List[string]]::new()
    foreach ($entry in $Profile.sceneExpected.GetEnumerator()) {
        $actual = if ($Pixels.Contains($entry.Key)) {
            [string]$Pixels[$entry.Key]
        } else {
            $null
        }
        if ($actual -ceq [string]$entry.Value) {
            ++$score
        } else {
            $mismatches.Add("$($entry.Key)=$actual expected=$($entry.Value)")
        }
    }
    $expectedScore = $Profile.sceneExpected.Count
    return [pscustomobject][ordered]@{
        valid = $score -eq $expectedScore
        score = $score
        expectedScore = $expectedScore
        mismatches = @($mismatches)
    }
}

function Get-XpD3d9ResetAssessment {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Profile,
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Pixels
    )

    if (-not $Profile.resetExpected) {
        throw 'D3D9 visual profile has no Reset expectations'
    }
    $mismatches = [Collections.Generic.List[string]]::new()
    foreach ($entry in $Profile.resetExpected.GetEnumerator()) {
        $actual = if ($Pixels.Contains($entry.Key)) {
            [string]$Pixels[$entry.Key]
        } else {
            $null
        }
        if ($actual -cne [string]$entry.Value) {
            $mismatches.Add("$($entry.Key)=$actual expected=$($entry.Value)")
        }
    }
    return [pscustomobject][ordered]@{
        valid = $mismatches.Count -eq 0
        mismatches = @($mismatches)
    }
}
