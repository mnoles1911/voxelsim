# Fetches NASA SVS sky-dome reference textures (star map + moon) into
# tools/sky-assets/, consumed by ue-project/Tools/import_sky_textures.py.
# Public domain / attribution-required (recorded in
# ue-project/Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md, which this
# script does not modify -- rerun the credits update by hand if you re-pin).
#
# Pinned by recorded byte size (and sha256 once downloaded, in the credits
# file) rather than a version tag, because these are not versioned releases
# like the DXC/Vulkan-Headers fetches (fetch-dxc.ps1, fetch-vulkan-headers.ps1)
# -- an SVS page can have its file replaced in place with no name or URL
# change. The size check below is a tripwire for that: if NASA swaps the
# file, the download will land but the byte count won't match, and this
# script warns instead of silently accepting different bytes under the same
# pinned filename.
#
# URLs verified 2026-07-29 via HEAD request only (Invoke-WebRequest -Method
# Head from this machine; no bytes downloaded to test):
#   starmap_2020_4k.exr        HTTP 200  Content-Length 35997085  (34.3 MB)
#   starmap_2020_8k.exr        HTTP 200  Content-Length 130530278 (124.5 MB)
#   lroc_color_poles_4k.tif    HTTP 200  Content-Length 13095388  (12.5 MB)
#   ldem_4_uint.tif            HTTP 200  Content-Length 2076866   (2.0 MB)
# Landing pages: https://svs.gsfc.nasa.gov/4851 (star map),
#                https://svs.gsfc.nasa.gov/4720 (moon)
#
# Attribution (required -- see SKY_ASSET_CREDITS.md for the full record):
#   Star map: NASA/Goddard Space Flight Center Scientific Visualization
#   Studio. Gaia DR2: ESA/Gaia/DPAC.
#   Moon: NASA's Scientific Visualization Studio.
#
# Default fetch is the 4k star map (34 MB) -- plenty for a sky dome at
# first and keeps the default fetch quick. Pass -EightK for the 124 MB
# 8192x4096 version.
param(
    [switch]$EightK
)
$ErrorActionPreference = "Stop"

$Dest = Join-Path $PSScriptRoot "sky-assets"
New-Item -ItemType Directory -Force $Dest | Out-Null

$StarBase = "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851"
$MoonBase = "https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720"

if ($EightK) {
    $StarFile = "starmap_2020_8k.exr"
    $StarSize = 130530278
} else {
    $StarFile = "starmap_2020_4k.exr"
    $StarSize = 35997085
}

# name, source page (for the credits record), url, file, pinned size in bytes
$Assets = @(
    @{ Name = "Star map"; Page = "https://svs.gsfc.nasa.gov/4851"; Url = "$StarBase/$StarFile"; File = $StarFile; Size = $StarSize },
    @{ Name = "Moon colour"; Page = "https://svs.gsfc.nasa.gov/4720"; Url = "$MoonBase/lroc_color_poles_4k.tif"; File = "lroc_color_poles_4k.tif"; Size = 13095388 },
    @{ Name = "Moon displacement"; Page = "https://svs.gsfc.nasa.gov/4720"; Url = "$MoonBase/ldem_4_uint.tif"; File = "ldem_4_uint.tif"; Size = 2076866 }
)

foreach ($asset in $Assets) {
    $outPath = Join-Path $Dest $asset.File
    if (Test-Path $outPath) {
        Write-Host "$($asset.Name) already present at $outPath"
        continue
    }
    Write-Host "Downloading $($asset.Name): $($asset.Url)"
    Invoke-WebRequest -Uri $asset.Url -OutFile $outPath

    $actualSize = (Get-Item $outPath).Length
    if ($actualSize -ne $asset.Size) {
        Write-Warning ("{0}: expected {1} bytes, got {2} -- NASA may have " +
            "replaced the file in place. Re-verify before trusting it, and " +
            "update the pinned size/sha256 here and in SKY_ASSET_CREDITS.md." `
            -f $asset.File, $asset.Size, $actualSize)
    }

    $hash = (Get-FileHash -Path $outPath -Algorithm SHA256).Hash.ToLower()
    Write-Host "$($asset.File): sha256=$hash size=$actualSize source=$($asset.Page)"
    Write-Host "  -> record this line in ue-project/Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md"
}

Write-Host "Sky assets ready at $Dest"
