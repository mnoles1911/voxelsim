# Water system: brief for a deep-dive / refactor session

Written 2026-08-05 at the owner's request: *"deep dive on our water system and
propose possible refactors or improvements."* This is the **input** to that
session, not its output. It says what the system is, what is settled, what has
already been falsified so nobody re-runs it, and where the real cracks are.

Read `water-handover-2026-08-04.md` alongside this. That one records how the
system got here; this one records what is wrong with it now.

**The standing goal, in the owner's words:** *"realistic, natural looking rivers
that flow from origin source points out to the ocean or wherever they end. Lakes
and basins filled. Water placement determined by bake, filling terrain features
cut by erosion."* And the older mandate that constrains every answer: *"I don't
want to use lighting or shadow gimmicks. I want to physically change the
geometry."*

---

## 1. What the system is, in one pass

Five stages, three of which can each independently produce "no water on screen":

1. **Climate → runoff.** Budyko: precipitation minus evaporative demand. Per
   cell, in mm/yr. `terrain-service/terrain_service/bake/water.py`.
2. **Runoff → discharge.** D8/MFD accumulation up the drainage pyramid. Since
   the carried-discharge change, real Q flows up the pyramid rather than a
   local-runoff proxy.
3. **Discharge → channel geometry.** Hydraulic geometry laws:
   `width = 1.5 m x (Q/Qp)^0.398`, `channel_depth = 0.3 m x (Q/Qp)^0.352`,
   `water_depth = 3/4 x channel_depth` (the remaining quarter is bank
   freeboard — the rim *is* the bank crest). `Qp = 315,576 m3/yr` (10 l/s).
4. **Geometry → painted cells.** `fill_to_local_surface` (bake_ver 13) floods a
   cell to the surface of the first drawn channel cell on its own downstream D8
   path. `bridge_to_face_contact` (bake_ver 14) adds the corner a diagonal step
   needs, because **diagonal voxels share an edge, not a face, and the client
   cannot draw an edge-only connection**.
5. **Painted cells → pixels.** Near field: implicit water bricks within
   **+/-25.6 m horizontally and +/-12.8 m vertically** of the camera. Beyond
   that: ribbon and sheet actors. `VoxelWaterSubsystem.cpp`.

**Version split that makes all of this tractable:** `TERRAIN_VERSION` (the
ground) and `BAKE_VERSION` (everything else) are separate. A water-only change
leaves terrain bit-identical, verified by `tools/verify_water_only_change.py`.
Preserve this. It is the only reason water can be iterated at all.

---

## 2. Settled — do not re-derive

* **The bake is the authority on where water is.** The client draws it; it does
  not decide it.
* **The amplified surface is not a water datum.** Three grounds get conflated
  repeatedly and it has cost days: the raw *sample field*, the *spline
  reconstruction* (`reconstructedGroundMm`), and the *amplified surface*
  (`GroundMmAt`). The datum is reconstructed ground plus baked depth, and it
  **never reads the amplifier**.
* **Ocean is a real datum**, not a special case bolted on.
* **The greedy mesher is already optimal on water tops.** `meshBrick<8>` hits
  the theoretical 64:1 maximum. Any proposal that "reduces water geometry" is
  competing against a win already banked. `vxc::Quad` is **12 B**, not 152 B —
  that error appeared in a plan and inverted a cost comparison.
* **Near-field mesh budget has exactly 8% headroom.** With the underground
  ground-floor fix, demand against the 11,520 bricks/s drain is **0.92x**.
  Before it, 78x. Any change that grows the near-field window breaks this: a
  32 m ring base needs 1.55x the window area, which is 1.43x capacity — over.
* **Isolation between world variants is by cache root**, not by forging a
  provider identity. `D:\voxelsim\tile-cache` (arid corridor, bake_ver 14) and
  `D:\vox-wet-cache` (wet alpine block) coexist; the tile dir alone decides
  which world you are standing in.

---

## 3. Falsified — do not re-run these

Seventeen confident explanations were killed by measurement in three days. The
ones a refactor session is most likely to rediscover:

