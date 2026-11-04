function Get-XpLastCompleteDoorbellProfile {
    param(
        [string]$Text,
        [int]$StartIndex,
        [string[]]$MarkerNames = @('QXPICD/', 'QXPD3D/')
    )

    if ([string]::IsNullOrEmpty($Text) -or -not $MarkerNames.Count -or
        $StartIndex -lt 0 -or $StartIndex -gt $Text.Length) {
        return $null
    }
    $profileText = $Text.Substring($StartIndex)
    $markerPattern = @($MarkerNames | ForEach-Object {
        [regex]::Escape($_)
    }) -join '|'
    $markers = [regex]::Matches($profileText, $markerPattern)
    for ($index = $markers.Count - 1; $index -ge 0; --$index) {
        $localStart = $markers[$index].Index
        $localEnd = if ($index + 1 -lt $markers.Count) {
            $markers[$index + 1].Index
        } else {
            $profileText.Length
        }
        $scope = $profileText.Substring(
            $localStart, $localEnd - $localStart)
        $publish = [regex]::Match($scope,
            'DoorbellProfile publish calls (\d+) total (\d+) us avg (\d+) us')
        $swap = [regex]::Match($scope,
            'DoorbellProfile swap calls (\d+) total (\d+) us avg (\d+) us')
        $direct = [regex]::Match($scope,
            'DoorbellProfile direct calls (\d+) total (\d+) us')
        if (-not ($publish.Success -and $swap.Success -and
                  $direct.Success)) {
            continue
        }
        $topDirect = [regex]::Match($scope,
            'DoorbellProfile direct ([^\r\n ]+) calls (\d+) total (\d+) us avg (\d+) us')
        return [pscustomobject][ordered]@{
            markerName = $markers[$index].Value
            markerIndex = $StartIndex + $localStart
            publishCalls = [long]$publish.Groups[1].Value
            publishTotalUs = [long]$publish.Groups[2].Value
            publishAverageUs = [long]$publish.Groups[3].Value
            swapCalls = [long]$swap.Groups[1].Value
            swapTotalUs = [long]$swap.Groups[2].Value
            swapAverageUs = [long]$swap.Groups[3].Value
            directCalls = [long]$direct.Groups[1].Value
            directTotalUs = [long]$direct.Groups[2].Value
            topDirect = if ($topDirect.Success) {
                [pscustomobject][ordered]@{
                    name = $topDirect.Groups[1].Value
                    calls = [long]$topDirect.Groups[2].Value
                    totalUs = [long]$topDirect.Groups[3].Value
                    averageUs = [long]$topDirect.Groups[4].Value
                }
            } else { $null }
        }
    }
    return $null
}
