# Creature reconstruction pilot checkpoint

## Latest continuation: wolf coat study

The user explicitly supported the process and requested continuing the wolf.
`asset-forge/tools/refine_wolf_pilot.py` now generates a separate reference-informed
coat candidate from the previous source study. See
`asset-forge/refs/reconstruction/grey-wolf/coat-study-review.md` for visual findings
and the newly identified shoulder-height discrepancy. The comparison lives at
`asset-forge/out/creature-reconstruction/grey-wolf-coat-study/before-after.png`.
This is an appearance improvement candidate; anatomical approval remains open.

Exact occupied cells and surface indices match the prior study; six projections
from the refactored exporter are pixel-identical to the baseline, and exterior
triangle count/winding checks pass. The inspection studio now uses fixed -1.5 EV
exposure to avoid washing out the coat. Re-render older oblique images under the
same exposure for a fair comparison. Orthographic RGB images remain unlit.

The user accepted the anatomy-first eight-species pilot and explicitly made
anatomical and visual realism the primary acceptance criterion. Continue
autonomously; do not ask for approval of ordinary experimentation.

## Completed experiments

- Added the eight-species manifest, pinned BSD-3-Clause Infinigen templates,
  public-domain wolf photograph, source provenance and explicit visual reviews.
- Generated eight 12.5 mm procedural candidates outside the saved library.
  They did not establish the required realism. The stag has detached antler
  sections; the orca requires research tiles beyond the current dense-body limit.
- Examined three freely licensed wolf meshes. The sculpt has stylized fur;
  the rigged wolf has weak paws, bright oversized eyes and noisy coloration;
  the educational wolf has obvious polygonal anatomy and layered fur cards.
  None qualifies as an approved master.
- Implemented texture-preserving mesh voxelization with scene transforms,
  explicit scale assumptions, connected-component reporting, six orthographic
  views, filtered RGB sampling and actual exposed-cube GLB geometry for oblique
  inspection. Corrected the glTF vertex-color linear/sRGB conversion.
- The wolf experiment produces 101,602 occupied voxels at 12.5 mm, one component,
  14,752 surface voxels and 49,296 exterior triangles. Data alignment, uniqueness,
  pitch and outward winding checks passed. These are technical checks only.

## Files and output

Reproduction and per-source reviews:
`asset-forge/refs/reconstruction/README.md`.
Tools: `reconstruct_pilot.py`, `voxelize_reference_mesh.py`,
`inspect_mesh_blender.py` under `asset-forge/tools`.
Ignored generated output:
`asset-forge/out/creature-reconstruction/`.
The most recent full-color wolf comparison is in
`grey-wolf-source-study/six-views.png`; actual cubic oblique renders are in
`grey-wolf-source-study/oblique/view-*.png`.

## Outstanding work

No pilot model is visually approved or ready to replace the wildlife library.
The next meaningful step is a better evidence-based wolf master, correcting
proportions and coat regions against the real reference dossier. Do not spend
another bulk pass on generic primitive assemblies or declare a marketplace mesh
realistic from its title. Expand across the other seven pilot species after
demonstrating a convincing result.

Current VXA exports use shared material IDs and cannot preserve research RGB
appearance. A deliberate runtime appearance solution remains necessary.
Animation/skin transfer and biological joint validation are also unfinished.

The existing 382 wildlife outputs are baselines, not newly approved art.
Full publisher resync currently interprets many generated library entries as
kept; resolve explicit draft curation before running it. Environment banks also
have pending stale-output/version obligations outside this pilot.

## Integration coordination

The separate task “Improve voxel raft asset” owns PR #234 and the isolated
`codex/environment-integration-2026-09-07` checkout. It incorporated earlier
goblin removal and approved craft-body loading work. It also fixed the generated
palette source comment to use a repository-relative path. Preserve that fix.
Do not stage unrelated water, terrain or environment changes from the shared
checkout. This pilot checkpoint does not claim PR #234 is merged.
