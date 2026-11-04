param(
    [Parameter(Mandatory = $true)]
    [string]$SpecPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'acceptance-run-contract.ps1')
. (Join-Path $PSScriptRoot 'acceptance-run-artifacts.ps1')
. (Join-Path $PSScriptRoot 'acceptance-matrix-verifier.ps1')

$specFile = (Resolve-Path -LiteralPath $SpecPath).Path
$outputFile = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $outputFile
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    throw "Matrix verification output parent does not exist: $outputParent"
}
if (Test-Path -LiteralPath $outputFile) {
    throw "Refusing to overwrite matrix verification output: $outputFile"
}

try {
    $spec = Get-Content -Raw -LiteralPath $specFile | ConvertFrom-Json
} catch {
    throw "Matrix spec is not valid JSON: $($_.Exception.Message)"
}

$verification = Get-XpAcceptanceMatrixVerification -Spec $spec
Write-XpAcceptanceAtomicText -Path $outputFile `
    -Text (($verification | ConvertTo-Json -Depth 20) +
        [Environment]::NewLine)
Write-Output "AcceptanceMatrix taskId=$($verification.taskId) verdict=$($verification.verdict) valid=$($verification.valid) bundles=$(@($verification.bundles).Count) findings=$(@($verification.findings).Count) output=$outputFile"
foreach ($finding in @($verification.findings)) {
    Write-Output "AcceptanceMatrixFinding=$finding"
}
if (-not $verification.valid) { exit 1 }
