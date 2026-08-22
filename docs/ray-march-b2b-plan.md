# P3-B2b — rings and the 4 km cascade

**Status:** **B-2b-1 BUILT 2026-08-20** (two rings, L0 + L1, 256 m, overlap 0),
behind `voxel.March.Rings` default 0. Sections 3-7 below describe what it does;
sections 1-2 include work deferred past it (six levels, per-level dirty rebuild).
Unmeasured until a leg runs.

**Why it is the critical path:** three workstreams are blocked behind it. The
quad path is built, gated and measured (2.49x cheaper per chunk, VRAM
2,197 MB -> 388 MB) but cannot be switched on, because with quads off the world
beyond 51 m is empty. Assets into the volume (~26 MiB measured, against a
1,110 MiB estimate) wait on the volume being the renderer. The shadow march's
64 m reach — the thing the owner actually sees in captures — is the same
limitation.

---

## 1. Ring transitions

**The geometry is simpler than it looks, and that is the main finding of this
plan.** `kDefaultRingPresets` is expressed as radial distance in metres, and the
ray starts at the camera with a normalised direction. So the distance from the
camera to `P(t)` **is** `t`, and each ring's segment is a plain interval:

```
ring L covers t in [inner_L * 100, outer_L * 100)   UU
R0 0–128 m -> t in [0, 12800)      R3 512–1024 m
R1 128–256 m                       R4 1024–2048 m
R2 256–512 m                       R5 2048–4096 m
```

No cone rule is needed to *select* the level, because residency already decides
it: only level-L chunks exist in ring L. `brick-volume-format.md` section 5 says
this outright — R1..R5 **are** levels 1..5. The cone rule becomes a
**cross-check** (does the level the ring gives match the level the footprint
wants?) rather than a selector, and disagreement between them is a finding about
ring sizing, not a per-ray decision.

**So the traversal is a loop over levels, each running the existing hierarchical
walk over its own t-interval, first hit wins.** The walk itself does not change:
chunk DDA -> brick DDA -> cell DDA, with level-L voxel size `0.1 * 2^L` m and
level-L chunk size `3.2 * 2^L` m.

### The frame origin must be snapped to 1024 level-0 voxels

