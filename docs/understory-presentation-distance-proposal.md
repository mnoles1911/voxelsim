# Understory presentation distance candidates — preparation only

The private cache contains 339 models from 113 species profiles, all at 25mm
source pitch. This proposal changes only the distance at which a model is drawn.
It does not change ecological placement/query radius, source resolution, bank
loading, endorsement, collision or the configured ring maximum. This report uses the default256m ring; an explicit512m ring is not silently restricted to256m. Nothing is applied.

Reproduce with:

```powershell
python tools/analyze-detail-presentation.py --manifest ue-project/Content/Voxel/Generated/DetailPreview/temperate-attributes-full-1/detail-bake.json --output asset-forge/out/ecological-placement/detail-presentation-analysis-1/report.json
```

The JSON pins the manifest and includes every model's species, actual saved
bounds, LOD counts, triangles, UV channels and proposed distance. Bounds include
the existing horizontal wind expansion; Z remains geometry height.

A first visual trial could use `size = max(height, 0.5 * horizontal width)` and
`end = min(ring, max(min(32, ring), ceil(128 * size / 16) * 16))` metres. Height is the main
silhouette dimension, while half-width stops broad low patches vanishing solely
because they are short. The 32m floor gives small ground cover a useful near
field. Assets at least 2m tall retain the full256m default ring. With a512m configured ring, a4m shrub reaches512m while a2m shrub still uses256m. This is an adjustable
presentation heuristic, not an ecological rule or accepted default.

| End distance | Models |
|---|---:|
| 32m | 96 |
| 48m | 79 |
| 64m | 42 |
| 80–128m | 56 |
| 144–240m | 32 |
| 256m | 34 |

| Example | Height | Proposed end | LOD0 / last LOD triangles |
|---|---:|---:|---:|
| Feather moss seed 12 | 0.05m | 32m | 288 / 144 |
| Common dog violet seed 12 | 0.075m | 32m | 136 / 136 |
| Arrowhead seed 7 | 0.55m | 80m | 4,704 / 2,352 |
| Hazel coppice seed 7 | 3.55m | 256m | 149,922 / 74,959 |
| Rhododendron thicket seed 7 | 3.475m | 256m | 258,678 / 129,339 |
| Elder seed 7 | 3.625m | 256m | 234,654 / 117,325 |

There are 16 models with one LOD, 260 with two and 63 with three. Median LOD0
complexity is 2,252 triangles; the 90th percentile is 61,898. The largest shrubs
remain expensive even at their last LOD: rhododendron reaches 129,339 triangles.
Distance culling alone does not solve those outliers when they remain visible.
Inspect their silhouettes and consider further distant representations separately.
The complete single-LOD list and top 12 complexity outliers are in the JSON.

Saved mesh packages total 172,481,163 bytes (164.5MiB), excluding material
packages. The report also supplies an explicitly assumed buffer-storage proxy:
1,144,390,324 bytes across every LOD, using 12-byte positions, 16-byte tangents,
4-byte colors, 8 bytes per UV channel and 32-bit indices. This is neither measured
VRAM nor engine memory: index/tangent/UV formats, retained CPU copies, allocator
alignment, HISM data and materials change the real number. No actual per-model
resource-allocation measurements for all 339 are available in this manifest.

Equal-weight circular area at the candidate distances is 80.3% smaller than at
256m for every variant. This is ONLY an area comparison, assuming uniform density
and equal variant abundance. It is not a measured instance reduction, GPU gain,
frame-time gain or memory saving. Regional plant density, occlusion, LOD and
existing source/cache retention invalidate those substitutions. Earlier 48m test
captures cannot establish 256m performance, either.

At an illustrative 1280px viewport with horizontal FOV90, the pinhole focal
length is 640px: a size of 1m at 128m subtends about 5px. Wider FOV, aspect-ratio
handling, resolution, camera elevation and grazing views change apparent size;
zooming narrows FOV and exposes premature culling. Ground mats viewed from above
are governed by width, while walking views often see their height. Wind bounds
are conservative; alpha cutouts and clustering alter perceived density. A
hard cutoff can visibly pop even at five pixels, and tall shrubs capped at256m
can be much larger. Do not claim a fade unless the material actually consumes
PerInstanceFadeAmount; the current audit has not established one.

Next validation should compare fixed cameras and approach/retreat routes at
32/48/64m boundaries and at the full256m cap, with wide/narrow FOV and both player
and elevated views. Record missing-cover/pop complaints, visible instance counts,
GPU timings and actual resource residency separately. Preserve an opt-out for
silhouette-significant species. Source banks remain eagerly retained until a
separate validated loading/retirement change addresses them.
