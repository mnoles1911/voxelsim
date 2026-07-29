# Ablate ONE worldgen term to zero, rebuild both sides, and shoot it.
#
# WHY ABLATION AND NOT A SCALE. The amplitude bracket falsified its own premise:
# scaling every octave to 1/4 left the ground still reading as a dense field of
# one-voxel steps. That is only possible if the static is not coming from the
# octave ladder at all -- and indeed `scaledAmpMm` touches only the octave table,
# while the rill (300 mm) and bedding (120 mm) terms are ADDITIVE and unscaled,
# so at 1/4 scale they become the dominant contributors rather than the quiet
# ones. A scale sweep cannot separate them. Turning one term off at a time can.
#
# BOTH SIDES, ALWAYS. The first bracket edited only the CPU while the GPU -- which
# generates the rendered voxels -- kept the old value, so every arm looked the
# same and the builds were silently divergent. This script edits the C++ header
# and worldgen.ush together and refuses to start if it cannot find both.
#
# Usage:
#   tools\voxel-ablate.ps1 -Const kRillAmplitudeMm    -Header detail_rill.h
#   tools\voxel-ablate.ps1 -Const kBeddingAmpMm       -Header detail_bedding.h -Value 0

param(
    [Parameter(Mandatory=$true)][string]$Const,
    [Parameter(Mandatory=$true)][string]$Header,
    [int]$Value = 0,
    [string]$SpawnAt = '-84480,53760',
    [int]$SettleSec = 120,
    [string]$BuildDir = 'D:\voxelsim\build-p3'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Hdr  = Join-Path $Root "voxel-core\include\voxelcore\$Header"
$Ush  = Join-Path $Root 'voxel-core\shaders\worldgen.ush'

$hMatch = Select-String -Path $Hdr -Pattern "inline constexpr int64_t $Const = (\d+);"
$uMatch = Select-String -Path $Ush -Pattern "static const int64_t $Const = (\d+);"
if (-not $hMatch) { throw "$Const not found in $Hdr" }
if (-not $uMatch) { throw "$Const not found in $Ush -- ablating one side only measures nothing" }
$hOrig = [int]$hMatch.Matches[0].Groups[1].Value
$uOrig = [int]$uMatch.Matches[0].Groups[1].Value
if ($hOrig -ne $uOrig) { throw "MIRROR ALREADY BROKEN: $Const is $hOrig in C++ and $uOrig in the shader" }
Write-Host "$Const : $hOrig -> $Value (both sides), will restore" -ForegroundColor Cyan

function Set-Both([int]$v) {
    (Get-Content $Hdr) -replace "inline constexpr int64_t $Const = \d+;", "inline constexpr int64_t $Const = $v;" |
        Set-Content $Hdr -Encoding utf8
    (Get-Content $Ush) -replace "static const int64_t $Const = \d+;", "static const int64_t $Const = $v;" |
        Set-Content $Ush -Encoding utf8
}

try {
    Set-Both $Value
    $build = cmake --build $BuildDir --config Release 2>&1
    $errs = $build | Select-String -Pattern 'error C' | Select-Object -First 3
    if ($errs) {
        Write-Warning "$Const = $Value does not build:"
        $errs | ForEach-Object { Write-Host "    $_" }
        return
    }
    & (Join-Path $PSScriptRoot 'compile-shaders.ps1') -UpdatePrebuilt 2>&1 |
        Select-String -Pattern 'error' | Select-Object -First 2
    Write-Host ("  digest " + (& "$BuildDir\bench\Release\vxc_bench.exe" --radius 128 --digest 2>&1 | Select-Object -Last 1))
    # Parity per arm: an ablation that diverges CPU from GPU is not an experiment,
    # it is two different worlds photographed at once.
    $gpu = & "$BuildDir\bench\Release\vxc_gpu.exe" 2>&1 | Select-String -Pattern 'PASS|FAIL' | Select-Object -Last 1
    Write-Host "  $gpu"

    Copy-Item "$BuildDir\Release\voxelcore.lib" (Join-Path $Root 'build\voxel-core-msvc\voxelcore.lib') -Force
    & "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" VoxelEarthEditor Win64 Development `
        -Project="$Root\ue-project\VoxelEarth.uproject" -WaitMutex 2>&1 |
        Select-String -Pattern 'Result:' | Select-Object -Last 1
    & (Join-Path $PSScriptRoot 'voxel-capture.ps1') -Name "ablate-$Const" -SpawnAt $SpawnAt -SettleSec $SettleSec
}
finally {
    Set-Both $hOrig
    cmake --build $BuildDir --config Release 2>&1 | Select-String -Pattern 'error C' | Select-Object -First 2
    & (Join-Path $PSScriptRoot 'compile-shaders.ps1') -UpdatePrebuilt 2>&1 |
        Select-String -Pattern 'Updated prebuilt' | Select-Object -Last 1
    Write-Host "restored $Const = $hOrig and rebuilt both sides" -ForegroundColor Cyan
}
