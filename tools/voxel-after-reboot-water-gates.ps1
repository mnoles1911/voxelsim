# The water/tide wave's GPU recovery sequence -- run after the 2026-09-04 GPU
# wedge is cleared by a REBOOT (see docs/water-ocean-tides-plan-2026-09-04.md
# "GPU OUTAGE" and the voxelsim-gpu-wedge-2026-09-04 memory).
#
# WHAT THIS AUTOMATES AND WHAT IT DELIBERATELY DOES NOT. Steps 1-3 are
# deterministic and run here: prove the GPU is back with the exact probe that
# was failing, regenerate the material chain (the wedge struck mid-chain:
# sky+dome landed, ripple/water/ocean/underwater/voxel/clipmap did not), and
# prove build coherence. The LEGS are printed, not run -- each one's output
# needs reading against its gate (engagement counters, settle lines) before
# the next is worth spending, and that judgment loop lives in the session,
# not in a script (a leg run blind is a leg wasted -- house rule).
param(
    [string]$Editor = 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe',
    [string]$Project = 'D:\voxelsim\ue-project\VoxelEarth.uproject'
)
$ErrorActionPreference = 'Stop'

# --- 1. GPU probe: the EXACT failing path, not a proxy -----------------------
# The wedge failed CreateCommandQueue with DXGI_ERROR_DEVICE_REMOVED on every
# -AllowCommandletRendering launch. A ripple-materials run IS the probe; if it
# passes, it has also completed the first pending regen step.
# NB: the probe recreates the ripple RTs -- before that script's 2026-09-05 materials-first-delete fix, step 2's rerun therefore needed the RT .uasset files hand-deleted first.
$log = 'D:\voxelsim\Saved\sky-chain\probe-after-reboot.log'
Write-Host '=== 1. GPU probe (ripple materials commandlet, the path that was failing)'
$p = Start-Process -FilePath $Editor -PassThru -WindowStyle Hidden -ArgumentList @(
    "`"$Project`"", '-run=pythonscript',
    '-script="D:\voxelsim\ue-project\Tools\create_ripple_field_materials.py"',
    '-unattended', '-nop4', '-nosplash', "-abslog=`"$log`"", '-AllowCommandletRendering')
$p.WaitForExit(480000) | Out-Null
$removed = Select-String -Path $log -Pattern 'DXGI_ERROR|DEVICE_REMOVED' -ErrorAction SilentlyContinue
$okMark  = Select-String -Path $log -SimpleMatch 'Python script executed successfully' -ErrorAction SilentlyContinue
if ($removed -or -not $okMark) {
    throw "GPU STILL WEDGED (or probe failed another way) -- see $log. Do not run legs; the reboot did not take, or something new broke."
}
Write-Host '    GPU is back.' -ForegroundColor Green

# --- 2. Full material chain (idempotent; reruns sky+dome too) ----------------
Write-Host '=== 2. Material chain regen (sky -> dome -> ripple -> water -> ocean -> ...)'
& D:\voxelsim\tools\voxel-sky-chain-regen.ps1

# --- 3. Build coherence (covers the review fixes + Phase C if it landed) -----
Write-Host '=== 3. Build verify'
& D:\voxelsim\tools\voxel-build.ps1 -Verify -AllowDirty

# --- 4. The legs, IN ORDER, each gated before the next ----------------------
@'
=== 4. LEG CHECKLIST (run each, READ its gate, then the next) ===
Site: PRIMARY tide-gate shore, tile (-15,-7), world m (-224715,-100215)
  -- coarse-derived: FIRST verify the column with -VoxelOceanSurvey through the
     engine (the doctrine that caught 3 of 9 bad vista sites).

A-gate (tide engagement + waterline moves):
  1. Survey:   flight/capture harness at -VoxelSpawnAt=-224715,-100215 with
               -VoxelOceanSurvey=80 -> confirm ground crosses 0 near the column.
  2. High pin: add cvars 'voxel.Water.Tide 1' + 'voxel.Water.Tide.ForceOffsetMm 1500'
               -> grep 'Tide ENABLED' and 'Tide: steps=N' (N>0; N==0 FAILS)
               -> grep 'Ocean: sea surface moved' (ocean followed; absent FAILS)
               -> capture the shoreline.
  3. Low pin:  'voxel.Water.Tide.ForceOffsetMm -1500' -> same greps -> capture.
  4. Owner judges the pair (waterline moved, no plane/near-field gap).
  5. Off arm:  'voxel.Water.Tide 0' -> water log lines byte-identical to baseline.

B-gates (waves visible):
  6. Lakeshore capture at the kept vista lake: crests moving on the sheet
     (grep 'wave tessellation ENABLED' + 'N of M basin(s) tessellated, V vertex(es)'),
     strafe pair for cracks, vista unchanged past 96 m.
  7. Ocean horizon + beach captures at the tide site (ring seams, break line,
     calm control with wind 0).
  NOTE: the lake-sheet shore clip (black-band fix) is INERT until the sheet MID
  sets WaterShoreClipEnabled=1 -- see the review-pass section of the plan doc.

C-gates (connectivity/pools): see Phase C's SCOREBOARD rows ('pending GPU') --
  'OceanConnect:' engagement line, pool-held probe at both pins, inland
  byte-identical check, conservation sweep.

D-gates (boat): voxel.Boat.Spawn -> 'VoxelAssetBody:' line (instances != 0,
  LOADED not PLACEHOLDER) -> voxel.Boat.Stat (probes>0, wet>0) ->
  voxel.Water.Ripple.Stat (injected rising) -> captures (rest/underway/beached).
E-gate (glider): voxel.Glider.Spawn 20 40 -> capture; voxel.Glider.Stat.
Then: perf legs (tools/voxel-perf-gate.ps1) once images are owner-approved.
'@ | Write-Host
