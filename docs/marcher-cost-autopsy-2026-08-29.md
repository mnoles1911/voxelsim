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
