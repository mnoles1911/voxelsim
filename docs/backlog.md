# Backlog

One place for everything known-and-not-done. Written 2026-07-25 after the
worldgen v8 climate wave; **section 0 added 2026-07-28** after the streaming and
draw-path programme merged (PR #165).

Each item says what it is, why it matters, what it costs, and what unblocks it.
Items are grouped by **what kind of decision they need**, not by subsystem —
because the thing that stalls work here is usually "whose call is this", not
"where does the code live".

`docs/status.md` remains the chronological record of what happened. This file is
the forward-looking list. When an item lands, delete it here and write the result
there.

---

## 0. ENGINE PERFORMANCE — the current front

**Read this section first.** Everything below it predates the streaming
programme; §6b in particular is stale and now says so.

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
ms against a frame delta of +16.5 / +13.3) — the render thread blocked. The voxel
tick does rise on slow frames (+5.1 ms), which is exactly why it looked causal;
it is a passenger, because a frame where lots of terrain arrives has more
streaming work *and* more GPU work, and only the second is on the critical path.

**Shadow cascades are not involved.** `shadowGather=0` throughout and ~1.03
gathers per frame — the "pool re-gathered 4–5× for shadows" hypothesis is dead.

**The cull walks the whole pool every frame:** 62,657 runs frustum-tested per
gather, of which only ~10,300 survive. At the 45–90 ns per test recorded at
Wave G that is ~4.4 ms — roughly a third of the render thread — and it scales
with *resident* chunks, not visible ones. **This is now the best-supported single
item on the list** (see 0.1a).

### 0.1a NEW P0 — Hierarchical cull

Stop testing 62,657 boxes individually. Runs are sorted by pool offset and
streaming fills the pool in roughly spatial order, so grouping every N
consecutive runs under one bound lets a single test reject thousands. Worth up
to ~4 ms of a ~13 ms render thread, costs **no visual quality**, and is contained
entirely within the cull.

Confirm the ~4.4 ms with a direct timer first — it is derived from a per-test
figure recorded at Wave G, not re-measured.

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
but at ~7.8 triangles per pixel at 2K most survivors are behind something.
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

**Worth:** unmeasured, but the ceiling is high. If half the in-frustum geometry
is occluded, drawn quads roughly halve, and the draw path responds to geometry
volume at ~0.4 ms per million quads.

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

**S2-5 drop the CPU shadow — blocked on S2-1.** `InitPool` allocates
`PooledQuads` + `QuadChunkIds` at full capacity (12 B/quad) and
`CreateSceneProxy` copies both **whole** — at the current 80M capacity, ~960 MB
of system RAM plus a 960 MB game-thread memcpy on any render-state rebuild.
Blocked because the shadow's last writer on the GPU-only path is
`RemoveChunkInternal`'s per-quad id stamp, which IS the mechanism that hides
freed geometry. Also gates any pool growth beyond 80M.

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

- **~187 speculative dispatches per window still return zero quads.**
  `dropOvertaken=0` and `dropPoolFull=0`, so it is all zero-quad results — and
  "zero quads" covers all-**solid** as well as all-air, which is why they cluster
  at both ends of the surface band and never the middle. Feeding the D6 band back
  from speculative results made the shared buried skip fire at all (0 → 51 per
  window) but end-to-end effect was small, because speculation is bounded by
  `SpeculativeMaxInFlight` rather than by candidate supply. Further reduction
  needs an **analytic** empty test at enumeration time. Bounded cost if never
  done: an air or buried chunk parks nothing, holds no pool range, evicts nothing.
- **`voxel.Stream.VelocityLeadSec` still defaults to 0.** T4-1 is confirmed (93%
  fewer flight-phase holes, no throughput/memory/game-thread cost) but ships off
  pending an owner call. Recommended default **2.0** — the effect saturates at
  1 s, so the cheapest setting is also the best.

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

**A second pregen in an arid region.** The 25-tile launch set is uniformly wet
(660-1650 mm/yr, no arid pixels), so DESERT and SAVANNA have no real-tile
regression coverage — they are exercised only by synthetic tests and the
`biome_map` golden sweep. **Cost:** one GPU session. **Note** the seed makes this
harder than it sounds; see §5.

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

**GPU harness cannot see real tiles.** *(Still open, confirmed 2026-07-28:
`gpu_harness.cpp:1869` still takes `SyntheticTileSampler&`.)* `gpu_harness.cpp`
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

*Still open, confirmed 2026-07-28:* `test_amplifier.cpp` mentions 3750 only in
those two comments — no 3750 environment exists yet.

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
