# RungProbe: refuted at the engagement gate, both barrels

2026-08-29. `voxel.March.RungProbe` stays default 0 permanently. One leg
(RP-eng-sky2, static sky +30, spawn -61440,-61440, HoleStats 1, view 1280x720)
settled it; no timing pairs were owed and none were run.

## The two readings, either of which kills it alone

    engagement:  rungProbe: probed=53,514,957 = skipped 4,509,583 (8.43%)
                 + walked 49,005,374 | bits=70,604,449 (1.32 per probed rung)
                 pre-registered expectation: skipped/probed >= ~95%.  MISS x11.
                 (two deferred prints converged: 8.43% / 8.42%; the
                 probed = skipped + walked partition held exactly on both.)

    cost:        marchMs 173.7 vs ~5.8 control at the same pose and stats level.
                 The leg ran at ~175 ms/frame END TO END, GPU-bound
                 (renderMs=175, rhiMs=2, gameWaitMs=173). The probe DDA runs
                 4.46M times per frame; even a "cheap" pre-test at that
                 frequency is a ~30x slowdown.

## Why the rate is 8%, not 95% -- the hypothesis was wrong, not the instrument

The 90% sky retry rate with 0.054% hits suggested retry rungs cross empty
space. They do -- but "empty" and "non-resident" are different predicates, and
the probe can only read residency. The buried-skip retirement made sky air
RESIDENT WITH RECORDS, so `MarchBlockOccupied` (a verified superset of
residency) reads occupied across 91.6% of retry intervals, mean 1.32 occupied
blocks each. The 0.054% hit rate stands; residency bits simply cannot prove
those retries useless. **The exact-skip route through residency structure is
closed.** Any future attack on the retry rate needs either a SOLIDITY-based
structure (which reintroduces the hole-safety problem the ladder exists to
solve) or a wave-coherent ladder gate -- and the per-lane clamp family is
already dead (Bound, ZTight, anySolid).

## The rule this extends

"Nothing that adds a per-step test can pay inside this ring cascade" already
covered per-segment loads (bound arm). It now covers per-RUNG probes: retry
rungs run at millions per frame at sky, and the multiplier eats any per-unit
cost. The probe was hoisted above rung setup, early-outs on first set bit,
and reads a bitfield already resident in cache-friendly form -- and still cost
30x. Do not build a v2 with a cheaper probe; the multiplier is the problem.

## What the arm leaves behind

- The doctrine block at the probe site (residency-vs-solidity, the BlockSkip
  freshness contract) is correct and documents a real trap for any future
  consumer of `MarchBlockOccupied`.
- Counters (slots 69-72, exact partition) are sound instruments if a future
  arm wants rung-level attribution.
- Code stays in-tree behind the define, control path byte-identical, refused
  vs SkyLadder at compile time. Commit eb0c336.
