#!/usr/bin/env python3
"""Rank the whole world by RUNOFF and by TRUNK DISCHARGE, to pick a bake region.

    # stage 1: stitch every coarse tile, fill, accumulate, cache the arrays
    python tools/survey_world_water.py accumulate \
        --coarse-dir D:/voxelsim/tile-cache/<ns>/<seedhex>/s1 \
        --out D:/tmp/worldwater

    # stage 2: score every contiguous tile block on the cached arrays
    python tools/survey_world_water.py blocks --dir D:/tmp/worldwater

WHY THIS EXISTS
---------------
Every water measurement in this project came from one corridor picked months
ago because runoff was high the whole way, and "high" there means 149-292
mm/yr. Choosing the NEXT region by eye, or by precipitation, would repeat the
mistake in a new place. This scores the actual quantity the bake consumes.

THREE THINGS IT DOES THAT A PROXY DOES NOT, each because the naive version is
wrong by a factor this large:

1. RUNOFF, NOT PRECIPITATION. It calls `basins.pet_mm_yr` and
   `basins.budyko_runoff_mm_yr` through `province.dequantize_climate` and
   `province.box_smooth` -- the identical chain `water.runoff_field_mm_yr`
   runs, at the identical `province_smooth_m`. Precipitation ranks the world
   almost backwards at the top end: the wettest tile by rain (-15,-16, 2724
   mm/yr) converts 56% of it to runoff at 22 C, while an alpine tile with half
   the rain (-4,-4, 1449 mm/yr) converts 87% at -4.4 C. Runoff is rainfall
   minus evaporative demand, so cold-and-wet beats warm-and-wet.

2. IT FILLS THE DEPRESSIONS FIRST. `accumulate_mfd`'s own docstring says a raw
   surface parks each pit's catchment in the pit, and at 30 m over a whole
   world that is not a detail: unfilled, the world's largest trunk read 1.5
   m^3/s, which is 0.6% of the water the sky delivers to it. Filled, the arid
   corridor's mouth reproduces the 5.87e7 m^3/yr already recorded in
   `pipeline`'s carried-discharge note. That agreement is the check that this
   tool is measuring the same world the bake does.

   BUT FILLING ALSO FLOODS THE OCEAN. This docstring used to cite an 82.5
   m^3/s figure here as "the world's largest trunk". It is not a river: it is
   a SUBMARINE SINK at -3132 m, created because `fill_depressions` does not
   know the sea floor is not land and happily fills ocean basins. Any
   whole-world ranking off this tool must exclude cells below sea level before
   it means anything. The number stood in three documents before it was
   caught.

3. IT ACCUMULATES OVER THE WHOLE WORLD, NOT A WINDOW. A catchment leaves any
   window smaller than itself, and the hydrology pyramid is precisely the
   machinery that carries that upstream water in as `inflow_q_m3_yr`. Scored on
   a 6x4 tile window the same alpine block reads 0.3 m^3/s; whole-world it
   reads 10.9. A window understates exactly the big rivers worth baking.

WHAT `containment` MEANS, and why a region needs it. It is
``1 - Q_entering_the_block / Q_max_in_the_block``, computed from the D8
receivers across the block's one-cell boundary ring. Near 1 the block MAKES its
own river -- headwaters to mouth inside the footprint, which is what "fly a
whole river" requires. Near 0 the block is a mid-reach of a river born
somewhere else, and its trunk is an inflow boundary condition rather than
anything the region can show.

SEA IS REPORTED SEPARATELY. River water paints over the seafloor and the bv13
lateral fill amplifies it, so a block's land fraction is printed on every row
and blocks under 50% land are dropped: their wet-cell counts describe the
defect, not the rainfall.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service.tile_codec import decode  # noqa: E402
from terrain_service.bake import basins as _basins  # noqa: E402
from terrain_service.bake import flow as _flow  # noqa: E402
from terrain_service.bake import province as _province  # noqa: E402
from terrain_service.bake import water as _water  # noqa: E402

TILE_PX = 512
COARSE_PX_M = 30.0
SECS_PER_YR = 365.25 * 86400.0
#: pipeline.BakeConstants.province_smooth_m, the pitch every climate consumer
#: smooths at. Stated here rather than imported so this tool cannot silently
#: start smoothing differently from `runoff_field_mm_yr` without the constant
#: being edited in view.
PROVINCE_SMOOTH_M = 480.0


def _stitch(coarse_dir: Path):
    """Every `<x>_<y>.vxtl` in one grid, row index tracking y, column x.

    The axis order is `pipeline._ring_windows`': it places a ring tile at
    ``(apron + dy*n, apron + dx*n)``, so row <- tile y and column <- tile x,
    both increasing. Getting this transposed would rank a mirrored world.
    """
    coords = []
    for p in coarse_dir.glob("*.vxtl"):
        try:
            sx, sy = p.stem.split("_")
            coords.append((int(sx), int(sy), p))
        except ValueError:
            continue
    if not coords:
        raise SystemExit(f"no <x>_<y>.vxtl under {coarse_dir}")
    xs = sorted({c[0] for c in coords})
    ys = sorted({c[1] for c in coords})
    x0, y0 = xs[0], ys[0]
    H = (ys[-1] - y0 + 1) * TILE_PX
    W = (xs[-1] - x0 + 1) * TILE_PX
    print(f"{len(coords)} tiles, x[{xs[0]},{xs[-1]}] y[{ys[0]},{ys[-1]}] -> {H}x{W}",
          flush=True)
    elev = np.zeros((H, W), np.float32)
    tu8 = np.zeros((H, W), np.uint8)
    pu8 = np.zeros((H, W), np.uint8)
    for (tx, ty, p) in coords:
        t = decode(p.read_bytes())
        sl = (slice((ty - y0) * TILE_PX, (ty - y0 + 1) * TILE_PX),
              slice((tx - x0) * TILE_PX, (tx - x0 + 1) * TILE_PX))
        elev[sl] = t.elevation
        tu8[sl] = t.climate[_province.CLIMATE_ORDER.index("temperature")]
        pu8[sl] = t.climate[_province.CLIMATE_ORDER.index("precipitation")]
    return elev, tu8, pu8, x0, y0


def _runoff(tu8, pu8):
    """Budyko runoff, mm/yr, over the stitched grid -- the bake's own chain."""
    half = max(int(round(PROVINCE_SMOOTH_M / COARSE_PX_M / 2.0)), 1)
    tlo, thi = _province.CLIMATE_RANGES["temperature"]
    plo, phi = _province.CLIMATE_RANGES["precipitation"]
    temp = _province.box_smooth(
        np.float32(tlo) + tu8.astype(np.float32) * np.float32((thi - tlo) / 255.0),
        half)
    precip = _province.box_smooth(
        np.float32(plo) + pu8.astype(np.float32) * np.float32((phi - plo) / 255.0),
        half)
    wb = _basins.WaterBalance()
    pet = _basins.pet_mm_yr(temp, wb)
    return (np.asarray(_basins.budyko_runoff_mm_yr(precip, pet, wb), np.float32),
            np.asarray(temp, np.float32), np.asarray(precip, np.float32))


