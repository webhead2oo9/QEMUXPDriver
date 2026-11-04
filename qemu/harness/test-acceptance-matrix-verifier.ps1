$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'acceptance-matrix-verifier.ps1')

function Assert-XpMatrixVerifier {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Write-XpMatrixFixtureText {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text,
        [Text.UTF8Encoding]::new($false))
}

function New-XpMatrixFixtureContext {
    param(
        [string]$Directory,
        [string]$RunId,
        [string]$TaskId,
        [string]$Tier
    )
    return Initialize-XpAcceptanceRunArtifacts -Directory $Directory `
        -RunId $RunId -TaskId $TaskId -Tier $Tier `
        -CommandLine "synthetic $RunId"
}

function Complete-XpMatrixFixtureBundle {
    param(
        [psobject]$Context,
        [psobject]$Result
    )
    $Result | Add-Member -NotePropertyName artifacts `
        -NotePropertyValue @(
            Get-XpAcceptanceRunArtifactRecords -Context $Context |
                ForEach-Object { $_.name }) -Force
    [void](Write-XpAcceptanceRunResult -Context $Context -Result $Result)
    [void](Write-XpAcceptanceRunManifest -Context $Context)
}

function Add-XpMatrixProfileWord {
    param(
        [Collections.Generic.List[string]]$Lines,
        [string]$Label,
        [uint64]$Value,
        [int]$Words,
        [switch]$Miniport
    )
    [void]$Lines.Add($Label)
    $values = if ($Words -eq 2) {
        @([uint32]($Value / [uint64]4294967296),
          [uint32]($Value % [uint64]4294967296))
    } else { @([uint32]$Value) }
    foreach ($value in $values) {
        if ($Miniport) { [void]$Lines.Add('QXPMP STATUS') }
        [void]$Lines.Add(('0x{0:X8}' -f $value))
    }
}

function New-XpMatrixProfileFixture {
    $lines = [Collections.Generic.List[string]]::new()
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DP2 CALLS' 10 1
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DP2 COMMAND BYTES' 1000 2
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DP2 TICKS' 500 2
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DRAW IOCTL CALLS' 4 1
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DRAW IOCTL BYTES' 2000 2
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF DRAW IOCTL TICKS' 300 2
    Add-XpMatrixProfileWord $lines 'QXPDISP PERF FREQUENCY' 100 2
    Add-XpMatrixProfileWord $lines 'QXPMP D3D DRAWS' 20 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D DRAW IOCTLS' 0 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D DRAW BATCH IOCTLS' 4 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D DRAW BATCH MAX' 8 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PRESENTS' 3 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D CLEARS' 2 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D UPLOADS' 1 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D BATCHES' 2 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D BATCHMAX' 12 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D FIXEDREUSE' 1 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D TEXREUSE' 2 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D ARRAYREUSE' 3 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D INTERLEAVED' 18 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D COMPACT' 18 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF DRAW TICKS' 1000 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF PACKET TICKS' 100 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF PREPARE TICKS' 50 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF STATE TICKS' 100 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF SUBMIT TICKS' 700 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF INTERLEAVED COPY TICKS' 500 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF INTERLEAVED COPY BYTES' 4096 2 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF INTERLEAVED COPY CALLS' 18 1 -Miniport
    Add-XpMatrixProfileWord $lines 'QXPMP D3D PERF FREQUENCY' 100 2 -Miniport
    [void]$lines.Add('QXPMP D3D CLOSE+')
    return $lines -join "`r`n"
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-matrix-verifier-$PID-$([Guid]::NewGuid().ToString('N'))"
[void](New-Item -ItemType Directory -Path $testRoot)
try {
    $taskId = 'matrix-verifier-test'
    $selfDirectory = Join-Path $testRoot 'self'
    $selfContext = New-XpMatrixFixtureContext -Directory $selfDirectory `
        -RunId 'matrix-self' -TaskId $taskId -Tier T0
    Write-XpMatrixFixtureText -Path (Join-Path $selfDirectory 'suite.log') `
        -Text "suite passed`n"
    $selfResult = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-harness-self-test'
        taskId = $taskId
        runId = 'matrix-self'
        tier = 'T0'
        result = 'pass'
        exitCode = 0
        scriptsParsed = 22
        numericObservation = 1.25
        parseErrors = @()
        suites = @(
            [pscustomobject]@{ name = 'suite-a'; result = 'pass'; exitCode = 0 },
            [pscustomobject]@{ name = 'suite-b'; result = 'pass'; exitCode = 0 })
    }
    Complete-XpMatrixFixtureBundle $selfContext $selfResult
    $selfCheck = Get-XpAcceptanceBundleVerification `
        -Directory $selfDirectory -Type harness-self-test `
        -RunId matrix-self -TaskId $taskId `
        -ExpectedSuites @('suite-a', 'suite-b') `
        -ExpectedFields @(
            [pscustomobject]@{ path = 'scriptsParsed'; value = 22 },
            [pscustomobject]@{
                path = 'numericObservation'
                value = 1.2505
                tolerance = 0.001
            })
    Assert-XpMatrixVerifier $selfCheck.valid `
        "valid self-test bundle failed: $($selfCheck.findings -join '; ')"

    $suitePath = Join-Path $selfDirectory 'suite.log'
    Add-Content -LiteralPath $suitePath -Value 'tampered'
    $tampered = Get-XpAcceptanceBundleVerification `
        -Directory $selfDirectory -Type harness-self-test `
        -RunId matrix-self -TaskId $taskId
    Assert-XpMatrixVerifier (-not $tampered.valid -and
        @($tampered.findings | Where-Object {
            $_ -like 'artifact * differs for suite.log'
        }).Count -ge 1) 'tampered manifest member was accepted'
    Write-XpMatrixFixtureText -Path $suitePath -Text "suite passed`n"

    $wrongField = Get-XpAcceptanceBundleVerification `
        -Directory $selfDirectory -Type harness-self-test `
        -RunId matrix-self -TaskId $taskId `
        -ExpectedFields @([pscustomobject]@{
            path = 'scriptsParsed'; value = 21
        })
    Assert-XpMatrixVerifier (-not $wrongField.valid -and
        @($wrongField.findings | Where-Object {
            $_ -like "*result path 'scriptsParsed' differs*"
        }).Count -eq 1) 'expected-field mismatch was accepted'
    $duplicateField = Get-XpAcceptanceBundleVerification `
        -Directory $selfDirectory -Type harness-self-test `
        -RunId matrix-self -TaskId $taskId `
        -ExpectedFields @(
            [pscustomobject]@{ path = 'scriptsParsed'; value = 22 },
            [pscustomobject]@{ path = 'scriptsParsed'; value = 22 })
    Assert-XpMatrixVerifier (-not $duplicateField.valid -and
        @($duplicateField.findings | Where-Object {
            $_ -like "*repeats expected result path 'scriptsParsed'"
        }).Count -eq 1) 'duplicate expected-field path was accepted'

    $installDirectory = Join-Path $testRoot 'install'
    $installContext = New-XpMatrixFixtureContext -Directory $installDirectory `
        -RunId 'matrix-install' -TaskId $taskId -Tier T2
    $package = [pscustomobject][ordered]@{
        valid = $true
        driverVer = '01/02/2026,1.0.0.9'
        certificate = [pscustomobject]@{ thumbprint = ('A' * 40) }
        catalogSignature = [pscustomobject]@{ signerThumbprint = ('A' * 40) }
        signerMatchesBundledCertificate = $true
        files = @([pscustomobject]@{ name = 'qxp.inf'; sha256 = ('B' * 64) })
        findings = @()
    }
    Write-XpMatrixFixtureText `
        -Path (Join-Path $installDirectory 'package-identity.json') `
        -Text (($package | ConvertTo-Json -Depth 8) + "`n")
    $installResult = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-run'
        taskId = $taskId
        runId = 'matrix-install'
        result = 'pass'
        harnessStatus = 'passed'
        teardownOk = $true
        launcher = [pscustomobject]@{
            phase = 'finalized'; finalized = $true; childExitCode = 0
        }
        package = $package
        install = [pscustomobject]@{
            guestReported = $true
            conflictingReports = $false
            matchesStagedPackage = $true
            driverVer = '01/02/2026,1.0.0.9'
            signerThumbprint = ('A' * 40)
        }
        d3dDriverProfile = [pscustomobject]@{
            status = 'absent'; valid = $false
        }
    }
    Complete-XpMatrixFixtureBundle $installContext $installResult
    $installCheck = Get-XpAcceptanceBundleVerification `
        -Directory $installDirectory -Type install `
        -RunId matrix-install -TaskId $taskId `
        -ExpectedDriverVer '01/02/2026,1.0.0.9' `
        -ExpectedSignerThumbprint ('A' * 40) `
        -ExpectedPackageFiles @([pscustomobject]@{
            name = 'qxp.inf'; sha256 = ('B' * 64)
        })
    Assert-XpMatrixVerifier $installCheck.valid `
        "valid install bundle failed: $($installCheck.findings -join '; ')"

    $profileDirectory = Join-Path $testRoot 'profile'
    $profileContext = New-XpMatrixFixtureContext -Directory $profileDirectory `
        -RunId 'matrix-profile' -TaskId $taskId -Tier T3-diagnostic
    $profileLog = New-XpMatrixProfileFixture
    $profile = ConvertFrom-XpD3dDriverProfile -Text $profileLog
    Assert-XpMatrixVerifier $profile.valid 'synthetic profile is invalid'
    Write-XpMatrixFixtureText -Path (Join-Path $profileDirectory 'driver.log') `
        -Text ($profileLog + "`r`n")
    Write-XpMatrixFixtureText -Path (Join-Path $profileDirectory 'console.log') `
        -Text (@(
            'FramePublicationProfile workload=MaxPayne durationUs=45000000 callbacks=30 fresh=20 duped=10 dupedPercent=33.33 freshFps=0.444 callbackFps=0.667 intervals=19 p50Us=100000 p95Us=200000 p99Us=250000 maxUs=300000 stalls50ms=10 stalls100ms=5 stalls250ms=1 stalls1000ms=0 valid=True',
            'D3dGamePerformance observedSeconds=45 draws=20 presents=3 clears=2 uploads=1 batches=2 maxDrawsPerBatch=12 averageDrawsPerBatch=10 fixedStateReuse=1 textureStageReuse=2 arrayStateReuse=3 drawsPerSecond=0.44 transportPresentsPerSecond=0.07',
            'D3dGameTransportOk=True context=True host=True draw=True present=True drawFailure=False dp2Failure=False surfaceFailure=False targetFailure=False primitiveSkip=False swapLimitFailure=False') -join "`n")
    Write-XpMatrixFixtureText `
        -Path (Join-Path $profileDirectory 'd3d-driver-profile.json') `
        -Text (($profile | ConvertTo-Json -Depth 12) + "`n")
    $profileResult = [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-acceptance-run'
        taskId = $taskId
        runId = 'matrix-profile'
        result = 'pass'
        harnessStatus = 'passed'
        teardownOk = $true
        capture = $false
        captureMode = 'capture-free'
        launcher = [pscustomobject]@{
            phase = 'finalized'; finalized = $true; childExitCode = 0
        }
        d3dDriverProfile = $profile
    }
    Complete-XpMatrixFixtureBundle $profileContext $profileResult
    $profileCheck = Get-XpAcceptanceBundleVerification `
        -Directory $profileDirectory -Type d3d-profile `
        -RunId matrix-profile -TaskId $taskId
    Assert-XpMatrixVerifier $profileCheck.valid `
        "valid profile bundle failed: $($profileCheck.findings -join '; ')"
    Write-XpMatrixFixtureText -Path (Join-Path $profileDirectory 'frame.png') `
        -Text 'not an image'
    $captured = Get-XpAcceptanceBundleVerification `
        -Directory $profileDirectory -Type d3d-profile `
        -RunId matrix-profile -TaskId $taskId
    Assert-XpMatrixVerifier (-not $captured.valid -and
        @($captured.findings | Where-Object {
            $_ -like 'capture-free bundle contains capture artifacts*'
        }).Count -eq 1) 'capture artifact was accepted'
    Remove-Item -LiteralPath (Join-Path $profileDirectory 'frame.png') -Force
    Write-XpMatrixFixtureText -Path (Join-Path $profileDirectory 'frame.bmp') `
        -Text 'not an image'
    $capturedBmp = Get-XpAcceptanceBundleVerification `
        -Directory $profileDirectory -Type d3d-profile `
        -RunId matrix-profile -TaskId $taskId
    Assert-XpMatrixVerifier (-not $capturedBmp.valid -and
        @($capturedBmp.findings | Where-Object {
            $_ -like 'capture-free bundle contains capture artifacts*'
        }).Count -eq 1) 'BMP capture artifact was accepted'
    Remove-Item -LiteralPath (Join-Path $profileDirectory 'frame.bmp') -Force

    $source = Get-XpMatrixFileIdentity -Path `
        (Join-Path $PSScriptRoot 'acceptance-matrix-verifier.ps1')
    $spec = [pscustomobject][ordered]@{
        schema = 1
        taskId = $taskId
        frozenFiles = @([pscustomobject]@{
            path = $source.path
            bytes = $source.bytes
            sha256 = $source.sha256
        })
        inputFiles = @()
        repositories = @()
        bundles = @([pscustomobject]@{
            type = 'harness-self-test'
            directory = $selfDirectory
            runId = 'matrix-self'
            expectedSuites = @('suite-a', 'suite-b')
            expectedFields = @([pscustomobject]@{
                path = 'scriptsParsed'; value = 22
            })
        })
    }
    $matrix = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier $matrix.valid `
        "valid matrix failed: $($matrix.findings -join '; ')"
    $specPath = Join-Path $testRoot 'matrix-spec.json'
    $outputPath = Join-Path $testRoot 'matrix-verification.json'
    Write-XpMatrixFixtureText -Path $specPath `
        -Text (($spec | ConvertTo-Json -Depth 12) + "`n")
    $entry = Join-Path $PSScriptRoot 'verify-xp-acceptance-matrix.ps1'
    $entryOutput = @(& powershell.exe -NoLogo -NoProfile -NonInteractive `
        -ExecutionPolicy Bypass -File $entry -SpecPath $specPath `
        -OutputPath $outputPath 2>&1)
    $entryExit = $LASTEXITCODE
    Assert-XpMatrixVerifier ($entryExit -eq 0 -and
        (Test-Path -LiteralPath $outputPath -PathType Leaf)) `
        "matrix verifier entry point failed: $($entryOutput -join '; ')"
    $entryResult = Get-Content -Raw -LiteralPath $outputPath |
        ConvertFrom-Json
    Assert-XpMatrixVerifier ($entryResult.valid -and
        $entryResult.verdict -ceq 'PASS') `
        'matrix verifier entry point wrote a non-pass result'
    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $overwriteOutput = @(& powershell.exe -NoLogo -NoProfile `
            -NonInteractive -ExecutionPolicy Bypass -File $entry `
            -SpecPath $specPath -OutputPath $outputPath 2>&1)
        $overwriteExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    Assert-XpMatrixVerifier ($overwriteExit -ne 0 -and
        ($overwriteOutput -join "`n") -like
            '*Refusing to overwrite matrix verification output*') `
        'matrix verifier entry point overwrote existing evidence'
    $spec.frozenFiles[0].sha256 = ('0' * 64)
    $stale = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier (-not $stale.valid -and
        @($stale.findings | Where-Object {
            $_ -like 'frozen file identity differs:*'
        }).Count -eq 1) 'stale frozen source was accepted'
    $spec.frozenFiles[0].sha256 = $source.sha256

    # A spec that pins statusContentSha256 catches an edit inside an
    # already-dirty file that porcelain status cannot see; a spec without
    # it keeps verifying on status alone.
    $repoRoot = Join-Path $testRoot 'content-repo'
    [void](New-Item -ItemType Directory -Path $repoRoot)
    & git -C $repoRoot init -q
    & git -C $repoRoot config user.email 'test@example.invalid'
    & git -C $repoRoot config user.name 'Harness Test'
    [IO.File]::WriteAllText((Join-Path $repoRoot 'tracked.txt'), "original`n")
    & git -C $repoRoot add -A
    & git -C $repoRoot -c commit.gpgsign=false commit -q -m seed
    Assert-XpMatrixVerifier ($LASTEXITCODE -eq 0) 'content repo setup failed'
    [IO.File]::WriteAllText((Join-Path $repoRoot 'tracked.txt'), "dirty v1`n")
    $repoIdentity = Get-XpMatrixRepositoryIdentity -Repository (
        [pscustomobject]@{ name = 'content-repo'; kind = 'windows';
            path = $repoRoot })
    $spec.repositories = @([pscustomobject][ordered]@{
        name = 'content-repo'
        kind = 'windows'
        path = $repoRoot
        branch = $repoIdentity.branch
        commit = $repoIdentity.commit
        statusSha256 = $repoIdentity.statusSha256
        statusContentSha256 = $repoIdentity.statusContentSha256
    })
    $repoMatch = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier $repoMatch.valid `
        "matching repository content identity failed: $($repoMatch.findings -join '; ')"
    [IO.File]::WriteAllText((Join-Path $repoRoot 'tracked.txt'), "dirty v2`n")
    $repoStale = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier (-not $repoStale.valid -and
        @($repoStale.findings | Where-Object {
            $_ -eq 'repository identity differs: content-repo'
        }).Count -eq 1) `
        'edited dirty-file content passed a content-pinned spec'
    $spec.repositories = @([pscustomobject][ordered]@{
        name = 'content-repo'
        kind = 'windows'
        path = $repoRoot
        branch = $repoIdentity.branch
        commit = $repoIdentity.commit
        statusSha256 = $repoIdentity.statusSha256
    })
    $repoLegacy = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier $repoLegacy.valid `
        "status-only repository spec failed: $($repoLegacy.findings -join '; ')"
    $spec.repositories = @()

    $spec.bundles = @()
    $empty = Get-XpAcceptanceMatrixVerification -Spec $spec
    Assert-XpMatrixVerifier (-not $empty.valid -and
        @($empty.findings | Where-Object {
            $_ -eq 'matrix spec contains no bundles'
        }).Count -eq 1) 'empty matrix was accepted'

    Write-Output 'Acceptance matrix verifier tests passed'
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force `
        -ErrorAction SilentlyContinue
}
