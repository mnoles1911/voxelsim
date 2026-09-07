# Terrain fitting for environment assets

Date: 2026-09-06.

The first implementation seats the four environment LOD prototype assets on
their terrain footprint before constructing meshes or collision grids. It fixes
the previous one-column spawn rule that added 10 cm of clearance and assumed a
flat base. Ordinary generated assets still use the existing shared resolver;
the production integration below is deliberately not a renderer-only offset.

## Placement policy

The loader extracts the lowest supporting voxel in each asset column. Trees
use wood in the bottom 50 cm, soft plants their actual lowest occupied layer,
and rocks the lowest 30% of their height. Canopies do not enlarge the tree's
ground footprint. The support cells are reduced to sixteen footprint bins.

A 5 by 5 terrain-height grid bounds those bins. Each occupied bin uses the
lowest and highest of its four terrain corners. The engine-free solver chooses
the highest lattice-aligned vertical origin that seats every sampled bin. Trees
also receive a 25 mm embed allowance. The whole actor moves together; it is
never tilted, stretched, or adjusted only in its material. Rendering, all LODs,
digging, and the existing coarse collision queries retain the same origin.

| Pilot asset | Maximum lowering from centre anchor | Maximum sampled base burial |
| --- | ---: | ---: |
| Oak | 800 mm | 1,000 mm |
| Boulder | 45% of height | 65% of height |
| Bramble | 150 mm | 300 mm |
| Daisy | 50 mm | 100 mm |

Trees and rocks retain 100 mm origin alignment; soft plants use their source
voxel pitch. These are initial policy values, not biological measurements.
Rocks are partially embedded rather than rotated: arbitrary rotation would
invalidate the prototype's axis-aligned collision and voxel lookup assumptions.

The current oak's low wood footprint is approximately 2.95 by 2.80 m. Its
1 m burial ceiling can refuse a continuous slope around 15–20 degrees,
depending on heading and root shape. Truly steep-site trees need suitable
authored root variants or bounded voxel root adaptation in a later pass;
this implementation does not claim to grow a root system around any terrain.

If the requested site exceeds either limit, eight deterministic alternatives
are tried, at cardinal/diagonal offsets of 2 m per axis (2.83 m maximum travel).
The first acceptable site wins. If none fits, the instance is refused and a
warning records why. Refusal is preferable to planting a floating root system
or burying several metres of trunk. Every successful placement logs its final
position, candidate, sample count, lowering, burial, and elapsed CPU time.

There is no per-frame fitting. The actor retains the resulting transform.
Reset reloads the source and resolves against a fresh terrain baseline, so
vertical offsets do not accumulate.

## Cost and practical limits

- Accepted first candidate: 25 height queries; worst case: 225. Height queries
  are not per voxel. Footprint extraction scans source columns once during
  loading, before the existing mesh build. The prototype uses the existing
  game-thread surface sampler, which may synchronously load missing fine tiles.
These counts are bounded; they are not a guarantee of negligible spawn time.
- The corner envelope bounds planar/bilinear slopes. It cannot certify a cave,
  narrow ledge, or depression between samples. The prototype's analytic surface
  query also does not account for player-dug overlay holes. Sampling actual
  solid support and edit invalidation are required for production placement.
- The method seats the lower supporting base. It does not bend exposed roots
  down a cliff or manufacture new roots, and terrain is never modified.
- Nearby retries can change spacing. The isolated pilots are 15 m apart; a
  population scatterer must apply its spacing/water/biome rules again after a
  moved candidate. These retries must not be copied into production unchanged.
- Surface-conforming grass patches should eventually split into small clumps
  with individual anchors. Lowering a wide grass carpet is not a good substitute.

## Production integration

`AssetField::instancesForRect` currently resolves one anchor-column fact set
per site through `assetResolveSite`. The latter already applies species slope,
biome, water and solid-anchor gates, but the instance's `anchorZMm` is simply
the centre column's surface. A valid centre anchor does not prove that the
entire base is supported. Those instances feed world composition, CPU/GPU
rendering, mining, and the detail/cover paths.

