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
   `-VoxelPerfStaticAt=X,Y,Z` (UU) pins somewhere other than the spawn column,
   which is the only way to aim this fixture at anything underground.
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
10. **Never edit a `.ush` while any editor run is in flight.** Shader source is
    loaded from disk at runtime, not compiled into the DLL, so an edit under a
    live editor kills the whole shader compile batch and presents as a GPU fault
    at `ShaderCompiler.cpp:2298` — not as an edit collision. It cost Wave B a
    whole batch of legs.

    **Scope, precisely, because the cheap version of this rule blocks work that
    is actually safe.** `VoxelEarthShadersModule.cpp:40-42` maps `/VoxelCore` to
    `FPaths::ProjectDir()/../voxel-core/shaders`, so an editor launched with
    `-project=<worktree>/ue-project/...` reads **that worktree's** copy and no
    other. Editing `worldgen.ush` inside your own worktree therefore cannot
    disturb another agent. (The DDC is not a collision vector either: differing
    preprocessed source yields different shader map keys, so the two coexist.)

    **THE EXCEPTION THAT MATTERS: not every wave works in a worktree.** Wave A
    works in the main checkout `D:\voxelsim`. So `D:\voxelsim`'s copy of any
    `.ush` is genuinely dangerous whenever an editor is running out of the main
    checkout, and it is the copy to leave alone. The rule is therefore *"never
    edit a `.ush` in a checkout an in-flight editor was launched from"*, and the
    safe habit is to do all `.ush` work in your own worktree. **Wave D5 changes
    `worldgen.ush` unconditionally and will hit this.**
11. **A code path with no recorded executions is untested, however green
    everything around it is.** Two waves hit this from opposite directions on the
    same day, which is why it is a rule rather than an anecdote.

    - **Wave G, the runtime shape.** The pool's shadow-level cap added an
      "everything was capped, draw nothing" branch. It is the one branch whose
      failure costs 13M quads a frame *and* looks exactly like the feature
      working. Across both measured legs it executed **zero times** — no scene
      naturally produces it. Forced with `voxel.Stream.GPUShadowMaxLevel -1`, it
      immediately exposed a defect: `FMeshElementCollector::AllocateMesh()` hands
      back a **recycled** `FMeshBatch`, so reading `Elements[0].NumPrimitives`
      before populating it returned a *previous gather's* value. The census
      reported 185,612,113 quads against a 13,088,897-quad pool.
    - **Wave D, the build-time shape.** A new `.usf` missing its
      `Platform.ush` include **links clean and reports a successful build**,
      because `IMPLEMENT_GLOBAL_SHADER` records only a path and an entry point.
      Nothing verifies the file compiles until something dispatches it.

    Both are the same failure: *the surrounding system is green because it never
    asked the question.* The habit that closes it is to **force every branch you
    add at least once, deliberately, and check its output is what you predicted**
    — a cvar set to an absurd value, a fixture that produces the degenerate case.
    A branch that has never run is a branch you have not written yet.

    **And note what actually caught the Wave G defect**, because the magnitude
    did not: the same run measured **3.5× faster** than the un-capped config while
    apparently drawing **14× more**. Two numbers that cannot both be true, one of
    which is the instrument. **When an instrument and a wall-clock disagree, the
    instrument is the suspect** — the draw was correct the entire time.

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

**The claim this wave is allowed to make, narrowed 2026-07-26.** Wave A's result
is in and it changes what Wave D may be sold on: with the cull working and near
optimal (drawing 3.0% of the pool against 1.9% visible), the pooled path
straight down still costs **2.44 ms against the component path's 2.15**, with a
3.3× worse p95. There is a fixed per-frame pooled cost that culling does not
touch and compaction will not fix. So **Wave D must not claim an end-to-end
frame win.** The defensible claim — and the only one `gpu-g0-sizing.md` actually
supports, and the one Wave F depends on — is **producer-side: per-ring chunk
throughput under motion, and cold-fill time.** Measure those; do not quote frame
time as a Wave D result.

### D0 — what already exists, which the rest of this section under-counted

Written after reading the code rather than the summaries, same as the rest of
this plan was supposed to be:

- **The async runner is real and complete.** See D4 below.
- **Verification step 1 is already built**, and this plan did not know it.
  `ue-project/Source/VoxelEarth/VoxelGpuMeshAsyncVerify.cpp` (~640 lines) gives
  `voxel.GPU.VerifyAsyncMesh`: it meshes K chunks through
  `FVoxelGpuMeshJobManager` and **byte-compares the packed quads against
  `MeshChunkBricks` + `PackVoxelChunkQuad`** — the shipping CPU mesher, not a
  transcription — checks the exactly-one-outcome contract by catching double
  delivery, and reports dispatch→ready p50/p95 and chunks/s. It also ships
  `voxel.GPU.VerifyAsyncMesh.Control`, which runs the *identical* requests
  through `RunRegionBlocking` and so splits "the region setup is wrong" from
  "the async runner is wrong" in one run.

  **Correction:** the "Verification, in this order" list below says to extend
  `voxel.GPU.SpawnPool`'s CPU round trip. ~~Do that.~~ Do not — this harness is
  purpose-built, strictly stronger (it compares chunk-local quads against the
  real mesher), and already has the control experiment. Build on it.

### D1 — make the pool GPU-writable (`VoxelGpuPoolComponent.cpp:117-147`)

- Add `EBufferUsageFlags::UnorderedAccess` to the quad and chunk-id buffers; create
  UAVs alongside the existing SRVs.
- Bring both into RDG via `ConvertToExternalBuffer` / `RegisterExternalBuffer` — the
  pattern `docs/gpu-g2-draw-path.md:89-93` prescribes and the shipped code did not
  follow.
- **Buffer identity must stay stable.** The factory bakes these SRVs into a uniform
  buffer built once in `InitRHI`. In-place UAV writes are fine; reallocation is not.
**Scoped 2026-07-26, not built. And one thing I reported earlier was wrong.**

~~`BufferQuads = InQuads.Num()` means the GPU buffer is sized to the CPU
shadow's current length, so a GPU-written range only exists if the shadow
already covers it.~~ **Wrong — retracted.** `PooledQuads.SetNumZeroed(CapacityQuads)`
(`:852`) sizes the shadow to the **full pool capacity**, and `CreateSceneProxy`
says so explicitly (`:1248-1250`): *"The whole capacity is uploaded, not just the
used prefix: incremental writes address absolute pool offsets, so the buffer has
to be that big from the start."* So `InQuads.Num()` **is** capacity and the range
always exists. Recorded rather than quietly fixed, because it was reported
upward as a finding.

**The real clobber, which is exactly what the brief said.** Steady state is fine:
updates go through `UpdateQuadRange_RenderThread` (`:1088`), which writes only the
dirty slice. The danger is **proxy recreation** — four `MarkRenderStateDirty`
sites (`:993`, `:1022`, `:1032`, `:1228`) — after which `CreateSceneProxy`
re-uploads all of `PooledQuads`. For a GPU-written range the CPU shadow holds
zeros or stale content, so the geometry silently reverts.

**Three candidate fixes, in increasing order of correctness:**
1. *Mark GPU-owned ranges and skip them on rebuild.* Cheapest, but the rebuild is
   the only thing that populates the buffer, so a skipped range is left
   uninitialised — trades a revert for a garbage read.
2. *Re-dispatch GPU chunks on recreation.* Correct, and recreation is rare, but it
   couples the pool to the mesher and makes an unrelated event cost a GPU burst.
3. **Move quad-buffer ownership off the proxy** so recreation rebinds rather than
   re-uploads. This is what "buffer identity must stay stable" is really asking
   for, and it removes the clobber structurally rather than defending against it.
   It is also the shape a compute compaction pass wants. Interacts with the
   vertex factory baking the SRVs into a uniform buffer in `InitRHI`, so it needs
   care around Wave A's work.

