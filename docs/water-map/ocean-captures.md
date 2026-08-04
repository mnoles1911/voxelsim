# The ocean captures — work item 8, §6.4

The three visual claims the watershed plan listed as **"still outstanding, and
it needs the editor"**: the inland pit, the breach with its
`voxel.Water.ImplicitOcean 0` control, and the plane/voxel-water tone.

**No judgement of how these look is offered here.** The owner judges
screenshots; readings taken here have been wrong in both directions before.
What follows is conditions, settle evidence, and numbers.

    build      claude/ocean-captures off claude/water-integration (fa16d55)
    editor     VoxelEarthEditor Win64 Development, built from this tree
    world      seed 20260719, provider terrain-diffusion-unlabeled-71e2b362e3241e71
    fine tier  ENABLED, provider ...-b2dffc351, 0 refused, 0 identityMismatch,
               0 gateLeaks on every run
    sun        FROZEN 12:00 on 03-20, `voxel.Sky.ExposureMode` at its default 2
               (manual EV curve against sun altitude — brightness is NOT a
               function of what was on screen a moment ago, which is what makes
               a cross-run A/B possible at all)
    frames     2560x1440

---

## The sites, and how they were chosen

Both sites are **named on the command line, never searched for at run time**,
and both were re-derived at the exact column through the engine's own
`GetSurfaceHeightUU` — the same amplified worldgen ground that
`oceanSurfaceMmAt(groundMm)` is gated on. `-VoxelOceanSurvey=<radiusM>` prints
that function over a grid and exists for this. Three of nine vista sites in
this project were wrong when they were picked off a map by eye.

Both sites sit inside the baked fine-tile set (tile (−4,−10) and its baked
neighbours (−5,−10), (−4,−11), (−5,−11), (−3,−11)), at least 4.2 km from the
boundary with any tile that is **not** baked.

| | column (m) | engine ground | note |
|---|---|---|---|
| inland pit | `-59047,-150847` | **+4.70 m**, above the datum | 270 m from the nearest below-datum column |
| breach | `-61329,-146132` | **−1.72 m**, below the datum | on a shore that rises to +36.9 m within 40 m |

The first candidate shore was **rejected on its survey**: at
`-57900,-149317` the 80 × 80 m grid read min −16.29 m, max **+0.19 m** — a
shoal that barely breaks the surface, not a coast. `ocean-shore-site.png` is
the accepted site before anything was dug.

**Framing is bounded by the water, not by taste.** Near-field implicit water
exists only within `kImplicitRadiusBricks`/`kImplicitRadiusBricksZ` of the
camera brick — ±25.6 m in xy, ±12.8 m in z. Every dig here is inside that box
and the pose says so; a dig outside it is meshed correctly and is invisible.
No existing fixture could put a dig there (`-VoxelHeadlessDigTest` carves at
pawn+100 m; `-VoxelBreachTest` scans around the world ORIGIN, not around spawn,
whatever its comment says; `-VoxelDigDownTest` carves a 60 m shaft, deeper than
the z half-box). Hence `VoxelOceanCaptureFixture`.

---

## 1. The inland pit — `ocean-pit-dry.png`

Dug the same three-pass way `test_ocean.cpp`'s
`reservoir_v0_floods_an_inland_pit_the_datum_test_leaves_dry` digs it, through
`UVoxelWorldSubsystem::CarveSphere`, the authority edit path a player's
explosive uses.

    pose      -59047,-150859, +4.0 m above ground (camera z 8.7 m),
              pitch -25, yaw 90; pit centre 12 m ahead
    site      worldgen surface +4.70 m ABOVE the datum
    pass 1    surface -> datum,   z  4.70 ->  0.00 m,   4 edits, 181,148 voxels
    pass 2    first band below,   z -0.10 -> -1.50 m,   1 edit,    8,484 voxels
    pass 3    "keep digging",     z -1.60 -> -3.00 m,   1 edit,   41,532 voxels

Water after **every** pass and 40 s after the last:

    activeBricks=0  storedBricks=0  volume=0  maxFill=0
    digest=0xCBF29CE484222325   (unchanged from the empty world)

Pass 3 is the pass that broke Reservoir v0 — its top cell has pass 2's own air
above it, below the datum and holding no CA fill, which is verbatim v0's
"adjacent cell is implicit ocean" test. Zero bricks were mobilised by any of
the three.

The pit floor at −3.00 m is inside the camera's ±12.8 m z box (camera z 8.7 m
→ box −4.1 .. 21.5 m), so "dry" is a measurement and not a consequence of the
water never having been evaluated there. `RefreshImplicitWater` is logged
rebuilt at the camera brick.

