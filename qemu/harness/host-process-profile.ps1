function New-XpHostProcessCpuSnapshotFailure {
    param(
        [string]$ErrorMessage,
        [DateTime]$CapturedAtUtc = [DateTime]::UtcNow,
        [long]$MonotonicTimestamp =
            [Diagnostics.Stopwatch]::GetTimestamp(),
        [long]$MonotonicFrequency = [Diagnostics.Stopwatch]::Frequency,
        [double]$CaptureDurationMilliseconds = 0
    )

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-host-process-cpu-snapshot'
        valid = $false
        error = $ErrorMessage
        capturedAtUtc = $CapturedAtUtc.ToString('o')
        monotonicTimestamp = $MonotonicTimestamp
        monotonicFrequency = $MonotonicFrequency
        captureDurationMilliseconds = [Math]::Round(
            $CaptureDurationMilliseconds, 3)
        processId = $null
        processStartAtUtc = $null
        totalCpuTicks = $null
        userCpuTicks = $null
        kernelCpuTicks = $null
        threadEnumerationComplete = $false
        threadFailures = 0
        threads = @()
    }
}

function New-XpHostProcessCpuIncompleteProfile {
    param(
        [string[]]$Findings,
        [psobject]$Start,
        [psobject]$Stop,
        [ValidateRange(1, 4096)]
        [int]$ProcessorCount = [Environment]::ProcessorCount
    )

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-host-process-cpu-profile'
        status = 'incomplete'
        valid = $false
        findings = @($Findings | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_)
        })
        processorCount = $ProcessorCount
        processId = if ($Start) { $Start.processId } else { $null }
        processStartAtUtc = if ($Start) {
            $Start.processStartAtUtc
        } else { $null }
        window = $null
        process = $null
        threads = $null
        snapshots = [pscustomobject][ordered]@{
            start = $Start
            stop = $Stop
        }
    }
}

function Get-XpHostProcessCpuSnapshot {
    param(
        [Diagnostics.Process]$Process
    )

    $capturedAtUtc = [DateTime]::UtcNow
    $monotonicTimestamp = [Diagnostics.Stopwatch]::GetTimestamp()
    $monotonicFrequency = [Diagnostics.Stopwatch]::Frequency
    $captureStopwatch = [Diagnostics.Stopwatch]::StartNew()
    $threads = [Collections.Generic.List[object]]::new()
    $threadFailures = 0
    $threadEnumerationComplete = $true

    if (-not $Process) {
        $captureStopwatch.Stop()
        return New-XpHostProcessCpuSnapshotFailure `
            -ErrorMessage 'host process is unavailable' `
            -CapturedAtUtc $capturedAtUtc `
            -MonotonicTimestamp $monotonicTimestamp `
            -MonotonicFrequency $monotonicFrequency `
            -CaptureDurationMilliseconds `
                $captureStopwatch.Elapsed.TotalMilliseconds
    }

    try {
        $Process.Refresh()
        $processId = $Process.Id
        $processStartAtUtc = $Process.StartTime.ToUniversalTime().ToString('o')
        # These managed properties are sampled separately. The derived profile
        # records their disagreement and uses the boundary capture duration to
        # distinguish expected sampling skew from impossible evidence.
        $totalCpuTicks = $Process.TotalProcessorTime.Ticks
        $userCpuTicks = $Process.UserProcessorTime.Ticks
        $kernelCpuTicks = $Process.PrivilegedProcessorTime.Ticks
    } catch {
        $captureStopwatch.Stop()
        return New-XpHostProcessCpuSnapshotFailure `
            -ErrorMessage $_.Exception.Message `
            -CapturedAtUtc $capturedAtUtc `
            -MonotonicTimestamp $monotonicTimestamp `
            -MonotonicFrequency $monotonicFrequency `
            -CaptureDurationMilliseconds `
                $captureStopwatch.Elapsed.TotalMilliseconds
    }

    try {
        foreach ($thread in @($Process.Threads)) {
            try {
                $threads.Add([pscustomobject][ordered]@{
                    id = $thread.Id
                    startAtUtc = $thread.StartTime.ToUniversalTime().ToString('o')
                    totalCpuTicks = $thread.TotalProcessorTime.Ticks
                    userCpuTicks = $thread.UserProcessorTime.Ticks
                    kernelCpuTicks = $thread.PrivilegedProcessorTime.Ticks
                })
            } catch {
                ++$threadFailures
            } finally {
                if ($thread -is [IDisposable]) {
                    try { $thread.Dispose() } catch { }
                }
            }
        }
    } catch {
        $threadEnumerationComplete = $false
        ++$threadFailures
    }
    $captureStopwatch.Stop()

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-host-process-cpu-snapshot'
        valid = $true
        error = $null
        capturedAtUtc = $capturedAtUtc.ToString('o')
        monotonicTimestamp = $monotonicTimestamp
        monotonicFrequency = $monotonicFrequency
        captureDurationMilliseconds = [Math]::Round(
            $captureStopwatch.Elapsed.TotalMilliseconds, 3)
        processId = $processId
        processStartAtUtc = $processStartAtUtc
        totalCpuTicks = $totalCpuTicks
        userCpuTicks = $userCpuTicks
        kernelCpuTicks = $kernelCpuTicks
        threadEnumerationComplete = $threadEnumerationComplete
        threadFailures = $threadFailures
        threads = @($threads)
    }
}

