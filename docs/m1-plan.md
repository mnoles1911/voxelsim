# M1 — Walkable world in UE5 (working plan)

Gate (plan §4): walk & dig at 60fps on min-spec. Staged; each stage lands as
a PR. Doctrine constraints in force: custom scene proxies (NOT
ProceduralMeshComponent), edits only via the edit log/overlay, budgeted work
per frame (§2.5), LWC + origin rebasing from day one (§3.3).

## Decisions (stage 1 unless revisited)

| Topic | Decision |
|---|---|
| Brick size | 8³ (bench favored it; revisit after GPU port per status.md) |
| Render chunk | 4×4×4 bricks = 32³ voxels (3.2m cube). One `UVoxelChunkComponent` + one `FVoxelChunkSceneProxy` per render chunk; re-mesh unit on edit. |
| Vertex path | Stage 1: `FLocalVertexFactory` with CPU-built vertex/index buffers (quads → 4 verts / 6 idx). Custom compressed vertex factory + pooled GPU buffers when perf data demands (stage 3+). |
| Vertex data | Position local to chunk origin; normal/tangent from quad axis; vertex color = (materialId, ao, 0, 255); UVs world-planar per axis. |
| Material | One master `M_VoxelTerrain`: per-material albedo/rough via LUT indexed by vertex-color R; AO from vertex-color G; texture arrays later (stage 3). |
| World ownership | `UVoxelWorldSubsystem` (UWorldSubsystem) owns `vxc::World<8>` + `SyntheticTileSampler`, seed from config (default 20260719). All voxel-core access via this subsystem, game thread only in stage 1. |
| Generation | Stage 1: synchronous generation of a fixed radius (default 24m) around origin at world init. Async budgeted streaming is stage 2 — do not half-build it in stage 1. |
| Meshing input | Mesher's [-1,B] apron sampled through `World::materialAt` (overlay-aware), so cross-brick faces cull correctly and edits re-mesh seamlessly. |
| Coordinates | 1 voxel = 10cm = 10 UU (UE cm). Voxel (0,0,0) corner at UE origin, +Z up matches. All world↔voxel transforms through one header (`VoxelCoords.h`); LWC `FVector` in, `int64` out. |

## Stage 2 decisions (threading, budgets, edits)

| Topic | Decision |
|---|---|
| Worker threading | Streaming gen+mesh workers touch ONLY `GeneratedWorld` (pure function of seed — lock-free, no overlay access). Chunks containing edited bricks are meshed on the game thread via `World` (overlay-aware). Edited chunks are rare; this buys zero-lock streaming. |
| Async plumbing | UE::Tasks (background priority) per chunk job; results into an MPSC queue; game thread drains with budgets. |
| Budgets (§2.5) | Defaults, all tunable: ≤2×cores jobs in flight, ≤8 chunk component applies/frame, ≤4 unloads/frame, ≤4 edit re-meshes/frame. Nearest-first priority (distance²; screen-space error arrives with LOD in M2). |
| Hysteresis | Load ring 64m, unload ring 80m (chunk enters desired set inside 64, leaves outside 80). |
| Dig/place | Custom integer DDA raycast (voxelcore/raycast.h — deterministic, server-reusable) from camera; dig = sphere r=3 voxels → cells grouped per brick → `World::applyEdit` per brick (edit-log authority path §2.4); place = single voxel on hit face. Dirty render chunks (incl. neighbors when the edit touches a chunk-border apron) re-mesh game-thread, budgeted. |
| Pawn/input | Spectator fly pawn + legacy input bindings (Enhanced Input assets deferred — dev tooling, not shipping input). |

## Stages

1. **Voxels on screen** — subsystem + chunk component + scene proxy +
   master material + fixed-radius synchronous generation; editor map with
   directional light + sky; builds clean; screenshot proof.
2. **Streaming + dig/place** — async gen/mesh workers feeding a game-thread
   apply queue with per-frame budgets + hysteresis (§2.5); ring follows
   pawn; dig/place via DDA raycast → `World::applyEdit` → re-mesh dirty
   render chunks (edit-log authority path, §2.4); spectator pawn.
3. **Walkable + LWC** — DDA box-sweep character collision (plan §3.3: no
   Chaos for terrain), origin rebasing, material polish, perf pass vs the
   60fps gate on this machine + min-spec proxy settings.

## Verification per stage

Stage 1: `Build.bat VoxelEarthEditor` clean; PIE via mcp-unreal (after
server reconnect); viewport screenshot showing terrain surface with AO;
`LogVoxelEarth` reports generated brick/chunk/quad counts matching a
`vxc_bench`-style digest run at the same seed/radius.
