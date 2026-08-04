# Water: waves A–C and the backlog behind them

Written 2026-08-04 after PRs #207–#211 landed. This is the plan being executed
autonomously. `docs/water-handover-2026-08-04.md` is the state of the world;
this is the order of work against it.

**The milestone**, in the owner's words: *realistic, natural looking rivers that
flow from origin source points out to the ocean or wherever they end. Lakes and
basins filled. Water placement determined by bake, filling terrain features cut
by erosion.*

**Where that stands.** On one four-tile corridor, composed the way the client
draws it, a river runs **20,269 m from 389.0 m to −476.0 m**, 5.0% lake sheet,
at a width that follows its own discharge (p90 7.50 m, max 15.91 m). 255 basins
fill, none dry. That is the milestone met **on one corridor of four tiles out of
289**. Wave C is what makes it true of the world.

---

## Wave A — in flight

| item | deliverable | state |
|---|---|---|
| A1 river ribbon actor | rivers visible beyond the 26 m near-field box | running, holds the editor |
| A2 hydrostatic cap (#60) | end the silent failure; correct fill above 65,536 cells | running |
| A3 river longitudinal profile | depth, width, growth/contraction, descent — the owner asked directly | running |

**Per-item exit:** sync onto `main`, push, PR, CI green, merge. A red CI is
fixed, never merged around — two failures today were invisible locally
(`bankprobe.cpp` on g++/MSVC, `test_basins.py` without numba).

---

## Wave B — correctness and truth, after A

### B1. Correct the handover's seam mechanism — **do this first, it is cheap and it is wrong today**

The doc says the downstream tile was *starved*. It was not. The same
**8.6e7 m³/yr crossed either way** (D8/MFD = 0.99 on the total). The water was
**divided**: MFD's largest single crossing held 8.66e6 against D8's 2.02e7, and
the consumer is a hard threshold that a fan cannot clear. A wrong mechanism in a
handover is worse than a missing one.

Also fold in: the cap finding (A2's precondition), and that a river now reaches
the sea.

### B2. The seam residual — position, not magnitude

10 of 35 multi-km reaches still end at an interior seam. D8 fixed the magnitude
at the crossing; what remains is that **the coarse trunk sits 165 m from the
fine trunk**, and on flat near-coast plain (194.5–199.8 m over 375 m, no incised
channel) the injection lands on the wrong side of a divide a few metres high and
the water ends up 1.3 km east. A 960 m apron cannot heal what has no valley to
fall into.

**Blocked, and the block is the interesting part:** placing the entry cell by
the fine thalweg is prevented because `_edge_entries` shares crossings with the
**area** currency, so moving the entry moves the ground. Scope whether the two
currencies can be separated without touching `TERRAIN_VERSION`. If they cannot,
say so and stop — this is not worth a terrain re-key.

### B3. Phase 5 — the two-renderer tone problem

The sea is a 40 km plane and is deliberately not meshed; lakes and river reaches
**are** meshed. Every boundary between them is a shell meeting a surface.
Measured at the lake seam: **+44.5 vs +76.8** blueness. Measured at a breach
against the control's own plane: where the shell draws, the difference runs
**−14.1 to +9.0, mean −1.3 — not a constant offset, and the sign flips with
viewing angle.** A fixed tint cannot reconcile them.

Still unmeasured: the **river/sea** join, which could not be taken because that
cache had no water plane. A1 makes it measurable. Needs the editor, so it queues
behind A1.

---

## Wave C — make it true of the world, not of one corridor

### C0. First: get the shipped cache to the current bake version — ~~the real gap~~ **DONE 2026-08-04**

The gap was real: there was no bv11 or bv12 cache on disk, so #209's
seam-crossing and width work had never been baked into anything the client
could load, and every capture to that date was against the *narrowest* river
the project will ship.

**Closed.** The four corridor tiles are baked at bv12 and are loadable:

```
D:\voxelsim\tile-cache\terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb
                       \000000000135276f\s16\{-11_-4,-11_-5,-12_-5,-11_-6}.vxtl
```

172 MB of tiles (CODEC_ZSTD) + 322 MB of flow pyramid = 492 MB. **Point captures
here.** The namespace is the one `fine_id_for()` derives from the bake
fingerprint, so the version bump landed in its own directory: the bv10 water
cache (`-b4d02b092`) and bv9 (`-b196f6020`) are untouched and remain the
comparison baselines. Note bv10 is CODEC_RAW at ~210 MB/tile; bv12 is
CODEC_ZSTD at ~45 MB, which is a codec choice, not a content difference.

**Water, against bv10 on the same four tiles: 27,347 → 317,665 wet cells
(11.6x).** Per tile 48,911 / 103,476 / 42,056 / 123,222 (0.073% / 0.154% /
0.063% / 0.184% wet). 255 basins — 246 overflowing, 9 terminal, zero dry playa,
salt flat or seasonal. Ribbon width p90 **7.50 m**, max **15.91 m**.

**Composed the way the client draws it** (river plane ∪ the 255 lake extents,
labelled once), reproduced off the shipped tiles: **2,679,902 composed cells**
in 258 components; longest **20,269 m from 389.0 m to −476.0 m at 5.0% lake
sheet**; then 17,909 m from 269.4 m; the 1,540 m head's 13,030 m component is
**94.4% lake sheet** and still must not be quoted as a river. Lake area
8.305 km². Every figure in §"Where that stands" above is confirmed.

**One number in the record is off by five.** Composed cells measure
**2,679,902**, not 2,679,897 — the river and lake sets are disjoint here and
317,665 + 2,362,237 sums to 2,679,902 exactly. 0.0002%, no bearing on anything,
but the record said 2,679,897 and it is 2,679,902. The along-channel length
(30,577 m) and sinuosity (1.51) were **not** re-checked: no shipped tool
computes them, so they came from an ad-hoc script that is not in the repo.
The composition itself no longer has that problem —
`tools/corridor_composed_reach.py` is the instrument, reusing
`measure_corridor_fragmentation`'s labeller and `bake.basins.lake_extent_mask`
rather than re-deriving either. C3 should quote it, not a fresh script.

**Terrain did not move.** `elevation_control_points` and the flow plane are
byte-identical to bv10 on all four tiles, on the same datum, through the
codec's own operator. `TERRAIN_VERSION` stays 8.

**The client loads them**, proven through the shipping C++ decode rather than
inferred from the files: `vxc_riverribbonprobe` reports `loaded=4 (with a water
plane: 4) refused=0`, 0 unresolved blocks, and 0 disagreements between the
far-field fast fill and `RiverSampler::surfaceAtPixel`; `vxc_burialprobe`
reports `blockDecodeFailures=0` over all 317,665 wet pixels. Both bind the
game's own `libzstd.dll`.

**The npz shortcut does not work — bake, don't encode.** Three of the four
products `_encode_fine` needs are in the `--npz-dir` dump (elevation, flow,
water plane); the **B5 basin registry is not**, and it cannot be recovered from
the dump because `survey_basins` consumes padded pre-reopen state
(`basin_depth`, padded `z`/`acc`/climate) that the dump does not carry.
Encoding from npz would ship `basins=None`, which clears
`FLAG_BASINS_PRESENT` — and per `TileV2`'s own contract that reads as "predates
the registry", not "surveyed, holds nothing". So a re-bake it is: **~300 CPU-s
per tile, 1,180 CPU-s for the corridor.** The re-bake was verified
*bit-identical* to the diagnostic bv12 npz on all five arrays, so every
measurement taken on `D:/tmp/seamwidth-npz` describes exactly these shipped
tiles.

### C1. A modest region, and only once the version is settled

Everything claimed about rivers reaching the sea rests on **four tiles**. The
temptation is to bake the world; the reason not to, right now:

* the bake version is still moving (bv10 → 11 → 12 within one session), and
  tiles baked at a version we then change are wasted compute;
* the binding constraint on *seeing* a river is rendering, not coverage — at
  20 km a river needs ~15.9 m of width to cover one pixel and **46% of it is
  buried at that width**, so more tiles change nothing about visibility;
* ~300 CPU-s × 289 tiles is ~24 CPU-hours, which is not a background task.

So: a **contiguous block of 16–25 tiles** containing one complete drainage from
mountain head to coast, baked **after** the version settles. Enough to expose
seam behaviour across many more crossings than four tiles can, without spending
a day on compute that a version bump would invalidate.

Selection rule, learned the hard way: **do not pick the corridor on runoff being
high the whole way.** That is exactly when the old proxy was already right, and
it made the carried-discharge test worthless. Pick a system that crosses a
climate gradient, since that is the case that used to vanish entirely.

**World-scale bake is explicitly NOT scheduled.** It needs a reason beyond
completeness, and the reason would have to be something only world coverage can
answer.

### C2. Re-run the water overlay against the new region

`08-water-streams-rivers-lakes.png` currently draws channels from the **289
coarse tiles** and lakes from the fine bake over **256 of 289 tiles**. With more
fine tiles baked, the lake half improves and the two halves come closer to
agreeing. The tool already exists (`tools/worldmaps/water.py`) and already reads
its constants from `bake/water.py` — do not fork it.

### C3. World-scale validation

Per-region: component count, longest reach, reaches ≥ 2 km, pieces meeting the
coast, width distribution, basin fill. Same instruments as the corridor
(`measure_corridor_fragmentation.py`, `corridor_coast_reach.py`) so the numbers
are comparable rather than merely similar.

---

## Backlog, in support of water

Ordered by how much they block the milestone.

1. **#55 — a breach that needs water to RISE stops at the puncture depth.** The
   mobilized sea is at equilibrium and therefore inactive, and Phase C explores
   dry headroom only through active bricks. Not a budget problem: 64× the front
   budget changed nothing. Likely entangled with #60; check before building.
2. **`ELEV_DATA` digest churn.** A water-only re-bake leaves the elevation plane
   bit-identical but changes the tile's identity — 696 of 603,979,776 control
   points, 1 mm each — because no flow superblock is retained under a fine
   namespace. Physically nothing moves; on the wire every client re-downloads.
   This gets worse with every Wave C tile.
3. **#52 — slicing.** Already scoped: blocks are individually framed, the index
   carries `(offset, comp_len, …)`, ZSTD median block 34,008 B against a 32–56 MB
   tile. The only open unknown is whether the transport supports byte ranges.
   Directly reduces the cost of #2 above.
4. **#17 — client must complement the bake, not add to it.** Reopened; the
   specific water justification was measured false (amplifier adds p50 **+3 mm**
   in-channel). Needs its own measurement before promotion, not the old one.
5. **#57 far-field** closes when A1 lands; **#47/#48** (fine-tier drainage
   stranding, amplifier re-stranding) are terrain-side and touch the same ground
   rivers run on — worth re-measuring once Wave C has more baked tiles.

---

## Rules being followed while unattended

* **Never merge a red CI.** Fix it. Local green has been worthless twice.
* **Never touch `TERRAIN_VERSION`.** Every change here is water-only, and that
  is verified by comparing elevation control points and the flow plane
  byte-for-byte through the codec's own `elevation_control_points` — a
  hand-rolled quantiser missing the §2 prefilter already produced one false
  "terrain moved" alarm.
* **One UE editor.** Only one agent holds it at a time.
* **No appearance verdicts.** Captures are delivered with conditions, settle
  confirmation (`jobsInFlight=0 pendingJobs=0 unloaded=0` and
  `RefreshImplicitWater: DRAINED`) and a pixel-diffed control. Two controls were
  worthless today from different causes.
* **Report negatives first.** Six confident explanations about this river have
  already been wrong and two nearly became code.
* **Owner decisions stay parked**, with the measurement attached, not guessed.
