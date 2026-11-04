$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'd3d-driver-profile.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')

function Add-XpMatrixFinding {
    param(
        [Collections.Generic.List[string]]$Findings,
        [string]$Message
    )

    if (-not [string]::IsNullOrWhiteSpace($Message)) {
        [void]$Findings.Add($Message)
    }
}

function ConvertTo-XpMatrixCanonicalJson {
    param($Value)

    return $Value | ConvertTo-Json -Depth 20 -Compress
}

function Get-XpMatrixSortedStrings {
    param([object[]]$Values)

    $strings = @($Values | ForEach-Object { [string]$_ })
    [Array]::Sort($strings, [StringComparer]::Ordinal)
    return $strings
}

function Test-XpMatrixExactStrings {
    param(
        [object[]]$Actual,
        [object[]]$Expected
    )

    $actualStrings = @(Get-XpMatrixSortedStrings -Values $Actual)
    $expectedStrings = @(Get-XpMatrixSortedStrings -Values $Expected)
    return $actualStrings.Count -eq $expectedStrings.Count -and
        (($actualStrings -join "`n") -ceq ($expectedStrings -join "`n"))
}

function Get-XpMatrixJsonPath {
    param(
        $InputObject,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return [pscustomobject]@{ found = $true; value = $InputObject }
    }
    $current = $InputObject
    foreach ($segment in $Path.Split('.')) {
        if ($null -eq $current) {
            return [pscustomobject]@{ found = $false; value = $null }
        }
        if ($segment -match '^\[(\d+)\]$') {
            $index = [int]$Matches[1]
            $values = @($current)
            if ($index -ge $values.Count) {
                return [pscustomobject]@{ found = $false; value = $null }
            }
            $current = $values[$index]
            continue
        }
        $property = @($current.PSObject.Properties | Where-Object {
            $_.Name -ceq $segment
        })
        if ($property.Count -ne 1) {
            return [pscustomobject]@{ found = $false; value = $null }
        }
        $current = $property[0].Value
    }
    return [pscustomobject]@{ found = $true; value = $current }
}

function Test-XpMatrixExpectedValue {
    param(
        $Actual,
        $Expected,
        $Tolerance
    )

    if ($null -eq $Expected) { return $null -eq $Actual }
    if ($null -eq $Actual) { return $false }
    if ($null -ne $Tolerance) {
        try {
            return [Math]::Abs([double]$Actual - [double]$Expected) -le
                [double]$Tolerance
        } catch { return $false }
    }
    if ($Expected -is [string]) { return [string]$Actual -ceq $Expected }
    if ($Expected -is [bool]) { return $Actual -is [bool] -and
            [bool]$Actual -eq $Expected }
    if ($Expected -is [ValueType]) {
        try { return [decimal]$Actual -eq [decimal]$Expected }
        catch { return $false }
    }
    return (ConvertTo-XpMatrixCanonicalJson -Value $Actual) -ceq
        (ConvertTo-XpMatrixCanonicalJson -Value $Expected)
}

function Add-XpMatrixExpectedFieldFindings {
    param(
        $Result,
        [object[]]$ExpectedFields,
        [Collections.Generic.List[string]]$Findings,
        [string]$Prefix
    )

    $seenPaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($assertion in @($ExpectedFields)) {
        $path = [string]$assertion.path
        if ([string]::IsNullOrWhiteSpace($path)) {
            Add-XpMatrixFinding $Findings "$Prefix has an empty expected path"
            continue
        }
        if (-not $seenPaths.Add($path)) {
            Add-XpMatrixFinding $Findings `
                "$Prefix repeats expected result path '$path'"
            continue
        }
        $resolved = Get-XpMatrixJsonPath -InputObject $Result -Path $path
        if (-not $resolved.found) {
            Add-XpMatrixFinding $Findings "$Prefix result path '$path' is missing"
            continue
        }
        $tolerance = if ($assertion.PSObject.Properties['tolerance']) {
            [double]$assertion.tolerance
        } else { $null }
        if (-not (Test-XpMatrixExpectedValue -Actual $resolved.value `
                -Expected $assertion.value -Tolerance $tolerance)) {
            $actualText = ConvertTo-XpMatrixCanonicalJson $resolved.value
            $expectedText = ConvertTo-XpMatrixCanonicalJson $assertion.value
            Add-XpMatrixFinding $Findings `
                "$Prefix result path '$path' differs; actual=$actualText expected=$expectedText"
        }
    }
}

function ConvertFrom-XpMatrixKeyValueRecord {
    param(
        [string]$Text,
        [string]$Prefix
    )

    $matches = [regex]::Matches($Text,
        "(?m)^$([regex]::Escape($Prefix))\s+([^\r\n]+)\r?$")
    if (-not $matches.Count) { return $null }
    $record = [ordered]@{}
    foreach ($token in $matches[$matches.Count - 1].Groups[1].Value `
            -split '\s+') {
        if ($token -notmatch '^([^=]+)=(.*)$') { continue }
        $name = $Matches[1]
        $textValue = $Matches[2]
        $value = $textValue
        if ($textValue -ceq 'True' -or $textValue -ceq 'False') {
            $value = [Convert]::ToBoolean($textValue)
        } else {
            $integer = [long]0
            $floating = [double]0
            if ([long]::TryParse($textValue,
                    [Globalization.NumberStyles]::Integer,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref]$integer)) {
                $value = $integer
            } elseif ([double]::TryParse($textValue,
                    [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [ref]$floating)) {
                $value = $floating
            }
        }
        $record[$name] = $value
    }
    return [pscustomobject]$record
}

