# Phase 3 integration pass — collision live + the faucet/sink lifecycle v1

> **STATUS 2026-08-09: CURRENT.** The integration pass that wired the three
> parallel builds (fluid spikes `66f0619`, scalar hydrology `ebcd6c0`, basin
> v2 `88e83c1`) into one lifecycle. Companion to
> `docs/water-rearchitecture-plan-2026-08-09.md` (Phase 3 row) and
> `docs/water-architecture.md` §3. The measurement script at the bottom is the
> integrator's; captures are judged by the owner, not by the implementer.

## What is now wired

1. **Collision is live.** `VOXEL_FLUID_HAS_COLLISION=1` is baked into every
   solver compile (`FVoxelFluidShader::ModifyCompilationEnvironment`). The
   per-iteration Δp correction projects against the occupancy volume, and
   `FluidFinalizeMain` walks the FULL step (stored position → constrained
   position) through `VoxelFluidResolveCollisionEx`, lifting the previous
   position onto the contact plane so the derived velocity has exactly zero
   approach speed (contract items 2/5/7 — the ratified recipe is now the
   implementation). The anti-tunnelling guarantee lives in that finalize walk:
   a prediction that crossed a thin wall during integrate is caught from the
   known-free start-of-step position.
2. **The occupancy volume is owned and fed.** `UVoxelFluidSubsystem` owns the
   `FVoxelFluidOccupancyVolume`; the origin latches as the volume's min corner
   centred on the camera (contract item 1: `FluidOriginUU == origin voxel ×
   10`; positions live in `[0, 5120]` UU; boundary box centre is uploaded, no
   longer origin-centred). Initial fill: 512 regions of 64³ voxels, queued
   centre-out, packed on the game thread from the overlay-aware
   `IsSolidAtVoxel` under a time+count budget
   (`voxel.Fluid.Occupancy.RegionsPerTick` / `.PackMsPerTick`). Terrain edits
   and arrivals reach the volume through
   `UVoxelWorldSubsystem::SetFluidTerrainDirtyListener`, fired from the two
   documented call sites (`MarkChunkDirtyForRemesh`, level-0 keys only, before
   its untracked early-return; `ApplyMeshResult`, level 0).
3. **Ordering is enforced, not trusted.** `AddPasses` runs in the SAME
   `FRDGBuilder` before any solver pass; a `checkf` against the volume's new
   `FStats::AddPassesCount` fails any refactor that moves it. A tick without a
   usable volume is SKIPPED and counted (`skippedNoOcc` in the perf line) —
   with the define baked on there is no silent no-collision path.
4. **The verify gate exists** (`voxel.Fluid.Occupancy.Verify 1`): 16 MiB
   readback, one 64³ region byte-compared per landed snapshot (rotating
   cursor) against `vxc::fluidFillRegion` — the unit-tested CPU reference the
   GPU kernel hand-mirrors. Results: `verify=pass|FAIL|stale|off` in the perf
   line; every mismatch logs its first differing word with world coordinates.
   `stale` = an edit landed between snapshot and compare (skipped, counted).
