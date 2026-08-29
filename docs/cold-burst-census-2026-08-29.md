# Cold-burst census: the coverage proof PASSES; the cap is buildable

2026-08-29. dd4ee9e pre-registered that the demand-side cold-shading cap
"needs its own coverage proof before anyone builds it". Built as a
measurement-only instrument (commit 78ae33b: WouldSampleForDispatch mirrors
ShadingImpl's own decision chain -- a naive slow-path test is blind to mode 0
and anchor flushes; coverage via FChunkRecord::HoldsTerrain walked up
ReplacementCovered's parent-key arithmetic). One leg: COLDCENSUS-a, line
23.4 m/s, shipped config.

## The readings (flight windows, 2 s each)

    coldTotal/window     ~850-1,160   (fill peak 5,321)
    maxPerTick           85-97 EVERY window -- the burst is consistent
    ticks in 17+ bucket  6-9 per window
    wouldDefer cap4      ~360-600 per window (about half of all colds)
    coverable            100% in most windows; worst window ~95%
    partition            coverable + holeRisk == coldTotal exactly, all lines

A 90-cold tick at the measured 0.33-0.59 ms per cold sample IS the 43-52 ms
reqHdr single-frame spike -- sized directly for the first time. The census
leg's own p95 (10.80 ms) matches the uninstrumented arms: the probe costs
nothing visible.

## Verdict against the pre-registered rule

"Buildable only if burst-excess cold shadings are overwhelmingly coverable" --
they are (holeRisk 0-5%). And the classification exists AT SUBMIT TIME, so
the cap can be hole-safe BY CONSTRUCTION: only a coverable cold submit may
wait; a holeRisk cold submit always goes through. The rule's kill branch
never fires.

## One correction to the original scope

dd4ee9e said deferral "delays specific submits by 1-2 ticks". Against a
90-burst, cap 4-8 delays tail submits 10-20 ticks (~150-300 ms at flight
tick rates), during which the coarser level shows -- a bounded mip-pop in
place of a 50 ms hitch. The A/B must therefore gate on BOTH sides:
stutterPct toward the 0.10% goal AND uncovered/holes not rising, with the
substituted counter read as the trade's receipt, not as a failure.

## Cap design constraints (for the arm, from the census)

- Cap applies per tick at the submit site, cold+coverable only; default 0.
- holeRisk exempt (hole-safety by construction, not by tuning).
- The deferred submit must re-enter NEXT tick ahead of new work of its ring,
  or a sustained burst starves it (the census says bursts are clustered, so
  fairness matters at the tail).
- Gates: stutterPct falls materially toward 0.10, maxReqHdr leaves the
  tens-of-ms band, p95/p99 not worse, uncovered not rising -- any one moving
  the wrong way refutes, default stays 0.
