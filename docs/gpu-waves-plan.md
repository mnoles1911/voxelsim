# Build GPU waves A–F, then push R0 to 128 m

## Context

ADR-0006 recorded G0–G5 complete. They are not. G2/G3 built the **destination** — a
GPU-resident geometry pool drawn as one primitive in one draw call — and never
switched the **producer**. Chunk geometry is still meshed on CPU worker threads by
`MeshChunkBricks` (`ue-project/Source/VoxelEarth/VoxelChunkMesher.h:70`), dispatched
from `VoxelWorldSubsystem.cpp:5952`. The ADR's invariant 1 is unbuilt.

`docs/gpu-roadmap-remaining.md` (committed on `claude/gpu-roadmap`) sorts what is
left into waves A–F. This plan executes all of them in order, and finishes with
the change they exist to enable.

**The destination is a recorded decision.** `docs/gpu-g0-sizing.md:117-141`:

> **Decision (Matt, 2026-07-24): target R0 = 128 m** … It cannot ship on the CPU
> path, and the arithmetic is not close … Shipping this before generation moves to
> the GPU would take the exact symptom being chased and multiply it by four.

| | R0 radius | R0 chunks | cascade total | cascade edge |
|---|---|---|---|---|
| today | 64 m | 2,035 | 10,503 | 2 km |
| target | **128 m** | **~8,100** | **~16,600** | **4 km** |

Ring counts are flat per level (`gpu-g0-sizing.md:50-54`), so R0 = {0,128} shifts
every ring outward one slot and the world edge doubles for free. **This is why the
waves run in order A→F and R0 moves last:** moving it first quadruples the near
field on a CPU path that already cannot keep up.

**Not in scope:** the reported ring-gap symptom. The owner will investigate it
manually in a later session using `docs/manual-verification-checklist.md` item 1.

---

## Ground rules for every wave

These are the traps this programme has actually fallen into. They are not
boilerplate.

1. **Never conclude from a single run.** Two identical configurations have
   repeatedly differed more than the effect being measured (43 fps vs 103 fps on
   the same settled scene; 21.0 vs 47.4 ms p50 on an idle box). Every performance
   claim needs ≥2 legs per config and a stated noise floor. A published set of
   percentile improvements was retracted this way once already.
2. **Pin the camera.** `-VoxelPerfFlight=static` with `-VoxelPerfYaw=`/`-VoxelPerfPitch=`
   pins position *and* rotation and logs the pose. Unpinned poses swamp the signal.
3. **Rebuild voxel-core before believing any terrain.** `VoxelEarth.Build.cs:96-143`
   now warns on a stale `voxelcore.lib`. That staleness has already sent two
   investigations into the wrong subsystem and produced one wrong conclusion
   reported to the owner. If the warning fires, stop.
4. **A pooled primitive fails silently.** It renders *something* whatever you do.
   Screenshot-diff against a measured same-path noise floor; never accept
   "looks fine". The biome-tint result (17.4% → 4.3% against a 1.1% floor) is the
   standard.
5. **Let the world load before capturing.** A screenshot taken as the session opens
   shows an empty world and reads as a rendering bug. **Wait for the cascade to
   settle** — the full 2 km cascade takes ~10–30 s — and confirm it from the log
   (`jobsInFlight=0 pendingJobs=0`, `pending` 0 on every ring) rather than by
   counting seconds. Every capture must state the settle evidence next to it. This
   also means every screenshot is a *settled, stationary* scene, which structurally
   cannot show a movement-induced gap — so never offer one as evidence about
   streaming under motion.
6. **Never resolve a merge conflict with `git checkout --ours`.** It replaces the
   whole file with HEAD's version and still compiles — it silently deleted 22
   references to a feature earlier in this programme.
7. Branch per wave, PR per wave, CI green before the next wave starts. **CI does
   not build the UE project** — it covers voxel-core only (gcc/clang/msvc
   determinism digests, the float ban, and the shader lint). Every UE-side compile
   and every measurement in this plan is local, so "CI green" is necessary and
   nowhere near sufficient.
8. **Every claim in a PR body must name how it was measured**, or be marked as
   unverified. This programme has now retracted one set of numbers and corrected
   **two** root-cause diagnoses (the determinism gate twice); all three would have
   been caught by this rule.
9. **Line numbers in this plan drift.** `VoxelWorldSubsystem.cpp` is ~10,600 lines
   and moves under every PR. Anchor on symbol names — `DispatchJobs`,
   `DrainResults`, `MeshChunkBricks`, `EnsureVolumeOrigin` — and treat the numbers
   as hints. Several quoted here were already stale when this plan was written.

## How to use this document

This is the execution plan other sessions should follow. It is written **from the
code**, not from summaries — which is why it contradicts
`docs/gpu-roadmap-remaining.md` in four places, all listed at the bottom under
"Corrections owed".

**Each wave's PR updates its own section here with what was actually measured**, so
this stays a record rather than an intention. If a wave's stated cause turns out
wrong, correct it in place and say so — do not delete the wrong version, the same
way `backlog.md` §6a keeps its superseded diagnosis visible.

## Order of execution

```
A (cull) ─┐
B (GI)   ─┼─ parallel, disjoint files ──▶ C green ──▶ D1–D4 ──▶ D5 ──▶ F (R0=128m)
C (gate) ─┤
E (G4)   ─┘
```

A, B, C and E run concurrently as sub-agents. D cannot start until C is green (it
writes GPU-generated voxel state) and until A's buffer work has landed. D5 starts
only after D1–D4 are measured. F is last, because it is the change every earlier
wave exists to afford.

---

## Wave A — per-chunk frustum culling for the pool — **LANDED 2026-07-26**

**In-game:** frame cost stops scaling with *resident* geometry and starts scaling
with *visible* geometry. This is the precondition for `voxel.Stream.GPU` being
defensible as a default, and for R0 = 128 m not making frames worse.

### Result

**The cull is correct and the pool now scales with visibility.** The cause was
the second of the three hypotheses A1 was built to separate: **`SV_VertexID` does
not include the draw's base vertex on D3D12**, so every range drew pool quads
`[0, Count)` instead of `[First, First+Count)`. Fixed by passing the range's start
explicitly — `QuadIndex = VoxelRange.BaseQuad + VertexId/6`, with every draw
starting at vertex 0.

p50, two legs per config, one box state (L1 / L2):

| pose | component | pooled-uncull | pooled-cull |
|---|---|---|---|
| horizon (yaw 200, pitch −5) | 9.42 / 9.89 | 12.47 / 12.46 | **9.34 / 9.31** |
| straight down (pitch −89) | 2.25 / 2.30 | 12.60 / 12.65 | **2.42 / 2.43** |
| **horizon → down** | −76% / −77% | **+1.0% / +1.5%** | **−74% / −74%** |

**Defaults: `voxel.Stream.GPUCull` → 1. `voxel.Stream.GPU` stays 0.** The pool
now scales with visibility and beats the component path at the horizon, but it is
still **0.15 ms** behind it straight down — small, but consistent across both legs
and larger than the harness resolves. The rule that decided this was written down
before the deciding legs ran; see below.

### The defect, as it was

Pinned camera, settled scene, identical geometry both paths,
`loaded=9822 quads=8813242`, three legs each:

| camera | component | pooled | delta |
|---|---|---|---|
| horizon p50 | 15.12 ms | 18.58 ms | **+23%** |
| straight down p50 | 5.39 ms | 19.05 ms | **+253%** |

The control is the second row: looking at almost nothing makes the component path
64% cheaper and leaves the pool unchanged. A renderer whose cost does not depend
on what is on screen is not culling.

**This wave reproduced that defect and then removed it in the same run.** The
`pooled-uncull` config is the old behaviour, unchanged, and it still measures
**+0.7%** horizon→down — invariant, exactly as before. `pooled-cull` over the same
pose pair, same scene, same harness, measures **−74%**. One config still shows the
defect and one no longer does, which is as clean a before/after as this programme
has produced.

### A1 — the experiment, and what it said — **RESOLVED: outcome 2**

