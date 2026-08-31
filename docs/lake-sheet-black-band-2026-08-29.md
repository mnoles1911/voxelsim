# The black band around every lake: the water's absorption depth is unbounded

**Owner-reported 2026-08-29** from aerial captures: *"the small pond/lake ... have
a black graphical rendering bug outline around the border/edge of the water
body."*

## It is the SHEET, proven by subtraction

Matched captures, alpine (-54233, -68221), 250 m up, pitch -45, everything else
identical:

    arm                    dark px (frame)   dark px (pond region)
    default                    14,644              14,122
    -VoxelLakeSheets=0             49                   0

**Not terrain, not lighting, not the LOD colour fix** (which was armed in both --
its own A/B is byte-identical here).

## Root cause

The sheet uses `MSM_SingleLayerWater`. That model's absorption depth is the
engine's `BehindWaterSceneDepth - WaterSurfaceSceneDepth` -- a distance measured
**along the view ray**. `create_water_voxel_material.py` blends two depths:

    total = engineAbsorb * (1 - a*valid) * sceneRayDepth        <- ALONG THE RAY
          + engineAbsorb * (a*valid)     * slantBakedDepth      <- baked vertical

with `a = BathyDepthAuthority = 0.85`. **The engine half is unbounded and the
material cannot override it** -- the generator says so in its own comment:
*"there is no material input that overrides it."*

The sheet's extent mask is quantised to 1.875 m cells at the finest LOD band and
**over-covers** the basin deliberately, because a mask that under-covers leaves a
gap at the shoreline ([[voxelsim-lake-extent-two-directions]]). On an
over-covered cell the bake answers DRY -- and "dry" is `validity = 1, depth = 0`
(`VoxelBathyField.cpp:295-304`), not `validity = 0`. So:

    baked half  = 0.85 * 0            = 0
    engine half = 0.15 * sceneRayDepth

At a grazing view over ground falling away past the shore, `sceneRayDepth` runs
to hundreds of metres. 0.15 of that is still tens of metres of water, absorption
saturates, and all that survives is the Fresnel sky term.

**Three independent confirmations:**

1. `docs/water-appearance-plan-2026-08-11.md:95` predicted it three weeks ago:
   *"Thickness today is scene depth - pixel depth ... **It goes wrong at grazing
   angles.**"*
2. The band scales with grazing-ness -- share of the lake's own pixels that are
   near-black: **14.0% at pitch -18, 8.6% at -45.**
3. The residual colour is **RGB (26, 30, 36)** against snow at (205, 201, 197):
   never zero, and BLUE-dominant. That is a fully-absorbed body with only the sky
   reflection left, not a missing-geometry hole (which reads as sky) and not a
   shader writing black (which would be exactly 0).

**This is a band around EVERY basin.** Nothing about this pond is special; it is
where the owner happened to look, and it is worst at grazing angles.

## The fix I tried, and it FAILED -- read this before repeating it

`-VoxelWaterDepthAuthority=<float>` overrides `BathyDepthAuthority` for SHEETS
ONLY through one shared `UMaterialInstanceDynamic` (unset = no instance created,
so the control arm is byte-identical). Matched captures, engagement proved by the
latch line in every leg:

    pose                    dark px CONTROL   dark px AUTHORITY 1.0
    80 m  pitch -18              27,003              26,948
    250 m pitch -45              14,644              14,651

**No effect on the band.** The switch is NOT inert -- it moved the lake's own
pale-water pixel count from 166,485 to 129,965 at the same pose, so the parameter
reaches the material and changes the water's grading. It simply does not touch
the black band.

**What that rules in.** At authority 1.0 the engine's weight is `1 - 1*validity`.
If the band's cells had `validity = 1` the engine term would be zero there and the
band would have gone. It did not, so **the band's cells must have validity 0** --
the bake has NO ANSWER there, not "an answer that says dry".

That is consistent with the same root mechanism (an unbounded along-view-ray
absorption term) but relocates the trigger: it is not the over-covered dry cells
inside the bake's coverage, it is cells the bathymetry field never filled. The
deciding number is the BathyField HOLE FRACTION, which was logged at **Verbose**
and therefore invisible on every leg ever run; it is now at Log.

**A hole fraction near 1.0 with lakes on screen means the whole baked-bathymetry
path is inert at runtime**, which would make every water surface fall back to the
view-dependent screen-space depth -- exactly what `VoxelBathyField.cpp:63` warns
about. NOT YET MEASURED. Do not assert it.

## Why re-weighting could NEVER have worked -- the real shape of it

Two measurements settled it:

* **BathyField hole fraction is 0.0%** (`holes=0.0% fill=5.94ms`, one published
  960 m window). So validity is 1, not 0. My second guess is refuted too.
* **With the sheet off there is NO BASIN.** The ground under the "lake" is flat
  snow with faint contour lines -- no bowl, no excavation. Terrain under the
  black band reads (203.6, 199.7, 194.8) and under the pale sheet (196.8, 193.1,
  188.4): the same bright ground either side.

So the sheet is a flat plane over FLAT ground -- surface and bed very nearly
COPLANAR -- and the along-view-ray separation between them diverges as the view
gets grazing. That is the unbounded quantity.

**The baked half cannot cause it.** `bathy_field_graph.py:139-180` converts the
baked vertical depth to a slant length through Snell at IOR 1.33, which is
provably bounded in `[d, 1.52 d]` for every view direction. That bound is
deliberate and well argued; it is not the problem.

**The engine half is unbounded AND uncancellable.** `MSM_SingleLayerWater`
computes its own `BehindWaterSceneDepth - WaterSurfaceSceneDepth`, unrefracted,
and the material's only hook is `ColorScaleBehindWater`, which MULTIPLIES the
scene colour behind the water *before* the engine's transmittance. The blend is
implemented as a cancellation: brighten the scene colour to undo the engine's
over-absorption. **A multiply cannot recover a value the engine has already
flushed to zero.** Once `exp(-absorb * hugeSlant)` underflows, `0 * anything` is
still 0.

**That is exactly why authority 1.0 changed the water's grading everywhere else
and did nothing to the band.** The band is the region where the engine term has
already saturated, and no re-weighting reaches it.

## Where the fix has to be

Not in the blend. Either:

1. **Do not draw sheet where the bake says there is no water column.** The bake
   already ships a signed distance to shore (`G`, 100 mm LSB) precisely so the
   waterline can be placed sub-pixel instead of on the 1.875 m raster
   (`VoxelBathyField.h`). It currently feeds FOAM only
   (`create_water_voxel_material.py:1574-1626`) and does not gate the sheet's own
   coverage. Gating opacity on it removes the near-coplanar over-covered band
   outright, at every basin.
2. **Or give the basin a bed.** The water is drawn on flat ground; a lake with an
   actual excavated bowl is not near-coplanar with its own surface and the engine
   term stays small. That is a bake question, not a rendering one.

(1) is the rendering fix and is general. Both need an owner decision and (1)
needs a material regeneration through `tools/voxel-sky-chain-regen.ps1`.

## The correction that matters

My first writeup asserted the trigger was the over-covered cells at
`validity = 1, depth = 0`. **That is refuted by the table above.** The mechanism
(unbounded engine absorption along the view ray) still stands on the subtraction
test and the grazing-angle scaling; the question of WHICH cells fall through it
is open.
