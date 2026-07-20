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
| Band 3 (heightmap clipmap) | Separate track after R1–R4 prove out: CDLOD-style clipmap fed by TileGridSampler elevation directly (30m/px → 1.25m via partial amplifier later). Not in the first M2 wave. |
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
