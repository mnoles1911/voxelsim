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
3. **Walkable + LWC** — in order:
   a. **Streaming perf** (first — compounds everything): the worker mesh
      job samples `GeneratedWorld::materialAt`, which recomputes the full
      amplifier column per VOXEL query; measured ~5 chunks/s wall-clock
      (2026-07-19 run: 321 chunks / 63s while ~4300 queued). Fix = per-job
      extended column grid ((chunkEdge+2)² columns computed once, mesher
      apron sampler reads the grid), exactly like vxc_bench. Target ≥50
      chunks/s single measurement on this machine.
   b. DDA box-sweep character collision (plan §3.3: no Chaos for terrain),
      walk mode on the pawn.
   c. Origin rebasing (LWC) + material polish.
   d. Perf pass vs the 60fps gate (min-spec proxy settings).

## Player experience decisions (Matt, 2026-07-19 — binding for M1 polish)

| Topic | Decision |
|---|---|
| Slope feel | Auto-step absorbs ≤3 voxels (30cm) silently; camera Z is smoothed (spring toward target) so steps read as ramps. Taller rises need a jump. |
| Speeds | Walk 4.5 m/s, sprint 7 m/s (Shift). Fly speed unchanged (30 m/s). |
| Jump | ~1.0m apex (10 voxels); gravity tuned accordingly. Air control ~30%. |
| Dig sizes | 1³ / 2³ / 4³ grid-aligned cubes, centered on the hit voxel and biased into the surface. Scroll wheel cycles size (1/2/3 keys as shortcuts); on-screen size indicator. Replaces the r=3 sphere dig. |
| Dig timing | Instant for M1 (creative feel). Hardness/timed mining deferred (revisit with survival systems; NPC dig costs in plan §3.6 are unaffected). |
| Place | Creative palette: cycle material (rock/soil/sand) with a key; placement uses the same size selector, grid-snapped against the hit face. |
| Explosives v1 | Thrown with arc (hold to aim further), simple bounce physics, 3s fuse; carves a ~3m-radius sphere via the edit-log path with hash-ragged falloff edges. Re-mesh stays budgeted. |
| Cameras | First person + over-the-shoulder third person: boom 2.5m back, 0.4m right, collision-aware pull-in. `C` toggles. Dig/place always traces camera-through-crosshair. |
| Character proxy | Blocky voxel-style body (~1.8m, box torso/head/limbs) with basic walk/idle bob — placeholder until M5 per-bone voxel bodies. Visible in TP, hidden in FP. |

## Verification per stage

Stage 1: `Build.bat VoxelEarthEditor` clean; PIE via the native UE 5.8
editor MCP (after
server reconnect); viewport screenshot showing terrain surface with AO;
`LogVoxelEarth` reports generated brick/chunk/quad counts matching a
`vxc_bench`-style digest run at the same seed/radius.