A chunk at level L spans `32 * 2^L` level-0 voxels; at L5 that is 1024. The
camera-anchored frame origin is currently snapped to 32 (chunk-aligned at level
0 only). **Snapping to 1024 makes it chunk-aligned at every level**, which keeps
every level's `worldVoxel >> 5` exact and avoids a per-level fixup. One `>> 10
<< 10` instead of `>> 5 << 5`; costs at most 102 m of frame offset, which the
box already covers.

### What is needed from the pool side — ONE thing

**Ring residency extended by one chunk beyond each ring's outer radius.**

Section 5's overlap rule is a residency requirement as much as a traversal one:
the ray leaves ring L's grid and resumes in ring L+1's at the same world `t`, and
`mips.h` is solid iff >=4/8, so a thin feature can vanish one level up. The
mitigation is to overlap by one chunk and take the first hit found in either
level.

**CORRECTED 2026-08-20 — the "~2%" was wrong by about 4x, and it was measured.**
The overlap has already been tried and reverted. Padding every ring's admit band
measured:

```
resident chunks  +9.2%
p50 frame        14.9 -> 17.3 ms
chunks/s          968 -> 672
hitches             1 -> 47
```

The ~2% figure came from plan section 5's estimate, not from a measurement, and I
repeated it as though it were one. It is built behind `-VoxelRingOverlapChunks=`,
default 0.

**So the overlap is NOT free and must not be assumed by B-2b.** Two consequences:

1. B-2b-1 runs with overlap **0**, which is what makes the seam visible and
   attributable — a silhouette pop is then either the missing overlap or the
   traversal, and the ring-discordant column distinguishes them.
2. If the overlap is later wanted, its residency and frame cost must be measured
   **at the poses B-2b actually uses**, not inherited from that leg. 47 hitches
   is a quality regression the marcher would be paying for a seam it may not
   need — the marcher has no mesh, so the crack the overlap exists to hide may
   not exist for it at all. That is a question to test, not to assume either way.

**Nothing else is needed from the pool.** The index seam, the record contract and
the capacities all already carry `level` and are level-agnostic.

---

## 2. The chunk index across six levels

### The dimensions already work, unchanged

Ring L has outer radius `128 * 2^L` m and chunk size `3.2 * 2^L` m, so the chunks
across a ring are `2 * 128 * 2^L / (3.2 * 2^L)` = **80 at every level**. That is
the cascade property, and it means the existing `kDimXY = 128`, `kDimZ = 128`
grid is correct for all six levels with no resizing and no new aliasing analysis
— the `static_assert` generalises as written.

Cost: 8 MiB per level, **48 MiB across six**. Against a 388 MiB pool commit that
is affordable.

### The full rebuild does NOT survive six levels

Rebuild-and-upload-everything was chosen deliberately, to make stale slots
structurally impossible. At one level and 8 MiB on dirty frames that was free. At
six levels it is 48 MiB per dirty frame, and during cold fill that is every
frame — roughly 2.9 GB/s at 60 Hz. That is no longer a rounding error, it is a
streaming regression introduced by the renderer.

**Two steps, in this order:**

1. **Per-level dirty rebuild.** The pool's delta already carries `Key.Level`, so
   only the levels that changed are rebuilt. Most flushes touch one or two.
   Simple, keeps the no-stale-slot property *within* a level, and is likely
   sufficient.
2. **Scatter upload, only if (1) shows up in a frame time.** Tens of changed
   entries per flush, uploaded as a small index/value list and scattered by a
   compute pass. Measure before optimising — the same rule that kept the first
   version blunt.

**Staleness is already survivable either way**, and this is what makes (2) safe
when it comes: the record carries its own `OriginVoxel` and ring level, and the
traversal validates both. A stale index entry becomes a **miss**, never a wrong
chunk. That check has read `origin=0` mismatches across every leg since it was
added.

---

## 3. The ring-discordant column goes live

The rule was pre-registered in `VoxelMarchRenderer.h` before the source existed
and **is not to be adjusted after seeing a number**:

> A compared pixel is RING-DISCORDANT if its hit distance `t` lies within
> ±ONE CHUNK WIDTH of a ring transition radius, measured at the COARSER of the
> two levels meeting there. Such pixels get their own column, are EXCLUDED from
> the interior gate, and are never folded into agreement.

Band half-widths, by boundary: 6.4 m at 128 m, 12.8 at 256, 25.6 at 512, 51.2 at
1024, 102.4 at 2048. Small as a fraction of the ray at every level.

It has read exactly **zero** through every level-0 leg, which was the
pre-registered self-test — the classifier must not fire where no boundary is
crossed. B-2b is the first time it reports for real.

---

## 4. What the depth gate reports instead

Extending the reach **breaks comparability with every gate number recorded so
far**, and the log must say so rather than leaving it to memory.

| what changes | why |
|---|---|
| `miss` collapses | rays stop ending at 25.6 m |
| interior max in "voxels" mixes scales | a level-5 voxel is 32x a level-0 one |
| `hitOnSky` becomes a real signal | the marcher sees terrain the quad path culled or LOD'd away |
| the judged population changes character | far geometry is denser per pixel, so silhouette density rises |

**Changes to make:**

- **Per-level breakdown** of compared / disagree / interior-max, so scales never
  mix in one number.
- **Max reported in UU as well as voxels.** UU is comparable across levels;
  "voxels" is not, and the threshold `0.5 * voxelSize(level)` silently relaxes
  32x by level 5. Report the effective UU threshold beside it so the loosening
  is visible rather than implied.
- **A one-time banner** in the log stating that gate numbers are not comparable
  across the reach change.

---

## 5. The skip ratio at real range

~37x was measured over a 51.2 m box. The ratio should **improve** with reach —
skipping wins more the longer the ray — and 4 km is the case the entire
performance argument rests on.

**One design point that decides whether the number means anything:** the flat
control must be **flat within the ring cascade**, not flat at level 0 across
4 km. A level-0 flat walk over 4 km is 40,000 voxel steps and would inflate the
ratio into meaninglessness. So:

- **flat** = ring transitions, no chunk or brick skipping
- **hier** = ring transitions plus the full hierarchy

Same rays, same frame, same comparator. That keeps the ratio measuring the thing
it claims to measure, which a naive control would not.

---

## 6. Mode-hunting, designed in rather than bolted on

Every B-2b number will be read across a bimodal-or-worse population. Nine rounds
of B-2a were spent reading round-to-round deltas of a bimodal quantity as if it
were one.

**The mode is a property of the world, and the world is identified exactly by the
index content hash.** That is already implemented and already rides the readback.
So:

1. **Every comparator report prints a MODE FINGERPRINT as one greppable line:**
   index content hash, resident chunks per level, rays, and the hit population
   per ring.

   **CORRECTED 2026-08-20 — the "rate per million compared rays" asked for here
   was built and falsified before this section could be implemented.** Rays
   compared barely moves between legs, so dividing by it is *arithmetically
   inert*: measured at 72x per unit against 72.4x raw, i.e. the raw spread
   rescaled and nothing more. A denominator only normalises if it CO-VARIES with
   what it divides. The fingerprint carries per-ring hit counts instead, because
   those do vary and they say what the leg actually exercised — a leg that never
   reached ring 1 cannot speak about ring 1, and the line says so outright.
2. **Per-term counts are comparable across legs ONLY at equal hash**, and the log
   already says so. Extend it to the fingerprint line so the rule travels with
   the data.
3. **No conclusion from a single leg.** The standing protocol is repeats until a
   high mode is confirmed; the harness should bucket legs by hash automatically
   rather than a human eyeballing which are comparable.
4. **Report the defect's spatial shape, not just its magnitude** — disagreements
   per ring level, and per descent bin. A "high" leg should be recognisable by
   its *shape* from one leg, which is the closest achievable thing to making the
   mode identifiable without comparison.

Honest limit, stated: the mode cannot be made identifiable from a *single frame*,
because the world is fixed within a leg. Multi-frame sampling within a leg will
not reveal it. The hash plus the rate is the best available substitute and it is
sufficient for bucketing.

---

## 7. Recommended first step — B-2b-1

**Two rings (L0 + L1), reach 256 m, no overlap.**

Directly analogous to B-2a's "level 0 only", and for the same reason: it forces
the one new mechanism (transitions) and excludes everything that merely
multiplies it.

- **one** ring boundary, at 128 m — enough to exercise transitions completely
- overlap **0**, so the seam is visible and attributable (see section 1)
- index at **two** levels, 16 MiB — no rebuild-cost question yet
- the **ring-discordant column fires for the first time**, on one boundary, where
  it can be checked against a hand-computed band
- the skip ratio at **256 m against 37x at 51.2 m** — the first datapoint on
  whether the ratio improves with reach, which is the whole performance argument

**Deliverable:** ring-discordant behaves as pre-registered on one boundary, the
comparator stays at its floor across the transition, and one ratio datapoint at
5x the reach.

**Explicitly not in B-2b-1:** six levels, the overlap (so the seam is visible and
attributable), the per-level dirty rebuild, and any depth-gate restructuring.
Each is a separate, measurable step afterwards.

## PRECONDITION ADDED 2026-08-20: gate v4 on every level

The hier-vs-flat certification closed (`docs/measurements/ray-march-certification-close-2026-08-20.txt`).
Roughly 90% of the disagreement population turned out to be float ambiguity at
cell planes -- not a defect -- and the old gate (flat walk as truth,
disagreements to zero) was retired as ill-posed. Gate v4 replaces it and is
stated on the uncontested asymmetry, `flat-only <= 1.5 * hier-only`.

**This is a precondition on B-2b, not a footnote.** The residual defect is a
directional skip. **Corrected twice on 2026-08-20, and the claim did not
survive.** Both the "~20 rays in 1.35M" magnitude and the 2.2x-58.6x
asymmetries were artefacts of a comparison that discarded flat-side and
hier-side candidates at different rates. With all three of the arbiter's
boundary comparisons corrected, the one leg with statistical weight reads
1.23x -- a PASS. **The directional skip defect is not established and may not
exist**; no readable verdict exists yet in either direction.

The gate is still a precondition, because absence of evidence here is not
evidence of absence and **the severity scales with level** -- a skipped
brick is 0.8 m at level 0 and 25.6 m at level 5, where the same defect is a
hole the size of a building.

So: run gate v4 at every level the cascade adds, and set the bar for each level
*before* building it. The one number that must not be inherited from level 0 is
"this defect is negligible".

## DECISION 2026-08-20: proceed to B-2b-1, and why the FAIL does not block it

Gate v4 on the first clean population reads **FAIL at 1.53x** (flat-only 6,302,
hier-only 4,124, ARBFAIL collapsed to 2). That is a real asymmetry -- 21 sigma
from 1.0 -- and a small one: the hierarchy-specific excess is **2,178 rays,
0.16% of the frame**, against a flat walk that misses content on 4,124 rays of
its own.

**Proceeding is not "accepting a FAIL". It is the only way to get the
measurement that decides whether the FAIL matters.**

The question that actually governs this defect is whether the asymmetry is
LEVEL-DEPENDENT:

* If it holds near 1.5x as levels are added, it is a level-independent property
  of the walk. Then it is judged on its level-0 consequence -- 0.16% of the
  frame as scattered specks -- and that does not justify a third brick-loop
  rewrite (the second regressed 188x).
* If it GROWS with level, that is decisive and it must be fixed before the
  cascade ships, because the same skipped brick is 0.8 m at level 0 and 25.6 m
  at level 5. A hole that is speckle here is structural there.

**Neither branch can be distinguished without levels to measure across, and
B-2b-1 is what adds them.** Blocking on the level-0 number would mean tuning
the traversal at the one level where the defect is known to be harmless, with
no instrument for the levels where it might not be.

CONDITIONS, both already required by the plan and restated because this
decision rests on them:

1. Gate v4 runs **per level**, and the L1 reading is compared against the L0
   reading of 1.53x. The comparison IS the deliverable of B-2b-1, alongside the
   ring transitions themselves.
2. The consequence threshold for coarse levels is derived and registered
   BEFORE those levels are built -- from what density and size of missing
   terrain is visible in a frame, not from whatever the cascade happens to
   give. The 1.5x bar is a level-0 number and must not be inherited upward.

RISK ACCEPTED: if the asymmetry does grow with level, the ring-transition work
is still sound and the fix lands underneath it. The build is needed either way.
