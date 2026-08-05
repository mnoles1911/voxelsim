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

   READ `trunk` BEFORE QUOTING EITHER NUMBER. Both are real accumulations and
   NEITHER is a river, for two different reasons that cost a whole bake to
   find:

     * 82.5 m^3/s IS NOT ON LAND. Its cell is at -3132 m, on the abyssal
       floor at the grid's west edge, and its 24,440 km^2 "catchment" is 82%
       of every land cell in the world. The fill floods the ocean basin too,
       so the whole planet's runoff converges into one submarine pit. It is
       the sea, counted once. The largest accumulation ON LAND is 14.2 m^3/s
       draining 1,960 km^2 -- 5.8x smaller.
     * 14.2 m^3/s IS NOT DRAWN. Its lower reaches cross tiles that are 10-40%
       filled depression, and the bake does not fill them: it registers them
       as basins, writes them dry, and the trunk sinks. Baked, tile (-7,-5)
       carried an interior discharge maximum of 1.3e6 m^3/yr = 0.041 m^3/s
       against the 3.95e8 m^3/yr the pyramid injected -- a factor of 275.

   So the quantity that predicts a bake is the trunk on land WITH A FILL
   CONSTRAINT, which is what `trunk` ranks and what `blocks` now prints as
   `fill%`. The tiles that already baked large rivers are 0.3-2.4% filled;
   every block above 5% is a chain of pits the fill joined into a channel.

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

    print("d8 discharge, area, and the UNFILLED control ...", flush=True)
    #: Three extra fields, each answering a question the MFD discharge alone
    #: cannot and each cheap next to the fill that already ran:
    #:   acc_d8   MFD splits at a delta, so the mouth cell of a real river
    #:            reads low and a mouth ranking on `acc` is wrong by ~35%.
    #:            D8 does not split, so a catchment is a catchment.
    #:   area     the catchment in m^2, which is the number a river is
    #:            actually compared to Earth by. D8 FOR THE SAME REASON, and
    #:            additionally so it is the catchment OF `acc_d8`: an MFD area
    #:            read at a D8 trunk cell is a different river's number and
    #:            printed 687 km^2 where the D8 catchment is 1,960.
    #:   acc_raw  the same accumulation on the UNFILLED surface. The ratio to
    #:            `acc` is how much of a "river" is the fill's invention.
    src = (runoff.astype(np.float64) * 1e-3) * (COARSE_PX_M * COARSE_PX_M)
    acc_d8 = np.asarray(_flow.accumulate_d8(zf, cell_m=COARSE_PX_M, source=src,
                                            receivers=rec), np.float64)
    area = np.asarray(_flow.accumulate_d8(
        zf, cell_m=COARSE_PX_M, receivers=rec,
        source=np.full(zf.shape, COARSE_PX_M * COARSE_PX_M, np.float64)),
        np.float64)
    acc_raw = np.asarray(_flow.accumulate_mfd(elev, cell_m=COARSE_PX_M,
                                              source=src), np.float64)
    del src
    print(f"  d8 trunk {acc_d8.max()/SECS_PER_YR:.1f} m3/s; "
          f"UNFILLED trunk {acc_raw.max()/SECS_PER_YR:.1f} m3/s "
          f"({acc_raw.max()/max(acc.max(), 1.0):.1%} of the filled one)",
          flush=True)

    np.save(out / "elev.npy", elev)
    np.save(out / "runoff.npy", runoff)
    np.save(out / "temp.npy", temp)
    np.save(out / "precip.npy", precip)
    np.save(out / "acc.npy", acc.astype(np.float32))
    np.save(out / "acc_d8.npy", acc_d8.astype(np.float32))
    np.save(out / "acc_raw.npy", acc_raw.astype(np.float32))
    np.save(out / "area.npy", area.astype(np.float32))
    np.save(out / "fill.npy", (zf - elev).astype(np.float32))
    np.save(out / "rec.npy", np.asarray(rec, np.int32))
    (out / "meta.json").write_text(json.dumps(
        {"x0": x0, "y0": y0, "tile_px": TILE_PX, "coarse_px_m": COARSE_PX_M,
         "province_smooth_m": PROVINCE_SMOOTH_M,
         "coarse_dir": str(a.coarse_dir)}, indent=1))
    print(f"-> {out}")
    return 0


