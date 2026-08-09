# ADR-0007: A depth term in the SWE eligibility predicate, so thin films stay CA-owned

> **STATUS 2026-08-09: [SUPERSEDED by `docs/water-rearchitecture-plan-2026-08-09.md`
> — kept for history].** Proposed, never implemented ("Nothing has been
> implemented. Human sign-off: REQUIRED"), and it amends ADR-0004, which is
> itself now the shelved SWE fallback rather than the active direction. The
> thin-film/eligibility problem it diagnoses only matters if SWE is revived.

- **Status:** proposed
- **Date:** 2026-07-29
- **Doctrine sections affected:** NONE. §2.3's integer-only rule is kept; no
  float appears, no waiver is requested. What this asks Matt to approve is a
  **behaviour change to a shipped-but-default-off simulation layer**, its
  version bump, and the golden it moves.
- **Amends:** ADR-0004 (W4 shallow water, ACCEPTED 2026-07-21).
- **Human sign-off:** REQUIRED. Nothing has been implemented.

## Context

W4's SWE layer was enabled behind `voxel.Water.SWE` (standalone-only) on
2026-07-28, and the ADR-0004 renderer union now draws sheet depth, so its
behaviour is visible for the first time.

**The observation, from Matt's first hands-on pass.** The same 30,000-unit pour
behaves differently with the layer on: the CA settles it into compact basins,
while SWE spreads it laterally as a thin film. Volume is conserved exactly
either way — measured `sheetVolume 29780 + caVolume 220 == 30000`, zero
conservation failures — so this is not a leak. It is a difference in physics.

**Matt's verdict:** CA-style pooling reads as more correct. Recorded as a
preference, and explicitly flagged as something he may revisit.

**The request was to "tune SWE toward pooling". That is not possible**, and
establishing why is the substance of this ADR.

## Why no tuning knob does it

- **Spreading to a level surface is what the mass equation does.** It is not a
  parameter. `dampingQ8` and `gainShift` set how *fast* and how *flat*, never
  *whether*.
- **Both trade directly against ADR-0004's own deadband.** That ADR derives
  `d_min = 2^gainShift · (256 − dampingQ8)/256`. Moving `dampingQ8` 224→192, or
  `gainShift` 7→8, each **doubles** the deadband from 16 to 32 fill units — a
  settled surface flat only to 12.5% of a voxel, i.e. a visible step — while
  weakening the surge by the same factor. You would lose the momentum the layer
  exists for *and* gain a stepped surface, to buy nothing.
- **`absorbPerTick` / `promoteDwellTicks` change only the transient**, not the
  settled shape.

## Where the actual lever is

`SweCaCoupler`'s eligibility predicate decides which columns the sheet owns.
It gates on **headroom** (`openClearanceVoxels`) and **lateral openness**
(`minOpenNeighbours`) — and on nothing else.

**It has no depth term.** A one-unit film on open ground is exactly as eligible
as a lake. That is the whole mechanism behind what Matt saw: a thin pour on
open terrain is, by this predicate, indistinguishable from the large open body
the layer was designed for.

## Proposal

Add a **minimum-depth conjunct** to eligibility: a column is SWE-eligible only
if its water depth exceeds `minSweDepthFillUnits`, and demotes below it.

Sizing it is the part that needs care, and there is one hard constraint:
**it must exceed the deadband.** ADR-0004's settled surface is flat only to
±16 fill units, so a threshold at or below 16 would have columns crossing it
every tick — promotion chatter, which `demoteDwellTicks` exists to prevent and
which would be far worse than the film. A first estimate is **64 fill units**
(a quarter voxel, 4× the deadband), with hysteresis: promote above 64, demote
below 32.

This is a *shape* proposal. The number wants measurement against the breach
fixture, not selection here.

## Costs, stated plainly

- **`swe.h` changes** — it is golden-pinned, so this is not a local edit.
- **`kSweVersion` 1 → 2**, and the SWE golden `0x61523E585CF7B782` is
  **re-pinned**. Different columns promote, so the sheet evolves differently.
- **Saved worlds with SWE state are invalidated.** Today that is acceptable
  precisely because the cvar is default-off and standalone-only, so no shipped
  world has any. **This window closes** the moment SWE is enabled by default or
  reaches M3 replication — which is the strongest argument for deciding now
  rather than later.
- **The water goldens do NOT move.** `kWaterCAVersion` stays 4. Nothing here
  touches `WaterCA::step()`, so the 4.7× optimisation landed 2026-07-29 is
  unaffected.

## The prerequisite, and why it is not optional

**Run `-VoxelSweBreachTest` first.** A gentle pour onto near-flat ground cannot
distinguish:

- **(a)** shallow water correctly spreading as a thin film — which is what SWE
  is *for*; from
- **(b)** bed heights seated slightly wrong, so water sits where it should have
  drained.

Both look like a film. The fixture's `SweBreachBeds phase=pre-breach` line
settles it: `sweOwnedMismatched > 0` before anything is dug has no innocent
explanation, and the fixture prints `TUNING: WITHHELD` rather than proceeding.

**If the beds are wrong, this ADR is moot** — the film is a bug, and adding a
depth term would bury it behind a threshold rather than fix it. That failure
mode is why the tuning was not simply applied when it was asked for.

## Alternatives considered

- **Tune damping/gain.** Rejected above: does not change the settled shape, and
  costs the deadband and the surge.
- **Do nothing; accept the film.** Legitimate. SWE is default-off, and the film
  only appears when it is armed. The cost of waiting is that the free
  golden-re-pin window closes at M3.
- **Make the CA own all shallow water via a rendering rule instead.** Rejected:
  it would draw one thing and simulate another, and the two would drift under
  any flow the sheet actually models.
- **Raise `openClearanceVoxels` so thin water fails on headroom.** Rejected: it
  is the wrong axis. It would also disqualify genuinely deep water under a low
  ceiling, which is exactly the confined-flow case the coupler handles well.

## Prerequisite run, 2026-07-29 — result: step 2 passed, step 3 is BLOCKED

`-VoxelSweBreachTest=25 -VoxelSweBreachSwe=1` was run. Recording it here
because this ADR is explicitly gated on it.

- **Step 2 passed.** `pre-breach: columns=16384 seated=16384 sweOwned=12193
  mismatched=0 sweOwnedMismatched=0 maxAbsDelta=0`. Hypothesis (b), mis-seated
  beds, is **refuted**: every bed agreed with the live terrain before anything
  was dug. The film is not a seating defect.
- **Step 3 could not be reached.** The breach carved real terrain (up to 14
  voxels removed across ~50 columns) and *nothing happened*: `punctured=0`,
  no basin drawdown, no downstream front, `ca=8`. The fixture printed
  `TUNING: WITHHELD`.

The cause was a separate bug, now fixed engine-side (see
`UVoxelWaterSubsystem::ReseatEditedSweBeds` and
`swe_coupler_a_carved_bed_is_lost_to_the_sheet_until_the_caller_reseats_it`):
the sheet's beds were seated once at arm time and never re-seated, so every
column the carve touched was left resting on air, which `eligible()` rejects
forever — a permanent hard wall exactly where the ground was removed.

**That fix is necessary but not sufficient, and this ADR stays blocked.**
Re-seating lets a breached column rejoin the sheet; it does not empty a basin.
`swe.h` §5 has no SWE→CA channel at a **lateral** boundary — an SWE-owned pool
cannot spill into a CA-owned neighbour however much lower that neighbour's bed
is — and the floor of a breach notch is a narrow channel, which §5's
`minOpenNeighbours` excludes from the sheet on purpose. Reproduced in
voxel-core at this fixture's geometry: a notch cut three voxels below the
waterline drains **0.5% of the pool over 200 ticks**, with or without the
re-seat. Until an SWE-owned body can lose water through a breach, the fixture
cannot show directed momentum, and the depth term cannot be sized against it.

Closing that gap is a **fourth exchange channel in §5**, i.e. `kSweVersion`
1 → 2 and a re-pin of the SWE golden `0x61523E585CF7B782` — the same costs this
ADR already lists, which is an argument for deciding both together rather than
paying them twice.

## Decision

**Pending Matt's sign-off.** Nothing implemented. Recommended sequence:

1. Run `-VoxelSweBreachTest=25` with `-VoxelSweBreachSwe=1` and `0`.
   — done 2026-07-29, see above.
2. If `sweOwnedMismatched > 0` — stop, fix bed seating, and discard this ADR.
   — passed; beds are clean.
3. If the beds are clean and the surge is directed, implement the depth term,
   size the threshold against the fixture, re-pin the SWE golden, bump
   `kSweVersion`.
   — **blocked**: there is no surge to read yet. Decide the lateral spill
   channel first, or together with this.
