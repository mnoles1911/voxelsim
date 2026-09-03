# CYCLE 2: the four experiments queued behind the 2026-08-28 chain, one script.
#
# Order and design constraints, each from a measured burn:
#   * anySolid is a TIMING comparison at -0.13 ms scale -- it gets alternated
#     PAIRS (ctl,on,ctl,on), never blocks, because the box drifts thermally
#     across a 40-minute sweep and a block design confounds arm with clock.
#   * VelocityLead and pass-free are COUNTER comparisons -- one arm each; their
#     control is the pooled AS-ctl pair (same binary, flag-free, same protocol).
#   * The batch-256 confirmation exists ONLY to clear the doubleGrant asterisk
#     on the 2026-08-24 refutation (that leg logged 8 collisions pre-fix). Its
#     expected result is "still worse than 64"; a leg with ANY 'DOUBLE GRANT'
#     line is invalid regardless of its numbers.
#
# READ EACH LEG IN THIS ORDER (the same order that caught three dead sweeps):
#   1. Engagement -- the counter that proves the arm is ON (listed per leg
#      below). No engagement = void, do not read timings.
#   2. Validity  -- grep -c 'DOUBLE GRANT' == 0; holes not regressed.
#   3. The experiment's own gate.
#   4. brickFirstLoads= on the streaming line -- the 2026-08-28 counter repair
#      rides every leg for free; loaded ~= brickFirstLoads + adopted must
#      reconcile, and brickFirstLoads=0 on any MARCHER leg means the repair
#      did not engage.
#
# Engagement lines per experiment:
#   AS-on  : census word cellIndexEmpty > 0 (the anySolid refine pass ran) and
#            the marcher's own anySolid skip counter moving. -0.13 ms is near
#            single-pair noise (1.9% azimuthal spread) -- pool BOTH pairs, and
#            if the pooled delta is inside noise the verdict is "leave default
#            0, item closed", which is a real result.
#   VL-4s  : speculative line leadSec=4.00, parkedNow rising above the ctl's
#            ~850, spec dispatched share rising above ~17%. FAILURE readings
#            (revert): evictedUnused lifts off 0, hit% < 90, dropOvertaken > 0.
#   PF-c/d : [gpu-jobcost] jobLean=ON, passFree > 0, overCap > 0. Gate:
#            promoteExit batchCap= falls AND delivered rises (M10 2026-09-02
#            split the old `quota=`, which also carried the pass-free and spec
#            allowances; batchCap= is the MeshBatchCap half and the only one
#            this gate ever meant). LIKELY NULL per
#            q-armc2 (overCap 0-73 vs passFree 3-6k): a null closes the item.
#   B256   : [gpu-jobcost] batch cap line reads 256; expected worse than 64.
param(
    [int]$RunSec = 120
)
$ErrorActionPreference = 'Stop'
$leg = Join-Path $PSScriptRoot 'voxel-run-flight-leg.ps1'
$cv  = 'voxel.Stream.CoverageVerify 1,voxel.Stream.FrameAttribution 2'
$cvAS = $cv + ',voxel.March.IndexAnySolid 1'
$cvVL = $cv + ',voxel.Stream.VelocityLeadSec 4.0'

$runs = @(
    @{ Name='C2-ASctl-a'; Cvars=$cv;   Extra=@() },
    @{ Name='C2-ASon-a';  Cvars=$cvAS; Extra=@() },
    @{ Name='C2-ASctl-b'; Cvars=$cv;   Extra=@() },
    @{ Name='C2-ASon-b';  Cvars=$cvAS; Extra=@() },
    @{ Name='C2-VL4';     Cvars=$cvVL; Extra=@() },
    @{ Name='C2-PFc';     Cvars=$cv;   Extra=@('-VoxelGpuJobLean=1') },
    @{ Name='C2-PFd';     Cvars=$cv;   Extra=@('-VoxelGpuJobLean=1','-VoxelGpuMeshInFlight=2048') },
    @{ Name='C2-B256';    Cvars=$cv;   Extra=@('-VoxelGpuMeshBatchCap=256') }
)
foreach ($r in $runs) {
    Write-Host "=== $($r.Name) ===" -ForegroundColor Cyan
    & $leg -LogName $r.Name -Cvars $r.Cvars -ExtraArgs $r.Extra `
           -SpawnAt '-61440,-61440' -Width 2560 -Height 1440 `
           -Flight line -RunSec $RunSec -PreflightSec 90 -LingerSec 30 -LogIntervalSec 2
    if ($LASTEXITCODE -ne 0) { Write-Host "$($r.Name) FAILED exit $LASTEXITCODE -- continuing" -ForegroundColor Red }
}
Write-Host "cycle 2 sweep done" -ForegroundColor Green
