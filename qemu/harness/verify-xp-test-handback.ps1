param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$StartStatePath,
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputPath,
    [string[]]$RunDirectory = @(),
    [string[]]$T0Directory = @(),
    [string]$PackageDirectory
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'acceptance-matrix-verifier.ps1')
. (Join-Path $PSScriptRoot 'package-identity.ps1')

# Records one named handback check. A failed check carries the handoff
# protocol's stop-token class so the caller can classify without parsing prose.
$script:checks = [Collections.Generic.List[object]]::new()
function Add-HandbackCheck {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$FailureClass,
        [string]$Detail
    )
    $script:checks.Add([pscustomobject][ordered]@{
        name = $Name
        ok = $Ok
        failureClass = if ($Ok) { $null } else { $FailureClass }
        detail = $Detail
    })
}

$outputFile = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $outputFile
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw "Handback output parent does not exist: $outputParent"
}
if (Test-Path -LiteralPath $outputFile) {
    throw "Refusing to overwrite handback output: $outputFile"
}

$startStateFile = [IO.Path]::GetFullPath($StartStatePath)
try {
    $startState = Get-Content -Raw -LiteralPath $startStateFile |
        ConvertFrom-Json
} catch {
    throw "Start state is not valid JSON: $($_.Exception.Message)"
}
if ($startState.schema -ne 1 -or
        $startState.kind -ne 'xpdriver-test-preflight') {
    throw "Start state is not an xpdriver-test-preflight record: $startStateFile"
}
$startEnv = $startState.environment

# A failed preflight means the window began unsafely; nothing measured inside
# it can hand back cleanly regardless of what the later checks find.
$preflightFailedDetail = "testing began from a failed preflight " +
    "(result=$($startState.result)); rerun verify-xp-test-preflight.ps1 " +
    'and require a pass'
Add-HandbackCheck 'preflightPassed' ($startState.result -eq 'pass') `
    'PREFLIGHT_FAILED' $preflightFailedDetail

# The observation window opened before the preflight's own snapshots, so a
# process started mid-preflight cannot fall between the records.
$windowStart = if ($startState.observationStarted) {
    [DateTimeOffset]::Parse($startState.observationStarted)
} else {
    [DateTimeOffset]::Parse($startState.timestamp)
}

# Base disk identity must be byte-identical to the preflight record. The base
# is read-only evidence; a changed size or last-write time means something
# opened it for writing and every boot built on it is no longer reproducible.
$startBase = $startEnv.baseImage
$endBase = [pscustomobject][ordered]@{
    path = [string]$startBase.path
    exists = $false
    bytes = $null
    lastWriteTimeUtc = $null
    sha256 = $null
}
if (Test-Path -LiteralPath ([string]$startBase.path) -PathType Leaf) {
    $baseItem = Get-Item -LiteralPath ([string]$startBase.path)
    $endBase.exists = $true
    $endBase.bytes = $baseItem.Length
    $endBase.lastWriteTimeUtc = $baseItem.LastWriteTimeUtc.ToString('o')
    if ($startBase.sha256) {
        $endBase.sha256 = (Get-FileHash -LiteralPath ([string]$startBase.path) `
            -Algorithm SHA256).Hash
    }
}
$baseSame = $endBase.exists -and
    ([uint64]$endBase.bytes -eq [uint64]$startBase.bytes) -and
    ([string]$endBase.lastWriteTimeUtc -ceq [string]$startBase.lastWriteTimeUtc) -and
    (-not $startBase.sha256 -or
        [string]$endBase.sha256 -ceq [string]$startBase.sha256)
Add-HandbackCheck 'baseImageUnchanged' $baseSame 'BASE_IMAGE_MUTATED' `
    ("base disk identity changed during the test window; start=" +
     "$($startBase.bytes)@$($startBase.lastWriteTimeUtc) end=" +
     "$($endBase.bytes)@$($endBase.lastWriteTimeUtc)")

# No VM process started inside the test window may remain. Creation time
# alone decides membership: a process the preflight snapshot happened to
# capture still belongs to this window if it started after the window
# opened, so snapshot membership is recorded evidence, never an exemption.
$currentProcesses = @(Get-XpTestVmProcesses)
$newProcesses = @($currentProcesses | Where-Object {
    $_.started -and
    ([DateTimeOffset]::Parse($_.started) -gt $windowStart)
})
$newProcessDetail = ($newProcesses | ForEach-Object {
    "PID $($_.pid) ($($_.name)) started $($_.started)"
}) -join '; '
Add-HandbackCheck 'processesReleased' ($newProcesses.Count -eq 0) `
    'CLEANUP_INCOMPLETE' `
    "VM process started during the test window is still running: $newProcessDetail"

# The assigned QMP port must be free again. The port comes only from the
# start state; the audit cannot be pointed at a different, conveniently free
# port.
$effectivePort = if ($startEnv.qmpPort -and $startEnv.qmpPort.effective) {
    [int]$startEnv.qmpPort.effective
} else { $null }
if ($effectivePort) {
    $portFree = Test-XpTestPortFree -Port $effectivePort
    Add-HandbackCheck 'qmpPortFree' $portFree 'CLEANUP_INCOMPLETE' `
        "QMP port $effectivePort is still bound; identify the owner before returning"
}

