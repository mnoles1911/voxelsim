# Streaming environmental assets larger than a render chunk

Date: 2026-08-10
Status: design + voxel-core foundation built and tested; UE wiring specified, not written
Prior art: `docs/tree-asset-generator-plan.md`, `docs/tree-asset-generator-research.md` §8

This session had no access to the Unreal editor (another session held it). Everything
below is static analysis, C++ that compiles, and headless tests. Every claim about
current behaviour carries a `file:line`. The section "What remains unverified" says
plainly what a later session has to check in the editor.

---

## 1. The machinery, as it actually is

### 1.1 The world is a heightfield function plus an edit overlay

`voxel(x,y,z) = f(seed, x, y, z)` patched by replayed diffs (`voxel-core/include/voxelcore/world.h:2-5`).
`f` is column-based and this is the single most important structural fact for assets:

- `Amplifier::column(vx, vy) -> ColumnSample` (`amplifier.h:307`)
- `static MaterialId Amplifier::materialAt(const ColumnSample&, int64_t vz)` (`amplifier.h:442`)

The per-voxel rule is a **static function of (column, z)**. Anything genuinely 3D has to
ride on the `ColumnSample` — the header says so directly, and `cave`, `cavern` and the
now-deleted `d3` all did exactly that (`amplifier.h:76-101`). A baked tree cannot: it is
arbitrary 3D data, not a reduction of a column.

The only other source of voxels is the edit overlay, `ChunkMap<8> overlay_`
(`world.h:137`), reached through `World::applyEdit` (`world.h:63`).

### 1.2 Chunk lifecycle

- **Key**: `FVoxelLevelChunkKey { int32 Level; FVoxelChunkKey{X,Y,Z} }` (`VoxelCoords.h:130`).
  Residency map is `TMap<FVoxelLevelChunkKey, FChunkRecord> ChunkRecords`
  (`VoxelWorldSubsystem.cpp:2748`). A chunk is 32 voxels = **3.2 m** at level 0
  (`VoxelCoords.h:27-28`), doubling per mip level across 6 levels (`VoxelCoords.h:50`).
- **Admission**: `RecomputeDesiredSet` (`VoxelWorldSubsystem.cpp:7081`), per-level
  annuli `{0,128} {128,256} … {2048,4096}` metres (`VoxelWorldSubsystem.h:133-140`),
  then `AddCandidate` (`:7524`) applies resurrection, parked-geometry adoption,
  a per-level admission budget, a distance cutoff, and the fine-tile residency gate.
- **Vertical extent** comes from `FootprintChunkZRangeCached` → `ComputeFootprintChunkZRange`
  (`:6624`). This is where assets die; see §2.
- **Generation**: worker task at `:9471`; at level 0 it builds a 34×34 grid of
  `ColumnSample` and meshes through `GridSampler`, which is literally
  `vxc::Amplifier::materialAt(Columns[…], Z)` (`:9665-9671`).
- **Meshing**: `MeshChunkBricks` (`VoxelChunkMesher.h:70`) over 4×4×4 bricks, each via
  `vxc::meshBrick` with a **1-voxel apron** on every axis (`mesher.h:39`).
- **Eviction**: exit scan in `RecomputeDesiredSet` (`:7192-7279`), purely radius-based
  with 1.25× hysteresis on the outer edge only; `DrainUnloads` (`:11672`).
- **Edits**: everything funnels through `ApplyGroupedEdits` (`:12214`);
  `StampVoxels(TArray<FVoxelCoord>, uint8 Material)` (`:12245`) is a ready-made
  multi-chunk stamp, used only by the test fixtures.
- **GPU**: GPU meshing is default ON for the whole cascade (`:9417-9431`), and the GPU
  **generates its own voxels** — `VoxelizeMain` in `voxel-core/shaders/worldgen.ush`
  mirrors `Amplifier::materialAt` / `makeBrick` (`worldgen.ush:20-23`).

### 1.3 The precedent that matters

The fine-tile streamer is the closest existing thing to "data of a different extent than
a chunk feeding chunk generation", and its shape is the one to copy:

- The tile sampler is injected at `World` construction — one line,
  `VoxelWorldSubsystem.cpp:2619`.
