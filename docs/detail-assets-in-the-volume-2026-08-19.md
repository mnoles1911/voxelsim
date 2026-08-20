# Phase 6 — detail assets in the voxel volume

Date: 2026-08-19. Companion to `docs/ray-marching-plan-2026-08-19.md` §9 (assets)
and §4 (memory), which this document **corrects in two places**. Scope approved
by the coordinator on the order in §8.

Territory: `voxel-core` (assetfield/assetgrid), `voxel-core/bench/volumeprobe.cpp`,
`asset-forge/tools/lattice_ab.py`, `VoxelDetailAssetSubsystem.cpp`,
`ue-project/Shaders/VoxelAssetStamp.usf`. **Not** `VoxelWorldSubsystem.cpp` and
**not** the brick pool — both are live under another workstream.

---

## 1. Two corrections to what has been briefed

### 1.1 The palette unblock has already shipped

This phase has been justified on `docs/backlog.md` §8's premise — *"forty-two
species across six kinds are authored … and none of them is in the world,"*
blocked on palette wiring into the vertex factory. **That premise is retracted in
the backlog itself.** `docs/backlog.md:1004-1011` carries a review banner dated
2026-08-19:

> **LARGELY SUPERSEDED BY §10 — reviewed 2026-08-19. Read §10 first.** The
> premise here ("forty-two species, none of them in the world") is two programmes
> out of date: **828 species** are authored, the engine composes them into terrain
> chunks on both the CPU and GPU paths …

and `docs/backlog.md:1019-1023` records **"The shader palette — DONE."** The
wiring is in code, not only in prose:

* `ue-project/Shaders/VoxelQuadVertexFactory.ush:901-905` — `VoxelPaletteUnpack`
  → `TexCoords[3] = (R,G)`, `TexCoords[4] = (B, isAsset)`.
* `ue-project/Tools/create_voxel_material.py:155-160` —
  `BaseColor = lerp(biomeAlbedo, paletteRGB, isAsset)`.
* `ue-project/Content/Voxel/M_VoxelTerrain.uasset` (Aug 12) postdates the script
  (Aug 11), and its name table contains `MaterialExpressionTextureCoordinate`
  and `MaterialExpressionLinearInterpolate`.

**What the marcher actually buys on colour, stated narrowly enough to be true.**
`create_voxel_material.py:160`: `isAsset` is 1 **only for the ten materials at or
above `MAT_BARK`** (bark, heartwood, deadwood, six leaf types, pale bark). Every
terrain voxel stays on the climate/biome path, and a greedy quad still carries
**one** material across up to 8×8 voxels. A marcher makes flat per-voxel colour
**universal** and deletes the interpolant plumbing entirely — no `TexCoords[3]/[4]`,
no `VertexColor` budget, no generated material graph, no editor-bound commandlet.

**That is a real simplification. It is not an unblock, and this phase must not be
sold as one.**

*Open owner check, and it is cheap:* the material-graph regeneration is
commandlet-driven, which is the classic silent no-op. The mtime and name-table
evidence above is consistent with it having run but does not prove it. **Bark on a
tree should read palette-brown rather than climate-tinted.** That is a screenshot
question; it goes to the owner with conditions and no verdict.

### 1.2 "Already voxels, then discarded" is wrong — in our favour

`docs/ray-marching-plan-2026-08-19.md:969-971` states that `VoxelAssetStamp.usf`
stamps instances into `Cells` and *"today they are discarded with the transient."*
**`Out.Cells` is not discarded before the brick chain.** In one `FRDGBuilder`, in
this order:

| pass | file:line |
|---|---|
| `VoxelizeMain` → writes `Out.Cells` | `VoxelGpuWorldGen.cpp:1127-1139` |
| `AssetStampMain` / `AssetStampCoarseMain` → `Params->OutCells = Out.Cells` | `:1190`, `:1261` |
| `BrickClassifyMain` → `Params->InCells = CreateSRV(Out.Cells)` | **`:1367`** |
| `BrickPackMain` → `Params->InCells = CreateSRV(Out.Cells)` | **`:1389`** |
| mesh chain (quads) | `:1435+` |

