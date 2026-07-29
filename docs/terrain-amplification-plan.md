# Terrain amplification redesign — 30 m diffusion tiles → 10 cm voxels

**Status, updated 2026-07-29.** **Phase 0 and Phase 1 (v9) are LANDED**, merged as **PR #171**
(`0e55c4c`, `cf9aa46`, `e9fb524`) — worldgen is at **v9** (`core.h:54`), `ctest` is green, and
`vxc_gpu` passes bit-exact on AMD. **Phase 2 is LANDED**, merged as **PR #176**: server bake
(`terrain_service/bake/{flow,noise,incise,thermal,pipeline}.py`), `.vxtl` v2 on both sides, service
plumbing, `pregen --mode bake`, and — since integrated into `claude/phase3-integration` — the
client half too (v2 parse + block index in `tilestore.cpp`, `FineTileSampler`, the residency gate
and prefetch ring in `tilestreaming.h/.cpp` and `VoxelFineTileStreamer`). **Phase 3 (client detail
rework) is in progress**: the carrier is extracted to `voxelcore/carrier.h` with analytic curvature
added, `detail_rill.h` and `detail_bedding.h` exist as standalone, tested, bounded functions, but
**none of the three is wired into `evalSurface` yet** — worldgen output and `kWorldGenVersion` (9)
are unchanged by any of it so far. **Phase 4 (3D density band) is not started.**
**Supersedes the park decision** in `docs/terrain-amplification-reconciliation.md` — see
"How this unparks the 2026-07-24 proposal" below.

**A phasing assumption this status corrects:** the Phasing section below expected Phase 2 to be a
`kWorldGenVersion` bump to v10. It is not, and that is a design win rather than a slip:
`FineTileSampler` (`tilestore.h:257-277`) is an `ITileSampler` like the coarse `TileGridSampler`, and
`Amplifier` was already written against that interface with a carrier that is pixel-size-agnostic
(`carrier.h:258`, "scale 1, 8 and 16 — deliberate, the direct lesson of v9"). Pointing an `Amplifier`
at a `FineTileSampler` evaluates the *same* v9 carrier code on the fine lattice; no second spline
implementation exists to drift, and no version bump is needed to change *which tile source* is
plugged in. The version bump Phase 3 will need is for wiring the new detail terms into
`evalSurface`, not for the fine tier's existence.

## Prerequisite reading (an executing session starts cold — read these first)

