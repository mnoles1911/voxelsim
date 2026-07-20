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
