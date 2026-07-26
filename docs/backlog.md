# Backlog

One place for everything known-and-not-done. Written 2026-07-25, after the
worldgen v8 climate wave.

Each item says what it is, why it matters, what it costs, and what unblocks it.
Items are grouped by **what kind of decision they need**, not by subsystem —
because the thing that stalls work here is usually "whose call is this", not
"where does the code live".

`docs/status.md` remains the chronological record of what happened. This file is
the forward-looking list. When an item lands, delete it here and write the result
there.

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

**GPU harness cannot see real tiles.** `gpu_harness.cpp` and
`VoxelGpuVerify.cpp` take a concrete `SyntheticTileSampler&`, not
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

**Re-run `-VoxelMatHistogram` in engine.** v8's fix is verified by the CPU-side
top-voxel census (surface materials 12.4% → 100.00%), not by the quad census
that produced the original `MAT_ROCK 15% / MAT_SUBSOIL 85%` measurement in
`VoxelClimateProbe.h`. Re-run the switch and update that comment with real quad
numbers before anyone builds an id-keyed appearance rule on it.

**`VoxelEarth.Build.cs` hardcodes `build/voxel-core-msvc/voxelcore.lib`** while
multi-config VS generators emit `Release/voxelcore.lib`, so every fresh worktree
needs a manual copy. Known since Track B2 and hit again during v8's UE build.
One line.

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

## 6a. LIVE REGRESSION: the UNREAL leg of the determinism gate is failing (the bench leg is green)

**`voxel.GPU.VerifyRegion` fails in-engine while the standalone bench passes
bit-exact, from the same HLSL source, on the same GPU, against the same CPU
reference.** The two legs of the cross-toolchain gate disagree with each other,
which is precisely the class of fault this two-leg gate exists to detect.

| leg | toolchain | result | digest |
|---|---|---|---|
| `build/voxel-core-msvc/bench/vxc_gpu.exe` | DXC -> SPIR-V -> Vulkan | **PASS**, bit-exact over 8192 columns / 393216 cells / 6668 quads | `6e893ab3679a8c81` |
| `voxel.GPU.VerifyRegion` | UE -> DXIL -> D3D12 | **FAIL** vs the CPU reference | `046b4a9f9c5e49b7` |

Both on the same AMD Radeon RX 7800 XT.

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

- **Columns match** (all 4,096) — `ColumnMain` agrees under both toolchains.
- **Cells differ**, material ids only, and in a consistent direction: the DXIL
  build's soil column sits one layer shallower than the CPU's. What the CPU calls
  rock, it calls subsoil; what the CPU calls subsoil, it calls the *surface*
  biome material.
- **The mesher is innocent** — quad decode matches exactly (20,544 vertices); the
  2-quad count difference is downstream of the cell mismatch.

So the fault is isolated to **`VoxelizeMain` as compiled by Unreal**. Given that
identical source is bit-exact under DXC/SPIR-V, the prime suspect is
**floating-point contraction**: an FMA or reassociation in UE's HLSL compilation
flipping a `<` at a layer boundary by one ULP. Layer-boundary comparisons are
exactly where a one-ULP difference becomes a whole different material.

### Next step

Compare the shader compilation flags UE uses for these kernels against the
standalone DXC invocation in `voxel-core/bench` — specifically anything affecting
FMA contraction, fast math, or IEEE strictness. If contraction is the cause, the
fix is to force strict IEEE / disable contraction for `VoxelizeMain`, not to
change the kernel maths.

### On the recorded digest — deliberately NOT updated

Four files quote `f3c48a4df3e20e9a` (`gpu-streaming-plan.md:63-64`,
`streaming-handoff.md:12,120`, `gpu-g2-draw-path.md:115`,
`voxel-core/shaders/prebuilt/README.md:378`). The current **bench** value is
`6e893ab3679a8c81` and it is green, so that one is a legitimate re-baseline
whenever someone wants it. The **Unreal** value `046b4a9f9c5e49b7` must not be
recorded as a baseline at all: it is the output of a build that disagrees with
the CPU, and blessing it would turn a loud failure into a silent one.

### Related but separate: a stale prebuilt library

`build/voxel-core-msvc/voxelcore.lib` was built at 15:37 against an
`amplifier.cpp` last modified at 19:40, so UE builds on main were linking pre-v8
CPU worldgen. That is a real problem in its own right and gives the
`VoxelEarth.Build.cs` hardcoded-lib item below teeth. It is **not** the cause
here — rebuilding voxel-core from scratch and relinking reproduces the identical
in-engine failure.

### Consequences while the Unreal leg is red

- GPU-meshed terrain would differ from CPU-meshed terrain **in-engine**, so this
  blocks the GPU meshing programme (6b) on correctness independently of its
  performance question.
- The NVIDIA leg cannot be meaningfully run until this is green.

---

## 6b. GPU streaming (ADR-0006), after G0-G5 landed

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

- **(3) Scheme A, not Scheme B - the recorded bar was never passed.**
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
  Owed before this is final: a screenshot of the wall+roof fixture, which is the
  geometry the error concentrates on.
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
  the default. Only the pooled vertex factory samples the volume. So shipping the
  GI volume on by default is **coupled to Wave A's outcome**, not independent of
  it.

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
