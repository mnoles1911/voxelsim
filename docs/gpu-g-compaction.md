# Wave G — GPU-driven compaction for the voxel pool

> **STATUS 2026-07-26: G0 measured. The wave's premise did not survive it, and
> nothing past G0 was built.**
>
> Camera-only compaction reaches **7.3%** of this pool's over-draw. The other
> 92.6% is in four shadow cascades, which the ADR's own scheduling precedent
> cannot serve because shadow frusta do not exist at
> `PreInitViews_RenderThread`. `S_Δ` = 6.34 / 3.42 against a pre-registered
> stop-threshold of 2.0, so the saving straight down is **0.036 ms** against a
> residual of 0.13–0.17 ms — short by a factor of four.
>
> **`voxel.Stream.GPU` cannot be flipped by this route.** Wave A's diagnosis that
> the residual is over-draw rather than overhead survives intact; what changed is
> whose over-draw. Details in §4b–§4e. Shadow-targeted compaction is costed in
> §4c and is **not authorised** — the owner holds that decision.

Written 2026-07-26 **from the 5.8 engine source and the current tree**, before any
code was changed, because `docs/gpu-waves-plan.md`'s ground rule "cost before
build" has caught four wrong premises in Wave D alone.

This document's first job is to answer one question the owner asked directly:
**is `docs/gpu-g2-draw-path.md`'s indirect-draw design still accurate against the
code, and what does it understate?**

Short answer: **every positive claim it makes is true and I verified all of them.
What it omits is where the compute pass goes, and that omission is most of the
wave.**

---

## 1. What the ADR gets right (verified, do not re-derive)

| claim | verified at |
|---|---|
| `NumPrimitives == 0` is the signal to use `IndirectArgsBuffer` | `MeshBatch.h:272`, and it is **enforced both ways** — see below |
| non-indexed indirect draws exist and are reachable | `MeshPassProcessor.cpp:1369-1385` → `RHICmdList.DrawPrimitiveIndirect` |
| D3D12 implements it | `D3D12Commands.cpp:1227` — `ExecuteIndirect` on `GetDrawIndirectCommandSignature()`, `MaxCommandCount = 1` |
| persistent buffers are `TRefCountPtr<FRDGPooledBuffer>` via `ConvertToExternalBuffer` | still the pattern; `RenderGraphUtils.h:1352` |

The args struct is `FRHIDrawIndirectParameters` (`RHI.h:561`):

```cpp
uint32 VertexCountPerInstance;   // = CompactedQuadCount * 6
uint32 InstanceCount;            // = 1
uint32 StartVertexLocation;      // = 0 forever (see §5)
uint32 StartInstanceLocation;    // = 0
```

**Stronger than "documented": it is `checkf`'d in both directions.**
`FMeshDrawCommand::SetDrawParametersAndFinalize` (`MeshPassProcessor.cpp:952-963`)
fails a check if `NumPrimitives > 0` while an args buffer is set, *and* fails a
check if `NumPrimitives == 0` while one is not. There is no silent-wrong-geometry
mode here, which is a relief on a renderer whose characteristic failure is
exactly that.

**One correction to the brief.** `SubmitDrawIndirectEnd` (`MeshPassProcessor.cpp:1408`)
does branch on `NumPrimitives == 0` as advertised — but it has **zero callers in
the entire Renderer module**. Only the declaration (`MeshPassProcessor.h:1468`)
and the definition exist. The function that actually submits a base-pass draw is
**`SubmitDrawEnd`** (`:1332`), which handles the indirect case through the same
test at `:1371`. Same outcome; it matters only because anyone bringing this up by
breakpointing `SubmitDrawIndirectEnd` will never hit it and will read that as
"the indirect path is not being taken".

**A genuine piece of luck, worth stating because it removes a whole failure
mode.** `SubmitDrawEnd` can *override* a command's own args from
`SceneArgs.IndirectArgsBuffer`, but only when
`MeshDrawCommand.PrimitiveIdStreamIndex >= 0` (`:1334`). This factory
deliberately omits `SupportsPrimitiveIdStream` (`VoxelQuadVertexFactory.cpp:210-214`),
so that index is `INDEX_NONE` and instance culling cannot hijack our args buffer.
*(Confirm during bring-up rather than trusting this paragraph — it is read from
the flag list, not measured.)*

---

## 2. What it understates, #1: **there is nowhere in the current architecture to put the compute pass**

The ADR describes the compaction pass as a step. It is the wave.

