param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$BundleDirectory,
    [string]$OutputPath,
    [string]$ReviewPath
)

# Read-only bundle verification. The caller must select an append-only output
# outside the bundle; the verifier never adds, removes, or changes any file in
# the preserved evidence directory.
# Exit codes: 0 = PASS or OPEN (OPEN is recorded, not failed), 1 = FAIL,
# 2 = usage/identity error.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'game-observe-visual.ps1')
. (Join-Path $PSScriptRoot 'maxpayne-visual.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')

function Get-XpEvidenceVerifierIdentity {
    $files = @(
        'verify-xp-evidence-bundle.ps1',
        'game-observe-visual.ps1',
        'maxpayne-visual.ps1',
        'acceptance-run-artifacts.ps1'
    )
    $identities = [Collections.Generic.List[object]]::new()
    $combinedBytes = [Collections.Generic.List[byte]]::new()
    foreach ($name in $files) {
        $path = Join-Path $PSScriptRoot $name
        $bytes = [IO.File]::ReadAllBytes($path)
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $identities.Add([pscustomobject][ordered]@{
            path = "qemu/harness/$name"
            sha256 = $hash
        })
        $combinedBytes.AddRange([byte[]]$bytes)
    }
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $combined = [BitConverter]::ToString(
            $sha256.ComputeHash($combinedBytes.ToArray())).Replace('-', '')
    } finally {
        $sha256.Dispose()
    }
    return [pscustomobject][ordered]@{
        digest = $combined
        files = @($identities)
    }
}

function Exit-XpEvidenceVerifierUsage {
    param([string]$Message)
    [Console]::Error.WriteLine($Message)
    exit 2
}

