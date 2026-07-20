# M2 — LOD cascade (working plan)

Gate (plan §4): 50km+ vista, 60fps, fast flight with no hitches (only ring
coarsening). Bands per plan §3.3. Prereqs in place: voxel mip chain
(voxelcore/mips.h, worldgen-versioned), streaming machinery (stage 2/3a),
C++ tile client (tilestore.h) for clipmap source data.

## Decisions (binding once implementation starts; ADR for deviations)

| Topic | Decision |
|---|---|
| Ring structure | R0 = true voxels (existing). R1–R4 = mip levels 1–4 (20cm→1.6m cubes). Ring radii (default preset): R0 64m, R1 128m, R2 256m, R3 512m, R4 1024m; power-of-two aligned so ring boundaries land on parent-cell edges ("8 cubes become 1 in place"). |
| Ring streaming | Generalize FChunkRecord/queues to (level, chunkKey): one desired-set pass computes, per level, the annulus [innerR(level), outerR(level)] with the same hysteresis/budget machinery. Level-L render chunks cover 32 level-L cells (so world size doubles per level); one component type serves all levels (quads are level-agnostic; position scale = VoxelSizeUU << level). |
| Mip sourcing | Workers build level-L bricks via voxelcore MipChain over the SAME pure-generated source path used today (no overlay in workers; edited chunks take the game-thread overlay-aware path, all levels). MipChain caching stays in the worker-side impl keyed identically — measure before adding cross-job sharing. |
| Meshing mips | Same greedy mesher (it is level-agnostic over Brick<8>); apron sampling at level L reads level-L neighbors — provide a level-aware sampler from MipChain, never mix levels inside one mesh. |
| Ring transitions | v0: hard boundary (inner ring simply occludes outer; both rendered in the overlap band of 1 chunk). Dithered blue-noise cross-fade is a POLISH item (plan flags it as the historical slip risk) — schedule after Band 3 exists, behind a cvar. |
| Band 3 (heightmap clipmap) | **First slice landed** (see "Band 3 first slice" below): `AVoxelClipmapActor`, 4 concentric `UProceduralMeshComponent` levels (65×65 verts each), TILE elevation direct (30m/px bilinear, same seed as the ring cascade), covering the ring cascade's edge (~1km) out to ~16.4km radius (~32.8km diameter). CDLOD polish (dithered cross-fades, τ-driven LOD, partial-amplifier detail) remains a later track. |
| Distant edits | Edits already bump generation ids per (level-0) chunk; propagate: an edit marks dirty its ancestor mip chunks up the chain (cheap key math) → re-mesh through the overlay-aware path at each level. Mip of edited bricks = MipChain over World::brickAt (overlay-aware) — needs a small overlay-aware source hook, game-thread only. |
| τ (screen-space error) | v0: pure distance rings (above). τ-driven selection + user presets arrive with Band 3 when there is something to trade off. |
| Perf budget | Ring levels share the existing job/apply budgets; per-level counters logged. Rings beyond R0 are ~constant cost by construction (log-scale property, plan §3.3). |

## First implementation wave (after current LWC/ocean/GPU-voxelize wave lands)

1. Level-aware streaming records + desired-set annuli (subsystem refactor).
2. MipChain worker integration + level-aware apron sampler.
3. Component/proxy: per-level position scale; per-level material tint debug
   cvar (visualize ring boundaries).
4. Verification: mountain spawn screenshot showing R0→R4 rings; flight run
   (scripted camera speed) with frame-time log proving no hitches at ring
   crossings; counters per level.

### Wave 1 status (implemented)

- `VoxelCoords::FVoxelLevelChunkKey` (level, chunk) generalizes every
  streaming record/queue in `VoxelWorldSubsystem.cpp` (`ChunkRecords`,
  `PendingJobKeys`, `PendingGameThreadKeys`, `PendingUnloadKeys`). Desired set
  per level = annulus `[RingPresets[level].Inner, .Outer)` (radii from the
  table above); outer-edge hysteresis only (`UnloadRingMultiplier = 1.25`,
  same 64/80m ratio R0 always had) — the inner edge has none, per the v0
  "hard boundary" decision (a chunk crossing into a finer level's annulus
  unloads from the coarser level immediately; the ~1-chunk quantization
  overlap this leaves is the plan's accepted overlap band, not extra
  hysteresis). Priority: nearest-first within a level, lower level wins ties
  (`FVoxelWorldImpl::SortPendingQueues`). Each level's O(candidates) entry
  scan is gated on that level's own chunk crossing
  (`bHasRecomputedLevel`/`LastAnchorChunkPerLevel`), not the level-0 3.2m
  trigger, so outer rings don't re-scan on every player step.
