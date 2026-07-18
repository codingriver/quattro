param(
    [string]$BuildRoot = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$generator = Join-Path $PSScriptRoot "generate-embedded-asset-pack.ps1"
$resourceHeader = Join-Path $root "resources\resource.h"
$temporaryRoot = if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    Join-Path $root "build-embedded-asset-generator-acceptance"
} else {
    [System.IO.Path]::GetFullPath($BuildRoot)
}

if (Test-Path -LiteralPath $temporaryRoot) {
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null

try {
    $rawCatalog = Join-Path $temporaryRoot "EmbeddedAssets.raw.catalog"
    $rawResource = Join-Path $temporaryRoot "EmbeddedAssets.raw.rc"
    & $generator -Root $root -PackOutput $rawCatalog -ResourceOutput $rawResource `
        -ResourceHeader $resourceHeader -ResourceName IDR_QUATTRO_ASSET_PACK -Mode raw | Out-Host

    $rawBytes = [System.IO.File]::ReadAllBytes($rawCatalog)
    $rawMagic = [System.Text.Encoding]::ASCII.GetString($rawBytes, 0, 8)
    $rawResourceText = [System.IO.File]::ReadAllText($rawResource)
    if ($rawMagic -ne "QARCAT01" -or $rawBytes.Length -ge 4096) {
        throw "Raw mode must emit a small catalog without copied asset payloads."
    }
    if ([regex]::Matches($rawResourceText, "(?m)^\d+ RCDATA ").Count -ne 2 -or
        !$rawResourceText.Contains("theme/default.xml") -or
        !$rawResourceText.Contains("tabler-icons.ttf") -or
        $rawResourceText.Contains("tabler-icons.css")) {
        throw "Raw mode must reference only the runtime theme and icon font resources."
    }

    $xpressPack = Join-Path $temporaryRoot "EmbeddedAssets.xpress.pack"
    $xpressResource = Join-Path $temporaryRoot "EmbeddedAssets.xpress.rc"
    & $generator -Root $root -PackOutput $xpressPack -ResourceOutput $xpressResource `
        -ResourceHeader $resourceHeader -ResourceName IDR_QUATTRO_ASSET_PACK -Mode xpress | Out-Host

    $xpressBytes = [System.IO.File]::ReadAllBytes($xpressPack)
    $xpressMagic = [System.Text.Encoding]::ASCII.GetString($xpressBytes, 0, 8)
    $assetBytes = (Get-Item `
        (Join-Path $root "theme\default.xml"), `
        (Join-Path $root "icons\menu\tabler\tabler-icons.ttf") |
        Measure-Object Length -Sum).Sum
    if ($xpressMagic -ne "QASPACK1" -or $xpressBytes.Length -ge $assetBytes) {
        throw "XPRESS mode must emit a compressed embedded asset pack."
    }

    "embedded_asset_generator_acceptance=passed raw_catalog_bytes=$($rawBytes.Length) xpress_pack_bytes=$($xpressBytes.Length)"
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
