$ErrorActionPreference = 'Stop'
$script:XpAcceptanceLeaseOffset = [long]2147483647

function New-XpAcceptanceRunId {
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    $suffix = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    return "$timestamp-$PID-$suffix"
}

function Test-XpAcceptanceRunId {
    param([string]$RunId)

    return -not [string]::IsNullOrWhiteSpace($RunId) -and
        $RunId -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'
}

function Get-XpAcceptanceTimeoutBudget {
    param(
        [ValidateRange(1, 86400)]
        [int]$RequestedSeconds,
        [bool]$Xpdm,
        [bool]$LongRunning,
        [bool]$CaptureGameObserve,
        [bool]$RecordVideo,
        [bool]$UT2003VisualOracle = $false
    )

    $effectiveSeconds = if ($UT2003VisualOracle -and
                             $RequestedSeconds -lt 7200) {
        # The recorded synchronous-capture control needed 608.5 host seconds
        # for 46.625 guest-observed seconds. Scaling the oracle's bounded
        # 300-second startup plus 180-second dwell and adding margin yields a
        # conservative two-hour VM deadline; outer encoding remains separate.
        7200
    } elseif ($CaptureGameObserve -and $RequestedSeconds -lt 600) {
        # A 45-second guest observation can take more than six host minutes
        # when the frontend performs synchronous readback or frame dumping.
        600
    } elseif ($LongRunning -and $RequestedSeconds -lt 300) {
        300
    } elseif ($Xpdm -and $RequestedSeconds -lt 180) {
        180
    } else {
        $RequestedSeconds
    }
    # This is guidance for a caller's outer process timeout. It covers ISO
    # refresh, shutdown, artifact scoring, and optional video encoding, which
    # occur outside the VM observation deadline above.
    $postRunAllowanceSeconds = if ($RecordVideo) { 360 } else { 240 }
    return [pscustomobject]@{
        RequestedSeconds = $RequestedSeconds
        EffectiveSeconds = $effectiveSeconds
        RecommendedOuterSeconds = $effectiveSeconds +
            $postRunAllowanceSeconds
    }
}

function Write-XpAcceptanceArtifactLease {
    param([psobject]$Lease)

    $Lease.Metadata.updatedUtc = [DateTime]::UtcNow.ToString('o')
    $json = $Lease.Metadata | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json +
        [Environment]::NewLine)
    $Lease.Stream.Position = 0
    $Lease.Stream.Write($bytes, 0, $bytes.Length)
    $Lease.Stream.SetLength($bytes.Length)
    $Lease.Stream.Flush()
}

function Read-XpAcceptanceArtifactLeaseText {
    param([IO.FileStream]$Stream)

    try {
        $Stream.Position = 0
        $buffer = [byte[]]::new([int]$Stream.Length)
        $read = $Stream.Read($buffer, 0, $buffer.Length)
        if ($read -gt 0) {
            return [Text.Encoding]::UTF8.GetString($buffer, 0, $read).Trim()
        }
    } catch {
        return '<owner metadata unavailable>'
    }
    return '<owner metadata unavailable>'
}

function Enter-XpAcceptanceArtifactLease {
    param(
        [string]$LockPath,
        [string]$RunId,
        [int]$QmpPort,
        [int]$RequestedTimeoutSeconds,
        [int]$EffectiveTimeoutSeconds
    )

    if (-not (Test-XpAcceptanceRunId -RunId $RunId)) {
        throw "Invalid acceptance run ID '$RunId'; use 1-64 letters, digits, dots, underscores, or hyphens"
    }
    $parent = Split-Path -Parent $LockPath
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Acceptance artifact lease directory does not exist: $parent"
    }

    $stream = [IO.File]::Open(
        $LockPath, [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        # Lock a byte beyond any realistic metadata payload so a contending
        # process can still read byte zero onward and report the current owner.
        $stream.Lock($script:XpAcceptanceLeaseOffset, 1)
    } catch [IO.IOException] {
        $owner = Read-XpAcceptanceArtifactLeaseText -Stream $stream
        $stream.Dispose()
        throw "Acceptance artifacts are owned by another run. Lease=$LockPath Owner=$owner"
    }

    $startedUtc = [DateTime]::UtcNow.ToString('o')
    $lease = [pscustomobject]@{
        Path = $LockPath
        Stream = $stream
        Metadata = [pscustomobject][ordered]@{
            schemaVersion = 1
            status = 'active'
            runId = $RunId
            harnessPid = $PID
            hostPid = $null
            qmpPort = $QmpPort
            mode = $null
            contentPath = $null
            requestedTimeoutSeconds = $RequestedTimeoutSeconds
            effectiveTimeoutSeconds = $EffectiveTimeoutSeconds
            startedUtc = $startedUtc
            updatedUtc = $startedUtc
            failureType = $null
            failureMessage = $null
            cleanupHostStopped = $null
        }
    }
    try {
        Write-XpAcceptanceArtifactLease -Lease $lease
    } catch {
        $stream.Unlock($script:XpAcceptanceLeaseOffset, 1)
        $stream.Dispose()
        throw
    }
    return $lease
}

