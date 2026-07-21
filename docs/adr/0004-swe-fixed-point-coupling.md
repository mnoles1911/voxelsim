# ADR-0004: W4 shallow water — a fixed-point reduced SWE, and the CA↔SWE coupling

- **Status:** proposed
- **Date:** 2026-07-21
- **Doctrine sections affected:** **NONE.** This is the headline result and it
  is the opposite of what the milestone brief anticipated. §2.3's integer-only
  rule is *kept*, not deviated from: no float or double appears anywhere in the
  shipped code, CI `float-ban` is clean, and no waiver is requested. What this
  ADR asks Matt to approve is a **physics fidelity reduction** and a **future
  enablement**, not a numerics exception.
- **Human sign-off:** **ACCEPTED by Matt, 2026-07-21.** See "Decision" below.

## Context

Implementation plan §4's water track reaches W4 ("SWE + force field"), and §4's
own slip-risk list names **"W4 CA↔SWE coupling"** as one of three historically
risky items. Two things had to be resolved before any code was worth writing.

**1. What W4 is actually for.** `waterca.h`'s Phase C says outright that flow
momentum is "NOT MODELED … a depth-scaled breach *inrush jet* needs Layer C's
SWE patches", and that Phase C "only ever computes a static equilibrium level,
instantly-ish over a few ticks, never a surge". That is the gap: today a lake is
a level, not a body of water. It has no current, no waves, no direction, and
therefore nothing that can push a boat (W5), advect foam (W5), or make a breach
feel like an inrush rather than an instant re-level. W4 is the layer that gives
water a **velocity field**.

**2. The numerics problem.** SWE is conventionally floating point; voxel-core is
integer-only by doctrine §2.3, because worldgen and simulation must be
bit-identical across machines and GPU vendors — a rule paid for in blood when
the M0 cross-vendor gate passed on AMD and failed on NVIDIA against identical
committed SPIR-V (ADR-0001). The brief's framing was: if faithful SWE cannot be
done in fixed point, that is a doctrine deviation needing sign-off.

The investigation's answer is that **the framing contains a false dichotomy**,
and the rest of this document is the argument for that.

## The numerics question, resolved

### Why "port the textbook solver to fixed point" is the wrong question

The conservative shallow-water equations

```
  ∂h/∂t   + ∇·(h u)                = 0
  ∂(hu)/∂t + ∇·(h u⊗u) + g h ∇(h+b) = friction
```

are normally discretised with an approximate Riemann solver (HLL/HLLC/Roe).
Two properties of that family are decisive here.

- **It needs wave speeds `u ± √(g h)`** — a square root per face per tick.
  Expensive in fixed point, but survivable (integer sqrt is deterministic).
  This is *not* the blocker, and it would have been easy to stop here and
  conclude "too hard".
- **It needs a WELL-BALANCED (C-property) treatment.** For a lake at rest the
  pressure flux and the bed-slope source term must cancel *exactly*, or the
  solver invents currents over uneven ground. This is the single
  most-written-about failure mode of SWE solvers, and in floating point the
  cancellation is only ever *approximate* — well-balancedness is an entire
  sub-literature precisely because getting it right is hard. In fixed point,
  where each term is separately truncated, exact cancellation is not reachable
  at all without carrying the cancellation symbolically, which is a redesign of
  the scheme rather than a port of it.

So a faithful Riemann-solver SWE in fixed point is genuinely impractical.
**But that is a statement about one discretisation family, not about shallow
water in integers.**

### What is practical, and why it is better here

The **virtual-pipe reduced SWE** (the Mei-class hydraulic-network formulation
used in production terrain-erosion and game water) keeps the depth-averaged
mass equation exactly and **linearises the momentum equation**: the nonlinear
advection term `∇·(h u⊗u)` is dropped, and momentum is carried as a per-face
stateful flux driven by the hydraulic head difference and bled by a damping
term standing in for friction. This is a *documented physical simplification*,
not a numerical approximation of something we wish we had.

In integers it is not merely adequate — on three counts it is **strictly better
behaved than the faithful scheme would be in floats**:

