# Fetches the NIGHT-SKY REVIEW SET into tools/sky-assets/options/ -- a spread of
# candidate full-sky (equirectangular) skybox textures for the owner to pick from,
# NOT the shipping asset. The shipping fetch is the separate, pinned
# tools/fetch-sky-assets.ps1; this script is deliberately a superset that
# includes options we may never use.
#
# Every file here is public domain, CC0, or CC BY 4.0 (or a CC-BY-4.0-derived
# grant that waives attribution). The per-file source URL, author, licence,
# verified pixel dimensions and required attribution line are recorded in
# tools/sky-assets/options/README.md -- that file is the licence record and
# MUST be updated in the same change as any edit here. Rejected candidates and
# why are recorded there too, so the same dead ends are not re-walked.
#
# Style follows fetch-sky-assets.ps1: pinned byte sizes as a tripwire (none of
# these are versioned releases -- a CMS can swap a file in place under the same
# URL), skip-if-present so reruns are cheap, sha256 printed for the record.
# Unlike fetch-sky-assets.ps1 these downloads are resumable: several are
# 100-600 MB, so a dropped connection should not restart from zero. Resume is
# done with curl.exe -C - (shipped with Windows 10+); Invoke-WebRequest has no
# resume. A partial file is left in place on failure precisely so the next run
# continues it -- which is why the size check below is load-bearing: a truncated
# file is the expected failure mode here, not a corrupted one.
#
# Sizes verified 2026-08-10 by downloading (not by HEAD): the byte counts below
# are what actually landed on disk and were opened successfully.
#
# NOT FETCHED HERE -- two candidates need a step this script cannot pin:
#
#   NASA SVS 4851 starmap_2020_16k.exr (16384x8192, ~423 MB, public domain).
#   The single best realistic starfield in the set and the direct upgrade for
#   the 4k map's measured grain (see ue-project/Tools/import_sky_textures.py,
#   "WHICH STAR MAP"). svs.gsfc.nasa.gov (169.154.143.10:443) was unreachable
#   from this machine all through 2026-08-10 -- TCP connect timeout, not a 404
#   or a DNS failure, while www.nasa.gov and science.nasa.gov answered fine, so
#   this is an outage or a route block and not a moved URL. Pass -RetryNasa to
#   sit in a retry loop and grab it when the host comes back.
#
#   Space Spheremaps itch.io sampler (4x 8192x4096 PNG). itch.io hands out a
#   60-second signed CDN URL in response to an authenticated-by-CSRF POST, so
#   there is no stable URL to pin. Get-ItchSampler below does the two-step; it
#   is off by default because it depends on itch.io's private endpoint shape
#   (POST /<slug>/file/<upload_id>) which can change without notice. Pass
#   -ItchSampler to try it, or just download the zip by hand from
#   https://space-spheremaps.itch.io/space-spheremaps .
param(
    [switch]$RetryNasa,
    [switch]$ItchSampler,
    [int]$NasaRetryMinutes = 60
)
$ErrorActionPreference = "Stop"

$Dest = Join-Path (Join-Path $PSScriptRoot "sky-assets") "options"
New-Item -ItemType Directory -Force $Dest | Out-Null

$Curl = "$env:SystemRoot\System32\curl.exe"
if (-not (Test-Path $Curl)) { throw "curl.exe not found at $Curl -- required for resumable downloads" }

# Name        short label for console output
# Page        the human landing page, where the licence is stated (record it)
# Url         direct file URL
# File        destination filename under options/
# Size        pinned byte count -- tripwire for in-place replacement AND for a
#             truncated resume
# Referer     some CDNs (solarsystemscope) 403 a bare request
$Options = @(
    @{ Name = "Solar System Scope 8k stars";        Page = "https://www.solarsystemscope.com/textures/";
       Url  = "https://www.solarsystemscope.com/textures/download/8k_stars.jpg";
       File = "sss_8k_stars.jpg";                   Size = 1753492;
       Referer = "https://www.solarsystemscope.com/textures/" },

    @{ Name = "Solar System Scope 8k stars+MilkyWay"; Page = "https://www.solarsystemscope.com/textures/";
       Url  = "https://www.solarsystemscope.com/textures/download/8k_stars_milky_way.jpg";
       File = "sss_8k_stars_milky_way.jpg";         Size = 1905513;
       Referer = "https://www.solarsystemscope.com/textures/" },

    @{ Name = "ESO GigaGalaxy Zoom Milky Way";      Page = "https://www.eso.org/public/images/eso0932a/";
       Url  = "https://cdn.eso.org/images/original/eso0932a.tif";
       File = "eso0932a_milkyway_panorama.tif";     Size = 29082084 },

    @{ Name = "Poly Haven Qwantani Night 24k";      Page = "https://polyhaven.com/a/qwantani_night_puresky";
       Url  = "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/16k%2B/qwantani_night_puresky_24k.hdr";
       File = "qwantani_night_puresky_24k.hdr";     Size = 627680593 },

    @{ Name = "Poly Haven Kloppenheim 02 16k";      Page = "https://polyhaven.com/a/kloppenheim_02_puresky";
       Url  = "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/16k%2B/kloppenheim_02_puresky_16k.hdr";
       File = "kloppenheim_02_puresky_16k.hdr";     Size = 296041508 },

    @{ Name = "Space Spheremaps multi-nebulae 1";   Page = "https://www.spacespheremaps.com/multi-nebulae-spheremaps/";
       Url  = "https://www.spacespheremaps.com/wp-content/uploads/HDR_multi_nebulae_1.hdr";
       File = "ssm_HDR_multi_nebulae_1.hdr";        Size = 93360461;
       Referer = "https://www.spacespheremaps.com/hdr-spheremaps/" },

    @{ Name = "Space Spheremaps blue nebulae 2";    Page = "https://www.spacespheremaps.com/blue-nebulae-spheremaps/";
       Url  = "https://www.spacespheremaps.com/wp-content/uploads/HDR_blue_nebulae_2.hdr";
       File = "ssm_HDR_blue_nebulae_2.hdr";         Size = 91129911;
       Referer = "https://www.spacespheremaps.com/hdr-spheremaps/" },

    @{ Name = "Space Spheremaps galactic plane 1";  Page = "https://www.spacespheremaps.com/galactic-plane-spheremaps/";
       Url  = "https://www.spacespheremaps.com/wp-content/uploads/HDR_galactic_plane_1.hdr";
       File = "ssm_HDR_galactic_plane_1.hdr";       Size = 89923360;
       Referer = "https://www.spacespheremaps.com/hdr-spheremaps/" }
)