| Claim | Reality |
|---|---|
| The amplifier buries the river | **+3 mm** at the centreline (p50). Nearly rolled `kWorldGenVersion` for nothing. |
| Rivers die at tile seams | 0.5–2.3% vs a 0.39% chance rate. Not a seam problem. |
| The hydrostatic cap bounds the flood | It never bounded anything; it only stopped *recording*. |
| Rim instancing scales better than interior | Inverted. Rim grows **faster** (R^1.46–2.80 vs R^1.07–1.91). A river is a 1-D feature. |
| A heightfield "film" is cheaper at distance | Forfeits the banked 64:1 mesher win at 23x the cascade's cost, and shows **0 px of vertical step** — the flat look the owner already rejected. |
| Carrying real Q would coalesce the wet mask | 1,954 → 2,014 components. It did not. |
| 82.5 m3/s is the world's largest river | A **submarine sink at −3,132 m**. `fill_depressions` floods ocean basins. |
| Full-disc rebuild is the near-field bottleneck | Ratio exactly 1.0. The real cause was **68.8% of offered bricks being underground**. |

---

## 4. The open cracks, ranked by how much they cost the player

### 4.1 The painted river does not obey its own law — **start here**

Measured on the wet alpine block: Spearman(law width, distance downstream) =
**+0.728**, but Spearman(**drawn** width, distance) = **+0.401**, and
Spearman(drawn width, Q) = **+0.457**. At km 20 the law asks for **18.2 m** and
the water plane holds **3.75 m — two pixels**. Centreline median drawn width
over the whole 44 km reach is 3.75 m against a law median well above it.

**Whatever converts Q into painted cells is losing most of the growth the law
specifies.** This is the single defect that most makes rivers look wrong, it is
independent of climate, and wetter country does not fix it — it only makes it
easier to see. Everything downstream of stage 3 is suspect.

### 4.2 The exponents make a big river unreachable — a design question, not a bug

`width ∝ Q^0.398`, `depth ∝ Q^0.352`. Measured against observation the laws are
*exact*: 6.3x the discharge produced 2.07x the width and 1.91x the depth,
against 6.3^0.398 = 2.07 and 6.3^0.352 = 1.92. Nothing clamps —
`CHANNEL_MAX_DEPTH_M` (25 m) needs `Q = 9.18e10 m3/yr` and the largest Q
observed anywhere in this world is **1.68e8**, i.e. **0.18% of the cap**.

So: **a 10x visibly bigger river needs ~320x the discharge, and this world's
entire runoff range spans about 6x at the trunk.** No region of this world can
deliver a dramatically bigger river under these laws.

The owner has already pushed on this twice — *"Why are rivers a maximum of 2 m
deep?"* and *"How are the world's rivers under 1.2 m deep? That doesn't sound
right... we need to revisit the architecture design."* **He is right and this
brief does not have the answer.** Options a deep dive should cost out:

