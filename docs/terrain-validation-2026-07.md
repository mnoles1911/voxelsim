# Where the 30 m → 10 cm pipeline stops looking like Earth

*Measured 2026-07-29 on the `terrain-diffusion-unlabeled-UNPINNED-UNVERIFIEDDATA-27ac04bc8c6b7b7d`
world, seed `000000000135276f`. Every number below is reproducible from the commands in
§9; nothing here is read off a screenshot.*

---

## 1. The answer, in four sentences

1. **S0 and S1 look like Earth. S3 does not.** The 30 m diffusion tile passes every
   realism gate that can be evaluated on it, and the bake at 1.875 m reproduces a real
   till plain's slope-by-scale curve to within 12% across the whole amplification band.
   The client's detail octaves (`Amplifier::evalSurface`, S2 → S3) then multiply the
   1.875 m mean slope of a plain by **3.1x** (2.04° → 6.27°) and classify **35%** of it
   as ridge-or-peak where real plains measure 5–7%.
2. **The break is at S3, and it is a gentle-ground failure specifically.** The same
   detail pass adds +4.23° of mean slope to a plain (+207%) and +0.89° to alpine
   (+3.3%). The added roughness is ~0.3–0.6 m RMS regardless of class, so it is
   negligible against the alpine window's 166 m detrended σ and overwhelming against the
   plains window's 6.7 m.
3. **A plain does not look like a plain.** At 1.875 m our plains tile finishes at
   `frac_ridge_peak = 0.351` where real plains measure 0.054 (Llano) and 0.073
   (Illinois) — and where **our own alpine tile measures 0.061**. The class ordering is
   inverted: the finished plain is six times more ridged than the finished mountain.
4. **The bake has a second, independent defect that the client masks rather than
   fixes.** In the sub-30 m band the bake's slope-area concavity collapses to
   θ = 0.028–0.046 (real: 0.177–0.318) and its ridge+peak fraction at a matched
   10-cell lookout is 0.3–1.7% (real: 3.3–4.8%). The client then sprays isotropic
   detail over the top, which restores the *fraction* of ridge cells without restoring
   any drainage structure.

---

## 2. What was measured, and how resolution was matched

Five stages, on the same world rectangle, in metres:

| stage | what | who wrote it |
|---|---|---|
| S0 | raw 30 m diffusion tile, int16 whole metres | `tools/dump_stage_heightfields.py` |
| S1 | bake output at 1.875 m (carrier + roughness + flow + incision + thermal) | same, `--bake` |
| S2 | client carrier only, all detail octaves off | `vxc_stagedump` |
| S3 | client full continuous surface, `Amplifier::surfaceMm` | `vxc_stagedump` |
| S4 | voxelised: top face of the topmost solid voxel, via `stratigraphyAt` | `vxc_stagedump` |

S2/S3/S4 are dumped twice — over a 30 m `TileGridSampler` (**coarse**) and over a
`FineTileSampler` on a baked v2 tile (**fine**) — because the amplifier deletes the
25.6 m and 6.4 m landform octaves on a fine world. Both are reported; the fine tier is
the shipping configuration wherever a baked tile exists.

**Footprints.** One coarse tile per class, origin on the tile corner, span 15330 m
(512 nodes at 30 m, inside the 8192² bake interior). The 1.875 m band is a centred
2040 m sub-rectangle (1089² nodes), chosen to match the 2048 m 3DEP window.

**Resolution matching** is enforced by `geomorph.require_same_resolution` on the
*results*, not asserted in prose:

* **30 m band.** Ours is natively 30.0 m. Copernicus is read on the manifest's `aeqd30`
  grid — a local azimuthal-equidistant reprojection at exactly 30.0 m square pixels —
  so neither side is resampled. The 3DEP 1 m DTM is block-averaged ×30. All fields are
  cropped to the same 508² cell count.
* **1.875 m band.** The 3DEP 1 m DTM is area-averaged onto exactly 1.875 m cells by an
  exact box filter over the piecewise-constant source (`stage_realism_report.box_resample`,
  an integral-image method: 8 output cells span exactly 15 input cells). Block-averaging
  ×2 to 2.0 m instead would leave a 6.7% cell mismatch, which the guard correctly
  refuses. All fields cropped to 1089². The resampler agrees with a brute-force
  partial-cell accumulation to 1.4e-13 and is bit-identical to `block_mean` at integer
  ratios; its only artifact is an 8-cell-periodic ripple of ~0.058 × (source rise per
  cell), which is 0.0014–0.0023 m (< 0.08° of grade) on the plains references and
  0.058 m (< 1.8°) on the Teton one. It makes the reference marginally *rougher*, so
  every "ours is rougher than Earth" conclusion below is conservative with respect to
  it.

**Reference choice, per the DSM caveat.** `data/earth_reference/manifest.json`'s own
findings block records that Copernicus is *smoother* than bare ground at its own posting
on 6 of 9 sites, and that the DSM penalty is worst on low-relief vegetated ground
(`nc_coastal_plain` 5.3x). So every 1.875 m comparison uses the 3DEP DTM only, and the
30 m table carries both so the disagreement is visible (Llano DSM/DTM mean-slope ratio
1.358, Illinois 1.075, Teton 0.902).

### Three things that are measurements of us, not of the landscape

* **Pit and fill metrics on S1 and everything downstream measure our own epsilon fill.**
  `bake.pipeline` B2a fills depressions before incising. `fill_volume_per_area_m` is a
  realism gate on S0 and on the references; on S1–S4 it says only that the fill ran.
* **Our 30 m samples of S1–S4 are point samples**, every 16th fine cell; a real 30 m DEM
  is closer to an area average. The `S1 bake (16x16 block mean)` column is the same bake
  area-averaged, so the size of that convention difference is visible: on plains it moves
  `frac_flat` from 0.269 to 0.471 and `curvature_asymmetry` from 0.076 to 0.098. **Read
  the 30 m S3/S4 columns with that in mind** — a 30 m point sample of a surface carrying
  6.4 m detail is aliased, and the clean statement about S3 is the one made at 1.875 m.
* **Climate planes are synthetic.** See §8.

---

## 3. Per class: does each stage look like Earth?

Realism gates, from `terrain-service/docs/geomorph-validation.md`: `fill_volume_per_area_m` (real
0.07–0.97 m, fakes 20–65 m), `curvature_asymmetry` (real 0.010–0.111, fakes < 0.003),
`hack_h` (real 0.475–0.557, fakes 0.161–0.287), `tail_asymmetry` (real 0.87–1.32, fakes
0.99–1.00). `theta` is marginal and is reported for §7 rather than as a gate.

### 3.1 plains — coarse tile (−55, 20), 90.7% land, p50 grade 1.7%

**30 m band** (508², 15.24 km; references cropped to the same cell count)