| Property | Faithful SWE, floats | Reduced SWE, fixed point (shipped) |
|---|---|---|
| Lake at rest over uneven bed | approximately exact; spurious currents are the classic failure | **exactly** exact, provably and permanently |
| Mass conservation | conserved to rounding; residue accumulates | **bit-exact**, structurally — nothing is ever rounded *on* the conserved quantity |
| Failure mode when unstable | NaN/Inf, dead frame | bounded ugly oscillation; depth stays in `[0, max]` by clamp |
| Cross-vendor reproducibility | not guaranteed (ADR-0001) | bit-identical by construction |

The lake-at-rest row is the important one and it is worth being precise about
*why* it inverts. In the pipe formulation **there is no source term to cancel
against**: the bed enters only through `head = bed*255 + depth`, one integer
addition. Zero head difference ⇒ identically zero flux increment ⇒ the damping
term decays stored flux to exactly zero ⇒ zero transport, forever. The hardest
property in the float literature is free here.

The mass-conservation row inverts for a related reason: **rounding is confined
to the momentum accumulator, which is not a conserved quantity.** Truncation
there is a bounded dissipative bias — it can only ever remove momentum, never
add it, so it is a stability *asset*. The conserved quantity (volume) is only
ever moved by a single integer subtracted from one column and added to another
in the same statement.

Two fixed-point traps were found and are worth recording, since the second one
is invisible until you look for it:

1. **Asymmetric shift.** A plain `v >> s` rounds negatives toward −∞, so the
   damping term applied to a stored flux of −1 yields −1 again: the flux
   **sticks at −1 forever** and a "settled" lake carries a permanent
   one-unit-per-tick current in one direction only, while the mirror-image +1
   case decays correctly. `shiftSym` rounds both signs toward zero. Without
   this, the exact-lake-at-rest claim above is simply false. Covered by
   `swe_sign_symmetric_shift_lets_both_signs_decay_to_exactly_zero`, which
   asserts the buggy behaviour explicitly so the trap cannot silently return.
2. **Truncation deadband.** With the flux stored in whole units, any head
   difference below `2^gainShift` truncates to a zero increment and the surface
   settles only to within **half a voxel**, visibly stepped. Fix: carry the
   flux accumulator in **Q8** (8 fractional bits). Small head differences then
   accumulate sub-unit momentum tick over tick until it crosses a whole unit,
   and the residual deadband is set by the damping-limited steady state
   instead of by truncation. Quantified below.

### VERDICT

**A stable, exactly-conservative shallow-water solver with a usable force field
IS expressible in fixed point, and no doctrine deviation is required.** The
cost is a reduction in *physics fidelity*, not in *numerical correctness*, and
that cost is enumerated and quantified in the next section rather than buried.

## What the fidelity reduction actually costs — quantified

### 1. Settle deadband: ±16 fill units (6.3% of a voxel, 6.3 mm)

Holding a head difference `d` fixed, the flux accumulator's steady state is
`f* = d · 2^(8−gainShift) · 256 / (256 − dampingQ8)`, and transport is nonzero
only once `f* ≥ 256`. So the smallest head difference the scheme acts on is

```
  d_min = 2^gainShift · (256 − dampingQ8) / 256  =  128 · 32 / 256  =  16 fill units
```

at the shipped defaults (`dampingQ8 = 224`, `gainShift = 7`). One voxel is 255
fill units and `kVoxelSizeMm = 100`, so **a settled SWE surface is flat to
±6.3 mm**, against WaterCA Phase C's ±1 fill unit (±0.4 mm).

`sweSettleTolerance()` computes this from the config so tests assert against
the derivation, not a magic number
(`swe_pour_settles_flat_within_the_derived_deadband`).

Is 6.3 mm visible? At 10 cm voxels it is 6% of one voxel — below the resolution
of the mesher and far below the resolution of a rendered water surface. My
assessment is that it is not perceptible. It is nonetheless a real regression
against the CA's flatness and it is the number Matt should look at hardest.
It can be reduced by raising `dampingQ8` toward 256, at a strict cost in
stability margin (next item) — the two are the same dial.

### 2. Stability: a CFL analogue, with a 6.25% margin, enforced not trusted

Linearising two adjacent columns, the tick map has characteristic polynomial
`z² − (1+D)z + (D + L·G) = 0` with `D = dampingQ8/256`, `G = 2^−gainShift`, and
`L` the largest discrete-Laplacian eigenvalue (8 in 2D). Both roots are inside
the unit circle iff