1. Bake support-bin metadata alongside each immutable bank grid. Store plant
   class, root footprint, allowed embed/burial and maximum relocation in species
   policy. Share the small engine-free fit solver introduced in this change.
2. Resolve terrain samples once per stable site identity and cache the accepted
   transform or refusal. Include bank/policy/terrain-generation revisions in
   the key. Do not multiply 25 queries by every brick's repeated site resolve.
3. Apply the resolved anchor in the shared `AssetField` instance path, so
   `GeneratedWorld::materialAt`, `makeBrick`, coarse/near rendering, GPU asset
   stamps, HISM and cover volume all consume the same instance placement.
   A GPU-only or HISM-only vertical adjustment would create invisible solids or
   render trees in places that mining still considers empty.
4. Expand fine-tier residency footprints by support reach plus allowed search
   distance before worker dispatch. Recheck site gates at every relocated
   anchor. Revisit spatial query dilation, vertical admission bounds and site
   ownership when horizontal anchors can move across chunk boundaries.
5. Version the worldgen placement policy before changing existing saves:
   overlays can contain materialized old asset voxels. A new anchor must not
   silently duplicate or resurrect previously cut trees. Authoritative support
   loss from later digging should trigger collapse/detachment, not move a grown
   tree sideways or snap it into newly excavated terrain.

## Validation

`test_assetslopefit.cpp` covers negative-coordinate lattice alignment, downhill
support contact, excessive burial/cliff refusal, sink limits, repeatability,
invalid samples and dense planar footprints at 32 slope headings. Engine and
visual validation are coordinated by the parent task; consult that task's
build/capture results for runtime evidence. No populated-forest performance
claim follows from the bounded sample count alone.

The core test target compiled successfully and all 838 cases passed, including
the six new cases, in `Saved/slope-fit-core-tests.log`. The first coordinated UE build
passed; a capture-lambda uninitialized-local warning was subsequently corrected
before the parent task's final rebuild.

The first runtime check at spawn `-61440,-61440` correctly refused the oak:
its sampled uphill burial would be 1.8–2.7 m across the nine local candidates.
The boulder seated on candidate 4 (206 mm lowering, 612 mm burial); bramble
seated immediately (106 mm lowering). The 20 cm-wide daisy base also refused
this steep hillside: candidates needed 70–98 mm lowering, beyond its 50 mm
budget. This was confirmed by a standalone probe using the same seed
20260719 and the same four fine tiles plus coarse tiles. Its old-site boulder
and bramble values reproduced the runtime log exactly.

Measured first-run fitting times were 57.582 ms for the refused oak, 8.715 ms
for the boulder, 0.626 ms for bramble, and 0.176 ms for the refused daisy. This
supports keeping fitting out of per-frame work; the oak scan/query cost should
be amortized with baked footprint metadata and cached worker resolution before
forest-wide rollout.

The deterministic test fixture can instead use `-VoxelSpawnAt=-61472,-61504`,
32 m west and 64 m south of the original spawn. All four pass their first
candidate in the offline probe: oak 432/684 mm lowering/burial, boulder 377/736,
bramble 31/31 and daisy 43/72. No policy was relaxed to find this site. Probe
results are saved in `Saved/slope-fixture-probe.log`.

The subsequent runtime at this fixture confirmed all four first-candidate
placements and reproduced those lowering/burial values exactly. Each made 25
terrain queries. Total fitting times were oak **45.189 ms**, boulder 0.066 ms,
bramble 0.532 ms and daisy 0.041 ms (`Saved/tree-felling-game.log`). This is
placement-log validation, not yet visual confirmation of the bases or a
forest-scale benchmark. The oak's 45 ms startup cost is material and must not
be described as negligible or ready for synchronous mass spawning.

The current timer combines scanning the asset occupancy, constructing support
bins, and querying terrain. A production follow-up should profile these phases
separately, then bake/cache the immutable support footprint and dispatch
terrain fitting on residency-gated workers. These measurements do not identify
which phase dominates, and no per-frame fitting was introduced.