| metric | S0 wire | S1 bake | S2 fine | S3 fine | S4 fine | S3 coarse | REF Llano cop30 | REF Llano 3DEP@30 | REF Illinois cop30 | REF Illinois 3DEP@30 |
|---|---|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.070 | *0.020* | *0.021* | *0.055* | *0.055* | *0.112* | 0.101 | 0.0043 | 0.097 | 0.0037 |
| `curvature_asymmetry` | **0** ⚠ | 0.076 | 0.071 | 0.044 | 0.036 | 0.093 | 0.020 | 0.080 | 0.016 | 0.070 |
| `hack_h` (r²) | 0.574 (0.98) | 0.650 (1.00) | 0.659 (0.99) | 0.669 (1.00) | 0.690 (1.00) | 0.645 (1.00) | 0.600 (1.00) | −0.221 (0.06) | 0.555 (1.00) | 0.659 (0.89) |
| `tail_asymmetry` | 0.985 | 0.972 | 0.973 | 0.968 | 0.969 | 0.909 | 0.924 | 0.990 | 0.989 | 1.146 |
| `mean_deg` | 1.23 | 1.16 | 1.16 | 1.22 | 1.22 | 1.44 | 0.75 | 0.42 | 1.16 | 1.30 |
| `frac_flat` | 0.251 | 0.269 | 0.269 | 0.199 | 0.207 | **0.014** | 0.673 | 0.995 | 0.479 | 0.161 |
| `frac_ridge_peak` | 0.105 | 0.105 | 0.105 | 0.126 | 0.124 | **0.214** | 0.044 | 0 | 0.080 | 0.065 |
| `relief_m` | 192 | 195 | 195 | 196 | 196 | 194 | 52 | 15 | 56 | 32 |

*Italic = measuring our epsilon fill, not the landscape. ⚠ = see §6. The 3DEP@30 rungs
are 68² cells (a 2048 m window block-averaged ×30) — enough for slope and curvature,
not for Hack's law, which is why Llano 3DEP@30 returns h = −0.221 at r² = 0.06. Take the
30 m Hack numbers off Copernicus.*

**1.875 m band** (1089², 2.04 km) — the band the pipeline actually invents

| metric | S1 bake | S2 fine | S3 fine | S4 fine | S3 coarse | REF Llano 3DEP | REF Illinois 3DEP |
|---|---|---|---|---|---|---|---|
| `curvature_asymmetry` | 0.091 | 0.019 | **0.0013** | **0** ⚠ | 0.0019 | 0.035 | 0.014 |
| `hack_h` (r²) | 0.580 (0.99) | 0.716 (0.72) | 0.599 (0.99) | 0.591 (0.99) | 0.723 (0.72) | 0.586 (0.94) | 0.546 (0.82) |
| `tail_asymmetry` | 0.614 | 0.633 | 0.986 | 0.986 | 0.998 | 0.989 | 1.123 |
| `theta` (r²) | 1.94 (0.59) | 0.076 (0.91) | 0.304 (0.54) | 0.250 (0.51) | 0.272 (0.87) | 0.148 (0.90) | 0.089 (0.59) |
| `mean_deg` | **2.04** | 2.06 | **6.27** | 6.29 | **7.06** | 0.91 | **2.13** |
| `p95_deg` | 5.25 | 5.31 | 12.3 | 12.4 | 16.4 | 2.76 | 5.79 |
| `frac_flat` | 0.095 | 0.071 | **0** | **0** | **0** | 0.555 | 0.038 |
| `frac_ridge_peak` | 0.096 | 0.099 | **0.352** | 0.351 | 0.269 | 0.054 | 0.073 |
| `frac_valley_pit` | 0.058 | 0.065 | **0.343** | 0.346 | 0.293 | 0.039 | 0.058 |
| `frac_flat_coarse_thresh` | 0.851 | 0.831 | **2.4e-05** | **0** | **3.0e-04** | 0.943 | 0.826 |

**Verdict.** S0, S1 and S2-fine are Earth-like on this class. Against the Illinois till
plain, S1 matches mean slope to 4% (2.04° vs 2.13°), p95 to 9% (5.25° vs 5.79°),
`frac_valley_pit` exactly (0.058 vs 0.058), `frac_flat_coarse_thresh` to 3% (0.851 vs
0.826) and `frac_ridge_peak` to 32% (0.096 vs 0.073); it is flatter-at-1° than Illinois
by 2.5x (0.095 vs 0.038), which puts it between the two real plains rather than outside
them. That is a genuinely good match for a real, young, gently dissected plain. **S3 destroys it.** Every metric that carries landform
identity moves an order of magnitude away from every real plains reference, and
`curvature_asymmetry` drops to 0.0013 — inside the Gaussian-surrogate band (< 0.003)
that the whole metric suite exists to detect.

### 3.2 alpine — coarse tile (−5, 15), 100% land, p50 grade 40.6%

**30 m band**

| metric | S0 wire | S1 bake | S3 fine | S3 coarse | REF Alps cop30 | REF Teton cop30 | REF Teton 3DEP@30 |
|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.072 | *0.013* | *0.020* | *0.068* | 0.097 | 0.075 | 0.0010 |
| `curvature_asymmetry` | **0** ⚠ | 0.027 | 0.0014 | 0.193 | 0.063 | 0.049 | 0.107 |
| `hack_h` (r²) | 0.523 (0.89) | 0.483 (0.99) | 0.499 (0.99) | 0.535 (0.94) | 0.544 (0.90) | 0.516 (0.95) | n/a |
| `tail_asymmetry` | 1.074 | 1.338 | 1.310 | 0.814 | 0.869 | 0.701 | 0.513 |
| `theta` | 0.194 | 0.207 | 0.143 | 0.202 | 0.203 | 0.283 | 0.329 |
| `mean_deg` | 21.1 | 21.1 | 21.1 | 20.8 | 23.0 | 21.6 | 43.2 |
| `frac_ridge_peak` | 0.060 | 0.058 | 0.059 | 0.068 | 0.059 | 0.079 | 0.141 |
| `relief_m` | 1831 | 1808 | 1808 | 1826 | 2785 | 2084 | 1144 |

**1.875 m band**

| metric | S1 bake | S2 fine | S3 fine | S4 fine | S3 coarse | REF Teton 3DEP |
|---|---|---|---|---|---|---|
| `curvature_asymmetry` | 0.0066 | 0.013 | **0.0049** | **0** ⚠ | 0.0082 | 0.039 |
| `tail_asymmetry` | 0.919 | 0.963 | 0.989 | 0.987 | 0.985 | 0.825 |
| `theta` | **0.028** | 0.029 | 0.078 | 0.079 | 0.066 | 0.177 |
| `mean_deg` | 26.8 | 26.8 | 27.7 | 27.7 | 28.1 | 44.8 |
| `frac_slope` | 0.643 | 0.644 | 0.509 | 0.501 | 0.515 | 0.381 |
| `frac_ridge_peak` | 0.064 | 0.063 | 0.061 | 0.062 | 0.061 | 0.089 |
| `frac_valley_pit` | **0.0037** | 0.0037 | 0.021 | 0.023 | 0.022 | 0.099 |

**Verdict.** At 30 m the alpine tile is a good alpine landscape: `hack_h` 0.523,
`theta` 0.194 and the geomorphon histogram all land between the Alps and the Tetons at
their own 30 m posting. At 1.875 m the picture is different but the failure is *not*
the same one as on plains — the client adds only +3.3% slope here. What is wrong at
1.875 m on alpine is **the bake**: `frac_valley_pit` 0.0037 against the Teton DTM's
0.099 (a 27x deficit) and `theta` 0.028 against 0.177.