```
  D + L·G < 1     ⇔     dampingQ8 + ((8 << 8) >> gainShift) < 256
```

At defaults: `224 + 16 = 240 < 256`, a **6.25% margin**. `SweConfig::stableIn2D()`
is exactly this inequality in integers, and the `SweGrid` constructor **clamps
an unstable config** (weakening the gain) rather than trusting the caller —
because these constants look like designer-tunable feel parameters and are not.

Note the failure mode if it is ever violated anyway: sustained ringing, never
divergence, because depth is clamped to `[0, maxColumnDepth]` by the outflow
and headroom caps regardless. **The CFL condition here is a quality constraint,
not a safety one** — which is a property fixed point gives us and floats do not.

### 3. Physics NOT modelled (the actual fidelity cost)

Direct consequences of dropping the advection term:

- **Supercritical flow and hydraulic jumps** are not resolved.
- **A dam-break bore front travels at the wrong speed.** Water still floods
  through a breach with visible momentum and a metered surge, but the front is
  a damped diffusive wave, not a shock.
- **No vertical structure** (the depth-averaged assumption itself). This is not
  a defect but a domain limit, and it is exactly what the coupling's promotion
  predicate exists to test for.

My assessment: for lakes, floodplains, rivers, boats and foam — everything §4's
water track actually names — none of these are load-bearing. If a future
milestone wants a genuine dam-break shock as a *set piece*, that is better
served by a scripted/authored effect than by a solver rewrite.

### 4. Performance (measured, this machine, Release/-O2)

Settled lake, disturbed by one unit per tick — the case ADR-0003 identified as
the motivating one, and the case the CA is worst at:

| Footprint | SWE sheet | WaterCA (4 voxels deep, memo ON) | Speedup |
|---|---|---|---|
| 64×64 (4,096 columns) | **0.083 ms/tick** | 6.13 ms/tick | **74×** |
| 128×128 (16,384 columns) | **0.364 ms/tick** | 4.14 ms/tick | **11×** |
| 256×256 (65,536 columns) | **1.48 ms/tick** | — | — |

Two readings, and the second matters more than the first:

- SWE scales **linearly** in columns (4.4× and 4.1× for successive 4× area
  increases), as a dense 2D stencil should.
- The CA appears to get *cheaper* from 64×64 to 128×128. It is not getting
  cheaper — it is **declining to simulate**: the larger body trips
  `kMaxHydrostaticComponentCells` and Phase C skips the component entirely
  (`waterca.h`, and ADR-0003 measured the same inversion). So the comparison
  understates the win: on large bodies the CA's cost is low *because the
  physics is being deferred*, whereas SWE's cost is low while actually
  simulating.

Memory: 17 bytes/column persistent + 20 bytes/column scratch ⇒ ~2.4 MB for a
256×256 sheet.

## The CA↔SWE coupling design

This is the part §4 flags as risky, so it is specified concretely rather than
gestured at. Full prose lives in `voxelcore/swe.h` §5; this is the decision
content.

### Ownership is a partition. Nothing is ever in both.

Decided per **column**, enforced per **voxel**:

- A column is either SWE-owned or CA-owned — never both, never neither.
- **SWE-owned:** voxels from `bed+1` up to the top of the sheet belong to the
  sheet. Voxels at or below `bed` — the cave under the lake, the aquifer, the
  drain shaft — stay CA-owned **unconditionally**.
- **CA-owned:** every voxel in the column is the CA's; the CA is unmodified and
  unaware that SWE exists.

So "a cave under a lake" is not a conflict, it is the ordinary case: lake is
SWE, cave is CA, and the solid bed between them **is** the ownership boundary.
**The boundary is always a surface — a solid bed or a domain perimeter — never
a shared cell.** That is precisely what makes the exchange expressible as a
ledgered integer transfer rather than a flux-matching condition between two
different discretisations, which is where couplings of this kind usually go
wrong.

The partition is enforced on both sides, and getting this right required a
correction found by a test rather than by reasoning:

