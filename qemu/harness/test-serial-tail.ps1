$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'serial-tail.ps1')

function Assert-Equal {
    param($Actual, $Expected, [string]$Label)

    if ($Actual -ne $Expected) {
        throw "$Label expected '$Expected', got '$Actual'"
    }
}

$path = Join-Path ([IO.Path]::GetTempPath()) `
    "xpdriver-serial-tail-$([Guid]::NewGuid().ToString('N')).log"
$writer = $null
$reader = $null
try {
    $writer = [IO.File]::Open(
        $path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::ReadWrite)
    $reader = [IO.File]::Open(
        $path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    $buffer = [byte[]]::new(17)
    $pending = ''
    $messages = [Collections.Generic.Queue[string]]::new()

    $first = [Text.Encoding]::ASCII.GetBytes(
        'QXP_D3DVS_BENCH_PHASE rc=0 order=3 mode=shader frames=16 d')
    $writer.Write($first, 0, $first.Length)
    $writer.Flush()
    Receive-SerialLines -Stream $reader -Buffer $buffer `
        -Pending ([ref]$pending) -Messages $messages
    Assert-Equal $messages.Count 0 'partial-line queue count'
    Assert-Equal $pending ([Text.Encoding]::ASCII.GetString($first)) `
        'partial-line buffer'

    $second = [Text.Encoding]::ASCII.GetBytes(
        "raws-per-frame=16`r`nNEXT partial")
    $writer.Write($second, 0, $second.Length)
    $writer.Flush()
    Receive-SerialLines -Stream $reader -Buffer $buffer `
        -Pending ([ref]$pending) -Messages $messages
    Assert-Equal $messages.Count 1 'completed-line queue count'
    Assert-Equal $messages.Dequeue() `
        'QXP_D3DVS_BENCH_PHASE rc=0 order=3 mode=shader frames=16 draws-per-frame=16' `
        'completed split line'
    Assert-Equal $pending 'NEXT partial' 'second partial-line buffer'

    $third = [Text.Encoding]::ASCII.GetBytes(" line`nTHIRD`r`n")
    $writer.Write($third, 0, $third.Length)
    $writer.Flush()
    Receive-SerialLines -Stream $reader -Buffer $buffer `
        -Pending ([ref]$pending) -Messages $messages
    Assert-Equal $messages.Count 2 'multi-line queue count'
    Assert-Equal $messages.Dequeue() 'NEXT partial line' 'second line'
    Assert-Equal $messages.Dequeue() 'THIRD' 'third line'
    Assert-Equal $pending '' 'final partial-line buffer'
} finally {
    if ($reader) { $reader.Dispose() }
    if ($writer) { $writer.Dispose() }
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
}

Write-Output 'serial-tail: complete-line buffering checks passed'