5. **The lifecycle v1** (`voxel.Fluid.Faucets 1`):
   - **Headwater faucets**: `UVoxelWaterSubsystem::GatherHeadwaterFaucets` —
     baked `SECTION_HEADWATERS` (`FineTile::heads()`, bv24) when resident,
     else the bv23 fallback (`RiverNetwork::buildFromBakedWater` over the
     active box, `headwaterNodes()`; Q unknown there → cvar
     `voxel.Fluid.Faucets.DefaultQ`, default 8e6 m³/yr ≈ 253/s). Emission is
     scheduled by an exact integer accumulator (below), under the shared
     budget `voxel.Fluid.MaxSpawnPerTick`; overflow is re-carried, never
     dropped.
   - **Basin sink v1**: one basin (stated limit) — nearest `holdsWater()`
     basin whose bbox intersects the active region
     (`vxc::fluidPickBasinSink`, unit-tested), its clipped bbox + LIVE
     ledger-adjusted datum uploaded as finalize uniforms; particles inside
     below datum despawn `DESPAWN_BASIN` and the CPU credits
     `CreditBasinVolume` at exactly **255 units per particle**. Refused
     credits (tile streamed out) stay pending and retry.
   - **Boundary sink v1**: `DESPAWN_BOUNDARY` counts → 255 units each →
     `InjectRiverInflowNearVoxel`, attributed to the segment nearest the
     volume centre (positions are not read back — stated v1 limit). No graph
     in reach → units stay pending and retry; teardown records any final
     remainder as LOST, loudly.
   - **Sill faucets**: `UVoxelWaterSubsystem` holds spill events whose baked
     outlet falls inside the fluid's registered intercept box; the fluid
     drains them and emits `floor(units/255)` particles at the outlet;
     sub-particle tails and anything unemitted at teardown are refunded
     (`refundSpill`). Events outside the box route through the graph exactly
     as Phase 2 shipped; a grace-window flush (10 s) refunds anything a dead
     fluid host never drained.
6. **Conservation, extended across the seam.** The GPU-side invariant
   (spawned − despawned == alive) is unchanged; every readback additionally
   reconciles, in ledger units:
   `basinDespawns×255 == credited + pending + lost`,
   `boundaryDespawns×255 == injected + pending + lost`,
   `spillClaimed == spillEmitted×255 + spillRefunded + outstanding`.
   Violations are counted and logged (`scalarViolations`).

## The unit conversion, derived once

```
1 particle == one 10 cm voxel of water at rest spacing   (contract :27-29)
one 10 cm voxel == (100 mm)^3 = 1,000,000 mm^3 == exactly 1 litre
one ledger unit == 1/255 voxel                            (basinledger.h "THE UNIT")
⇒ 1 particle == 1 L == 255 ledger units
```

The constant is `vxc::kFluidLedgerUnitsPerParticle` in
`voxel-core/include/voxelcore/fluidlifecycle.h`, **defined from**
`kBasinLedgerUnitsPerVoxel` (never a literal), with `static_assert`s pinning
the chain and `test_fluidlifecycle.cpp` asserting both classic factor-of-255
errors as inequalities.

