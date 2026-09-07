# Creature reconstruction pipeline

## Target and completion boundary

The user accepted the wolf displayed in Forge at 12.5 mm on 2026-09-07.
Anatomy must be convincing; restrained stylization in fur, markings and expression
is welcome. Finish seven other static pilot animals before asking for review.
Do not pursue Tripo/AI 3D service comparisons (latest user direction).

## 1. Define the animal before selecting art

Record scientific name, adult/juvenile, sex when relevant, seasonal coat, pose,
and a plausible physical scale. Avoid mixing a stag's winter neck with summer
color, a juvenile eagle's markings with an adult, or male and female orca fins.
Population ranges constrain plausibility; they are not measured dimensions of
the source specimen. Record measurement endpoints (body versus nose-to-tail).

## 2. Keep evidence and asset provenance separate

Use primary biological research and institution species accounts to identify
landmarks and diagnostic features. Follow citations when actual quantitative
anatomy is required. Reference photographs constrain shape and color but are not
automatically licensed for redistribution. Record their individual rights.
Publicly viewable photographs can be consulted without vendoring them.

For source meshes, save title, author, source URL, individual license, original
description, download date and file hash. The Objaverse archive license does not
license each asset. `find_creature_sources.py` caches metadata and creates
CC BY/CC0 discovery sheets. Its shortlist is not automatic clearance: exclude
recognizable game rips, mislabeled species, toy scans and incompatible poses.

## 3. Inspect the continuous master

Inspect scene transforms, axis conventions, skins, disconnected meshes, ground
planes and annotations. Render six oblique views covering both hemispheres before voxelization. Select
geometry explicitly; never let text or display stands become part of the animal.
Check muzzle/bill, orbit placement, chest and pelvis, joint bends, appendage
counts, contact stance, tail/fin/wing shape. A good front preview is insufficient.

Use uniform scale for physical size. Any local deformation needs a specific
anatomical rationale and a before/after review. Do not force unrelated population
averages onto an individual by anisotropic scaling.

## 4. Convert at the authored pitch

Keep a continuous master and a true cubic occupancy grid at 0.0125 m. Voxelize
after geometry corrections. Texture filtering uses a footprint in linear light;
author species coat regions where source textures have noise, lighting or wrong
markings. Thin feathers, toes, antlers and fins need connectivity checks and
explicit local decisions. Never silently coarsen the final model.

## 5. Review the result, not just the input

Render six orthographic views, six obliques and a game-distance view. Comparisons
must match physical pixel scale or explicitly state that they are fit-to-frame.
Inspect face, feet and appendage attachment closeups. Compare diagnostic features
with the evidence record; note unresolved issues. Numbers cannot grant visual
approval. Check connected components, occupancy, lattice pitch, bounding scale,
appearance-to-cell alignment, and VXA roundtrip. Review the actual Forge viewport.

## 6. Save a reviewable model

Save a separate imported draft with VXA, VOX, full-color GLB, RGB surface sidecar,
thumbnail and attribution. Preserve the old species baseline. Forge's RGB1 trailer
displays authored colors; VXA engine colors still use the shared material palette.
State rigging/runtime limitations. The orca may need a sparse or assembly solution
for engine limits; research chunking is not runtime completion.

## 7. Make the pipeline reproducible

Record selected inputs and hashes, script arguments, modifications, rejected
alternatives, actual review findings and final artifact paths in a per-species
record. Commit completed files and update `pilot-manifest.json` and the handover.
After all eight models are ready, present them together for user review. Only
then consider expanding to the remaining library, incorporating review lessons.

## Observed failure modes

- Tiger whisker strands became a thick voxel moustache. Remove explicitly
  identified sub-voxel strands in the master instead of deleting arbitrary cells.
- The Korppi raven scan looked intact from the original front camera sweep but
  had an open back. Rotating the master exposed it; a direct transform and new
  opposite-side source renders confirmed this was source incompleteness.
- Scans split across mesh chunks may require filling their combined shell.
  `--fill-union` is explicit and cannot repair a missing exterior surface.
- HTTP 429 means incomplete discovery. Stop requests and report deferred records;
  do not interpret an empty shortlist as absence of licensed sources.


## Completed pilot lessons

All eight static pilots are now delivered for set review; see pilot-manifest.json.
A museum scan can preserve anatomy while having a fused perch, distorted feet or
preservation colors. Remove supports with explicit anatomical masks and document any
reconstructed anatomy. Never treat largest-component selection as anatomical repair.
The raven and eagle have authored articulated feet; the trout has authored living colors.

At 12.5 mm a 0.65 m fish is only about 52 cubes long. Fine scales, spots, toes and
feather barbs cannot be literal photographic detail. Keep silhouette and diagnostic
landmarks first, then use restrained color. Scaling a specimen does not establish
adult morphometrics. Generic mesh species/subspecies labels require explicit caveats.

Constant PBR materials can return one RGB(A) value instead of one per UV. Broadcast
that result and bypass expensive nearest-triangle texture queries when every mesh
shares the same constant material. For complex textured scans, small query batches
bound nearest-surface memory. Preserve RGB sidecars: palette VXA alone is not the
reviewed full-color appearance. Appearance approval and runtime publication are distinct.
