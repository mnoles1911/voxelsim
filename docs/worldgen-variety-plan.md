# Worldgen variety — making the world feel alive

> ## OUTCOME RECORDED 2026-08-05 — this plan mostly executed. Read the result, not the proposal.
>
> **Current state lives in `docs/world-generation-architecture.md` §6.** This
> file is kept because the *reasoning* and the two falsified hypotheses below
> are worth not re-running. Where it and the architecture document disagree, the
> architecture document is right.
>
> | wave | what happened |
> |---|---|
> | **1a** rebuild `synthetic_map_stats.json` from real rasters | **SHIPPED** (`8ce2890`), pinned (`49bb67b`, `41a73a4`) |
> | **1b** couple precipitation to terrain | **SHIPPED** — orographic rain shadow, correlation −0.734, mean multiplier 0.493 behind a >600 m barrier |
> | **2a** savanna's gate | **SHIPPED as worldgen v22** (`2fce31a`) — but *not* the way this plan proposed; see the correction below |
> | **2b** elevation tails | **HALF shipped.** Tails stretched (`elev_gain` 1.6). `cond_snr[0]` was tested at 0.15 and **REJECTED** — see the correction below |
> | **3a** re-measure the coarse tier | **SHIPPED** — `measurements/geomorphon-v21-2026-08-01.txt` |
> | **3b** gate the coarse shaping octaves | **NOT APPLIED.** Still open, still conditional on one more measurement |
> | **3c** unblock the PROVISIONAL amplitude | **RUN, and it said "not yet"** — see the correction below |
> | **4** landform provinces Tier 1 | **SHIPPED at `BAKE_VERSION` 7** (`4f9a6e7`) |
>
> **Two "before" numbers in this file are softer than they read.** The 7.8%
> precipitation-range figure is a real measurement. "DESERT 1.84% / SAVANNA
> 0.00%" is a classification of the *sketch* recorded in plan prose only, never
> in a measurement file — quote it as indicative. And the rebuild alone did
> **not** fix deserts: the coarse census still read DESERT 0.00% after both
> halves of Wave 1, and what made deserts exist was a later monotone remap of
> the model's output in `adapt_raster_to_tile` (`3b511e3`, `56257c8`). The world
> now measures **DESERT 9.74%, RAINFOREST 4.73%**, all eight mappable biomes
> non-zero.

## Context

The owner's complaint: the world reads as monotonous. Terrain character should
vary — large mountain ranges, cliffs, ravines, deserts, plains, temperate
forests, rolling hills — and those should occur in close proximity so a player
encounters real variety in a session rather than a continent.

Three hypotheses were tested this session. **Two were wrong and are recorded so
nobody re-runs them:**

* *"Transitions are Earth-slow, compress the conditioning."* **False.** Measured
  on the delivered coarse tiles (model output, not the sketch): temperature
  decorrelates at 23.9 km, precipitation 19.6 km, elevation 26.2 km. Earth's
  temperature never decorrelates within 17,500 km and its precipitation takes
  2,285 km. **The pipeline is already ~100x faster than Earth** and is saturated
  — median |dT| is 12.0 C over 31 km and 12.3 C over 100 km. `frequency_mult`
  also *cannot* be a variance knob: quantile matching guarantees the marginal
  regardless of frequency, which is exactly why the `frequency_mult[0]`
  1.5->0.4 experiment raised the mean (2240 m, 66% above treeline) and was
  rejected.
* *"Relief character persists too long."* **False.** Ruggedness decorrelates at
  ~6 km, faster than elevation itself.

**What is actually wrong**, all measured:

1. **The climate is one-dimensional.** Over land, temperature spans 43.9% of its
   encodable range and **precipitation spans 7.8%** (p5-p95 = 15-30 of 255). The
   world is effectively isohyetal, so every biome decision collapses onto
   temperature and elevation. You walk a thermal ladder — BEACH -> GRASSLAND ->
   TEMPERATE_FOREST -> TAIGA -> TUNDRA_ALPINE -> BARE_ROCK — quickly, and then
   walk it again. Fast transitions between few outcomes read as monotony.
2. **Relief is squeezed to the middle.** Local relief per 2 km window on land:
   p50 394 m, **96.6% above 100 m** (no plains), **1.4% above 1000 m** (no
   extremes). The sketch's own elevation table implies 22.6% of land above
   1000 m, so **the coarse model is compressing the tails**, not the sketch.
