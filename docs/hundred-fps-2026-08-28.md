# The >100 fps programme

2026-08-28, owner decision: "Go over 100 FPS." 50k/s retired the same day. The
owner is judging pinned CAPTURES, not editor flights, for the present.

## Where the frame stands (VL-def, the shipping default after the day's fixes)

    FAST   frame  7.30   gpu 6.24   game 2.39      (137 fps typical)
    SLOW   frame 13.43   gpu 8.93   game 5.81      (p95 band)
    steps  FAST->SLOW  gpu +2.69  GAME +3.42       <- game now LEADS the p95 step
           SLOW->TAIL  gpu -0.43  game +9.71       <- tail is pure game thread

    percentiles: p50 8.69 / p95 11.31 / p99 12.01  (115 / 88 / 83 fps)
    stutters (>20 ms): ~0.5% against the 0.10% goal

**The two-regime picture moved under the night's fixes**: removing the band took
the GPU out of the p95 driver's seat. The goal has TWO halves and they have
different owners now:

- **3a (p95 < 10 ms): needs -1.4 ms**, split between a GPU share (marcher+TSR
  scale with resolution; streaming worldgen does not) and a game share
  (tick 3.65 at SLOW).
- **3b (stutters <= 0.10%): is game-thread submit spikes** -- reqHdr singles of
  43-52 ms and raster-atlas fills of 33-43 ms -- which RESOLUTION CANNOT TOUCH.
  Known from the submit split (docs/submit-split-2026-08-28.md).

## Lever 1 -- internal resolution (the owner-authorized trade)

Current: 60.6% screen percentage (1552x873 TSR'd to 2560x1440). Sweep 55% and
50% with matched pinned captures; the owner judges stills. Engagement proof per
leg: the marcher's own `view=` line must change -- the harness banner lies
(-ResX is inert; the resolution comes from GameUserSettings/TSR).

Expected: rays scale with s^2; marcher ~4.5 ms and TSR ~1.4 scale, worldgen
~2.7 at p95 does not. 55% ~= -0.85 ms GPU at p95; 50% ~= -1.5 ms. Whether that
converts to FRAME ms depends on how often the p95 frame is GPU-bound -- measured,
not modelled, by the sweep.

## Lever 2 -- the p95/p99 game-thread step (required for 3b regardless)

In size order on today's split: submit +4.72 (reqHdr cold terrain samples +
raster page fills, both bimodal singles), unnamed-in-tick +4.28, dispatch-other.
Candidates, none free: warm the reqHdr samples ahead of the camera (the
speculation path already predicts position; the shading cache is at its
theoretical ceiling so this is about WHEN, not whether, the sample runs);
amortise the raster page-column fill (cap tuned, async refuted -- the remaining
shape is prefetch-by-prediction, same idea). The outer-ring stagger (parked,
awaiting a flight the owner cannot give yet) addresses worst-frames, not p95.

## Order

1. Resolution sweep + captures (running). Owner judges stills.
2. Re-pool attribution at the chosen resolution -- the split moves again.
3. Attack the game-thread step with whatever the new split names largest.
