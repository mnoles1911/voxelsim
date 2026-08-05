# Wet country, re-baked at bv14, with three verified spawn sites

The owner asked to fly rivers in wet alpine country and was flown down the arid
corridor instead. This is the data half of the correction: the six wet tiles
re-baked with the current water rules, the ground proved not to have moved, and
three places to stand that were checked against the shipped bytes before being
handed over.

Companion to `docs/water-wet-country-2026-08-05.md`, which picked the region and
recorded its long profile at bv13. Everything there still holds; both of its
caveats are repeated at the bottom of this note, because they are still true.

## What changed

Seed 20260719, `TERRAIN_VERSION` 8, **`BAKE_VERSION` 14**, no `--diagnostic`.

Six tiles, `(-5,-4) (-4,-4) (-3,-4) (-5,-5) (-4,-5) (-3,-5)`, 2,386 CPU-s.

* cache root `D:/vox-wet-cache` (unchanged — isolation here is by root)
* **new fine namespace `terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c`**

That namespace string is the same one the arid corridor's bv14 tiles carry,
because it is content-addressed off the bake fingerprint and the bake is the
same bake. **The two sets are told apart by their cache ROOT and nothing else.**
`D:/voxelsim/tile-cache/...-b10cf6d2c` holds the four corridor tiles;
`D:/vox-wet-cache/...-b10cf6d2c` holds these six. Point the client at the wrong
root and it will not error — the tile coordinates simply will not be there, and
missing fine tiles look exactly like a world with no water in it.

The bv13 namespace `-ba9c62170` is untouched and still loadable.

## The ground did not move

`tools/verify_water_only_change.py`, shipped-bytes against shipped-bytes,
bv13 `-ba9c62170` vs bv14 `-b10cf6d2c`, on all six tiles:

| tile | datum | elevation_cp | flow | wet cells bv13 → bv14 |
|---|---|---|---|---|
| (-5,-4) | SAME | IDENTICAL | IDENTICAL | 365,776 → 371,599 (+1.59%) |
| (-4,-4) | SAME | IDENTICAL | IDENTICAL | 223,226 → 235,235 (+5.38%) |
| (-3,-4) | SAME | IDENTICAL | IDENTICAL | 251,035 → 261,512 (+4.17%) |
| (-5,-5) | SAME | IDENTICAL | IDENTICAL | 507,605 → 518,569 (+2.16%) |
| (-4,-5) | SAME | IDENTICAL | IDENTICAL | 407,550 → 414,250 (+1.64%) |
| (-3,-5) | SAME | IDENTICAL | IDENTICAL | 396,674 → 400,977 (+1.08%) |

**PASS, 6 tiles, 0 problems.** Both sides are CODEC_RAW at quant 100 mm and both
are read through `decode_v2`, so this is a like-against-like comparison through
the codec's own operator. It is worth saying because the opposite mistake — a
ZSTD tile compared against a RAW one, and `quant == 1` misread as 1 mm rather
than 100 mm — once produced a "696 control points differ" claim that was really
zero, and it reached a PR.

## bv14 landed on these tiles, and it is the larger fix here

`tools/river_column_contact.py` over all six tiles, same tiles, both namespaces.
This is voxel 6-connectivity — whether the drawn water column touches the one
next to it — which is the thing the owner was actually looking at.

| | bv13 | bv14 |
|---|---|---|
| wet cells | 2,151,866 | 2,202,142 |
| isolated cells (touch nothing) | 67,377 = **3.13%** | 1,855 = **0.084%** |
| broken face pairs | 42,410 | **15** |
| pieces as drawn (face components) | 85,098 | **4,118** |
| pieces in plan (8-connected) | 1,165 | 1,165 |

Per tile the drawn-piece count falls 9,195 → 527, 21,208 → 430, 18,088 → 354,
16,637 → 1,037, 11,588 → 932, 8,382 → 838.

At the steep site below, walking 40 cells downstream from the spawn column —
30.8 m of descent in 75 m, 411 m/km — **bv13 drew water in 6 of those 40 cells
and bv14 draws all 40.**

## Client load, without the editor

`vxc_riverribbonprobe` over the six new tiles:

```
tiles: loaded=6 (with a water plane: 6) refused=0
unresolved blocks : 0
FAST FILL vs RiverSampler::surfaceAtPixel: 4,194,304 cells, disagreements: 0
```

Identical to the bv13 run (6/6, 0, 0, 0). Repeated at each of the three sites'
own 3.84 km regions: 0 unresolved and 0 disagreements at all three.

## The three sites

All three are inside `x -5..-3, y -5..-4`, i.e. world x −76,800 .. −30,720 m and
y −76,800 .. −46,080 m, and all three are at least 2.5 km inside that edge.
Every number below was read out of the bv14 `.vxtl` bytes at that exact column.

### 1. River — the valley trunk, tile (-3,-5)

```
-VoxelSpawnAt=-33582,-74092   -VoxelSpawnYaw=102
```

| | |
|---|---|
| terrain surface (river bed) | **222.7 m** |
| water surface | **224.1 m** |
| water depth here | **1.43 m** |
| the reach | 7,002 m across, falls 321 m, 46 m/km, median depth 1.32 m, max 3.90 m |
| drawn width, probe over this region | mean 11.54 m, centreline median 10.60 m, max 45.07 m |
| looking at yaw 102 | 169 m of the first 300 m ahead is water; 28.6 km of baked ground ahead |

This is a river and not a lake sheet: it is the river plane with **every**
registered basin's lake extent subtracted first (`basins.lake_extent_mask`, the
same rule `lakes.h` fills with). It is also the widest water in the block by a
long way — the bv13 note measured a 3.75 m median drawn width on the reach it
walked; the probe reads 10.60 m here.

