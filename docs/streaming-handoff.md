# Streaming-speed + GPU-rendering handoff (2026-07-24)

Session handoff for the next engineer. All work is on branch
**`claude/terrain-holes-wip`** (pushed to origin). **Nothing is merged to
`main`.** Everything below is CPU-side streaming work plus a GPU design doc.

## TL;DR state

| Item | Status |
|---|---|
| See-through concentric rings (broken ring cross-fade) | ✅ Fixed, Matt confirmed |
| Slow fill (minutes) | ✅ Adaptive applies → ~45 s, Matt confirmed |
| Holes while moving | ⚠️ Coverage-based load-before-unload landed but **NOT yet tested in-engine** |
| GPU-resident rendering | 📄 ADR-0006 + plan written, **pending Matt sign-off**, no code |
| D (apply-priority), F (trim underground) | ⏸️ Held, not started |

The **immediate next action is Matt (or you) testing the latest build in UE** —
walk/fly fast and check whether the rolling-ring holes are gone. The editor DLL
is already relinked; just reopen and play. If holes remain, see "If holes
persist" below.

## What changed, commit by commit (on `claude/terrain-holes-wip`)

1. **`3db8089` Fix see-through LOD rings** — the M2 ring cross-fade
   (`ComputeRingFadeParams` in `VoxelChunkComponent.cpp`) was fading real terrain
   to transparent because `RingPresets` annuli ABUT with zero overlap, so the
   dither cross-dissolve had no second LOD to dissolve into. Disabled by default;
   `-VoxelRingCrossFade` re-enables the old fade for A/B. **Confirmed fixed.**

2. **`ebf4fc7` Adaptive time-budgeted applies** — root cause of slow fill was
   `voxel.Stream.MaxAppliesPerFrame=3` (render-thread `AddPrimitive` funnel).
   `DrainResults` now drains until `voxel.Stream.ApplyBudgetMs` (default 6 ms)
   with `MaxAppliesPerFrame` raised 3→64 as a ceiling. Companion caps: unloads
   2→24, remeshes 2→8. Fill minutes→~45 s. **Confirmed better.**

3. **`5fc8145` Load-before-unload (timer)** — kept a visible LOD-transition chunk
   drawn as a stand-in for `voxel.Stream.LodRetentionMs`. Helped but left
   **rolling rings of holes**: under fast movement the fixed 1 s timer expired
   before the replacement streamed in.

4. **`75a48a1` ADR-0006 + `docs/gpu-streaming-plan.md`** — GPU rendering design
   (see below). Design only.

5. **`995f054` Coverage-based load-before-unload** — replaces the timer with
   actual-coverage release. This is the **untested** change that should kill the
   rolling rings. Mechanism:
   - `ColumnGeomCount` (member `TMap<FIntVector,int32>` in `FVoxelWorldImpl`):
     per `(level, chunkX, chunkY)` XY-column count of resident records with
     visible geometry (`LastQuadCount>0`). Maintained by `ReconcileColumnGeom`
     at geometry gain/loss sites.
   - On an LOD-transition eviction, `RecomputeDesiredSet` stamps
     `FChunkRecord::RetainReplaceDir` (finer took over from inside / coarser from
     outside) and a safety-cap time.
   - `DrainUnloads` keeps the stand-in drawn until `ReplacementCovered` reports
     the replacement footprint is on screen (finer: all 4 child columns at L-1
     have geometry; coarser: the L+1 parent column does), OR the safety cap
     (`LodRetentionMs`, now default 5000 ms) elapses.

## How to build + test