3. **Every place is shaped by identical physics.** The bake applies one global
   constant set, so similar relief + similar climate => siblings.

### The root cause of (1), and it is a stale data file

`D:\terrain-diffusion\_prep_stats.py` records that WorldClim was unreachable
when the conditioning statistics were built, so **hand-written latitude formulas
were substituted for bio_1/4/12/15**. The shipped
`data/global/synthetic_map_stats.json` was built from that fake climate. The
real WorldClim rasters are present in `data/global` (dated 2019) and unused.
`_load_stats_cache` (`synthetic_map.py:185-188`) loads it **unconditionally,
keyed on nothing**.

| | cached (fake) | real WorldClim |
|---|---|---|
| precip p25/p50/p75 | 487 / 776 / 935 mm | 296 / 588 / 1148 mm |
| precip IQR | 448 mm | **852 mm** |
| precip p5 | 318 mm | **38 mm** |
| land under 400 mm/yr | ~16% | **33.5%** |

That one file is the primary cause of the isohyetal world, and therefore of
DESERT at 1.84% and RAINFOREST at 0%.

### Decisions taken from the owner this session

* **Caused variety, not placed variety.** Variety must arrive through mechanisms
  that explain it — mountains cast rain shadows, so desert appears downwind of
  ranges. A player should be able to see why the world changed.
* **Keep the anti-terracing floor; accept that nothing is truly flat.** Contrast
  comes from the upper tail (higher peaks against hill country), not from
  manufacturing plains. This removes the whole "lower the micro floor" branch.
* **The inherited bake work is merged and committed before handoff** (Wave 0).

---

## Wave 0 — merge the verified bake work (this session, before handoff)

Three agents produced byte-identical bake optimisations in worktrees based on
`2170a61`, which predates today's ten commits. All three verified against the
same reference: re-bake tile (-5,3) and match
`sha256 dcf052013eecade47f6769eed3d355203fcb8a72525ee61d63947523aecf7ec9`,
201,367,675 bytes.

| worktree | contents | measured |
|---|---|---|
| `agent-aec55848a50ae33ab` | argsort reuse, `flow.enforce_descent`, `rec` int32, `dist`/`regional` | peak commit 7.27 -> 6.86 GiB |
| `agent-a46f028e8cc2dbc9b` | level-parallel `_profile_pass`, Newton `pow` | `_profile_pass` 39.1 -> 10.8 s wall |
| `agent-a281f7710fe57165e` | per-stage wall timing, `estimate_peak_bytes` 3.16 -> 6.33 GiB | instrumentation only |

Merge order: memory/dtype first, then `_profile_pass` (disjoint regions of
`incise.py`), then instrumentation. **Re-verify the sha256 after the merge, not
just per-worktree.** Delete `agent-a46f028e8cc2dbc9b/_bench/` (~2.4 GB untracked
scratch) before committing.

Three things to carry into the commit message:

* `B2d.stream_power`'s **CPU time goes UP** (99.8 -> 117.9 s) while its wall goes
  down — `process_time()` bills spin-wait. Anyone reading the stage table after
  this will think it regressed.
* The old B4 descent loop had a **silent 256-pass cap** with no error on
  non-convergence; synthetic trees needed 199-220. The single sweep has no cap.
* A donor **can** precede its receiver in `order` (stable argsort breaks float32
  ties by index). The sequential pass survived this silently; a naive level
  decomposition would not. Tile (-5,3) has zero such cells, so the production
  bake could never have caught it — it is handled in a sequential bucket 0 and
  proved with an adversarial tie-saturated harness.

---

## Wave 1 — rebuild the conditioning from real climate

**This is the highest-value work in the plan and it is mostly a data fix.**

### 1a. Rebuild `synthetic_map_stats.json` from the real rasters

`_compute_map_stats` (`synthetic_map.py:45-132`) already reads the correct
files; only the cache is poisoned. Rebuild it, then:

* **Add `synthetic_map_stats.json` to `DEFAULT_CONDITIONING_FILES`**
  (`terrain-service/terrain_service/providers/diffusion.py:422-428`). It is the
  derived file that decides every channel's distribution and it is **not
  currently in the identity digest** — regenerating it today changes every tile
  byte under an unchanged `provider_id`. It is a config field
  (`diffusion.py:637`), so extending it rolls the id honestly.
* Expect DESERT and RAINFOREST to become reachable (8.6% and 12.25% of Earth
  land respectively).

