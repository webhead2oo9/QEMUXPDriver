[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)

    if (-not $Condition) { throw "Assertion failed: $Message" }
}

function Write-AsciiFile {
    param([string]$Path, [string]$Text)

    [IO.File]::WriteAllText($Path, $Text, [Text.Encoding]::ASCII)
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureRoot = Join-Path $repo ('.tmp\slipstream-' + [guid]::NewGuid())
$fixtureFull = [IO.Path]::GetFullPath($fixtureRoot)
Assert-True ($fixtureFull.StartsWith($repo + '\',
    [StringComparison]::OrdinalIgnoreCase)) 'fixture escaped the repository'

try {
    $source = Join-Path $fixtureFull 'source'
    $i386 = Join-Path $source 'I386'
    $package = Join-Path $fixtureFull 'package'
    [void](New-Item -ItemType Directory -Path $i386,$package -Force)
    Write-AsciiFile (Join-Path $i386 'TXTSETUP.SIF') '[SetupData]'
    Write-AsciiFile (Join-Path $i386 'LAYOUT.INF') '[Version]'
    $original = @'
[Data]
MsDosInitiated = "0"

[Unattended]
OemPreinstall = No
OemPnPDriversPath = "Drivers\Audio;Drivers\Net"
DriverSigningPolicy = Warn

[GuiUnattended]
TimeZone = 4
'@ -replace "`n", "`r`n"
    $answer = Join-Path $i386 'WINNT.SIF'
    Write-AsciiFile $answer $original
    $payload = @{
        'qxp.inf' = @'
[Version]
CatalogFile = qxp.cat
DriverVer = 07/13/2026,1.0.0.76
[Models]
Device = Install,PCI\VEN_1234&DEV_1111
'@
        'qxp.cat' = 'catalog'
        'qxpmp.sys' = 'miniport'
        'qxpmap.sys' = 'mapping'
        'qxpdisp.dll' = 'display'
        'qxpicd.dll' = 'icd'
        'qxpcoin2.dll' = 'coinstaller'
    }
    foreach ($name in $payload.Keys) {
        Write-AsciiFile (Join-Path $package $name) $payload[$name]
    }

    $script = Join-Path $PSScriptRoot 'slipstream-xp.ps1'
    $result = & $script -SourcePath $source -PackagePath $package
    Assert-True (-not $result.CreatedAnswerFile) 'existing WINNT.SIF was replaced'
    Assert-True ($result.DriverVersion -eq '07/13/2026,1.0.0.76') `
        'driver version was not reported'
    $driverDirectory = Join-Path $i386 '$OEM$\$1\Drivers\XPDriver'
    foreach ($name in $payload.Keys) {
        $copied = Join-Path $driverDirectory $name
        Assert-True (Test-Path -LiteralPath $copied -PathType Leaf) `
            "missing staged file $name"
        Assert-True (([IO.File]::ReadAllText($copied)) -eq $payload[$name]) `
            "staged file differs: $name"
    }
    Assert-True (@(Get-ChildItem -LiteralPath $driverDirectory -File).Count -eq 7) `
        'slipstream did not stage exactly seven package payloads'
    $updated = [IO.File]::ReadAllText($answer)
    Assert-True ($updated -match '(?im)^OemPreinstall\s*=\s*Yes\s*$') `
        'OemPreinstall was not enabled'
    Assert-True ($updated -match
        '(?im)^OemPnPDriversPath\s*=\s*"Drivers\\Audio;Drivers\\Net;Drivers\\XPDriver"\s*$') `
        'XPDriver path was not appended'
    Assert-True ($updated -match
        '(?im)^DriverSigningPolicy\s*=\s*Ignore\s*$') `
        'test-signed package policy was not selected'
    Assert-True ($updated -match '(?im)^TimeZone\s*=\s*4\s*$') `
        'unrelated answer-file content changed'
    $backup = "$answer.xpdriver.bak"
    Assert-True (Test-Path -LiteralPath $backup -PathType Leaf) `
        'answer-file backup was not created'
    Assert-True (([IO.File]::ReadAllText($backup)) -eq $original) `
        'answer-file backup does not contain the original'

    & $script -SourcePath $i386 -PackagePath $package | Out-Null
    $second = [IO.File]::ReadAllText($answer)
    $pathMatches = [regex]::Matches(
        $second, '(?i)(?:^|;)Drivers\\XPDriver(?:;|"|$)')
    Assert-True ($pathMatches.Count -eq 1) 'second run duplicated the driver path'
    Assert-True (([IO.File]::ReadAllText($backup)) -eq $original) `
        'second run replaced the original backup'

    $freshSource = Join-Path $fixtureFull 'fresh-source'
    $freshI386 = Join-Path $freshSource 'I386'
    [void](New-Item -ItemType Directory -Path $freshI386 -Force)
    Write-AsciiFile (Join-Path $freshI386 'TXTSETUP.SIF') '[SetupData]'
    Write-AsciiFile (Join-Path $freshI386 'LAYOUT.INF') '[Version]'
    $freshResult = & $script -SourcePath $freshSource -PackagePath $package `
        -KeepDriverSigningPolicy
    Assert-True $freshResult.CreatedAnswerFile 'new WINNT.SIF was not reported'
    $freshAnswer = [IO.File]::ReadAllText((Join-Path $freshI386 'WINNT.SIF'))
    Assert-True ($freshAnswer -match
        '(?im)^OemPnPDriversPath\s*=\s*"Drivers\\XPDriver"\s*$') `
        'new answer file does not include XPDriver'
    Assert-True ($freshAnswer -notmatch '(?im)^DriverSigningPolicy\s*=') `
        'KeepDriverSigningPolicy added a signing policy'

    $rejected = $false
    try {
        & $script -SourcePath $source -PackagePath $package `
            -DriverPath '..\escape' | Out-Null
    } catch {
        $rejected = $true
    }
    Assert-True $rejected 'unsafe DriverPath was accepted'

    Remove-Item -LiteralPath (Join-Path $package 'qxpmap.sys')
    $missingMapRejected = $false
    try {
        & $script -SourcePath $source -PackagePath $package | Out-Null
    } catch {
        $missingMapRejected = $_.Exception.Message -like
            "*Driver package is missing 'qxpmap.sys'*"
    }
    Assert-True $missingMapRejected 'package missing qxpmap.sys was accepted'
    Write-AsciiFile (Join-Path $package 'qxpmap.sys') $payload['qxpmap.sys']

    $realPackage = Join-Path $repo 'guest\xpdm\build\package'
    $realPackageResult = 'skipped'
    if (Test-Path -LiteralPath (Join-Path $realPackage 'qxp.inf') -PathType Leaf) {
        $realSource = Join-Path $fixtureFull 'real-package-source'
        $realI386 = Join-Path $realSource 'I386'
        [void](New-Item -ItemType Directory -Path $realI386 -Force)
        Write-AsciiFile (Join-Path $realI386 'TXTSETUP.SIF') '[SetupData]'
        Write-AsciiFile (Join-Path $realI386 'LAYOUT.INF') '[Version]'
        $realResult = & $script -SourcePath $realSource `
            -PackagePath $realPackage
        Assert-True ($realResult.DriverVersion -match '^\d{2}/\d{2}/\d{4},') `
            'real package DriverVer was not recognized'
        foreach ($name in $payload.Keys) {
            $sourceHash = (Get-FileHash -Algorithm SHA256 `
                -LiteralPath (Join-Path $realPackage $name)).Hash
            $copyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath `
                (Join-Path $realResult.DriverDirectory $name)).Hash
            Assert-True ($sourceHash -eq $copyHash) `
                "real package copy differs: $name"
        }
        $realPackageResult = 'verified'
    }

    Write-Output "SlipstreamFixture=ok idempotent=true backup=true traversal=false real_package=$realPackageResult"
} finally {
    if (Test-Path -LiteralPath $fixtureFull) {
        $resolvedFixture = [IO.Path]::GetFullPath($fixtureFull)
        if ($resolvedFixture.StartsWith($repo + '\.tmp\',
                [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
        }
    }
}