#: 8-neighbourhood, for the main-stem walk.
_N8 = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))


def _load(d: Path, name: str):
    p = d / f"{name}.npy"
    if not p.exists():
        raise SystemExit(
            f"{p} is missing -- it is written by `accumulate`. A cache made "
            "before the trunk fields were added must be re-accumulated.")
    return np.load(p)


def _stem_upstream(zf, q, r0, c0, max_steps=200000):
    """The main stem from (r0,c0) upstream, as a list of (row, col).

    STEP TO THE HIGHEST-Q NEIGHBOUR THAT IS STRICTLY HIGHER ON THE FILLED
    SURFACE, not to the largest D8 donor. Both were tried and the donor walk
    is wrong here: `acc` is MULTI-receiver, so a cell's accumulation is not
    monotone along the single-receiver forest, and the walk oscillated -- it
    reported Q rising 10.8 -> 8.4 -> 14.2 m^3/s over three consecutive cells
    of what it claimed was one river, then terminated after 37 cells in a
    coastal flat. Requiring ascent on `zf` makes the path a genuine profile;
    it costs the walk the right to cross a filled pit, which is a feature,
    because a stem that only exists inside a pit is the thing being hunted.
    """
    seen = {(r0, c0)}
    path = [(r0, c0)]
    H, W = zf.shape
    r, c = r0, c0
    for _ in range(max_steps):
        best, bq = None, -1.0
        for dr, dc in _N8:
            rr, cc = r + dr, c + dc
            if not (0 <= rr < H and 0 <= cc < W) or (rr, cc) in seen:
                continue
            if zf[rr, cc] <= zf[r, c]:
                continue
            if q[rr, cc] > bq:
                bq, best = float(q[rr, cc]), (rr, cc)
        if best is None:
            return path
        path.append(best)
        seen.add(best)
        r, c = best
    return path


def _tile_grid(elev, fill, q_land, area, x0, y0):
    """Per-tile (land fraction, trunk on land, its catchment, filled fraction).

    `fill_pct` is the share of LAND cells the depression fill raised at all,
    and it is the column this tool exists to print. See the module docstring:
    the two blocks that have baked large rivers are 0.3-2.4% filled, the
    world's nominally largest trunk crosses tiles at 10-40%, and the bake
    draws 0.041 m^3/s there against a predicted 14.2.
    """
    H, W = elev.shape
    ny, nx = H // TILE_PX, W // TILE_PX
    out = {k: np.zeros((ny, nx)) for k in ("land", "q", "area", "fill")}
    for j in range(ny):
        for i in range(nx):
            sl = (slice(j * TILE_PX, (j + 1) * TILE_PX),
                  slice(i * TILE_PX, (i + 1) * TILE_PX))
            land = elev[sl] > 0
            out["land"][j, i] = land.mean()
            if not land.any():
                continue
            out["q"][j, i] = float(np.where(land, q_land[sl], 0.0).max())
            out["area"][j, i] = float(np.where(land, area[sl], 0.0).max())
            out["fill"][j, i] = float((fill[sl][land] > 0.01).mean()) * 100.0
    return out


