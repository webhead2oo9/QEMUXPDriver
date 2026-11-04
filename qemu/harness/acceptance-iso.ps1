function Get-XpAcceptanceIsoIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Acceptance ISO is missing: $fullPath"
    }
    return [pscustomobject][ordered]@{
        path = $fullPath
        sha256 = (Get-FileHash -LiteralPath $fullPath `
            -Algorithm SHA256).Hash
        length = (Get-Item -LiteralPath $fullPath).Length
    }
}

function Get-XpContentProfileTokenSpans {
    param([string]$Text)

    return @([regex]::Matches($Text, '"([^"]*)"|(\S+)') |
        ForEach-Object {
            [pscustomobject][ordered]@{
                value = if ($_.Groups[1].Success) {
                    $_.Groups[1].Value
                } else {
                    $_.Groups[2].Value
                }
                index = $_.Index
                length = $_.Length
            }
        })
}

function Set-XpContentAcceptanceIsoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ContentText,
        [Parameter(Mandatory = $true)]
        [string]$IsoPath
    )

    $isoArgument = [IO.Path]::GetFullPath($IsoPath).Replace('\', '/')
    if ($isoArgument.Contains('"')) {
        throw 'Acceptance ISO path contains an unsupported quote'
    }
    $tokens = @(Get-XpContentProfileTokenSpans -Text $ContentText)
    $candidates = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $tokens.Count; ++$index) {
        $option = $tokens[$index]
        if ($option.value -ieq '-cdrom') {
            if ($index + 1 -lt $tokens.Count) {
                $candidates.Add([pscustomobject]@{
                    kind = 'legacy'
                    valueToken = $tokens[$index + 1]
                    segments = $null
                })
            }
            continue
        }
        if ($option.value -ine '-drive' -or
            $index + 1 -ge $tokens.Count) { continue }
        $valueToken = $tokens[$index + 1]
        $segments = @($valueToken.value -split ',')
        $mediaSegments = @($segments | Where-Object {
            $_ -ieq 'media=cdrom'
        })
        if ($mediaSegments.Count) {
            $candidates.Add([pscustomobject]@{
                kind = 'drive'
                valueToken = $valueToken
                segments = $segments
                mediaCount = $mediaSegments.Count
            })
        }
    }
    if ($candidates.Count -ne 1) {
        throw 'Prepared-ISO cycles require exactly one CD-ROM content argument'
    }

    $candidate = $candidates[0]
    $replacement = '"' + $isoArgument + '"'
    if ($candidate.kind -eq 'drive') {
        if ($candidate.mediaCount -ne 1) {
            throw 'Prepared-ISO CD-ROM drive requires exactly one media=cdrom option'
        }
        $fileIndexes = @()
        for ($index = 0; $index -lt $candidate.segments.Count; ++$index) {
            if ($candidate.segments[$index].StartsWith(
                    'file=', [StringComparison]::OrdinalIgnoreCase)) {
                $fileIndexes += $index
            }
        }
        if ($fileIndexes.Count -ne 1) {
            throw 'Prepared-ISO CD-ROM drive requires exactly one file option'
        }
        $candidate.segments[$fileIndexes[0]] = 'file=' + $isoArgument
        $replacement = '"' + ($candidate.segments -join ',') + '"'
    }
    $valueToken = $candidate.valueToken
    return $ContentText.Substring(0, $valueToken.index) + $replacement +
        $ContentText.Substring($valueToken.index + $valueToken.length)
}

function Assert-XpPreparedAcceptanceIso {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[A-Fa-f0-9]{64}$')]
        [string]$ExpectedSha256
    )

    $identity = Get-XpAcceptanceIsoIdentity -Path $Path
    $expected = $ExpectedSha256.ToUpperInvariant()
    if ($identity.sha256 -cne $expected) {
        throw "Prepared acceptance ISO hash mismatch: expected $expected, found $($identity.sha256)"
    }
    return $identity
}

function Get-XpAcceptanceIsoPackageFiles {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $inspector = Join-Path $PSScriptRoot 'inspect_acceptance_iso.py'
    $output = @(& py.exe -3 $inspector $fullPath 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw ("Acceptance ISO package inspection failed: " +
            (($output | ForEach-Object { "$_" }) -join ' '))
    }
    try {
        $parsed = (($output | ForEach-Object { "$_" }) -join
            [Environment]::NewLine) | ConvertFrom-Json
        $records = @($parsed | ForEach-Object { $_ })
    } catch {
        throw "Acceptance ISO package inspection returned invalid JSON"
    }
    if ($records.Count -ne 8) {
        throw "Acceptance ISO package inspection returned an invalid member count"
    }
    return $records
}
