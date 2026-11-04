function Get-XpGameObserveBitmapAssessment {
    param([psobject]$Bitmap)

    if (-not $Bitmap -or $Bitmap.Width -le 0 -or $Bitmap.Height -le 0) {
        return [pscustomobject]@{
            valid = $false
            rich = $false
            colors = 0
            blackPercent = 100.0
            desktop = $false
            desktopBottom = $false
            desktopTop = $false
        }
    }
    $samples = 0
    $blackSamples = 0
    $colors = @{}
    for ($y = 0; $y -lt $Bitmap.Height; $y += 16) {
        for ($x = 0; $x -lt $Bitmap.Width; $x += 16) {
            $pixel = $Bitmap.GetPixel($x, $y)
            ++$samples
            if ($pixel.R -lt 8 -and $pixel.G -lt 8 -and
                $pixel.B -lt 8) { ++$blackSamples }
            $colors[('{0:X2}{1:X2}{2:X2}' -f
                $pixel.R, $pixel.G, $pixel.B)] = $true
        }
    }
    $startX = [Math]::Min(25, $Bitmap.Width - 1)
    $taskX = [Math]::Min(300, $Bitmap.Width - 1)
    $startBottom = $Bitmap.GetPixel(
        $startX, [Math]::Max(0, $Bitmap.Height - 15))
    $taskBottom = $Bitmap.GetPixel(
        $taskX, [Math]::Max(0, $Bitmap.Height - 10))
    $startTop = $Bitmap.GetPixel(
        $startX, [Math]::Min(14, $Bitmap.Height - 1))
    $taskTop = $Bitmap.GetPixel(
        $taskX, [Math]::Min(9, $Bitmap.Height - 1))
    $desktopBottom =
        ($startBottom.G -gt 100 -and
         $startBottom.G -gt $startBottom.R) -or
        ($taskBottom.B -gt 150 -and
         $taskBottom.B -gt ($taskBottom.R + 80))
    $desktopTop =
        ($startTop.G -gt 100 -and
         $startTop.G -gt $startTop.R) -or
        ($taskTop.B -gt 150 -and
         $taskTop.B -gt ($taskTop.R + 80))
    $blackPercent = if ($samples) {
        [Math]::Round(100.0 * $blackSamples / $samples, 2)
    } else { 100.0 }
    $desktop = $desktopBottom -or $desktopTop
    return [pscustomobject]@{
        valid = $true
        rich = -not $desktop -and $colors.Count -ge 8 -and
            $blackPercent -lt 90
        colors = $colors.Count
        blackPercent = $blackPercent
        desktop = $desktop
        desktopBottom = $desktopBottom
        desktopTop = $desktopTop
    }
}

function Resolve-XpBundleConfinedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BundleRoot,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    # Manifest and fixed bundle artifact names must be bare leaf names that
    # resolve beneath the canonical bundle root.  Any path component from the
    # candidate leaf up to and including the bundle root that carries the
    # reparse-point attribute (symlink, junction, mounted volume) redirects
    # reads outside the preserved evidence and is rejected before any byte
    # is read.  All comparisons are case-insensitive Windows canonical
    # comparisons.
    if ([string]::IsNullOrWhiteSpace($Name) -or
        $Name -cne $Name.Trim() -or
        $Name -ceq '.' -or $Name -ceq '..' -or
        $Name.IndexOfAny([char[]]@('\', '/', ':')) -ge 0) {
        return $null
    }
    $root = [IO.Path]::GetFullPath($BundleRoot).TrimEnd('\', '/')
    $candidate = [IO.Path]::GetFullPath((Join-Path $root $Name))
    $parent = [IO.Path]::GetDirectoryName($candidate)
    if ([string]::IsNullOrWhiteSpace($parent) -or
        -not $parent.Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }
    $component = $candidate
    while ($true) {
        $item = Get-Item -LiteralPath $component -Force `
            -ErrorAction SilentlyContinue
        if ($item -and
            (([IO.FileAttributes]$item.Attributes -band
              [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            return $null
        }
        if ($component.Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $next = [IO.Path]::GetDirectoryName($component)
        if ([string]::IsNullOrWhiteSpace($next) -or $next -ceq $component) {
            break
        }
        $component = $next
    }
    return $candidate
}

Add-Type -AssemblyName System.Drawing
if (-not ('XpBitmapVisual' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

public sealed class XpBitmapVisualEvidence
{
    public int Width { get; set; }
    public int Height { get; set; }
    public string CanonicalRgbSha256 { get; set; }
    public byte[] Signature { get; set; }
    public int SignatureStep { get; set; }
    public int SignatureMarginX { get; set; }
    public int SignatureMarginY { get; set; }
    public int SignatureColumns { get; set; }
    public int SignatureRows { get; set; }
}

public sealed class XpUT2003MenuVisualEvidence
{
    public bool Valid { get; set; }
    public int TitleBright { get; set; }
    public int TitleGold { get; set; }
    public int HighlightGold { get; set; }
    public int HostRowBright { get; set; }
    public int InstantRowBright { get; set; }
    public int SettingsRowBright { get; set; }
}

public static class XpBitmapVisual
{
    public static XpBitmapVisualEvidence Analyze(string path, int step)
    {
        if (step < 1) throw new ArgumentOutOfRangeException("step");
        using (Bitmap source = new Bitmap(path))
        using (Bitmap decoded = new Bitmap(
            source.Width, source.Height, PixelFormat.Format24bppRgb))
        {
            using (Graphics graphics = Graphics.FromImage(decoded))
                graphics.DrawImageUnscaled(source, 0, 0);
            Rectangle rectangle = new Rectangle(
                0, 0, decoded.Width, decoded.Height);
            BitmapData data = decoded.LockBits(
                rectangle, ImageLockMode.ReadOnly,
                PixelFormat.Format24bppRgb);
            byte[] rgb = new byte[decoded.Width * decoded.Height * 3];
            try
            {
                int rowBytes = decoded.Width * 3;
                byte[] row = new byte[rowBytes];
                for (int y = 0; y < decoded.Height; ++y)
                {
                    Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride),
                                 row, 0, rowBytes);
                    int destination = y * rowBytes;
                    for (int x = 0; x < decoded.Width; ++x)
                    {
                        int sourceOffset = x * 3;
                        rgb[destination + sourceOffset] =
                            row[sourceOffset + 2];
                        rgb[destination + sourceOffset + 1] =
                            row[sourceOffset + 1];
                        rgb[destination + sourceOffset + 2] =
                            row[sourceOffset];
                    }
                }
            }
            finally
            {
                decoded.UnlockBits(data);
            }
            string digest;
            using (SHA256 sha256 = SHA256.Create())
                digest = BitConverter.ToString(
                    sha256.ComputeHash(rgb)).Replace("-", "");
            int marginX = Math.Min(16, Math.Max(0, decoded.Width / 4));
            int marginY = Math.Min(16, Math.Max(0, decoded.Height / 4));
            List<byte> signature = new List<byte>();
            int stride = decoded.Width * 3;
            for (int y = marginY; y < decoded.Height - marginY; y += step)
            {
                for (int x = marginX; x < decoded.Width - marginX; x += step)
                {
                    int offset = y * stride + x * 3;
                    signature.Add(rgb[offset]);
                    signature.Add(rgb[offset + 1]);
                    signature.Add(rgb[offset + 2]);
                }
            }
            return new XpBitmapVisualEvidence {
                Width = decoded.Width,
                Height = decoded.Height,
                CanonicalRgbSha256 = digest,
                Signature = signature.ToArray(),
                SignatureStep = step,
                SignatureMarginX = marginX,
                SignatureMarginY = marginY,
                SignatureColumns =
                    (decoded.Width - 2 * marginX + step - 1) / step,
                SignatureRows =
                    (decoded.Height - 2 * marginY + step - 1) / step
            };
        }
    }

    public static double ChangedPercent(
        XpBitmapVisualEvidence left,
        XpBitmapVisualEvidence right,
        int rgbDistanceThreshold)
    {
        if (left == null || right == null ||
            left.Width != right.Width || left.Height != right.Height ||
            left.Signature == null || right.Signature == null ||
            left.Signature.Length != right.Signature.Length ||
            left.Signature.Length == 0)
            return 0.0;
        int changed = 0;
        int pixels = left.Signature.Length / 3;
        for (int offset = 0; offset < left.Signature.Length; offset += 3)
        {
            int distance =
                Math.Abs((int)left.Signature[offset] -
                         (int)right.Signature[offset]) +
                Math.Abs((int)left.Signature[offset + 1] -
                         (int)right.Signature[offset + 1]) +
                Math.Abs((int)left.Signature[offset + 2] -
                         (int)right.Signature[offset + 2]);
            if (distance >= rgbDistanceThreshold) ++changed;
        }
        return 100.0 * changed / pixels;
    }

    public static byte[] LoadDownscaledRgb(string path, int width, int height)
    {
        if (width < 1) throw new ArgumentOutOfRangeException("width");
        if (height < 1) throw new ArgumentOutOfRangeException("height");
        using (Bitmap source = new Bitmap(path))
        using (Bitmap scaled = new Bitmap(
            width, height, PixelFormat.Format24bppRgb))
        {
            using (Graphics graphics = Graphics.FromImage(scaled))
            {
                graphics.InterpolationMode =
                    System.Drawing.Drawing2D.InterpolationMode.Bilinear;
                graphics.PixelOffsetMode =
                    System.Drawing.Drawing2D.PixelOffsetMode.Half;
                graphics.DrawImage(
                    source, new Rectangle(0, 0, width, height));
            }
            Rectangle rectangle = new Rectangle(0, 0, width, height);
            BitmapData data = scaled.LockBits(
                rectangle, ImageLockMode.ReadOnly,
                PixelFormat.Format24bppRgb);
            byte[] rgb = new byte[width * height * 3];
            try
            {
                int rowBytes = width * 3;
                byte[] row = new byte[rowBytes];
                for (int y = 0; y < height; ++y)
                {
                    Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride),
                                 row, 0, rowBytes);
                    int destination = y * rowBytes;
                    for (int x = 0; x < width; ++x)
                    {
                        int sourceOffset = x * 3;
                        rgb[destination + sourceOffset] =
                            row[sourceOffset + 2];
                        rgb[destination + sourceOffset + 1] =
                            row[sourceOffset + 1];
                        rgb[destination + sourceOffset + 2] =
                            row[sourceOffset];
                    }
                }
            }
            finally
            {
                scaled.UnlockBits(data);
            }
            return rgb;
        }
    }

    public static double MeanAbsoluteDifference(
        byte[] left, int leftOffset, byte[] right)
    {
        if (left == null || right == null || right.Length == 0 ||
            leftOffset < 0 ||
            (long)leftOffset + right.Length > left.LongLength)
            return double.MaxValue;
        long total = 0;
        for (int offset = 0; offset < right.Length; ++offset)
            total += Math.Abs(
                (int)left[leftOffset + offset] - (int)right[offset]);
        return (double)total / right.Length;
    }

    public static int BestMatchFrame(
        byte[] frames,
        int frameLength,
        int frameCount,
        byte[] sample,
        out double bestDifference)
    {
        bestDifference = double.MaxValue;
        int best = -1;
        if (frames == null || sample == null || frameLength < sample.Length ||
            frameCount < 1 ||
            frames.LongLength < (long)frameLength * frameCount)
            return best;
        for (int frame = 0; frame < frameCount; ++frame)
        {
            double difference = MeanAbsoluteDifference(
                frames, frame * frameLength, sample);
            if (difference < bestDifference)
            {
                bestDifference = difference;
                best = frame;
            }
        }
        return best;
    }

    public static XpUT2003MenuVisualEvidence AnalyzeUT2003Menu(
        XpBitmapVisualEvidence evidence)
    {
        XpUT2003MenuVisualEvidence result =
            new XpUT2003MenuVisualEvidence();
        if (evidence == null || evidence.Width != 640 ||
            evidence.Height != 480 || evidence.SignatureStep != 8 ||
            evidence.SignatureMarginX != 16 ||
            evidence.SignatureMarginY != 16 ||
            evidence.SignatureColumns != 76 ||
            evidence.SignatureRows != 56 ||
            evidence.Signature == null ||
            evidence.Signature.Length !=
                evidence.SignatureColumns * evidence.SignatureRows * 3)
            return result;

        int offset = 0;
        for (int row = 0; row < evidence.SignatureRows; ++row)
        {
            int y = evidence.SignatureMarginY +
                row * evidence.SignatureStep;
            for (int column = 0;
                 column < evidence.SignatureColumns;
                 ++column, offset += 3)
            {
                int x = evidence.SignatureMarginX +
                    column * evidence.SignatureStep;
                if (x < 60 || x >= 580) continue;
                int red = evidence.Signature[offset];
                int green = evidence.Signature[offset + 1];
                int blue = evidence.Signature[offset + 2];
                bool bright = red + green + blue >= 540 &&
                    Math.Max(red, Math.Max(green, blue)) >= 190;
                bool gold = red >= 120 && green >= 80 &&
                    red >= green && green * 4 >= blue * 5 &&
                    red + blue < 360;

                if (y >= 15 && y < 190)
                {
                    if (bright) ++result.TitleBright;
                    if (gold) ++result.TitleGold;
                }
                else if (y >= 242 && y < 274)
                {
                    if (gold) ++result.HighlightGold;
                }
                else if (y >= 278 && y < 306)
                {
                    if (bright) ++result.HostRowBright;
                }
                else if (y >= 315 && y < 345)
                {
                    if (bright) ++result.InstantRowBright;
                }
                else if (y >= 350 && y < 380)
                {
                    if (bright) ++result.SettingsRowBright;
                }
            }
        }
        result.Valid = result.TitleBright >= 70 &&
            result.TitleGold >= 15 &&
            result.HighlightGold >= 15 &&
            result.HostRowBright >= 20 &&
            result.InstantRowBright >= 10 &&
            result.SettingsRowBright >= 5;
        return result;
    }
}
'@
}

function Get-XpCanonicalBitmapVisualEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [ValidateRange(1, 64)]
        [int]$SignatureStep = 8
    )

    return [XpBitmapVisual]::Analyze(
        [IO.Path]::GetFullPath($Path), $SignatureStep)
}

function Test-XpUT2003MainMenuCoverage {
    param([XpBitmapVisualEvidence[]]$VisualEvidence)

    $requiredFrames = 6
    $qualifiedFrames = 0
    $menuEvidence = [Collections.Generic.List[object]]::new()
    foreach ($evidence in @($VisualEvidence)) {
        $menu = [XpBitmapVisual]::AnalyzeUT2003Menu($evidence)
        $menuEvidence.Add($menu)
        if ($menu.Valid) { ++$qualifiedFrames }
    }
    return [pscustomobject][ordered]@{
        valid = @($VisualEvidence).Count -eq 8 -and
            $qualifiedFrames -ge $requiredFrames
        selectedCount = @($VisualEvidence).Count
        qualifyingFrames = $qualifiedFrames
        requiredFrames = $requiredFrames
        evidence = @($menuEvidence)
    }
}

function Select-XpGameObserveSampleFrames {
    param(
        [IO.FileInfo[]]$Frames,
        [ValidateRange(1, 64)]
        [int]$Required = 8
    )

    Add-Type -AssemblyName System.Drawing
    $qualified = @($Frames | Sort-Object LastWriteTimeUtc, Name |
        Where-Object {
            $bitmap = $null
            try {
                $bitmap = [Drawing.Bitmap]::new($_.FullName)
                $assessment = Get-XpGameObserveBitmapAssessment `
                    -Bitmap $bitmap
                return $assessment.valid -and $assessment.rich -and
                    -not $assessment.desktop
            } catch {
                return $false
            } finally {
                if ($bitmap) { $bitmap.Dispose() }
            }
        })
    if ($qualified.Count -lt $Required) { return @() }

    return @(for ($index = 0; $index -lt $Required; ++$index) {
        $frameIndex = if ($Required -eq 1) { 0 } else {
            [int][Math]::Round(
                $index * ($qualified.Count - 1) / ($Required - 1.0))
        }
        $qualified[$frameIndex]
    })
}

