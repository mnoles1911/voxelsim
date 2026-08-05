# The bv12 river, captured

Written 2026-08-04. Thirteen `-on`/`-off`/`-diff` triples of the 30.6 km reach
walked in `docs/measurements/river-long-profile-2026-08-04.txt`, taken against
the **bake_ver 12** corridor — the first captures of this river that are not
against bv10.

**No claim is made here about how any frame looks.** Conditions, poses, settle
evidence and numbers only. Every frame is listed with the measured width and
depth at its frame centre so it can be checked against a number rather than an
impression.

---

## 1. The cache, verified from the bytes

Provider `terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb`, seed
`000000000135276f`, four fine tiles: (-11,-4) (-11,-5) (-12,-5) (-11,-6).

`bake_ver` was read out of the tile header (offset 29, `u16` — the field
`tilestore.cpp:544` parses) rather than inferred from a directory date, because
the newest fine directory by mtime is **not** the right one:

| namespace | bake_ver | flags | water plane | codec |
|---|---|---|---|---|
| `-b52995abb` (used) | **12** | 7 | **yes** | ZSTD |
| `-bd3d0ddc7` (newest by mtime) | 8 | 3 | **NO** | ZSTD |
| `-b4d02b092` (the bv10 baseline) | 10 | 7 | yes | raw |

`-bd3d0ddc7` has 38 tiles against `-b52995abb`'s 4 and is newer on disk, and it
carries no water at all. A capture taken there would have shown dry valleys and
nothing in the frame or the filename would have said so.

**On the size.** The four tiles are 179.4 MB on disk; the 492 MB figure carried
in the brief is the *decoded* footprint — the fine tier logs `decoded 0.12 GiB`
per tile, four of which is ~0.48 GiB. Both numbers are right about different
things.

## 2. Sites verified BEFORE shooting

Every pose was checked with `vxc_riverribbonprobe` over these tiles first. All
five site regions returned `loaded=4 (with a water plane: 4) refused=0`,
**0 unresolved water blocks**, and **0 disagreements** against
`RiverSampler::surfaceAtPixel`. Centreline width rises downstream exactly as the
long profile says it should: p50 1.87 m at the head, 5.30 m at the knickpoint,
5.62 m on the lower trunk, max 10.60 m.

Targets are nodes on the walked thalweg, converted to world metres as
`(x0*8192 + col_px + 0.5) * 1.875` with `x0,y0 = (-12,-6)` from
`composed-bv12.npz`. Every camera landed **0–1 m off the channel**.

**The actor was then corroborated against voxel-core independently.** At the
ladder column with a window matching the actor's own 4267 x 4267 px wet mask:

| | voxel-core probe | the UE actor | delta |
|---|---|---|---|
| wet pixels | 45,437 | 45,462 | 0.05% |
| centreline pixels | 21,711 | 21,729 | 0.08% |
| reaches | 58 | 58 | exact |
| unresolved water blocks | 0 | 0 | — |

The residual is my probe origin being rounded to the pixel while the actor
centres its window on the exact camera position. The ribbon in these frames is
the bake's own river.

---

## 3. THE SETTLE RULE IS ALTITUDE-DEPENDENT

This corrects a rule that has been carried in several briefs, and it matters
enough to state before the inventory.

`RefreshImplicitWater`'s disc is **52 x 52 x 26 m centred on the camera**
(`VoxelWaterSubsystem.cpp:1317`). Above roughly 26 m there is no implicit water
inside it, so `RefreshImplicitWater: DRAINED` **cannot** appear — and
`voxel-capture.ps1` warns about its absence anyway. Requiring that line on an
altitude capture demands something structurally impossible.

    near-field (<= ~26 m)  require  RefreshImplicitWater: DRAINED
    altitude               require  River ribbons: DRAINED build
                                    and read its reach/quad counts

Both are recorded for every frame below. Reading a structurally-absent line as a
failed settle costs a needless re-shoot; the worse failure is the other
direction — treating the warning as background noise at 10 m, where its absence
means the near field genuinely had not drained.

The set demonstrates the rule rather than asserting it. Across the 26 delivered
captures, `RefreshImplicitWater: DRAINED` appears on **exactly the two 10 m
captures — both the `-on` arm and its control — and on none of the other 24.**
That it fires on the control too is the point: the near field is not the
variable under test, so it must engage identically in both arms, and it does.

Terrain settle (`jobsInFlight=0 pendingJobs=0 unloaded=0`) held on all 26, and
all 13 pairs have byte-identical `Capture: cam loc=` between their arms.

---

## 4. The altitude ladder — one column, so the counts compare

All five rungs sit at `-160632,-85613` (chain km 20.16) with yaw -6.8, so the
ribbon's 4 km scan radius gathers the **same reach set** at every altitude. That
is the only arrangement in which reach and quad counts mean anything across a
ladder, and it is what makes the near/far handover legible.

