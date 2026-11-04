$ErrorActionPreference = 'Stop'

function Write-XpAcceptanceAtomicText {
    param(
        [string]$Path,
        [string]$Text
    )

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Artifact destination directory does not exist: $parent"
    }
    $temporary = Join-Path $parent `
        ('.{0}.tmp-{1}-{2}' -f (Split-Path -Leaf $Path), $PID,
            [Guid]::NewGuid().ToString('N'))
    $backup = $null
    $replaceCompleted = $false
    try {
        [IO.File]::WriteAllText(
            $temporary, $Text, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $Path) {
            $backup = Join-Path $parent `
                ('.{0}.backup-{1}-{2}' -f (Split-Path -Leaf $Path), $PID,
                    [Guid]::NewGuid().ToString('N'))
            [IO.File]::Replace($temporary, $Path, $backup)
            $replaceCompleted = $true
            Remove-Item -LiteralPath $backup -Force
            $backup = $null
        } else {
            [IO.File]::Move($temporary, $Path)
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
        # A backup left by a failed replace is recovery evidence. Only remove
        # it when the atomic swap completed and ordinary cleanup then failed.
        if ($replaceCompleted -and $backup) {
            Remove-Item -LiteralPath $backup -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Initialize-XpAcceptanceRunArtifacts {
    param(
        [string]$Directory,
        [string]$RunId,
        [string]$TaskId,
        [string]$Tier,
        [string]$CommandLine
    )

    if (-not (Test-XpAcceptanceRunId -RunId $RunId)) {
        throw "Invalid artifact run ID '$RunId'"
    }
    $fullPath = [IO.Path]::GetFullPath($Directory)
    if (Test-Path -LiteralPath $fullPath) {
        if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
            throw "Artifact destination is not a directory: $fullPath"
        }
        if (@(Get-ChildItem -LiteralPath $fullPath -Force).Count) {
            throw "Artifact destination is not empty: $fullPath"
        }
    } else {
        [void](New-Item -ItemType Directory -Path $fullPath)
    }
    $context = [pscustomobject]@{
        Directory = $fullPath
        RunId = $RunId
        TaskId = if ([string]::IsNullOrWhiteSpace($TaskId)) {
            $RunId
        } else {
            $TaskId
        }
        Tier = if ([string]::IsNullOrWhiteSpace($Tier)) { 'T2' } else { $Tier }
        StartedUtc = [DateTime]::UtcNow.ToString('o')
    }
    Write-XpAcceptanceAtomicText `
        -Path (Join-Path $fullPath 'command.txt') `
        -Text ($CommandLine.TrimEnd() + [Environment]::NewLine)
    return $context
}

function Copy-XpAcceptanceRunArtifact {
    param(
        [psobject]$Context,
        [string]$Source,
        [string]$Name,
        [switch]$Optional
    )

    if ([string]::IsNullOrWhiteSpace($Source) -or
        -not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        if ($Optional) { return $null }
        throw "Required run artifact does not exist: $Source"
    }
    if ([string]::IsNullOrWhiteSpace($Name) -or
        [IO.Path]::GetFileName($Name) -ne $Name -or
        $Name -eq '.' -or $Name -eq '..') {
        throw "Run artifact name must be one safe file name: $Name"
    }
    $destination = Join-Path $Context.Directory $Name
    $temporary = Join-Path $Context.Directory `
        ('.{0}.tmp-{1}-{2}' -f $Name, $PID,
            [Guid]::NewGuid().ToString('N'))
    $backup = $null
    $replaceCompleted = $false
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary
        if (Test-Path -LiteralPath $destination) {
            $backup = Join-Path $Context.Directory `
                ('.{0}.backup-{1}-{2}' -f $Name, $PID,
                    [Guid]::NewGuid().ToString('N'))
            [IO.File]::Replace($temporary, $destination, $backup)
            $replaceCompleted = $true
            Remove-Item -LiteralPath $backup -Force
            $backup = $null
        } else {
            [IO.File]::Move($temporary, $destination)
        }
    } finally {
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
        if ($replaceCompleted -and $backup) {
            Remove-Item -LiteralPath $backup -Force `
                -ErrorAction SilentlyContinue
        }
    }
    return $destination
}

function Get-XpAcceptanceRunArtifactRecords {
    param(
        [psobject]$Context,
        [string[]]$Exclude = @('artifact-manifest.json')
    )

    return @(Get-ChildItem -LiteralPath $Context.Directory -File |
        Where-Object {
            $Exclude -notcontains $_.Name -and
            $_.Name -notmatch '^\..+\.tmp-'
        } | Sort-Object Name | ForEach-Object {
            [pscustomobject][ordered]@{
                name = $_.Name
                size = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName `
                    -Algorithm SHA256).Hash
            }
        })
}

function Test-XpAcceptanceReviewVideoRecords {
    param(
        [object[]]$Records,
        [string]$NamePrefix
    )

    if ([string]::IsNullOrWhiteSpace($NamePrefix)) { return $false }
    $pattern = '^{0}[0-9a-f]{{12}}\.mp4$' -f
        [regex]::Escape($NamePrefix)
    return @($Records | Where-Object {
        $_ -and ([string]$_.name) -cmatch $pattern
    }).Count -eq 1
}

