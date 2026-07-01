param(
    [string]$ThemePath = "",
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($ThemePath)) {
    $ThemePath = Join-Path $root "theme\default.xml"
}
if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path (Join-Path $root "build\Release") "logs"
}
if (!(Test-Path $ThemePath)) {
    throw "Theme not found: $ThemePath"
}
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Add-Type -AssemblyName System.Drawing

[xml]$theme = Get-Content -LiteralPath $ThemePath -Encoding UTF8
$palette = @{}
foreach ($color in $theme.Theme.Palette.Color) {
    $palette[$color.name] = $color.value
}

$reservedComponents = @(
    "panel",
    "iconButton",
    "miniButton",
    "toggle",
    "radio",
    "listItem",
    "slider",
    "tooltip",
    "separator"
)
foreach ($component in $reservedComponents) {
    $componentNode = $theme.Theme.Component | Where-Object { $_.name -eq $component } | Select-Object -First 1
    if (!$componentNode) {
        throw "Missing reserved theme component: $component"
    }
    if (!$componentNode.State) {
        throw "Reserved theme component has no states: $component"
    }
}

function Resolve-ColorValue {
    param([string]$Value, [int]$Depth = 0)
    if ($Depth -gt 8) { return [System.Drawing.Color]::FromArgb(255, 32, 36, 43) }
    $value = $Value.Trim()
    if ($value.StartsWith("@")) {
        $key = $value.Substring(1)
        if (!$palette.ContainsKey($key)) { throw "Missing palette color: $key" }
        return Resolve-ColorValue $palette[$key] ($Depth + 1)
    }
    if ($value.StartsWith("#") -and $value.Length -eq 9) {
        $a = [Convert]::ToInt32($value.Substring(1, 2), 16)
        $r = [Convert]::ToInt32($value.Substring(3, 2), 16)
        $g = [Convert]::ToInt32($value.Substring(5, 2), 16)
        $b = [Convert]::ToInt32($value.Substring(7, 2), 16)
        return [System.Drawing.Color]::FromArgb($a, $r, $g, $b)
    }
    if ($value -match '^rgb\((\d+),(\d+),(\d+)\)$') {
        return [System.Drawing.Color]::FromArgb(255, [int]$matches[1], [int]$matches[2], [int]$matches[3])
    }
    if ($value -match '^argb\((\d+),(\d+),(\d+),(\d+)\)$') {
        return [System.Drawing.Color]::FromArgb([int]$matches[1], [int]$matches[2], [int]$matches[3], [int]$matches[4])
    }
    throw "Unsupported color value: $value"
}

function Get-StateColor {
    param([string]$Component, [string]$State, [string]$Role)
    $componentNode = $theme.Theme.Component | Where-Object { $_.name -eq $Component } | Select-Object -First 1
    if ($componentNode) {
        $stateNode = $componentNode.State | Where-Object { $_.name -eq $State } | Select-Object -First 1
        if ($stateNode -and $stateNode.$Role) { return Resolve-ColorValue $stateNode.$Role }
        $normalNode = $componentNode.State | Where-Object { $_.name -eq "normal" } | Select-Object -First 1
        if ($normalNode -and $normalNode.$Role) { return Resolve-ColorValue $normalNode.$Role }
    }
    $globalNode = $theme.Theme.Component | Where-Object { $_.name -eq "global" } | Select-Object -First 1
    $globalState = $globalNode.State | Where-Object { $_.name -eq "normal" } | Select-Object -First 1
    if ($globalState -and $globalState.$Role) { return Resolve-ColorValue $globalState.$Role }
    if ($palette.ContainsKey($Role)) { return Resolve-ColorValue $palette[$Role] }
    return Resolve-ColorValue $palette["text"]
}

function Get-Metric {
    param([string]$Component, [string]$Name, [double]$Fallback)
    $componentNode = $theme.Theme.Component | Where-Object { $_.name -eq $Component } | Select-Object -First 1
    if ($componentNode) {
        $metricNode = $componentNode.Metric | Where-Object { $_.name -eq $Name } | Select-Object -First 1
        if ($metricNode -and $metricNode.value) {
            return [double]$metricNode.value
        }
    }
    return $Fallback
}

function Get-TextY {
    param([string]$Component, [int]$Top, [int]$Height, [double]$FallbackTextHeight = 20)
    $textHeight = Get-Metric $Component "textHeight" $FallbackTextHeight
    $offsetY = Get-Metric $Component "textOffsetY" 0
    return [int]($Top + (($Height - $textHeight) / 2) + $offsetY)
}

