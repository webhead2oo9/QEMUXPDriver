param(
    [string]$TaskId,
    [string]$EvidenceDirectory,
    [string]$CandidatePackageDirectory,
    [string]$FrozenInputsPath,
    [string]$AcceptanceIsoPath,
    [string]$ChildName,
    [string]$BasePath,
    [ValidateRange(0, 65535)]
    [int]$QmpPort,
    # Diagnostic only. Leave unset for every ordinary run.
    [ValidateRange(0, 65535)]
    [int]$GdbPort = 0,
    [string[]]$Modes = @('Install', 'D3dVisual', 'D3d9'),
    [string]$RetroHostPath = $env:XPD_RETRO_HOST,
    [string]$CorePath,
    [ValidateRange(1, 25)]
    [int]$PerformanceRuns = 3,
    [ValidateRange(15, 180)]
    [int]$GuestLaunchDelaySeconds = 40,
    [switch]$ContinueOnFailure,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'package-identity.ps1')
. (Join-Path $PSScriptRoot 'frozen-inputs.ps1')

function ConvertTo-XpCandidateCycleModes {
    param([string[]]$InputModes)

    return @($InputModes | ForEach-Object { $_ -split ',' } |
        ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

function Get-XpCandidateCyclePackageFingerprint {
    param([psobject]$Identity)

    if (-not $Identity -or -not $Identity.valid) {
        throw 'cannot fingerprint an invalid package identity'
    }
    $parts = [Collections.Generic.List[string]]::new()
    $parts.Add("driverVer=$([string]$Identity.driverVer)")
    $parts.Add("certificate=$([string]$Identity.certificate.thumbprint)")
    $parts.Add(
        "catalogSigner=$([string]$Identity.catalogSignature.signerThumbprint)")
    $parts.Add(
        "signerMatches=$([bool]$Identity.signerMatchesBundledCertificate)")
    foreach ($file in @($Identity.files | Sort-Object name)) {
        $parts.Add("$($file.name):$($file.size):$($file.sha256)")
    }
    return ($parts -join [Environment]::NewLine)
}

function Assert-XpCandidateCycleValidPackage {
    param(
        [string]$Role,
        [psobject]$Identity
    )

    if (-not $Identity -or -not $Identity.valid) {
        $findings = if ($Identity) {
            @($Identity.findings) -join '; '
        } else {
            'identity provider returned no record'
        }
        throw "$Role package identity is invalid: $findings"
    }
}

function Read-XpCandidateCycleManifest {
    param([string]$Path)

    try {
        return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    } catch {
        throw "candidate frozen inputs manifest is invalid JSON: $($_.Exception.Message)"
    }
}

function Assert-XpCandidateCycleManifestPackage {
    param(
        [psobject]$Manifest,
        [psobject]$CandidateIdentity,
        [string]$AcceptanceIsoPath,
        [string]$RetroHostPath,
        [string]$CorePath
    )

    if (-not $Manifest -or [int]$Manifest.schemaVersion -ne 1 -or
            [string]$Manifest.kind -cne 'xpdriver-frozen-inputs') {
        throw 'candidate frozen inputs manifest has an unsupported schema'
    }
    if (-not $Manifest.snapshot -or -not $Manifest.snapshot.directory) {
        throw 'candidate frozen inputs manifest lacks a snapshot directory'
    }
    if (-not $Manifest.package -or -not $Manifest.package.directory) {
        throw 'candidate frozen inputs manifest lacks a package directory'
    }
    $snapshotDirectory = [IO.Path]::GetFullPath(
        [string]$Manifest.snapshot.directory)
    $manifestPackageDirectory = [IO.Path]::GetFullPath(
        [string]$Manifest.package.directory)
    $snapshotPackageDirectory = [IO.Path]::GetFullPath(
        (Join-Path $snapshotDirectory 'package'))
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
            $manifestPackageDirectory, $snapshotPackageDirectory)) {
        throw 'candidate frozen inputs package directory must name the snapshot package'
    }
    if ([string]$Manifest.package.driverVer -cne
            [string]$CandidateIdentity.driverVer) {
        throw 'candidate DriverVer differs from frozen inputs manifest'
    }
    if ([string]$Manifest.package.signerThumbprint -cne
            [string]$CandidateIdentity.certificate.thumbprint) {
        throw 'candidate signer differs from frozen inputs manifest'
    }
    if (-not (Compare-XpFrozenPackageFiles `
            -ManifestFiles @($Manifest.package.files) `
            -CurrentFiles @($CandidateIdentity.files))) {
        throw 'candidate package differs from frozen inputs manifest'
    }
    if ($AcceptanceIsoPath) {
        if (-not $Manifest.acceptanceIso -or
                -not $Manifest.acceptanceIso.path -or
                -not $Manifest.acceptanceIso.sha256) {
            throw 'candidate frozen inputs manifest lacks an acceptance ISO record'
        }
        $manifestIsoPath = [IO.Path]::GetFullPath(
            [string]$Manifest.acceptanceIso.path)
        $snapshotIsoPath = [IO.Path]::GetFullPath(
            (Join-Path $snapshotDirectory 'acceptance.iso'))
        if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
                $manifestIsoPath, $snapshotIsoPath)) {
            throw 'candidate frozen inputs acceptance ISO must name the snapshot ISO'
        }
        $selectedIso = Get-XpFrozenFileIdentity -Path $AcceptanceIsoPath
        if ([string]$Manifest.acceptanceIso.sha256 -cne
                [string]$selectedIso.sha256 -or
                ($Manifest.acceptanceIso.bytes -and
                 [int64]$Manifest.acceptanceIso.bytes -ne
                    [int64]$selectedIso.bytes)) {
            throw 'selected acceptance ISO differs from frozen inputs manifest'
        }
    }
    foreach ($spec in @(
            @('retro host', $RetroHostPath, $Manifest.retroHost),
            @('paired core', $CorePath, $Manifest.core))) {
        if (-not $spec[2] -or -not $spec[2].path -or
                -not $spec[2].sha256) {
            throw "candidate frozen inputs manifest lacks a $($spec[0]) record"
        }
        $selected = Get-XpFrozenFileIdentity -Path ([string]$spec[1])
        if ([string]$spec[2].sha256 -cne [string]$selected.sha256 -or
                ($spec[2].bytes -and
                 [int64]$spec[2].bytes -ne [int64]$selected.bytes)) {
            throw "selected $($spec[0]) differs from frozen inputs manifest"
        }
    }
}

