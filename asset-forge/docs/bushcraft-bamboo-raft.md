# Photo-inspired bushcraft bamboo raft

Created from the owner's supplied lake-raft photograph. A separate species,
`bushcraft-bamboo-raft`, keeps the previous rough timber raft available.

The nominal deck is 4 x 2 metres with 16 close-packed bamboo culms. Each has
its own node spacing, slight taper, bend and end stagger. The culms have hollow
interiors and solid node diaphragms. Two under-deck bearers, two upper crossbars,
two upright posts and short side pegs carry the photo's silhouette. Rope turns,
knots and short tails are modeled in voxels. Colour uses existing pale tan,
warm wood and weathered grey palette slots.

All geometry is cubic at 25 mm pitch. Seed 1 has 52,214 occupied voxels and
measures 4.225 x 2.45 x 1.50 m including crossbar overhangs and posts.
The photograph does not establish exact dimensions; these are game-scale
authoring choices. This is geometry, not a buoyancy or player-collision change.

## Files and regeneration

Run `python tools/bambooprobe.py` from asset-forge. It writes into `out/artifact`:

- `bushcraft-bamboo-raft-0001.vxa`: game asset, embedded 25 mm pitch.
- `bushcraft-bamboo-raft-0001.vox`: editable MagicaVoxel model.
- `bushcraft-bamboo-raft-0001.obj` and `.mtl`: exposed cubic faces, metres,
  Z up, centred horizontally. Keep both files together when importing.
- `bushcraft-bamboo-raft-reference-view.png`: view from the open end.
- `bushcraft-bamboo-raft-validation.json`: measurements for four seeds.

The OBJ uses flat faces and no smoothing; it is not a rounded approximation.
Its 79,216 quads include the exposed interiors of the hollow bamboo culms.

Seeds 1–4 pass pipeline health, face connectivity without repair bridges,
deck coverage, node-count and post-height checks. VXA read-back preserves all
voxel materials and pitch. Repeated generation is deterministic and all four
seeds differ. Existing species hashes are unchanged. Matt approved seed 1 on
2026-09-06; it was saved through Forge's normal Keep path to the local library
as `bushcraft-bamboo-raft-0001` at 25 mm pitch.

The `bamboo_raft` form reuses artifact dimensions: depth is culm diameter,
thwart thickness is crossbar diameter, frame radius is post radius, frame drop
is post height above the deck centre, and deck fraction locates the crossbars.