**Recommendation: (3), and cost it before building it** — the same order that
just paid off for D6.

#### Option 3 costed, 2026-07-26, against Wave A's merged code

**The lifetime today is coherent, which is why the current design works.** All of
it is proxy-owned and dies together:

| thing | where |
|---|---|
| `FVoxelQuadVertexFactory VertexFactory` | proxy **member** (`VoxelGpuPoolComponent.cpp:503`) |
| `QuadBuffer` / `ChunkIdSRV` / `OriginSRV` / `ParamsSRV` | created in `CreateRenderThreadResources` (`:176-182`, `:827`) |
| handed to the factory | `SetQuadBufferSRV` / `SetPoolBuffers` (`:208-209`), then `InitResource` (`:210`) |
| baked into a uniform buffer | `InitRHI` (`VoxelQuadVertexFactory.cpp:95-96`), `UniformBuffer_MultiFrame`, built **once** |
| torn down | `~FVoxelGpuPoolSceneProxy` → `VertexFactory.ReleaseResource()` (`:153`) → `ReleaseRHI` releases the uniform buffer **and** the SRVs (`:102-104`) |

**The good news, and it is the thing worth costing for: the uniform buffer does
NOT have to survive.** It is built *from* the SRVs and holds nothing else of
substance. If the **buffers and SRVs** outlive the proxy, a new proxy's
`InitRHI` simply builds a fresh uniform buffer pointing at the *same* SRVs — and
the GPU-resident quads survive because the buffer was never destroyed or
re-uploaded. So "buffer identity must stay stable" is satisfied by moving one
level of ownership, not by making the factory immortal.

That makes option 3 substantially smaller than it looks:

1. A component-owned render resource (`FVoxelGpuPoolBuffers`) holding
   `QuadBuffer` + `ChunkIdBuffer`, their SRVs, and — for D1 — their **UAVs**.
   Created once, released at component destruction via `BeginReleaseResource` +
   fence.
2. `CreateRenderThreadResources` stops *creating* those two buffers and just
   takes references, calling `SetQuadBufferSRV` / `SetPoolBuffers` exactly as it
   does now. **No vertex factory change. No shader change.**
3. `CreateSceneProxy` stops re-uploading `PooledQuads` — the buffer already holds
   the data. It uploads on **first** creation only. *This is the line that
   removes the clobber.*
4. `UpdateQuadRange_RenderThread` retargets to the component-side buffer.

The chunk table (origins / params / runs) is small, genuinely proxy-scoped, and
**stays where it is** — `MaxChunks` is derived per proxy and nothing about the
clobber involves it.

**Precedent that this is an accepted shape here:** `GetElementShaderBindings`
already fetches `GVoxelGIVolume.GetUniformBuffer()` (`VoxelQuadVertexFactory.cpp:190`),
a resource that outlives every proxy.

**Estimate:** one file plus a small header, ~200–300 lines touched, no `.ush`
edit, no digest exposure, no bench impact. **The one genuinely delicate part is
render-resource lifetime** — created on the render thread, released behind a
fence — which is standard but is exactly the kind of thing that fails as a
crash-on-exit rather than a compile error.

**It is also the right destination on the merits:** a component-owned buffer set
is precisely where a compute compaction pass's compacted-quad-id buffer wants to
live, so this is the enabling step for that too rather than a detour around it.

#### Built 2026-07-26 — and it closes ONE of THREE clobber paths, not all of them

The two "remaining unknowns" were written down and then actually checked. One
came back clean; **the other did not, and it matters more than the one that was
fixed.**

**Path 1 — proxy recreation. FIXED.** `CreateSceneProxy` → `CreateRenderThreadResources`
re-uploading the whole CPU shadow. Now uploads on first creation only; later
proxies rebind. Four triggers (`MarkRenderStateDirty` at `:993`, `:1022`,
`:1032`, `:1228`). Gated by `voxel.Stream.PoolClobberTest`.

**Path 2 — the incremental dirty span. NOT FIXED, and it is the frequent one.**
`DirtyQuads` is a **span, not a set**: `PushUpdatesToProxy` merges every dirty
region with `Min(First)` / `Max(Last)` (`:1060-1061`), and
`UpdateQuadRange_RenderThread` then writes that whole span **from `PooledQuads`**
(`:1088-1089`, writer at `:436-448`).

So two CPU-written chunks at distant pool offsets produce a dirty span covering
**everything between them** — and any GPU-written range in that gap is
overwritten with CPU-shadow content on the very next update. This is the
**steady-state** path, not a rare one: it runs whenever a chunk is added or
removed, which is continuously during streaming. It is *more* likely to bite than
proxy recreation, not less.

*This is the same shape of mistake as the original brief's option 1: a fix that
addresses the path you were looking at while the more frequent one stays open.
The persistent buffers are still a precondition for fixing it — they are just not
sufficient on their own.*

Candidate fixes, not yet costed: track GPU-owned ranges and split the dirty span
around them (turns one write into several, but the span is already a coarse
over-approximation); or keep the dirty set as an interval list rather than a
single span; or have the GPU writer also update the CPU shadow, which defeats the
purpose. **Cost before building — the same rule that has now paid off three
times in this wave.**

**Path 3 — buffer growth. HANDLED, but with a gap the caller has to close.** If a
later proxy needs more than the allocation, the buffers are rebuilt and
re-uploaded from the CPU shadow. That is *correct in itself* — the GPU-resident
contents genuinely are gone once the old buffer is destroyed, so there is nothing
else to restore from. **But nothing currently re-dispatches the GPU-written
chunks afterwards**, so their geometry silently reverts to whatever the shadow
holds. Stated plainly because it is the branch a reader will most want to see:
growth **invalidates every GPU-written range**, and D4 must treat a capacity
rebuild as a re-mesh trigger. The capacity is allocated up front from
`kPoolCapacityQuads`, so this should be rare — but "rare and silent" is the
combination this wave keeps being bitten by.

**Status: D1 is not done.** The persistent-buffer work is correct and is the
**necessary foundation, not a sufficient fix**. Path 2 must close before any GPU
writer can be trusted, and path 3 is a D4 correctness requirement (below).

**The lesson is not "the pool has three clobber paths."** It is that **a fix
aimed at the failure you happened to notice is not a fix.** Option 1 in the
original brief, and my own path-1 work, are the same shape: both close the door
you were looking at while the busier one stays open. The only reason path 2 was
found is that it was written down as an unknown and then actually checked.

#### Path 2 costed, 2026-07-26 — and the structural option LOSES this time

Three candidates, costed rather than pre-judged. **The expectation going in was
that a structural fix would beat a defensive one, as it did twice earlier in this
wave. It does not here**, and the reason is worth keeping.

**A — make the dirty region an interval list instead of one span. RECOMMENDED.**

The decisive observation: **the span is already an over-approximation of data the
CPU actually wrote.** Every dirty region is generated by a CPU write to one
chunk's range (`AddChunk` `:913`, `UpdateChunk` `:1137`/`:1177`, `RemoveChunk`'s
repoint). The `Min(First)`/`Max(Last)` merge at `:1060-1061` is what throws that
precision away. Restoring it is *not adding information* — it is **stopping the
discarding of information already present**.

That makes it exactly correct for any CPU/GPU mix, and — the part that matters —
**it needs no GPU-range tracking at all.** An interval only ever covers quads the
CPU wrote, so a GPU-written range is untouchable by construction rather than by
exclusion.

- Touches ~5 sites (`:920`, `:1054-1061`, `:1072`, `:1082`, `:1088-1089`, `:1141`)
  plus `UpdateQuadRange_RenderThread` taking N slices instead of 1.