Constraint 5 of `gpu-g2-draw-path.md` says RDG inserts the compute→draw barrier
automatically "as long as both the writing pass and the draw declare the same
`FRDGBufferRef`". **The draw cannot declare anything.** The cull decision lives in
`FVoxelGpuPoolSceneProxy::GetDynamicMeshElements`, and `FMeshElementCollector`
exposes `FRHICommandList& GetRHICommandList()` (`MeshElementCollector.h:68`) and
**no `FRDGBuilder` at all**. So the prescribed pattern is unavailable at the only
place that currently knows which chunks are visible.

**The one in-engine precedent for this exact shape** — a compute cull writing
indirect args consumed by an `FMeshBatchElement` — is Landscape, and the piece
that makes it work is named nowhere in our design:

```cpp
// LandscapeCulling.cpp:767
DispatchIntermediates.IndirectArgsBuffer =
    ConvertToExternalAccessBuffer(GraphBuilder, IndirectArgsRDG, ERHIAccess::IndirectArgs);
FRHIBuffer* IndirectArgsBufferRHI = DispatchIntermediates.IndirectArgsBuffer->GetRHI();
...
Args.IndirectArgsBuffer = IndirectArgsBufferRHI;          // raw FRHIBuffer* onto the element
// LandscapeRender.cpp:3038
ViewDependentMeshBatch.Elements[0].IndirectArgsBuffer = CullingArgs.IndirectArgsBuffer;
```

`ConvertToExternalAccessBuffer(..., ERHIAccess::IndirectArgs)` is **load-bearing,
not tidiness**. `FD3D12CommandContext::SetupIndirectArgument`
(`D3D12Commands.cpp:108-118`) does *not* transition the buffer — it flushes
pending barriers and updates residency, nothing else. The buffer must already be
in the indirect-args state by the time the draw is submitted, and nothing on the
mesh-pass path will put it there. Get this wrong and the symptom is a D3D12
validation error at best and undefined draw arguments at worst.

And Landscape schedules that work from
**`FLandscapeSceneViewExtension::PreInitViews_RenderThread(FRDGBuilder&)`**
(`LandscapeRender.cpp:1328`), a `FSceneViewExtension` override, called from
`SceneVisibility.cpp:5127` before visibility runs — i.e. before
`GetDynamicMeshElements` gathers.

> **`ue-project/Source` contains zero matches for `SceneViewExtension`.** Wave G
> has to introduce one: registration, render-thread lifetime, and per-view state
> keyed by `FSceneView*`. That is subsystem-scale plumbing and it appears nowhere
> in the ADR's scope.

