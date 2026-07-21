param(
    [Parameter(Mandatory = $true)]
    [string]$Root,
    [Parameter(Mandatory = $true)]
    [string]$Config,
    [Parameter(Mandatory = $true)]
    [string]$GeneratedHeader,
    [string]$FontPath = ""
)

$ErrorActionPreference = "Stop"
$rootPath = [System.IO.Path]::GetFullPath($Root)
$configPath = [System.IO.Path]::GetFullPath($Config)
$headerPath = [System.IO.Path]::GetFullPath($GeneratedHeader)
$data = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json

function To-Identifier([string]$Name) {
    return (($Name -split '-') | ForEach-Object {
        $_.Substring(0, 1).ToUpperInvariant() + $_.Substring(1)
    }) -join ''
}

$manifestNames = New-Object System.Collections.Generic.HashSet[string]
$manifestIdentifiers = New-Object System.Collections.Generic.HashSet[string]
foreach ($sectionName in @("icons", "extraGlyphs")) {
    $section = $data.$sectionName
    if ($null -eq $section) { continue }
    foreach ($property in $section.PSObject.Properties) {
        [void]$manifestNames.Add([string]$property.Name)
        [void]$manifestIdentifiers.Add((To-Identifier ([string]$property.Name)))
    }
}

if (!(Test-Path -LiteralPath $headerPath)) {
    throw "Generated Tabler manifest header not found: $headerPath"
}
$headerText = Get-Content -LiteralPath $headerPath -Raw
foreach ($identifier in $manifestIdentifiers) {
    if ($headerText -notmatch ("\b" + [regex]::Escape($identifier) + "\b")) {
        throw "Generated Tabler manifest is missing identifier: $identifier"
    }
}

$sourceFiles = Get-ChildItem -LiteralPath (Join-Path $rootPath "src") -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h') }
$literalPattern = '(?:TablerIconGlyph|DrawTablerIcon|CreateTablerIconHandle)\s*\([^)]*L"([^"]+)"'
$idPattern = 'TablerIconId::([A-Za-z_][A-Za-z0-9_]*)'
foreach ($file in $sourceFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    if ($file.Name -ne 'TablerIconFacade.cpp' -and
        ($text -match 'AddFontResourceExW|L"tabler-icons"')) {
        throw "Direct Tabler font access is not allowed outside TablerIconFacade.cpp: $($file.FullName)"
    }
    foreach ($match in [regex]::Matches($text, $literalPattern)) {
        $name = $match.Groups[1].Value
        if (!$manifestNames.Contains($name)) {
            throw "Unregistered Tabler icon literal '$name' in $($file.FullName)"
        }
    }
    foreach ($match in [regex]::Matches($text, $idPattern)) {
        $identifier = $match.Groups[1].Value
        if (!$manifestIdentifiers.Contains($identifier)) {
            throw "Unregistered Tabler icon identifier '$identifier' in $($file.FullName)"
        }
    }
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '(?i)(\btabler\b|tabler(?=icon|font|glyph)|glyph|chevron)' -and
            $line -match '0x[0-9a-f]{4,6}') {
            throw "Hard-coded Tabler glyph is not allowed in $($file.FullName): $line"
        }
    }
}

if (![string]::IsNullOrWhiteSpace($FontPath)) {
    $fontPath = [System.IO.Path]::GetFullPath($FontPath)
    if (!(Test-Path -LiteralPath $fontPath)) {
        throw "Tabler font for coverage validation not found: $fontPath"
    }
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if (!$python) {
        throw "Python is required for Tabler font cmap validation."
    }
    $verify = @'
import json, sys
from fontTools.ttLib import TTFont
config_path, font_path = sys.argv[1:]
config = json.load(open(config_path, encoding='utf-8'))
names = []
for section in ('icons', 'extraGlyphs'):
    names.extend(item['codepoint'] for item in config.get(section, {}).values())
expected = {int(value[2:], 16) for value in names}
font = TTFont(font_path)
cmap = set()
for table in font['cmap'].tables:
    cmap.update(table.cmap.keys())
missing = sorted(expected - cmap)
if missing:
    raise SystemExit('missing cmap codepoints: ' + ','.join('U+%X' % value for value in missing))
families = {record.toUnicode() for record in font['name'].names if record.nameID == 1}
if 'tabler-icons' not in families:
    raise SystemExit('font family tabler-icons is missing')
print('tabler_font_cmap=passed codepoints=%d bytes=%d' % (len(expected), __import__('os').path.getsize(font_path)))
'@
    & $python.Source -c $verify $configPath $fontPath
    if ($LASTEXITCODE -ne 0) {
        throw "Tabler font cmap validation failed."
    }
}

"tabler_icon_coverage=passed manifest_entries=$($manifestNames.Count)"