# The shared artifact lease recorded at preflight must be free. A lease held
# by a process started inside the window is this task's unfinished run; any
# other holder is a foreign owner.
$lockFull = [string]$startEnv.lease.path
$lease = Test-XpAcceptanceArtifactLease -LockPath $lockFull
$leaseClass = $null
$leaseDetail = $null
if (-not $lease.free) {
    $leaseClass = 'VM_OWNERSHIP_CONFLICT'
    $leaseDetail = "lease still held: $($lease.owner)"
    try {
        $owner = $lease.owner | ConvertFrom-Json
        $ownerProcess = Get-Process -Id ([int]$owner.harnessPid) `
            -ErrorAction SilentlyContinue
        if ($ownerProcess -and
                $ownerProcess.StartTime -gt $windowStart.LocalDateTime) {
            $leaseClass = 'CLEANUP_INCOMPLETE'
            $leaseDetail = "lease held by this task's own run " +
                "(runId=$($owner.runId) harnessPid=$($owner.harnessPid)); " +
                'wait for it or stop the verified process before returning'
        }
    } catch { }
}
Add-HandbackCheck 'leaseReleased' ([bool]$lease.free) $leaseClass $leaseDetail

# Repositories must be identical to the preflight record, including the
# contents of every already-dirty path: an edited dirty file keeps its status
# line, so the status text and the per-file content identity are both
# required.
$repositoryAudits = [Collections.Generic.List[object]]::new()
foreach ($startRepo in @($startEnv.repositories |
        Where-Object { $_.exitCode -eq 0 })) {
    $endRepo = Get-XpMatrixRepositoryIdentity -Repository (
        [pscustomobject]@{ name = [string]$startRepo.name;
            kind = [string]$startRepo.kind; path = [string]$startRepo.path })
    $statusSame = [string]$endRepo.statusSha256 -ceq
        [string]$startRepo.statusSha256
    $contentSame = $true
    $contentComparable = $true
    if ($startRepo.statusContentSha256) {
        if ($endRepo.statusContentSha256) {
            $contentSame = [string]$endRepo.statusContentSha256 -ceq
                [string]$startRepo.statusContentSha256
        } else {
            $contentSame = $false
            $contentComparable = $false
        }
    }
    # The staged index is compared when the start state recorded it; an MM
    # path can change its staged blob while status and worktree stay same.
    $cachedSame = $true
    if ($startRepo.PSObject.Properties['cachedDiffSha256'] -and
            $startRepo.cachedDiffSha256) {
        $cachedSame = [string]$endRepo.cachedDiffSha256 -ceq
            [string]$startRepo.cachedDiffSha256
    }
    $unchanged = $endRepo.exitCode -eq 0 -and
        ([string]$endRepo.branch -ceq [string]$startRepo.branch) -and
        ([string]$endRepo.commit -ceq [string]$startRepo.commit) -and
        $statusSame -and $contentSame -and $cachedSame
    $detail = $null
    if (-not $unchanged) {
        if ($endRepo.exitCode -ne 0) {
            $detail = "git identity now fails (exit $($endRepo.exitCode))"
        } elseif ([string]$endRepo.branch -cne [string]$startRepo.branch -or
                [string]$endRepo.commit -cne [string]$startRepo.commit) {
            $detail = "start=$($startRepo.branch)@$($startRepo.commit) " +
                "end=$($endRepo.branch)@$($endRepo.commit)"
        } else {
            $detailParts = [Collections.Generic.List[string]]::new()
            if (-not $statusSame) {
                $startLines = @($startRepo.statusLines |
                    ForEach-Object { [string]$_ })
                $endLines = @($endRepo.statusLines |
                    ForEach-Object { [string]$_ })
                $added = @($endLines | Where-Object {
                    $startLines -notcontains $_ })
                $removed = @($startLines | Where-Object {
                    $endLines -notcontains $_ })
                $detailParts.Add(
                    "status added=[$($added -join ' | ')] " +
                    "removed=[$($removed -join ' | ')]")
            }
            if (-not $cachedSame) {
                $detailParts.Add('staged index content changed')
            }
            if (-not $contentSame) {
                if (-not $contentComparable) {
                    $detailParts.Add('content identity unavailable at handback')
                } else {
                    $startFiles = @($startRepo.statusFiles)
                    $endFiles = @($endRepo.statusFiles)
                    $changed = [Collections.Generic.List[string]]::new()
                    foreach ($endFile in $endFiles) {
                        $startFile = @($startFiles | Where-Object {
                            [string]$_.path -ceq [string]$endFile.path })
                        if ($startFile.Count -eq 1 -and (
                                [string]$startFile[0].xy -cne `
                                    [string]$endFile.xy -or
                                $startFile[0].exists -ne $endFile.exists -or
                                $startFile[0].hashError -ne `
                                    $endFile.hashError -or
                                [string]$startFile[0].sha256 -cne `
                                    [string]$endFile.sha256)) {
                            $changed.Add([string]$endFile.path)
                        }
                    }
                    $detailParts.Add(
                        "content changed=[$($changed -join ', ')]")
                }
            }
            $detail = $detailParts -join ' '
        }
    }
    $repositoryAudits.Add([pscustomobject][ordered]@{
        name = [string]$startRepo.name
        unchanged = [bool]$unchanged
        detail = $detail
    })
    Add-HandbackCheck "repositoryUnchanged:$($startRepo.name)" $unchanged `
        'WORKSPACE_MUTATED' "$($startRepo.name): $detail"
}

# Canonical run bundles. Every supplied directory must be a complete
# finalized bundle: a missing result or manifest is incomplete evidence, not
# a passing gate. The shared bundle audit proves document shape, membership,
# and byte identity; the checks below add the finalized VM-run contract
# without requiring a passing verdict, because a failed run that hands back
# honest evidence is still a valid handback.
$bundleAudits = [Collections.Generic.List[object]]::new()
$seenBundleRunIds = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($directory in @($RunDirectory)) {
    $audit = Get-XpAcceptanceBundleAudit -Directory $directory
    $full = $audit.directory
    $bundleDetail = [Collections.Generic.List[string]]::new(
        [string[]]@($audit.findings))
    $result = $audit.result
    if ($result) {
        if ([string]$result.kind -cne 'xpdriver-acceptance-run') {
            $bundleDetail.Add(
                "result kind '$($result.kind)' is not xpdriver-acceptance-run")
        }
        # Every supplied bundle must belong to this handback's task, its
        # manifest must name the same run as its result, and no run ID may
        # appear twice - a mis-wired or duplicated bundle is an accident
        # this audit exists to catch.
        if ([string]$result.taskId -cne [string]$startState.taskId) {
            $bundleDetail.Add("result taskId '$($result.taskId)' does not " +
                "match the preflight taskId '$($startState.taskId)'")
        }
        $resultRunId = [string]$result.runId
        if ($audit.manifest -and
                [string]$audit.manifest.runId -cne $resultRunId) {
            $bundleDetail.Add("manifest runId '$($audit.manifest.runId)' " +
                "does not match result runId '$resultRunId'")
        }
        if ($resultRunId -and -not $seenBundleRunIds.Add($resultRunId)) {
            $bundleDetail.Add(
                "duplicate run ID '$resultRunId' across supplied bundles")
        }
        $launcher = $null
        if ($result.PSObject.Properties['launcher']) {
            $launcher = $result.launcher
        }
        if (-not $launcher) {
            $bundleDetail.Add('result lacks finalized launcher identity')
        } else {
            if ($launcher.phase -ne 'finalized' -or
                    -not ($launcher.PSObject.Properties['finalized'] -and
                        $launcher.finalized)) {
                $bundleDetail.Add("launcher is not finalized (phase=$($launcher.phase))")
            } else {
                foreach ($required in @('console.log', 'launcher-command.json')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $full $required) `
                            -PathType Leaf)) {
                        $bundleDetail.Add("finalized bundle lacks $required")
                    }
                }
            }
            if (-not ($launcher.PSObject.Properties['childExitCode'] -and
                    $null -ne $launcher.childExitCode)) {
                $bundleDetail.Add('launcher lacks child exit code')
            } else {
                $childExit = [int]$launcher.childExitCode
                $agrees = $true
                if ($result.PSObject.Properties['exitCode'] -and
                        $null -ne $result.exitCode) {
                    $agrees = ($childExit -eq [int]$result.exitCode)
                } elseif ($result.PSObject.Properties['result']) {
                    $agrees = (($childExit -eq 0) -eq ($result.result -eq 'pass'))
                }
                if (-not $agrees) {
                    $bundleDetail.Add("launcher child exit $childExit disagrees with the run verdict")
                }
            }
        }
    }
    $bundleOk = $bundleDetail.Count -eq 0
    $bundleAudits.Add([pscustomobject][ordered]@{
        directory = $full
        ok = [bool]$bundleOk
        detail = ($bundleDetail -join '; ')
    })
    Add-HandbackCheck "runBundle:$(Split-Path -Leaf $full)" $bundleOk `
        'EVIDENCE_INVALID' ($bundleDetail -join '; ')
}

# T0 aggregate bundles are written directly by the self-test entry point
# without launcher finalization: they carry the same manifest contract but no
# launcher identity. Their shape is checked against the self-test kind rather
# than the canonical VM-run contract.
$t0Audits = [Collections.Generic.List[object]]::new()
foreach ($directory in @($T0Directory)) {
    $audit = Get-XpAcceptanceBundleAudit -Directory $directory
    $full = $audit.directory
    $bundleDetail = [Collections.Generic.List[string]]::new(
        [string[]]@($audit.findings))
    $result = $audit.result
    if ($result -and
            $result.kind -ne 'xpdriver-acceptance-harness-self-test') {
        $bundleDetail.Add('result is not a schema-1 self-test record')
    }
    $bundleOk = $bundleDetail.Count -eq 0
    $t0Audits.Add([pscustomobject][ordered]@{
        directory = $full
        ok = [bool]$bundleOk
        detail = ($bundleDetail -join '; ')
    })
    Add-HandbackCheck "t0Bundle:$(Split-Path -Leaf $full)" $bundleOk `
        'EVIDENCE_INVALID' ($bundleDetail -join '; ')
}