The alternative — dispatching the compute shader straight onto the collector's
`FRHICommandList` from inside `GetDynamicMeshElements` — is possible
(`FComputeShaderUtils::Dispatch` has an RHI overload) but that method is called
**concurrently across views and shadow cascades** (`bSupportsParallelGDME`, the
reason Wave A had to move the cull's working set off the proxy). Issuing a
dispatch whose result a later pass reads, from a parallel gather task, with no
RDG dependency to order it, is the kind of thing that works on one vendor and
one driver. Not the first thing to try.

---

## 3. What it understates, #2: **shadow gathers, and what they do to the headline number**

`GetDynamicMeshElements` is called once per shadow cascade as well as once per
view — Wave A's cull handles this explicitly, taking
`View.GetDynamicMeshElementsShadowCullFrustum()` and backing out the
pre-shadow-translation from every plane (`VoxelGpuPoolComponent.cpp:567-591`).
The pool sets `CastShadow = true` in its constructor.

Shadow frusta **do not exist** at `PreInitViews_RenderThread` — they are built in
`InitDynamicShadows`, afterwards. So a compaction pass scheduled the way
Landscape schedules its own can serve **only the main view(s)**, and every shadow
gather keeps either Wave A's range path or the full draw.

**This is not fatal, but it resizes the claim, and the resizing must happen
before the numbers are quoted rather than after.** Wave A's
~1.19 µs / 1000 quads model was fitted against **whole-frame p50**, so whatever
the shadow depth passes cost for this pool is already inside that slope. The
`drawnQuads` figure the model was fed is logged per gather and the periodic log
does not separate main-view gathers from shadow gathers. Therefore:

> **The predicted 4.7 ms at the horizon is an upper bound on what main-view
> compaction can recover, not an estimate of it.**

### The instrument that produced those numbers is structurally blind to shadow gathers

**Found before asking for box time, in Wave A's own retained logs.** Across the
five sessions in `ue-project/Saved/Logs` that contain cull lines there are **112
`cull: runs=` samples and every single one says `shadowGather=0`.**

That is not evidence the pool has no shadow gathers. **It is an aliasing
artifact, and the arithmetic is exact.**

`BuildCulledRanges` logs on `CullLogCounter.Increment() % 600 == 1`.
`FThreadSafeCounter::Increment()` returns the **post**-increment value
(`ThreadSafeCounter.h:52-55`), so samples land at counter values 1, 601,
1201, … — that is, at gather ordinals 0, 600, 1200, …. With a constant `G`
gathers per frame (main view plus shadow cascades), the sampled gather's index
within its frame is `(600k) mod G`, which is **0 for every k whenever `G`
divides 600**. And 600 = 2³·3·5², so every plausible value of `G`
(1, 2, 3, 4, 5, 6, 8, 10, 12…) divides it.

**The periodic log has been sampling gather index 0 — and only gather index 0 —
in every leg ever run.** The corroborating tell was there all along and read as
stability: `visibleQuads=164534` is identical *to the digit* across many samples
and across both straight-down legs. A rotating sample could not do that; an
aliased one must.

Two consequences, pulling in opposite directions:

- **Good for the ratios.** Wave A's 2.72× and 1.62× over-draw figures describe
  the main view specifically, which is exactly the quantity compaction targets.
  They are not a blend of main and shadow.
- **Bad for the slope.** The ~1.19 µs / 1000 quads model divided a **whole-frame**
  Δp50 by a **main-view-only** Δ`drawnQuads`. If shadow gathers also draw the
  pool — and they do go through the same proxy — the true Δquads between uncull
  and cull is larger than the logged one, so the **true slope is smaller than
  1.19** and the saving compaction can recover is correspondingly less. The two
  poses agreeing to 1.1% does not rescue this: both were measured with the same
  aliased instrument, so a shared bias cancels in the cross-validation and
  survives into the estimate.

**The fix is one character of period, not a new harness.** Make the log period
coprime with any plausible `G` — 601 is prime — or, better, accumulate
per-gather-type totals across a frame and log those, which answers the question
directly instead of by sampling.

### G0 as built, and the rule that reads it — written before the legs run

`voxel.Stream.GPUCullStatsPeriod N` turns on a **census**, not a sample. Every
gather is accumulated at the point of submission — after the elements are built,
before `Collector.AddMesh` — so it counts what the proxy actually asked the GPU
for, on the cull and uncull branches alike. The uncull control is half the cost
model's evidence and the cull's own log never runs on it at all.

The aliased sampling log is fixed in the same change: period 600 → **601, which
is prime**, so it shares no factor with any gathers-per-frame count and walks
every gather instead of pinning one. It now prints `gather=` so the rotation is
visible rather than inferred. **Expect that line to look noisier than it used to;
that is the fix working.** Its former stability was the aliasing.

#### The quantity that decides it

Between `uncull` and `cull` at one pose, write `Δcam` and `Δshadow` for the drop
in quads submitted by camera and shadow gathers. Wave A divided the whole-frame
`Δp50` by `Δcam` alone. Define

> **`S_Δ = (Δcam + Δshadow) / Δcam`**

The true slope is then `1.19 / S_Δ` µs per 1000 quads, and since compaction
scheduled the Landscape way reaches only the camera view, the saving it can
recover is the camera over-draw priced at that true slope:

| pose | camera over-draw (Wave A) | predicted saving |
|---|---|---|
| horizon | 6,221,534 − 2,285,770 = **3,935,764** | **4.68 / `S_Δ` ms** |
| straight down | 266,656 − 164,534 = **102,122** | **0.122 / `S_Δ` ms** |

#### Pre-registered verdicts

The decision this feeds is whether compaction can close the straight-down
residual that kept `voxel.Stream.GPU` at 0 — Wave A measured that residual at
**0.17 / 0.13 ms**, against its resolution `R` = 0.114 ms and a direct control
spread of 0.05 ms.

- **`S_Δ ≤ 1.15`** — down saving ≥ 0.106 ms, horizon ≥ 4.07 ms. The model stands
  essentially as recorded. **Wave proceeds as briefed.**
- **`1.15 < S_Δ < 2.0`** — down saving 0.061–0.106 ms, i.e. **below Wave A's own
  `R`** and within sight of the 0.05 ms control spread. Horizon still 2.3–4.1 ms.
  **Wave proceeds, but the headline changes** from "puts the pooled path ahead"
  to "large horizon win; the default flip is not established by compaction
  alone". Recorded here so that restatement is not a post-hoc rescue.
- **`S_Δ ≥ 2.0`** — down saving ≤ 0.061 ms, at or under the direct control
  spread. **Camera-only compaction cannot deliver the wave's stated purpose.**
  The honest responses are to extend compaction to shadow gathers — which needs a
  different scheduling story, since shadow frusta do not exist at
  `PreInitViews_RenderThread` — or to stop and say so.

**The cheapest good outcome is binary:** if `shadowGathers == 0`, then
`S_Δ = 1` exactly, Wave A's numbers stand unmodified, and the aliasing was a real
bug in the instrument that happened not to bite. One leg settles it.

#### The control that makes the census falsifiable

The census must **reproduce the aliased log's camera figures**. Straight down with
the cull on, camera quads per gather should land on 266,656 and camera visible on
164,534. If the census disagrees with the old log *on a camera gather*, the census
is wrong and not the log — because the one thing the old log was definitely doing
is reporting a camera gather. An instrument whose first output cannot be checked
against a known number is not an instrument yet.

### The first experiment, and it can fail

Split the existing `drawnQuads` / `visibleQuads` counters by gather type — the
proxy already computes `bShadowGather` two lines away — and log the two totals
separately over a settled pinned pose.

- It is a **count**, not a frame time: immune to the 400 ms clamp and to GPU
  contention, which makes it the safest evidence available.
- It costs one leg per pose and **needs no compaction, no indirect draw, and no
  view extension** to run.
- **It can fail, and failure is informative.** If most of the horizon's drawn
  quads come from shadow gathers, then main-view compaction cannot deliver
  anything near 4.7 ms and the wave's headline is wrong before a line of it is
  written. That would redirect Wave G toward the shadow path — or toward a
  cheaper answer entirely — for the price of one measurement.

This is deliberately the opposite shape from Wave A's first cull control, which
"covered 98% of the pool regardless of contents and certified nothing".

---

## 4. What it understates, #3: `BaseQuad` does not disappear, it forks

The plan says the `.ush` becomes `QuadIndex = CompactedIds[VertexId / 6]` and the
per-range `BaseQuad` Wave A added "becomes 0 forever". True **of the compacted
path**. Still needing Wave A's expression, on the same day the compacted path
ships:

- `voxel.Stream.GPUCull 0` — the uncull control, which every measurement in Wave
  A is read against and which must stay byte-identical to survive as a control.
- The single-chunk `UVoxelGpuChunkComponent` (`PoolMode == 0`).
- Every shadow gather, per §3.
- `GPUCullDebugSplit` / `DebugInvert` / `DebugAllVisible`, all three of which are
  range-shaped by construction.

So `VoxelQuadVertexFactory.ush:110` becomes a **branch**, not a substitution —
one flag in a uniform buffer, or one permutation. It is still exactly one place
in the file, which is the property that matters; it is not one line.

## 4a. Smaller traps, recorded so they are not rediscovered

- **`MaxVertexIndex` is still read when `NumPrimitives == 0`.**
  `SetDrawParametersAndFinalize` skips `VertexParams` on the indirect branch, but
  `MESH_DRAW_COMMAND_STATS` (`:965-969`) reads
  `MaxVertexIndex - MinVertexIndex + 1` unconditionally. Only a stat, but leave
  the element's Min/Max sane rather than zeroed.
- **A capacity rebuild invalidates every GPU-written range.** Flagged by Wave D
  for its own buffers; it applies to a compaction output buffer identically. The
  compacted list and the args buffer must be re-derived, not carried, across any
  path that goes through `MarkRenderStateDirty`.
- **`kMaxBatchElements = 64` stops being a constraint on the compacted path** —
  one element, one draw — but the fallback paths above still live under it, so the
  constant and its reasoning stay.

---

## 4b. G0 RESULT — measured 2026-07-26. The wave was aimed at 7.3% of its own target.

Five legs, one box state, **every leg settled identically** —
`loaded=13190 quads=13,088,897 jobsInFlight=0 pendingJobs=0` — so scene
equivalence is observed rather than assumed. `LogInit: Command Line:` verified per
leg. Raw evidence in `docs/measurements/wave-g-g0-census.txt`.

| pose | config | camera quads/frame | shadow quads/frame | shadow gathers | S |
|---|---|---|---|---|---|
| horizon | cull | 7,251,008 | 21,206,739 | **4.00** | 3.93 |
| horizon | uncull | 13,088,897 | 52,355,588 | **4.00** | 5.000 |
| down | cull | 2,053,329 | 25,618,715 | **4.00** | 13.48 |
| down | uncull | 13,088,897 | 52,355,588 | **4.00** | 5.000 |

**`S_Δ` = 6.34 at the horizon, 3.42 straight down. Both fire the pre-registered
`S_Δ ≥ 2.0` branch.** Applying the corrected slope `1.19 / S_Δ` to Wave A's own
scene and residual: straight down **0.036 ms** against a 0.13–0.17 ms residual and
a 0.114 ms resolution floor — short by a factor of four, not marginally. Horizon
**0.74 ms**, not 4.7 ms.

**Camera-only compaction cannot flip `voxel.Stream.GPU`.** Reported as it landed,
under a rule written when the expected answer was `S_Δ = 1`.

### The instrument earned the right to overturn the old one

Three independent checks, all passed:

1. **It reproduces the old log exactly where the old log was valid.** Leg 1's
   camera gather (`gather=9015, shadowGather=0`) reports
   `drawnQuads=7251008 visibleQuads=1931679` — identical to the census. Wave A's
   266,656 / 164,534 could not serve as the planned control because the scene has
   grown since (13,190 chunks against 9,822), so a same-session check replaced it,
   which is stronger.
2. **Both uncull legs land on `S = 5.000` and `camera = 13,088,897` exactly** —
   precisely the full pool on all five gathers, which is the arithmetic the uncull
   path is defined by.
3. **The prime period visibly rotates.** Four of six samples read
   `shadowGather=1`; the old period never showed one in 112 samples across five
   sessions.

### Where the geometry actually is

Straight down, cull on, per frame:

| gather | visible | drawn | over-draw |
|---|---|---|---|
| camera | 962,859 | 2,053,329 | 1,090,470 |
| 4 shadow cascades | ~11,853,770 | 25,618,715 | **13,764,945** |

**Shadow gathers are 92.6% of submitted quads and 92.7% of the removable
over-draw.** The camera view — the only thing a Landscape-shaped pass can reach —
is 7.3% of the prize. Three of four cascades sit pinned at `ranges=64`, so the
element cap binds hardest exactly where the geometry is.

**Wave A's diagnosis survives completely: the residual is over-draw, not
overhead.** What changed is *whose*. Only a gather-split census could tell the
difference, and nothing else in the programme would have found it.

## 4c. The two cheap levers, priced — one ruled out on paper, one measured

### Merge gap: ruled out analytically, and raising it is strictly harmful

No legs needed; this falls out of the two merge passes as written.

Pass 1 merges every gap ≤ `MergeGap`. Pass 2, if more than `MaxRanges` remain,
merges the smallest gaps first up to a threshold `T`. **When the cap binds, every
gap surviving pass 1 exceeds `MergeGap`, so `T > MergeGap` and the final merged
set is `{gaps ≤ T}` either way — the same result `MergeGap = 0` would give.**

So:

- **Cap-bound (three of four cascades, and the camera): the knob is inert.** It
  cannot help, at any value.
- **Not cap-bound: `MergeGap = 0` is strictly better**, because pass 2 still
  guarantees the element limit and produces the optimal cover. The one
  non-cap-bound cascade measured (`ranges=35`, 492,838 drawn / 363,312 visible)
  offers at most 129,526 quads — **under 1% of the 14.9M total over-draw.**
- **Raising it is strictly wrong in both regimes.**

`voxel.Stream.GPUCullMergeGap 0` is therefore weakly better than any positive
value, everywhere, and worth almost nothing. **Do not spend legs on this knob.**

### Range budget: real, bounded, and it buys over-draw with draw calls

`voxel.Stream.GPUCullStatsPeriod` now also logs the exact **range-budget curve**,
computed from the unmerged survivors before either merge pass. It is arithmetic,
not a simulation: pass 2 merges smallest-gaps-first, which is optimal for "fewest
redrawn quads subject to ≤ K ranges", so for R survivors
`drawn(K) = visible + (sum of the R−K smallest gaps)`.

Measured, straight down, settled (leg 5):

| gather | runs | visible | drawn(64) | drawn(256) | drawn(1024) | drawn(∞) |
|---|---|---|---|---|---|---|
| camera | 1,008 | 962,859 | 1,985,064 | 1,204,913 | **962,859** | 962,859 |
| shadow A | 4,693 | 4,449,054 | 9,074,719 | 7,820,773 | 5,591,580 | 4,449,054 |
| shadow B | 3,330 | 3,152,813 | 5,298,190 | 4,397,682 | 3,165,143 | 3,152,813 |
| shadow D | 373 | 363,312 | 454,271 | 363,312 | 363,312 | 363,312 |
| **total** | | **8,928,038** | **16,812,244** | **13,786,680** | **10,082,894** | **8,928,038** |

Over-draw removed against the K=64 baseline: **18% at K=128, 38% at K=256, 85% at
K=1024, 100% at K=∞.**

**Two findings, and the second is the one that matters.**

**The 64-range over-draw is a floor, not slack.** Because pass 2 is already
optimal for its budget, `drawn(64)` *is* the best any range-based cull can do with
64 elements. There is no tuning left in the current shape.

**Compaction is not 15% better than a large range budget — it is off the curve
entirely.** A K-range draw costs K draw calls. Reaching zero over-draw by
budget alone needs K ≈ 4,693 *per gather*, and at 5 gathers that is ~23,000 draw
calls a frame — **more than the ~9,800 per-chunk draws of the component path this
whole architecture exists to replace.** Compaction is **one** draw call *and*
zero over-draw. The range approach can have one or the other.

*This reframes the redirect rather than softening it.* The case for compaction was
never really the 4.7 ms; it is that every alternative trades over-draw against
draw count and compaction does not. But that case is now attached to the **shadow**
gathers, which hold 92.6% of the quads — and that is the part with no scheduling
story, because shadow frusta do not exist at `PreInitViews_RenderThread`.

*Worth recording precisely because it is convenient for the camera path:* the
camera gather converges at **K = 1,024 exactly** (runs = 1,008), so for the camera
view a larger element budget is compaction-equivalent, at 1,024 draws. It is the
cascades at 3,330 and 4,693 runs that do not converge anywhere affordable.

## 4d. Does shadow over-draw scale with cascade count? — yes, and it opens a lever outside this wave

Asked directly, so answered directly. **No, each cascade does not draw close to
the full pool** — the four cascades' *visible* sets are 34.0%, 29.7%, 24.1% and
2.8% of it. They are nested annuli, and they largely partition rather than
duplicate.

But two things follow anyway:

- **Over-draw scales with cascade count**, because each cascade independently pays
  its own merge tax against its own 64-element budget — 1.36×, 1.66×, 2.04× and
  2.78× respectively. Four cascades means four merge taxes, not one.
- **Collectively the cascades' visible geometry is ~90.6% of the pool** — against
  **7.4%** for the camera at the same pose. The sun sees essentially the whole
  resident world; the camera sees almost none of it.

That second number is the lever, and it is nobody's current work. **Even perfect
compaction still draws the 11.85M quads that are genuinely visible to some
cascade.** That floor is set by every resident chunk out to 2 km casting a
dynamic shadow, including the coarse outer rings whose shadow contribution at that
distance is negligible. Capping shadow casting by ring or level attacks the floor
itself rather than the merge tax on top of it — and it is a relevance change, not
a rendering-architecture change.

**Not costed here and not this wave's to take.** Recorded because it is cheaper
than either compaction route and, on these numbers, plausibly larger than both.

## 4e. Keep the census and the prime regardless of what this wave becomes

Stated plainly because it is easy to lose when a wave is rescoped. Neither of
these is Wave G scaffolding:

- **The gather census** converted an unmeasured assumption into a number, and it
  is what any future claim about this renderer's cost has to be read against.
  Every quantitative statement Wave A made about drawn quads was main-view-only
  without knowing it.
- **The prime period** stops the next person inheriting the same blind spot. A
  round period aliases against gathers-per-frame; that is not a quirk of 600, it
  is a property of round numbers.

They cost nothing when `voxel.Stream.GPUCullStatsPeriod` is 0, which is the
default.

## 4f. Costing the shadow-casting cap — NOT BUILT, owner's decision

Costed on request, 2026-07-26. **No code written, no legs spent.** One leg would
replace this section's single estimate with a measurement; see the end.

### 1. What it would take — and the framing question turns out to be moot

The question asked was whether **per-chunk shadow relevance is expressible when
one primitive covers the whole pool**. Engine-side the answer is no: shadow
relevance is per-primitive (`Result.bShadowRelevance = IsShadowCast(View)`,
`Mesh.CastShadow = IsShadowCast(Views[ViewIndex])`), one primitive means one
answer, and nothing in `FPrimitiveViewRelevance` is per-chunk.

**But we never need relevance to express it, because the pool already runs its own
per-chunk cull inside the shadow gather.** Three facts, all already true in the
shipped code:

- `BuildCulledRanges` runs **once per gather**, and already distinguishes shadow
  gathers — `bShadowGather`, from `View.GetDynamicMeshElementsShadowCullFrustum()`.
- It already **iterates per-chunk runs** in that gather.
- It already reads each chunk's **level**, because `AddChunk` stores
  `Scale = float(1 << Level)` in the chunk table's `.w` and the cull loads it as
  `const float Scale = Entry.W` — three lines above the frustum test.

So the cap is one extra rejection in a loop that already exists:

```cpp
if (bShadowGather && Scale > MaxShadowScale) { ++SkippedShadowLevel; continue; }
```

**No new buffers, no new plumbing, no relevance change, no shader change, and no
per-chunk data that is not already resident.** The level is already there because
the vertex factory needs it for mip scale.

That makes this by a wide margin the cheapest lever on the board — cheaper than
the range budget, and not comparable to compaction.

### The hazard that would otherwise ship silently

**`VisibleQuads == 0` currently falls back to drawing the ENTIRE pool.** That is
deliberate and correct today — a cull that hides the world is this pool's worst
failure mode, so the conservative branch draws. But with a shadow cap it inverts:
a distant cascade whose entire visible set is above the level cap would empty its
range list, hit that fallback, and draw **13.09M quads instead of 0** — turning
the optimisation into the largest regression available.

The fix is not difficult but it is not optional: the empty case needs three states,
not two — *nothing visible* (draw everything, as now), *everything capped* (draw
nothing, correctly), and *cull produced ranges* (draw them). Anyone building this
must handle it explicitly, and it is exactly the shape of bug this codebase has
shipped before.

**Two unknowns I did not resolve and would not assume:**
- **Shadow-map caching.** If cached whole-scene shadows or VSM pages hold
  geometry that later stops casting, the cache needs invalidating. Dynamic
  relevance suggests this is fine; I have not verified it.
- The cap must apply **only** to shadow gathers. The camera must still draw
  everything it can see, at every level.

### 2. What it would save

Ring occupancy at the settled scene (`loaded=13190`, pool 13,088,897 quads), and
the distance band each level covers under `kDefaultRingPresets`:

| level | band | chunks | share | cumulative share at level ≥ N |
|---|---|---|---|---|
| 0 | 0–64 m | 3,042 | 23.1% | 100% |
| 1 | 64–128 m | 2,029 | 15.4% | 76.9% |
| 2 | 128–256 m | 2,225 | 16.9% | 61.6% |
| 3 | 256–512 m | 2,262 | 17.1% | **44.7%** |
| 4 | 512–1024 m | 2,014 | 15.3% | **27.5%** |
| 5 | 1024–2048 m | 1,618 | 12.3% | **12.3%** |

Against the measured shadow floor of **11.85M quads visible to some cascade** —
the floor that survives even perfect compaction — a cap would remove roughly:

| cap | terrain stops casting beyond | est. share of pool | est. quads removed from the floor |
|---|---|---|---|
| level ≥ 5 | 1,024 m | 12.3% | ~1.5M |
| level ≥ 4 | 512 m | 27.5% | ~3.3M |
| level ≥ 3 | 256 m | 44.7% | ~5.3M |

**This rests on one assumption I could not verify and am flagging rather than
burying: that quads-per-chunk is roughly uniform across levels.** It is plausible
— the clipmap keeps every chunk at 32³ voxels regardless of level, which is the
whole point of the structure — and it may lean conservative, since coarser terrain
is smoother and likely meshes to *fewer* quads per chunk. But it is an assumption,
and the numbers above are estimates, not measurements. **Nothing in the census
breaks quads down by level**, which is the one thing it cannot currently answer.

For scale: even the mildest cap (~1.5M) is comparable to the **entire** camera-view
over-draw that Wave G was originally built to remove (1.09M straight down), and
the level ≥ 3 cap is roughly **five times** it.

### 3. What it would cost visually — and this needs a human, not a diff

This is a **quality reduction**, stated plainly:

- **Terrain beyond the cap stops casting shadows onto anything.** At a low sun
  angle, long shadows thrown by distant ridges across intervening valleys simply
  disappear.
- **Distant terrain stops self-shadowing**, which is the more noticeable loss.
  Relief at distance reads substantially through its own shadowing; without it,
  far mountains flatten and read brighter than they should.
- **The cap boundary is a hard edge in world space.** Shadows stopping at exactly
  256 m or 512 m is the kind of artefact that reads as a bug rather than a
  setting, unless the boundary is chosen to coincide with a cascade transition
  where the shadow resolution already changes.

Mitigations exist — align the cap with a cascade edge, or cap the *outermost*
level only (level ≥ 5, beyond 1 km, where a chunk's shadow subtends very little
screen area) — but **which cap is acceptable is a judgement about how the game
looks, and no harness in this project can score it.** It belongs on
`docs/manual-verification-checklist.md` and in front of the owner's eyes, in a
scene with a low sun.

The honest framing for that decision: **level ≥ 5 is close to free visually and
removes ~1.5M quads; level ≥ 3 removes ~5.3M and will be seen.** The curve between
them is the whole trade.

### One leg would replace the estimate with a measurement

The only soft number above is quads-per-level. Adding a per-level breakdown to the
census — the chunk table already carries the level, and the census already runs
per gather — turns every estimate in this section into a measured count, split by
cascade, still immune to the frame-time clamp and to contention. **It is one short
leg and no draw-path change.** Not run, because this section was commissioned as a
costing rather than a measurement, and the shape of the answer is already clear
enough to decide on.

## 5. Why compaction, and not "indirect draw plus a base"

Recorded in `gpu-waves-plan.md` Wave A and re-confirmed here: an indirect draw
supplies `StartVertexLocation` from the GPU buffer, and D3D12 does **not** put
`StartVertexLocation` into `SV_VertexID`
(`RHISupportsAbsoluteVertexID` is Vulkan-only). So an indirect draw cannot
express "start at pool quad F" either. Anything drawing a sub-range of the pool
needs the base in-shader regardless of how the draw is issued.

Compaction sidesteps it: the compacted list *is* the indirection, `StartVertexLocation`
is 0, and the 64-range element cap — which is what forces the horizon's 2.72×
over-draw — stops existing.

---

## 6. Plan, staged so each layer is proven before the next is stacked

The project has **zero indirect draws today**. Every stage below is separately
falsifiable, and the early ones deliberately have a known-exact correct output.

### G0 — the gather split (no code on the draw path)
Per §3. Counts only. Decides whether the wave's stated value is real.

Two edits, both inside `BuildCulledRanges`'s logging and neither on the draw
path: break the 600-aliasing, and accumulate `visibleQuads` / `drawnQuads` per
gather type rather than sampling one gather. **Both are in
`VoxelGpuPoolComponent.cpp`, which Wave G is holding off until D1 merges** — so
G0 is written and ready but cannot land first, which is the one place the file
ownership order and the experiment order disagree.

### G1 — an indirect draw that reproduces the current full draw, bit for bit
The minimum machinery that can prove the draw mechanism:

- a persistent buffer with `EBufferUsageFlags::DrawIndirect` holding
  `{ NumQuads * 6, 1, 0, 0 }`, written CPU-side on the render thread;
- `NumPrimitives = 0`, `IndirectArgsBuffer` set, `IndexBuffer = nullptr`;
- **no compute pass, no view extension, no RDG, no shader change.**

Correct output is known exactly: pixel-identical to the current uncull full draw
against a same-session floor. **A failure here is unambiguously the draw
mechanism** — the state transition, the command signature, the `checkf` pair, or
the factory's lack of a primitive-id stream — and not compaction logic, because
there is none yet.

*If G1 cannot be made to render, the ADR's design does not work in this project
and that is worth knowing on day one rather than after the compute pass exists.*

### G2 — the compaction kernel, in its own `.usf`
`ue-project/Shaders/VoxelQuadCompact.usf`, following `VoxelQuadScan.usf`'s
precedent and its stated reasons: outside `worldgen.ush`, outside the determinism
digest's blast radius, no SPIR-V respin, clear of ground rule 10.

Verified against a CPU reference first — the same compacted id list the render
thread can compute from `Runs` — so the kernel is proven before it is ever the
thing driving a picture.

### G3 — schedule it: the `FSceneViewExtension` and the external-access conversion
Per §2. This is where the real bring-up risk sits, and by this point both the
draw and the kernel are already known good.

### G4 — the `.ush` branch, and the A/B
Only then does `VoxelQuadVertexFactory.ush:110` fork. **That file is shared and is
coordinated through the owner before it is touched.**

### Ordering against Wave D
G1's args buffer is proxy-owned and touches no pool buffer, so it does not
conflict with `claude/wave-d-pool-uav`. **G2's kernel writes into buffers Wave D
is moving to component ownership** — that is D1's foundation being used as
intended, and G2's landing is gated on D1 merging.

---

## 7. Verification order (from the owner's brief, unchanged)

1. **Pixel identity first** — against a same-session floor, because the floor is
   bimodal (two clusters, 0.00% within, ~1.81% between; a fixed threshold reads
   the per-session latch as signal).
2. **Then the mechanism** — `drawnQuads` should fall to `visibleQuads`. A count,
   not a time: immune to the clamp and to contention.
3. **Then the pinned-pose A/B**, ≥2 legs, both poses, against Wave A's board
   (component 9.42/9.89 horizon, 2.25/2.30 down; pooled-cull 9.34/9.31 and
   2.42/2.43).
4. **Then, only if the numbers support it**, the `voxel.Stream.GPU` default flip —
   criterion written down before the deciding legs run, as Wave A did.
