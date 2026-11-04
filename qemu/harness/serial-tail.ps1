function Receive-SerialLines {
    param(
        [Parameter(Mandatory)]
        [IO.FileStream]$Stream,
        [Parameter(Mandatory)]
        [byte[]]$Buffer,
        [Parameter(Mandatory)]
        [ref]$Pending,
        [Parameter(Mandatory)]
        [Collections.Generic.Queue[string]]$Messages
    )

    if (-not $Stream.CanRead) { throw 'Serial stream is not readable' }
    if (-not $Buffer.Length) { throw 'Serial read buffer is empty' }

    while ($Stream.Position -lt $Stream.Length) {
        $available = [int][Math]::Min(
            [long]$Buffer.Length, $Stream.Length - $Stream.Position)
        $read = $Stream.Read($Buffer, 0, $available)
        if ($read -le 0) { break }
        $Pending.Value += [Text.Encoding]::ASCII.GetString(
            $Buffer, 0, $read)
        while (($newline = $Pending.Value.IndexOf(
                    "`n", [StringComparison]::Ordinal)) -ge 0) {
            $line = $Pending.Value.Substring(0, $newline).TrimEnd("`r")
            $Pending.Value = $Pending.Value.Substring($newline + 1)
            $Messages.Enqueue($line)
        }
    }
}
