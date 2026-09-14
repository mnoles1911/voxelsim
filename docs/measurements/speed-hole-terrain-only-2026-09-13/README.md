# The terrain-only world at 20 m/s: one hole was mine, the rest is the R0 seam

The owner's bar: no holes at any speed, flying included. Terrain-only fixture,
forest spawn, 100 m altitude, `voxel-moving-capture.ps1` legs at 20 m/s, all
after the flight-leg checkpoint fix (before it, legs did not fly where asked).

## 1. The crater was the cold-resolve cap, and it is fixed

In a world with no asset field the cap I shipped that afternoon read every chunk
as cold: `deferred=552,861` per window, dispatch pinned at **8.00 per tick**, a
kilometre-wide crater. Guarded on a resolve actually being paid (`f8bf8ed`).
After: 3,861–4,200 chunks/s, 56–62 fps, no crater, `deferred=0`. The owner's
live check: "mostly no longer any holes as the player flies forward."

## 2. What is left, measured over a 9.6 km flight

`Voxel coverage (window): holes=` rises through the flight from 0 to
1,000–2,900 of ~10,050 scanned columns. Three arms, same flight:

| arm | mean holes | median | p90 |
|---|---|---|---|
| baseline (lead 4 s) | 1,424 | 1,563 | 2,267 |
| coverage-first pick — **never engaged** | 987 | 1,070 | 1,929 |
| anchor lead 1 s | 928 | 878 | 1,864 |

**The arm that provably did nothing moved the number 30%.** That is this probe's
run-to-run noise, and it means the 1 s-lead arm is not distinguishable from it.
Lead is not the lever, or not provably.

## 3. The far-ring starvation theory, and why it was wrong about the holes

Per-ring dispatch during flight reads R0 3,800–4,900 per window, R1 ~2,000, and
**R4–R7 at 1–2 each** with 1,000–3,500 queued — nearest-first hands the fine rings
a capped budget and the far rings get exactly their floor of one slot. That is
real and it is recorded. But the probe's classified examples say the holes are
not there: **1,936 of 2,032 are R0**, at **r = 62–64 m** — R0's outermost shell —
split `unsettled` (admitted, mesh not back) and `no-record` (not yet admitted).
With the lead cut from 4 s to 1 s the split moved to 1,771 / 162 while the total
did not: earlier admission, same latency.

So the coverage-first pick (`voxel.Stream.CoverageFirstPick`, default 0) was
aimed at the wrong ring. It is kept, off, with its counter, as a measured null.

## 4. Two counters that lie, recorded so nobody reads them again

- `zq=` on the ring line reads **100% of total on every ring** under
  direct-to-pool. It is a legacy quad counter, not "buried rock".
- `Voxel coverage (window): holes=` counts **records**, not pixels. A missing R0
  record over a resident R1 is a mip-pop under fallthrough, not a black square.
  The instrument for what the owner sees is `voxel.March.HoleStats 2` —
  uncovered RAYS, by ring and by reason (never / pending / evicted) — and it is
  the next leg.

## 5. The standing hypothesis for the visible holes

An R0 chunk at the seam that is admitted but not yet meshed has a one-rung
ladder to fall through to R1; at the R0/R1 seam the R1 chunk above it may be in
the same state. At 20 m/s the seam advances 40 m per window and every chunk on
it is by construction "just admitted". The ray statistics will say whether the
uncovered rays are `pending` (latency at the seam) or `never` (admission), and at
which ring.

## 6. The ray instrument cannot be used while moving — three legs and a memory file learned it

`voxel.March.HoleStats 2` was the obvious next instrument (uncovered rays by ring
and by reason). Three legs were flown on it before the control was looked at:

| leg | dispatched/s | picture at 8,192 m |
|---|---|---|
| no instrument | **3,958** | intact |
| HoleStats 2, control | 834 | **black void, 2.7 fps, GPU job 1.5 s** |
| HoleStats 2, ahead cap 1024 | 656 | black void, 2.5 fps, GPU job 3.3 s |
| HoleStats 1, control | 867 | — |

**Both levels collapse streaming at speed.** The readback path stalls the mesh
pipeline; the GPU saturates; chunks stop arriving; the world goes black. The
"~2% on the kernel" figure recorded for level 1 came from a parked timing leg and
does not survive motion. Every by-ring / by-reason number those legs produced —
including a plausible-looking "attributed absences halved" for the ahead-cap arm
— describes a system the instrument had already broken, and none of it is used
here.

What remains usable at speed, in order of trust: the screenshot; dispatched/s
and the per-ring dispatch line; the record probe with a repeat. The ahead-cap
experiment is being re-run on exactly those.
