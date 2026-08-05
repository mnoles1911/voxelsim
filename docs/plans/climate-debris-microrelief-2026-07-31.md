# Climate-conditioned erosion, debris fields, biome micro-relief

**Status, updated 2026-08-05: Gap 1 is largely closed, Gaps 2 and 3 are untouched.**
Written 2026-07-31, after an audit of the pipeline against the Gaea/Quadspinner
stage model (primitives → structural → erosion → post-process/surface).

> **Gap 1 — "the bake has no climate conditioning at all" — was closed by
> landform provinces Tier 1** (`BAKE_VERSION` 7, commit `4f9a6e7`, 2026-08-01).
> The grep below now returns matches in `province.py`, `pipeline.py`,
> `basins.py` and `water.py`. Erosion is no longer byte-identical everywhere:
> `province.py` builds six **per-cell parameter fields** from the terrain and its
> climate — `profile_K_dt` (the incision coefficient this plan asked for),
> `a_crit_m2`, `stream_m`, `gate_q`, `meso_amp15_m`, `meso_amp11_m` — consumed by
> `incise.py` via `field_scale` and `noise.py` via `amp_scale`.
>
> Two details of how it was done, because they are load-bearing and this plan did
> not anticipate them. Provinces are a **per-cell field, never a per-tile constant
> set** — two adjacent tiles baked with different constants disagree along their
> shared edge. And every climate discriminant is **smoothed to landform scale
> before it reaches a threshold**, because climate arrives at 30 m and uint8
> quantised (precipitation's least significant bit is 47 mm/yr) and would
> otherwise print 30 m blocks into the erosion intensity.
>
> **Stage A below is therefore partly redundant.** What Tier 1 did not do:
> per-material weathering rates and climate-varying repose angles (`stream_n` and
> `incision_cap_m` were left out on purpose — they are numba kernel scalars).
>
> **Gaps 2 (debris and boulder fields) and 3 (biome-varying micro-roughness) are
> unstarted.** Both are client-side and neither is blocked.
>
> Current pipeline state: `docs/world-generation-architecture.md`, §6.5 for
> provinces.

## What the audit found

Measured against the reference pipeline, stages 1–3 are in good shape and stage 3 is in places
ahead of it — we run real geomorphology (priority-flood fill, MFD accumulation with D8
centrelines, stream-power incision `K·A^m·S^n`, mass-conserving slope-limited thermal
relaxation with a per-material repose field, and the couloir pass) rather than an artistic
approximation of it. Structural modification is present too: landform-scale domain warp (v19),
folded strata, and bench-and-cliff structure standing in for fractal terracing.

Two gaps are real, and both were called out by the owner before this audit ran.

### Gap 1 — the bake has no climate conditioning at all

```
$ rg 'precip|temperature|climate|biome' terrain-service/terrain_service/bake/*.py
(no matches)
```

Every tile erodes with byte-identical parameters. A rainforest and a hyper-arid range get the
same incision coefficient, the same repose angles, the same weathering rate. This is a
first-order realism miss, because precipitation *is* the discharge term that stream power is
written in, and the fluvial/thermal balance is what makes arid and humid landscapes look
different at a glance:

| | humid (high P) | arid (low P) |
|---|---|---|
| dominant process | fluvial | thermal / mass-wasting |
| drainage density | high, deeply dendritic | low, sparse, disconnected |
| divides / crests | rounded, soil-mantled | angular, bare rock |
| footslopes | concave, soil-filled | debris-mantled, straight |

**The data is already on the tile the bake loads.** The coarse `.vxtl` carries four uint8
climate channels beside the int16 elevation — temperature (`bio_1`), seasonality (`bio_4`),
precipitation, precip variability — and `assemble_padded_coarse` reads the elevation plane and
ignores the rest. The client already reads these for biome classification and topsoil depth
(`cachedClimate`, `classifyBiome`), so this is plumbing an existing field into existing
parameters, not new data.

The strength field added at bake_ver 5 is *lithologic* — world-anchored rock variation. It is
orthogonal to this and stays.

### Gap 2 — no discrete debris or boulders anywhere

There is not a clast, boulder or block object in the codebase. "Talus" in our pipeline is only
a continuous slope at the angle of repose in the heightfield. The original amplification plan
deferred this explicitly ("not in the bake: clasts, stratigraphy, overhangs"), correctly at the
time — but at 10 cm voxels it is a conspicuous absence. Real scree is *made of blocks*, cliffs
shed angular rockfall, channel beds are cobbled.

Secondary benefit worth stating plainly: discrete objects break contour continuity by
occlusion. Every anti-banding fix so far has perturbed a continuous surface; debris interrupts
it. That is a different mechanism from anything tried in the twenty-odd attempts to date.

### Gap 3 — micro-roughness is global, not biome-varying

The client's fine ladder (3.2 m / 1.6 m / 0.4 m / 0.2 m) is gated by slope and curvature but
not by climate. Desert pavement, alpine frost-shattered rubble and grassland turf get the same
high-frequency signature.

---

## Stage A — climate-conditioned erosion (bake, `bake_ver` bump)

Plumb the coarse tile's climate channels through the padded assembly (they need the same apron
treatment as elevation) and derive two normalised fields at fine resolution: **wetness** from
precipitation, and **frost** from temperature crossed with seasonality (freeze–thaw intensity
peaks where the mean sits near 0 °C *and* seasonality is high — not simply where it is cold).