```
"D:/UE_5.8/Engine/Build/BatchFiles/Build.bat" VoxelEarthEditor Win64 Development \
  -Project="D:/voxelsim/ue-project/VoxelEarth.uproject" -WaitMutex -NoHotReloadFromIDE
```
Close the UE editor first (it locks `UnrealEditor-VoxelEarth.dll`; a compile
succeeds but the link fails with LNK1104 while it's open). Repro framing for the
holes: real tiles, `-VoxelSpawnAt=-84480,53760`, then walk/fly fast.

Runtime knobs (no rebuild needed):
- `voxel.Stream.ApplyBudgetMs 10` — fill harder.
- `voxel.Stream.LodRetentionMs 0` — disable retention (holes return; A/B proof).
- `voxel.Stream.LodRetentionMs 8000` — longer safety cap.
- `-VoxelRingCrossFade` — restore the old (broken) fade.

## If holes persist after the coverage change

Debug order:
1. Confirm retention fires at all: temporarily log in the eviction block
   (`RecomputeDesiredSet`, where `RetainReplaceDir` is set) and in the
   `DrainUnloads` coverage gate.
2. Check `ColumnGeomCount` correctness — **prime suspect: a leak makes a column
   read "covered" when it isn't → premature park → hole.** The two pre-dispatch
   **buried/sky skip sites** in `DispatchJobs` (search `BuriedSkipEnabled` and
   `IsChunkProvablyAllAir`) clear a record's geometry but were **left without a
   `ReconcileColumnGeom(..., false)` call** (records there normally have no
   component, so low-risk — but verify). Add the reconcile there if a counted
   chunk can reach those paths.
3. Consider that some holes may be the **residual ring-boundary T-junction
   seams** (the original bug on this branch, commit `3b33a79` — a partial skirt
   fix in `MeshChunkBricks`/`ComputeRingSkirtMask`). Those are thin cracks at
   boundaries, present regardless of load speed. Bisect with
   `voxel.Stream.LodRetentionMs 0` (if holes remain with retention off AND on,
   they're likely seams, not load/unload).

## Not started / TODO backlog

- **D — apply-priority**: the `ResultsQueue` applies FIFO-by-completion. Dispatch
  is already near-first (ring quota + sorted queues), so only add explicit
  apply-side priority if testing shows far/underground chunks landing ahead of
  the terrain under the player. Held pending the coverage test.
- **F — trim underground R0 column**: the deep R0 column (38.4 m, hidden) competes
  for the apply budget with the visible surface. Defer/skip it until dug.
  **Risky** — touches dig-reveal + collision; do it isolated, after the moving-
  holes are confirmed fixed, with its own A/B. (`bDeepAnchorRelative` on
  `FChunkRecord`, `VoxelUnderground` namespace.)
- **Debug overlay**: add `ColumnGeomCount` size + retained-stand-in count to the
  HUD (`VoxelEarthHUD.cpp`) so this is observable in-engine.
- **Reconcile the two skip sites** (see debug step 2 above) for full correctness.
- **M1 gate re-run**: these throttles were raised past the M1 zero-hitch tuning
  (Matt approved trading strict zero-hitch for silky/fast). A fresh
  `-VoxelPerfRun` p95 + hitch count should be recorded before merging to main.
- **Residual ring-boundary T-junction skirt** (`3b33a79`) — the skirt only fires
  on faces whose neighbour is a finer ring; verify it actually closes the thin
  seams or finish it.
- **Underground rework** (caves/tunnels/cavern-lakes) — separate design-led
  backlog item, Matt wants human input; NOT part of this workstream. See the
  `underground-rework-backlog` memory.

## GPU rendering — the big leap (design done, needs sign-off)

`docs/adr/0006-gpu-resident-voxel-streaming.md` + `docs/gpu-streaming-plan.md`.
Core decision: GPU-generated, GPU-resident geometry drawn via a few persistent
primitives + indirect draws, so streaming a chunk is a pool-region + draw-arg
update, **never a per-chunk `FScene` mutation** (that funnel is the real ceiling;
the CPU work above only widens it). **Key doctrine call:** the runtime mesh is
DISPLAY-ONLY and not required cross-vendor bit-exact — authority (voxel state,
collision, dig, water, replication) stays on the CPU integer-deterministic path.
This is flagged in the ADR for **Matt's explicit sign-off** because it interprets
the §2 determinism boundary. Builds on ADR-0001 (integer HLSL), the done+AMD-
verified voxelize kernel, and the speced-but-unbuilt `docs/gpu-mesher-design.md`.
Milestones G0 (sizing study — Matt picks view distance vs VRAM) → G5 (flip
default). **Do not start coding until Matt signs off ADR-0006.**

## Merge path
When the coverage change is verified in-engine and the M1 gate is re-run, this
branch (`claude/terrain-holes-wip`) can go to `main` as one PR (it also carries
the ADR + plan docs). Consider splitting the docs into their own PR if you want
the ADR reviewable independently of the code.
