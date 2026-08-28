# The rasteriser-bound arm: RETIRED, with the full causal chain

2026-08-28. `voxel.March.Bound` stays default 0 permanently. The code remains in
the tree, disarmed, as the falsifier scaffold it built (the height-pyramid
precedent). Do not build a third version; this page is why.

## The two timing verdicts

    v1 (as designed):     marchMs 4.519 -> 4.897 (+0.38)   boundMs 5.607
                          net -5.99 ms  (gate >= +0.35: FAIL x17)
    v2 (cull + half-res): marchMs 4.797 -> 16.650 (+11.85) boundMs 11.7-12.4
                          net -23.6 ms  (FAIL x68, WORSE)

Both with engagement PROVEN: 30.9% of segments skipped whole, identity counters
holding, no crashes, no validation errors. The mechanism worked as specified and
the frame got slower every time.

## The three causes, in the order they were discovered

1. **Producer fill (v1's 5.6 ms).** ~80k chunk cubes, 7 min-blend slices, no
   early-Z. Estimated 0.15-0.5 ms; measured 5.6.
2. **Phantom slots (found by v2's cull counters).** The list pass drew 353k
   instances/frame against ~80k resident chunks -- freed pool slots keep stale
   record bytes that decode as plausible L0 chunks (the design's R7, at 4.4x).
   97% decoded L0; the cull removed only 10.14% because phantoms are scattered
   through the whole torus. Phantoms also widen every interval, so the bound
   proves little even where it is consulted.
3. **The consult itself is a net loss, twice.** With phantoms in BOTH runs, 30.9%
   real segment skips did not pay for the per-segment slice loads and the
   fallthrough debt (a bound removal opens the ladder exactly as an absent
   crossing does -- correct for holes, expensive for time). v2's dilation made
   far boxes bigger and the consult dearer: +0.38 became +11.85.

## Why not a v3 (the phantom fix is known and cheap)

Stamping freed records' level nibble invalid would fix cause 2. It would not fix
cause 3, which has been NEGATIVE in both measurements and is structural: the
skip's value per segment (20-40 cells whose brick-mask test is already free) is
small, and this project's standing rule -- "nothing that adds a per-step test can
pay inside this ring cascade" -- turns out to cover per-SEGMENT texture loads
too. The sky-ladder precedent applies verbatim: do not build the second (here,
third) attempt.

## What stands, for the record

- The census (91-99% removable ITERATIONS) was correct and its warning clause
  decided the outcome: iterations and time moved in opposite directions, at 68x
  the gate. The Stage 0a gate cost one day; the gates after it kept every
  iteration honest; total spend on a wrong idea was ~2 days instead of weeks.
- The Dreams-inversion analysis stands unchanged: ordering cannot move to a
  depth buffer here. Now the BOUNDING half is measured dead too. **The marcher's
  remaining lever is ray count, which is a product decision (internal
  resolution), not an engineering one.** That decision is with the owner.
- Salvage: the census counters (slots 49-55) and the cull-stats pattern
  (identity-checked two-buffer readback) are good instruments independent of
  this arm. The phantom-slot finding is REAL and may matter elsewhere -- any
  future consumer of VoxelBrickChunkTable must not trust freed slots' bytes.
