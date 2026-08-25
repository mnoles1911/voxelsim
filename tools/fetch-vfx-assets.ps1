# Fetches the CC0 VFX and atmosphere source assets listed in
# docs/vfx-and-atmosphere-assets.md into tools/vfx-assets/ (gitignored).
# Provenance, licence and byte sizes are recorded in
# ue-project/Content/VFX/VFX_ASSET_CREDITS.md, which this script does not
# modify -- update it by hand if you re-pin.
#
# Same shape and same reasons as tools/fetch-sky-assets.ps1: these are large
# third-party binaries, the largest tracked binary in this repo is 385 KB, and
# a fetch script plus a credits file is how that squares. Nothing here is
# committed.
#
# PINNED BY RECORDED BYTE SIZE, not a version tag. Poly Haven, ambientCG and
# Kenney can all replace a file in place under the same URL; the size check
# below is the tripwire. A mismatch warns rather than silently accepting
# different bytes under a pinned name.
#
# URLs AND SIZES VERIFIED 2026-08-24 by HEAD request from this machine, no
# bytes downloaded. Every one returned HTTP 200:
#
#   kloofendal_48d_partly_cloudy_puresky_4k.exr   75,640,460   (72.1 MB)
#   kloppenheim_06_4k.hdr                         23,526,002   (22.4 MB)
#   belfast_sunset_puresky_4k.exr                 73,122,844   (69.7 MB)
#   rocky_terrain_02_nor_gl_4k.jpg                16,219,215   (15.5 MB)
#   kenney_particle-pack.zip                      15,001,764   (14.3 MB)
#   kenney_smoke-particles.zip                     6,019,666    (5.7 MB)
#   Rock023_2K-PNG.zip                            63,106,399   (60.2 MB)
#   Ground037_2K-PNG.zip                          72,439,572   (69.1 MB)
#
# NOTE ON SIZE, because the source doc undersells it: that doc calls its
# recommended six "minimal, high-value" without giving figures. Measured, the
# six are 200 MB and the full eight are 329 MB -- against a repo whose largest
# tracked binary is 385 KB. Still the right call to fetch rather than commit,
# but "minimal" describes the count, not the download.
#
# LICENCE: all eight are CC0 / public domain, and none requires attribution.
# That is why this set was chosen over OpenGameArt, whose uploads are a
# per-asset mix of CC0/CC-BY/GPL. Re-check the licence badge on the asset page
# before shipping if this is revisited -- these three sources are CC0-stable,
# but "was CC0 in August 2026" is a fact with a date on it.
#
# WHAT IS ALREADY DONE, DO NOT RE-FETCH: the sky half of that document. The
# star map and moon textures come from tools/fetch-sky-assets.ps1 and are
# recorded in ue-project/Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md,
# which carries a NASA attribution obligation this set does not have.
param(
    # Skip the two ambientCG PBR sets (129 MB of the 329 MB) -- the sprite
    # packs and HDRIs are what a first Niagara emitter actually needs.
    [switch]$SkipGroundTextures,
    # Print the plan and exit without downloading anything.
    [switch]$WhatIfOnly
)
$ErrorActionPreference = "Stop"

$Dest = Join-Path $PSScriptRoot "vfx-assets"

$PolyHavenBase = "https://dl.polyhaven.org/file/ph-assets"
$KenneyBase    = "https://www.kenney.nl/media/pages/assets"

$Assets = @(
    @{ Name = "kloofendal_48d_partly_cloudy_puresky_4k.exr"
       Url  = "$PolyHavenBase/HDRIs/exr/4k/kloofendal_48d_partly_cloudy_puresky_4k.exr"
       Size = 75640460
       Use  = "SkyLight + reflection capture, clear / partly-cloudy day state" }
    @{ Name = "kloppenheim_06_4k.hdr"
       Url  = "$PolyHavenBase/HDRIs/hdr/4k/kloppenheim_06_4k.hdr"
       Size = 23526002
       Use  = "SkyLight + reflection, overcast / soft-light state" }
    @{ Name = "belfast_sunset_puresky_4k.exr"
       Url  = "$PolyHavenBase/HDRIs/exr/4k/belfast_sunset_puresky_4k.exr"
       Size = 73122844
       Use  = "SkyLight + reflection, dusk / golden-hour state" }
    @{ Name = "rocky_terrain_02_nor_gl_4k.jpg"
       Url  = "$PolyHavenBase/Textures/jpg/4k/rocky_terrain_02/rocky_terrain_02_nor_gl_4k.jpg"
       Size = 16219215
       Use  = "Wet-rock detail normal, and a coarse water ripple layer. nor_gl is the OpenGL convention -- the right one for UE5. Do NOT substitute nor_dx." }
    @{ Name = "kenney_particle-pack.zip"
       Url  = "$KenneyBase/particle-pack/f8fe0f8cb8-1677578741/kenney_particle-pack.zip"
       Size = 15001764
       Use  = "Niagara sprites: splash, spark, glow, embers (80 sprites, 512x512 PNG)" }
    @{ Name = "kenney_smoke-particles.zip"
       Url  = "$KenneyBase/smoke-particles/23249a0d35-1677695171/kenney_smoke-particles.zip"
       Size = 6019666
       Use  = "Niagara sprites: smoke and fog wisps, chimney smoke, dust, storm haze (70 sprites)" }
)