- The single span exists to make the upload ONE lock, so N slices is the real
  cost. Bounded, though: chunks applied per frame are already throttled by the
  streaming apply budget, so N is small per update. Merge intervals within a gap
  threshold to cap it.
- **Likely a win on the shipped CPU path today, independent of Wave D.** The span
  currently uploads every quad between the lowest and highest dirty chunk,
  including all the untouched geometry in the gaps; with chunks scattered across
  a 14 M-quad pool that can be most of the buffer per update. The comment at
  `:1051-1061` records that this code already caused one streaming collapse by
  copying too much. **This should be measured on its own merits before Wave D
  needs it.**

**B — split the span around known GPU-owned ranges.** Requires tracking GPU
ranges, and produces up to (GPU ranges + 1) slices — so it degrades to *more*
slices than A precisely as GPU meshing becomes the common case. Strictly worse
than A. Rejected.

**C — give GPU-written chunks their own buffer or pool region.** The structural
option: a CPU dirty span could never span a GPU range because they would not
share an address space.

Two buffers is the expensive form — two SRVs, and the draw stops being one
contiguous range, which ADR-0006 forbids and which forecloses the static-relevance
path. The cheap form is **one buffer, two allocator regions** (CPU allocates from
one end, GPU from the other): one SRV, one draw range, no draw-path change at all.

**Rejected anyway, for a reason the codebase already documents.** Two regions
cannot share headroom, so the split has to be sized in advance — and
`VoxelGpuPoolComponent.h` already flags `GetLargestFreeRun`/`GetFreeRunCount` as
the early warning that *allocations start failing on a pool that still looks half
empty*. A fixed partition manufactures exactly that condition, and it would be
wrong at a different ratio for terrain and for water. It trades a fixable
correctness bug for a capacity-tuning problem that fails in the mode this pool is
already fragile in.

**So: A.** Smaller than the structural fix, exactly correct, no GPU-range
bookkeeping, and probably a performance win in its own right. Cost before
building still applies to the interval-merge threshold — do not tune it blind.

- **Neutralise the CPU shadow clobber — the highest-severity silent-corruption risk
  in this wave.** `PooledQuads`/`QuadChunkIds` (`VoxelGpuPoolComponent.h:148-152`)
  are a full CPU mirror, and `CreateSceneProxy` re-uploads all of it. Any
  `MarkRenderStateDirty` path would overwrite GPU-written quads with stale CPU
  content — presenting as **terrain reverting to older geometry after an unrelated
  event**. Mark GPU-owned ranges and skip them on rebuild, or drive rebuilds from a
  re-dispatch.

### D2 — gated `MeshEmit` variant emitting pool-ready quads — **LANDED 2026-07-26 (PR #126)**

**What shipped.** `VXC_MESH_CHUNK_LOCAL`, a permutation on `FVoxelMeshEmitCS`
and the only permutation domain in `VoxelGpuWorldGen.cpp`. Under it the kernel
bakes each interior brick's chunk-local origin into `slice/u0/v0` as it packs,
and offsets its writes by a new `QuadWriteBase` uniform. The CPU-side
`RebaseQuadsToChunkLocal` no longer runs on the default path.

The `% kBricksPerChunkEdge` in `maskChunkLocalOrigin` is what makes the
**batched** multi-chunk region D4 wants work later: interior brick 4 on an axis
is brick 0 of the *next* chunk, so its chunk-local origin is 0 again — not 32,
which would not even fit the 8-bit field.

**Verification — every claim, and how it was checked. No editor run was made;
the box was queued to other waves.**

| claim | evidence |
|---|---|
| the bench leg still runs the program its digest was recorded from | all seven kernels respun with `compile-shaders.ps1`'s exact flags; **SHA-256 byte-identical** to `voxel-core/shaders/prebuilt/*.spv` |
| the permutation is not a no-op | `MeshEmitMain` DXIL at `cs_6_6` with the define 0 vs 1 — hashes **differ** |
| both permutations compile, including the shared `greedyMask` | `MeshEmitMain` ×2 and `MeshCountMain` at `=1`, all exit 0 |
| it cannot leak into the SPIR-V leg | `=1` without `VXC_UE` fails with the intended `#error` |
| shader lint | `lint-shader-ub.py` clean, incl. the new `UNSIGNED_UNDERFLOW` annotation |
| it links | `VoxelEarthEditor Win64 Development` Succeeded, `voxelcore.lib` rebuilt first so the staleness warning could not fire |

**MEASURED 2026-07-26, AMD Radeon RX 7800 XT. D2 and D3 are both GREEN.**

Four legs, `voxel.GPU.VerifyAsyncMesh 64 8`, 64 chunks × 8 in flight, seed
20260719, two separate processes:

| leg | emit path | result |
|---|---|---|
| 1a | GPU chunk-local (`MeshChunkLocal 1`) | 64/64, gpu 88,860 quads = cpu 88,860, **BYTE-IDENTICAL** |
| 1b | GPU chunk-local | 64/64, 88,860 = 88,860, **BYTE-IDENTICAL** |
| 2a | GPU brick-local + CPU rebase (`MeshChunkLocal 0`, the control) | 64/64, 88,860 = 88,860, **BYTE-IDENTICAL** |
| 2b | GPU brick-local + CPU rebase | 64/64, 88,860 = 88,860, **BYTE-IDENTICAL** |

Both permutations produce the **same 88,860 quads** and both are byte-identical
to `MeshChunkBricks` + `PackVoxelChunkQuad`. Zero job failures, zero double
deliveries, 64/64 delivered on every leg — the exactly-one-outcome contract held.

**The determinism gate did not move, which is the whole point of D2 being a
permutation:**

```
CPU reference digest: 6e893ab3679a8c81 (matches the pinned value; vxc::kWorldGenVersion = 8)
PASS: Unreal-compiled GPU output is bit-exact with the CPU reference (13.8 ms total)
```

**D3's quad-total guard, first firing in anger — PASS, both fixture regions:**

```
[D3 quad-total cross-check] PASS — QuadTotalMain 3424 == CPU-derived 3424 (Offsets[6911] + Counts[6911], 6912 masks)
[D3 quad-total cross-check] PASS — QuadTotalMain 3244 == CPU-derived 3244 (Offsets[6911] + Counts[6911], 6912 masks)
```

**NO THROUGHPUT NUMBER IS QUOTED FROM THIS RUN, and none should be.** Three
reasons, all disqualifying on their own:
1. **Leg 1a is warmup-contaminated** — `dispatch->ready` max 921 ms against
   ~22 ms on every subsequent leg. That is shader compile and PSO warmup, and it
   dragged 1a's rate to 46.3 chunks/s against 127–136 on the three warm legs.
   Ground rule 1, exactly.
2. **The world was streaming the full cascade concurrently** in every leg
   (`dispatched=7137` in the first 5 s window). That is contention.
3. It is a synthetic harness at a fixed in-flight cap of 8, not the streaming
   path. Wave D's throughput claim has to come from per-ring dispatch rate under
   motion, which needs the D4 wiring first.

What the run *does* support: `dispatch->ready` p50 is **~21 ms** on all three
warm legs, and chunk-local vs brick-local are indistinguishable (132.3 vs 127.3
/ 135.5 chunks/s) — i.e. **D2 costs nothing measurable**, which is expected, as
it is the same kernel plus an add.

**Two design points worth keeping:**
- The CPU rebase is **kept as the control**, not deleted. `voxel.GPU.MeshChunkLocal`
  0 restores it, and the harness logs which permutation a run used — a PASS that
  does not say which path passed is not evidence about either.