- Level-L (L>=1) worker jobs build bricks via `vxc::MipChain<8>`
  (`MakeLevelSampler` in `VoxelWorldSubsystem.cpp`) over a pure
  `GeneratedWorld` level-0 source with a per-job LRU (64 entries) of
  level-0 `(bx,by)` column grids — avoids re-running the amplifier for every
  level-0 brick in a vertical stack sharing one XY footprint. Level 0 keeps
  its existing hand-tuned column-grid fast path unchanged (routing it
  through MipChain too would be strictly slower: an extra `Brick<8>`
  materialize + `get()` per voxel for no benefit at level 0).
- `UVoxelChunkComponent::SetLevel` + `VoxelCoords::ChunkOriginWorldForLevel`
  give each render chunk its `VoxelSizeUU << level` position/bounds scale;
  one component type serves every level (`VoxelChunkComponent.cpp`
  `MakePos`/`CalcBounds`).
- `voxel.Debug.Rings` cvar + `VoxelDebug::RingLevelTint` (R0 green .. R4
  magenta) reuse the P1 `SetDebugTint`/MID machinery; takes priority over
  `voxel.Debug.ChunkStates` if both are enabled (one MID per component, no
  blending). `-VoxelDebugRings` forces `voxel.Debug=2` +
  `voxel.Debug.Rings=1` for headless verification runs.
- Per-level loaded/pending counts added to `FVoxelPerfSnapshot`
  (`LevelLoadedCount`/`LevelPendingCount`, `VoxelDebug.h`) and the perf HUD
  (`Rings: R0 n/p R1 n/p ...` row).

### Known limitation (wave 1, tracked for a later M2 item)

**Distant edits do not propagate to mip levels.** Only level 0 takes the
overlay-aware, edit-log-authority game-thread mesh path
(`ChunkHasEditedBrick`/`MarkChunkDirtyForRemesh`/`PendingGameThreadKeys` are
level-0-only). Levels 1-4 always mesh via `MakeLevelSampler`'s pure-generated
`MipChain`, which never consults `World`'s overlay — so a dig/place/explosion
crater is visible up close (R0) but invisible in the coarser rings around it
until that ground truly leaves R0's radius and the mip chunk is rebuilt from
scratch (which still won't show the edit, since the rebuild is still
pure-generated). Fixing this needs: (a) an overlay-aware `MipChain` level-0
source hook (`World::brickAt`, game-thread only, per the original plan row),
and (b) edit-time propagation that marks dirty every ancestor mip chunk up
the chain and re-meshes them through that overlay-aware path. Deferred to a
later M2 wave; the plan's "Distant edits" decisions-table row already
describes the intended design.

## Band 3 first slice (heightmap clipmap, implemented)

`AVoxelClipmapActor` (`VoxelClipmapActor.h/.cpp`), spawned by
`AVoxelEarthGameMode::BeginPlay` alongside the light rig/ocean actor: 4
concentric levels, each a `UProceduralMeshComponent` section over a fixed
65×65-vertex (64×64-quad) grid. Every level shares identical local topology
(triangle indices + UVs, built once in `BuildSharedTopology`) — only spacing,
world placement, and sampled heights differ per level.

**Geometry/coverage math (deviation from the task spec's illustrative
numbers — see below).** Hole half-extent = `HoleHalfIndex` (16) × spacing;
grid half-extent = `HalfIndex` (32) × spacing = 2× hole half-extent, so every
level's hole is exactly a quarter of its own area, constant across all 4
levels. Level 0's spacing is derived from the ring cascade's own outer edge
(`UVoxelWorldSubsystem::RingPresets[R4].OuterMeters`, 1024m) so its hole
lands exactly there: `spacing0 = 1024m / 16 = 64m/vertex`. Levels double
spacing per step:

| Level | Spacing | Inner (hole) | Outer |
|---|---|---|---|
| 0 | 64 m/vertex | 1024 m (ring edge) | 2048 m |
| 1 | 128 m/vertex | 2048 m | 4096 m |
| 2 | 256 m/vertex | 4096 m | 8192 m |
| 3 | 512 m/vertex | 8192 m | 16384 m |

