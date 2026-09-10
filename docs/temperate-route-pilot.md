# Actual-terrain route pilot (bounded route evidence)

The opt-in route mode uses the actual pawn collision/movement controller at
2.2 m/s, steering toward fixed XY waypoints. It never teleports, clears geometry,
or flattens terrain. The sample is derived from world-capture-13-low-cover survey
coordinates and ends at a screened building candidate; connectivity and ground
support are not established. It is approximately 175 m and does not represent
all required habitats or multiple seeds.

Launch in a fresh UE process with the same asset/configuration, terrain provider,
spawn and spring settings as the ecological walking test, replacing
`-VoxelWalkTest=1 -VoxelWalkWaitForEcology` with:

```
-VoxelEcologyRoute=D:/voxelsim/docs/temperate-route-pilot.json
-VoxelEcologyRouteSha256=0d058ebb798740684b26b687230070cfea2cedbf40575543094b88823a0d4642
-VoxelEcologyRouteOutput=<fresh absolute directory>
```

Required existing arguments include `-VoxelAssetDir`, `-VoxelEcologyConfig`,
`-VoxelSpawnAt=-154740,-81476`, and the same isolated user/cache settings as the
baseline. The current ecological-walk-validation.ps1 expects the old mechanics
completion markers and is not a route-mode runner. Do not use its success
criteria for this mode. Preserve the full invocation and terrain identity in the
run record; the route itself currently pins configuration, species manifest and
route bytes, not the terrain cache.

Startup allows at most 1,800 seconds for services, streaming and grounding; each
leg allows 180 seconds, with failure after 15 seconds without at least 0.25 m
improvement in distance to its target. Overall timeout is 3,600 seconds. This
simple driver has no pathfinder or automatic detours: failure means this authored
polyline was not demonstrated traversable, not that no nearby human route exists.
Arrival tolerance is 1.5 m. A checkpoint stops movement and requests a screenshot;
the two-second screenshot phase is outside WALK_BEGIN/WALK_END windows.

Outputs: pinned route.json, route-samples.csv (twice-second position, terrain,
movement and sampled frame duration), checkpoint images and result.txt. Only
`VoxelRoute COMPLETE PASS` plus matching final hashes and complete artifacts is
local route success. The sampled frame duration is diagnostic, not a frame-time
percentile capture. No line-of-sight verdict is emitted: world voxel raycasts omit
noncolliding understory. No building support verdict is emitted.

Default mechanics mode is unchanged. Runtime validation must additionally check
camera steering, screenshot completion, configured spawn and a deliberate
blocked waypoint/timeout before relying on route PASS results. Broader acceptance
still requires ridge, hollow, closed stand, gap, edge and shoreline routes across
seeds, understory-aware visual review and supported/accessed building endpoints.

CSV velocity columns now distinguish `controller_speed_m_s` (the mover's internal
velocity, which may remain nonzero against an obstacle) from `actual_speed_m_s`
(horizontal displacement between consecutive sample positions divided by their
wall-clock interval). The latter is net interval displacement, not path length
around turns. Both sample baselines reset on entry/re-entry to walking, so startup
falling and screenshot pauses are excluded. The first sample is emitted after
at least 0.5 seconds of walking.

## September 10 validation and profiling

Use `Tools/ecological-route-validation.ps1` for current captures. It pins the
route, configuration, inventory, optional mesh cache and runtime modules, and
checks completion. Route 4 reached its ten authored detour waypoints and recorded
a bounded foundation survey. Route 5's hill polyline failed before its first
waypoint. These outcomes are deliberately kept separate: passing one route does
not certify another habitat or every straight line through a forest.

Build 58 adds opt-in `-ProfileFrames` on the wrapper, passed to the engine as
`-VoxelEcologyRouteProfile`. The profiler starts after settling and grounding,
labels every route tick with walking state, target point, relative elapsed time
and capture frame index, and waits for asynchronous CSV export before quitting.
The file is `results/route-frames.csv`. Screenshot and transition frames are
marked inactive; the ordinary twice-second trajectory CSV remains separate.

`asset-forge/tools/analyze_ecological_route_frames.py` verifies the underlying
route evidence, capture completion, independent event intervals and frame clock,
then reports movement timings from active-labelled frames. Relative timestamps
avoid losing subsecond precision to the large platform-clock offset. The labels
describe the route tick and do not establish individual CPU/GPU causal chains.
The parser and analysis regressions pass; actual profiled-route export and
runtime analysis remain pending. Profiling is disabled by default.
