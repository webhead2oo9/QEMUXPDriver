[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourcePath,

    [string]$PackagePath =
        (Join-Path $PSScriptRoot '..\guest\xpdm\build\package'),

    [string]$DriverPath = 'Drivers\XPDriver',

    [switch]$KeepDriverSigningPolicy
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$packageFiles = @(
    'qxp.inf',
    'qxp.cat',
    'qxpmp.sys',
    'qxpmap.sys',
    'qxpdisp.dll',
    'qxpicd.dll',
    'qxpcoin2.dll'
)

function Get-NormalizedDriverPath {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw 'DriverPath must not be empty.'
    }
    $normalized = $Path.Replace('/', '\').Trim('\')
    if ([IO.Path]::IsPathRooted($normalized)) {
        throw 'DriverPath must be relative to the XP system drive.'
    }
    $segments = @($normalized -split '\\')
    foreach ($segment in $segments) {
        if ([string]::IsNullOrWhiteSpace($segment) -or
            $segment -eq '.' -or $segment -eq '..' -or
            $segment.EndsWith('.') -or $segment.EndsWith(' ') -or
            $segment.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
            throw "DriverPath contains an unsafe segment: '$segment'."
        }
    }
    return $segments -join '\'
}

function Find-I386Directory {
    param([string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).ProviderPath
    if ((Split-Path -Leaf $resolved) -ieq 'I386') {
        $i386 = $resolved
    } else {
        $i386 = Join-Path $resolved 'I386'
    }
    if (-not (Test-Path -LiteralPath $i386 -PathType Container) -or
        -not (Test-Path -LiteralPath (Join-Path $i386 'TXTSETUP.SIF') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $i386 'LAYOUT.INF') -PathType Leaf)) {
        throw "SourcePath is not an extracted Windows XP source: '$Path'."
    }
    return (Resolve-Path -LiteralPath $i386).ProviderPath
}

function Read-TextFile {
    param([string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    $offset = 0
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xff -and
        $bytes[1] -eq 0xfe) {
        $encoding = [Text.Encoding]::Unicode
        $offset = 2
    } elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xfe -and
              $bytes[1] -eq 0xff) {
        $encoding = [Text.Encoding]::BigEndianUnicode
        $offset = 2
    } elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and
              $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf) {
        $encoding = [Text.UTF8Encoding]::new($true)
        $offset = 3
    } else {
        $encoding = [Text.Encoding]::GetEncoding(1252)
    }
    $text = $encoding.GetString($bytes, $offset, $bytes.Length - $offset)
    $newLine = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    return [pscustomobject]@{
        Text = $text
        Encoding = $encoding
        NewLine = $newLine
    }
}

function Get-IniValue {
    param(
        [Collections.Generic.List[string]]$Lines,
        [string]$Section,
        [string]$Key
    )

    $inSection = $false
    $sectionPattern = '^\s*\[' + [regex]::Escape($Section) + '\]\s*$'
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*)$'
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[.*\]\s*$') {
            $inSection = $line -match $sectionPattern
            continue
        }
        if ($inSection -and $line -match $keyPattern) {
            return $Matches[1].Trim()
        }
    }
    return $null
}

function Set-IniValue {
    param(
        [Collections.Generic.List[string]]$Lines,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )

    $sectionStart = -1
    $sectionEnd = $Lines.Count
    $sectionPattern = '^\s*\[' + [regex]::Escape($Section) + '\]\s*$'
    for ($index = 0; $index -lt $Lines.Count; ++$index) {
        if ($Lines[$index] -match $sectionPattern) {
            $sectionStart = $index
            for ($end = $index + 1; $end -lt $Lines.Count; ++$end) {
                if ($Lines[$end] -match '^\s*\[.*\]\s*$') {
                    $sectionEnd = $end
                    break
                }
            }
            break
        }
    }
    if ($sectionStart -lt 0) {
        if ($Lines.Count -and $Lines[$Lines.Count - 1] -ne '') {
            $Lines.Add('')
        }
        $Lines.Add("[$Section]")
        $Lines.Add("$Key = $Value")
        return
    }

    $matchingIndexes = [Collections.Generic.List[int]]::new()
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    for ($index = $sectionStart + 1; $index -lt $sectionEnd; ++$index) {
        if ($Lines[$index] -match $keyPattern) {
            $matchingIndexes.Add($index)
        }
    }
    if (-not $matchingIndexes.Count) {
        $Lines.Insert($sectionEnd, "$Key = $Value")
        return
    }
    $Lines[$matchingIndexes[0]] = "$Key = $Value"
    for ($index = $matchingIndexes.Count - 1; $index -ge 1; --$index) {
        $Lines.RemoveAt($matchingIndexes[$index])
    }
}

