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
| Ring transitions | **v1 landed** (see "Ring transitions v1" below): v0's hard boundary is upgraded to a dithered cross-fade, pure material change (`UVoxelChunkComponent`/`M_VoxelTerrain`), no subsystem/streaming edit, no cvar (the fade is always-on; its own "no fade" inert defaults are how it stays inert on any material instance that never gets per-level params set). Level 0 has no inner fade (nothing coarser to fade in from); the outermost ring (R4) keeps no outer fade (matches the plan's "hard boundary v0" staying at the clipmap seam until that gets its own polish pass). |
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

## Ring transitions v1 (dithered cross-fade, implemented)

Upgrades the "Ring transitions" row's v0 hard boundary
(`docs/voxel-earth-implementation-plan.md` §3.3: "dithered cross-fade band
(outer 15-20% of each ring; blue-noise threshold; both LODs rendered in
band; TSR resolves)"). Pure material + component change — `VoxelWorldSubsystem.*`
untouched (owned by a parallel M2 agent this pass; the file-ownership split
required reading `RingPresets` read-only for the fade-table math below, never
writing it).

**Material graph** (`Tools/create_voxel_material.py`, regenerated
`M_VoxelTerrain.uasset` headlessly via `UnrealEditor-Cmd.exe -run=pythonscript`).
Blend mode switches Opaque → Masked; four new `ScalarParameter`s
(`RingInnerFadeStart/End`, `RingOuterFadeStart/End`, UU) drive:

```
CameraDistance = Distance(AbsoluteWorldPosition, CameraPositionWS)   -- LWC-safe: both
                                                                         operands are engine LWC
                                                                         expressions, so the
                                                                         subtraction happens in
                                                                         emulated double precision
                                                                         before the (small,
                                                                         camera-relative) result
                                                                         downcasts to float
InnerRamp   = saturate((CameraDistance - InnerStart) / (InnerEnd - InnerStart))   -- 0 -> 1 fade IN
OuterRamp   = saturate((OuterEnd - CameraDistance) / (OuterEnd - OuterStart))     -- 1 -> 0 fade OUT
FadeFactor  = InnerRamp * OuterRamp
OpacityMask = DitherTemporalAA(FadeFactor)   -- Engine's
    /Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA,
    the same blue-noise-esque screen-door dither TAA/TSR resolves over
    several frames that foliage/grass LOD crossfades use
```

Inert sentinel Start/End pairs (`-2/-1` for "inner fade already fully up",
`1e7/2e7` UU for "outer fade hasn't started") are both the material's own
base defaults AND what the component sets for level 0's inner side / R4's
outer side — same formula path, no shader branch, "material params ...
default = no fade, fully opaque" holds by construction. Verified headlessly
(load the regenerated asset + `MaterialEditingLibrary` introspection, not
just "the script didn't throw"): `BlendMode=BLEND_MASKED`, the four defaults
above, 23 expressions, `OpacityMask` wired to the `DitherTemporalAA` call
node — not left floating. Existing vertex-color/AO/DebugTint → BaseColor path
and the Roughness constant are untouched; Masked with `OpacityMask==1`
(the inert default) draws every pixel exactly like Opaque did, so the asset
change alone is inert on any material instance that never gets per-level
params set.

**Component** (`VoxelChunkComponent.h/.cpp`). `SetLevel`/`SetMaterial` both
funnel into a new `ApplyRingFadeParams()` that lazily creates (and thereafter
reuses) a single `ChunkMID` — merged with the P1 chunk-state `DebugTint` MID
per the task's "one MID per component maximum" constraint, since fades are
always-on and therefore need a MID unconditionally (SetDebugTint/ClearDebugTint
now just set/reset a parameter on that same instance instead of creating and
tearing down a second one; "zero-cost when off" now means "no extra work",
not "no MID" — there's only ever one). `ApplyRingFadeParams` no-ops until both
`SetLevel` and `SetMaterial` have run at least once (order isn't fixed —
`SetLevel` currently runs first per the `NewObject`/`RegisterComponent` call
site in `VoxelWorldSubsystem.cpp`, but the code doesn't assume that).

**Fade table** (`ComputeRingFadeParams`, derived from `RingPresets`, read-only
reference — single source of truth for the ring radii, no hardcoded second
copy). Band = 15% of that level's own annulus width (`Outer - Inner`):

| Level | Annulus (m) | Band (m) | Inner fade (m) | Outer fade (m) |
|---|---|---|---|---|
| R0 | [0, 64) | 9.6 | *none (level 0)* | [54.4, 64] |
| R1 | [64, 128) | 9.6 | [64, 73.6] | [118.4, 128] |
| R2 | [128, 256) | 19.2 | [128, 147.2] | [236.8, 256] |
| R3 | [256, 512) | 38.4 | [256, 294.4] | [473.6, 512] |
| R4 | [512, 1024) | 76.8 | [512, 588.8] | *none (outermost)* |

**Overlap finding (task item 3).** Confirmed both levels ARE loaded/rendered
together around every boundary, comfortably covering each side's own fade
band: `FVoxelWorldImpl::RecomputeDesiredSet`'s outer-edge-only hysteresis
(`UnloadRingMultiplier = 1.25`) keeps a level-L chunk loaded until
`Outer(L)*1.25`, while level-(L+1) starts loading immediately at
`Inner(L+1) == Outer(L)` (no inner-edge hysteresis) — so both are resident
across `[Outer(L), Outer(L)*1.25)`: 16m at the R0/R1 boundary, 32m at R1/R2,
64m at R2/R3, 128m at R3/R4, each wider than either side's own fade band
(9.6-76.8m) before per-chunk quantization is even counted (a level-(L+1)
chunk's footprint is `(1<<(L+1))`× a level-0 chunk's 3.2m edge, so its center
can already be well past `Inner(L+1)` — i.e. its near edge well inside the
annulus — the first tick it's created). This matches the wave-1 code comment
("both rendered in the overlap band of 1 chunk") — no limitation to report
on loaded-chunk overlap itself.

The actual limitation is about the FADE BANDS' placement, not the loaded-chunk
overlap: because level L fades out over the tail of its OWN annulus and level
L+1 fades in over the head of ITS OWN annulus, the two bands sit on opposite
sides of the shared boundary point (e.g. R0's outer band ends at exactly 64m,
R1's inner band starts at exactly 64m) rather than spanning the same distance
range. There is therefore no moment where both levels are simultaneously at a
partial opacity blending together like a true additive crossfade — instead
the inner ring dithers down to fully transparent right at the boundary and
the outer ring dithers up from fully transparent starting there, i.e.
fade-to-nothing-then-fade-in rather than a blended dissolve. This is an
inherent consequence of "each level fades over its own 15% band" (the plan's
literal wording) done as a pure material change with no subsystem/annulus
edits, and matches the task's own anticipated fallback ("it will read as a
fade-to-nothing which is still better than a hard pop") — a real improvement
over v0's instant pop either way, just not a full symmetric blend. A future
pass that wants a true blend would need the subsystem to widen the desired-set
overlap band explicitly to a shared distance range and hand both levels the
SAME start/end pair at a boundary — out of scope for this pure-material pass
(and the file-ownership split for this task).

**Verification.** `-VoxelCameraHigh=300 -VoxelScreenshotAfter=50 -VoxelDebugRings`
and the same without `-VoxelDebugRings` (screenshot paths in the M2 polish
report). `-VoxelPerfRun=30`: p50=4.10ms / p95=18.33ms (max=400.00ms,
15 hitches, 9549 chunks loaded, 62.8% budget saturation) — within noise of
the pre-fade baseline (~4.3/18.5 p50/p95), no measurable Masked-material cost
at this ring/chunk count.