**Terrain-lattice assets are already in the brick volume. There is nothing to
build for them.** Two conditions on that statement, both load-bearing:

* `voxel.GPU.BrickPack` defaults to **0** (`VoxelGpuMeshJobManager.cpp:45-52`).
* The CPU packer is the dominant producer (82,653 of 87,805 chunks), and it
  composes through `materialAtResolved` — also terrain-lattice only. Both paths
  agree, which is why the byte proof held.

**The half that is genuinely missing is gated three times, deliberately, and no
gate is in the shader:**

1. `VoxelWorldSubsystem.cpp:10261` — the resolve filling `Req.AssetInstances`
   passes `/*terrainOnly*/ true`.
2. `voxel-core/include/voxelcore/assetfield.h:297-299` — `resolveForCompose`
   drops any layer with `!terrainLattice` and any grid with `!onTerrainLattice()`.
3. `voxel-core/include/voxelcore/assetfield.h:346,354` — `materialAtResolved`
   returns `MAT_AIR` for them, described in-file as *"the last line of defence …
   checked twice deliberately."*

**Where persistence hooks in:** gates 1-3, plus the span-table build at
`VoxelWorldSubsystem.cpp:10285` (`GpuSpansForGrid`).
**`VoxelAssetStamp.usf` needs no change for pitch.** `AssetStampMain` maps baked
index → cell index 1:1 (`Rvx = AnchorRel.x + RotOriginX + Rx`), which is exactly
correct when a 5 cm grid stamps into a 5 cm volume. It needs pointing at a
different `OutCells`, nothing more.

---

## 2. The library, split by lattice — measured

Replayed over all 828 specs in `asset-forge/specs/`; agrees with
`asset-forge/README.md:33-41`.

* **Terrain lattice — 216 species** (tree 78, rock 102, bush 33, grass 3), all at
  `kVoxelSizeMm` = 100 mm. **Already in the volume.** No lattice change needed.
* **Detail lattice — 612**, of which **382 are animals** (bird 127, quadruped 131,
  fish 106, cetacean 18). Layer 255 `kAssetLayerNotScattered`, dropped by
  `assetSpeciesTableFromManifest`, structurally incapable of coming out of the
  resolver.
* **Actual Phase 6 scope: 230 species** — flower 87, grass 86, reed 33, bush 24.
  **223 at 5 cm; 7 bushes at 2 cm.** "Every one on the 5 cm lattice" is not quite
  true, and §7.3 covers the 7.

---

## 3. The lattice question, answered honestly

Two quantizations are conflated by the phrase "snapping to the world lattice".
They have opposite answers and must be priced separately.

**Position — costs nothing.** `VoxelDetailAssetSubsystem.cpp:542-544` places at
`anchorXMm * 0.1` (arbitrary millimetres in XY) and `(anchorVz + 1) * 10 UU` in Z.
**Z is already on the 10 cm lattice**, so it is exact at 5 cm with no rounding at
all. XY snapping to 5 cm moves an instance by ≤2.5 cm inside an L3 placement cell
of **800 mm** (`asset-forge/forge/manifest.py:591-593`) — 3% of the cell. The
de-gridding survives intact.

**Shape — this is the entire cost.** Measured box extents from the banks (§4):
grass 15×15×11 voxels (73×73×55 cm), flower 15×15×13 (75×76×64 cm). At 10 cm,
nearest-neighbour `coarseRep` keeps **one voxel in eight**: a grass tuft falls
from 257 solid voxels to ~32, a flower from 480 to ~60, and any blade one voxel
thick has a 50%-per-axis chance of disappearing entirely.

The forge already established this one step finer.
`asset-forge/tools/lattice_ab.py:24-26`: *"At 5 cm a blade is one voxel wide
whatever you ask for."* At 10 cm a blade is a 10 cm slab. **Moving the 230 detail
species to 10 cm is not degradation; it is a re-authoring project.**

