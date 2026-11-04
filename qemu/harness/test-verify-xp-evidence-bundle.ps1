$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
. (Join-Path $PSScriptRoot 'maxpayne-visual.ps1')
. (Join-Path $PSScriptRoot 'test-visual-bundle-fixtures.ps1')

function Assert-EvidenceVerifier {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-EvidenceBundleIdentity {
    param([string]$Directory)

    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    return (@(Get-ChildItem -LiteralPath $root -File -Recurse |
        Sort-Object FullName | ForEach-Object {
            [pscustomobject][ordered]@{
                path = $_.FullName.Substring($root.Length + 1)
                length = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName `
                    -Algorithm SHA256).Hash
            }
        }) | ConvertTo-Json -Depth 4 -Compress)
}

$verifierScript = Join-Path $PSScriptRoot 'verify-xp-evidence-bundle.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'

function Invoke-EvidenceVerifier {
    param(
        [string]$Directory,
        [string]$ReviewPath,
        [string]$OutputPath
    )
    $arguments = @(
        '-NoLogo', '-NoProfile', '-NonInteractive',
        '-ExecutionPolicy', 'Bypass',
        '-File', $verifierScript,
        '-BundleDirectory', $Directory
    )
    if ($ReviewPath) { $arguments += @('-ReviewPath', $ReviewPath) }
    if ($OutputPath) { $arguments += @('-OutputPath', $OutputPath) }
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $powershell @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedPreference
    }
    return [pscustomobject][ordered]@{
        exitCode = $exitCode
        output = @($output)
    }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-evidence-verifier-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    foreach ($mode in @('UT2003Visual', 'Warcraft3D3D8Visual')) {
        $bundle = Join-Path $testRoot $mode
        [void](New-GameObserveBundleFixture -Directory $bundle -Mode $mode)
        if ($mode -eq 'UT2003Visual') {
            $identityBeforePolicyChecks = Get-EvidenceBundleIdentity `
                -Directory $bundle
            $missingOutput = Invoke-EvidenceVerifier -Directory $bundle
            $insideOutput = Invoke-EvidenceVerifier -Directory $bundle `
                -OutputPath (Join-Path $bundle 'verification.json')
            $junctionPath = Join-Path $testRoot 'bundle-output-junction'
            $junctionOutput = $null
            try {
                [void](New-Item -ItemType Junction -Path $junctionPath `
                    -Target $bundle)
                $junctionOutput = Invoke-EvidenceVerifier `
                    -Directory $bundle `
                    -OutputPath (Join-Path $junctionPath 'verification.json')
            } finally {
                if (Test-Path -LiteralPath $junctionPath) {
                    [IO.Directory]::Delete($junctionPath)
                }
            }
            $identityAfterPolicyChecks = Get-EvidenceBundleIdentity `
                -Directory $bundle
            Assert-EvidenceVerifier `
                ($missingOutput.exitCode -eq 2 -and
                 ($missingOutput.output -join "`n") -like
                    '*-OutputPath is required*' -and
                 $insideOutput.exitCode -eq 2 -and
                 ($insideOutput.output -join "`n") -like
                    '*must be outside the preserved bundle*' -and
                 $junctionOutput.exitCode -eq 2 -and
                 ($junctionOutput.output -join "`n") -like
                    '*must not traverse a filesystem reparse point*' -and
                 $identityAfterPolicyChecks -ceq
                    $identityBeforePolicyChecks) `
                'verifier output policy allowed or modified its input bundle'
        }
        $verificationPath = Join-Path $testRoot `
            "$mode-verification.json"
        $bundleIdentityBefore = Get-EvidenceBundleIdentity -Directory $bundle
        $run = Invoke-EvidenceVerifier -Directory $bundle `
            -OutputPath $verificationPath
        $bundleIdentityAfter = Get-EvidenceBundleIdentity -Directory $bundle
        Assert-EvidenceVerifier ($run.exitCode -eq 0) `
            "$mode verification failed: $($run.output -join '; ')"
        Assert-EvidenceVerifier `
            ($bundleIdentityAfter -ceq $bundleIdentityBefore) `
            "$mode verifier modified the preserved bundle"
        Assert-EvidenceVerifier `
            (Test-Path -LiteralPath $verificationPath -PathType Leaf) `
            "$mode did not write its external verification record"
        $record = Get-Content -Raw -LiteralPath $verificationPath |
            ConvertFrom-Json
        $expectedVerdict = if ($mode -eq 'UT2003Visual') {
            'OPEN'
        } else { 'PASS' }
        Assert-EvidenceVerifier ($record.verdict -ceq $expectedVerdict -and
            $record.bundle.mode -ceq $mode -and
            $record.bundle.runId -ceq "fixture-$mode" -and
            $record.verifier.files.Count -eq 4 -and
            $record.checks.Count -eq 1 -and
            [bool]$record.checks[0].valid -and
            ($mode -ne 'UT2003Visual' -or
             (-not [bool]$record.review.bindingsValid -and
              @($record.review.findings | Where-Object {
                $_ -like '*no direct review record supplied*'
              }).Count -eq 1))) `
            "$mode verification record content is wrong"
        $rerun = Invoke-EvidenceVerifier -Directory $bundle `
            -OutputPath $verificationPath
        Assert-EvidenceVerifier ($rerun.exitCode -eq 2 -and
            ($rerun.output -join "`n") -like
                '*verification record already exists*') `
            "$mode overwrote an existing verification record"

        $tamperBundle = Join-Path $testRoot "$mode-tampered"
        $videoName = New-GameObserveBundleFixture `
            -Directory $tamperBundle -Mode $mode
        Add-Content -LiteralPath (Join-Path $tamperBundle $videoName) `
            -Value 'tampered'
        $tampered = Invoke-EvidenceVerifier -Directory $tamperBundle `
            -OutputPath (Join-Path $testRoot "$mode-tampered-verification.json")
        Assert-EvidenceVerifier ($tampered.exitCode -eq 1 -and
            ($tampered.output -join "`n") -like '*video hash mismatch*') `
            "$mode tamper was not a FAIL verdict"
        $tamperRecordPath = Join-Path $testRoot `
            "$mode-tampered-verification.json"
        Assert-EvidenceVerifier `
            (Test-Path -LiteralPath $tamperRecordPath -PathType Leaf) `
            "$mode FAIL verdict was not recorded"
        $tamperRecord = Get-Content -Raw -LiteralPath `
            $tamperRecordPath | ConvertFrom-Json
        Assert-EvidenceVerifier ($tamperRecord.verdict -ceq 'FAIL') `
            "$mode FAIL verdict content is wrong"
    }

    $frozenPixelBundle = Join-Path $testRoot `
        'ut2003-metadata-diverse-frozen'
    [void](New-GameObserveBundleFixture -Directory $frozenPixelBundle `
        -Mode 'UT2003Visual' -MetadataDiverseFrozenSamples)
    $frozenPixelOutput = Join-Path $testRoot `
        'ut2003-metadata-diverse-frozen-verification.json'
    $frozenPixelRun = Invoke-EvidenceVerifier `
        -Directory $frozenPixelBundle -OutputPath $frozenPixelOutput
    $frozenPixelRecord = Get-Content -Raw -LiteralPath $frozenPixelOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($frozenPixelRun.exitCode -eq 1 -and
         $frozenPixelRecord.verdict -ceq 'FAIL' -and
         ($frozenPixelRun.output -join "`n") -like
            '*fail recomputed canonical-pixel diversity*') `
        'metadata-diverse frozen retained samples passed bundle verification'

    $junctionBundle = Join-Path $testRoot 'ut2003-junction-descendant'
    [void](New-GameObserveBundleFixture `
        -Directory $junctionBundle -Mode 'UT2003Visual')
    $junctionSample = @(Get-ChildItem -LiteralPath $junctionBundle `
        -Filter 'xpdm-ut2003-sample-3-*.bmp')
    Assert-EvidenceVerifier ($junctionSample.Count -eq 1) `
        'junction fixture lacks its third retained sample'
    $junctionTarget = Join-Path $testRoot 'junction-outside-target'
    [void](New-Item -ItemType Directory -Path $junctionTarget)
    Remove-Item -LiteralPath $junctionSample[0].FullName -Force
    [void](New-Item -ItemType Junction `
        -Path $junctionSample[0].FullName -Target $junctionTarget)
    $junctionOutput = Join-Path $testRoot `
        'ut2003-junction-verification.json'
    $junctionRun = Invoke-EvidenceVerifier `
        -Directory $junctionBundle -OutputPath $junctionOutput
    $junctionRecord = Get-Content -Raw -LiteralPath $junctionOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($junctionRun.exitCode -eq 1 -and
         $junctionRecord.verdict -ceq 'FAIL' -and
         ($junctionRun.output -join "`n") -like
            '*reparse point*') `
        'reparse-point sample descendant passed bundle verification'
    [IO.Directory]::Delete($junctionSample[0].FullName)

    $wrongPixelBundle = Join-Path $testRoot 'warcraft-wrong-pixels'
    [void](New-GameObserveBundleFixture `
        -Directory $wrongPixelBundle -Mode 'Warcraft3D3D8Visual' `
        -PermutedVideo)
    $wrongPixelOutput = Join-Path $testRoot `
        'warcraft-wrong-pixels-verification.json'
    $wrongPixelRun = Invoke-EvidenceVerifier `
        -Directory $wrongPixelBundle -OutputPath $wrongPixelOutput
    $wrongPixelRecord = Get-Content -Raw -LiteralPath $wrongPixelOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($wrongPixelRun.exitCode -eq 1 -and
         $wrongPixelRecord.verdict -ceq 'FAIL' -and
         ($wrongPixelRun.output -join "`n") -like
            '*manifested sample*pixels*') `
        'content-correct metadata with wrong video pixels passed verification'

    $harnessTamperBundle = Join-Path $testRoot 'harness-result-tampered'
    [void](New-GameObserveBundleFixture `
        -Directory $harnessTamperBundle -Mode 'UT2003Visual')
    $harnessTamperPath = Join-Path $harnessTamperBundle `
        'harness-result.json'
    $harnessTamper = Get-Content -Raw -LiteralPath $harnessTamperPath |
        ConvertFrom-Json
    $harnessTamper.mode = '-xpdm-warcraft3d3d8'
    $harnessTamper.result = 'fail'
    [IO.File]::WriteAllText($harnessTamperPath,
        ($harnessTamper | ConvertTo-Json))
    $harnessTamperOutput = Join-Path $testRoot `
        'harness-result-tampered-verification.json'
    $harnessTamperIdentityBefore = Get-EvidenceBundleIdentity `
        -Directory $harnessTamperBundle
    $harnessTamperRun = Invoke-EvidenceVerifier `
        -Directory $harnessTamperBundle -OutputPath $harnessTamperOutput
    $harnessTamperIdentityAfter = Get-EvidenceBundleIdentity `
        -Directory $harnessTamperBundle
    $harnessTamperRecord = Get-Content -Raw -LiteralPath `
        $harnessTamperOutput | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($harnessTamperRun.exitCode -eq 1 -and
         ($harnessTamperRun.output -join "`n") -like
            '*harness result manifest size/hash mismatch*' -and
         $harnessTamperRecord.verdict -ceq 'FAIL' -and
         $harnessTamperRecord.bundle.mode -ceq
            'UntrustedHarnessResult' -and
         $null -eq $harnessTamperRecord.bundle.runId -and
         $harnessTamperIdentityAfter -ceq
            $harnessTamperIdentityBefore) `
        'unbound harness result was trusted or input bundle was modified'

    foreach ($malformedBound in @($false, $true)) {
        $malformedStem = if ($malformedBound) {
            'malformed-bound-harness'
        } else { 'malformed-unbound-harness' }
        $malformedBundle = Join-Path $testRoot $malformedStem
        [void](New-GameObserveBundleFixture `
            -Directory $malformedBundle -Mode 'UT2003Visual')
        $malformedResultPath = Join-Path $malformedBundle `
            'harness-result.json'
        [IO.File]::WriteAllText($malformedResultPath, '{')
        if ($malformedBound) {
            $malformedManifestPath = Join-Path $malformedBundle `
                'artifact-manifest.json'
            $malformedManifest = Get-Content -Raw -LiteralPath `
                $malformedManifestPath | ConvertFrom-Json
            $malformedHarnessRecord = @($malformedManifest.files |
                Where-Object { $_.name -ceq 'harness-result.json' })
            $malformedHarnessRecord[0].size =
                (Get-Item -LiteralPath $malformedResultPath).Length
            $malformedHarnessRecord[0].sha256 =
                (Get-FileHash -LiteralPath $malformedResultPath `
                    -Algorithm SHA256).Hash
            [IO.File]::WriteAllText($malformedManifestPath,
                ($malformedManifest | ConvertTo-Json -Depth 5))
        }
        $malformedOutput = Join-Path $testRoot `
            "$malformedStem-verification.json"
        $malformedIdentityBefore = Get-EvidenceBundleIdentity `
            -Directory $malformedBundle
        $malformedRun = Invoke-EvidenceVerifier `
            -Directory $malformedBundle -OutputPath $malformedOutput
        $malformedIdentityAfter = Get-EvidenceBundleIdentity `
            -Directory $malformedBundle
        $malformedRecord = Get-Content -Raw -LiteralPath `
            $malformedOutput | ConvertFrom-Json
        $expectedFinding = if ($malformedBound) {
            '*bound harness result is invalid*'
        } else { '*harness result manifest size/hash mismatch*' }
        Assert-EvidenceVerifier `
            ($malformedRun.exitCode -eq 1 -and
             ($malformedRun.output -join "`n") -like $expectedFinding -and
             $malformedRecord.verdict -ceq 'FAIL' -and
             $malformedRecord.bundle.mode -ceq
                'UntrustedHarnessResult' -and
             $null -eq $malformedRecord.bundle.reportedMode -and
             $malformedIdentityAfter -ceq $malformedIdentityBefore) `
            "$malformedStem did not fail closed without bundle mutation"
    }

    foreach ($historicalMode in @(
            '-xpdm-ut2003gameplay',
            '-xpdm-ut2003gameplayswvp')) {
        $historicalStem = $historicalMode.TrimStart('-').Replace('-', '_')
        $historicalUtBundle = Join-Path $testRoot $historicalStem
        [void](New-GameObserveBundleFixture `
            -Directory $historicalUtBundle -Mode 'UT2003Visual')
        $historicalUtResultPath = Join-Path $historicalUtBundle `
            'harness-result.json'
        $historicalUtResult = Get-Content -Raw -LiteralPath `
            $historicalUtResultPath | ConvertFrom-Json
        $historicalUtResult.mode = $historicalMode
        [IO.File]::WriteAllText($historicalUtResultPath,
            ($historicalUtResult | ConvertTo-Json))
        $historicalManifestPath = Join-Path $historicalUtBundle `
            'artifact-manifest.json'
        $historicalManifest = Get-Content -Raw -LiteralPath `
            $historicalManifestPath | ConvertFrom-Json
        $historicalHarnessRecord = @($historicalManifest.files |
            Where-Object { $_.name -ceq 'harness-result.json' })
        $historicalHarnessRecord[0].size =
            (Get-Item -LiteralPath $historicalUtResultPath).Length
        $historicalHarnessRecord[0].sha256 =
            (Get-FileHash -LiteralPath $historicalUtResultPath `
                -Algorithm SHA256).Hash
        [IO.File]::WriteAllText($historicalManifestPath,
            ($historicalManifest | ConvertTo-Json -Depth 5))
        $historicalOutput = Join-Path $testRoot `
            "$historicalStem-verification.json"
        $historicalIdentityBefore = Get-EvidenceBundleIdentity `
            -Directory $historicalUtBundle
        $historicalUtRun = Invoke-EvidenceVerifier `
            -Directory $historicalUtBundle -OutputPath $historicalOutput
        $historicalIdentityAfter = Get-EvidenceBundleIdentity `
            -Directory $historicalUtBundle
        $expectedMode = if ($historicalMode -ceq
                '-xpdm-ut2003gameplayswvp') {
            'UT2003DirectMapHistorical'
        } else { 'UT2003Gameplay' }
        $expectedScopeFinding = $historicalMode -cne
                '-xpdm-ut2003gameplayswvp' -or
            ($historicalUtRun.output -join "`n") -like
                '*software-VP direct-map bundle cannot prove ordinary joined gameplay*'
        Assert-EvidenceVerifier `
            ($historicalUtRun.exitCode -eq 1 -and $expectedScopeFinding) `
            "historical mode $historicalMode was admitted as visual proof"
        Assert-EvidenceVerifier `
            ($historicalIdentityAfter -ceq $historicalIdentityBefore) `
            "historical mode $historicalMode bundle was modified"
        $historicalUtRecord = Get-Content -Raw -LiteralPath `
            $historicalOutput | ConvertFrom-Json
        Assert-EvidenceVerifier `
            ($historicalUtRecord.verdict -ceq 'FAIL' -and
             $historicalUtRecord.bundle.mode -ceq $expectedMode -and
             -not [bool]$historicalUtRecord.checks[0].valid) `
            "historical mode $historicalMode rejection record is incorrect"
    }

    $currentUtGameplayBundle = Join-Path $testRoot `
        'UT2003Gameplay-current'
    [void](New-GameObserveBundleFixture `
        -Directory $currentUtGameplayBundle -Mode 'UT2003Gameplay')
    $currentUtGameplayOutput = Join-Path $testRoot `
        'ut2003-gameplay-current-verification.json'
    $currentUtGameplayRun = Invoke-EvidenceVerifier `
        -Directory $currentUtGameplayBundle `
        -OutputPath $currentUtGameplayOutput
    $currentUtGameplayRecord = Get-Content -Raw `
        -LiteralPath $currentUtGameplayOutput | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($currentUtGameplayRun.exitCode -eq 0 -and
         $currentUtGameplayRecord.verdict -ceq 'PASS' -and
         $currentUtGameplayRecord.bundle.mode -ceq 'UT2003Gameplay' -and
         [bool]$currentUtGameplayRecord.checks[0].valid) `
        'complete current UT2003 joined-gameplay bundle did not pass'

    $utReviewBundle = Join-Path $testRoot 'UT2003Visual-reviewed'
    [void](New-GameObserveBundleFixture `
        -Directory $utReviewBundle -Mode 'UT2003Visual')
    $utEvidence = Test-XpGameObserveVisualBundle `
        -Directory $utReviewBundle -Mode 'UT2003Visual'
    Assert-EvidenceVerifier $utEvidence.valid `
        'UT2003 direct-review fixture failed its automated bundle gate'
    $utReview = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-ut2003-main-menu-visual-direct-review'
        scope = 'post-visual-ready'
        verdict = 'pass'
        bindings = [pscustomobject][ordered]@{
            harnessResultSha256 = (Get-FileHash -LiteralPath (
                Join-Path $utReviewBundle 'harness-result.json') `
                -Algorithm SHA256).Hash
            artifactManifestSha256 = (Get-FileHash -LiteralPath (
                Join-Path $utReviewBundle 'artifact-manifest.json') `
                -Algorithm SHA256).Hash
            semanticTransition = 'main-menu'
            visualBoundary = 'post-visual-ready'
        }
        summary = [pscustomobject][ordered]@{
            reviewedSampleCount = 8
            menuVisibleCount = 8
            nvidiaLogoOnlyCount = 0
            glyphsLegibleCount = 8
            opaqueGlyphRectangleCount = 0
            reviewedVideoCount = 1
        }
        images = @($utEvidence.evidence.samples | ForEach-Object {
            [pscustomobject][ordered]@{
                fileName = $_.fileName
                sha256 = $_.sha256
                canonicalRgbSha256 = $_.canonicalRgbSha256
                menuVisible = $true
                nvidiaLogoOnly = $false
                glyphsLegible = $true
                opaqueGlyphRectanglesVisible = $false
            }
        })
        video = [pscustomobject][ordered]@{
            fileName = $utEvidence.evidence.video.fileName
            sha256 = $utEvidence.evidence.video.sha256
            reviewed = $true
        }
    }
    $utReviewPath = Join-Path $testRoot 'ut2003-main-menu-review.json'
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utPassPath = Join-Path $testRoot 'ut2003-reviewed-pass.json'
    $utPassRun = Invoke-EvidenceVerifier -Directory $utReviewBundle `
        -ReviewPath $utReviewPath -OutputPath $utPassPath
    $utPassRecord = Get-Content -Raw -LiteralPath $utPassPath |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utPassRun.exitCode -eq 0 -and
         $utPassRecord.verdict -ceq 'PASS' -and
         [bool]$utPassRecord.review.bundleIdentityValid -and
         [bool]$utPassRecord.review.bindingsValid -and
         [bool]$utPassRecord.review.contentValid -and
         [bool]$utPassRecord.review.supportsPass) `
        'bound intact UT2003 menu review did not produce PASS'

    $utReview.scope = 'post-main-menu'
    $utReview.bindings.visualBoundary = 'post-main-menu'
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utLegacyBoundaryPath = Join-Path $testRoot `
        'ut2003-legacy-boundary-pass.json'
    $utLegacyBoundaryRun = Invoke-EvidenceVerifier `
        -Directory $utReviewBundle -ReviewPath $utReviewPath `
        -OutputPath $utLegacyBoundaryPath
    $utLegacyBoundaryRecord = Get-Content -Raw `
        -LiteralPath $utLegacyBoundaryPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utLegacyBoundaryRun.exitCode -eq 1 -and
         $utLegacyBoundaryRecord.verdict -ceq 'FAIL' -and
         [bool]$utLegacyBoundaryRecord.review.bundleIdentityValid -and
         -not [bool]$utLegacyBoundaryRecord.review.bindingsValid -and
         -not [bool]$utLegacyBoundaryRecord.review.contentValid) `
        'legacy post-main-menu review boundary did not fail closed'
    $utReview.scope = 'post-visual-ready'
    $utReview.bindings.visualBoundary = 'post-visual-ready'

    $utReview.verdict = 'fail'
    $utReview.images[0].menuVisible = $false
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utFailPath = Join-Path $testRoot 'ut2003-reviewed-fail.json'
    $utFailRun = Invoke-EvidenceVerifier -Directory $utReviewBundle `
        -ReviewPath $utReviewPath -OutputPath $utFailPath
    $utFailRecord = Get-Content -Raw -LiteralPath $utFailPath |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utFailRun.exitCode -eq 1 -and
         $utFailRecord.verdict -ceq 'FAIL' -and
         [bool]$utFailRecord.review.bindingsValid) `
        'bound UT2003 fail review did not produce FAIL'

    $utReview.verdict = 'pass'
    $utReview.images[0].menuVisible = $true
    $utReview.images[0].glyphsLegible = $false
    $utReview.summary.glyphsLegibleCount = 7
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utMalformedPassPath = Join-Path $testRoot `
        'ut2003-malformed-pass.json'
    $utMalformedPassRun = Invoke-EvidenceVerifier `
        -Directory $utReviewBundle -ReviewPath $utReviewPath `
        -OutputPath $utMalformedPassPath
    $utMalformedPassRecord = Get-Content -Raw `
        -LiteralPath $utMalformedPassPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utMalformedPassRun.exitCode -eq 1 -and
         $utMalformedPassRecord.verdict -ceq 'FAIL' -and
         [bool]$utMalformedPassRecord.review.bindingsValid -and
         -not [bool]$utMalformedPassRecord.review.supportsPass) `
        'malformed UT2003 pass review did not fail closed'

    $utReview.images[0].glyphsLegible = $true
    $utReview.summary.glyphsLegibleCount = 8
    $utReview.images[0].PSObject.Properties.Remove('glyphsLegible')
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utMissingBooleanPath = Join-Path $testRoot `
        'ut2003-missing-boolean-pass.json'
    $utMissingBooleanRun = Invoke-EvidenceVerifier `
        -Directory $utReviewBundle -ReviewPath $utReviewPath `
        -OutputPath $utMissingBooleanPath
    $utMissingBooleanRecord = Get-Content -Raw `
        -LiteralPath $utMissingBooleanPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utMissingBooleanRun.exitCode -eq 1 -and
         $utMissingBooleanRecord.verdict -ceq 'FAIL' -and
         [bool]$utMissingBooleanRecord.review.bundleIdentityValid -and
         [bool]$utMissingBooleanRecord.review.bindingsValid -and
         -not [bool]$utMissingBooleanRecord.review.contentValid) `
        'bundle-bound pass with a missing visual boolean stayed OPEN'
    $utReview.images[0] | Add-Member -NotePropertyName glyphsLegible `
        -NotePropertyValue $true

    $utReview.PSObject.Properties.Remove('schema')
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $missingSchemaPath = Join-Path $testRoot `
        'ut2003-missing-schema-pass.json'
    $missingSchemaRun = Invoke-EvidenceVerifier `
        -Directory $utReviewBundle -ReviewPath $utReviewPath `
        -OutputPath $missingSchemaPath
    $missingSchemaRecord = Get-Content -Raw -LiteralPath $missingSchemaPath |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($missingSchemaRun.exitCode -eq 1 -and
         $missingSchemaRecord.verdict -ceq 'FAIL' -and
         [bool]$missingSchemaRecord.review.bundleIdentityValid -and
         -not [bool]$missingSchemaRecord.review.contentValid) `
        'bundle-bound pass without schema stayed OPEN or passed'
    $utReview | Add-Member -NotePropertyName schema -NotePropertyValue 1

    foreach ($zeroField in @(
            'nvidiaLogoOnlyCount', 'opaqueGlyphRectangleCount')) {
        $utReview.summary.PSObject.Properties.Remove($zeroField)
        [IO.File]::WriteAllText($utReviewPath,
            ($utReview | ConvertTo-Json -Depth 10))
        $missingZeroPath = Join-Path $testRoot `
            "ut2003-missing-$zeroField-pass.json"
        $missingZeroRun = Invoke-EvidenceVerifier `
            -Directory $utReviewBundle -ReviewPath $utReviewPath `
            -OutputPath $missingZeroPath
        $missingZeroRecord = Get-Content -Raw -LiteralPath $missingZeroPath |
            ConvertFrom-Json
        Assert-EvidenceVerifier `
            ($missingZeroRun.exitCode -eq 1 -and
             $missingZeroRecord.verdict -ceq 'FAIL' -and
             [bool]$missingZeroRecord.review.bundleIdentityValid -and
             -not [bool]$missingZeroRecord.review.contentValid) `
            "bundle-bound pass without $zeroField stayed OPEN or passed"
        $utReview.summary | Add-Member -NotePropertyName $zeroField `
            -NotePropertyValue 0
    }

    $utReview.summary.menuVisibleCount = '8'
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $nonIntegerSummaryPath = Join-Path $testRoot `
        'ut2003-string-summary-pass.json'
    $nonIntegerSummaryRun = Invoke-EvidenceVerifier `
        -Directory $utReviewBundle -ReviewPath $utReviewPath `
        -OutputPath $nonIntegerSummaryPath
    $nonIntegerSummaryRecord = Get-Content -Raw `
        -LiteralPath $nonIntegerSummaryPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($nonIntegerSummaryRun.exitCode -eq 1 -and
         $nonIntegerSummaryRecord.verdict -ceq 'FAIL' -and
         [bool]$nonIntegerSummaryRecord.review.bundleIdentityValid -and
         -not [bool]$nonIntegerSummaryRecord.review.contentValid) `
        'bundle-bound pass with string summary count stayed OPEN or passed'
    $utReview.summary.menuVisibleCount = 8

    $utReview.bindings.harnessResultSha256 = '0' * 64
    [IO.File]::WriteAllText($utReviewPath,
        ($utReview | ConvertTo-Json -Depth 10))
    $utForeignPath = Join-Path $testRoot 'ut2003-foreign-review.json'
    $utForeignRun = Invoke-EvidenceVerifier -Directory $utReviewBundle `
        -ReviewPath $utReviewPath -OutputPath $utForeignPath
    $utForeignRecord = Get-Content -Raw -LiteralPath $utForeignPath |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($utForeignRun.exitCode -eq 0 -and
         $utForeignRecord.verdict -ceq 'OPEN' -and
         -not [bool]$utForeignRecord.review.bundleIdentityValid -and
         -not [bool]$utForeignRecord.review.bindingsValid) `
        'foreign UT2003 review did not stay OPEN'

    $maxBundle = Join-Path $testRoot 'MaxPayneVisual'
    $sidecar = New-MaxPayneVisualBundleFixture -Directory $maxBundle
    $openRecordPath = Join-Path $testRoot `
        'MaxPayneVisual-open-verification.json'
    $openRun = Invoke-EvidenceVerifier -Directory $maxBundle `
        -OutputPath $openRecordPath
    Assert-EvidenceVerifier ($openRun.exitCode -eq 0) `
        "MaxPayne verification without review failed: $($openRun.output -join '; ')"
    Assert-EvidenceVerifier `
        (Test-Path -LiteralPath $openRecordPath -PathType Leaf) `
        'MaxPayne OPEN verdict was not recorded'
    $openRecord = Get-Content -Raw -LiteralPath `
        $openRecordPath | ConvertFrom-Json
    Assert-EvidenceVerifier ($openRecord.verdict -ceq 'OPEN' -and
        -not $openRecord.review.bindingsValid -and
        @($openRecord.review.findings | Where-Object {
            $_ -like '*no direct review record supplied*'
        }).Count -eq 1) `
        'MaxPayne without review did not stay OPEN'

    $reviewBundle = Join-Path $testRoot 'MaxPayneVisual-reviewed'
    $sidecar = New-MaxPayneVisualBundleFixture -Directory $reviewBundle
    $harnessHash = (Get-FileHash -LiteralPath (
        Join-Path $reviewBundle 'harness-result.json') `
        -Algorithm SHA256).Hash
    $sidecarHash = (Get-FileHash -LiteralPath (
        Join-Path $reviewBundle 'maxpayne-visual-sequence.json') `
        -Algorithm SHA256).Hash
    $review = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-maxpayne-visual-direct-review'
        verdict = 'pass'
        bindings = [pscustomobject][ordered]@{
            harnessResultSha256 = $harnessHash
            visualSequenceSha256 = $sidecarHash
        }
        summary = [pscustomobject][ordered]@{
            reviewedImageCount = 3
            visibleFrameCount = 3
            renderingCorrectCount = 3
        }
        images = @($sidecar.images | ForEach-Object {
            [pscustomobject][ordered]@{
                fileName = $_.fileName
                sha256 = $_.sha256
                frameVisible = $true
                renderingCorrect = $true
            }
        })
    }
    $reviewPath = Join-Path $testRoot 'maxpayne-review.json'
    [IO.File]::WriteAllText($reviewPath,
        ($review | ConvertTo-Json -Depth 8))
    $passOutputPath = Join-Path $testRoot 'verification-pass.json'
    $passRun = Invoke-EvidenceVerifier -Directory $reviewBundle `
        -ReviewPath $reviewPath -OutputPath $passOutputPath
    Assert-EvidenceVerifier ($passRun.exitCode -eq 0) `
        "MaxPayne reviewed verification failed: $($passRun.output -join '; ')"
    $passRecord = Get-Content -Raw -LiteralPath `
        $passOutputPath | ConvertFrom-Json
    Assert-EvidenceVerifier ($passRecord.verdict -ceq 'PASS' -and
        $passRecord.review.bindingsValid -and
        $passRecord.review.contentValid -and
        $passRecord.review.supportsPass) `
        'bound pass review did not produce a PASS verdict'

    $review.images[0].frameVisible = $false
    $review.summary.visibleFrameCount = 2
    [IO.File]::WriteAllText($reviewPath,
        ($review | ConvertTo-Json -Depth 8))
    $incompletePassPath = Join-Path $testRoot `
        'verification-incomplete-pass.json'
    $incompletePassRun = Invoke-EvidenceVerifier `
        -Directory $reviewBundle -ReviewPath $reviewPath `
        -OutputPath $incompletePassPath
    $incompletePassRecord = Get-Content -Raw `
        -LiteralPath $incompletePassPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($incompletePassRun.exitCode -eq 1 -and
         $incompletePassRecord.verdict -ceq 'FAIL' -and
         $incompletePassRecord.review.bindingsValid -and
         -not $incompletePassRecord.review.supportsPass) `
        'bound Max Payne pass with an unseen frame did not fail closed'
    $review.images[0].frameVisible = $true
    $review.summary.visibleFrameCount = 3

    $review.images[0].PSObject.Properties.Remove('renderingCorrect')
    [IO.File]::WriteAllText($reviewPath,
        ($review | ConvertTo-Json -Depth 8))
    $missingBooleanPath = Join-Path $testRoot `
        'verification-missing-boolean.json'
    $missingBooleanRun = Invoke-EvidenceVerifier `
        -Directory $reviewBundle -ReviewPath $reviewPath `
        -OutputPath $missingBooleanPath
    $missingBooleanRecord = Get-Content -Raw `
        -LiteralPath $missingBooleanPath | ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($missingBooleanRun.exitCode -eq 1 -and
         $missingBooleanRecord.verdict -ceq 'FAIL' -and
         $missingBooleanRecord.review.bindingsValid -and
         -not $missingBooleanRecord.review.contentValid) `
        'bound Max Payne pass with a missing visual boolean did not fail closed'
    $review.images[0] | Add-Member -NotePropertyName renderingCorrect `
        -NotePropertyValue $true

    $review.verdict = 'fail'
    [IO.File]::WriteAllText($reviewPath,
        ($review | ConvertTo-Json -Depth 8))
    $failRun = Invoke-EvidenceVerifier -Directory $reviewBundle `
        -ReviewPath $reviewPath `
        -OutputPath (Join-Path $testRoot 'verification-fail.json')
    Assert-EvidenceVerifier ($failRun.exitCode -eq 1) `
        'bound fail review did not produce a FAIL verdict'

    $review.verdict = 'pass'
    $review.bindings.harnessResultSha256 = '0' * 64
    [IO.File]::WriteAllText($reviewPath,
        ($review | ConvertTo-Json -Depth 8))
    $foreignRun = Invoke-EvidenceVerifier -Directory $reviewBundle `
        -ReviewPath $reviewPath `
        -OutputPath (Join-Path $testRoot 'verification-foreign.json')
    Assert-EvidenceVerifier ($foreignRun.exitCode -eq 0) `
        'foreign review did not stay non-fatal'
    $foreignRecord = Get-Content -Raw -LiteralPath `
        (Join-Path $testRoot 'verification-foreign.json') | ConvertFrom-Json
    Assert-EvidenceVerifier ($foreignRecord.verdict -ceq 'OPEN' -and
        -not $foreignRecord.review.bindingsValid -and
        @($foreignRecord.review.findings | Where-Object {
            $_ -like '*does not bind this harness result*'
        }).Count -eq 1) `
        'foreign review record was bound to this bundle'

    $duplicateBundle = Join-Path $testRoot 'MaxPayneVisual-duplicate-names'
    [void](New-MaxPayneVisualBundleFixture -Directory $duplicateBundle)
    foreach ($jsonName in @(
            'maxpayne-visual-sequence.json', 'harness-result.json')) {
        $jsonPath = Join-Path $duplicateBundle $jsonName
        $document = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
        $evidenceRoot = if ($jsonName -ceq 'harness-result.json') {
            $document.visualEvidence
        } else { $document }
        $evidenceRoot.images[1].fileName =
            [string]$evidenceRoot.images[0].fileName
        [IO.File]::WriteAllText($jsonPath,
            ($document | ConvertTo-Json -Depth 12))
    }
    $duplicateManifestPath = Join-Path $duplicateBundle `
        'artifact-manifest.json'
    $duplicateManifest = Get-Content -Raw -LiteralPath `
        $duplicateManifestPath | ConvertFrom-Json
    $duplicateHarnessRecord = @($duplicateManifest.files | Where-Object {
        $_.name -ceq 'harness-result.json'
    })
    $duplicateHarnessPath = Join-Path $duplicateBundle 'harness-result.json'
    $duplicateHarnessRecord[0].size =
        (Get-Item -LiteralPath $duplicateHarnessPath).Length
    $duplicateHarnessRecord[0].sha256 =
        (Get-FileHash -LiteralPath $duplicateHarnessPath `
            -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($duplicateManifestPath,
        (($duplicateManifest | ConvertTo-Json -Depth 5) + "`n"))
    $duplicateOutput = Join-Path $testRoot `
        'MaxPayneVisual-duplicate-verification.json'
    $duplicateRun = Invoke-EvidenceVerifier `
        -Directory $duplicateBundle -OutputPath $duplicateOutput
    $duplicateRecord = Get-Content -Raw -LiteralPath $duplicateOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($duplicateRun.exitCode -eq 1 -and
         $duplicateRecord.verdict -ceq 'FAIL' -and
         ($duplicateRun.output -join "`n") -like
            '*file name is not unique*') `
        'duplicate Max Payne image names passed verification'

    $escapeBundle = Join-Path $testRoot 'MaxPayneVisual-out-of-bundle'
    [void](New-MaxPayneVisualBundleFixture -Directory $escapeBundle)
    foreach ($jsonName in @(
            'maxpayne-visual-sequence.json', 'harness-result.json')) {
        $jsonPath = Join-Path $escapeBundle $jsonName
        $document = Get-Content -Raw -LiteralPath $jsonPath | ConvertFrom-Json
        $evidenceRoot = if ($jsonName -ceq 'harness-result.json') {
            $document.visualEvidence
        } else { $document }
        $evidenceRoot.images[0].fileName = '..\outside.bmp'
        [IO.File]::WriteAllText($jsonPath,
            ($document | ConvertTo-Json -Depth 12))
    }
    $escapeManifestPath = Join-Path $escapeBundle `
        'artifact-manifest.json'
    $escapeManifest = Get-Content -Raw -LiteralPath `
        $escapeManifestPath | ConvertFrom-Json
    $escapeHarnessRecord = @($escapeManifest.files | Where-Object {
        $_.name -ceq 'harness-result.json'
    })
    $escapeHarnessPath = Join-Path $escapeBundle 'harness-result.json'
    $escapeHarnessRecord[0].size =
        (Get-Item -LiteralPath $escapeHarnessPath).Length
    $escapeHarnessRecord[0].sha256 =
        (Get-FileHash -LiteralPath $escapeHarnessPath `
            -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($escapeManifestPath,
        (($escapeManifest | ConvertTo-Json -Depth 5) + "`n"))
    $escapeOutput = Join-Path $testRoot `
        'MaxPayneVisual-escape-verification.json'
    $escapeRun = Invoke-EvidenceVerifier `
        -Directory $escapeBundle -OutputPath $escapeOutput
    $escapeRecord = Get-Content -Raw -LiteralPath $escapeOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($escapeRun.exitCode -eq 1 -and
         $escapeRecord.verdict -ceq 'FAIL' -and
         ($escapeRun.output -join "`n") -like
            '*escapes the canonical bundle*') `
        'out-of-bundle Max Payne image name passed verification'

    $nonBitmapBundle = Join-Path $testRoot 'MaxPayneVisual-non-bitmap'
    [void](New-MaxPayneVisualBundleFixture `
        -Directory $nonBitmapBundle -NonBitmapBytes)
    $nonBitmapOutput = Join-Path $testRoot `
        'MaxPayneVisual-non-bitmap-verification.json'
    $nonBitmapRun = Invoke-EvidenceVerifier `
        -Directory $nonBitmapBundle -OutputPath $nonBitmapOutput
    $nonBitmapRecord = Get-Content -Raw -LiteralPath $nonBitmapOutput |
        ConvertFrom-Json
    Assert-EvidenceVerifier `
        ($nonBitmapRun.exitCode -eq 1 -and
         $nonBitmapRecord.verdict -ceq 'FAIL' -and
         ($nonBitmapRun.output -join "`n") -like
            '*not a decodable bitmap*') `
        'non-bitmap Max Payne image bytes passed verification'

    Write-Output 'evidence bundle verifier tests passed'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}