function Test-XpGameObserveFrameInterval {
    param(
        [Parameter(Mandatory = $true)]
        [IO.FileInfo]$Frame,
        [DateTime]$StartedAt = [DateTime]::MinValue,
        [DateTime]$CompletedAt = [DateTime]::MaxValue,
        [int]$StartSequence = -1,
        [int]$EndSequence = -1,
        [switch]$StrictStartTime
    )

    if ($Frame.Name -notmatch '^frame_\d+x\d+_(\d{6})\.bmp$') {
        return $false
    }
    $sequence = [int]$Matches[1]
    if ($StartedAt -eq [DateTime]::MinValue) {
        return $sequence -gt $StartSequence -and
            ($EndSequence -lt 0 -or $sequence -le $EndSequence)
    }
    $notBefore = if ($StrictStartTime) {
        $StartedAt
    } else { $StartedAt.AddSeconds(-1) }
    return ($StartSequence -lt 0 -or $sequence -gt $StartSequence) -and
        ($EndSequence -lt 0 -or $sequence -le $EndSequence) -and
        $Frame.LastWriteTime -ge $notBefore -and
        ($CompletedAt -eq [DateTime]::MaxValue -or
         $Frame.LastWriteTime -le $CompletedAt.AddSeconds(2))
}

function Test-XpUT2003VisualFrameCoverage {
    param(
        [IO.FileInfo[]]$SelectedFrames,
        [ValidateRange(1, 86400000)]
        [int64]$RequiredObservedMs = 180000
    )

    $requiredCount = 8
    $requiredSpanSeconds = [Math]::Max(
        1.0, ($RequiredObservedMs / 1000.0) - 30.0)
    $maximumAllowedGapSeconds = 45.0
    $requiredCanonicalDistinctHashes = 4
    $requiredMeaningfulTransitions = 3
    $minimumChangedPercent = 0.05
    $rgbDistanceThreshold = 48
    $ordered = @($SelectedFrames | Sort-Object LastWriteTimeUtc, Name)
    $visualEvidence = [Collections.Generic.List[object]]::new()
    foreach ($frame in $ordered) {
        try {
            $visualEvidence.Add(
                (Get-XpCanonicalBitmapVisualEvidence `
                    -Path $frame.FullName -SignatureStep 8))
        } catch { }
    }
    $canonicalDistinctHashes = @($visualEvidence |
        Select-Object -ExpandProperty CanonicalRgbSha256 -Unique).Count
    $meaningfulTransitions = 0
    $maximumChangedPercent = 0.0
    for ($index = 1; $index -lt $visualEvidence.Count; ++$index) {
        $changedPercent = [XpBitmapVisual]::ChangedPercent(
            $visualEvidence[$index - 1], $visualEvidence[$index],
            $rgbDistanceThreshold)
        $maximumChangedPercent = [Math]::Max(
            $maximumChangedPercent, $changedPercent)
        if ($changedPercent -ge $minimumChangedPercent) {
            ++$meaningfulTransitions
        }
    }
    $spanSeconds = 0.0
    $maximumGapSeconds = 0.0
    if ($ordered.Count -ge 2) {
        $spanSeconds = [Math]::Max(0.0,
            ($ordered[-1].LastWriteTimeUtc -
             $ordered[0].LastWriteTimeUtc).TotalSeconds)
        for ($index = 1; $index -lt $ordered.Count; ++$index) {
            $gap = [Math]::Max(0.0,
                ($ordered[$index].LastWriteTimeUtc -
                 $ordered[$index - 1].LastWriteTimeUtc).TotalSeconds)
            $maximumGapSeconds = [Math]::Max($maximumGapSeconds, $gap)
        }
    }
    return [pscustomobject][ordered]@{
        valid = $ordered.Count -eq $requiredCount -and
            $visualEvidence.Count -eq $requiredCount -and
            $spanSeconds -ge $requiredSpanSeconds -and
            $maximumGapSeconds -le $maximumAllowedGapSeconds -and
            $canonicalDistinctHashes -ge
                $requiredCanonicalDistinctHashes -and
            $meaningfulTransitions -ge $requiredMeaningfulTransitions
        selectedCount = $ordered.Count
        decodedCount = $visualEvidence.Count
        canonicalDistinctHashes = $canonicalDistinctHashes
        requiredCanonicalDistinctHashes = $requiredCanonicalDistinctHashes
        meaningfulTransitions = $meaningfulTransitions
        requiredMeaningfulTransitions = $requiredMeaningfulTransitions
        minimumChangedPercent = $minimumChangedPercent
        maximumChangedPercent = [Math]::Round($maximumChangedPercent, 3)
        spanSeconds = [Math]::Round($spanSeconds, 3)
        requiredSpanSeconds = [Math]::Round($requiredSpanSeconds, 3)
        maximumGapSeconds = [Math]::Round($maximumGapSeconds, 3)
        maximumAllowedGapSeconds = $maximumAllowedGapSeconds
        visualEvidence = @($visualEvidence)
    }
}

function ConvertFrom-XpManifestTextBytes {
    param([byte[]]$Bytes)

    if ($null -eq $Bytes) { return '' }
    if ($Bytes.Length -ge 4 -and
        $Bytes[0] -eq 0xff -and $Bytes[1] -eq 0xfe -and
        $Bytes[2] -eq 0x00 -and $Bytes[3] -eq 0x00) {
        return [Text.Encoding]::UTF32.GetString(
            $Bytes, 4, $Bytes.Length - 4)
    }
    if ($Bytes.Length -ge 4 -and
        $Bytes[0] -eq 0x00 -and $Bytes[1] -eq 0x00 -and
        $Bytes[2] -eq 0xfe -and $Bytes[3] -eq 0xff) {
        $utf32Be = [Text.UTF32Encoding]::new($true, $true)
        return $utf32Be.GetString($Bytes, 4, $Bytes.Length - 4)
    }
    if ($Bytes.Length -ge 2 -and
        $Bytes[0] -eq 0xff -and $Bytes[1] -eq 0xfe) {
        return [Text.Encoding]::Unicode.GetString(
            $Bytes, 2, $Bytes.Length - 2)
    }
    if ($Bytes.Length -ge 2 -and
        $Bytes[0] -eq 0xfe -and $Bytes[1] -eq 0xff) {
        return [Text.Encoding]::BigEndianUnicode.GetString(
            $Bytes, 2, $Bytes.Length - 2)
    }
    if ($Bytes.Length -ge 3 -and
        $Bytes[0] -eq 0xef -and $Bytes[1] -eq 0xbb -and
        $Bytes[2] -eq 0xbf) {
        return [Text.Encoding]::UTF8.GetString(
            $Bytes, 3, $Bytes.Length - 3)
    }
    return [Text.Encoding]::UTF8.GetString($Bytes)
}

function Remove-XpUnpublishedReviewVideo {
    param(
        [string]$Path,
        [bool]$Published
    )

    if (-not $Published -and
        -not [string]::IsNullOrWhiteSpace($Path)) {
        Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    }
}

function Publish-XpValidatedReviewVideo {
    param(
        [string]$Destination,
        [scriptblock]$HashAction = {
            param([string]$Path)
            (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        },
        [scriptblock]$MoveAction = {
            param([string]$Source, [string]$Target)
            Move-Item -LiteralPath $Source -Destination $Target -Force
        }
    )

    try {
        $hash = [string](& $HashAction $Destination)
        if ($hash -notmatch '^[0-9A-Fa-f]{64}$') {
            throw 'Review-video hash action returned an invalid SHA-256'
        }
        $digest = $hash.Substring(0, 12).ToLowerInvariant()
        $hashedDestination = Join-Path (Split-Path -Parent $Destination) `
            ("{0}-{1}{2}" -f
                [IO.Path]::GetFileNameWithoutExtension($Destination),
                $digest, [IO.Path]::GetExtension($Destination))
        [void](& $MoveAction $Destination $hashedDestination)
        if (-not (Test-Path -LiteralPath $hashedDestination -PathType Leaf) -or
            (Test-Path -LiteralPath $Destination)) {
            throw 'Review-video publication did not atomically rename the file'
        }
        return [pscustomobject][ordered]@{
            path = $hashedDestination
            sha256 = $hash.ToUpperInvariant()
        }
    } catch {
        Remove-XpUnpublishedReviewVideo `
            -Path $Destination -Published $false
        throw
    }
}

function Test-XpGameObserveEncodedVideoTiming {
    param(
        [int]$ExpectedFrames,
        [int]$EncodedFrames,
        [int]$FrameRate,
        [string]$NominalFrameRate,
        [string]$AverageFrameRate,
        [double]$DurationSeconds
    )

    if ($ExpectedFrames -lt 2 -or $FrameRate -le 0 -or
        $EncodedFrames -ne $ExpectedFrames -or
        $NominalFrameRate -cne "$FrameRate/1" -or
        $AverageFrameRate -cne "$FrameRate/1" -or
        [double]::IsNaN($DurationSeconds) -or
        [double]::IsInfinity($DurationSeconds)) {
        return $false
    }
    return [Math]::Abs(
        $DurationSeconds - ($ExpectedFrames / [double]$FrameRate)) -le 0.001
}

function Get-XpGameObserveVideoProbe {
    param([string]$Path)

    $workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $ffprobe = Join-Path $workspace `
        '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin\ffprobe.exe'
    if (-not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
        throw 'Pinned FFprobe is missing'
    }
    $probeOutput = @(& $ffprobe -v error -count_frames `
        -select_streams 'v:0' `
        -show_entries `
            'stream=width,height,nb_read_frames,r_frame_rate,avg_frame_rate:format=duration' `
        -of json $Path 2>&1)
    if ($LASTEXITCODE -ne 0 -or -not $probeOutput.Count) {
        throw "FFprobe failed with exit code $LASTEXITCODE"
    }
    try {
        $probe = ($probeOutput -join [Environment]::NewLine) |
            ConvertFrom-Json
    } catch {
        throw "FFprobe returned invalid JSON: $($_.Exception.Message)"
    }
    $streams = @($probe.streams)
    $frames = 0
    $width = 0
    $height = 0
    $duration = 0.0
    if ($streams.Count -ne 1 -or
        -not [int]::TryParse(
            [string]$streams[0].nb_read_frames, [ref]$frames) -or
        -not [int]::TryParse([string]$streams[0].width, [ref]$width) -or
        -not [int]::TryParse([string]$streams[0].height, [ref]$height) -or
        -not [double]::TryParse(
            [string]$probe.format.duration,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$duration)) {
        throw 'FFprobe returned incomplete video metadata'
    }
    return [pscustomobject][ordered]@{
        encodedFrames = $frames
        width = $width
        height = $height
        nominalFrameRate = [string]$streams[0].r_frame_rate
        averageFrameRate = [string]$streams[0].avg_frame_rate
        durationSeconds = $duration
    }
}
function Get-XpGameObserveDownscaledVideoFrames {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [ValidateRange(16, 320)]
        [int]$Width = 160,
        [ValidateRange(16, 240)]
        [int]$Height = 120
    )

    $workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $ffmpeg = Join-Path $workspace `
        '.tools\ffmpeg-8.0.1\ffmpeg-8.0.1-essentials_build\bin\ffmpeg.exe'
    if (-not (Test-Path -LiteralPath $ffmpeg -PathType Leaf)) {
        throw 'Pinned FFmpeg is missing'
    }
    $temporary = Join-Path ([IO.Path]::GetTempPath()) `
        "xpdriver-video-binding-$PID-$([Guid]::NewGuid().ToString('N')).raw"
    try {
        $ffmpegOutput = @(& $ffmpeg -hide_banner -loglevel error -y `
            -i $Path -vf "scale=${Width}:${Height}" `
            -f rawvideo -pix_fmt rgb24 $temporary 2>&1)
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $temporary -PathType Leaf)) {
            throw "FFmpeg frame decode failed with exit code " +
                "$LASTEXITCODE`: $($ffmpegOutput -join ' ')"
        }
        $bytes = [IO.File]::ReadAllBytes($temporary)
    } finally {
        Remove-Item -LiteralPath $temporary -Force `
            -ErrorAction SilentlyContinue
    }
    $frameLength = $Width * $Height * 3
    if ($bytes.LongLength -lt $frameLength -or
        ($bytes.LongLength % $frameLength) -ne 0) {
        throw "FFmpeg decoded $($bytes.LongLength) bytes, not a whole " +
            'number of downscaled frames'
    }
    return [pscustomobject][ordered]@{
        bytes = $bytes
        width = $Width
        height = $Height
        frameLength = $frameLength
        frameCount = [int]($bytes.LongLength / $frameLength)
    }
}

function Test-XpGameObserveVideoPixelBinding {
    param(
        [ValidateSet(
            'UT2003Visual', 'UT2003Gameplay', 'NFSU2Visual',
            'Warcraft3D3D8Visual')]
        [string]$Mode,
        [Parameter(Mandatory = $true)]
        [string]$VideoPath,
        [Parameter(Mandatory = $true)]
        [string[]]$SamplePaths,
        [ValidateRange(2, 1000000)]
        [int]$ExpectedFrames,
        [double]$MaximumMeanAbsoluteDifference = 10.0,
        [DateTime]$ObservationStartUtc = [DateTime]::MinValue,
        [double]$VideoDurationSeconds = 0,
        [DateTime[]]$SampleCapturedUtc = @()
    )

    # Bind decoded review-video pixels to the manifested source frames.
    # Warcraft encodes the 256-frame ring non-decimated, so the video frame
    # at each deterministic sample position must match that sample best.
    # UT videos resample the observed frame sequence onto a fixed 15 fps
    # timeline covering at most the first 45 seconds of the observation, so
    # every manifested sample whose capture time provably falls inside the
    # encoded window (capture <= observation start + video duration) must
    # appear in the video in sequence order; the timing model makes no
    # claim about later samples.  Lossy encoding noise is bounded by the
    # mean absolute difference cap; unrelated or constant content misses it
    # by an order of magnitude.
    $findings = [Collections.Generic.List[string]]::new()
    $matchedFrames = [Collections.Generic.List[int]]::new()
    $maximumObserved = 0.0
    $decoded = Get-XpGameObserveDownscaledVideoFrames -Path $VideoPath
    if ($decoded.frameCount -ne $ExpectedFrames) {
        $findings.Add('decoded review video frame count ' +
            "$($decoded.frameCount) does not match the manifested " +
            "timing model $ExpectedFrames")
    }
    $samplePixels = @()
    foreach ($samplePath in @($SamplePaths)) {
        $samplePixels += ,([XpBitmapVisual]::LoadDownscaledRgb(
            [IO.Path]::GetFullPath($samplePath),
            $decoded.width, $decoded.height))
    }
    if ($samplePixels.Count -ne 8) {
        $findings.Add('review video pixel binding requires exactly eight ' +
            "manifested samples, found $($samplePixels.Count)")
    }
    if (-not $findings.Count) {
        if ($Mode -eq 'Warcraft3D3D8Visual') {
            for ($index = 0; $index -lt 8; ++$index) {
                $position = [int][Math]::Round(
                    $index * ($decoded.frameCount - 1) / 7.0)
                $frameOffset = $position * $decoded.frameLength
                $self = [XpBitmapVisual]::MeanAbsoluteDifference(
                    $decoded.bytes, $frameOffset, $samplePixels[$index])
                $nearestOther = [double]::MaxValue
                for ($other = 0; $other -lt 8; ++$other) {
                    if ($other -eq $index) { continue }
                    $nearestOther = [Math]::Min($nearestOther,
                        [XpBitmapVisual]::MeanAbsoluteDifference(
                            $decoded.bytes, $frameOffset,
                            $samplePixels[$other]))
                }
                $maximumObserved = [Math]::Max($maximumObserved, $self)
                if ($self -gt $MaximumMeanAbsoluteDifference) {
                    $findings.Add("review video frame $position does not " +
                        "match manifested sample $index pixels")
                } elseif ($self -gt ($nearestOther + 0.001)) {
                    $findings.Add("review video frame $position matches " +
                        "manifested sample $index worse than another " +
                        'manifested sample')
                }
                $matchedFrames.Add($position)
            }
        } else {
            $windowEndUtc = [DateTime]::MaxValue
            if ($ObservationStartUtc -gt [DateTime]::MinValue -and
                $VideoDurationSeconds -gt 0) {
                $windowEndUtc = $ObservationStartUtc.AddSeconds(
                    $VideoDurationSeconds)
            }
            $capturedTimes = @($SampleCapturedUtc)
            $previous = -1
            for ($index = 0; $index -lt 8; ++$index) {
                $capturedUtc = if ($index -lt $capturedTimes.Count) {
                    $capturedTimes[$index]
                } else { [DateTime]::MinValue }
                if ($windowEndUtc -ne [DateTime]::MaxValue -and
                    $capturedUtc -gt $windowEndUtc) {
                    # The production timing model encodes at most the first
                    # 45 seconds of the observation; later manifested
                    # samples make no claim on this video.
                    continue
                }
                $bestDifference = 0.0
                $bestFrame = [XpBitmapVisual]::BestMatchFrame(
                    $decoded.bytes, $decoded.frameLength,
                    $decoded.frameCount, $samplePixels[$index],
                    [ref]$bestDifference)
                $maximumObserved = [Math]::Max(
                    $maximumObserved, $bestDifference)
                if ($bestFrame -lt 0 -or
                    $bestDifference -gt $MaximumMeanAbsoluteDifference) {
                    $findings.Add('no decoded review video frame matches ' +
                        "manifested sample $index pixels")
                } elseif ($bestFrame -lt $previous) {
                    $findings.Add("manifested sample $index appears out " +
                        'of sequence order in the review video')
                } else {
                    $previous = $bestFrame
                }
                $matchedFrames.Add($bestFrame)
            }
        }
    }
    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        findings = @($findings)
        matchedFrames = @($matchedFrames)
        maximumMeanAbsoluteDifference = [Math]::Round($maximumObserved, 3)
    }
}


function Add-XpGameObserveUniqueMessage {
    param(
        [Collections.Generic.List[string]]$Messages,
        [string]$Message
    )

    if ($null -eq $Messages -or [string]::IsNullOrWhiteSpace($Message) -or
        $Messages.Contains($Message)) { return $false }
    $Messages.Add($Message)
    return $true
}

function Test-XpUT2003VisualGuestContract {
    param(
        [string[]]$ReadyMessages,
        [string[]]$StageMessages,
        [string[]]$ResultMessages
    )

    $findings = [Collections.Generic.List[string]]::new()
    $ready = $null
    $stage = $null
    $result = $null
    if (@($ReadyMessages).Count -ne 1) {
        $findings.Add('expected exactly one unique visual-ready stage')
    } else {
        $ready = [regex]::Match([string]$ReadyMessages[0],
            '^QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-ready boundary=foreground-d3d8-window-stable warmup-ms=30000$')
        if (-not $ready.Success) {
            $findings.Add(
                'visual-ready stage does not match exact UT2003 contract')
        }
    }
    if (@($StageMessages).Count -ne 1) {
        $findings.Add('expected exactly one unique visual-complete stage')
    } else {
        $stage = [regex]::Match([string]$StageMessages[0],
            '^QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003 stage=visual-complete observed-ms=(\d+) required-ms=180000 renderer=d3d8 system32=1 boundary=post-visual-ready$')
        if (-not $stage.Success) {
            $findings.Add(
                'visual-complete stage does not match exact UT2003 contract')
        }
    }
    if (@($ResultMessages).Count -ne 1) {
        $findings.Add('expected exactly one unique UT2003 terminal result')
    } else {
        $result = [regex]::Match([string]$ResultMessages[0],
            '^QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003 stage=complete outcome=observed exit=259 modules=([0-9A-Fa-f]+) observed-ms=(\d+)$')
        if (-not $result.Success) {
            $findings.Add(
                'terminal result does not match exact UT2003 contract')
        }
    }
    $observedMs = [int64]-1
    if ($stage -and $stage.Success) {
        $parsed = [int64]0
        if (-not [int64]::TryParse(
                $stage.Groups[1].Value, [ref]$parsed) -or
            $parsed -lt 180000) {
            $findings.Add(
                'visual-complete duration is shorter than 180000 ms')
        } else { $observedMs = $parsed }
    }
    if ($result -and $result.Success) {
        $moduleMask = [uint32]0
        if (-not [uint32]::TryParse(
                $result.Groups[1].Value,
                [Globalization.NumberStyles]::HexNumber,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$moduleMask) -or ($moduleMask -band 1) -eq 0) {
            $findings.Add('terminal result lacks the D3D8 module bit')
        }
        $resultMs = [int64]0
        if (-not [int64]::TryParse(
                $result.Groups[2].Value, [ref]$resultMs) -or
            $resultMs -lt 180000) {
            $findings.Add('terminal duration is shorter than 180000 ms')
        } elseif ($observedMs -ge 0 -and $resultMs -ne $observedMs) {
            $findings.Add('stage and terminal durations disagree')
        }
    }
    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        observedMs = $observedMs
        visualBoundary = if ($ready -and $ready.Success) {
            'foreground-d3d8-window-stable'
        } else { $null }
        findings = @($findings)
    }
}

function Test-XpUT2003GameplayGuestContract {
    param([string]$Console)

    $findings = [Collections.Generic.List[string]]::new()
    $semanticLines = [regex]::Matches($Console,
        '(?m)^GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=semantic transition=dm-antalus-ready evidence=browse-load-deathmatch-level-play\r?$')
    if ($semanticLines.Count -ne 1) {
        $findings.Add(
            'expected exactly one DM-Antalus gameplay semantic stage')
    }
    $inputLines = [regex]::Matches($Console,
        '(?m)^GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=input action=fire attempt=([1-3]) injected=2 foreground=1 ready=1\r?$')
    if ($inputLines.Count -ne 3) {
        $findings.Add('expected three accepted foreground fire attempts')
    } else {
        for ($index = 0; $index -lt 3; ++$index) {
            if ([int]$inputLines[$index].Groups[1].Value -ne $index + 1) {
                $findings.Add('fire attempts are not ordered exactly 1 through 3')
                break
            }
        }
    }
    $hostInputLines = [regex]::Matches($Console,
        '(?m)^GuestInput=mouse-left ok=True\r?$')
    if ($hostInputLines.Count -ne 3) {
        $findings.Add('expected three response-checked host mouse inputs')
    }
    $moduleLines = [regex]::Matches($Console,
        '(?m)^GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=ut2003gameplay stage=module name=d3d8\.dll path=(?i:C:\\WINDOWS\\system32\\d3d8\.dll) system32=1\r?$')
    if ($moduleLines.Count -ne 1) {
        $findings.Add('canonical System32 D3D8 module evidence is missing')
    }
    $resultLines = [regex]::Matches($Console,
        '(?m)^GuestResult=QXP_GAME_OBSERVE_RESULT rc=0 game=ut2003gameplay stage=complete outcome=observed exit=259 modules=([0-9A-Fa-f]+) observed-ms=(\d+)\r?$')
    $observedMs = [int64]-1
    if ($resultLines.Count -ne 1) {
        $findings.Add('expected exactly one UT2003 gameplay terminal result')
    } else {
        $moduleMask = [uint32]0
        if (-not [uint32]::TryParse(
                $resultLines[0].Groups[1].Value,
                [Globalization.NumberStyles]::HexNumber,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$moduleMask) -or ($moduleMask -band 1) -eq 0) {
            $findings.Add('gameplay terminal result lacks the D3D8 module bit')
        }
        $parsedMs = [int64]0
        if (-not [int64]::TryParse(
                $resultLines[0].Groups[2].Value, [ref]$parsedMs) -or
            $parsedMs -lt 45000) {
            $findings.Add('gameplay observation is shorter than 45000 ms')
        } else { $observedMs = $parsedMs }
    }
    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        observedMs = $observedMs
        semanticTransition = if ($semanticLines.Count -eq 1) {
            'dm-antalus-ready'
        } else { $null }
        findings = @($findings)
    }
}

function Test-XpGameObserveVisualBundleCore {
    param(
        [string]$Directory,
        [ValidateSet(
            'UT2003Visual', 'UT2003Gameplay', 'NFSU2Visual',
            'Warcraft3D3D8Visual')]
        [string]$Mode,
        [scriptblock]$VideoProbeAction
    )

    if ($null -eq $VideoProbeAction) {
        $VideoProbeAction = {
            param([string]$Path)
            Get-XpGameObserveVideoProbe -Path $Path
        }
    }

    $findings = [Collections.Generic.List[string]]::new()
    $harness = $null
    $manifest = $null
    $harnessBytes = $null
    $consoleBytes = $null
    $console = ''
    foreach ($spec in @(
            @('harness-result.json', 'harness result'),
            @('artifact-manifest.json', 'artifact manifest'),
            @('console.log', 'console log'))) {
        $path = Resolve-XpBundleConfinedPath `
            -BundleRoot $Directory -Name $spec[0]
        if (-not $path) {
            $findings.Add("$($spec[1]) is a reparse point or escapes " +
                'the canonical bundle')
            continue
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $findings.Add("missing $($spec[1])")
            continue
        }
        try {
            if ($spec[0] -eq 'harness-result.json') {
                $harnessBytes = [IO.File]::ReadAllBytes($path)
            } elseif ($spec[0] -eq 'artifact-manifest.json') {
                $manifest = Get-Content -Raw -LiteralPath $path |
                    ConvertFrom-Json
            } else {
                $consoleBytes = [IO.File]::ReadAllBytes($path)
            }
        } catch {
            $findings.Add("invalid $($spec[1]): $($_.Exception.Message)")
        }
    }
    $harnessRecords = @(if ($manifest) {
        $manifest.files | Where-Object {
            [string]$_.name -ceq 'harness-result.json'
        }
    })
    if ($harnessRecords.Count -ne 1) {
        $findings.Add('harness result is not uniquely manifested')
    } elseif ($null -ne $harnessBytes) {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $harnessHash = [BitConverter]::ToString(
                $sha256.ComputeHash($harnessBytes)).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
        if ($harnessBytes.LongLength -ne
                [int64]$harnessRecords[0].size -or
            $harnessHash -cne [string]$harnessRecords[0].sha256) {
            $findings.Add('harness result manifest size/hash mismatch')
        } else {
            try {
                $harness = ConvertFrom-XpManifestTextBytes `
                    -Bytes $harnessBytes | ConvertFrom-Json
            } catch {
                $findings.Add(
                    "invalid harness result: $($_.Exception.Message)")
            }
        }
    }
    if ($harness -and ($harness.result -cne 'pass' -or
            $harness.harnessStatus -cne 'passed' -or
            [int]$harness.exitCode -ne 0 -or -not [bool]$harness.capture)) {
        $findings.Add('harness result is not a passing capture run')
    }
    if ($harness -and $manifest -and
        $harness.runId -cne $manifest.runId) {
        $findings.Add('manifest runId does not match harness result')
    }
    $consoleRecords = @(if ($manifest) {
        $manifest.files | Where-Object {
            [string]$_.name -ceq 'console.log'
        }
    })
    if ($consoleRecords.Count -ne 1) {
        $findings.Add('console log is not uniquely manifested')
    } elseif ($null -ne $consoleBytes) {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            $consoleHash = [BitConverter]::ToString(
                $sha256.ComputeHash($consoleBytes)).Replace('-', '')
        } finally {
            $sha256.Dispose()
        }
        if ($consoleBytes.LongLength -ne [int64]$consoleRecords[0].size -or
            $consoleHash -cne [string]$consoleRecords[0].sha256) {
            $findings.Add('console log manifest size/hash mismatch')
        } else {
            # Parse only the immutable byte snapshot whose size and digest
            # matched the unique manifest record above.
            $console = ConvertFrom-XpManifestTextBytes -Bytes $consoleBytes
        }
    }

    $prefix = if ($Mode -eq 'UT2003Visual') {
        'xpdm-ut2003'
    } elseif ($Mode -eq 'UT2003Gameplay') {
        'xpdm-ut2003gameplay'
    } elseif ($Mode -eq 'NFSU2Visual') {
        'xpdm-nfsu2'
    } else { 'xpdm-warcraft3d3d8' }
    $expectedWidth = if ($Mode -eq 'Warcraft3D3D8Visual') {
        800
    } else { 640 }
    $expectedHeight = if ($Mode -eq 'Warcraft3D3D8Visual') {
        600
    } else { 480 }
    $samplePattern = '^' + [regex]::Escape($prefix) +
        '-sample-(\d+)-([0-9a-f]{12})\.bmp$'
    $videoPattern = '^' + [regex]::Escape($prefix) +
        '-review-([0-9a-f]{12})\.mp4$'
    $sampleRecords = @(if ($manifest) {
        $manifest.files | Where-Object {
            [string]$_.name -cmatch $samplePattern
        } | Sort-Object name
    })
    $videoRecords = @(if ($manifest) {
        $manifest.files | Where-Object {
            [string]$_.name -cmatch $videoPattern
        }
    })
    if ($sampleRecords.Count -ne 8) {
        $findings.Add("expected 8 retained samples, found $($sampleRecords.Count)")
    }
    if ($videoRecords.Count -ne 1) {
        $findings.Add("expected 1 retained video, found $($videoRecords.Count)")
    }

    $samples = @()
    $samplePaths = [Collections.Generic.List[string]]::new()
    $sampleCapturedUtc = [Collections.Generic.List[DateTime]]::new()
    $sampleVisualEvidence = [Collections.Generic.List[object]]::new()
    $expectedIndex = 0
    foreach ($record in $sampleRecords) {
        $match = [regex]::Match([string]$record.name, $samplePattern)
        if (-not $match.Success -or
            [int]$match.Groups[1].Value -ne $expectedIndex) {
            $findings.Add('retained sample indices are not exactly 0 through 7')
        }
        ++$expectedIndex
        $path = Resolve-XpBundleConfinedPath `
            -BundleRoot $Directory -Name ([string]$record.name)
        if (-not $path) {
            $findings.Add("retained sample is a reparse point or escapes the canonical bundle $($record.name)")
            continue
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $findings.Add("missing retained sample $($record.name)")
            continue
        }
        $samplePaths.Add($path)
        $sampleCapturedUtc.Add((Get-Item -LiteralPath $path).LastWriteTimeUtc)
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -cne [string]$record.sha256 -or
            -not $hash.ToLowerInvariant().StartsWith(
                $match.Groups[2].Value)) {
            $findings.Add("retained sample hash mismatch $($record.name)")
        }
        $canonical = $null
        try {
            $canonical = Get-XpCanonicalBitmapVisualEvidence -Path $path
        } catch {
            $findings.Add(
                "retained sample is not a decodable bitmap $($record.name)")
        }
        if ($canonical -and
            ([int]$canonical.Width -ne $expectedWidth -or
             [int]$canonical.Height -ne $expectedHeight)) {
            $findings.Add("retained sample geometry does not match " +
                "expected ${expectedWidth}x${expectedHeight} $($record.name)")
        }
        if ($canonical) { $sampleVisualEvidence.Add($canonical) }
        $samples += [pscustomobject][ordered]@{
            index = [int]$match.Groups[1].Value
            fileName = [string]$record.name
            sha256 = $hash
            canonicalRgbSha256 = if ($canonical) {
                [string]$canonical.CanonicalRgbSha256
            } else { $null }
            width = if ($canonical) { [int]$canonical.Width } else { 0 }
            height = if ($canonical) { [int]$canonical.Height } else { 0 }
        }
    }

    $videoPath = $null
    $video = $null
    if ($videoRecords.Count -eq 1) {
        $record = $videoRecords[0]
        $match = [regex]::Match([string]$record.name, $videoPattern)
        $path = Resolve-XpBundleConfinedPath `
            -BundleRoot $Directory -Name ([string]$record.name)
        if (-not $path) {
            $findings.Add("retained video is a reparse point or escapes the canonical bundle $($record.name)")
        } elseif (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $findings.Add("missing retained video $($record.name)")
        } else {
            $videoPath = $path
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            if ($hash -cne [string]$record.sha256 -or
                -not $hash.ToLowerInvariant().StartsWith(
                    $match.Groups[1].Value)) {
                $findings.Add("retained video hash mismatch $($record.name)")
            }
            $video = [pscustomobject][ordered]@{
                fileName = [string]$record.name
                sha256 = $hash
            }
        }
    }

    $observedMs = $null
    $semanticTransition = $null
    $visualSpanSeconds = $null
    $visualMaximumGapSeconds = $null
    $visualCanonicalDistinctHashes = $null
    $visualMeaningfulTransitions = $null
    $recomputedCanonicalDistinctHashes = $null
    $recomputedMeaningfulTransitions = $null
    $menuQualifyingFrames = $null
    $recomputedMenuQualifyingFrames = $null
    $readyFrameSequence = $null
    $guardFrameSequence = $null
    $firstAcceptedFrameSequence = $null
    $observationStartUtc = [DateTime]::MinValue
    $cadence = $null
    if ($Mode -eq 'UT2003Visual') {
        $timing = [regex]::Match($console,
            'UT2003VisualTiming valid=True observedMs=(\d+) requiredMs=180000 boundary=post-visual-ready')
        if (-not $timing.Success -or
            [int64]$timing.Groups[1].Value -lt 180000) {
            $findings.Add('UT2003 sustained visual timing is missing or short')
        } else { $observedMs = [int64]$timing.Groups[1].Value }
        $guestContractLines = [regex]::Matches($console,
            '(?m)^UT2003VisualGuestContract valid=True ready=1 stages=1 results=1 findings=0\r?$')
        if ($guestContractLines.Count -ne 1) {
            $findings.Add(
                'UT2003 visual guest contract is missing or non-unique')
        }
        $readyLines = [regex]::Matches($console,
            '(?m)^UT2003VisualBoundary valid=True boundary=foreground-d3d8-window-stable messages=1\r?$')
        if ($readyLines.Count -ne 1) {
            $findings.Add(
                'UT2003 visual-ready boundary is missing or non-unique')
        }
        $boundaryLines = [regex]::Matches($console,
            '(?m)^UT2003CaptureBoundary readySequence=(-?\d+) lastGuardSequence=(-?\d+) firstAcceptedSequence=(-?\d+) guardCount=2 forceSyncReadback=True\r?$')
        if ($boundaryLines.Count -ne 1) {
            $findings.Add('UT2003 synchronous guarded capture boundary is missing or non-unique')
        } else {
            $readyFrameSequence = [int]$boundaryLines[0].Groups[1].Value
            $guardFrameSequence = [int]$boundaryLines[0].Groups[2].Value
            $firstAcceptedFrameSequence =
                [int]$boundaryLines[0].Groups[3].Value
            $expectedGuardSequence =
                [Math]::Max(0, $readyFrameSequence) + 2
            if ($guardFrameSequence -ne $expectedGuardSequence -or
                $firstAcceptedFrameSequence -le $guardFrameSequence) {
                $findings.Add('UT2003 guarded capture sequences are inconsistent')
            }
            $intervalLines = [regex]::Matches($console,
                '(?m)^GameObserveFrames game=UT2003 count=\d+ sequence=(-?\d+)-(-?\d+) time=(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))-(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))\r?$')
            if ($intervalLines.Count -ne 1 -or
                [int]$intervalLines[0].Groups[1].Value -ne
                    $guardFrameSequence) {
                $findings.Add('UT2003 retained frame interval does not start after the guard frame')
            }
            if ($intervalLines.Count -eq 1) {
                $parsedStart = [DateTimeOffset]::MinValue
                if ([DateTimeOffset]::TryParse(
                        $intervalLines[0].Groups[3].Value,
                        [Globalization.CultureInfo]::InvariantCulture,
                        [Globalization.DateTimeStyles]::None,
                        [ref]$parsedStart)) {
                    $observationStartUtc = $parsedStart.UtcDateTime
                } else {
                    $findings.Add('UT2003 retained frame interval start time is invalid')
                }
            }
        }
        $coverageLines = [regex]::Matches($console,
            '(?m)^UT2003VisualCoverage valid=True selected=8 decoded=8 canonicalDistinctHashes=([4-8]) requiredCanonicalDistinctHashes=4 meaningfulTransitions=([3-7]) requiredMeaningfulTransitions=3 minimumChangedPercent=0\.05(?:0+)? maximumChangedPercent=([\d.]+) spanSeconds=([\d.]+) requiredSpanSeconds=150(?:\.0+)? maximumGapSeconds=([\d.]+) maximumAllowedGapSeconds=45(?:\.0+)?\r?$')
        $parsedSpan = 0.0
        $parsedMaximumGap = 0.0
        if ($coverageLines.Count -ne 1 -or
            -not [double]::TryParse(
                $coverageLines[0].Groups[4].Value,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsedSpan) -or
            -not [double]::TryParse(
                $coverageLines[0].Groups[5].Value,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$parsedMaximumGap) -or
            $parsedSpan -lt 150.0 -or $parsedMaximumGap -gt 45.0) {
            $findings.Add('UT2003 sustained rich-frame coverage is missing or invalid')
        } else {
            $visualCanonicalDistinctHashes =
                [int]$coverageLines[0].Groups[1].Value
            $visualMeaningfulTransitions =
                [int]$coverageLines[0].Groups[2].Value
            $visualSpanSeconds = $parsedSpan
            $visualMaximumGapSeconds = $parsedMaximumGap
        }
        $recomputedCanonicalDistinctHashes = @($sampleVisualEvidence |
            Select-Object -ExpandProperty CanonicalRgbSha256 -Unique).Count
        $recomputedMeaningfulTransitions = 0
        for ($index = 1; $index -lt $sampleVisualEvidence.Count; ++$index) {
            $changedPercent = [XpBitmapVisual]::ChangedPercent(
                $sampleVisualEvidence[$index - 1],
                $sampleVisualEvidence[$index], 48)
            if ($changedPercent -ge 0.05) {
                ++$recomputedMeaningfulTransitions
            }
        }
        if ($sampleVisualEvidence.Count -ne 8 -or
            $recomputedCanonicalDistinctHashes -lt 4 -or
            $recomputedMeaningfulTransitions -lt 3) {
            $findings.Add('manifest-bound UT2003 samples fail recomputed ' +
                'canonical-pixel diversity or visible-change requirements')
        } elseif ($null -eq $visualCanonicalDistinctHashes -or
            $null -eq $visualMeaningfulTransitions -or
            $visualCanonicalDistinctHashes -ne
                $recomputedCanonicalDistinctHashes -or
            $visualMeaningfulTransitions -ne
                $recomputedMeaningfulTransitions) {
            $findings.Add('console UT2003 coverage disagrees with recomputed ' +
                'manifest-bound canonical pixels')
        }
        $menuLines = [regex]::Matches($console,
            '(?m)^UT2003MenuVisual valid=True selected=8 qualifyingFrames=([6-8]) requiredFrames=6 signatureStep=8 geometry=640x480\r?$')
        $semanticLines = [regex]::Matches($console,
            '(?m)^UT2003Semantic valid=True transition=main-menu evidence=manifest-bound-menu-layout qualifyingFrames=([6-8]) requiredFrames=6 boundary=post-visual-ready\r?$')
        if ($menuLines.Count -ne 1 -or $semanticLines.Count -ne 1) {
            $findings.Add(
                'UT2003 manifest-bound menu semantic is missing or non-unique')
        } elseif ($menuLines[0].Groups[1].Value -cne
                  $semanticLines[0].Groups[1].Value) {
            $findings.Add(
                'UT2003 menu and semantic qualifying-frame counts disagree')
        } else {
            $menuQualifyingFrames =
                [int]$menuLines[0].Groups[1].Value
        }
        $recomputedMenu = Test-XpUT2003MainMenuCoverage `
            -VisualEvidence @($sampleVisualEvidence)
        $recomputedMenuQualifyingFrames =
            [int]$recomputedMenu.qualifyingFrames
        if (-not $recomputedMenu.valid) {
            $findings.Add('manifest-bound UT2003 samples do not prove ' +
                'the expected main-menu visual layout')
        } elseif ($null -eq $menuQualifyingFrames -or
                  $menuQualifyingFrames -ne
                      $recomputedMenuQualifyingFrames) {
            $findings.Add('console UT2003 menu result disagrees with ' +
                'recomputed manifest-bound pixels')
        } else {
            $semanticTransition = 'main-menu'
        }
        if ($console -notmatch
            'GameObserveVisual=recorded game=UT2003 .*desktop=False') {
            $findings.Add('UT2003 rich non-desktop visual result is missing')
        }
        $videoLine = [regex]::Match($console,
            'ReviewVideo=.* frames=(\d+) fps=(\d+) seconds=([\d.]+) sourceSize=640x480')
        if (-not $videoLine.Success -or
            [int]$videoLine.Groups[1].Value -lt 2 -or
            [int]$videoLine.Groups[2].Value -ne 15) {
            $findings.Add('UT2003 review-video timing result is invalid')
        } elseif ($video) {
            $video | Add-Member -NotePropertyName frames `
                -NotePropertyValue ([int]$videoLine.Groups[1].Value)
            $video | Add-Member -NotePropertyName fps `
                -NotePropertyValue 15
            $video | Add-Member -NotePropertyName durationSeconds `
                -NotePropertyValue ([double]$videoLine.Groups[3].Value)
        }
        if ($console -notmatch
            'ReviewVideoArtifact valid=True sources=1 artifacts=1') {
            $findings.Add('UT2003 review-video artifact binding is invalid')
        }
    } elseif ($Mode -eq 'UT2003Gameplay') {
        $gameplayContract =
            Test-XpUT2003GameplayGuestContract -Console $console
        foreach ($finding in $gameplayContract.findings) {
            $findings.Add($finding)
        }
        $observedMs = $gameplayContract.observedMs
        $semanticTransition = $gameplayContract.semanticTransition
        if ($harness -and
            ($harness.mode -cne '-xpdm-ut2003gameplay' -or
             $harness.captureMode -cne 'video')) {
            $findings.Add(
                'harness result is not the joined-gameplay video mode')
        }
        $frameLines = [regex]::Matches($console,
            '(?m)^GameObserveFrames game=UT2003Gameplay count=(\d+) sequence=(-?\d+)-(-?\d+) time=(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))-(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))\r?$')
        if ($frameLines.Count -ne 1 -or
            [int]$frameLines[0].Groups[1].Value -lt 8 -or
            [int]$frameLines[0].Groups[2].Value -lt 0 -or
            [int]$frameLines[0].Groups[3].Value -le
                [int]$frameLines[0].Groups[2].Value) {
            $findings.Add(
                'UT2003 gameplay frame interval is missing or invalid')
        }
        if ($frameLines.Count -eq 1) {
            $parsedStart = [DateTimeOffset]::MinValue
            if ([DateTimeOffset]::TryParse(
                    $frameLines[0].Groups[4].Value,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::None,
                    [ref]$parsedStart)) {
                $observationStartUtc = $parsedStart.UtcDateTime
            } else {
                $findings.Add('UT2003 gameplay frame interval start time is invalid')
            }
        }
        $recomputedCanonicalDistinctHashes = @($sampleVisualEvidence |
            Select-Object -ExpandProperty CanonicalRgbSha256 -Unique).Count
        $recomputedMeaningfulTransitions = 0
        for ($index = 1; $index -lt $sampleVisualEvidence.Count; ++$index) {
            $changedPercent = [XpBitmapVisual]::ChangedPercent(
                $sampleVisualEvidence[$index - 1],
                $sampleVisualEvidence[$index], 48)
            if ($changedPercent -ge 0.5) {
                ++$recomputedMeaningfulTransitions
            }
        }
        $visualCanonicalDistinctHashes =
            $recomputedCanonicalDistinctHashes
        $visualMeaningfulTransitions =
            $recomputedMeaningfulTransitions
        if ($sampleVisualEvidence.Count -ne 8 -or
            $recomputedCanonicalDistinctHashes -lt 4 -or
            $recomputedMeaningfulTransitions -lt 1) {
            $findings.Add('manifest-bound UT2003 gameplay samples are ' +
                'frozen or lack a visible post-input transition')
        }
        if ($console -notmatch
            'GameObserveVisual=recorded game=UT2003Gameplay .*desktop=False') {
            $findings.Add(
                'UT2003 gameplay rich non-desktop visual result is missing')
        }
        $videoLine = [regex]::Match($console,
            'ReviewVideo=.* frames=(\d+) fps=(\d+) seconds=([\d.]+) sourceSize=640x480')
        $videoSeconds = 0.0
        if (-not $videoLine.Success -or
            [int]$videoLine.Groups[1].Value -lt 450 -or
            [int]$videoLine.Groups[2].Value -ne 15 -or
            -not [double]::TryParse(
                $videoLine.Groups[3].Value,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$videoSeconds) -or
            $videoSeconds -lt 30.0 -or
            [Math]::Abs(
                [int]$videoLine.Groups[1].Value -
                $videoSeconds * 15.0) -gt 1.0) {
            $findings.Add('UT2003 gameplay review-video timing is invalid')
        } else {
            $visualSpanSeconds = $videoSeconds
            if ($video) {
                $video | Add-Member -NotePropertyName frames `
                    -NotePropertyValue ([int]$videoLine.Groups[1].Value)
                $video | Add-Member -NotePropertyName fps `
                    -NotePropertyValue 15
                $video | Add-Member -NotePropertyName durationSeconds `
                    -NotePropertyValue $videoSeconds
            }
        }
        if ($console -notmatch
            'ReviewVideoArtifact valid=True sources=1 artifacts=1') {
            $findings.Add(
                'UT2003 gameplay review-video artifact binding is invalid')
        }
    } elseif ($Mode -eq 'NFSU2Visual') {
        $boundaryLines = [regex]::Matches($console,
            '(?m)^NFSU2VisualBoundary valid=True boundary=foreground-d3d9-window-stable messages=1\r?$')
        if ($boundaryLines.Count -ne 1) {
            $findings.Add(
                'NFSU2 visual-ready boundary is missing or non-unique')
        }
        $completionLines = [regex]::Matches($console,
            '(?m)^GuestStage=QXP_GAME_OBSERVE_STAGE rc=0 game=nfsu2 stage=visual-complete observed-ms=(\d+) required-ms=180000 renderer=d3d9 system32=1 boundary=post-visual-ready\r?$')
        if ($completionLines.Count -ne 1 -or
            [int64]$completionLines[0].Groups[1].Value -lt 180000) {
            $findings.Add(
                'NFSU2 sustained stock-D3D9 visual contract is missing or short')
        } else {
            $observedMs = [int64]$completionLines[0].Groups[1].Value
            $visualSpanSeconds = [Math]::Round($observedMs / 1000.0, 3)
            $semanticTransition = 'd3d9-window-stable'
        }
        if ($harness -and
            ($harness.mode -cne '-xpdm-nfsu2' -or
             $harness.captureMode -cne 'video')) {
            $findings.Add(
                'harness result is not the NFSU2 stock-D3D9 video mode')
        }
        $frameLines = [regex]::Matches($console,
            '(?m)^GameObserveFrames game=NFSU2 count=(\d+) sequence=(-?\d+)-(-?\d+) time=(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))-(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2}))\r?$')
        if ($frameLines.Count -ne 1 -or
            [int]$frameLines[0].Groups[1].Value -lt 8 -or
            [int]$frameLines[0].Groups[2].Value -lt 0 -or
            [int]$frameLines[0].Groups[3].Value -le
                [int]$frameLines[0].Groups[2].Value) {
            $findings.Add(
                'NFSU2 retained frame interval is missing or invalid')
        } else {
            $parsedStart = [DateTimeOffset]::MinValue
            if ([DateTimeOffset]::TryParse(
                    $frameLines[0].Groups[4].Value,
                    [Globalization.CultureInfo]::InvariantCulture,
                    [Globalization.DateTimeStyles]::None,
                    [ref]$parsedStart)) {
                $observationStartUtc = $parsedStart.UtcDateTime
            } else {
                $findings.Add(
                    'NFSU2 retained frame interval start time is invalid')
            }
        }
        $recomputedCanonicalDistinctHashes = @($sampleVisualEvidence |
            Select-Object -ExpandProperty CanonicalRgbSha256 -Unique).Count
        $recomputedMeaningfulTransitions = 0
        for ($index = 1; $index -lt $sampleVisualEvidence.Count; ++$index) {
            $changedPercent = [XpBitmapVisual]::ChangedPercent(
                $sampleVisualEvidence[$index - 1],
                $sampleVisualEvidence[$index], 48)
            if ($changedPercent -ge 0.05) {
                ++$recomputedMeaningfulTransitions
            }
        }
        $visualCanonicalDistinctHashes =
            $recomputedCanonicalDistinctHashes
        $visualMeaningfulTransitions =
            $recomputedMeaningfulTransitions
        if ($sampleVisualEvidence.Count -ne 8 -or
            $recomputedCanonicalDistinctHashes -lt 4 -or
            $recomputedMeaningfulTransitions -lt 3) {
            $findings.Add('manifest-bound NFSU2 samples are frozen or ' +
                'lack sustained visible transitions')
        }
        if ($console -notmatch
            'GameObserveVisual=recorded game=NFSU2 .*desktop=False') {
            $findings.Add(
                'NFSU2 rich non-desktop visual result is missing')
        }
        $videoLine = [regex]::Match($console,
            'ReviewVideo=.* frames=(\d+) fps=(\d+) seconds=([\d.]+) sourceSize=640x480')
        $videoSeconds = 0.0
        if (-not $videoLine.Success -or
            [int]$videoLine.Groups[1].Value -lt 2 -or
            [int]$videoLine.Groups[2].Value -ne 15 -or
            -not [double]::TryParse(
                $videoLine.Groups[3].Value,
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$videoSeconds) -or
            $videoSeconds -le 0.0 -or
            [Math]::Abs(
                [int]$videoLine.Groups[1].Value -
                $videoSeconds * 15.0) -gt 1.0) {
            $findings.Add('NFSU2 review-video timing is invalid')
        } elseif ($video) {
            $video | Add-Member -NotePropertyName frames `
                -NotePropertyValue ([int]$videoLine.Groups[1].Value)
            $video | Add-Member -NotePropertyName fps `
                -NotePropertyValue 15
            $video | Add-Member -NotePropertyName durationSeconds `
                -NotePropertyValue $videoSeconds
        }
        if ($console -notmatch
            'ReviewVideoArtifact valid=True sources=1 artifacts=1') {
            $findings.Add(
                'NFSU2 review-video artifact binding is invalid')
        }
    } else {
        $ring = [regex]::Match($console,
            'Warcraft3D3D8FrameRing result=pass frames=256 durationSeconds=([\d.]+) visible=256 dark=0 black=0 isolatedBlack=0 interiorBlackRuns=0 vvb=0')
        if (-not $ring.Success) {
            $findings.Add('Warcraft frame-ring result is missing or invalid')
        }
        if ($console -notmatch
            'GameObserveVisual=recorded game=Warcraft3D3D8 .*desktop=False') {
            $findings.Add('Warcraft rich non-desktop visual result is missing')
        }
        $videoLine = [regex]::Match($console,
            'ReviewVideo=.* sourceFrames=256 encodedFrames=256 fps=30 seconds=([\d.]+) sourceSize=800x600 nonDecimated=True')
        if (-not $videoLine.Success) {
            $findings.Add('Warcraft non-decimated review-video result is invalid')
        } elseif ($video) {
            $video | Add-Member -NotePropertyName frames -NotePropertyValue 256
            $video | Add-Member -NotePropertyName fps -NotePropertyValue 30
            $video | Add-Member -NotePropertyName durationSeconds `
                -NotePropertyValue ([double]$videoLine.Groups[1].Value)
        }
        $cadenceName = "$prefix-black-cadence.json"
        $cadencePath = Resolve-XpBundleConfinedPath `
            -BundleRoot $Directory -Name $cadenceName
        $cadenceRecords = @(if ($manifest) {
            $manifest.files | Where-Object {
                [string]$_.name -ceq $cadenceName
            }
        })
        if ($cadenceRecords.Count -ne 1) {
            $findings.Add('Warcraft cadence sidecar is not uniquely manifested')
        }
        if (-not $cadencePath) {
            $findings.Add('Warcraft cadence sidecar is a reparse point or escapes the canonical bundle')
        } elseif (-not (Test-Path -LiteralPath $cadencePath -PathType Leaf)) {
            $findings.Add('missing Warcraft cadence sidecar')
        } else {
            try {
                $cadenceDocument = Get-Content -Raw -LiteralPath `
                    $cadencePath | ConvertFrom-Json
                $classes = [string]$cadenceDocument.classes
                $cadenceHash = (Get-FileHash -LiteralPath $cadencePath `
                    -Algorithm SHA256).Hash
                if ($cadenceRecords.Count -eq 1 -and
                    $cadenceHash -cne [string]$cadenceRecords[0].sha256) {
                    $findings.Add('Warcraft cadence manifest hash mismatch')
                }
                if ([int]$cadenceDocument.frames -ne 256 -or
                    $classes -cne ('V' * 256)) {
                    $findings.Add('Warcraft cadence is not exactly 256 visual frames')
                }
                $cadence = [pscustomobject][ordered]@{
                    fileName = $cadenceName
                    sha256 = $cadenceHash
                    frames = [int]$cadenceDocument.frames
                    classes = $classes
                    durationSeconds = [double]$cadenceDocument.durationSeconds
                }
            } catch {
                $findings.Add("invalid Warcraft cadence sidecar: $($_.Exception.Message)")
            }
        }
        if ($console -notmatch
            'ReviewVideoArtifact valid=True sources=1 artifacts=1') {
            $findings.Add('Warcraft review-video artifact binding is invalid')
        }
    }

    if ($video -and $videoPath) {
        try {
            $probe = & $VideoProbeAction $videoPath
            if ($null -eq $probe -or
                [int]$probe.width -ne $expectedWidth -or
                [int]$probe.height -ne $expectedHeight -or
                -not (Test-XpGameObserveEncodedVideoTiming `
                    -ExpectedFrames ([int]$video.frames) `
                    -EncodedFrames ([int]$probe.encodedFrames) `
                    -FrameRate ([int]$video.fps) `
                    -NominalFrameRate ([string]$probe.nominalFrameRate) `
                    -AverageFrameRate ([string]$probe.averageFrameRate) `
                    -DurationSeconds ([double]$probe.durationSeconds)) -or
                [Math]::Abs(
                    [double]$probe.durationSeconds -
                    [double]$video.durationSeconds) -gt 0.002) {
                $findings.Add(
                    'manifest-bound review video fails decoded frame, ' +
                    'timing, frame-rate, or geometry validation')
            } else {
                $video | Add-Member -NotePropertyName decodedFrames `
                    -NotePropertyValue ([int]$probe.encodedFrames)
                $video | Add-Member -NotePropertyName decodedWidth `
                    -NotePropertyValue ([int]$probe.width)
                $video | Add-Member -NotePropertyName decodedHeight `
                    -NotePropertyValue ([int]$probe.height)
                $video | Add-Member -NotePropertyName decodedDurationSeconds `
                    -NotePropertyValue ([double]$probe.durationSeconds)
                if ($sampleVisualEvidence.Count -eq 8 -and
                    $samplePaths.Count -eq 8) {
                    try {
                        $pixelBinding = Test-XpGameObserveVideoPixelBinding `
                            -Mode $Mode -VideoPath $videoPath `
                            -SamplePaths @($samplePaths) `
                            -ExpectedFrames ([int]$probe.encodedFrames) `
                            -ObservationStartUtc $observationStartUtc `
                            -VideoDurationSeconds `
                                ([double]$probe.durationSeconds) `
                            -SampleCapturedUtc @($sampleCapturedUtc)
                        foreach ($bindingFinding in @($pixelBinding.findings)) {
                            $findings.Add($bindingFinding)
                        }
                        if ($pixelBinding.valid) {
                            $video | Add-Member -NotePropertyName `
                                pixelMatchedFrames -NotePropertyValue `
                                @($pixelBinding.matchedFrames)
                            $video | Add-Member -NotePropertyName `
                                pixelMaximumMeanAbsoluteDifference `
                                -NotePropertyValue ([double]$pixelBinding.maximumMeanAbsoluteDifference)
                        }
                    } catch {
                        $findings.Add('manifest-bound review video pixel ' +
                            "binding failed: $($_.Exception.Message)")
                    }
                }
            }
        } catch {
            $findings.Add(
                "manifest-bound review video is not decodable: $($_.Exception.Message)")
        }
    }

    return [pscustomobject][ordered]@{
        valid = $findings.Count -eq 0
        findings = @($findings)
        evidence = [pscustomobject][ordered]@{
            kind = 'xpdriver-game-observe-visual'
            mode = $Mode
            observedMs = $observedMs
            semanticTransition = $semanticTransition
            visualSpanSeconds = $visualSpanSeconds
            visualMaximumGapSeconds = $visualMaximumGapSeconds
            canonicalDistinctHashes = $visualCanonicalDistinctHashes
            meaningfulTransitions = $visualMeaningfulTransitions
            recomputedCanonicalDistinctHashes =
                $recomputedCanonicalDistinctHashes
            recomputedMeaningfulTransitions =
                $recomputedMeaningfulTransitions
            menuQualifyingFrames = $menuQualifyingFrames
            recomputedMenuQualifyingFrames =
                $recomputedMenuQualifyingFrames
            readyFrameSequence = $readyFrameSequence
            guardFrameSequence = $guardFrameSequence
            firstAcceptedFrameSequence = $firstAcceptedFrameSequence
            selectedCount = $sampleRecords.Count
            samples = $samples
            video = $video
            cadence = $cadence
        }
    }
}

function Test-XpGameObserveVisualBundle {
    param(
        [string]$Directory,
        [ValidateSet(
            'UT2003Visual', 'UT2003Gameplay', 'NFSU2Visual',
            'Warcraft3D3D8Visual')]
        [string]$Mode,
        [scriptblock]$VideoProbeAction
    )

    try {
        return Test-XpGameObserveVisualBundleCore `
            -Directory $Directory -Mode $Mode `
            -VideoProbeAction $VideoProbeAction
    } catch {
        return [pscustomobject][ordered]@{
            valid = $false
            findings = @(
                "visual bundle verification failed closed: $($_.Exception.Message)")
            evidence = [pscustomobject][ordered]@{
                kind = 'xpdriver-game-observe-visual'
                mode = $Mode
                observedMs = $null
                semanticTransition = $null
                visualSpanSeconds = $null
                visualMaximumGapSeconds = $null
                selectedCount = 0
                samples = @()
                video = $null
                cadence = $null
            }
        }
    }
}
