param(
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $root "theme\assets"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Add-Type -AssemblyName System.Drawing

function New-Bitmap {
    param([int]$Width, [int]$Height)
    $bitmap = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
    $graphics.Clear([System.Drawing.Color]::Transparent)
    return @{ Bitmap = $bitmap; Graphics = $graphics }
}

function New-RoundedPath {
    param(
        [float]$X,
        [float]$Y,
        [float]$Width,
        [float]$Height,
        [float]$Radius
    )
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function Save-Png {
    param(
        [System.Drawing.Bitmap]$Bitmap,
        [string]$Path
    )
    $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
}

$mark = New-Bitmap -Width 96 -Height 96
try {
    $g = $mark.Graphics
    $shadowPath = New-RoundedPath 10 12 76 72 22
    $shadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(38, 15, 23, 42))
    $g.FillPath($shadowBrush, $shadowPath)
    $shadowBrush.Dispose()
    $shadowPath.Dispose()

    $cardPath = New-RoundedPath 8 8 78 78 24
    $cardBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.RectangleF(8, 8, 78, 78)),
        [System.Drawing.Color]::FromArgb(255, 37, 99, 235),
        [System.Drawing.Color]::FromArgb(255, 20, 184, 166),
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal
    )
    $g.FillPath($cardBrush, $cardPath)
    $cardBrush.Dispose()

    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(245, 255, 255, 255))
    $dot = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(210, 255, 255, 255))
    $g.FillEllipse($white, 27, 24, 16, 16)
    $g.FillEllipse($white, 53, 24, 16, 16)
    $g.FillEllipse($white, 27, 54, 16, 16)
    $g.FillEllipse($dot, 53, 54, 16, 16)
    $white.Dispose()
    $dot.Dispose()
    $cardPath.Dispose()

    Save-Png $mark.Bitmap (Join-Path $OutputDir "quattro-mark.png")
} finally {
    $mark.Graphics.Dispose()
    $mark.Bitmap.Dispose()
}

$empty = New-Bitmap -Width 220 -Height 160
try {
    $g = $empty.Graphics
    $panelPath = New-RoundedPath 18 26 184 104 18
    $panelBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(245, 255, 255, 255))
    $panelPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 213, 219, 229), 2)
    $g.FillPath($panelBrush, $panelPath)
    $g.DrawPath($panelPen, $panelPath)
    $panelBrush.Dispose()
    $panelPen.Dispose()
    $panelPath.Dispose()

    $accent = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 37, 99, 235))
    $soft = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 226, 236, 255))
    $muted = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 148, 163, 184), 2)
    $g.FillRectangle($soft, 38, 48, 42, 42)
    $g.DrawLine($muted, 100, 54, 178, 54)
    $g.DrawLine($muted, 100, 74, 158, 74)
    $g.DrawLine($muted, 100, 94, 178, 94)
    $g.FillEllipse($accent, 152, 92, 34, 34)
    $plusPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, 4)
    $plusPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $plusPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($plusPen, 169, 101, 169, 117)
    $g.DrawLine($plusPen, 161, 109, 177, 109)
    $accent.Dispose()
    $soft.Dispose()
    $muted.Dispose()
    $plusPen.Dispose()

    Save-Png $empty.Bitmap (Join-Path $OutputDir "empty-launcher.png")
} finally {
    $empty.Graphics.Dispose()
    $empty.Bitmap.Dispose()
}

"generated theme assets: $OutputDir"
