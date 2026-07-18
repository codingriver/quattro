param(
    [Parameter(Mandatory = $true)]
    [string]$Config,
    [Parameter(Mandatory = $true)]
    [string]$HeaderOutput,
    [Parameter(Mandatory = $true)]
    [string]$CodepointsOutput
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Parse-Codepoint([string]$Value) {
    if ($Value -notmatch '^U\+([0-9A-Fa-f]{4,6})$') {
        throw "Invalid Tabler icon codepoint: $Value"
    }
    $number = [Convert]::ToInt32($Matches[1], 16)
    if ($number -lt 0x20 -or $number -gt 0x10FFFF) {
        throw "Tabler icon codepoint out of range: $Value"
    }
    return $number
}

function To-Identifier([string]$Name) {
    $parts = $Name -split '-'
    $result = ""
    foreach ($part in $parts) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            throw "Invalid empty Tabler icon name segment: $Name"
        }
        $result += $part.Substring(0, 1).ToUpperInvariant() + $part.Substring(1)
    }
    if ($result -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
        throw "Invalid Tabler icon identifier: $Name"
    }
    return $result
}

$configPath = Resolve-FullPath $Config
$headerPath = Resolve-FullPath $HeaderOutput
$codepointsPath = Resolve-FullPath $CodepointsOutput
$data = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($data.fontFamily)) {
    throw "Tabler icon manifest must define fontFamily."
}

$entries = New-Object System.Collections.Generic.List[object]
$seenNames = @{}
$seenCodepoints = @{}
foreach ($sectionName in @("icons", "extraGlyphs")) {
    $section = $data.$sectionName
    if ($null -eq $section) {
        continue
    }
    foreach ($property in $section.PSObject.Properties) {
        $name = [string]$property.Name
        if ($seenNames.ContainsKey($name)) {
            throw "Duplicate Tabler icon name: $name"
        }
        $codepoint = Parse-Codepoint ([string]$property.Value.codepoint)
        $hex = "{0:X}" -f $codepoint
        if ($seenCodepoints.ContainsKey($hex)) {
            $seenCodepoints[$hex] = $seenCodepoints[$hex] + "," + $name
        } else {
            $seenCodepoints[$hex] = $name
        }
        $seenNames[$name] = $true
        $entries.Add([pscustomobject]@{
            Name = $name
            Identifier = To-Identifier $name
            Codepoint = $codepoint
            Hex = $hex
        }) | Out-Null
    }
}

$entries = @($entries | Sort-Object Name)
$uniqueCodepoints = @($entries | Select-Object -ExpandProperty Codepoint -Unique | Sort-Object)
$headerLines = New-Object System.Collections.Generic.List[string]
$headerLines.Add("#pragma once") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add("#include <string_view>") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add("namespace TablerIconManifest {") | Out-Null
$headerLines.Add("enum class Id {") | Out-Null
$headerLines.Add("    Invalid = 0,") | Out-Null
foreach ($entry in $entries) {
    $headerLines.Add("    $($entry.Identifier),") | Out-Null
}
$headerLines.Add("};") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add("struct Entry {") | Out-Null
$headerLines.Add("    Id id;") | Out-Null
$headerLines.Add("    const wchar_t* name;") | Out-Null
$headerLines.Add("    wchar_t glyph;") | Out-Null
$headerLines.Add("};") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add(('inline constexpr wchar_t kFontFamily[] = L"' + $data.fontFamily + '";')) | Out-Null
$headerLines.Add("inline constexpr Entry kEntries[] = {") | Out-Null
foreach ($entry in $entries) {
    $headerLines.Add(('    {Id::' + $entry.Identifier + ', L"' + $entry.Name + '", static_cast<wchar_t>(0x' + $entry.Hex + ')},')) | Out-Null
}
$headerLines.Add("};") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add("inline Id FromName(std::wstring_view name) {") | Out-Null
$headerLines.Add("    for (const Entry& entry : kEntries) {") | Out-Null
$headerLines.Add("        if (name == entry.name) return entry.id;") | Out-Null
$headerLines.Add("    }") | Out-Null
$headerLines.Add("    return Id::Invalid;") | Out-Null
$headerLines.Add("}") | Out-Null
$headerLines.Add("") | Out-Null
$headerLines.Add("inline wchar_t Glyph(Id id) {") | Out-Null
$headerLines.Add("    for (const Entry& entry : kEntries) {") | Out-Null
$headerLines.Add("        if (id == entry.id) return entry.glyph;") | Out-Null
$headerLines.Add("    }") | Out-Null
$headerLines.Add("    return L'\0';") | Out-Null
$headerLines.Add("}") | Out-Null
$headerLines.Add("}") | Out-Null

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $headerPath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $codepointsPath) | Out-Null
[System.IO.File]::WriteAllText($headerPath, (($headerLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
$codepointLines = @($uniqueCodepoints | ForEach-Object { "U+{0:X}" -f $_ })
[System.IO.File]::WriteAllText($codepointsPath, (($codepointLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
"tabler_icon_manifest=generated entries=$($entries.Count) unique_codepoints=$($uniqueCodepoints.Count)"