*Caveat on the alpine 1.875 m column, stated because it would otherwise be over-read:*
the Teton 3DEP window is a 2 km cirque-headwall window whose 30 m mean slope is 43.2°,
while the 20 km Teton Copernicus window averages 21.6° — our alpine tile averages 21.1°
at the tile scale, so it is a fair alpine tile sitting beside an unusually steep 2 km
reference. Read the *shape* of the alpine fine-band comparison (valley/pit deficit,
concavity deficit), not the absolute steepness ratio.

### 3.3 rolling — coarse tile (15, 55): a tile-selection failure, not a pipeline one

This exemplar is labelled *rolling* and is not rolling country. Over 15.2 km it carries
**1364 m of relief and a 14.7° mean slope**; the Southern Iowa Drift Plain carries 87 m
and 3.6°. Every geomorphon fraction is closer to our own alpine tile than to Iowa
(`frac_ridge_peak` 0.086 vs alpine 0.060 vs Iowa 0.178; `frac_flat_coarse_thresh` 0.022
vs Iowa 0.301). **Whatever assigned the class label to this tile is wrong**, and the
tile says nothing about the amplifier. It does reproduce the two bake defects: at
1.875 m, `theta` = 0.046 against Iowa's 0.318, and `frac_ridge_peak` = 0.020 against
0.188.

### 3.4 Summary table — where each class stops passing

| class | S0 (30 m) | S1 (30 m) | S1 (1.875 m) | S2 fine (1.875 m) | S3 fine (1.875 m) | S4 fine (1.875 m) |
|---|---|---|---|---|---|---|
| plains | pass (gate ⚠) | pass | **pass** | pass | **FAIL** | FAIL |
| alpine | pass (gate ⚠) | pass | partial: θ, valley/pit | partial | partial | partial |
| rolling | pass (gate ⚠) | pass | partial: θ, ridge/peak | partial | partial | partial |

---

## 4. Where it stops looking like Earth — the numbers, not the impression

The single clearest measurement is mean slope as a function of the scale it is measured
at, on **the same 2040 m rectangle** for every one of our stages and on the 2048 m 3DEP
window for the reference. Ours is block-averaged from 1.875 m; the reference is
area-resampled from 1 m to 1.875 m and then block-averaged the same way, so every column
is the same operator applied to a different surface.

| field | 1.875 m | 3.75 m | 7.5 m | 15 m | 30 m |
|---|---|---|---|---|---|
| plains S1 bake | 2.041 | 1.959 | 1.800 | 1.518 | 1.148 |
| plains S2 fine (carrier) | 2.056 | 1.963 | 1.799 | 1.517 | 1.147 |
| **plains S3 fine (surface)** | **6.268** | 2.856 | 1.931 | 1.530 | 1.148 |
| **plains S3 coarse** | **7.057** | 4.845 | 3.430 | 2.231 | 1.319 |
| REF Llano 3DEP | 0.909 | 0.676 | 0.542 | 0.463 | 0.416 |
| **REF Illinois 3DEP** | **2.134** | 1.943 | 1.728 | 1.523 | 1.301 |
| alpine S1 bake | 26.775 | 26.677 | 26.474 | 26.097 | 25.386 |
| alpine S3 fine | 27.664 | 26.773 | 26.483 | 26.099 | 25.387 |
| REF Teton 3DEP | 44.810 | 44.997 | 44.959 | 44.505 | 43.210 |

Read the plains rows across: **the bake tracks a real till plain at every scale from
30 m down to 1.875 m** (2.041 / 1.959 / 1.800 / 1.518 / 1.148 against 2.134 / 1.943 /
1.728 / 1.523 / 1.301 — within 12% everywhere, and within 5% over three of the five
rungs). The carrier reproduces the bake to 0.018 m RMS. Then S3 triples the shortest
rung and leaves every other rung alone. **The defect is one octave wide and it is
introduced by the client's detail pass.**

The residual `S3 − S2` says the same thing directly:

| class / tier | `S3 − S2` RMS | p99 \|d\| | as a fraction of the window's own detrended σ |
|---|---|---|---|
| plains, fine | 0.295 m | 0.716 m | 4.4% (σ = 6.67 m) |
| plains, coarse | 0.825 m | 2.39 m | 12.4% |
| alpine, fine | 0.597 m | 1.48 m | 0.36% (σ = 166 m) |
| alpine, coarse | 2.064 m | 6.30 m | 1.24% |

The detail amplitude differs by 2x between a plain and a mountain; the landform relief
differs by 25x. `Amplifier::evalSurface` conditions detail amplitude on the carrier's
*gradient* (`slopeScaleQ10`, clamped to 0.25x–4.0x), not on the landform's relief, so on
a 1.7%-grade plain the amplifier still lays down ~0.3 m of roughness at metre
wavelengths — which at a 1.875 m posting is a 9–18% local grade, an order of magnitude
above the regional one it is supposed to decorate.

**S4 is not a source of error.** `S4 − S3` RMS is 0.032 m on plains (the 10 cm
quantisation floor is 0.029 m) and 0.137 m on alpine, where `density3` displaces 0.43%
of columns on plains and more on steep ground. Voxelisation faithfully reproduces S3;
it also, unhelpfully, zeroes one of the realism metrics — §6.

---

## 5. Do plains look like plains?

**No. At 1.875 m the finished plain reads as a ridged landscape, and specifically as a
*more* ridged landscape than our own alpine tile.**

Geomorphon histograms at the library default 300 m lookout, 1.875 m cells, all fields
1089²:

| field | flat | slope | ridge+peak | valley+pit | flat @3° |
|---|---|---|---|---|---|
| ours, plains S1 bake | 0.095 | 0.385 | 0.096 | 0.058 | 0.851 |
| **ours, plains S3 fine** | **0** | 0.102 | **0.352** | **0.343** | **2.4e-05** |
| ours, plains S4 fine | 0 | 0.101 | 0.351 | 0.346 | 0 |
| **ours, alpine S3 fine** | 0 | 0.509 | **0.061** | 0.021 | 0 |
| REF Llano Estacado 3DEP | 0.555 | 0.094 | 0.054 | 0.039 | 0.943 |
| REF Illinois till plain 3DEP | 0.038 | 0.497 | 0.073 | 0.058 | 0.826 |
| REF Teton Range 3DEP | 0 | 0.381 | 0.089 | 0.099 | 0 |

Three statements, each of which is a separate failure:

1. **Ridge+peak is 4.8–6.5x the real plains value** (0.352 vs 0.073 Illinois and 0.054
   Llano) and valley+pit is 5.9–8.8x (0.343 vs 0.058 and 0.039).
2. **Flat is zero** — not "low", zero at the 1° threshold and 2.4e-05 at the 3° one,
   where real plains are 0.83–0.94 flat at 3°. The Llano Estacado is 55% flat at 1°;
   our finished plain has 0 flat cells in 1.19 M samples.
