# Phase 5 — retiring the terrain quad path

Companion to `docs/ray-marching-plan-2026-08-19.md`. Written for the marcher and GI workstreams to
work against directly, because it names couplings on both their territory.

**Status: step 1 done, steps 2–4 held.** The quad path cannot be turned off yet — the marcher misses
86% of the hits it should make, so the quad pool is the only thing drawing terrain. The deliverable is
a switch that is *ready to flip*, plus measurements taken with it flipped in an arm. Not a change of
default.

**The prize, measured not argued:** `fill 0.227 + pack 0.160 = 0.389 ms/chunk` against today's
`mesh 0.810 + pack 0.158 = 0.969` — **2.49× cheaper per chunk**, with `fill < mesh` by 3.57×.
Plus ~2,197 MB of committed VRAM (below).

---

## 1. Step 1 (DONE): the all-solid pre-dispatch hole

`VoxelFootprintBand.h:85 BandProvesChunkEmpty` answers *"does this chunk mesh to zero quads"*. Both
all-air and all-solid do. For a mesher, dropping either before dispatch is free and byte-identical.

**For a marcher they are opposites.**

| case | no chunk record | marcher reads | verdict |
|---|---|---|---|
| all-air | lookup misses | empty | correct |
| all-solid | lookup misses | empty | **see through solid rock** |

Measured on `p3b1-storage-r1`, one 5 s fill window: `skipped=10210 (air=1462 solid=8748)`. Over 9,300
all-solid chunks dropped before dispatch and never packed, against L0 residency of 16,892. Steady-state
windows read `skipped=0`, which is why this was previously recorded as "fired 0 times this leg" — **it
fires during fill and is finished before anyone reads a settled window.**

### Why brick coverage could not see it

Coverage was reported as **99.4% of the 88,151 chunks that get a mesh job**. A chunk dropped before
dispatch is not in that denominator. The metric is *structurally incapable* of counting this failure.
Anything that skips a chunk before dispatch must ask `BandSkipMayDropChunk`, never `BandProvesChunkEmpty`.

### Four drop sites found, three fixed

| site | file:line | default | fixed |
|---|---|---|---|
| dispatch-time buried skip | `VoxelWorldSubsystem.cpp:11001` | **ON** | yes |
| admission all-solid skip (`IsChunkProvablyAllSolid`) | `VoxelWorldSubsystem.cpp:9457` | **ON** | yes |
| admission band skip | `VoxelWorldSubsystem.cpp:9385` | off (mode 0) | yes |
| coverage-verify hole scan | `VoxelWorldSubsystem.cpp:10066` | — | **no — see §7** |

The policy lives in exactly one place (`VoxelFootprintBand.h:139 BandSkipMayDropChunk` / `:173 AllSolidProofMayDropChunk`) and is gated on `VoxelBrickCpuArm::VolumeNeedsSolidChunks()` =
`voxel.GPU.BrickPack`. That is the **master** gate, not `ShouldPack()` — the GPU fork packs under
`BrickPack` alone with `voxel.Brick.PackOnCpu` off, and asking `ShouldPack` would keep dropping
all-solid chunks on exactly the arm where the fork is the only producer. With the volume off, every
site behaves as before, byte-identical.

### How it evidences itself, and how it can fail

Two exclusive counters — `solid=` (actually dropped) and `solidKept=` (proof overridden) — printed on
the existing skip lines with `volumeFed=`. The pair is falsifiable **from both sides**:

- `volumeFed=1` → `solid=` **must be 0**, `solidKept=` large during fill.
- `volumeFed=0` → `solidKept=` must be 0, `solid=` carries its old meaning.

Either reading zero when it should not is an Error line. This is **not vacuous**: the three fixed sites
are gated, but a *new* skip site added later lands in `solid=` and trips the wire — which is the
regression actually worth catching.

Offline gate: `VoxelEarth.BrickSink.BandSkipPolicy` pins that the underlying proof is indifferent to
air-vs-solid (the trap), that volume-off behaviour is unchanged, and that volume-on drops air and keeps
solid. It fails if the policy is reverted.

**Expected on the next leg:** brick residency rises by roughly the all-solid skip count; the coverage
denominator changes with it, so 99.4% is not comparable across this fix.

---

## 2. The shape: mostly *not producing*, not rewiring

