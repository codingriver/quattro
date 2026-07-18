param(
    [Parameter(Mandatory = $true)]
    [string]$Root,
    [Parameter(Mandatory = $true)]
    [string]$PackOutput,
    [Parameter(Mandatory = $true)]
    [string]$ResourceOutput,
    [Parameter(Mandatory = $true)]
    [string]$ResourceHeader,
    [Parameter(Mandatory = $true)]
    [string]$ResourceName,
    [ValidateSet("raw", "xpress")]
    [string]$Mode = "raw"
)

$ErrorActionPreference = "Stop"
$rawResourceIdBase = 1100

function Initialize-NativeCompression {
    if ("QuattroNativeCompression" -as [type]) {
        return
    }

    $savedLibEnvironment = $env:LIB
    try {
        $env:LIB = $null
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class QuattroNativeCompression
{
    public const uint AlgorithmXpressHuff = 4;

    [DllImport("cabinet.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CreateCompressor(
        uint algorithm,
        IntPtr allocationRoutines,
        out IntPtr compressor);

    [DllImport("cabinet.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool Compress(
        IntPtr compressor,
        byte[] uncompressedData,
        UIntPtr uncompressedDataSize,
        byte[] compressedBuffer,
        UIntPtr compressedBufferSize,
        out UIntPtr compressedDataSize);

    [DllImport("cabinet.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseCompressor(IntPtr compressor);
}
'@
    } finally {
        $env:LIB = $savedLibEnvironment
    }
}

function Test-EmbeddedAssetFile {
    param([string]$RelativePath)

    $name = [System.IO.Path]::GetFileName($RelativePath)
    return !$name.Equals("README.md", [System.StringComparison]::OrdinalIgnoreCase) -and
        !$name.Equals("LICENSE", [System.StringComparison]::OrdinalIgnoreCase) -and
        !$RelativePath.Equals("icons/menu/tabler/tabler-icons.css", [System.StringComparison]::OrdinalIgnoreCase)
}

function Compress-XpressHuff {
    param([byte[]]$Bytes)

    Initialize-NativeCompression
    $compressor = [IntPtr]::Zero
    if (![QuattroNativeCompression]::CreateCompressor(
            [QuattroNativeCompression]::AlgorithmXpressHuff,
            [IntPtr]::Zero,
            [ref]$compressor)) {
        throw "CreateCompressor failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }

    try {
        $required = [UIntPtr]::Zero
        $sized = [QuattroNativeCompression]::Compress(
            $compressor,
            $Bytes,
            [UIntPtr]::new([uint64]$Bytes.Length),
            $null,
            [UIntPtr]::Zero,
            [ref]$required)
        $sizeError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($sized -or $sizeError -ne 122) {
            throw "Compress sizing failed: $sizeError"
        }

        if ($required.ToUInt64() -gt [int]::MaxValue) {
            throw "Compressed asset is too large."
        }
        $compressed = New-Object byte[] ([int]$required.ToUInt64())
        $written = [UIntPtr]::Zero
        if (![QuattroNativeCompression]::Compress(
                $compressor,
                $Bytes,
                [UIntPtr]::new([uint64]$Bytes.Length),
                $compressed,
                [UIntPtr]::new([uint64]$compressed.Length),
                [ref]$written)) {
            throw "Compress failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        if ($written.ToUInt64() -ne [uint64]$compressed.Length) {
            [Array]::Resize([ref]$compressed, [int]$written.ToUInt64())
        }
        return $compressed
    } finally {
        if ($compressor -ne [IntPtr]::Zero) {
            [QuattroNativeCompression]::CloseCompressor($compressor) | Out-Null
        }
    }
}

function Write-BytesIfChanged {
    param([string]$Path, [byte[]]$Bytes)

    if (Test-Path -LiteralPath $Path) {
        $existing = [System.IO.File]::ReadAllBytes($Path)
        if ($existing.Length -eq $Bytes.Length) {
            $same = $true
            for ($i = 0; $i -lt $Bytes.Length; ++$i) {
                if ($existing[$i] -ne $Bytes[$i]) {
                    $same = $false
                    break
                }
            }
            if ($same) {
                [System.IO.File]::SetLastWriteTimeUtc($Path, [DateTime]::UtcNow)
                return
            }
        }
    }
    [System.IO.File]::WriteAllBytes($Path, $Bytes)
}

function Write-TextIfChanged {
    param([string]$Path, [string]$Text)

    if ((Test-Path -LiteralPath $Path) -and
        [System.IO.File]::ReadAllText($Path) -eq $Text) {
        [System.IO.File]::SetLastWriteTimeUtc($Path, [DateTime]::UtcNow)
        return
    }
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

$rootPath = (Resolve-Path -LiteralPath $Root).Path
$rootPrefix = $rootPath.TrimEnd("\") + "\"
$packPath = [System.IO.Path]::GetFullPath($PackOutput)
$resourcePath = [System.IO.Path]::GetFullPath($ResourceOutput)
$resourceHeaderPath = [System.IO.Path]::GetFullPath($ResourceHeader)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $packPath) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resourcePath) | Out-Null

$relativeFiles = New-Object System.Collections.Generic.List[string]
foreach ($dir in @("theme", "icons/menu", "icons/url")) {
    $path = Join-Path $rootPath $dir
    if (!(Test-Path -LiteralPath $path)) {
        continue
    }
    Get-ChildItem -LiteralPath $path -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($rootPrefix.Length).Replace("\", "/")
            if (Test-EmbeddedAssetFile -RelativePath $relative) {
                $relativeFiles.Add($relative) | Out-Null
            }
        }
}

$entries = New-Object System.Collections.Generic.List[object]
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    for ($index = 0; $index -lt $relativeFiles.Count; ++$index) {
        $relative = $relativeFiles[$index]
        $sourcePath = Join-Path $rootPath $relative.Replace("/", "\")
        $bytes = [System.IO.File]::ReadAllBytes($sourcePath)
        $compressed = if ($Mode -eq "xpress") { Compress-XpressHuff -Bytes $bytes } else { $null }
        $entries.Add([pscustomobject]@{
            Relative = $relative
            SourcePath = $sourcePath
            PathBytes = [System.Text.Encoding]::UTF8.GetBytes($relative)
            Original = $bytes
            Compressed = $compressed
            Sha256 = $sha256.ComputeHash($bytes)
            ResourceId = $rawResourceIdBase + $index
        }) | Out-Null
    }
} finally {
    $sha256.Dispose()
}

$memory = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($memory, [System.Text.Encoding]::UTF8, $true)
try {
    if ($Mode -eq "xpress") {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("QASPACK1"))
        $writer.Write([uint32]1)
        $writer.Write([uint32]4)
        $writer.Write([uint32]$entries.Count)
        foreach ($entry in $entries) {
            $writer.Write([uint32]$entry.PathBytes.Length)
            $writer.Write([uint64]$entry.Original.Length)
            $writer.Write([uint64]$entry.Compressed.Length)
            $writer.Write([byte[]]$entry.Sha256)
            $writer.Write([byte[]]$entry.PathBytes)
            $writer.Write([byte[]]$entry.Compressed)
        }
    } else {
        $writer.Write([System.Text.Encoding]::ASCII.GetBytes("QARCAT01"))
        $writer.Write([uint32]1)
        $writer.Write([uint32]$entries.Count)
        foreach ($entry in $entries) {
            $writer.Write([uint32]$entry.PathBytes.Length)
            $writer.Write([uint64]$entry.Original.Length)
            $writer.Write([byte[]]$entry.Sha256)
            $writer.Write([uint32]$entry.ResourceId)
            $writer.Write([byte[]]$entry.PathBytes)
        }
    }
    $writer.Flush()
    Write-BytesIfChanged -Path $packPath -Bytes $memory.ToArray()
} finally {
    $writer.Dispose()
    $memory.Dispose()
}

$rcPackPath = $packPath.Replace("\", "/").Replace('"', '\"')
$rcHeaderPath = $resourceHeaderPath.Replace("\", "/").Replace('"', '\"')
$resourceLines = New-Object System.Collections.Generic.List[string]
$resourceLines.Add("#include `"$rcHeaderPath`"") | Out-Null
$resourceLines.Add("$ResourceName RCDATA `"$rcPackPath`"") | Out-Null
if ($Mode -eq "raw") {
    foreach ($entry in $entries) {
        $rcSourcePath = $entry.SourcePath.Replace("\", "/").Replace('"', '\"')
        $resourceLines.Add("$($entry.ResourceId) RCDATA `"$rcSourcePath`"") | Out-Null
    }
}
Write-TextIfChanged -Path $resourcePath -Text (($resourceLines -join "`r`n") + "`r`n")

$originalBytes = ($entries | ForEach-Object { $_.Original.Length } | Measure-Object -Sum).Sum
$payloadBytes = if ($Mode -eq "xpress") {
    ($entries | ForEach-Object { $_.Compressed.Length } | Measure-Object -Sum).Sum
} else {
    $originalBytes
}
"embedded_asset_resources mode=$Mode files=$($entries.Count) original_bytes=$originalBytes payload_bytes=$payloadBytes catalog_bytes=$((Get-Item $packPath).Length)"