3. **The class ordering inverts.** Our plain finishes at ridge+peak 0.352; our alpine
   finishes at 0.061. In the references the ordering is the other way (Teton 0.089 >
   Illinois 0.073 > Llano 0.054). A classifier handed our two finished surfaces would
   call the plain the mountain.

And the corresponding statement for alpine: **yes, roughly.** Our alpine S3 gives
ridge+peak 0.061 against Teton's 0.089 and slope 0.509 against 0.381 — the right shape,
somewhat under-textured. The failure is confined to gentle ground, exactly as the brief
predicted.

The bake, by contrast, *is* a plain: S1's histogram (0.095 / 0.385 / 0.096 / 0.058 /
0.851) is a near-match for the Illinois till plain (0.038 / 0.497 / 0.073 / 0.058 /
0.826). **Everything the bake got right about plains is destroyed between S2 and S3.**

---

## 6. Two places a metric says we are fine and we are not, and one where it says nothing

### 6.1 `curvature_asymmetry` is identically zero on any quantised surface — it cannot be read on S0 or S4

Both S0 (int16 **whole metres** on the wire) and S4 (10 cm voxels) return
`curvature_asymmetry` = **exactly 0**, which in the validation table is the signature of
a Gaussian fake (< 0.003). It is not. The metric is |quantile skew of the Laplacian|,
and on a field quantised to an LSB comparable to its own curvature over one cell, the
p01 and p99 Laplacian quantiles land on the *same pair of discrete levels* and the skew
is exactly zero by arithmetic. Adding dither restores it:

