# Bracket kDetailAmplitudeScaleQ10 by eye: build, capture, repeat.
#
# WHY A BRACKET AND NOT A TUNE. The detail band's LEVEL is the one parameter in
# this whole stack that no instrument in the repo can decide. Bounds, digests,
# GPU parity, the seam ratio, the local Hurst exponent and two flight legs all
# passed on a v10 that reads in-engine as per-voxel static. The S2 solve says the
# band is 4-7x too loud but its target is anchored on an int16-METRE raster whose
# quantisation floor sits ABOVE the amplitude it extrapolates to, so it cannot be
# taken literally either. When neither the instrument nor the theory can settle a
# number, bracket it and look.
#
# Each arm is a full rebuild because the scale is a compile-time constant under
# kWorldGenVersion, deliberately: a cvar that changes worldgen would let two
# clients disagree about the ground.
#
# The script edits amplifier.cpp in place, builds voxel-core, relinks the UE
# module against it, captures, and then RESTORES the file. If it dies partway the
# constant may be left at an arm's value -- it prints the original so that is
# recoverable, and `git diff voxel-core/src/amplifier.cpp` shows it.
#
# Usage:
#   tools\voxel-amplitude-bracket.ps1 -Scales 1024,512,256 -SpawnAt '-84480,53760'

param(
    [int[]]$Scales = @(1024, 512, 256),
    [string]$SpawnAt = '-84480,53760',
    [string]$Tag = 'amp',
    [int]$SettleSec = 120,
    [string]$BuildDir = 'D:\voxelsim\build-p3'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path "$PSScriptRoot\..").Path
$Amp  = Join-Path $Root 'voxel-core\src\amplifier.cpp'
$Const = 'kDetailAmplitudeScaleQ10'

$original = Select-String -Path $Amp -Pattern "constexpr int64_t $Const = (\d+);"
if (-not $original) { throw "could not find $Const in $Amp" }
$originalValue = [int]$original.Matches[0].Groups[1].Value
Write-Host "original $Const = $originalValue (will restore)" -ForegroundColor Cyan

$results = @()
try {
    foreach ($s in $Scales) {
        Write-Host "`n===== scale $s =====" -ForegroundColor Yellow
        (Get-Content $Amp) -replace "constexpr int64_t $Const = \d+;", "constexpr int64_t $Const = $s;" |
            Set-Content $Amp -Encoding utf8

        # The bound's static_asserts are DERIVED from the table, so they move with
        # the scale and will fail until re-pinned. That is the guard working, not a
        # bug -- but it makes an arm unbuildable, so report it and skip rather than
        # editing the asserts under a bracket that is about to be thrown away.
        $build = cmake --build $BuildDir --config Release 2>&1
        $errs = $build | Select-String -Pattern 'error C' | Select-Object -First 3
        if ($errs) {
            Write-Warning "scale $s does not build (expected: the derived bound asserts move with the table):"
            $errs | ForEach-Object { Write-Host "    $_" }
            $results += [pscustomobject]@{ Scale = $s; Digest = 'BUILD FAILED'; Shot = '' }
            continue
        }

        $digest = (& "$BuildDir\bench\Release\vxc_bench.exe" --radius 128 --digest 2>&1 | Select-Object -Last 1)
        Copy-Item "$BuildDir\Release\voxelcore.lib" (Join-Path $Root 'build\voxel-core-msvc\voxelcore.lib') -Force
        & "D:\UE_5.8\Engine\Build\BatchFiles\Build.bat" VoxelEarthEditor Win64 Development `
            -Project="$Root\ue-project\VoxelEarth.uproject" -WaitMutex 2>&1 |
            Select-String -Pattern 'Result:' | Select-Object -Last 1

        $shot = & (Join-Path $PSScriptRoot 'voxel-capture.ps1') -Name "$Tag-$s" -SpawnAt $SpawnAt -SettleSec $SettleSec |
            Select-Object -Last 1
        $results += [pscustomobject]@{ Scale = $s; Digest = $digest; Shot = $shot }
    }
}
finally {
    (Get-Content $Amp) -replace "constexpr int64_t $Const = \d+;", "constexpr int64_t $Const = $originalValue;" |
        Set-Content $Amp -Encoding utf8
    Write-Host "`nrestored $Const = $originalValue" -ForegroundColor Cyan
}

$results | Format-Table -AutoSize
Write-Host "Digests MUST differ between arms; identical digests mean the scale never reached evalSurface."
