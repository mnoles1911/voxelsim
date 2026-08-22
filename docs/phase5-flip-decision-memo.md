# Phase 5 — what must be true before the terrain quad path can be retired

**This memo is the deliverable, not the flip.** The flip is the owner's call and
it should be made against a list, not a feeling. Every item below names its
evidence and says plainly whether it is met.

**Nothing here recommends flipping today. Six of nine items are unmet.**

---

## The checklist

| # | Must be true | Evidence | Status |
|---|---|---|---|
| 1 | The marcher draws what the quad path drew | depth gate at the cascade | **RUN IT** — spec in §10 |
| 2 | Hier and flat walks agree, or the disagreement is understood | descent verdict, gate v4 | **OPEN, NOT BLOCKING** |
| 3 | The ring cascade is measured, not merely resident | `entered[]`, `hit[]`, arm stamp | **MET** |
| 4 | The retirement arm provably engaged | plan §4 self-check + `GetUsedQuads()` | **RUN IT** |
| 5 | Guards that go permanently silent are re-expressed | 3 verifiers + `Uncheckable` + mutation pair | **WRITTEN, PROOF IN FLIGHT** |
| 6 | GI keeps its volume origin | escalation landed; hoist held | **PARTIAL — owner decision** |
| 7 | The pawn does not fall through the world | `HoldsTerrain()` | **WRITTEN** |
| 8 | Cold fill pre-registered before the leg | `phase5-coldfill-prereg-2026-08-20.txt` | **MET** |
| 9 | Frame-time evidence in a quiet window | p50/p95/hitch | **BLOCKED — owner's box busy** |

**Writing is done. Four items are now legs, one is an owner decision, one is open
and does not block.**

---

## 1. The marcher draws what the quad path drew — PARTIAL, and the order matters

The depth gate passed on the brick source: **0.0433% interior disagreement
against a 0.2671% reference-noise floor**. That was measured at **51.2 m, level
0, before rings existed**. It has never run at the cascade.

**And it cannot be run after retirement.** `voxel.March.VerifyDepth` compares the
marcher against **raster depth in the same frame** — the quad path *is* its
reference. Retiring the quad path removes the gate's own control.

> **THE DEPTH GATE MUST RUN, AT THE CASCADE, BEFORE THE FLIP.** Not after, not
> alongside. This is a sequencing constraint, not a preference, and it is the
> single item most likely to be skipped because the gate "already passed once".

## 2. The two walks — UNMET, and this is the honest blocker

Against an **identical index hash**, thousands of quiet frames, a pinned camera,
disagreement swung **48×** across three prints (178 / 8,597 / 726). The descent
verdict separates two mechanisms that swap dominance:

```
seq=1   60 lost:  neverDescended=  18  descendedAndMissed=  42
seq=2 8205 lost:  neverDescended=2485  descendedAndMissed=5720
seq=3  544 lost:  neverDescended= 522  descendedAndMissed=  22
```

* `descendedAndMissed` — the hierarchy entered the right brick and its inner DDA
  missed. **Not a data problem**: both walks read `VoxelBrickOcc` through
  textually identical expressions, so an unlanded payload makes *both* miss and
  produces no disagreement. It is a stepping difference at brick re-entry —
  doctrine 8's diagnosis, still unverified, whose implementation was a 188×
  regression and was reverted.
* `neverDescended` — the advance skipped past the brick. Doctrine 7, the
  chunk-level advance still being jump-and-re-floor, deliberately unfixed.

**Two defects, both already named, neither fixed.** Retiring the control arm
while the replacement disagrees with itself 48× frame-to-frame is the one thing
this memo exists to prevent.

## 5. The guards that go permanently silent — RE-DERIVED against the current tree

Plan §5's line numbers had drifted. The current sites:

| guard | file:line | fires on | under retirement |
|---|---|---|---|
| buried-skip soundness | `VoxelWorldSubsystem.cpp:13727` | `Result.QuadCount() > 0` | **can never fire** |
| solid-skip soundness | `VoxelWorldSubsystem.cpp:13745` | `Result.QuadCount() > 0` | **can never fire** |
| crown-chunk census | `VoxelWorldSubsystem.cpp:11930` | `Result.Quads.Num() > 0` | **always 0** |

The first two keep incrementing their `...VerifyCheckedSinceLog` counters, so the
census reads **"checked N, violations 0" — healthy** — while being structurally
incapable of reporting a violation. That is the failure mode this project has hit
repeatedly, and the code comment at :13721 already anticipates the *class* for
the CPU/GPU quad case. Retirement is the same failure one step further out.

**The re-expression exists and needs no new plumbing.** These guards ask "did a
chunk we claimed was empty / all-solid actually have content?" Under retirement
that answer lives in the brick pack, not in quads:
`FVoxelBrickCpuPack::bAnySolid` / `bAllSolid` (`VoxelBrickPool.h:226-227`) are
computed by the packer's own walk of the cell data.

* buried/empty claim → **violation if `bAnySolid`**
* all-solid claim → **violation if `!bAllSolid`**

