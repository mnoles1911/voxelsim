# GPU (ADR-0006) — what remains, and what each wave buys the game

Written 2026-07-26, after G0–G5 were recorded complete. They were not: G2/G3 built
the *destination* (a GPU-resident geometry pool drawn in one call) but never
switched the *producer*. Chunk geometry is still meshed on CPU worker threads.

Each wave below says what it changes **in the game**, not just in the code, and
carries an honest value verdict. Two waves are marked questionable; one item is
closed as "do not build".

> **This document was written from summaries. `docs/gpu-waves-plan.md` was written
> from the code, and it corrects four things below.** Read that one before
> starting work; it is the execution plan. The corrections are marked inline as
> **CORRECTED** and the reasoning is in the plan.
>
> Two scope decisions have also been taken since (Matt, 2026-07-26): **Wave D is
> in scope including coarse levels**, and **Wave B ships GI on by default** —
> which changes the verdicts recorded below for both.

---

## Wave A — Make the pooled renderer worth having (frustum culling) — **LANDED 2026-07-26 (PR #127)**

**Result, 12 legs on one box state, every leg settled identically:**

| pose | component | pooled-uncull | pooled-cull |
|---|---|---|---|
| horizon | 9.42 / 9.89 | 12.47 / 12.46 | **9.34 / 9.31** |
| straight down | 2.25 / 2.30 | 12.60 / 12.65 | **2.42 / 2.43** |
| **horizon → down** | −76% / −77% | **+1.0% / +1.5%** | **−74% / −74%** |

The pool's cost now tracks visibility instead of residency. **`voxel.Stream.GPUCull`
is ON by default; `voxel.Stream.GPU` stays 0**, decided by a criterion committed
before the deciding legs ran (residual 0.17/0.13 ms against a 0.114 ms threshold).

**Root cause:** `SV_VertexID` does **not** include the draw's base vertex on D3D12
— `RHISupportsAbsoluteVertexID` is true only for Vulkan. Every range was drawing
pool quads `[0, Count)` regardless of where it started.

**The finding that outlived the wave:** pooled cost is a **~2 ms fixed floor plus
~1.19 µs per 1000 quads *drawn*** — two poses, 4× apart in frame time, agreeing to
1.1%. That model makes the residual *geometry*, not overhead: straight down the
cull draws 3.0% against 1.9% visible (**1.62× over-draw**), priced at 0.122 ms
against 0.17/0.13 measured. **Compaction is therefore worth ~0.12 ms straight down
and ~4.7 ms at the horizon**, where over-draw is 2.72× and the 64-range cap binds
— likely enough to put the pooled path ahead of the component path outright.

*(The original defect, for the record: +23% at p50 and +253% with little in
frustum, with the pool unmoved at 18.58 → 19.05 ms while the component path got
64% cheaper.)*

| | |
|---|---|
| **In-game effect** | Frame cost stops scaling with *resident* geometry and starts scaling with *visible* geometry. |
| **Status** | **Done.** Cull on by default, output verified pixel-identical to the single full draw (0.00% of 745,600 px, twice). |
| **Next lead** | ~~The zero-stride vertex stream.~~ **CORRECTED** — `GNullColorVertexBuffer` is 16 bytes at **stride 0**, so the fetch address is `Base + StartVertexLocation * 0` and is always in bounds. The real lead is `RHISupportsAbsoluteVertexID`, which returns **false on D3D12**: `SV_VertexID` may not include the draw's base vertex, so every range would draw quads `[0, Count)`. See `gpu-waves-plan.md` Wave A2. |
| **Real fix if confirmed** | Pass each range's base quad down so every draw starts at vertex 0 and the shader computes `QuadIndex = BaseQuad + VertexId/6`. **CORRECTED** — this must go through a **uniform buffer**, not a loose per-element `FShaderParameter`: those are measured **not to bind** in this project (`gpu-g2-draw-path.md`), and `ShaderBindings.Add()` on an unbound parameter is a **silent** no-op. |
| **End state** | GPU-driven culling — a compute pass over the chunk table emitting an **indirect draw**, which keeps *one* draw call rather than one per visible run. This is the ADR's own design (`gpu-g2-draw-path.md`), and the docs state twice that the pool must compact into one contiguous range per frame. |
| **Verdict** | **Worth doing.** Without it the pool is a measured regression and cannot be the default. With it, one draw call for the whole world is a genuine architectural win. |

