# Astra creature modeling pipeline and lessons learned

Updated 2026-09-07. This is the single maintained process guide for creating
reference-based cubic voxel animals in Asset Forge. The user accepted all eight
static pilot appearances: grey wolf, tiger, red deer stag, common raven, golden
eagle, rainbow trout, great white shark and orca.

## Definition of success

The primary test is whether the animal looks anatomically and visually correct.
Restrained stylization is welcome when it makes the creature interesting, but
must preserve believable proportions, joints, stance, appendages and diagnostic
markings. A dense voxel count or clean export cannot establish anatomical quality.

The accepted pitch is **0.0125 m = 12.5 mm cubic voxels**. Keep that physical pitch
through the final occupancy grid, saved model and Forge viewer. Do not quietly
coarsen a difficult animal. These are accepted **static appearances**, not finished
runtime creatures: rigging, animation, collision, LOD, performance and engine color
integration require their own work and verification.

## What Astra does

Astra researches the animal, evaluates source quality, prepares an anatomical
master, writes explicit geometry/color corrections, voxelizes and reviews the
result. This is a reference-mesh and authored-color workflow, not text-to-3D.
Tripo and other hosted generative 3D services were set aside for this pilot.

Six photographs do not automatically determine a faithful 3D animal. Unrelated
photos differ in subject, pose, focal length, perspective, lighting and visibility;
the underside is often missing. Treat them as constraints on a continuous master.
A coherent licensed scan or good sculpt supplies the initial surface, which must
still be inspected and corrected. Matched views of one subject are preferable.

## 1. Define the subject and evidence before modeling

Record the intended species/subspecies, sex when relevant, life stage, seasonal
coat, pose and size. Mark unknowns explicitly. Decide which dimensions are being
measured: nose-to-tail, body length, shoulder height and total pose bounds differ.
Population ranges establish plausibility; they do not calibrate a particular scan.

Build a reference record with:

- Institution or primary research sources for skeleton, joints, proportions and
  diagnostic anatomy. Follow the actual methods and measurement endpoints when
  using quantitative studies; do not invent precise landmark measurements.
- Side, opposite-side, front, rear, dorsal and ventral evidence where available,
  plus face, foot, feather, fin or antler closeups. Record missing views.
- Color references appropriate to the intended season, age and sex, distinguishing
  pigmentation from shadows, wetness, preservation discoloration and baked light.
- URLs, author/institution, retrieval date, relevant findings and image rights.
  Publicly viewable imagery is not automatically licensed for redistribution.