`GPUCullDebugSplit N` tiles the pool into N exact contiguous ranges — the same
quads as the single full draw, with the frustum test bypassed. Run at N = 2, 8
and 64 on a settled scene (`jobsInFlight=0 pendingJobs=0`, `loaded=9822
quads=8828373`), screenshot at 40 s:

| outcome | predicted | observed |
|---|---|---|
| identical to the full draw | `SV_VertexID` fine, fault in the frustum test | **no** — geometry visibly missing at every N |
| **first half renders twice, second half missing** | `SV_VertexID` does **not** include the base vertex | **YES** |
| second half missing, first half renders once | the draw itself is being rejected | **no** — see the cost row |

At every N, **only the first 1/N of the pool reached the screen** — at N=64 a
single sliver of terrain in an otherwise empty world. Pictures alone cannot
separate outcomes 2 and 3, because both leave exactly the first 1/N visible. The
**cost** separates them:

| config | drawn fraction of pool | p50 |
|---|---|---|
| full draw | 100% | 12.26 ms |
| split 2 | first 50% visible | 12.02 ms |
| split 8 | first 12.5% visible | 11.86 ms |
| split 64 | first 1.6% visible | **14.07 ms** |

If the draws were being *rejected* (outcome 3) the vertex workload would have
fallen 64-fold and the cost with it. Instead the cost stayed flat and then **rose**,
because each of the 64 draws still processed its full quad count starting at pool
quad 0, and N=64 added 63 draw calls on top. That is outcome 2, and only outcome 2.

*These four cost figures were taken before the exclusive-box protocol existed and
may have been contended. They are load-bearing only as a 64× directional argument
— cost failing to fall while 63/64 of the world vanished — which contention
cannot manufacture. Do not quote them as clean numbers.*

*This is the experiment the earlier control failed to be. That control was
retracted in `52c8a73`: "a single range spanning first-visible..last-visible
covers 98% of the pool no matter whose runs are in the array. The experiment could
not fail."*

### A2 — confirmed: the engine declines to make the assumption the code made

**The zero-stride theory is dead.** `GNullColorVertexBuffer` is 16 bytes with
stride 0, so the fetch address is `Base + StartVertexLocation * 0` — always in
bounds. A1 settles it independently: the draws executed and cost full price, they
simply addressed the wrong quads. Nothing was rejected.

**The engine's own header was the right lead**
(`RHI/Public/DataDrivenShaderPlatformInfo.h`):

```cpp
// Returns true if SV_VertexID contains BaseVertexIndex passed to the draw call,
// false if shaders must manually construct an absolute VertexID.
inline bool RHISupportsAbsoluteVertexID(const FStaticShaderPlatform P)
{
    return IsVulkanPlatform(P) || IsVulkanMobilePlatform(P);
}
```

D3D12 is not in that set, and `FLocalVertexFactory` compensates by threading the
base through a uniform buffer (`VF_VertexOffset`). The comment that used to sit in
`VoxelGpuPoolComponent.cpp` — *"SV_VertexID includes the draw's start vertex for a
non-indexed draw"* — asserted exactly what the engine declines to assume. The
caveat that every in-engine call site of that helper is an *indexed* draw is now
moot: A1 measured the non-indexed case directly.

### A3 — the fix, as built

Every range draws from vertex 0 and names its start explicitly, so `VertexId`
unambiguously runs `[0, Count*6)` and the result is correct whether or not the
platform includes the base vertex.

- `FVoxelQuadRangeParameters` — one `SHADER_PARAMETER(uint32, BaseQuad)`,
  registered shader-side as `VoxelRange`, created per range with
  `CreateUniformBufferImmediate(..., UniformBuffer_SingleFrame)`.
- Carried on `FMeshBatchElement::UserData` as `FVoxelQuadRangeUserData`, allocated
  with `Collector.AllocateOneFrameResource<T>()`. A stack local does not survive to
  draw submission — `GetDynamicMeshElements` only *gathers*; bindings are read
  later when draw commands are built.
- **Not** a loose `FShaderParameter`. Those measurably do not bind here, and
  `ShaderBindings.Add()` on an unbound parameter is a **silent no-op** — that
  version would have compiled, run, logged nothing, and drawn the wrong geometry.
- Elements with no `UserData` bind the factory's `BaseQuad = 0` buffer, so the
  single full-pool draw and the single-chunk component compute the identical
  expression to before. The uncull path is unchanged, not merely equivalent, which
  is what makes it usable as the control.
- Shader change is one expression, in the one place `QuadIndex` is derived.

**Visual gate — pixel identity, not similarity.** Settled scene, pinned pose,
`r.AntiAliasingMethod 0`, 1280×720, HUD masked, threshold 8/255:

| comparison | pixels differing |
|---|---|
| full draw vs 64-way split | **0.00%** (0 / 745,600) |
| full draw vs frustum cull | **0.00%** (0 / 745,600) |
| same-config repeat (noise floor) | 1.81% |

The floor turned out to be **bimodal**, not noise-like: captures fall into two
clusters (some per-session latch, probably eye adaptation), **0.00% within each
cluster** and 1.81–1.82% between, identical magnitude across all four cross pairs.
Each cluster contains one unmodified full draw *and* one changed-path capture — so
the culled image and the 64-way split image are each pixel-identical to an
unmodified full draw, not merely close to one. Wave A's falsification condition
("culled and unculled frames differ visually at all") is met by construction.

**Also fixed, all three of which actively mislead a reader:**

- `voxel.Stream.GPUCull`'s help text said **"1 = on (default)"** while the default
  is 0 — that is how someone concludes the cull is active when it is not.
- **`voxel.Stream.GPUCullMergeGap` was dead**: declared, documented, referenced
  only from a comment; the merge ran entirely off a threshold derived from
  `GPUCullMaxRanges`. It is now the *first* merge pass, with the range-cap merge as
  the backstop that still guarantees the element limit by construction.
- **`GPUCullMaxRanges` defaulted to 256** against an element loop of
  `(1ull << BatchElementIndex) & BatchElementMask` — undefined for index ≥ 64.
  Capped at 64, including the debug-split path.

Deleted: the two vestigial `LAYOUT_FIELD(FShaderParameter)` bindings (neither name
had existed in the `.ush` for some time; both reported `IsBound() == 0`), and
`FVoxelChunkDrawData` with the `UserData` pointer at it. That pointer was dead
code, but `UserData` is now *read* by the factory, so a differently-typed pointer
there would have become a type confusion rather than merely unused.

### A4 — measured

`-VoxelPerfFlight=static`, two poses, ≥2 legs per config, 50 s per leg,
`postWarmup` (t ≥ 10 s) percentiles. Every leg logs its own settle state and scene
size beside its timing, so scene equivalence across configs is visible rather than
assumed.

Every leg below settled identically — `loaded=9822 quads=8828373 jobsInFlight=0
pendingJobs=0`, pool `liveChunks=9818 highWater=8823501` — so scene equivalence
across configs is observed, not assumed.

| leg | pose | config | p50 | p95 | ranges | visibleQuads | drawnQuads |
|---|---|---|---|---|---|---|---|
| L1 | horizon | component | 9.42 | 10.99 | — | — | — |
| L2 | horizon | component | 9.89 | 11.47 | — | — | — |
| L1 | horizon | uncull | 12.47 | 12.83 | 1 | — | 8823501 |
| L2 | horizon | uncull | 12.46 | 12.80 | 1 | — | 8823501 |
| L1 | horizon | cull | 9.34 | 9.68 | **64** | 2285770 | 6221534 |
| L2 | horizon | cull | 9.31 | 9.70 | **64** | 2285770 | 6228356 |
| L1 | down | component | 2.25 | 3.02 | — | — | — |
| L2 | down | component | 2.30 | 3.21 | — | — | — |
| L1 | down | uncull | 12.60 | 13.22 | 1 | — | 8823501 |
| L2 | down | uncull | 12.65 | 13.36 | 1 | — | 8823501 |
| L1 | down | cull | 2.42 | 3.12 | 29 | 164534 | 266656 |
| L2 | down | cull | 2.43 | 3.17 | 26 | 164534 | 270838 |
| L1 | down | allvisible | 12.66 | 13.28 | 1 | 8823501 | 8823501 |
| L2 | down | allvisible | 12.60 | 13.16 | 1 | 8823501 | 8823501 |

