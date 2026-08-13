# Everything that has to happen before wind-driven waves and interactive ripples
# can be play-tested, in the order it has to happen in.
#
# WHY A SCRIPT AND NOT FOUR COMMANDS: because the ordering rules here are not
# guessable and getting them wrong is silent. Two of the four steps have already
# failed silently on this project in the last day.
#
# THE ORDER
# =========
#
#   1. voxelcore.lib, forced. Nothing in this pass changes voxel-core sources
#      any more, but the capture and playtest guards compare timestamps rather
#      than content, and weather.h is newer than the last lib build. A forced
#      relink is the honest fix -- see step 2 for why weakening the guard is not.
#
#   2. The UE module. It must come AFTER the lib: UBT does not track voxel-core's
#      sources, so Build.bat prints "Result: Succeeded" while linking a
#      voxelcore.lib from hours ago, and the failure is not a link error, it is
#      WRONG TERRAIN. And it must come after nothing else, because a running
#      editor holds UnrealEditor-VoxelEarth.dll and the link simply fails.
#
#   3. The ripple assets (create_ripple_field_materials.py). BEFORE the water
#      material, because the water material's ripple sampling loads
#      RT_VoxelRippleField and the material by name and RAISES if they are
#      absent -- unlike the wave half, which degrades to a fallback. Running
#      these in the wrong order fails loudly, which is the good case, but it
#      wastes an editor launch.
#
#   4. The water material (create_water_voxel_material.py), which consumes both
#      the wave module and the ripple field.
#
# WHAT THIS DELIBERATELY DOES NOT DO: rebuild the sky. create_sky_material.py
# DELETES and recreates MPC_VoxelSky, after which every one of its six
# dependents must be rebuilt or it draws as UE's DEFAULT MATERIAL while every log
# reports success. The collection already carries WindVectorMS and
# WindFieldValid (committed in 3c0b17c), so there is nothing to add and no
# reason to take that risk. If the sky ever does need rebuilding, use
# tools/voxel-sky-chain-regen.ps1, which knows all six.

param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [string]$BuildBat = 'D:\UE_5.8\Engine\Build\BatchFiles\Build.bat',
    [int]$TimeoutSec = 2400,
    # Skip straight to launching, for a second run where nothing has changed.
    [switch]$LaunchOnly,
    # Where to drop the player. Defaults to the deep end of the pond every water
    # judgement this session was made against: datum 1650.235 m over a bed at
    # 1644.237 m, so 6 m of water, shallowing to ~1.3 m about 20 m south.
    [string]$SpawnAt = '-65102,-51084',
    [double]$AltM = 25,
    [double]$Pitch = -25,
    [double]$Yaw = 200
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Tools   = Join-Path $Root 'ue-project\Tools'
$LogDir  = Join-Path $Root 'Saved'

