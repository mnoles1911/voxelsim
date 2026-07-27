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
| Character volume | 0.6 × 0.6 × 1.8 m box (6 × 6 × 18 voxels), actor at the box centre. Eye at 1.6 m. |
| Speeds | **Revised 2026-07-27** — an 8-tier speed dial on the mouse wheel replaces the single walk/sprint pair: 0.7 / 1.4 / 2.2 / 3.2 / **4.5** / 6.0 / 7.5 / 9.5 m/s (Creep · Walk · Stride · Trot · **Jog** · Run · Sprint · Mad dash). Jog (4.5) is the default and is the old walk speed exactly. Shift is a momentary override to the top tier and only engages while heading forward (dot ≥ 0.7), so the effective sprint is 9.5 m/s rather than the 7 m/s this row previously bound. Fly speed unchanged (30 m/s default). |
| Crouch | **New 2026-07-27** — hold `C`. Box shrinks to 1.2 m (12 voxels) from the top only, so the feet stay planted; eye drops to 1.0 m; speed clamps to `min(dial, 1.5 m/s)`. Releasing under a ceiling does nothing until there is room — the stand-up tests the 6-voxel slab it would newly occupy and retries every tick, so walking out from under a ledge stands you up on its own. While crouched, movement refuses to walk off a ledge into open air (per-axis, so sliding *along* an edge still works). |
| Jump | ~1.0m apex (10 voxels); gravity tuned accordingly. Air control ~30%. **Extended 2026-07-27** with coyote time (0.1 s), input buffering (0.15 s) and variable height (releasing early scales the rise by 0.45). |
| Dig sizes | 1³ / 2³ / 4³ grid-aligned cubes, centered on the hit voxel and biased into the surface. **Revised 2026-07-27:** the 1/2/3 keys are now the only size selector — the scroll wheel became the movement speed dial, which needs fluid one-handed control far more than a three-value selector does. On-screen size indicator unchanged. |
| Dig timing | Instant for M1 (creative feel). Hardness/timed mining deferred (revisit with survival systems; NPC dig costs in plan §3.6 are unaffected). |
| Place | Creative palette: cycle material (rock/soil/sand) with a key; placement uses the same size selector, grid-snapped against the hit face. |
| Explosives v1 | Thrown with arc (hold to aim further), simple bounce physics, 3s fuse; carves a ~3m-radius sphere via the edit-log path with hash-ragged falloff edges. Re-mesh stays budgeted. |
| Cameras | First person + over-the-shoulder third person: boom 2.5m back, 0.4m to the active shoulder, collision-aware pull-in (voxel DDA raycast — a `USpringArmComponent` probe would never hit anything, since terrain carries no Chaos collision). **Revised 2026-07-27:** `V` toggles FP/TP (`C` became crouch) and `Q` swaps shoulders; the boom now eases toward its target with exponential lag, applied *before* the pull-in so a lagging camera can never smooth itself into rock. First person gained a landing view punch, speed-scaled head bob, and an FOV kick across the top speed tiers. Dig/place still always traces camera-through-crosshair. |
| Character proxy | Blocky voxel-style body (~1.8m, box torso/head/limbs) with basic walk/idle bob — placeholder until M5 per-bone voxel bodies. Visible in TP, hidden in FP. **Revised 2026-07-27:** crouching squashes it vertically by exactly the collision box's half-extent ratio (2/3), so it keeps filling the collision volume in both stances; the limb-swing phase now comes from the movement component, the same phase the first-person head bob uses, so the visible footfalls and the camera dip cannot drift apart. |

### Movement architecture (2026-07-27)

The walk-mode mover was extracted out of `AVoxelEarthFlyPawn` into
`UVoxelCharacterMovementComponent`
(`ue-project/Source/VoxelEarth/VoxelCharacterMovement.{h,cpp}`). The pawn had
grown into two unrelated concerns welded together, and the mover has three
future consumers it could never serve from there: `AVoxelAgent` (NPCs walking
the same terrain), the M3 authoritative server, and any future player pawn.

The split is:

* **`UVoxelCharacterMovementComponent`** — the kinematic step. Axis-separated
  swept AABB against `UVoxelWorldSubsystem::IsSolidAtVoxel`, gravity, jump feel,
  crouch state, the speed dial, the step-up retry, the swim placeholder, and both
  unstreamed-terrain guards. Never ticks itself; the owning pawn calls
  `TickMovement` so there is one movement clock.
* **`AVoxelEarthFlyPawn`** — input bindings, fly mode, and cameras (including
  camera *feel*: the two height channels, head bob, landing punch, FOV kick).
  Every pre-existing public accessor stayed on the pawn as a thin forwarder, so
  `AVoxelEarthHUD` and `AVoxelEarthPlayerController` were unaffected by the move.
* **`VoxelMovementTuning.h`** — one constants header for every speed, height,
  accel and camera number, in the spirit of `VoxelCoords.h`. It exists because
  `VoxelProxyBody.h` used to carry its own copy of the reference walk speed with
  a comment admitting it mirrored the pawn's and had to be synced by hand.

Still deliberately **client presentation only**: the determinism boundary covers
world *derivation*, not player motion, so plain doubles are correct here and
nothing in these files feeds a digest.

### Key reference (current)

| Key | Action |
|---|---|
| `WASD` | Move |
| `Space` | Jump (walk) / fly up. Release early for a shorter hop |
| `LeftCtrl` | Fly down (walk mode ignores it — crouch is `C`) |
| `Shift` | Sprint to top tier, forward-only (walk) / 4× boost (fly) |
| `Mouse wheel` | Speed dial: gait tier (walk) or fly speed step |
| `C` | Crouch (hold) |
| `V` | First / third person |
| `Q` | Third-person shoulder swap |
| `G` | Walk / fly mode |
| `[` `]` | Fly speed step (keyboard alias for the wheel) |
| `LeftAlt` | Fly precision modifier |
| `1` `2` `3` | Dig size 1³ / 2³ / 4³ |
| `LMB` / `RMB` | Dig / place |
| `T` | Cycle placement material |
| `F` | Explosive charge / throw |
| `F1` / `F3` | Debug overlay / `voxel.Debug` mode |

## Verification per stage

Stage 1: `Build.bat VoxelEarthEditor` clean; PIE via the native UE 5.8
editor MCP (after
server reconnect); viewport screenshot showing terrain surface with AO;
`LogVoxelEarth` reports generated brick/chunk/quad counts matching a
`vxc_bench`-style digest run at the same seed/radius.
