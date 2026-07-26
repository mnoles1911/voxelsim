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