---

## Wave B — Voxel GI as a GPU volume (steps 3–5)

**The point is not throughput.** Today GI is baked into vertex colours at mesh
time, so a dig re-shades a chunk partly just to refresh its lighting. Sampling a
volume texture by world position decouples the two: lighting updates become texel
uploads, geometry updates stay geometry updates.

> **CORRECTED.** An earlier version of this section said the re-mesh being retired
> was on the **pooled** path. It is not: `BrickComponents` has no entry for pooled
> bricks (`VoxelGI.h:154-161`), so pooled chunks are never re-shaded at all, and
> `CollectDirtyChunks` already expands an edit only to the 1-voxel mesher apron,
> never to a lighting radius. The 5×5×5 re-shade that is genuinely retirable is on
> the **component** path, and the larger cost is the `voxel.GI.MaxQuadSpanVoxels`
> quad subdivision. See `gpu-waves-plan.md` Wave B.

Steps 0–2 are landed and verified: the pooled path feeds the light field (it
previously never did — `voxel.GI.Enabled 1` was a silent no-op under
`voxel.Stream.GPU`), the volume is sampled per pixel, and the encode matches the
CPU sampler at **0.000 mean error**, with a deliberate half-cell-shifted control
at 0.565 proving the harness is not comparing a thing against itself.

| step | what it does | in-game effect |
|---|---|---|
| **3** | Decide Scheme A vs B from the measured horizontal error (6.165 bytes) | Whether side-facing surfaces get exact or averaged irradiance. Affects cave wall lighting quality. |
| **4** | Camera-following re-centring (the origin is static today) | At the shipped default only **36 of ~1,950** resident bricks fall inside the volume. Without this, GI simply is not there for most of the world. |
| **5** | Retire baked GI on the pooled path | **The actual prize.** Digging updates lighting immediately, without re-meshing the chunk. Removes a whole class of re-mesh work from edits. |

Also untested from step 2: the X-run upload merge on a dig's contiguous
neighbourhood (measured at 1.4 bricks/run in steady state, but the dig case is
what it was built for), and zero-on-revoxelize / zero-on-evict, which are correct
by construction only.

**Verdict: worth doing, and the most clearly valuable GPU work left.** It stands
regardless of where meshing runs, and it improves something a player sees (cave
lighting responding to digging).

---

## Wave C — Correctness gates — **GREEN, landed 2026-07-26 (PR #124)**

**Both legs are bit-exact at `6e893ab3679a8c81`**, two legs each: `bench/vxc_gpu.exe`
via DXC → SPIR-V → Vulkan, and `voxel.GPU.VerifyRegion` via UE → DXIL → D3D12.

**The cause was never the toolchain — it was worldgen version skew.** The failing
run compared a **v6 CPU reference** against a **v8 GPU kernel**: `voxelcore.lib`
predated the v8 climate landing while Unreal compiled the current `worldgen.ush`.
Proven by reconstruction rather than argued — voxel-core rebuilt at the pre-v8
commit and driven with the current SPIR-V reproduces the recorded failure's exact
values, and the reverse pairing gives the mirror image.

**Both compile-flag hypotheses were falsified**: the `VXC_UE` `$Globals` layout is
byte-identical to the bench's explicit cbuffer at both shader models, and a packing
or int64-codegen divergence cannot coexist with bit-exactness over 393,216 cells.

**Guards added so it cannot present this way again:** `voxel.GPU.VerifyRegion` pins
its own CPU-reference digest with nothing from the GPU in it; mismatches are
classified by stage and counted uncapped; and `worldgen.ush` carries a compile-time
version lock against `vxc::kWorldGenVersion`.

