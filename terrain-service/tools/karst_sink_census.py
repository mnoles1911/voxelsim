"""karst_sink_census.py -- DOES THE WORLD HAVE ENOUGH SINKS TO CARRY A KARST NETWORK?

WHY THIS EXISTS, AND WHY IT RUNS BEFORE ANYTHING IS BUILT.

The karst plan (docs/karst-*.md, and the session plan) routes conduits by
Dijkstra from SINKS to SPRINGS, and its cost model assumes the world supplies
roughly 400 independent systems per flow superblock (4x4 coarse tiles). If the
real world supplies 20, then "caves everywhere" is false before a line of the
generator is written, the per-system corridor sampling has nothing to sample,
and the whole approach needs rethinking at the geology layer rather than at the
tuning layer.

That premise is checkable TODAY, against tiles already on disk, in about a
minute. This file is that check. It is deliberately the first thing built.

WHAT IT CAN AND CANNOT SEE -- read this before quoting a number.

The shipped elevation plane is DEPRESSION-FILLED (bake stage B2a, and B5 only
re-opens registered lake holes), so a fine tile does NOT carry the world's
closed depressions. `basin_depth` is a live intermediate inside the bake and is
never emitted. Therefore:

  * The basin counts here are REGISTERED basins only -- lakes, playas, salt
    flats, seasonal pans. That is a strict LOWER BOUND on karst sinks. The
    doline population, which is much larger, can only be counted by a stage
    running inside the bake with `basin_depth` in hand.
  * The stream network is derived from FLOW ACCUMULATION, not from the flow
    plane's channel BIT. That bit is not what its name suggests and using it
    was this tool's first result, discarded: `pack_flow_plane` sets it as
    `(a_eff >= channel_area_m2) | (incision_m >= channel_depth_m)` with
    `channel_depth_m = 0.25`, so on real terrain the incision term dominates
    and the flag lands on **80% of a tile**. Read as a watercourse mask it
    yields a drainage density of 278 km/km^2 against a real-world 0.5-5, which
    is how it was caught. Accumulation, thresholded at a stated area, is the
    honest network; the threshold is printed with every number that depends on
    it because the number moves with it.

So this tool answers "is the premise obviously dead", not "is the premise
right". A pass here is permission to keep going; it is not evidence of density.
Both bounds are printed, labelled, and never added together.

THE ENGINE'S OWN DECODER, NOT A SECOND ONE. Every number comes through
`terrain_service.tile_codec.decode_v2` and the `BasinEntry` / `HeadEntry`
records it returns -- the same path the bake writes and voxelcore reads. This
project has lost four separate nights to a probe that measured a world the
engine was not running; see `voxelsim-instrument-must-run-the-engine-binding`.

Usage:
    python tools/karst_sink_census.py <tile-dir> [--json out.json]
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import numpy as np  # noqa: E402

from terrain_service import tile_codec as tc  # noqa: E402

# --- geometry, from the format itself rather than retyped ------------------
FINE_PX_M = 1.875              # metres per fine pixel, tile_codec FINE_SCALE
TILE_PX = 8192                 # fine pixels per tile edge
TILE_KM = FINE_PX_M * TILE_PX / 1000.0          # 15.36 km
TILE_KM2 = TILE_KM * TILE_KM                    # 235.9 km^2
#: A flow superblock is 4x4 coarse tiles (pipeline.py FlowLevel), and one
#: coarse tile is one fine tile's footprint, so 16 fine tiles per superblock.
TILES_PER_SUPERBLOCK = 16

#: What the plan assumes it will get, per superblock. Printed alongside the
#: measurement so the verdict is a comparison and not an impression.
PLAN_SYSTEMS_PER_SUPERBLOCK = 400

#: Catchment-area thresholds, as log2(m^2), at which to call a cell part of the
#: stream network. 16 is ~65,536 m^2 and sits just under the bake's own
#: `channel_area_m2` of 1e5; the ladder is printed because "drainage density"
#: is meaningless without the threshold that produced it, and because a karst
#: sink is a stream that ENDS, so which streams you count decides the answer.
ACC_THRESHOLDS_LOG2 = (14, 16, 18, 20)

KIND_NAMES = {
    tc.BASIN_KIND_DRY_PLAYA: "dry_playa",
    tc.BASIN_KIND_SALT_FLAT: "salt_flat",
    tc.BASIN_KIND_SEASONAL: "seasonal",
    tc.BASIN_KIND_LAKE_TERMINAL: "lake_terminal",
    tc.BASIN_KIND_LAKE_OVERFLOWING: "lake_overflowing",
}

#: Kinds that are TERMINAL -- water arrives and does not leave by the surface.
#: In karst these are ponors: the water goes down. An overflowing lake is not a
#: sink, it is a through-flow feature, so it is excluded by name rather than by
#: an inequality on the enum value.
TERMINAL_KINDS = frozenset({
    tc.BASIN_KIND_DRY_PLAYA,
    tc.BASIN_KIND_SALT_FLAT,
    tc.BASIN_KIND_SEASONAL,
    tc.BASIN_KIND_LAKE_TERMINAL,
})


def census_tile(path: pathlib.Path) -> dict:
    """Decode one tile and count what bears on sink supply."""
    tile = tc.decode_v2(path.read_bytes())

    row = {
        "tile": path.stem,
        "bake_ver": tile.bake_ver,
        "basins_total": 0,
        "basins_terminal": 0,
        "by_kind": collections.Counter(),
        "heads": 0,
        "net_px": {},
        "net_km": {},
        "spanning": 0,
    }

    if tile.basins is not None:
        row["basins_total"] = len(tile.basins)
        for b in tile.basins:
            row["by_kind"][KIND_NAMES.get(b.kind, f"kind{b.kind}")] += 1
            if b.kind in TERMINAL_KINDS:
                row["basins_terminal"] += 1
            # A basin whose extent leaves the tile is one physical lake seen
            # twice; counted so the aggregate can say how much double-counting
            # is in it rather than silently carrying it.
            if getattr(b, "span_flags", 0) & tc.BASIN_SPAN_CROSSES_TILE:
                row["spanning"] += 1

    if tile.heads is not None:
        row["heads"] = len(tile.heads)

    if tile.flow is not None:
        # bits 0-4 are log2(flow accumulation in m^2), clamped 0-31
        # (tile_codec FLOW_LOG2_MASK, pipeline.pack_flow_plane).
        log2acc = (tile.flow & tc.FLOW_LOG2_MASK).astype(np.int32)
        for thr in ACC_THRESHOLDS_LOG2:
            n = int(np.count_nonzero(log2acc >= thr))
            row["net_px"][thr] = n
            # A one-pixel-wide network: length is the honest reading, area is
            # not.
            row["net_km"][thr] = n * FINE_PX_M / 1000.0

    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tile_dir", type=pathlib.Path)
    ap.add_argument("--json", type=pathlib.Path, default=None)
    args = ap.parse_args()

    paths = sorted(args.tile_dir.glob("*.vxtl"))
    if not paths:
        print(f"no .vxtl under {args.tile_dir}", file=sys.stderr)
        return 2

    rows, failed = [], []
    for p in paths:
        try:
            rows.append(census_tile(p))
        except Exception as exc:                      # noqa: BLE001
            failed.append((p.name, str(exc)))

    if not rows:
        print("every tile failed to decode:", failed, file=sys.stderr)
        return 2

    n = len(rows)
    tot_basin = sum(r["basins_total"] for r in rows)
    tot_term = sum(r["basins_terminal"] for r in rows)
    tot_heads = sum(r["heads"] for r in rows)
    net_km = {t: sum(r["net_km"].get(t, 0.0) for r in rows) for t in ACC_THRESHOLDS_LOG2}
    tot_span = sum(r["spanning"] for r in rows)
    kinds = collections.Counter()
    for r in rows:
        kinds.update(r["by_kind"])

    bakes = sorted({r["bake_ver"] for r in rows})

    print(f"KARST SINK CENSUS -- {n} fine tiles, bake_ver {bakes}")
    print(f"  tile = {TILE_KM:.2f} km square = {TILE_KM2:.1f} km^2; "
          f"superblock = {TILES_PER_SUPERBLOCK} tiles = {TILE_KM2 * TILES_PER_SUPERBLOCK:.0f} km^2")
    if failed:
        print(f"  !! {len(failed)} tile(s) failed to decode: {failed[:3]}")
    print()

    print("REGISTERED BASINS (a LOWER bound on sinks -- dolines are not in the tile)")
    print(f"  total                 {tot_basin:>8}   ({tot_basin / n:8.1f} per tile)")
    print(f"  terminal (ponors)     {tot_term:>8}   ({tot_term / n:8.1f} per tile)")
    print(f"  tile-spanning rows    {tot_span:>8}   (double-counted physical lakes)")
    for name, c in kinds.most_common():
        print(f"    {name:<20}{c:>8}   ({c / n:8.1f} per tile)")
    print()

    print("STREAM NETWORK, by catchment-area threshold (NOT the flow plane's")
    print("channel bit -- see the header for why that bit is not a river mask)")
    for t in ACC_THRESHOLDS_LOG2:
        dens = net_km[t] / (n * TILE_KM2)
        note = "  <- plausible" if 0.5 <= dens <= 5.0 else ""
        print(f"  acc >= 2^{t:<2} ({2 ** t:>9,} m^2)  {net_km[t]:>9.0f} km   "
              f"density {dens:6.2f} km/km^2{note}")
    print(f"  baked headwaters      {tot_heads:>8}   ({tot_heads / n:8.1f} per tile, "
          f"{tot_heads / (n * TILE_KM2):.1f}/km^2)")
    print()

    # --- the verdict -------------------------------------------------------
    # Systems are counted per SPRING, and a spring needs a drain. The two
    # independent estimates are printed side by side rather than averaged,
    # because they measure different things and their disagreement is
    # information.
    term_per_sb = tot_term / n * TILES_PER_SUPERBLOCK
    heads_per_sb = tot_heads / n * TILES_PER_SUPERBLOCK

    print("VERDICT -- against the plan's assumed "
          f"{PLAN_SYSTEMS_PER_SUPERBLOCK} systems per superblock")
    print(f"  from terminal basins   {term_per_sb:>10.0f} systems/superblock")
    print(f"  from baked headwaters  {heads_per_sb:>10.0f} systems/superblock")
    print()

    best = max(term_per_sb, heads_per_sb)
    if best >= PLAN_SYSTEMS_PER_SUPERBLOCK:
        print("  PASS -- the world supplies at least the assumed system count from")
        print("  registered features alone, before any doline is counted.")
    elif best >= PLAN_SYSTEMS_PER_SUPERBLOCK * 0.25:
        print("  MARGINAL -- registered features alone fall short, but they are a")
        print("  lower bound and the doline population is not visible here. The")
        print("  premise survives; measure dolines inside the bake before tuning.")
    else:
        print("  FAIL as stated -- registered features supply far less than assumed.")
        print("  Do NOT proceed on the per-system corridor plan until the doline")
        print("  count is measured inside the bake with `basin_depth` in hand.")
    print()
    print("  Neither number counts dolines. Both are printed because they")
    print("  measure different things; do not add them.")

    if args.json:
        args.json.write_text(json.dumps({
            "tiles": n,
            "bake_ver": bakes,
            "basins_total": tot_basin,
            "basins_terminal": tot_term,
            "basins_spanning": tot_span,
            "by_kind": dict(kinds),
            "heads": tot_heads,
            "network_km_by_log2_acc": {str(t): net_km[t] for t in ACC_THRESHOLDS_LOG2},
            "drainage_density_km_per_km2_by_log2_acc": {
                str(t): net_km[t] / (n * TILE_KM2) for t in ACC_THRESHOLDS_LOG2},
            "systems_per_superblock_from_terminal": term_per_sb,
            "systems_per_superblock_from_heads": heads_per_sb,
            "plan_assumption": PLAN_SYSTEMS_PER_SUPERBLOCK,
            "per_tile": [
                {k: (dict(v) if isinstance(v, collections.Counter) else v)
                 for k, v in r.items()} for r in rows
            ],
            "failed": failed,
        }, indent=2), encoding="utf-8")
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
