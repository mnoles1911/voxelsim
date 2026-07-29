# Terrain amplification redesign — 30 m diffusion tiles → 10 cm voxels

**Status:** planned and approved (Matt, 2026-07-28). **Phases 0 and 1 are LANDED** on
`claude/terrain-amplification-redesign` (`0e55c4c`, `cf9aa46`, `e9fb524`) — worldgen is at v9,
`ctest` is green, and `vxc_gpu` passes bit-exact on AMD. Phases 2–4 are not started.
**Supersedes the park decision** in `docs/terrain-amplification-reconciliation.md` — see
"How this unparks the 2026-07-24 proposal" below.

## Prerequisite reading (an executing session starts cold — read these first)

| File | Why |
|---|---|
| `docs/terrain-amplification-reconciliation.md` | Why the 2026-07-24 proposal was parked. This plan's central move is the answer to it. |
| `docs/research/terrain-amplification-design-doc.md` | The original stage-by-stage proposal. Stages §4, §5, §7, §8 are reused nearly as written. |
| `docs/determinism.md` | The float ban and CPU/GPU mirror contract that constrain every client-side change. |
| `voxel-core/src/amplifier.cpp:223-500` | The v2 octave table and its rationale comment — a worked example of diagnosing this system with measurements rather than screenshots. |
| `docs/voxel-earth-implementation-plan.md` §2 | Doctrine: determinism boundary, budgets, the "generated once, cached forever" rule this plan leans on. |

## Critical files

- `voxel-core/src/amplifier.cpp` — the height/material synthesis being replaced
- `voxel-core/shaders/worldgen.ush` — its bit-exact HLSL mirror; every change lands twice
- `voxel-core/include/voxelcore/{amplifier.h,biome.h,hash.h,tiles.h,tilestore.h}`
- `voxel-core/bench/terrainprobe.cpp` — the verification instrument (Phase 0, landed)
- `terrain-service/terrain_service/{tile_codec.py,providers/diffusion.py,cache.py,pregen.py}`
- `voxel-core/src/tilestore.cpp` — tile parse, gains the v2 fine-tier decode

---

## Why this work exists

Terrain below 30 m reads as noisy and random, and the 30 m tile grid is plainly visible as
squares whose edges do not flow into their neighbours. Both were diagnosed to specific code and
then **measured** with `vxc_terrainprobe`; neither is a tuning problem.

### What is actually wrong

`Amplifier::evalSurface` (`amplifier.cpp:672-717`) computes
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
| 1 | `bilinearBaseMm` is C⁰ but not C¹ — the surface *gradient* steps at every pixel line. Invisible in the height, glaring under directional light. | `amplifier.cpp:357-361` | Carrier-only seam scan: straddle/interior `S2` ratio **8× at 0.1 m rising to 950× at 12.8 m**, doubling exactly with lag over an interior floor of 0.48 mm. Linear-in-lag growth over a zero floor is the signature of a slope discontinuity. On the amplified surface it survives at **3.28× / 1.61× / 1.23×** for 0.1 / 0.2 / 0.4 m lags and is masked beyond — i.e. visible exactly at voxel scale. |
| 2 | `tileSlopeMmPerPx` is a forward difference **constant over the whole cell**, driving `slopeScaleQ10` (0.25×…4.0×), which multiplies landform detail amplitude. The same noise field gets a step-discontinuous gain across every 30 m line. | `amplifier.cpp:363-365, 688, 316-318` | Detail envelope steps by a **median of 150–310 mm, p90 770–1075 mm, max 2302 mm** across every boundary (four sites, 5.6%–118% grade). That is 1.5–3 voxels of texture amplitude appearing along a dead-straight 30 m line as the *typical* case. |
| 3 | Climate is read nearest-pixel, so `classifyBiome`, `surfaceMat` and topsoil depth flip exactly on the 30 m grid — blocky material patches. | `amplifier.cpp:990`, `biome.h:141-194` | Not yet instrumented. |

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

`amplifier.cpp:262-266` states the governing assumption: *"30 m is the end of that cascade and no
finer model exists. Everything below 30 m is procedural and always will be."* The first clause is
true. **The second is what this plan overturns** — not by training a finer model, but by
*simulating* the missing band once, offline, and shipping it as data.

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

The one unbounded dependency is flow accumulation. Handle it with a **hydrology pyramid mirroring
the model's own hierarchy**: accumulate at the coarse-map level (7.7 km/px, where the coarse model
is tiny and huge areas cost almost nothing), then at 30 m over the local neighbourhood, and inject
upstream accumulation as inflow boundary conditions at the fine domain edge. Residual defect: fine
rills meandering more than 960 m before rejoining a coarse-resolved channel may kink at a tile
edge — sub-metre, 1–2 px wide, regression-testable via the design doc's §11 checklist.

### Layer 2 — wire format (`.vxtl` v2)