**Controls held across passes**, which is what licenses reading the two passes as
one measurement: horizon-uncull 0.08%, down-uncull 0.4%, horizon-cull 0.3%,
down-component 2.2%, horizon-component 5.0%. Four of five repeat under half a
percent. *Recorded because it matters below: the 5.0% pair is the one the
pre-registered resolution floor was derived from — the least representative of
the five, chosen in advance and in good faith.*

**Method note, recorded because it cost a run.** The first attempt produced six
clean legs and then six invalid ones: the **component** path — which this wave does
not touch at all — moved from 10.38 ms to 29.75 ms between passes. When the
untouched control triples, nothing in that pass is a measurement, and it was
discarded whole rather than reasoned around. The box is now held by one agent at a
time by explicit handover rather than by checking for a running process; a process
check cannot distinguish "finished" from "between legs".

**Second process note, from this wave's one wrong turn.** The straight-down
residual was attributed, in an earlier draft of this document, to the cull's own
per-frame render-thread work. The mechanism was plausible and the code reading
behind it was correct — `BuildCulledRanges` really is called inside the per-view
loop and really does handle shadow frustums, so it really does run several times
per frame, each O(runs) over ~9,800 chunks plus a sort. It costs **0.06 ms**. Two
lessons worth more than the corrected paragraph:

- **A structural confirmation is not a cost confirmation.** "This runs N times per
  frame over M elements" and "this is where the time goes" are different claims,
  and the first was allowed to stand in for the second by everyone who looked at
  it, author and reviewer alike.
- **The evidence for the hypothesis came from a run we had already agreed was
  contaminated.** A `p95` of 9.91 ms against a `p50` of 2.44 was read as
  "occasional expensive frames", which fits a periodic CPU cost. On a quiet box the
  same config measures p95 **3.12** ms. The signature was contention. Theorising on
  a number already ruled inadmissible is the actual error here; the fix is that a
  discarded run is discarded for *all* purposes, including as the motivation for a
  hypothesis, not just as a source of headline figures.

The probe was still worth building. It converted a plausible story into a measured
0.06 ms for the cost of one leg, and a null result that kills a wrong explanation
is a better return on a leg than a confirmation would have been.

#### Decision rule for `voxel.Stream.GPU`, fixed before the deciding legs landed

The straight-down residual (pass 1: pooled-cull 2.42 vs component 2.25,
**D₁ = +0.17 ms**) is the same size as this harness's plausible resolution, so the
flip decision must not be made by looking at pass 2 and deciding what it means.
The rule below was written and committed **before `down-component-L2` and
`down-cull-L2` were observed**.

Let `D_i = down-cull-L_i − down-component-L_i`, paired **within** a pass, since the
two legs of a pass share a box state and the drift being controlled for is drift
*between* passes.

Let the resolution be

> `R = max( |down-component-L2 − down-component-L1| , 0.050 × mean(down-component) )`

The first term is the direct yardstick: the untouched control, same pose, same
config, differing by nothing but time. The second is a **floor**, and it is why
this is not simply the obvious rule — with n = 2 a spread can come out near zero by
luck, which would make the test arbitrarily strict and let a single lucky pair
manufacture a "significant" 0.17 ms. The floor is the fractional spread already
measured on the horizon control pair (9.42 → 9.89 ms, **5.0%**), the only estimate
of this harness's repeatability in hand that was not taken at the pose under test.
It puts R at **≥ 0.11 ms** whatever pass 2 does.

- **Flip justified** — `D₁ ≤ R` and `D₂ ≤ R`, *and* pooled-cull ≤ component at the
  horizon on both legs. The pool would then be at parity where it was worst and
  ahead where it was already fine.
- **Flip waits** — `D₁ > R` and `D₂ > R`. A consistent regression at the pose the
  wave exists to fix, however small in absolute terms.
- **Inconclusive** — the two legs straddle R. Then the verdict is *inconclusive* and
  the recommendation is more legs. Not "pick the leg that agrees with the
  conclusion I prefer".

**Burden of proof sits on the flip**, because enabling a default is the action with
a blast radius and leaving it disabled costs only that this wave's win goes
unbanked for another wave. So a marginal pass resolves to *waits* or *inconclusive*,
never to *justified*.

`D₁ = 0.17` is already on the board, so the flip requires `R ≥ 0.17 ms` — a control
spread of 7.6% at the down pose, against the 5.0% measured at the horizon.
**Stated in advance: I expect "waits", marginally.** If pass 2 says otherwise, the
rule decides, not the expectation.

**Not contingent on any of this:** `voxel.Stream.GPUCull` → 1. It rests on the
pixel-identity gate and the scaling result (+1.0% → −74%), both of which are
already established and neither of which is a near-noise comparison. The only
precondition is that the controls agree across passes; if they do not, the pass is
struck and nothing here is read at all.

#### Verdict under that rule

Direct spread |2.30 − 2.25| = 0.05 ms; floor = 0.050 × 2.275 = 0.114 ms; so
**`R` = 0.114 ms**, resting on the floor.

| leg | down-component | down-cull | `D` | vs `R` |
|---|---|---|---|---|
| L1 | 2.25 | 2.42 | **0.17** | > R |
| L2 | 2.30 | 2.43 | **0.13** | > R |

Both legs exceed `R`; neither straddles. **Flip waits — `voxel.Stream.GPU` stays 0.**

**The verdict is robust to the floor being wrong, which is the only reason it is
worth anything.** `R` rests on the discredited generous floor, and the floor is the
*flip-favourable* assumption — a larger `R` makes the residual easier to dismiss as
noise. It fails anyway. The direct spread, the better estimate, is 0.05 ms, less
than half the floor and a third of the residual, so it drives the same verdict
harder. Both yardsticks agree and the one to trust gives the cleaner margin.

*Stated so it is not read as stronger than it is:* on the floor alone, L2 clears
`R` by 0.016 ms, which is itself below resolution — on that yardstick L2 is a tie,
not a pass. The verdict rests on the direct spread. No extra control legs were
spent: with both yardsticks agreeing, more of them could only tighten `R` downward
and strengthen a verdict already reached.

### What the residual actually is — and a cost model that fell out of it

The cull's own numbers, logged per frame (L1 / L2):

| pose | visible | drawn | over-draw | ranges | cull vs component |
|---|---|---|---|---|---|
| horizon | 25.9% | 70.5% | **2.72x** | **64 (capped)** | pool ahead by 0.08 / 0.58 ms |
| straight down | 1.86% | 3.02 / 3.07% | **1.62x** | 29 / 26 (uncapped) | **pool behind by 0.17 / 0.13 ms** |

**Two earlier readings of this table were wrong, and the run killed both.**

*Wrong reading 1: the residual is the cull's own render-thread work.* Measured
directly with `GPUCullDebugAllVisible`, which runs the entire CPU cull and then
keeps every run, so the GPU draws exactly what `uncull` draws and the delta is the
CPU cost alone: **+0.06 ms (L1), −0.05 ms (L2)**. The two probes bracket zero and
the sign flips between passes. `BuildCulledRanges` really does run per view per
shadow cascade, O(runs) over ~9,800 runs plus a sort, and it costs nothing
measurable. *Caveat kept for honesty: with everything visible the merge collapses
to a single range (`ranges=1` in both probe legs), so the gap sort runs over a
degenerate list. The probe bounds the O(runs) scan — the part that runs identically
either way — not the sort. The scan is free.*