- Tile residency is **independent** of chunk residency; one tile covers ~4800² chunks and
  there is no tile→chunk map. The many-to-many is handled forward, per chunk candidate,
  at admission time (`:7664-7729`).
- The gate logic lives in voxel-core as pure functions
  (`tilestreaming.h:173-217`, `tilesCoveringFootprint` / `missingTilesForFootprint`)
  specifically so it can be tested in **both** directions. The header records why:
  "A gate whose decision lives in host code that needs a UE editor, a baked 200 MB tile
  and a flight leg to observe is a gate that will only ever be tested in the easy
  direction."

That last sentence is why the foundation built this session is in voxel-core.

---

## 2. Where an over-chunk asset breaks

Four blockers, in the order they bite. Only the first is widely known.

### B1 — The sky-band trim never requests the chunks (known; §8B of the research doc)

`ComputeFootprintChunkZRange` (`VoxelWorldSubsystem.cpp:6624`) derives a footprint's
admitted Z range from **terrain surface alone**: four corner columns, `-1` chunk below,
`+2` chunks above (`:6680`), then clamped tighter still by the analytic sky-band trim
(`:6704-6716`) using `Amplifier::surfaceUpperBoundMm`.

At level 0 that headroom is 2 × 3.2 m = **6.4 m** above the corner-sampled maximum. A 28 m
emergent, a 12 m pine, and a 9 m boulder stack all have crowns in chunks that are never
requested. `IsChunkProvablyAllAir` (`:6545`) independently proves the same chunks empty.

The soundness argument for the bound is quoted at `amplifier.h:334-339`: "materialAt is
unconditionally MAT_AIR above the surface … no pass in the amplifier can turn air into
solid." An asset falsifies that premise outright.

### B2 — Even in an admitted chunk, the brick skip deletes the asset

This one is not in the prior write-up and it means "just raise the bound" is not enough.

The level-0 worker's `SkipBrick` predicate (`VoxelWorldSubsystem.cpp:9690-9742`) skips any
brick whose interior z-min is above the column's top solid voxel (`:9709`), and the
sampler it would have used, `GridSampler` (`:9665-9671`), is a pure column function that
*cannot return an asset voxel anyway*. `GeneratedWorld::surfaceBrickRange`
(`generator.h:80-90`) does the same reduction on the voxel-core side.

So B1 and B2 must be fixed together. Fixing only B1 admits chunks that then mesh to
nothing, which reads as "the bound fix didn't work".

### B3 — The edit-overlay route, which is how the current fixture works, does not scale

The hand-authored tree (`:14925`) works because `StampVoxels` writes to the **edit
overlay**, and `docs/status.md:853-855` explains it survives only because it "sits inside
R0 + the desired-set's +2-chunk Z headroom" plus the edit escape hatch
`EditedFootprintMaxZ` (`:7467-7473`). Scaling that to real vegetation fails four ways:

1. Every instance is appended to the **edit log** (`world.h:63`) and persisted
   (`SaveEditLogToDisk`, `:13520`) — a forest becomes a save file.
2. Overlay bricks are **never evicted**; `World` has no overlay eviction at all.
3. Every asset-bearing chunk trips `NeedsOverlayAwarePath` and is routed to the
   **game thread** at `voxel.Stream.MaxRemeshesPerFrame` = 4 chunks/frame
   (`:11614-11624`). A forest puts the visible world on the game thread.
4. It is replicated to clients (`BroadcastNewEntries`, `:13321`).

### B4 — GPU worldgen would not produce assets

GPU meshing is default ON for all six levels (`:9417-9431`) and generates voxels itself
(`worldgen.ush:20-23`), gated by digest parity (`vxc_gpu`) and the `shader-ub-lint` CI
job. Anything added to `f` must be mirrored in HLSL or GPU-generated chunks silently
have no vegetation.

### Not blockers (checked, because they were plausible)

