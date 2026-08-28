# Chain 2 ledger: one gate failed decisively, four axes closed, one flip earned

2026-08-28 evening. Everything below ran in one serialized box chain; every leg
carries DOUBLE GRANT = 0 and the loaded= reconciliation (delta=0 on all nine
flight legs).

## THE BOUND ARM FAILED ITS TIMING GATE, by ~60x the noise floor

    marchMs   ctl 4.519 / 4.515 (spread 0.004)     Bound 1: 4.897  (+0.38 SLOWER)
    boundMs   producer 5.607 (own bracket; ProfileGPU corroborates: 0.168 list
              + ~5.39 across 7 raster slices)
    net       (4.519 - 4.897) - 5.607 = -5.99 ms   GATE was >= +0.35. FAIL.

Engagement was REAL -- 30.9% of segments skipped whole, 34.5% of walks shortened
-- and the time still went the wrong way in BOTH terms. **This is the
literature's warning made flesh: 91% removable ITERATIONS became negative TIME**
("iterations and time have moved in opposite directions", pre-registered in the
census doc). Two independent causes:

1. **Producer fill: 5.6 ms/frame.** The list pass emits every live pool slot
   (~80k cubes) with no frustum cull, into 7 min-blend slices with no early-Z.
   R1's estimate was 0.15-0.5 ms; measured 5.6. Mitigations (frustum cull in the
   compaction pass, half-res with conservative decode) are authored and pending
   one build.
2. **The consult itself costs more than the skips save: marchMs +0.38.** The
   per-segment slice loads and clamp arithmetic exceed the value of the skipped
   iterations at the horizon pose. **This term survives a free producer.** Unless
   the mitigated consumer (fewer/cheaper loads) flips ITS sign, the arm is dead
   at the gate pose regardless of producer cost.

One mitigated timing pair decides retire-vs-continue. The image pair exists
(VoxelVerify00354 ctl / 00356 armed, same shutter pose) and goes to the owner
only if the arm survives; a timing corpse needs no image verdict.

## CLOSED AXES (do not re-run)

- **anySolid moving: NULL, default stays 0.** Engagement proven (refine
  dispatches=11,163, cleared=82,842, casLost=0, wrongClear=0); pooled deltas
  p50 +0.03 / p95 -0.10 / p99 +0.18 ms, all inside the noise, and the drift
  between the two CONTROL legs equals any cross-arm delta -- the alternated
  design earning its keep.
- **Pass-free (-VoxelGpuJobLean=1): NOT null -- a REGRESSION. Never ship.**
  Engaged (passFree 3.8-5.7k/window) but overCap=0 always, delivered FELL
  (1.014M -> 880-991k), promote tickHz collapsed 125 -> 78, and the frame tail
  exploded: p95 30-62 ms, p99 139-197 ms against ~12 control. The q-armc2 null
  was the kind reading.
- **MeshBatchCap 256: refutation now unasterisked.** Clean post-doubleGrant leg:
  delivered -13.4%, p99 129.5 ms. Axis shut.
- **+90 census:** removableUp 99.78%, but capRays = 0.24% -- the vertical-cap
  population exists and is trivial even straight up.

## EARNED: VelocityLead 4.0 s with the clamp at 240 m

The first "4 s" A/B never ran 4 s -- the 60 m VelocityLeadMaxUU clamp bound at
93.6 m of requested lead (the pre-registered failure reading, caught). With the
clamp lifted: peak parkedNow 2,326 vs 1,106 control, adoptions +6.8%, hit 98%,
evictedUnused=0, dropOvertaken inside the control band, frame p99 slightly
BETTER. No revert counter fired. Defaults staged (VoxelDebug.cpp), commit after
the third build's role-reversed confirmation.

## A COORDINATION FAILURE OF THE COORDINATOR'S, recorded

BT-on-2 is VOID: it booted into a shader tree being edited live -- the
mitigation edits I dispatched at ~18:31 while the box owner still had bound
timing legs QUEUED. My rule "edits do not affect already-started editors" is
true only for already-STARTED; a queued leg that starts after the edit begins
compiles the half-state (four bind errors naming parameters not in the DLL).
The box owner caught it and correctly refused to continue the branch. The rule
that prevents it: NO shader edits may be dispatched while any leg that could
compile them remains QUEUED -- the fence is chain-end, not leg-start.