### 1b. Couple precipitation to terrain — the "caused variety" mechanism

`finalize_synthetic_map` (`synthetic_map.py:232-254`) couples temperature to
elevation via a real lapse rate (`:239-240`) and couples precipitation to
**nothing** — it is read at `:236` and emitted unmodified at `:254`.

`terrain-service/tools/make_conditioning.py` **already implements** the physics:
continentality (`_distance_to`, `:160-190`), a wind direction
(`wind_from_deg`, `:117`), and a marching orographic rain shadow
(`_orographic`, `:349-373`).

**Cheapest correct insertion is pointwise inside `finalize_synthetic_map`**,
where `synthetic_elev` (`:233`) and `synthetic_precip` (`:236`) are both already
in scope — a windward/lee multiplier is a two-line change and the temperature
lapse rate at `:239-240` is the template.

**Do not** reach for `set_custom_conditioning_import` as the first move. Three
traps, all load-bearing:
* Any import **disables `finalize` for all five channels**
  (`world_pipeline.py:899-903`), so importing only precipitation silently
  removes the temperature lapse rate.
* `tiff_export.py:35` applies `internal_scale = 100.0` to `temperature_std.tif`
  while `make_conditioning.py:341` already writes bio_4 (x100) units — a
  suspected **100x error**; the two comments contradict each other and it was
  not executed end to end.
* Non-local terms (distance transforms, wind marches) seam, because `finalize`
  runs per 64x64 coarse-cell window **with no halo** (`world_pipeline.py:924`).
  Widening requires sampling raw over `[i1-P, i2+P]` inside
  `_conditioning_model_input` (`:882-903`); `_sample_raw_conditioning`
  (`:821-827`) accepts arbitrary bounds and has **intentionally swapped
  coordinates** (`:823`).

### 1c. Verification — 35 seconds, no GPU pod, no full tiles

`world.coarse[:, ci0:ci1, cj0:cj1]` runs the coarse stage alone; latent and
decoder are demand-driven `InfiniteTensor`s that are never touched
(`world_pipeline.py:684-688`, `:986-992`). Canonical idiom at
`terrain_diffusion/inference/explorer/server.py:58-64`.

`terrain-service/tools/world_map.py` is the consumer and already parses biome
thresholds live out of `voxel-core/include/voxelcore/biome.h` (`:85-127`,
`classify` at `:145-166`) so it cannot drift from the client. Start the explorer
per its header (`:12-19`), then `--window`, `--style biome`, `--audition` for a
multi-seed panel. A/B recipe: two explorers on different ports with
`--kwarg` (`worldgen-levers.md:202-226`).

**Acceptance:** DESERT and RAINFOREST both non-zero; precipitation spread over
land materially above today's 7.8%; deserts visibly downwind of ranges rather
than scattered.

---

## Wave 2 — the two calibration bugs Wave 1 will expose

### 2a. SAVANNA's gate is empirically empty — a `biome.h` bug

`classifyBiome` requires `bio_1 >= 18 C` **and** `bio_4 >= 1500`
(`kBiomeTempWarmU8` / `kBiomeSeasonalHighU8`, `biome.h:120,128`). Checked
directly against the real rasters: on Earth +/-60 deg, **those never co-occur** —
max `bio_4` where `bio_1 >= 18` is 1084. Rebuilding the stats cache raises the
achievable ceiling only to ~1194.

So **no amount of conditioning work reaches SAVANNA.** Re-derive
`kBiomeSeasonalHighU8` against the real WorldClim joint distribution. Small
change, rolls `kWorldGenVersion`.

> **CORRECTION — what actually shipped, worldgen v22 (`2fce31a`).** The
> diagnosis above is right and the prescription is wrong. **No `bio_4`
> threshold fixes savanna, because `bio_4` is the wrong variable.** At
> `bio_4 >= 200` the gate calls Houston, Brisbane and Miami savanna while still
> rejecting the Serengeti, the Cerrado and Tsavo. Real savanna is a wet season
> and a dry season, which is `bio_15` (variability of monthly precipitation),
> not hot summers and cold winters.
>
> So `kBiomeSeasonalHighU8` was **deleted, not re-derived**, and
> `classifyBiome`'s third argument moved to `bio_15` with a 70% threshold —
> `kBiomePrecipSeasonalHighU8 = 89` (`biome.h:184`). The 70% was derived twice
> and agreed: `sqrt(4/8) = 70.7%` from the physical definition of a 4-wet-month
> regime, and 15.57% of Earth's land empirically against the real ~15.6%. Wire
> format unchanged; `provider_id` did not roll. Every mention of
> `kBiomeSeasonalHighU8` in this file refers to a symbol that no longer exists.
> See `measurements/biome-gates-2026-08-01.txt` §2.