def cmd_accumulate(a) -> int:
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    elev, tu8, pu8, x0, y0 = _stitch(Path(a.coarse_dir))
    runoff, temp, precip = _runoff(tu8, pu8)
    del tu8, pu8
    print(f"runoff mm/yr: mean {runoff.mean():.1f} max {runoff.max():.1f}", flush=True)

    print("filling depressions ...", flush=True)
    zf = _flow.fill_depressions(elev)
    print("accumulating MFD (runoff volume as source) ...", flush=True)
    src = (runoff.astype(np.float64) * 1e-3) * (COARSE_PX_M * COARSE_PX_M)
    acc = np.asarray(_flow.accumulate_mfd(zf, cell_m=COARSE_PX_M, source=src),
                     np.float64)
    del src
    print(f"trunk max {acc.max():.4e} m3/yr = {acc.max()/SECS_PER_YR:.1f} m3/s",
          flush=True)
    print("d8 receivers (for the boundary-flux term) ...", flush=True)
    rec, _ = _flow.d8_receivers(zf, cell_m=COARSE_PX_M)

    np.save(out / "elev.npy", elev)
    np.save(out / "runoff.npy", runoff)
    np.save(out / "temp.npy", temp)
    np.save(out / "precip.npy", precip)
    np.save(out / "acc.npy", acc.astype(np.float32))
    np.save(out / "rec.npy", np.asarray(rec, np.int32))
    (out / "meta.json").write_text(json.dumps(
        {"x0": x0, "y0": y0, "tile_px": TILE_PX, "coarse_px_m": COARSE_PX_M,
         "province_smooth_m": PROVINCE_SMOOTH_M,
         "coarse_dir": str(a.coarse_dir)}, indent=1))
    print(f"-> {out}")
    return 0