function Draw-RoundRect {
    param(
        [System.Drawing.Graphics]$Graphics,
        [System.Drawing.Rectangle]$Rect,
        [System.Drawing.Color]$Fill,
        [System.Drawing.Color]$Border,
        [int]$Radius = 7
    )
    if ($Radius -le 0) {
        $brush = New-Object System.Drawing.SolidBrush $Fill
        $pen = New-Object System.Drawing.Pen $Border, 1
        $Graphics.FillRectangle($brush, $Rect)
        $Graphics.DrawRectangle($pen, $Rect)
        $brush.Dispose()
        $pen.Dispose()
        return
    }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $Radius * 2
    $path.AddArc($Rect.Left, $Rect.Top, $d, $d, 180, 90)
    $path.AddArc($Rect.Right - $d, $Rect.Top, $d, $d, 270, 90)
    $path.AddArc($Rect.Right - $d, $Rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($Rect.Left, $Rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.SolidBrush $Fill
    $pen = New-Object System.Drawing.Pen $Border, 1
    $Graphics.FillPath($brush, $path)
    $Graphics.DrawPath($pen, $path)
    $brush.Dispose()
    $pen.Dispose()
    $path.Dispose()
}

$output = Join-Path $LogDir "theme-state-matrix.png"
$realOutput = Join-Path $LogDir "theme-state-real-controls.png"
$bitmap = New-Object System.Drawing.Bitmap 980, 430
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
$bg = Get-StateColor "dialog" "normal" "bg"
$graphics.Clear($bg)

$font = New-Object System.Drawing.Font "Microsoft YaHei UI", 9
$bold = New-Object System.Drawing.Font "Microsoft YaHei UI", 9, ([System.Drawing.FontStyle]::Bold)
$textBrush = New-Object System.Drawing.SolidBrush (Get-StateColor "global" "normal" "text")
$mutedBrush = New-Object System.Drawing.SolidBrush (Resolve-ColorValue $palette["mutedText"])

$columns = @("normal", "hover", "pressed", "focused", "disabled")
$rowDefs = @(
    @{ label = "Button"; component = "button"; states = @("normal", "hover", "pressed", "focused", "disabled") },
    @{ label = "Mini"; component = "miniButton"; states = @("normal", "hover", "pressed", "focused", "disabled") },
    @{ label = "Edit"; component = "edit"; states = @("normal", "hover", "focused", "readonly", "error") },
    @{ label = "Combo"; component = "comboBox"; states = @("normal", "hover", "focused", "selected", "disabled") },
    @{ label = "Checkbox"; component = "checkbox"; states = @("normal", "hover", "checked", "checkedHover", "disabled") },
    @{ label = "Tab"; component = "tabButton"; states = @("normal", "hover", "selected", "selectedHover", "disabled") },
    @{ label = "Semantic"; component = "global"; states = @("info", "success", "warning", "danger", "normal") }
)

$graphics.DrawString("default theme state matrix", $bold, $textBrush, 24, 18)
$graphics.DrawString("Generated from theme/default.xml", $font, $mutedBrush, 24, 38)

$startX = 130
$startY = 78
$cellW = 154
$cellH = 48
for ($i = 0; $i -lt $columns.Count; ++$i) {
    $graphics.DrawString($columns[$i], $bold, $mutedBrush, $startX + ($i * $cellW), $startY - 26)
}

for ($row = 0; $row -lt $rowDefs.Count; ++$row) {
    $def = $rowDefs[$row]
    $y = $startY + ($row * 56)
    $graphics.DrawString($def.label, $bold, $textBrush, 24, $y + 14)
    for ($col = 0; $col -lt $def.states.Count; ++$col) {
        $state = $def.states[$col]
        $x = $startX + ($col * $cellW)
        $controlH = [int](Get-Metric $def.component "height" 34)
        $rect = New-Object System.Drawing.Rectangle $x, $y, 124, $controlH
        if ($def.component -eq "checkbox") {
            $controlH = [int](Get-Metric "checkbox" "height" 24)
            $boxSize = [int](Get-Metric "checkbox" "boxSize" 16)
            $boxOffsetX = [int](Get-Metric "checkbox" "boxOffsetX" 0)
            $boxOffsetY = [int](Get-Metric "checkbox" "boxOffsetY" 0)
            $gap = [int](Get-Metric "checkbox" "gap" 8)
            $boxLeft = $x + $boxOffsetX
            $boxTop = [int]($y + (($controlH - $boxSize) / 2) + $boxOffsetY)
            $boxBg = Get-StateColor "checkbox" $state "boxBg"
            $border = Get-StateColor "checkbox" $state "border"
            Draw-RoundRect $graphics (New-Object System.Drawing.Rectangle $boxLeft, $boxTop, $boxSize, $boxSize) $boxBg $border ([int](Get-Metric "checkbox" "radius" 4))
            if ($state -like "checked*") {
                $pen = New-Object System.Drawing.Pen (Get-StateColor "checkbox" $state "mark"), ([int](Get-Metric "checkbox" "markWidth" 2))
                $graphics.DrawLines($pen, @(
                    (New-Object System.Drawing.Point ($boxLeft + [int]($boxSize / 4)), ($boxTop + [int]($boxSize / 2))),
                    (New-Object System.Drawing.Point ($boxLeft + [int]($boxSize * 7 / 16)), ($boxTop + $boxSize - [int]($boxSize / 4))),
                    (New-Object System.Drawing.Point ($boxLeft + $boxSize - [int]($boxSize / 5)), ($boxTop + [int]($boxSize / 4)))
                ))
                $pen.Dispose()
            }
            $graphics.DrawString($state, $font, $textBrush, $boxLeft + $boxSize + $gap, (Get-TextY "checkbox" $y $controlH))
        } elseif ($def.component -eq "global") {
            $fill = Get-StateColor "global" $state "bg"
            $fg = Get-StateColor "global" $state "text"
            Draw-RoundRect $graphics $rect $fill $fg 7
            $brush = New-Object System.Drawing.SolidBrush $fg
            $graphics.DrawString($state, $font, $brush, $x + 12, $y + 9)
            $brush.Dispose()
        } else {
            $fill = Get-StateColor $def.component $state "bg"
            $border = Get-StateColor $def.component $state "border"
            $fg = Get-StateColor $def.component $state "text"
            Draw-RoundRect $graphics $rect $fill $border ([int](Get-Metric $def.component "radius" 7))
            $brush = New-Object System.Drawing.SolidBrush $fg
            $paddingX = [int](Get-Metric $def.component "paddingX" 12)
            $graphics.DrawString($state, $font, $brush, $x + $paddingX, (Get-TextY $def.component $y $controlH))
            $brush.Dispose()
        }
    }
}

$graphics.Dispose()
$bitmap.Save($output, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

$check = [System.Drawing.Bitmap]::FromFile($output)
try {
    $colors = New-Object 'System.Collections.Generic.HashSet[int]'
    for ($y = 0; $y -lt $check.Height; $y += 12) {
        for ($x = 0; $x -lt $check.Width; $x += 12) {
            [void]$colors.Add($check.GetPixel($x, $y).ToArgb())
        }
    }
    if ($colors.Count -lt 18) {
        throw "Theme state matrix does not contain enough visual variation."
    }
} finally {
    $check.Dispose()
    $font.Dispose()
    $bold.Dispose()
    $textBrush.Dispose()
    $mutedBrush.Dispose()
}

"theme_state_tests=passed"
"screenshot=$output"

$probe = Join-Path (Join-Path $root "build\Release") "QuattroThemeStateProbe.exe"
if (Test-Path $probe) {
    & $probe $realOutput
    if ($LASTEXITCODE -ne 0) {
        throw "QuattroThemeStateProbe failed with exit code $LASTEXITCODE"
    }
    if (!(Test-Path $realOutput)) {
        throw "Real control state matrix was not generated."
    }
    $real = [System.Drawing.Bitmap]::FromFile($realOutput)
    try {
        if ($real.Width -lt 900 -or $real.Height -lt 680) {
            throw "Real control state matrix is too small for reserved component coverage: $($real.Width)x$($real.Height)"
        }
        $colors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y = 0; $y -lt $real.Height; $y += 12) {
            for ($x = 0; $x -lt $real.Width; $x += 12) {
                [void]$colors.Add($real.GetPixel($x, $y).ToArgb())
            }
        }
        if ($colors.Count -lt 18) {
            throw "Real control state matrix does not contain enough visual variation."
        }
        $reservedColors = New-Object 'System.Collections.Generic.HashSet[int]'
        for ($y = 380; $y -lt $real.Height; $y += 10) {
            for ($x = 120; $x -lt 780; $x += 10) {
                [void]$reservedColors.Add($real.GetPixel($x, $y).ToArgb())
            }
        }
        if ($reservedColors.Count -lt 12) {
            throw "Reserved component test area does not contain enough visual variation."
        }
    } finally {
        $real.Dispose()
    }
    "real_control_state_tests=passed"
    "reserved_component_tests=passed"
    "real_screenshot=$realOutput"
} else {
    "real_control_state_tests=skipped"
    "reason=probe_not_built"
}
