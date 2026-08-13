# Build M_Underwater, the lake underwater post-process material.
#
# WHY A SCRIPT FOR ONE COMMANDLET INVOCATION: because the two ways this
# regeneration reports success are both unreliable, and this project has been
# burned by each of them.
#
#   * THE EXIT CODE IS NOT THE TEST, IN EITHER DIRECTION. UE's end-of-run
#     summary counts three PRE-EXISTING project errors unrelated to any script,
#     so a perfect run still exits 1; and an exception inside -run=pythonscript
#     can still leave a 0. Measured on this project 2026-08-11.
#
#   * "Failed to compile Material" IS NOT ENOUGH HERE, and this one is specific
#     to a post-process material. An EMPTY post-process material compiles
#     perfectly cleanly -- so a run that created the asset, set the domain and
#     then died before wiring anything would emit no compile failure at all, and
#     the game would apply a transparent no-op blendable. The symptom of that is
#     "the underwater view looks exactly like it did before", which is also the
#     symptom of the material not being hooked up, and of the C++ not having
#     been rebuilt. Three causes, one appearance. So the generator prints a
#     final line naming what it wired, and this script requires it.
#
# ORDERING: M_Underwater binds MPC_VoxelSky (SunDirection and MoonLightFraction)
# for its day/night ambient, so the collection must already exist in its current
# form. It does -- nothing here rebuilds the sky. Do NOT add create_sky_material.py
# to this script: that script DELETES and recreates the collection, and every
# material holding a binding to the old one silently compiles to UE's DEFAULT
# material while the log reports success. That is the 2026-08-10 failure, and it
# cost a night.

param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [int]$TimeoutSec = 1800,
    [string]$LogName = 'regen-underwater.log'
)

$ErrorActionPreference = 'Stop'
$Root    = (Resolve-Path "$PSScriptRoot\..").Path
$Project = (Resolve-Path "$Root\ue-project\VoxelEarth.uproject").Path
$Script  = Join-Path $Root 'ue-project\Tools\create_underwater_material.py'
$Log     = Join-Path $Root "Saved\$LogName"

if (-not (Test-Path $Script)) { throw "generator not found: $Script" }

# ONE EDITOR PER BOX -- two editors on this machine have destroyed each other's
# work before.
$running = @(Get-Process UnrealEditor-Cmd, UnrealEditor -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $detail = ($running | ForEach-Object { "PID $($_.Id)" }) -join ', '
    throw "REFUSING TO START: editor already running -- $detail."
}

Write-Host "=== create_underwater_material.py -> $Log" -ForegroundColor Cyan
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList @(
    "`"$Project`"", '-run=pythonscript', "-script=`"$Script`"",
    '-unattended', '-nop4', '-nosplash', "-abslog=`"$Log`""
)
$p.WaitForExit($TimeoutSec * 1000) | Out-Null
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; throw "TIMED OUT after ${TimeoutSec}s -- see $Log" }

$ok = Select-String -Path $Log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
$py = @(Select-String -Path $Log -SimpleMatch 'LogPython: Error' -ErrorAction SilentlyContinue)
if (-not $ok -or $py.Count -gt 0) {
    if ($py.Count -gt 0) { $py | Select-Object -First 15 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red } }
    throw "FAILED (successMarker=$([bool]$ok), pythonErrors=$($py.Count), exit=$($p.ExitCode)) -- see $Log"
}

$failed = @(Select-String -Path $Log -SimpleMatch 'Failed to compile Material' -ErrorAction SilentlyContinue)
if ($failed.Count -gt 0) {
    $failed | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.Line)" -ForegroundColor Red }
    throw "$($failed.Count) material compile failures -- see $Log"
}

# THE GENERATOR'S OWN CLOSING LINE. See the header for why a clean compile is
# not sufficient evidence for a post-process material.
$built = Select-String -Path $Log -Pattern 'M_Underwater' -ErrorAction SilentlyContinue |
         Select-Object -Last 6
if (-not $built) { throw "no 'M_Underwater' line in $Log -- the generator did not report what it built." }
$built | ForEach-Object { Write-Host "  $($_.Line -replace '^.*?M_Underwater','M_Underwater')" -ForegroundColor Green }

# --- THE TWO LOGS MUST AGREE ON THE PHYSICS -------------------------------
#
# M_WaterVoxel and M_Underwater are two different renderers of the same water --
# a Single Layer Water surface seen from above, and a post-process seen from
# within. They read the same constants from Tools/water_optics.py precisely so
# they cannot drift, and each prints the derived numbers into its own log. This
# compares the two blocks.
#
# It is a WARNING and not a failure, deliberately: the water log here may be
# from an older run, and a stale comparison should not block a good build. What
# it protects against is the specific failure this whole arrangement exists to
# prevent -- looking into a pond and swimming in it showing two different
# liquids, which shipped for one commit and was caught by eye rather than by
# anything automated.
$waterLogs = Get-ChildItem -Path (Join-Path $Root 'Saved') -Recurse -Filter '*create_water_voxel_material*.log' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($waterLogs) {
    function Get-OpticsBlock([string]$path) {
        (Select-String -Path $path -Pattern 'extinction\s+R|deep-water albedo\s+R' -ErrorAction SilentlyContinue |
         ForEach-Object { ($_.Line -replace '^.*?(extinction|deep-water albedo)', '$1').Trim() }) -join ' | '
    }
    $a = Get-OpticsBlock $Log
    $b = Get-OpticsBlock $waterLogs.FullName
    if (-not $a -or -not $b) {
        Write-Warning "could not read the optics block from both logs -- surface/underwater agreement UNVERIFIED."
    }
    elseif ($a -ne $b) {
        Write-Warning ("SURFACE AND UNDERWATER DISAGREE ON THE WATER:`n" +
                       "  M_Underwater : $a`n" +
                       "  M_WaterVoxel : $b  ($($waterLogs.Name), $($waterLogs.LastWriteTime))`n" +
                       "If the water log is stale, rebuild it and re-check. If it is not, water_optics.py " +
                       "is not reaching one of the two generators.")
    }
    else {
        Write-Host "  optics agree with $($waterLogs.Name): $a" -ForegroundColor Green
    }
}
else { Write-Warning "no create_water_voxel_material log found -- surface/underwater agreement UNVERIFIED." }

Write-Host "M_Underwater BUILT" -ForegroundColor Green
Write-Host "NEXT: rebuild the VoxelEarth module (the C++ that binds this material), then" -ForegroundColor Yellow
Write-Host "      tools\voxel-underwater-captures.ps1" -ForegroundColor Yellow
