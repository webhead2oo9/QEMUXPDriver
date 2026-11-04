$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-identity.ps1')

function Assert-PackageIdentity {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$identity = ConvertFrom-XpAcceptanceInfIdentity -Text @'
[Version]
Signature = "$Windows NT$"
CatalogFile = qxp.cat
DriverVer = 07/17/2026,1.0.0.247
'@
Assert-PackageIdentity ($identity.driverVer -eq
                        '07/17/2026,1.0.0.247') `
    'exact INF DriverVer was not retained'
Assert-PackageIdentity ($identity.driverVersion -eq '1.0.0.247') `
    'INF driver version was not parsed'
Assert-PackageIdentity ($identity.catalogFile -eq 'qxp.cat') `
    'INF catalog name was not parsed'

foreach ($badInf in @(
        "[Version]`nCatalogFile=qxp.cat",
        "[Version]`nDriverVer=bad,1.0`nCatalogFile=qxp.cat",
        "[Version]`nDriverVer=07/17/2026,1.0.0.247`nCatalogFile=..\qxp.cat",
        "[Version]`nDriverVer=07/17/2026,1.0.0.247`nDriverVer=07/18/2026,1.0.0.248`nCatalogFile=qxp.cat")) {
    $rejected = $false
    try {
        [void](ConvertFrom-XpAcceptanceInfIdentity -Text $badInf)
    } catch {
        $rejected = $true
    }
    Assert-PackageIdentity $rejected 'malformed INF identity was accepted'
}
$transportRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-transport-abi-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $transportRoot)
try {
    foreach ($name in @('qxpmp.sys', 'qxpdisp.dll')) {
        [IO.File]::WriteAllBytes(
            (Join-Path $transportRoot $name),
            [Text.Encoding]::ASCII.GetBytes(
                "prefix-QXP_D3D_TRANSPORT_ABI=60UL-suffix"))
    }
    Assert-PackageIdentity (
        (Get-XpPackagedTransportAbi -Directory $transportRoot) -eq 60) `
        'matching packaged transport ABI markers were not decoded'
    [IO.File]::WriteAllBytes(
        (Join-Path $transportRoot 'qxpdisp.dll'),
        [Text.Encoding]::ASCII.GetBytes(
            "prefix-QXP_D3D_TRANSPORT_ABI=61UL-suffix"))
    $mismatchRejected = $false
    try {
        [void](Get-XpPackagedTransportAbi -Directory $transportRoot)
    } catch {
        $mismatchRejected = $_.Exception.Message -like
            '*display and miniport transport ABIs differ*'
    }
    Assert-PackageIdentity $mismatchRejected `
        'mismatched packaged transport ABI markers were accepted'
    [IO.File]::WriteAllBytes(
        (Join-Path $transportRoot 'qxpdisp.dll'),
        [Text.Encoding]::ASCII.GetBytes('marker-missing'))
    $missingMarkerRejected = $false
    try {
        [void](Get-XpPackagedTransportAbi -Directory $transportRoot)
    } catch {
        $missingMarkerRejected = $_.Exception.Message -like
            '*exactly one transport ABI marker*'
    }
    Assert-PackageIdentity $missingMarkerRejected `
        'missing packaged transport ABI marker was accepted'
} finally {
    Remove-Item -Recurse -Force $transportRoot `
        -ErrorAction SilentlyContinue
}