def _tiles_table(elev, runoff, temp, precip, acc, x0, y0):
    H, W = elev.shape
    rows = []
    for ty in range(y0, y0 + H // TILE_PX):
        for tx in range(x0, x0 + W // TILE_PX):
            sl = (slice((ty - y0) * TILE_PX, (ty - y0 + 1) * TILE_PX),
                  slice((tx - x0) * TILE_PX, (tx - x0 + 1) * TILE_PX))
            za, qa, ra = elev[sl], acc[sl], runoff[sl]
            land = za > 0
            qm = float(qa.max())
            rows.append(dict(
                x=tx, y=ty, q_max=qm, q_max_m3s=qm / SECS_PER_YR,
                width_m=float(_water.channel_width_m(qm)),
                depth_m=float(_water.water_depth_m(qm)),
                runoff=float(ra.mean()),
                runoff_land=float(ra[land].mean()) if land.any() else 0.0,
                temp_c=float(temp[sl].mean()), precip=float(precip[sl].mean()),
                land=float(land.mean()), z_mean=float(za.mean()),
                z_min=float(za.min()), z_max=float(za.max())))
    return rows


def _block(elev, runoff, acc, recf, inside_buf, x0, y0, bx, by, nx, ny):
    H, W = elev.shape
    r0, c0 = (by - y0) * TILE_PX, (bx - x0) * TILE_PX
    r1, c1 = r0 + ny * TILE_PX, c0 + nx * TILE_PX
    if r1 > H or c1 > W:
        return None
    sl = (slice(r0, r1), slice(c0, c1))
    za, qa, ra = elev[sl], acc[sl], runoff[sl]
    land = za > 0
    if land.mean() < 0.5:
        return None
    q_max = float(qa.max())
    if q_max <= 0.0:
        return None

    inside_buf[:] = False
    inside_buf.reshape(H, W)[sl] = True
    ring = []
    for rr in (r0 - 1, r1):
        if 0 <= rr < H:
            ring.append(np.arange(max(c0 - 1, 0), min(c1 + 1, W)) + rr * W)
    for cc in (c0 - 1, c1):
        if 0 <= cc < W:
            ring.append(np.arange(max(r0 - 1, 0), min(r1 + 1, H)) * W + cc)
    q_in = 0.0
    if ring:
        rg = np.unique(np.concatenate(ring))
        rg = rg[~inside_buf[rg]]
        rcv = recf[rg]
        ent = rg[(rcv >= 0) & inside_buf[np.maximum(rcv, 0)]]
        if ent.size:
            q_in = float(acc.ravel()[ent].sum())
    return dict(
        x0=bx, y0=by, nx=nx, ny=ny, n=nx * ny,
        tiles=[[bx + i, by + j] for j in range(ny) for i in range(nx)],
        runoff_land=float(ra[land].mean()),
        runoff_p90=float(np.percentile(ra[land], 90)),
        q_max=q_max, q_max_m3s=q_max / SECS_PER_YR,
        width_m=float(_water.channel_width_m(q_max)),
        depth_m=float(_water.water_depth_m(q_max)),
        q_in=q_in, containment=1.0 - min(q_in / q_max, 1.0),
        land=float(land.mean()), relief=float(za.max() - za.min()),
        z_mean=float(za.mean()))


def cmd_blocks(a) -> int:
    d = Path(a.dir)
    meta = json.loads((d / "meta.json").read_text())
    x0, y0 = meta["x0"], meta["y0"]
    elev = np.load(d / "elev.npy")
    runoff = np.load(d / "runoff.npy")
    temp = np.load(d / "temp.npy")
    precip = np.load(d / "precip.npy")
    acc = np.load(d / "acc.npy").astype(np.float64)
    recf = np.load(d / "rec.npy").ravel()
    H, W = elev.shape

    rows = _tiles_table(elev, runoff, temp, precip, acc, x0, y0)
    (d / "tiles.json").write_text(json.dumps(rows, indent=1))
    r = np.array([q["runoff"] for q in rows])
    print(f"\nWORLD per-tile mean runoff, mm/yr, over {len(rows)} tiles:")
    print("  " + "  ".join(f"p{p}={np.percentile(r, p):.0f}"
                           for p in (0, 25, 50, 75, 90, 99, 100)))

    inside = np.zeros(H * W, bool)
    blocks = []
    shapes = [tuple(int(v) for v in s.split("x")) for s in a.shapes.split(",")]
    for nx, ny in shapes:
        for bx in range(x0, x0 + W // TILE_PX - nx + 1):
            for by in range(y0, y0 + H // TILE_PX - ny + 1):
                b = _block(elev, runoff, acc, recf, inside, x0, y0, bx, by, nx, ny)
                if b:
                    blocks.append(b)
    (d / "blocks.json").write_text(json.dumps(blocks, indent=1))

    sel = [b for b in blocks
           if b["n"] >= a.min_tiles and b["land"] >= a.min_land
           and b["containment"] >= a.min_containment]
    print(f"\n{len(blocks)} blocks scored; {len(sel)} pass "
          f"(>= {a.min_tiles} tiles, land >= {a.min_land:.0%}, "
          f"containment >= {a.min_containment:.2f})")
    print(f"\n{'block':>20} {'n':>2} {'runoffL':>8} {'p90':>7} {'trunk m3/s':>10} "
          f"{'width':>7} {'depth':>6} {'contain':>8} {'land%':>6} {'relief':>7}")
    for b in sorted(sel, key=lambda z: -z["runoff_land"])[:a.top]:
        print(f" x{b['x0']:>3}..{b['x0']+b['nx']-1:<3} y{b['y0']:>3}..{b['y0']+b['ny']-1:<3} "
              f"{b['n']:2d} {b['runoff_land']:8.0f} {b['runoff_p90']:7.0f} "
              f"{b['q_max_m3s']:10.2f} {b['width_m']:6.1f}m {b['depth_m']:5.2f}m "
              f"{b['containment']:8.2f} {b['land']*100:5.1f}% {b['relief']:7.0f}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    acc = sub.add_parser("accumulate", help="stitch, fill, accumulate, cache")
    acc.add_argument("--coarse-dir", required=True)
    acc.add_argument("--out", required=True)
    acc.set_defaults(fn=cmd_accumulate)
    blk = sub.add_parser("blocks", help="score contiguous tile blocks")
    blk.add_argument("--dir", required=True)
    blk.add_argument("--shapes", default="3x2,2x3,2x2",
                     help="block shapes to score, e.g. 3x2,2x3")
    blk.add_argument("--min-tiles", type=int, default=4)
    blk.add_argument("--min-land", type=float, default=0.95)
    blk.add_argument("--min-containment", type=float, default=0.5)
    blk.add_argument("--top", type=int, default=20)
    blk.set_defaults(fn=cmd_blocks)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    raise SystemExit(main())