**And the failure would be silent.** `asset-forge/README.md:38-41`:

> `tools/all_to_5cm.py` is reversed and must stay that way — nothing in voxel-core
> resamples, so a 5 cm rock read through `AssetGrid::at` comes out at twice its size.

There is no resampler. A 5 cm grid stamped into a 10 cm lattice does not degrade;
it **doubles in size**. Any 10 cm option must resample explicitly, and that
resampler does not exist today.

### 3.1 Does R0 need to be 5 cm? No — and this parts company with the plan

`docs/ray-marching-plan-2026-08-19.md:795-797` asserts *"the asset migration and
finer voxels are the same work item."* **They are not.**

R0-at-5cm delivers **5 cm terrain**, which by the probe's own admission
(`voxel-core/bench/volumeprobe.cpp:95-98`) is *"exact in z and SATURATED in xy"* —
2× vertical detail and literal horizontal duplication, because `Amplifier::column`
is addressed in integer level-0 voxels and has no sub-100 mm query. Cover gets a
home **as a side effect, while paying for the terrain.**

The requirement is *"cover in a volume with per-voxel palette colour"*, not *"one
volume"*. Invert the dependency — give cover its own scoped 5 cm volume — and the
cost falls by roughly **20×** (§5).

---

## 4. Measured input — the bank census

Parsed all **1,748 baked `.vxa` grids** in `asset-forge/out/engine/banks` directly
from the VXA1 run tables (48-byte header; 5-byte run records of `mat:u8, len:u32`;
format at `voxel-core/src/assetgrid.cpp:56-118`). Solid voxels = sum of run
lengths where `mat != MAT_AIR`. Reproducible without the engine.

| kind | pitch | grids | mean solid | median | mean box (cm) |
|---|---:|---:|---:|---:|---|
| flower | 50 mm | 348 | 480 | 170 | 75×76×64 |
| grass | 50 mm | 344 | 257 | 212 | 73×73×55 |
| reed | 50 mm | 129 | 978 | 612 | 137×134×246 |
| bush | 50 mm | 66 | 2,688 | 1,996 | 143×144×99 |
| bush | 20 mm | 28 | 2,612 | 1,722 | 106×108×100 |
| *(tree, reference)* | 100 mm | 308 | 132,058 | 60,628 | 974×968×1717 |

**Density is bounded, not estimated.** The L3 layer is `cell_mm=800`,
`density_per_mille=300` (`asset-forge/forge/manifest.py:591-593`) →
**≤0.469 instances/m², a hard ceiling.** The cross-biome census agrees
independently at 4,000-4,500 per hectare in the densest biomes
(`docs/biome-placement-survey.md:40-49`) = 0.40-0.45/m². Two independent sources,
same number.

---

## 5. VRAM — computed from measured density

Byte contract (`docs/brick-volume-format.md`, PROVEN): mixed brick =
`8 (descriptor) + 64 (occupancy) + 16 (palette, bpp ≤ 4) + ceil(solid × bpp / 8)`;
uniform brick collapses to 8 B. **Payload is per solid voxel, popcount-compacted** —
which is exactly why the earlier census over-predicted material by 2.3× (it priced
all 512 cells: `volumeprobe.cpp:701`). Sparse content is cheap under this contract.

**A 5 cm cover volume over the 112 m detail ring (224×224 m):**

* ≤23,500 instances × ~700 placement-weighted solid voxels = **≤16.5M solid voxels**
* ~350,000 distinct mixed bricks (40 cm bricks, ~25% areal coverage), ~47 solid
  per brick, 2 bpp
* ⇒ **16-35 MiB; call it under 50 MiB with margin**

Against the plan's **measured 1,110 MiB** for R0-at-5cm
(`docs/ray-marching-plan-2026-08-19.md:759`), and against today's 388 MiB commit /
167.6 MiB resident.

**~50 MiB scoped against 1,110 MiB coupled is not a difference in approach. It is
a different project.**

### 5.1 The falsifier, stated plainly