Faucet rate: `particles/s = Q[m³/yr] × 1000 / 31,557,600` (Julian year).
8e6 m³/yr → 253.5/s (the plan's worked example, asserted by test). The
schedule is the integer accumulator `vxc::FluidFaucetAccumulator` (carry in
m³·µs/yr; one particle costs `31,557,600 × 1000`), drift-free over hours by
test.

## Deviations / stated limits

- **No recentring (v0)**: the origin latches once; toggle
  `voxel.Fluid.Enable` to re-anchor (the volume's own cost note; toroidal
  addressing remains the planned contract change).
- **One basin sink per frame** — a region straddling two lakes credits only
  the picked one; v2 is a table upload (contract item 6 says so in place).
- **Boundary attribution** is by region centre, not exit position (positions
  aren't read back). Counted, documented.
- **Fallback headwaters** (bv23): rim of the box can carry false heads
  (rivernet.h's own caveat), and Q is the DefaultQ cvar, not baked. Both
  vanish when the running bake lands bv24 heads.
- **Occupancy solidity** goes through boolean `IsSolidAtVoxel` (the exposed
  overlay-aware path). Under `-VoxelWaterMarker=1` the marker's solid voxels
  would wall off marked rivers — the marker is a bring-up diagnostic and the
  fluid is not expected to run in that mode (noted at the pack site).
- **Initial fill is multi-second by design** (generation-bound, budgeted);
  unfilled space is SOLID, so early particles freeze rather than leak — the
  perf line's `occupancy=<built>/<deferred>` shows progress and the fill
  logs completion.
- Headwater faucet water is **exogenous** (created from baked Q, like graph
  baseflow); it is not debited from any ledger. Sill-faucet water IS ledger
  water and is fully reconciled.

## Perf line (1 Hz), all fields distinguishable from did-not-run

```
Fluid perf [run] alive=… spawned=… requested=… despawnBasin=… despawnBoundary=…
  simGpuMs=… iters=… slots=… violations=…
  faucet=<n>/s(n=<count>,heads|fallback)|off|gathering  spill=<n>/s
  sink(basin)=<n>/s|none|off  sink(boundary)=<n>/s
  occupancy=<regionsBuilt>/<deferred>  verify=pass|FAIL|stale|pending|off
  skippedNoOcc=<n>
Fluid ledger: emittedFaucet=… emittedSpill=… creditedToBasin=…(+… pending)
  injectedToGraph=…(+… pending) spillClaimed=… spillRefunded=… scalarViolations=…
```

## In-editor measurement script (integrator)

All PIE/game, one editor per box. Conditions + captures per standing rules;
no verdicts here.

**A. Dam break vs terrain (collision live)**
```
voxel.Fluid.Enable 1
voxel.Fluid.DebugDraw 1
; wait for "initial fill COMPLETE" in the log (occupancy=512/0), then:
voxel.Fluid.Spawn 5000
```
Read: particles pool ON the ground/slopes instead of the old infinite plane;
`Fluid perf` line `simGpuMs`, `violations=0`, `skippedNoOcc=0`. Dig a trench
under the pool (`TryDig`) — water should follow the edit within the region
budget. 90 s same-pose streaming guard applies if anything looks off.

**B. Occupancy verify gate**
```
voxel.Fluid.Occupancy.Verify 1
```
Read: `verify=pass` cycling (one region per snapshot; 512 regions ≈ full
volume sweep). Any `FAIL` line carries the first mismatching word + world
voxel. Turn off after — it costs a 16 MiB readback per cycle.

**C. Faucet river filling a basin (the lifecycle loop)**
Fly to a spot where a baked river reaches a lake (lake-survey sites; verify
the site per the site-verification tools first). Then:
```
voxel.Fluid.Enable 1
voxel.Fluid.Faucets 1
voxel.Fluid.DebugDraw 1
```
Read, in order: `faucet=<n>/s(n=…,heads|fallback)` non-zero → stream of
particles at the head(s); `sink(basin)=<n>/s` non-zero as particles reach the
lake; `Fluid ledger: creditedToBasin=…` climbing;
`voxel.Water.CreditBasin`-style verification via `GetBasinLedgerStats`
(sumUnits climbing) and the LAKE SHEET rising over minutes (the sink datum is
the live ledger-adjusted level, so the rise feeds back into where particles
despawn). At 253/s ≈ 65 kL/h — for a visible rise in minutes on a small
basin, raise `voxel.Fluid.Faucets.DefaultQ` (e.g. 8e8 ≈ 25k/s, budget-capped
by `voxel.Fluid.MaxSpawnPerTick`) and say so in the conditions.
- The scalar-only cross-check (no particles): `voxel.Water.CreditBasin
  <units>` still works and should agree with the sheet behaviour.

**D. Sill faucet + boundary sink**
With C running and the lake credited to its sill (`GetBasinLedgerStats`
spilled units > 0): spill events whose outlet is inside the region emit as
particles at the saddle (`spill=<n>/s`); events outside route/refund as
Phase 2 (`voxel.Water.Rivers 1` to give them a graph). Boundary:
`sink(boundary)=<n>/s` with `injectedToGraph` climbing when a graph is armed;
with no graph, `+pending` holds the units (nothing dropped) — that is the
designed state, not a leak.

**E. Conservation soak**
Leave C running 10+ minutes: `violations=0`, `scalarViolations=0`,
`spillRefunded + emitted` closing against `spillClaimed`, then
`voxel.Fluid.Enable 0` and read the teardown line (refunds + any stated LOST
units).