Consume them at four existing knobs. All four are single-parameter changes to code that already
exists; none is a new stage:

1. **Incision coefficient** scales with wetness. This is physically the discharge term already
   implicit in `K`, so it is the most defensible of the four. Expect drainage density to
   separate visibly between wet and dry tiles — which is the single strongest cue for "where on
   Earth am I".
2. **Fluvial/thermal balance** — arid tiles get relatively more thermal relaxation per unit
   incision, humid tiles the reverse. This is what produces angular-vs-rounded divides.
3. **Repose angle** opens slightly in arid regimes (dry granular debris stands steeper than
   soil-mantled, vegetated slopes) and closes where soil forms.
4. **Weathering rate** scales with frost, so periglacial terrain shatters and mantles while
   warm-stable terrain does not.

**Verification.** Bake one wet and one dry tile from the corpus and measure drainage density,
mean divide curvature, and the slope histogram. The pass criterion is *separation*: the two must
differ in the direction the table above predicts, by a margin larger than tile-to-tile variance
within a class. A change that improves nothing measurable but makes both tiles differ from
today is not a pass — that is the trap the meso-band work fell into twice.

Drainage invariants (0 interior sinks, 0.0% stranded) must hold on both, as for every bake
change.

**Risk.** The climate raster is 30 m/px and quantised to uint8; naive use will print 30 m
blocks into erosion intensity — exactly symptom 3 from the original plan, one level up. It must
be smoothed to landform scale (hundreds of metres) before it modulates anything. This is the
one part of Stage A that can go visibly wrong, and it is the part to test first.

---

## Stage B — debris and boulder fields (client, O(1), no wire change)

Deterministic per-voxel, integer, no bake bytes, no format change — it drops into the existing
bounded 3D density band, which already has the gating machinery and a proven bound.

Three populations, each with a different placement rule, because they are different processes:

- **Rockfall blocks** below cliffs: gated on being downslope of a steep face within a fall
  shadow, largest near the apex, angular.
- **Scree clasts** on talus slopes: gated on slope near repose plus low material strength;
  dense, small, well-sorted (fining downslope, which is real and cheap — sorting by distance
  from the source face).
- **Channel cobbles**: gated on the flow plane the bake already ships, so beds are cobbled and
  banks are not.

Size distribution should be power-law (real clast populations are), truncated so a single
boulder is at most a few voxels — we are not placing landmarks, we are breaking up surfaces.

**Cost is the thing to watch.** The existing 3D band is aggressive-gated for a reason: the
density pass costs +5–10 % world-average only because both gates reject early. Debris must
respect the same discipline — zero work on flat, low-slope, non-channel ground, which is most
of the world. Budget: no measurable change to `vxc_bench` amplify stage outside cliff and talus
chunks.

**Determinism.** Same rules as the rest of the client: integer only, CPU/GPU mirrored in
`worldgen.ush` (which carries its own constant copies — a CPU-only edit changes nothing the
renderer draws), `kWorldGenVersion` bump, `vxc_gpu` digest parity.

---

## Stage C — biome-varying micro-roughness (client, same version bump as B)

Modulate the fine ladder's amplitudes and spectral tilt by biome rather than shipping one
global signature: desert smoother with ripple anisotropy, alpine coarser and more angular,
vegetated-temperate mid. The gate structure exists; the amplitudes become functions of the
already-read climate sample instead of constants.

Fold into Stage B's version bump — worldgen version bumps invalidate saved edit logs, so batch
them.

---

## Sequencing

Stage A first and alone: it is bake-side, it has the largest realism-per-unit-effort, and it
lands without a client version bump. B and C share one bump and follow.

All three touch files the couloir/dissection work is currently editing (`pipeline.py`,
`noise.py`, `amplifier.cpp`, `worldgen.ush`), so none of this starts until that lands.

## What this plan does not do

It does not address the standing banding problem — that is the couloir and normal-blending work
in flight. Stage B helps incidentally by occlusion, but it is not the fix and should not be
sold as one.
