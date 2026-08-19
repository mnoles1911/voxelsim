# The assets compose and nothing appears: what is proven, and what is not

Written 2026-08-16 at the end of the first session with the editor. **The owner
looked at every capture and said, correctly, that none of them contains a placed
object.** This file is the evidence for where the break is, so the next session
does not repeat the search.

---

## 1. Proven working, with numbers

* **Manifest loads in UE**, counts byte-identical to the headless fixture:
  `4 layers, 438 species placeable (382 detail entities, 6 too rare, 0 no biome,
  266 without banks)`.
* **Composition runs.** `26,462,610 bankGrid requests, 26,462,610 served,
  0 refused` in one capture; 7,080,071 in another. The world function is asking
  the field for grids millions of times per run.
* **Placement resolves on real ground.** `vxc_assetprobe`: 117,006 instances at
  one site, 278,732 at another, **0 floating anchors of 278,732 audited**, each
  re-read through `Amplifier::materialAt` rather than trusted from the resolver.
* **The admission gate is wired** to `assetAwareSurfaceUpperBoundMm`, per layer.
* **Opt-in is intact**: with no `-VoxelAssetDir`, the CPU reference digest is
  still `b2b5d2f1044caa35` and Unreal's GPU output is bit-exact with it.

## 2. The break, stated as a number

**UE composes 5–7 species everywhere, regardless of biome.**

| site | biome, elevation | census bank files | UE loaded |
|---|---|---|---|
| −84480, 53760 | bare rock, 0 m | 172 | **28** |
| −65102, −51084 | alpine, 1645 m | 136–196 | **28** |
| −234211, −242007 | rainforest, 654 m | 144 | **20** |

`AssetBankLibrary` has **no budget and no eviction** — banks load on first touch
and stay — and the resident set was stable across each run, so this is not
thrashing. Four seed files per species means 20–28 files is 5–7 species, against
the 36–49 the census reports placeable on the same ground.

Three wildly different biomes give the same tiny number. Whatever narrows the
species pick is **biome-insensitive**, which is the strongest clue available.

## 3. One confirmed contributor, which is NOT sufficient

`assetColumnFactsFromSample` leaves `distanceToWaterMm` at
`kAssetNoWaterDistanceMm`, and says so plainly: *"a species that needs it is
refused rather than placed on an assumption."* `placement.water_max_m` is
documented as unservable and failing closed.

Measured, that refuses **everywhere, in every biome**:

| kind | total | water-bound | share |
|---|---|---|---|
| tree | 78 | 20 | **26%** |
| grass | 89 | 29 | 33% |
| reed | 33 | 14 | 42% |
| flower | 87 | 25 | 29% |
| rock | 102 | 17 | 17% |

A quarter of the tree library — `common-alder`, `bigleaf-maple`, `black-mangrove`,
`date-palm` — cannot be placed anywhere until the shore distance is servable.
**But 74% of trees are not water-bound, so this does not explain 5–7 species.**
It is a real bug and a partial cause.

## 4. What was ruled out, so nobody re-runs it

* **The GPU mesh fork.** `-VoxelNoGpuMesh` changed nothing. (It *is* true that
  `MakeCoarseLevelSampler` reads `Amp.column` directly and bypasses `makeBrick`,
  so **no high-altitude camera can ever show an asset** — every coarse LOD level
  is pure terrain by construction. That framed three wasted captures.)
* **Pixel diffing between capture arms.** Useless here: 22.00% of pixels differ
  between assets-on and assets-off against a **21.97% within-run noise floor**.
  Two frames of the same settled run differ as much as the two arms do.
* **Site choice.** Chased with the biome map (`world-maps/…/02-biomes.png`,
  calibrated against UE ground truth: the y axis is FLIPPED relative to the
  naive reading, verified by classifying the alpine site correctly). Only
  **4.8% temperate forest + 5.2% rainforest** in this world, which is why hand-picked
  coordinates kept landing in rock, alpine and ocean.

## 5. The next move

Log the **bankId histogram** over a run — which species ids are actually being
requested — and compare against `vxc_assetprobe`'s species list on identical
ground. That single comparison separates the three remaining candidates:

1. the species pick in `assetResolveSite` is degenerate (same ids every draw);
2. the column facts UE supplies differ from the census's (biome or slope wrong,
   which would also explain biome-insensitivity);
3. the instances resolve correctly but their grids stamp nothing visible —
   in which case compare `materialOfInstance` non-AIR returns against requests.

**Do not** start by looking at the renderer. The renderer is downstream of a
pick that is already only choosing seven species.
