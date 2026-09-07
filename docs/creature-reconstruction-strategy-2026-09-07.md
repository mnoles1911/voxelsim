# Creature reconstruction strategy — 2026-09-07

The owner rejected the current visual quality and asked for a fresh research-based
approach. This supersedes the assumption that a successful bulk regeneration
constitutes a successful realism improvement. No new reconstruction system has
been implemented or benchmarked yet.

**Owner's primary acceptance rule:** every model must appear anatomically and
visually correct/real. A technically valid model that looks wrong fails. Compare
proportions, pose, species-defining anatomy, surface transitions and coloration
against real evidence in multiple views. Automated metrics support this decision;
they do not replace visual inspection or establish biological correctness.

## Findings from the actual saved models

Inspected grey-wolf, bengal-tiger and common-raven thumbnails, plus the quadruped
generator and material definitions. The wolf/tiger torsos read as long boxes;
head and neck transitions are abrupt; tiger stripes remain repetitive vertical
bands. The raven lacks convincing separation of its anatomical and feather
forms. These are visual observations, not measured biological diagnoses of all
382 species. Existing determinism/export/rig tests establish technical validity,
not realism. Higher voxel density cannot fix the wrong underlying anatomy.

## Recommended pipeline

Reference dossier -> anatomical surface model -> calibrated base color ->
12.5 mm cubic occupancy/color sampling -> rig transfer -> multi-view comparison.

1. Resolve each species to a scientific name and a chosen adult sex, season and
   neutral pose. Store specimen IDs, measurements, source URLs, image licenses,
   camera estimates, masks, landmarks and confidence. Do not combine juvenile,
   adult, breeding and nonbreeding proportions into one supposed specimen.
2. Prefer a usable licensed surface scan when available. Otherwise fit a
   family-specific articulated mesh to photographs and measured landmarks.
   Build separate families for canids, felids, ungulates, birds and aquatic
   shapes rather than stretching one generic creature form.
3. Establish skull, rib cage, pelvis and joint positions before skin. Fit chest
   depth, abdominal tuck, shoulder/hip volumes, muzzle, ears, paws/hooves and
   tail root explicitly. Bird templates need beak sections, folded-wing
   envelopes and feather groups; fish need cross-sections and fin insertion
   landmarks. Templates should supply anatomy where images cannot constrain it.
4. Author a continuous textured master in real units. Fit silhouettes and
   landmarks from consistent camera views; retain uncertainty where coverage
   is missing. Do not mistake generated unseen views for biological evidence.
5. Sample base color separately from illumination. Use body-region masks,
   anatomy-following pattern coordinates, and controlled variation. Avoid
   projecting photographic shadows/highlights into permanent voxel colors.
   Current shared material IDs restrict the available colors. Evaluate a
   separate appearance palette alongside stable gameplay material IDs before
   expanding the binary/rendering contract. Do not globally recolor shared IDs.
6. Convert the master into actual cubic occupancy at 0.0125 m, sampling texture
   color across each exposed voxel footprint. Preserve watertightness, slender
   appendages and part ownership. Transfer rig weights/labels from the master.
   Blender's voxel remeshing alone is not the game's colored cubic export.
7. Use sparse/chunked generation for large creatures instead of silently
   dropping resolution because a dense temporary box is too expensive. Keep
   finest authoring data and derive distance LOD separately.
8. Compare source and output in six orthographic views plus oblique turntables,
   under matched neutral lighting. Inspect both plain grey shape and unlit
   color; then inspect game lighting at actual encounter distances. Measure
   landmark and silhouette errors, but retain visual assessment: a good
   silhouette score cannot certify muscles, faces or correct coloration.

At 12.5 mm, one metre spans 80 cells; a 20 cm animal spans 16 cells. A 5 mm
feature is sub-voxel. Accurate proportions and coherent colors remain possible,
but individual hairs, fine feather barbs and tiny eye anatomy cannot be explicit
cube geometry at that pitch. Do not enlarge eyes to compensate.

## Six-view reconstruction

Six aligned views of the SAME model/pose are useful modeling constraints. Six
unrelated internet photographs differ in pose, individual anatomy, perspective,
scale and lighting. Use those as reference evidence, not as a calibrated scan.
Left/right symmetry can constrain unobserved shape but does not recover unique
markings. Belly/top references will sometimes be absent. Silhouette intersection
recovers a visual hull, not every concavity; joint recesses, mouth interiors and
other hidden surfaces still need depth information or anatomical priors.

[COLMAP's capture guidance](https://colmap.github.io/tutorial) requires overlapping
views of the same object. Its conventional reconstruction assumes consistent
scene geometry; moving animals need synchronized capture or a deformable model.
[Laurentini's visual-hull research](https://iris.polito.it/handle/11583/1401917?mode=full)
explains the limits of silhouette-derived shape.

## Useful sources and tools

- [oVert / MorphoSource](https://www.floridamuseum.ufl.edu/blackburn-lab/research/morphological-diversity-evolution/):
  over 13,000 CT-scanned vertebrate specimens. Use skeletal and sometimes soft
  tissue geometry; these are not automatically colored living-animal surfaces.
- [DigiMorph](https://www.digimorph.org/): CT anatomy and specimen imagery useful
  for skull and structural proportions. Inspect each item's reuse conditions.
- [AVONET](https://onlinelibrary.wiley.com/doi/10.1111/ele.13898): bird measurements
  including beak dimensions, wing, tarsus and tail. These constrain models but
  do not supply complete surfaces; some species values are inferred.
- [FishBase](https://www.fishbase.se/search/home.php) and
  [Fish-Vista](https://arxiv.org/abs/2407.08027): fish morphology and annotated
  trait imagery. Use to constrain fin placement, shape and diagnostic regions.
- [Smithsonian Open Access](https://www.si.edu/openaccess/faq),
  [Wikimedia Commons](https://commons.wikimedia.org/wiki/Commons:Reusing_content_outside_Wikimedia/en)
  and [iNaturalist](https://help.inaturalist.org/en/support/solutions/articles/151000173511):
  specimen imagery and live appearance references. Retain per-image licensing;
  freely viewable does not mean freely reusable in a commercial asset. Prefer
  CC0/CC BY material for direct texture/reconstruction inputs.
- [SMAL](https://smal.is.tue.mpg.de/): research on articulated animal shape
  fitting. Useful methodological reference; model/data licensing is a separate
  gate from publication access.
- [3DAnimals / MagicPony / 3D-Fauna](https://github.com/3DAnimals/3DAnimals):
  open code for articulated reconstruction from animal images. Candidate
  initialization and fitting tools; published coverage does not demonstrate
  accurate reconstruction of every species in our library.
- [TRELLIS.2](https://github.com/microsoft/TRELLIS.2): image-conditioned 3D
  generation with shape/appearance output; candidate prototype accelerator.
  Generated hidden anatomy must be checked against evidence. Its representation
  still requires conversion into this game's cubic voxel/material/rig format.

## First benchmark before another bulk pass

Use grey wolf, Bengal tiger, red deer, common raven, golden eagle, rainbow trout,
great white shark and orca. These are proposed representative cases, not a claim
that a new art direction is already proven. Compare the current generator,
anatomical template fitting, and an image-to-3D candidate where practical.

Deliver source dossier, measured master, 12.5 mm export, six-view comparison,
turntable, color error assessment and generation/runtime cost for each pilot.
Expand family by family only when the pilot visibly improves anatomy and color.
Keep the old library as a baseline. Do not call regenerated outputs approved
merely because a script wrote them successfully.
