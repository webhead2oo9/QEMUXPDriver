param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# Keep the visual-review encoder local to the workspace.  The archive version
# and digest are fixed so two development machines install the same tool.
$version = '8.0.1'
$archiveName = "ffmpeg-$version-essentials_build.zip"
$archiveSha256 =
    'e2aaeaa0fdbc397d4794828086424d4aaa2102cef1fb6874f6ffd29c0b88b673'
$archiveUrl =
    "https://www.gyan.dev/ffmpeg/builds/packages/$archiveName"
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolsRoot = Join-Path $workspace '.tools'
$installRoot = Join-Path $toolsRoot "ffmpeg-$version"
$archivePath = Join-Path $toolsRoot $archiveName
$ffmpeg = Join-Path $installRoot `
    "ffmpeg-$version-essentials_build\bin\ffmpeg.exe"

if ((Test-Path -LiteralPath $ffmpeg) -and -not $Force) {
    Write-Output $ffmpeg
    exit 0
}

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
if ($Force -or -not (Test-Path -LiteralPath $archivePath) -or
    (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne
        $archiveSha256) {
    Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
    Write-Output "Downloading deterministic FFmpeg $version archive..."
    Invoke-WebRequest -UseBasicParsing -Uri $archiveUrl -OutFile $archivePath
}

$actualSha256 = (Get-FileHash -LiteralPath $archivePath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $archiveSha256) {
    throw "FFmpeg archive digest mismatch: expected $archiveSha256, got $actualSha256"
}

if (Test-Path -LiteralPath $installRoot) {
    $resolvedTools = (Resolve-Path -LiteralPath $toolsRoot).Path
    $resolvedInstall = (Resolve-Path -LiteralPath $installRoot).Path
    if (-not $resolvedInstall.StartsWith(
            $resolvedTools + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace unexpected tool path: $resolvedInstall"
    }
    Remove-Item -LiteralPath $resolvedInstall -Recurse -Force
}
Expand-Archive -LiteralPath $archivePath -DestinationPath $installRoot

if (-not (Test-Path -LiteralPath $ffmpeg)) {
    throw "FFmpeg archive did not contain the expected executable: $ffmpeg"
}

Write-Output $ffmpeg
