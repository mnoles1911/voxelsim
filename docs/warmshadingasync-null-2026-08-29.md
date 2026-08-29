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

## The named next step (before any further warm work)

Attribute the census's colds: add level (and ring, if cheap) to the
cold-burst census so one leg says WHERE demand's ~800 colds/window live.
If they are coarse-level ring maintenance, the walk needs a coarse-level
candidate source, not more capacity. No warm arm should be tuned until that
one leg has been read -- this family has now been built three times against
an unmeasured population.