$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$stagedPackage = Join-Path $workspace 'guest\xpdm\build\package'
if (Test-Path -LiteralPath $stagedPackage -PathType Container) {
    $package = Get-XpAcceptancePackageIdentity -Directory $stagedPackage
    Assert-PackageIdentity $package.valid `
        "current staged XPDM package identity is invalid: $($package.findings -join '; ')"
    Assert-PackageIdentity ($package.files.Count -eq 8) `
        'staged package did not hash every required file'
    Assert-PackageIdentity $package.signerMatchesBundledCertificate `
        'staged catalog signer does not match its bundled certificate'
    Assert-PackageIdentity ($null -ne $package.catalogCoverage -and
        $package.catalogCoverage.memberDigests -eq 6 -and
        $package.catalogCoverage.uncovered.Count -eq 0) `
        'staged package payloads are not all covered by the catalog'
    [void]($package | ConvertTo-Json -Depth 8)
    Write-Output "PackageIdentityFixture=staged driverVer=$($package.driverVer) signer=$($package.certificate.thumbprint)"

    # A signed catalog must never validate a payload replaced after signing:
    # mutating one member while keeping qxp.cat and the certificate untouched
    # has to invalidate the identity even though the catalog itself still
    # verifies and the signer still matches.
    $mutatedRoot = Join-Path ([IO.Path]::GetTempPath()) `
        "xpdriver-catalog-member-$PID-$([Guid]::NewGuid().ToString('N'))"
    [void](New-Item -ItemType Directory -Path $mutatedRoot)
    try {
        Copy-Item -LiteralPath $stagedPackage `
            -Destination (Join-Path $mutatedRoot 'package') -Recurse
        $mutationPackage = Join-Path $mutatedRoot 'package'
        $unmutated = Get-XpAcceptancePackageIdentity `
            -Directory $mutationPackage
        Assert-PackageIdentity $unmutated.valid `
            ("unmutated package copy is invalid: " +
                ($unmutated.findings -join '; '))
        foreach ($memberName in @('qxpdisp.dll', 'qxpmap.sys')) {
            $memberPath = Join-Path $mutationPackage $memberName
            $memberBytes = [IO.File]::ReadAllBytes($memberPath)
            $memberBytes[0] = $memberBytes[0] -bxor 0xFF
            [IO.File]::WriteAllBytes($memberPath, $memberBytes)
            $mutated = Get-XpAcceptancePackageIdentity `
                -Directory $mutationPackage
            Assert-PackageIdentity (-not $mutated.valid) `
                "mutated $memberName still validated against the old catalog"
            Assert-PackageIdentity (@($mutated.findings | Where-Object {
                $_ -eq "package member is not covered by the catalog: $memberName"
            }).Count -eq 1) `
                "mutated $memberName was not reported as uncovered"
            $memberBytes[0] = $memberBytes[0] -bxor 0xFF
            [IO.File]::WriteAllBytes($memberPath, $memberBytes)
        }
    } finally {
        Remove-Item -Recurse -Force $mutatedRoot `
            -ErrorAction SilentlyContinue
    }
} else {
    Write-Output 'PackageIdentityFixture=skipped reason=staged-package-absent'
}

# Membership is exact: extra files or subdirectories inside the package
# directory would ride the install ISO without appearing in any recorded
# hash, so they invalidate the identity.
$syntheticRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-package-identity-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $syntheticRoot)
try {
    foreach ($name in @('qxp.inf', 'qxp.cat', 'xpdriver-test.cer',
            'qxpmp.sys', 'qxpmap.sys', 'qxpdisp.dll', 'qxpicd.dll',
            'qxpcoin2.dll')) {
        [IO.File]::WriteAllText((Join-Path $syntheticRoot $name), 'stub')
    }
    [IO.File]::WriteAllText((Join-Path $syntheticRoot 'extra.txt'),
        'stowaway')
    [void](New-Item -ItemType Directory -Path (
        Join-Path $syntheticRoot 'nested'))
    $synthetic = Get-XpAcceptancePackageIdentity -Directory $syntheticRoot
    Assert-PackageIdentity (-not $synthetic.valid) `
        'package with extra members was reported valid'
    Assert-PackageIdentity (@($synthetic.findings | Where-Object {
        $_ -eq 'unexpected package file: extra.txt' }).Count -eq 1) `
        'extra package file was not flagged'
    Assert-PackageIdentity (@($synthetic.findings | Where-Object {
        $_ -eq 'unexpected package subdirectory: nested' }).Count -eq 1) `
        'package subdirectory was not flagged'
    Remove-Item -LiteralPath (Join-Path $syntheticRoot 'qxpmap.sys')
    $missingMap = Get-XpAcceptancePackageIdentity -Directory $syntheticRoot
    Assert-PackageIdentity (-not $missingMap.valid) `
        'package missing qxpmap.sys was reported valid'
    Assert-PackageIdentity (@($missingMap.findings | Where-Object {
        $_ -eq 'required package file is missing: qxpmap.sys'
    }).Count -eq 1) `
        'missing qxpmap.sys was not flagged'
} finally {
    Remove-Item -Recurse -Force $syntheticRoot -ErrorAction SilentlyContinue
}

Write-Output 'XPDM package identity tests passed'