| File | Why |
|---|---|
| `docs/terrain-amplification-reconciliation.md` | Why the 2026-07-24 proposal was parked. This plan's central move is the answer to it. |
| `docs/research/terrain-amplification-design-doc.md` | The original stage-by-stage proposal. Stages §4, §5, §7, §8 are reused nearly as written. |
| `docs/determinism.md` | The float ban and CPU/GPU mirror contract that constrain every client-side change. |
| `voxel-core/src/amplifier.cpp:350-419` (was `:223-500` before the carrier's extraction to `carrier.h`) | The v2 octave table and its rationale comment — a worked example of diagnosing this system with measurements rather than screenshots. |
| `docs/voxel-earth-implementation-plan.md` §2 | Doctrine: determinism boundary, budgets, the "generated once, cached forever" rule this plan leans on. |

## Critical files

- `voxel-core/src/amplifier.cpp` — the height/material synthesis being replaced
- `voxel-core/shaders/worldgen.ush` — its bit-exact HLSL mirror; every change lands twice
- `voxel-core/include/voxelcore/{amplifier.h,biome.h,hash.h,tiles.h,tilestore.h}`
- `voxel-core/include/voxelcore/carrier.h` — **new (Phase 3)**: the C² carrier, moved out of
  `amplifier.cpp` verbatim, plus the analytic curvature added for §3c
- `voxel-core/include/voxelcore/detail_rill.h`, `detail_bedding.h` — **new (Phase 3)**: the rill and
  bedding terms, built and tested but not yet wired into `evalSurface`
- `voxel-core/include/voxelcore/tilestreaming.h` / `voxel-core/src/tilestreaming.cpp` — **new
  (Phase 2)**: the residency/prefetch-ring policy layer over `FineTileSampler`
- `voxel-core/bench/terrainprobe.cpp` — the verification instrument (Phase 0, landed; gained the
  drainage-connectivity/curvature/anisotropy structure metrics referenced under §3c)
- `terrain-service/terrain_service/{tile_codec.py,providers/diffusion.py,cache.py,pregen.py}`
- `terrain-service/terrain_service/bake/{flow,noise,incise,thermal,pipeline}.py` — **new (Phase 2)**:
  the geomorphic bake itself
- `terrain-service/tools/calibrate_stream_k.py` — the K/channel-initiation calibration procedure
- `voxel-core/src/tilestore.cpp` — tile parse, gains the v2 fine-tier decode
- `ue-project/Source/VoxelEarth/VoxelFineTileStreamer.{h,cpp}` — **new (Phase 2)**: UE-side glue for
  the residency/prefetch manager, gated behind `-VoxelFineTileDir=`
- `docs/vxtl-v2-format.md` — **frozen, normative** bit-level spec for the fine tier; where it and
  this plan's wire-format narrative disagree, the frozen spec wins

---

## Why this work exists

Terrain below 30 m reads as noisy and random, and the 30 m tile grid is plainly visible as
squares whose edges do not flow into their neighbours. Both were diagnosed to specific code and
then **measured** with `vxc_terrainprobe`; neither is a tuning problem.

### What is actually wrong

> **These next two paragraphs describe the v8 code this plan replaced, kept for the diagnosis's
> own sake.** `bilinearBaseMm` and `tileSlopeMmPerPx` no longer exist — Phase 1 deleted them and
> moved the replacement (the C² carrier) to `voxelcore/carrier.h`. The line numbers below are
> therefore historical citations into code that is gone, not live references; `evalSurface` itself
> is still in `amplifier.cpp`, now at line 854.

`Amplifier::evalSurface` (`amplifier.cpp:854`, was `672-717` in v8) computed
`surface = bilinear(4 tile corners) + Σ 5 octaves of isotropic integer value noise`.

**Symptom 1 — "noisy and random."** Terrain below 30 m is organised by *process*: water cuts
dendritic hollows, gravity builds talus at the angle of repose, creep rounds crests and fills
footslopes concave-up, bedrock exposes bedding. All of it is anisotropic and conditioned on the
coarse field. Isotropic fBm has none of it, so the eye reads static draped over a ramp — which is
literally what it is. Measured: directional roughness (across-slope vs along-slope `S2`) is
**0.97–1.00 at every lag** — perfectly isotropic. Real hillslopes are grooved by rills and score
well above 1.

**Symptom 2 — visible 30 m cells.** Three independent mechanisms, two of them now measured:

| # | Mechanism | Location | Measured |
|---|---|---|---|
| 1 | `bilinearBaseMm` is C⁰ but not C¹ — the surface *gradient* steps at every pixel line. Invisible in the height, glaring under directional light. | *removed in v9, was `amplifier.cpp:357-361`* | Carrier-only seam scan: straddle/interior `S2` ratio **8× at 0.1 m rising to 950× at 12.8 m**, doubling exactly with lag over an interior floor of 0.48 mm. Linear-in-lag growth over a zero floor is the signature of a slope discontinuity. On the amplified surface it survives at **3.28× / 1.61× / 1.23×** for 0.1 / 0.2 / 0.4 m lags and is masked beyond — i.e. visible exactly at voxel scale. |
| 2 | `tileSlopeMmPerPx` is a forward difference **constant over the whole cell**, driving `slopeScaleQ10` (0.25×…4.0×), which multiplies landform detail amplitude. The same noise field gets a step-discontinuous gain across every 30 m line. | *removed in v9, was `amplifier.cpp:363-365, 688, 316-318`* | Detail envelope steps by a **median of 150–310 mm, p90 770–1075 mm, max 2302 mm** across every boundary (four sites, 5.6%–118% grade). That is 1.5–3 voxels of texture amplitude appearing along a dead-straight 30 m line as the *typical* case. |
| 3 | Climate is read nearest-pixel, so `classifyBiome`, `surfaceMat` and topsoil depth flip exactly on the 30 m grid — blocky material patches. | `amplifier.cpp:1324` (was `:990`), `biome.h:145-171` | Fixed in v9 — see the Phase 0 baseline table below (material boundaries on the grid: 89.1% → 0.0%). |

No tile-local RNG is involved; the amplifier's noise is a global function of world mm and is
seamless by construction. **The "tile" you can see is the 30 m pixel cell itself.**

> **Measurement note, recorded because it cost time.** Mechanism 2 does *not* show up in a
> windowed roughness estimate — a first attempt measured local roughness at 0.2 m lag per
> half-cell and scored 0.76–1.35 against a 1.0 control, i.e. noise. That lag is dominated by the
> microrelief band, whose gate (`microScaleQ10`) is deliberately almost slope-flat. The band that
> steps is the landform band. Measure the gain discontinuity *directly off the tile raster*
> (`gainStepDirect`), not through an estimator that mostly sees the other band.

### Upstream facts that constrain the fix

- terrain-diffusion's learned cascade **ends at 30 m**. `decoder_model/` is a fixed 8× latent→pixel
  stage (240 → 30 m/px); no finer checkpoint exists.
- The API's `scale=2/4/8` is **pure bilinear upsampling** (`terrain-diffusion/.../api.py:139-146`),
  and our scale-8 path does the same (`providers/diffusion.py:1028-1073`). **The 3.75 m/px tier
  already exists in the format and carries zero information today.** That is the slot this fills.
- Elevation crosses the wire as int16 **whole metres** — 1 m vertical quantisation, 10× coarser
  than a voxel.
- terrain-diffusion's own Minecraft mod (`minecraft_api.py:265-355`) does exactly what we do —
  bilinear + slope-gated fBm — and reproduces mechanisms 1 and 2 for the same reasons. It is not
  a target to match.

`amplifier.cpp:384-387` (was `:262-266`) states the governing assumption: *"30 m is the end of that
cascade and no finer model exists. Everything below 30 m is procedural and always will be."* The
first clause is true. **The second is what this plan overturns** — not by training a finer model,
but by *simulating* the missing band once, offline, and shipping it as data.

## How this unparks the 2026-07-24 proposal

`terrain-amplification-reconciliation.md` parked the earlier design over three collisions: the
float ban forces a fixed-point rewrite plus HLSL mirror per stage (≈2× cost); the geomorphic
passes (§6) break the O(1) point queries collision and digging need; and 3.2 m chunks cannot
carry the 512 m regions-with-aprons the doc assumed.

**All three dissolve if §6 moves off the client and into the tile bake.** Baked bytes need no
HLSL mirror, no fixed-point rewrite, and no relaxation at query time — the client just reads a
raster, which is already what `TileGridSampler` does. The doc's networked-desync objection is
resolved too, because the fine tier becomes a hard dependency rather than a progressive
enhancement. The bake sits on the *"generated once, cached forever"* side of the determinism
boundary, alongside the diffusion model itself, where floats and iteration are already legal.

---

## Design: band ownership

Every band gets exactly one owner; no band has two.

| Wavelength | Owner | Mechanism |
|---|---|---|
| ≥ 60 m | terrain-diffusion (unchanged) | learned 30 m/px tiles |
| 60 m → 7.5 m | **server bake (new)** | flow routing, stream-power incision, thermal relaxation → shipped raster |
| 7.5 m → 0.2 m | **client amplifier (rebuilt)** | C² carrier + curvature/slope-gated, downslope-anisotropic detail |
| 0.7 m → 0.1 m, 3D | **client density band (new)** | bedding ledges and pockets on steep rock faces |

### Layer 1 — server-side geomorphic bake

> **What it buys:** hillsides that drain. Right now every slope is the same everywhere; after
> this, water has visibly organised the ground into ridges and hollows that connect into a
> network, the way real terrain does. This is the one item that changes the *category* of the
> problem rather than moving a number — connected drainage is unreachable by any amount of
> better noise, because noise is a point function and drainage is not.

Runs on the GPU pod that already generates tiles.

- **B0 — C² carrier.** Cubic B-spline prefilter + 8× upsample. Must produce exactly the
  reconstruction the client computes, or quantisation drift leaks in.
- **B1 — conditioned roughness.** Slope-, curvature- and climate-gated fBm in the 240 → 8 m band,
  hashed in *world* coordinates so it is bake-batch invariant. This exists so the erosion passes
  have substrate to act on: incision carved into a smooth ramp reads as lines scribed on plastic.
  This is exactly why compositing noise *after* the surface is finalised — what the client does
  today — cannot work: no process ever touches it.
- **B2 — flow routing + incision.** Priority-flood depression fill; **MFD for the accumulation
  field, D8 for channel centrelines** (at 3.75 m/px, D8's 45° faceting shows as bevelled channel
  walls); carve `depth = K·A^m·S^n` (m≈0.45, n≈0.8), width `∝ A^0.4`, erodibility from climate and
  soil index. `terrain-diffusion/.../postprocessing.py:6-60` has reference `d8_flow` /
  `flow_accumulation` — right algorithms, but the `heapq` loop must be replaced for 17 M cells.
- **B3 — slope-limited thermal relaxation.** ~48 Jacobi iterations, per-cell talus angle by regime.
  Runs *after* incision so gully walls weather and spoil forms talus cones and fan aprons. Also a
  low-pass on the residual, so it directly improves compression.

**Bake prototype — measured, and it corrected three of this plan's own choices.**
`terrain-service/tools/bake_prototype.py` runs B0–B3 at full resolution on a real tile (needs the
`terrain-diffusion` venv for numba/scipy). Per stage, 4096², one tile:

Reported as **wall AND process CPU time**. Wall-clock is what a user waits but is badly distorted
by anything else on the machine (these were taken while another session held the box). CPU-seconds
sum kernel+user across this process's threads, so a competing process steals wall-clock but not
CPU — that column is the contention-robust one, and it also separates parallel from sequential
work for free.

| stage | wall s | **cpu s** | note |
|---|---|---|---|
| B0 B-spline upsample | 0.55 | 0.52 | |
| B1 conditioned fBm | 7.47 | 7.30 | single-threaded; scipy `zoom` dominates, easily improved |
| B2a priority-flood fill | 2.67 | 2.55 | genuinely sequential — cpu≈wall confirms it, stays on CPU |
| B2b D8 receivers | 1.32 | 1.67 | |
| B2c elevation sort | 0.58 | 1.27 | |
| B2d MFD accumulation | 1.74 | 1.67 | sequential sweep |
| B2e stream-power incision | 0.34 | 0.34 | |
| B3 thermal relaxation ×48 | 4.59 | **25.31** | ~5.5× parallel — the one stage a GPU really helps |
| **total** | **19.25** | **40.62** | vs the ≈1.5 s originally estimated |

The split is the useful part: **~13.5 CPU-s is sequential** (B1, priority-flood, sort, MFD,
incision — all cpu≈wall, so more cores do not help), and **~27 CPU-s is parallel**, almost all of
it thermal relaxation, which is exactly the stage a GPU eats. A production bake with the parallel
work on the pod's GPU and B1 written properly plausibly lands at **5–10 s/tile**.

The genuinely sequential core (priority-flood + sort + accumulation) is ~5.8 s; the rest is
GPU-parallel. A production bake plausibly lands at 6–10 s/tile — still **4–6× the estimate**, but
the on-demand argument is unaffected because coarse diffusion (22.5 s/tile, and 5 tiles per step
of the frontier against 3 bakes) dominates it either way.

Result: a real dendritic network — branching, tributaries joining, no lattice faceting — carved
into the surface as valleys with sharp interfluves between them (max accumulation 78 km²; 15,773
cells above 1 km²).

> ⚠️ **THE DRAINAGE FIGURES AND THE K TABLE BELOW ARE MEASURED ON A MIS-ROUTED FIELD.** The
> prototype's priority-flood was a plain fill, so every filled pit became a **level lake** — and on
> a level lake no cell has a lower neighbour, so MFD terminates. On the same tile that produced the
> numbers below: **341,368 inland dead-ends and 69.2% of land area stranded**, never reaching the
> sea. Barnes' ε variant (every newly discovered cell raised to at least `spill + 2 ULP`) fixes it,
> and is the default in `bake/flow.py`. Isolated cleanly: with `flat_eps=0` the production module
> reproduces the prototype's numbers exactly, so ε is the only difference.
>
> | | prototype | corrected |
> |---|---|---|
> | inland dead-ends | 341,368 | **0** |
> | max catchment | 69.6 km² | **203.1 km²** |
> | cells > 1 km² | 13,256 | **107,496** |
>
> **K must be re-calibrated, not scaled** — and it has been (2026-07-29), on branch
> `claude/k-calibration-and-channel-init`. With ~2.9× the catchment area, `A^0.45` is ~1.6× deeper
> on trunk channels, so the expectation was that K would have to fall from 0.15 to ~0.09. **That
> expectation was wrong.** Judged on a hillshade of real tile (−5, 3) at 1.875 m/px, 0.09 leaves
> trunk channels legible but tributaries not; **0.15 gives a legible dendritic network at both
> 7.7 km and 1.4 km zoom**; 0.25 begins showing parallel grooving at the 1.4 km zoom. The 1.6×
> arithmetic was right about the depth and wrong about the conclusion — the original judgement
> that motivated it was made on a *different tile at 3.75 m/px*, so there was never a like-for-like
> appearance to preserve in the first place. **`stream_K = 0.15` stands, now for a measured reason
> instead of an inherited one** (`terrain_service/bake/incise.py`, `pipeline.py`'s
> `BakeConstants.stream_K`).
>
> **Calibrating it surfaced a second, independent defect: there was no channel-initiation
> threshold at all.** `depth = K·A^m·S^n` incised *every* cell with any upslope area — on tile
> (−5, 3), 77.6% of the domain past one voxel at K=0.03, 98.6% at K=0.15. That is not a drainage
> network; it is a slope-dependent lowering of the whole surface with a network faintly embedded
> in it. Added: a soft gate `A^q / (A^q + A_crit^q)` at `channel_init_area_m2 = 1e4` (mid-range for
> the 10³–10⁵ m² channel-initiation areas reported for humid soil-mantled landscapes), `q = 2` so
> the transition is C^∞ and roughly a decade wide — a hard cutoff would put a *step* in incision
> depth along the contour where contributing area crosses the threshold, the exact seam class this
> project exists to remove, just relocated off the 30 m grid. With the gate, the same tile drops to
> **25.4% incised at p99 depth effectively unchanged (8.66 m → 8.34 m)**: hillslopes released,
> channels not. This is the knob that sets **drainage density**; without it, K had to set both how
> deep channels cut and how many there are, which are not the same question.
>
> **Two measurement caveats from the calibration run, worth preserving so they are not
> rediscovered:** the `incision_cap_m` clamp binds at *every* K tested, including 0.03, so `max`
> incision is censored and **p99 is the only usable tail statistic**; and incision depth alone
> cannot separate "carved correctly" from "over-carved" (it rises monotonically with K, without a
> kink), so the judgement also rests on the fraction of the domain left steeper than the angle of
> repose — measured as a 2D gradient magnitude, which can exceed the repose angle by up to √2 on
> ridges and corners **without violating the per-axis rule `thermal.relax` actually enforces**.
> See `terrain-service/tools/calibrate_stream_k.py` and the comments atop `bake/incise.py`.

**Erodibility K matters more than anything else for whether it reads as terrain**, and the
plausible-looking first value was far too small (values below are on the mis-routed field):

| K | incision mean | p99 | reads as |
|---|---|---|---|
| 0.0004 | 0.00 m | 0.01 m | network exists in the flow field and nowhere in the ground |
| 0.012 | 0.13 m | 0.63 m | gentle modulation; visible but not landform |
| 0.05 | 0.56 m | 2.61 m | valleys emerging |
| **0.15** | **1.66 m** | **7.84 m** | **properly dissected hillslope** |

This table's numbers are on the mis-routed field and are kept only to show *why 0.0004 looked
plausible and was 400× too small* — the actual calibration decision was re-run on correctly-routed
drainage and is recorded in the warning box above. **`K = 0.15` is confirmed, not merely carried
over**: it survived re-measurement on a real, correctly-routed tile and a from-scratch hillshade
judgement, and the channel-initiation gate (also above) is now what separates "how deep" from "how
many" — over-carving's other failure mode, indiscriminate incision, is what that gate fixes.

**The prototype's output is still too smooth, and the probe says exactly where.** Detrended H of
the baked surface, per stage (`report_spectrum`, same S2 definition as `vxc_terrainprobe`):

| band | H, no prefilter | H, with prefilter | verdict |
|---|---|---|---|
| 120–240 m | 0.892 | **0.832** | real 30 m data, physical |
| 60–120 m | 1.172 | **1.027** | |
| 30–60 m | 1.475 | **1.239** | |
| 7.5–30 m | ~1.60 | ~1.63 | below source Nyquist — B1 must supply this |
| 3.8–7.5 m | 1.669 | 1.653 | ditto |

Two separate findings, both of which the plan already specifies and the prototype violated:

1. **The B-spline prefilter is required, and it is measurable.** A B-spline *approximates* its
   control points, so feeding it raw samples low-passes the source: without the prefilter the
   carrier is smoother than the raster it came from, right in the band the bake is meant to
   extend. Adding it (`scipy.ndimage.spline_filter`) improves H throughout 30–240 m. This is the
   float IIR pass the plan puts in the bake precisely because it cannot live in `voxel-core` — and
   it is why the tier ships **control points, not samples**.
2. **B1 must be built to a target SPECTRUM, not a target RMS.** H stays ~1.65 below 30 m at any
   roughness tried, and quadrupling total roughness (1.5 → 6 m) moved S2(7.5 m) only 1.5×, because
   nearly all of the fBm's energy sits at coarse scales. H > 1 is "smoother than linear" — the same
   signature `amplifier.cpp:350-381` (was `:242`) records as worldgen v1's failure. Extrapolating the coarse
   raster's own H (0.83, measured at 120–240 m) down to 7.5 m gives a target of S2 ≈ 1.06 m against
   the 0.23 m currently produced.

Consequence for Phase 3: **do not calibrate the client's octaves off this prototype's output.**
Its H is unphysical, so extrapolating from it would make the client *smoother*, which is
backwards. `report_spectrum` refuses to emit amplitudes when H > 1.05 for that reason.

### The spectrum is NOT a sufficient acceptance criterion — demonstrated

`--rough -1` builds B1 to the fitted target spectrum instead of a target RMS (`spectrum_fitted_fbm`
— fit H and C from the carrier's own 120–240 m band, synthesise only octaves *below* the 30 m
source Nyquist, amplitude `A(L) = C·L^H`). It works, spectrally: **H goes from 1.65 to 0.91 at the
fine end**, squarely physical.

**And the terrain gets worse.** The hillshade reads as uniform crumpled paper, and the flow field
— dendritic and connected before — collapses into a confetti of disconnected micro-catchments.
The 3.19 m of 30 m-wavelength noise required to hit the target spectrum is enough to break up
drainage organisation entirely: every noise dimple becomes its own sink.

This is this document's own opening argument, now demonstrated instead of asserted: *"That is
spectrally reasonable and structurally meaningless… The fix is not 'better noise.'"* A surface can
have a textbook-correct self-affine spectrum and no landform structure whatsoever.

**So the Phase 3 gate "H ∈ [0.6, 1.0] at every lag decade" is necessary but nowhere near
sufficient, and on its own it is actively misleading** — it is satisfiable by exactly the failure
mode the project exists to fix. It must be read alongside the directional-anisotropy metric (which
distinguishes grooved from crumpled) and a hillshade, and no octave retune should be accepted on
the H number alone.

The constructive reading for Phase 2: B1's roughness is **substrate for erosion, not final
texture**. Keep it modest, and let the fine-scale spectrum be filled by *process* — finer rills,
more incision detail — rather than by amplitude. If H is still too high after that, the answer is
more geomorphology, not more noise.

### The same trap, one layer down: post-fill drainage statistics are not sufficient either

The Barnes ε-fill that makes MFD routing work (see the drainage-figures warning above) has its own
version of the H problem, measured directly in `vxc_terrainprobe`'s drainage-connectivity metrics
(`voxel-core/bench/terrainprobe.cpp:829-868`). Priority-flood + epsilon does not merely repair a
pitted field — it **manufactures a plausible-looking drainage network on any input at all**,
because it raises every pit until the whole domain has a monotone path to an edge, and D8 then
traces respectable channels down the ramps the fill just built. On the v9 baseline, the
**detail-only field** — five octaves of isotropic value noise, no landform whatsoever — scores an
exceedance slope **β = 0.64 (R² = 0.995)**, 37 channel components and 1,031 junctions/km², every one
of which reads as "a network" if quoted alone. The same field strands **97.9% of its area in 51,704
interior pits per km²** before the fill ever touches it.

This is exactly this document's existing "spectrum is not a sufficient acceptance criterion"
argument, recurring at the routing stage instead of the spectral one: a summary statistic computed
*after* a repair pass can look textbook-correct on an input the repair pass had to invent structure
for. **The fix is the same shape, too: read the post-fill `net.*` numbers only next to their
pre-fill `raw.*` companions** (`raw.interior_sinks_per_km2`, `raw.stranded_area`,
`raw.mean_path_len`) — the raw pit census, taken before any fill runs, is what actually carries the
verdict on whether a surface drains. Quoting `net.*` alone is the same class of error as quoting H
alone.

**Six more defects found when the prototype was rewritten as production modules.** Recorded
because three of them revise things written here as settled:

- **Thermal lost mass at the border.** Both passes ran `1..h-2`, so material shed *towards* the
  edge row was subtracted from the donor and gathered by nobody. Small in magnitude, but it made
  the conservation invariant untestable — which is presumably why the 128 m cliff-stripping bug
  above was caught by eye rather than by an assertion.
- **One repose limit for cardinal and diagonal neighbours.** `tan(36°)·cell_m` across a √2 longer
  run is a **41% gentler** limit on the diagonals — the classic eight-neighbour artifact, and it
  produces octagonal talus cones. Scale the drop by pair distance.
- **`band /= band.std()` breaks seams independently of the RNG.** The normaliser is measured over
  the domain, so the same world location gets a different amplitude in two overlapping bakes *even
  if the noise itself were world-anchored*. This is a **second, separate** blocker to the apron
  argument, and the seam test's "inject a shared pre-computed field" workaround masked it. The
  apron conclusion still stands — isolating apron adequacy is exactly what the injection was for —
  but "world-anchor the noise" was two problems, not one. Compute the normaliser analytically.
- **The steepest-pair rule bounds what a cell GIVES, not what it RECEIVES.** An isolated one-cell
  pit is filled by all eight neighbours in the same step, each correctly shedding its whole budget
  into it, so max slope can *rise* before converging (measured 128 → 164 in a single step, then
  2.16 by 48 iterations and 0.88 by 800). It converges at `rate=0.4` with mass exact throughout,
  but it is the sharp edge of the scheme, and it bites hardest here because a 25 m incision cap at
  1.875 m/px produces narrow pit-like channels. `relax()` now rejects `rate > 0.5`.
- The prototype's default roughness path ignored the source-Nyquist rule that the
  spectrum-fitted path implements — it started at whole-domain wavelengths and layered noise
  across the entire band the diffusion model owns.
- The carrier accumulated in float32 with float32 weights. At `tq=0` the weights are `(1,4,1,0)/6`
  and the prefilter's whole job is to make that reproduce the sample, so float32 accumulation
  spends part of the prefilter's accuracy before erosion even starts.

**Three prototype bugs, each of which validated a choice in this plan by violating it:**

- *Thermal must conserve mass.* Subtracting the over-repose excess without depositing it stripped
  128 m from cliff tops in 48 iterations. And the shed must be scaled by the **steepest** pair, not
  the sum over eight neighbours — scaling by the sum diverged the field to ~1e23.
- *B1's fBm must be properly interpolated.* Box-upsampling each octave (`np.kron`) produced a
  hillshade of hard rectangles at every octave scale — the exact grid artifact this project exists
  to remove, reintroduced by the pass meant to supply natural roughness. Invisible in any averaged
  statistic; obvious the instant it is shaded.
- *MFD, not D8, for the accumulation field.* Pure D8 gave dead-straight 45° diagonal channels tens
  of pixels long, exactly as predicted below. D8 stays right for tracing a channel centreline; it
  is wrong for the area field.

**Rejected — full hydraulic pipe-model erosion (Mei et al.).** Its distinctive outputs at this
resolution duplicate B2+B3 at 100–1000× the iteration count: pipe models move sediment over
kilometres across thousands of small timesteps, while stream power *solves for the answer*
geometrically. It is also history-dependent, which makes the apron argument strictly worse. The
band where it genuinely wins (meandering, braid bars) is below 3.75 m/px anyway.

**Not in the bake:** clasts, stratigraphy, overhangs — materials and 3D structure, already
O(1)-compatible client-side, and they would bloat the wire format for something the client derives
free.

**Aux plane.** One `uint8` per fine pixel: log₂ flow accumulation with top bits flagging
channel / bank / deposition. Mostly zeros, ~5–10 KB compressed. Lets the client paint alluvium in
beds and exposed subsoil on cut banks, and later feeds water placement and bank undercuts.

**Seams — aprons, not blending.** Each tile bakes on its domain plus a 256-fine-px (960 m) apron
and writes only its interior. Every bounded pass has influence radius well under the apron
(thermal 48 cells, carve stamps ~20), so each interior equals the infinite-domain answer and
neighbours agree exactly — seamless by construction, no stitching pass.

**MEASURED, and 960 m is right with room to spare.** `terrain-service/tools/bake_seam_check.py`
bakes one domain spanning two adjacent tiles (the "infinite domain" answer), bakes each tile
separately on its own domain + apron, and compares. All bakes share one world-anchored noise
field, so the test isolates apron adequacy rather than this prototype's array-coordinate fBm.

| apron | mean err | **max err** | step across the join, vs the terrain's own gradient |
|---|---|---|---|
| 30 m | 0.43 cm | **9.78 m** | **+40.32 cm** |
| 120 m | 0.36 cm | 6.05 m | −0.06 cm |
| 480 m | 0.00 cm | 2.93 cm | +0.01 cm |
| **960 m (this plan)** | **0.00 cm** | **0.01 cm** | **+0.00 cm** |

At the plan's apron the per-tile bakes reproduce the single-domain answer to **0.1 mm**, and the
one-cell step across the tile boundary is identical to truth (26.50 cm mean, 221.86 cm p99 — that
is the terrain's own gradient, not a seam). The small-apron rows are the control proving the test
can see a bad apron at all: at 30 m it finds ~10 m of error and a 40 cm join step. Error collapses
between 120 m and 480 m, so the influence radius is a few hundred metres and 960 m carries real
margin.

The test also probes the *unbounded* dependency directly, by comparing the accumulation field
rather than only the height. Across 99,893 cells with a catchment ≥ 0.1 km² (largest 10.55 km²),
the per-tile/truth accumulation ratio at 960 m is **1.000 at median, p01 and min** — the apron
captured every catchment present. At 120 m the min falls to 0.082, i.e. a cell seeing 8% of its
true catchment, which is what produces that row's 6 m height error.

**What this does NOT establish.** The whole test domain is ~15 km across, so the largest catchment
it can contain is ~10 km². The hydrology pyramid exists for rivers whose catchment spans *many*
tiles — hundreds of km² — and no apron of any practical size captures those. That regime is
untested and the pyramid remains justified; what is now established is that everything *below* it
is handled by the apron alone, exactly.

The one unbounded dependency is flow accumulation. Handle it with a **hydrology pyramid mirroring
the model's own hierarchy**: accumulate at the coarse-map level (7.7 km/px, where the coarse model
is tiny and huge areas cost almost nothing), then at 30 m over the local neighbourhood, and inject
upstream accumulation as inflow boundary conditions at the fine domain edge. Residual defect: fine
rills meandering more than 960 m before rejoining a coarse-resolved channel may kink at a tile
edge — sub-metre, 1–2 px wide, regression-testable via the design doc's §11 checklist.

### Layer 2 — wire format (`.vxtl` v2)

> **Superseded by the frozen spec.** This section was written against a scale-8 (3.75 m/px) fine
> tier. `docs/vxtl-v2-format.md` (frozen 2026-07-29) ships **scale 16 (1.875 m/px)** instead — see
> "Fine resolution decided: 1.875 m, not 3.75 m" below open risk #4 — and its block geometry is
> **480 m blocks (256×256 fine px at 1.875 m/px), 32×32 = 1,024 blocks per tile**, not the
> 960 m / 256-per-tile numbers below. The reasoning in this section (why absolute control points,
> why independent per-block zstd frames, why MED prediction) is unchanged and still the normative
> rationale; only the pixel size and the resulting block/tile counts moved. Where the two disagree,
> **`docs/vxtl-v2-format.md` is authoritative** — it is the frozen bit-level contract between the
> Python encoder and the C++ decoder, this is design narrative.

Keep the s1 tile byte-identical. **Redefine the s8 slot** to hold one fine container per coarse
tile coordinate — safe, since nothing was ever generated at scale 8 (`tile_codec.py:41`,
`tilestore.h:66`). The addressing change rolls `provider_id` through `_tile_format_fingerprint`,
exactly as that mechanism is designed to.

```
Header (AS SHIPPED — see vxtl-v2-format.md §3): extends v1 <4sHQiiBH> with
  version=2, scale=16 (1.875 m/px), size=8192,
  block_log2=8 (256×256 fine px = 480 m blocks, 32×32 = 1,024 blocks per tile),
  predictor, quant, codec, bake_ver, flags, base_offset_mm i32, parent_scale, reserved, n_sections
Sections: ELEV_INDEX (1,024 × {offset, comp_len, mode, const_cp, resid_bits, pad}) + ELEV_DATA
          (independent zstd frames per block; CONSTANT mode = 0 bytes)
          optional FLOW_INDEX / FLOW_DATA
```

**Ship absolute B-spline control points, not residuals against the coarse tier.** int16 with a
**10 cm LSB** (exactly one voxel) plus a per-tile `base_offset_mm`, giving ±3.27 km of relief about
the tile's own datum; `quant` allows a 25 cm fallback for the rare higher-relief tile. Residual
coding compresses slightly better but makes fine decode depend on a resident 3×3 ring of coarse
tiles and re-imports the coarse tier's C¹ break into client arithmetic. Absolute control points
make the contract trivially clean — **the fine plane *is* the control lattice**, so the client
never interpolates samples at all — and the entropy lives in the gradient, not the absolute value.

Per block: MED/LOCO-I prediction, zigzag mapping, one independent zstd frame. Independent frames
buy per-block random access — the client decompresses only the ~0.92 km² blocks it needs.

**Size — MEASURED, and the model that predicted it was wrong.**
`terrain-service/tools/measure_fine_tier_size.py` decodes a real tile, upsamples it 8× with the
same cubic B-spline the client carrier uses, adds controlled roughness, and runs the actual MED +
per-block codec. On tile (−6,3) — 3683 m of relief, so a hard case. (zstd is not installed on this
box; zlib and lzma bracket it, and zstd −19 normally lands nearer lzma.)

| added roughness | RMS | MED σ | zlib | **lzma** | bits/px | model said | KB/km² |
|---|---|---|---|---|---|---|---|
| none (smooth 8× upsample) | — | 27.1 cm | 7.91 MB | **6.05 MB** | 2.88 | 7.01 | 25.0 |
| fBm (correlated) | 0.5 m | 27.2 cm | 8.60 MB | **6.65 MB** | 3.17 | 7.01 | 27.5 |
| fBm | 1.0 m | 27.2 cm | 9.39 MB | **7.33 MB** | 3.50 | 7.01 | 30.3 |
| fBm | 2.0 m | 27.4 cm | 10.67 MB | **8.47 MB** | 4.04 | 7.02 | 35.1 |
| white (uncorrelated) | **0.05 m** | 28.5 cm | 15.77 MB | **12.89 MB** | 6.15 | 7.08 | 53.4 |
| white | 0.15 m | 37.3 cm | 18.85 MB | **15.17 MB** | 7.23 | 7.46 | 62.8 |

Three things this changes:

1. **The 8 MB/tile planning number stands — but only for *correlated* detail.** fBm at a full 2 m
   RMS lands at 8.47 MB. White noise at **0.05 m** — forty times less amplitude — costs
   **12.89 MB**, half as much again. Compressed size tracks how *uncorrelated* the added detail
   is, not how large it is. That is a hard constraint on the bake: B1's roughness must stay fractal
   (it is), and the centimetre quantisation must not dither.
2. **σ is the wrong predictor, and the old model was wrong.** It predicted ~7.0 bits/px across the
   whole sweep where measurement ranges 2.88–7.23. It assumes i.i.d. Laplacian errors; real MED
   errors are spatially correlated, so a real coder beats the marginal entropy by roughly 2×. It
   over-predicts, i.e. it erred safe — the 8 MB number was accidentally right for the wrong
   reason. Do not re-derive sizes from σ.
3. **The floor is 4.0–6.3 MB/tile** across three tiles sampled (smooth upsample, no detail at all),
   16–26 KB/km². Nothing the bake does can go below it.

> **These numbers are all at 3.75 m/px (scale 8).** The shipped tier is **1.875 m/px (scale 16)** —
> see open risk #4, now resolved as a decision below — which is 4× the pixels. Per
> `docs/vxtl-v2-format.md`, the measured shipped figure is **~21–25 MB/tile compressed** rather
> than a naive 4× of the number above, because halving post spacing also halves the per-step
> gradient MED sees, buying back roughly 1 bit/px. The *reasoning* above (compressed size tracks
> correlation, not amplitude; σ over-predicts) is unaffected by the pixel size and still governs the
> finer tier.

**Codec finding:** 73–159 MED errors per tile exceed int16 even at 10 cm quantisation — a 30 m
cliff across one 3.75 m post will do it. The block format needs an escape: an int32 block mode or
a per-block residual-width field, not a bare int16 everywhere.

Decoded block 128 KB; zstd decode of a ~40 KB frame ≈0.1–0.3 ms.

Decode is a pure integer function of the bytes: zstd decode is bit-exact by format spec, MED
inverse and B-spline evaluation are exact int64 with specified floor division. **The encoder need
not be deterministic** — the same licence the diffusion model already enjoys, and the reason the
bake may use floats and GPUs at all.

### Layer 3 — client amplifier rebuild

All integer, all O(1) point functions, all mirrored in `worldgen.ush`.

**3a. C² carrier — uniform cubic B-spline, replacing `bilinearBaseMm`.**

> **What it buys:** the visible grid of creases goes away. This is the direct fix for the single
> largest artifact, and it is measurable rather than a matter of taste — see the acceptance bar.

Chosen over Catmull-Rom for three reasons in order of weight: B-spline weights are non-negative
and sum to the divisor, so the carrier stays a convex combination of control points and
`surfaceBoundsMm` keeps a *provable* bound with no new slack derivation (Catmull-Rom's negative
weights would need one, in the one place an error is a hole in the world); it is C², not merely
C¹, which matters because §3c conditions detail on curvature and a C¹ carrier would reproduce the
same artifact one derivative down; and its approximation deficit is fixed for free by prefiltering
at bake time, where floats are legal.

Fixed point: fraction in q10 via the `fadeFractionMm` precedent (`hash.h:90-96`); weights as
integer numerators over `6·1024³`; **two-stage separable evaluation with an intermediate
division** — forced, not chosen: the exact tensor form overflows int64 by ~10 orders of magnitude
at `pxMm=30000`. Signed division stays on `floorDiv`/`truncDiv` per the discipline that already
diverged AMD vs NVIDIA (`worldgen.ush:181-234`).

Cost: a 34×34 level-0 column job spans ≤2×2 fine cells, so the whole job touches a **5×5 control
window once**. Hoisting stage-1 row sums gives ~170 reductions instead of 4,624 — amortised to
roughly **5 int64 multiplies per column, cheaper than today's bilinear plus four memo probes**.
Unhoisted on GPU it is ~34 multiplies against an `evalSurface` already spending ~300 on hashing;
do not contort the HLSL to hoist.

`kSurfaceBoundMaxCornersPerAxis` (`amplifier.h:159`, was `:87`) must rise **16 → 34** for the
dilated stencil (34² int64 = 9.2 KB stack). This also discharges the standing warning that the
bound silently degrades at level ≥5 on scale-8 tiles. **Superseded again since**: once open risk #4
resolved in favour of the 1.875 m fine tier (§2 below), 34 was no longer enough either — a level-5
footprint at 1.875 m needs ~62 control points and would decline at 34, the identical cliff this
rise was meant to remove, just one tier finer. The constant now stands at **64** (32 KB stack;
`amplifier.h:128-159` records the full history: 16 at v8 → 34 at v9/3.75 m → 64 for the shipped
1.875 m tier).

**3b. Continuous modulation — delete `tileSlopeMmPerPx`.**

> **What it buys:** ground texture stops changing character at invisible lines. Measured today at
> a median 150–310 mm step per boundary; this drives it to zero by construction rather than by
> tuning.

Slope becomes the analytic spline derivative: a convex combination of control-point first
differences with quadratic B-spline weights (closed form, same q10 machinery, all weights ≥0).
Curvature falls out as a linear B-spline of second differences. Both continuous, so the gain step
becomes structurally impossible.

Change the modulation currency from **mm-per-pixel to mm-per-metre**. Not cosmetic: it fixes a
latent bug the code already flags at `biome.h:56-63` — `slopeScaleQ10`, `microScaleQ10` and
`classifyBiome` all take mm-per-*pixel* and would silently mean something different on 3.75 m
tiles. That file asks for exactly these three to be fixed together before scale-8 tiles exist.

Climate moves to quintic-faded bilinear in u8×256 fixed point plus a ±2-unit ecotone dither on a
1.6 m lattice, so biome boundaries become smooth iso-curves speckled over a few metres. Fixes
mechanism 3.

**3c. Structured, anisotropic detail — replace, do not layer.**

> **What it buys:** ground that looks shaped instead of textured — ridges crisp, hollows soft,
> hillsides grooved down the fall line the way real slopes are.

**v9's sub-30 m band has NO drainage connectivity — measured, not assumed, on shipped worldgen.**
`vxc_terrainprobe`'s drainage-connectivity metrics (added alongside this work,
`voxel-core/bench/terrainprobe.cpp`), run at seed 20260719, site (−84480, 53760): the **carrier**
drains fine on its own — 41 sinks/km², 3.8% of area stranded, 166 m mean flow path before
termination. **Adding v9's five isotropic detail octaves** — exactly the band this subsection
replaces — turns that into **44,352 sinks/km², 97.3% stranded, 4.3 m mean flow path**. Confirmed at
0.1 / 1 / 3 m voxel cells, so it is not a sampling artifact of one resolution. The same tool reports
curvature conditioning at 0.98–1.03 (i.e. conditioned on nothing — a curvature gate should move
this toward ~3.5) and rill anisotropy at 0.98–1.01 with a 45° control column at 1.00–1.02 (i.e. no
anisotropy at all, on or off the voxel lattice). This is the same isotropic-fBm diagnosis the plan
opened with, now quantified on the exact band Phase 3 is about to replace, and it is the number this
whole subsection exists to fix.