- **SWE side (absolute).** A CA-owned column is marked inactive and the tick
  rules treat it as a hard wall — the sheet cannot push water into CA-owned
  ground at all. **This was missing in the first implementation**: membership
  lived only in the coupler's bookkeeping, so the sheet kept happily flowing
  into a column the coupler had just handed to the CA — a genuine double-owned
  cell. Caught by
  `swe_coupler_puncture_meters_the_inrush_then_hands_the_column_over`.
- **CA side (rate-limited).** CA fill appearing inside a sheet range is
  evacuated at up to `absorbPerTick` per column per tick. Being precise,
  because this is weaker: the guarantee is *no voxel is ever written by both
  solvers*, **not** *such fill is always instantaneously zero*. A CA source
  pushing water into a sheet faster than `absorbPerTick` leaves a bounded
  standing residue until it stops. That residue is conserved and correctly
  ledgered — it is water in transit — but **a renderer must draw the union of
  the two, not the sheet alone.**

### Exchange: three channels, each a ledgered integer move

The invariant that holds after any sequence of coupled ticks:
`ca.totalVolume() + grid.totalVolume() == total injected`.

- **(a) DRAIN, SWE → CA** — "dig a hole in the lake floor". If the bed voxel is
  no longer solid the column is *punctured*: `min(depth, drainPerTick)` units
  leave the sheet and are placed in the CA voxel at the puncture, which wakes
  the owning brick so the CA carries it away in 3D. Only what the CA *accepted*
  is debited, so a full cell below back-pressures the drain instead of
  destroying volume.

  The bed is deliberately **not** re-seated downward to follow the hole. That
  was the first design and it is wrong: it moves the ownership boundary down
  over voxels the CA is at that moment carrying water through — the exact thing
  the partition forbids. A punctured column is instead simply a **metered
  source** into the CA, which is also the right physical story (a hole in a
  lake floor is confined 3D flow, i.e. CA territory by definition of the
  split). The hand-over then completes with **no extra machinery at all**: a
  non-solid bed fails the eligibility predicate, so after `demoteDwellTicks`
  the column demotes and the CA takes the whole column. Observable behaviour is
  a metered inrush with visibly falling sheet depth, then a clean full
  transfer — which is the desired game feel, and it falls out of the hysteresis
  rather than being special-cased.
- **(b) ABSORB, CA → SWE** — water welling up a shaft into the basin; also the
  mechanism enforcing the no-shared-cell rule.
- **(c) MEMBERSHIP CHANGE** — promotion runs (b) at full rate; demotion pushes
  sheet depth back into CA cells bottom-up. Demotion is **all-or-nothing**
  (pre-flighted against available CA capacity): a half-demoted column would
  hold sheet depth *and* CA fill in the same z-range, so if the CA cannot take
  the whole column this tick, not one unit moves and the demotion retries next
  tick.

### Where the domains meet: the eligibility predicate

A column may be SWE-owned only where the depth-averaged assumption is
legitimate. Concretely: it must rest on a solid bed, have `openClearanceVoxels`
of non-solid space above it (**no lid** — a flooded tunnel is not a free
surface), and have at least `minOpenNeighbours` of its 4 lateral neighbours
non-solid just above the bed (**open, not a one-voxel pipe** — horizontal scale
must exceed depth). Confined 3D flow therefore stays with the CA *by
construction*, not by policy.

### Not oscillating — three independent mechanisms

Since this is the named risk, it is excluded structurally rather than tuned:

1. **Hysteresis on membership.** Promotion requires eligibility for
   `promoteDwellTicks` consecutive ticks; demotion requires failure for
   `demoteDwellTicks` (default 4×). A Schmitt trigger in the time domain: a
   column on the edge of eligibility cannot chatter, and every flip costs a
   full dwell window. `swe_coupler_hysteresis_prevents_membership_chatter`
   flips eligibility *every single tick* for 200 ticks and observes **zero**
   ownership changes.
2. **One direction per column per tick.** Drain and absorb never both run on a
   column in a tick; direction is chosen once, at tick start, from the puncture
   test. Bidirectional handoff within a tick is the classic way coupled domains
   ping-pong volume, and it is structurally excluded.
3. **Rate limits, not equalization.** Every transfer is capped per tick.
   Neither side ever tries to reach equilibrium with the other in one tick, so
   neither can overshoot: the exchange is a first-order lag, which cannot
   oscillate.

## What was implemented, and its evidence

