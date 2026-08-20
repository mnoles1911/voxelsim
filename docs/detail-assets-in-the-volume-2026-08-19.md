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
  **223 at 5 cm; 7 at 2 cm.** "Every one on the 5 cm lattice" is not quite true.
  The 7 are the coral set — `kind: bush`, ocean-only, no land placement — so
  every detail species that can appear on land is at 5 cm. §7.4.2 has the list.

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
* ⇒ **16-35 MiB; call it under 50 MiB with margin** — *predicted before the run;
  §5.2 has the measurement, which landed at 26.5 MiB.*

Against the plan's **measured 1,110 MiB** for R0-at-5cm
(`docs/ray-marching-plan-2026-08-19.md:759`), and against today's 388 MiB commit /
167.6 MiB resident.

**~50 MiB scoped against 1,110 MiB coupled is not a difference in approach. It is
a different project.**

### 5.1 The falsifier, stated plainly — and resolved

The two inputs — grid occupancy and instance density — were **measured**. The soft
term was the **brick-touch count** (how many distinct 40 cm bricks the cover
actually lands in, after overlap), which was **modelled, not measured**.

**The falsifier, written before the run: if `vxc_volumeprobe --detail-cover`
returns ~500 MiB or more, this document is wrong and
`ray-marching-plan-2026-08-19.md:795-797` is right.**

### 5.2 Measured 2026-08-19/20 — the doc stands by a factor of 20–48

Full record: `docs/measurements/cover-volume-census-2026-08-19.txt`.
Ring 112 m, pitch 50 mm, payload = descriptors + occupancy + 16 B palette +
adaptive cells.

| site | ground | inst/ha | **payload** | flat index |
|---|---|---:|---:|---:|
| **grassland −92445,−75855** | **real fine + real climate** | 3,611 | **25.4 MiB** | 29.1 MiB |
| grassland −92445,−75855 | coarse-only\* | 3,717 | 26.5 MiB | 25.4 MiB |
| alpine bank −61440,−61440 | real fine + real climate | 156 | 10.5 MiB | 290.1 MiB |
| **CONTROL** — terrain at 10 cm, same alpine ground, `--radius 112` | real | — | 31.3 MiB | 37.4 MiB |

\* synthetic elevation over real coarse climate — the instrument the biome survey
used for 6 of its 10 sites. Named, not hidden. **Superseded by the fine-tile row
above** (§5.5), and kept because the pair is the evidence that the proxy was
sound: the two disagree by **4%**.

**25.4 MiB on real ground at a dense biome, against a 500 MiB falsifier.** On
identical ground the cover volume is **34% of what the terrain volume already
costs there** (10.5 against 31.3 MiB). The band predicted in §5 before any run —
16–35 MiB — contained the measurement, and so did the narrower 15–45 MiB
prediction made for the fine-tile row specifically (§5.5).

**The site nearly buried it, and that is the reading rule.** The first run used
the brick census's own site and returned 10.5 MiB — but `vxc_assetprobe` over the
same ground says only **three species place there** (alpine-krummholz 600,
juniper-scrub 331, granite-boulder 6): **no grass, no flowers, no reeds**, a steep
above-treeline rock face where biome weight alone refuses 83.5% of pairs. The two
sites disagree in *both* directions and nearly cancelled — alpine has **24× fewer**
instances per hectare but each is **38× bigger** (8,173 against 214 solid voxels).
Either site reported alone would have been a confident number about the wrong
world.

