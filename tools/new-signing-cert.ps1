# SPDX-License-Identifier: GPL-2.0-only
<#
Creates XPDriver's persistent project signing certificate, once, with the
WDK's MakeCert exactly like the development certificate staging makes (so
its shape is the one Windows XP has accepted): self-signed, SHA-1, no key
usage or extended key usage restriction (XP's driver check rejects a
certificate limited to Code Signing with CERT_E_WRONG_USAGE), but RSA-2048
and valid until 2049-12-31 at most (a UTCTime date). It is written as a
password-protected PFX plus its public .cer next to it. Keep both outside the
repository and back the PFX up: every release is signed with it, so users
who trusted one release's xpdriver-test.cer keep trusting the next.

  tools\new-signing-cert.ps1 -Path D:\keys\xpdriver-signing.pfx

Then, before guest\xpdm\stage-package.cmd or tools\make-release.ps1:

  set XPD_SIGN_PFX=D:\keys\xpdriver-signing.pfx
  set XPD_SIGN_PFX_PASSWORD=...   (optional; prompted for otherwise)

XPD_SIGN_PFX_PASSWORD, when set, is also the new PFX's password here.
#>
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Subject = 'CN=XPDriver Project Test Signing',
    [string]$WdkRoot = 'C:\WinDDK\7600.16385.1'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$pfx = [IO.Path]::GetFullPath($Path)
if ($pfx.StartsWith($repo + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "keep the PFX outside the repository: $pfx"
}
$cer = [IO.Path]::ChangeExtension($pfx, '.cer')
foreach ($file in $pfx, $cer) {
    if (Test-Path -LiteralPath $file) { throw "$file already exists" }
}

function Read-NewPassword {
    $first = Read-Host -AsSecureString 'New PFX password'
    $second = Read-Host -AsSecureString 'Repeat the password'
    $a = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($first)
    $b = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($second)
    try {
        $same = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($a) -ceq
            [Runtime.InteropServices.Marshal]::PtrToStringBSTR($b)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($a)
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($b)
    }
    if (-not $same) { throw 'the passwords differ' }
    return $first
}
$password = if ($null -eq $env:XPD_SIGN_PFX_PASSWORD) {
    Read-NewPassword
} elseif ($env:XPD_SIGN_PFX_PASSWORD -eq '') {
    New-Object Security.SecureString
} else {
    ConvertTo-SecureString $env:XPD_SIGN_PFX_PASSWORD -AsPlainText -Force
}

$makeCert = Join-Path $WdkRoot 'bin\x86\MakeCert.exe'
if (-not (Test-Path -LiteralPath $makeCert -PathType Leaf)) { throw "MakeCert not found: $makeCert" }
$notAfter = (Get-Date).AddYears(25)
$lastUtcTime = Get-Date -Year 2049 -Month 12 -Day 31
if ($notAfter -gt $lastUtcTime) { $notAfter = $lastUtcTime }

# MakeCert leaves the key in a CurrentUser store of its own (no password
# dialog, unlike -sv); it is exported and removed again.
$store = 'XPDriverNewCertificate'
& $makeCert -r -pe -a sha1 -len 2048 -n $Subject -e $notAfter.ToString('MM/dd/yyyy') -ss $store $cer
if ($LASTEXITCODE -ne 0) { throw "MakeCert failed (exit $LASTEXITCODE)" }
$certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2 $cer
$stored = "Cert:\CurrentUser\$store\$($certificate.Thumbprint)"
try {
    Export-PfxCertificate -Cert (Get-Item -LiteralPath $stored) -FilePath $pfx -Password $password | Out-Null
} finally {
    Remove-Item -LiteralPath $stored -DeleteKey
}
Write-Host "Created $pfx and $cer"
Write-Host "Thumbprint $($certificate.Thumbprint), valid until $($certificate.NotAfter.ToString('yyyy-MM-dd'))"
