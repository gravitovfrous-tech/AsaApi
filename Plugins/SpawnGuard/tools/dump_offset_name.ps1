# ---------------------------------------------------------------------------
# SpawnGuard helper: print names from the AsaApi offset cache.
#
# The cache (ArkApi\Cache\...\cached_offsets.cache) is a binary serialized
# unordered_map<string, intptr_t>:
#
#     repeat: [uint64 key_size][key bytes][int64 value]
#
# It contains the exact hook names of the game build that is running, so when
# the log says "Failed to get the offset of 'APrimalStructure...'", this script
# shows what the cache actually has for that method.
#
# Usage (from the server folder, e.g. ...\ShooterGame\Binaries\Win64):
#     .\dump_offset_name.ps1
#     .\dump_offset_name.ps1 -Pattern IsAllowedToBuild
#     .\dump_offset_name.ps1 -Pattern "APrimalStructure." -All
#     .\dump_offset_name.ps1 -CacheDir "D:\ARK\ShooterGame\Binaries\Win64\ArkApi"
# ---------------------------------------------------------------------------

param(
    [string]$Pattern = "IsAllowedToBuild",
    [string]$CacheDir = "",
    [switch]$All
)

$ErrorActionPreference = "Stop"

function Find-CacheRoot([string]$start) {
    $root = $start
    if ([string]::IsNullOrWhiteSpace($root)) { $root = (Get-Location).Path }

    while ($root) {
        foreach ($candidate in @((Join-Path $root "ArkApi\Cache"), (Join-Path $root "ArkApi"))) {
            if (Test-Path $candidate) { return $candidate }
        }
        $parent = Split-Path $root -Parent
        if ([string]::IsNullOrEmpty($parent) -or $parent -eq $root) { break }
        $root = $parent
    }
    return ""
}

$cacheRoot = Find-CacheRoot $CacheDir
if ([string]::IsNullOrEmpty($cacheRoot)) {
    Write-Host "cached_offsets.cache not found. Pass the plugin root folder, e.g. -CacheDir 'C:\ARK\ShooterGame\Binaries\Win64\ArkApi'" -ForegroundColor Red
    exit 1
}

$files = @(Get-ChildItem -Path $cacheRoot -Recurse -Filter "cached_offsets.cache" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)

if ($files.Count -eq 0) {
    Write-Host "cached_offsets.cache not found under $cacheRoot" -ForegroundColor Red
    exit 1
}

$match = if ($All -or [string]::IsNullOrEmpty($Pattern)) { "" } else { $Pattern }

foreach ($file in $files) {
    Write-Host ("=== {0}   (modified {1:yyyy-MM-dd HH:mm})" -f $file.FullName, $file.LastWriteTime) -ForegroundColor Cyan

    $stream = [IO.File]::OpenRead($file.FullName)
    $reader = New-Object IO.BinaryReader($stream)
    try {
        $total = 0
        $found = 0

        while ($stream.Position -lt $stream.Length) {
            $keySize = $reader.ReadUInt64()
            if ($keySize -eq 0 -or $keySize -gt 1048576 -or
                ($stream.Position + [int64]$keySize + 8) -gt $stream.Length) {
                Write-Host "warning: unexpected cache format, stopped at offset $($stream.Position)" -ForegroundColor Yellow
                break
            }

            $key = [Text.Encoding]::UTF8.GetString($reader.ReadBytes([int]$keySize))
            $null = $reader.ReadInt64()
            $total++

            if ($All -or $key.IndexOf($match, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                Write-Host $key
                $found++
            }
        }

        Write-Host ("entries: {0}, matched: {1}" -f $total, $found) -ForegroundColor DarkGray
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}
