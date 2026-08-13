# Generates the final multi-resolution CHEBURNET application icon.
# The mark keeps the original green-eared silhouette and adds the orange
# connection node used by the public release artwork.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Preview
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath {
    param(
        [single]$X,
        [single]$Y,
        [single]$Width,
        [single]$Height,
        [single]$Radius
    )

    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = [single]($Radius * 2.0)
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng {
    param([int]$Size)

    $bitmap = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)

    try {
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.Clear([System.Drawing.Color]::Transparent)

        $s = [single]($Size / 256.0)
        $backgroundPath = New-RoundedRectanglePath (6 * $s) (6 * $s) (244 * $s) (244 * $s) (52 * $s)
        $backgroundBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
            [System.Drawing.RectangleF]::new(0, 0, $Size, $Size),
            [System.Drawing.Color]::FromArgb(255, 18, 24, 30),
            [System.Drawing.Color]::FromArgb(255, 5, 8, 11),
            55.0)
        $graphics.FillPath($backgroundBrush, $backgroundPath)
        $backgroundBrush.Dispose()

        $borderWidth = [single][Math]::Max(1.0, 5.0 * $s)
        $borderPen = [System.Drawing.Pen]::new(
            [System.Drawing.Color]::FromArgb(255, 255, 122, 24),
            $borderWidth)
        $borderPen.Alignment = [System.Drawing.Drawing2D.PenAlignment]::Inset
        $graphics.DrawPath($borderPen, $backgroundPath)
        $borderPen.Dispose()
        $backgroundPath.Dispose()

        $green = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 46, 230, 107))
        $greenDark = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 12, 77, 43))
        $panel = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 10, 15, 19))

        # The two ears preserve the recognizable CHEBURNET mascot silhouette.
        $graphics.FillEllipse($green, 24 * $s, 78 * $s, 82 * $s, 82 * $s)
        $graphics.FillEllipse($green, 150 * $s, 78 * $s, 82 * $s, 82 * $s)
        $graphics.FillEllipse($greenDark, 38 * $s, 92 * $s, 54 * $s, 54 * $s)
        $graphics.FillEllipse($greenDark, 164 * $s, 92 * $s, 54 * $s, 54 * $s)

        # Central protected-runtime node.
        $graphics.FillEllipse($green, 62 * $s, 54 * $s, 132 * $s, 150 * $s)
        $graphics.FillEllipse($panel, 73 * $s, 65 * $s, 110 * $s, 128 * $s)

        $orangeWidth = [single][Math]::Max(1.2, 8.0 * $s)
        $orangePen = [System.Drawing.Pen]::new(
            [System.Drawing.Color]::FromArgb(255, 255, 122, 24),
            $orangeWidth)
        $orangePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $orangePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $graphics.DrawLine($orangePen, 128 * $s, 80 * $s, 128 * $s, 112 * $s)
        $graphics.DrawLine($orangePen, 94 * $s, 128 * $s, 116 * $s, 128 * $s)
        $graphics.DrawLine($orangePen, 140 * $s, 128 * $s, 162 * $s, 128 * $s)

        $orange = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 255, 122, 24))
        $nodeSize = [single][Math]::Max(4.0, 22.0 * $s)
        $graphics.FillEllipse(
            $orange,
            (128 * $s) - ($nodeSize / 2),
            (128 * $s) - ($nodeSize / 2),
            $nodeSize,
            $nodeSize)

        # A short status bar remains readable even at 16x16.
        $statusPath = New-RoundedRectanglePath (92 * $s) (163 * $s) (72 * $s) (10 * $s) (5 * $s)
        $graphics.FillPath($green, $statusPath)
        $statusPath.Dispose()

        $orange.Dispose()
        $orangePen.Dispose()
        $panel.Dispose()
        $greenDark.Dispose()
        $green.Dispose()

        $stream = [System.IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,$stream.ToArray()
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $images += ,(New-IconPng -Size $size)
}

$outPath = [System.IO.Path]::GetFullPath($Out)
$outDirectory = [System.IO.Path]::GetDirectoryName($outPath)
if (-not [System.IO.Directory]::Exists($outDirectory)) {
    [System.IO.Directory]::CreateDirectory($outDirectory) | Out-Null
}

$fileStream = [System.IO.File]::Create($outPath)
$writer = [System.IO.BinaryWriter]::new($fileStream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)

    $offset = 6 + (16 * $images.Count)
    for ($index = 0; $index -lt $images.Count; ++$index) {
        $size = $sizes[$index]
        $dimensionByte = if ($size -eq 256) { [byte]0 } else { [byte]$size }
        $writer.Write($dimensionByte)
        $writer.Write($dimensionByte)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$index].Length)
        $writer.Write([uint32]$offset)
        $offset += $images[$index].Length
    }

    foreach ($image in $images) {
        $writer.Write($image)
    }
}
finally {
    $writer.Dispose()
    $fileStream.Dispose()
}

if (-not [string]::IsNullOrWhiteSpace($Preview)) {
    $previewPath = [System.IO.Path]::GetFullPath($Preview)
    $previewDirectory = [System.IO.Path]::GetDirectoryName($previewPath)
    if (-not [System.IO.Directory]::Exists($previewDirectory)) {
        [System.IO.Directory]::CreateDirectory($previewDirectory) | Out-Null
    }
    [System.IO.File]::WriteAllBytes($previewPath, $images[-1])
}

Write-Output "icon written: $outPath ($((Get-Item -LiteralPath $outPath).Length) bytes, $($sizes.Count) sizes)"