Settle: `jobsInFlight=0 pendingJobs=0 unloaded=0`, peak loaded 27,184.

## 2. The breach and its control — `ocean-breach.png` / `ocean-breach-control-noocean.png`

Identical pose, identical sun, identical dig; the single variable is
`voxel.Water.ImplicitOcean`.

    pose        -61329,-146118, +18.0 m above ground (camera z 5.1 m),
                pitch -20, yaw 270; channel starts 14 m ahead
    dig         3 sphere edits, radius 4 m, 4 m of channel on heading 270,
                each sphere centred on its own column's surface
    removed     284,313 voxels in BOTH arms — identical to the voxel
    shutter     cam loc=(-6132900, -14611826, 583) rot=(pitch -20.0 yaw -90.0)
                in BOTH arms — identical to the unit

|  | ImplicitOcean 1 | ImplicitOcean 0 (control) |
|---|---|---|
| pre-dig | 0 bricks, 0 units | 0 bricks, 0 units |
| post-dig | maxFill **255** | maxFill **0** |
| +40 s | activeBricks 19,636, stored 10,449, **volume 501,024,510** | activeBricks **0**, stored **0**, volume **0** |
| digest | 0xDF1606318232F760 | 0xCBF29CE484222325 (unchanged) |

The control latched and said so: `voxel.Water.ImplicitOcean = 0 (watershed
§6.4). The sea is ABSENT from the water ImplicitFn`.

### The control is sound, and here is the evidence rather than the assertion

The last control shipped in this project differed from its pair by 85% of
pixels because the exposure moved. This one was checked three ways.

**Exposure did not move.** The same rock rectangle (0,80–600,260), which no
water touches in either arm:

    ImplicitOcean 1   R 8.6  G 4.7  B 2.0   blueness -4.7
    ImplicitOcean 0   R 8.8  G 4.9  B 2.1   blueness -4.8

**The in-session noise floor was measured, not assumed.** The capture path
takes two frames 2 s of world time apart; `ocean-breach.png` and
`ocean-breach-repeat.png` are that pair, 13 s of wall clock apart in one
process:

    0.68% of pixels differ by >24/255, mean max-channel delta 1.67/255
    all of it in the dark rock at the top of frame; the water band is 0.0-0.8%

**The pair's difference is localised to the water.** At the same threshold:

    6.63% of pixels differ by >24/255, mean max-channel delta 5.39/255
    CONCENTRATION 0.55 in the densest 10% of cells (0.10 = perfectly diffuse)

and the per-cell grid puts 42–61% differing pixels in the lower-centre cells —
where the shell draws — against 0.0–2.5% in the lower-left and lower-right,
which is where the plane draws in both arms. That is ten times the noise floor,
in the right place. `tools/imgdiff.py --where` prints all of it.

## 3. The plane/voxel tone — measured on `ocean-breach.png`

    blueness = B - (R + G)/2, per pixel, 0..255 sRGB, meaned over a rectangle

**The statistic is calibrated against the recorded lake number.** Applied to
the archived `lake-sheet-handover.png` it returns **+47.2** on the sheet side
and **+75.9** on the voxel side, against the **+44.5 / +76.8** recorded in the
plan for that seam — so this is the same statistic, and the numbers below are
comparable to it.

**The control is what makes the measurement clean.** Two rectangles in the same
frame differ in distance, depth and seabed as well as in renderer. In the
control both rectangles are pure plane, so their difference there is the
confound, and it can be subtracted:

| rectangle pair | ImplicitOcean 1 | control (both plane) | mismatch |
|---|---|---|---|
| (200,900–560,1080) vs (700,900–1060,1080) | +36.3 → +29.7 = **−6.6** | +36.2 → +36.4 = +0.2 | **−6.8** |
| (200,1150–560,1330) vs (700,1150–1060,1330) | +35.8 → +39.4 = **+3.6** | +35.8 → +36.1 = +0.3 | **+3.3** |

Taken over the whole water band as a 100 × 100 px grid of
`blueness(ON) − blueness(OFF)` — the plane acting as its own reference, so
distance, depth and seabed cancel exactly:

    175 tiles over y 700..1400
    102 tiles where the shell does not draw:  mean -0.01, max |d| 0.47
     73 tiles where the shell DOES draw:      range -14.1 .. +9.0, mean -1.3

So the plane and the voxel shell **do not agree, and do not disagree by a
constant**: over one join in one frame the shell reads up to 14.1 blueness
units LESS blue than the plane it covers in the far half of the shell, and up
to 9.0 units MORE blue in the near half. The sign flips with viewing angle,
which is the signature §6.4 predicted — one surface against a shell the view
ray crosses a varying number of times. The 23-unit span is comparable to the
32-unit lake mismatch (+44.5 → +76.8) that this was expected to resemble.

