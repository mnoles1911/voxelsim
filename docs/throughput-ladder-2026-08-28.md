# The 50k/s ladder, re-measured: the apply cap was standing in front of the wall

2026-08-28. Three independent reads taken in one afternoon: the APT rung-1 sweep
(three legs, current binary), a code+log audit of rung 2, and a flight-regime
churn audit. Each corrected at least one standing belief.

## Rung 1 (MaxAppliesPerFrame 192): REFUTED -- released cleanly, throughput FELL

    arm        engagement              fill exits (first 3 windows)          settle mean
    ctl        maxApplies=192          countCap 132 / 77 / 46                8,243/s
    512        maxApplies=512 (cvar    countCap 5/4/12; queueEmpty 122,      8,107/s
               192 -- override took)   wallClock 21
    1024       maxApplies=1024         countCap ~0; queueEmpty + wallClock   7,567/s

**The cap released exactly as intended and throughput went DOWN.** With the cap
lifted the drain empties the queue every tick (queueEmpty 122 vs the control's 2)
and waits on the producer. **The 192 cap was never the wall -- it was standing in
front of it.** The producer makes ~8.2k/s, and at 1024 the large apply bursts slow
the tick itself (-8%), the same count-cap-vs-tick-rate trade the MeshBatchCap 256
sweep showed (-31%, one FRDGBuilder swallowing 512 chunks, tickHz -20%).

The earlier "this was lifted once and it worked" precedent (q-a1024 legs,
2026-08-23) does not transfer: those ran on the 164k-job cascade before the
worklist-claim flip. On today's binary the producer is the bound.

**Do not re-run.** The pattern to carry: EVERY per-tick count cap measured so far
trades against tick rate past its knee. The wall is tick rate x producer rate, not
any single cap.

## Rung 2 (MeshBatchCap / pass-free): three premises corrected by audit

- **"passFree never fired" is FALSE.** It fired on Saved/q-armc2.log (2026-08-23,
  passFree=5,918/window) -- it is gated on `-VoxelGpuJobLean=1`, default OFF, and
  the leg I read did not pass the flag. The arm-C result is recorded NOWHERE in
  docs/ (handoff-gpu-jobcost-round2.md still calls it unrun). Reading of that leg:
  overCap 0-73 against passFree 3,000-5,900 = ~0.5% of pass-free promotions were
  ones the shipped gate would have blocked -- "converted almost nothing".
- **Raising MeshBatchCap is measured-refuted**: R-batch{16,64,256}.log sweep, 256
  is -31% vs 64. Caveat: the 256 leg predates the doubleGrant fix (logs 8
  collisions) -- ONE confirmation re-run owed before airtight; its shape
  (cap-dominant exits, 512/batch, tickHz -20%) is structural regardless.
- **Two stale walls lifted**: the Write-triple 442-record/26.5k/s ceiling moved to
  Y (real: 1,023 records ~ 61k/s); the cell budget is 384 not 256 (~23k/s @60 Hz).
- MeshBatchCap semantics: it bounds demand ALLOWANCE UNITS, not chunks -- fused
  Z-siblings and pass-free jobs do not consume it, so "64 x 60 Hz = 3,840/s" in
  the older docs is wrong arithmetic on a fused leg (measured 141 chunks/tick at
  cap 64).

## Flight regime: THERE IS NO CHURN PROBLEM

The premise "flight is 93% churn = waste" is dead, on counters:
- **`resurrected=0` in all 119 windows** -- not one chunk evicted and re-admitted.
  That is the counter a too-narrow hysteresis would move, pinned at zero.
- Hysteresis exists: admit 64 m, evict 80 m (5 chunk edges). added ~ evicted is
  steady state by construction; chunk lifetime closes with ring geometry to 93%.
- Admission is not behind -- it IS geometry: per-ring incremental scans fire within
  one count of the ring-crossing ceiling; every throttle counter is zero; dispatch
  has 4x headroom (exitEmpty is the HEALTHY reading).
- **Speculation is the live lever**: 99% adoption hit, `evictedUnused=0` -- 
  under-supplied, zero measured waste. Experiment queued: VelocityLeadSec 2->4 on
  a terrain-bearing leg; failure counters pre-registered (evictedUnused lifts off
  0 / hit < 90% / dropOvertaken>0 = revert).
- CAUTION on that audit's "no geometry / 745 ms wasted worker time" claims: they
  read QUAD-path counters (residentQuads, zeroQuad) which the marcher retires by
  design -- brick side reads 79,972 chunks resident / 506,603 bricks packed.
  Those two claims are QUARANTINED until re-derived from brick counters. The
  findings above rest on non-quad counters and stand.
- Instrument smell to check: `loaded=` and cumulative `adopted=` byte-identical
  (55,757) -- possibly aliased counters.

## The next cycle, in order

1. **PF arms** (one flag): PF-a control / PF-c `-VoxelGpuJobLean=1` /
   PF-d + `-VoxelGpuMeshInFlight=2048`. Engagement: `jobLean=ON`, `passFree>0`,
   `overCap>0`; gate: promoteExit quota= falls AND delivered rises. Likely
   outcome per q-armc2: overCap~0, converted nothing -- budget for the null.
2. **VelocityLeadSec 2->4** (terrain-bearing leg, counters above).
3. **anySolid moving A/B** (runtime cvar, no build): control vs
   `voxel.March.IndexAnySolid 1`; decides arming the measured -0.13 ms.
4. One MeshBatchCap=256 confirmation leg on the post-doubleGrant build (closes
   the asterisk), then that axis is shut.

50k/s remains 6x away from the measured producer rate. Nothing on this ladder
plausibly closes that gap by itself; if the PF arms come back null, the honest
report is that 50k/s needs producer-side redesign (tick-rate decoupling or wider
GPU worldgen batches), and that is a scoping decision for the owner, not a knob.
