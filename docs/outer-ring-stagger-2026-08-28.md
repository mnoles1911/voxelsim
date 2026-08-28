# Outer-ring scan stagger: a stutter fix, not a percentile fix

2026-08-28. `-VoxelAmortizeOuterScans=1`, four legs alternated A,B,A,B, same
binary, 2560x1440 line flight from `-61440,-61440`. **Default stays 0.**

## The mechanism it targets

A level re-scans when the anchor leaves its level-L chunk, and **level chunk
boundaries are nested** -- a level-5 boundary is also a boundary of every finer
level -- so the tick that crosses one crosses all seven and every ring scans
together. The counters show it: R0 fires 12x per 2 s window and R5 once, so every
R5 scan IS one of R0's twelve. That tick pays up to 19.07 ms against a 0.11 ms
mean on the 89% of frames where nothing is due.

The fix budgets how many levels >= R4 (512 m and out) may scan per tick. It does
not slice a scan, so nothing is partially processed.

## Result

    arm            p50            p95            p99          max frame     hitches >=33ms
    control    8.52 (117.4)  11.92 (83.9)  13.99 / 13.86   86.97 / 134.54     45 / 28
    stagger    8.56 (116.9)  12.00 (83.3)  14.05 / 14.01   26.41 /  25.47     25 / 25

    recompute TAIL mean  4.083 -> 3.527      maxRecompute  17.30 -> 11.72

**The worst frame falls 4-5x and REPRODUCES TIGHTLY** -- 26.41 and 25.47 against
a control that swings 86.97 to 134.54. Hitch frames fall from 28-45 to 25/25.

**AND p99 DOES NOT MOVE. My predicted -3.5 ms did not happen.** Stated before the
legs ran and wrong. The reason: coincidence ticks are ~2 per 220 frames, ~0.9%,
so they sit AT AND ABOVE the p99 boundary. Removing 135 ms frames moves p99.9 and
max; it cannot move the 99th percentile itself.

`recompute`'s mean also fell 14%, which I had said in advance would be a bug
signal ("if it falls a lot, something is being skipped"). It is not: a permanent
one-tick lag means a permanently slightly smaller resident set, so slightly less
work. It is consistent with the coverage figure below rather than with skipping.

## The cost, and it is real and reproducible

    tracked chunks   control 79,273 / 79,246      stagger 78,963 / 78,955

**0.37% fewer chunks resident, consistently.** ~290 chunks, all in the outer rings
(512 m - 4 km), each arriving one tick later than it would have. `loaded` is
unchanged within noise (61,355/61,429 vs 61,265/61,506), so nothing is being
dropped -- the set is lagging, not shrinking.

## WHAT NO INSTRUMENT HERE CAN SHOW

A capture settles 120 s before the shutter, by which time both arms have
converged on the same world. **So a parked capture proves the settled image is
unchanged and is structurally incapable of showing the thing this change trades
away** -- a transient far-field lag while moving. This project has no
moving-capture capability; that is recorded rather than worked around.

So the parked A/B is necessary and not sufficient, and the remaining question is
one only a person flying it can answer: **is a one-tick-later arrival of geometry
between 512 m and 4 km visible at 20 m/s?** The arithmetic says a tick is ~8.5 ms
and the nearest affected ring is 512 m away, which is why the bound was set at R4
rather than lower.

## Verdict

**Default stays 0 and this does not ship on these numbers.** It buys a 4-5x
better worst frame and ~40% fewer hitches for 0.37% more far-field lag, and it
moves the owner's stated gate (p99) by nothing at all. That is a trade worth
offering, not one worth taking unasked.