Two cross-checks passed: instance density **3,717/ha against the survey's
3,908/ha** (§4, two independent walks, 5% apart), and per-instance solid voxels
against the bank census (214 against grass 257 / flower 480; 8,173 against
juniper-scrub's own 5,131–10,900).

**The reading rule this generalises to, past this phase: a single-site census is a
claim about that site.** The alpine row is the project's own standard site, it
looked entirely plausible, and it was measuring ground with no ground cover on it.
What caught it was not a better number but a **cross-instrument** check —
`vxc_assetprobe` naming the three species that place there. A census and its
cross-check should come from different walks; agreement between two readings of
the same walk proves only that the walk is self-consistent.

### 5.3 DESIGN REQUIREMENT C1 — a cover volume must be sparsely indexed

**Not an observation. A requirement, and the only one this measurement imposes.**

A flat 3D brick index over the cover footprint costs **290.1 MiB at the alpine
site against 25.4 MiB at grassland** — an 11× swing driven by **terrain relief,
not by cover**. Cover is a thin shell following the ground, so a dense index over
its own bounding box is **0.075% occupied** on steep slopes: the index is sized by
the landscape's vertical range while the content is sized by a metre of plants.

> **C1. The cover volume is addressed by a sparse structure — per-chunk table,
> hash, or octree — never by a dense 3D brick array over its bounding box.**
> A flat index costs 11–28× the payload it addresses and scales with terrain
> relief, which cover does not control. Any design that reaches for a flat index
> here has to price it at the steepest ground it will run on, not the flattest.

This is the same design consequence the plan's §4 records for 2.5 cm, reached
independently from the cover side, and **it is the only term anywhere near the
falsifier** — payload never came close. Read the payload column for what a cover
volume costs and the index column for what addressing it the wrong way would.

### 5.4 A secondary result, from the same walk

The probe meshes every brick it censuses, so quads and bricks are two readings of
one field. Cover as a **volume** costs 26.5 MiB at grassland; the same cover as
**quads** is 9,074,442 quads = **103.8 MiB** at 12 B/quad. The volume is **3.9×
cheaper than meshing the same cover** — 7.9× at the alpine site. That is a
measured argument for retiring the HISM path (§6), not just a tidiness one.

### 5.5 The open item, and the prediction — CLOSED 2026-08-20

**The prediction, recorded before the run:** a vegetated fine-tile census would
move the payload *by the elevation gate's share, not by an order of magnitude* —
**between 15 and 45 MiB, and not above 100**. The reasoning was put on the record
with it: at the one site where gate attribution was measurable, **biome weight
refused 83.5%** of (site, species) pairs and **elevation band 15.8%**; the
grassland run already had real climate driving the dominant gate, so real
elevation could only refine the minority one.

**Result: 25.4 MiB. The prediction holds, and the proxy was accurate to 4%**
(25.4 against 26.5 MiB). Instance density moved from 3,717 to **3,611/ha**, still
within 8% of the survey's independently walked 3,908.

No bake was needed. `-7_-5.vxtl` already existed in the v28 fine namespace UE is
pinned to (`tile-cache/…-b19d281fd/000000000135276f/s16/`), and the grassland site
sits inside that tile's footprint — tile edge is 15,360 m, so (−7,−5) covers
X −107,520…−92,160 and Y −76,800…−61,440. **The instrument gap was a staging
oversight, not a missing measurement**, which is worth remembering next time a
result is qualified as "blocked on a bake".

**What this closes and what it does not.** The 5 cm cover volume over the 112 m
detail ring costs **25.4 MiB on real ground at a dense biome**, against a
**31.3 MiB terrain control** and a **500 MiB falsifier**. What is still unmeasured
is any biome other than grassland and alpine on real fine tiles — and by this
document's own reading rule (§5.2), that is a limit on the claim, not a caveat to
wave at it.

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

### 7.3 Mutate the code, not just the reference

**A third standing rule, earned 2026-08-20 and stated here with the other two
because this is where anyone looking for gate doctrine will read.**

A gate proved only against a *correct* implementation is proved against the easy
case. The only way to know a check can fail is to **break the thing it watches
and watch it go red** — and to break it in more than one place, so selectivity is
shown rather than assumed.

This is not theory here. Mutating `produceCoverChunk` two ways found that one
mutation — counting a chunk as packed before knowing whether it packed anything —
**broke nothing**, because every counter test then written exercised a path that
returned at the z-reject *before* the counter under test. The assertions were
real, the suite was green, and it was measuring a line it never reached. **The
test suite was itself a check that could not fail**, the eleventh such instrument
in one night on this project and the first one that was a test rather than a
probe. Reading the code would not have shown it; only mutation did.

So: for every guarantee a gate claims, there must exist a mutation that makes it
fail, and it must have been run. Mutation did not confirm these tests. **It found
the hole in them.** See §8b for the case that closed it.

### 7.4 Four ways the gate fails for non-defect reasons

1. **Anchor division sign.** `anchorXMm` is signed millimetres and the census
   ground is at (−39661, −57292). C++ `floorDiv` floors; HLSL `/` truncates toward
   zero. The current stamp never divides — anchors arrive pre-divided from the
   host. **Keep it that way at 50 mm.**
2. **The 7 species at 2 cm — and measuring them named them, which changed what
   they are.** Not expressible on a 5 cm lattice, so **refused at load with the
   species named**, exactly as `SizeZ > 4095` is refused
   (`VoxelGpuWorldGen.cpp:897-905`). Running `--detail-cover` printed the list:
   `black-coral-tree, branching-stony-coral, carnation-soft-coral,
   cold-water-coral, elkhorn-coral, leather-coral, staghorn-coral`. **All seven
   are corals** — filed as `kind: bush` in the specs, authored at 2 cm, and
   weighted **`ocean` only, zero in every land biome.** So this is not seven
   awkward shrubs a land cover volume has to explain away; it is **the entire
   reef library, which never places on land at all** and which would want its
   own 2 cm reef volume if it is ever rendered. The land cover volume at 5 cm
   refuses nothing it would ever have drawn. *(This is why the refusal is by
   name: an unnamed count of "7 dropped" would have read as a defect in the
   land path.)*
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
2. **`vxc_volumeprobe --detail-cover`** — **DONE 2026-08-19, §5.2.** 26.5 MiB at
   grassland, 10.5 MiB at the alpine site, against a 31.3 MiB terrain control on
   identical ground and a 500 MiB falsifier. Record:
   `docs/measurements/cover-volume-census-2026-08-19.txt`.
3. **A 10 cm column in `asset-forge/tools/lattice_ab.py`**, in parallel — offline,
   no editor. **The owner judges the shape question; state conditions, offer no
   verdict.** The caption must carry the tool's own methodological point: *shrinking
   a 2 cm asset into a 5 cm lattice answers a different and easier question than
   designing one for 5 cm* — so the honest comparison is authored-for-10 cm beside
   authored-for-5 cm, at one physical scale.
4. **Refuse the 7 bushes at 2 cm by name at load** (§7.4.2).

**The one-species stamp is held until the census lands.**

---

## 8b. The producer — built 2026-08-20, and the seam it still needs

**Landed in voxel-core** (`voxelcore/covervolume.h`, tests in `test_assetfield.cpp`):

* `produceCoverChunk(...)` — **`packChunkBricksCanonical` over
  `coverMaterialOfResolved`**. The producer *is* the shipping packer handed a
  cover accessor, which is why a GPU cover stamp is checkable against it from day
  one rather than against a gate written afterwards (§7.2).
* `coverVolumeInit(manifest, pitchMm)` — refuses a non-tiling pitch **whole**, and
  returns the refused species **by name** (§7.4.2).
* `CoverProducerCounters` — a strict funnel, `attempted → resolved → packed`.

**Requirement C1 is in the producer, not just the doc.** A chunk with no cover
returns `anyCover = false` and accumulates nothing — no zeroed pack, no reserved
slot. A z-reject runs before any packing, because a cover volume is mostly empty
vertically.

**The counters exist to fail.** `attempted == 0` means *the producer did not run*;
`attempted > 0 && packed == 0` means *it ran and found nothing* — a real answer
about the ground, and exactly what the alpine site turned out to be. A single
`coverChunks` counter cannot tell those apart, and this project has now found ten
instruments in a night that reported plausibly while measuring nothing, two of
them counters that could not fail.

**And mutating the producer found an eleventh — in my own test suite.** Moving
`chunksPacked` above the `anyCover` check is a real defect: it counts an empty
pack as produced. **Every counter test I had written passed anyway**, because
their "produced nothing" chunk exits at the **z-reject**, before the counter is
ever reached — so the funnel was only ever proved on a path that skips it. The
test suite was itself a check that could not fail, for exactly the reason this
document keeps warning about, and reading the code would not have shown it.

Closed by `coverproducer_packs_an_all_air_species_without_counting_it_as_packed`:
a species whose baked grid is entirely air resolves, survives the z-reject, fills
the shortlists, reaches the packer and yields nothing — the one path where a
premature counter lies. It now fails under that mutation (`packed 1 vs 0`) while
the other four producer tests stay green.

**The rule this earns: mutate the code, not just the reference.** A gate proved
only against a correct implementation is proved against the easy case. Mutation
here did not confirm the tests — it found the hole in them.

### What I need at the seams I do not own

I can specify these but must not build them — no UE build this round, and the
files below belong to other workstreams. Routing request, in priority order:

1. **A cvar, default off.** `voxel.Cover.Produce` (0 = off), read once per
   dispatch and latched, the shape `voxel.GPU.BrickPack` already uses
   (`VoxelGpuMeshJobManager.cpp:45-52`). **`voxel.Cover.Produce 0` is the control
   arm**, and `-VoxelNoDetailAssets` stays as the HISM kill switch so volume and
   HISM can be A/B'd against each other rather than only against nothing.
2. **A stats line** — `voxel.Cover.Stats` printing the funnel verbatim
   (`attempted / resolved / packed / anchored / solid / bytes`), and printing the
   words *"did not run"* when `attempted == 0` rather than a row of zeroes. Zeroes
   are what a broken instrument and an empty world have in common.
3. **A sparse store.** The cover volume must not go into a dense array keyed by
   brick coordinate — **requirement C1, measured at 290.1 MiB against 25.4 MiB of
   payload**. Whether it earns a slice of the brick pool or its own sparse table
   is a call for whoever owns `VoxelBrickPool` / the marcher; I only need the
   store to be keyed sparsely and to accept "this chunk has nothing".
4. **The driving loop.** `VoxelDetailAssetSubsystem.cpp` is mine and already has
   the ring scan, the residency gate and the worker dispatch (§6, "must survive").
   It needs `columnFacts` bound to the engine's own
   `assetColumnFactsFromSample(Amplifier::column(...), assetChannelsAt(...))` — the
   binding rule of 2026-08-17: an instrument that runs a different binding is
   measuring a different world.

**Not yet, deliberately:** the one-species end-to-end stamp. §7.2's rule applies
to this phase too — the reference exists now, so the gate can be built against it,
and the stamp comes after the gate rather than before.

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
