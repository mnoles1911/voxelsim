# Detail-asset rendering (TASK #7): the invisible 85%, drawn

Date: 2026-08-17. Branch `claude/f6-interior-rim-injection`. Companion to
`docs/asset-placement-audit.md` (finding 3 / §5.3 is the gap this closes) and
`docs/asset-placement-design.md`. Code:
`ue-project/Source/VoxelEarth/VoxelDetailAssetSubsystem.{h,cpp}` (new), plus a
three-accessor hook in `VoxelWorldSubsystem.{h,cpp}` (marked `TASK #7` blocks),
two module deps in `VoxelEarth.Build.cs`, and
`ue-project/Tools/create_detail_asset_material.py` (new material script).

## What it does, in one paragraph

voxel-core already resolves every detail-lattice instance — grass, ferns,
flowers, reeds, small rocks; 85% of all placements, 266 species — through the
same deterministic gates as the trees, and until now nothing consumed the
answer: every UE call passed `terrainOnly=true`, and detail grids never enter
the world voxel lattice by design (they carry their own pitch, typically 5 cm).
`UVoxelDetailAssetSubsystem` walks a ring of 2×2 level-0 chunk groups
(6.4 m squares) around the streaming anchor, resolves each group's instances on
a **worker** via `AssetField::instancesForRect(rect, facts, terrainOnly=false)`,
converts each `(species, seed)` grid **once** into a small vertex-coloured
static mesh, and draws all instances of that grid through **one HISM component
per (species, seed)** with per-instance transform = anchor position +
`yawQuarter` × 90°.

## Explicitly excluded

**Animals** (fish / bird / quadruped / cetacean kinds) — owner decision: they
need animation first. The exclusion is structural, not a filter: those species
carry layer 255 (`kAssetLayerNotScattered`), never enter the folded species
table, and therefore cannot come out of `instancesForRect` at all. The worker
additionally skips any grid with rig parts (`hasParts()` — only animals carry
them), so the exclusion holds even against a mis-baked plant.

## Architecture

```
game thread (Tick)                      worker (UE::Tasks, ≤4 in flight)
─────────────────                       ────────────────────────────────
ring scan: 2×2-chunk groups within      instancesForRect(rect, facts, false)
112 m of the anchor (cmdline            facts = assetColumnFactsFromSample(
-VoxelDetailRingMeters=, 16–512)                Amplifier::column(ax, ay))
  ├─ residency gate: group rect +         ├─ keep detail-lattice layers only
  │  max layer reach must be              │  (terrain ones are already voxels)
  │  IsFootprintResident (a worker        ├─ keep instances ANCHORED in this
  │  column into a non-resident fine      │  group (reach dilation returns the
  │  tile is a fatal gate leak)           │  same instance to ≤4 neighbours —
  ├─ dispatch ≤8 groups/tick,             │  anchor ownership is the dedup)
  │  nearest first                        ├─ bankGrid(bankId, seedIndex) — the
  └─ drain results (MPSC queue)           │  library's own mutex; detail banks
       ├─ apply group: store              │  load on first touch here
       │  instances, append to built      └─ for (species,seed) unseen at
       │  HISMs                              dispatch: naive visible-face quads
       ├─ build ≤4 static meshes/tick        → positions/normals/colours
       │  (BuildFromMeshDescriptions,
       │  bFastBuild — the documented
       │  runtime path)
       ├─ rebuild ≤4 dirty HISMs/tick
       └─ release groups past ring×1.15
```

Key decisions, and why:

* **Worker resolve, never the render/game thread.** The resolve costs ~100
  amplifier columns per group (a level-0 mesh job pays ~1,150). The worker
  calls exactly what meshing workers already call concurrently
  (`Amplifier::column`, `bankGrid`); thread-safety rests on the fine-tier
  rules documented in `VoxelFineTileStreamer.h`, and legality on the residency
  gate above — dilated by the **full** layer-table reach because
  `terrainOnly=false` evaluates terrain-layer anchors too.
* **Naive visible-face quads, not the greedy mesher.** The grids are 100–5,000
  voxels, and the look doctrine (`materialpalette.h`) wants per-voxel colour
  jitter — which makes almost every greedy merge illegal anyway. Winding and
  corner order are copied from `FVoxelChunkSceneProxy`'s proven convention
  (the 2026-07-21 winding fix).
* **Colour is baked into vertex colours** from `vxc::kMaterialPalette` (the
  single appearance authority), face-classed top/side/bottom, jitter hashed
  from local voxel coords + (species,seed) — deterministic; instances of one
  seed are identical, exactly as terrain-lattice trees are. Colours travel as
  linear floats, the engine encodes the colour buffer as sRGB bytes
  (`ToFColor(true)`, verified in 5.8 source), the GPU reads raw UNORM, and
  **M_VoxelDetailAsset applies the Power(2.2) decode**. If that asset is
  missing, cover renders grey-with-correct-shapes plus a warning naming
  `Tools/create_detail_asset_material.py` — run it once (one editor per box).