function Get-XpMatrixPerformanceObservations {
    param([string]$ConsoleText)

    $frame = ConvertFrom-XpMatrixKeyValueRecord -Text $ConsoleText `
        -Prefix 'FramePublicationProfile'
    $game = ConvertFrom-XpMatrixKeyValueRecord -Text $ConsoleText `
        -Prefix 'D3dGamePerformance'
    $transportMatches = [regex]::Matches($ConsoleText,
        '(?m)^D3dGameTransportOk=(True|False)\s+([^\r\n]+)\r?$')
    $transport = $null
    if ($transportMatches.Count) {
        $last = $transportMatches[$transportMatches.Count - 1]
        $transport = ConvertFrom-XpMatrixKeyValueRecord `
            -Text ("Transport ok=$($last.Groups[1].Value) " +
                $last.Groups[2].Value) -Prefix 'Transport'
    }
    return [pscustomobject][ordered]@{
        framePublication = $frame
        d3dGamePerformance = $game
        d3dGameTransport = $transport
    }
}

# Contract-neutral audit of one artifact bundle directory: the two required
# documents exist and parse to non-null JSON objects, every JSON artifact
# parses, and the actual files match the manifest byte-for-byte. Callers
# (the matrix pass-contracts, the handback coherence checks) layer their own
# result contracts on top of the returned documents.
function Get-XpAcceptanceBundleAudit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    $findings = [Collections.Generic.List[string]]::new()
    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    $actualFiles = @()
    if (-not (Test-Path -LiteralPath $fullDirectory -PathType Container)) {
        Add-XpMatrixFinding $findings "bundle directory is missing: $fullDirectory"
    } else {
        $actualFiles = @(Get-ChildItem -LiteralPath $fullDirectory -File |
            ForEach-Object { $_.Name })
        foreach ($required in @('artifact-manifest.json',
                                 'harness-result.json')) {
            if ($actualFiles -cnotcontains $required) {
                Add-XpMatrixFinding $findings "bundle is missing $required"
            }
        }
    }

    $documents = @{}
    foreach ($name in @($actualFiles | Where-Object {
            [IO.Path]::GetExtension($_) -ieq '.json' })) {
        try {
            $documents[$name] = Get-Content -Raw -LiteralPath (
                Join-Path $fullDirectory $name) | ConvertFrom-Json
        } catch {
            $label = switch ($name) {
                'artifact-manifest.json' { 'artifact manifest is not valid JSON' }
                'harness-result.json' { 'harness result is not valid JSON' }
                default { "bundle JSON is not valid: $name" }
            }
            Add-XpMatrixFinding $findings "${label}: $($_.Exception.Message)"
        }
    }
    # Valid but falsey JSON (null, false, 0, "", []) parses cleanly yet
    # carries no checkable document; a required document must be a non-null
    # object or every downstream schema and contract check would be silently
    # skipped.
    foreach ($required in @('artifact-manifest.json', 'harness-result.json')) {
        if ($documents.ContainsKey($required) -and
                -not ($documents[$required] -is `
                    [Management.Automation.PSCustomObject])) {
            Add-XpMatrixFinding $findings `
                "bundle document is not a JSON object: $required"
            $documents.Remove($required)
        }
    }
    $manifest = $documents['artifact-manifest.json']
    $result = $documents['harness-result.json']

    if ($manifest) {
        if ($manifest.schema -ne 1) {
            Add-XpMatrixFinding $findings 'artifact manifest schema is not 1'
        }
        $recordNames = @($manifest.files | ForEach-Object { [string]$_.name })
        $expectedFiles = @($recordNames) + 'artifact-manifest.json'
        if (-not (Test-XpMatrixExactStrings $actualFiles $expectedFiles)) {
            Add-XpMatrixFinding $findings `
                "bundle membership differs from manifest; actual=[$((Get-XpMatrixSortedStrings $actualFiles) -join ',')] expected=[$((Get-XpMatrixSortedStrings $expectedFiles) -join ',')]"
        }
        $seen = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)
        foreach ($record in @($manifest.files)) {
            $name = [string]$record.name
            if ([string]::IsNullOrWhiteSpace($name) -or
                [IO.Path]::GetFileName($name) -cne $name -or
                $name -eq '.' -or $name -eq '..') {
                Add-XpMatrixFinding $findings `
                    "artifact manifest contains unsafe name '$name'"
                continue
            }
            if (-not $seen.Add($name)) {
                Add-XpMatrixFinding $findings `
                    "artifact manifest repeats name '$name'"
                continue
            }
            $path = Join-Path $fullDirectory $name
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
            $file = Get-Item -LiteralPath $path
            if ([uint64]$file.Length -ne [uint64]$record.size) {
                Add-XpMatrixFinding $findings `
                    "artifact size differs for $name"
            }
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($hash -cne [string]$record.sha256) {
                Add-XpMatrixFinding $findings `
                    "artifact SHA-256 differs for $name"
            }
        }
    }
    if ($result -and $result.schema -ne 1) {
        Add-XpMatrixFinding $findings 'harness result schema is not 1'
    }

    return [pscustomobject][ordered]@{
        directory = $fullDirectory
        files = @($actualFiles)
        manifest = $manifest
        result = $result
        findings = @($findings)
    }
}