**Each replacement must be proven able to fail before it is trusted** — the
standard set by the DXIL-distinctness gate and the two fit checks: compile or run
it against a deliberately violating input and require the failure. A guard nobody
has seen fail is a guard nobody knows works.

## 6. GI loses its volume origin — UNMET

Under retirement `ApplyMeshResult` takes its `NumQuads == 0` return and **never
reaches `GetOrCreateGpuPool`**, so the terrain pool is never created and GI's
deferral becomes **permanent**. The shader gates on `bInsideVolume`, so there is
**no error** — GI simply goes absent, for water too.

Note this is *not* the bug plan §6.1 describes. That one (GI adopting the first
`TObjectIterator` pool, possibly a water pool) has been fixed. The symptom is the
same and the mechanism is different, which is why the plan text misleads.

## 7. The pawn — UNMET

`HoldsGeometry()` is quad/component-valued. Under retirement every chunk reports
no geometry, so `VoxelCharacterMovement` holds the pawn in `bWaitingForTerrain`
permanently. **A gameplay break, not a metric break.**

---

## What the flip needs, in one sentence

**The depth gate re-run at the cascade against a quad path that still exists,
the two stepping defects either fixed or characterised well enough that the
marcher's disagreement is bounded, and items 5, 6 and 7 landed in the SAME
change that flips the default** — because each of them fails silently, and three
silent failures arriving together is indistinguishable from the renderer
working.


---

## 10. The depth gate at the cascade — exact spec

It has only ever run at **51.2 m, level 0, before rings existed**. It is the one
item most likely to be waved through on "it already passed".

**Cvars:**

```
voxel.March 2                  mode 2, scratch targets
voxel.March.HTileProbe 0       REFUSED by the gate otherwise
voxel.March.Source 1           brick pool
voxel.March.Rings 1            the cascade
voxel.March.RingCount 6        0-128 / 128-256 / ... / 2048-4096 m
voxel.March.StepBudget 3328    >= 2218, the ring-0 diagonal requirement
voxel.March.VerifyDepth 1
voxel.Terrain.RetireQuads 0    THE QUAD PATH IS THE GATE'S OWN REFERENCE
```

**That last line is the whole point.** The gate compares the marcher against
**raster depth in the same frame**. Retiring the quad path removes its control,
so this measurement is only possible BEFORE the flip and never after.

**Pass condition, v2, unchanged and not to be renegotiated:**

> interior disagreement rate <= the CONTROL's interior rate, AND interior max
> < 1.0 voxel — where the control is the raster depth against a one-pixel
> displaced copy of itself, through the same threshold and mask.

**What is NEW at the cascade and must be read per level, not pooled:**

* **"voxels" is not comparable across levels.** A level-5 voxel is 32x a level-0
  one, so the `< 1.0 voxel` threshold silently relaxes 32x by ring 5. Read the
  UU figure beside it and the per-level breakdown; a pooled max is meaningless.
* **Ring-discordant pixels are excluded**, per the rule pre-registered before
  the source existed: within +/- one chunk at the COARSER level of the boundary
  (6.4 m at 128 m). Own column, never folded into agreement.
* **`miss` collapses and `hitOnSky` becomes a real signal** — the marcher now
  sees terrain the quad path culled or LOD'd away. Neither is a failure.
* **Gate numbers are NOT comparable to the 51.2 m record.** Different reach,
  different population, different scales in one number.

**Expected outcome, registered before the leg:** the interior rate should stay
at or below the control as it did at 51.2 m (0.0433% against a 0.2671% floor).
If the CASCADE rings fail while ring 0 passes, the failure is in ring
transitions and not in the walk — which is exactly what the per-level split is
there to say.

---

## 11. The shortest path to a flippable state

Three legs and one decision. Nothing else is written work.

1. **`skipverify-mut-on` / `-off`** — in flight. Exactly one `(MUTATED)`
   violation at each verifier with it on, zero with it off. Closes item 5.
2. **The depth gate at the cascade** (§10), with quads ON. Closes item 1, and
   it can never be run after the flip.
3. **One leg with `voxel.Terrain.RetireQuads 1`**, reading the engagement
   counters and the `Uncheckable` lines. Closes item 4, and it is the first
   time the arm has ever been exercised.
4. **The owner decides the GI hoist** — move pool creation above the
   `NumQuads == 0` return, or accept GI absent under retirement. Closes item 6.

**Then the switch is flippable, with this named as not certified:**

> **The hier-vs-flat burst.** Against a frozen index hash, thousands of quiet
> frames and a pinned camera, disagreement swings up to 675x between prints and
> **the sign flips between legs**. Four legs, no surviving mechanism. The
> near-field ratio is unaffected (43.5x +/- 2% over six prints) and the
> CASCADE ratio is contaminated by it and stays unquoted.
>
> It does not block the flip: the marcher's correctness against the RASTER is
> what the depth gate measures, and that is a different question from the two
> walks agreeing with each other. But it is the reason the cascade skip ratio
> has no number, and it should be named whenever the cascade is described.