function Get-PnpPathValue {
    param(
        [Collections.Generic.List[string]]$Lines,
        [string]$NewPath
    )

    $current = Get-IniValue -Lines $Lines -Section 'Unattended' `
        -Key 'OemPnPDriversPath'
    if ($current) {
        $current = $current.Trim()
        if ($current.Length -ge 2 -and $current[0] -eq '"' -and
            $current[$current.Length - 1] -eq '"') {
            $current = $current.Substring(1, $current.Length - 2)
        }
    }
    $paths = [Collections.Generic.List[string]]::new()
    if ($current) {
        foreach ($path in $current -split ';') {
            $candidate = $path.Trim()
            if ($candidate) { $paths.Add($candidate) }
        }
    }
    $present = $false
    foreach ($path in $paths) {
        if ($path.Replace('/', '\').Trim('\') -ieq $NewPath) {
            $present = $true
            break
        }
    }
    if (-not $present) { $paths.Add($NewPath) }
    return '"' + ($paths -join ';') + '"'
}

$driverPath = Get-NormalizedDriverPath $DriverPath
$i386 = Find-I386Directory $SourcePath
$sourceRoot = Split-Path -Parent $i386
$package = (Resolve-Path -LiteralPath $PackagePath).ProviderPath
foreach ($file in $packageFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $file) -PathType Leaf)) {
        throw "Driver package is missing '$file': '$package'."
    }
}
$infPath = Join-Path $package 'qxp.inf'
$infText = [IO.File]::ReadAllText($infPath)
if ($infText -notmatch '(?im)^\s*CatalogFile\s*=\s*qxp\.cat\s*$' -or
    $infText -notmatch 'PCI\\VEN_1234&DEV_1111') {
    throw "Package INF is not the XPDriver QEMU Standard VGA package: '$infPath'."
}

$oemRoot = Join-Path $i386 '$OEM$\$1'
$target = $oemRoot
foreach ($segment in $driverPath -split '\\') {
    $target = Join-Path $target $segment
}
$targetFull = [IO.Path]::GetFullPath($target)
$oemFull = [IO.Path]::GetFullPath($oemRoot).TrimEnd('\') + '\'
if (-not $targetFull.StartsWith($oemFull,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "DriverPath escapes the XP OEM driver tree: '$DriverPath'."
}

if ($PSCmdlet.ShouldProcess($targetFull, 'Stage XPDriver package')) {
    [void](New-Item -ItemType Directory -Path $targetFull -Force)
    foreach ($file in $packageFiles) {
        Copy-Item -LiteralPath (Join-Path $package $file) `
            -Destination (Join-Path $targetFull $file) -Force
    }
}

$answerFile = Join-Path $i386 'WINNT.SIF'
$answerBackup = "$answerFile.xpdriver.bak"
$createdAnswerFile = -not (Test-Path -LiteralPath $answerFile -PathType Leaf)
if ($createdAnswerFile) {
    $fileInfo = [pscustomobject]@{
        Text = '; Generated by the XPDriver slipstream helper.'
        Encoding = [Text.Encoding]::GetEncoding(1252)
        NewLine = "`r`n"
    }
} else {
    $fileInfo = Read-TextFile $answerFile
}
$lines = [Collections.Generic.List[string]]::new()
foreach ($line in ($fileInfo.Text -split '\r\n|\n|\r')) {
    $lines.Add($line)
}
$pnpValue = Get-PnpPathValue -Lines $lines -NewPath $driverPath
Set-IniValue -Lines $lines -Section 'Unattended' -Key 'OemPreinstall' `
    -Value 'Yes'
Set-IniValue -Lines $lines -Section 'Unattended' -Key 'OemPnPDriversPath' `
    -Value $pnpValue
if (-not $KeepDriverSigningPolicy) {
    Set-IniValue -Lines $lines -Section 'Unattended' `
        -Key 'DriverSigningPolicy' -Value 'Ignore'
}
$updatedText = $lines -join $fileInfo.NewLine

if ($PSCmdlet.ShouldProcess($answerFile, 'Update XP Setup answer file')) {
    if (-not $createdAnswerFile -and
        -not (Test-Path -LiteralPath $answerBackup)) {
        Copy-Item -LiteralPath $answerFile -Destination $answerBackup
    }
    $temporary = "$answerFile.xpdriver.tmp"
    try {
        [IO.File]::WriteAllText($temporary, $updatedText, $fileInfo.Encoding)
        Move-Item -LiteralPath $temporary -Destination $answerFile -Force
    } finally {
        Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    }
}

$driverVersion = [regex]::Match(
    $infText, '(?im)^\s*DriverVer\s*=\s*(.+?)\s*$').Groups[1].Value
[pscustomobject]@{
    SourceRoot = $sourceRoot
    I386 = $i386
    Package = $package
    DriverVersion = $driverVersion
    DriverDirectory = $targetFull
    AnswerFile = $answerFile
    CreatedAnswerFile = $createdAnswerFile
    SigningPolicyChanged = -not $KeepDriverSigningPolicy
}
