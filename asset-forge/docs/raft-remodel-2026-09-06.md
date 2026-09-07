# Temperate log raft remodel

The existing `raft` stays a draft vehicle at **25 mm cubic voxel pitch**, with
a nominal 4 × 2 m plan. This is separate from the approved bamboo raft.

Seven logs now retain rounder individual profiles: packing changed from 15%
radial overlap to 3%, the centrelines no longer force every log to the same top
height, and alternating taper is gentler. Bark coverage is predominantly dark
spruce-like plates, with interrupted longitudinal furrows and one heavily
peeled log. Existing branch collars, uneven cuts, sapwood, pith and checks remain.
Two weathered 14 cm cross-poles have three rope turns and compact tightening
knots at all fourteen crossings. Material IDs and the engine palette are unchanged.

The visual references are artistic guidance, not an engineering claim:

- [Daniel Carter Beard, Boat-Building and Boating](https://electriccanadian.com/pioneering/beard/boatbuilding.pdf)
  describes pine/spruce logs and crosspieces in the raft chapter.
- [Oregon Historical Society, Log rafts](https://digitalcollections.ohs.org/log-rafts-2)
  documents rough timber on a working river raft.
- The earlier photograph and wood-weathering references remain in
  [the previous raft notes](raft-2026-09-05.md).

## Reproducible review and checks

Run `python tools/raftprobe.py` from `asset-forge`. Seeds 1–4 each produce one
face-connected component, clean pipeline health and zero repair bridges.
Repeated seed 1 is byte-identical; all four seeds differ. VXA read-back preserves
the complete material grid and 0.025 m pitch. Both VXA and VOX are exported.
Seed 1 has 139,476 occupied voxels and bounds 4.225 × 2.05 × 0.50 m.
Central deck coverage is 99.87%; its 5th–95th-percentile height relief is 12.5 cm.

The probe now permits narrow seams between circular logs (minimum 94% plan
coverage) and measures the central 90% of surface heights (maximum 25 cm relief),
instead of treating every seam floor as the walking surface. Connectivity still
uses strict six-neighbour adjacency. Player movement/buoyancy are outside this
asset-generation check.

Actual-model renders, with the same renderer and no generated illustration:

- Before: `.scratch/raft-remodel-before/raft-0001-review.png`
- After: `out/artifact/raft-0001-review.png`
- Opposite end: `out/artifact/raft-0001-end-review.png`
- Numeric evidence: `out/artifact/raft-validation.json`

The original generator, spec, VXA, VOX and statistics are preserved under
`.scratch/raft-remodel-before/`. No log-raft library entry existed, so the asset
remains a spec and review export awaiting an owner verdict; bamboo is untouched.