def cmd_trunk(a) -> int:
    """Find the largest river ON LAND, and rank blocks that can actually bake it."""
    d = Path(a.dir)
    meta = json.loads((d / "meta.json").read_text())
    x0, y0 = meta["x0"], meta["y0"]
    elev = _load(d, "elev")
    acc = _load(d, "acc").astype(np.float64)
    q8 = _load(d, "acc_d8").astype(np.float64)
    raw = _load(d, "acc_raw").astype(np.float64)
    area = _load(d, "area").astype(np.float64)
    fill = _load(d, "fill")
    H, W = elev.shape
    land = elev > 0
    land_km2 = float(land.sum()) * COARSE_PX_M ** 2 / 1e6

    def loc(i):
        r, c = divmod(int(i), W)
        return r, c, x0 + c // TILE_PX, y0 + r // TILE_PX

    print(f"world {H}x{W} @ {COARSE_PX_M:g} m = "
          f"{H * W * COARSE_PX_M ** 2 / 1e6:,.0f} km2 extent, "
          f"{land_km2:,.0f} km2 land ({land.mean():.1%})")

    # THE TRAP, PRINTED FIRST. A whole-world maximum is almost always in the
    # sea, and quoting it as a river is the mistake this section exists to
    # make impossible to repeat.
    r, c, tx, ty = loc(np.argmax(acc))
    print(f"\nWHOLE-GRID maximum  {acc[r, c] / SECS_PER_YR:8.2f} m3/s  "
          f"tile ({tx},{ty})  z = {elev[r, c]:,.0f} m  "
          f"catchment {area[r, c] / 1e6:,.0f} km2")
    if elev[r, c] <= _basins.SEA_LEVEL_M:
        print("  ^^ BELOW SEA LEVEL. This is not a river. The fill floods the "
              "ocean basin\n     as readily as a valley, so the planet's "
              "runoff converges on one\n     submarine pit. Quote the LAND "
              "figure below instead.")

    q8l = np.where(land, q8, 0.0)
    r, c, tx, ty = loc(np.argmax(q8l))
    q_land = q8l[r, c]
    print(f"\nLARGEST TRUNK ON LAND {q_land / SECS_PER_YR:8.2f} m3/s  "
          f"tile ({tx},{ty}) px ({c % TILE_PX},{r % TILE_PX})  "
          f"z = {elev[r, c]:,.0f} m")
    print(f"  catchment {area[r, c] / 1e6:,.0f} km2 = "
          f"{area[r, c] / 1e6 / land_km2:.1%} of the world's land")
    print(f"  law says  {float(_water.channel_width_m(q_land)):.1f} m wide, "
          f"{float(_water.water_depth_m(q_land)):.2f} m deep")
    print(f"  UNFILLED, the largest land trunk is only "
          f"{np.where(land, raw, 0.0).max() / SECS_PER_YR:.2f} m3/s -- "
          "the rest is fill.")

    grid = _tile_grid(elev, fill, q8, area, x0, y0)
    ny, nx = grid["land"].shape
    rows = []
    for shape in (tuple(int(v) for v in s.split("x")) for s in a.shapes.split(",")):
        bw, bh = shape
        for j in range(ny - bh + 1):
            for i in range(nx - bw + 1):
                s = (slice(j, j + bh), slice(i, i + bw))
                if grid["land"][s].min() < a.min_land:
                    continue
                rows.append(dict(
                    x0=x0 + i, y0=y0 + j, nx=bw, ny=bh, n=bw * bh,
                    q_m3s=float(grid["q"][s].max()) / SECS_PER_YR,
                    area_km2=float(grid["area"][s].max()) / 1e6,
                    fill_max=float(grid["fill"][s].max()),
                    fill_mean=float(grid["fill"][s].mean()),
                    land=float(grid["land"][s].mean())))
    rows.sort(key=lambda z: -z["q_m3s"])
    (d / "trunk_blocks.json").write_text(json.dumps(rows, indent=1))

    def _table(sel, title):
        print(f"\n{title}")
        print(f"  {'block':>18} {'n':>2} {'trunk m3/s':>10} {'catch km2':>10} "
              f"{'width':>7} {'depth':>6} {'fill max':>8} {'fill avg':>8} "
              f"{'land':>6}")
        for b in sel[:a.top]:
            print(f"  x{b['x0']:>3}..{b['x0']+b['nx']-1:<3} "
                  f"y{b['y0']:>3}..{b['y0']+b['ny']-1:<3} {b['n']:2d} "
                  f"{b['q_m3s']:10.2f} {b['area_km2']:10.0f} "
                  f"{float(_water.channel_width_m(b['q_m3s'] * SECS_PER_YR)):6.1f}m "
                  f"{float(_water.water_depth_m(b['q_m3s'] * SECS_PER_YR)):5.2f}m "
                  f"{b['fill_max']:7.1f}% {b['fill_mean']:7.1f}% "
                  f"{b['land']*100:5.1f}%")

    _table(rows, "BLOCKS BY TRUNK -- the naive ranking, KEPT SO IT CAN BE "
                 "COMPARED, not used:")
    keep = [b for b in rows if b["fill_max"] <= a.max_fill]
    _table(keep, f"BLOCKS THE BAKE CAN CARRY (every tile <= {a.max_fill:g}% "
                 "filled) -- USE THIS ONE:")

    if a.stem:
        zf = elev + fill
        bx, by = (int(v) for v in a.stem.split(","))
        sl = (slice((by - y0) * TILE_PX, (by - y0 + 1) * TILE_PX),
              slice((bx - x0) * TILE_PX, (bx - x0 + 1) * TILE_PX))
        sub = np.where(elev[sl] > 0, q8[sl], 0.0)
        rr, cc = np.unravel_index(int(np.argmax(sub)), sub.shape)
        r0 = (by - y0) * TILE_PX + rr
        c0 = (bx - x0) * TILE_PX + cc
        path = _stem_upstream(zf, q8, r0, c0)
        print(f"\nLONG PROFILE of the stem through tile ({bx},{by}), "
              f"mouth end first: {len(path)} cells = "
              f"{len(path) * COARSE_PX_M / 1000:.1f} km")
        print(f"  {'km':>7} {'tile':>10} {'z m':>8} {'m3/s':>7} {'km2':>9} "
              f"{'width':>7} {'depth':>6} {'fill m':>7}")
        step = max(1, len(path) // a.profile_rows)
        for k in list(range(0, len(path), step)) + [len(path) - 1]:
            r, c = path[k]
            q = q8[r, c]
            print(f"  {k * COARSE_PX_M / 1000:7.1f} "
                  f"({x0 + c // TILE_PX:>3},{y0 + r // TILE_PX:>3}) "
                  f"{elev[r, c]:8.0f} {q / SECS_PER_YR:7.2f} "
                  f"{area[r, c] / 1e6:9.0f} "
                  f"{float(_water.channel_width_m(q)):6.1f}m "
                  f"{float(_water.water_depth_m(q)):5.2f}m "
                  f"{fill[r, c]:7.1f}")
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


def _block(elev, runoff, acc, recf, inside_buf, x0, y0, bx, by, nx, ny,
           fill=None):
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
        # See the module docstring: a high trunk over highly-filled ground is
        # a chain of pits, and the bake will not draw it.
        fill_pct=(float((fill[sl][land] > 0.01).mean()) * 100.0
                  if fill is not None and land.any() else float("nan")),
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
    fillp = d / "fill.npy"
    fill = np.load(fillp) if fillp.exists() else None
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
                b = _block(elev, runoff, acc, recf, inside, x0, y0, bx, by,
                           nx, ny, fill=fill)
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
          f"{'width':>7} {'depth':>6} {'contain':>8} {'land%':>6} {'fill%':>6} "
          f"{'relief':>7}")
    for b in sorted(sel, key=lambda z: -z["runoff_land"])[:a.top]:
        print(f" x{b['x0']:>3}..{b['x0']+b['nx']-1:<3} y{b['y0']:>3}..{b['y0']+b['ny']-1:<3} "
              f"{b['n']:2d} {b['runoff_land']:8.0f} {b['runoff_p90']:7.0f} "
              f"{b['q_max_m3s']:10.2f} {b['width_m']:6.1f}m {b['depth_m']:5.2f}m "
              f"{b['containment']:8.2f} {b['land']*100:5.1f}% "
              f"{b['fill_pct']:5.1f}% {b['relief']:7.0f}")
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
    trk = sub.add_parser(
        "trunk", help="the largest river ON LAND, and the blocks that can bake it")
    trk.add_argument("--dir", required=True)
    trk.add_argument("--shapes", default="2x2,3x2,2x3")
    trk.add_argument("--min-land", type=float, default=0.90,
                     help="minimum land fraction of EVERY tile in the block")
    trk.add_argument("--max-fill", type=float, default=3.0,
                     help="maximum %% of a tile's land the depression fill "
                          "raised. The blocks that baked real rivers are "
                          "0.3-2.4%%; above ~5%% the trunk is a chain of pits "
                          "the fill joined up and the bake will not draw it.")
    trk.add_argument("--top", type=int, default=15)
    trk.add_argument("--stem", default=None, metavar="X,Y",
                     help="also walk and print the long profile of the stem "
                          "through this tile, upstream from its largest cell")
    trk.add_argument("--profile-rows", type=int, default=22)
    trk.set_defaults(fn=cmd_trunk)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    raise SystemExit(main())
