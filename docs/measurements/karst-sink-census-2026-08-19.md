# Does the world supply enough sinks for a karst network? — PASS, by 4.5×

**Date:** 2026-08-19 · **Tool:** `terrain-service/tools/karst_sink_census.py` (new)
**Data:** `karst-sink-census-2026-08-19.txt`, `.json`
**Sample:** all 15 baked fine tiles, bake_ver 28, seed 20260719 — 3,538 km² of real world.

## The question

The karst plan routes conduits by Dijkstra **from sinks to springs**, and prices the work on ~400
independent systems per flow superblock (16 fine tiles, 3,775 km²). If the world supplies 20, the
per-system corridor sampling has nothing to sample and the approach needs rethinking at the
geology layer, not the tuning layer. That is checkable against tiles already on disk, so it was
checked before anything was built.

## Result

| | per tile | per superblock | vs. plan |
|---|---|---|---|
| registered basins, all kinds | 638.7 | 10,219 | — |
| **terminal basins (ponors)** | **113.3** | **1,813** | **4.5× the assumed 400** |
| baked headwaters | 30,723 | 491,571 | — |

Terminal kinds are `dry_playa` (45), `salt_flat` (0), `seasonal` (1,112) and `lake_terminal`
(543) — water that arrives and does not leave by the surface. `lake_overflowing` (7,880) is
excluded by name: it is a through-flow feature, not a sink.

**PASS.** The world supplies more than four times the assumed system count from **registered
basins alone**, before a single doline is counted — and dolines are the larger population by far.

## The stream network, and a number that had to be thrown away

Drainage density, by catchment-area threshold:

| threshold | network | density |
|---|---|---|
| acc ≥ 2¹⁴ (16,384 m²) | 56,783 km | 16.05 km/km² |
| acc ≥ 2¹⁶ (65,536 m²) | 27,089 km | 7.65 km/km² |
| **acc ≥ 2¹⁸ (262,144 m²)** | 11,871 km | **3.35 km/km²** |
| **acc ≥ 2²⁰ (1,048,576 m²)** | 4,485 km | **1.27 km/km²** |

The last two sit inside the real-world range of 0.5–5 km/km², which is the sanity check the
threshold ladder exists to provide.

**The first version of this tool used the flow plane's `FLOW_BIT_CHANNEL` and reported a drainage
density of 278 km/km².** That bit is not a watercourse mask. `pack_flow_plane` sets it as
`(a_eff >= channel_area_m2) | (incision_m >= channel_depth_m)` with `channel_depth_m = 0.25`, so
on real terrain the incision term dominates and the flag lands on **80% of a tile**. It was caught
by physical implausibility — 278 against a real-world 0.5–5 — not by reading the code first. The
tool now derives the network from accumulation and prints the threshold beside every number that
depends on it, because the number moves with it.

## Limits of this measurement — read before quoting it

* **These are registered basins only, and that is a strict LOWER bound.** The shipped elevation
  plane is depression-filled (`B2a`, with `B5` re-opening only registered lake holes), so a fine
  tile does not carry the world's closed depressions. `basin_depth` is a live intermediate through
  `B7` (`pipeline.py:4829`, used at `:5545`) and is never emitted. **The doline population can
  only be counted by a stage running inside the bake** — which is an independent argument for
  putting the karst stage there, alongside the pathfinding one.
* 230 of the 9,580 basin rows are tile-spanning, i.e. one physical lake seen twice. Not corrected
  for; it is 2.4% and it inflates the pass.
* The headwater count (130/km²) is dense for real headwater spacing. It is the bake's own table
  and is reported as-is; the karst plan uses it as a **cross-check** on springs, not as a sink
  source, so its absolute density does not enter the verdict.
* Both estimates are printed side by side and never added. They measure different things.

## What this changes in the plan

Nothing needs rethinking. The premise survives with margin, and the sink supply is not the
constraint. The next open question is the doline count, which needs the in-bake stage.