Total coverage: ring edge (~1km) → 16.384km radius, ~32.8km diameter —
close to the task's "~1km→30km diameter overall" target. **Deviation:** the
task's illustrative numbers (16m/vertex level 0, doubling to 128m/vertex,
"~16km half-extent" for the outermost level) don't reconcile with each
other for a fixed 65-vertex grid — 65 vertices at 128m spacing spans only
~8.2km, not a 16km half-extent (would need ~4x more vertices or ~4x coarser
spacing to hit that). The task explicitly permitted tuning to hit the stated
diameter goal, so the table above is the corrected, self-consistent version
of the same doubling-annulus idea, chosen because it exactly extends
`UVoxelWorldSubsystem::RingPresets`' own R0–R4 doubling pattern outward
(single source of truth: level 0's spacing is *computed from* `RingPresets`,
not a hardcoded duplicate of 1024m).

**Height source.** TILE elevation directly, 30m/px bilinear
(`SampleHeightUU` in `VoxelClipmapActor.cpp`, file-local free function — no
voxel-core header ever appears in the UHT-parsed `VoxelClipmapActor.h`), via
a function-local `static vxc::SyntheticTileSampler` seeded with
`UVoxelWorldSubsystem::DefaultSeed` (same seed the ring cascade uses, so the
clipmap lines up with voxel terrain at the shared seam). `SyntheticTileSampler`
is stateless (no caching), so this is 4 `valueNoise2` evaluations per tap,
called 4× (bilinear corners) × 4225 vertices × up to 1 level/frame — trivial
per the task's own "trivially cheap" characterization; no perf issue
observed (see verification below).

**Recenter/rebuild.** Each level snaps to its own vertex-spacing grid
(`FMath::GridSnap`) as the camera moves; a level only rebuilds when its
snapped origin changes. Steady state is round-robin, ≤1 rebuild/tick. The
very first tick a camera becomes available is a one-time exception: all 4
levels build immediately (avoids a 4-frame terrain pop-in at spawn) — a
one-off cost, not a recurring one. First build per level uses
`CreateMeshSection`; every rebuild after that uses `UpdateMeshSection`
(topology is invariant, so this skips scene-proxy recreation).

**Cracks/overlap.** Quads fully inside a level's hole are never emitted
(annulus-only rendering). Both the outer grid edge and the inner hole
boundary drop 2× that level's spacing (skirts), computed from unmodified
heights so normals/slope/snow shading stay correct — only vertex position
dips. Seam artifacts against the ring cascade and between adjacent clipmap
levels are an accepted v1 gap (matches the plan's own "z-fighting is
acceptable v1" allowance); the CDLOD polish item is the real fix.

**Material.** `Tools/create_clipmap_material.py` authors `M_VoxelClipmap`:
vertex-color-driven (R = slope factor, G = snow factor, both computed
per-vertex in `RebuildLevel`, not in the material graph) two-lerp blend
(green low/flat → grey steep → white above a 2700–2900m ramp centred on the
amplifier's 2800m snowline), plus the same `DebugTint` vector-parameter
pattern as `M_VoxelTerrain` for the cyan debug tint
(`VoxelDebug::HeightmapBandTint`, `voxel.Debug.Rings`). **Deviation:** the
material is two-sided (`M_VoxelTerrain` is one-sided) — clipmap triangle
winding was picked by hand and isn't visually verifiable in this headless
task, so two-sided is a defensive guarantee the terrain renders right-side
up regardless; a follow-up can flip it once a screenshot confirms winding.

**PMC exception (ADR-worthy, flagged per the task spec).** `AVoxelChunkComponent`
uses a hand-rolled `FPrimitiveSceneProxy` specifically because the doctrine
(plan §3.3 Band 1) targets *voxel* rendering (per-quad material id/
orientation/AO, GPU greedy meshing). Band 3 has none of that — it's a
conventional heightmap with no voxel data, rebuilt as flat vertex/index
buffers on the CPU. `UProceduralMeshComponent` is the doctrine-clean tool for
that content, not a doctrine violation; this mirrors the reasoning
`AVoxelOceanActor`'s header already gives for the opposite call (plain
`UStaticMeshComponent`, since ocean isn't procedural mesh data at all). No
ADR file was added in this pass — recorded here and in the class comment
instead; promote to `docs/adr/` if a reviewer wants it split out.

**Verification switch.** `-VoxelCameraHigh=<meters>` (`AVoxelEarthGameMode::RestartPlayer`)
spawns the pawn that many meters above the surface instead of the default
+5m, for vista screenshots where a ground-level spawn can't see 30km out.