The instrument's own floor is ±0.5 blueness (the 102 plane-vs-plane tiles).

---

## What could not be captured, and why

**The river/sea join could not be measured — there is no baked river anywhere
in this tile cache.** Every one of the 17 baked fine tiles decodes at
`bake_ver=7` with `basins=0` and no water plane; the runtime says the same
thing in its own words (`Lakes come from the basin table (bake_ver 8) and
rivers from the water plane (bake_ver 9); a tile baked before either carries it
not`). A river mouth needs a tile carrying both a reach and a coast, and none
exists yet. Measuring the plane/voxel join at a *breach* — which this does — is
the same pairing of renderers, but it is not the river-mouth frame and is not
offered as it.

---

## Two defects this turned up

**1. `tools/voxel-capture.ps1` cleared the edit log but not the water blob, and
it silently contaminated the next run.** Persisted world state is two files —
the `.vxlog` edit log and the ADR-0005 `.vxwater` CA fill blob, which is
irreducible simulation state and is *not* re-derivable from seed + edit log.
Only the first was cleared. Measured here: a breach run left 4.1 MB of
`.vxwater`, and the next run logged `LoadWaterState: restored 886,179,570 fill
units across 38,712 stored brick(s), 17,235 mobilized brick(s)` before its own
dig. It came up already flooded. **Run after the ocean-on arm, the
`ImplicitOcean 0` control — a run whose whole content is "no water appears" —
would have inherited the ocean-on arm's water and photographed it**, and the
pair would have read as a null result. Fixed in this commit; the script now
clears both and prints what it discarded. The control above was taken after the
fix, from a verified `no water blob ... fresh world`.

**2. A breach into the open sea starts a mobilisation front that does not
stop.** This is not a settle failure of the capture; it is the documented
behaviour of `WaterMobilizer::advanceFront`, which "has NO LENGTH BOUND … it is
DRAINAGE, not disturbance, that runs away", bounded only by
`setFrontGate`/`setMobilizedCeiling` — **neither of which `UVoxelWaterSubsystem`
ever calls**. Measured over the six minutes after the dig:

    activeBricks   19,636 -> 41,613      (monotone)
    volume        501.0M -> 884.4M units (monotone)
    water tickMs   2,000 -> 2,547 ms     (against a 10 Hz fixed step)

and the engine says so itself, repeatedly:
`WaterCA over budget: steppedBrickCount=41,615 > voxel.Water.MaxActiveBricks=4096
(tick ran in full -- no safe mid-step cutoff exists; raise the cap or
investigate runaway spread)`.

**The frame is nevertheless stable while that runs**, which is why it is
offered at all: the two shutter frames 13 s apart differ by 0.68% of pixels at
>24/255 with none of it in the water. The front is converting sea *outside* the
frame that the plane was already drawing at the same datum. The capture is
honest as a picture of the join; it is **not** a settled world, and the numbers
above are the reason.

---

## Reproducing

    tools\voxel-capture.ps1 -Name ocean-pit-dry `
      -SpawnAt '-59047,-150859' -SpawnAltM 4 -SpawnPitch -25 -SpawnYaw 90 `
      -SettleSec 200 -TimeoutSec 900 -ExtraArgs @(
        '-VoxelTileDir=<cache>\terrain-diffusion-unlabeled-71e2b362e3241e71\000000000135276f\s1',
        '-VoxelFineTileDir=<cache>',
        '-VoxelOceanDig=pit', '-VoxelOceanDigAt=-59047,-150847',
        '-VoxelOceanDigRadiusM=3', '-VoxelOceanDigDepthM=3',
        '-VoxelOceanDigAfter=100', '-VoxelOceanDigReport=40')

    tools\voxel-capture.ps1 -Name ocean-breach-on `
      -SpawnAt '-61329,-146118' -SpawnAltM 18 -SpawnPitch -20 -SpawnYaw 270 `
      -SettleSec 200 -TimeoutSec 900 -ExtraArgs @(
        ...same two tile switches...,
        '-VoxelOceanDig=breach', '-VoxelOceanDigAt=-61329,-146132',
        '-VoxelOceanDigRadiusM=4', '-VoxelOceanDigLenM=4',
        '-VoxelOceanDigHeadingDeg=270',
        '-VoxelOceanDigAfter=100', '-VoxelOceanDigReport=40')

The control is the same line plus `-Cvars 'voxel.Water.ImplicitOcean 0'` and
nothing else changed. Logs for every run are in `Saved/capture-<name>.log`.