### 2b. Elevation tails — the model compresses them, not the sketch

The cached table implies 22.6% of land above 1000 m; delivered is 1.4%. Likely
mechanism: `sqrt(5814) = 76.2` normalises to **+2.87 sigma**
(`world_pipeline.py:925`), well outside training, so the denoiser regresses to
the mean.

There is no existing variance knob. The lever is the 64-knot table itself, and
the cheapest hook is a **pure table edit between load (`:185`) and build
(`:227`)** — pin the sea-level knot and the land-median knot, apply a monotone
gain > 1 to the knots above. Safe by construction: the table is monotone and
`np.interp` clamps at both ends (`perlin_transform.py:45`).

Pair it with a **lower** `cond_snr[0]` (0.3 -> ~0.15) to force obedience.
**`worldgen-levers.md:108` documents `cond_snr` backwards** — `t = atan(snr)`,
`cond = cos(t)*sketch + sin(t)*noise` (`world_pipeline.py:975`, `:932`), so
**lower means tighter**. Fix that line while you are there.

> **CORRECTION — what shipped, and what did not.**
>
> * **The `cond_snr` documentation fix landed.** `worldgen-levers.md:108` now
>   reads "LOWER IS TIGHTER" with the mixing-angle formula.
> * **`cond_snr[0]` itself was NOT lowered.** Shipped value is still **0.30**
>   (`providers/diffusion.py:525`). Tested at 0.15 and rejected: tightening it
>   *reduced* relief — land above 1 km fell 27.05% → 24.87% and land fraction
>   40.5% → 37.7%. `measurements/elevation-tails-2026-08-01.txt` records
>   "Shipping keeps 0.30." Anyone reading "the SNR was lowered" is reading a
>   proposal, not the world.
> * **The tails were stretched and that did ship:** `elev_gain = 1.6`,
>   `elev_gain_power = 2.0` (`diffusion.py:590-591`). Land above 1 km 24.31% →
>   **27.05%**, above 2 km 5.29% → **8.07%**, coarse max 4,799 → **7,465 m**. A
>   gain of 2.0 was rejected as visible clipping (table asks 11,628 m, model
>   returns 8,144 m).
> * **The premise above was itself wrong.** "The table implies 22.6% of land
>   above 1000 m; delivered is 1.4%" conflated two quantities — the 1.4% was
>   *local relief per 2 km window*, not elevation above 1000 m. At gain 1.0 the
>   model **over-delivers** (table 22.2%, model 24.31%). The tail stretch was
>   still worth shipping; the stated diagnosis was not the reason.

Unknown, and it needs a measurement rather than an argument: how much 2 km-scale
relief responds to a coarse-table change. That statistic is produced by the
latent+decoder stages, which the coarse sketch only weakly steers.

---

## Wave 3 — stop the client flattening class contrast

**Scope narrowed by the owner's decision:** the anti-terracing floor stays. This
wave targets the *coarse tier's shaping octaves*, which is a different and
larger contributor.

The "0.3-0.6 m regardless of class" figure in
`docs/terrain-validation-2026-07.md` is a **worldgen v12** number. v13/v14/v18
were built against it and the fine tier is now largely fixed (~80 mm total
excursion on a fine plain, computed from shipped constants). **The coarse tier
is not:** `engageQ10` is exactly 0 below 3600 mm of relief30
(`kDetailCapReliefLoMm`, `amplifier.cpp:1191`, applied `:1595-1605`), so on flat
coarse ground **the entire gradient cap is switched off**, and the 25.6 m +
6.4 m *shaping* octaves contribute ~647 mm at their 0.10x relief floor —
landform-scale invention on ground that has no landform.

### 3a. Re-measure before designing

**There is no post-v12 geomorphon measurement anywhere in the repo.** Every
`frac_ridge_peak` figure carried forward is the v12 dataset. Re-run
`docs/terrain-validation-2026-07.md`'s recipe
(`earth_reference.py` -> `dump_stage_heightfields.py` -> `vxc_stagedump` ->
`stage_realism_report.py`). This is the highest-value hour in the wave and may
resize the problem.