The two inputs — grid occupancy and instance density — are **measured**. The soft
term is the **brick-touch count** (how many distinct 40 cm bricks the cover
actually lands in, after overlap), which is **modelled, not measured**. That single
term is what §8 step 2 exists to measure.

**If `vxc_volumeprobe --detail-cover` returns ~500 MiB or more, this document is
wrong and `ray-marching-plan-2026-08-19.md:795-797` is right: the coupling holds,
and cover should ride R0-at-5cm rather than get its own volume.** The step tests
the claim rather than arguing it. Nothing downstream should be designed before
that number lands.

---

## 6. What retires from `VoxelDetailAssetSubsystem`, and what must not

**Retires** (all `VoxelDetailAssetSubsystem.cpp`): `BuildNaiveFaceGeometry` (~:323);
the runtime `BuildFromMeshDescriptions` path and its ≤4/tick budget; the
per-(species, seed) HISM components and the ClearInstances/AddInstances
rebuild-on-release pass; the vertex-colour palette bake (~:374-413) and its sRGB
decode; `-VoxelDetailShadows`; the per-component `OriginUU` float-precision trick;
`M_VoxelDetailAsset` and `Tools/create_detail_asset_material.py`.

**Must survive — each with the failure that follows if it does not:**

* **The residency gate.** Group rect dilated by the **full** layer-table reach
  against `IsFootprintResident`. The file calls a worker column into a
  non-resident fine tile *"a fatal gate leak"*. `terrainOnly=false` evaluates
  terrain-layer anchors too, so the dilation must stay full-width.
* **Anchor-ownership dedup.** Reach dilation returns the same instance to up to
  four neighbouring groups; anchor ownership is the only dedup.
* **The animal exclusion.** Layer 255 plus the defensive `hasParts()` guard
  (`:530`).
* **Consume-the-resolver determinism.** This subsystem computes no placement.
  Its replacement must not either, or worldgen digests, admission bounds and
  multiplayer determinism all move.
* **`-VoxelNoDetailAssets`.** Keep it. It is a free control arm for every A/B.

**Three things are added, not retired, and none is small:**

1. **Cover becomes worldgen state, not presentation.** The header today promises
   cover *"puts nothing in the world lattice … changes no worldgen digest,
   admission bound or streaming decision."* In a volume that sentence stops being
   true. Streaming bounds and admission must account for cover.
2. **A second producer.** `ChunkHasEditedBrick` (`VoxelWorldSubsystem.cpp:7872`)
   routes edited chunks to the CPU path (plan §7 finding 3). Cover in the volume
   needs the CPU brick packer to compose cover too, and the byte gate must reach
   both. This also *fixes* the known v1 limit that cover ignores dig/place edits.
3. **The `bankGrid` mutex becomes a hard dependency.** Plan §9 cause #3:
   26,462,610 lock requests in one capture, **explicitly not fixed and not to be
   absorbed by this project.** Cover resolution is its heaviest caller. It is a
   named prerequisite, not a side effect.

---

## 7. The correctness gate

### 7.1 The noise floor here really is zero

Both sides are integer voxels. There is no float anywhere in the path. Unlike the
standing rule *"calibrate any verifier against the reference's own noise floor,
not zero,"* **the correct tolerance here is exactly zero. Nobody should add an
epsilon out of habit from the depth gate.**

### 7.2 The reference must be built before the gate, not alongside it

**Stated as a rule, because the obvious gate here cannot fail.** The natural
reference, `materialAtResolved`, **returns `MAT_AIR` for every detail grid by
design** (`assetfield.h:354`). A byte gate built on it compares *cover* against
*air* — and **passes if the GPU stamp also produces nothing.** That is a check
that cannot fail, the sixth of that shape found in a single day on this project.

So: **build `resolveForCoverCompose` / `coverMaterialAtResolved` first, prove them
against unit tests, and only then point a byte gate at them.** Same shape as
`packChunkBricksCanonical` for P1 and `fluidoccupancy.h` against
`VoxelFluidCollision.ush`: one engine-free header, one test file, one definition.

### 7.3 Four ways the gate fails for non-defect reasons