- The choice is **latched per job at `Submit`**, not read at delivery. A cvar
  flip between dispatch and readback would otherwise rebase quads the GPU had
  already rebased: silent, and looks exactly like a mesher bug.
- `ValidateRegionRequest` **rejects** a non-zero `QuadWriteBase` without the
  permutation. The off permutation does not read that uniform, so it would be
  silently ignored and every quad would land at offset 0 — the same shape as the
  unbound-`FShaderParameter` and dead-`GPUCullMergeGap` failures already on this
  project's record.

### D2 — the original brief, for the record

`voxel-core/shaders/worldgen.ush` emit block, `VoxelGpuWorldGen.cpp:455-468`.
Today's quads are brick-local (`slice/u0/v0` are 0..7 inside one 8³ brick). Bake in
(a) the brick→chunk offset, giving chunk-local 0..31 exactly as `MeshChunkBricks`
produces; and (b) a **pool base offset** so the shader writes straight into the
chunk's allocated range — it already writes at `InQuadOffsets[maskIndex] + local`,
so this is an added uniform, not a restructure.

**Must be a gated permutation, not an edit in place:** the digest gate hashes quad
fields, so changing emitted bytes changes the digest even though quads are
display-only.

### D3 — allocation without a big readback — **RESEQUENCED, and LANDED 2026-07-26 (PR #126)**

**What shipped.** `ue-project/Shaders/VoxelQuadScan.usf` / `QuadTotalMain` (one
thread, one add), added to the graph **unconditionally** so `RunRegionBlocking`
cross-checks the GPU total against the CPU derivation on every
`voxel.GPU.VerifyRegion` run — a kernel only the streaming path exercises is a
kernel whose first bug appears in the streaming path.

`FVoxelGpuMeshJobManager` is now three-phase: `Dispatched` (4-byte total
pending) → `TotalDone` → `QuadsDispatched` → `ReadbackDone`. `Voxel.Quads` is
`ConvertToExternalBuffer`'d in phase 1 because it must outlive its graph;
`NumQuads == 0` skips phase 2 and delivers a frame early, which at level 0 is
common rather than an edge case; and `Counts`/`Offsets` are still read only on
the brick-local control path, so that control stays an honest reproduction of
the old behaviour.

**The cost, stated so it gets measured rather than assumed:** two round trips
instead of one — latency-to-delivery roughly doubles while bandwidth drops
~70–100×. Right trade for a path whose in-flight cap already absorbs latency,
but a trade. If latency binds before bandwidth does, the next lever is a
single-phase speculative read of a bounded prefix (~4,096 quads covers a typical
chunk in one trip). Deliberately not built on spec, since D1 deletes this half.

**`DispatchToReadyMs` changed meaning** — it is now "the total landed", not "the
whole readback landed". It will drop for reasons unrelated to GPU speed. Do not
compare it across this change; `SubmitToDeliverMs` is the honest end-to-end one.

**Still owed:** the same box time D2 owes. Nothing measured.

### D3 — the original brief, for the record

**Sequencing correction, 2026-07-26. This plan had the dependency backwards.**
D3 was written as a refinement to apply after D4 had wired GPU meshing into
`DispatchJobs`. It is not a refinement; it is the thing that makes D4 worth
doing, and the arithmetic is not close:

> `MaskCount` is 3,072 for a one-chunk region, so `MaxQuads` = 3,072 × 32 =
> **98,304**, and the quad readback is **786 KB per chunk** — against a typical
> ~900–1,500 live quads, i.e. **~7–12 KB**. That is a **65–100× over-read on
> every single chunk**. At `MaxInFlight` 8 it is 6.3 MB of readback per batch,
> and filling R0 once at 128 m (~8,100 chunks) is **~6.4 GB of PCIe traffic**
> against a kernel time `gpu-g0-sizing.md` puts at **~0.09 s**.

So on the readback path it is the *readback*, not the kernel, that sets
throughput, and a D4 measured with it in place would be measuring the wrong
thing. **Land D3 first**, then wire `Submit` into `DispatchJobs`.

Upper-bound reservation does not work either (that same static ≈98k
quads/chunk bound, and the draw's vertex count comes from
`Pool.GetHighWaterMark()`, so over-allocation directly inflates the thing Wave A
just proved the renderer is bound by). **Read back only the 4-byte scan total
per chunk.** Latency, not bandwidth:

