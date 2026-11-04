$ErrorActionPreference = 'Stop'
$wrapper = Join-Path $PSScriptRoot 'invoke-xp-candidate-cycle.ps1'
. $wrapper

function Assert-CandidateCycle {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

$normalizedModes = @(ConvertTo-XpCandidateCycleModes `
    -InputModes @(' Install,D3d9 ', ' D3dVs11 '))
Assert-CandidateCycle `
    (($normalizedModes.Count -eq 3) -and
     (($normalizedModes -join ',') -ceq 'Install,D3d9,D3dVs11')) `
    'comma-joined -File modes were not normalized in order'
$emptyModes = @(ConvertTo-XpCandidateCycleModes -InputModes @(' , ', ''))
Assert-CandidateCycle ($emptyModes.Count -eq 0) `
    'empty mode tokens were not discarded'

# The production wrapper uses the signed-package identity provider. These
# focused tests replace it with a deterministic all-members identity so the
# staging and restoration contract can be exercised without signing fixtures.
function Get-XpAcceptancePackageIdentity {
    param([string]$Directory)

    $full = [IO.Path]::GetFullPath($Directory)
    $findings = [Collections.Generic.List[string]]::new()
    $files = [Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        $findings.Add("package directory is missing: $full")
    } else {
        foreach ($member in @(Get-ChildItem -LiteralPath $full -Force |
                Sort-Object Name)) {
            if ($member.PSIsContainer) {
                $findings.Add("unexpected package subdirectory: $($member.Name)")
            } else {
                $files.Add([pscustomobject][ordered]@{
                    name = $member.Name
                    size = $member.Length
                    sha256 = (Get-FileHash -LiteralPath $member.FullName `
                        -Algorithm SHA256).Hash
                })
            }
        }
    }
    if (-not $files.Count) {
        $findings.Add('package contains no files')
    }
    $thumbprint = 'A' * 40
    return [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-test-package-identity'
        directory = $full
        valid = $findings.Count -eq 0
        driverVer = '07/29/2026,1.0.0.999'
        certificate = [pscustomobject][ordered]@{
            subject = 'CN=XPDriver Candidate Cycle Test'
            thumbprint = $thumbprint
        }
        catalogSignature = [pscustomobject][ordered]@{
            signerThumbprint = $thumbprint
        }
        signerMatchesBundledCertificate = $true
        files = @($files)
        findings = @($findings)
    }
}

function Get-XpFrozenInputs {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "test frozen inputs manifest is missing: $Path"
    }
    $manifest = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    $package = Get-XpAcceptancePackageIdentity `
        -Directory ([string]$manifest.package.directory)
    if (-not $package.valid -or
            -not (Compare-XpFrozenPackageFiles `
                -ManifestFiles @($manifest.package.files) `
                -CurrentFiles @($package.files))) {
        throw 'test frozen package files differ on disk'
    }
    return $manifest
}

function Assert-XpFrozenExecutionInputs {
    param(
        [psobject]$Manifest,
        [string]$RetroHostPath,
        [string]$CorePath
    )

    foreach ($spec in @(
            @('retroHost', $RetroHostPath),
            @('core', $CorePath))) {
        $record = $Manifest.($spec[0])
        $selected = Get-XpFrozenFileIdentity -Path $spec[1]
        Assert-CandidateCycle `
            ([StringComparer]::OrdinalIgnoreCase.Equals(
                [IO.Path]::GetFullPath([string]$record.path),
                [IO.Path]::GetFullPath([string]$spec[1])) -and
             [string]$record.sha256 -ceq $selected.sha256) `
            "test frozen $($spec[0]) binding differs"
    }
    return [pscustomobject]@{
        retroHost = Get-XpFrozenFileIdentity -Path $RetroHostPath
        core = Get-XpFrozenFileIdentity -Path $CorePath
    }
}

function New-CandidateCycleFixture {
    param(
        [string]$Root,
        [string]$Name
    )

    $scenario = Join-Path $Root $Name
    $staging = Join-Path $scenario 'staging-package'
    $candidate = Join-Path $scenario 'candidate-package'
    [void](New-Item -ItemType Directory -Path $staging -Force)
    [void](New-Item -ItemType Directory -Path $candidate -Force)
    foreach ($file in @('display.dll', 'miniport.sys')) {
        [IO.File]::WriteAllText(
            (Join-Path $staging $file), "accepted-$Name-$file")
        [IO.File]::WriteAllText(
            (Join-Path $candidate $file), "candidate-$Name-$file")
    }
    [IO.File]::WriteAllText(
        (Join-Path $staging 'accepted-only.bin'), "accepted-$Name-extra")
    [IO.File]::WriteAllText(
        (Join-Path $candidate 'candidate-only.bin'), "candidate-$Name-extra")

    $iso = Join-Path $scenario 'acceptance.iso'
    $retroHost = Join-Path $scenario 'retro-host.exe'
    $core = Join-Path $scenario 'paired-core.dll'
    [IO.File]::WriteAllText($iso, "iso-$Name")
    [IO.File]::WriteAllText($retroHost, "host-$Name")
    [IO.File]::WriteAllText($core, "core-$Name")

    $snapshot = Join-Path $scenario 'snapshot'
    $snapshotPackage = Join-Path $snapshot 'package'
    [void](New-Item -ItemType Directory -Path $snapshotPackage -Force)
    foreach ($member in @(Get-ChildItem -LiteralPath $candidate -File)) {
        Copy-Item -LiteralPath $member.FullName `
            -Destination (Join-Path $snapshotPackage $member.Name)
    }
    $snapshotIso = Join-Path $snapshot 'acceptance.iso'
    [IO.File]::Copy($iso, $snapshotIso)

    $candidateIdentity = Get-XpAcceptancePackageIdentity -Directory $candidate
    $manifestPath = Join-Path $scenario 'frozen-inputs.json'
    $manifest = [pscustomobject][ordered]@{
        schemaVersion = 1
        kind = 'xpdriver-frozen-inputs'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        transportAbi = 999
        snapshot = [pscustomobject][ordered]@{
            directory = [IO.Path]::GetFullPath($snapshot)
            packageDigest = '0' * 64
        }
        package = [pscustomobject][ordered]@{
            directory = [IO.Path]::GetFullPath($snapshotPackage)
            driverVer = $candidateIdentity.driverVer
            signerThumbprint = $candidateIdentity.certificate.thumbprint
            files = @($candidateIdentity.files)
        }
        acceptanceIso = Get-XpFrozenFileIdentity -Path $snapshotIso
        retroHost = Get-XpFrozenFileIdentity -Path $retroHost
        core = Get-XpFrozenFileIdentity -Path $core
    }
    [IO.File]::WriteAllText($manifestPath,
        ($manifest | ConvertTo-Json -Depth 10))

    return [pscustomobject][ordered]@{
        root = $scenario
        staging = $staging
        hold = "$staging.accepted-hold"
        candidate = $candidate
        manifest = $manifestPath
        iso = $iso
        snapshotPackage = $snapshotPackage
        retroHost = $retroHost
        core = $core
        evidence = Join-Path $scenario 'evidence'
        acceptedFingerprint = Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity -Directory $staging)
        candidateFingerprint = Get-XpCandidateCyclePackageFingerprint `
            -Identity $candidateIdentity
    }
}

function Assert-CandidateCycleRestored {
    param([psobject]$Fixture)

    Assert-CandidateCycle `
        (Test-Path -LiteralPath $Fixture.staging -PathType Container) `
        'accepted staging package was not restored'
    Assert-CandidateCycle `
        (-not (Test-XpCandidateCycleReparsePoint -Path $Fixture.staging)) `
        'restored accepted staging package remains a reparse point'
    Assert-CandidateCycle `
        (-not (Test-Path -LiteralPath $Fixture.hold)) `
        'accepted package hold directory was not retired'
    Assert-CandidateCycle `
        ((Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity `
                -Directory $Fixture.staging)) -ceq
            $Fixture.acceptedFingerprint) `
        'accepted package identity changed across the candidate cycle'
    Assert-CandidateCycle `
        ((Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity `
                -Directory $Fixture.candidate)) -ceq
            $Fixture.candidateFingerprint) `
        'candidate package identity changed across the candidate cycle'
}

$script:cycleBehavior = 'success'
$script:observedCycleParameters = $null
$script:observedStagingWasReparse = $false
$script:expectedStaging = $null
$testCycleInvoker = {
    param([string]$CycleScriptPath, [hashtable]$Parameters)

    $script:observedCycleParameters = $Parameters
    $script:observedStagingWasReparse =
        Test-XpCandidateCycleReparsePoint -Path $script:expectedStaging
    Assert-CandidateCycle `
        (Test-Path -LiteralPath (
            Join-Path $script:expectedStaging 'candidate-only.bin') `
            -PathType Leaf) `
        'canonical staging path did not expose the candidate package'
    if ($script:cycleBehavior -eq 'mutate-accepted-hold') {
        [IO.File]::WriteAllText(
            (Join-Path "$($script:expectedStaging).accepted-hold" `
                'accepted-only.bin'), 'mutated while held')
    }
    if ($script:cycleBehavior -eq 'child-failure') { return [int]37 }
    return [int]0
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-candidate-cycle-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
$testPublisher = Join-Path $testRoot 'test-publish-frozen-inputs.ps1'
$testPublisherSource = @'
param(
    [string]$PackageDirectory,
    [string]$IsoPath,
    [string]$RetroHostPath,
    [string]$CorePath,
    [string]$OutputPath,
    [switch]$SkipBuild
)
if (Test-Path -LiteralPath $OutputPath) {
    throw "test publisher refuses to overwrite: $OutputPath"
}
$snapshot = Join-Path (Split-Path -Parent $OutputPath) 'published-snapshot'
$snapshotPackage = Join-Path $snapshot 'package'
[void](New-Item -ItemType Directory -Path $snapshotPackage -Force)
foreach ($member in @(Get-ChildItem -LiteralPath $PackageDirectory -File)) {
    Copy-Item -LiteralPath $member.FullName `
        -Destination (Join-Path $snapshotPackage $member.Name)
}
$snapshotIso = Join-Path $snapshot 'acceptance.iso'
[IO.File]::Copy($IsoPath, $snapshotIso)
$package = Get-XpAcceptancePackageIdentity -Directory $snapshotPackage
$manifest = [pscustomobject][ordered]@{
    schemaVersion = 1
    kind = 'xpdriver-frozen-inputs'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    transportAbi = 999
    snapshot = [pscustomobject][ordered]@{
        directory = [IO.Path]::GetFullPath($snapshot)
        packageDigest = '0' * 64
    }
    package = [pscustomobject][ordered]@{
        directory = [IO.Path]::GetFullPath($snapshotPackage)
        driverVer = $package.driverVer
        signerThumbprint = $package.certificate.thumbprint
        files = @($package.files)
    }
    acceptanceIso = Get-XpFrozenFileIdentity -Path $snapshotIso
    retroHost = Get-XpFrozenFileIdentity -Path $RetroHostPath
    core = Get-XpFrozenFileIdentity -Path $CorePath
}
[IO.File]::WriteAllText($OutputPath,
    ($manifest | ConvertTo-Json -Depth 10))
Write-Output "TestFrozenInputsPublished output=$OutputPath"
'@
[IO.File]::WriteAllText($testPublisher, $testPublisherSource)
try {
    # Success also exercises immutable manifest publication. The canonical
    # path exposes the candidate only during publication and child invocation,
    # then the accepted package returns byte-for-byte.
    $success = New-CandidateCycleFixture -Root $testRoot -Name 'success'
    Remove-Item -LiteralPath $success.manifest -Force
    $script:cycleBehavior = 'success'
    $script:observedCycleParameters = $null
    $script:observedStagingWasReparse = $false
    $script:expectedStaging = $success.staging
    $successExit = 99
    Invoke-XpCandidatePackageCycleCore `
        -StagingPackageDirectory $success.staging `
        -CandidatePackageDirectory $success.candidate `
        -FrozenInputsPath $success.manifest `
        -AcceptanceIsoPath $success.iso `
        -RetroHostPath $success.retroHost `
        -CorePath $success.core `
        -CycleScriptPath $wrapper `
        -PublisherScriptPath $testPublisher `
        -CycleParameters @{
            TaskId = 'candidate-success'
            EvidenceDirectory = $success.evidence
            ChildName = 'candidate-success-child'
            BasePath = Join-Path $success.root 'base.qcow2'
            QmpPort = 55001
            Modes = @('Install', 'D3d9')
        } `
        -ExitCode ([ref]$successExit) `
        -CycleInvoker $testCycleInvoker
    Assert-CandidateCycle ($successExit -eq 0) `
        'successful candidate cycle did not return exit code 0'
    Assert-CandidateCycle $script:observedStagingWasReparse `
        'candidate cycle did not invoke the child through a staging junction'
    $successManifest = Get-Content -Raw -LiteralPath $success.manifest |
        ConvertFrom-Json
    Assert-CandidateCycle `
        ([StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath(
                [string]$script:observedCycleParameters.PackageDirectory),
            [IO.Path]::GetFullPath(
                [string]$successManifest.package.directory)) -and
         [StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath(
                [string]$script:observedCycleParameters.FrozenInputsPath),
            [IO.Path]::GetFullPath($success.manifest))) `
        'canonical cycle parameters were not pinned to snapshot and frozen inputs'
    Assert-CandidateCycle `
        (Test-Path -LiteralPath $success.manifest -PathType Leaf) `
        'success path did not publish the requested frozen inputs manifest'
    Assert-CandidateCycleRestored -Fixture $success

    # Manifest mismatch is rejected before moving the accepted package or
    # invoking the child.
    $mismatch = New-CandidateCycleFixture -Root $testRoot -Name 'mismatch'
    $mismatchManifest = Get-Content -Raw -LiteralPath $mismatch.manifest |
        ConvertFrom-Json
    $mismatchManifest.package.files[0].sha256 = '0' * 64
    [IO.File]::WriteAllText($mismatch.manifest,
        ($mismatchManifest | ConvertTo-Json -Depth 10))
    $script:observedCycleParameters = $null
    $mismatchExit = 99
    $mismatchCaught = $false
    try {
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $mismatch.staging `
            -CandidatePackageDirectory $mismatch.candidate `
            -FrozenInputsPath $mismatch.manifest `
            -AcceptanceIsoPath $mismatch.iso `
            -RetroHostPath $mismatch.retroHost `
            -CorePath $mismatch.core `
            -CycleScriptPath $wrapper `
            -CycleParameters @{
                TaskId = 'candidate-mismatch'
                EvidenceDirectory = $mismatch.evidence
            } `
            -ExitCode ([ref]$mismatchExit) `
            -CycleInvoker $testCycleInvoker
    } catch {
        $mismatchCaught = $_.Exception.Message -like
            '*candidate package differs from frozen inputs manifest*'
    }
    Assert-CandidateCycle $mismatchCaught `
        'candidate/manifest mismatch did not fail before staging'
    Assert-CandidateCycle ($null -eq $script:observedCycleParameters) `
        'candidate/manifest mismatch invoked the child'
    Assert-CandidateCycleRestored -Fixture $mismatch

    # A nonzero child result is preserved after successful restoration.
    $childFailure = New-CandidateCycleFixture `
        -Root $testRoot -Name 'child-failure'
    $script:cycleBehavior = 'child-failure'
    $childFailureExit = 99
    $script:expectedStaging = $childFailure.staging
    Invoke-XpCandidatePackageCycleCore `
        -StagingPackageDirectory $childFailure.staging `
        -CandidatePackageDirectory $childFailure.candidate `
        -FrozenInputsPath $childFailure.manifest `
        -AcceptanceIsoPath $childFailure.iso `
        -RetroHostPath $childFailure.retroHost `
        -CorePath $childFailure.core `
        -CycleScriptPath $wrapper `
        -CycleParameters @{
            TaskId = 'candidate-child-failure'
            EvidenceDirectory = $childFailure.evidence
        } `
        -ExitCode ([ref]$childFailureExit) `
        -CycleInvoker $testCycleInvoker
    Assert-CandidateCycle ($childFailureExit -eq 37) `
        'candidate wrapper did not preserve the child failure exit code'
    Assert-CandidateCycleRestored -Fixture $childFailure

    # Altering any held accepted-package member is detected after the directory
    # is restored; restoration cannot be reported as a pass on a partial copy.
    $restoreFailure = New-CandidateCycleFixture `
        -Root $testRoot -Name 'restore-failure'
    $script:cycleBehavior = 'mutate-accepted-hold'
    $script:expectedStaging = $restoreFailure.staging
    $restoreFailureExit = 99
    $restoreFailureCaught = $false
    try {
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $restoreFailure.staging `
            -CandidatePackageDirectory $restoreFailure.candidate `
            -FrozenInputsPath $restoreFailure.manifest `
            -AcceptanceIsoPath $restoreFailure.iso `
            -RetroHostPath $restoreFailure.retroHost `
            -CorePath $restoreFailure.core `
            -CycleScriptPath $wrapper `
            -CycleParameters @{
                TaskId = 'candidate-restore-failure'
                EvidenceDirectory = $restoreFailure.evidence
            } `
            -ExitCode ([ref]$restoreFailureExit) `
            -CycleInvoker $testCycleInvoker
    } catch {
        $restoreFailureCaught = $_.Exception.Message -like
            '*restored accepted package identity differs from its pre-cycle identity*'
    }
    Assert-CandidateCycle $restoreFailureCaught `
        'changed accepted package was reported as successfully restored'
    Assert-CandidateCycle `
        ((Test-Path -LiteralPath $restoreFailure.staging `
            -PathType Container) -and
         -not (Test-XpCandidateCycleReparsePoint `
            -Path $restoreFailure.staging) -and
         -not (Test-Path -LiteralPath $restoreFailure.hold)) `
        'restoration failure did not leave the accepted directory inspectable'
    Assert-CandidateCycle `
        ((Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity `
                -Directory $restoreFailure.candidate)) -ceq
            $restoreFailure.candidateFingerprint) `
        'restoration failure changed the candidate package'

    # A candidate reached through a reparse leaf aliases storage outside the
    # audited tree and must fail before the accepted package moves.
    $leafAlias = New-CandidateCycleFixture -Root $testRoot -Name 'leaf-alias'
    Move-Item -LiteralPath $leafAlias.candidate `
        -Destination (Join-Path $leafAlias.root 'candidate-real')
    [void](New-Item -ItemType Junction -Path $leafAlias.candidate `
        -Target (Join-Path $leafAlias.root 'candidate-real'))
    $script:observedCycleParameters = $null
    $leafAliasExit = 99
    $leafAliasCaught = $false
    try {
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $leafAlias.staging `
            -CandidatePackageDirectory $leafAlias.candidate `
            -FrozenInputsPath $leafAlias.manifest `
            -AcceptanceIsoPath $leafAlias.iso `
            -RetroHostPath $leafAlias.retroHost `
            -CorePath $leafAlias.core `
            -CycleScriptPath $wrapper `
            -CycleParameters @{
                TaskId = 'candidate-leaf-alias'
                EvidenceDirectory = $leafAlias.evidence
            } `
            -ExitCode ([ref]$leafAliasExit) `
            -CycleInvoker $testCycleInvoker
    } catch {
        $leafAliasCaught = $_.Exception.Message -like
            '*candidate package must not be a reparse point*'
    }
    Assert-CandidateCycle $leafAliasCaught `
        'candidate reparse leaf did not fail before staging'
    Assert-CandidateCycle ($null -eq $script:observedCycleParameters) `
        'candidate reparse leaf invoked the child'
    Assert-CandidateCycleRestored -Fixture $leafAlias
    & cmd.exe /d /c rmdir $leafAlias.candidate

    # An ordinary candidate leaf below a reparse ancestor still aliases the
    # real target; the component walk must reject it before staging.
    $ancestorAlias = New-CandidateCycleFixture `
        -Root $testRoot -Name 'ancestor-alias'
    $realParent = Join-Path $ancestorAlias.root 'real-parent'
    [void](New-Item -ItemType Directory -Path $realParent)
    Move-Item -LiteralPath $ancestorAlias.candidate -Destination $realParent
    $aliasParent = Join-Path $ancestorAlias.root 'alias-parent'
    [void](New-Item -ItemType Junction -Path $aliasParent -Target $realParent)
    $aliasCandidate = Join-Path $aliasParent 'candidate-package'
    $script:observedCycleParameters = $null
    $ancestorAliasExit = 99
    $ancestorAliasCaught = $false
    try {
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $ancestorAlias.staging `
            -CandidatePackageDirectory $aliasCandidate `
            -FrozenInputsPath $ancestorAlias.manifest `
            -AcceptanceIsoPath $ancestorAlias.iso `
            -RetroHostPath $ancestorAlias.retroHost `
            -CorePath $ancestorAlias.core `
            -CycleScriptPath $wrapper `
            -CycleParameters @{
                TaskId = 'candidate-ancestor-alias'
                EvidenceDirectory = $ancestorAlias.evidence
            } `
            -ExitCode ([ref]$ancestorAliasExit) `
            -CycleInvoker $testCycleInvoker
    } catch {
        $ancestorAliasCaught = $_.Exception.Message -like
            '*candidate package path contains a reparse point*'
    }
    Assert-CandidateCycle $ancestorAliasCaught `
        'candidate below a reparse ancestor did not fail before staging'
    Assert-CandidateCycle ($null -eq $script:observedCycleParameters) `
        'candidate below a reparse ancestor invoked the child'
    Assert-CandidateCycle `
        ((Test-Path -LiteralPath $ancestorAlias.staging `
            -PathType Container) -and
         -not (Test-XpCandidateCycleReparsePoint `
            -Path $ancestorAlias.staging) -and
         -not (Test-Path -LiteralPath $ancestorAlias.hold)) `
        'reparse ancestor rejection disturbed the accepted staging package'
    Assert-CandidateCycle `
        ((Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity `
                -Directory $ancestorAlias.staging)) -ceq
            $ancestorAlias.acceptedFingerprint) `
        'reparse ancestor rejection changed the accepted package'
    & cmd.exe /d /c rmdir $aliasParent

    # The acceptance ISO is bound by hash; an ancestor alias on its path must
    # fail before the cycle trusts those bytes.
    $isoAlias = New-CandidateCycleFixture -Root $testRoot -Name 'iso-alias'
    $isoRealParent = Join-Path $isoAlias.root 'iso-real'
    [void](New-Item -ItemType Directory -Path $isoRealParent)
    Move-Item -LiteralPath $isoAlias.iso -Destination $isoRealParent
    $isoAliasParent = Join-Path $isoAlias.root 'iso-link'
    [void](New-Item -ItemType Junction -Path $isoAliasParent `
        -Target $isoRealParent)
    $aliasedIso = Join-Path $isoAliasParent 'acceptance.iso'
    $script:observedCycleParameters = $null
    $isoAliasExit = 99
    $isoAliasCaught = $false
    try {
        Invoke-XpCandidatePackageCycleCore `
            -StagingPackageDirectory $isoAlias.staging `
            -CandidatePackageDirectory $isoAlias.candidate `
            -FrozenInputsPath $isoAlias.manifest `
            -AcceptanceIsoPath $aliasedIso `
            -RetroHostPath $isoAlias.retroHost `
            -CorePath $isoAlias.core `
            -CycleScriptPath $wrapper `
            -CycleParameters @{
                TaskId = 'candidate-iso-alias'
                EvidenceDirectory = $isoAlias.evidence
            } `
            -ExitCode ([ref]$isoAliasExit) `
            -CycleInvoker $testCycleInvoker
    } catch {
        $isoAliasCaught = $_.Exception.Message -like
            '*acceptance ISO path contains a reparse point*'
    }
    Assert-CandidateCycle $isoAliasCaught `
        'acceptance ISO below a reparse ancestor did not fail before staging'
    Assert-CandidateCycle ($null -eq $script:observedCycleParameters) `
        'acceptance ISO below a reparse ancestor invoked the child'
    Assert-CandidateCycle `
        ((Test-Path -LiteralPath $isoAlias.staging -PathType Container) -and
         -not (Test-XpCandidateCycleReparsePoint -Path $isoAlias.staging) -and
         -not (Test-Path -LiteralPath $isoAlias.hold)) `
        'ISO reparse ancestor rejection disturbed the accepted staging package'
    Assert-CandidateCycle `
        ((Get-XpCandidateCyclePackageFingerprint `
            -Identity (Get-XpAcceptancePackageIdentity `
                -Directory $isoAlias.staging)) -ceq
            $isoAlias.acceptedFingerprint) `
        'ISO reparse ancestor rejection changed the accepted package'
    & cmd.exe /d /c rmdir $isoAliasParent

    Write-Output 'candidate package cycle wrapper tests passed'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}