function Test-XpCandidateCycleReparsePoint {
    param([string]$Path)

    $item = Get-Item -LiteralPath $Path -Force
    return (($item.Attributes -band
        [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Test-XpCandidateCycleReparseAlias {
    param([string]$Path)

    # A reparse point in any existing component aliases the real target just
    # like a reparse leaf does, so walk every component up to the volume
    # root. Components that do not exist yet cannot alias anything.
    $component = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($component)) {
        if ((Test-Path -LiteralPath $component) -and
                (Test-XpCandidateCycleReparsePoint -Path $component)) {
            return $true
        }
        $parent = Split-Path -Parent $component
        if (-not $parent -or
                [StringComparer]::OrdinalIgnoreCase.Equals(
                    $parent, $component)) {
            break
        }
        $component = $parent
    }
    return $false
}

function Test-XpCandidateCyclePathWithin {
    param(
        [string]$Child,
        [string]$Parent
    )

    if ([StringComparer]::OrdinalIgnoreCase.Equals($Child, $Parent)) {
        return $true
    }
    $trimmedParent = $Parent.TrimEnd([char[]]@('\', '/'))
    $prefix = $trimmedParent + [IO.Path]::DirectorySeparatorChar
    return $Child.StartsWith($prefix,
        [StringComparison]::OrdinalIgnoreCase)
}

function Get-XpCandidateCycleMutexName {
    param([string]$StagingPackageDirectory)

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(
            $StagingPackageDirectory.ToUpperInvariant())
        $digest = $sha.ComputeHash($bytes)
    } finally {
        $sha.Dispose()
    }
    $suffix = ([BitConverter]::ToString($digest)).Replace('-', '')
    return "Local\XPDriver-CandidatePackage-$suffix"
}

function Invoke-XpCandidatePackageCycleCore {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$StagingPackageDirectory,
        [Parameter(Mandatory = $true)]
        [string]$CandidatePackageDirectory,
        [Parameter(Mandatory = $true)]
        [string]$FrozenInputsPath,
        [string]$AcceptanceIsoPath,
        [Parameter(Mandatory = $true)]
        [string]$RetroHostPath,
        [Parameter(Mandatory = $true)]
        [string]$CorePath,
        [Parameter(Mandatory = $true)]
        [string]$CycleScriptPath,
        [string]$PublisherScriptPath,
        [Parameter(Mandatory = $true)]
        [hashtable]$CycleParameters,
        [Parameter(Mandatory = $true)]
        [ref]$ExitCode,
        # A non-VM seam used only by the focused wrapper tests.
        [scriptblock]$CycleInvoker
    )

    $ExitCode.Value = 1
    $staging = [IO.Path]::GetFullPath($StagingPackageDirectory)
    $candidate = [IO.Path]::GetFullPath($CandidatePackageDirectory)
    $frozen = [IO.Path]::GetFullPath($FrozenInputsPath)
    $evidence = [IO.Path]::GetFullPath(
        [string]$CycleParameters.EvidenceDirectory)
    $retroHost = [IO.Path]::GetFullPath($RetroHostPath)
    $core = [IO.Path]::GetFullPath($CorePath)
    $cycleScript = [IO.Path]::GetFullPath($CycleScriptPath)
    $acceptanceIso = if ($AcceptanceIsoPath) {
        [IO.Path]::GetFullPath($AcceptanceIsoPath)
    } else { $null }
    $hold = "$staging.accepted-hold"

    if (-not (Test-Path -LiteralPath $cycleScript -PathType Leaf)) {
        throw "canonical cycle script is missing: $cycleScript"
    }
    foreach ($inputFile in @(
            @('retro host', $retroHost),
            @('paired core', $core))) {
        if (-not (Test-Path -LiteralPath $inputFile[1] -PathType Leaf)) {
            throw "$($inputFile[0]) is missing: $($inputFile[1])"
        }
    }
    if ($acceptanceIso -and
            -not (Test-Path -LiteralPath $acceptanceIso -PathType Leaf)) {
        throw "acceptance ISO is missing: $acceptanceIso"
    }
    if ((Test-XpCandidateCyclePathWithin -Child $candidate -Parent $staging) -or
            (Test-XpCandidateCyclePathWithin -Child $staging -Parent $candidate)) {
        throw 'candidate and staging package directories must not overlap'
    }
    foreach ($auxiliary in @($frozen, $evidence, $acceptanceIso)) {
        if (-not $auxiliary) { continue }
        foreach ($protected in @($staging, $candidate, $hold)) {
            if (Test-XpCandidateCyclePathWithin `
                    -Child $auxiliary -Parent $protected) {
                throw "auxiliary path must not be inside a package directory: $auxiliary"
            }
        }
    }

    $mutex = [Threading.Mutex]::new(
        $false, (Get-XpCandidateCycleMutexName -StagingPackageDirectory $staging))
    $lockTaken = $false
    try {
        try {
            $lockTaken = $mutex.WaitOne(0)
        } catch [Threading.AbandonedMutexException] {
            $lockTaken = $true
        }
        if (-not $lockTaken) {
            throw 'another candidate-package cycle owns the staging directory'
        }
        if (-not (Test-Path -LiteralPath $staging -PathType Container)) {
            throw "accepted staging package is missing: $staging"
        }
        if (Test-XpCandidateCycleReparsePoint -Path $staging) {
            throw "accepted staging package is already a reparse point: $staging"
        }
        if (Test-Path -LiteralPath $hold) {
            throw "candidate-cycle recovery directory already exists: $hold"
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
            throw "candidate package is missing: $candidate"
        }
        if (Test-XpCandidateCycleReparsePoint -Path $candidate) {
            throw "candidate package must not be a reparse point: $candidate"
        }
        if ((Test-Path -LiteralPath $frozen) -and
                -not (Test-Path -LiteralPath $frozen -PathType Leaf)) {
            throw "frozen inputs path is not a file: $frozen"
        }
        foreach ($reparseGuarded in @(
                @('candidate package', $candidate),
                @('accepted staging package', $staging),
                @('frozen inputs', $frozen),
                @('acceptance ISO', $acceptanceIso),
                @('retro host', $retroHost),
                @('paired core', $core))) {
            if (-not $reparseGuarded[1]) { continue }
            if (Test-XpCandidateCycleReparseAlias -Path $reparseGuarded[1]) {
                throw ("$($reparseGuarded[0]) path contains a reparse " +
                    "point: $($reparseGuarded[1])")
            }
        }

        $acceptedIdentity = Get-XpAcceptancePackageIdentity -Directory $staging
        Assert-XpCandidateCycleValidPackage `
            -Role 'accepted staging' -Identity $acceptedIdentity
        $acceptedFingerprint = Get-XpCandidateCyclePackageFingerprint `
            -Identity $acceptedIdentity
        $candidateIdentity = Get-XpAcceptancePackageIdentity -Directory $candidate
        Assert-XpCandidateCycleValidPackage `
            -Role 'candidate' -Identity $candidateIdentity
        $candidateFingerprint = Get-XpCandidateCyclePackageFingerprint `
            -Identity $candidateIdentity

        $manifestExisted = Test-Path -LiteralPath $frozen -PathType Leaf
        if ($manifestExisted) {
            $manifest = Read-XpCandidateCycleManifest -Path $frozen
            Assert-XpCandidateCycleManifestPackage `
                -Manifest $manifest `
                -CandidateIdentity $candidateIdentity `
                -AcceptanceIsoPath $acceptanceIso `
                -RetroHostPath $retroHost `
                -CorePath $core
        } elseif (-not $acceptanceIso) {
            throw ('candidate frozen inputs manifest is missing; pass ' +
                '-AcceptanceIsoPath to publish one before the cycle')
        }
        if (-not $manifestExisted) {
            $manifestParent = Split-Path -Parent $frozen
            if (-not (Test-Path -LiteralPath $manifestParent `
                    -PathType Container)) {
                throw "frozen inputs destination directory is missing: $manifestParent"
            }
            if (-not $PublisherScriptPath) {
                throw 'frozen inputs publisher script was not supplied'
            }
            $publisherScript = [IO.Path]::GetFullPath($PublisherScriptPath)
            if (-not (Test-Path -LiteralPath $publisherScript -PathType Leaf)) {
                throw "frozen inputs publisher script is missing: $publisherScript"
            }
        }

        $cycleArguments = @{}
        foreach ($entry in $CycleParameters.GetEnumerator()) {
            $cycleArguments[$entry.Key] = $entry.Value
        }
        $cycleArguments['EvidenceDirectory'] = $evidence
        $cycleArguments['FrozenInputsPath'] = $frozen
        $cycleArguments['RetroHostPath'] = $retroHost
        $cycleArguments['CorePath'] = $core

        Write-Output (("CandidatePackageCycle candidate={0} staging={1} " +
            "frozenInputs={2}") -f $candidate, $staging, $frozen)

        $swapStarted = $false
        $primaryError = $null
        $restorationError = $null
        try {
            Move-Item -LiteralPath $staging -Destination $hold
            $swapStarted = $true
            # Repeat the reparse guard at the last moment: the candidate must
            # still resolve only through ordinary directories, and the staging
            # ancestry must not have gained an alias while the accepted
            # package was moved aside.
            if (Test-XpCandidateCycleReparseAlias -Path $candidate) {
                throw ("candidate package path contains a reparse " +
                    "point: $candidate")
            }
            if (Test-XpCandidateCycleReparseAlias -Path $staging) {
                throw ("accepted staging package path contains a reparse " +
                    "point: $staging")
            }
            [void](New-Item -ItemType Junction -Path $staging -Target $candidate)
            Write-Output "CandidatePackageCycleSwap staging=$staging candidate=$candidate"

            $stagedCandidateIdentity = Get-XpAcceptancePackageIdentity `
                -Directory $staging
            Assert-XpCandidateCycleValidPackage `
                -Role 'staged candidate' -Identity $stagedCandidateIdentity
            if ((Get-XpCandidateCyclePackageFingerprint `
                    -Identity $stagedCandidateIdentity) -cne
                    $candidateFingerprint) {
                throw 'staged candidate identity differs from candidate source'
            }

            if (-not $manifestExisted) {
                & $publisherScript `
                    -PackageDirectory $staging `
                    -IsoPath $acceptanceIso `
                    -RetroHostPath $retroHost `
                    -CorePath $core `
                    -OutputPath $frozen `
                    -SkipBuild
                if (-not (Test-Path -LiteralPath $frozen -PathType Leaf)) {
                    throw 'frozen inputs publisher did not create its output'
                }
            }

            $frozenInputs = Get-XpFrozenInputs -Path $frozen
            Assert-XpCandidateCycleManifestPackage `
                -Manifest $frozenInputs `
                -CandidateIdentity $candidateIdentity `
                -AcceptanceIsoPath $acceptanceIso `
                -RetroHostPath $retroHost `
                -CorePath $core
            $frozenRetroHost = [string]$frozenInputs.retroHost.path
            $frozenCore = [string]$frozenInputs.core.path
            [void](Assert-XpFrozenExecutionInputs `
                -Manifest $frozenInputs `
                -RetroHostPath $frozenRetroHost `
                -CorePath $frozenCore)
            $cycleArguments['PackageDirectory'] =
                [string]$frozenInputs.package.directory
            $cycleArguments['RetroHostPath'] = $frozenRetroHost
            $cycleArguments['CorePath'] = $frozenCore

            if ($CycleInvoker) {
                $invokeResults = @(& $CycleInvoker $cycleScript $cycleArguments)
                if ($invokeResults.Count -ne 1 -or
                        $invokeResults[0] -isnot [int]) {
                    throw 'candidate cycle test invoker must return one integer exit code'
                }
                $cycleExit = [int]$invokeResults[0]
            } else {
                $global:LASTEXITCODE = 0
                & $cycleScript @cycleArguments
                $cycleExit = [int]$LASTEXITCODE
            }
            $ExitCode.Value = $cycleExit

            $candidateAfter = Get-XpAcceptancePackageIdentity `
                -Directory $candidate
            Assert-XpCandidateCycleValidPackage `
                -Role 'candidate after cycle' -Identity $candidateAfter
            if ((Get-XpCandidateCyclePackageFingerprint `
                    -Identity $candidateAfter) -cne $candidateFingerprint) {
                throw 'candidate package changed during the acceptance cycle'
            }
        } catch {
            $primaryError = $_
        } finally {
            if ($swapStarted) {
                try {
                    if (Test-Path -LiteralPath $staging) {
                        if (-not (Test-XpCandidateCycleReparsePoint `
                                -Path $staging)) {
                            throw ('canonical staging path is no longer the ' +
                                "candidate junction: $staging")
                        }
                        & cmd.exe /d /c rmdir $staging
                        if ($LASTEXITCODE -ne 0 -or
                                (Test-Path -LiteralPath $staging)) {
                            throw "candidate staging junction could not be removed: $staging"
                        }
                    }
                    if (-not (Test-Path -LiteralPath $hold `
                            -PathType Container)) {
                        throw "accepted package recovery directory is missing: $hold"
                    }
                    Move-Item -LiteralPath $hold -Destination $staging
                    $restoredIdentity = Get-XpAcceptancePackageIdentity `
                        -Directory $staging
                    Assert-XpCandidateCycleValidPackage `
                        -Role 'restored accepted staging' `
                        -Identity $restoredIdentity
                    if ((Get-XpCandidateCyclePackageFingerprint `
                            -Identity $restoredIdentity) -cne
                            $acceptedFingerprint) {
                        throw 'restored accepted package identity differs from its pre-cycle identity'
                    }
                    Write-Output "CandidatePackageCycleRestore result=pass staging=$staging"
                } catch {
                    $restorationError = $_
                }
            }
        }

        if ($restorationError) {
            $prior = if ($primaryError) {
                "primary failure: $($primaryError.Exception.Message)"
            } elseif ($ExitCode.Value -ne 0) {
                "cycle exit code $($ExitCode.Value)"
            } else {
                'cycle completed before restoration failed'
            }
            throw (('candidate package restoration failed after {0}: {1}; ' +
                'inspect the staging and accepted-hold paths before another run') -f
                $prior, $restorationError.Exception.Message)
        }
        if ($primaryError) {
            throw $primaryError
        }
    } finally {
        if ($lockTaken) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        foreach ($required in @(
                @('TaskId', $TaskId),
                @('EvidenceDirectory', $EvidenceDirectory),
                @('CandidatePackageDirectory', $CandidatePackageDirectory),
                @('FrozenInputsPath', $FrozenInputsPath),
                @('ChildName', $ChildName),
                @('BasePath', $BasePath))) {
            if ([string]::IsNullOrWhiteSpace([string]$required[1])) {
                throw "-$($required[0]) is required"
            }
        }
        if ($QmpPort -le 0) {
            throw '-QmpPort is required'
        }
        $Modes = @(ConvertTo-XpCandidateCycleModes -InputModes $Modes)
        if (-not $Modes.Count) {
            throw 'No modes requested'
        }
        if ($Modes -notcontains 'Install') {
            throw 'candidate-package cycles require Install mode on a fresh child'
        }
        if ($DryRun -and
                -not (Test-Path -LiteralPath $FrozenInputsPath -PathType Leaf)) {
            throw '-DryRun requires an existing frozen inputs manifest'
        }

        $workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        if (-not $RetroHostPath) {
            throw 'Pass -RetroHostPath or set XPD_RETRO_HOST to retro_host.exe'
        }
        if (-not $CorePath) {
            $CorePath = Get-XpDefaultCorePath -Workspace $workspace
        }
        $stagingPackage = Join-Path $workspace 'guest\xpdm\build\package'
        $cycleParameters = @{
            TaskId = $TaskId
            EvidenceDirectory = $EvidenceDirectory
            ChildName = $ChildName
            BasePath = $BasePath
            QmpPort = $QmpPort
            Modes = $Modes
            PerformanceRuns = $PerformanceRuns
            GuestLaunchDelaySeconds = $GuestLaunchDelaySeconds
        }
        if ($GdbPort -gt 0) { $cycleParameters['GdbPort'] = $GdbPort }
        if ($ContinueOnFailure) {
            $cycleParameters['ContinueOnFailure'] = $true
        }
        if ($DryRun) { $cycleParameters['DryRun'] = $true }

        $candidateCycleExit = 1
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $stagingPackage `
            -CandidatePackageDirectory $CandidatePackageDirectory `
            -FrozenInputsPath $FrozenInputsPath `
            -AcceptanceIsoPath $AcceptanceIsoPath `
            -RetroHostPath $RetroHostPath `
            -CorePath $CorePath `
            -CycleScriptPath (Join-Path $PSScriptRoot `
                'invoke-xp-test-cycle.ps1') `
            -PublisherScriptPath (Join-Path $PSScriptRoot `
                'publish-xp-acceptance-inputs.ps1') `
            -CycleParameters $cycleParameters `
            -ExitCode ([ref]$candidateCycleExit)
        Write-Output (("CandidatePackageCycle taskId={0} result={1} " +
            "exitCode={2} evidence={3}") -f
            $TaskId,
            $(if ($candidateCycleExit -eq 0) { 'pass' } else { 'fail' }),
            $candidateCycleExit,
            ([IO.Path]::GetFullPath($EvidenceDirectory)))
        exit $candidateCycleExit
    } catch {
        [Console]::Error.WriteLine(
            "CandidatePackageCycle failed: $($_.Exception.Message)")
        exit 1
    }
}
