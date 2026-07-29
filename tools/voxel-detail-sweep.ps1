# Sweep detail-band constants and report TERRACE STATISTICS, not screenshots.
#
# WHY THIS REPLACED THE SCREENSHOT LOOP. The amplitude bracket cost ~5 minutes an
# arm (rebuild voxel-core, relink UE, launch the editor, settle 120 s, capture)
# and its answer was a judgement call on a compressed high-oblique view. Then
# `terracePlateaus` turned out to already exist in terrainprobe.cpp, described in
# that file as "the metric that actually matches the screenshot" -- so the
# instrument for this was there the whole time and nobody had pointed it at the
# question.
#
# It measures the right thing. Voxelising takes floor(h/100mm), so a smooth
# surface becomes flat runs separated by one-voxel risers. Long runs read as
# machined terracing; runs of two or three voxels read as static. The shipped v10
#配置 measures mean run 2.62 voxels, median 2 -- the surface changes voxel height
# every 2.6 columns, an effective local gradient of ~0.38 everywhere, which is
# exactly what the in-engine capture shows.
#
# Only the CPU probe is needed, so no shader respin and no UE build: seconds an
# arm instead of minutes. The shader still has to be mirrored before anything is
# SHIPPED -- see tools/voxel-ablate.ps1 -- but for choosing a number, this is the
# loop.
#
# Usage:
#   tools\voxel-detail-sweep.ps1

param(
    [string]$Site = '-84480,53760',
    [string]$BuildDir = 'D:\voxelsim\build-p3',
    [string]$TileDir = 'D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-3e11cf157a836c70\000000000135276f\s1'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Amp  = Join-Path $Root 'voxel-core\src\amplifier.cpp'
$Rill = Join-Path $Root 'voxel-core\include\voxelcore\detail_rill.h'
$Bed  = Join-Path $Root 'voxel-core\include\voxelcore\detail_bedding.h'

$xy = $Site.Split(',')

# name, amplitudeScaleQ10, rillMm, beddingMm, octaveOverride ("lattice=amp,...")
$arms = @(
    @{ n='shipped';          s=1024; r=300; b=120; o=$null },
    @{ n='no-bedding';       s=1024; r=300; b=1;   o=$null },
    @{ n='no-rill';          s=1024; r=1;   b=120; o=$null },
    @{ n='no-rill-no-bed';   s=1024; r=1;   b=1;   o=$null },
    @{ n='scale-0.5';        s=512;  r=300; b=120; o=$null },
    @{ n='scale-0.25';       s=256;  r=300; b=120; o=$null },
    @{ n='scale-0.25+ablate';s=256;  r=1;   b=1;   o=$null }
)

$origAmp  = (Select-String -Path $Amp  -Pattern 'constexpr int64_t kDetailAmplitudeScaleQ10 = (\d+);').Matches[0].Groups[1].Value
$origRill = (Select-String -Path $Rill -Pattern 'inline constexpr int64_t kRillAmplitudeMm = (\d+);').Matches[0].Groups[1].Value
$origBed  = (Select-String -Path $Bed  -Pattern 'inline constexpr int64_t kBeddingAmpMm = (\d+);').Matches[0].Groups[1].Value
Write-Host "originals: scale=$origAmp rill=$origRill bedding=$origBed" -ForegroundColor Cyan

function Set-Arm($s, $r, $b) {
    (Get-Content $Amp)  -replace 'constexpr int64_t kDetailAmplitudeScaleQ10 = \d+;', "constexpr int64_t kDetailAmplitudeScaleQ10 = $s;" | Set-Content $Amp -Encoding utf8
    (Get-Content $Rill) -replace 'inline constexpr int64_t kRillAmplitudeMm = \d+;', "inline constexpr int64_t kRillAmplitudeMm = $r;" | Set-Content $Rill -Encoding utf8
    (Get-Content $Bed)  -replace 'inline constexpr int64_t kBeddingAmpMm = \d+;',    "inline constexpr int64_t kBeddingAmpMm = $b;"    | Set-Content $Bed -Encoding utf8
}

$rows = @()
try {
    foreach ($a in $arms) {
        Set-Arm $a.s $a.r $a.b
        # Probe target only: the test binary pins constants that legitimately move
        # under an ablation, and rebuilding it would fail the arm for the wrong reason.
        $build = cmake --build $BuildDir --config Release --target vxc_terrainprobe 2>&1
        $errs = $build | Select-String -Pattern 'error C' | Select-Object -First 2
        if ($errs) {
            Write-Warning "$($a.n): build failed"; $errs | ForEach-Object { Write-Host "    $_" }
            $rows += [pscustomobject]@{ Arm=$a.n; MeanRun='BUILD FAIL'; Median=''; P90=''; Plateaus=''; Flat4m='' }
            continue
        }
        $out = & "$BuildDir\bench\Release\vxc_terrainprobe.exe" $TileDir 20260719 $xy[0] $xy[1] 2000 2>&1
        $runLine  = ($out | Select-String -Pattern 'count=\d+\s+mean=' | Select-Object -First 1).ToString()
        $plateau  = ($out | Select-String -Pattern 'components=\d+' | Select-Object -First 1).ToString()
        $flat     = ($out | Select-String -Pattern 'fraction of area in plateaus' | Select-Object -First 1).ToString()
        $mean   = if ($runLine -match 'mean=([\d.]+)')            { $Matches[1] } else { '?' }
        $median = if ($runLine -match 'median=(\d+)')             { $Matches[1] } else { '?' }
        $p90    = if ($runLine -match 'p90=(\d+)')                { $Matches[1] } else { '?' }
        $comp   = if ($plateau -match 'components=(\d+)')         { $Matches[1] } else { '?' }
        $f4     = if ($flat    -match ':\s*([\d.]+)%')            { $Matches[1] } else { '?' }
        $rows += [pscustomobject]@{ Arm=$a.n; MeanRun=$mean; Median=$median; P90=$p90; Plateaus=$comp; Flat4m=$f4 }
        Write-Host ("  {0,-18} mean run {1,5} vox  median {2,2}  p90 {3,3}  plateaus {4,6}  >=4m flat {5,5}%" -f $a.n,$mean,$median,$p90,$comp,$f4)
    }
}
finally {
    Set-Arm $origAmp $origRill $origBed
    cmake --build $BuildDir --config Release --target vxc_terrainprobe 2>&1 | Select-String -Pattern 'error C' | Select-Object -First 2
    Write-Host "restored" -ForegroundColor Cyan
}

Write-Host ""
$rows | Format-Table -AutoSize
Write-Host "Higher mean run = flatter/terraced. Lower = noisier. v10 shipped is 2.62 voxels, which reads as static."