* A bigger world, or a coarser cell, so trunk Q can actually accumulate.
* Different exponents (these are Earth's at-a-station / downstream values, and
  a game may not want Earth's).
* Decouple *visual* width from hydraulic width below some Q, explicitly and
  honestly, rather than by accident as in 4.1.

Whatever is chosen, it should be chosen, not inherited.

### 4.3 Water is invisible past 25.6 m

Near-field implicit water reaches **+/-25.6 m horizontally, +/-12.8 m
vertically**. Beyond that a river is ribbon/sheet actors, which the owner
rejected on sight: *"The flat quads look terrible within 1 km. We need more real
voxels out to 500 m or 1 km."*

The **ring cascade** is the agreed answer and is **half-built** on branch
`claude/water-ring-cascade` (`D:\vox-cascade`, commit `454c635`, unpushed).
LOD L covers `[base<<(L-1), base<<L)` with a `(0.1<<L)` m voxel — area x4,
resolution ÷4, cost per ring flat. Priced at **16,872–31,059 bricks and
38,282–49,528 quads out to 1,024 m: 1.45x–2.62x today's box for 40x the
radius.** At a lake reach it draws 5,844 quads where today's box draws zero.

**What is done:** the material's surface offset is a parameter instead of a
hardcoded world-space `-10.0` (at LOD 5 the old literal floated coarse water up
to 3.1 m above its own ground — a step at every ring boundary); the water chunk
component can be drawn at a level at all; the component leak and the underground
ground floor both landed via the near-field work.

**What is not:** the **plane enumerator**. The cascade is only affordable if the
binding site builds a wet mask *first* — block-major, rejecting CONSTANT-dry
blocks index-only, which is 72–87% of the plane at zero bytes fetched — and only
then resolves ground and datum for the ~1% of columns that are wet. Wiring rings
to a naive column sweep instead puts back exactly the demand-over-capacity
stutter that was just removed. `farwaterenum.h` and `test_farwaterenum.cpp` were
in progress when this paused; treat them as a sketch, not as working code.

Two constants that must move together: `GetImplicitWaterDiscUU` (still 25.6 m)
and `RefreshFarWater`. Moving the disc before the rings draw cuts an 819 m hole
in the ribbon with nothing inside it — strictly worse than today.

Ring base is **25.6 m, not 32 m** — see §2, the near-field headroom.

### 4.4 Lakes dominate, and nobody designed that

On the longest reach in the wet block, **42.3% is lake sheet, not river**. Lake
depth p50 **4.82 m**, max **45.4 m**, against a river-centreline max of
**2.05 m**. The deepest water on this "river", by a factor of 22, is standing
water. In the arid corridor the same measure was 0.6%.

That is not obviously wrong — alpine wet country makes lakes — but it means a
flythrough is partly a chain of lakes, and it means **the lake path, not the
river path, is what the player mostly sees in wet country**. The lake path has
had far less scrutiny. A deep dive should look at it as a first-class subject.

Related and unexplained: on the wet block, **32,199 centreline runs hit the
32 px (60 m) width cap**, and the probe's own comment says *"the bake writes
basins DRY so these should be rare"*. Either basins are not being written dry,
or the cap is being hit by something that is not a basin. **Unresolved.**

### 4.5 Rivers paint the seafloor

**66% of wet pixels in the trunk block** are seafloor. In the arid corridor,
2,156,457 of 2,891,487 wet cells — **74.6%** — were the single coastal tile,
where river water paints the seabed and the lateral fill then amplifies it. Every
wet-cell statistic quoted for a coastal tile is contaminated by this, which is
why the wet-block numbers are deliberately quoted inland-only.

### 4.6 Burial away from the centreline

At the centreline the water datum stands **0.52 m above the drawn ground**
(p50), with only **1.05%** buried. But at the widened edge used for a 5 km view
it is **56.7% buried**, and at the 20 km capped edge **81.7%**. The sub-pixel
widening policy draws water where there is no bed for it. This interacts with
4.3: if the cascade replaces widened quads with real voxels inside 1 km, most of
this goes away — but it should be confirmed, not assumed.

### 4.7 The two-renderer tone seam has never been measured at a river mouth

Known to exist, never quantified where it matters most. Cheap to settle.

---

## 5. Where the code is

| Concern | File |
|---|---|
| Laws, extents, all the constants | `terrain-service/terrain_service/bake/water.py` |
| Lateral fill / slope contact | same, `fill_to_local_surface`, `bridge_to_face_contact` |
| Whole-world discharge survey | `terrain-service/tools/survey_world_water.py` |
| Lake + river sampling, ocean datum | `voxel-core/include/voxelcore/lakes.h` |
| Ring cascade rules | `voxel-core/include/voxelcore/farwater.h` |
| Incremental near-field window | `voxel-core/include/voxelcore/waterwindow.h` |
| Client: near field, disc, chunks | `ue-project/Source/VoxelEarth/VoxelWaterSubsystem.cpp` |
| Water material / WPO | `ue-project/Tools/create_water_voxel_material.py` |

**Probes that actually answer questions:** `vxc_riverribbonprobe <tiledir>
[--origin PX PY] [--region PX]` is the highest-value one — it reports wet
fraction, connectivity of the baked mask, centreline widths, reach lengths, the
sub-pixel policy as numbers, and burial, and it cross-checks the far-field fill
against `RiverSampler::surfaceAtPixel` cell by cell. Also `vxc_waterdatumprobe`,
`vxc_bankprobe`, `vxc_hydroprobe`.

---

## 6. Test on wet country, not the arid corridor

Every water number in this project until 2026-08-05 came from **four dry tiles**
(`-11,-4 / -11,-5 / -11,-6 / -12,-5`, runoff 149–292 mm/yr, world p80). The owner
caught this from a screenshot: *"this still looks like the arid desert, not an
alpine, high wetness, more water location."*

**Use the wet alpine block:** tiles `-5,-4 / -4,-4 / -3,-4 / -5,-5 / -4,-5 /
-3,-5`, cache root `D:\vox-wet-cache`, runoff 445–1,262 mm/yr (mean 751, **3.4x**
the corridor), 100% land, relief 4,906 m. Measured there 2026-08-05: 6/6 tiles
loaded, 0 refused, 0 unresolved, **0.560% wet**, 467 connected components with
the largest spanning **14.63 km**, 1,365 reaches, **425 km of channel**, mean
width 7.52 m, p99 66 m, widest 170 m.

World runoff over 289 coarse tiles: p25 3, p50 13, p75 103, p90 364, p99 1,238,
max 1,515 mm/yr. **The world is not short of water. It was short of water we had
looked at.**

A live playtest site on that block, verified: `-VoxelSpawnAt=-64019,-69172`,
water surface 1,728 m, on a 3,865 m reach.

---

## 7. Traps that have each cost hours

* **A live process is not a running editor.** Reported twice off process state,
  wrong twice. Confirm from **log progression**.
* **Zen Storage Server took 4,822 s cold** once and the editor never came up.
  Warm restart: 3.954 s. If it hangs, kill Zen and relaunch.
* **Comparing a ZSTD tile against a RAW tile invents diffs.** That produced a
  "696 control points differ by 1 mm" claim that was really **zero**, and
  `quant == 1` is the *code* for 100 mm — a 100x unit error. It reached a PR.
* **Bench targets carry `-Wall -Wextra -Werror` behind `if(NOT MSVC)`**, so this
  box warns about nothing and CI fails after the fact. Seven CI failures in
  three days from exactly this. Compile changed TUs with clang and those flags.
* **A stale prebuilt `voxelcore.lib`** produces link errors that look like
  missing code. Rebuild `build/voxel-core-msvc` first.
* **Live Coding blocks `Build.bat`** while the editor is open. Not a build
  error; close the editor.
* **`Build.bat` exits 0 on `RulesError`.** Gate on `Result: Succeeded`.
* **Blank captures are usually unloaded terrain**, not a rendering bug. The log
  says which.

---

## 8. Open branches and PRs at pause

| Branch / PR | State |
|---|---|
| `main` @ `0db6622` | bake_ver 14 merged (slope face contact) |
| `claude/water-ring-cascade` (`D:\vox-cascade`, `454c635`) | material offset + levelled component; **unpushed**, plane enumerator unbuilt |
| PRs #226 / #227 / #228 | cascade rules, near-field ground floor + incremental window, measurement — check which have merged |

A background bake of the six wet tiles at bake_ver 14 into `D:\vox-wet-cache`
was running at pause. If it completed, the wet block gains the slope fix and
becomes the right place to test everything in §4.

---

## 9. What the deep-dive session should come back with

Not a list of everything wrong — this document is already that. Come back with:

1. **A diagnosis of §4.1** — where between the width law and the painted cells
   the growth is lost. That is one measurement away and it is the biggest
   visible defect.
2. **A decision framed for the owner on §4.2**, in plain English, with the cost
   of each option. He has asked twice; he deserves a real answer rather than
   another restatement of the exponents.
3. **A refactor proposal that says what it deletes**, not only what it adds. The
   water path has accumulated a near-field brick path, a ribbon path, a sheet
   path, a lake path, an ocean path and now a ring cascade. Some of those should
   not survive.

And measure before proposing. The falsification rate on confident explanations
about this system is running at better than five a day.