1. frame N: dispatch into a **persistent** scratch buffer (external, not an RDG
   transient — today's `Voxel.Quads` dies at `GraphBuilder.Execute()`), enqueue the
   count readback;
2. frame N+k: count lands → `Pool.Alloc(exact)` → GPU→GPU compaction copy into the
   allocated range;
3. CPU writes the chunk-table entry as it does today.

**The 4-byte total is not free — it is a new kernel.** Nothing on the GPU holds
it today: `RunRegionBlocking` and the manager both derive it CPU-side as
`Offsets[MaskCount-1] + Counts[MaskCount-1]`, which is *why* the whole 12 KB
`Counts` and `Offsets` arrays come back. Producing a 1-element total needs a
pass that writes it. Put that pass in a **new `.usf` under
`ue-project/Shaders/`, not in `worldgen.ush`** (`VoxelQuadDecodeTest.usf` is the
precedent): it keeps the total out of the determinism digest's blast radius,
needs no SPIR-V respin, and stays clear of ground rule 10's `.ush` hazard.

The same file is the natural home for the **GPU→GPU compaction copy** step 2
needs, so writing it now is not throwaway work — it is what D1 will plug the
pool range into.

### D4 — wire the async runner into dispatch

**Most of this already exists and is unwired.** `FVoxelGpuMeshJobManager`
(`VoxelEarthShaders/{Public,Private}/VoxelGpuMeshJobManager.*`) is an async
per-request state machine with `Submit`/`Tick`/`CancelAll`, it calls
`GraphBuilder.Execute()` **without** `SubmitAndBlockUntilGPUIdle`
(`.cpp:380-383`), and its header already states the exactly-one-outcome invariant
(`.h:22-29`) that mirrors the CPU path's. Its own scope note says: *"Nothing here
is wired into DispatchJobs or the streaming path, and the brick-local → chunk-local
rebase still happens on the CPU after readback."*

~~So D4 is: delete the three `AddEnqueueCopyPass` calls at
`VoxelGpuMeshJobManager.cpp:370-375` and the poll/`Lock`/rebase tail, point the
emit at the pool UAV from D1/D2, and wire `Submit` into `DispatchJobs`
(`VoxelWorldSubsystem.cpp:5807`) behind a cvar, **level 0, non-edited chunks only**.~~

Do **not** build on `RunRegionBlocking` — it captures its outputs **by reference**
into the render command (`VoxelGpuWorldGen.cpp:493-502`), which is safe only
because of the flush at `:544`/`:587`. *(That part was right and stands.)*

**CORRECTED 2026-07-26, from reading the file. The runner is as small as claimed;
the work item is not.** Struck rather than deleted, because the estimate is the
obvious one and the next person will make it too. Three ways it is wrong:

**(a) "Delete the three `AddEnqueueCopyPass` calls" contradicts D3 above.** D3
requires a 4-byte scan-total readback, and that total does not exist as a buffer
— it is derived CPU-side from the full `Counts`/`Offsets` arrays. So D4 deletes
**one** copy (the 786 KB `Quads` one), replaces the other two with a smaller one,
and **adds a kernel** to produce it. A net addition, not a deletion.

**(b) "Delete the poll/`Lock` tail" would delete the only completion event —
i.e. it breaks this section's own hard invariant, two paragraphs below where it
is stated.** The render-thread poll is the *only* thing that knows the GPU
finished. Delete it and there is nothing to hang `LevelJobsInFlight[]--`,
`FootprintBandCache.Add`, `FootprintBlindJobInFlight.Remove` and
`Rec->bJobInFlight` on — which is exactly the dropped-result / stranded-(X,Y)-column
bug this plan warns about and that has already been diagnosed and fixed once
here. With no readback at all, "exactly one result" would have to degrade to
fire-and-forget at dispatch. **The poll stays**; what shrinks is what gets
`Lock()`ed, from ~810 KB to 4 bytes. Only the **rebase** tail is genuinely
deletable — and that is D2, landed 2026-07-26.

**(c) The `DispatchJobs` wiring is the bulk of D4 and this estimate did not cost
it at all.** Everything `voxel.GPU.VerifyAsyncMesh` gets to skip by being
synthetic is real work there: the raster window from real tiles instead of
`vxc::SyntheticTileSampler`; the two counters decrementing on different threads
in different places; `GenerationId` snapshot/compare, whose stale rate gets
*worse* with multi-frame latency; and the `NeedsOverlayAwarePath` carve-out.

And one this plan states under "What stays on the CPU" but never carries into
the value case:

> **GPU meshing does not remove the CPU job. It removes the meshing inside it.**

`FootprintBandCache` is reduced via `ColumnDeepestAirVoxel`, which needs
`Col.cave.segs[]`, `shaftMarginSq`, `cavern` and `bedrockDepthMm` — fields
`FVoxelGpuColumnSample` does not carry. The band feeds two admission/dispatch
skips *and* the cold-band throttle, which deadlocks a whole (X,Y) column if
bands stop arriving. So a GPU-meshed chunk **still needs a CPU column pass**,
and the wave's ceiling is the ~55% of level-0 job time that is meshing. That
materially tempers the value case for the whole wave and should be read next to
the ~0.09 s figure, not instead of it.

#### …but the ceiling is NOT structural. Corrected 2026-07-26, same session.

~~until the GPU column struct grows~~ — that phrasing (mine, one commit
earlier) implies the fix is to widen `GpuColumnSample` and read it back. **It is
not, and doing that would re-create the exact problem D3 just removed:**

| | bytes |
|---|---|
| `vxc::ColumnSample` = 5 scalars + `CaveColumn` (108) + `CavernColumn` (56) | **184** |
| `FVoxelGpuColumnSample` — the 5 scalars only | **20** (11% of it) |
| widened struct, read back over a 34×34 band grid (1,156 columns) | **213 KB/chunk** |

**The cave and cavern data already exists on the GPU.** `worldgen.ush` mirrors
`caveColumnFor` and `cavernColumnFor` **bit-for-bit** (`:812-814`, `:1170-1173`)
and `VoxelizeMain` computes `GpuCaveColumn cave` / `GpuCavernColumn cavern` per
column at `:1325`/`:1334` — then discards them. That was a deliberate choice
recorded in `amplifier.h:36-41`: *"the GPU mirror recomputes it inside
VoxelizeMain rather than widening GpuColumnSample"*.

And `ColumnDeepestAirVoxel` (`VoxelWorldSubsystem.cpp:1305`) is a **pure
per-column reduction** — one `vxc::ColumnSample` in, one `int64` out. So the
right move is **a band-reduction kernel, not a wider struct**: run the reduction
where the cave/cavern data already is, and read back **~4 bytes per column
(~4.6 KB per chunk)** instead of 213 KB. No new worldgen mirroring is needed —
the hard part is done and is already gated by the determinism digest.

**So "GPU meshing removes the meshing, not the job" is true of D1–D4 as scoped,
and is a missing kernel rather than a permanent ceiling.** Sequence it after
D1–D4 are measured, like D5: it is the change that would let a level-0 job stop
touching the CPU column path at all. Do not let the caveat above be read as an
argument that the wave cannot go further than ~55%.

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

**Decided 2026-07-26: "in flight" means COMPLETED** — `LevelJobsInFlight[]` keeps
decrementing in `DrainResults` on delivery of the GPU result, exactly as the CPU
path does today. Reason: the throttle exists to bound *outstanding* work, and
"dispatched" would let a ring re-admit the same columns repeatedly before the
first result lands. The cost is that the in-flight cap now has to absorb GPU
latency as well as work, so the slot floor wants sizing against latency × rate
rather than against worker count — **and one knob at a time.** The recorded
catastrophe is what happens otherwise: floors `{0,2,3,4,4}` reserved 13 of 24
slots and collapsed throughput from 49,179 chunks to 558.

### D4 wiring design — written from the code, 2026-07-26

Read-only survey of `VoxelWorldSubsystem.cpp` done while waiting for box time.
Line numbers are today's.

**The finding that shapes everything: the 34×34 column grid is built on the
WORKER thread, inside the job lambda — not in `DispatchJobs`.** Storage is
`static thread_local vxc::ColumnSample ScratchColumns[1156]` (`:6212-6251`),
built at `:6261-6268`, and `DispatchJobs` touches no columns anywhere. The
`static thread_local` idiom settles it; the comment at `:6227` names the cost as
"24 workers × 208 KB".

**Consequence: there is no CPU column pass for a GPU job to reuse.** If
`DispatchJobs` submits to the GPU instead of launching a task, no worker runs,
so no columns are evaluated and **no band is produced**. That is not a
performance detail — the band feeds the cold-band throttle, and a (X,Y) column
that stops receiving bands is the stranded-column bug. So a GPU level-0 job must
do one of:

  - **(i) keep a worker task that builds columns and the band but does NOT
    mesh.** This is exactly "GPU meshing removes the meshing, not the job", and
    it is the D4 scope. `Result.GridMs`/`GridCycles` already exist (`:1238`) so
    the split is already measurable.
  - **(ii) the band-reduction kernel** (see the correction above). Removes the
    CPU pass entirely. After D1–D4, not during.

**The job has no struct — it is a lambda capture list** (`:6142-6143`), launched
via `UE::Tasks::Launch(TEXT("VoxelChunkMeshJob"), ..., BackgroundNormal)`. So
the GPU branch is a clean fork at `:6140`, after every gate has already passed.

**Counters, with the "completed" ruling applied.** The two are asymmetric today:

| counter | inc | dec | thread |
|---|---|---|---|
| `JobsInFlightCounter` | `:6109` game | `:6535` **worker**, after `ResultsQueue.Enqueue` | mixed |
| `LevelJobsInFlight[]` | `:6110` game | `:7188` game, in `DrainResults`, before the stale `continue` | game |

For a GPU job there is no worker, so `JobsInFlightCounter.Decrement()` moves to
**the manager's `OnJobComplete`, at the moment the result is pushed to
`ResultsQueue`** — the exact analogue of `:6535`. `LevelJobsInFlight[]` is
untouched: it already decrements in `DrainResults`, which is what "in flight
means completed" requires.

**Three things `DrainResults` does before the stale check, all of which a GPU
result must still trigger** (`:7101`, `:7113`, `:7188`): the band `Add`, the
`FootprintBlindJobInFlight.Remove`, and the per-ring decrement. A GPU result must
enter the same `ResultsQueue` as a CPU one and be indistinguishable to
`DrainResults`, or all three are lost.

**A wiring detail the plan missed: `ApplyMeshResult` takes
`TArray<FVoxelChunkQuad>` and packs internally** at `:6835`
(`Packed[I] = PackVoxelChunkQuad(Quads[I])`). The GPU delivers **already-packed
`TArray<uint64>`**. D4 needs an overload that takes pre-packed quads and skips
the pack loop, or it will unpack GPU quads solely to repack them.

**Tiles are a non-issue, verified.** `VoxelGpuRegionBuild::FillRasterWindow` is
templated and needs only `pixelSizeMm()` / `elevationMm()` / `climate()` — i.e.
exactly `vxc::ITileSampler`. The production path passes `*Impl->Tiles` instead of
a `SyntheticTileSampler`; the only change is widening the two existing callers'
parameter from the concrete synthetic type to `vxc::ITileSampler&`.
`TileGridSampler` loads every `.vxtl` eagerly with a deterministic flat-sea
fallback on a miss — but the CPU path reads the *same* sampler through
`Amp.column`, so this is **not a GPU-specific risk**.

**REQUIREMENT D4-R1 — a pool capacity rebuild must trigger a re-mesh. Owner: D4.**

Not a risk to watch, a correctness obligation on the wiring. When the pool
outgrows its buffer allocation, `CreateRenderThreadResources` rebuilds the
buffers and re-uploads from the CPU shadow. That is correct in itself — the old
buffer's contents genuinely are gone, so there is nothing else to restore from —
but it means **a capacity rebuild invalidates every GPU-written range**, and
nothing currently re-dispatches them. Their geometry silently reverts to whatever
the shadow holds.

D4 must treat a capacity rebuild as a re-mesh trigger for every GPU-meshed
resident chunk: bump their `GenerationId` (or clear `bMeshSettled`) so
`RecomputeDesiredSet` re-queues them. **It is deliberately stated as a
requirement rather than filed under risks because it is *rare and silent*, which
is the combination this wave has repeatedly been caught by and the one least
likely to be found by testing** — capacity is allocated up front from
`kPoolCapacityQuads`, so a test would have to deliberately undersize the pool to
reach it.

**Two asymmetries to measure rather than assume:**
- The CPU path has a per-brick `SkipBrick` lambda (`:6357-6409`) driven by the
  same band scratch arrays, so it skips provably-all-air/all-solid bricks. **The
  GPU mesher has no equivalent and meshes all 64.** That compounds with the 3.4×
  halo waste below.
- `EditEpochSnapshot` (`:6143`) and `GenerationId` (`:7191`) both widen as a
  failure window when a job spans frames. Watch `StaleResultsDiscarded`.

**Batch across chunks.** A region dispatch is a brick-aligned slab with a 1-brick
halo, so meshing one 32³ chunk alone dispatches 48³ voxels — 3.4× waste. Batch a
footprint's column or a tile of neighbours and slice the quad stream by
`meshBrickIndex`. Cap: `MaskCount ≤ 65,536` (single-workgroup scan) ⇒ ≤1,365
interior bricks per dispatch.

### D6 — move the footprint band to the GPU — **costed and APPROVED 2026-07-26, not yet built**

Costed *before* wiring, deliberately, so the answer could shape the build. It
did — the brief's stated risk was wrong, and the design that survives is
smaller than the one that was asked for.

#### The brief's risk was wrong, and this is why

The question was *"can admission tolerate a band that arrives a few frames
late?"*, on the assumption that a late band would stall admission. **It does not,
and the system already depends on it not doing so.**

The cold-band throttle (`VoxelWorldSubsystem.cpp:5930-5951`) defers a chunk only
when there is **no band AND** a live `FootprintBlindJobInFlight` mark younger
than `kBlindJobMarkTimeoutSeconds`. That mark is added (`:6549`) only *after* a
launch and only for a footprint with no band yet. So **the first job for every
footprint already runs blind**, produces the band, and later chunks in that (X,Y)
column benefit from it.

The band is therefore an **optimisation** — skip provably-empty chunks — **not an
admission gate**, and lateness is the designed-for case, not a hazard.

**The real risk is not *late*, it is *never*.** If bands stop arriving at all the
column throttles; the mark timeout is the existing backstop that stops even that
being permanent. Recorded because the wrong version of this ("cannot tolerate a
late band") would have killed a change that is actually cheap.

#### Design, as approved

1. **The band rides D3's phase-1 readback. 4 bytes → 12** (`QuadTotal`,
   `MaxSurfaceTopVoxel`, `SolidBelowVoxel`), in the **same** result object.
   `DrainResults` is untouched and keeps doing `FootprintBandCache.Add`
   (`:7101`), `FootprintBlindJobInFlight.Remove` (`:7113`) and
   `--LevelJobsInFlight` (`:7188`) in one pass on one result.

   **This is the load-bearing choice.** A separate band readback would create two
   independent async streams that must *both* satisfy exactly-one-outcome, with a
   job able to deliver quads but no band. Doubling that invariant surface is the
   class of change that produced the stranded-column bug in the first place. Zero
   new invariant surface is worth far more than the bytes.

2. **The band is z-independent.** `ColumnSurfaceTopVoxel` (`:1282`) and
   `ColumnDeepestAirVoxel` (`:1305`) are pure functions of one
   `vxc::ColumnSample` — no chunk-z anywhere. So the band needs `ColumnMain`
   plus cave/cavern, and **does not need `VoxelizeMain` or its cell grid at
   all**. Much smaller than the brief assumed.

3. **No `worldgen.ush` edit, and no atomics.** A **new `.usf` that `#include`s
   `worldgen.ush`** and adds a `BandReduceMain` entry point — the same shim shape
   `VoxelWorldGen.usf` already uses. That gives it `caveColumnFor` /
   `cavernColumnFor` for free while leaving `worldgen.ush` byte-identical: no
   SPIR-V respin, no new bench descriptor bindings, nothing added to the
   determinism digest's blast radius, and clear of ground rule 10.

   **The atomic min/max question dissolves rather than needing a verdict.** One
   workgroup of 256 striding over the 34×34 = 1,156 columns, local min/max, then
   a fixed-order groupshared tree reduction — exactly the pattern `ScanSumsMain`
   already uses and that the gate already covers. Deterministic by construction,
   with no order-independence argument to defend. *(The atomic route was going to
   be verified against the gate rather than assumed, on the grounds that this file
   has already produced one vendor divergence from an operation that looked
   order-independent on paper. Not needing the argument at all is better.)*

   Cost of this route: cave/cavern are recomputed for 1,156 columns rather than
   read out of `VoxelizeMain`'s registers, so ~50% extra cave work against
   `VoxelizeMain`'s 2,304 columns. Accepted, because it buys deleting the CPU
   column pass entirely and keeps `VoxelizeMain` out of a change it does not need
   to be in.

4. **The integer sqrt is not a determinism risk.** `ColumnDeepestAirVoxel` needs
   `CeilSqrtI64` (`:1266`), which seeds with `FMath::Sqrt(double(V))` and then
   corrects with `while (R*R < V) ++R; while (R > 0 && (R-1)*(R-1) >= V) --R;`.
   `worldgen.ush` has **no sqrt of any kind** and is integer-only by contract and
   by CI's float ban.

   That is fine, because **ceil-sqrt has a unique integer answer** — the `R` with
   `(R-1)² < V ≤ R²`. A pure-integer binary-search isqrt in HLSL and the
   FP-seeded CPU version converge on the same `R` **by construction**. The fix
   removes floating point from the comparison rather than trying to make two FP
   paths agree, which is the correct shape given this file's history with vendor
   divergence (`floorDiv`'s `OpSRem`, the M0 AMD-vs-NVIDIA failure).

#### What it is worth

It removes the CPU column pass from a GPU-meshed level-0 job, i.e. the
**~45%** of job time that is not meshing — the other half of the ~55% D1–D4
address. Together they are what turns *"GPU meshing removes the meshing, not the
job"* into removing the job.

**Sequencing: after D1–D4 are landed and measured**, alongside D5, for the same
reason D5 waits — debugging shared plumbing against a novel path doubles the
search space for any failure.

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
   compare packed quads byte for byte. ~~Extend `voxel.GPU.SpawnPool`'s existing CPU
   round trip into an A/B assert rather than inventing a harness.~~
   **Corrected — use `voxel.GPU.VerifyAsyncMesh`, which already does exactly
   this and more; see D0.** D2 additionally uses it as its own A/B: run it at
   `voxel.GPU.MeshChunkLocal 1` and `0` and both producers are compared against
   the shipping CPU mesher, in one session on one binary.
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

  > **CORRECTED 2026-07-26 — the sentence in bold is the wrong way round, and it
  > is the sentence the whole item was resting on.** Kept above rather than
  > overwritten, the way `backlog.md` §6a keeps its superseded diagnosis visible.
  > Full write-up below under "E1 — what was actually found".
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

### E1 — what was actually found (2026-07-26)

**Nothing shipped, and `M_VoxelTerrain.uasset` is unmodified on this branch.**
What landed instead is the reason the planned implementation would have broken
the default renderer.

#### The premise is inverted

A material that asks for texture coordinate 1 does **not** get zero on the
component path. It gets **texture coordinate 0**.

1. The component path is `FLocalVertexFactory` fed by
   `FStaticMeshVertexBuffers::InitFromDynamicVertex(&VertexFactory, Vertices)`
   (`ue-project/Source/VoxelEarth/VoxelChunkComponent.cpp:634`). That overload's
   `NumTexCoords` parameter **defaults to 1**
   (`Engine/Source/Runtime/Engine/Public/StaticMeshResources.h:347`), and
   `BuildChunkVertexData` writes only `Vert.TextureCoordinate[0]` (`:437`).
2. `LocalVertexFactory.ush` **clamps** the material's request to what the mesh
   has, and clamping *duplicates* rather than zeroes. Manual vertex fetch — the
   SM5+/D3D12 path — does it at `:729-730`:

   ```hlsl
   // Clamp coordinates to mesh's maximum as materials can request more than are available
   uint ClampedCoordinateIndex = min(CoordinateIndex, NumFetchTexCoords-1);
   ```

   and the non-manual-fetch branch does the same explicitly at `:737`
   (`Result.TexCoords[1] = NumFetchTexCoords > 1 ? Input.TexCoords0.zw : Result.TexCoords[0];`).
   Both branches agree, so the answer does not depend on which one compiles.
3. Those UVs are world-planar metres wrapped to a 32 m period (`WrapWorldToUV`,
   `VoxelChunkComponent.cpp:136-141`), so component-path `TexCoord1` is a
   **position-varying value in (−32, 32)** — not 0, not 1.

The renderer that genuinely delivers zero is the **pooled** one:
`VoxelQuadVertexFactory.ush`'s `GetMaterialPixelParameters` writes only
`Result.TexCoords[0]` (`:437-439`), and `MakeInitializedMaterialPixelParameters`
zero-fills the struct (`MaterialTemplate.ush:664-671`).

So identity-as-zero would not have blacked out the component path. It would have
multiplied the **default renderer's** BaseColor by ±32 of position-dependent
garbage — which is worse than black, because it reads as a shading bug rather
than an obviously broken build.

#### Measured, on both renderers

Reasoning about the graph is not admissible here (the parity doc says as much),
so this was put on screen. `ue-project/Tools/probe_texcoord1.py` edits
`M_VoxelTerrain` to add **`EmissiveColor = abs(TexCoord1) * 0.05`** and nothing
else. BaseColor is untouched, so the term is purely additive: if `TexCoord1` is
zero the frame is unchanged; if it is anything else the frame lights up.

Same anchor (default spawn), same `-VoxelScreenshotAfter=50`, same build; the
only variable is the material, and then the renderer.

Four runs: {shipped, probe} × {component, pooled}. Each run captures at +51 s and
+53 s, so **each run carries its own same-path noise floor** — the two shots are
2 s of streaming and TSR apart, with nothing else different.

| renderer | probe vs its own control | that run's own noise floor | verdict |
|---|---|---|---|
| **component** | **30.91%** (shot 0), **71.21%** (shot 1) | 3.58% / 3.63% | **8.5–20× the floor** |
| **pooled** | 3.68% (shot 0), 2.48% (shot 1) | 4.25% / 4.90% | **inside the floor** |

Same material, same anchor, same build, same settle. One renderer moves an order
of magnitude past its own floor; the other does not move at all.

The component-path probe frame is not subtle: the snowfield is repainted in red
and green sawtooth bands that reset on a 32 m grid — exactly `(|U|, |V|, 0)`
with `U`, `V` the world-planar UV, wrapping where `fmod` returns to zero. Red is
`|U|`, green is `|V|`, blue stays 0 because the probe appends a zero.

Images: `docs/images/e1-texcoord/{01-component-control, 02-component-probe,
03-pooled-probe, 04-pooled-control}.png`.

**A bonus control nobody designed.** The pooled frame is not 100% pooled — at the
capture the pool held 1,039 of 1,055 chunks (998,294 of 1,010,693 quads, 98.5%),
and the handful that were not pooled are drawn by `UVoxelChunkComponent` in the
same frame. Those are the *only* geometry in `03-pooled-probe.png` that lights
up: one small floating formation, glowing exactly as it does in
`02-component-probe.png`, against pooled terrain that does not. Both renderers,
one frame, one material, and the boundary is visible. (What routes those chunks
to components under `voxel.Stream.GPU 1` is not established here and is worth a
separate look; it is not this finding.)

**Settle state, stated because the rule requires it:** these captures are **not**
settled — `loaded≈2000` of ~10,000 chunks, `jobsInFlight≈20`, `pendingJobs≈1400`
at the capture, because three other agents' UE sessions were running on the box
and streaming fell to ~14 chunks/s against ~220 chunks/s uncontended. That is
acceptable *for this specific question and no other*: the signal is a per-pixel
shading term on whatever terrain is drawn, not a difference in which terrain is
drawn, and the control and probe runs tracked each other to within 1% chunk for
chunk at the capture frame (2006 vs 1987 loaded). It would not be acceptable for
a frame-time or a geometry-coverage claim.

#### The corrected encoding

Both renderers wrap UVs to a 32 m period with `fmod`, so **no texture coordinate
this project can produce ever leaves (−32, 32)**. That makes a sentinel range
free, and it is correct on both paths with one graph, no switch node, no
permutation and no component-path cost:

```
tinted   ->  store (tint * k) + 1000
identity ->  anything below ~100   (covers BOTH the component path's
                                    duplicated UV and the pooled path's zero)
```

The alternative — make the component path supply a real second UV set
(`InitFromDynamicVertex(..., /*NumTexCoords=*/2)` plus
`TextureCoordinate[1] = (0,0)`) — also works and restores identity-as-zero
honestly, but costs 8 bytes per vertex on the renderer that is currently the
default, for a debug-only feature. Prefer the sentinel.

#### What is left, and why this wave did not do it

Finishing E1 needs two things this wave does not own:

- `ue-project/Shaders/VoxelQuadVertexFactory.ush` — widen `TexCoords` to
  `float4` and set `Result.TexCoords[1]` (Wave A owns `VoxelQuadVertexFactory.*`).
- the pooled-path writer that packs the tint into `ChunkParams.w`
  (`VoxelGpuPoolComponent`, also Wave A).

And `docs/gpu-g4-parity-plan.md` correction 1 says in terms that regenerating the
binary asset the shipping renderer draws with is not on the critical path for
anything and should not be done "while we're here". Regenerating it for a feature
that cannot be exercised in this wave, under an encoding that would then have to
change again, is exactly that. The asset was edited only by the probe and
restored with `git checkout --`.

### E2 — attempted on an idle box; NO NUMBER, and the reason is the harness

**Result: `-VoxelPerfRun` cannot measure this scene at all.** Not "the effect is
inside the noise" — the instrument reads a constant.

The scene, the fixture and the gates all worked. On an exclusive box:

- `STATIC pose pinned at (42030, 21000, 96062) yaw=0.0 pitch=-25.0
  (-VoxelPerfStaticAt)` — the pin path verified end to end.
- `LogInit: Command Line:` checked per leg for the `voxel.Water.GPU` value, so an
  A/B silently running two identical configs was ruled out rather than assumed.
- Camera underground at the lake, `55972 candidate brick(s)` in the implicit
  pass.

And then every frame-time sample came back **exactly 400.00 ms**. The delta-clamp ground
rule above is the mechanism: the world delta is clamped at `MaxUndilatedFrameTime`,
this anchor never rises above 2.5 fps, so `p50` would have been 400.00 for all
six legs whatever `voxel.Water.GPU` was set to.

| attempt | cascade | frames sampled | at the 400 ms clamp | below it |
|---|---|---|---|---|
| full | 6 rings, 2 km | 159 | 159 | **0** |
| trimmed | 4 rings, 512 m *(rejected — see below)* | 51 | 51 | **0** |
| trimmed | 6 rings, 1 km | 33 | 33 | **0** |

Trimming the cascade was the right idea and did not work: the cost here is
**underground streaming and deep-column tracking**, not ring extent. At 1 km the
tracked-chunk count fell only 2726 → 2101 and the scene stayed under 2.5 fps.
Note also that the 4-entry override was **silently ignored** — `tracked` matched
the full-cascade run frame for frame — so `-VoxelRingInnerMeters`/`OuterMeters`
appear to require all six entries, and nothing in the log says so.

**What would actually work**, for whoever picks this up:

1. **A surface pour, not a cavern lake.** `voxel.Water.SpawnIn` puts real voxel
   water at a *surface* anchor, where the scene runs far above 2.5 fps and the
   terrain A/B already enjoys a 1.1% floor. This is the candidate
   `gpu-water-pool-design.md:158-163` already names, and the clamp is now a
   second, independent reason to prefer it. The trade is that a pour is CA water
   rather than the implicit lake, so let it settle to `activeBricks=0` first or
   the 10 Hz re-mesh becomes the variable.
2. Or measure with `stat unit` by hand at the cavern (manual checklist 4a), which
   reads the real frame time and is not clamped.

**Do not** re-run the cavern legs against this harness expecting a different
answer. The null it produces is manufactured.

The measurement needs the box to itself. During this session there were up to
**four concurrent UE instances** (Wave A, Wave B, Wave C and this one) plus
concurrent UBT builds; one editor launch blocked ~3 minutes on the UBT mutex, and
streaming throughput fell to ~14 chunks/s against ~220 chunks/s uncontended.
Interleaving the legs cancels *drift* in machine load but not *step changes* when
another agent's run starts or ends mid-sequence, and a contended frame-time
number is worse than no number because it looks like a result. That is the same
failure mode as the retracted G5 numbers.

**The fixture is built and ready**, and one piece of it was missing until now:

- `-VoxelPerfFlight=static` pins the pose the pawn *spawned* at, and
  `RestartPlayer` always spawns on a **surface** column — it grounds the pawn on
  the highest solid voxel with air above it, and `-VoxelCameraHigh` only moves it
  further up (it rejects values ≤ 0). So the fixture could not be aimed at the
  only voxel water this world has, which is underground; and the two fixtures
  that *do* go there (`-VoxelFloodTest`, `-VoxelCavernShot`) move the camera on
  their own timers, which static mode then overwrites on the next tick.
  **`-VoxelPerfStaticAt=X,Y,Z` (UU)** closes that, and takes UU rather than
  metres so the pose `-VoxelFloodTest` prints can be copied verbatim.
  **Status: compiles; its abort-on-malformed branch is confirmed working in a
  live run; the pin itself is NOT yet exercised end to end.** The abort fired for
  real on the first attempt — `FParse::Value`'s default terminator set includes
  `,`, so `-VoxelPerfStaticAt=42030,21000,96062` was read as `42030`, exactly the
  trap `ParseSpawnColumnUU` documents for `-VoxelSpawnAt`
  (`VoxelEarthGameMode.cpp:37-41`). Fixed with `bShouldStopOnSeparator=false`.
  Without the abort the fixture would have silently pinned at the surface spawn
  and produced a perfectly plausible "cavern lake" measurement of a mountaintop.
  Re-run the pin before trusting any leg.
- The scene: `-VoxelFloodTest=70` locates the flooded cavern at lake surface
  `(42000, 21000, 95848)` UU and reports its shore pose as
  `(42030, 21000, 96062) rot(pitch −25, yaw 0)` — reproduced this session, with
  `59970 candidate brick(s)` in the implicit pass and the lake filling most of
  the frame. Pinning there directly gives the **static** implicit lake with no CA
  activity and no carve, so the water geometry is identical between legs.
- Plan: `-VoxelPerfRun=150 -VoxelPerfFlight=static -VoxelPerfStaticAt=42030,21000,96062
  -VoxelPerfYaw=0 -VoxelPerfPitch=-25`, legs interleaved `0,1,0,1,0,1` on
  `voxel.Water.GPU`, three per config, noise floor taken as the worst
  within-config spread, and **no number published if the ranges overlap**.
  Scene identity checked per leg from the implicit candidate-brick count and the
  terrain `loaded=`/`quads=` at settle, not assumed.

**The constraint to record regardless of the number** (asked for while planning,
and already stated at length in `docs/gpu-water-pool-design.md:46-82`): one pooled
primitive means **one translucent sort key for all water in the world**. It is
harmless for the current material only because base colour and opacity are
constant and there is no refraction, so a stack of N surfaces transmits
`(1−0.55)^N` in any order. W5's fill-fraction shading, foam, caustics or
refraction would each break that, as would a second translucent material
intersecting the water volume. Do not add them and assume the pool still holds.

---

## UNOWNED WORK ON THE CRITICAL PATH — compute compaction

**Raised 2026-07-26 from Wave D, escalated to the owner. Recorded here because
it is a gap in the plan, not in any one wave.**

The dependency chain, end to end:

> **Wave F needs D4** → D4 delivers GPU-produced geometry **into the pool** →
> the pool is **defaulted off** (`voxel.Stream.GPU 0`), because Wave A measured
> it ~0.15 ms *worse* than the component path at a down-facing pose against a
> pre-registered criterion → closing that gap needs **compute compaction** →
> **nobody is building compaction.**

So D1–D6 are currently improving a renderer the project has chosen not to ship,
and Wave F's payoff is gated behind a step with no owner. That is a structural
problem with the plan rather than a technical one with any wave, which is why it
is recorded at this level.

**The evidence says it is closable, not that the pool is a dead end.** Wave A's
cost model — a ~2 ms fixed floor plus ~1.19 µs per 1000 quads drawn, holding to
1.1% across two poses 4× apart in frame time — prices the remaining deficit as
**over-draw**, not overhead: 1.62× straight down and 2.72× at the horizon where
the 64-range cap binds. Compaction is worth **~0.12 ms straight down and ~4.7 ms
at the horizon** under that model, which would not merely close the 0.15 ms gap
but likely put the pooled path clearly ahead.

**Wave D's D1 work is a precondition for compaction itself, not only for D4.** A
compute pass writing a compacted quad-id list needs exactly what D1 built: a
**component-owned** buffer set (so it survives proxy recreation), **GPU-writable**
(UAVs), and a dirty-tracking scheme that cannot overwrite GPU-authored ranges
(the interval list). Whoever picks compaction up should start from D1 rather than
alongside it. The one shader line that changes is
`QuadIndex` in `VoxelQuadVertexFactory.ush`, which Wave A notes is derived in
exactly one place.

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