Useful institution references used by the pilot include [Cornell bird identification](https://www.allaboutbirds.org/guide/),
[British Deer Society red deer](https://bds.org.uk/information-advice/about-deer/deer-species/red-deer/),
[Florida Museum white shark](https://www.floridamuseum.ufl.edu/discover-fish/species-profiles/white-shark/),
[NOAA killer whale](https://www.fisheries.noaa.gov/species/killer-whale), and
[Missouri Department of Conservation rainbow trout](https://mdc.mo.gov/discover-nature/field-guide/rainbow-trout).
Per-species review records retain the sources actually consulted. These are
evidence sources, not substitutes for measured specimen registration.

## 2. Select and license a source mesh

Prefer complete museum/university scans or anatomically credible artist meshes.
Compare several candidates before investing in cleanup. Reject toy proportions,
missing anatomy, irrecoverable pose distortions, game rips and incompatible rights.

Record the source ID, URL, title, author, individual license and license URL,
retrieval date, source SHA-256, original description and all modifications.
The pilot used CC0 and CC BY sources. Objaverse is a discovery/download mechanism;
its dataset license does not replace the license of each model. Preserve attribution
with derived GLB, voxel and library assets. Do not vendor unlicensed reference art.

`asset-forge/tools/find_creature_sources.py` caches discovery metadata and review
sheets; its current search categories are tiger, deer, crow, eagle and shark.
Extend it deliberately for another category. HTTP 429 is incomplete discovery,
not evidence that no suitable model exists. Cache results and respect rate limits.

Downloaded originals live in ignored `asset-forge/out/creature-reconstruction/source-meshes/`.
Use the recorded source ID to retrieve a missing original, then verify its hash
before treating it as the same reproduction input. With Objaverse multiprocessing,
run downloads from a real Python file guarded by `if __name__ == '__main__':`,
not a script piped through stdin on Windows.

## 3. Inspect and prepare the continuous master

Use Blender to evaluate the static pose, bake transforms and preserve UV/materials.
Inspect every mesh and scene object: support branches, stands, labels, whisker
cards and separate body shells can be mistaken for anatomy. Rendering only the
original front sweep is insufficient. Inspect both hemispheres and the underside.

`prepare_creature_master.py` proposes horizontal heading from PCA and records the
transform. Check that proposal visually; PCA does not know the head from the tail.
Maintain documented axis conventions: Blender inspection is Z-up; exported glTF
is Y-up; the voxelizer accepts glTF and produces the project's voxel coordinates.

Set physical size with uniform scaling. Local proportion changes need an explicit
anatomical rationale, affected region, and before/after views. Avoid forcing several
unrelated population averages onto one individual by stretching all three axes.

Inspect silhouette and landmarks: skull and muzzle/bill, eyes and ears, shoulder
and pelvis, spinal line, joint bends, digit count, support/contact stance, tail,
wing or fin insertions. Fix anatomy in the continuous master where practical.
For fused specimen supports, document the cut region and any reconstructed limbs.

## 4. Voxelize and author appearance

Convert at 12.5 mm after reviewing the master. `voxelize_reference_mesh.py` requires
explicit physical length and long axis. `--length-m` scales the source bounding
length, including tails; it is not an inferred biological measurement.

Use `--fill-union` only for inspected closed exteriors whose separate mesh chunks
need a combined interior fill. It cannot reconstruct a missing back or lost limbs.
Inspect component counts before deleting anything. Taking the largest component
is not anatomical repair; a detached component may be an antler tine or toe.

Sample texture footprints in linear light and retain the authored sRGB surface
sidecar. Constant PBR materials may return one RGBA value instead of one per UV:
broadcast it, and skip nearest-triangle queries when all source meshes share that
constant material. Small nearest-surface batches bound memory on complex scans.

Author coherent coat regions before adding restrained local variation. Preserve
diagnostic eye patches, stripes, countershading and fin/foot colors. Avoid random
speckle, directional banding, rectangular masks, excessively bright eyes and baked
shadows. Use smooth boundaries where biology calls for them. Document color
restoration as authored interpretation, especially for preserved specimens.

Thin parts need an explicit decision at the lattice scale: retain connected
anatomical volume, simplify a feature, or remove an identified sub-voxel strand.
At 0.65 m, a trout is only about 52 cubes long. Fine scales, feather barbs, toe
details and spots cannot all be literal photographic detail at this pitch.

## 5. Review appearance and validate storage separately

Review six orthographic views, six obliques covering both sides, diagnostic
closeups, and the actual Forge viewport at a useful viewing distance. Fit-to-frame
images are useful for inspection but not proportional measurement across animals;
use matched physical pixel scale for measurement comparisons.

Check the animal against the evidence record rather than against the previous
bad model alone. List interpretation limits and unresolved questions honestly.
Technical checks cannot certify taxonomy, age, anatomy or user acceptance.

Before installation, verify:

- Exactly 0.0125 m pitch and plausible physical bounds, with endpoints described.
- Occupancy is one face-connected component for these static pilots, with no
  unintended supports, missing regions or detached anatomical parts.
- Surface-cell coordinates match the occupancy surface in the same order; RGB
  is uint8 with one triplet per surface cell.
- VXA occupancy/pitch roundtrip and exact RGB viewer payload byte equality.
- The actual saved entry loads with the intended colors and shape in Forge.

The accepted results and counts are recorded in
[`pilot-validation.json`](../asset-forge/refs/reconstruction/pilot-validation.json).

## 6. Install, approve and deliver

Install a new imported draft instead of overwriting the old species baseline.
`install_creature_pilot.py` refuses an existing entry or spec and validates geometry,
pitch and color alignment. Save VXA, VOX, full-color GLB, RGB sidecar, thumbnail,
spec, metadata and attribution together. Its CLI accepts quadruped, bird, fish
and cetacean kinds. After review, update completion/approval metadata explicitly.

Full-color Forge appearance depends on `appearance.npz` and the RGB1 viewer trailer.
The VXA shared material palette alone does not preserve this exact color range.
The orca fits the Forge viewer, but its dense runtime budget needs separate work.
Successful viewing does not prove engine decoder or animation compatibility.

Appearance approval is recorded in `meta.json` as `visual_approved: true` and
`review_status: appearance_approved`, with approval provenance. It does not promote
the procedural spec into the runtime bank or set `runtime_ready`. Source attribution
is a provenance record; current approval belongs in model metadata, not the license.

Present one gallery with pictures and direct model links. Both Asset Library and
the Forge dropdown expose saved imported models. Use **Approve appearance** for
review and **Export to game** only for the separately validated runtime workflow.
The gallery at `http://127.0.0.1:8731/static/pilot-review.html` needs the local server;
start the Asset Forge desktop shortcut if it is unavailable. The shortcut refreshes
a verified stale server from the local checkout; it does not automatically pull Git.

Commit exact task files, preserve unrelated sessions' edits, and merge through a
reviewable PR with relevant checks. Keep the source records and interpretation
limits alongside approved models so later sessions can reproduce or revise them.

## Reusable command sequence

Run from `asset-forge` with Python (NumPy, SciPy, Pillow, trimesh, rtree; Objaverse
for discovery) and Blender available. Use the project's installed versions; record
versions with a new study. The pilot used Python 3.12 and Blender 5.1.

The following is a command template, not a recipe for automatically accepting an
arbitrary source. Replace capitalized placeholders with reviewed paths and values.

```text
blender --background --disable-autoexec --python tools/inspect_mesh_blender.py -- SOURCE.glb SOURCE_REVIEW_DIR
blender --background --disable-autoexec --python tools/prepare_creature_master.py -- SOURCE.glb MASTER.glb LENGTH_METRES
blender --background --disable-autoexec --python tools/inspect_mesh_blender.py -- MASTER.glb MASTER_REVIEW_DIR
python tools/voxelize_reference_mesh.py MASTER.glb STUDY_DIR --length-m LENGTH_METRES --long-axis 0
# Apply the species-specific reviewed geometry/color refinement here.
blender --background --disable-autoexec --python tools/inspect_mesh_blender.py -- FINAL_STUDY/colored-voxels.glb FINAL_STUDY/oblique
python tools/install_creature_pilot.py FINAL_STUDY NEW_SPECIES_ID KIND ATTRIBUTION.json
python tools/export_categories.py
python -m forge.cli selftest --quick
```

The voxelizer/refinement `write_views` step produces `six-views.png`, RGB occupancy
data and colored cubic GLB. Inspection produces the obliques. Installation checks
for those artifacts but their existence is not evidence of human visual review.
Regenerate the category index after adding entries. For web source changes run
`npm run build` from `asset-forge/web` and commit the matched production output.

## Accepted pilot recipes and lessons

Species scripts deliberately contain masks and measurements for their selected
source; they are not generic segmentation algorithms for every animal.

| Animal | Retained reproduction steps | Principal lesson and remaining interpretation |
|---|---|---|
| Grey wolf | `refine_wolf_pilot.py` → `refine_wolf_proportions.py` → `finish_wolf_pilot.py` | Coherent coat regions and justified torso correction mattered more than finer voxels. The finishing script includes legacy installation; run it only in an isolated reproduction workspace. |
| Tiger | `refine_tiger_master.py`, then voxelize cleaned master | Thirty-two tiny whisker strands became an oversized voxel moustache. Remove identified strands in the master. Source taxonomy is tiger, not a verified Bengal specimen. |
| Red deer stag | Fill reviewed combined shell, then `refine_deer_pilot.py` | Constant-material fast path avoids wasted texture queries. Smooth coat masks prevent rectangular rump patches and artificial bands. Generic deer sculpt; age and exact taxon unverified. |
| Common raven | Museum body study → `refine_raven_voxels.py` | Front-only inspection missed another source's open back. Selected museum scan needed explicit support removal and reconstructed bent legs/toes. |
| Golden eagle | Museum body study → `refine_eagle_pilot.py` | Remove fused perch; reconstruct feathered lower legs and talons. Retained white patches are interpreted as immature plumage. |
| Rainbow trout | University scan at chosen game scale → `refine_trout_pilot.py` | Preservation color needed authored restoration. Uniformly scaling a specimen does not establish adult morphometrics; spots are simplified at 52 cubes long. |
| Great white shark | Reviewed 4.2 m master → `refine_shark_pilot.py` | Countershading and gill-region color matter; retain fusiform body and fin insertions. Sex/individual measurements unverified. |
| Orca | Reviewed 6.5 m master → `refine_orca_pilot.py` | Use restrained black/off-white with correctly placed patches and saddle. Tall fin is male-like, not a documented sex/ecotype identification. |

Final study directories, saved entry IDs, dimensions and per-model limits are in
[`pilot-manifest.json`](../asset-forge/refs/reconstruction/pilot-manifest.json).
Each species folder under `asset-forge/refs/reconstruction/` retains attribution
and review evidence. Preserve the wolf refinement dependencies even though they
were named as studies: they still lead to the accepted result.

## Retired approaches and next boundary

The initial `reconstruct_pilot.py` procedural body-template generator failed the
visual benchmark and was removed after the reference-mesh workflow succeeded.
Its shared `ortho` renderer was extracted unchanged into `creature_review_views.py`.
The first `refine_raven_master.py` perch-removal experiment was also removed after
the explicit voxel reconstruction superseded it. Historical findings, licensed
reference material and Git history retain the reasons for those decisions.

Do not delete accepted-model inputs, dependency scripts or license records merely
because their names contain "study" or "pilot". Do not confuse cleanup with a new
library-wide rollout. All eight appearances are now approved; the pilot automation
is paused. Select the scope of subsequent modeling or runtime work as a separate
task, using this process and the approved set as the benchmark.
