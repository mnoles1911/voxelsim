# WarmShadingAsync: the machine works, the map is wrong; default stays 0

2026-08-29. One armed leg (WSA-armed-a, async=32) against the day's control
family. The arm removed the inline warm's capacity ceiling (0.25 ms/tick
bought <1 cold fill/tick; workers bought ~200/window) and every mechanism
gate passed:

    asyncLaunched 147-208/window, drained == launched, filled ~= drained
    asyncStale=0  asyncGateNotResident=0  asyncCapStops ~0  offThread=0 x134
    no fine-tier gate leak (the leg completed; the worker branch is fatal)
    holes med 92 p90 168 (family)   p95 10.90 (family)   p99 14.90 (-0.3)
    maxMs 40.17 -- best leg of the day

And the pre-registered verdict counter did not move hard:

    cacheMiss (submit population): -15 to -25%, not "falls hard"
    cold-burst census: coldTotal 530-960/window vs control 850-1160;
    maxPerTick 69-84 vs 85-97 -- the BURST TICKS barely dented
    stutterPct 0.34 (family 0.30-0.39) -- null

## The reading

Capacity was never the binding constraint after all -- COVERAGE is. The walk
warms ~180 candidates/window and finds ~1,300-1,900 already cached
(warmSkippedCached); demand meanwhile pays ~800 colds/window that the walk's
candidate set never visits. The warm walk's population (predicted-anchor
admission annuli) and the submit path's cold population are mostly DISJOINT.
Same failure shape as the atlas prefetch crescent: an arm complete for its
set, aimed at the wrong set.

## What survives

- FillFromPrecomputed, the factored pure math (audit mismatch= is now a real
  kill signal), the residency gate, and the counters -- all sound; the arm
  re-arms with one cvar the day the candidate set is fixed.
- p99 -0.3 and maxMs 40 suggest the fills that DO land help the tail edge.
- maxReqHdr printed 22.08 on BOTH legs to the digit -- treat that field as
  suspect (likely a lifetime max latched during fill, not a window max);
  read the ++ site before ever quoting it again.

## The named next step -- RUN SAME DAY, and the map is decisive

coldByLevel added to the census (4fdd5db) and read on CBL-map:

    L0 ~55%   L1 ~25%   L2 ~12%   L3 ~0   L4-L6 episodic pulses of ~40-50

The colds are NOT coarse-level ring maintenance -- they live at the fine
levels the walk already targets. The missing column was on the same line
all along: cacheEvict ~370-490/window against ~800-1,000 colds. The shading
table is DIRECT-MAPPED (131,072 slots), and collision evictions run at half
the cold rate -- so a large share of demand's colds are RE-samples of ground
that was warm and got collision-evicted. No predictive walk can pre-fill a
churn population; the walk and demand were "disjoint" because demand's set
is substantially THE WALK'S OWN PAST FILLS, recycled by collisions.

## The warm family is CLOSED. The successor question is cache shape

Fourth-arm candidates are not warmers: (a) set-associativity or a victim
slot for the table (a collision then evicts the colder of two, not whoever
hashed there); (b) sizing/keying analysis -- 131,072 slots against how many
live footprints? Either is a VoxelApplyBatch.cpp table change, gated by the
audit (mismatch=0) and judged on cacheEvict and cacheMiss falling together
on the submit population. Nobody should build a fourth warmer.
