# Shore survey route, first runs, 2026-09-10 (route captures 12 and 13)

Fixture `full-forest-low-cover-1`, spawn -153936,-81480, 48 m diagnostic ring, cache
`temperate-authored-lods-full-4`, retirement-pilot binary (retire OFF). Pins verified.

**Capture 12, `docs/temperate-route-shore-survey-pilot.json` (7 targets): FAIL, 6/7.**
`result.txt`: stalled on the last leg after 130.754 m of the 135.47 m authored line, 4.4 m short
of the last target, grounded, controller commanding 2.2 m/s with zero actual speed.
`shore-blocker-analysis.json` (the hill-route blocker method pointed at world-capture-15's
captured tree grids, exact rotated voxels at pawn height and +0.3 m) names the obstacle:
**european-beech-0012, trunk material, anchored at (-153964.1, -81368.0)**, sitting on the
authored line; the target centre itself is inside temperate-sapling-0012, beyond the 0.75 m
arrival disk, and 7 of 9 points on that disk are obstructed. Legs 0-5 are clear in the same
grids, matching the six arrivals.

**Capture 13, `docs/temperate-route-shore-survey-detour.json` (same 7 targets + 1 via): PASS,
8/8**, 136.255 m, 115/116 grounded, zero waiting, 2.2 m/s median. The via point is 4 m west of
the authored line; wp5->via and via->disk-entry had zero blocked poses in the grids before the
run. No collision, placement or approval edits, as the hill-route precedent requires.

Checkpoint images for both runs are the owner's evidence (`checkpoint-*.png`, `detour/`).
Scope: one authored pawn route each; not general shore navigability, continuous dryness (the
route file's own caveat), sightlines or building support.
