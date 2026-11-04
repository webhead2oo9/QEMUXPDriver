$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'frame-ring-black-cadence.py'
& python $script --self-test
if ($LASTEXITCODE -ne 0) {
    throw "frame-ring black-cadence self-test failed: $LASTEXITCODE"
}