1. **Anchor division sign.** `anchorXMm` is signed millimetres and the census
   ground is at (−39661, −57292). C++ `floorDiv` floors; HLSL `/` truncates toward
   zero. The current stamp never divides — anchors arrive pre-divided from the
   host. **Keep it that way at 50 mm.**
2. **The 7 bushes at 2 cm.** Not expressible on a 5 cm lattice. **Refuse at load
   with the species named**, exactly as `SizeZ > 4095` is refused
   (`VoxelGpuWorldGen.cpp:897-905`). Content failing a gate and then being
   debugged as code is a real and repeated cost here.
3. **Instance ordering.** First-non-air-wins depends on per-instance dispatch in
   `instancesForRect` order via the RDG pass barrier. Checked: at ≤0.469
   instances/m² and a 1.6 m chunk at 5 cm this is **~1 instance per chunk**, well
   inside the existing shape. No new kernel is needed for ordering.
4. **Two producers, one gate.** Once the CPU brick packer composes cover (§6),
   the gate must run against both, or the path carrying 94% of chunks is
   unverified.

---

## 8. Approved order

1. **`resolveForCoverCompose` + `coverMaterialAtResolved` in voxel-core** —
   engine-free, unit-tested, single definition. This is the byte-equality
   reference (§7.2), and it is a prerequisite for step 2, not a parallel task.
2. **`vxc_volumeprobe --detail-cover`** against the terrain-only census, same seed,
   same site. One arm (cover composed at 50 mm over the detail ring), one control
   (today's `--cascade`), one number (MiB added, plus the brick-touch count that
   is the soft term in §5.1). CPU bench — no editor, no UBT, no shader.
3. **A 10 cm column in `asset-forge/tools/lattice_ab.py`**, in parallel — offline,
   no editor. **The owner judges the shape question; state conditions, offer no
   verdict.** The caption must carry the tool's own methodological point: *shrinking
   a 2 cm asset into a 5 cm lattice answers a different and easier question than
   designing one for 5 cm* — so the honest comparison is authored-for-10 cm beside
   authored-for-5 cm, at one physical scale.
4. **Refuse the 7 bushes at 2 cm by name at load** (§7.3.2).

**The one-species stamp is held until the census lands.**

---

## 9. Side findings, routed

* **`docs/biome-placement-survey.md:81` is stale by one day.** It states flowers
  and reeds have no banks. All 87 flower and 33 reed species are banked —
  `asset-forge/out/engine/banks/BAKED.json`, dated Aug 18; 439 species directories
  present (bush 57, flower 87, tree 77, rock 96, grass 89, reed 33). Its finding 1
  and the bolded "placed-but-invisible" columns need re-reading against that.
* **`docs/detail-asset-rendering.md` quotes a pre-retune density.** Its
  "~20k per 128×128 m" is 1.22 instances/m², 2.6× the current ceiling.
  `VoxelDetailAssetSubsystem.cpp:147` records the cut from 1000 → 300 per-mille.
  Use `docs/biome-placement-survey.md` for density; it is post-retune.
* **`asset-forge/tools/lattice_ab.py` was measuring size on screen, not detail
  — found and fixed while adding the 10 cm column (step 3).** `render.scale_for`
  quantises pixels-per-voxel to {8,6,4,3,2,1} and each variant was scaled to
  FILL its own cell, so metres-per-pixel differed across a row: measured, the
  5 cm reed rendered 63x118 px against the 2 cm reed's 151x345 px for plants of
  2.65 m and 2.86 m. **The coarse column looked better than it was, which is the
  exact direction that would have biased this phase's own decision.** The tool
  now sets pixels-per-voxel proportional to pitch (1 px per cm of voxel edge,
  integral because every authored pitch is a whole number of cm) and applies one
  common per-row downscale. Any earlier sheet from this tool — including
  `out/lattice-ab.png` — carries the old bias and should not be re-read.
* **The `bankGrid` global mutex is a prerequisite for this phase**, not a side
  effect of it — see §6, item 3.