function Get-XpAcceptanceBundleVerification {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [ValidateSet('harness-self-test', 'install', 'd3d-profile')]
        [string]$Type,
        [string]$RunId,
        [string]$TaskId,
        [object[]]$ExpectedFields = @(),
        [string[]]$ExpectedSuites = @(),
        [string]$ExpectedDriverVer,
        [string]$ExpectedSignerThumbprint,
        [object[]]$ExpectedPackageFiles = @()
    )

    $findings = [Collections.Generic.List[string]]::new()
    $audit = Get-XpAcceptanceBundleAudit -Directory $Directory
    $fullDirectory = $audit.directory
    $manifest = $audit.manifest
    $result = $audit.result
    $profile = $null
    $observations = $null
    $actualFiles = @($audit.files)
    foreach ($auditFinding in @($audit.findings)) {
        Add-XpMatrixFinding $findings $auditFinding
    }

    if ($manifest -and $RunId -and [string]$manifest.runId -cne $RunId) {
        Add-XpMatrixFinding $findings `
            "artifact manifest run ID differs; actual=$($manifest.runId) expected=$RunId"
    }
    if ($result) {
        if ($RunId -and [string]$result.runId -cne $RunId) {
            Add-XpMatrixFinding $findings `
                "harness result run ID differs; actual=$($result.runId) expected=$RunId"
        }
        if ($TaskId -and [string]$result.taskId -cne $TaskId) {
            Add-XpMatrixFinding $findings `
                "harness result task ID differs; actual=$($result.taskId) expected=$TaskId"
        }
        $expectedArtifactReferences = @($actualFiles | Where-Object {
            $_ -cne 'artifact-manifest.json' -and
            $_ -cne 'harness-result.json'
        })
        if (-not (Test-XpMatrixExactStrings @($result.artifacts) `
                $expectedArtifactReferences)) {
            Add-XpMatrixFinding $findings `
                'harness result artifact references differ from bundle membership'
        }
        Add-XpMatrixExpectedFieldFindings -Result $result `
            -ExpectedFields $ExpectedFields -Findings $findings `
            -Prefix $Type
    }

    if ($result -and $Type -eq 'harness-self-test') {
        if ([string]$result.kind -cne 'xpdriver-acceptance-harness-self-test' -or
            [string]$result.result -cne 'pass' -or
            [int]$result.exitCode -ne 0 -or
            @($result.parseErrors).Count -ne 0 -or
            @($result.suites | Where-Object {
                $_.result -cne 'pass' -or [int]$_.exitCode -ne 0
            }).Count -ne 0) {
            Add-XpMatrixFinding $findings `
                'harness self-test result or one of its suites did not pass'
        }
        if ($ExpectedSuites.Count -and -not (Test-XpMatrixExactStrings `
                @($result.suites | ForEach-Object { [string]$_.name }) `
                $ExpectedSuites)) {
            Add-XpMatrixFinding $findings `
                'harness self-test suite names differ from the matrix spec'
        }
    }

    if ($result -and $Type -eq 'install') {
        if ([string]$result.kind -cne 'xpdriver-acceptance-run' -or
            [string]$result.result -cne 'pass' -or
            [string]$result.harnessStatus -cne 'passed' -or
            -not [bool]$result.teardownOk -or
            [string]$result.launcher.phase -cne 'finalized' -or
            -not [bool]$result.launcher.finalized -or
            [int]$result.launcher.childExitCode -ne 0) {
            Add-XpMatrixFinding $findings `
                'install launcher, result, or teardown did not pass'
        }
        if (-not [bool]$result.package.valid -or
            -not [bool]$result.package.signerMatchesBundledCertificate -or
            -not [bool]$result.install.guestReported -or
            [bool]$result.install.conflictingReports -or
            -not [bool]$result.install.matchesStagedPackage) {
            Add-XpMatrixFinding $findings `
                'install package or guest-reported identity is invalid'
        }
        if ($ExpectedDriverVer -and
            ([string]$result.package.driverVer -cne $ExpectedDriverVer -or
             [string]$result.install.driverVer -cne $ExpectedDriverVer)) {
            Add-XpMatrixFinding $findings 'install DriverVer differs'
        }
        if ($ExpectedSignerThumbprint -and
            ([string]$result.package.certificate.thumbprint -cne
                $ExpectedSignerThumbprint -or
             [string]$result.package.catalogSignature.signerThumbprint -cne
                $ExpectedSignerThumbprint -or
             [string]$result.install.signerThumbprint -cne
                $ExpectedSignerThumbprint)) {
            Add-XpMatrixFinding $findings `
                'install signer or certificate thumbprint differs'
        }
        foreach ($expectedFile in @($ExpectedPackageFiles)) {
            $matches = @($result.package.files | Where-Object {
                [string]$_.name -ceq [string]$expectedFile.name
            })
            if ($matches.Count -ne 1 -or
                [string]$matches[0].sha256 -cne [string]$expectedFile.sha256) {
                Add-XpMatrixFinding $findings `
                    "install package identity differs for $($expectedFile.name)"
            }
        }
        if (@($ExpectedPackageFiles).Count -and
            @($result.package.files).Count -ne
                @($ExpectedPackageFiles).Count) {
            Add-XpMatrixFinding $findings `
                'install package file count differs from matrix spec'
        }
        $identityPath = Join-Path $fullDirectory 'package-identity.json'
        if (-not (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
            Add-XpMatrixFinding $findings `
                'install bundle is missing package-identity.json'
        } else {
            try {
                $identity = Get-Content -Raw -LiteralPath $identityPath |
                    ConvertFrom-Json
                if ((ConvertTo-XpMatrixCanonicalJson $identity) -cne
                    (ConvertTo-XpMatrixCanonicalJson $result.package)) {
                    Add-XpMatrixFinding $findings `
                        'package-identity.json differs from the result package object'
                }
            } catch {
                Add-XpMatrixFinding $findings `
                    "package-identity.json is invalid: $($_.Exception.Message)"
            }
        }
        if (Test-Path -LiteralPath (Join-Path $fullDirectory `
                'd3d-driver-profile.json')) {
            Add-XpMatrixFinding $findings `
                'install bundle unexpectedly contains a D3D profile artifact'
        }
    }

    if ($result -and $Type -eq 'd3d-profile') {
        if ([string]$result.kind -cne 'xpdriver-acceptance-run' -or
            [string]$result.result -cne 'pass' -or
            [string]$result.harnessStatus -cne 'passed' -or
            -not [bool]$result.teardownOk -or
            [string]$result.launcher.phase -cne 'finalized' -or
            -not [bool]$result.launcher.finalized -or
            [int]$result.launcher.childExitCode -ne 0 -or
            [string]$result.captureMode -cne 'capture-free' -or
            [bool]$result.capture) {
            Add-XpMatrixFinding $findings `
                'D3D profile launcher, capture policy, result, or teardown did not pass'
        }
        if ([string]$result.d3dDriverProfile.status -cne 'complete' -or
            -not [bool]$result.d3dDriverProfile.valid) {
            Add-XpMatrixFinding $findings `
                'D3D profile result is not complete and valid'
        }
        $captureFiles = @($actualFiles | Where-Object {
            [IO.Path]::GetExtension($_) -match '^\.(bmp|png|mp4|avi|gif|jpg|jpeg)$'
        })
        if ($captureFiles.Count) {
            Add-XpMatrixFinding $findings `
                "capture-free bundle contains capture artifacts: $($captureFiles -join ',')"
        }
        $profilePath = Join-Path $fullDirectory 'd3d-driver-profile.json'
        if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
            Add-XpMatrixFinding $findings `
                'D3D profile bundle is missing d3d-driver-profile.json'
        } else {
            try {
                $profile = Get-Content -Raw -LiteralPath $profilePath |
                    ConvertFrom-Json
                if ((ConvertTo-XpMatrixCanonicalJson $profile) -cne
                    (ConvertTo-XpMatrixCanonicalJson $result.d3dDriverProfile)) {
                    Add-XpMatrixFinding $findings `
                        'd3d-driver-profile.json differs from the result profile object'
                }
            } catch {
                Add-XpMatrixFinding $findings `
                    "d3d-driver-profile.json is invalid: $($_.Exception.Message)"
            }
        }
        $driverLog = Join-Path $fullDirectory 'driver.log'
        if (-not (Test-Path -LiteralPath $driverLog -PathType Leaf)) {
            Add-XpMatrixFinding $findings `
                'D3D profile bundle is missing driver.log'
        } else {
            try {
                $reparsed = ConvertFrom-XpD3dDriverProfile -Text `
                    (Get-Content -Raw -LiteralPath $driverLog)
                if (-not $reparsed.valid -or
                    [string]$reparsed.status -cne 'complete') {
                    Add-XpMatrixFinding $findings `
                        'raw driver.log does not reparse as a complete valid profile'
                } elseif ((ConvertTo-XpMatrixCanonicalJson $reparsed) -cne
                          (ConvertTo-XpMatrixCanonicalJson `
                              $result.d3dDriverProfile)) {
                    Add-XpMatrixFinding $findings `
                        'raw driver.log reconstruction differs from the result profile object'
                }
            } catch {
                Add-XpMatrixFinding $findings `
                    "raw driver.log profile replay failed: $($_.Exception.Message)"
            }
        }
        $consolePath = Join-Path $fullDirectory 'console.log'
        if (-not (Test-Path -LiteralPath $consolePath -PathType Leaf)) {
            Add-XpMatrixFinding $findings `
                'D3D profile bundle is missing console.log'
        } else {
            $observations = Get-XpMatrixPerformanceObservations `
                -ConsoleText (Get-Content -Raw -LiteralPath $consolePath)
            $frame = $observations.framePublication
            if (-not $frame -or -not [bool]$frame.valid -or
                [long]$frame.fresh -le 1 -or
                [long]$frame.callbacks -ne
                    [long]$frame.fresh + [long]$frame.duped -or
                [long]$frame.intervals -ne [long]$frame.fresh - 1) {
                Add-XpMatrixFinding $findings `
                    'capture-free frame publication observation is missing or invalid'
            }
            $game = $observations.d3dGamePerformance
            if (-not $game -or [double]$game.observedSeconds -le 0 -or
                [long]$game.draws -le 0 -or [long]$game.presents -le 0 -or
                [long]$game.batches -le 0 -or
                ($profile -and [long]$game.draws -ne
                    [long]$profile.miniport.logicalDraws)) {
                Add-XpMatrixFinding $findings `
                    'D3D game performance observation is missing or inconsistent'
            }
            if (-not $observations.d3dGameTransport -or
                -not [bool]$observations.d3dGameTransport.ok) {
                Add-XpMatrixFinding $findings `
                    'D3D game transport audit is missing or failed'
            }
        }
    }

    return [pscustomobject][ordered]@{
        name = Split-Path -Leaf $fullDirectory
        type = $Type
        directory = $fullDirectory
        runId = if ($result) { [string]$result.runId } else { $RunId }
        valid = $findings.Count -eq 0
        findings = @($findings)
        files = @(Get-XpMatrixSortedStrings $actualFiles)
        result = $result
        profile = $profile
        observations = $observations
    }
}

