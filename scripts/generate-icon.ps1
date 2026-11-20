param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\src\MMIDownloader.ico')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectanglePath {
    param([float]$X, [float]$Y, [float]$Width, [float]$Height, [float]$Radius)
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconPng {
    param([int]$Size)
    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $scale = $Size / 256.0

    $background = New-RoundedRectanglePath 8 8 240 240 42
    $matrix = [System.Drawing.Drawing2D.Matrix]::new()
    $matrix.Scale($scale, $scale)
    $background.Transform($matrix)
    $backgroundBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 38, 43, 52))
    $graphics.FillPath($backgroundBrush, $background)

    $blueBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 22, 140, 230))
    $arrow = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $arrow.AddPolygon([System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(104 * $scale, 50 * $scale),
        [System.Drawing.PointF]::new(152 * $scale, 50 * $scale),
        [System.Drawing.PointF]::new(152 * $scale, 119 * $scale),
        [System.Drawing.PointF]::new(184 * $scale, 119 * $scale),
        [System.Drawing.PointF]::new(128 * $scale, 174 * $scale),
        [System.Drawing.PointF]::new(72 * $scale, 119 * $scale),
        [System.Drawing.PointF]::new(104 * $scale, 119 * $scale)
    ))
    $graphics.FillPath($blueBrush, $arrow)

    $linePen = [System.Drawing.Pen]::new([System.Drawing.Color]::White, [Math]::Max(1.5, 13 * $scale))
    $linePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $linePen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawLine($linePen, 64 * $scale, 198 * $scale, 192 * $scale, 198 * $scale)
    $graphics.DrawLine($linePen, 82 * $scale, 220 * $scale, 174 * $scale, 220 * $scale)

    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $stream.ToArray()

    $stream.Dispose()
    $linePen.Dispose()
    $arrow.Dispose()
    $blueBrush.Dispose()
    $backgroundBrush.Dispose()
    $background.Dispose()
    $matrix.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    return $bytes
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = foreach ($size in $sizes) {
    [pscustomobject]@{ Size = $size; Bytes = (New-IconPng -Size $size) }
}

$outputDirectory = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$stream = [System.IO.File]::Create([System.IO.Path]::GetFullPath($OutputPath))
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
        $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$image.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $image.Bytes.Length
    }
    foreach ($image in $images) {
        $writer.Write([byte[]]$image.Bytes)
    }
} finally {
    $writer.Dispose()
    $stream.Dispose()
}

Write-Output ([System.IO.Path]::GetFullPath($OutputPath))