# The signed package is retained evidence: record its exact membership and
# require a valid identity — existence alone would bless any directory of
# stub files as the package that was installed.
$packageAudit = $null
if ($PackageDirectory) {
    $packageFull = [IO.Path]::GetFullPath($PackageDirectory)
    $packageExists = Test-Path -LiteralPath $packageFull -PathType Container
    $packageFiles = @()
    $packageIdentity = $null
    $packageValid = $false
    $packageDetail = "package directory not found: $packageFull"
    if ($packageExists) {
        $packageFiles = @(Get-ChildItem -LiteralPath $packageFull -File |
            Sort-Object Name | ForEach-Object {
                Get-XpMatrixFileIdentity -Path $_.FullName
            })
        try {
            $packageIdentity = Get-XpAcceptancePackageIdentity `
                -Directory $packageFull
            $packageValid = [bool]$packageIdentity.valid
            $packageDetail = 'package identity is invalid: ' +
                "$(@($packageIdentity.findings) -join '; ')"
        } catch {
            $packageDetail = 'package identity could not be computed: ' +
                $_.Exception.Message
        }
    }
    $packageAudit = [pscustomobject][ordered]@{
        path = $packageFull
        exists = $packageExists
        valid = $packageValid
        driverVer = if ($packageIdentity) {
            $packageIdentity.driverVer
        } else { $null }
        findings = if ($packageIdentity) {
            @($packageIdentity.findings)
        } else { @() }
        files = $packageFiles
    }
    Add-HandbackCheck 'packageDirectory' `
        ($packageExists -and $packageValid) 'EVIDENCE_INVALID' $packageDetail
}

$failureCount = @($script:checks | Where-Object { -not $_.ok }).Count
$handback = [pscustomobject][ordered]@{
    schema = 1
    kind = 'xpdriver-test-handback'
    taskId = [string]$startState.taskId
    timestamp = Get-Date -Format o
    startState = $startStateFile
    result = if ($failureCount) { 'fail' } else { 'pass' }
    checks = @($script:checks)
    audit = [pscustomobject][ordered]@{
        windowStart = $windowStart.ToString('o')
        base = [pscustomobject][ordered]@{
            start = $startBase
            end = $endBase
        }
        processes = [pscustomobject][ordered]@{
            atStart = @($startEnv.processes)
            current = $currentProcesses
            newSinceStart = @($newProcesses)
        }
        qmpPort = $effectivePort
        lease = $lease
        repositories = @($repositoryAudits)
        runBundles = @($bundleAudits)
        t0Bundles = @($t0Audits)
        package = $packageAudit
    }
}
Write-XpAcceptanceAtomicText -Path $outputFile `
    -Text (($handback | ConvertTo-Json -Depth 12) + [Environment]::NewLine)
Write-Output "TestHandback taskId=$($handback.taskId) result=$($handback.result) checks=$($script:checks.Count) failures=$failureCount output=$outputFile"
foreach ($failed in @($script:checks | Where-Object { -not $_.ok })) {
    $detail = [string]$failed.detail
    $detail = $detail.Replace("`r", ' ').Replace("`n", ' ')
    Write-Output "TestHandbackFailure name=$($failed.name) class=$($failed.failureClass) detail=$detail"
}
if ($failureCount) { exit 1 }