function Update-XpAcceptanceArtifactLease {
    param(
        [psobject]$Lease,
        [int]$HostPid,
        [int]$QmpPort,
        [string]$Mode,
        [string]$ContentPath
    )

    if ($PSBoundParameters.ContainsKey('HostPid')) {
        $Lease.Metadata.hostPid = $HostPid
    }
    if ($PSBoundParameters.ContainsKey('QmpPort')) {
        $Lease.Metadata.qmpPort = $QmpPort
    }
    if ($PSBoundParameters.ContainsKey('Mode')) {
        $Lease.Metadata.mode = $Mode
    }
    if ($PSBoundParameters.ContainsKey('ContentPath')) {
        $Lease.Metadata.contentPath = $ContentPath
    }
    Write-XpAcceptanceArtifactLease -Lease $Lease
}

function Get-XpTestVmProcesses {
    # Snapshot of VM-related processes for ownership and cleanup auditing.
    $found = [Collections.Generic.List[object]]::new()
    foreach ($name in @('retro_host.exe', 'qemu-system-x86_64.exe',
            'qemu-system-i386.exe')) {
        foreach ($proc in @(Get-CimInstance Win32_Process `
                -Filter "Name='$name'" -ErrorAction SilentlyContinue)) {
            $found.Add([pscustomobject][ordered]@{
                pid = [int]$proc.ProcessId
                name = [string]$proc.Name
                started = if ($proc.CreationDate) {
                    $proc.CreationDate.ToString('o')
                } else { $null }
                commandLine = [string]$proc.CommandLine
            })
        }
    }
    return @($found)
}

function Test-XpTestPortFree {
    param([int]$Port)

    # Same ownership probe the runner performs before launch: a bindable
    # loopback QMP port is free, a bound one names a live frontend.
    $listener = $null
    try {
        $listener = [Net.Sockets.TcpListener]::new(
            [Net.IPAddress]::Loopback, $Port)
        $listener.Start()
        return $true
    } catch [Net.Sockets.SocketException] {
        return $false
    } finally {
        if ($listener) { $listener.Stop() }
    }
}

function Test-XpAcceptanceArtifactLease {
    param([string]$LockPath)

    # Read-only ownership probe for preflight and handback tooling. It never
    # creates the lock file and never writes metadata: an absent or unlocked
    # lock is free, and a held lock reports the readable owner metadata. The
    # byte-range lock is OS-held, so a held lock always names a live owner.
    if (-not (Test-Path -LiteralPath $LockPath -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            path = $LockPath
            exists = $false
            free = $true
            owner = $null
        }
    }
    $stream = [IO.File]::Open(
        $LockPath, [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    try {
        try {
            $stream.Lock($script:XpAcceptanceLeaseOffset, 1)
        } catch [IO.IOException] {
            $owner = Read-XpAcceptanceArtifactLeaseText -Stream $stream
            return [pscustomobject][ordered]@{
                path = $LockPath
                exists = $true
                free = $false
                owner = $owner
            }
        }
        $stream.Unlock($script:XpAcceptanceLeaseOffset, 1)
        return [pscustomobject][ordered]@{
            path = $LockPath
            exists = $true
            free = $true
            owner = $null
        }
    } finally {
        $stream.Dispose()
    }
}

function Exit-XpAcceptanceArtifactLease {
    param(
        [psobject]$Lease,
        [ValidateSet('passed', 'failed')]
        [string]$Status
    )

    if (-not $Lease -or -not $Lease.Stream) { return }
    try {
        $Lease.Metadata.status = $Status
        Write-XpAcceptanceArtifactLease -Lease $Lease
    } finally {
        try {
            $Lease.Stream.Unlock($script:XpAcceptanceLeaseOffset, 1)
        } catch { }
        $Lease.Stream.Dispose()
        $Lease.Stream = $null
    }
}