$GroundTextures = @(
    @{ Name = "Rock023_2K-PNG.zip"
       Url  = "https://ambientcg.com/get?file=Rock023_2K-PNG.zip"
       Size = 63106399
       Use  = "Cliff / stone detail normal for voxel rock faces, wet-surface sheen" }
    @{ Name = "Ground037_2K-PNG.zip"
       Url  = "https://ambientcg.com/get?file=Ground037_2K-PNG.zip"
       Size = 72439572
       Use  = "Ground detail normal + roughness, damp moss and earth under rain wetness" }
)

if (-not $SkipGroundTextures) { $Assets += $GroundTextures }

# ForEach-Object, not `Measure-Object -Property Size`: these are hashtables,
# and in Windows PowerShell 5.1 Measure-Object reads PSObject PROPERTIES, which
# a hashtable key is not. The -Property form fails outright here.
$TotalBytes = ($Assets | ForEach-Object { $_.Size } | Measure-Object -Sum).Sum
Write-Host ""
Write-Host ("fetch-vfx-assets: {0} files, {1:N0} MB total" -f $Assets.Count, ($TotalBytes / 1MB))
Write-Host ("destination: {0}" -f $Dest)
Write-Host ""
foreach ($a in $Assets) {
    Write-Host ("  {0,-46} {1,8:N1} MB  {2}" -f $a.Name, ($a.Size / 1MB), $a.Use)
}
Write-Host ""

if ($WhatIfOnly) {
    Write-Host "-WhatIfOnly: nothing downloaded."
    return
}

New-Item -ItemType Directory -Force $Dest | Out-Null

$Mismatched = @()
foreach ($a in $Assets) {
    $out = Join-Path $Dest $a.Name
    if (Test-Path $out) {
        $have = (Get-Item $out).Length
        if ($have -eq $a.Size) {
            Write-Host ("skip (already correct): {0}" -f $a.Name)
            continue
        }
        Write-Host ("re-fetching (size {0:N0} != pinned {1:N0}): {2}" -f $have, $a.Size, $a.Name)
    }
    Write-Host ("fetching {0} ..." -f $a.Name)
    Invoke-WebRequest -Uri $a.Url -OutFile $out -UseBasicParsing

    $got = (Get-Item $out).Length
    if ($got -ne $a.Size) {
        # NOT a hard failure: the file may legitimately have been updated
        # upstream. But it is no longer the file this script was pinned to,
        # and accepting it silently is how a swapped asset gets shipped.
        Write-Warning ("SIZE MISMATCH for {0}: got {1:N0}, pinned {2:N0}. The upstream file may have been replaced in place. Verify the asset page and update the pin in this script AND in VFX_ASSET_CREDITS.md before using it." -f $a.Name, $got, $a.Size)
        $Mismatched += $a.Name
    }
    $sha = (Get-FileHash -Algorithm SHA256 $out).Hash.ToLower()
    Write-Host ("  {0:N1} MB  sha256 {1}" -f ($got / 1MB), $sha)
}

Write-Host ""
if ($Mismatched.Count -gt 0) {
    Write-Warning ("{0} file(s) did not match their pinned size: {1}" -f $Mismatched.Count, ($Mismatched -join ", "))
}
Write-Host "Record the sha256 values above in ue-project/Content/VFX/VFX_ASSET_CREDITS.md."
Write-Host "Nothing here is committed -- tools/vfx-assets/ is gitignored."
