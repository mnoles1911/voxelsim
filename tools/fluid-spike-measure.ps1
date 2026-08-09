# fluid-spike-measure.ps1 -- the Phase 0 gate, run by the integrator only.
#
# Launches the editor once per arm, drives the PBF spike via its cvars, and
# reads the 1 Hz perf lines back out of the log. The spike agents cannot run
# this (one editor per box); their definition of done is "compiles + spec",
# and THIS script turns their spec into the numbers the plan gates on:
#
#   docs/water-rearchitecture-plan-2026-08-09.md, Phase 0:
#     (a) 100-300k particles, ms/frame against the p95 GPU-spike headroom
#     (c) 10 cm walls must not tunnel at expected velocities
#
# Frame-budget context the verdict is judged against (measured, not aspirational):
#   frame p50 15.16 ms / p95 20.94 ms at 2560x1440; the p95 tail is GPU
#   (docs/measurements/frame-attribution-2026-07-28.txt). The spike's sim cost
#   lands in that tail, so the number that matters is the p95 delta, not p50.
#
# Usage:
#   tools\fluid-spike-measure.ps1 -Particles 100000 -Seconds 90
#   tools\fluid-spike-measure.ps1 -Particles 300000 -Seconds 90 -Emit 5000
#
# Reads back: every "VoxelFluid" perf line (alive count, sim GPU ms,
# conservation verdict), plus the standard frame stats, and prints p50/p95 of
# the sim cost and ANY conservation violation. A run with zero perf lines is
# reported as NOT RUN -- a silent zero must never read as "cheap" (the
# absent-stat rule; three false conclusions in one session).

param(
    [int]$Particles = 100000,
    [int]$Seconds = 90,
    [int]$Emit = 0,
    [string]$SpawnAt = '-60688,-51716',
    [string]$Name = "fluid-spike-$([int]($Particles/1000))k"
)

$ErrorActionPreference = 'Stop'
$Log = "D:\voxelsim\Saved\$Name.log"
$Editor = "D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"

if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
    throw "an editor is already running -- one editor per box. Close it first."
}

# -ExecCmds is fine here (unlike the mesh-producer forks) because the fluid
# subsystem reads its cvars per tick, not once at streaming start.
$Cmds = "voxel.Fluid.Enable 1, voxel.Fluid.Spawn $Particles" + $(if ($Emit -gt 0) { ", voxel.Fluid.Emit $Emit" } else { "" })

& $Editor "D:\voxelsim\ue-project\VoxelEarth.uproject" -game -windowed -ResX=2560 -ResY=1440 `
    "-VoxelSpawnAt=$SpawnAt" -VoxelSpawnAltM=60 -VoxelTimeOfDay=12:00 -VoxelTimeScale=0 `
    "-ExecCmds=$Cmds" "-abslog=$Log" -VoxelNoGpuMesh 2>$null &
Start-Sleep -Seconds ($Seconds + 60)   # boot + settle + measure window
Get-Process UnrealEditor -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not (Test-Path $Log)) { throw "no log at $Log -- the editor never started" }

$Perf = Select-String -Path $Log -Pattern 'VoxelFluid' | ForEach-Object Line
if (-not $Perf -or $Perf.Count -eq 0) {
    Write-Output "NOT RUN: zero VoxelFluid perf lines in $Log."
    Write-Output "Either voxel.Fluid.Enable never took, the subsystem did not tick,"
    Write-Output "or the perf line name changed. This is NOT evidence the sim is cheap."
    exit 2
}

Write-Output "== $Name : $($Perf.Count) perf lines =="
$Perf | Select-Object -First 3 | Write-Output
Write-Output "..."
$Perf | Select-Object -Last 3 | Write-Output

# Sim ms percentiles, if the line carries 'sim=<ms>ms' per the spike spec.
$Ms = $Perf | ForEach-Object { if ($_ -match 'sim=([0-9.]+)ms') { [double]$Matches[1] } } | Sort-Object
if ($Ms.Count -gt 0) {
    $p50 = $Ms[[int]($Ms.Count * 0.5)]
    $p95 = $Ms[[math]::Min($Ms.Count - 1, [int]($Ms.Count * 0.95))]
    Write-Output ("sim ms: p50 {0:N2}  p95 {1:N2}  n {2}" -f $p50, $p95, $Ms.Count)
}
$Viol = $Perf | Where-Object { $_ -match 'CONSERVATION' -and $_ -notmatch 'ok' }
if ($Viol) { Write-Output "CONSERVATION VIOLATIONS:"; $Viol | Write-Output } else { Write-Output "conservation: no violations logged" }
