function Update-XpD3d9MultistreamOrder {
    param(
        [ValidateRange(0, 9)]
        [int]$CurrentStage,
        [ValidateSet(
            'Ready', 'Draw', 'Present', 'SceneCaptureAck', 'Release',
            'Lifetime', 'Reset', 'ResetCaptureAck', 'Result')]
        [string]$Event
    )

    $events = @(
        'Ready', 'Draw', 'Present', 'SceneCaptureAck', 'Release',
        'Lifetime', 'Reset', 'ResetCaptureAck', 'Result'
    )
    $expected = if ($CurrentStage -lt $events.Count) {
        $events[$CurrentStage]
    } else {
        $null
    }
    $ok = $null -ne $expected -and $Event -ceq $expected
    [pscustomobject][ordered]@{
        ok = $ok
        stage = if ($ok) { $CurrentStage + 1 } else { $CurrentStage }
        expected = $expected
        observed = $Event
    }
}