function Test-XpEvidencePathHasReparsePoint {
    param([string]$Path)

    $candidate = [IO.Path]::GetFullPath($Path)
    while (-not (Test-Path -LiteralPath $candidate)) {
        $parent = [IO.Path]::GetDirectoryName($candidate)
        if ([string]::IsNullOrWhiteSpace($parent) -or
            $parent -ceq $candidate) {
            return $false
        }
        $candidate = $parent
    }
    while (-not [string]::IsNullOrWhiteSpace($candidate)) {
        $item = Get-Item -LiteralPath $candidate -Force
        if (([IO.FileAttributes]$item.Attributes -band
             [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $true
        }
        $parent = [IO.Path]::GetDirectoryName($candidate)
        if ([string]::IsNullOrWhiteSpace($parent) -or
            $parent -ceq $candidate) {
            break
        }
        $candidate = $parent
    }
    return $false
}


$bundleInput = [IO.Path]::GetFullPath($BundleDirectory)
if (Test-XpEvidencePathHasReparsePoint -Path $bundleInput) {
    Exit-XpEvidenceVerifierUsage `
        '-BundleDirectory must not traverse a filesystem reparse point'
}
$bundle = (Resolve-Path -LiteralPath $bundleInput).Path
$resultPath = Resolve-XpBundleConfinedPath `
    -BundleRoot $bundle -Name 'harness-result.json'
if (-not $resultPath) {
    Exit-XpEvidenceVerifierUsage `
        'harness-result.json must be a confined non-reparse file at the bundle root'
}
if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
    Exit-XpEvidenceVerifierUsage `
        "bundle is missing harness-result.json: $bundle"
}
try {
    $resultBytes = [IO.File]::ReadAllBytes($resultPath)
} catch {
    Exit-XpEvidenceVerifierUsage `
        "harness-result.json cannot be read: $($_.Exception.Message)"
}
$result = $null
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Exit-XpEvidenceVerifierUsage `
        '-OutputPath is required and must be outside the preserved bundle'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-XpEvidencePathHasReparsePoint -Path $OutputPath) {
    Exit-XpEvidenceVerifierUsage `
        '-OutputPath must not traverse a filesystem reparse point'
}
$bundlePrefix = $bundle.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($OutputPath.Equals($bundle, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputPath.StartsWith(
        $bundlePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    Exit-XpEvidenceVerifierUsage `
        '-OutputPath must be outside the preserved bundle'
}
$harnessResultSize = $resultBytes.LongLength
$resultSha256 = [Security.Cryptography.SHA256]::Create()
try {
    $harnessResultHash = [BitConverter]::ToString(
        $resultSha256.ComputeHash($resultBytes)).Replace('-', '')
} finally {
    $resultSha256.Dispose()
}
$manifestPath = Resolve-XpBundleConfinedPath `
    -BundleRoot $bundle -Name 'artifact-manifest.json'
$manifest = $null
$manifestHash = $null
$harnessBindingFindings = [Collections.Generic.List[string]]::new()
if (-not $manifestPath) {
    $harnessBindingFindings.Add('artifact manifest is a reparse point or escapes the canonical bundle')
} elseif (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    $harnessBindingFindings.Add('artifact manifest is missing')
} else {
    $manifestHash = (Get-FileHash -LiteralPath $manifestPath `
        -Algorithm SHA256).Hash
    try {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath |
            ConvertFrom-Json
    } catch {
        $harnessBindingFindings.Add(
            "artifact manifest is invalid: $($_.Exception.Message)")
    }
}
$harnessRecords = @(if ($manifest) {
    $manifest.files | Where-Object {
        [string]$_.name -ceq 'harness-result.json'
    }
})
if ($manifest -and $harnessRecords.Count -ne 1) {
    $harnessBindingFindings.Add(
        'harness result is not uniquely manifested')
} elseif ($harnessRecords.Count -eq 1 -and
    ($harnessResultSize -ne [int64]$harnessRecords[0].size -or
     $harnessResultHash -cne [string]$harnessRecords[0].sha256)) {
    $harnessBindingFindings.Add(
        'harness result manifest size/hash mismatch')
}
if ($harnessBindingFindings.Count -eq 0) {
    try {
        $result = ConvertFrom-XpManifestTextBytes -Bytes $resultBytes |
            ConvertFrom-Json
    } catch {
        $harnessBindingFindings.Add(
            "bound harness result is invalid: $($_.Exception.Message)")
    }
}
$harnessResultBindingValid = $harnessBindingFindings.Count -eq 0
$historicalUtDirectMap = $false
if ($harnessResultBindingValid) {
    $historicalUtDirectMap = [string]$result.mode -ceq
        '-xpdm-ut2003gameplayswvp'
    $mode = switch ([string]$result.mode) {
        '-xpdm-ut2003' { 'UT2003Visual' }
        '-xpdm-ut2003gameplay' { 'UT2003Gameplay' }
        '-xpdm-ut2003gameplayswvp' { 'UT2003DirectMapHistorical' }
        '-xpdm-warcraft3d3d8' { 'Warcraft3D3D8Visual' }
        '-xpdm-d3d-gameplay' { 'MaxPayneVisual' }
        default {
            Exit-XpEvidenceVerifierUsage `
                "bundle mode '$($result.mode)' is not a visual evidence mode"
        }
    }
} else {
    $mode = 'UntrustedHarnessResult'
}

$verifier = Get-XpEvidenceVerifierIdentity
$checks = [Collections.Generic.List[object]]::new()
if (-not $harnessResultBindingValid) {
    $checks.Add([pscustomobject][ordered]@{
        name = 'harness-result-manifest-binding'
        valid = $false
        findings = @($harnessBindingFindings)
    })
    $evidence = [pscustomobject][ordered]@{
        kind = 'xpdriver-untrusted-harness-result'
    }
} elseif ($historicalUtDirectMap) {
    $checks.Add([pscustomobject][ordered]@{
        name = 'ut2003-normal-menu-scope'
        valid = $false
        findings = @(
            'software-VP direct-map bundle cannot prove ordinary joined gameplay')
    })
    $evidence = [pscustomobject][ordered]@{
        kind = 'xpdriver-ut2003-direct-map-historical'
        semanticTransition = $null
    }
} elseif ($mode -eq 'MaxPayneVisual') {
    $check = Test-XpMaxPayneVisualBundle -Directory $bundle
    $checks.Add([pscustomobject][ordered]@{
        name = 'maxpayne-visual-bundle'
        valid = [bool]$check.valid
        findings = @($check.findings)
    })
    $evidence = $check.evidence
} else {
    $check = Test-XpGameObserveVisualBundle -Directory $bundle -Mode $mode
    $checks.Add([pscustomobject][ordered]@{
        name = 'game-observe-visual-bundle'
        valid = [bool]$check.valid
        findings = @($check.findings)
    })
    $evidence = $check.evidence
}

$review = $null
if ($mode -eq 'UT2003Visual') {
    $reviewFindings = [Collections.Generic.List[string]]::new()
    $identityFindings = [Collections.Generic.List[string]]::new()
    $bindingFindings = [Collections.Generic.List[string]]::new()
    $contentFindings = [Collections.Generic.List[string]]::new()
    $passFindings = [Collections.Generic.List[string]]::new()
    $reviewSupportsPass = $false
    if (-not $ReviewPath) {
        $reviewFindings.Add('no direct review record supplied; automated ' +
            'post-boundary evidence is valid but menu rendering remains open')
    } elseif (-not (Test-Path -LiteralPath $ReviewPath -PathType Leaf)) {
        $reviewFindings.Add("review record not found: $ReviewPath")
    } else {
        try {
            $reviewRecord = Get-Content -Raw -LiteralPath $ReviewPath |
                ConvertFrom-Json
        } catch {
            $reviewRecord = $null
            $reviewFindings.Add(
                "review record is invalid: $($_.Exception.Message)")
        }
        if ($reviewRecord) {
            if ([string]$reviewRecord.bindings.harnessResultSha256 -cne
                    $harnessResultHash) {
                $identityFindings.Add('review record does not bind this ' +
                    'harness result hash')
            }
            if ([string]$reviewRecord.bindings.artifactManifestSha256 -cne
                    $manifestHash) {
                $identityFindings.Add('review record does not bind this ' +
                    'artifact manifest hash')
            }
            $schemaProperty = $reviewRecord.PSObject.Properties['schema']
            if ($null -eq $schemaProperty -or
                $schemaProperty.Value -isnot [int] -or
                [int]$schemaProperty.Value -ne 1) {
                $contentFindings.Add('review record schema must be exact ' +
                    'integer version 1')
            }
            if ([string]$reviewRecord.kind -cne
                    'xpdriver-ut2003-main-menu-visual-direct-review') {
                $contentFindings.Add('review record kind is not a UT2003 ' +
                    'main-menu direct review')
            }
            if ([string]$reviewRecord.verdict -cnotin @('pass', 'fail')) {
                $contentFindings.Add('review verdict is not pass or fail')
            }
            if ([string]$reviewRecord.bindings.semanticTransition -cne
                    'main-menu' -or
                [string]$reviewRecord.bindings.visualBoundary -cne
                    'post-visual-ready') {
                $bindingFindings.Add('review record does not bind the ' +
                    'post-visual-ready main-menu evidence')
            }
            if ([string]$reviewRecord.scope -cne 'post-visual-ready') {
                $contentFindings.Add('review scope is not post-visual-ready')
            }
            $summaryExpectations = [ordered]@{
                reviewedSampleCount = 8
                menuVisibleCount = 8
                nvidiaLogoOnlyCount = 0
                glyphsLegibleCount = 8
                opaqueGlyphRectangleCount = 0
                reviewedVideoCount = 1
            }
            foreach ($name in $summaryExpectations.Keys) {
                $property = if ($reviewRecord.summary) {
                    $reviewRecord.summary.PSObject.Properties[$name]
                } else { $null }
                if ($null -eq $property -or
                    $property.Value -isnot [int] -or
                    [int]$property.Value -ne $summaryExpectations[$name]) {
                    $contentFindings.Add("review summary field $name must be " +
                        "exact integer $($summaryExpectations[$name])")
                }
            }
            $reviewImages = @($reviewRecord.images)
            if ($reviewImages.Count -ne 8) {
                $contentFindings.Add('review record must contain exactly ' +
                    'eight sample reviews')
            }
            if ($evidence) {
                foreach ($image in @($evidence.samples)) {
                    $bound = @($reviewImages | Where-Object {
                        [string]$_.fileName -ceq [string]$image.fileName
                    })
                    if ($bound.Count -ne 1 -or
                        [string]$bound[0].sha256 -cne
                            [string]$image.sha256 -or
                        [string]$bound[0].canonicalRgbSha256 -cne
                            [string]$image.canonicalRgbSha256) {
                        $bindingFindings.Add('review record does not bind ' +
                            "sample $($image.fileName)")
                    }
                    if ($bound.Count -eq 1 -and
                        (($bound[0].menuVisible -isnot [bool]) -or
                         ($bound[0].nvidiaLogoOnly -isnot [bool]) -or
                         ($bound[0].glyphsLegible -isnot [bool]) -or
                         ($bound[0].opaqueGlyphRectanglesVisible -isnot
                            [bool]))) {
                        $contentFindings.Add('sample review lacks exact ' +
                            "visual booleans $($image.fileName)")
                    }
                }
                $reviewVideo = $reviewRecord.video
                if (-not $evidence.video -or -not $reviewVideo -or
                    [string]$reviewVideo.fileName -cne
                        [string]$evidence.video.fileName -or
                    [string]$reviewVideo.sha256 -cne
                        [string]$evidence.video.sha256) {
                    $bindingFindings.Add('review record does not bind the ' +
                        'companion video')
                }
                if (-not $reviewVideo -or
                    $reviewVideo.reviewed -isnot [bool] -or
                    -not [bool]$reviewVideo.reviewed) {
                    $contentFindings.Add('companion video is not marked ' +
                        'reviewed with an exact boolean')
                }
            }
            $bundleIdentityValid = $identityFindings.Count -eq 0
            $bindingsValid = $bundleIdentityValid -and
                $bindingFindings.Count -eq 0
            $contentValid = $contentFindings.Count -eq 0
            if ($bindingsValid -and $contentValid -and
                [string]$reviewRecord.verdict -ceq 'pass') {
                foreach ($image in $reviewImages) {
                    if (-not [bool]$image.menuVisible -or
                        [bool]$image.nvidiaLogoOnly -or
                        -not [bool]$image.glyphsLegible -or
                        [bool]$image.opaqueGlyphRectanglesVisible) {
                        $passFindings.Add('pass review contains a sample ' +
                            "without intact visible menu glyphs: $($image.fileName)")
                    }
                }
                $reviewSupportsPass = $passFindings.Count -eq 0
            }
            foreach ($list in @($identityFindings, $bindingFindings,
                    $contentFindings, $passFindings)) {
                foreach ($finding in $list) {
                    $reviewFindings.Add($finding)
                }
            }
            $review = [pscustomobject][ordered]@{
                path = (Resolve-Path -LiteralPath $ReviewPath).Path
                sha256 = (Get-FileHash -LiteralPath $ReviewPath `
                    -Algorithm SHA256).Hash
                verdict = [string]$reviewRecord.verdict
                bundleIdentityValid = $bundleIdentityValid
                bindingsValid = $bindingsValid
                contentValid = $contentValid
                supportsPass = $reviewSupportsPass
                findings = @($reviewFindings)
            }
        }
    }
    if ($null -eq $review) {
        $review = [pscustomobject][ordered]@{
            path = $null
            sha256 = $null
            verdict = $null
            bundleIdentityValid = $false
            bindingsValid = $false
            contentValid = $false
            supportsPass = $false
            findings = @($reviewFindings)
        }
    }
} elseif ($mode -eq 'MaxPayneVisual') {
    $reviewFindings = [Collections.Generic.List[string]]::new()
    $identityFindings = [Collections.Generic.List[string]]::new()
    $bindingFindings = [Collections.Generic.List[string]]::new()
    $contentFindings = [Collections.Generic.List[string]]::new()
    $passFindings = [Collections.Generic.List[string]]::new()
    $reviewSupportsPass = $false
    if (-not $ReviewPath) {
        $reviewFindings.Add('no direct review record supplied; capture ' +
            'presence only, rendering correctness remains open')
    } else {
        if (-not (Test-Path -LiteralPath $ReviewPath -PathType Leaf)) {
            $reviewFindings.Add("review record not found: $ReviewPath")
        } else {
            try {
                $reviewRecord = Get-Content -Raw -LiteralPath $ReviewPath |
                    ConvertFrom-Json
            } catch {
                $reviewRecord = $null
                $reviewFindings.Add(
                    "review record is invalid: $($_.Exception.Message)")
            }
            if ($reviewRecord) {
                if ([string]$reviewRecord.kind -cne
                        'xpdriver-maxpayne-visual-direct-review') {
                    $contentFindings.Add('review record kind is not a ' +
                        'Max Payne direct review')
                }
                if ([string]$reviewRecord.bindings.harnessResultSha256 -cne
                        $harnessResultHash) {
                    $identityFindings.Add('review record does not bind this ' +
                        'harness result hash')
                }
                $sidecarPath = Resolve-XpBundleConfinedPath `
                    -BundleRoot $bundle -Name 'maxpayne-visual-sequence.json'
                $sidecarHash = if ($sidecarPath -and
                        (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) {
                    (Get-FileHash -LiteralPath $sidecarPath `
                        -Algorithm SHA256).Hash
                } else { $null }
                if ($null -eq $sidecarHash -or
                        [string]$reviewRecord.bindings.visualSequenceSha256 -cne
                        $sidecarHash) {
                    $identityFindings.Add('review record does not bind this ' +
                        'visual sequence sidecar hash')
                }
                $schemaProperty = $reviewRecord.PSObject.Properties['schema']
                if ($null -eq $schemaProperty -or
                    $schemaProperty.Value -isnot [int] -or
                    [int]$schemaProperty.Value -ne 1) {
                    $contentFindings.Add('review record schema must be ' +
                        'exact integer version 1')
                }
                if ([string]$reviewRecord.verdict -cnotin @('pass', 'fail')) {
                    $contentFindings.Add('review verdict is not pass or fail')
                }
                $summaryExpectations = [ordered]@{
                    reviewedImageCount = 3
                    visibleFrameCount = 3
                    renderingCorrectCount = 3
                }
                foreach ($name in $summaryExpectations.Keys) {
                    $property = if ($reviewRecord.summary) {
                        $reviewRecord.summary.PSObject.Properties[$name]
                    } else { $null }
                    if ($null -eq $property -or
                        $property.Value -isnot [int] -or
                        [int]$property.Value -ne $summaryExpectations[$name]) {
                        $contentFindings.Add("review summary field $name " +
                            "must be exact integer $($summaryExpectations[$name])")
                    }
                }
                $reviewImages = @($reviewRecord.images)
                if ($reviewImages.Count -ne 3) {
                    $contentFindings.Add('review record must contain exactly ' +
                        'three image reviews')
                }
                if ($evidence) {
                    foreach ($image in @($evidence.images)) {
                        $bound = @($reviewImages | Where-Object {
                            [string]$_.fileName -ceq [string]$image.fileName
                        })
                        if ($bound.Count -ne 1 -or
                                [string]$bound[0].sha256 -cne
                                [string]$image.sha256) {
                            $bindingFindings.Add('review record does not ' +
                                "bind image $($image.fileName)")
                        }
                        if ($bound.Count -eq 1 -and
                            (($bound[0].frameVisible -isnot [bool]) -or
                             ($bound[0].renderingCorrect -isnot [bool]))) {
                            $contentFindings.Add('image review lacks exact ' +
                                "visual booleans $($image.fileName)")
                        }
                    }
                }
                $bundleIdentityValid = $identityFindings.Count -eq 0
                $bindingsValid = $bundleIdentityValid -and
                    $bindingFindings.Count -eq 0
                $contentValid = $contentFindings.Count -eq 0
                if ($bindingsValid -and $contentValid -and
                    [string]$reviewRecord.verdict -ceq 'pass') {
                    foreach ($image in $reviewImages) {
                        if (-not [bool]$image.frameVisible -or
                            -not [bool]$image.renderingCorrect) {
                            $passFindings.Add('pass review contains an ' +
                                'image without an intact visible frame: ' +
                                "$($image.fileName)")
                        }
                    }
                    $reviewSupportsPass = $passFindings.Count -eq 0
                }
                foreach ($list in @($identityFindings, $bindingFindings,
                        $contentFindings, $passFindings)) {
                    foreach ($finding in $list) {
                        $reviewFindings.Add($finding)
                    }
                }
                $review = [pscustomobject][ordered]@{
                    path = (Resolve-Path -LiteralPath $ReviewPath).Path
                    sha256 = (Get-FileHash -LiteralPath $ReviewPath `
                        -Algorithm SHA256).Hash
                    verdict = [string]$reviewRecord.verdict
                    bindingsValid = $bindingsValid
                    contentValid = $contentValid
                    supportsPass = $reviewSupportsPass
                    findings = @($reviewFindings)
                }
            }
        }
    }
    if ($null -eq $review) {
        $review = [pscustomobject][ordered]@{
            path = $null
            sha256 = $null
            verdict = $null
            bindingsValid = $false
            contentValid = $false
            supportsPass = $false
            findings = @($reviewFindings)
        }
    }
}

$bundleValid = [bool](@($checks | Where-Object { -not $_.valid }).Count -eq 0)
$verdict = if (-not $bundleValid) {
    'FAIL'
} elseif ($mode -ceq 'UT2003Visual') {
    if ($review -and $review.bundleIdentityValid) {
        if ($review.verdict -ceq 'fail') {
            'FAIL'
        } elseif ($review.verdict -ceq 'pass' -and
                  $review.supportsPass) {
            'PASS'
        } else {
            # A review that binds this bundle but makes an invalid or
            # incomplete pass claim is a failure, never an OPEN foreign review.
            'FAIL'
        }
    } else {
        'OPEN'
    }
} elseif ($mode -ceq 'MaxPayneVisual') {
    if ($review -and $review.bindingsValid) {
        if ($review.verdict -ceq 'fail') {
            'FAIL'
        } elseif ($review.verdict -ceq 'pass' -and
                  $review.contentValid -and $review.supportsPass) {
            'PASS'
        } else {
            # A review that binds this bundle but makes an invalid or
            # incomplete pass claim is a failure, never an OPEN foreign review.
            'FAIL'
        }
    } else {
        'OPEN'
    }
} else {
    'PASS'
}

$record = [pscustomobject][ordered]@{
    schemaVersion = 1
    kind = 'xpdriver-evidence-verification'
    verifiedUtc = [DateTime]::UtcNow.ToString('o')
    verifier = $verifier
    bundle = [pscustomobject][ordered]@{
        directory = $bundle
        taskId = if ($harnessResultBindingValid) {
            [string]$result.taskId
        } else { $null }
        runId = if ($harnessResultBindingValid) {
            [string]$result.runId
        } else { $null }
        mode = $mode
        reportedMode = if ($harnessResultBindingValid) {
            [string]$result.mode
        } else { $null }
        harnessResultSha256 = $harnessResultHash
        manifestSha256 = $manifestHash
    }
    checks = @($checks)
    review = $review
    verdict = $verdict
}

if (Test-Path -LiteralPath $OutputPath) {
    Exit-XpEvidenceVerifierUsage `
        "verification record already exists: $OutputPath"
}
# Re-validate the output path immediately before publication: the startup
# gate and this write are separated by the entire verification window.
if (Test-XpEvidencePathHasReparsePoint -Path $OutputPath) {
    Exit-XpEvidenceVerifierUsage `
        '-OutputPath must not traverse a filesystem reparse point'
}
if ($OutputPath.Equals($bundle, [StringComparison]::OrdinalIgnoreCase) -or
    $OutputPath.StartsWith(
        $bundlePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    Exit-XpEvidenceVerifierUsage `
        '-OutputPath must be outside the preserved bundle'
}
Write-XpAcceptanceAtomicText -Path $OutputPath `
    -Text (($record | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
Write-Output (("EvidenceVerification runId={0} mode={1} verdict={2} " +
    "verifier={3} output={4}") -f
    $record.bundle.runId, $mode, $verdict,
    $verifier.digest.Substring(0, 12).ToLowerInvariant(), $OutputPath)
foreach ($checkRecord in @($checks)) {
    foreach ($finding in @($checkRecord.findings)) {
        Write-Output "EvidenceVerificationFinding=$finding"
    }
}
if ($verdict -eq 'FAIL') { exit 1 }
exit 0
