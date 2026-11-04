# SPDX-License-Identifier: GPL-2.0-only
# Signs the staged catalog with XPDriver's persistent project certificate and
# writes that certificate's public part for the package (stage-package.cmd
# runs this when XPD_SIGN_PFX is set). XPD_SIGN_PFX names the PFX, kept
# outside the repository (tools\new-signing-cert.ps1 creates it);
# XPD_SIGN_PFX_PASSWORD is its password, else it is prompted for. The PFX
# goes into CurrentUser\My only for the SignTool run, so no password reaches
# a command line, and leaves again unless it was there before.
param(
    [Parameter(Mandatory = $true)][string]$SignTool,
    [Parameter(Mandatory = $true)][string]$Catalog,
    [Parameter(Mandatory = $true)][string]$CertificateOut
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$pfx = $env:XPD_SIGN_PFX
if (-not $pfx -or -not (Test-Path -LiteralPath $pfx -PathType Leaf)) {
    throw "XPD_SIGN_PFX does not name a PFX file: '$pfx'"
}
$password = if ($null -eq $env:XPD_SIGN_PFX_PASSWORD) {
    Read-Host -AsSecureString "Password of $pfx"
} elseif ($env:XPD_SIGN_PFX_PASSWORD -eq '') {
    New-Object Security.SecureString
} else {
    ConvertTo-SecureString $env:XPD_SIGN_PFX_PASSWORD -AsPlainText -Force
}

$certificates = @((Get-PfxData -FilePath $pfx -Password $password).EndEntityCertificates)
if ($certificates.Count -ne 1) {
    throw "$pfx must hold exactly one end-entity certificate, not $($certificates.Count)"
}
$certificate = $certificates[0]
$thumbprint = $certificate.Thumbprint
$storePath = "Cert:\CurrentUser\My\$thumbprint"
$wasPresent = Test-Path -LiteralPath $storePath
if (-not $wasPresent) {
    Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\My `
        -Password $password | Out-Null
}
try {
    & $SignTool sign /s My /sha1 $thumbprint /v $Catalog
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed (exit $LASTEXITCODE)" }
} finally {
    if (-not $wasPresent) { Remove-Item -LiteralPath $storePath -DeleteKey }
}
[IO.File]::WriteAllBytes($CertificateOut, $certificate.Export('Cert'))
Write-Host "Signed with the project certificate $thumbprint ($($certificate.Subject))"