### 3b. Then act on what it says

Likely target: relief-gate the coarse shaping octaves harder, or engage the cap
below 3600 mm. Note the measured non-monotonicity that makes a constant floor
impossible (`amplifier.cpp:1140-1192`) — the required allowance is non-monotone
in slope, which is why the ramp is on relief rather than slope.

### 3c. Also unblock the PROVISIONAL amplitude

`kFineDetailOctaves`' 3200 mm entry says *"The plan requires this be set by probe
measurement against the fine tier's measured S2, and that measurement does not
exist yet. Do not tune it by eye."* The comment is also **stale**: it describes
900, v18 shipped 100 for an unrelated rib-length reason. `vxc_terrainprobe
--calibrate --fine-dir` is the tool and **appears never to have been run**.

> **CORRECTION — it HAS been run, three times, on 2026-08-01.** Both flags exist
> and compose (`bench/terrainprobe.cpp:48`, `:52-55`), and the runs are recorded
> in `measurements/geomorphon-v21-2026-08-01.txt` §8. The item is still open for
> a different reason: **the measurement gave three different answers.**
>
> | site | H_used | r² | amp_3200 | amp_1600 | amp_400 | amp_200 |
> |---|---|---|---|---|---|---|
> | flat | 0.607 | 0.959 | 488 | 343 | 157 | 101 |
> | mid | 0.704 | 0.964 | 1253 | 770 | 323 | 175 |
> | steep | 1.025 | 0.983 | 332 | 310 | 92 | 24 |
> | **shipped** | | | **100** | **100** | **400** | **200** |
>
> `amp_3200` spans **3.8× across three sites inside one tile**, and it is not
> noise (residuals 7.5–8.3%, r² above 0.96). `H_used = 1.025` on the steep site
> is outside the fBm range, so that row cannot be averaged in. Recorded verdict:
> *"the measurement is possible and its answer is 'NOT YET'"* — **no number was
> proposed.** Closing it needs a defined calibration-site corpus, a rule for
> combining sites, and a re-run of v18's rib-length measurement.

---

## Wave 4 — landform provinces, Tier 1 only

Design doc: `docs/landform-provinces-plan.md` (corrected this session). Province
selects **per-cell parameter fields**, never per-tile constant sets, and is
derived from what the model produced rather than hashed independently.

**The precedent is not only `repose_field`.** The *regional-energy factor*
(`pipeline.py:2302-2320`) is already exactly the proposed architecture — a
per-cell field built from a 576^2 coarse-mean slope, world-anchored by
construction, multiplied into `kfac` — and it has a *stronger* seam story
because it is a pure function of the carrier rather than of hashed noise. The
relief discriminant is therefore **free**; it already exists.

### Ordering, by implementation cost — these are very unequal

1. **`profile_K_dt` per-province is free today.** `kfac = K_dt * A^m * regional *
   erodibility * gate * taper` is elementwise (`incise.py:447-462`), so a
   per-cell K_dt is arithmetically identical to a per-cell erodibility
   multiplier. Fold it into the array already passed as `erodibility=`. **No
   kernel signature change anywhere.**
2. `channel_init_area_m2`, `channel_init_q`, `stream_m` — same elementwise
   block; relax the scalar guards at `incise.py:395-398`.
3. `meso_amp15_m` / `meso_amp11_m` — elementwise (`noise.py:753-758`).
4. **`stream_n` and `incision_cap_m` are not cheap** — scalars inside the numba
   Newton kernel (`incise.py:254,292-294,307`).
5. **Cut `mfd_p` from Tier 1.** It drives the fine MFD *and* the superblock MFD
   at 30 m and 120 m/px (`pipeline.py:2280`, `:1512`) and is hashed as a scalar
   into `superblock_inputs_fingerprint` (`:1277`). A per-cell value at 1.875 m
   has no counterpart at the parent levels, so parent and child would route
   differently — worsening a known residual. Lowest visibility, highest
   structural cost.

### Climate must be plumbed in — but the data is already there

`bake_padded_domain` receives **elevation only** (`pipeline.py:2119-2129`);
`pregen._coarse_elevation_m` (`pregen.py:53-70`) discards the rest. But the
decoded `Tile` carries a `(4,512,512)` uint8 climate plane, the shipped cache
tiles are the full 1,572,889-byte form, and the encoding is a tested two-sided
contract (`tests/test_climate_contract.py`). Extend `CoarseFetch`, gather three
more times, **index at `//16` rather than upsampling**. Rolls `BAKE_VERSION`.