| file | alt | pitch | centre km | reaches | quads | sub-px | implicit | diff ratio |
|---|---|---|---|---|---|---|---|---|
| `bv12-ladder-10m-*` | 10 m | -20° | 20.19 | 58 | **2013** | 2 | **DRAINED x1** | 0.026 |
| `bv12-ladder-200m-*` | 200 m | -25° | 20.72 | 58 | 2012 | 2 | x0 (expected) | 0.019 |
| `bv12-ladder-1km-*` | 1 km | -35° | 22.00 | 58 | 2012 | 2 | x0 (expected) | 0.003 |
| `bv12-ladder-5km-*` | 5 km | -55° | 24.65 | 58 | 2012 | 12 | x0 (expected) | 0.002 |
| `bv12-ladder-20km-*` | 20 km | -75° | 27.89 | 58 | 2012 | 58 | x0 (expected) | 0.008 |

**The handover clip is exact.** 2013 quads at 10 m against 2012 at every
altitude, from the identical 58 reaches: one segment split in two by the implicit
disc boundary, which is exactly and only what a segment crossing the edge of the
hole should produce. This reproduces the bv10 finding (102 against 101) at bv12's
20x scale.

**200 m is the lowest rung where the ribbon carries the river alone** — the
camera is above the 26 m disc, so nothing of the near field is in the frame.

**The sub-pixel count is the known limit, as a number**: 2 reaches below one
pixel at 10 m and 200 m, 12 at 5 km, 58 of 58 at 20 km. Geometry is bit-stable
across the whole ladder; only the projection changes. 20 km is recorded but is
**not** the standard ladder — that is 10 m / 200 m / 1 km / 5 km.

---

## 5. The named features

All at yaw set so the camera looks **downstream along the channel**, which puts
kilometres of river in frame rather than one crossing. Pitch -25° at 200 m,
-35° at 1 km.

| file | alt | centre km | what | surf m | depth m | drawn width | Q m³/yr | reaches/quads | ratio |
|---|---|---|---|---|---|---|---|---|---|
| `bv12-head-1km-*` | 1 km | 1.75 | steep head, no widening | 332.28 | 0.502 | **3.75 m** | 3.10e6 | 39 / 3059 | 0.012 |
| `bv12-seam-1km-*` | 1 km | 5.00 | **the seam pinch** | 265.33 | 0.515 | 7.50 m | 3.33e6 | 76 / 3179 | 0.017 |
| `bv12-knick-1km-*` | 1 km | 19.60 | knickpoint | 95.32 | 0.596 | 7.50 m | 5.05e6 | 65 / 1906 | 0.002 |
| `bv12-trunkmid-200m-*` | 200 m | 22.00 | lower trunk | 45.33 | 0.783 | 7.50 m | 1.10e7 | 59 / 2148 | 0.034 |
| `bv12-lake-200m-*` | 200 m | 23.00 | **coastal lake** | 10.82 | see below | composed 15.00 m at centre | — | 51 / 1858 | 0.005 |
| `bv12-shore-200m-*` | 200 m | 24.00 | shoreline | 0.79 | 0.788 | 7.50 m | 1.11e7 | 41 / 1819 | 0.000 |
| `bv12-mouth-1km-*` | 1 km | 24.30 | below the shore | 0.77 | 0.788 | 7.50 m | 1.11e7 | 50 / 1844 | 0.045 |
| `bv12-trunkwide-1km-*` | 1 km | 24.60 | **widest river** | 0.76 | 0.788 | **8.44 m** | 1.11e7 | 46 / 1758 | 0.198 |

### The seam pinch, with the numbers either side

The frame is centred at km 5.00 and the defect sits **160 m beyond frame
centre**, at world **(-164405, -76801)** — on the tile (-11,-5)/(-11,-6)
boundary, which lies at y = -76800 exactly.

It is the **only step in all 13,896 thalweg cells where Q falls between two
river cells**:

| | drawn width p50 | p90 | Q p50 | depth p50 |
|---|---|---|---|---|
| upstream, km 3.7–5.22 | **7.50 m** | 7.50 m | 3.29e6 | 0.515 m |
| downstream, km 5.22–6.7 | **3.75 m** | 3.75 m | 3.00e6 | 0.500 m |

Q falls 3.3537e6 -> 2.8647e6 across one cell, **-14.6%**, and the drawn ribbon
halves and stays halved for 1.5 km. This is captured deliberately rather than
avoided; it is the residual mechanism PR #209 did not fix (it fixed the
magnitude crossing a seam, not the position).

### The coastal lake