function Get-Option($opt) {
    $outPath = Join-Path $Dest $opt.File
    if ((Test-Path $outPath) -and ((Get-Item $outPath).Length -eq $opt.Size)) {
        Write-Host "$($opt.Name): already present and correct size"
        return
    }
    Write-Host "Downloading $($opt.Name) -> $($opt.File)"
    # Not $args -- that is an automatic variable and splatting it is a trap.
    $curlArgs = @("-sL", "-4", "--max-time", "3600", "-C", "-",
                  "-A", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                  "-o", $outPath, $opt.Url)
    if ($opt.Referer) { $curlArgs = @("-e", $opt.Referer) + $curlArgs }
    & $Curl @curlArgs
    if ($LASTEXITCODE -ne 0) {
        # Left on disk on purpose: -C - resumes it next run.
        Write-Warning "$($opt.File): curl exit $LASTEXITCODE -- partial file kept, rerun to resume"
        return
    }

    $actual = (Get-Item $outPath).Length
    if ($actual -ne $opt.Size) {
        Write-Warning ("{0}: expected {1} bytes, got {2}. Either the download is " +
            "truncated (rerun to resume) or the host replaced the file in place " +
            "-- re-verify the licence and dimensions and update the pin here and " +
            "in options/README.md." -f $opt.File, $opt.Size, $actual)
    }
    $hash = (Get-FileHash -Path $outPath -Algorithm SHA256).Hash.ToLower()
    Write-Host "  $($opt.File): sha256=$hash size=$actual source=$($opt.Page)"
}

foreach ($opt in $Options) { Get-Option $opt }

function Get-ItchSampler {
    # itch.io free-download flow, as implemented by static.itch.io/game.min.js:
    # POST /<slug>/file/<upload_id>?source=view_game&as_props=1 with the page's
    # csrf_token, which answers {"url": "<signed CDN url>"} valid for 60s.
    # Upload id is pinned; it changes if the author re-uploads the pack.
    $slug = "space-spheremaps"
    $uploadId = 9043348
    $out = Join-Path $Dest "space_spheremaps.zip"
    if (Test-Path $out) { Write-Host "itch sampler zip already present"; return }

    $page = Invoke-WebRequest -Uri "https://$slug.itch.io/$slug" -UseBasicParsing
    if ($page.Content -notmatch 'name="csrf_token"\s+value="([^"]+)"') {
        throw "itch.io: no csrf_token on the page -- the endpoint shape has changed, download by hand"
    }
    $csrf = $Matches[1]
    $resp = Invoke-RestMethod -Method Post `
        -Uri "https://$slug.itch.io/$slug/file/$uploadId`?source=view_game&as_props=1" `
        -Body @{ csrf_token = $csrf } `
        -Headers @{ Referer = "https://$slug.itch.io/$slug" }
    if (-not $resp.url) { throw "itch.io: no signed url in response -- download by hand" }

    Write-Host "Downloading itch sampler (signed url expires in 60s)"
    & $Curl "-sL" "-4" "--max-time" "600" "-o" $out $resp.url
    # The zip is the delivered original; the four PNGs inside are what the
    # integrator wants, so expand and drop the container.
    Expand-Archive -Path $out -DestinationPath $Dest -Force
    Remove-Item $out
    Write-Host "  expanded 4x 8192x4096 PNG into $Dest"
}

if ($ItchSampler) { Get-ItchSampler }

if ($RetryNasa) {
    # See the header: this host was down, not moved. Poll rather than fail, and
    # download the moment it answers anything at all (any HTTP status means the
    # route is back; a 404 there would be real news worth seeing).
    $url  = "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/starmap_2020_16k.exr"
    $out  = Join-Path $Dest "starmap_2020_16k.exr"
    $deadline = (Get-Date).AddMinutes($NasaRetryMinutes)
    while ((Get-Date) -lt $deadline) {
        & $Curl "-s" "-4" "--max-time" "20" "-o" "$env:TEMP\svsprobe" "-w" "%{http_code}" `
            "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/" | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "SVS reachable -- downloading starmap_2020_16k.exr (~423 MB)"
            & $Curl "-sL" "-4" "--max-time" "3600" "-C" "-" "-o" $out $url
            Write-Host "  size=$((Get-Item $out).Length) -- record sha256 + dimensions in options/README.md"
            return
        }
        Start-Sleep -Seconds 30
    }
    Write-Warning "svs.gsfc.nasa.gov still unreachable after $NasaRetryMinutes min -- see header"
}

Write-Host "Sky options in $Dest -- licences recorded in options/README.md"