function Get-XpMatrixFileIdentity {
    param([string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            path = $fullPath
            exists = $false
            bytes = $null
            sha256 = $null
        }
    }
    return [pscustomobject][ordered]@{
        path = $fullPath
        exists = $true
        bytes = (Get-Item -LiteralPath $fullPath).Length
        sha256 = (Get-FileHash -LiteralPath $fullPath `
            -Algorithm SHA256).Hash
    }
}

function Get-XpMatrixStatusHash {
    param([string[]]$Lines)

    $text = if ($Lines.Count) { ($Lines -join "`n") + "`n" } else { '' }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    } finally { $sha.Dispose() }
}

function Get-XpMatrixStatusFileRecords {
    param(
        [string]$RepositoryPath,
        [string[]]$StatusLines
    )

    # Content identity for every path git reports. A dirty path whose contents
    # change keeps the same status line, so the status text alone cannot prove
    # a snapshot untouched; per-file bytes and hashes can.
    $records = [Collections.Generic.List[object]]::new()
    foreach ($line in @($StatusLines)) {
        $text = [string]$line
        if ($text.Length -lt 4) { continue }
        $xy = $text.Substring(0, 2)
        $path = $text.Substring(3)
        $arrowIndex = $path.LastIndexOf(' -> ')
        if ($arrowIndex -ge 0) {
            $path = $path.Substring($arrowIndex + 4)
        }
        if ($path.Length -ge 2 -and $path.StartsWith('"') -and
                $path.EndsWith('"')) {
            $path = $path.Substring(1, $path.Length - 2)
        }
        $fullPath = [IO.Path]::GetFullPath(
            (Join-Path $RepositoryPath ($path -replace '/', '\')))
        $exists = Test-Path -LiteralPath $fullPath -PathType Leaf
        $bytes = $null
        $sha256 = $null
        $hashError = $false
        if ($exists) {
            $bytes = (Get-Item -LiteralPath $fullPath).Length
            try {
                $sha256 = (Get-FileHash -LiteralPath $fullPath `
                    -Algorithm SHA256).Hash
            } catch {
                # A file locked by another process (for example the shared
                # artifact lease) cannot be hashed; record the condition so
                # the identity is stable while it persists and changes when
                # it clears, never crashing the audit.
                $hashError = $true
            }
        }
        $records.Add([pscustomobject][ordered]@{
            path = $path
            xy = $xy
            exists = [bool]$exists
            bytes = $bytes
            sha256 = $sha256
            hashError = [bool]$hashError
        })
    }
    return @($records | Sort-Object path)
}