function New-XpHostProcessCpuProfile {
    param(
        [psobject]$Start,
        [psobject]$Stop,
        [ValidateRange(1, 4096)]
        [int]$ProcessorCount = [Environment]::ProcessorCount,
        [ValidateRange(1, 128)]
        [int]$TopThreadCount = 12
    )

    if (-not $Start) {
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings 'measurement-start CPU snapshot is absent' `
            -Start $Start -Stop $Stop -ProcessorCount $ProcessorCount
    }
    if (-not $Stop) {
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings 'measurement-stop CPU snapshot is absent' `
            -Start $Start -Stop $Stop -ProcessorCount $ProcessorCount
    }
    if (-not $Start.valid -or -not $Stop.valid) {
        $invalidFindings = [Collections.Generic.List[string]]::new()
        if (-not $Start.valid) {
            $invalidFindings.Add(
                "measurement-start CPU snapshot is invalid: $($Start.error)")
        }
        if (-not $Stop.valid) {
            $invalidFindings.Add(
                "measurement-stop CPU snapshot is invalid: $($Stop.error)")
        }
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings @($invalidFindings) -Start $Start -Stop $Stop `
            -ProcessorCount $ProcessorCount
    }
    if ($Start.processId -ne $Stop.processId -or
        $Start.processStartAtUtc -ne $Stop.processStartAtUtc) {
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings `
                'host process identity changed during the measurement window' `
            -Start $Start -Stop $Stop -ProcessorCount $ProcessorCount
    }
    if ($Start.monotonicFrequency -le 0 -or
        $Start.monotonicFrequency -ne $Stop.monotonicFrequency) {
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings 'monotonic timer frequency is invalid or changed' `
            -Start $Start -Stop $Stop -ProcessorCount $ProcessorCount
    }

    $wallTicks = [long]$Stop.monotonicTimestamp -
        [long]$Start.monotonicTimestamp
    $totalCpuTicks = [long]$Stop.totalCpuTicks -
        [long]$Start.totalCpuTicks
    $userCpuTicks = [long]$Stop.userCpuTicks - [long]$Start.userCpuTicks
    $kernelCpuTicks = [long]$Stop.kernelCpuTicks -
        [long]$Start.kernelCpuTicks
    if ($wallTicks -le 0 -or $totalCpuTicks -lt 0 -or
        $userCpuTicks -lt 0 -or $kernelCpuTicks -lt 0) {
        return New-XpHostProcessCpuIncompleteProfile `
            -Findings 'host process CPU or monotonic time moved backwards' `
            -Start $Start -Stop $Stop -ProcessorCount $ProcessorCount
    }

    $findings = [Collections.Generic.List[string]]::new()
    $wallSeconds = $wallTicks / [double]$Start.monotonicFrequency
    $totalCpuSeconds = $totalCpuTicks / [double][TimeSpan]::TicksPerSecond
    $userCpuSeconds = $userCpuTicks / [double][TimeSpan]::TicksPerSecond
    $kernelCpuSeconds = $kernelCpuTicks /
        [double][TimeSpan]::TicksPerSecond
    $breakdownSkewTicks = $totalCpuTicks -
        ($userCpuTicks + $kernelCpuTicks)
    $breakdownSkewSeconds = $breakdownSkewTicks /
        [double][TimeSpan]::TicksPerSecond
    $captureSkewSeconds = (
        [double]$Start.captureDurationMilliseconds +
        [double]$Stop.captureDurationMilliseconds) / 1000.0
    $counterSkewToleranceSeconds = [Math]::Max(0.05, $captureSkewSeconds)
    $counterSkewToleranceTicks = [long][Math]::Ceiling(
        $counterSkewToleranceSeconds * [TimeSpan]::TicksPerSecond)
    if ([Math]::Abs($breakdownSkewTicks) -gt $counterSkewToleranceTicks) {
        $findings.Add(
            'process total CPU differs from user plus kernel beyond sampling tolerance')
    }
    $averageUtilizedCores = if ($wallSeconds -gt 0) {
        $totalCpuSeconds / $wallSeconds
    } else { 0.0 }
    $hostCapacityPercent = 100.0 * $averageUtilizedCores / $ProcessorCount
    if ($averageUtilizedCores -gt $ProcessorCount +
            ($counterSkewToleranceSeconds / $wallSeconds)) {
        $findings.Add(
            'process CPU exceeds host logical-processor capacity beyond sampling tolerance')
    }

    $startThreads = @{}
    foreach ($thread in @($Start.threads)) {
        if ($null -eq $thread.id -or
            [string]::IsNullOrWhiteSpace([string]$thread.startAtUtc)) {
            continue
        }
        $key = '{0}|{1}' -f $thread.id, $thread.startAtUtc
        $startThreads[$key] = $thread
    }
    $stopThreads = @{}
    foreach ($thread in @($Stop.threads)) {
        if ($null -eq $thread.id -or
            [string]::IsNullOrWhiteSpace([string]$thread.startAtUtc)) {
            continue
        }
        $key = '{0}|{1}' -f $thread.id, $thread.startAtUtc
        $stopThreads[$key] = $thread
    }

    $threadDeltas = [Collections.Generic.List[object]]::new()
    $stableThreadCpuTicks = [long]0
    foreach ($key in @($stopThreads.Keys)) {
        if (-not $startThreads.ContainsKey($key)) { continue }
        $startThread = $startThreads[$key]
        $stopThread = $stopThreads[$key]
        $threadTotalTicks = [long]$stopThread.totalCpuTicks -
            [long]$startThread.totalCpuTicks
        $threadUserTicks = [long]$stopThread.userCpuTicks -
            [long]$startThread.userCpuTicks
        $threadKernelTicks = [long]$stopThread.kernelCpuTicks -
            [long]$startThread.kernelCpuTicks
        if ($threadTotalTicks -lt 0 -or $threadUserTicks -lt 0 -or
            $threadKernelTicks -lt 0) {
            $findings.Add(
                "stable thread $($stopThread.id) CPU time moved backwards")
            continue
        }
        $threadBreakdownSkewTicks = $threadTotalTicks -
            ($threadUserTicks + $threadKernelTicks)
        if ([Math]::Abs($threadBreakdownSkewTicks) -gt
            $counterSkewToleranceTicks) {
            $findings.Add(
                "stable thread $($stopThread.id) total CPU differs from user plus kernel beyond sampling tolerance")
        }
        $stableThreadCpuTicks += $threadTotalTicks
        $threadDeltas.Add([pscustomobject][ordered]@{
            id = $stopThread.id
            startAtUtc = $stopThread.startAtUtc
            totalCpuSeconds = [Math]::Round(
                $threadTotalTicks / [double][TimeSpan]::TicksPerSecond, 6)
            userCpuSeconds = [Math]::Round(
                $threadUserTicks / [double][TimeSpan]::TicksPerSecond, 6)
            kernelCpuSeconds = [Math]::Round(
                $threadKernelTicks / [double][TimeSpan]::TicksPerSecond, 6)
            breakdownSkewSeconds = [Math]::Round(
                $threadBreakdownSkewTicks /
                    [double][TimeSpan]::TicksPerSecond, 6)
            shareOfProcessCpuPercent = if ($totalCpuTicks -gt 0) {
                [Math]::Round(100.0 * $threadTotalTicks / $totalCpuTicks, 3)
            } else { 0.0 }
        })
    }
    $topThreads = @($threadDeltas | Sort-Object totalCpuSeconds -Descending |
        Select-Object -First $TopThreadCount)
    $stableCoveragePercent = if ($totalCpuTicks -gt 0) {
        100.0 * $stableThreadCpuTicks / $totalCpuTicks
    } else { 0.0 }
    if (($totalCpuTicks -eq 0 -and $stableThreadCpuTicks -gt 0) -or
        $stableThreadCpuTicks -gt
            $totalCpuTicks + $counterSkewToleranceTicks) {
        $findings.Add(
            'stable thread CPU exceeds process CPU beyond sampling tolerance')
    }
    if (-not $Start.threadEnumerationComplete -or
        -not $Stop.threadEnumerationComplete -or
        $Start.threadFailures -or $Stop.threadFailures) {
        $findings.Add('one or both thread snapshots are incomplete')
    }

    [pscustomobject][ordered]@{
        schema = 1
        kind = 'xpdriver-host-process-cpu-profile'
        status = if ($findings.Count) { 'valid-with-findings' } else { 'valid' }
        valid = $true
        findings = @($findings)
        processorCount = $ProcessorCount
        processId = $Start.processId
        processStartAtUtc = $Start.processStartAtUtc
        window = [pscustomobject][ordered]@{
            startAtUtc = $Start.capturedAtUtc
            stopAtUtc = $Stop.capturedAtUtc
            wallSeconds = [Math]::Round($wallSeconds, 6)
            startSnapshotDurationMilliseconds =
                $Start.captureDurationMilliseconds
            stopSnapshotDurationMilliseconds =
                $Stop.captureDurationMilliseconds
            counterSkewToleranceSeconds = [Math]::Round(
                $counterSkewToleranceSeconds, 6)
        }
        process = [pscustomobject][ordered]@{
            totalCpuSeconds = [Math]::Round($totalCpuSeconds, 6)
            userCpuSeconds = [Math]::Round($userCpuSeconds, 6)
            kernelCpuSeconds = [Math]::Round($kernelCpuSeconds, 6)
            breakdownSkewSeconds = [Math]::Round(
                $breakdownSkewSeconds, 6)
            averageUtilizedCores = [Math]::Round($averageUtilizedCores, 6)
            hostCapacityPercent = [Math]::Round($hostCapacityPercent, 3)
        }
        threads = [pscustomobject][ordered]@{
            startCount = $startThreads.Count
            stopCount = $stopThreads.Count
            stableCount = $threadDeltas.Count
            startedCount = @($stopThreads.Keys | Where-Object {
                -not $startThreads.ContainsKey($_)
            }).Count
            exitedCount = @($startThreads.Keys | Where-Object {
                -not $stopThreads.ContainsKey($_)
            }).Count
            stableCpuSeconds = [Math]::Round(
                $stableThreadCpuTicks /
                    [double][TimeSpan]::TicksPerSecond, 6)
            stableCoveragePercent = [Math]::Round(
                $stableCoveragePercent, 3)
            top = $topThreads
        }
        snapshots = [pscustomobject][ordered]@{
            start = $Start
            stop = $Stop
        }
    }
}