The crossing is only **134 m of channel** (58 thalweg nodes, km 23.001–23.135)
and is the reason the composed network reaches the sea. Within it the drawn
water reaches **120.00 m** across unguarded / **93.75 m** guarded (four-axis
90.16 m — the two measures agree, so it is a real sheet and not a bend
artefact), and **3.429 m deep**, the deepest water anywhere on the reach. The
frame centre at km 23.00 is the lake's upstream entry; the whole sheet is within
135 m of it and the camera is 429 m back.

---

## 6. Controls

Every `-off` is `-VoxelRiverRibbons=0` at the **byte-identical shutter pose**
(verified: `Capture: cam loc=` matches its pair exactly in all 13), and every one
logs `River ribbons: DISABLED (-VoxelRiverRibbons=0)` with zero
`DRAINED build` lines — so a suppressed-by-switch control is distinguishable
from a suppressed-by-absence one from the log alone.

Judged by the **corrected** test: the ratio of signed to absolute background
mean, not `mean |delta|`. An exposure/tonemap shift is a *bias* (large signed
mean); TAA and stochastic reflection off translucent water are *symmetric*
(signed mean ~0 against a large absolute mean). The tool warns at ratio > 0.30
with |mean| > 0.5.

**All 13 controls are sound**, ratio 0.000 to 0.198.

Two are worth naming rather than burying:

* `trunkwide-1km` at **0.198** is the highest by 4x. Its signed mean is -0.111
  against |mean| 0.561. It stays well under the 0.30 threshold and its
  difference **contracts** properly (8.21% of frame at threshold 8 -> 0.07% at
  96 -> 0 at 128; bbox 100% -> 24%), so it is a localised feature and usable.
  It is the frame to re-shoot first if the owner questions any of them.
* `mouth-1km` at **0.045**. Both are the most ocean-filled frames in the set,
  which is the expected noise source.

`ladder-10m` at ratio 0.026 with |mean| 2.11 is the near-field pair, and it
reproduces the bv10 near-field pair (0.026 / 2.129) almost exactly — the pair
whose |mean| once got a sound control wrongly condemned.

---

## 7. One site could NOT be shot honestly at 200 m, and was discarded

`trunkwide-200m` — km 24.6, the widest river cross-section — was taken and then
**thrown away**. Its log carries:

    FINE TIER GATE LEAK: elevation query at fine pixel (-81920,-46423) landed in
    NON-RESIDENT tile (-10,-6) ... and is being answered with SEA LEVEL.
    ... gateLeaks=2898 absentOnDisk=1

That is fabricated terrain in the frame, not a graceful fallback. The cause is
geometric and worth recording: km 24.6 lies only **3,644 m** from the eastern
edge of baked coverage (x = -153600), and at 200 m the camera must sit just
429 m back, which puts the streaming ring across the boundary. The site was
re-shot at **1 km**, where the camera sits 1,428 m back at x = -158581, and came
back clean.

The empirical threshold from this set: a camera **≥ ~4.5 km** from the edge of
coverage was clean; the one at 4,073 m was not.

**Every other capture in the set — all 26 — reports `gateLeaks=0`,
`absentOnDisk=0`, `identityMismatch=0`, `corrupt=0` and 0 refused tiles.** The
terrain in all of them is real baked terrain.

---

## 8. Conditions common to every frame

* 2560x1440, sun **frozen** 12:00 on 03-20 (`-VoxelTimeScale=0`), the archive's
  reproducible pose.
* `-SettleSec 300`; all 26 reached `jobsInFlight=0 pendingJobs=0 unloaded=0`.
* Provider overrides on the command line, verified in each log's own
  `Fine tier ENABLED` / `Baked water tier ENABLED` lines rather than assumed
  from the switches.
* zstd bound at runtime from `ue-project/Binaries/ThirdParty/zstd/Win64/`; every
  log records the bind and 0 tiles refused for `kNoDecompressor`.
* `-VoxelFineTileGateFatal=0` as a precaution for the four-tile coverage. It was
  needed exactly once — the discarded frame in §7 — and the count is reported
  for every frame either way.
* Persisted world state cleared before each run (`cleared persisted world state:
  20260719.vxlog (31 B)`); no `.vxwater` blob ever accumulated, so the
  886,179,570-fill-unit inheritance bug did not recur.
* `-VoxelRiverRibbonMinPx` left at its default **0**. Widening is dead and the
  measurement that killed it is in `docs/river-farfield-actor-2026-08-04.md` §2.

## 9. Reproducing

    tools\bv12-shoot-one.ps1 -Name <site> -At '<x,y>' -Alt <m> -Pitch <deg> -Yaw <deg> -Arm on|off
    python tools\capture-pixdiff.py <on>.png <off>.png --out <diff>.png

`tools\bv12-river-captures.ps1` drives the whole set; `bv12-shoot-one.ps1`
exists because a backgrounded driver was killed mid-run and left a pose
half-shot, and a capture set that cannot be resumed frame-by-frame has to be
restarted whole.
