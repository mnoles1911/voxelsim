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
    # Phase 3 integration arms (docs/water-phase3-integration-2026-08-09.md):
    # -Faucets drives the headwater/sill faucet lifecycle + both sinks;
    # -Verify runs the GPU-vs-CPU occupancy gate for the whole window;
    # -DefaultQ overrides voxel.Fluid.Faucets.DefaultQ (m^3/yr) for a visible
    # lake rise in a short window. Pass -Particles 0 to run faucets-only.
    [switch]$Faucets,
    [switch]$Verify,
    [switch]$Render,
    [switch]$GpuTiming,
    [double]$DefaultQ = 0,
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
$Cmds = "voxel.Fluid.Enable 1"
if ($Particles -gt 0) { $Cmds += ", voxel.Fluid.Spawn $Particles" }
if ($Emit -gt 0)      { $Cmds += ", voxel.Fluid.Emit $Emit" }
if ($Faucets)         { $Cmds += ", voxel.Fluid.Faucets 1" }
if ($Verify)          { $Cmds += ", voxel.Fluid.Occupancy.Verify 1" }
if ($Render)          { $Cmds += ", voxel.Fluid.Render 1" }
if ($GpuTiming)       { $Cmds += ", voxel.Fluid.GpuTiming 1" }
if ($DefaultQ -gt 0)  { $Cmds += ", voxel.Fluid.Faucets.DefaultQ $DefaultQ" }

# Start-Process with ONE manually-quoted argument string. Two traps live here:
# a trailing '&' is bash, not PowerShell 5.1; and the array form of
# -ArgumentList mangles the -ExecCmds quoting (5.1 re-quotes whole elements,
# UE then reads `-ExecCmds=voxel.Fluid.Enable` with no value and the run
# reports NOT RUN). Quote it ourselves, pass one string.
$EditorArgs = "`"D:\voxelsim\ue-project\VoxelEarth.uproject`" -game -windowed " +
    "-ResX=2560 -ResY=1440 -VoxelSpawnAt=$SpawnAt -VoxelSpawnAltM=60 " +
    "-VoxelTimeOfDay=12:00 -VoxelTimeScale=0 " +
    "-ExecCmds=`"$Cmds`" -abslog=`"$Log`" -VoxelNoGpuMesh"
Start-Process -FilePath $Editor -ArgumentList $EditorArgs
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

# Sim GPU ms percentiles ('simGpuMs=<ms>' -- the shipped perf-line field; the
# spike-era 'sim=<ms>ms' form is kept as a fallback so old logs still parse).
$Ms = $Perf | ForEach-Object {
    if ($_ -match 'simGpuMs=([0-9.]+)') { [double]$Matches[1] }
    elseif ($_ -match 'sim=([0-9.]+)ms') { [double]$Matches[1] }
} | Where-Object { $_ -ge 0 } | Sort-Object
if ($Ms.Count -gt 0) {
    $p50 = $Ms[[int]($Ms.Count * 0.5)]
    $p95 = $Ms[[math]::Min($Ms.Count - 1, [int]($Ms.Count * 0.95))]
    Write-Output ("sim GPU ms: p50 {0:N2}  p95 {1:N2}  n {2}" -f $p50, $p95, $Ms.Count)
}
$Viol = $Perf | Where-Object { $_ -match 'CONSERVATION' -and $_ -notmatch 'ok' }
if ($Viol) { Write-Output "CONSERVATION VIOLATIONS:"; $Viol | Write-Output } else { Write-Output "conservation: no violations logged" }

# Phase 3 arms: scalar-ledger violations, occupancy verify verdicts, and the
# last lifecycle ledger line -- each reported as ran/did-not-run explicitly.
$Scalar = Select-String -Path $Log -Pattern 'SCALAR LEDGER VIOLATION' | ForEach-Object Line
if ($Scalar) { Write-Output "SCALAR LEDGER VIOLATIONS:"; $Scalar | Write-Output }
elseif ($Faucets) { Write-Output "scalar ledger: no violations logged" }
if ($Verify) {
    $Fails = $Perf | Where-Object { $_ -match 'verify=FAIL' }
    $Pass = ($Perf | Where-Object { $_ -match 'verify=pass' }).Count
    if ($Fails) { Write-Output "OCCUPANCY VERIFY FAILURES:"; $Fails | Select-Object -First 5 | Write-Output }
    elseif ($Pass -gt 0) { Write-Output "occupancy verify: $Pass pass line(s), zero failures" }
    else { Write-Output "occupancy verify: NEVER REACHED pass/fail (fill incomplete or stale-skipped all window) -- not evidence it passes" }
}
if ($Faucets) {
    $Ledger = Select-String -Path $Log -Pattern 'Fluid ledger:' | ForEach-Object Line
    if ($Ledger) { Write-Output "last ledger line:"; $Ledger | Select-Object -Last 1 | Write-Output }
    else { Write-Output "faucets: NO ledger line -- the lifecycle emitted nothing (no heads in range, or DefaultQ 0)" }
}
