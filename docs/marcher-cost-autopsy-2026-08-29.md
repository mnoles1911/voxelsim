# The marcher's cost model, corrected: air is free, SEGMENT ENTRY is not

2026-08-29, the bound-arm autopsy. Three experiments on one binary, poses proven
pinned (the first two sweep attempts are recorded below as harness traps).

## The three readings

**1. The bound at ±pitch (AUT3, poses verified `STATIC pose pinned`):**

    pose        Bound 0   Bound 1   consult effect        boundMs (producer)
    sky  +30     5.765     5.876    +0.11 -- NOTHING       1.446
    down -90     1.347     3.671    +2.32 (pathological)   2.087

At the pose where the frame is ~100% empty-space walking, removing the empty
segments' INTERIOR (proven skips) saves nothing. **The air cost was never in the
iterations.**

**2. The ring line at sky:** RingCount 6 / 4 / 2 -> marchMs 5.765 / 4.101 / 2.087.
**Perfectly linear: ~0.9 ms per ring.** Combined with reading 1 (iterations
removed, setups kept, zero saving), the arithmetic isolates the term:
**segment ENTRY costs ~0.67 us/ray (~2,000 GPU cycles), and a sky ray pays it
up to 14 times (7 rings x 2 fallthrough rungs).** That is the 5.8 ms.

This also retro-explains: RingCount reduction's -1.356 ms (fewer entries); the
bound's failure with real skips (its empty-interval collapse "still calls the
walk" -- i.e. STILL PAYS SETUP, by its own design comment); and the census's
irrelevance (91% removable iterations were the free part).

**3. The producer is state-dependent (from the mismatched boundMs readings):**
12.4 ms on the long timing legs vs 1.4-2.1 ms at t=100 s on short ones -- phantom
slots are FREED-slot debris, and a longer/churnier session has more of them. As
built, the bound producer degrades with play time. Recorded as a second
independent disqualifier.

## Harness traps paid for here (both now known)

- `-VoxelSpawnPitch` is OVERRIDDEN by static flight: `VoxelPerfRun: STATIC pose
  pinned ... pitch=-15.0 (spawn pose)`. The pose switch for perf legs is
  **`-VoxelPerfPitch=`**. An entire 4-leg sweep measured pitch -15 while
  labelled -90/+30; caught because all poses read identically.
- `marchMs` prints only after a deferred `voxel.March.Stats`
  (`voxel.DeferExec 100 voxel.March.Stats` in -Cvars).

## The corrected model, and the three arms it orders

    cost(ray) ~= (segments entered) x ~0.67us  +  in-span hit work  +  small

**C (primary): enter fewer segments.** The ZCut shader was sound; its INPUT was
refuted (cumulative union, never narrows, 819 m pads). Repair the input: a
per-frame GPU reduce over the chunk table -> TIGHT per-level z-bounds (7 uniform
pairs, no per-ray state), and hoist the test ABOVE walk setup in the ring loop --
the bound's specific mistake (skipping inside the walk) explicitly avoided.
Ceiling at sky: several entries removed x 0.9 ms-ish each.

**A (falsifier first): wave occupancy.** The down-pose +2.32 consult anomaly and
the residual in-span costs may be divergence; a per-wave active-lane census
under HoleStats is a one-day kill test.

**B (reserve): temporal tStart** from last frame's own MarchHitT -- zero
producer, one texture read, conservative-early by construction. Only pays if
in-span prefix work matters after C; keep behind C.

**D (running now): anySolid at the SKY pose.** Its moving null was measured at
the horizon only; if resident-air record fetches contribute at sky (the
buried-skip removal made all that air resident-with-records), the already-built
bit shows it there. Cvar-only test.

**GATE RULE, new and mandatory: every marcher timing gate runs at sky AND down,
never horizon alone.** The bound passed design review against a horizon gate and
died at poses nobody priced.

---

# ADDENDUM (same day): the disease is 1/64 SIMD occupancy; clamps were symptoms

The ZTight arm — zero per-ray state, uniforms only, hoisted above setup, every
lesson applied — ALSO failed its gate: sky 5.858 -> 7.071 (+1.21), down 1.216 ->
1.632 (+0.42), with engagement REAL (8.49% of segments skipped, 0.35% of
retries). Third clamp arm, third failure, same signature: the permutation is
slower even where its skips fire.

**The wave census says why, in one number: waveWidth=64, meanActive=1.00.**
Trip-weighted mean active lanes on the deep iterations is ~1 of 64 — the
expensive work runs essentially serialized, 1/64 of the machine. That one
mechanism explains every prior observation at once:

- ~2,000 cycles per "segment entry" = ordinary code x64 serialization;
- removing iterations per-RAY saves nothing (the WAVE lives as long as its
  longest lane; a skipped lane just idles);