New, self-contained, and **inert**: `voxelcore/swe.h`, `src/swe.cpp`,
`tests/test_swe.cpp`. Nothing in voxel-core or the engine constructs a
`SweGrid`; `SweCoupleConfig::enabled` **defaults to false** and makes
`SweCaCoupler::step()` a total no-op (asserted, not assumed:
`swe_coupler_is_a_total_no_op_when_disabled`).

The one edit outside the new files is two **additive** single-cell hooks on
`WaterCA` (`addWaterAt` / `removeWaterAt`). The coupling genuinely requires
them and neither existing entry point works: `addWater()` stacks upward, which
for a lake draining through a punctured bed would push water back up into the
sheet it just left; `setReplicatedFill()` is documented client-mirror-only,
overwrites rather than adds, and deliberately wakes nothing, so injected water
would sit frozen. **No tick rule changed.**

Suite: **154 → 170 PASS / 0 FAIL.** Both pinned water goldens
(`0x3D2224BE4A253404`, `0x56BC18914355A205`) and the solid-cache/wake tests are
unmoved; `kWaterCAVersion` stays at 4. Clang clean under
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror`; CI `float-ban` clean.
New SWE golden pinned at `0x61523E585CF7B782` under an independent
`kSweVersion`, so a future SWE change can never invalidate a water golden.

Covered: exact lake-at-rest over an uneven bed (digest byte-identical after 500
ticks, not merely "settles back"); exact mass conservation under a violent
transient with per-tick ledger *and* independent re-sum; order independence
under identity/reversed/per-tick-reshuffled/duplicated column orders; the
derived settle deadband; the closed outer boundary; the force field
(correctly signed while flowing, exactly zero at rest); both fixed-point traps;
and, on the coupling side, cross-boundary conservation through a real dug shaft,
metered inrush and hand-over, zero membership chatter, the ownership partition,
and the eligibility predicate rejecting lidded and confined columns.

## Options considered

- **A. Faithful Riemann-solver SWE in floats.** Rejected: requires a doctrine
  §2.3 waiver, breaks cross-vendor bit-identity (ADR-0001), desyncs multiplayer,
  and — per the table above — would be *worse* at lake-at-rest and conservation
  than what ships.
- **B. Faithful Riemann-solver SWE in fixed point.** Rejected: the
  well-balanced/C-property cancellation is not reachable under per-term
  truncation without redesigning the scheme. This is the option the brief
  anticipated recommending against, and it is indeed not viable.
- **C. Virtual-pipe reduced SWE in fixed point (RECOMMENDED, shipped inert).**
  Above.
- **D. Extend the CA's Phase C with a velocity field instead.** Rejected:
  Phase C computes a *static equilibrium level* by construction — there is no
  momentum in the formulation to expose. Adding one is writing this solver
  anyway, but inside a world-breaking, golden-pinned tick rule instead of
  beside it.
- **E. Do nothing until W5 needs it.** Rejected: W5 (foam, particles,
  buoyancy, boats) consumes the force field. Deferring W4 defers W5 wholesale,
  and the plan already flags this item as the risky one — discovering the
  coupling design late is exactly the slip risk §4 names.

## Decision

**ACCEPTED by Matt, 2026-07-21** ("I accept your ADR-0004 recommendations").

Both recommendations are adopted as written:
- **Item 2 ADOPTED** — the reduced (virtual-pipe) SWE is *the* W4 model.
  Accepted consequences: settled surface flat to +/-6.3 mm (vs Phase C
  +/-0.4 mm); no hydraulic jumps, no supercritical flow, dam-break bore at
  the wrong speed; damping/gain are CFL-bounded numerics constants, not
  designer feel-dials. Bought with: no float, no doctrine waiver,
  cross-vendor bit-identity, exact conservation, and 11-74x the CA.
- **Item 3 DEFERRED as recommended** — the coupler stays disabled until M3
  networked water wires its membership/dwell/depth state into replication.
  Enabling earlier is a guaranteed desync.

1. **Merged now, no sign-off needed (inert, byte-identical).** The SWE core,
   force field and coupler ship default-off and unreferenced. No golden moved,
   no version bumped, no doctrine section touched, float-ban clean. Worst case
   if this is later abandoned: delete three files and two `WaterCA` methods.

2. **Needs sign-off: adopt the reduced (virtual-pipe) SWE as *the* W4 model,
   and accept its fidelity cost.** Concretely, approving this means accepting:
   - a settled surface flat to **±6.3 mm** instead of ±0.4 mm (§"cost" 1);
   - **no hydraulic jumps, no supercritical flow, and a dam-break bore front at
     the wrong speed** (§"cost" 3) — water surges through a breach with real
     momentum, but the front is a damped wave, not a shock;
   - `dampingQ8`/`gainShift` being **numerics constants under the CFL bound,
     not designer feel-dials** — the constructor clamps them.

   In exchange: no float, no doctrine waiver, bit-identical cross-vendor
   determinism, exact conservation, exact lake-at-rest, and 11–74× the CA's
   cost on a settled lake while actually simulating it. **Recommendation:
   ADOPT.** The rejected alternative that would fix the 6.3 mm and the bore
   speed is option A, and it costs multiplayer determinism — which is not a
   trade worth making for 6 mm.

3. **Needs sign-off (or explicit deferral): enabling the coupler on a live
   world.** This is where the observable risk is, and it is *not* being flipped
   on by this ADR. Before `enabled = true` may be set anywhere:
   - **Determinism/multiplayer.** The coupler is a second simulation whose
     state (membership, dwell counters, sheet depth) must replicate or be
     derived identically on every client, exactly like `WaterCA`. It is
     deterministic and order-independent by construction, but it is **not yet
     wired into the replication path at all**. Enabling it before that is a
     guaranteed desync — the same class of bug as ADR-0003's item 2, and it
     should be landed the same way: plumbing first, flag second, as separate
     commits.
   - **Terrain-edit signalling.** The eligibility predicate and the puncture
     test both query solidity every tick, so the coupler inherits ADR-0003's
     caller obligation wholesale. It does not add a new one, but it does not
     escape it either.
   - **Renderer.** Per the ownership section, the visible water surface becomes
     the *union* of sheet depth and CA fill. A renderer drawing only one of
     them will show water vanishing at a boundary.
   - **Tuning `absorbPerTick`** against the fastest CA inflow the design
     permits, so the in-transit residue stays small.

   **Recommendation: DEFER enablement** to the M3-networked water integration,
   where the replication path is being built anyway, and keep this ADR's scope
   to items 1 and 2. There is no gameplay value in enabling it before a
   renderer and a replication path exist to consume it.

## Consequences

- W4's design question is answered and the plan's named slip risk is retired:
  the coupling is specified, implemented, and tested, and the two non-obvious
  errors in it (the ownership leak through inactive columns, and bed re-seating
  on puncture) were found and fixed **before** the coupler is live anywhere.
- voxel-core now contains a second simulation layer with its own version
  constant (`kSweVersion`), deliberately independent of `kWaterCAVersion` so
  the two can evolve without invalidating each other's goldens.
- The precedent this sets is worth stating plainly, because it will come up
  again: **"this algorithm is conventionally floating point" is not by itself
  grounds for a doctrine deviation.** The productive question is which
  *formulation* of the physics is natural in integers. Here that reframing
  produced a solver that is exactly conservative, exactly well-balanced and
  bit-identical across vendors — properties the float version could not have
  offered at any price.
- If item 2 is rejected, W4 has no viable path that preserves doctrine, and
  W5 (foam/buoyancy/boats) has no force field to consume. That should be
  weighed as part of the decision rather than discovered later.

## Follow-ups (not in scope here)

- **Lateral orifice / breach flux** between an SWE column and a *confined* CA
  neighbour (dig a hole in a dam wall, as opposed to the lake floor). Designed
  in `swe.h` §5's perimeter discussion but not implemented; the puncture path
  covers the floor case, which is the marquee one.
- **Sparse/tiled residency.** The reference grid is a dense rectangle on
  purpose, so the conservation and order-independence arguments stay legible.
  A production sheet tiles it; the tick rules do not change.
- **W3 hookup.** `rivernet.h`'s `kDivertChannel` diff is still a documented
  stub, and its discharge could drive SWE inflow at network outlets.
- **GPU port.** The tick is Jacobi and order-independent with no colouring
  needed (unlike the CA's 8 colours), which makes it a markedly easier port
  than `WaterCA` was.