Keep the s1 tile byte-identical. **Redefine the s8 slot** to hold one fine container per coarse
tile coordinate — safe, since nothing was ever generated at scale 8 (`tile_codec.py:41`,
`tilestore.h:66`). The addressing change rolls `provider_id` through `_tile_format_fingerprint`,
exactly as that mechanism is designed to.

```
Header: extends v1 <4sHQiiBH> with
  version=2, block_log2=8 (256×256 fine px = 960 m blocks, 16×16 = 256 per tile),
  predictor, quant, codec, bake_ver, flags, base_offset_mm i32, clamp, n_sections
Sections: ELEV_INDEX (256 × {offset, comp_len, mode, const}) + ELEV_DATA
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

`kSurfaceBoundMaxCornersPerAxis` (`amplifier.h:87`) must rise **16 → 34** for the dilated stencil
(34² int64 = 9.2 KB stack). This also discharges the standing warning at `amplifier.h:78-86` that
the bound silently degrades at level ≥5 on scale-8 tiles.

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

Where the fine tier is present, **delete the 25.6 m and 6.4 m landform octaves**
(`amplifier.cpp:276-277`); they would fight measured landforms with hash noise. Then:

- a continuation ladder `{3200, 1600, 400, 200}` mm, amplitudes **set by probe measurement, not
  taste**, placed so `S2(d)` continues at H≈0.8 through 7.5 m;
- a **curvature gate** — convex crests roughen ~1.75×, concave hollows smooth to ~0.5× (colluvial
  fill). This is the ridge-sharp/valley-smooth asymmetry that reads as shaped ground;
- a **rill/flute term** with the domain anisotropically scaled by the local gradient frame,
  ~1.6 m across-slope and ~13 m along-slope. The frame uses an octagonal norm (`max + min/2`, ≤12%
  direction error, which reads as natural wavelength variation) — no sqrt, no trig. Its gate goes
  to zero below ~10% grade, which also neutralises the frame's instability where the gradient
  vanishes. Domain warping cannot change value noise's output *range*, so this costs the bound
  exactly its gated amplitude;
- a **bedding term** with regional strike/dip hashed from an 819.2 m lattice, strike from a
  compile-time table of 16 integer direction pairs.

Net detail envelope drops from ~15.7 m gated (`amplifier.cpp:497-499`) to ~3.0 m — **the bound
tightens**, which streaming feels as more effective trims.

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

`stratigraphyAt` (`amplifier.cpp:1101-1119`) gains a displacement `D(x,y,z)` with a compile-time
envelope `|D| ≤ 700 mm`, identically zero unless **both** gates pass: the voxel is within ±700 mm
of the surface (outside that band `D` cannot flip the test, so skipping is *exact*, not
approximate), and column slope exceeds ~60% grade. `D` = a 3D bedding term sharing the §3c
strike/dip field (so a recessed weak bed under a protruding resistant one *is* an overhang) plus a
rock-gated `valueNoise3` pocket term.

Cost: ~**+50–80% `VoxelizeMain` on a cliff-dominated chunk, +5–10% world-average**. Affordable
*with* both gates and rejected without them — an ungated volumetric term roughly doubles
voxelisation everywhere. Bounds survive by widening by the constant 700 mm; the three-reason air
enumeration at `amplifier.h:167-196` gains a fourth reason with a constant bound.

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
(`terrain-service/docs/diffusion-bringup.md:277`); bake ≈1.5 s/tile (estimate — measure in Phase 1);
fine bake for tile *T* needs the coarse 3×3 ring. Prefetch **coarse ring radius 2 tiles** (±30.7 km)
and **fine ring radius 1** (±15.4 km), so bake dependencies are always resident. Moving one tile
costs 5 coarse (≈112 s) + 3 bakes (≈5 s).

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

**Storage.** Server ~9.5 MB/tile covering 236 km²: 1 M km² ≈ 40 GB, 100 M km² ≈ 3.8 TB — immutable
content-addressed blobs, so CDN caching is free. Client cache reuses the existing layout
(`cache.py:20-27`) with three additions: **sub-block granularity** (a 2 km corridor caches ~10
blocks / 1.3 MB, not 8 MB); **LRU with a configurable budget**, default 8–16 GB ≈ 1,700 tiles ≈
400,000 km², pinning the active ring; and **validate, never trust** — the server pins the fine
`provider_id` in the handshake and sends per-tile digests, and `EditLog::checkProvider()` already
refuses replay on mismatch. Client RAM at 4 km view distance ≈ **40 MB**.

---

## Phasing

Each client phase is one `kWorldGenVersion` bump (`core.h:44`, currently **8**), one
`VXC_WORLDGEN_VERSION_USH` bump, prebuilt SPIR-V respin, golden regeneration, and `vxc_gpu` digest
parity on both vendors. Saved edit logs invalidate each time — batch aggressively.

**Phase 0 — instrumentation. LANDED (no version bump).** `voxel-core/bench/terrainprobe.cpp` gains
a carrier-only mode, a **seam scan**, a **direct gain-step** readout off the tile raster, and
**directional roughness**. Baselines recorded below. *This is the phase that makes every later
phase verifiable instead of arguable.*

**Phase 1 — v9: C² carrier + continuous conditioning, on the existing 30 m tier.** B-spline
carrier, analytic slope and curvature, the mm/m renormalisation of
`slopeScaleQ10`/`microScaleQ10`/`classifyBiome`/topsoil, faded climate + ecotone dither, rewritten
`surfaceBoundsMm`, corner cap 16→34. **Deliberately before the bake:** it de-risks the spline
fixed-point maths and kills mechanisms 1–3 at their worst scale, so it is also the fastest visible
win. In parallel, prototype the bake in Python to confirm the ≈1.5 s/tile estimate.

**Phase 2 — v10: bake + `.vxtl` v2 + fine-tier ingestion.** Server: `terrain_service/bake/`
(`flow.py`, `incise.py`, `thermal.py`, `noise.py`, `pipeline.py`), `tile_codec.py` v2, delete the
bilinear scale-8 branch, hydrology pyramid in `cache.py`/`pregen.py`. Client: v2 parse + zstd +
block index in `tilestore.cpp`, fine-plane sampler, streaming residency gate and prefetch ring.

**Phase 3 — v11: detail rework.** Delete landform octaves, add the 3.2 m octave, rill term,
curvature gate, bedding term; calibrate amplitudes off the fine tier's measured `S2(7.5 m)`.

**Phase 4 — v12: 3D density band.** `ColumnSample` widening, displaced-depth `stratigraphyAt`,
`valueNoise3` both sides, bound and brick-range widening with static_asserts.

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
- **Codec:** golden fine tiles encoded in Python, decoded by the C++ parser, digest-compared; plus
  a sample-for-sample check that C++ and Python B-spline evaluations agree.
- **Streaming:** the sanctioned flight leg (`tools/voxel-run-flight-leg.ps1` +
  `voxel-leg-summary.ps1`, spawn `-84480,53760`, `line` flight) after Phases 2 and 4, watching
  flight-phase hole distribution — not `holes(final)`, which cannot decide a latency question.

  > **OUTSTANDING for Phase 1.** This leg has *not* been run for v9. It is the check that matters
  > most for the rewritten `surfaceBoundsMm` and the deleted UE-side bound, because a bound that is
  > too tight shows up as holes under motion and nowhere else. `VoxelEarthEditor` builds and the
  > CPU/GPU gate is green, but neither exercises streaming admission.
  >
  > It could not be run here: `voxel-run-flight-leg.ps1` correctly refused to start while another
  > session's `UnrealEditor-Cmd` (a `pr149-water` parity run) held the box, and a contended leg
  > reads exactly like a slow configuration. Run it when the box is free, and compare against a v8
  > leg rather than against the recorded numbers, since v9 changes chunk admission.
- **Perf:** `vxc_bench` amplify-stage regression against the +5–10% world-average budget in Phase 4.

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
alone, so **the baked cache becomes part of the save** — growing ~9.5 MB per tile visited (~1 GB
after 100 tiles). Give locally-baked worlds their own `provider_id` namespace, never re-bake an
existing tile, and design the bake for order-determinism anyway (fixed iteration counts, no
order-dependent atomics) so cache loss usually reproduces rather than silently changing the world.

**Hardware floor is the real limiter on Tier B:** the model plus ~0.5 GB of bake grids wants
~4–6 GB VRAM beyond the game's own usage, and the CPU path is hours per tile, so there is no
low-end fallback. On a mid-range GPU on-demand exploration may stop keeping up — which puts you
back at Tier A with a larger shipped region.

---

## Open risks

1. **Bake cost is estimated, not measured.** The ≈1.5 s/tile figure drives the whole on-demand
   latency argument. Measure in Phase 1 before committing to prefetch ring sizes.
2. ~~Compressed tile size is modelled, not measured.~~ **RETIRED** — measured on three real tiles
   with `terrain-service/tools/measure_fine_tier_size.py`; see the size section. 8 MB/tile stands
   for correlated detail. The live risk is now narrower and different: **the bake must not
   introduce uncorrelated per-pixel noise**, because 5 cm of white costs more than 2 m of fBm. Also
   newly open: the codec needs an int16 escape (73–159 residuals per tile overflow).
3. **`kWorldGenVersion` collision.** `core.h:42-43` notes v8 was taken to dodge a collision with an
   unmerged branch; confirm what is in flight before claiming v9–v12.
4. **Fine resolution may want to be 1.875 m.** 3.75 m is the recommendation; the format leaves
   `size` and `block_log2` free so scale 16 is a config change, not a redesign — at 4× the bytes.
   Decide after seeing Phase 3 in-engine.
5. **Phase 1 changes the world before the bake exists.** Terrain will look *different* — smoother
   carrier, no crease grid, continuous material boundaries — but the sub-30 m band is still
   procedural, so it will not yet look *right*. Do not re-tune octaves in Phase 1; Phase 3 owns
   those numbers.
