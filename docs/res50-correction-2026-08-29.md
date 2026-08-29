# The 103.6 fps p95 claim: RETRACTED. The lever is real; the number was not.

2026-08-29. The flag-free confirmation leg for the shipped
`r.ScreenPercentage=50` read p95 10.90 ms (92 fps) against the sweep's
promised 9.66 (103.6). Chasing that 12.8% gap found the sweep table itself
was wrong, on every row.

## What the sweep night actually measured

The sweep table (60.6/55/50 -> p95 88.9/96.8/103.6 fps) does not match any
`SETTLED-MOVING scope=total` line in its own legs' logs:

    SP-ctl (60.6%)  full exposure n=13,998  p95 12.70 ms (79 fps)   not 88.9
    SP-55  (55%)    full exposure n=15,490  p95 11.60 ms (86 fps)   not 96.8
    SP-50  (50%)    n=1,497 ONLY            p95  9.60 ms (104 fps)  the "103.6"

SP-50 -- the one leg that crossed 100 -- settled late, so its "moving"
segment was the final ~10 seconds of a 120-second flight: terrain already
densely streamed, the churn tail excluded. The only over-100 reading in the
set is also the only truncated one. (Its own log even prints the guard "NO
>100 FPS claim may be made" on the n=0 windows earlier in the leg.)

## The corrected ladder -- one binary, four matched full-exposure legs

SETTLED-MOVING scope=total, line flight 23.4 m/s, spawn -61440,-61440,
2560x1440 backbuffer, engagement read off the SECOND view= print:

    61%  1562x879  p50 8.5 (118 fps)  p95 13.0 (77)   p99 17.2  stut 0.53%
    50%  1280x720  p50 7.0 (143)      p95 10.8-11.0 (92)  p99 15.1-15.3  0.32%
         (three legs: ini flag-free x2 at 10.90/10.80, -Cvars form 11.00)

## What stands and what changes

- **The lever is real and 50% stays shipped**: ~2.1 ms at p95, ~1.5 ms at
  p50, stutters down, owner-judged stills unchanged. The ini line alone does
  it -- the flag-free legs prove the config path (gate 1 of the confirmation
  PASSED; ini vs flag identical within 0.1 ms).
- **The >100 fps p95 claim is retracted.** p95 moving is ~10.9 ms (92 fps);
  p50 is 143. Resolution cannot close the rest: the p95/p99 step is
  game-thread-led (docs/p99-game-thread-split), so the remaining ~1 ms at
  p95 belongs to the streaming tail (dispatch + recompute), not to rays.
- **The lesson is the harness's own rule, again**: a segment metric is only
  comparable at matched exposure. `n=` is part of the reading. The sweep
  compared a 10-second tail sample against nothing and the one truncated leg
  produced the headline. Check `n=` before comparing any two frame-dist
  lines.

Legs: Saved/RES50-confirm{,2}.log, RES50-flagctl.log, RES61-ctl.log,
SP-{ctl,55,50}.log. Ini comment corrected in the same commit.
