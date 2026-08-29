# ColdShadingCapPerTick: REFUTED on the image gate; default 0 permanently

2026-08-29. Two armed legs (CSC-cap4-{a,b}) against a five-leg control
family (CSC-ctl-a + RES50 x3 + COLDCENSUS-a). Engagement PROVEN and exact:
maxPerTick collapsed 88-97 -> 4, overshooting only by that window's
capExempt (the holeRisk exemption working as designed).

## The gates, as pre-registered

    stutterPct toward 0.10:  0.31/0.34 armed vs 0.32-0.39 ctl  -> NULL
    p95 not worse:           11.0 vs 10.8-11.0                 -> held
    p99:                     14.60 BOTH armed vs 15.1-15.3 all ctl
                             (a real, consistent -0.5 ms side effect)
    uncovered not rising:    flight holes med 92 -> 160-161,
                             p90 154 -> 647-771, max 211 -> 851  -> REFUTED

"Any one moving the wrong way refutes; default stays 0." One did, 4-5x at
p90, on both armed legs.

## Why the census's coverage proof did not save it

The census classified every cold at SUBMIT TIME: coverable-by-coarser-
ancestor, holeRisk 0-5%. That reading was correct and is not retracted. What
failed is its lifetime: at cap 4 the backlog diverges (deferred= climbs
18k -> 84k per window across a leg; ~320 chunks pending and growing), so a
deferral lasts seconds -- and a coverage fact checked at defer time does not
survive seconds of camera motion and eviction churn. The volatile-
observation-stable-premise failure shape, in a new coat: the instrument
measured "coverable NOW"; the arm needed "coverable UNTIL SUBMITTED".

A sustainable cap (>= mean arrival) would defer only bursts and hold
deferrals short -- but the stutter gate already read null at the TIGHTEST
cap, so no larger cap can buy the thing the arm was built for.

## The attribution advance (what the arm actually bought)

With reqHdr bounded to ~2.4 ms/tick in flight, stutterPct did not move.
The 43-52 ms reqHdr bursts are therefore NOT the stutter frames -- the
stutter budget belongs to the RASTER-ATLAS FILLS (worst armed flight window:
raster=480.9 ms vs reqHdr=165.8 inside one 2 s window; and the standing
bimodal submitMs p50=0/max=1090 finding). The p99 -0.5 ms says the reqHdr
bursts live at p99, not in the stutter tail.

## What remains for the stutter half (0.32% vs 0.10% goal)

dd4ee9e's shortfall (1), now the only named candidate: the atlas prefetch
crescent scan covers RIM RINGS only and caught ~98 of ~600 due pages. The
fix is scanning the whole predicted column. Async fill stays REFUTED (it
spreads the stall); the demand cap stays tuned; cold-shading deferral now
joins them.

Code stays in-tree, default 0, engagement counters wired (capPerTick=,
deferred=, deferredTicks=, capExempt= on the census line). Commit 7159856.