$live = @(Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
if ($live.Count -gt 0) {
    throw "REFUSING TO START: an editor is already running -- $(($live | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', ')."
}

function Step([string]$Title, [scriptblock]$Body) {
    $t0 = Get-Date
    Write-Host ''
    Write-Host "##### $Title" -ForegroundColor Magenta
    & $Body
    Write-Host ("##### done in {0:0.0} min" -f ((Get-Date) - $t0).TotalMinutes) -ForegroundColor Magenta
}

function Invoke-Generator([string]$Script, [string]$LogName) {
    $log = Join-Path $LogDir $LogName
    Write-Host "  $Script -> $log" -ForegroundColor Gray
    $p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList @(
        "`"$Project`"", '-run=pythonscript', "-script=`"$Tools\$Script`"",
        '-unattended', '-nop4', '-nosplash', "-abslog=`"$log`""
    )
    $p.WaitForExit($TimeoutSec * 1000) | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "$Script TIMED OUT" }

    # The exit code is not the test in either direction on this project: UE's
    # end-of-run summary counts three pre-existing project errors, so a perfect
    # run exits 1, and an exception inside -run=pythonscript can still leave 0.
    $ok = Select-String -Path $log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
    $py = @(Select-String -Path $log -SimpleMatch 'LogPython: Error' -ErrorAction SilentlyContinue)
    if (-not $ok -or $py.Count -gt 0) {
        if ($py.Count -gt 0) { $py | Select-Object -First 15 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red } }
        throw "$Script FAILED (marker=$([bool]$ok), pythonErrors=$($py.Count)) -- see $log"
    }
    $bad = @(Select-String -Path $log -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
    if ($bad.Count -gt 0) {
        $bad | Select-Object -First 6 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
        throw "$Script produced $($bad.Count) material compile failure(s) -- a failed material is replaced by the engine DEFAULT, which renders, and renders fast."
    }
    # Echo any ARM line so the log says which variant is on disk.
    Select-String -Path $log -Pattern 'ARM:' -ErrorAction SilentlyContinue |
        Select-Object -Last 4 | ForEach-Object { Write-Host "    $($_.Line -replace '^.*?(M_\w+)','$1')" -ForegroundColor Green }
}

if (-not $LaunchOnly) {
    Step 'voxelcore.lib (forced relink)' {
        # Touching a real source rather than the timestamp, so the lib is
        # genuinely rebuilt from current sources rather than merely looking new.
        (Get-Item (Join-Path $Root 'voxel-core\src\tilestore.cpp')).LastWriteTime = Get-Date
        & cmake --build (Join-Path $Root 'build\voxel-core-msvc') --config Release --target voxelcore 2>&1 |
            Select-Object -Last 3
        if ($LASTEXITCODE -ne 0) { throw "voxel-core build FAILED (exit $LASTEXITCODE)" }
    }

    Step 'UE module' {
        $log = Join-Path $LogDir 'build-playtest.log'
        & $BuildBat VoxelEarthEditor Win64 Development -Project="$Project" -WaitMutex *>&1 |
            Tee-Object -FilePath $log | Select-Object -Last 8
        if ($LASTEXITCODE -ne 0) { throw "BUILD FAILED (exit $LASTEXITCODE) -- see $log" }
        if (-not (Select-String -Path $log -SimpleMatch 'Result: Succeeded' -ErrorAction SilentlyContinue)) {
            throw "no 'Result: Succeeded' in $log despite exit 0 -- treat the build as UNKNOWN."
        }
    }

    Step 'ripple assets' { Invoke-Generator 'create_ripple_field_materials.py' 'regen-ripple-field.log' }
    Step 'water material' { Invoke-Generator 'create_water_voxel_material.py' 'regen-water-playtest.log' }
}

Write-Host ''
Write-Host '=============================================================' -ForegroundColor Cyan
Write-Host ' READY. Launching. Console is `~`.' -ForegroundColor Cyan
Write-Host '=============================================================' -ForegroundColor Cyan
Write-Host ''
Write-Host ' WIND -- these take effect IMMEDIATELY, no regeneration:' -ForegroundColor Yellow
Write-Host '   voxel.Weather.PinMps 2      calm, Hs ~2.4 cm'
Write-Host '   voxel.Weather.PinMps 5      the lake you signed off on (Hs ~6 cm)'
Write-Host '   voxel.Weather.PinMps 12     Hs ~14 cm, shore breaking visible'
Write-Host '   voxel.Weather.PinMps 20     Hs ~24 cm'
Write-Host '   voxel.Weather.PinFromDeg 90 swing the direction; crests should turn'
Write-Host '   voxel.Weather.GustScale 0   steady wind, no twitch'
Write-Host '   voxel.Weather.Enabled 0     wind off -> the pre-wind lake (the A arm)'
Write-Host ''
Write-Host ' RIPPLES:' -ForegroundColor Yellow
Write-Host '   voxel.Water.Ripple.Drop -6510200 -5108400   a ripple at a fixed spot'
Write-Host '   ...or just jump in. Entry is auto-detected.'
Write-Host ''
Write-Host ' IF SOMETHING LOOKS WRONG, check these before judging the look:' -ForegroundColor Yellow
Write-Host '   - grep the log for "Failed to compile Material" (default material renders FAST and grey-brown)'
Write-Host '   - LogVoxelWeather should say "bound to ... both wind parameters present"'
Write-Host '   - LogVoxelWater should NOT say "RippleField: assets missing"'
Write-Host ''

& (Join-Path $PSScriptRoot 'water-playtest.ps1') -SpawnAt $SpawnAt -AltM $AltM -Pitch $Pitch -Yaw $Yaw
