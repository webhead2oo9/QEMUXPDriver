$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')

function Assert-FrozenInputs {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$stagedPackage = Join-Path $workspace 'guest\xpdm\build\package'
$oracleIso = Join-Path $workspace `
    'guest\acceptance\xpdriver-acceptance-ut-oracle-r5.iso'
$invalidatedOracleIso = Join-Path $workspace `
    'guest\acceptance\xpdriver-acceptance-ut-oracle-r2.iso'
$unselectedOracleIso = Join-Path $workspace `
    'guest\acceptance\xpdriver-acceptance-ut-oracle-r3.iso'
$supersededOracleIso = Join-Path $workspace `
    'guest\acceptance\xpdriver-acceptance-ut-oracle-r4.iso'
$currentManifestPath = Join-Path $workspace `
    'guest\acceptance\frozen-inputs.json'
$oracleManifestPath = Join-Path $workspace `
    'guest\acceptance\frozen-inputs-ut-oracle-r5.json'
$invalidatedOracleManifestPath = Join-Path $workspace `
    'guest\acceptance\frozen-inputs-ut-oracle-r2.json'
$unselectedOracleManifestPath = Join-Path $workspace `
    'guest\acceptance\frozen-inputs-ut-oracle-r3.json'
$supersededOracleManifestPath = Join-Path $workspace `
    'guest\acceptance\frozen-inputs-ut-oracle-r4.json'
$retroHost = $env:XPD_RETRO_HOST
$core = Get-XpDefaultCorePath -Workspace $workspace
$publisher = Join-Path $PSScriptRoot 'publish-xp-acceptance-inputs.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-frozen-inputs-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    Assert-FrozenInputs `
        ((Get-FileHash -LiteralPath $currentManifestPath `
            -Algorithm SHA256).Hash -ceq
            '0C77C4449E7218F85D666C4C9C1E37C6057C9F81B8D464ED48159465CEE3A688' -and
         (Get-FileHash -LiteralPath $invalidatedOracleIso `
            -Algorithm SHA256).Hash -ceq
            '7AAC989A32BE6FD7DB27A2EF0A421D17BF68A2A93D309296D74E457BB2B906F0' -and
         (Get-FileHash -LiteralPath $invalidatedOracleManifestPath `
            -Algorithm SHA256).Hash -ceq
            'ADFE1168AADF6092C9416CFA0A088E19D8A525BB9E7EB02C08100DA7F138DBE1' -and
         (Get-FileHash -LiteralPath $unselectedOracleIso `
            -Algorithm SHA256).Hash -ceq
            '666473CB2ECE5644B95AB7C55EEB0B1B4B02A3B67054D4F7C28716981F0184C6' -and
         (Get-FileHash -LiteralPath $unselectedOracleManifestPath `
            -Algorithm SHA256).Hash -ceq
            'C06803A8EF89437994E8BC9E2B38D90180DCD111CDF22206801009E5D976711A' -and
         (Get-FileHash -LiteralPath $supersededOracleIso `
            -Algorithm SHA256).Hash -ceq
            '12F9EBACB6849C9F3862B546300EF1A1055982C998AA089051CE025B9AB701B5' -and
         (Get-FileHash -LiteralPath $supersededOracleManifestPath `
            -Algorithm SHA256).Hash -ceq
            '856099AFE77D52CE498C76CA5A82699C459F5D0299D618541FF544FEA556B87E' -and
         (Get-FileHash -LiteralPath $oracleManifestPath `
            -Algorithm SHA256).Hash -ceq
            'CF79E6FCDFD6941E0960CEF0C5DC8C0D282392D78EC05943B9889CA46C42C9EE') `
        'current, invalidated, unselected, or superseded frozen input generation was not preserved'
    $currentManifest = Get-Content -Raw -LiteralPath $currentManifestPath |
        ConvertFrom-Json
    $oracleManifest = Get-Content -Raw -LiteralPath $oracleManifestPath |
        ConvertFrom-Json
    $oracleIsoBytes = [IO.File]::ReadAllBytes($oracleIso)
    $oracleIsoText = [Text.Encoding]::ASCII.GetString($oracleIsoBytes)
    Assert-FrozenInputs `
        ([StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath([string]$oracleManifest.acceptanceIso.path),
            [IO.Path]::GetFullPath($oracleIso)) -and
         [string]$oracleManifest.acceptanceIso.sha256 -ceq
            'EEDDB4A0B743EFE34D6D5AD701377129B696E82CCF52DF9A4BCADEA32CB4984D' -and
         [string]$oracleManifest.core.sha256 -ceq
            [string]$currentManifest.core.sha256 -and
         [string]$oracleManifest.retroHost.sha256 -ceq
            [string]$currentManifest.retroHost.sha256 -and
         [string]$oracleManifest.package.signerThumbprint -ceq
            '1F590CA35B62264685FBCE150E252C9E2377A9D9' -and
         -not (Compare-XpFrozenPackageFiles `
            -ManifestFiles @($oracleManifest.package.files) `
            -CurrentFiles @($currentManifest.package.files))) `
        'selected oracle did not retain its historical package with current host/core pairing'
    Assert-FrozenInputs `
        ($oracleIsoText.Contains('error=fresh-log-reset') -and
         $oracleIsoText.Contains(
            'renderer=d3d8 audio=default visual=extended launch=normal') -and
         $oracleIsoText.Contains(
            'transition=main-menu evidence=browse-failed-entry-spawn-controller') -and
         $oracleIsoText.Contains('boundary=post-main-menu') -and
         $oracleIsoText.Contains('error=process-handoff-not-allowed') -and
         -not $oracleIsoText.Contains('ut2003gameplayvisual')) `
        'oracle ISO does not contain the repaired normal-menu observer'

    $stagedInputsReady = $retroHost -and
        (Test-Path -LiteralPath $stagedPackage -PathType Container) -and
        (Test-Path -LiteralPath $retroHost -PathType Leaf) -and
        (Test-Path -LiteralPath $core -PathType Leaf)
    if ($stagedInputsReady) {
        $manifestPath = Join-Path $testRoot 'frozen-inputs.json'
        $sourcePackage = Join-Path $testRoot 'source-package'
        $sourceIso = Join-Path $testRoot 'source.iso'
        $snapshotDirectory = Join-Path $testRoot 'snapshot'
        $objectStore = Join-Path $testRoot 'objects'
        Copy-Item -LiteralPath $stagedPackage -Destination $sourcePackage `
            -Recurse
        $republishCore = Join-Path $testRoot 'republish-core.dll'
        [IO.File]::WriteAllBytes($republishCore,
            [IO.File]::ReadAllBytes($core) + [byte[]]@(0x5A))
        $savedPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $publishOutput = @(& $powershell -NoLogo -NoProfile `
                -NonInteractive -ExecutionPolicy Bypass -File $publisher `
                -PackageDirectory $sourcePackage `
                -IsoPath $sourceIso -SnapshotDirectory $snapshotDirectory `
                -ObjectStorePath $objectStore -OutputPath $manifestPath 2>&1)
            $publishExit = $LASTEXITCODE
            $publishedManifestHash = if ($publishExit -eq 0) {
                (Get-FileHash -LiteralPath $manifestPath `
                    -Algorithm SHA256).Hash
            } else { $null }
            $republish = @(& $powershell -NoLogo -NoProfile `
                -NonInteractive -ExecutionPolicy Bypass -File $publisher `
                -SkipBuild -PackageDirectory $sourcePackage `
                -IsoPath $sourceIso -SnapshotDirectory $snapshotDirectory `
                -ObjectStorePath $objectStore -OutputPath $manifestPath 2>&1)
            $republishExit = $LASTEXITCODE
            $changedRepublish = @(& $powershell -NoLogo -NoProfile `
                -NonInteractive -ExecutionPolicy Bypass -File $publisher `
                -SkipBuild -PackageDirectory $sourcePackage `
                -IsoPath $sourceIso -SnapshotDirectory $snapshotDirectory `
                -ObjectStorePath $objectStore -OutputPath $manifestPath `
                -CorePath $republishCore 2>&1)
            $changedRepublishExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $savedPreference
        }
        Assert-FrozenInputs ($publishExit -eq 0) `
            "publisher failed: $($publishOutput -join '; ')"
        $manifest = Get-XpFrozenInputs -Path $manifestPath
        Assert-FrozenInputs `
            (-not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$manifest.package.directory, $sourcePackage) -and
             -not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$manifest.acceptanceIso.path, $sourceIso) -and
             -not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$manifest.retroHost.path, $retroHost) -and
             -not [StringComparer]::OrdinalIgnoreCase.Equals(
                [string]$manifest.core.path, $core)) `
            'publisher retained mutable source paths'
        Remove-Item -LiteralPath $sourcePackage -Recurse -Force
        Remove-Item -LiteralPath $sourceIso -Force
        $manifest = Get-XpFrozenInputs -Path $manifestPath
        $executionInputs = Assert-XpFrozenExecutionInputs `
            -Manifest $manifest `
            -RetroHostPath ([string]$manifest.retroHost.path) `
            -CorePath ([string]$manifest.core.path)
        Assert-FrozenInputs `
            ($executionInputs.retroHost.sha256 -ceq
                [string]$manifest.retroHost.sha256 -and
             $executionInputs.core.sha256 -ceq
                [string]$manifest.core.sha256) `
            'matching frozen host/core execution inputs were rejected'
        foreach ($mutation in @(
                @('retro host', (Join-Path $testRoot 'alternate-host.exe'),
                    [string]$manifest.core.path),
                @('paired core', [string]$manifest.retroHost.path,
                    (Join-Path $testRoot 'alternate-core.dll')))) {
            $executionMutationCaught = $false
            try {
                [void](Assert-XpFrozenExecutionInputs `
                    -Manifest $manifest -RetroHostPath $mutation[1] `
                    -CorePath $mutation[2])
            } catch {
                $executionMutationCaught = $_.Exception.Message -like
                    "*selected $($mutation[0]) path differs from frozen inputs*"
            }
            Assert-FrozenInputs $executionMutationCaught `
                "alternate $($mutation[0]) path was not rejected"
        }
        Assert-FrozenInputs ($manifest.transportAbi -ge 1) `
            'frozen manifest did not record the transport ABI'
        Assert-FrozenInputs (
            [string]$manifest.package.signerThumbprint -match '^[0-9A-F]{40}$') `
            'frozen manifest did not record the signer thumbprint'
        Assert-FrozenInputs ($republishExit -eq 0) `
            ("publisher rejected a byte-identical regeneration: " +
                ($republish -join '; '))
        Assert-FrozenInputs ($changedRepublishExit -ne 0 -and
            ($changedRepublish -join "`n") -like '*already exists*') `
            ('publisher overwrote an existing frozen manifest with ' +
                'changed content')
        Assert-FrozenInputs ((Get-FileHash -LiteralPath $manifestPath `
                -Algorithm SHA256).Hash -ceq $publishedManifestHash) `
            'republication altered the frozen manifest bytes'

        $tamperedPath = Join-Path $testRoot 'frozen-inputs-tampered.json'
        $tampered = Get-Content -Raw -LiteralPath $manifestPath |
            ConvertFrom-Json
        $tampered.acceptanceIso.sha256 = '0' * 64
        [IO.File]::WriteAllText($tamperedPath,
            ($tampered | ConvertTo-Json -Depth 10))
        $tamperedCaught = $false
        try {
            [void](Get-XpFrozenInputs -Path $tamperedPath)
        } catch {
            $tamperedCaught = $_.Exception.Message -like
                '*acceptance ISO identity differs on disk*'
        }
        Assert-FrozenInputs $tamperedCaught `
            'tampered ISO hash was not rejected'
        $packageTamperedPath = Join-Path $testRoot `
            'frozen-inputs-package-tampered.json'
        $packageTampered = Get-Content -Raw -LiteralPath $manifestPath |
            ConvertFrom-Json
        $packageTampered.package.files[0].sha256 = '0' * 64
        [IO.File]::WriteAllText($packageTamperedPath,
            ($packageTampered | ConvertTo-Json -Depth 10))
        $packageTamperedCaught = $false
        try {
            [void](Get-XpFrozenInputs -Path $packageTamperedPath)
        } catch {
            $packageTamperedCaught = $_.Exception.Message -like
                '*frozen package files differ on disk*'
        }
        Assert-FrozenInputs $packageTamperedCaught `
            'tampered package file hash was not rejected'

        $abiTamperedPath = Join-Path $testRoot `
            'frozen-inputs-abi-tampered.json'
        $abiTampered = Get-Content -Raw -LiteralPath $manifestPath |
            ConvertFrom-Json
        $abiTampered.transportAbi = [int64]$abiTampered.transportAbi + 1
        [IO.File]::WriteAllText($abiTamperedPath,
            ($abiTampered | ConvertTo-Json -Depth 10))
        $abiTamperedCaught = $false
        try {
            [void](Get-XpFrozenInputs -Path $abiTamperedPath)
        } catch {
            $abiTamperedCaught = $_.Exception.Message -like
                '*differs from manifest ABI*'
        }
        Assert-FrozenInputs $abiTamperedCaught `
            'mismatched packaged/manifest transport ABI was not rejected'
        $abiMissingPath = Join-Path $testRoot `
            'frozen-inputs-abi-missing.json'
        $abiMissing = Get-Content -Raw -LiteralPath $manifestPath |
            ConvertFrom-Json
        $abiMissing.PSObject.Properties.Remove('transportAbi')
        [IO.File]::WriteAllText($abiMissingPath,
            ($abiMissing | ConvertTo-Json -Depth 10))
        $abiMissingCaught = $false
        try {
            [void](Get-XpFrozenInputs -Path $abiMissingPath)
        } catch {
            $abiMissingCaught = $_.Exception.Message -like
                '*lacks a transport ABI record*'
        }
        Assert-FrozenInputs $abiMissingCaught `
            'manifest without a transport ABI record was not rejected'

        # The manual install path must never drift from the frozen inputs:
        # install-driver-305.bat hardcodes the same two identities.
        $installBat = Join-Path $workspace 'install-driver-305.bat'
        if (Test-Path -LiteralPath $installBat -PathType Leaf) {
            $batText = Get-Content -Raw -LiteralPath $installBat
            $isoMatch = [regex]::Match($batText,
                'set "EXPECTED_ISO=([0-9A-Fa-f]{64})"')
            $coreMatch = [regex]::Match($batText,
                'set "EXPECTED_CORE=([0-9A-Fa-f]{64})"')
            Assert-FrozenInputs ($isoMatch.Success -and $coreMatch.Success) `
                'install-driver-305.bat does not pin ISO and core hashes'
            Assert-FrozenInputs (
                $isoMatch.Groups[1].Value -ceq
                    [string]$currentManifest.acceptanceIso.sha256 -and
                $coreMatch.Groups[1].Value -ceq
                    [string]$currentManifest.core.sha256) `
                'install-driver-305.bat drifted from the frozen inputs'
        }
    } else {
        Write-Output 'FrozenInputsFixture=skipped reason=staged-inputs-absent'
    }

    $missingCaught = $false
    try {
        [void](Get-XpFrozenInputs -Path (
            Join-Path $testRoot 'missing.json'))
    } catch {
        $missingCaught = $_.Exception.Message -like '*manifest is missing*'
    }
    Assert-FrozenInputs $missingCaught `
        'missing manifest did not fail closed'

    Write-Output 'frozen inputs tests passed'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}