Yaw 102 looks up-valley. Downstream (279°) the channel immediately bends out of
frame — only 30 m of the next 300 m is water that way — and it also runs at the
block edge, so up-valley is both wetter and safer.

### 2. Lake — alpine lake, tile (-3,-4), basin 38

```
-VoxelSpawnAt=-39661,-57292   -VoxelSpawnYaw=326
```

| | |
|---|---|
| spawn column | **dry bank at 1,367.5 m** |
| lake surface | **1,356.1 m** — 11.4 m below the bank |
| depth at the point you are looking at | **45.4 m** (median over the lake 15.6 m) |
| lake area | 0.274 km² |
| looking at yaw 326 | water starts 32 m ahead, 270 m of the first 300 m is water, 636 m of water across the sightline; deepest point 228 m out |

**The spawn is on the bank on purpose.** `-VoxelSpawnAltM` is measured from the
terrain surface, which under a lake is the lake BED — spawning on the water at
12 m would have put the camera 33 m under the surface.

The block makes real lakes and plenty of them: 156 registered basins on (-4,-5)
alone, 106 on (-3,-5). The largest is 0.573 km² at 245.7 m with 39.8 m of depth,
but it sits 800 m from the block edge, so the deeper one inland was chosen.

### 3. Steep descending reach — the cascade, tile (-4,-4)

```
-VoxelSpawnAt=-49608,-55126   -VoxelSpawnYaw=310
```

| | |
|---|---|
| terrain surface (bed) | **2,394.8 m** |
| water surface | **2,398.6 m** |
| water depth here | **3.80 m** |
| the reach | 2,466 m across, falls 852 m, **346 m/km**; median bed slope 0.686, i.e. 34° |
| the next 75 m downstream | falls 30.8 m — 411 m/km |
| bv13 vs bv14 over those 40 cells | water in **6** of 40 → water in **40** of 40 |
| looking at yaw 310 | downstream, down the slope; 128 m of the first 300 m ahead is water; 28.3 km of baked ground ahead |

`vxc_riverribbonprobe` independently finds this as the longest reach in its
3.84 km region: 1,592 m, midpoint (-49497, -55253), datum 2,341.7 m. This is
the site that shows what bv14 changed.

## The command line for the river site

```
-game -nosplash -sm6 -dx12 -windowed -ResX=2560 -ResY=1440 -WinX=0 -WinY=0
-VoxelSpawnAt=-33582,-74092 -VoxelSpawnAltM=12 -VoxelSpawnPitch=-12 -VoxelSpawnYaw=102
-VoxelTimeOfDay=12:00 -VoxelDate=03-20 -VoxelTimeScale=0
-VoxelTileDir=D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4\000000000135276f\s1
-VoxelFineTileDir=D:\vox-wet-cache -VoxelFineTileProviderId=terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c
-VoxelFineTileGateFatal=0 -abslog=D:\voxelsim\Saved\wet-bv14.log
```

The coarse and fine roots are deliberately different here and must not be
collapsed. The coarse root has all 289 world tiles; the fine root has only these
six. The six coarse tiles are byte-identical in both roots (checked), so the
split costs nothing.

Verified on disk before this line was written: all six `.vxtl` present under
`D:\vox-wet-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c\000000000135276f\s16\`.

For the other two sites, swap `-VoxelSpawnAt` and `-VoxelSpawnYaw` and give the
log a different name.

## Carried forward from the bv13 note, still true

1. **The drawn river does not follow its own width law.** Spearman of drawn
   width against distance downstream is +0.401 where the law's own width gives
   +0.728; at km 20 the law asks 18.2 m and the plane held 3.75 m. bv14 does not
   touch this — it fixes whether the water TOUCHES ITSELF, not how wide it is
   drawn. The river site above is genuinely wider water (10.60 m median at the
   centreline), but that is a better reach, not a fixed law.

2. **Do not call the 32.9 km composed reach a river.** 42.3% of it is lake
   sheet, and the lakes are deeper (p50 4.82 m, max 45.4 m) than the river is
   anywhere (centreline max 2.05 m). The three sites above are labelled as what
   they are for exactly this reason: the river site has every lake extent
   subtracted before it was chosen, and the lake site is presented as a lake.

## One warning from the bake, unresolved

```
tile (-3,-5) contains a flat/basin 1003 m across, wider than the 960 m apron.
Elevations still agree across the seam (the effect is sub-ULP, below the 100 mm
wire LSB) but its ROUTING may not -- see pipeline.APRON_BLIND_SPOT.
```

Same warning bv13 raised. The elevations agree; the routing inside that one
flat may not. None of the three sites is in it.

## Reproduce

```
python tools/bake_tiles_from_cache.py --seed 20260719 \
    --cache-dir D:/vox-wet-cache \
    --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4 \
    --tiles="-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5" --npz-dir D:/vox-wet-npz

python tools/verify_water_only_change.py \
    --tiles="-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5" --cache-dir D:/vox-wet-cache \
    --ns-before terrain-diffusion-unlabeled-80b9ca451a23eae4-ba9c62170 \
    --ns-after  terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c

python tools/river_column_contact.py --cache-dir D:/vox-wet-cache \
    --namespace terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c \
    --seed-hex 000000000135276f --tiles="-5,-4 -4,-4 -3,-4 -5,-5 -4,-5 -3,-5"

build/voxel-core-msvc/bench/Release/vxc_riverribbonprobe.exe \
    D:/vox-wet-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-b10cf6d2c/000000000135276f/s16
```