function Get-XpMatrixWslStatusContentHash {
    param([string]$RepositoryPath)

    # WSL-side content identity: one tracked diff against HEAD plus one hashed
    # untracked listing, canonicalized and hashed like the Windows records.
    $diff = @(& wsl.exe git -C $RepositoryPath diff --binary HEAD 2>$null |
        ForEach-Object { [string]$_ })
    $untracked = @(& wsl.exe bash -c (
        'cd "' + ($RepositoryPath -replace '"', '\"') + '" && ' +
        'git ls-files --others --exclude-standard -z | ' +
        'xargs -0 -r sha256sum') 2>$null | ForEach-Object { [string]$_ })
    $lines = @($diff) + @('-- untracked --') + @($untracked)
    return Get-XpMatrixStatusHash -Lines $lines
}

function Get-XpMatrixRepositoryIdentity {
    param($Repository)

    $kind = if ($Repository.kind) { [string]$Repository.kind } else { 'windows' }
    $path = [string]$Repository.path
    $branch = $null
    $commit = $null
    $status = @()
    $cachedDiff = @()
    $exitCode = 0
    if ($kind -ceq 'wsl') {
        $branch = (& wsl.exe git -C $path branch --show-current)
        $exitCode = $LASTEXITCODE
        if ($exitCode -eq 0) {
            $commit = (& wsl.exe git -C $path rev-parse HEAD)
            $exitCode = $LASTEXITCODE
        }
        if ($exitCode -eq 0) {
            $status = @(& wsl.exe git -C $path status --short `
                --untracked-files=all)
            $exitCode = $LASTEXITCODE
        }
        if ($exitCode -eq 0) {
            # Staged content is invisible to both the status text and the
            # worktree bytes: an index blob can change while both stay
            # identical. --binary patches are ASCII-armored, so the diff
            # text is a stable identity for the staged state.
            $cachedDiff = @(& wsl.exe git -C $path diff --cached --binary)
            $exitCode = $LASTEXITCODE
        }
    } elseif ($kind -ceq 'windows') {
        $branch = (& git -C $path branch --show-current)
        $exitCode = $LASTEXITCODE
        if ($exitCode -eq 0) {
            $commit = (& git -C $path rev-parse HEAD)
            $exitCode = $LASTEXITCODE
        }
        if ($exitCode -eq 0) {
            $status = @(& git -C $path status --short --untracked-files=all)
            $exitCode = $LASTEXITCODE
        }
        if ($exitCode -eq 0) {
            $cachedDiff = @(& git -C $path diff --cached --binary)
            $exitCode = $LASTEXITCODE
        }
    } else {
        $exitCode = 1
    }
    $statusFiles = @()
    $statusContentSha256 = $null
    if ($exitCode -eq 0 -and $kind -ceq 'windows') {
        $statusFiles = @(Get-XpMatrixStatusFileRecords -RepositoryPath $path `
            -StatusLines $status)
        $statusContentSha256 = Get-XpMatrixStatusHash -Lines @(
            $statusFiles | ForEach-Object {
                '{0}|{1}|{2}|{3}|{4}|{5}' -f `
                    $_.xy, $_.path, $_.exists, $_.bytes, $_.sha256, $_.hashError
            })
    } elseif ($exitCode -eq 0 -and $kind -ceq 'wsl') {
        $statusContentSha256 = Get-XpMatrixWslStatusContentHash `
            -RepositoryPath $path
    }
    return [pscustomobject][ordered]@{
        name = [string]$Repository.name
        kind = $kind
        path = $path
        exitCode = $exitCode
        branch = if ($null -ne $branch) { [string]$branch.Trim() } else { $null }
        commit = if ($null -ne $commit) { [string]$commit.Trim() } else { $null }
        statusLines = @($status | ForEach-Object { [string]$_ })
        statusSha256 = Get-XpMatrixStatusHash -Lines @($status |
            ForEach-Object { [string]$_ })
        statusFiles = $statusFiles
        statusContentSha256 = $statusContentSha256
        cachedDiffSha256 = if ($exitCode -eq 0) {
            Get-XpMatrixStatusHash -Lines @($cachedDiff |
                ForEach-Object { [string]$_ })
        } else { $null }
    }
}

function Get-XpAcceptanceMatrixVerification {
    param(
        [Parameter(Mandatory = $true)]
        $Spec
    )

    $started = Get-Date
    $findings = [Collections.Generic.List[string]]::new()
    if ($Spec.schema -ne 1) {
        Add-XpMatrixFinding $findings 'matrix spec schema is not 1'
    }
    if ([string]::IsNullOrWhiteSpace([string]$Spec.taskId)) {
        Add-XpMatrixFinding $findings 'matrix spec taskId is empty'
    }
    if (@($Spec.bundles).Count -eq 0) {
        Add-XpMatrixFinding $findings 'matrix spec contains no bundles'
    }

    $sourceIdentities = [Collections.Generic.List[object]]::new()
    foreach ($expected in @($Spec.frozenFiles)) {
        $identity = Get-XpMatrixFileIdentity -Path ([string]$expected.path)
        $matches = $identity.exists -and
            [uint64]$identity.bytes -eq [uint64]$expected.bytes -and
            [string]$identity.sha256 -ceq [string]$expected.sha256
        $identity | Add-Member -NotePropertyName matchesExpected `
            -NotePropertyValue $matches
        $sourceIdentities.Add($identity)
        if (-not $matches) {
            Add-XpMatrixFinding $findings `
                "frozen file identity differs: $($identity.path)"
        }
    }

    $inputIdentities = [Collections.Generic.List[object]]::new()
    foreach ($expected in @($Spec.inputFiles)) {
        $identity = Get-XpMatrixFileIdentity -Path ([string]$expected.path)
        $identity | Add-Member -NotePropertyName name `
            -NotePropertyValue ([string]$expected.name)
        $matches = $identity.exists -and
            [string]$identity.sha256 -ceq [string]$expected.sha256
        if ($expected.PSObject.Properties['bytes']) {
            $matches = $matches -and
                [uint64]$identity.bytes -eq [uint64]$expected.bytes
        }
        $identity | Add-Member -NotePropertyName matchesExpected `
            -NotePropertyValue $matches
        $inputIdentities.Add($identity)
        if (-not $matches) {
            Add-XpMatrixFinding $findings `
                "input file identity differs: $($identity.path)"
        }
    }

    $repositories = [Collections.Generic.List[object]]::new()
    foreach ($expected in @($Spec.repositories)) {
        $identity = Get-XpMatrixRepositoryIdentity -Repository $expected
        $matches = $identity.exitCode -eq 0 -and
            [string]$identity.branch -ceq [string]$expected.branch -and
            [string]$identity.commit -ceq [string]$expected.commit -and
            [string]$identity.statusSha256 -ceq
                [string]$expected.statusSha256
        # Porcelain status alone misses an edit inside an already-dirty
        # file; a spec that pins the dirty-content hash gets it compared.
        if ($expected.PSObject.Properties['statusContentSha256'] -and
                $expected.statusContentSha256) {
            $matches = $matches -and
                [string]$identity.statusContentSha256 -ceq
                    [string]$expected.statusContentSha256
        }
        # Same for the staged index: an MM path can change its staged blob
        # while status and worktree bytes stay identical.
        if ($expected.PSObject.Properties['cachedDiffSha256'] -and
                $expected.cachedDiffSha256) {
            $matches = $matches -and
                [string]$identity.cachedDiffSha256 -ceq
                    [string]$expected.cachedDiffSha256
        }
        $identity | Add-Member -NotePropertyName matchesExpected `
            -NotePropertyValue $matches
        $repositories.Add($identity)
        if (-not $matches) {
            Add-XpMatrixFinding $findings `
                "repository identity differs: $($identity.name)"
        }
    }

    $package = $null
    if ($Spec.package) {
        try {
            $package = Get-XpAcceptancePackageIdentity `
                -Directory ([string]$Spec.package.directory)
            if (-not $package.valid) {
                Add-XpMatrixFinding $findings 'staged package identity is invalid'
            }
            if ($Spec.package.driverVer -and
                [string]$package.driverVer -cne
                    [string]$Spec.package.driverVer) {
                Add-XpMatrixFinding $findings `
                    'staged package DriverVer differs from matrix spec'
            }
            if ($Spec.package.signerThumbprint -and
                ([string]$package.certificate.thumbprint -cne
                    [string]$Spec.package.signerThumbprint -or
                 [string]$package.catalogSignature.signerThumbprint -cne
                    [string]$Spec.package.signerThumbprint)) {
                Add-XpMatrixFinding $findings `
                    'staged package signer identity differs from matrix spec'
            }
            foreach ($expectedFile in @($Spec.package.files)) {
                $matches = @($package.files | Where-Object {
                    [string]$_.name -ceq [string]$expectedFile.name
                })
                if ($matches.Count -ne 1 -or
                    [string]$matches[0].sha256 -cne
                        [string]$expectedFile.sha256) {
                    Add-XpMatrixFinding $findings `
                        "staged package file differs: $($expectedFile.name)"
                }
            }
            if (@($Spec.package.files).Count -and
                @($package.files).Count -ne @($Spec.package.files).Count) {
                Add-XpMatrixFinding $findings `
                    'staged package file count differs from matrix spec'
            }
        } catch {
            Add-XpMatrixFinding $findings `
                "staged package validation failed: $($_.Exception.Message)"
        }
    }

    $bundles = [Collections.Generic.List[object]]::new()
    $seenBundleDirectories = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($bundleSpec in @($Spec.bundles)) {
        $bundleDirectory = [IO.Path]::GetFullPath(
            [string]$bundleSpec.directory)
        if (-not $seenBundleDirectories.Add($bundleDirectory)) {
            Add-XpMatrixFinding $findings `
                "matrix spec repeats bundle directory: $bundleDirectory"
            continue
        }
        $bundle = Get-XpAcceptanceBundleVerification `
            -Directory $bundleDirectory `
            -Type ([string]$bundleSpec.type) `
            -RunId ([string]$bundleSpec.runId) `
            -TaskId ([string]$Spec.taskId) `
            -ExpectedFields @($bundleSpec.expectedFields) `
            -ExpectedSuites @($bundleSpec.expectedSuites) `
            -ExpectedDriverVer ([string]$Spec.package.driverVer) `
            -ExpectedSignerThumbprint ([string]$Spec.package.signerThumbprint) `
            -ExpectedPackageFiles @($Spec.package.files)
        $bundles.Add($bundle)
        foreach ($finding in @($bundle.findings)) {
            Add-XpMatrixFinding $findings "$($bundle.name): $finding"
        }
    }

    return [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-matrix-verification'
        taskId = [string]$Spec.taskId
        timestamp = Get-Date -Format o
        valid = $findings.Count -eq 0
        verdict = if ($findings.Count -eq 0) { 'PASS' } else { 'FAIL' }
        durationSeconds = [Math]::Round(
            ((Get-Date) - $started).TotalSeconds, 3)
        findings = @($findings)
        frozenFiles = @($sourceIdentities)
        inputFiles = @($inputIdentities)
        repositories = @($repositories)
        package = $package
        bundles = @($bundles)
    }
}