Where the fine tier is present, **delete the 25.6 m and 6.4 m landform octaves**
(`amplifier.cpp:213-214`, was `:276-277` before the carrier extraction); they would fight measured
landforms with hash noise. **Not yet done** — as of this revision the octave table is unchanged and
`kLandformMaxMm`/`kMicroMaxMm` (`amplifier.cpp:540-541`) still reflect the old five-octave table;
Phase 3 has built the replacement terms alongside the old ones, not yet swapped them in. Then:

- a continuation ladder `{3200, 1600, 400, 200}` mm, amplitudes **set by probe measurement, not
  taste**, placed so `S2(d)` continues at H≈0.8 through 7.5 m;
- a **curvature gate** — convex crests roughen ~1.75×, concave hollows smooth to ~0.5× (colluvial
  fill). This is the ridge-sharp/valley-smooth asymmetry that reads as shaped ground. **Built**
  (`carrier.h`'s `curvatureScaleQ10`, analytic Laplacian via `evalCarrierCurvature`), proved
  monotone and bounded at compile time, **not yet wired into `evalSurface`**, and its constants are
  explicitly marked provisional pending calibration against the fine tier's measured spectrum;
- a **rill/flute term** with the domain anisotropically scaled by the local gradient frame,
  ~1.6 m across-slope and ~13 m along-slope. The frame uses an octagonal norm (`max + min/2`, ≤12%
  direction error, which reads as natural wavelength variation) — no sqrt, no trig. Its gate goes
  to zero below ~10% grade, which also neutralises the frame's instability where the gradient
  vanishes. Domain warping cannot change value noise's output *range*, so this costs the bound
  exactly its gated amplitude. **Built** (`voxelcore/detail_rill.h`, `rillMm()`), measured
  across/along ratio **31.1× at 1 m lag, 14.6× at 2 m, 6.9× at 3 m** at a constant 60% gradient
  (bar is >1), **not yet wired into `evalSurface`**, amplitude 300 mm provisional (chosen as 0.6×
  the old isotropic 1600 mm octave, pending calibration against the fine tier's measured `S2`).
  >
  > **A specification error found while building this, worth recording as a design lesson.** This
  > plan's text (above) says to rotate the sample point into the local gradient frame — literally,
  > one rotation matrix built from the gradient, applied to the *world* position. In an *unbounded*
  > world that is wrong: the noise phase becomes `R(∇h(p))·p`, whose derivative carries a term
  > proportional to **distance from the world origin**. On a real hillside, where aspect turns
  > roughly 1 radian over a few hundred metres, that term reaches ~50 at 10 km out — one metre of
  > ground motion becomes fifty metres of noise motion, and the 1.6 m rills degenerate into
  > per-voxel static purely as a function of how far the player is from `x = 0`. It stays
  > mathematically continuous throughout, so this is a **conditioning failure, not a
  > discontinuity** — a continuity test would not catch it, and it would have shipped. Measured:
  > **61× excess roughness at 54 km** from the origin, against a literal implementation of this
  > plan's text kept as a regression control in `test_detail_rill.cpp`. The fix: quantise the
  > rotation to a compile-time table of 16 fixed directions and blend the two nearest fields, so
  > each field is a *fixed* linear map of world position with no lever arm — only the (bounded)
  > blend weight sees the gradient. Costs ≤4.07° of misorientation and a ≤30% smooth modulation of
  > rill contrast with aspect, both measured and documented rather than papered over;
- a **bedding term** with regional strike/dip hashed from an 819.2 m lattice, strike from a
  compile-time table of 16 integer direction pairs. **Built** (`voxelcore/detail_bedding.h`, both
  the 2D surface displacement and the Phase-4 3D form, sharing one hashed field so they cannot
  independently drift apart), **not yet wired into `evalSurface`/`stratigraphyAt`**.

Net detail envelope drops from ~15.7 m gated (`amplifier.cpp:680`, was `:497-499`) to ~3.0 m — **the
bound tightens**, which streaming feels as more effective trims. **Not yet true of the shipped
binary**: the envelope is still ~15.7 m today because the old octaves have not been deleted (see
above); this is the target once the swap lands.

**3d. Bounded 3D density — overhangs.**

> **What it buys:** cliffs you can stand under. A heightfield cannot produce an overhang, so
> today every rock face is a slope; this gives ledged faces, recesses and undercut noses. This
> changes the *category* of what the terrain can express, which is the whole point of a voxel
> medium.

A heightmap gives no overhang information, but real overhangs are not arbitrary — they are
consequences of *structure*: bedding planes in layered rock, differential weathering of hard and
soft beds, joint-controlled chimneys, undercut banks. All are functions of surface, slope, aspect,
lithology and drainage, which we have. The 3D pass does not invent information; it expresses
structure the heightfield already implies.

`stratigraphyAt` (`amplifier.cpp:1384`, was `:1101-1119`) gains a displacement `D(x,y,z)` with a compile-time
envelope `|D| ≤ 700 mm`, identically zero unless **both** gates pass: the voxel is within ±700 mm
of the surface (outside that band `D` cannot flip the test, so skipping is *exact*, not
approximate), and column slope exceeds ~60% grade. `D` = a 3D bedding term sharing the §3c
strike/dip field (so a recessed weak bed under a protruding resistant one *is* an overhang) plus a
rock-gated `valueNoise3` pocket term.

Cost: ~**+50–80% `VoxelizeMain` on a cliff-dominated chunk, +5–10% world-average**. Affordable
*with* both gates and rejected without them — an ungated volumetric term roughly doubles
voxelisation everywhere. Bounds survive by widening by the constant 700 mm; the three-reason air
enumeration at `amplifier.h:239-264` (was `:167-196`) gains a fourth reason with a constant bound.

River-bank undercuts are **deferred** until the flow plane ships — a client-side proxy would need
non-local flow, precisely the O(1) collision the reconciliation doc rules out. It drops into the
same `D` band later as a pure additive term.

---

## Production: unbounded world, on-demand serving

| Piece | Where | Why not the other option |
|---|---|---|
| Coarse diffusion tile | Server GPU | Already true; diffusion is not bit-deterministic across GPUs. |
| Geomorphic bake | Server GPU | Floats and iteration ⇒ not reproducible across vendors. Two clients baking locally would compute different collision. Also: compute once, serve every player who ever visits. |
| 3D density + microrelief | Client | Per-voxel, integer, identical everywhere, must answer point queries for collision and digging. |

**Latency.** Tile = 15.36 km; coarse generation **22.5 s/tile on a 4090**
(`terrain-service/docs/diffusion-bringup.md:277`); bake was estimated at ≈1.5 s/tile — **measured
since, at the shipped 1.875 m/px scale, ≈165 CPU-s/tile** (`docs/vxtl-v2-format.md` §1; open risk
#1 above has the attribution). That is a two-order-of-magnitude miss on the original estimate, and
`docs/vxtl-v2-format.md` §1 point 3 already flags the consequence: **"the bake stops being free"** —
at 20–40 s/tile in production (the parallel share of the 165 CPU-s on a GPU), advancing the frontier
by one tile (3 bakes) becomes comparable to its 5 coarse tiles at 22.5 s, not the rounding error the
numbers below assume. Fine bake for tile *T* needs the coarse 3×3 ring. Prefetch **coarse ring
radius 2 tiles** (±30.7 km) and **fine ring radius 1** (±15.4 km), so bake dependencies are always
resident — **ring sizing should be re-derived against the corrected bake cost**, per the frozen
spec's own note, rather than inherited from the ≈5 s/tile assumption below.

Moving one tile costs 5 coarse (≈112 s) + 3 bakes (≈5 s **assumed here — see the correction above:
plausibly ≈60–120 s once the bake's real cost is priced in**).

| Player speed | Time to cross a tile | Verdict |
|---|---|---|
| 20 m/s (ground) | 768 s | trivial, ~7× margin on one worker |
| 100 m/s (flight) | 154 s | one worker holds with ~25% headroom; run two |

**Block-until-ready never stalls a moving player.** Only first spawn and teleport into virgin
terrain stall, and both get an ordinary loading screen — terrain is a pure function of the seed, so
the server can pre-warm any fast-travel destination. If the frontier starves, the fallback is a
soft speed cap or an "unexplored region" boundary, **never** a procedural fallback: two clients
with different tile availability would compute different collision, which is a desync.

**Economic risk to watch.** A player flying continuously through virgin terrain consumes ≈0.76 GPU
(112 s per 154 s of travel) ≈ **$0.38/hr at $0.5/GPU-hr**. That is per *frontier*, not per player —
tiles are cached forever and shared, so cost tracks exploration area and collapses as the explored
set saturates. Mitigations in order: pre-warm spawn regions and corridors; cap speed in
never-visited territory; batch coarse generation across nearby frontier requests.

**Storage.** The ~9.5 MB/tile figure below was sized against the original 3.75 m/8 MB fine tier; at
the **shipped 1.875 m/px tier** `docs/vxtl-v2-format.md` records **~106 KB/km²** directly (~21–25 MB
of the ~25 MB/tile total is the fine plane), which over a 236 km² tile is closer to **~25 MB/tile**
than 9.5. At ~106 KB/km²: 1 M km² ≈ **106 GB**, 100 M km² ≈ **10.6 TB** — roughly 2.6× the numbers
below, still immutable content-addressed blobs, so CDN caching is still free, just of more of it.
Client cache reuses the existing layout (`cache.py:20-27`) with three additions: **sub-block
granularity** (a 2 km corridor caches ~10 blocks, now ~4 MB at 1.875 m/px rather than the 1.3 MB
figure below); **LRU with a configurable budget**, default 8–16 GB — re-derive the tile count and
area at the corrected per-tile size rather than reusing 1,700 tiles / 400,000 km² below, which
assumed the smaller tier; and **validate, never trust** — the server pins the fine `provider_id` in
the handshake and sends per-tile digests, and `EditLog::checkProvider()` already refuses replay on
mismatch. Client RAM at 4 km view distance ≈ **40 MB** (this estimate should also be re-checked
against the larger per-block size).

---

## Phasing

Each client phase that changes `evalSurface`'s output is one `kWorldGenVersion` bump (`core.h:54`,
currently **9**, was `:44`/currently 8 in the original draft of this plan), one
`VXC_WORLDGEN_VERSION_USH` bump, prebuilt SPIR-V respin, golden regeneration, and `vxc_gpu` digest
parity on both vendors. Saved edit logs invalidate each time — batch aggressively. **Correction to
this rule, learned in Phase 2:** *fine-tier ingestion did not need a bump.* `FineTileSampler` is an
`ITileSampler` like the coarse sampler, and `Amplifier`'s carrier is pixel-size-agnostic by
construction (`carrier.h:258`), so swapping which tile source an `Amplifier` reads from is not a
change to `evalSurface`'s code — see the status note at the top of this document. The version bump
is earned by wiring new *terms* into `evalSurface` (Phase 3), not by changing where the control
lattice comes from.

**Phase 0 — instrumentation. LANDED (no version bump).** `voxel-core/bench/terrainprobe.cpp` gains
a carrier-only mode, a **seam scan**, a **direct gain-step** readout off the tile raster, and
**directional roughness**. Baselines recorded below. *This is the phase that makes every later
phase verifiable instead of arguable.*

**Phase 1 — v9: C² carrier + continuous conditioning, on the existing 30 m tier. LANDED, PR #171.**
B-spline carrier, analytic slope and curvature, the mm/m renormalisation of
`slopeScaleQ10`/`microScaleQ10`/`classifyBiome`/topsoil, faded climate + ecotone dither, rewritten
`surfaceBoundsMm`, corner cap 16→34 (since raised again to 64, see open risk #4). **Deliberately
before the bake:** it de-risked the spline fixed-point maths and killed mechanisms 1–3 at their
worst scale, so it was also the fastest visible win. In parallel, the bake was prototyped in Python
— the ≈1.5 s/tile estimate it was meant to confirm turned out to be off by more than an order of
magnitude (see open risk #1).

**Phase 2 — bake + `.vxtl` v2 + fine-tier ingestion. LANDED, PR #176 (server) plus subsequent client
work merged into `claude/phase3-integration`.** Server: `terrain_service/bake/` (`flow.py`,
`incise.py`, `thermal.py`, `noise.py`, `pipeline.py`), `tile_codec.py` v2, hydrology pyramid in
`pipeline.py`/`cache.py`/`pregen.py`. Client: v2 parse + block index in `tilestore.cpp`,
`FineTileSampler`, the residency gate and prefetch ring in `tilestreaming.h/.cpp` and
`VoxelFineTileStreamer`. **Did not need a `kWorldGenVersion` bump** — see the correction above; it
landed without one, and worldgen is still v9. The fine tier shipped at **1.875 m/px (scale 16)**,
not the 3.75 m recommended in the original draft of this section — see open risk #4, now resolved
as a decision.

**Phase 3 — detail rework. IN PROGRESS, not yet wired.** Delete landform octaves, add the
continuation-ladder octave, rill term, curvature gate, bedding term; calibrate amplitudes off the
fine tier's measured `S2(7.5 m)`. As of this revision: the carrier is extracted to `carrier.h` with
analytic curvature added and tested; `detail_rill.h` and `detail_bedding.h` exist as standalone,
bounded, tested functions; **none of the three — curvature gate, rill term, bedding term — is wired
into `evalSurface` yet**, the old landform octaves have not been deleted, and the version has not
moved past 9. `vxc_terrainprobe`'s new drainage-connectivity/curvature/anisotropy metrics
(§3c above) already show, on the *shipped* v9 field, exactly the failure this phase exists to fix.

**Phase 4 — 3D density band. NOT STARTED.** `ColumnSample` widening, displaced-depth
`stratigraphyAt`, `valueNoise3` both sides, bound and brick-range widening with static_asserts.
`detail_bedding.h`'s 3D form (`beddingDisplacement3Mm`) already exists as a function, built
alongside the 2D term in the same commit, sharing one hashed field so the two cannot independently
drift apart — but nothing in `stratigraphyAt` calls it yet.

---

## Verification

Verify by instrument, not by eye — `amplifier.cpp:229-266` is a worked example of the probe
catching what screenshots did not.

Run: `vxc_terrainprobe <tiledir> 20260719 -84480 53760 2000`
(the shipped tile set is `tile-cache/terrain-diffusion-unlabeled-3e11cf157a836c70/000000000135276f/s1`;
`-84480,53760` is the tile-coverage centre — a run on the flat fallback plane is worthless).

### Phase 0 baselines, measured 2026-07-28 on real tiles (v8)

| Metric | v8 baseline | **v9 measured** | Target |
|---|---|---|---|
| Seam ratio, carrier only | 8× @0.1 m → 950× @12.8 m (interior floor 0.48 mm) | **0.86–1.26, no trend in lag** | < 1.2 at all lags |
| Seam ratio, amplified | 3.28 / 1.61 / 1.23 @ 0.1 / 0.2 / 0.4 m | **1.09 / 1.03 / 1.00** (max 1.09 at any lag) | < 1.2 at all lags |
| Gain step (envelope) | median 150–310 mm, p90 770–1075, max 2302 | **median 0 mm, p90 7, max 14 — and the mid-cell control reads the same** | ~0 (continuous by construction) |
| Material boundaries on the grid | **89.1%** (49 of 55, vs 1.0% by chance = 89× excess) | **0.0%** (0 of 28) | ≈ chance |
| Directional across/along | 0.97–1.00 (isotropic) | 0.95–1.00 (unchanged — Phase 3 owns this) | > 1 on ≥20% grades |
| `S2` local H @0.1–0.2 m | 1.404 | unchanged (Phase 3 owns this) | H ∈ [0.6, 1.0] per decade |
| Terrace runs | mean 2.71 vox, 0.5% in runs ≥20 | mean 2.76, 0.1% | no regression |

**Multi-site confirmation (v9, five sites, 2 km transects).** Both fixed mechanisms hold across
the whole grade range the shipped tile set offers, not just at the spawn point:

| Site (m) | grade | seam ratio @0.1 / 0.2 / 0.4 m | gain step median / max |
|---|---|---|---|
| −110000, 85000 | 5.6% | 1.05 / 0.97 / 0.93 | 0 mm / 7 mm |
| −84480, 53760 | 7.7% | 1.09 / 1.03 / 1.00 | 0 mm / 14 mm |
| −95000, 70000 | 15.1% | 1.05 / 1.04 / 1.04 | 0 mm / 7 mm |
| −70000, 25000 | 64.6% | 1.10 / 1.13 / 1.11 | 4 mm / 18 mm |
| −65000, 60000 | 116.6% | 0.92 / 0.90 / 0.93 | 0 mm / 14 mm |

Worst seam ratio anywhere is 1.13 against a bar of 1.2; worst gain step is 18 mm against v8's
median of 150–310 mm and max of 2302 mm.

The carrier-only residual of ~1.2 at 1.6–12.8 m lags is **not** a crease: it is flat in lag,
whereas a slope discontinuity grows linearly with lag (v8 went 8→950 by doubling). It is the
C² spline's own curvature variation near knots, and the amplified surface — what is actually
rendered — stays at ≤1.09 everywhere.

The v2 octave work already fixed most of the terracing; the remaining smooth-band defect is
confined to the 0.1–0.2 m lag, plus a spectral dip at 3.2 m (local H = 0.348). **Phase 3 owns
both**, along with the still-isotropic roughness.

Cross-checked with `vxc_climateprobe` on real tiles that the mm/px → mm/m change is a pure unit
conversion and not a retune: median slope 7000 mm/px → 233 mm/m and the cliff gate 21000 → 700,
both exactly ÷30, with the biome census unchanged (OCEAN, GRASSLAND and BARE_ROCK identical to
the column; everything else differs by ≤4 columns in 147,456, i.e. 0.003%).

### Other gates

- **Determinism:** `vxc_gpu` digest parity on AMD and NVIDIA every phase; Phase 4 must compare the
  full `VoxelizeMain` cell buffer, since that pass now hashes per voxel.

  > **Read this before trusting a `vxc_gpu` result.** Two traps were found and fixed in Phase 1,
  > and both made the gate report something other than what it was testing.
  >
  > 1. **`vxc_gpu` loads `voxel-core/shaders/prebuilt/`, not `build/shaders/`.**
  >    `tools/compile-shaders.ps1` used to write only the latter, so editing the shader and
  >    re-running the gate compared *months-old bytecode* against fresh CPU code. The script now
  >    takes `-UpdatePrebuilt` and warns loudly when the two differ. The prebuilt `.spv` are
  >    **committed artifacts** — respin and commit them with any shader change.
  > 2. **The harness never set `params.CoarseScale`**, which is a multiplier (level 0 = 1, not 0),
  >    so every column in every dispatch evaluated at world origin. It hid because
  >    `rasterElevationMm` clamps to each region's window, so the wrong answer still varied per
  >    region. `validateWorldGenParams` now rejects 0. Consequence while it was broken: the mesh
  >    chain had nothing to emit, so the gate reported "0 quads" and the greedy mesher was never
  >    compared at all.
  >
  > The gate now reports **total** mismatches and the share of columns affected (the printed list
  > is still capped at 20 — previously the cap *was* the reported number, so 49,141 mismatches
  > across 100% of columns printed as "20"), a per-region breakdown, and a per-region first-column
  > dump. It also has a third fixture region at **all-positive** coordinates; there was none
  > before, which is why "it only breaks at negative coordinates" stayed plausible for as long as
  > it did.
- **Bounds:** adversarial sweep of real columns against the new `surfaceUpperBoundMm` /
  `solidBelowBoundMm` every phase. A bound violation is a hole in the world.

  **Bound soundness — v9 measured on REAL tiles, 0 violations.** `vxc_terrainprobe` gained a
  `boundSweep` that walks every level of the ring cascade over the loaded tile set, densely samples
  each footprint, and checks all three bounds. This matters because `test_amplifier.cpp`'s existing
  `amplifier_surface_bound_adversarial` runs on `SyntheticTileSampler` only (its own FOLLOW-UP note
  asks for more), and real diffusion tiles are a different shape of input — kilometres of near-flat
  ocean, and cliffs where the 30 m raster steps hard. v9's bound is a Lipschitz envelope around one
  centre evaluation, so its tightness depends on the footprint-to-relief ratio and synthetic
  coverage does not transfer for free.

  | level | footprints | declined | mean upper slack | mean lower slack | violations |
  |---|---|---|---|---|---|
  | 0 | 173,889 | 0 | 5.48 m | 5.50 m | **0** |
  | 1 | 43,681 | 0 | 5.94 m | 5.96 m | **0** |
  | 2 | 10,816 | 0 | 7.16 m | 7.19 m | **0** |
  | 3 | 2,704 | 0 | 9.92 m | 9.97 m | **0** |
  | 4 | 676 | 0 | 17.31 m | 17.35 m | **0** |
  | 5 | 169 | 0 | 37.81 m | 36.35 m | **0** |

  232,935 footprints, **zero violations and zero declines** — so the bound is computed rather than
  skipped at every level the cascade uses, including level 5, which the old 16-corner cap would
  have made decline on scale-8 tiles. Slack grows 5.5 m → ~37 m from level 0 to 5, the expected
  shape for a Lipschitz bound over a growing footprint, and looseness is the *safe* direction.
  Sampling can only miss a violation, never invent one, so any reported failure is a real
  counterexample. This is a correctness check, so it is unaffected by box contention.
- **Codec:** golden fine tiles encoded in Python, decoded by the C++ parser, digest-compared; plus
  a sample-for-sample check that C++ and Python B-spline evaluations agree.
- **Streaming:** the sanctioned flight leg (`tools/voxel-run-flight-leg.ps1` +
  `voxel-leg-summary.ps1`, spawn `-84480,53760`, `line` flight) after Phases 2 and 4, watching
  flight-phase hole distribution — not `holes(final)`, which cannot decide a latency question.

  > **Still unrun for v9**, because `voxel-run-flight-leg.ps1` correctly refuses to start while
  > another session holds the box. When it is run, compare against a fresh v8 leg rather than the
  > recorded numbers, since v9 changes chunk admission.
  >
  > **But the risk it was standing in for is now retired directly, and better.** The concern was
  > that the rewritten `surfaceBoundsMm` might be too tight, which shows up as holes under motion.
  > A flight leg only finds such a bound if the player happens to fly past one; an adversarial
  > sweep tests the property itself. `vxc_terrainprobe` now does that over **real tiles** — see
  > "Bound soundness" below. The existing `amplifier_surface_bound_adversarial` test covers only
  > `SyntheticTileSampler`, which is the gap that mattered.
- **Perf:** `vxc_bench` amplify-stage regression against the +5–10% world-average budget in Phase 4.

  > ⚠️ **THESE TIMINGS ARE CONTAMINATED AND MUST BE RE-RUN.** Every number in the table below was
  > measured while another session's `UnrealEditor-Cmd` held the box (a `pr149-water` parity run,
  > ~8,500 s of CPU and 5 GB resident). Both arms ran under contention but not necessarily *equal*
  > contention, so neither the absolutes nor the ratio are trustworthy — a contended run reads
  > exactly like a slow configuration, which is the standing lesson in
  > `docs/measurements/s1-close-2026-07-27.txt`.
  >
  > What does still stand is the *correctness* claim: the bench digest is `ad1c1e8e8b5ba749`
  > across the naive and both cached variants, so the block memos are bit-identical. The
  > *direction* (16 per-point memo probes per column cost more than one block probe) is
  > near-certain. The magnitudes are not. Re-run on a quiet box before quoting them.

  **The mechanism is proved by COUNTING instead, which contention cannot touch.** Wall-clock was
  the wrong instrument for the claim in the first place: the block memos exist to turn 16
  per-column tile probes into 1, and that is a count. Build with `-DVXC_MEMO_STATS=ON` (off by
  default — it adds a `thread_local` increment to the hottest read in worldgen, and the point is to
  measure the code rather than the instrument) and `vxc_bench` reports:

  | brick | columns | stencil hit rate | elev probes/col | vs 16/col before |
  |---|---|---|---|---|
  | 8³ | 10,240,000 | **99.979%** | 0.0034 | **4,745×** fewer |
  | 16³ | 18,534,400 | **99.982%** | 0.0029 | **5,589×** fewer |

  The counterfactual is exact rather than estimated: before the block memo, `evalSurface` called
  `cachedElevationMm` sixteen times per column, by construction. A 3.2 m chunk sits inside one 30 m
  cell, so essentially every column in it wants the same sixteen control points — hence the ~99.98%
  hit rate. The digest is unchanged under instrumentation, so the counters do not perturb worldgen.

  This settles *whether the mechanism works*. It does not settle *whether the saving is worth
  anything in time* — that still needs a quiet box.

  **Phase 1 measured (radius 128, brick 8³, A/B against a v8 worktree):**

  | | v8 | v9 naive | v9 shipped |
  |---|---|---|---|
  | amplify | 3321 ms | 6616 ms (**2.00×**) | **3892 ms (1.17×)** |
  | total | 6126 ms | 9365 ms (1.53×) | **6627 ms (1.08×)** |

  The naive column reads 16 control points per column through the per-point elevation memo where
  v8's bilinear read 4, and sixteen hash-and-compare probes per column is what the 2× cost me.
  The reads were nearly all memo *hits* — the probing was the cost, not the sampling. Fixed by
  memoizing the 4×4 stencil as a **block** keyed on the cell (`cachedStencil`), plus the same
  treatment for the climate 2×2 (`cachedClimateQuad`): a 3.2 m chunk sits inside one 30 m cell,
  so every column in it wants the same sixteen control points. Both are bit-identical by
  construction and the bench digest (`ad1c1e8e8b5ba749`) is unchanged across all three variants,
  which is what proves it.

  The residual +17% is the honest cost of a 4×4 cubic evaluation plus an analytic gradient over a
  2×2 bilinear. Note this is the *CPU* amplifier, which under ADR-0006 serves sparse queries
  (raycast, dig, collision) rather than streaming generation.

---

## Appendix — offline single-player (not scheduled)

**Possible, with no changes to the client, wire format, or amplifier.** The bake's output is bytes
in a content-addressed cache and nothing downstream cares who produced them, so "offline
single-player" is the same pipeline with the pod on the player's machine. Single-player also
*relaxes the hard constraint*: cross-machine bit-exactness exists to keep two clients agreeing on
collision, and with one client the bake need only be self-consistent on that machine across
sessions — satisfied trivially by caching, since the cached bytes become authority once written.

**Tier A — pre-baked bounded world (recommended, near-zero engineering).** Run `pregen.py` offline,
ship the output. 7×7 tiles = **107 km square for ~470 MB**; 13×13 = **200 km square for ~1.6 GB**.
No model on the player's machine, no PyTorch dependency, no VRAM floor, no first-launch wait,
identical world for everyone, and it needs nothing this plan does not already build.

**Tier B — unbounded local generation.** Real, but it costs: native ONNX inference (all three
models are already exported under `terrain-diffusion/onnx_export/`, and `trace_FORMAT.md` documents
a C++ `WorldPipeline` port with single-tile parity to 1.7e-6 — the unported piece is the
InfiniteTensor tile scheduler, i.e. the cross-tile windowing you cannot skip; *confirm where that
port's source lives, it is not under `voxelsim/` or `MiraThalVoxelBake/`*); porting the bake off
torch (thermal and carve are trivially parallel, flow accumulation needs a sort/scan,
priority-flood is best left on CPU); and budgeting the GPU against rendering (assume 60–90 s/tile
when sharing with the renderer).

**The consequence to accept in Tier B:** locally-baked tiles are not reproducible from the seed
alone, so **the baked cache becomes part of the save** — growing ~25 MB per tile visited at the
shipped 1.875 m/px tier (was estimated at ~9.5 MB against the smaller 3.75 m tier; ~2.5 GB after
100 tiles). Give locally-baked worlds their own `provider_id` namespace, never re-bake an existing
tile, and design the bake for order-determinism anyway (fixed iteration counts, no order-dependent
atomics) so cache loss usually reproduces rather than silently changing the world.

**Hardware floor, corrected.** The model plus bake grids wants **~3 GB** in flight per tile, not
the ~0.5 GB this appendix originally assumed (`pipeline.py`'s `estimate_peak_bytes`, open risk #6
above — a counted figure over the padded 9216² domain, not a timing) — beyond the game's own VRAM
usage that is a materially higher floor than stated here before.

The **"CPU path is hours per tile"** claim needs separating into what was actually measured and
what was actually warned about, because they are not the same thing. `terrain-service/docs/
diffusion-bringup.md:71` warns about **cloud-pod inference silently falling back to CPU** when the
Vast image's bundled torch build doesn't match the host driver — `torch.cuda.is_available()`
returns `False` with no error, and diffusion inference runs on CPU "at 4090 prices," i.e. billed
GPU-hours for CPU work. That is a real, measured failure mode, but it is about a misconfigured
*rented* GPU box, not a statement about commodity CPU-only hardware.

Separately, and on a genuinely GPU-less dev box (`torch 2.12.1+cpu`, onnxruntime CPU-only, no
`nvidia-smi`), **CPU diffusion inference was measured at ~179 s/tile** for a 512² native tile — one
cold tile on roughly 8 threads. That contradicts "hours per tile" by roughly an order of magnitude
for the diffusion model itself, though it says nothing about the *bake*, which is a separate CPU
cost (`pipeline.py`'s measured ~165 CPU-s/tile at production scale, open risk #1) that would stack
on top of it in an unbounded Tier B. **This directly affects the claim two paragraphs up** ("there
is no low-end fallback on hardware grounds"): a ~179 s/tile CPU diffusion cost plus a ~165 CPU-s
bake is slow — several minutes per frontier tile — but it is not the "no fallback at all" situation
the original "hours" figure implied, and it is worth re-costing Tier B's CPU-only floor properly
rather than treating it as ruled out. On a mid-range GPU, on-demand exploration may still stop
keeping up — which puts you back at Tier A with a larger shipped region — but that is now a
capacity argument to re-measure, not a hardware-impossibility argument to assert.

---

## Open risks

1. **Bake cost, re-measured at the shipped 1.875 m/px scale.** The original prototype figure
   (40.6 CPU-s/tile, 4096² at 3.75 m/px) is superseded: the shipped tier is 8192² (4× the cells),
   and `docs/vxtl-v2-format.md` records a measured **~165 CPU-s/tile** production figure at that
   scale. Attributed, not just totalled: `pipeline.py`'s B2 module owner independently measured
   fill 12.5 + D8 2.2 + accumulate 15.8 ≈ **30 CPU-s** at full 8192² — so B2 (flow routing) is *not*
   the bottleneck at production scale; thermal relaxation still is, and it is still the stage a GPU
   eats. The on-demand argument is unaffected either way — coarse diffusion at 22.5 s/tile per the
   *actually measured* number (`terrain-service/docs/diffusion-bringup.md:277`) still dominates a
   frontier-tile's cost (5 coarse tiles ≈ 112 s vs 3 bakes ≈ 495 CPU-s, and the bake parallelises).
2. ~~Compressed tile size is modelled, not measured.~~ **RETIRED**, and the number moved with the
   resolution decision. Measured at 3.75 m/px on three real tiles with
   `terrain-service/tools/measure_fine_tier_size.py`, 8 MB/tile stood for correlated detail there;
   at the **shipped 1.875 m/px tier** `docs/vxtl-v2-format.md` records **~21–25 MB/tile** (4× the
   pixels, ~1 bit/px cheaper — see the size section above). The live risk is unchanged in kind: the
   bake must not introduce uncorrelated per-pixel noise, because a few cm of white costs more than
   metres of fBm. Also still open: the codec needs the int16→int32 escape it now has
   (`resid_bits`, §5 of the frozen spec) — 73–159 residuals per tile overflow int16 even at 100 mm
   quantisation.
3. ~~`kWorldGenVersion` collision.~~ **RESOLVED**, and answered directly in the code rather than
   left as a question: `core.h:40-43` records that `claude/erosion-v7` (unmerged) already claims 7,
   this work branched from `main` at v6 and deliberately took 8 so the two land in either order
   without colliding (the check at `editlog.h` is exact equality, not a range, so the gap is
   harmless), and v9 is this plan's Phase 1. No collision is in flight.
4. ~~Fine resolution may want to be 1.875 m.~~ **RESOLVED as a decision, not a risk.** 1.875 m/px
   (scale 16) was chosen over 3.75 m deliberately (Matt, 2026-07-29) — see `docs/vxtl-v2-format.md`
   §1 — because it buys the 3.75–7.5 m band a 1.8 m player actually occupies: small stream
   channels, gullies you can climb into, cut banks, terrace risers. Costs are measured, not
   assumed: ~21–25 MB/tile compressed and ~165 CPU-s/tile to bake (vs. 8 MB and the original
   estimate at 3.75 m). `kSurfaceBoundMaxCornersPerAxis` rose again, 34 → 64, as a direct
   consequence (see 3a above).
5. **Phase 1 changed the world before the bake existed; the bake has since landed, but Phase 3 has
   not.** Terrain looks *different* from v8 — smoother carrier, no crease grid, continuous material
   boundaries — but as of this revision the sub-30 m band is still
   procedural, so it will not yet look *right*. Do not re-tune octaves in Phase 1; Phase 3 owns
   those numbers. This is now measured, not just expected — see "v9's sub-30 m band has NO
   drainage connectivity" under §3c above.
6. **NEW — peak bake memory is ~3 GB per tile in flight, not the ~0.5 GB the offline appendix
   assumed.** `pipeline.py`'s `estimate_peak_bytes` counts 8 float32 grids (carrier, roughness
   delta, slope, filled, acc, depth, eroded, relaxed) plus one int64 receiver array over the
   **padded** 9216² domain — the module docstring puts the live set at ~3 GB. This is a *counted*
   figure (bytes, not timed), so box contention cannot distort it. Size the bake pod accordingly,
   and the offline-appendix Tier B VRAM budget (below) needs revisiting against it.
7. **NEW — the ε-flat routing seam bites at ~960 m, i.e. exactly one apron width.** The Barnes
   epsilon fill's staircase across a flat runs outward from whichever border the flood reached it
   from first, so a flat **wider than the apron** can be crossed in different directions by two
   neighbouring tiles' bakes. Heights still agree to sub-ULP (well under the 100 mm wire LSB, so
   shipped elevation is unaffected), but flow accumulation — and therefore incision — need not.
   At 1.875 m/px, "wider than the apron" is 512 px: an ordinary lake, floodplain or playa, not a
   corner case. `pipeline.py`'s `HYDROLOGY_RESIDUALS` #6 names this and `BakeResult` reports
   `basin_exceeds_apron` per tile so it is at least cheaply detectable; fixing it wants the same
   shape of answer as the hydrology pyramid (a world-anchored, shared routing decision), which is
   not wired for this case.
8. **NEW — exploration order is permanently baked into an on-demand world.** The hydrology
   pyramid's superblocks are built from whatever coarse tiles exist when the first tile inside them
   is baked (`HYDROLOGY_RESIDUALS` #1); a river entering from a never-generated region contributes
   nothing at bake time, and because a shipped tile is never regenerated, later exploration does
   **not** retroactively correct it. `pregen.py --mode bake` sidesteps this by building every
   superblock over the requested radius before baking anything, so a pre-generated world is
   order-independent; a live on-demand frontier is not. This is as much a policy question (do
   players' arrival order get to matter to the river network they see) as a numerical one, and it
   is the largest unresolved residual in the hydrology pyramid.
