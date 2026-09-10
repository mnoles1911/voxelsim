# Oak species family and world population proposal

Requested after the oak pilot: plan small, medium and large individuals with
different sizes, silhouettes and growth patterns for a world-stamped library.
This document is a proposed next implementation, not a claim that a variant
library or runtime placement feature has already been delivered.

## Model one species with several growth forms

Keep a common oak identity: leaf-group construction, branch taper, bark,
forking language and asymmetry bounds. Separate three choices:

1. Development class selects a coherent set of proportions and branch counts.
2. Growth form selects a spatial architecture and intended placement context.
3. Individual seed supplies bounded differences within that architecture.

Do not scale a completed voxel tree to manufacture a juvenile. Choose physical
dimensions before growing the skeleton, then sample it at the target pitch.
Never use nonuniform voxel scaling or change voxel size to change tree size.

Proposed initial art ranges, not botanical age measurements:

| Class | Height | Intended structure |
|---|---|---|
| Small / young | 3–6 m | Slender bole, fewer main forks, simpler branching |
| Medium / developing | 6–11 m | More secondary branches, fuller crown |
| Large / mature | 11–18 m | Substantial bole, complex crown, heavy limbs |

The 9.9 m pilot belongs to the medium-height range but has a mature,
open-grown architecture. Height class must not be treated as chronological age.
Permit an older compact tree and a taller narrow tree in the same species.
Saplings below 3 m and exceptional veterans should be separate later cohorts.

For each class, author three recognizable growth forms:

- **Open-grown:** lower branching, broad crown, spreading limbs; clearings
  and open ground. This is the pilot's starting form.
- **Woodland:** taller clear bole, narrower crown, stronger upward structure;
  dense stands.
- **Edge-grown:** uneven crown extension toward an opening, asymmetric lower
  branching; transitions between woodland and open ground.

These are authoring goals. Their ecological placement must be implemented
and tested explicitly; the current biome fields do not automatically encode
canopy competition, a clearing edge, or a light direction.

## Parameters must interact

Expose height, trunk diameter, clear-bole fraction, crown width/depth,
scaffold count, fork heights/angles, branch curvature, taper, shoot length,
leaf-group dimensions, foliage coverage, and asymmetry.

Define profile-dependent relationships: increasing height adjusts supporting
wood and crown size; juvenile classes reduce hierarchy complexity; woodland
profiles raise the crown and constrain lateral spread. Keep leaf-group size
roughly stable in metres across size classes. More crown volume is supplied
by more branches and shoots, not inflated leaf blobs.

Individual seeds vary fork positions, missing/shortened branch sectors,
curvature, crown balance, sparse regions and moderate lean. Avoid unconstrained
independent random sliders that can produce a giant crown on a thin trunk.
Broken limbs and veteran features should be deliberate optional forms, not
frequent accidental defects.

Use distinct deterministic random streams for architecture and foliage so a
leaf-density adjustment does not reroll the tree's trunk. Record generator
version, resolved parameters, source seed and content hash with each bake.

## First review batch

Generate four candidates for each of three size classes and three forms:
**36 individuals**. Review in a 3x3 class/form matrix, each with the same
1.80 m player reference, followed by multiple angles and leaf-off views.
Select about two per cell for an initial **18-tree curated bank**, retaining
more only when they add a distinct useful silhouette. Those counts are targets,
not automatic approvals. A mixed grove view is necessary: individual thumbnails
can conceal repeated shapes and awkward spacing.

Build the initial shipping candidates at 100 mm. Use 50 mm on representative
small/medium/large examples to establish whether the extra foliage detail is
useful. Pitch variants share a metric master and individual identity; they
are not additional biological variants.

## Connect to existing Forge and world contracts

The pilot is an imported draft. `forge.manifest.kept_seeds` explicitly excludes
imports, and `export_banks.py` regenerates from the procedural pipeline. Merely
duplicating the saved pilot directory cannot deliver a working seed bank.

1. Move the oak master into a versioned generator module and provide an
   explicit oak-family opt-in through spec validation and `pipeline.build`.
   Default behavior and hashes for other species must remain unchanged.
2. Add class/form presets and a batch interface in Forge; keep the familiar
   Generate -> review -> Keep workflow. Preserve the prototype as a baseline.
3. Represent the nine class/form combinations as related authoring profiles,
   all displayed under the oak family. Initially they can compile to separate
   engine species records while sharing family identity in the UI. This avoids
   needing a new runtime variant-weighting format for the first library.
4. Assign each profile appropriate biome weights, spacing and a supported
   placement layer. The current manifest assigns layers from nominal height
   and spacing at species-record granularity; actual bank extents are checked
   separately. Do not put 3 m and 18 m trees in one record without reconciling
   layer bounds and spacing. Measure every kept tree's crown radius and height.
5. Bake only kept/approved individuals using the existing publisher. Include
   generator-version/content freshness in the bake contract: a spec-only hash
   does not establish that generator code is unchanged.
6. Verify a deterministic test grove in the engine, including terrain joins,
   collisions, chunk boundaries, slope burial, canopy footprint and repetition.

Current terrain tree admission is 100 mm. Enabling 50 mm world stamps requires
an explicit terrain/LOD integration decision; keep 50 mm studies offline until
that is resolved. Do not change a tree's category to bypass pitch validation.

## Population selection

Desired pipeline: placement context -> oak family -> eligible class/form ->
kept individual -> supported orientation -> voxel stamp. Choose reproducibly
from the world seed and placement coordinate, with stable asset identifiers.

Use correlated stand composition rather than equal random selection of every
tree everywhere: clusters of similar development class, occasional smaller
trees between mature specimens, broad open-grown oaks in openings, and
asymmetric crowns near edges. Start with existing biome/spacing controls;
local cover/edge detection and directional crown orientation are additional
runtime work, not capabilities assumed to exist.

Prefer native-lattice quarter-turn orientations for direct stamps unless the
engine already supports another validated transform. Arbitrary rotation and
runtime scale should not be relied on to hide a small bank: they can alter
voxel connectivity or require resampling. Variation belongs in the authored
geometry first.

## Acceptance gates

- Visibly distinct individuals that still read as the same oak species.
- Small trees have simpler architecture rather than miniaturized old limbs.
- Leaf groups remain supported by shoot paths at both pitches.
- Face-connected structural wood, attached asset, ground contact, and no
  significant geometry discarded by cleanup.
- Repeat builds and export round trips preserve occupancy, origin and pitch.
- All exported profiles fit their assigned layer and streaming bounds.
- Placement, appearance approval and annotation edits do not reroll geometry.
- A representative grove passes visual review and measured runtime budgets.

The recommended next deliverable is the generator/profile integration plus
the 36-candidate review sheet. World publication follows curation and the
engine grove check, not generation alone.