- cost linear in ring count (each ring extends the longest lane's lifetime);
- every clamp permutation regresses (added registers/branches tax an
  occupancy-marginal kernel by whole percents, and per-lane wins are invisible
  at 1/64 utilization);
- the bound's down-pose +2.32 pathology (any extra per-lane work x64).

**And a second monster in the same line: the sky fallthrough-retry rate is ~90%**
(58.8M retries entered vs 65.4M segments) — nearly every sky segment walks
TWICE. The 14x entry multiplier is real and is mostly the LADDER, not the rings.

## The programme, restated for the last time tonight

**Per-lane clamp arms are structurally dead in this kernel. Three bodies:
Bound (-6.0/-12.6), anySolid (null twice), ZTight (+1.2/+0.4).** Do not build a
fourth. All three stay in the tree, default 0, as instruments.

**Family A — occupancy — is the programme now**, with a theoretical ceiling
larger than everything tried tonight combined (the deep work runs at 1/64):
1. Survivor compaction / two-phase march: march N steps, compact live rays into
   dense waves, continue. Published shape (Aila & Laine; wavefront tracing).
2. The 90% sky retry rate as a COHERENT target: sky waves retry together, so a
   wave-coherent ladder gate (not per-lane) could halve sky segment count
   without per-lane divergence cost. Needs its own falsifier: measure the
   retry rate's wave-coherence first (are retries taken by whole waves?).
3. Base-kernel register diet as an enabler (the fatness that makes every
   permutation regress is itself an occupancy tax).

Verify before building: the wave census metric is trip-weighted (that IS the
cost weighting, argued at the site) — one sanity pass on the counter's math on a
down-pose leg (short walks, expect meanActive high if the metric is honest:
down-pose deep trips are few). meanActive down-pose reading is the census's own
falsifier: if it ALSO reads 1.00 there, suspect the instrument, not the machine.
---

# CORRECTION (minutes later): the 1/64 reading was the INSTRUMENT

The census's own pre-registered falsifier fired: meanActive read 1.00 at the
DOWN pose too, where short walks should read high. Cause, confirmed in source:
`WaveActiveCountBits(true)` was evaluated INSIDE `if (WaveIsFirstLane())` -- a
branch where exactly one lane is active by construction. The counter could
only ever read 1.00. Fixed (count hoisted outside the predicate), remeasure
pending.

**Status of the addendum above: the three clamp failures are FACTS; the 1/64
occupancy story is a HYPOTHESIS awaiting the fixed counter.** The 90% sky
retry rate stands (it comes from the ZTight engagement partition, a different
instrument). And the process note owed here: the addendum's headline was
committed BEFORE its falsifier ran -- the falsifier then fired. The doc you are
reading is the correction the rule exists to force.
---

# FINAL (fixed counter): waves are FULL -- family A is dead too; what remains

    pose   chunk loop meanActive   leaf DDA meanActive     (waveWidth=64)
    sky         63.21 (99%)             25.42
    down        63.49                   51.50

The instrument now discriminates (poses and loops differ), so it is believed:
**the chunk loop runs at 99% lane occupancy at the sky pose. Divergence is not
the sky cost, and survivor compaction has nothing to compact.** The leaf DDA's
40% at sky is real but small (sky rays barely enter the leaf).

## The full kill-list, one line each

    empty-space skips (5 arms + Bound + ZTight)  iterations are not the cost
    ray count (half-res)                          owner-rejected on image
    divergence (family A)                         waves are 99% full
    per-lane anything                             three bodies, one pattern

## What is actually left, ranked

1. **The 90% sky retry rate** (58.8M retries vs 65.4M segments, ZTight's
   engagement partition -- an independent instrument, still standing). Near-
   universal at sky means waves retry COHERENTLY, so gating retries saves
   whole-wave time -- unlike every per-lane clamp. The legitimate gate needs
   design care (a retry is correctness when a coarse stand-in exists), and the
   first step is a counter: how many sky retries produce a HIT? If ~none, the
   retry is provably-useless work at that pose and a sound gate exists.
2. **Latency-bound with full lanes**: lanes-per-wave is NOT waves-per-SIMD.
   A register-fat kernel runs few waves per SIMD, leaving dependent-load
   latency (index -> record -> validate, ~2 loads/iteration) unhidden. This is
   consistent with EVERY observation, including permutations regressing on
   added registers. Falsifier: compiler stats (VGPR count / theoretical
   occupancy) from the shader toolchain, or a load-chain-shortening probe.
3. **The resolution lever** (product decision, already measured: 50% = 103.6
   fps p95) -- the only lever with a SHIPPED-QUALITY number attached today.

Process note, recorded with the same weight as the findings: tonight's
instrument shipped with a self-falsifier, the falsifier fired, the instrument
was fixed within the hour, and the corrected reading REVERSED the headline.
That is the cheapest reversal this programme has ever bought.
