# Backlog

One place for everything known-and-not-done. Written 2026-07-25 after the
worldgen v8 climate wave; **section 0 added 2026-07-28** after the streaming and
draw-path programme merged (PR #165).

Each item says what it is, why it matters, what it costs, and what unblocks it.
Items are grouped by **what kind of decision they need**, not by subsystem —
because the thing that stalls work here is usually "whose call is this", not
"where does the code live".

`docs/status.md` **stopped being written on 2026-07-29** and is now a historical
record, not the current one. Since then the chronological record is the merge
commit messages plus the dated document each programme leaves in `docs/` — water:
`docs/water-architecture.md`; assets: §10 below and the documents it cites. This
file is the forward-looking list. When an item lands, delete it here and say where
the result was written.

**Read in this order** (staleness reviewed 2026-08-19). §10 is the newest and
describes the world as it is now. §0 is the engine-performance front, but its
numbers are from 2026-07-28 and predate both the 192M pool and assets in the
world. §§1-7 predate August entirely; where this review falsified an item it now
says so inline.

---

## 0. ENGINE PERFORMANCE — the current front

Everything below this section predates the streaming programme — except §§8-10 —
and §6b in particular is stale and now says so.

> **Re-measure before quoting anything here. Reviewed 2026-08-19.** Two things
> changed underneath these numbers. The quad pool default went 80M → 192M
> (`036552f`), so "70% of 80M" no longer describes any run. And the world now
> composes environment assets into terrain chunks and draws ground cover as HISM
> instances out to 256 m (§10) — render-thread and GPU work that no leg below
> includes. The *conclusions* (render-thread bound, tail is GPU, game-thread work
> buys headroom not fps) are structural and are expected to hold; the
> milliseconds are not current.

### Where the engine is (2026-07-28)

Standard flight leg, shipped defaults, **2560x1440**:

| | value |
|---|---|
| p50 frame | 15.27 ms (**65.5 fps**) |
| p95 frame | 21.02 ms (47.6 fps) |
| max frame | 42.8 ms |
| hitch frames | 11–25 of ~16,000 |
| flight-phase holes (median) | ~57 |
| holes(final) / allocFail | 0 / 0 |
| chunks/s | ~1,074 |
| pool | 70% of 80M quads |

**Streaming is solved and is no longer the constraint.** 260.9 → ~1,074 chunks/s
across Waves S1–S4, zero permanent holes, and terrain now arrives ahead of the
camera (T4-1). Nothing in this section is a streaming item.

**The constraint is frame time, specifically its tail.** The median clears
60 fps; the 95th percentile does not, and there is ~1.5 ms of headroom against
the 16.7 ms budget before any gameplay system is added.

Full detail: `docs/measurements/session-summary-2026-07-28.txt`.

---

### 0.1 P0 — ANSWERED 2026-07-28: the frame is render-thread bound, the tail is GPU

Measured with `voxel.Stream.FrameAttribution 1` over two legs (~16,600 frames
each). Full numbers: `docs/measurements/frame-attribution-2026-07-28.txt`.

**A typical frame is the render thread.** render 13.49 ms against a 13.41 ms
frame, while the GAME thread spends 10.07 ms of it **waiting**. The voxel tick
contributes 0.47 ms. That retrospectively explains why cutting game-thread
publication from 2.94 ms to 0.055 ms moved nothing — the game thread had 10 ms
of slack to absorb it.

⇒ **Game-thread work buys headroom, never frame rate.** Anything aimed at fps
must reduce render-thread work.

**The tail is GPU.** The largest slow-frame delta is `renderWait` (+12.3 / +10.1
ms against a frame delta of +16.5 / +13.3) — the render thread blocked.

*The CPU voxel tick rises on slow frames (+5.1 ms) and was called "a passenger"
here. **The GPU capture partly reversed that:** the CPU tick is indeed a
passenger, but the GPU side of streaming — `WorldTick`, the meshing compute
passes — is **3.6–5.1 ms, 21–28% of the GPU frame**, and it IS on the critical
path. A frame with more terrain arriving has a longer GPU frame and therefore
more `renderWait`. See `docs/measurements/gpu-capture-2026-07-28.txt`.*

**Shadow cascades are not involved.** `shadowGather=0` throughout and ~1.03
gathers per frame — the "pool re-gathered 4–5× for shadows" hypothesis is dead.

**The cull walks the whole pool every frame:** 62,657 runs frustum-tested per
gather, of which only ~10,300 survive — and it scales with *resident* chunks, not
visible ones. *(An estimate of ~4.4 ms once stood here, derived from the 45–90 ns
per test recorded at Wave G. Measured, it is ~1.0 ms — see 0.1a. Do not use the
Wave G figure.)*

### 0.1a NEW P0 — Cheaper per-range binding (the emit, not the walk)

**Measured with `voxel.Stream.GPUCullTiming`.** The render thread's 13.72 ms:

| | per gather (~1/frame) |
|---|---|
| cull **walk** — 62,657 box tests | **~1.0 ms** |
| range **emit** — ~6,215 ranges | **~3.2 ms** |
| **not the voxel pool at all** | **~9.5 ms** |

**The ~4.4 ms walk estimate was wrong by 4×** — it multiplied 62,657 by the
45–90 ns/test recorded at Wave G; measured, it is ~17 ns. That hoist works far
better than its own comment claims.

So **the emit is the pool's real cost**, and its shape is one
`CreateUniformBufferImmediate` per range — exactly what the `kMaxRanges` comment
warned about.

**THE OBVIOUS FIX IS ALREADY FALSIFIED — read this before starting.** The
appealing version is to drop the per-range uniform buffer entirely by putting the
range's start in `FMeshBatchElement::BaseVertexIndex` and letting the shader
derive the quad from `SV_VertexID` alone. `VoxelQuadVertexFactory.ush` records
that this was tried and MEASURED:

> *SV_VertexID does not include the draw's base vertex on D3D12
> (`RHISupportsAbsoluteVertexID` is Vulkan-only), so a draw that starts at pool
> quad F still sees VertexId running from 0 … tiling the pool into N exact
> contiguous ranges drew only the first 1/N of it at N = 2, 8 and 64 while still
> paying for every quad.*

So every draw genuinely must be told its base explicitly on this RHI. The
remaining routes, neither cheap:

  - **per-instance vertex stream.** One entry per range holding `BaseQuad`, with
    each draw's `StartInstanceLocation` selecting its entry. Instance *fetch*
    honours the start location even though `SV_InstanceID` (like `SV_VertexID`)
    does not, which is the standard workaround. Needs vertex-factory stream work.
  - **cached multi-frame uniform buffers.** Keep a persistent ring sized to
    `kMaxRanges` and `RHIUpdateUniformBuffer` instead of creating ~6,215 per
    frame. Cheaper per range, but the single-frame lifetime exists precisely so a
    buffer is not rewritten while an in-flight draw still references it — the ring
    depth is the correctness argument and must be reasoned about, not guessed.

Either way this is vertex-factory work, not a call-site change, and it should be
planned as such.

*Interaction to remember:* the draw-path retune took ranges 1,023 → ~6,215 to
kill over-draw. Large net win (p50 30.59 → 15.15 ms), but it bought ~2.7 ms of
emit. If the per-range cost falls, the gap/ranges optimum moves and that sweep
should be re-run.

**Hierarchical cull is demoted** — it attacks the ~1 ms walk, not 4 ms. Real, no
visual cost, but rank it accordingly.

### 0.1c STRUCK — the depth prepass is not removable

Tested 2026-07-28, four arms: `docs/measurements/prepass-test-2026-07-28.txt`.
It was flagged as the largest and cheapest item on the 100 fps path. It is
neither.

`r.EarlyZPass 0` is ignored (still "Forced by Nanite"). `r.Nanite 0` does not
free it either — the forcing **hands over to DBuffer**. With both off it is still
5.708 ms. Two independent subsystems require a full depth prepass.

The fallback hypothesis — that the prepass was not earning its cost — dies too:
BasePass is 5.907–6.053 ms across all four arms, a 2.5% spread, so base-pass cost
is independent of it. GPU frame 17.25 → 17.08 ms. Nothing on offer.

⇒ **Realistic floor from the remaining items is ~12–13 ms (~80 fps), not 10.**
Reaching 10 ms needs the primitive count itself down — 17.8M per pass, drawn
twice — which points back at rendering distant rings as heightfield rather than
voxel geometry.

### 0.1b NEW P0 — What is the other ~9.5 ms?

The largest single unexplained block in the frame, and **bigger than everything
the streaming programme has optimised put together.** The voxel pool is only
~4.2 ms of the 13.7 ms render thread; the rest is `AVoxelClipmapActor` (the 30 km
heightfield), the water subsystem and its pool, sky, and the base/post chain.

Already ruled out: anything pixel-proportional (resolution is free). Break it
down with UE's render-thread stats or a ProfileGPU capture **before** sizing any
work against it.

<details><summary>Superseded framing (kept — this is the hypothesis that was falsified)</summary>

**Open, and the obvious hypothesis is already falsified.**

p50 15.27 ms against p95 21.02 ms is a ~5.7 ms spread. The streaming tick was
blamed because `perTick` (5.995 ms, on ~32% of frames) matched that gap almost
exactly. Incremental pool runs then removed a third of the tick — `buildRuns`
2.94 → 0.055 ms per publication, tick 11.73% → 7.75% of wall — and **p50/p95 did
not move at all**. The match was a coincidence.

The cause is unknown. The next attempt must find it rather than assume:

1. Capture a per-frame time series, not percentiles, and correlate slow frames
   against what else happened on them (publication? unload burst? GPU harvest?
   shadow cascade?). The `Hitch frame:` line already carries a
   subsystem/elsewhere split — extend that to ALL frames, not just >33.3 ms ones.
2. Establish whether slow frames are game-thread, render-thread or GPU bound.
   `renderMs` cannot answer this today: it is a HITCH field (lesson 17) and only
   exists above the threshold.
3. Only then pick a fix.

**Worth:** the difference between "60 fps median" and "60 fps floor", which is
the difference between the stated goal being met and not.

</details>

---

### 0.2 P1 — Occlusion culling

**Designed, not started. The largest remaining frame-time lever that costs no
visual quality.**

The cull is frustum-only. It already rejects 68% of chunks (41,946 of 62,119),
but most survivors are behind something: the GPU capture shows 17.8M primitives
submitted per pass, TWICE per frame, into an internal render target of 1552x873
(TSR upscales to 2560x1440) — roughly 11 triangles per rendered pixel.
Terrain occludes itself heavily — a ground-level camera sees a few hundred metres
of surface and nothing past the first ridge.

**UE's built-in occlusion cannot help.** Hardware occlusion queries are
per-PRIMITIVE, and ADR-0006 deliberately makes the whole pool one primitive with
one draw call. The renderer sees a single world-sized object and can only answer
"is the pool visible", which it always is.

Two approaches:

**(a) Horizon culling — cheaper, terrain-specific, and NOT SOUND FROM CHUNK
BOUNDS.** The appealing version is: build a per-bearing horizon-angle array from
the near chunks' `RunBounds` (which the cull already has on the render thread),
then reject any chunk whose angular extent falls entirely below it. Two passes
over ~10k in-frustum runs, no GPU readback, no frame of latency.

**It is wrong, and this was worked through on 2026-07-28 rather than discovered
in a screenshot.** A chunk's bounding box is not solid. Voxel terrain has caves,
arches, overhangs and thin spires, so a *near* chunk whose box reaches high does
not occlude anything — you can see straight past a spire, and straight through a
cave mouth. Occlusion derived from bounds is therefore not conservative: it hides
terrain the player can see, and a pooled primitive fails silently (ground rule 4),
so the symptom is missing landscape with nothing in any log.

To make it sound the horizon must come from something that is actually opaque —
the clipmap **heightfield** (`FootprintBandCache` / the surface-height columns),
not chunk bounds — and even then caves below the surface line break it. That
restricts it to "reject chunks entirely below the surface horizon", which is
close to what the buried skip already does at admission.

⇒ **Prefer (b).** Approach (a) is recorded here so it is not re-proposed as the
cheap option; its cheapness comes from using data that cannot answer the
question.

**(b) HZB occlusion — general, the real answer.** A compute pass tests each
chunk's bounds against the previous frame's hierarchical depth buffer and writes
a per-chunk visibility bit that `BuildCulledRanges` reads alongside the frustum
result. How UE's own GPU scene culling works; handles every camera pose. Costs
one frame of latency (standard, invisible at these rates) plus reprojection care.

Prerequisites and hazards:

- `ComputeRunBounds`/`RunBounds` already provide per-chunk world bounds on the
  render thread, so the input either approach needs exists.
- `RunBounds` is read by `GetDynamicMeshElements`, which runs **concurrently**
  across the camera view and every shadow cascade. A per-frame visibility buffer
  must not be written while a gather reads it — see the concurrency argument at
  `RebuildRunBounds`.
- **Shadow cascades must not use camera occlusion.** A chunk invisible to the
  camera can still cast a visible shadow. Gate on `bShadowGather`.
- Ground rule 4: a pooled primitive fails silently. Verify with a screenshot diff
  and a converged `CoverageVerify`, and add a debug mode that draws what
  occlusion rejected — the sibling of `GPUCullDebugAllVisible`.

**Worth:** unmeasured, but with the prepass struck (0.1c) this is now the
LARGEST remaining item — and uniquely, it cuts BOTH passes, since PrePass and
BasePass each submit the same 17.8M primitives. If half the in-frustum geometry
is occluded, both passes shrink together.

---

### 0.3 P2 — Deferred by owner decision (2026-07-28)

**S2-1 GPU hide pass — built, gated OFF, UNVERIFIED.**
`voxel.Stream.PoolGpuHide 0`; the pass, the pending-hide plumbing and
`UnmarkGpuHide` all ship and are inert at the default. Its forced probe went
through five rounds of harness bugs and still gives no trustworthy verdict. One
intermediate result was reported as "the probe caught a real bug" and is
**retracted** — the control, against the shipped CPU-shadow path, fails
identically. **Before trusting any verdict from that probe, verify it against a
known answer:** allocate one chunk, publish, read it back, assert the ids equal
that chunk's id. Full history:
`docs/measurements/s21-gpu-hide-probe-2026-07-28.txt`.

~~**S2-5 drop the CPU shadow — blocked on S2-1.**~~ **DONE 2026-08-18**
(`036552f`). The shadow was not dropped, it was **paged**: capacity now costs
VRAM only, the whole-array copy in `CreateSceneProxy` is gone, and the default
pool went 80M → 192M quads (`kPoolCapacityQuads`,
`VoxelWorldSubsystem.cpp:11650`) — which is what the asset-composed alpine vista
needed. S2-1 stayed gated off and did not block it. **Not yet seen in a rendered
frame at the new default** — that verification is §10b.

---

### 0.4 P3 — Geometry levers that cost visual quality

Take these only if 0.2 is exhausted and frame time is still short.

| item | saving | cost |
|---|---|---|
| **Per-chunk greedy meshing** — merging is per-brick (8³) not per-chunk (32³), so a flat 32×32 face becomes 16 quads instead of 1 | up to 16× on flat terrain in theory; far less in practice, since the merge key includes 4-corner AO | GPU mesher must match the CPU reference **bit-exactly** (`mesher.h`), gated by `VerifyAsyncMesh` and worldgen digest `6e893ab3679a8c81`. Both implementations change in lockstep, digest re-baselined. |
| **Cascade 6 rings → 5** (4 km → 2 km) | ~1/6 of resident chunks, ~7.8M quads | Draw distance. Not one line: `AVoxelClipmapActor` derives its entire vertex spacing from `RingPresets[kNumLevels-1].OuterMeters`. |
| **Coarser far rings** (32³ → 16³ at L4/L5) | halves far-ring quads, keeps draw distance | Distant detail. Biggest structural win, most work. |
| **Trim far-ring Z extent** | distant columns rarely need their full underground stack | Underground pop-in when descending at range. |

**Do not re-propose reducing per-ring quad counts as a bug fix.** Ring radii
double and chunk footprints double with them, so chunks-per-ring is constant **by
construction** — that is what a clipmap is. The flat L0–L5 distribution is
correct. (Proposed and withdrawn 2026-07-28.)

---

### 0.5 P4 — Speculation refinements (small)

- **DONE 2026-07-28 — `SpeculativeZTrim 1` and `SpeculativeMaxInFlight 16`.**
  Trimming one chunk from each end of the speculative band removes 98% of the
  zero-quad dispatches (183 → 3 per window) and lifts adopted-per-dispatch from
  46% to 85%. Both shipped as defaults.

  **But it bought no frame time, and that is the important part.** Cutting
  dispatches by 46% moved p50 by 0.08 ms. **Reducing GPU meshing work does not
  reduce frame time on this renderer** — the meshing compute evidently overlaps
  with rendering rather than sitting on the critical path, so removing it lets
  the GPU idle rather than shortening the frame.

  ⇒ This strikes "fewer mesh dispatches" from the 100 fps path, and it puts a
  question mark over **how much of the 18.4 ms GPU frame is serial at all**.
  Anything aimed at 100 fps has to establish that first, because the same
  overlap argument may apply to other GPU items on the list.

<details><summary>Original framing (kept — the reasoning that led here)</summary>

- **~187 speculative dispatches per window still return zero quads.**
  `dropOvertaken=0` and `dropPoolFull=0`, so it is all zero-quad results — and
  "zero quads" covers all-**solid** as well as all-air, which is why they cluster
  at both ends of the surface band and never the middle. Feeding the D6 band back
  from speculative results made the shared buried skip fire at all (0 → 51 per
  window) but end-to-end effect was small, because speculation is bounded by
  `SpeculativeMaxInFlight` rather than by candidate supply. Further reduction
  needs an **analytic** empty test at enumeration time. Bounded cost if never
  done: an air or buried chunk parks nothing, holds no pool range, evicts nothing.
</details>

- ~~**`voxel.Stream.VelocityLeadSec` still defaults to 0.**~~ **DONE** — default
  is now 2.0; T4-1 is on.

  <details><summary>original</summary>

  **`voxel.Stream.VelocityLeadSec` still defaults to 0.** T4-1 is confirmed (93%
  fewer flight-phase holes, no throughput/memory/game-thread cost) but ships off
  pending an owner call. Recommended default **2.0** — the effect saturates at
  1 s, so the cheapest setting is also the best.

  </details>

---

### 0.6 Standing rules for this area

1. **Read `docs/lessons-2026-07-27-s0-s1.md` first.** Seventeen lessons, most of
   them measurement failures where the number was arithmetically correct and
   answered a different question than the one asked.
2. **`frameMs`/`renderMs` are HITCH fields**, emitted only above a 33.3 ms
   threshold. Use the `VoxelPerfRun post-warmup` line for frame time.
3. **Run legs through `tools/voxel-run-flight-leg.ps1`**, summarise with
   `voxel-leg-summary.ps1` (refuses partial legs), audit with
   `voxel-audit-leg-overlap.ps1` (two legs sharing the box read exactly like a
   slow configuration).
4. **State the resolution.** The harness defaults to 1600x900; the target is 2K.
   Resolution is currently free because the renderer is geometry-bound — re-check
   after any change, because the moment something becomes pixel-bound every 900p
   measurement stops transferring.
5. **Never tune `GPUCullMergeGap` and `GPUCullMaxRanges` separately.** Swept
   alone, the winning value of each measures *worse* than the loser.
6. **Do not re-propose the falsified levers:** fork caps 16/32,
   `GpuMeshInFlight` 1024, `DispatchAfterDrain`, `AdmissionBandSkip`, ring
   cross-fade, slot-floor sweeps, ring-major dispatch, idle defrag (T3-6).
7. **A gate that no-ops and exits 0 is not a pass.** Several verification
   commands do exactly that when issued via `-ExecCmds` at startup.

---

## 1. Needs a decision or a spend, not engineering

**NVIDIA determinism leg.** The cross-vendor gate is verified bit-exact on the
AMD RX 7800 XT across all three modes, but the NVIDIA leg has never been run.
`docs/gpu-streaming-plan.md` already called it "the only unmet part of the
original gate", and worldgen v8's respin did not change that either way. Until
`tools/run-nvidia-digest.sh` runs against the current `.spv`, the cross-vendor
claim rests on one vendor. **Cost:** ~$5 and ~20 minutes on a rented box.
**Expected digests** are in `voxel-core/shaders/prebuilt/README.md`'s v8 respin
section.

~~**A second pregen in an arid region.**~~ **RESOLVED by regeneration, not by a
second pregen.** The orographic rain-shadow coupling plus the WorldClim
conditioning rebuild (§4, 2026-08-01) made the shipped world itself arid in
places: DESERT 9.74% of land, and both biomes now have real-tile coverage —
desert has **baked fine tiles** and a censused site, savanna a real interior
sample at s1 stride (`docs/biome-placement-survey.md`). The remaining arid gap is
authoring, not terrain: see §10a on roster starvation.

**Adopting the existing tile cache under the new identity.** Identity schema
v3 rolled every `provider_id`, so `pregen`/serving will not find
`terrain-diffusion-unlabeled-3e11cf157a836c70`. The game is unaffected
(`DefaultGame.ini` points at the directory by path and `TileGridSampler`
validates seed/scale, not identity). Set `provider_id_override` if the provider
should serve those tiles — that is exactly its sanctioned use.

---

## 2. Blocked on something else landing

**erosion-v7 (dendritic drainage).** ~340 lines of flow-accumulation valley
carving, parked. The version collision with v8 is trivial; the real blocker is
that the branch lands drainage CPU-only behind a flag defaulting ON, and PR #104
made `voxel.Stream.GPU` the default the same week. Under ADR-0006 display
geometry is GPU-generated while collision stays CPU, so drainage-on means up to
**5.6 m — 56 voxels — of solid-looking ground you fall through**. Full reasoning
in `docs/status.md`, "erosion-v7 is PARKED".

*Unblocks, cheapest first:* (a) land it with the flag defaulting OFF — default
output then equals `main` byte-for-byte, so no golden moves and no
`kWorldGenVersion` bump is needed at all; (b) port flow accumulation to a GPU
compute pass, which pairs naturally with the harness widening below.

**GPU harness cannot see real tiles.** *(Still open, confirmed 2026-08-19:
`gpu_harness.cpp:1963` still takes `SyntheticTileSampler&`.)* `gpu_harness.cpp`
and `VoxelGpuVerify.cpp` take a concrete `SyntheticTileSampler&`, not
`ITileSampler&`, so `vxc_gpu` can only ever exercise synthetic climate — never
the real-tile regime where v8's miscalibration actually lived. Mitigated for now
because v8 made the synthetic emission span every threshold, but that is a
mitigation, not a fix. **Worth doing before ADR-0006 makes the GPU path fully
authoritative.** Pure plumbing; the raster fill already goes through the virtual
interface.

**M1 gate min-spec-proxy re-run.** The 2026-07-24 numbers were taken at default
quality, not the historical min-spec protocol, so the gate row is uncoloured
pending a re-run. v8 changed terrain materially, so this wants redoing anyway.

---

## 3. Cheap and self-contained

**`tiff_export` round-trip smoke test.** `tools/make_conditioning.py`'s output is
validated against `tiff_export.CHANNEL_FILES` (five channels, float32, CRS, sane
ranges) but has never been round-tripped through the model, because a 200-cell
sketch upsamples 256x per axis into a 51200² raster. Run it once with a small
`--cells` (say 16 → 4096²) to prove the path end to end.

**Add a 3750 environment to `amplifier_surface_bound_adversarial`.** The test
crosses two pixel sizes, 30000 and 11250, and 11250 is *not* the scale-8 value —
that is 3750. The comment now says so. Adding a real 3750 environment is a new
environment plus a re-pin of a golden that is explicitly not worldgen output, so
it is a deliberate small change rather than a free rider.

**DONE — verified 2026-08-19.** `test_amplifier.cpp` now runs real 3750 *and*
1875 environments (`{kSeed, 3750}` at :591, and the `pixelMm[]` sweep at :792
crosses 30000/3750/1875); :869 records the digest re-pin those additions forced.

**Re-run `-VoxelMatHistogram` in engine.** v8's fix is verified by the CPU-side
top-voxel census (surface materials 12.4% → 100.00%), not by the quad census
that produced the original `MAT_ROCK 15% / MAT_SUBSOIL 85%` measurement in
`VoxelClimateProbe.h`. Re-run the switch and update that comment with real quad
numbers before anyone builds an id-keyed appearance rule on it.

~~**`VoxelEarth.Build.cs` hardcodes `build/voxel-core-msvc/voxelcore.lib`**~~
**DONE — verified stale 2026-07-28.** The module now probes
`Debug`/`RelWithDebInfo`/`Release`/`""` under `build/voxel-core-msvc` in a
deliberate order and errors with the full search list if none match, so
multi-config generators work without a manual copy.

**Shared `TileGridSampler` between `VoxelWorldSubsystem` and
`VoxelClimateProbe`.** Already flagged in `VoxelClimateProbe.h`. The two loaders
being copies is what caused the climate-vs-geometry divergence fixed in PR #113
— the tile-dir precedence is still a copy, and only the seed default is now
shared. Making both read one loader removes the class of bug rather than the
instance.

---

## 4. Larger, and worth scoping properly

**Natural language → world.** The design goal. The chain is already built except
the front end: `make_conditioning.py`'s `WorldSpec` is a small struct of named,
human-meaningful scalars, and everything downstream of it is that file plus
terrain-diffusion. Translating *"a big temperate continent with a dry interior
and a wet west coast"* into that struct is the only part that needs a language
model. `--dump-spec` already emits the resolved struct as JSON so a generated
spec can be inspected and edited before anything is generated.

*Two things to fix while doing it:* the round-trip is unproven (§3), and the
generated mountain ranges still read as drawn ribbons — deliberately not chased,
since elevation's `cond_snr` of 0.3 lets the model reinterpret them freely.

**Deserts require conditioning the precipitation channel.** Not a tuning
exercise. `synthetic_map.py` generates precipitation as an independent Perlin
field, never touched by elevation or distance from ocean — no orographic
rainfall, no rain shadow, no continentality. So no landmass scale produces a dry
interior. Either author the precipitation raster (`make_conditioning.py` does
this) or patch `synthetic_map.py` to couple it. See `docs/worldgen-levers.md` §2.

> **DONE 2026-08-01 — both halves.** `synthetic_map.py` was patched to couple
> precipitation to terrain (an orographic rain-shadow pass; the patch is
> `terrain-service/patches/terrain-diffusion-worldgen.patch`, parameters at
> `providers/diffusion.py:549-566`). Measured: correlation between the upwind
> barrier and the rainfall multiplier **−0.734**; mean multiplier **0.493**
> behind a barrier over 600 m against **1.393** with none. Separately, the
> conditioning statistics were rebuilt from the real WorldClim rasters — the
> previous file had used hand-written latitude formulas substituted when
> WorldClim was unreachable, which held precipitation to **7.8%** of its
> encodable range over land. The shipped world now measures **DESERT 9.74%,
> RAINFOREST 4.73%**, all eight mappable biomes non-zero. Note the rebuild alone
> was not enough: deserts only appeared after a monotone remap of the model's
> *output* in `adapt_raster_to_tile` (`3b511e3`, `56257c8`). See
> `docs/world-generation-architecture.md` §6.1–6.2.

**Narrow `diffusion.py`'s bio_12 quantization range** from 0..12000 to ~0..4000
mm/yr, and bio_1 from ±40 to ~±30 °C. Precipitation currently occupies 23 of 256
codes — 1 LSB is 47 mm/yr, coarser than the distinctions the biome thresholds
draw. This is the *root* fix for that; the physical-threshold work in v8 was the
consumer-side half. Changes tile bytes, so it needs a GPU rental and a
`provider_id` roll: **attach it as a rider to the next paid pregen**, not as its
own trip. Pleasant side effect: it would collapse `VoxelClimateProbe`'s remap to
near-identity and remove a calibration entirely.

**Scale-dependent slope thresholds.** `slopeMmPerPx` is proportional to
`pixelSizeMm`, so `kBiomeCliffSlopeMmPerPx`, the topsoil retention term, and
`slopeScaleQ10`/`microScaleQ10` in `amplifier.cpp` all mean different things at
scale 8 than at scale 1. Latent — only scale 1 has ever been generated — and
documented at the constant. Threading `pixelSizeMm` through `classifyBiome`
alone would be a half-fix at full CPU/GPU-mirror risk, so **do all three
together, before generating scale-8 tiles.**

**Restore per-material subsurface strata.** `VoxelClimateProbe`'s R channel is a
binary surface flag because thresholding a categorical material id through the
vertex-colour transform measured unreliable. The cost is that a cave wall is one
rock colour instead of bedrock/rock/gravel/subsoil/clay. v8 makes an id-keyed
rule viable again (surface materials now reach 100% of top voxels), but the
R-channel transform needs understanding first.

---

## 5. Remaining identity gaps

`DiffusionConfig.provider_id()` now covers checkpoint content, conditioning-data
content, world-shape kwargs, scale, channel mapping and the tile wire format.
Still outside it, in rough priority order:

1. **Execution environment.** `_load_pipeline` silently falls back to
   `device="cpu"` when no GPU is visible, so CPU- and GPU-generated tiles share
   a namespace; `torch`/cuDNN versions and TF32 flags are likewise absent.
   Doctrine §2.3 accepts cross-GPU non-determinism, but the id does not record
   which side of the CPU/GPU split a tile came from.
2. **`terrain_diffusion_version` defaults to `"UNRECORDED"`** and, unlike
   `UNPINNED`/`UNVERIFIED`, is neither refused before inference nor marked in
   the id.
3. **`pipeline.bind()`'s caching strategy.** `(x, y)` are not independent
   per-tile seeds; seamlessness comes from the tile store's cached context, so
   which neighbours are resident is process-history state that can influence
   output.

**`pregen --provider diffusion` still constructs the UNPINNED default config.**
Wire pinned-config selection into `app._make_provider` once a production
checkpoint is chosen.

---

## 6. Upstream (terrain-diffusion) issues we work around

**`drop_water_pct` silently does nothing.** `make_synthetic_map_factory` loads
`data/global/synthetic_map_stats.json` unconditionally and the cache is not
keyed on the parameters, so the land/ocean knob has no effect once that file
exists. `frequency_mult` escapes only because it also flows into the uncached
noise config. Delete the cache file to use `drop_water_pct`, and note that
recomputing it needs the WorldClim bio rasters present, not just
`etopo_10m.tif`. Details in `docs/worldgen-levers.md` §6.

**`python -m terrain_diffusion` imports the training stack** (`confection`),
which is not needed to run inference. Invoke the module function directly —
`tools/world_map.py`'s docstring shows how.

---

## 6a. RESOLVED 2026-07-26 (Wave C): the determinism gate is GREEN on both legs

**Both legs now print `6e893ab3679a8c81` and PASS bit-exact.** Two legs each,
same box, AMD Radeon RX 7800 XT:

| leg | toolchain | result | digest |
|---|---|---|---|
| `build/voxel-core-msvc/bench/vxc_gpu.exe` | DXC `cs_6_0` -> SPIR-V -> Vulkan | **PASS**, bit-exact, 8192 columns / 393216 cells / 6668 quads | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE `cs_6_6`/`6_8` -> DXIL -> D3D12 | **PASS**, bit-exact | `6e893ab3679a8c81` |

### The cause was VERSION SKEW inside one process, not the toolchain

The failing run compared a **worldgen v6 CPU reference** against a **worldgen v8
GPU kernel**. `voxelcore.lib` predated the v8 climate landing (`e25d563`, which
reached `main` at `2c7eb68`, 2026-07-25 19:28) while Unreal compiled the current
`worldgen.ush`. Nothing about DXIL, D3D12, `$Globals` packing or shader model was
ever involved.

**How that was established, rather than argued** (all on this box, 2026-07-26):

1. Built voxel-core at `2c7eb68^1` — `main` immediately before v8 — into an
   isolated worktree, and ran its `vxc_gpu` against its own committed SPIR-V:
   **PASS**, digest `f3c48a4df3e20e9a`. That is the pre-v8 baseline the four
   files listed below still quote, so the reconstruction is faithful.
2. Ran that same **v6 CPU** binary against the **current v8 SPIR-V**. It
   reproduces the recorded failure's values exactly:
   `cell(-64,-64,vz=11648): cpu=2 gpu=5` and `vz=11654: cpu=5 gpu=12`.
3. Ran the **reverse** pairing (current v8 CPU, pre-mirror v6 SPIR-V from
   `3fbf3f7^`). It produces the mirror image — `cpu=5 gpu=2`, `cpu=12 gpu=5` —
   which rules out "the shader was the stale half".
4. The recorded quad counts corroborate the direction: `3424 quads (cpu 3422)`
   is GPU-then-CPU, and 3422 is measured to be the **v6** count for that region
   while 3424 is the **v8** count.
5. No commit after the failure was recorded (`84b90fc`, 01:12) touched
   `worldgen.ush`, `shaders/prebuilt/`, `VoxelGpuWorldGen.cpp` or voxel-core's
   amplifier. Only the artifacts moved: `voxelcore.lib` was rebuilt at 02:00 and
   the editor relinked at 03:00. The gate has passed on every run since.

### Three wrong diagnoses, all kept, and what each one got wrong

1. **"worldgen v8 was never mirrored into the HLSL."** Wrong — the mirror
   (`3fbf3f7`) is faithful, as the bench proves. But the *class* was right: this
   was version skew. It looked for it in the source instead of in the link.
2. **"Floating-point contraction in DXIL flipped a layer comparison."**
   Impossible: `worldgen.ush` has no floating-point arithmetic anywhere (every
   operand is `int64_t`/`uint64_t`), and UE 5.8's D3D12 backend never translates
   `CFLAG_NoFastMath` (`D3DShaderCompiler.cpp:52-61`), so the proposed fix could
   not have been written either.
3. **"All 4,096 columns match, only cells differ."** Also wrong, and it is what
   sent both of the above hunting in the kernel. Under v6-CPU/v8-GPU the column
   fields `topsoilMm`/`subsoilMm` differ too (measured: `cpu=0 gpu=366`,
   `cpu=500 gpu=1232`), and `CompareRegion` prints a column's field mismatches
   *before* that column's cells. The quoted transcript was an excerpt with the
   `col(...)` lines dropped, and the conclusion was drawn from the excerpt.

### Guards added so this cannot present the same way again

* **`voxel.GPU.VerifyRegion` now pins a CPU-REFERENCE digest** of its own
  (`kExpectedCpuDigest`, `VoxelGpuVerify.cpp`), folded from
  `vxc::Amplifier`/`vxc::meshBrick` with no GPU involvement. A stale or
  mismatched `voxelcore.lib` now fails with "the linked voxelcore.lib is NOT the
  worldgen this gate is pinned to" instead of a list of per-cell materials. It
  also catches both sides moving *together*, which GPU-vs-CPU equality
  structurally cannot.
* **Mismatches are now counted and classified by stage** (column field / cell /
  quad) with the totals printed uncapped, and the capped list is labelled as an
  ordered subset that must not be quoted piecemeal. That is diagnosis 3's exact
  failure mode, closed.
* **`worldgen.ush` carries a compile-time version lock.**
  `VXC_WORLDGEN_VERSION_USH` must equal `vxc::kWorldGenVersion`, which
  `ModifyCompilationEnvironment` passes in as `VXC_WORLDGEN_VERSION_CPP`; a
  mismatch is a shader `#error` (verified by compiling with a deliberately wrong
  value). Because a define is part of the shader's DDC key, a version bump also
  forces a recompile instead of silently reusing the previous version's
  bytecode. Scope stated honestly in the file: this catches **source** skew, not
  a stale `.lib` — the header constant would still read 8. The two guards cover
  the two different faults.

### The historical entry, kept for the record

### An earlier version of this entry blamed worldgen v8. That was wrong.

The first diagnosis here said the v8 climate wave had changed CPU material rules
without mirroring them into the HLSL. **It had not.** The mirror exists and is
correct: `3fbf3f7` ("Step 2d: the worldgen.ush mirror + SPIR-V respin -- AMD leg
GREEN", 18:37) lands *after* `e25d563` ("Step 2 (CPU half)", 14:55), both are on
main, no CPU worldgen commit follows the mirror, and the bench passing bit-exact
proves the mirror is faithful. The wrong diagnosis is recorded rather than
deleted because it is the obvious one and the next person will reach for it too.

### Where the divergence actually is

From the in-engine failure (`voxel.GPU.VerifyRegion 0`):

```
[origin] 4096 columns, 196608 cells, 3424 quads (cpu 3422)
[origin] quad decode: 20544 vertices match the CPU reference exactly
    cell(-64,-64,vz=11648): cpu=2 gpu=5      MAT_ROCK    vs MAT_SUBSOIL
    cell(-64,-64,vz=11654): cpu=5 gpu=12     MAT_SUBSOIL vs MAT_PERMAFROST
```

- ~~**Columns match** (all 4,096) — `ColumnMain` agrees under both toolchains.~~
  **CORRECTED 2026-07-26: they did not.** The block above is an excerpt whose
  `col(-64,-64).topsoilMm` / `.subsoilMm` lines were dropped; the harness emits a
  column's field mismatches before that column's cells, so they were there. This
  single inferred sentence is what made the fault look confined to
  `VoxelizeMain`, and it produced the two wrong diagnoses that followed.
- **Cells differ**, material ids only, and in a consistent direction: the DXIL
  build's soil column sits one layer shallower than the CPU's. What the CPU calls
  rock, it calls subsoil; what the CPU calls subsoil, it calls the *surface*
  biome material.
- **The mesher is innocent** — quad decode matches exactly (20,544 vertices); the
  2-quad count difference is downstream of the cell mismatch.

~~So the fault is isolated to **`VoxelizeMain` as compiled by Unreal**. Given that
identical source is bit-exact under DXC/SPIR-V, the prime suspect is
**floating-point contraction**: an FMA or reassociation in UE's HLSL compilation
flipping a `<` at a layer boundary by one ULP.~~ **Both sentences are wrong.**
`worldgen.ush` contains no floating-point arithmetic to contract, and the two
sides of the comparison were different worldgen versions — see the resolution at
the top.

### ~~Next step~~ — superseded, and unbuildable as written

~~Compare the shader compilation flags UE uses for these kernels against the
standalone DXC invocation in `voxel-core/bench`.~~ Done, and they are not the
cause. For the record, since it is the obvious next reach: UE 5.8's D3D12 backend
maps only `PreferFlowControl`, `AvoidFlowControl` and `WarningsAsErrors`
(`D3DShaderCompiler.cpp:52-61`) and returns 0 for everything else, so
"force strict IEEE for `VoxelizeMain`" could not have been written at all. The
two real flag deltas — `cs_6_0` vs `cs_6_6`/`6_8`, and the explicit `cbuffer` vs
loose `$Globals` scalars — were both checked in Wave C and are both innocent:
DXC emits `$Globals` for the `VXC_UE` variant at byte-identical offsets to the
bench's `cbuffer` (`BrickZMin` at offset 44, 56 bytes, verified by disassembling
both), and the `cs_6_6` DXIL leg is now bit-exact over 393,216 cells.

### On the recorded digest — deliberately NOT updated

Four files quote `f3c48a4df3e20e9a` (`gpu-streaming-plan.md:63-64`,
`streaming-handoff.md:12,120`, `gpu-g2-draw-path.md:115`,
`voxel-core/shaders/prebuilt/README.md:378`). The current **bench** value is
`6e893ab3679a8c81` and it is green, so that one is a legitimate re-baseline
whenever someone wants it. The **Unreal** value `046b4a9f9c5e49b7` must not be
recorded as a baseline at all: it is the output of a build that disagrees with
the CPU, and blessing it would turn a loud failure into a silent one.

### ~~Related but separate~~: a stale prebuilt library — **THIS WAS THE CAUSE**

`build/voxel-core-msvc/voxelcore.lib` was built at 15:37 against an
`amplifier.cpp` last modified at 19:40, so UE builds on main were linking pre-v8
CPU worldgen. That is a real problem in its own right and gives the
`VoxelEarth.Build.cs` hardcoded-lib item below teeth. ~~It is **not** the cause
here — rebuilding voxel-core from scratch and relinking reproduces the identical
in-engine failure.~~

**Corrected 2026-07-26.** This paragraph had the answer in its first sentence and
then dismissed it. The dismissal rests on a claimed from-scratch rebuild that
does not reproduce today and left no transcript in the tree; every run since
`voxelcore.lib` was actually rebuilt (02:00) and the editor relinked (03:00) has
passed. The reconstruction at the top of this entry then confirms the direction
numerically. **The lesson worth keeping: a stale static library is not a
"separate" problem from a determinism-gate failure — it is one of its two most
likely causes, and it is the cheaper one to eliminate.** Eliminate it by rebuild,
never by argument.

### Consequences while the Unreal leg was red — now cleared

- ~~GPU-meshed terrain would differ from CPU-meshed terrain **in-engine**, so this
  blocks the GPU meshing programme (6b) on correctness.~~ Unblocked: Wave D's
  correctness precondition is met.
- **The NVIDIA leg (Wave C2) is now unblocked and still owed.** It has never been
  run; the cross-vendor claim rests on one AMD RX 7800 XT.
  `tools/run-nvidia-digest.sh` exists for a rented box. Not runnable from this
  machine — there is no NVIDIA GPU on it.
- **The min-spec-proxy M1 gate re-run (Wave C3) is still owed**, deliberately not
  attempted in Wave C: it is a 60 s frame-time measurement and this box was
  running three other build/editor agents concurrently. Ground rule 1 exists
  because numbers taken like that have already been retracted once. It also wants
  to land after Wave A's cull work, which changes what it measures.

---

## 6b. GPU streaming (ADR-0006), after G0-G5 landed

> **SUPERSEDED 2026-07-28 — see section 0.** Written before Waves S0-S4 and the
> draw-path work. Its central open question ("does the pool make frames faster?
> unmeasured") is now answered: p50 30.59 -> 15.2 ms at 2K and hitch frames
> 3,747 -> ~11 (`docs/measurements/drawpath-2k-2026-07-28.txt`). The fixed-camera
> harness it asks for was also built: `tools/voxel-run-flight-leg.ps1` plus the
> `VoxelPerfRun post-warmup` line give p50/p95/max over ALL frames. Kept below
> for the historical reasoning.

**Does the pool actually make frames faster? Unmeasured, and the harness cannot
currently say.** G5 flipped `voxel.Stream.GPU` on by default. The parity case is
solid (17.4% -> 4.3% of pixels differing, against a measured 1.1% noise floor)
and the mechanism is verified as a count (9,822 chunks / 8,813,242 quads as ONE
primitive and ONE draw, against 9,822 primitives). **The frame-time claim is
not** — an earlier p95/hitch table was retracted after twelve legs showed the
ranges overlapping, and a follow-up designed to remove streaming noise (anchor
stationary, cascade fully settled) still saw the component path render the
identical scene at 43 fps and 103 fps in two runs. Leading suspect is camera
orientation: the anchor is a position, and the pawn's yaw/pitch are neither
pinned nor logged. **Unblocks:** a fixed-camera perf harness that also LOGS the
pose. Start from `-VoxelFloodTest` / `Tools/capture_terrain_shots.ps1`. Until
then make no frame-time claim in either direction, including a negative one.
Full write-up in `docs/streaming-handoff.md`.

**GI volume steps 3-5 — LANDED (Wave B, 2026-07-26), with three corrections.** Steps 0-2 had landed: the pooled path feeds the light field,
the volume is sampled per pixel, and the encode matches the CPU sampler at
**0.000 mean error** (with a deliberate half-cell-shifted control to prove the
harness is not comparing a thing against itself). Steps 3-5 are now done; the
full write-up with every number and how it was measured is the Wave B section of
`docs/gpu-waves-plan.md`. The three things worth carrying here:

- **(3) Scheme A is BUILT and measured - the recorded bar was never passed.**
  `gpu-gi-volume-design.md:100-103` asks for an **RMS** under ~8/255. The figures
  on record (5.950, 6.165) are **mean-abs** compared against an RMS threshold.
  Measured RMS at the settled field is **20.4-21.1**, i.e. Scheme B misses the
  actual bar by **2.6x** and always did. The distribution is bimodal (p50 ~ 0.02,
  p95 ~ 52, max 105) because Scheme B stores the four horizontal directions as
  their mean: exact where they agree, worst where they disagree - a side face
  beside a vertical occluder, i.e. most of a cave wall. The two transcripts that
  disagreed in the tree are also reconciled: two legs on one build agree to
  +/-0.17 bytes, so the harness is precise and the figure tracks how settled the
  field was (1,947 vs 2,212 resident bricks). Quote the brick count beside the
  error or the number means nothing.
  Section 2's **"two samples"** cost for Scheme A **does not apply to this mesh**.
  Scheme A stores (+X,+Y,+Z,v) and (-X,-Y,-Z,v); section 2 itself establishes that
  every greedy-mesh normal is axis-aligned, so a face needs exactly **one** of the
  two textures. Scheme A costs memory and nothing else. And the configuration that
  settles it is missing from the sizing table: **Scheme A at N=192 is 56.6 MB,
  less VRAM and less RAM than Scheme B at N=256 (67.1 MB), which the design
  already recommends** - exact walls everywhere, at the price of 13 m of reach.
  **Measured after the switch**, same harness/scene/sample count: horizontal
  mean, RMS, p95, p99 and max all **0.000**, fraction over the bar **0.0000**,
  with the negative control still at 1.525 mean / 58.8 max so the zeros are
  load-bearing rather than a harness comparing a thing against itself. Memory
  54.0 MiB at Dim 192 - the design table's "56.6 MB" is the same quantity in
  decimal, not a discrepancy.
- **(5) The retirable cost does not exist, and this is the third statement of
  this item.** The roadmap said "stop re-meshing to refresh lighting on the
  pooled path"; `gpu-waves-plan.md` corrected that to "the component path's 5×5×5
  re-shade plus the quad subdivision". Both are wrong in the same direction:
  under `voxel.Stream.GPU 1` no level-0 `UVoxelChunkComponent` is ever
  constructed, and GI is level-0-only, so **both costs are already exactly zero
  on the path that has a volume to sample**. What was real and is now fixed:
  solved pooled bricks were still being pushed onto `RefreshQueue` to be popped
  and discarded, which is what forced step 0's 8× pop cap. **Wave B's prize is a
  capability, not a saving** — on the pooled renderer, baked per-vertex GI does
  not exist at all.
- **`voxel.GI.Volume 1` has no consumer under `voxel.Stream.GPU 0`,** which is
  the default. Only the pooled vertex factory samples the volume, so shipping the
  volume on is **coupled to Wave A's outcome**, not independent of it - the
  execution plan lists A and B as disjoint, which is true of their FILES and not
  of their shipping. The useful consequence: **GI can ship today without either**
  - the component path's CPU bake already works, so `voxel.GI.Enabled 1` with
  `voxel.GI.Volume 0` is a shipping configuration now, and that is what Wave B
  recommends. The volume is correct-and-off, waiting on a renderer default.
  Making the COMPONENT path sample the volume was considered and REJECTED: it
  needs a material-graph change (the one thing the design was built to avoid), a
  UVolumeTexture wrapper, per-chunk custom primitive data rewritten on every
  re-centre, and it would make the material graph a THIRD copy of a shade formula
  whose existing two copies had already drifted in four places.

- **NEW AND NOT ROOT-CAUSED: the light field is effectively empty under motion.**
  2,212 resident bricks settled and stationary; **0-12** for the whole of a 90 s
  `-VoxelPerfFlight=surface` run at ~20 m/s, with `pendingVox=0(+0 pooled)`
  throughout while streaming loaded normally around it (R0 loaded=3131). A player
  is moving most of the time, so if this holds, voxel GI is largely absent in
  play and every screenshot of the feature - all settled and stationary by house
  style - has measured the one case where it works. It also blocks verifying the
  volume's re-centring on the scripted flight, which is how it was found.
  Candidates not yet separated: the build-radius rejection in the voxelize drain,
  eviction outrunning the 16-chunk-per-frame budget, or the ingest hook not
  firing for most pooled applies. **The two cheap legs that split it:** brick
  count during the flight, and ~5 s after coming to rest. Recovering on stop
  means throughput/priority; not recovering means bricks are never requested -
  the same fork as the owner's ring-gap symptom.

Closed by the same wave: the X-run merge on a dig's contiguous neighbourhood and
zero-on-revoxelize / zero-on-evict now have a harness rather than an argument
(`voxel.GI.VolumeDigTest`), `voxel.GI.Volume`'s "read per frame" claim is true
rather than aspirational, and the volume texture is no longer allocated for
sessions that never enable GI (it was a `TGlobalResource` built in `InitRHI` —
67 MB at the recommended shipping size, charged to everyone).

**Per-chunk debug tints — the last G4 item, and the only one that still needs the
material asset.** Storage is already solved (`ChunkParams.w` is free). The route
is a `float4` `TexCoords` interpolant with the tint packed in `.zw`. **The
decision that makes it safe:** encode identity as ZERO, not white — the component
path supplies only a `float2` texture coordinate, so `TexCoord0.zw` arrives as
zero there regardless of the graph, and a naive unpack treating 0 as black would
render every component-path chunk black the moment the material is regenerated.
Debug-only, so its absence costs nothing in play.

**CORRECTED 2026-07-26 — identity-as-zero is unsafe, for the opposite reason.**
Superseded text kept above, per §6a's convention. A material asking for texture
coordinate 1 gets **texture coordinate 0** on the component path, not zero:
`FLocalVertexFactory` clamps the request to the mesh's UV count and clamping
duplicates (`LocalVertexFactory.ush:729-730`, `:737`), and the mesh has one UV
set because `VoxelChunkComponent.cpp:634` takes `InitFromDynamicVertex`'s default
`NumTexCoords = 1`. The value is the world-planar UV wrapped to 32 m, so ±32 of
position-dependent garbage would have been multiplied into the **default**
renderer's BaseColor. The path that does deliver zero is the pooled one. Measured
with a probe material adding `EmissiveColor = abs(TexCoord1) * 0.05`: 30.91% of
pixels differing at >8/255 on the component path against a 3.58% same-run floor,
nothing on the pooled path. **Corrected encoding:** a sentinel range — both paths
`fmod` UVs to a 32 m period, so nothing can leave (−32, 32); store `tint + 1000`
and treat anything under ~100 as identity, which is correct on both paths with
one graph and no switch node. Full write-up in `docs/gpu-waves-plan.md` Wave E.

**`voxel.Stream.AdmissionBandSkip` is off, and should stay off** until two things
are checked: its edit veto uses `EditedFootprintMinZ` where the dispatch site
uses `ChunkHasEditedBrick`, and the argument that they agree is reasoning rather
than measurement; and frame rate collapsed with it on (279 -> 89 ticks/5 s) for
reasons never explained. It reaches ~4% of the waste it was aimed at anyway (89
skips against 2,186), so there is little to gain.

**R0 to 128 m is now unblocked** — `RingPresets` became a runtime accessor
(`GetRingPresets()`), overridable via `-VoxelRingInnerMeters=` /
`-VoxelRingOuterMeters=`. Moving R0 itself is still an open call, and the
`+9.2%` resident-chunk cost of the seam-padding fix is the thing to weigh it
against.

**Ring cross-fade: do not build it for the pooled path.** Re-tested after the
seam fix gave the annuli their overlap band, and it still produces see-through
patches at ring boundaries. The G0 checklist listed it first; it is the one item
on that list that should not be built. Reasoning and the likely root cause (both
rings fading simultaneously across the shared band rather than crossing over) in
`docs/gpu-g4-parity-plan.md`.

---

## 7. Measured and CLOSED — do not re-litigate

Recorded so these are not re-attempted. Each was measured, not argued.

**The ~600 chunks/s pooled plateau (2026-07-27).** CLOSED at 797.0 chunks/s with
converged **holes = 0** — the first time the pooled arm completed the world at
the adopted 128 m / 4 km cascade. Cause was the per-chunk `PushUpdatesToProxy`
(98–99% of per-apply cost, rising to 2.1 ms/apply under flight), which backed up
the result queue until 42% of drained results were discarded as stale. Fixed by
batching publication once per tick, plus an unload budget raised from a value
sized for the old throughput. Full ladder:
`docs/measurements/s1-close-2026-07-27.txt`.

**"Results are not ARRIVING" as a reading of the plateau.** FALSE, and it was an
artefact of `LastAppliedFrac` dividing by the count cap while the drain loop
breaks on a wall clock. `queueEmpty` was 0 in every streaming window measured —
results arrived faster than they could be applied. Do not re-derive producer-side
work from that sentence.

**T1-3, the per-apply `Amplifier::column` / column-keyed params cache.** STRUCK.
`SampleChunkParamsForPool` measures 0.002–0.004 ms per apply — **0.2% of an
apply** — at every point in every leg, against `poolAdd`'s 98–99%. §1c names it
as an aggravator and it is not one. Caching it removes nothing.

**T3-6 idle pool defrag, as a response to the batching-era allocation failures.**
NOT NEEDED. 16,903 free runs and a 72× collapse in largest-free-run at equal
total free looks exactly like first-fit pathology; it was residency ballooning
because `MaxUnloadsPerFrame` was still 24, a value sized for a 260 chunks/s
pipeline. Raising it took `allocFail` 26,763 → 0 and holes 257 → 0. (Defrag may
still be wanted for other reasons; it is not the fix for this.)

**`MaxAppliesPerFrame` at 192 as a regression.** WRONG, and recorded here
because it was nearly entered as fact. A leg measuring 749 chunks/s / 179 holes
had shared the box with a second editor for 86 s; re-run alone on the same
binary it gave 1,033-1,048 with holes 0, a +30% win over 794. The default is now
192. A contended leg looks exactly like a slow configuration, and a plausible
mechanism had already been written to explain it.

**Handle recycling as a THROUGHPUT lever.** `Allocations` is append-only and
`BuildChunkRuns` walks it, giving a 3.72× walk:emit ratio by end of flight — but
cost tracks `emit`, not `walk`. Cutting the walk 45% bought 2%; `BuildChunkRuns`
is dominated by `Runs.Sort()` over live runs. Keep the recycling as an
unboundedness fix (`allocsEver` −60%, and it grows all session otherwise); do not
expect chunks/s from it.

**Sea level as a design lever.** Not a generation input at all: z=0 is inherited
from ETOPO's datum and hardcoded downstream (tile format, `biome.h`'s coastal
band, `caves.h`'s implicit ocean, the water system). Lowering it does not help
regardless — inland reach is *exactly* 123 km from 0 m through −500 m, because
the new coastline runs parallel to the old one, so the apron widens and no
interior deepens. It also destroys beaches (2.3% → 0.2% at −100 m).

**Seed selection, for continents or deserts.** A seed picks a realization, not a
process. The default sketch is Perlin FBm quantile-matched to Earth's histogram,
and Perlin is isotropic, so every seed is an archipelago: inland reach 69–92 km
across four seeds against a 246 km measurement ceiling.

**`frequency_mult[0]` for a habitable continental interior.** It does enlarge
landmasses (inland reach 123 → 192 km, largest landmass 197k → 315k km²) but the
interiors are not usable: elevation climbs monotonically inland to a mean of
2240 m at 100–200 km, 66% above the treeline, while precipitation barely moves.
The model learned Earth's hypsometry, where large landmasses have high interiors.
It buys **alpine plateau, not continental interior.** Leave it at its default;
use conditioning instead.

**`kBiomeTreelineBaseMm` (900 m at 0 °C).** Low-sensitivity and visually
undecidable: a 4× change moves the alpine share only 48.6% → 35.0%, and
in-engine captures at 300/900/1800 m are indistinguishable at both low ground
(below treeline everywhere) and summit (above it everywhere). It only moves a
boundary on mid-elevation slopes. Left at 900 m, which is physically defensible.
Revisit only if the world reads too bare in normal play.

---

## 8. ENVIRONMENT ASSETS (asset-forge) — added 2026-08-11

> **LARGELY SUPERSEDED BY §10 — reviewed 2026-08-19. Read §10 first.** The
> premise here ("forty-two species, none of them in the world") is two programmes
> out of date: **828 species** are authored, the engine composes them into terrain
> chunks on both the CPU and GPU paths, and the 2026-08-17/19 programme placed,
> censused and mapped them across every land biome. What survives is the
> asset-forge measurement discipline — the rock findings and the seven
> measurement traps at the end of this section, which are still the rules for
> judging a shape. Items found closed by the review are struck below.

Everything below is either the last mile of getting assets into the world or a
decision about who owns a number. Context: `asset-forge/README.md`,
`docs/tree-asset-generator-plan.md`.

~~**The sequenced plan lives in `docs/asset-forge-plan.md`**~~ — that plan's five
phases are done or overtaken, and it now carries a banner saying so. The current
forward list is §10.

**The shader palette — DONE.** *(Table 2026-08-11; the wiring landed with the
asset programme — `VoxelQuadVertexFactory.ush:874` carries palette RGB on
`TexCoords[3]/[4]` with `isAsset`, and the material graph does the
`lerp(biomeAlbedo, paletteRGB, isAsset)` recommended below. Kept for the
reasoning.)* ADR-0008 made
`vxc::kMaterialPalette` the one definition of what a material looks like.
asset-forge already read a generated copy; the renderer read nothing, so what a
designer approved in the forge and what the game drew were two different
answers.

Done, and verifiable without an editor:
`ue-project/Shaders/VoxelMaterialPalette.ush` is **generated** from the header by
`ue-project/Tools/gen_material_palette_ush.py` — all 26 materials, three face
classes each, sRGB converted to linear once at generation, plus the ADR's
evaluation (`VoxelMaterialColor(mat, faceClass, voxel)`: voxel-keyed hash tint
and the trilinear patch term, no lighting). `tools/compile-shaders.ps1` now runs
two checks on it: `--check` fails if the .ush has drifted from the header, and
DXC compiles `VoxelMaterialPaletteTest.usf` to **both** ADR-0001 targets. Both
guards were proved to fire — a perturbed colour is named with its line, and
moving `MAT_BARK_PALE` back up among the woods reproduces the historical
"out of step with the enum at index 19". Neither needs the editor, which is the
point: one editor per box is a hard rule, so a check that needs it is a check
nobody runs.

**What is left is the wiring, and it needs a decision as well as the box.** The
hook is named in `VoxelQuadVertexFactory.ush` (the `bMarker` branch: "the
smallest possible instance of the thing biomes need next: per-material
appearance in the pooled renderer... the id survives here"). Two constraints
meet there. `VertexColor` is full — R surface flag, G ambient occlusion, B and A
climate — and `TexCoords[1]/[2]` are already spoken for by the local-lights plan
(`docs/sky-and-local-light-plan.md`), so the palette needs `TexCoords[3]/[4]` at
pixel rate and a `M_VoxelTerrain` graph change to read them. The graph is a
generated artifact (`Tools/create_voxel_material.py`), so that edit is code, but
running it is a `-run=pythonscript` commandlet and therefore editor-bound.
**Recommended shape:** feed the palette only where the biome graph has no
answer — `BaseColor = lerp(biomeAlbedo, paletteRGB, isAsset)` — which leaves
terrain's climate-driven colour untouched and is inert until assets are actually
in the world. Do NOT replace the biome path wholesale as a first step; it cannot
be verified in the same motion and it is the only appearance path that currently
works.

**Also found — SINCE FIXED.** `ue-project/Tools/terrain_palette.py` *was* a
**second palette**, 16 entries, stopping at `MAT_WATERMARK`, and its own header
called it "single source of truth". Its RGB column is now **generated** from
`materialpalette.h` by `gen_material_palette_ush.py` and checked by
`compile-shaders.ps1`; the file owns only the UE-side `BIOME_TINT` policy and its
header now says so. Original entry kept for the reasoning. The ten asset materials (bark, heartwood, deadwood, six leaf types,
pale bark) have no appearance on the UE side at all. It is not urgent only
because no asset is in the world yet. When the wiring above lands, its RGB
column should be generated from the header too, leaving it to own just the
UE-side `BIOME_TINT` policy, which genuinely is not the engine's business.

~~**UE wiring for asset streaming. BLOCKED on the editor box.**~~ **DONE** —
built and verified in-editor across the 2026-08-15/19 programme: composition at
every LOD level on both the CPU and GPU paths, plus exact per-footprint
admission. Original entry:
The voxel-core half is designed and largely written — `assetplacement.h` gives a
provable upper bound on how high an asset reaches, which is the thing that lets
the streaming admission path keep skipping chunks it can prove empty. What is
left is the UE module: getting a baked asset into the volume the marcher reads,
and confirming a crown lands in the chunks the bound said it would. Neither half
of that can be *verified* without the editor, and one editor per box is a hard
rule here — two capture sessions on one machine killed each other's frames for
hours and read exactly like a slow configuration. Another session holds it.
**Unblocks:** the box, nothing else.

**Placement: an asset-forge panel, or `assetplacement.h`? — DECIDED, and the
answer was compile-the-spec.** asset-forge authors the intent (per-kind ×
per-biome densities, explicit allowlists, named rule overrides, in the web app's
Placement panel); `tools/export_manifest.py` compiles it into the VXM2 manifest;
the engine reads that as its `AssetLayer` numbers. One number, two readers — the
panel-vs-header split never grew. See `docs/placement-spec-schema.md`. Original
entry: The one thing that still matters while it waits: **neither side may
grow its own version of the other's numbers in the meantime.** A spacing
authored in a panel and a spacing typed into an `AssetLayer` is the Appendix's
failure with a new subject, and the deferral makes that more likely, not less,
because both halves stay half-built.

Right now it is neither, twice. asset-forge writes a `placement` group per
species — `abundance`, `spacing_m`, `cluster`, slope and elevation gates, biome
weights — that **no code reads**. voxel-core has `assetplacement.h`, which does
the per-chunk deterministic scatter and the bound, and gets its numbers from an
`AssetLayer` struct that nothing fills in from a spec. The two are not
alternatives so much as two halves that have never been introduced: the spec is
a designer's *intent* for a species, the header is the engine's *mechanism*, and
the open question is only which one owns each number and how the intent reaches
the mechanism. **Decide before either grows its own version of the other's
numbers** — a spacing authored in a panel and a spacing typed into an
`AssetLayer` is the Appendix's failure with a new subject. The cheap answer is
to compile the spec's placement group into `AssetLayer` at bake time so there is
one number with two readers; it is written here as a decision because choosing
the panel instead is a legitimate call and it changes what gets built.

**Review the 16 legacy rock species against the newer mechanisms.** The rock
generator gained real geology on 2026-08-10 — bedding, joint sets, columns,
corestones, veins, clasts, tafoni, flutes, pans, exfoliation, arches and
caprock — and the eighteen species authored during and after that wave use it.
The sixteen that predate it do not: ten of them (`granite-boulder`,
`standing-stone`, `summit-tor`, `glacial-erratic`, `river-cobble`,
`alpine-scree`, `limestone-slab`, `cliff-fall-block`, `desert-mesa-block`,
`mossy-forest-boulder`) run **no** mechanism at all, and six more
(`basalt-colonnade`, `exfoliating-dome`, `fractured-outcrop`,
`jointed-granite-tor`, `banded-sandstone-ledge`, `tafoni-sandstone`) run one or
two of them. That is not automatically wrong — a river cobble genuinely has no
joints to show — but every one of those sixteen was tuned around a weathering
pass that was removing **20 voxels from a stone of 90,000**, so their shapes
were won with `rough` and cut planes standing in for erosion that never ran.
Worth a pass to see which are now saying the wrong thing about their own rock
type. **Judge it with `tools/waistprobe.py`, NOT with `rockmech.py`** — and not
with voxel counts. `rock.build` measures the finished stone and rescales it to
the authored size, so a mechanism that plainly works lands within 0.1% of where
it started and reports "changes nothing"; five did exactly that.

That refit also breaks `rockmech.py` in the opposite direction, which was found
on 2026-08-11 and is the more dangerous half. Because the stone is rescaled, any
change in MASS reappears as a shift of the whole surface, so the divergence
metric reads large whether or not the mechanism did anything shaped. Measured
with the erosion scaling deliberately disabled, `caprock` still scored 43.5% and
`notch` 22.2% at 9 m. **`rockmech.py` showing "no dead sliders" was never
evidence that the mechanisms worked**, and it is what let a size-blind `erode` —
removing a constant ~4.75 voxel layers whether the stone was 3 m or 13 m, so
14.3% of a boulder and 3.5% of a hero — survive a full pass of the harness.
What discriminates is a measurement the refit cannot launder: retreat depth in
voxels against known size, and `waistprobe.py`'s cross-section-against-height
with its overhang number and its SEVERED check.

~~**Rocks have no allocation guard.**~~ **DONE 2026-08-11.** An over-ambitious
`rock.size_m` used to surface as a bare numpy `MemoryError` from whichever
temporary happened to be unlucky, which points at a line of arithmetic instead
of at the spec that caused it. `forge/rock.py` now sizes the working set before
allocating anything — six whole-grid float32 arrays are live at the peak — and
raises a `MemoryError` naming the spec, the resolution, the grid it would need
and the ceiling it broke (`MAX_WORKING_GB`, 20 GB; a 90 m hero at 10 cm sits at
about 12).

### The measurement traps in this repo, so the next person does not rediscover them

Every one of these was hit, twice in some cases, and each cost a round of
editing correct code.

1. **Strip colour before judging shape.** `MAT_ROCK` and `MAT_BEDROCK` render
   near-black on the dark contact sheet, so only the lit top faces show and a
   round boulder reads as a flat wedge. `tools/shapecheck.py` forces every solid
   voxel to one bright material; use it before believing a shape is wrong.
2. **A pass that runs is not a pass that does anything.** The rock weathering
   pass removed 0.44% of a surface while its slider said 30%, for the entire
   life of the module, because blurred uniform noise is a narrow bell and
   min/max rescaling does not flatten it. Nothing errored. Always measure what
   a pass removed, not that it ran.
3. **Voxel count is not a valid A/B for rocks.** `rock.build` fits the finished
   stone to the authored size, so anything that changes erosion gets scaled back
   out. Use shape divergence — crop both to their occupied box, `|A xor B| /
   |A or B|` — which is what `tools/rockmech.py` reports.
4. **Count the thing you are changing.** Tuning foliage by leaf-voxel count
   hides the whole question: the acacia's canopy went from 96 clumps to 271 and
   *down* in leaf voxels, which is precisely the change that was wanted. Clump
   count and clump radius are the measurement; voxels are a side effect.
5. **One seed is not a comparison.** `variation.amount` is doing its job, so the
   same spec at three seeds is 7.0–9.8 m tall. Any A/B on a single seed is
   reading that spread. Render the same seeds on both sides, and keep an
   untouched species in the frame as a control — that is what proves the change
   is yours and not a baseline someone else moved under you.
6. **A check that fires on the normal case teaches people to ignore checks.**
   The 256-voxel size flag fired on ten of forty-two species and made "keep all
   clean" skip every large tree. It was removed and the export was made to split
   instead.
7. **Gate on authored values, never on an internal correction factor.** A rock
   stage gated on the *scaled* size worked once and then switched itself off for
   every attempt after the fitting loop corrected downward.

---

## Appendix: the recurring failure mode

Three separate bugs this week were the same shape — **two copies of one
calibration drifting apart**:

* `biome.h`'s thresholds vs the encoding the tiles actually used (worldgen v8);
* `VoxelClimateProbe`'s remap window vs `gen_terrain_textures.py`'s LUT axes;
* `VoxelClimateProbe`'s tile loader vs `VoxelWorldSubsystem`'s (PR #113) — which
  had drifted in *two* independent ways and was self-reporting in the startup
  log the entire time.

The fixes that stuck were the ones that removed the second copy rather than
re-synchronising it: thresholds derived from `climate.h` at compile time, the
seed default taken from the subsystem rather than re-typed, `world_map.py`
parsing `biome.h` instead of hardcoding it. **Prefer deriving over copying, and
when a copy is genuinely unavoidable, make it fail loudly rather than quietly.**

## W3 rivers — PARKED 2026-07-29, blocked on the terrain pipeline

Matt's call, and the evidence supports it: **do not resume river water until
worldgen actually carves riverbeds and generates basins.**

**What is built and merged (all inert):**
- `channel.h`/`channel.cpp` — riverbed geometry from discharge. Verified
  standalone via `vxc_riverprobe`: a real river from +130.6 m to −99.9 m,
  monotone every step, 0 gaps across 571,500 centreline columns, 0.20% bank
  leakage against a <1% guard. **Consumed by nothing.**
- `rivercouple.h`/`.cpp` — discharge → CA water, sustained flux → channel
  promotion, ocean as sink. `kDivertChannel` is no longer a stub. Behind
  `voxel.Water.Rivers`, **default false**.

**Why it is parked.** Run in engine, the coupling works exactly as designed —
443 segments, 9,894,505 fill units injected, ledger exact to the unit
(`storage + outlets + toCA == injected`), 3.69M units delivered into the CA.
And it looks wrong: disconnected puddles scattered along the drainage lines
rather than continuous rivers. The cause is not the coupling. **There are no
riverbeds.** `ChannelField` is referenced nowhere in `amplifier.cpp`,
`world.h` or `VoxelWorldSubsystem.cpp`, so discharge lands on unmodified
hillside and pools wherever the ground happens to dip.

The carving pass avoided a `kWorldGenVersion` bump by not wiring itself in.
That was reported as a clean win; it was also precisely why the feature does
not work.

**What unblocks it, in order** *(reviewed 2026-08-19)*:
1. The amplifier and bake pipeline settle (the other session owns this).
2. Wire `ChannelField` into worldgen output. **Costs a `kWorldGenVersion` bump
   (now 28, so 28→29), golden re-pins, and an HLSL mirror change in lockstep** —
   there is no free route, because "free" was exactly what not-wiring-it bought.
   *Still true: `ChannelField` is referenced only by `channel.h`/`channel.cpp`.*
3. ~~Basin detection → ponds and lakes.~~ **DONE, by another route.** The water
   re-architecture (2026-08-09 onward) ships the baked basin registry,
   `FBasinLedger`, lake sheets that rise/spill/drain, and a GPU PBF solver for
   near-field water; the baked river plane was retired from the near-field draw
   in Phase 5 (`7925cb6`). **Re-read `docs/water-architecture.md` before resuming
   W3 at all** — it may have removed the reason this is parked, or the need for
   it.

**Collision warning.** `origin/claude/erosion-v7` took the version-bump route
for drainage carving — 341 lines into `amplifier.cpp`, goldens re-pinned across
five files — and is **permanently unmergeable**. The other session touched
`amplifier.cpp` ×8, `worldgen.ush` ×5 and the **binary** `.spv` prebuilts ×4 in
24 hours; binary conflicts do not merge. Coordinate before entering worldgen.

**Also parked:** the `swe.h` §5 lateral-spill gap (an SWE-owned pool cannot
spill into a lower CA-owned neighbour) and ADR-0007's depth term. Both need
`kSweVersion` 1→2 and a re-pin of `0x61523E585CF7B782`; ADR-0007 argues for
deciding them together.

---

## 9. WATER BUGS FROM THE 2026-08-13 PLAYTEST

Reported by the owner while testing wind waves, ripples and throwables. Neither
is diagnosed; both are written down while the observation is fresh, with what is
already known that bears on them. **Both still open — confirmed 2026-08-19; no
commit since the playtest touches either.**

### 9.1 P1 — Digging a voxel UNDER water crashes the frame rate and unloads nearby water

**What was seen.** A single left click destroying one voxel beneath the water
surface "crashed game performance and slowed things down to a crawl", and at the
same time caused "a shader rendering issue with water such that near water around
the edit just unloaded or became transparent".

**Why it is probably one bug and not two.** An edit under water invalidates two
things at once: the terrain mesh for the affected chunk, and the water that was
resting on it. The near-field water and the lake sheet are separate draw paths
(docs/water-architecture.md), and the sheet's extent is derived from the baked
basin rather than from live geometry, so an edit can put them briefly out of
agreement. "Became transparent" is what water with no volume behind it looks
like -- consistent with the water surface surviving while whatever it was
occluding went away, or with an implicit-water brick being dropped and not
rebuilt.

**What is already known that bears on it.**
- The water CA owns mobilized bricks and `implicitFillAt` reads 0 there
  (VoxelWaterSubsystem.cpp, the ownership partition). A dig that mobilizes a
  region hands it from the implicit path to the CA, and that hand-off is the
  obvious suspect for both the stall and the disappearance.
- Edits are replayed from the edit log on load, so this is reproducible from a
  saved world rather than only live -- which makes it capturable at a pinned
  pose.
- The stall being immediate and severe points at synchronous work on the game
  thread, not at streaming: streaming shows up as a hitch that recovers.

**First moves, cheapest first.** Reproduce at a pinned pose with
`stat unit` and `voxel.Debug 1` up; watch whether DRAW or GAME time moves.
Then `voxel.Water.MeshImplicitLakes 1` (default 0) to see whether the near-field
voxel path is involved at all. Then the CA's own counters -- this project's rule
is that every stage emits a ran-flag, and the water CA has them.

### 9.2 P2 — The wave field's own crest speed has never been judged separately

The wind field's timescales were slowed 8-15x on 2026-08-13 (weather.h) after
the owner rejected the default three times. That addressed how fast the wind
CHANGES. It did not address how fast a crest TRAVELS at a fixed wind, which is
`omega0` in water_wave_graph.py and is currently 1.59 m/s for the 5 m base octave
against a real deep-water value of 2.79 m/s -- i.e. already slower than physical.
If the surface still reads as too fast with a frozen wind, the fault is not there
and the next suspect is `WindPatchDriftFrac` (0.5, so the calm/choppy patch field
sweeps at half the wind speed -- 2.5 m/s at the 5 m/s default, which crosses a
50 m view in twenty seconds).

---

## 10. ASSET PLACEMENT + ASSET FORGE — after the 2026-08-17/19 programme (merged, `aab8f54`)

54 commits. What landed is in the merge message and `docs/status.md`; this is
only what did **not**. Grouped, as this file is, by whose call each one needs.

### 10a. Needs the owner, not engineering

**CURATION HAS NEVER BEEN EXERCISED.** 828 species, all `approved` by the
grandfather clause, **zero human verdicts**. The gate ships (`354339b`), the UI
ships (Asset Library tab: approve/reject/draft plus per-seed toggles), and
`tools/library.py` reports the split. Nothing else can substitute — only the
owner can say which of a species' four baked seeds are worth placing. Best done
now that the 3D inspector shows an asset beside its placement contract.
*Cost: an evening of looking. Unblocks: a library that means something.*

**RAINFOREST AND INLAND ROCKS ARE ROSTER GAPS, NOT DENSITY BUGS.** Rainforest
carries 4 weighted tree species and measured 53.7 trees/ha — below grassland.
Inland rocks sit at 1.5–5/ha against the beach's 132. Per-biome density
deliberately did NOT touch either (`003b136`): they are starved, not saturated,
and thinning a starved biome makes it worse. The fix is authoring — weight more
species into those biomes, or generate them.

**TERRAIN DRAMA — HELD BY OWNER DECISION.** `docs/terrain-relief-measurements.md`
(`77aef42`) measured this world's 2 km wall score at **1,964 m max** (Milford
Sound is 1,683) and 10 km relief at 5,610 m — the amplitude ceiling is already
DEM-class. Two real gaps: **abundance** (world p99 10 km relief is 2,740 m
against that 5,610 max; 6 of 289 tiles exceed a 1,500 m wall; exactly ONE
sea-to-summit wall exists in the world) and **grammar** (slope p99 saturates at
50–58 degrees in every region measured — the checkpoint's own ceiling, so no
U-floors, no benches, no vertical faces). Levers can plausibly buy abundance;
only regional DEM conditioning buys grammar. `docs/dem-reference-library.md`
(`dcae423`) has 12 candidate regions with bboxes, licensing and three routes
priced. Paused by the owner until placement judgement finishes.

**BARE-ROCK-ONLY SPECIES NOW HAVE NO HOST.** v27 removed BARE_ROCK from dry land
(owner's call); species weighted only for it can never place. Re-weight toward
alpine/scree, or accept them as submarine-only.

### 10b. Blocked on something else landing

**VERIFICATION CAPTURES FOR THE LAST THREE LANDINGS.** All merged, none seen
in-world: the 192M pool default (`036552f` — every capture that night needed
`-VoxelPoolCapacityQuads`; the default should now carry the alpine vista's 173M
unaided), adaptive detail budgets (`e30bb46` — grep `VoxelDetailAssets: CONVERGED`
for time-to-first-full-ring against that night's 591–656 s, and the count of
`Hitch frame` lines must not rise), and the v28 per-biome densities in a
rendered frame rather than a census. Blocked only on the editor, which is free.

**GPU COARSE BYTE-PARITY HARNESS — WRITTEN, NEVER RUN.** `04dbdfe` documents the
exact procedure (CPU `FCoarseChunkGridSampler` plus `MeshChunkBricks` against
`RunRegionBlocking`; compare cells in dispatch columns 7..40, then quads through
`VoxelChunkQuadsIdentical`; levels 1–5, all four yaws). Byte parity is the
acceptance bar for coarse GPU composition and is currently asserted, not measured.

**PER-CLASS OVERLAYS ARE STALE.** `bake-out/biome-survey/` overlays predate the
flower/reed bank bake (`37591e0`), so they under-draw every biome by the 14–38%
that was invisible. Regenerate with `vxc_assetprobe --overlay <base>` then
`asset-forge/tools/place_overlay.py`.

### 10c. Cheap and self-contained

**`voxel-capture.ps1` DEFAULTS `-FineProviderId` TO EMPTY.** That default is the
fall-through that produced three fatal gate leaks in one night: the dir comes
from the command line, the id falls through to the ini, and the cross-product
has never existed on disk. `terrain-service/tools/resolve_fine_namespace.py`
answers it (rc 2 = nothing baked, rc 3 = ambiguous);
`docs/fine-tile-provider-identity.md` section 7 has the drop-in snippet. **Make
the dir/id pair atomic**: refuse a run that supplies one without the other.

**~35 GB OF STALE TILE NAMESPACES.** `D:/vox-wet-cache` and
`D:/voxelsim/tile-cache` hold ~20 superseded bake namespaces at 0.3–2.2 GB each.
Keep `...-b19d281fd` (bake_ver 28, current) and `...-bdcab4bed` (the previous
pair). **Scan for junctions before any recursive delete** — this repo has the
scar (`windows-junction-recursive-delete-hazard`).

**THE BIOME MAP SHOULD COME FROM `vxc_climateprobe`, NOT A SECOND REPO'S WEB
SERVICE.** `terrain-service/tools/world_map.py` needs the terrain-diffusion
explorer running (torch, diffusers, rasterio, infinite-tensor — eight dependency
clusters), produces a coarse *preview* whose percentages disagree sharply with
the engine's own census, and duplicates classification by parsing `biome.h`.
`vxc_climateprobe` already classifies every column through the engine's own
classifier off baked tiles; a colour-keyed PNG mode there would be
authoritative, service-free and unable to drift. Note the proper
`terrain-service/tools/worldmaps/` set (heightmap, biomes+provinces,
temperature, vista index) IS the standing deliverable and does work — it needs
`D:/terrain-diffusion/.venv`, which its README documents.

**FLOWER AND REED SPECIES NEED THEIR OWN CURATION PASS.** 477 banks were baked in
one command after being invisible for their entire existence — nobody has ever
looked at them.

### 10d. Larger, worth scoping

**COARSE-LEVEL GPU ASSET STAMPING IS PARTIAL.** The gather kernel ships and the
fork serves coarse chunks, but a grid too tall for span packing (SizeZ > 4095)
still falls back to the CPU, and `ValidateRegionRequest` refuses instances on any
level it does not implement.

**DETAIL ASSETS DO NOT REACT TO EDITS.** A dug-out column keeps its ground cover
until the group leaves the 256 m ring and re-resolves. Terrain-lattice assets get
this right through the overlay; cover accepting a staleness window is the v1
trade, recorded in `docs/detail-asset-rendering.md`.

**PER-BIOME WATER KIND IS CARRIED, NOT SERVED.** The schema accepts `water_kind`
per rule (the owner's "must be near FRESH water" case), but the bake-28 distance
plane is salinity-blind — it answers "how far to water", not "to what kind".
Same precedent as `water_max` before bake-28: the gate exists the day the channel
does.

**ANIMALS.** 382 species (bird 127, fish 106, quadruped 131, cetacean 18) place
correctly and are structurally excluded from rendering by owner decision — they
need animation first. `assetdetail.h`'s group scatter (herds, shoals, flocks) is
built and has no consumer. 251 of them still have no density source (the
PanTHERIA work covered mammals; birds, fish and cetaceans were blocked on a
web-search budget).

### 10e. The recurring failure mode, fourth instance

This programme lost the most hours to **an instrument measuring a world the
engine was not running** — four separate times: a probe on synthetic climate; a
veto reading an empty debug channel; the engine calling the old column binding in
five places while the probe called the new one; and a stale `vxc_gpu.exe`
reporting a *plausible* CPU/GPU divergence that was really 16-hour-old CPU code
against a current shader. The countermeasures shipped — engine-bound counters
with `--json`/`--compare`, trunk-anchor overlays, and `assetColumnChannelsAt`
living on the world object so no composition path can opt out by omission — but
the discipline is the fix: **`ls -la` the probe binary and `grep -c` the accessor
on both sides before quoting any number.** See the appendix above and
`voxelsim-instrument-must-run-the-engine-binding` in the session memory.

## 11. FRONT END (main menu + loading screen) — added 2026-08-22

Landed: a 1:1 clone of the Mira-Thal / *Voxelmark* front end as pure C++ Slate
in a new `VoxelEarthUI` module, with named saves behind CONTINUE and LOAD GAME.
See `docs/front-end-plan.md` for what to run and
`docs/adr/0009-slate-front-end-and-committed-ui-art.md` for why it is built the
way it is.

**Nothing below has been compiled or photographed.** The environment this was
written in has no UE 5.8 install, and CI cannot compile the module either
(`ue-build.yml` is gated off — 30 GB engine, 14 GB runner disk). The two lints
that DO run in CI pass. Treat the first item as blocking.

1. **Build it, then run the capture set.** `tools\voxel-ui-capture.ps1 -Shot Menu`
   and its siblings. First contact with a compiler will find things; the module
   is ~3,500 lines of Slate that has never seen one.
2. **The colour probe (R1).** `FLinearColor(FColor)` decodes sRGB and Slate
   re-encodes on output; whether `#f0c14b` survives that round trip is an
   assumption. `voxel.UI.SRGBTint` A/Bs it at the single conversion site.
   **Nothing should call this port pixel-exact until this runs.**
3. **The gate-ring measurement.** `GateMaxRing = 3` is reasoned, not measured.
   `foreach ($n in 1..5) { tools\voxel-ui-capture.ps1 -Shot GateSweep -GateRing $n -MaxHold 180 }`,
   into `docs/measurements/front-end-gate-<date>.txt`. Until then the constant's
   comment says "hypothesis" and should keep saying it.
4. **The non-regression diff.** A world capture taken through
   `-VoxelMenuAutoStart` against the same capture without the front end, at the
   documented within-session noise floor. This is the one that protects the
   archive.
5. **Font metrics (R2).** Macondo at 84 px will not lay out identically in
   Slate and Godot. Expect a few pixels in title width and vertical centring;
   `Config/DefaultVoxelUI.ini` is where to correct it without a rebuild.
6. **Two API forms a read-only audit could not settle**, both cheap to check
   the moment there is an engine to check against. `FSlateDrawElement::MakeLines`
   is passed `TArray<FVector2D>` (`SVoxelHourglass.cpp`) on the grounds that
   the float-precision overload is the newer of the two; and
   `UWorldSubsystem::DoesSupportWorldType(const EWorldType::Type)` is the
   signature both `UVoxelFrontEndSubsystem` and the pre-existing
   `UVoxelWorldSubsystem` override. If the latter has been superseded by the
   `const UWorld*` form, both still COMPILE and both silently stop being
   called -- so it is worth one look, and if it is wrong it is wrong in the
   older file too.

**Deferred, deliberately, with their seams left open:**

- **Pause menu.** The seam is `EVoxelFrontEndState::Playing`; it needs a
  `Paused` state, an Escape binding on `AVoxelEarthPlayerController`, and an
  `SVoxelPauseMenu` reusing `FVoxelUIStyle` and `SVoxelMenuButton` unchanged.
  Its SAVE dialog binds to the existing `VoxelSave::Write`. Until it lands,
  saves are created through `voxel.SaveGame`.
- **Settings screen.** Currently a placeholder panel shaped like HELP and
  CREDITS. The Godot original persists to two files (`settings.json` and
  `graphics.json`); consolidating them is worth doing at the same time.
- **Menu music.** The three source WAVs are ~124 MB and one exceeds GitHub's
  per-file limit. The handoff hook is stubbed; the contract is adopt at
  BeginLoad, fade to −40 dB over 1.5 s at hand-off.
- **Per-save seeds.** `Seed` is baked into the amplifier in `Initialize`, before
  `Impl` exists, so a save from another seed cannot be opened — it is listed as
  unloadable with the reason shown. Lifting this means extracting a
  `ConstructImplForSeed()` out of `Initialize`, which also unlocks a seed picker
  on NEW GAME.
- **The Mira-Thal branding.** The menu says VOXELMARK and its tips describe
  Roland, Lethe's Draught and the Aelorin. That was the explicit 1:1 brief; all
  of it is in `VoxelUIStrings.cpp` so re-authoring is a single-file edit.