function New-XpHostProcessCpuProfileState {
    [pscustomobject][ordered]@{
        startAttempted = $false
        stopAttempted = $false
        start = $null
        stop = $null
        profile = $null
    }
}

function Complete-XpHostProcessCpuProfileState {
    param(
        [psobject]$State
    )

    if (-not $State -or $State.profile) {
        return $(if ($State) { $State.profile } else { $null })
    }
    if (-not $State.startAttempted) { return $null }
    try {
        $State.profile = New-XpHostProcessCpuProfile `
            -Start $State.start -Stop $State.stop
    } catch {
        $State.profile = New-XpHostProcessCpuIncompleteProfile `
            -Findings "CPU profile construction failed: $($_.Exception.Message)" `
            -Start $State.start -Stop $State.stop
    }
    $State.profile
}

function Start-XpHostProcessCpuProfileWindow {
    param(
        [psobject]$State,
        [Diagnostics.Process]$Process,
        [object]$StartEvent
    )

    if (-not $State -or -not $StartEvent) { return }
    if (-not $State.startAttempted) {
        $State.startAttempted = $true
        try {
            $State.start = Get-XpHostProcessCpuSnapshot -Process $Process
        } catch {
            $State.start = New-XpHostProcessCpuSnapshotFailure `
                -ErrorMessage `
                    "measurement-start snapshot failed: $($_.Exception.Message)"
        }
    }
    # Preserve every pre-existing event notification even when the diagnostic
    # snapshot itself has already been attempted.
    [void]$StartEvent.Set()
}

function Stop-XpHostProcessCpuProfileWindow {
    param(
        [psobject]$State,
        [Diagnostics.Process]$Process,
        [object]$StopEvent
    )

    if (-not $State -or -not $StopEvent) { return }
    # Stop the paired-host frame window before doing diagnostic enumeration,
    # matching the original runner's event ordering.
    [void]$StopEvent.Set()
    if (-not $State.stopAttempted) {
        $State.stopAttempted = $true
        try {
            $State.stop = Get-XpHostProcessCpuSnapshot -Process $Process
        } catch {
            $State.stop = New-XpHostProcessCpuSnapshotFailure `
                -ErrorMessage `
                    "measurement-stop snapshot failed: $($_.Exception.Message)"
        }
        [void](Complete-XpHostProcessCpuProfileState -State $State)
    }
}