`ApplyMeshResult`'s `NumQuads == 0` branch (`VoxelWorldSubsystem.cpp:12890`) is first-class and heavily
exercised: it releases geometry, keeps the record, marks the chunk **settled**, and fires the fluid
hook. 37,539 of 88,151 chunks take it every run. Phase 5 makes *every* terrain chunk take a path the
pipeline already runs on 43% of its traffic. That is the main de-risking fact.

### Retires

1. **Mesh/scan/emit dispatch.** CPU worker already gated by `voxel.Brick.SuppressQuadMesh`; the **GPU
   fork is not gated at all** — that cvar is worker-only and the fork still meshes its ~8%. The regions
   separate cleanly (mesh `VoxelGpuMeshJobManager.cpp:980`, brick `:1053`), but the job state machine is
   driven by `TotalReadback` and `PollInFlight` fails a job lacking it (`:1302`), so a brick-only job
   needs its own state path. **The only piece with real work in it.**
2. **Terrain quad pool allocation.** The code states its own cost at `VoxelWorldSubsystem.cpp:12144`:
   **1465 MB + 732 MB = ~2,197 MB committed** (192M quads × 8 B, plus the chunk-id buffer at 4 B).
   Against the brick pool's 388 MiB.
3. **CPU mesher as a *producer*.** It stays as the byte-equality reference and A/B control.

**Coupling worth stating:** retiring quad meshing turns off the 4.56× pack optimisation, because
`FDenseChunkSink` is fed *by the mesher*. Already handled — `PackChunkMaterialising` is the no-mesher
path at `fill 0.227` — but the switches are not independent and an arm must use the right one.

### Stays alive (water-shared, verified)

`UVoxelGpuPoolComponent`, `FVoxelQuadVertexFactory`, `VoxelQuadDecode.ush`, `FVoxelChunkQuad`,
`PackVoxelChunkQuad`, `FVoxelGpuGeometryPool`, the paged CPU shadow, the chunk-id buffer. Water runs
**its own independent instances** (`VoxelWaterSubsystem.cpp:1865` documents why). Only the terrain
*instance* retires.

### Collision — verified, not assumed

No collision, no navmesh, no physics body, no audio reads terrain meshes.
`VoxelChunkComponent.cpp:1096`, `VoxelGpuPoolComponent.cpp:2578`, `VoxelGpuChunkComponent.cpp:217` are
all `NoCollision`; movement is voxel-DDA via `IsSolidAtVoxel` (`VoxelCharacterMovement.cpp:186`).

---

## 3. Ring skirts — retire, but *after*, not alongside

Agreed that they retire. Evidence is one-sided: water never touches them (zero hits across all three
water files); the brick region already forces `RingSkirtMask = 0` with a written rationale citing format
§8 (`VoxelGpuMeshJobManager.cpp:279`); `VoxelEarth.BrickSink.Skirt` asserts the skirt never reaches
packed voxels; their only consumer is the mesher being retired.

**But** they reach `voxel-core/shaders/worldgen.ush` and **seven prebuilt SPIR-V blobs**, and
`worldgen.ush` is shared with the brick region's own `AddRegionPasses`. Removing them is a shader change
plus SPIR-V regeneration in a change that otherwise needs neither. Sequence after.

Small win when it happens: the skirt currently disqualifies the brick-level solid skip on boundary
bricks (`VoxelWorldSubsystem.cpp:11538`), so removing it slightly *speeds up* the packer.

Carry forward: the marcher's residual at ring boundaries is a **silhouette pop, not a hole**, mitigated
by one chunk of ring overlap (~2% residency) — `ray-marching-plan-2026-08-19.md` §5.

---

## 4. The gate, designed so it can fail

One switch, `voxel.Terrain.RetireQuads`, subsuming `SuppressQuadMesh`. Its self-check is **not** "did we
stop meshing" (unfalsifiable) but:

- terrain pool **used** quads == 0 while on;
- brick residency ≥ the chunk count that would have meshed, so "we stopped producing" cannot pass while
  "we also stopped packing" is true;
- the all-solid skip counters at zero (§1).

> **`UVoxelGpuPoolComponent::GetNumQuads()` returns CAPACITY, not usage** (`VoxelGpuPoolComponent.cpp:2679`).
> A gate built on the obvious accessor is vacuous. Use `FVoxelGpuGeometryPool::GetUsedQuads()`; the
> component needs a small public accessor for it.