| field | as measured | +U(±5 mm) | +U(±25 mm) | +U(±125 mm) |
|---|---|---|---|---|
| plains S0 @ 30 m (1 m LSB) | 0 | 0.00041 | 0.0020 | 0.0090 |
| alpine S0 @ 30 m (1 m LSB) | 0 | 0.00070 | 0.0035 | 0.0170 |
| plains S4 fine @ 1.875 m (0.1 m LSB) | 0 | — | — | 0.0011 (= S3's 0.0013) |
| alpine S4 fine @ 1.875 m (0.1 m LSB) | 0 | — | — | 0.0021 (≈ S3's 0.0049) |
| plains S4 fine @ 30 m (0.1 m LSB) | 0.0357 | — | — | 0.044 — *no artifact* |

**Which is wrong: the metric or the terrain?** The metric — but only as a *reader* of
these two stages. It is measuring something real (there is genuinely no resolvable
curvature asymmetry left after 1 m quantisation of a 1.7%-grade plain), it just cannot
distinguish "the surface has no asymmetry" from "the encoding threw the asymmetry away".
Practical rule: **`curvature_asymmetry` is undefined when the vertical LSB exceeds
roughly 1% of the cell size.** On S0 (1 m / 30 m = 3.3%) and S4-fine (0.1 m / 1.875 m =
5.3%) it is; on S4 at 30 m (0.33%) it is not, and there it reads normally. The realism
gate on S0 has to be `hack_h` + `tail_asymmetry` + `fill_volume_per_area_m`, all three of
which S0 passes on both classes.

### 6.2 At 30 m, every realism gate passes on a surface that is visibly not a plain

Plains S3-coarse at 30 m: `curvature_asymmetry` 0.093, `hack_h` 0.645 (r² 1.00),
`tail_asymmetry` 0.909, `fill_volume_per_area_m` 0.112. All four are inside the real
range; `curvature_asymmetry` is *four times* the real Llano value and six times the real
Illinois value, i.e. the gate says our surface is more Earth-like than Earth. The same
column's `frac_flat` is **0.014** against 0.48–0.67 for the real plains, and its
`frac_ridge_peak` is 0.214 against 0.044–0.080.

This is not a defect in the gates — `terrain-service/docs/geomorph-validation.md` says explicitly that
geomorphons separate classes and are blind to realism, and that the realism gates are
blind to class. It is a warning about how to *use* them: **the realism gates cannot
detect a landscape that is internally consistent and of the wrong kind.** Isotropic
detail with a hash-driven asymmetry passes a curvature-asymmetry test; it just does not
belong on a plain. Any future "does it look like Earth" gate must run the geomorphon
histogram against the reference for the *claimed class* as well.

### 6.3 `theta` on the 1.875 m plains band is a degenerate fit and should not be quoted

`theta` = 1.94 on plains S1 at 1.875 m with r² = 0.589. On a surface with 0.02 m of
mean fill and no resolved channel network at that scale, the slope–area regression is
fitting noise. The `slope_area_r2` column is in every table for this reason; treat
θ with r² < 0.8 as unreported.

---

## 7. The two known gaps, confirmed or refuted

### 7.1 Concavity: θ = 0.067 against Earth's 0.18–0.40 — **confirmed, and localised to the sub-30 m band**

| θ (r²) | plains | alpine | rolling |
|---|---|---|---|
| S0, 30 m | 0.010 (0.18) — degenerate | 0.194 (0.94) | 0.165 (0.88) |
| S1 bake, 30 m | 0.365 (0.66) | **0.207** (0.74) | 0.158 (0.82) |
| S1 bake, 1.875 m | 1.94 (0.59) — degenerate | **0.028** (0.74) | **0.046** (0.88) |
| S3 fine, 1.875 m | 0.304 (0.54) | 0.078 (0.95) | 0.073 (0.87) |
| real reference, 30 m | 0.262 Illinois 3DEP / 0.283 cop30 | 0.203 Alps / 0.329 Teton 3DEP | 0.387 (Iowa 3DEP) |
| **real reference, 1.875 m** | 0.148 Llano / 0.089 Illinois | **0.177 (Teton 3DEP)** | **0.318 (Iowa 3DEP)** |
| bake's own prediction m/n | 0.5625 | 0.5625 | 0.5625 |

**Which stage introduces it: the bake, and only below 30 m.** At 30 m the bake *raises*
alpine θ from the input's 0.194 to 0.207, landing on the Alps' own 0.203 — the incision
pass does the right thing at the scale it inherited. At 1.875 m the same baked surface
measures 0.028 on alpine and 0.046 on rolling, against 0.177 and 0.318 for the matched
real DTMs at the identical cell size — **a 4–7x concavity deficit that exists only in the
band the bake invents.** The client then *raises* θ (0.028 → 0.078 on alpine) by adding
detail, which is a coincidence of the slope–area regression rather than a fix: the same
pass takes `curvature_asymmetry` down to 0.005.

The 0.067 figure in `terrain-service/docs/geomorph-validation.md` is reproduced in kind (we measure
0.028–0.078 across three tiles) and the honest target is revised: **0.18–0.32 measured
by this method on real 1 m DTMs at 1.875 m, not 0.5625 and not 0.35–0.6.** One incision
pass is not steady state, so `m/n = 0.5625` remains an attractor and not a requirement —
but the measured value is on the wrong side of the input, not merely short of the
attractor.

### 7.2 "77% featureless SLOPE against 5–18% ridge+peak for real terrain" — **confirmed for the bake, at the matched lookout, with the real number restated**

The 77% claim was measured at a **10-cell lookout**; `geomorph.describe` defaults to a
300 m physical lookout, which at 1.875 m is 160 cells. Measured properly — search
= 10 × 1.875 m = 18.75 m on every field including the references:

| field | flat | slope | ridge+peak | valley+pit | hollow+footslope | spur |
|---|---|---|---|---|---|---|
| **alpine S1 bake** | 0.0004 | **0.848** | **0.0031** | 0.0043 | 0.063 | 0.081 |
| alpine S2 fine (carrier) | 0.0004 | 0.847 | 0.0030 | 0.0043 | 0.064 | 0.081 |
| alpine S3 fine (surface) | 0 | 0.482 | 0.0403 | 0.038 | 0.220 | 0.220 |
| **REF Teton 3DEP** | 0 | **0.610** | **0.0332** | 0.023 | 0.178 | 0.155 |
| **plains S1 bake** | 0.323 | 0.449 | **0.0166** | 0.012 | 0.086 | 0.061 |
| plains S3 fine (surface) | 0 | 0.105 | **0.3458** | 0.344 | 0.103 | 0.102 |
| REF Llano 3DEP | 0.619 | 0.092 | **0.0484** | 0.034 | 0.115 | 0.014 |
| REF Illinois 3DEP | 0.198 | 0.534 | **0.0434** | 0.038 | 0.088 | 0.043 |

**Confirmed, and worse than recorded: 84.8% SLOPE with 0.31% ridge+peak on the alpine
bake.** But the "5–18% ridge+peak for real terrain" comparison was against real terrain
at its own posting and lookout; at a *matched* 10-cell 18.75 m lookout the real DTMs give
**3.3–4.8%**. So the honest statement of gap 2 is:

* the bake's fine tier under-produces ridge+peak by **10.7x** on alpine (0.0031 vs
  0.0332) and **2.6–2.9x** on plains (0.0166 vs 0.0434–0.0484);
* **S2-fine is identical to S1** to within 0.001 in every class fraction, so the client's carrier inherits the
  defect exactly and does not create it — **the bake introduces it**;
* the client's detail pass then *overshoots* the fix: it takes alpine to 0.0403 against
  the real 0.0332 (good) and plains to **0.3458 against the real 0.0434** (8x too much).

The two gaps are the same underlying defect seen twice: **the bake produces almost
nothing but smooth interpolation between 30 m carrier samples, and the client
compensates with a class-blind isotropic detail field.**

---

## 8. Blockers found while running this, and what was done about them

1. **The 52-tile world cannot be read by anything.** Every tile in
   `.../000000000135276f/s1/` is 524313 bytes = header + elevation and **no climate
   planes**; a complete v1 tile is 1572889 bytes. `tile_codec.decode` raises
   `ValueError: buffer is smaller than requested size`, and voxel-core's v1 reader
   (`src/tilestore.cpp`, the four `if (!r.u8(tile.climate[c][i])) return std::nullopt;`
   lines) returns `nullopt`, so `TileGridSampler` — and the game client — sees no tile at
   all. `tools/gen_world_tiles2.py`'s docstring records the same defect; the tiles have
   not been regenerated. Regenerating them now is not a like-for-like substitute:
   `gen_world_tiles2.py` currently pins the checkpoint, which rolls `provider_id` and
   therefore produces a *different world under a different cache id*, not the one under
   study.

   **What was done:** `terrain-service/tools/reencode_elevation_only_tiles.py` copies
   elevation verbatim and fills the four climate channels with a constant 128. Every
   surface height in the pipeline is independent of that choice — `bake_tile` is handed
   `coarse_fetch(x, y) -> elevation` and never sees climate; `Amplifier::evalSurface`
   reads `elevationMm` only; `Amplifier::column` spends climate on `surfaceMat`,
   `topsoilMm`, `subsoilMm` and the biome. The one path from climate to a *height* is
   `col.d3 = density3ColumnFor(..., soilAboveRockMm(col))`, and the S4 sidecars bound it:
   **0 of 1.19 M** displaced columns on the plains coarse tier at 1.875 m, **5 of 262 144**
   on the plains coarse tier at 30 m, and **5061 of 1.19 M (0.43%)** on the plains fine
   tier at 1.875 m. No conclusion in this document rests on S4 material
   or on those 0.43%.

2. **`s16/` is empty**, so there was no shipped fine tier to dump S2/S3/S4-fine over, and
   `--verify-vxtl` could not be run. Instead `dump_stage_heightfields.py --emit-vxtl`
   encoded *this run's* S1 as the v2 tile the client read. That side-steps the
   `BAKE_VERSION` trap entirely rather than working around it: S1 and S2/S3/S4-fine are
   provably the same bake, because the fine tile was written from the same array. The
   check that S2-fine reproduces S1 came out at **0.018 m (plains) / 0.022 m (alpine)
   RMS**, against a 100 mm control-point quantisation floor.

3. **The fine tier reads 0 mm one fine pixel outside the baked tile**, so the 30 m
   full-tile lattice has a contaminated border (`vxc_stagedump` prints "rectangle is not
   fully covered"; 8312 missing-tile queries). Measured: cropping **one** cell takes the
   worst `S3_fine − S1` residual from 23.9 m to 3.1 m and cropping more changes nothing.
   All 30 m fields — ours and the references — are cropped by 2 cells.

4. **Earth-reference cache was not built** (only `manifest.json` is committed). Rebuilt
   for the five sites used here. Worth recording as a positive result: **every one of
   the twelve site records came back byte-identical** to the committed manifest, so the
   only diff was `generated_utc` and the rewrite was reverted. The corpus builder is
   reproducible, and the statistics quoted below are the committed ones.

---

## 9. Reproducing this

```sh
# 0. references (~230 MB for these five sites; resumable, checksummed)
python terrain-service/tools/earth_reference.py build \
    --site llano_estacado_plains --site illinois_till_plain \
    --site alps_valais_alpine --site teton_range_alpine --site iowa_rolling_hills

# 1. make the elevation-only world readable (elevation verbatim, climate <- 128)
python terrain-service/tools/reencode_elevation_only_tiles.py \
    --src  tile-cache/terrain-diffusion-unlabeled-UNPINNED-UNVERIFIEDDATA-27ac04bc8c6b7b7d/000000000135276f/s1 \
    --dest $SCRATCH/s1 --tile=-55,20 --tile=-5,15 --tile=15,55 --ring

# 2. S0 + S1, and the v2 fine tile the client will read (~3 min, 3.2 GiB, ONE AT A TIME)
python terrain-service/tools/dump_stage_heightfields.py \
    --out $SCRATCH/dump/plains --tiles-dir $SCRATCH/s1 --seed 20260719 \
    --origin -844800 307200 --span 15330 --cell 30000 --cell 1875:2040 \
    --bake --bake-cache-dir $SCRATCH/bakecache --emit-vxtl $SCRATCH/s16
#   alpine:  --origin -76800 230400   rolling: --origin 230400 844800

# 3. S2 + S3 + S4, both tiers, same rectangle
vxc_stagedump --out $SCRATCH/dump/plains --seed 20260719 \
    --coarse-dir $SCRATCH/s1 --fine-dir $SCRATCH/s16 \
    --origin -844800 307200 --span 15330 --cell 30000 --cell 1875:2040 --tier both

# 4. every metric, both bands, at matched resolution
python terrain-service/tools/stage_realism_report.py \
    --case "plains:$SCRATCH/dump/plains:llano_estacado_plains,illinois_till_plain" \
    --case "alpine:$SCRATCH/dump/alpine:alps_valais_alpine,teton_range_alpine" \
    --blockmean-s1 "$SCRATCH/bakecache/S1_bake_-55_20_seed20260719.npy@plains" \
    --cache $SCRATCH/metrics_cache.json --out $SCRATCH/results.json
```

`vxc_bench --radius 128 --digest` = `10a4f667898da1c2` on the build used here
(llvm-mingw/ninja Release from `voxel-core/`), unchanged.

---

## 10. What this says to do next, in order

1. **Condition the client's detail amplitude on landform relief, not only on carrier
   gradient.** Relative to the landform's own detrended σ the detail is 12x more significant on the
   plain than on the mountain, and its effect on 1.875 m mean slope is +207% against
   +3.3% — a factor of 63.
   The measurable target is the slope-by-scale ladder in §4: plains S3 at 1.875 m must
   come down from 6.27° to the 0.9–2.1° the real DTMs measure, without moving the
   7.5 m rung, which is already correct.
2. **Give the bake something to put between 30 m samples.** `frac_ridge_peak` = 0.0031
   at a 10-cell lookout and Hurst 0.95 over the whole fine range both say the same thing:
   the 1.875 m surface is the smooth spline through the carrier plus a little noise. Any
   fix to (1) that only *removes* client detail leaves a featureless surface behind.
3. **Add a class gate to whatever CI check follows this.** §6.2 shows the realism gates
   passing a surface that is not a plain by any landform measure. The gate needs
   `frac_flat_coarse_thresh` and `frac_ridge_peak` against the reference for the tile's
   claimed class, not only the four realism metrics.
4. **Regenerate the world tiles through the shipping provider**, and fix whatever
   assigned "rolling" to a tile with 1364 m of relief.
5. **Record in `geomorph`'s own docs that `curvature_asymmetry` is undefined when the
   vertical LSB exceeds ~1% of the cell size**, with S0 and S4-fine as the worked
   examples.

---

## Appendix A — every metric, every field, both bands, all three classes

Generated from `docs/measurements/terrain-stage-metrics-2026-07-29.json`, which is the
raw output of `stage_realism_report.py` (one `geomorph.describe` result per field, with
its own `cell_m`). `S1 bake (16x16 block mean)` is the sampling-convention control
described in §2; `REF ... 3DEP DTM@30 m` is a 68² window and is not usable for Hack's
law or drainage density.


#### plains / band30  (n=508)

| metric | S0 wire 30 m | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | S1 bake (16x16 block mean) | REF llano_estacado_plains cop30 DSM | REF llano_estacado_plains 3DEP DTM@30 m | REF illinois_till_plain cop30 DSM | REF illinois_till_plain 3DEP DTM@30 m |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.0701 | 0.02044 | 0.0515 | 0.1116 | 0.1115 | 0.02093 | 0.05455 | 0.05466 | 0.01262 | 0.1012 | 0.004346 | 0.09712 | 0.003667 |
| `curvature_asymmetry` | 0 | 0.0755 | 0.01274 | 0.09321 | 0.08696 | 0.07125 | 0.04388 | 0.03572 | 0.09791 | 0.02017 | 0.08009 | 0.01562 | 0.07029 |
| `hack_h` | 0.5735 | 0.65 | 0.3963 | 0.6452 | 0.6322 | 0.6594 | 0.6688 | 0.6897 | 0.6986 | 0.5999 | -0.2207 | 0.5549 | 0.6585 |
| `hack_r2` | 0.9774 | 0.9964 | 0.8079 | 0.9964 | 0.9978 | 0.9925 | 0.9971 | 0.9963 | 0.9898 | 0.9954 | 0.05663 | 0.9979 | 0.8938 |
| `tail_asymmetry` | 0.9849 | 0.9722 | 1.00 | 0.9094 | 0.9083 | 0.973 | 0.9684 | 0.9688 | 1.05 | 0.9236 | 0.99 | 0.989 | 1.15 |
| `theta` | 0.009804 | 0.3649 | 0.0674 | 0.1819 | 0.1729 | 0.1546 | 0.1214 | 0.1125 | 0.06017 | 0.1841 | 0.1011 | 0.2834 | 0.2618 |
| `slope_area_r2` | 0.1759 | 0.6564 | 0.6861 | 0.5584 | 0.5401 | 0.8118 | 0.581 | 0.6267 | 0.5076 | 0.9472 | 0.8308 | 0.8727 | 0.9572 |
| `frac_flat` | 0.2511 | 0.2689 | 0.5637 | 0.0141 | 0.01511 | 0.2693 | 0.1991 | 0.2069 | 0.4711 | 0.6733 | 0.9952 | 0.4794 | 0.1606 |
| `frac_slope` | 0.1496 | 0.1535 | 0.1441 | 0.2025 | 0.2017 | 0.1533 | 0.1647 | 0.1629 | 0.1426 | 0.0684 | 0.000434 | 0.1289 | 0.3225 |
| `frac_ridge_peak` | 0.1045 | 0.1052 | 0.04068 | 0.2137 | 0.2125 | 0.1052 | 0.1261 | 0.1242 | 0.05283 | 0.04376 | 0 | 0.07973 | 0.0651 |
| `frac_valley_pit` | 0.1044 | 0.08689 | 0.02779 | 0.2521 | 0.2509 | 0.08688 | 0.1169 | 0.1146 | 0.03872 | 0.03599 | 0 | 0.08699 | 0.05035 |
| `frac_flat_coarse_thresh` | 0.9187 | 0.9172 | 0.9467 | 0.7978 | 0.7972 | 0.9173 | 0.9026 | 0.9025 | 0.9365 | 0.9794 | 1.00 | 0.9038 | 0.9926 |
| `mean_deg` | 1.23 | 1.16 | 1.09 | 1.44 | 1.44 | 1.16 | 1.22 | 1.22 | 1.09 | 0.7496 | 0.4162 | 1.16 | 1.30 |
| `p95_deg` | 3.37 | 3.16 | 2.92 | 3.31 | 3.31 | 3.17 | 3.23 | 3.22 | 2.99 | 2.15 | 0.8555 | 3.39 | 2.55 |
| `relief_m` | 192 | 195 | 192 | 194 | 194 | 195 | 196 | 196 | 196 | 52.1 | 15.0 | 56.3 | 31.8 |

#### plains / band1875  (n=1089)

| metric | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | REF llano_estacado_plains 3DEP DTM@1.875 m | REF illinois_till_plain 3DEP DTM@1.875 m |
|---|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.01346 | 0.02321 | 0.1145 | 0.1152 | 0.01454 | 0.06735 | 0.06758 | 0.01107 | 0.01965 |
| `curvature_asymmetry` | 0.09142 | 0 | 0.001932 | 9.54e-07 | 0.01863 | 0.001257 | 0 | 0.03529 | 0.01365 |
| `hack_h` | 0.5797 | 0.2689 | 0.7232 | 0.784 | 0.7163 | 0.5991 | 0.5907 | 0.5859 | 0.5461 |
| `hack_r2` | 0.985 | 0.3465 | 0.7214 | 0.9122 | 0.7245 | 0.9937 | 0.9923 | 0.9355 | 0.8163 |
| `tail_asymmetry` | 0.6136 | 0.8796 | 0.9975 | 0.9973 | 0.6329 | 0.9864 | 0.9855 | 0.9889 | 1.12 |
| `theta` | 1.94 | 0.1009 | 0.2722 | 0.2333 | 0.0758 | 0.3039 | 0.25 | 0.1477 | 0.08922 |
| `slope_area_r2` | 0.5888 | 0.9549 | 0.8652 | 0.8146 | 0.913 | 0.5373 | 0.5124 | 0.8977 | 0.5882 |
| `frac_flat` | 0.09529 | 0.3288 | 0 | 0 | 0.07095 | 0 | 0 | 0.5548 | 0.03806 |
| `frac_slope` | 0.3846 | 0.3681 | 0.1523 | 0.1522 | 0.3902 | 0.1023 | 0.1012 | 0.09363 | 0.4972 |
| `frac_ridge_peak` | 0.09607 | 0.01446 | 0.269 | 0.2685 | 0.09906 | 0.3518 | 0.3505 | 0.0537 | 0.07339 |
| `frac_valley_pit` | 0.05753 | 0.01756 | 0.2929 | 0.2962 | 0.06465 | 0.3426 | 0.3456 | 0.03934 | 0.05803 |
| `frac_flat_coarse_thresh` | 0.8513 | 0.9891 | 0.000298 | 2.2e-05 | 0.8312 | 2.37e-05 | 0 | 0.9425 | 0.8264 |
| `mean_deg` | 2.04 | 1.37 | 7.06 | 7.08 | 2.06 | 6.27 | 6.29 | 0.9102 | 2.13 |
| `p95_deg` | 5.25 | 3.31 | 16.4 | 16.4 | 5.31 | 12.3 | 12.4 | 2.76 | 5.79 |
| `relief_m` | 39.2 | 38.1 | 41.4 | 41.3 | 39.2 | 40.4 | 40.4 | 15.6 | 35.3 |

#### alpine / band30  (n=508)

| metric | S0 wire 30 m | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | S1 bake (16x16 block mean) | REF alps_valais_alpine cop30 DSM | REF teton_range_alpine cop30 DSM | REF teton_range_alpine 3DEP DTM@30 m |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.07207 | 0.01331 | 0.06631 | 0.06826 | 0.06824 | 0.01334 | 0.02001 | 0.01998 | 0.006715 | 0.09712 | 0.07486 | 0.001025 |
| `curvature_asymmetry` | 0 | 0.02673 | 0.01407 | 0.1925 | 0.1915 | 0.02748 | 0.001378 | 0.005332 | 0.01655 | 0.06309 | 0.04872 | 0.1071 |
| `hack_h` | 0.5234 | 0.4834 | 0.498 | 0.535 | 0.5155 | 0.4708 | 0.4987 | 0.4955 | 0.5096 | 0.5443 | 0.5162 | n/a |
| `hack_r2` | 0.8935 | 0.9936 | 0.8666 | 0.9387 | 0.9095 | 0.944 | 0.9946 | 0.9934 | 0.9949 | 0.8983 | 0.9468 | n/a |
| `tail_asymmetry` | 1.07 | 1.34 | 1.13 | 0.8139 | 0.8153 | 1.34 | 1.31 | 1.31 | 1.29 | 0.8692 | 0.7009 | 0.5126 |
| `theta` | 0.1937 | 0.2068 | 0.2317 | 0.2016 | 0.1999 | 0.1527 | 0.1426 | 0.1417 | 0.1925 | 0.2029 | 0.2831 | 0.3289 |
| `slope_area_r2` | 0.9439 | 0.7354 | 0.9723 | 0.9611 | 0.961 | 0.9642 | 0.9553 | 0.9563 | 0.7614 | 0.9699 | 0.982 | 0.9525 |
| `frac_flat` | 0.003267 | 0.04171 | 0.01324 | 0.000164 | 0.000189 | 0.0417 | 0.03539 | 0.03613 | 0.04306 | 0.000256 | 0.06293 | 0 |
| `frac_slope` | 0.4804 | 0.4622 | 0.4985 | 0.4675 | 0.4677 | 0.4623 | 0.4601 | 0.46 | 0.4713 | 0.5211 | 0.4118 | 0.3181 |
| `frac_ridge_peak` | 0.06033 | 0.05759 | 0.05272 | 0.06797 | 0.06783 | 0.05763 | 0.05879 | 0.05875 | 0.05408 | 0.05947 | 0.07949 | 0.1411 |
| `frac_valley_pit` | 0.07537 | 0.06475 | 0.06469 | 0.08317 | 0.08318 | 0.06479 | 0.06706 | 0.06699 | 0.06282 | 0.06469 | 0.07783 | 0.09983 |
| `frac_flat_coarse_thresh` | 0.07184 | 0.076 | 0.07784 | 0.05436 | 0.05435 | 0.076 | 0.07364 | 0.07366 | 0.07841 | 0.02311 | 0.1096 | 0 |
| `mean_deg` | 21.1 | 21.1 | 20.8 | 20.8 | 20.8 | 21.1 | 21.1 | 21.1 | 21.0 | 23.0 | 21.6 | 43.2 |
| `p95_deg` | 39.7 | 39.7 | 39.1 | 39.3 | 39.3 | 39.7 | 39.8 | 39.8 | 39.6 | 46.6 | 47.9 | 66.7 |
| `relief_m` | 1.83e+03 | 1.81e+03 | 1.82e+03 | 1.83e+03 | 1.83e+03 | 1.81e+03 | 1.81e+03 | 1.81e+03 | 1.8e+03 | 2.79e+03 | 2.08e+03 | 1.14e+03 |

#### alpine / band1875  (n=1089)

| metric | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | REF teton_range_alpine 3DEP DTM@1.875 m |
|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 8.62e-06 | 1.31e-05 | 0.007483 | 0.00764 | 1.91e-05 | 0.002442 | 0.002627 | 0.003423 |
| `curvature_asymmetry` | 0.006623 | 0.01988 | 0.008239 | 6.1e-17 | 0.01261 | 0.004874 | 0 | 0.03855 |
| `hack_h` | 0.7058 | -0.0804 | 0.5642 | 0.5949 | 0.7345 | 1.01 | 0.8899 | 0.8908 |
| `hack_r2` | 0.932 | 0.3008 | 0.6421 | 0.7255 | 0.9318 | 0.8737 | 0.8942 | 0.7767 |
| `tail_asymmetry` | 0.9191 | 0.9357 | 0.9853 | 0.9871 | 0.9628 | 0.9886 | 0.9874 | 0.8246 |
| `theta` | 0.02832 | 0.04788 | 0.06623 | 0.07105 | 0.02909 | 0.07816 | 0.07944 | 0.1765 |
| `slope_area_r2` | 0.7402 | 0.7347 | 0.9754 | 0.96 | 0.7279 | 0.9546 | 0.9541 | 0.9322 |
| `frac_flat` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `frac_slope` | 0.6429 | 0.6298 | 0.5153 | 0.5114 | 0.6435 | 0.5086 | 0.5007 | 0.3809 |
| `frac_ridge_peak` | 0.06351 | 0.06662 | 0.06147 | 0.06218 | 0.06313 | 0.06059 | 0.06215 | 0.08937 |
| `frac_valley_pit` | 0.003725 | 0.004067 | 0.02178 | 0.02274 | 0.003734 | 0.02138 | 0.02318 | 0.09891 |
| `frac_flat_coarse_thresh` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `mean_deg` | 26.8 | 25.7 | 28.1 | 28.2 | 26.8 | 27.7 | 27.7 | 44.8 |
| `p95_deg` | 38.1 | 36.0 | 46.8 | 46.9 | 38.1 | 43.9 | 44.1 | 75.8 |
| `relief_m` | 708 | 695 | 697 | 697 | 708 | 709 | 709 | 1.17e+03 |

#### rolling / band30  (n=508)

| metric | S0 wire 30 m | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | S1 bake (16x16 block mean) | REF iowa_rolling_hills cop30 DSM | REF iowa_rolling_hills 3DEP DTM@30 m |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.01953 | 0.01583 | 0.01859 | 0.02022 | 0.02032 | 0.0159 | 0.01911 | 0.01917 | 0.006237 | 0.07784 | 0.009425 |
| `curvature_asymmetry` | 0 | 0.004756 | 0.01679 | 0.2082 | 0.2042 | 0.004591 | 0.01082 | 0.01298 | 0.01163 | 0.02203 | 0.1119 |
| `hack_h` | 0.5762 | 0.5221 | 0.6023 | 0.6003 | 0.6006 | 0.5231 | 0.5207 | 0.5218 | 0.5343 | 0.5521 | n/a |
| `hack_r2` | 0.9942 | 0.9985 | 0.9902 | 0.9924 | 0.9931 | 0.9985 | 0.9982 | 0.9985 | 0.9968 | 0.9981 | n/a |
| `tail_asymmetry` | 1.16 | 1.36 | 1.19 | 0.8401 | 0.8404 | 1.37 | 1.34 | 1.34 | 1.30 | 1.10 | 1.52 |
| `theta` | 0.1646 | 0.1584 | 0.2009 | 0.1772 | 0.1762 | 0.1413 | 0.1458 | 0.1442 | 0.1529 | 0.2781 | 0.3874 |
| `slope_area_r2` | 0.8817 | 0.8223 | 0.8882 | 0.9 | 0.8988 | 0.857 | 0.8583 | 0.8604 | 0.8562 | 0.9921 | 0.9631 |
| `frac_flat` | 0.000118 | 0.000176 | 0.000424 | 0 | 4.2e-06 | 0.000181 | 0.000101 | 0.000101 | 0.000336 | 0.03613 | 0.03168 |
| `frac_slope` | 0.4176 | 0.4162 | 0.4384 | 0.411 | 0.4111 | 0.4163 | 0.4137 | 0.4136 | 0.425 | 0.2492 | 0.2513 |
| `frac_ridge_peak` | 0.08555 | 0.08967 | 0.07918 | 0.0876 | 0.08755 | 0.08963 | 0.09102 | 0.09098 | 0.08577 | 0.1779 | 0.2053 |
| `frac_valley_pit` | 0.0982 | 0.09293 | 0.08921 | 0.1001 | 0.1002 | 0.09283 | 0.09364 | 0.09365 | 0.09099 | 0.1782 | 0.148 |
| `frac_flat_coarse_thresh` | 0.02198 | 0.02003 | 0.02569 | 0.01383 | 0.01382 | 0.01999 | 0.01822 | 0.01822 | 0.02255 | 0.3012 | 0.2148 |
| `mean_deg` | 14.7 | 14.8 | 14.3 | 14.4 | 14.4 | 14.8 | 14.8 | 14.8 | 14.7 | 3.63 | 3.54 |
| `p95_deg` | 30.3 | 30.7 | 29.6 | 29.8 | 29.8 | 30.7 | 30.7 | 30.7 | 30.4 | 8.31 | 6.99 |
| `relief_m` | 1.36e+03 | 1.37e+03 | 1.36e+03 | 1.36e+03 | 1.36e+03 | 1.37e+03 | 1.37e+03 | 1.37e+03 | 1.37e+03 | 87.0 | 53.7 |

#### rolling / band1875  (n=1089)

| metric | S1 bake | S2 carrier (coarse) | S3 surface (coarse) | S4 voxels (coarse) | S2 carrier (fine) | S3 surface (fine) | S4 voxels (fine) | REF iowa_rolling_hills 3DEP DTM@1.875 m |
|---|---|---|---|---|---|---|---|---|
| `fill_volume_per_area_m` | 0.000529 | 0 | 0.01064 | 0.01067 | 0.000546 | 0.00582 | 0.005885 | 0.004358 |
| `curvature_asymmetry` | 0.00911 | 0.001347 | 0.007017 | 1.17e-05 | 0.00035 | 0.004176 | 5.74e-17 | 0.04596 |
| `hack_h` | 0.7356 | -0.2157 | 0.5622 | 0.5369 | 0.7561 | 0.5259 | 0.514 | 0.5077 |
| `hack_r2` | 0.9422 | 0.6009 | 0.6773 | 0.6309 | 0.937 | 0.9823 | 0.9876 | 0.9723 |
| `tail_asymmetry` | 0.9136 | 0.9768 | 0.9927 | 0.9908 | 0.9165 | 0.9897 | 0.9916 | 1.23 |
| `theta` | 0.04585 | 0.08392 | 0.08345 | 0.08438 | 0.04579 | 0.07323 | 0.0727 | 0.3182 |
| `slope_area_r2` | 0.8831 | 0.8674 | 0.9917 | 0.9751 | 0.886 | 0.8719 | 0.8498 | 0.6495 |
| `frac_flat` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.003169 |
| `frac_slope` | 0.6097 | 0.5953 | 0.4583 | 0.457 | 0.6107 | 0.4386 | 0.437 | 0.3563 |
| `frac_ridge_peak` | 0.02004 | 0.009226 | 0.05438 | 0.05446 | 0.01987 | 0.06515 | 0.06572 | 0.1882 |
| `frac_valley_pit` | 0.01101 | 0.01505 | 0.05598 | 0.0565 | 0.01098 | 0.05359 | 0.05477 | 0.05049 |
| `frac_flat_coarse_thresh` | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.1202 |
| `mean_deg` | 18.1 | 16.7 | 19.7 | 19.7 | 18.1 | 19.5 | 19.5 | 5.34 |
| `p95_deg` | 33.3 | 28.1 | 38.0 | 38.0 | 33.3 | 36.9 | 36.9 | 14.6 |
| `relief_m` | 652 | 644 | 644 | 644 | 652 | 652 | 652 | 54.4 |