- **`FVoxelChunkQuad`'s uint8 fields** (`VoxelMeshTypes.h:19-28`). `Slice/U0/V0` are
  chunk-local 0..31 and `W/H` are brick-local 1..8. Under any design where each chunk
  generates its own slice of an asset, quads are chunk-local by construction and nothing
  can overflow. The header already asserts this ("nothing here can overflow for a
  32-voxel chunk", `:41`).
- **Per-chunk culling bounds**. `CalcBounds` returns exactly the chunk cube scaled by
  level (`VoxelChunkComponent.cpp:1242-1250`). Fine — but it is a hard constraint that
  geometry must never leave its chunk box, which per-chunk generation satisfies and a
  "one component owns the whole asset" design would violate.
- **Watertightness across a chunk seam**. `meshBrick` reads a 1-voxel apron on every axis
  (`mesher.h:39`). If the apron sampler answers with the neighbour's asset voxels — which
  it does automatically when the asset is a function of position — the seam closes exactly
  as terrain seams do. No new machinery.
- **Eviction splitting an asset**. Real under a stamp design; a non-issue when there is
  no per-asset residency to split.

---

## 3. Options

| | Approach | Verdict |
|---|---|---|
| A | Stamp into terrain voxels via the edit overlay | **No.** B3: save-file growth, no eviction, game-thread meshing at 4 chunks/frame, replication. |
| B | Separate instanced object layer (meshes/ISM) | **No.** Contradicts the owner's decision — "Trees are real voxels in the world, destructible and diggable. Not placed meshes" (research doc §7) — and needs a mesh pipeline that does not exist. |
| C | Split each asset into per-chunk pieces at bake time | **No.** Keys asset data to the level-0 chunk lattice, so levels 1-5 get nothing and each level needs its own re-slice; bakes world position into the file, so the same tree at two sites becomes two files. |
| D | Per-asset residency keyed to the asset | **No.** Adds a residency system the streaming layer has no pin mechanism for (there is no per-chunk pin, refcount or keep-alive anywhere — searched), and reintroduces "evict half an asset" as a problem to solve. |
| **E** | **Assets as a third term in the world function, sampled per chunk** | **Recommended.** |

### E, stated precisely

An asset is not an object the streaming layer holds. It is a **function of position**,
exactly like terrain:

1. A **species bank** — decoded `(species, seed) -> voxels` — is resident process-wide.
   It is small and bounded: 59 species × 64 seeds, and the runs are ~3-5% of dense size.
   Residency is per **bank**, never per instance and never per chunk.
2. A **deterministic placement index** answers, from hashes alone, "which instances
   overlap this box" and "how high can anything here reach".
3. Chunk generation samples both. Every chunk computes its own slice independently.

What this buys, directly against the blockers:

- The chunk-size limit **dissolves**. A 28 m tree is not streamed; each of the ~700 chunks
  it touches independently asks what is inside itself.
- Eviction can never split an asset, because there is nothing per-asset to evict.
- Chunk seams are watertight for free — the mesher's apron reads the neighbour's asset
  voxels through the same pure function.
- Nothing enters the edit log, so digging a tree still works and is still a diff.

The costs, stated honestly:

- **Assets should be level-0 only for v1.** Levels 1-5 are 6.4 m-102.4 m cells; a 10 cm
  branch is invisible there and the LOD material-majority reduction (`mips.h:60-77`)
  would delete it anyway — risk 2 in the plan doc. This means **trees stop existing
  beyond 128 m**, which is a visible design consequence and the owner's call to make,
  not mine. The alternative is a coarse per-species impostor baked at levels 1-2, which
  is real work and should be its own phase.
- **Level-0 asset-bearing chunks must route to the CPU worker**, exactly as
  `NeedsOverlayAwarePath` already routes edited chunks off the GPU path (`:9202`). This
  is what avoids the HLSL port (B4) entirely for v1. The placement index makes "does this
  chunk contain an asset" a cheap pure query, so the routing test is affordable.

---

## 4. What was built this session

All in `voxel-core`, all covered by `vxc_tests`, all integer-only.

### `voxelcore/assetgrid.h` + `src/assetgrid.cpp` — the VXA1 reader

Random-access reader for asset-forge's baked format, with a per-column index so
`at(x,y,z)` costs a short walk rather than a scan of the whole file. Dense decode was
rejected in the header comment with the arithmetic: a 28 m asset at 10 cm is 280³ = 22 M
cells, ~1.4 GB for a 64-seed bank.

Out-of-range reads answer `MAT_AIR` deliberately — the mesher's apron reads outside the
asset box on every load, and a clamp would weld the asset to its neighbour.

### `voxelcore/assetplacement.h` — the scatter and, mostly, the bound

Four independent scatter lattices, one per size class. The bound
`assetTopAboveSurfaceMm` is a function of the layer table alone, and
`assetAwareSurfaceUpperBoundMm` composes it with the terrain bound so it cannot be
composed wrongly.

Two design decisions worth surfacing:

**The bound is per-rect, not a constant.** The water marker — the only existing
solid-above-surface thing — widens `surfaceUpperBoundMm` by a flat
`kWaterMarkerHeightMm` (`amplifier.cpp:2087-2113`) because a per-rect water query would
cost a block decode. A flat widening sized for a 28 m emergent is ~9 extra level-0 chunk
layers over **every footprint on the planet**, to carry trees that stand on a fraction of
it. A per-rect answer is affordable here only because the query is pure hash — no tile
decode, no I/O, no column. Everything in the file is arranged to keep that true,
including the early-out: a layer's contribution is its height or nothing, so the scan
stops at the first site found, and the dense layers (most cells) answer first.

**The veto-only rule.** Placement must depend on biome, but evaluating biome means
evaluating a column — the exact cost this bound may not pay (`amplifier.h:428-431`). The
resolution is a rule on the policy: *a policy may only veto a site; it may not create
one, move one, or substitute a taller asset than its layer permits.* Under that rule the
bound never needs to know what the policy decided, because vetoing can only lower the
true maximum. Biome rules can then change freely — which they will, many times — without
any risk of invalidating a bound whose failure is silent.

### `kSurfaceBoundDeclined` / `kSurfaceLowerBoundDeclined` moved to `core.h`

Values and meanings unchanged; `amplifier.h` keeps a pointer. They moved because they
stopped being the amplifier's private vocabulary: any bound that composes with the
surface bound must speak the same decline convention, and making that module include
`amplifier.h` — caves, caverns, biome, climate and all — to learn one integer is the
wrong dependency edge.

### Tests — 20 new, in `test_assetgrid.cpp` and `test_assetplacement.cpp`

Reader tests run against **real asset-forge output** (two `.vxa` files copied into
`tests/fixtures/`), with extents, origins, run counts, solid counts and probe cells taken
from the Python side. A round trip against an encoder in the same file would prove only
that two copies of my own understanding agree — the closed loop the research doc §9 calls
out. The packed-vs-aligned numpy record stride is the specific thing this catches.

Bound tests drive the hard direction deliberately: rects with no assets near them, a site
anchored outside a rect that reaches into it, a terrain bound that declines, degenerate
inputs, and an adversarial sweep that places every instance on a **sloping** synthetic
surface and checks no voxel escapes the bound (a flat surface would let the dilation be
deleted and the test still pass). The sweep asserts it found >100 instances — the
vacuous-truth guard the fine-tier gate went without.

### Verification run

- 619/619 tests pass, exit 0.
- Worldgen digest `e02458de2be47309` **unchanged** before and after
  (`vxc_bench --radius 8 --digest`).
- Clean under MSVC `/W4 /WX` and clang `-Wall -Wextra -Wconversion -Werror`.
- Float-ban lint clean for all new files.

---

## 5. Two findings that block wiring this up, independent of design

### 5.1 Every baked asset uses materials the engine does not have

Pinned by `assetgrid_real_assets_use_materials_the_engine_does_not_have_yet`. The tundra
pine is built from 16 (bark), 17 (heartwood), 20 (needle); the daisy from 8 (grass) and
24 (blossom). `vxc::kMaterialCount` is **16** (`core.h:334`). asset-forge says so itself
— `forge/materials.py`: "SLOTS 16+ ARE PROPOSED AND DO NOT EXIST IN THE ENGINE YET".

`MaterialId` is `uint8_t` so nothing faults; what happens instead is that every
material-indexed array reads out of range — `kMineCostByMaterial[vxc::kMaterialCount]`
in `VoxelAgentSubsystem.cpp:64` first — and the mesher happily emits quads for the
unknown id. It renders as *something*, which is the worst outcome, because the missing
append is never noticed.

The append has three known tails, already written up in
`docs/tree-asset-generator-research.md` §8A: mine costs, the `mips.h:60,77` majority
arrays, and `test_mesher.cpp:123-132`'s golden-digest trap. **Read that last one before
appending** — a material append silently changed the golden digests once already.

### 5.2 Resolution: assets are authored at 5 cm, the world is 10 cm

All 59 specs carry `resolution_cm: 5` (checked). `forge/grid.py:29` still says
`VOXEL_M = 0.10 # matches vxc::kVoxelSizeMm = 100`, so the default and the specs
disagree — worth reconciling in asset-forge. The fixtures confirm 5 cm: the tundra pine
is 86 voxels tall, which is 4.3 m at 5 cm (plausible) and 8.6 m at 10 cm (not).

**This needs a bake-step change**, and it is the one place the on-disk format's *content*
has to change: the engine needs a 10 cm variant. The reduction rule matters more than it
looks. Majority vote over each 2³ group will delete thin branches — exactly risk 2 in the
plan doc. **Recommend "any-solid" occupancy** (a world voxel is solid if any of its 8
sub-voxels is), with the material taken from the most common non-air sub-voxel. That
preserves connectivity, which is the property the rasterizer already goes out of its way
to enforce (`tree-asset-generator-plan.md:70-73`), at the cost of fattening thin
branches. Fattening a branch is a look problem; severing one is a correctness problem.

The VXA1 *container* needs no change — origin, extent, RLE all carry over. Only the
lattice the bake writes at does.

---

## 6. What a later session has to do, and what needs the editor

Ordered. Items 1-2 are headless; 3 onward need the editor.

1. **Append the tree materials to `vxc::Material`** and its three tails (§5.1). Headless,
   but touches golden digests — expect to re-bless them deliberately, not silently.
2. **Add a 10 cm bake target to asset-forge** with the any-solid rule (§5.2), and wire
   `assetLayerAdmitsHeight` into the bake so an asset taller than its layer is refused at
   bake rather than discovered as a flat-topped tree.
3. **UE wiring** (all unverified, all needs the editor to see):
   - An `FVoxelAssetLibrary` holding decoded banks, injected like the tile sampler is at
     `VoxelWorldSubsystem.cpp:2619`.
   - `ComputeFootprintChunkZRange` (`:6624`) and `IsChunkProvablyAllAir` (`:6545`) to use
     `assetAwareSurfaceUpperBoundMm` instead of the raw terrain bound. **This is the
     change with a silent failure mode** — too tight is a hole in the world.
   - The level-0 worker's `GridSampler` (`:9665`) to consult the placement index and
     library after `Amplifier::materialAt`, and `SkipBrick` (`:9690`) to stop skipping
     bricks that an asset reaches into (B2).
   - Route asset-bearing level-0 chunks to the CPU worker, mirroring the
     `NeedsOverlayAwarePath` veto at `:9202`.
4. **Measure the streaming cost of the raised Z range before tuning anything else.** The
   plan doc flagged this as risk 1 and it is the right thing to be nervous about: the
   admission path is already the subject of several postmortems
   (`:7141-7157`, `:4295-4315`, `:4381-4395`).

### What remains unverified

Nothing in §4 has been run in the editor, because this session could not open it. In
particular: that the raised Z range does not cost unacceptable streaming throughput; that
an asset actually renders once the sampler is wired; and that the CPU-route veto does not
starve the GPU path at level 0. Those are all screenshot-and-measurement questions, and
per `docs/lessons.md` practice the captures are the owner's to judge, not mine.

### Honest scope note

The voxel-core half is built because it is the half whose failure is silent and whose
correctness can be proven headlessly. The UE half is specified rather than written
because it changes the admission path — the single most postmortem-dense area of this
codebase — and writing it blind, with no way to run it, on the path that decides whether
terrain generates at all, is not a trade worth making. That is the same reasoning
`VoxelFineTileStreamer.h:136-149` gives for leaving block-granular residency unbuilt, and
it applies here for the same reason.