*Wrong reading 2 (this document's own, now retracted): "whatever the residual is,
it is not geometry, and compaction cannot remove it."* It is geometry. The
argument that it wasn't leaned on 3.0% drawn against 1.9% visible looking like
"near-optimal culling" — but that is **1.62x over-draw**, 102,122 quads the pool
draws and the component path does not.

**The cost model that settles it.** Taking `uncull` and `cull` at the same pose as
two points on cost-versus-`drawnQuads`:

| pose | Δ quads | Δ p50 | implied |
|---|---|---|---|
| horizon | 2,601,967 | 3.13 ms | **1.203 µs / 1000 quads** |
| straight down | 8,556,845 | 10.18 ms | **1.190 µs / 1000 quads** |

Those two slopes are derived from completely different poses, geometry sets and
frame times, and they agree to **1.1%**. That is cross-validation, not a fit. The
pooled path costs a fixed floor of roughly 1.9–2.1 ms plus ~1.19 µs per 1000 quads
actually drawn, and nothing else in these numbers needs explaining.

Apply it to the residual: the 102,122 excess quads straight down predict
**0.122 ms**, against a measured 0.17 / 0.13 ms. The over-draw from range merging
accounts for essentially the whole gap.

**So the guidance to Wave D reverses.** The earlier draft warned that compaction
would not fix this because the cost was fixed per frame. The opposite is true: the
residual is over-draw, compaction drives `drawnQuads` to `visibleQuads` by
construction, and at 1.19 µs/1000 quads that removes ~0.12 ms straight down and a
far larger ~4.7 ms at the horizon, where over-draw is 2.72x and the 64-range cap
does bind. **Compaction is the fix, and it is worth more than this wave was.**

*How the wrong version survived as long as it did:* its supporting evidence was a
`p95` of 9.91 ms against a `p50` of 2.44 ms, read as "occasional expensive frames"
— a periodic-CPU-cost signature. That measurement came from the contaminated run.
The same config on a quiet box is p95 **3.12 / 3.17 ms**. There was never a tail to
explain.

### Deferred, not dropped — and there is a real tension to record

The ADR says twice that the pool **must compact into one contiguous draw range per
frame**, because `RHIMultiDrawIndexedPrimitiveIndirect` is never called with
`MaxDrawArguments > 1` and D3D12 implements the single-draw entry point on top of
it. N draw ranges is the thing the design says not to do, and multi-element batches
additionally foreclose the static-relevance upgrade path (static-mesh command
caching requires exactly one `FMeshBatchElement`, `PrimitiveSceneProxy.cpp:164`).
**The multi-range draw shipped here is explicitly an interim.**

**What `BaseQuad` means for that end state, learned while building it.** An
indirect draw supplies `StartVertexLocation` from a GPU buffer — and A1 just
measured that D3D12 does not put `StartVertexLocation` into `SV_VertexID`. So an
indirect draw *cannot* express "start at pool quad F" either. Anything drawing a
sub-range of the pool needs the base in-shader regardless of how the draw is
issued; this wave built the CPU-sourced version of a value the GPU-driven version
would write itself. The end state that genuinely reaches one draw is therefore not
"indirect draw plus a base" — it is **compaction**: a compute pass writing a
compacted quad-id list, after which the shader reads
`QuadIndex = CompactedIds[VertexId/6]` and `BaseQuad` is 0 forever. That is an
indirection, not an offset. `QuadIndex` is derived in exactly one place in the
`.ush`, which is the line that change would replace.

**The project has zero indirect draws today** — no `DrawPrimitiveIndirect`, no
indirect args anywhere in `ue-project/`. Revisit after Wave D, when the pool is
already GPU-written and the chunk table already lives on the GPU.

---

## Wave B — GI volume, steps 3–5

**In-game:** lighting stops being baked into geometry, so an edit updates lighting
by texel upload instead of by re-shading and re-uploading vertex colours. Cave and
dig lighting become correct at range instead of only within 12.8 m of spawn.

Steps 0–2 landed and are verified: the pooled path feeds the light field (it
previously never did — `voxel.GI.Enabled 1` was a silent no-op under
`voxel.Stream.GPU`), the volume is sampled **per pixel** via a `GIUVW : TEXCOORD1`
interpolant folded into vertex colour `.g` in `GetMaterialPixelParameters`
(`ue-project/Shaders/VoxelQuadVertexFactory.ush:50-55,238-251,389-430`), and the
±Z encode matches the CPU sampler at **0.000 mean error** against a half-cell
shifted control.

**Correction to carry into this wave.** The roadmap framed step 5 as "stop
re-meshing a chunk to refresh its lighting on the pooled path". That is not what
exists: `BrickComponents` has **no entry** for pooled bricks
(`VoxelEarth/VoxelGI.h:154-161`), so pooled chunks are never re-shaded at all, and
`CollectDirtyChunks` (`VoxelWorldSubsystem.cpp:7438-7473`) already expands an edit
only to the 1-voxel mesher apron, never to a lighting radius. The 5×5×5 re-shade
that is genuinely retirable lives on the **component** path
(`voxel.GI.EditDirtyRadiusBricks` → `RefreshQueue` → `UpdateGIVertexColors`).
Wave B's real prize is therefore (a) making the volume authoritative on **both**
paths and (b) deleting the quad subdivision, which is where GI's median cost
actually lives.

- **B1 (step 3) — settle the encoding.** The doc's bar is "mean horizontal error
  under 8/255 ⇒ Scheme B is free quality" (`gpu-gi-volume-design.md:103`); the
  measurement is 5.95–6.165, so **the expected outcome is: keep Scheme B, and
  record why**. Two loose ends first: `maxAbsErr` is ~102 bytes, so the passing
  mean hides a heavy tail worth characterising; and the number is a mean-abs where
  §2 asked for an **RMS**. Also reconcile the two transcripts in the tree — the
  design doc records `5.950 / control 0.591`, `backlog.md:301` and the roadmap
  record `6.165 / control 0.565`. Different runs, never reconciled.
- **B2 (step 4) — camera-following re-centring.** `EnsureVolumeOrigin`
  (`VoxelGI.cpp:482-555`) latches on `bVolumeOriginSet` and never re-enters, so at
  the shipped `voxel.GI.VolumeDim 64` only **36 of ~1,950** resident bricks are
  inside the volume, and `DrainVolumeUploads:585-589` silently **drops** the rest.
  Design is already decided (§4): **dead zone + staged re-upload over ~8 frames,
  with the origin uniform swapped on exactly the frame the last upload lands** —
  toroidal addressing and double-buffering were both considered and rejected. Add
  the proposed `voxel.GI.VolumeRecentreCells` (default 64). Needs a driver that can
  call `UpdateParameters_RenderThread` more than twice per session, and a
  re-address of `VolumeShadow`.
- **B3 (step 5) — make the volume authoritative and delete the baked path's cost.**
  Retire the component path's 5×5×5 re-shade for chunks inside volume coverage, and
  **turn off `voxel.GI.MaxQuadSpanVoxels` subdivision** (`VoxelChunkComponent.cpp:188,314-331`)
  when the volume supplies GI. Keep `NotifyPooledChunkMeshUpdated`'s quad hand-off
  — the field must still be fed.
- **B4 — reconcile the two shade formulas** before either can be authoritative in
  the same frame under `voxel.Stream.GPUMaxLevel`. Four live divergences, all found
  in the tree:
  1. `Strength`/`AmbientFloor` are **hardcoded** in `UpdateParameters_RenderThread`
     (`VoxelGIVolume.cpp:98-125`) rather than read from `voxel.GI.Strength` /
     `voxel.GI.AmbientFloor`. They coincide today (1.0 / 0.06) and diverge silently
     the moment either cvar moves.
  2. The fade radii are **derived from the volume extent** (896/1152 UU at Dim=64)
     instead of the CPU's 4800/6400. This is the design doc's risk 8, live: GI cuts
     off at the volume face as a plausible-looking lighting ring.
  3. The shader's `DistUU` is distance to the **volume centre**, not to the camera —
     equivalent only while the origin is camera-centred, which B2 is what makes true.
  4. The shader tries **one** probe offset (0.6 cells); the CPU tries three
     (`{0.6, 1.25, 2.0}`, `VoxelLightField.cpp:398-415`) specifically because a
     20 cm roof slab is half a cell thick. Thin geometry falls back to plain AO in
     the volume path where the CPU finds data.
- **B5 — close the two untested step-2 claims:** the X-run upload merge on a dig's
  contiguous neighbourhood (measured at 1.4 bricks/run in *steady state*, but the
  dig case is what it was built for), and zero-on-revoxelize / zero-on-evict, which
  are correct by construction only. Both need a dig-shaped test.
- **B6 — fix the live-toggle claim.** `voxel.GI.Volume`'s help text says it is read
  per frame; `UpdateParameters_RenderThread` has only three callers and no per-frame
  refresh, so `Enabled`/`DebugVis` are latched. B2's driver is the same hook — either
  make it true or correct the text.

- **B7 — ship it on.** Owner's call: Wave B is complete when **`voxel.GI.Enabled 1`
  and `voxel.GI.Volume 1` are the defaults**. That adds two obligations beyond
  correctness:
  - **Size the volume for real coverage.** `voxel.GI.VolumeDim 64` covers ±12.8 m.
    The design doc's sizing table recommends **N=256 → 51.2 m, 67.1 MB**
    (`gpu-gi-volume-design.md:119-124`); full-coverage parity at 349 MB is
    explicitly ruled out. Pick from a measured resident-brick-coverage run, not the
    table alone, and note `GetDim()` clamps to `[16,256]` and rounds down to whole
    bricks (`VoxelGIVolume.cpp:41-52`).
  - **Retune the fade** (design risk 8, and B4.2 above). Beyond the volume face, GI
    must hand back to plain AO without a visible ring. This is the one part of Wave
    B that a harness cannot score — it goes on the manual checklist.
  - **Measure the frame cost with the pinned-pose harness**, ≥2 legs, GI on vs off.
    The material is masked, so the depth pass samples the volume too
    (`gpu-gi-volume-design.md:230-236`). If the cost is not acceptable, report the
    number rather than shipping it on regardless.

**Verification:** `voxel.GI.VolumeCheck` (`VoxelGI.cpp:727-987`) — the harness that
produced the 0.000 / control pair — re-run after each step; shots at 15/30/45 s on
the scripted flight for B2 (no lighting pop, origin uniform changing on exactly one
frame per re-centre); a dig-and-hold PIE capture for B3; a pinned-pose A/B for B7.

---

## Wave C — the determinism gate — **GREEN, landed 2026-07-26**

**In-game:** nothing directly. But while this was red, GPU-generated voxel state
could not be trusted, and Wave D writes exactly that. **Wave D's correctness
precondition is now met.**

### Result

| leg | toolchain | result | digest |
|---|---|---|---|
| `bench/vxc_gpu.exe` | DXC `cs_6_0` → SPIR-V → Vulkan | **PASS**, bit-exact, ×2 legs | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE `cs_6_6`/`6_8` → DXIL → D3D12 | **PASS**, bit-exact, ×2 legs | `6e893ab3679a8c81` |

8,192 columns / 393,216 cells / 6,668 quads, AMD Radeon RX 7800 XT, both legs on
the same box on 2026-07-26.

### The cause: worldgen version skew inside one process

**Neither C1a nor C1b. Not the toolchain and not the shader.** The failing run
compared a **worldgen v6 CPU reference** against a **worldgen v8 GPU kernel**:
`voxelcore.lib` predated the v8 climate landing (`e25d563`, on `main` from
`2c7eb68`, 19:28) while Unreal compiled the current `worldgen.ush`. The gate went
green the moment the library was rebuilt (02:00) and the editor relinked (03:00);
no source commit in between touched `worldgen.ush`, `shaders/prebuilt/`,
`VoxelGpuWorldGen.cpp` or voxel-core's amplifier.

Established by reconstruction, not by argument — three isolated bench runs:

| CPU build | shader | result |
|---|---|---|
| v6 (`2c7eb68^1`, separate worktree and build dir) | its own v6 SPIR-V | **PASS** `f3c48a4df3e20e9a` — the reconstruction is faithful |
| **v6** | **current v8 SPIR-V** | `cpu=2 gpu=5` @ vz=11648, `cpu=5 gpu=12` @ vz=11654 — **the recorded failure, exactly** |
| current v8 | pre-mirror v6 SPIR-V (`3fbf3f7^`) | the mirror image, `cpu=5 gpu=2` / `cpu=12 gpu=5` — so the shader was not the stale half |

Corroborating: the recorded `3424 quads (cpu 3422)` is GPU-then-CPU, and 3,422 is
measured to be the **v6** quad count for that region while 3,424 is the **v8**
count. The two numbers on that one line came from different worldgen versions.

### C1a and C1b: both falsified, both cheaply

- **C1a — `$Globals` packing.** Clean. Disassembling both variants (`dxc -Fc`)
  gives byte-identical constant-buffer layouts: `DispatchColumns` 0,
  `RasterOriginPx` 8, `RasterSize` 16, `PixelSizeMm` 24, `SeedLo` 28, `SeedHi`
  32, `OriginVx` 36, `OriginVy` 40, **`BrickZMin` 44**, `BricksZ` 48,
  `ScanCount` 52, total 56 bytes — identical for the `VXC_UE` `$Globals` at
  `cs_6_6` and the bench's explicit `cbuffer` at `cs_6_0`. The proposed "write
  `BrickZMin` into an unused output slot" probe was not needed: a packing
  mismatch cannot coexist with bit-exactness over 393,216 cells, which the DXIL
  leg now delivers.
- **C1b — int64 codegen across shader models.** Clean. UE compiles the DXIL leg
  at `cs_6_6`/`6_8` and `tools/compile-shaders.ps1` compiles the SPIR-V leg at
  `cs_6_0`; they agree bit-for-bit. Nothing to isolate.
- **C0 — source/bytecode skew.** Re-verified live rather than on the record:
  `tools/compile-shaders.ps1` respun all seven kernels and every `.spv` came out
  **byte-identical** to the committed bytecode (SHA-256 matches
  `shaders/prebuilt/README.md`'s v8 table).

### What was actually changed, and why

The gate was already green when Wave C opened, so the work is the guards that
stop this failure presenting the same way a fourth time.

1. **`voxel.GPU.VerifyRegion` pins a CPU-reference digest** (`kExpectedCpuDigest`),
   folded from `vxc::Amplifier`/`vxc::meshBrick` with nothing from the GPU in it.
   A mismatched `voxelcore.lib` now fails with *"the linked voxelcore.lib is NOT
   the worldgen this gate is pinned to"* instead of a list of cell materials, and
   it catches both sides moving **together**, which GPU-vs-CPU equality
   structurally cannot. This is **not** a re-baseline of the Unreal GPU digest —
   `046b4a9f9c5e49b7` is recorded nowhere.
2. **Mismatches are classified by stage** (column field / cell / quad), counted
   uncapped, and the printed list is labelled as an ordered subset that must not
   be quoted piecemeal.
3. **A compile-time worldgen version lock in `worldgen.ush`.**
   `VXC_WORLDGEN_VERSION_USH` must equal `vxc::kWorldGenVersion`, which
   `ModifyCompilationEnvironment` passes in as `VXC_WORLDGEN_VERSION_CPP`; a
   mismatch is a shader `#error` (verified by compiling with a deliberately wrong
   value, and with the define absent). Because defines feed the shader's DDC key,
   a version bump now also forces a recompile instead of silently reusing the
   previous version's bytecode. Scope stated in the file: `kWorldGenVersion` is a
   header constant, so this catches **source** skew and not a stale `.lib` —
   that is guard 1's job, plus `VoxelEarth.Build.cs`'s timestamp warning.

`worldgen.ush`'s only edit is preprocessor-level and inside `#ifdef VXC_UE`,
which is why the SPIR-V respin is byte-identical: the bench leg is provably
running the same program as before.

### The lesson, which is not about shaders

Three published root causes, all wrong, and the same mechanism produced all
three: **each was derived from a transcript rather than from a re-run.** The
failing log's own first lines named the cause — column fields disagreeing — and
were dropped when the log was excerpted into `backlog.md`. Everything downstream
then had to explain a cells-only divergence, which is a kernel-shaped fault, so
three kernel-shaped hypotheses followed.

The cheap habit that closes it: **before theorising about a compiler, rebuild the
static library and re-run.** A stale `.lib` is not a "separate issue" from a
determinism-gate failure; it is one of its two likeliest causes and by far the
cheaper one to eliminate.

### Still owed

- **C2 — NVIDIA determinism leg.** Never run; the cross-vendor claim still rests
  on one AMD card. Now unblocked. **Not run here: this box has no NVIDIA GPU and
  no rented one was available to the session.** `tools/run-nvidia-digest.sh` is
  the entry point.
- **C3 — min-spec-proxy M1 gate re-run.** Not attempted, deliberately: it is a
  60 s frame-time measurement and this box was running three other build/editor
  agents concurrently. Ground rule 1 exists because numbers taken that way have
  already been retracted once. It also wants to land after Wave A, which changes
  what it measures.
- **`gpu-streaming-plan.md:63-64`, `streaming-handoff.md:12,120` and
  `gpu-g2-draw-path.md:115` still quote the pre-v8 bench digest
  `f3c48a4df3e20e9a`.** Re-baselining those to `6e893ab3679a8c81` is legitimate;
  left alone here only to stay off files other waves are editing.

### The original analysis, kept for the record

Same HLSL, same GPU, same CPU reference:

| leg | toolchain | result | digest |
|---|---|---|---|
| `bench/vxc_gpu.exe` | DXC → SPIR-V → Vulkan | **PASS**, bit-exact | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE → DXIL → D3D12 | **FAIL** | `046b4a9f9c5e49b7` |

Localised (`docs/backlog.md:219-243`): all 4,096 columns match, quad decode matches
exactly over 20,544 vertices, **cells differ on material ids only** and in one
direction — the DXIL build's soil column sits one layer shallower
(`cpu=2 gpu=5`, `cpu=5 gpu=12`).

### The recorded diagnosis is wrong. Do not chase it.

`docs/backlog.md:238-242`, `docs/gpu-roadmap-remaining.md:74-84` and the C1 entry of
an earlier draft of this plan all name **floating-point contraction** as the prime
suspect. **That hypothesis does not survive reading the shader.**

- `voxel-core/shaders/worldgen.ush` contains **no floating-point arithmetic at
  all** — every operand from raster to material id is `int64_t`/`uint64_t`, which
  the file's own header states (`worldgen.ush:6-13`). The only `float`/`precise`
  hits in 1,660 lines are inside comments. **There is no `<` for an FMA to flip by
  one ULP, because there is no float to hold a ULP.**
- Neither leg passes any FP flag. And on the D3D12 path in UE 5.8,
  `CFLAG_NoFastMath` **is never translated** — `TranslateCompilerFlagD3D11`
  (`D3DShaderCompiler.cpp:52-61`) maps only `PreferFlowControl`,
  `AvoidFlowControl` and `WarningsAsErrors`; everything else returns 0, and the DXC
  path has no `-Gis` / `D3DCOMPILE_IEEE_STRICTNESS` equivalent. **"Force strict
  IEEE for `VoxelizeMain`" is not a fix that can be written.**

This is the third wrong root-cause on this one failure. Record the correction in
`backlog.md` §6a and the roadmap alongside the existing one, rather than deleting
it — same reasoning the last correction used.

### What the evidence actually supports

The real compile-flag deltas between the legs:

| | bench (PASS) | Unreal (FAIL) |
|---|---|---|
| **target profile** | **`cs_6_0`** | **`cs_6_6` / `cs_6_8`** |
| **parameter binding** | `cbuffer WorldGenParams : register(b0)` | loose `$Globals` scalars (`VXC_UE`) |
| backend | `-spirv`, vulkan1.1 | DXIL, `-auto-binding-space 0` |
| optimisation | `-O3` | `-O3` (identical) |
| FP flags | none | none |

- **C1a — the `$Globals` packing hypothesis (test first).** Under `VXC_UE`,
  `worldgen.ush:396-406` declares loose globals instead of the bench's explicit
  `cbuffer` (`:408-422`). A packing mismatch on `BrickZMin`/`BricksZ` would produce
  **exactly** the observed signature: all 4,096 columns match, quad decode matches,
  and cells are wrong by a consistent vertical shift of one layer. Cheapest
  possible test: log the `BrickZMin` the shader actually receives (write it to an
  unused output slot) and compare to what the CPU passed. If they differ, this is
  the whole bug and no compiler theory is needed.
- **C1b — int64 codegen across shader models.** This file has *already* produced one
  cross-vendor divergence of exactly this class: the M0 AMD-vs-NVIDIA failure was
  64-bit `OpSDiv`/`OpSRem` with mixed-sign operands, fixed by routing through
  `absToU64` + `OpUDiv` (`worldgen.ush:146-199`). If C1a is clean, force UE to
  `cs_6_0` (or the bench to 6.6) and see which way the digest moves — that isolates
  shader model from backend in one run.
- **C0 — source/bytecode skew (ten-minute elimination).** The bench runs **committed
  SPIR-V**, not a fresh compile. Confirmed in sync — `worldgen.ush` and
  `worldgen.VoxelizeMain.spv` were last touched by the same commit (`3fbf3f7`) — so
  this is already eliminated on the record and needs re-checking only if either
  file moves.

Any edit to `worldgen.ush` passes CI's shader-lint job (signed `/` and `%` outside
the approved `floorDiv`/`truncDiv` helpers, wave-width assumptions, unclamped
writes) **and** requires a matching SPIR-V respin via `tools/compile-shaders.ps1`,
or the skew C0 eliminated is reintroduced deliberately.
- **C2 — NVIDIA determinism leg.** Never run; the cross-vendor claim rests on one
  AMD card. ~$5, ~20 min on a rented box. Only meaningful once C1 is green.
- **C3 — min-spec-proxy M1 gate re-run.** Owed, and now plausibly passable.

**Do not re-baseline the Unreal digest.** `046b4a9f9c5e49b7` is the output of a
build that disagrees with the CPU; recording it would turn a loud failure into a
silent one. Re-baselining the *bench* value is legitimate whenever wanted.

**Falsified if:** the flags are already identical — in which case the divergence is
in DXIL codegen itself and the fix is a targeted `precise` qualifier on the layer
comparison inputs, mirrored into the SPIR-V leg and re-gated.

---

## Wave D — GPU meshing into the streaming path

**In-game:** chunk geometry stops being produced by ~24 saturated CPU worker
threads. This is the change that makes R0 = 128 m affordable: `gpu-g0-sizing.md`
measures ~8,100 R0 chunks at **~0.09 s** on the GPU against **~11.5 s** on the CPU.

Gated on Wave C being green.

### D1 — make the pool GPU-writable (`VoxelGpuPoolComponent.cpp:117-147`)

- Add `EBufferUsageFlags::UnorderedAccess` to the quad and chunk-id buffers; create
  UAVs alongside the existing SRVs.
- Bring both into RDG via `ConvertToExternalBuffer` / `RegisterExternalBuffer` — the
  pattern `docs/gpu-g2-draw-path.md:89-93` prescribes and the shipped code did not
  follow.
- **Buffer identity must stay stable.** The factory bakes these SRVs into a uniform
  buffer built once in `InitRHI`. In-place UAV writes are fine; reallocation is not.
- **Neutralise the CPU shadow clobber — the highest-severity silent-corruption risk
  in this wave.** `PooledQuads`/`QuadChunkIds` (`VoxelGpuPoolComponent.h:148-152`)
  are a full CPU mirror, and `CreateSceneProxy` re-uploads all of it. Any
  `MarkRenderStateDirty` path would overwrite GPU-written quads with stale CPU
  content — presenting as **terrain reverting to older geometry after an unrelated
  event**. Mark GPU-owned ranges and skip them on rebuild, or drive rebuilds from a
  re-dispatch.

### D2 — gated `MeshEmit` variant emitting pool-ready quads

`voxel-core/shaders/worldgen.ush` emit block, `VoxelGpuWorldGen.cpp:455-468`.
Today's quads are brick-local (`slice/u0/v0` are 0..7 inside one 8³ brick). Bake in
(a) the brick→chunk offset, giving chunk-local 0..31 exactly as `MeshChunkBricks`
produces; and (b) a **pool base offset** so the shader writes straight into the
chunk's allocated range — it already writes at `InQuadOffsets[maskIndex] + local`,
so this is an added uniform, not a restructure.

**Must be a gated permutation, not an edit in place:** the digest gate hashes quad
fields, so changing emitted bytes changes the digest even though quads are
display-only.

### D3 — allocation without a big readback

Upper-bound reservation does not work (static bound ≈98k quads/chunk against a
typical ~900, and the draw's vertex count comes from `Pool.GetHighWaterMark()`, so
over-allocation directly inflates the thing Wave A just proved the renderer is
bound by). **Read back only the 4-byte scan total per chunk**, which the scan
already computes. Latency, not bandwidth:

1. frame N: dispatch into a **persistent** scratch buffer (external, not an RDG
   transient — today's `Voxel.Quads` dies at `GraphBuilder.Execute()`), enqueue the
   count readback;
2. frame N+k: count lands → `Pool.Alloc(exact)` → GPU→GPU compaction copy into the
   allocated range;
3. CPU writes the chunk-table entry as it does today.

### D4 — wire the async runner into dispatch

**Most of this already exists and is unwired.** `FVoxelGpuMeshJobManager`
(`VoxelEarthShaders/{Public,Private}/VoxelGpuMeshJobManager.*`) is an async
per-request state machine with `Submit`/`Tick`/`CancelAll`, it calls
`GraphBuilder.Execute()` **without** `SubmitAndBlockUntilGPUIdle`
(`.cpp:380-383`), and its header already states the exactly-one-outcome invariant
(`.h:22-29`) that mirrors the CPU path's. Its own scope note says: *"Nothing here
is wired into DispatchJobs or the streaming path, and the brick-local → chunk-local
rebase still happens on the CPU after readback."*

So D4 is: delete the three `AddEnqueueCopyPass` calls at
`VoxelGpuMeshJobManager.cpp:370-375` and the poll/`Lock`/rebase tail, point the
emit at the pool UAV from D1/D2, and wire `Submit` into `DispatchJobs`
(`VoxelWorldSubsystem.cpp:5807`) behind a cvar, **level 0, non-edited chunks only**.
Do **not** build on `RunRegionBlocking` — it captures its outputs **by reference**
into the render command (`VoxelGpuWorldGen.cpp:493-502`), which is safe only
because of the flush at `:544`/`:587`.

**The invariant that must not break**, quoted from `VoxelWorldSubsystem.cpp:7183-7188`:

> every job DispatchJobs launches produces exactly one result, live or stale, so
> this is the matching decrement and must happen BEFORE the stale `continue` below
> or a ring would leak slots against its floor.

Note the two counters are decremented in **different places**, and a GPU path has to
respect both: `JobsInFlightCounter` is decremented on the **worker thread** at
enqueue (`:6532-6535`), while `LevelJobsInFlight[]` is decremented on the **game
thread** in `DrainResults` (`:7188`). `FootprintBandCache` is read in `DispatchJobs`
(`:5931`, `:5994`, `:5324`) but written only in `DrainResults` (`:7101-7104`),
deliberately *before* the stale discard. A dropped result strands a whole (X,Y)
column via the cold-band throttle — a bug already diagnosed and fixed once here.

Decide explicitly whether "in flight" means dispatched or completed; with GPU
latency those differ by frames. Watch `StaleResultsDiscarded` (`:7191-7202`):
multi-frame latency makes stale results *more* common, and `GenerationId` is
snapshotted at `:6115` and compared at `:7191`.

**Batch across chunks.** A region dispatch is a brick-aligned slab with a 1-brick
halo, so meshing one 32³ chunk alone dispatches 48³ voxels — 3.4× waste. Batch a
footprint's column or a tile of neighbours and slice the quad stream by
`meshBrickIndex`. Cap: `MaskCount ≤ 65,536` (single-workgroup scan) ⇒ ≤1,365
interior bricks per dispatch.

### D5 — coarse-level GPU generation (levels 1–5) — **in scope (owner's call)**

`voxel-core/shaders/worldgen.ush` has **no level parameter**. Levels ≥1 are ~80% of
resident chunks and, under motion, ~75% of meshing work. This is the largest and
most determinism-risky piece in the programme: mirroring coarse generation
bit-exactly in HLSL and re-passing the Wave C gate **at every level**.

**Sequencing, not scope:** start D5 only after D1–D4 are landed and measured. The
plumbing (UAV pool, async state machine, exactly-one-result invariant, batching)
is shared, and debugging it against a novel generation path at the same time
doubles the search space for any failure.

Approach:
1. Add the level parameter to `worldgen.ush` and mirror `MakeCoarseLevelSampler`'s
   B×B amplifier evaluation (`VoxelWorldSubsystem.cpp:274-278`). Note the CPU cost
   is already flat in level, so this is a correctness port, not an optimisation.
2. **Gate each level independently.** Extend `voxel.GPU.VerifyRegion` to take a
   level and require bit-exactness at that level before enabling it in
   `DispatchJobs`. Ship level by level behind `voxel.Stream.GPUMaxLevel`, which
   already puts both producers in one frame.
3. The **ring skirt** has no GPU equivalent. `ComputeRingSkirtMask`
   (`VoxelWorldSubsystem.cpp:5397-5406`) returns 0 for level 0 — which is why D1–D4
   can ignore it — but coarse rings need the inward retaining wall or the cascade
   seam reopens as see-through edges. Mirror `RingSkirtMask` into the kernel as a
   uniform that forces the apron to `MAT_AIR` on flagged faces, exactly as
   `MeshChunkBricks`'s sampler does (`VoxelChunkMesher.h:119-134`).

**Falsified if:** any level cannot be made bit-exact against the CPU. Stop at the
last level that passes and ship that as `GPUMaxLevel`; do not relax the gate.

### What stays on the CPU

- **The 34×34 column grid and band reduction.** `FootprintBandCache` is reduced via
  `ColumnDeepestAirVoxel`, which needs `Col.cave.segs[]`, `shaftMarginSq`, `cavern`,
  `bedrockDepthMm` — fields `FVoxelGpuColumnSample` does not carry. The band feeds
  two admission/dispatch skips *and* the cold-band throttle, which deadlocks a whole
  (X,Y) column if bands stop arriving. So GPU meshing removes at most the ~55% of
  level-0 job time that is meshing, until the GPU column struct grows.
- **Edited chunks.** `NeedsOverlayAwarePath` already routes them to a game-thread
  path; the GPU never sees them. A carve-out, not a blocker.

### Verification, in this order

1. **Bit-exactness before any rendering claim.** Mesh the same chunk both ways,
   compare packed quads byte for byte. Extend `voxel.GPU.SpawnPool`'s existing CPU
   round trip into an A/B assert rather than inventing a harness.
2. **Screenshot diff** of GPU-meshed vs CPU-meshed level 0 against a measured
   same-path noise floor.
3. **Motion measurement** — per-ring dispatch rate × per-ring p50, before and after.
4. `voxel.Stream.GPUMaxLevel` keeps both renderers in one frame; it is the sharpest
   tool here.

**Falsified if:** GPU quads differ from CPU quads for any unedited level-0 chunk; or
`markTimeouts`/stranded columns appear; or the stale-result rate climbs enough to
negate the throughput gain.

---

## Wave E — remaining G4 parity items

- **E1 — per-chunk debug tints.** The last G4 item and the only one still needing
  `M_VoxelTerrain.uasset` edited. Storage is solved (`ChunkParams.w` is free); route
  is a `float4` `TexCoords` interpolant with the tint in `.zw`. **Encode identity as
  ZERO, not white** — the component path supplies only a `float2`, so `TexCoord0.zw`
  arrives as zero there regardless of the graph, and a naive unpack treating 0 as
  black would render every component-path chunk black the moment the material is
  regenerated. Debug-only in play.
- **E2 — water pool parity number.** `voxel.Water.GPU` is landed and renders
  correctly (2,231 cavern bricks / 28,862 quads in one primitive) but its comparison
  is honestly missing: same-path repeats differed 20–88%. Needs a scene whose only
  variable is the water (`-VoxelFloodTest=70`). Also record the constraint: one
  primitive means one translucent sort key for all water — harmless for the current
  constant-opacity material, but W5 fill-fraction shading, foam, caustics or
  refraction would each break it.
- **E3 — ring cross-fade: CLOSED, do not build.** Re-tested after the seam fix gave
  the annuli their overlap band; it still produces see-through patches at ring
  boundaries. It is the one item on the G0 checklist that should not be built.

---

## Wave F — R0 = 128 m

The recorded target, and the reason A–D exist. **Land last**, after Wave D is
measured green.

- **F1 — shift the cascade.** `kDefaultRingPresets`
  (`ue-project/Source/VoxelEarth/VoxelWorldSubsystem.h:92-99`) becomes
  `{0,128},{128,256},{256,512},{512,1024},{1024,2048},{2048,4096}`. **A whole-cascade
  re-layout is forced, not optional:** the annuli must abut exactly
  (`Outer[L] == Inner[L+1]`, asserted as an invariant at
  `VoxelWorldSubsystem.cpp:5082`), so R0 = 128 m cannot be changed alone without
  making R1 degenerate. Ring counts are flat per level, so every ring shifts one
  slot outward and **the world edge doubles to 4 km**.
  **No code change is needed to test it** — the shipped binary takes
  `-VoxelRingInnerMeters=0,128,256,512,1024,2048 -VoxelRingOuterMeters=128,256,512,1024,2048,4096`
  (parsing at `:2208-2231`). Prototype and measure before touching the default.
- **F2 — resize the pool.** `kPoolCapacityQuads = 14,000,000`
  (`VoxelWorldSubsystem.cpp:6701`) against 8.81M today. R0 alone goes 2,035 → ~8,100
  chunks at ~1,532 quads/chunk, adding **~9.3 M quads** — so 14 M is roughly 60%
  short and the target is ~22–24 M (≈180 MB). Size from the measured high-water mark
  of the F1 prototype, not from that estimate.
  Also: **the terrain pool never calls `SetChunkTableCapacity` at all.** It relies on
  `MaxChunks = Max(InOrigins.Num() * 4, ChunkTableCapacity)` with a default of 1024
  (`VoxelGpuPoolComponent.h:231`, `.cpp:117`) — the water subsystem is the only
  caller (`VoxelWaterSubsystem.cpp:526`). At ~16,600 chunks give it an explicit
  floor, or pay a full render-state rebuild each time it is crossed. Re-check
  `GetLargestFreeRun`/`GetFreeRunCount`, which the header flags as the early warning
  that allocations are about to fail on a pool that still looks half empty.
- **F3 — re-tune the ring budgets.** `kRingCapShare` and `kRingSlotFloorDefault`
  (`VoxelWorldSubsystem.cpp:1996,2038`) were sized for the current radii. Note the
  measured catastrophe on record: floors `{0,2,3,4,4}` reserved 13 of 24 slots and
  collapsed throughput from 49,179 chunks to 558. Change these one at a time, with
  measurement.
- **F4 — check `AVoxelClipmapActor`.** It derives its **entire** vertex spacing from
  `GetRingPresets()[GetMaxRingLevel()].OuterMeters` (`VoxelClipmapActor.cpp:529`), so
  doubling the cascade edge changes the 30 km heightmap's inner hole and spacing.
  Verify no seam opens between the voxel edge and the clipmap.
- **F5 — LWC/precision re-check.** The world is 84 km wide and float32 ULP is 1.0 UU
  at 8.4M UU; the rebase already happens in double precision, but a 4 km cascade
  puts more geometry further from the rebase origin. Confirm no vertex jitter at the
  new edge.

**Falsified if:** the F1 prototype's cold fill or motion residency is worse than
today's — in which case Wave D did not deliver and R0 must stay at 64 m.

---

## Verification summary

| wave | the measurement that decides it |
|---|---|
| A | `-VoxelPerfFlight=static` at two poses, ≥2 legs × 4 configs; pool must track the component path's visibility scaling |
| B | existing GI equivalence harness (the 0.000 / 0.565 control pair) + a dig-and-hold PIE capture |
| C | ✅ **DONE 2026-07-26.** Both legs bit-exact at `6e893ab3679a8c81`, ×2 legs each. C2 (NVIDIA) and C3 (M1 gate) still owed |
| D | packed-quad byte equality first; then screenshot diff vs noise floor; then per-ring dispatch-rate × p50 under motion |
| E | E1 screenshot on both render paths; E2 a water-only-variable scene, ≥2 legs |
| F | cold-fill time, motion residency per ring, pool high-water mark, clipmap seam screenshot |

Everything a human must confirm in the editor is appended to
`docs/manual-verification-checklist.md` as it arises, so the owner's PIE session at
the end has one list. `docs/gpu-roadmap-remaining.md` and `docs/backlog.md` §6a/§6b
are updated as each wave lands.

## Corrections owed to `docs/gpu-roadmap-remaining.md` (and `backlog.md` §6a/§6b)

Four, all found while planning this from the source:

1. **The determinism gate's "prime suspect" is impossible.** Both docs name
   floating-point contraction; `worldgen.ush` has no floating point in it, and
   UE 5.8's D3D12 backend cannot express the proposed fix anyway. Record the
   correction beside the existing one rather than replacing it — same reasoning the
   last correction used, and this is now the second wrong diagnosis on this one
   failure.
   **Settled 2026-07-26 — and this correction was itself incomplete.** Getting
   rid of the floating-point theory was right, but the replacement hypotheses
   (C1a `$Globals` packing, C1b shader model) were built on the same inherited
   error: *"all 4,096 columns match"*, which came from an excerpt with the
   `col(...)` mismatch lines dropped. The columns did **not** match, and the
   cause was worldgen version skew — a `voxelcore.lib` predating v8 against a v8
   kernel. Three wrong root causes on one failure, all three derived from a
   transcript instead of a re-run. See Wave C above.
2. **Wave A's stated cause is weak and its stated fix would not have worked.** The
   zero-stride buffer has stride 0, so a large `StartVertexLocation` is harmless;
   and "pass BaseQuad as per-element shader data" via a loose `FShaderParameter`
   would have hit the already-measured non-binding wall and failed **silently**.
   Both corrected in Wave A above.
3. **Wave B's stated purpose describes work that does not exist.** Pooled bricks are
   never re-shaded, so there is no lighting-driven pooled re-mesh to retire. The
   retirable cost is on the component path, plus the quad subdivision.
4. **The horizontal-error figure disagrees across the tree** — `6.165 / control
   0.565` in the roadmap and backlog, `5.950 / control 0.591` in the design doc's
   own transcript. Reconcile to one run, or record that they are two.

That is three of six waves whose published root cause or fix was wrong. Worth
noting plainly in the doc: **the roadmap was written from summaries, and this plan
was written from the code.**

## Parallelism

Waves A, B, C and E touch disjoint files and run concurrently as sub-agents:

- **A** — `VoxelGpuPoolComponent.cpp`, `VoxelQuadVertexFactory.*`, `.ush`
- **B** — `VoxelGIVolume.*`, GI paths in `VoxelWorldSubsystem.cpp`
- **C** — `worldgen.ush`, `VoxelEarthShaders` compilation environment, `VoxelGpuVerify.cpp`
- **E** — the material asset, `VoxelWaterSubsystem.cpp`

**D and F are serial and must not be parallelised**: D depends on C and on A's
buffer work; F depends on D's measurement. D5 depends on D1–D4's measured result.
