$ErrorActionPreference = 'Stop'

function Enter-XpWarcraft3D3D8RingSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,
        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory,
        [DateTime]$NotBeforeUtc = [DateTime]::MinValue,
        [ValidateRange(1, 30000)]
        [int]$LockTimeoutMilliseconds = 5000
    )

    $source = [IO.Path]::GetFullPath($SourceDirectory)
    $destination = [IO.Path]::GetFullPath($DestinationDirectory)
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Frame-ring source directory does not exist: $source"
    }
    if (Test-Path -LiteralPath $destination) {
        if (@(Get-ChildItem -LiteralPath $destination -Force).Count) {
            throw "Frame-ring snapshot directory is not empty: $destination"
        }
    } else {
        [void](New-Item -ItemType Directory -Path $destination)
    }

    # The core publishes through one temporary file and atomically replaces a
    # numeric slot.  Holding that temp file with FileShare.None waits out any
    # in-flight writer and makes the next fopen fail closed, permanently
    # quiescing this process's ring before any numeric slot is moved.
    $temporary = Join-Path $source 'frame-ring-tmp.bmp'
    $lock = $null
    $deadline = [DateTime]::UtcNow.AddMilliseconds($LockTimeoutMilliseconds)
    do {
        try {
            $lock = [IO.File]::Open(
                $temporary, [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 10
        }
    } while (-not $lock)

    try {
        $sourceFrames = @(Get-ChildItem -LiteralPath $source -File |
            Where-Object {
                $_.Name -match '^frame-ring-[0-9]+\.bmp$' -and
                $_.LastWriteTimeUtc -ge $NotBeforeUtc
            } | Sort-Object LastWriteTimeUtc, Name)
        foreach ($frame in $sourceFrames) {
            Move-Item -LiteralPath $frame.FullName `
                -Destination (Join-Path $destination $frame.Name)
        }
        $frames = @(Get-ChildItem -LiteralPath $destination -File |
            Where-Object { $_.Name -match '^frame-ring-[0-9]+\.bmp$' } |
            Sort-Object LastWriteTimeUtc, Name)
        return [pscustomobject][ordered]@{
            sourceDirectory = $source
            destinationDirectory = $destination
            temporaryPath = $temporary
            lockStream = $lock
            frames = $frames
            frameCount = $frames.Count
        }
    } catch {
        $lock.Dispose()
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
        throw
    }
}

function Exit-XpWarcraft3D3D8RingSnapshot {
    param([object]$Snapshot)

    if (-not $Snapshot) { return }
    if ($Snapshot.lockStream) {
        $Snapshot.lockStream.Dispose()
    }
    if ($Snapshot.temporaryPath) {
        Remove-Item -LiteralPath $Snapshot.temporaryPath -Force `
            -ErrorAction SilentlyContinue
    }
}

function Get-XpWarcraft3D3D8BlackRunWitness {
    param([object]$Cadence)

    if (-not $Cadence -or -not $Cadence.classes -or
        -not $Cadence.runs -or -not $Cadence.frameDetails) {
        return @()
    }
    $classes = [string]$Cadence.classes
    $blackRun = @($Cadence.runs | Where-Object {
        $_.class -eq 'B' -and $_.start -gt 0 -and
        $_.end + 1 -lt $classes.Length -and
        $classes[[int]$_.start - 1] -eq 'V' -and
        $classes[[int]$_.end + 1] -eq 'V'
    } | Select-Object -First 1)
    if (-not $blackRun.Count) { return @() }
    $indices = if ($blackRun[0].start -eq $blackRun[0].end) {
        @(([int]$blackRun[0].start - 1),
          ([int]$blackRun[0].start),
          ([int]$blackRun[0].end + 1))
    } else {
        @(([int]$blackRun[0].start - 1),
          ([int]$blackRun[0].start),
          ([int]$blackRun[0].end),
          ([int]$blackRun[0].end + 1))
    }
    $labels = if ($indices.Count -eq 3) {
        @('before', 'black', 'after')
    } else {
        @('before', 'black-first', 'black-last', 'after')
    }
    return @(for ($index = 0; $index -lt $indices.Count; ++$index) {
        [pscustomobject][ordered]@{
            label = $labels[$index]
            index = $indices[$index]
            frame = $Cadence.frameDetails[$indices[$index]]
        }
    })
}

function Test-XpWarcraft3D3D8EncodedFrameCount {
    param(
        [int]$SnapshotFrames,
        [int]$EncodedFrames
    )

    return $SnapshotFrames -eq 256 -and
           $EncodedFrames -eq $SnapshotFrames
}

function Test-XpWarcraft3D3D8EncodedVideoTiming {
    param(
        [int]$SnapshotFrames,
        [int]$EncodedFrames,
        [string]$NominalFrameRate,
        [string]$AverageFrameRate,
        [double]$DurationSeconds
    )

    if (-not (Test-XpWarcraft3D3D8EncodedFrameCount `
            -SnapshotFrames $SnapshotFrames `
            -EncodedFrames $EncodedFrames) -or
        $NominalFrameRate -cne '30/1' -or
        $AverageFrameRate -cne '30/1' -or
        [double]::IsNaN($DurationSeconds) -or
        [double]::IsInfinity($DurationSeconds)) {
        return $false
    }
    $expectedDuration = $SnapshotFrames / 30.0
    return [Math]::Abs($DurationSeconds - $expectedDuration) -le 0.001
}

function Add-XpWarcraft3D3D8UniqueMessage {
    param(
        [Collections.Generic.List[string]]$Messages,
        [string]$Message
    )

    if ($null -eq $Messages -or [string]::IsNullOrWhiteSpace($Message) -or
        $Messages.Contains($Message)) { return $false }
    $Messages.Add($Message)
    return $true
}

function Test-XpWarcraft3D3D8GuestContract {
    param(
        [string[]]$OracleStartMessages = @(),
        [string[]]$OracleCompleteMessages = @(),
        [string[]]$ResultMessages = @(),
        [DateTime]$OracleStartReceivedAt = [DateTime]::MinValue,
        [DateTime]$OracleCompleteReceivedAt = [DateTime]::MaxValue
    )

    $findings = [Collections.Generic.List[string]]::new()
    $completeMilliseconds = [uint64]0
    $resultMilliseconds = [uint64]0
    $moduleMask = [uint32]0
    $startMessages = @($OracleStartMessages)
    $completeMessages = @($OracleCompleteMessages)
    $results = @($ResultMessages)
    if ($startMessages.Count -ne 1) {
        $findings.Add("oracle-start count is $($startMessages.Count), expected 1")
    } elseif ($startMessages[0] -notmatch
        '^QXP_GAME_OBSERVE_STAGE rc=0 game=warcraft3d3d8 stage=oracle-start warmup-ms=15000 renderer=d3d8 system32=1$') {
        $findings.Add('oracle-start fields are malformed')
    }
    if ($completeMessages.Count -ne 1) {
        $findings.Add("oracle-complete count is $($completeMessages.Count), expected 1")
    } elseif ($completeMessages[0] -notmatch
        '^QXP_GAME_OBSERVE_STAGE rc=0 game=warcraft3d3d8 stage=oracle-complete observed-ms=([0-9]+) renderer=d3d8 system32=1$' -or
        -not [uint64]::TryParse($Matches[1], [ref]$completeMilliseconds)) {
        $findings.Add('oracle-complete fields are malformed')
    }
    if ($results.Count -ne 1) {
        $findings.Add("result count is $($results.Count), expected 1")
    } elseif ($results[0] -notmatch
        '^QXP_GAME_OBSERVE_RESULT rc=0 game=warcraft3d3d8 stage=complete outcome=observed exit=259 modules=([0-9A-Fa-f]{2}) observed-ms=([0-9]+) renderer=d3d8 system32=1$') {
        $findings.Add('result fields are malformed')
    } else {
        [void][uint32]::TryParse(
            $Matches[1], [Globalization.NumberStyles]::HexNumber,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$moduleMask)
        if (-not [uint64]::TryParse(
                $Matches[2], [ref]$resultMilliseconds)) {
            $findings.Add('result observed-ms is invalid')
        }
        if (($moduleMask -band 1) -eq 0) {
            $findings.Add('result module mask does not include D3D8')
        }
    }
    if ($completeMilliseconds -lt 25000) {
        $findings.Add('oracle-complete observed-ms is shorter than 25000')
    }
    if ($resultMilliseconds -ne $completeMilliseconds) {
        $findings.Add('result observed-ms does not match oracle-complete')
    }
    $hostMilliseconds = 0.0
    if ($OracleStartReceivedAt -eq [DateTime]::MinValue -or
        $OracleCompleteReceivedAt -eq [DateTime]::MaxValue -or
        $OracleCompleteReceivedAt -le $OracleStartReceivedAt) {
        $findings.Add('host oracle boundary is missing or reversed')
    } else {
        $hostMilliseconds = ($OracleCompleteReceivedAt -
            $OracleStartReceivedAt).TotalMilliseconds
        # Serial receipt timestamps can contract slightly around polling, but
        # must independently corroborate the guest's exact 25-second timer.
        if ($hostMilliseconds -lt 24500) {
            $findings.Add('host oracle boundary is shorter than 24500 ms')
        }
    }
    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        completeMilliseconds = $completeMilliseconds
        resultMilliseconds = $resultMilliseconds
        moduleMask = $moduleMask
        hostMilliseconds = [Math]::Round($hostMilliseconds, 3)
        findings = @($findings)
    }
}

function Test-XpWarcraft3D3D8CounterProfile {
    param([object]$Profile)

    $findings = [Collections.Generic.List[string]]::new()
    $miniport = if ($Profile) { $Profile.miniport } else { $null }
    if (-not $Profile) {
        $findings.Add('D3D counter profile is missing')
    } elseif (-not $Profile.valid) {
        $findings.Add('D3D counter profile is invalid')
    }
    if (-not $miniport) {
        $findings.Add('miniport counter session is missing')
    } else {
        if (-not $miniport.presentationSourceCountersPresent) {
            $findings.Add('presentation-source counter group is missing')
        }
        if (-not $miniport.logicalDraws) {
            $findings.Add('logical draw count is zero')
        }
        if (-not $miniport.presents) {
            $findings.Add('present count is zero')
        }
        if (-not $miniport.surfaceReadbacks) {
            $findings.Add('surface readback count is zero')
        }
        if (-not $miniport.surfaceTexturePresents) {
            $findings.Add('surface-texture present count is zero')
        }
        if ($miniport.cpuPresents) {
            $findings.Add('CPU presents remain in the Warcraft D3D8 path')
        }
        if ($miniport.emptyPresents) {
            $findings.Add('empty presents remain in the Warcraft D3D8 path')
        }
        if ($miniport.presentationSourceCountersPresent -and
            [uint64]$miniport.cpuPresents +
                [uint64]$miniport.surfaceTexturePresents +
                [uint64]$miniport.emptyPresents -ne
                    [uint64]$miniport.presents) {
            $findings.Add('presentation-source counts do not sum to presents')
        }
    }
    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        findings = @($findings)
    }
}
