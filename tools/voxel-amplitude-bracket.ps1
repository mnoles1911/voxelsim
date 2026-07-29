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
# THE SHADER IS NOT OPTIONAL. The first run of this script edited only the CPU,
# and the GPU -- which is what actually generates the rendered voxels -- kept
# producing unscaled terrain, so 1.0x, 0.5x and 0.25x came back looking the same
# and the arms were silently CPU/GPU divergent. Both files, every arm.
$Ush  = Join-Path $Root 'voxel-core\shaders\worldgen.ush'
$Const = 'kDetailAmplitudeScaleQ10'

$original = Select-String -Path $Amp -Pattern "constexpr int64_t $Const = (\d+);"
if (-not $original) { throw "could not find $Const in $Amp" }
$originalValue = [int]$original.Matches[0].Groups[1].Value
if (-not (Select-String -Path $Ush -Pattern "static const int64_t $Const = \d+;")) {
    throw "could not find $Const in $Ush -- the mirror is missing and this bracket would measure nothing"
}
Write-Host "original $Const = $originalValue (will restore both files)" -ForegroundColor Cyan

function Set-Scale([int]$v) {
    (Get-Content $Amp) -replace "constexpr int64_t $Const = \d+;", "constexpr int64_t $Const = $v;" |
        Set-Content $Amp -Encoding utf8
    (Get-Content $Ush) -replace "static const int64_t $Const = \d+;", "static const int64_t $Const = $v;" |
        Set-Content $Ush -Encoding utf8
}

$results = @()
try {
    foreach ($s in $Scales) {
        Write-Host "`n===== scale $s =====" -ForegroundColor Yellow
        Set-Scale $s

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
        # Respin so vxc_gpu stays meaningful for this arm. UE compiles worldgen.ush
        # itself, so the capture does not depend on this -- but a bracket that
        # leaves the two sides disagreeing is how the last one went wrong.
        & (Join-Path $PSScriptRoot 'compile-shaders.ps1') -UpdatePrebuilt 2>&1 |
            Select-String -Pattern 'error' | Select-Object -First 2
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
    # RESTORE AND REBUILD. Restoring the source alone leaves build-p3 holding the
    # last arm's binary, which then disagrees with the freshly-restored shader --
    # that produced a 99.96%-of-columns vxc_gpu failure that looked like a real
    # determinism break and was only a stale artifact.
    Set-Scale $originalValue
    cmake --build $BuildDir --config Release 2>&1 | Select-String -Pattern 'error C' | Select-Object -First 3
    & (Join-Path $PSScriptRoot 'compile-shaders.ps1') -UpdatePrebuilt 2>&1 |
        Select-String -Pattern 'Updated prebuilt|error' | Select-Object -Last 2
    Write-Host "restored $Const = $originalValue and rebuilt both sides" -ForegroundColor Cyan
}

$results | Format-Table -AutoSize
Write-Host "Digests MUST differ between arms; identical digests mean the scale never reached evalSurface."