function Get-XpAcceptanceGameObserveCapturePaths {
    param(
        [string]$Directory,
        [string]$Game
    )

    if ([string]::IsNullOrWhiteSpace($Directory) -or
        [string]::IsNullOrWhiteSpace($Game) -or
        -not (Test-Path -LiteralPath $Directory -PathType Container)) {
        return @()
    }
    $stem = "xpdm-$($Game.ToLowerInvariant())"
    $escapedStem = [regex]::Escape($stem)
    return @(Get-ChildItem -LiteralPath $Directory -File |
        Where-Object {
            $_.Name -cmatch ("^{0}-baseline\.bmp$" -f $escapedStem) -or
            $_.Name -cmatch
                ("^{0}-sample-[0-7]-[0-9a-f]{{12}}\.bmp$" -f
                    $escapedStem) -or
            $_.Name -cmatch
                ("^{0}-review-[0-9a-f]{{12}}\.mp4$" -f $escapedStem)
        } | Sort-Object Name | Select-Object -ExpandProperty FullName)
}

function Test-XpAcceptanceGameObserveCaptureNames {
    param(
        [string[]]$Names,
        [string]$Game
    )

    if ([string]::IsNullOrWhiteSpace($Game)) { return $false }
    $stem = [regex]::Escape("xpdm-$($Game.ToLowerInvariant())")
    if (@($Names | Where-Object {
            $_ -and ([string]$_) -cmatch ("^{0}-baseline\.bmp$" -f $stem)
        }).Count -ne 1) { return $false }
    if (@($Names | Where-Object {
            $_ -and ([string]$_) -cmatch
                ("^{0}-review-[0-9a-f]{{12}}\.mp4$" -f $stem)
        }).Count -ne 1) { return $false }
    for ($index = 0; $index -lt 8; ++$index) {
        if (@($Names | Where-Object {
                $_ -and ([string]$_) -cmatch
                    ("^{0}-sample-{1}-[0-9a-f]{{12}}\.bmp$" -f
                        $stem, $index)
            }).Count -ne 1) { return $false }
    }
    return $true
}

function Write-XpAcceptanceRunResult {
    param(
        [psobject]$Context,
        [psobject]$Result
    )

    $path = Join-Path $Context.Directory 'harness-result.json'
    $json = $Result | ConvertTo-Json -Depth 12
    Write-XpAcceptanceAtomicText -Path $path `
        -Text ($json + [Environment]::NewLine)
    return $path
}

function Write-XpAcceptanceRunManifest {
    param([psobject]$Context)

    $manifest = [pscustomobject][ordered]@{
        schema = 1
        runId = $Context.RunId
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        files = @(Get-XpAcceptanceRunArtifactRecords -Context $Context)
    }
    $path = Join-Path $Context.Directory 'artifact-manifest.json'
    Write-XpAcceptanceAtomicText -Path $path `
        -Text (($manifest | ConvertTo-Json -Depth 6) +
            [Environment]::NewLine)
    return $path
}

function Complete-XpAcceptanceLauncherArtifacts {
    param(
        [psobject]$Context,
        [string]$ConsoleStagingPath,
        [psobject]$LaunchRecord,
        [int]$ChildExitCode
    )

    [void](Copy-XpAcceptanceRunArtifact -Context $Context `
        -Source $ConsoleStagingPath -Name 'console.log')
    $launchPath = Join-Path $Context.Directory 'launcher-command.json'
    Write-XpAcceptanceAtomicText -Path $launchPath `
        -Text (($LaunchRecord | ConvertTo-Json -Depth 8) +
            [Environment]::NewLine)

    $resultPath = Join-Path $Context.Directory 'harness-result.json'
    if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        throw "Harness result is missing after child exit: $resultPath"
    }
    $result = Get-Content -Raw -LiteralPath $resultPath |
        ConvertFrom-Json
    # The result document must be the one this launcher ran: an identity
    # mismatch means a foreign or stale result was substituted between the
    # child exiting and finalization.
    foreach ($binding in @(
            @{ Field = 'runId'; Expected = [string]$Context.RunId },
            @{ Field = 'taskId'; Expected = [string]$Context.TaskId },
            @{ Field = 'tier'; Expected = [string]$Context.Tier })) {
        $actual = [string]$result.($binding.Field)
        if ($actual -cne $binding.Expected) {
            throw ("Harness result $($binding.Field) '$actual' does not " +
                "match the launcher's '$($binding.Expected)'")
        }
    }
    $launcher = [pscustomobject][ordered]@{
        phase = 'finalized'
        childStarted = $true
        finalized = $true
        childExitCode = $ChildExitCode
        console = 'console.log'
        command = 'launcher-command.json'
    }
    $result | Add-Member -NotePropertyName launcher `
        -NotePropertyValue $launcher -Force
    $finalArtifacts = @(Get-XpAcceptanceRunArtifactRecords `
        -Context $Context `
        -Exclude @('artifact-manifest.json', 'harness-result.json') |
        ForEach-Object { $_.name })
    $result | Add-Member -NotePropertyName artifacts `
        -NotePropertyValue $finalArtifacts -Force
    [void](Write-XpAcceptanceRunResult -Context $Context -Result $result)
    [void](Write-XpAcceptanceRunManifest -Context $Context)

    $manifestPath = Join-Path $Context.Directory 'artifact-manifest.json'
    $manifest = Get-Content -Raw -LiteralPath $manifestPath |
        ConvertFrom-Json
    $consoleRecord = @($manifest.files | Where-Object {
        $_.name -eq 'console.log'
    })
    if ($consoleRecord.Count -ne 1) {
        throw 'Final artifact manifest does not contain exactly one console.log'
    }
    $consolePath = Join-Path $Context.Directory 'console.log'
    $consoleHash = (Get-FileHash -LiteralPath $consolePath `
        -Algorithm SHA256).Hash
    if ($consoleRecord[0].size -ne (Get-Item -LiteralPath $consolePath).Length -or
        $consoleRecord[0].sha256 -ne $consoleHash) {
        throw 'Final artifact manifest console.log identity is wrong'
    }
    return $manifestPath
}