* **Placement conventions.** Instance stands on the **top surface** of its
  verified-solid anchor voxel (`(anchorVz + 1) × 10 UU`) — the terrain-lattice
  convention (base slab shares the ground voxel) would bury a 5 cm tuft
  entirely inside the 10 cm ground cube. Yaw quarter-turns about +Z through
  the anchor reproduce `assetgrid.cpp`'s forward map (UE positive yaw maps
  +X→+Y, same as yaw-1's (u,v)→(−v,u)). The grid's own origin offset is baked
  into the mesh, so rotation about the anchor is exact.
* **Release = budgeted rebuild, not index bookkeeping.** HISM instance removal
  reshuffles ids; instead of mirroring engine internals, a released group
  marks its (species,seed) components dirty and a budgeted pass rebuilds each
  from the surviving groups (ClearInstances + bulk AddInstances). Group state
  is the single source of truth.
* **Float precision at planet coordinates.** Instance transforms are stored as
  doubles; each HISM sets its world origin near the anchor at creation and
  instances go into the buffer as small offsets, so the buffer's float
  precision is spent on centimetres, not on the 400 km to the world origin.
* **Determinism.** Same seed + tiles ⇒ same instances — voxel-core's
  guarantee; this subsystem only consumes. It puts nothing in the world
  lattice, adds no collision, and changes no worldgen digest, admission bound
  or streaming decision. Presentation only, multiplayer-safe by construction.

## Knobs

| Switch | Default | Meaning |
|---|---|---|
| `-VoxelNoDetailAssets` | off | kill switch; subsystem stays inert |
| `-VoxelDetailRingMeters=N` | 112 | resolve ring (clamped 16–512; default sits inside R0's 128 m so every group stands on admitted, resident ground) |
| `-VoxelDetailShadows` | off | let cover cast dynamic shadows |
| (fixed) | ring×1.15 | release hysteresis; per-instance HISM cull 85→100% of ring |
| (fixed) | 4 / 8 / 4 / 4 | jobs in flight / dispatches per tick / mesh builds per tick / HISM rebuilds per tick |

## Known v1 limits (deliberate)

* **No reaction to dig/place edits**: a dug-out column keeps its resolved
  cover until its group leaves the ring and re-resolves. (Terrain assets get
  this right through the edit overlay; cover accepts its ring's staleness.)
* **No hue-axis jitter** (lightness only) and no slope-conforming tilt —
  cover is axis-aligned like everything else in a voxel world.
* Cover on steep ground can slightly float at the downhill edge of its anchor
  voxel; at 5 cm pitch this is subvoxel and invisible past a metre.
* Detail **bushes** (any non-10 cm plant kind on a scatter layer) render via
  the same path; only layer-255 entities are out of scope.

## What a capture must show (main session runs it)

Site: **the alpine bank at (−39661, −57292)** — the audit's census ground:
30,303 sites, 20,664 of them L3, 92 species. Same seed (20260719), same
`-VoxelAssetDir` as the tree captures, plus (once)
`Tools/create_detail_asset_material.py` for real colours.

1. **Ground-level pose** (camera ~1.7 m up, looking slightly down across a
   gentle alpine slope): the ground between the trees must carry **flowers and
   ground cover** — discrete tufts/blossoms at ~5 cm voxel scale, in species
   clumps (grass clustering median 0.95), NOT a uniform carpet and NOT bare
   terrain between trunks. This is the before/after money shot: the before is
   any existing tree capture at the same pose, where the ground is bare.
2. **Close pose** (~3–5 m from a cluster): individual plants read as small
   voxel models standing ON the surface — not floating above it, not sunk to
   half height, not clipped by terrain. Yaw variety visible (same species,
   different facings).
3. **Log checks** (settle first — blank/blank-ish captures are unloaded
   terrain, not rendering bugs):
   * `VoxelDetailAssets: STARTED -- ring 112 m ...`
   * `VoxelDetailAssets: first group applied -- N detail instances ...` with
     N > 0 (the alpine census predicts ~50 instances per 6.4 m group).
   * The 30 s stats line with instances in the tens of thousands at settle
     (~20k per 128×128 m at this site) and **no** M_VoxelDetailAsset fallback
     warning (else colours are grey and the script needs running).
4. **Negative checks**: no animals anywhere (excluded); terrain trees/rocks
   unchanged (this path draws only detail-lattice grids); with
   `-VoxelNoDetailAssets` the frame must match the pre-task look exactly.

Judgement of the captures is the owner's; present them with conditions.
