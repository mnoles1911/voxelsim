# Creature reconstruction pilot checkpoint

## Active checkpoint: accepted wolf, seven remaining pilots

LATEST: the desktop shortcut now checks a server startup source fingerprint and
restarts verified stale Forge listeners. Port 8731 is current (PID 7324 at test);
use this standard port for review, not the temporary 8747 server. Direct links:
`http://127.0.0.1:8731/?species=grey_wolf_anatomy_pilot` and
`http://127.0.0.1:8731/?species=bengal_tiger_anatomy_pilot`.
The Library defaults to Never reviewed, hiding draft imports; direct links
select the model and Any verdict. User requested this access fix while pilot
work continues. Second launcher run reused the same current PID.

Tiger candidate is installed with 210,538 voxels and RGB, one component, at
12.5 mm. Whisker cleanup removes 32 specific source strands. See new tiger
review and attribution under `refs/reconstruction/bengal-tiger`. Remaining
Bengal-specific proportion checks are explicit; do not claim set approval.

Bird checkpoint: read `refs/reconstruction/bird-source-review.md`.
CC0 raven MP 040 and eagle MP 407 are downloaded and prepared. Raven perch
removal remains experimental with visible support fragments; not installed.
Alternative Korppi raven was rejected for an incomplete back confirmed in
expanded original-source views 4/5. The preparation transform is not the cause.
Inspection now covers both hemispheres. Combined-shell fill is opt-in and passed
a split-cube test (729 occupied, 386 surface); it cannot close missing anatomy.
Source discovery stops on HTTP 429 and reports incomplete results. Prior shark
zero shortlist was rate limiting, not absence of sources.

The user approved the finished static wolf in Asset Forge (commit `2befc8f`)
and requests independent overnight completion of the other seven animals.
The latest direction explicitly sets Tripo/AI 3D services aside. Continue the
wolf method, using `asset-forge/refs/reconstruction/PIPELINE.md`.
An hourly thread heartbeat `finish-creature-anatomy-pilot` is active.
Do not wait for creative feedback; present the complete set for review.

Wolf entry `grey_wolf_anatomy_pilot-0001` is saved with full RGB sidecar and
CC BY attribution. Forge on port 8747 was visually verified. Shared-palette
VXA runtime appearance and rigging remain separate. The user's acceptance is
visual, not permission to label runtime work complete.

Tiger discovery has cached 66 source records and three license-filtered sheets
under `out/creature-reconstruction/research/tiger-*`. Deer discovery likewise
has 58 records and three sheets. `tools/find_creature_sources.py` reproduces
discovery; it does not approve sources. Three tiger GLBs downloaded: `10cf9935...`
(daniel.jibi, walking pose, promising), `fc2c7fd...` (Amil, running pose), and
`9488609...` (Jai.Gupta, not yet rendered). First two have four-view Blender
renders under `source-meshes/tiger-10cf-review` and `tiger-fc2-review`.
Avoid `ac718cd...`: description says 2D-to-3D Monster Mash. Other thumbnails
include toy scans and game characters, which are unsuitable starting masters.

Scene transforms and skin evaluation differ between raw trimesh and Blender
on these sources. `tools/prepare_creature_master.py` bakes evaluated static
geometry, retains UV/materials and normalizes heading; inspect its PCA heading
before using the resulting GLB with the existing voxelizer.

## Historical checkpoint: wolf coat study (superseded by accepted wolf above)

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

## Historical outstanding work (superseded by active checkpoint above)

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