---

## 5. Guards that go permanently silent — retire or rework *at the same time*

Three soundness verifiers are `Result.QuadCount() > 0` violations. Once terrain stops producing quads
they can **never fire**, while continuing to log as healthy:

| guard | file:line |
|---|---|
| buried-skip soundness | `VoxelWorldSubsystem.cpp:13452` |
| solid-skip soundness | `VoxelWorldSubsystem.cpp:13399` |
| third quad-count check | `VoxelWorldSubsystem.cpp:13498` |

These must be retired or re-expressed against the brick volume **in the same change** that stops quad
production, not later. A guard that cannot fail being quoted as evidence is the failure mode this
project has hit repeatedly.

Same category: `voxel.March.VerifyDepth` (`VoxelMarchRenderer.cpp:190`) compares the marcher against
**raster depth in the same frame**. Retiring the quad path removes the P3 gate's own reference — so that
gate must run *before* retirement.

---

## 6. What breaks that nobody listed

1. **GI reads a *water* pool's transform once the terrain instance is gone.**
   `VoxelGI.cpp:297 FindPoolWorldLocation` does `TObjectIterator<UVoxelGpuPoolComponent>` and takes the
   **first** registered pool as the GI volume origin. Water spawns several
   (`VoxelWaterSubsystem.cpp:1997`). Latent, silent, and **triggered by an absence** — nothing errors.
2. **Player position depends on terrain geometry residency — a gameplay break, not a metric break.**
   `VoxelCharacterMovement.cpp:872` teleports the pawn onto the analytic surface when
   `DebugChunkStatusAt` reports no component; `:435` vetoes gravity on the same signal. Both read
   `HoldsGeometry()`, which is quad/component-valued. Under retirement every chunk reports no geometry
   → the pawn is permanently `bWaitingForTerrain`. Guarded by `VoxelWalkTestSubsystem.cpp:187`.
3. **Parking and LOD retention refuse zero-quad chunks** (`:12338`, `:8693`). Parking hit rate (28,117
   parks, 84%) goes to zero and load-before-unload stand-ins stop being retained. Throughput, not
   correctness.
4. **GI's light field is rasterised from quads.** `VoxelLightField.cpp:236` sets `bHasGeometry` from
   `Quads.Num() > 0` — the deepest structural coupling. Owned by the GI workstream.
5. **`loaded=` means something different under retirement** — it counts geometry publication. Replacement
   is packs/s and the fill span on `voxel.Brick.Stats`.

---

## 7. Known-and-deliberately-unfixed

`VoxelWorldSubsystem.cpp:10066` — the coverage verifier suppresses a hole report for a whole column
proven empty, using `BandProvesChunkEmpty`. Under a fed volume this hides *missing all-solid chunks*
from the hole detector. Left alone in step 1 because changing it risks a flood of false holes on
legitimately-air columns, and because it is a **reporting** blind spot rather than a dispatch drop.
Revisit when the marcher is the draw path.

---

## 8. Measurements

**Available on an arm now:** chunks/s and cold fill via packs/s and the fill span; per-chunk cost via
the three-term readout (`mesh` / `fill` / `pack`); VRAM at settle.

**The VRAM half is measurable today with no code:** `-VoxelPoolCapacityQuads=1000000` clamps the terrain
pool to 12 MB (min clamp at `VoxelWorldSubsystem.cpp:12160`), and with `SuppressQuadMesh 1` nothing
allocates from it. That gives the 2,197 MB → 12 MB delta on the existing binary.

**The frame cannot be measured until the marcher works.** With quads retired and the marcher missing
86% of hits, the arm renders nothing. Throughput, VRAM and per-chunk cost are real; frame time is not,
and must not be reported.

---

## 9. Sequence

1. ~~Fix the all-solid pre-dispatch skip.~~ **Done.**
2. **HOLD** — re-measure the marcher's miss rate with (1) fixed. If 86% collapses, the traversal may be
   fine and the DDA hunt should stop; if it barely moves, both defects are real and independent.
3. Gate the GPU fork so a brick-only job exists.
4. Promote the switch to `voxel.Terrain.RetireQuads` with the falsifiable self-check and the pool-usage
   accessor.
5. Follow-ons with owners: GI ingest + `FindPoolWorldLocation`; the movement/residency coupling; skirts.
