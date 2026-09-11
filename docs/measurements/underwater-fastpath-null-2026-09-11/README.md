# The underwater fast path: engaged perfectly, bought nothing, retired

Two walk captures on one binary, a cvar apart, at the temperate forest site.
Quiet stretch of each (frames 0–199, the settled regime). Both receipts passed.

## The result

| scope (median ms) | fast path OFF | fast path ON | ripple feature OFF |
|---|---|---|---|
| GameThreadTime | 21.604 | 21.511 | **17.000** |
| FrameTime | 41.656 | 41.618 | 41.446 |
| GPUTime | 40.435 | 40.413 | 40.312 |
| OceanTick | 4.072 | 4.115 | 4.134 |
| └ OceanUnderwater | 4.070 | 4.113 | 4.132 |
| RippleTick | 4.069 | 4.061 | — |
| └ RippleAutoWatch | 3.990 | 3.985 | — |
| **UnderwaterQuery** | **8.103** | 8.167 | 4.140 |
| ├ **UnderwaterFill** | **8.088** | 8.165 | 4.138 |
| └ **UnderwaterSurface** | **0.013** | — | — |
| UnderwaterQuery calls/frame | 12 | 12 | 11 |
| Underwater fast-outs/frame | — | **12** | 11 |
| MovementWaterProbe | 0.021 | 0.012 | 0.013 |

**The arm engaged perfectly — 12 of 12 calls a frame took the short circuit —
and the game thread moved 0.093 ms, inside the 2.8% repeatability.**

## Why, and why it took one capture instead of a week

The short circuit skipped the worldgen ground sample above the waterline, on the
reasoning that `IsOpenSeaNowAtWorld` is

    WorldZ < SeaZ && GroundZ < SeaZ && WorldZ >= GroundZ

so above the sea surface the ground cannot change the answer. **That reasoning
is still correct.** It is algebra, not a tolerance. It simply removed the wrong
half.

The sub-scopes went in alongside the arm precisely because the same function had
measured 0.020 ms from one caller and 4.075 ms from another, and a 200× spread
is not something to infer a cause for. They answered it immediately:

- `UnderwaterFill` **8.088 ms** — 99.8% of the call
- `UnderwaterSurface` **0.013 ms** — the half the arm removed

A branch and a console variable to avoid 0.013 ms is dead weight, so the arm is
removed rather than left switched on, and this record is what it leaves behind.

## What the same capture did establish

**The cost is `GetWaterFillAtWorld`**, which is a cellular-automaton cell read
followed, on a miss, by an implicit-field read. That function now carries its own
split — `WaterFillCaMs` against `WaterFillImplicitMs` — and the next capture
names which of the pair it is.

**Twelve calls a frame, not two.** The ocean asks once for the camera; the ripple
field's auto-watcher asks for the pawn and for every watched actor.

**Two calls of the twelve carry the entire cost.** Turning the ripple feature
off removes exactly **one** call and **3.963 ms** with it, so the auto-watcher's
pawn call alone is ~3.96 ms. The eleven that remain cost 4.140 ms in total, and
`OceanUnderwater` accounts for 4.132 of that — leaving **about 0.008 ms for the
other ten calls put together.**

So it is not a gradient. Two calls are each ~4 ms and ten are free, and the two
expensive ones are the first call made inside their own subsystem's tick. That
points at something warm-vs-cold rather than at the arithmetic of any one query:
a memo or brick cache that the first caller in a tick misses and later callers
in the same tick hit, invalidated somewhere between the two subsystems. The
`WaterFillCaMs` / `WaterFillImplicitMs` split is the next thing to read, and if
both look flat the instrument has to move to per-call-site.

**Disabling the ripple feature takes the game thread from 21.60 to 17.00 ms.**
That prices the auto-watcher at 4.6 ms and is **not a proposal** — it removes a
feature. It is recorded because it bounds what a gate on the watcher could be
worth.

## The rule this is filed under

An arm must prove it engaged, and this one did. An arm that engages and measures
null gets retired with its record rather than accumulating in the tree — the
same treatment the bound arm and the rung probe got. What made this cheap was
shipping the instrument in the same build as the change, so a null was still an
answer rather than a dead end.