**Still owed, recorded as unverified:** C2 (NVIDIA leg — no NVIDIA GPU on this box)
and C3 (min-spec-proxy M1 gate).

> **A third error, and the one that mattered most.** The failing entry recorded
> "all 4,096 columns match" as a localising fact. **The columns did not match** —
> that was an inference from a log excerpt whose column-mismatch lines had been
> truncated away. Every investigation since, including two in this document,
> started from "columns fine, cells wrong" and so came out kernel-shaped. One
> wrong sentence cost three root causes.

<details><summary>The original failing entry, kept for the record</summary>

**C1 — The Unreal leg of the cross-toolchain determinism gate is FAILING.**
Same HLSL, same GPU, same CPU reference: the standalone bench passes bit-exact
(`6e893ab3679a8c81`), while in-engine `voxel.GPU.VerifyRegion` fails
(`046b4a9f9c5e49b7`). The two toolchains disagree with each other — exactly the
fault class a two-leg gate exists to catch. Localised: columns match, quad decode
matches exactly, **cells differ on material ids only** and in a consistent
direction (the DXIL build's soil column sits one layer shallower).

> **CORRECTED — the floating-point contraction theory is impossible.**
> `worldgen.ush` contains **no floating-point arithmetic at all**; every operand
> from raster to material id is `int64_t`/`uint64_t`, which the file's own header
> states. There is no `<` for an FMA to flip by one ULP because there is no float
> to hold a ULP. And the proposed fix cannot be written: on UE 5.8's D3D12 path
> `CFLAG_NoFastMath` is never translated (`TranslateCompilerFlagD3D11` maps three
> flags and returns 0 for the rest), and there is no `-Gis` equivalent.
>
> The real compile deltas are **`cs_6_0` vs `cs_6_6`/`cs_6_8`** and **`cbuffer` vs
> loose `$Globals` binding**. A `$Globals` packing mismatch on `BrickZMin` would
> produce exactly this signature — columns match, cells shift vertically by one
> layer. This is the *second* wrong diagnosis on this one failure; both are kept
> visible. Hypotheses and the cheapest test: `gpu-waves-plan.md` Wave C.

**C2 — NVIDIA determinism leg.** Never run. The cross-vendor claim rests on one
vendor (AMD RX 7800 XT). ~$5 and ~20 minutes on a rented box. Cannot be
meaningfully run until C1 is green.

**C3 — Min-spec-proxy M1 gate re-run.** Still owed. Now worth doing, because for
the first time there may be a configuration that passes it.

**Verdict: C1 is a hard blocker for Wave D and should be fixed regardless** — a
red determinism gate is the project's most important invariant failing quietly.

</details>

---

## Wave D — GPU meshing in the streaming path — **IN PROGRESS**

This is the never-built headline of ADR-0006 invariant 1: *"a compute pipeline …
replaces the CPU worker mesh + per-chunk vertex upload for streaming."*

**What it was designed to accomplish:** move chunk geometry generation off the CPU
entirely, freeing the ~24 saturated worker threads for physics, NPCs and water,
and removing meshing as a limit on view distance.

> **Scope decision (Matt, 2026-07-26): build it, including coarse levels.** The
> "questionable value" verdict below is superseded — it was conditioned on R0 = 128 m
> not being on the table, and R0 = 128 m is now the plan's Wave F. The evidence
> against it is kept because points 1 and 2 are still true and still bound what the
> wave can claim.

**Landed so far (PR #126, `70481ab`):**

- **D2 — gated chunk-local `MeshEmit` permutation.** Verified byte-identical to the
  shipping CPU mesher over 88,860 quads, four legs, in **both** permutations; the
  determinism digest did not move (`6e893ab3679a8c81`); all seven kernels respun
  **SHA-256 byte-identical** so the bench leg provably did not change.
- **D3 — 4-byte scan total**, replacing a **786 KB per-chunk readback** — a 65–100×
  over-read against a typical ~900–1,500 live quads, which at R0 = 128 m would have
  meant **~6.4 GB of PCIe traffic against a ~0.09 s kernel**. Recorded honestly as a
  **trade**: bandwidth down 70–100×, latency-to-delivery roughly doubled.

**The correction that most changes what this wave can claim:**

> **GPU meshing does not remove the CPU job. It removes the meshing inside it.**

`FootprintBandCache` is reduced from a CPU column pass whose fields
(`cave.segs[]`, `shaftMarginSq`, `cavern`, `bedrockDepthMm`) `FVoxelGpuColumnSample`
does not carry — and that pass is **~45% of level-0 job time**. Whether it can move
to the GPU is being costed as a design pass before the `DispatchJobs` wiring, since
the band is a **scheduling** input consumed on the game thread *before* dispatch,
and a readback there sits on the admission critical path.

**Also corrected:** the plan's instruction to "delete the poll/`Lock` tail" would
have deleted the only completion event and hung nothing off `LevelJobsInFlight[]--`
— reintroducing the stranded-column bug the same plan warns about.

| piece | what it needs |
|---|---|
| **D1 — no-readback GPU→pool** | UAV on the pool's quad buffer, `ConvertToExternalBuffer` to bring it into RDG, and a **gated** `MeshEmit` variant baking the brick→chunk rebase and a pool base offset so the shader writes straight into the chunk's allocated range. Must be gated: editing the emitted bytes in place changes the digest. |
| **D2 — coarse-level generation** | `worldgen.ush` has **no level parameter**. R1–R5 have no GPU path at all, and they are ~80% of resident chunks. Adding it means mirroring new generation logic bit-exactly in HLSL — the most expensive and determinism-risky work in the programme. |
| **D3 — edited chunks** | Stay on the CPU. `NeedsOverlayAwarePath` already routes them to a game-thread path; the GPU never sees them. A carve-out, not a blocker. |

**Why the value is questionable now, on evidence rather than opinion:**

1. **Meshing throughput is not the current bottleneck.** Making meshing 39%
   cheaper did **not** improve the reported ring-gap symptom — the near rings
   absorbed the slack.
2. **It accelerates the cheapest 20%.** GPU support is level-0 only; coarse rings
   are the majority of the work and have no path.
3. **Blocked on C1.** GPU-generated cells currently disagree with the CPU
   in-engine.
4. The current implementation measures **188 chunks/s against the CPU's 765–944
   single-threaded** across ~24 workers — but note that figure measures "GPU **plus
   a 768 KB readback plus a once-per-tick poll**", which D1 removes by
   construction. **It is not the GPU's ceiling and should not be used to condemn
   the approach.**

**What would change the verdict:** the recorded **R0 = 128 m** goal, which
quadruples level-0 work and which `gpu-g0-sizing.md` says "cannot ship on the CPU
path". That is the regime where CPU meshing stops being adequate. Also: sustained
CPU core pressure from physics/NPCs competing with 24 meshing workers.

~~**Verdict: do not build now. Revisit when R0 = 128 m is on the table.**~~
**Superseded — R0 = 128 m is on the table (Wave F), so the condition this verdict
named has been met.** Point 4 also turned out to understate the case: the 188
chunks/s figure was GPU *plus* a readback that D3 has since cut by 65–100×.

**What this wave may claim, and may not.** Producer-side chunk throughput and
cold-fill time — which is what Wave F depends on. **Not** an end-to-end frame win:
Wave A measured the pooled path still costing more than the component path at the
down pose, and that residual is a *rendering* problem with a *rendering* fix
(compaction). The two must not be added together in a summary.

---

## Wave E — Remaining G4 parity items

**E1 — Per-chunk debug tints.** The last G4 item, and the **only** one that still
needs `M_VoxelTerrain.uasset` edited. Storage is solved (`ChunkParams.w` is free);
the route is a `float4` `TexCoords` interpolant with the tint packed in `.zw`.
**The decision that makes it safe: encode identity as ZERO, not white** — the
component path supplies only a `float2`, so `TexCoord0.zw` arrives as zero there
regardless of the graph, and a naive unpack treating 0 as black would render
every component-path chunk black the moment the material is regenerated.
*In-game effect: debug-only. Invisible in normal play.* Low priority.

> **CORRECTED 2026-07-26 (Wave E). The paragraph above is exactly backwards, and
> the wrong half is the safety argument.** A material asking for texture
> coordinate 1 does **not** receive zero on the component path. It receives
> **texture coordinate 0** — the world-planar UV, wrapped to a 32 m period, so a
> position-varying value in (−32, 32). `FLocalVertexFactory` *clamps* a
> material's texcoord request to the number of UV sets the mesh actually has, and
> clamping duplicates rather than zeroes
> (`LocalVertexFactory.ush:729-730`, and again explicitly at `:737`); the mesh
> has one, because `VoxelChunkComponent.cpp:634` takes
> `InitFromDynamicVertex`'s default `NumTexCoords = 1`. The renderer that really
> does deliver zero is the **pooled** one.
>
> So identity-as-zero would not have blacked out the component path. It would
> have multiplied its BaseColor by ±32 of position-dependent garbage — worse,
> because it reads as a shading bug rather than an obviously broken build.
> **Measured, not argued:** a probe material with
> `EmissiveColor = abs(TexCoord1) * 0.05` repaints component-path terrain in red
> and green sawtooth bands at the 32 m UV wrap, 30.9% of pixels differing at
> >8/255 against a 3.6% same-run floor, while the identical material leaves the
> pooled path untouched. See `docs/gpu-waves-plan.md` Wave E for the images and
> the corrected encoding (a sentinel range, which works because *no* texture
> coordinate this project can produce leaves (−32, 32)).

**E2 — Water pool.** Landed, `voxel.Water.GPU 0`. Renders correctly (2,231 cavern
bricks / 28,862 quads in one primitive). **Its parity number is honestly missing**
— same-path repeat runs differed 20–88%, so the comparison said nothing. Needs a
scene whose only variable is the water. Also note: one primitive means one
translucent sort key for all water; harmless for the current constant-opacity
material, but W5 fill-fraction shading, foam, caustics or refraction would each
break it.

**E3 — Ring cross-fade: CLOSED. Do not build.** Re-tested after the seam fix gave
the annuli their overlap band; it still produces see-through patches at ring
boundaries. The G0 checklist listed it *first*; it is the one item on that list
that should not be built. Spending the material-graph change on it would be
porting a defect.

---

## Wave F — Adjacent streaming items

**R0 to 128 m** — now unblocked (`RingPresets` became a runtime accessor,
overridable via `-VoxelRingInnerMeters=` / `-VoxelRingOuterMeters=`). Moving it is
still an open call, and the `+9.2%` resident-chunk cost of the seam-padding fix is
what to weigh it against. *This is also the decision that would revive Wave D.*

**`voxel.Stream.AdmissionBandSkip` — off, and should stay off.** It reaches ~4% of
the waste it was aimed at (89 skips against 2,186), its edit veto differs from the
dispatch site's in a way argued but never measured, and frame rate collapsed with
it on for unexplained reasons.

**The reported ring gaps while flying — NO ROOT CAUSE.** Not reproducible in the
20 m/s scripted flight on current main; coarse rings measure 110–142% of at-rest
residency (over-resident from eviction lag, not starved), and raising the ring
slot floors changes residency by +1%. See `manual-verification-checklist.md` item
1 — the question that splits the space is whether a gap fills in after you stop
moving.

---

## Suggested order

1. **C1** — the determinism gate is red; that is the project's core invariant.
2. **The ring gaps** — the actual reported symptom, still unexplained.
3. **Wave A** — makes the pooled renderer a win instead of a regression.
4. **Wave B** — the clearest remaining value, and player-visible.
5. **E1/E2** — small, low priority.
6. **Wave D** — only if R0 = 128 m goes ahead.