Caveat: climate is 30 m/px and uint8-quantised. Naive use prints 30 m blocks
into erosion intensity; smooth to landform scale first, bounded well under the
960 m apron and computed on the padded domain.

### Two corrections to the design doc, both important

* **The seam guarantee it claims to preserve does not currently hold.**
  `APRON_BLIND_SPOT` (`pipeline.py:1915-1954`) measured **1.05% of the shipped
  interior moving past the 100 mm wire LSB, by up to 78.79 m**, with the domain
  border's influence reaching **3.8 km inward — four apron widths** — because
  the depression fill is unbounded and a truncated domain *invents an outlet*.
  The field architecture doesn't make this worse, but it must not be sold as
  preserving a guarantee that is already violated.
* **Tier 2's KARST gate lands precisely on that violating stage**, and B4b
  re-fills the whole domain afterwards (`pipeline.py:2524`) so a karst gate
  applied only at B2a is silently undone. If KARST is ever attempted, derive the
  mask from the **superblock** raster so coarse and fine hydrology agree about
  where sinks are allowed.

---

## Verification

| wave | check | tool |
|---|---|---|
| 0 | sha256 `dcf052013eecade4...`, 201,367,675 bytes, after merge | `tools/bake_real_tile.py --tile -5 3` |
| 0 | 365 tests pass; `vxc_gpu` bit-exact | `vxc_tests`, `vxc_gpu` |
| 1 | DESERT & RAINFOREST non-zero; precip spread >> 7.8% | `world_map.py --style biome`, ~35 s CPU |
| 1 | deserts downwind of ranges, not scattered | `world_map.py --style blend --relief` |
| 2a | SAVANNA reachable | `world_map.py`, then `vxc_climateprobe` on real tiles |
| 2b | fraction of land above 1000 m moves toward the table's 22.6% | `world_map.py`; then the 2 km-relief measurement on delivered tiles |
| 3a | post-v12 `frac_ridge_peak` per class | `stage_realism_report.py` |
| 3b | class ordering no longer inverted | same |
| 4 | drainage invariants hold per province: 0 interior sinks, 0.0% stranded | `vxc_terrainprobe --fine-dir` |
| 4 | province distinguishable from FLUVIAL on `dd_km_per_km2`, `hurst_overall`, `frac_above_repose`, `p99_deg` | `geomorph_validate.py` |

**A warning about the metrics.** `docs/geomorph-validation.md:39-56` records that
slope statistics, geomorphon histograms, hypsometry, drainage density and raw
pit density are all **blind to a spectrum-matched fake**. Only fill-volume,
curvature asymmetry, Hack's h and marginally theta see through it. And theta
separates classes by only ~1.8x against a within-scene spread of 2.6x — so "a
target concavity per province" is a weak acceptance test on a single window. Use
`dd_km_per_km2`, `hurst_overall`, `frac_above_repose`, `p99_deg`, which clear
three within-class sigmas.

The Earth corpus already maps almost 1:1 onto the proposed provinces
(`tools/earth_reference.py:181-500`): Teton/Alps -> GLACIAL (Teton has 1 m 3DEP
bare-earth), Smokies/Nepal -> FLUVIAL, Badlands -> ARID, Iowa/Llano/Illinois ->
LOWLAND. **There is no karst site**, which is one more reason KARST is not
Tier 1.

## Open risks

1. **Rebuilding the stats cache changes every tile.** All 25 coarse tiles and 9
   baked fine tiles become a previous world. Expected and correct — but it means
   the 71 minutes of baking done this session is spent, and the cache migration
   tool (`tools/migrate_cache_namespace.py`) becomes moot for that data.
2. **The coarse model may not obey a stretched elevation table.** It already
   compresses the sketch's tails 22.6% -> 1.4%. Lowering `cond_snr[0]` is the
   lever, and its effect is empirical.
3. **No GPU on this box.** `torch` is a CPU-only build and the GPU is AMD, so
   full tile generation needs pod time. Coarse-only sweeps run on CPU in ~35 s
   and are enough for Waves 1 and 2a.
4. **`terrain_diffusion` imports only from its own repo root** — `import
   terrain_diffusion` fails from other cwds. It is a namespace package with no
   `__file__`.
