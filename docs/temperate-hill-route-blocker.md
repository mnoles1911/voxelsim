# Hill route 5 obstruction and candidate detour

The failed run reached zero of seven waypoints and travelled 4.697m. It stopped
at approximately (-154051.499, -81114.899, 99.301)m, grounded and not waiting for
terrain. Actual displacement speed was zero despite controller speed2.2m/s.

`tools/analyze-route5-hill-blocker.py` reproduces the tree-only geometric screen.
Set PYTHONPATH to `.scratch/oak-python;asset-forge;asset-forge/tools` (absolute
paths recommended), then run it with the bundled Python. The report is
`asset-forge/out/ecological-placement/hill-blocker-analysis-1/report.json`.
It pins the captured placement, run records, movement samples, configuration,
species manifest and every source bank actually screened. Radius broad-phase
selection uses each variant's declared bounds before decoding its100mm grid.
The body is0.6×0.6×1.8m, with the movement code's30cm raised retry.

At the recorded position, the current body is clear of captured tree voxels.
A2cm negative-Y movement intersects five broadleaf material19 cells from
hawthorn-scrub-0014, bank62/slot2/yaw3. The same five cells block the30cm raised
retry. Their lattice coordinates are y=-811153,z=999 and
x=-1540517,-1540516,-1540514,-1540513,-1540512. This establishes an actual
foliage obstruction; proximity to a tree alone was not used as evidence.

The west-only2cm probe is tree-clear, yet X also eventually stops at a voxel
boundary. Its cause remains unresolved: this script does not query live terrain,
caves or edits. A stopped live WorldQuery should inspect the west leading slice
x=-1540518, y=-811152..-811146, z984..1002 and raised z987..1005, recording composed
and amplifier-only material IDs. A foliage Y obstruction plus a small terrain X
step could prevent the existing joint X/Y step retry, but that remains a hypothesis.

The original first target center intersects44 foliage voxels at the assumed
height. Its existing0.75m arrival disk has a clear west-side approach. The new
`docs/temperate-route-hill-detour-pilot.json` therefore inserts two points,
(-154053,-81114) then(-154053,-81116), each radius0.15m, before all seven unchanged
original waypoint objects. The original route remains intact. The detour route
hash is850a25f9c922e067889afc2e2d73f1aead9b3b82b189784e263c9b22cd76785c.

Polyline samples every25mm from the original spawn through those points to
(-154052.7,-81116), inside the original arrival disk, hit no tree voxels at body
center heights99.301 and99.601m. These are exact grid intersections at sampled
poses, not a swept-volume proof. Actual terrain height, corner cutting inside
arrival disks, ground support and all later route segments still require runtime
validation. No terrain, forest asset or original waypoint was removed to make
this candidate.
