# `other` was a missing field, and what was hiding in it

2026-08-28, two legs on the shipped default (buried skip off), 2560x1440 line
flight from `-61440,-61440`.

## The residual collapsed

    TAIL   before   tick=10.99  dispatch=4.76  apply=1.15  unload=0.33  brickFlush=0.10  other=4.64
           after    tick=10.12  recompute=3.81  dispatch=4.46  apply=1.00  unload=0.27  brickFlush=0.07  other=0.51

`other` **4.64 -> 0.51 / 0.35** across the two legs. The tick is now ~95% named.
`recompute` had always been a sibling on the `Voxel tick budget` window line and
had never been on the per-frame one, so `RecomputeDesiredSet` fell into the
residual on every leg this instrument has ever run.

Fast -> tail, the two terms that matter are now roughly equal:
**dispatch +4.29, recompute +3.70.**

## Inside recompute: 81% of it has no name either

    TAIL  recompute=3.805  fineResid=0.008  exitScan=0.699  queueFilter=0.000  sort=0.018
          rOther=3.080          maxRecompute=19.07 ms

The four timed sub-stages are 19% of it. The other 81% is the desired-set walk
itself -- the per-ring entry loop, which has a per-level timer
(`AccumLevelEntryMs[]`) that only ever printed on `Hitch frame` lines above
33.3 ms and on a per-window sum line.

## THE SHAPE: recompute is near-free on 89% of frames

    incremental scans per 2 s window:  R0=12  R1=6  R2=3  R3=1  R4=1  R5=1  R6=0-1
    full sweeps:                       first=0  edit=0  underground=0  config=0

**No full sweeps at all.** ~24 incremental ring scans across ~220 frames, so
recompute costs ~0.11 ms on the frames where nothing fires and lands in a lump on
the ~11% where a ring scan does. That is the entire bimodality, and it is why the
mean said 0.11 while the max says 19.07.

## R5 costs 7.7x R6 per footprint, reproducibly, and I do not know why

Pooled over every active window, both legs agreeing to within 2%:

    ring    footprints   entryMs   us/footprint      radii
    R0         94,958     406.2        4.3           0-64 m
    R1         49,687     312.3        6.3          64-128
    R2         26,482     187.6        7.1         128-256
    R3         14,157     153.6       10.8         256-512
    R4          8,749     199.7       22.8        512-1024
    R5          6,137     355.6       57.9       1024-2048   <- dearest ring, by total AND per footprint
    R6          4,535      33.8        7.5       2048-4096

**It is not "coarser costs more" -- R6 is coarser than R5 and costs 7.5.** R5 is a
discontinuity, and it carries the largest total entry cost in the cascade despite
having the second-fewest footprints.

**A hypothesis I checked and DISCARDED:** that R5 is the last ring inside the
raster atlas's coverage. The atlas reports `coverageRadius=2248`, which is
**pixels, not metres** -- at 1875 mm/px that is ~4.2 km, past R6's outer edge. The
boundary explanation does not survive its own units. **Cause not established. Do
not quote one.**

## The lever, and it does NOT depend on the R5 mystery

**A ring scan is not amortised within the ring.** It fires on one tick and pays
its whole cost there -- up to 19.07 ms on a single frame. Spreading one ring's
walk across N ticks would convert that spike into a flat ~0.3 ms/frame, which is
worth roughly **-3.5 ms at p99** (14.29 -> ~10.8 ms, ~92 fps) if it flattens
completely.

**AND IT IS THE RISKY CLASS.** Amortising admission means admitting later, and
this project has already lost that argument once: the harvest cap deferred
admission and coverage collapsed to 8,223 of 48,900 chunks. A hole is worse than
a hitch.

**The bounded version is the one to build:** amortise only the OUTER rings
(R4/R5/R6, together 589 ms of the 1,648 ms entry cost and all of the spikiness),
and leave R0-R2 immediate. A chunk 1-4 km away arriving one tick later is not
visible; a chunk 60 m away arriving late is. Any attempt must carry a coverage
check and matched captures, not a timing table alone.
