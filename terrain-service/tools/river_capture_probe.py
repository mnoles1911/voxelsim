"""Numbers to attach to a river SCREENSHOT: is this pose legal, and how wide is
the water actually drawn where the camera is pointed.

WHY THIS EXISTS AS A FILE. A capture of river water is only evidence if two
things are known independently of the picture, and neither of them is visible
in it:

  * WHETHER THE CAMERA WAS INSIDE BAKED COVERAGE BY ENOUGH MARGIN. A pose
    4,073 m from the edge of the baked tile set produced 2,898 fine-tier
    residency-gate leaks answering elevation queries at SEA LEVEL -- fabricated
    ground, in frame, in a capture that looked fine. The working rule since is
    4.5 km. That is a property of the tile set and the camera position alone,
    so it is decidable HERE, before an editor is launched and seven minutes of
    settle are spent on a frame that has to be thrown away.

    This is not redundant with the engine's own gate. Under `-unattended` a
    leak is fatal (VoxelFineTileStreamer.cpp:543), so the engine catches the
    gross case -- but it catches it by killing a run that has already cost its
    settle, and it only fires once a query actually lands off coverage. A pose
    can sit 1.6 km from the edge, never trip the gate on the frame it happens
    to draw, and still be a pose nobody should be quoting.

  * HOW WIDE AND HOW DEEP THE WATER IS AT THAT SPOT. "The river now fills the
    valley" is a claim about metres, and the owner's complaint was specifically
    about metres to the left and right. So every capture is quoted with a
    measured cross-section width and depth, and the picture is checked against
    the number rather than judged by eye.

WHICH SOURCE THE NUMBERS COME FROM, AND WHY IT IS NOT THE WIRE BY DEFAULT.
`--source npz` reads the bake's own unquantised float32 dumps, which is what
`docs/measurements/river-lateral-fill-2026-08-04.txt` quoted, so a number from
this tool is directly comparable to that document. `--source wire` decodes the
shipped .vxtl instead -- what the client actually loads -- and is ~46 s/tile
against ~2 s, which is why it is not the default.

They were checked against each other rather than assumed equal. On tile
(-12,-5), interior (row 850, col 2627), the doc's published site:

    npz    ground 348.0437 m   water 349.1055 m   -> depth 1.0618 m
    wire   water_cp 106 at a 10 mm LSB            -> depth 1.0600 m
    wet cell totals, wire vs the doc's percentages, all four tiles:
        (-11,-4) 148,232   (-11,-5) 406,999   (-11,-6) 2,156,457   (-12,-5) 179,799
    and (-11,-6) is the doc's own stated 2,156,457, exactly.

THE AXIS CONVENTION, WHICH IS THE ONE THING HERE THAT IS EASY TO GET BACKWARDS.
`BakeGeometry.padded_origin_cells` (pipeline.py:490) fixes it: ROW TRACKS
tile_y AND COLUMN TRACKS tile_x, BOTH INCREASING. Interior row 0 of tile ty is
that tile's MINIMUM y. There is no image-style top-down flip anywhere on this
path, and assuming one puts a site on the wrong side of its tile -- which, for
the site above, is the difference between 1.6 km and 13.8 km of edge clearance.
The convention is verified rather than trusted: `python river_capture_probe.py
verify` reproduces the doc's cross-section from these coordinates.

Usage:
    python tools/river_capture_probe.py coverage
    python tools/river_capture_probe.py verify
    python tools/river_capture_probe.py clearance -160874.5 -85643.1
    python tools/river_capture_probe.py at        -160874.5 -85643.1
    python tools/river_capture_probe.py section   -160874.5 -85643.1
    python tools/river_capture_probe.py scan --min-width 60
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# Production geometry (bake.pipeline.BakeGeometry.PRODUCTION), asserted against
# each tile header below rather than trusted.
COARSE_TILE_PX = 512
COARSE_PIXEL_M = 30.0
SCALE = 16
FINE_TILE_PX = COARSE_TILE_PX * SCALE          # 8192
PIXEL_M = COARSE_PIXEL_M / SCALE               # 1.875
TILE_SPAN_M = COARSE_TILE_PX * COARSE_PIXEL_M  # 15360.0

#: The gate-leak margin. See the module docstring.
CLEAR_M = 4500.0

WIRE = Path("D:/voxelsim/tile-cache/terrain-diffusion-unlabeled-"
            "80b9ca451a23eae4-ba9c62170/000000000135276f/s16")
NPZ = Path("D:/tmp/latfill-npz")

SOURCE = "npz"
_G: dict[tuple[int, int], np.ndarray] = {}
_W: dict[tuple[int, int], np.ndarray] = {}


def baked_tiles() -> list[tuple[int, int]]:
    out = []
    for p in sorted(WIRE.glob("*.vxtl")):
        tx, ty = p.stem.split("_")
        out.append((int(tx), int(ty)))
    return out


def _load(tx: int, ty: int) -> None:
    if (tx, ty) in _W:
        return
    if (tx, ty) not in set(baked_tiles()):
        raise KeyError(f"tile ({tx},{ty}) is NOT baked")
    if SOURCE == "npz":
        z = np.load(NPZ / f"{tx}_{ty}.npz")
        _G[(tx, ty)] = z["elevation_m"]
        _W[(tx, ty)] = z["water_surface_m"]      # NaN where dry
    else:
        import zstandard
        from terrain_service import tile_codec as tc
        dec = lambda b, n: zstandard.ZstdDecompressor().decompress(b, max_output_size=n)
        t = tc.decode_v2((WIRE / f"{tx}_{ty}.vxtl").read_bytes(), decompressor=dec)
        assert t.size == FINE_TILE_PX and t.scale == SCALE, (
            f"tile ({tx},{ty}) is not production geometry: size {t.size} scale {t.scale}")
        assert t.bake_ver == 13, f"tile ({tx},{ty}) is bake_ver {t.bake_ver}, not 13"
        # The wire carries DEPTH, not surface; ground is spline(cp) and is NOT
        # reconstructed here (see the doc note in tile_codec.water_surface_mm_from_depth).
        d = t.water_cp
        _G[(tx, ty)] = None
        _W[(tx, ty)] = np.where(d >= 0, d.astype(np.float32) * 0.01, np.nan)  # depth m
    if (tx, ty) not in _W:
        raise KeyError((tx, ty))


def world_to_cell(x_m: float, y_m: float) -> tuple[int, int, int, int]:
    """world metres -> (tile_x, tile_y, row, col); row/col interior 0..8191."""
    tx = math.floor(x_m / TILE_SPAN_M)
    ty = math.floor(y_m / TILE_SPAN_M)
    col = int((x_m - tx * TILE_SPAN_M) / PIXEL_M)
    row = int((y_m - ty * TILE_SPAN_M) / PIXEL_M)
    return tx, ty, min(row, FINE_TILE_PX - 1), min(col, FINE_TILE_PX - 1)


def cell_to_world(tx: int, ty: int, row: int, col: int) -> tuple[float, float]:
    return (tx * TILE_SPAN_M + (col + 0.5) * PIXEL_M,
            ty * TILE_SPAN_M + (row + 0.5) * PIXEL_M)


def ground_water(x_m: float, y_m: float) -> tuple[float, float]:
    """(ground_m, water_surface_m) -- water is NaN where dry."""
    tx, ty, row, col = world_to_cell(x_m, y_m)
    _load(tx, ty)
    g = _G[(tx, ty)]
    return (float("nan") if g is None else float(g[row, col]),
            float(_W[(tx, ty)][row, col]))


def is_wet(x_m: float, y_m: float) -> bool:
    try:
        tx, ty, row, col = world_to_cell(x_m, y_m)
        _load(tx, ty)
    except KeyError:
        return False   # off coverage reads as dry, deliberately
    return not math.isnan(float(_W[(tx, ty)][row, col]))


# --- the residency-gate clearance rule -------------------------------------

def clearance_m(x_m: float, y_m: float) -> tuple[float, tuple[int, int]]:
    """Distance from (x,y) to the nearest UNBAKED tile rectangle.

    Only tiles adjacent to the baked set can be the nearest one, so a 3-ring
    around it is exhaustive.
    """
    baked = set(baked_tiles())
    xs, ys = [t[0] for t in baked], [t[1] for t in baked]
    best, where = float("inf"), (0, 0)
    for tx in range(min(xs) - 3, max(xs) + 4):
        for ty in range(min(ys) - 3, max(ys) + 4):
            if (tx, ty) in baked:
                continue
            dx = max(tx * TILE_SPAN_M - x_m, 0.0, x_m - (tx + 1) * TILE_SPAN_M)
            dy = max(ty * TILE_SPAN_M - y_m, 0.0, y_m - (ty + 1) * TILE_SPAN_M)
            d = math.hypot(dx, dy)
            if d < best:
                best, where = d, (tx, ty)
    return best, where


# --- cross-sections --------------------------------------------------------

def section(x_m: float, y_m: float, azimuth_deg: float, half_m: float) -> dict:
    """The CONTIGUOUS wet run through (x,y) along one azimuth.

    Contiguous, not the total wet count along the ray: summing every wet sample
    would add the next meander of the same river to this section's width.
    """
    ux, uy = math.cos(math.radians(azimuth_deg)), math.sin(math.radians(azimuth_deg))
    n = int(half_m / PIXEL_M)
    wet = np.array([is_wet(x_m + ux * i * PIXEL_M, y_m + uy * i * PIXEL_M)
                    for i in range(-n, n + 1)])
    if not wet[n]:
        return {"wet": False, "width_m": 0.0}
    lo = hi = n
    while lo > 0 and wet[lo - 1]:
        lo -= 1
    while hi < len(wet) - 1 and wet[hi + 1]:
        hi += 1
    return {"wet": True, "width_m": (hi - lo + 1) * PIXEL_M,
            "clipped": lo == 0 or hi == len(wet) - 1,
            "lo_m": (lo - n) * PIXEL_M, "hi_m": (hi - n) * PIXEL_M,
            "azimuth_deg": azimuth_deg}


def cross_section(x_m: float, y_m: float, half_m: float = 400.0,
                  step_deg: int = 5) -> dict:
    """The NARROWEST contiguous run over a sweep of azimuths.

    A transect taken ALONG the river reports a huge "width" that is really
    length, and it is the axis a naive probe finds first. The minimum over
    azimuth is the cross-section -- the quantity the owner's complaint was
    about ("to the left and right there is empty air").
    """
    best = None
    for az in range(0, 180, step_deg):
        s = section(x_m, y_m, az, half_m)
        if not s["wet"]:
            return s
        if best is None or s["width_m"] < best["width_m"]:
            best = s
    return best


def depth_stats(x_m: float, y_m: float, radius_m: float = 40.0) -> dict:
    """Water depth over the wet cells within a small disc of the point."""
    tx, ty, row, col = world_to_cell(x_m, y_m)
    _load(tx, ty)
    g, w = _G[(tx, ty)], _W[(tx, ty)]
    r = int(radius_m / PIXEL_M)
    r0, r1 = max(0, row - r), min(FINE_TILE_PX, row + r + 1)
    c0, c1 = max(0, col - r), min(FINE_TILE_PX, col + r + 1)
    ws = w[r0:r1, c0:c1]
    wet = ~np.isnan(ws)
    if not wet.any():
        return {"n": 0}
    d = (ws[wet] - g[r0:r1, c0:c1][wet]) if g is not None else ws[wet]
    return {"n": int(wet.sum()), "mean_m": float(d.mean()),
            "p50_m": float(np.median(d)), "max_m": float(d.max()),
            "surface_m": float(np.nanmean(ws[wet])) if g is not None else float("nan")}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", choices=("npz", "wire"), default="npz")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("clearance", "at", "section"):
        p = sub.add_parser(name)
        p.add_argument("x", type=float)
        p.add_argument("y", type=float)
        if name == "section":
            p.add_argument("--half", type=float, default=400.0)
    sub.add_parser("coverage")
    sub.add_parser("verify")
    sc = sub.add_parser("scan")
    sc.add_argument("--min-width", type=float, default=60.0)
    sc.add_argument("--top", type=int, default=25)
    dr = sub.add_parser("dry")
    dr.add_argument("--top", type=int, default=12)

    args = ap.parse_args()
    global SOURCE
    SOURCE = args.source

    if args.cmd == "coverage":
        import zstandard
        from terrain_service import tile_codec as tc
        dec = lambda b, n: zstandard.ZstdDecompressor().decompress(b, max_output_size=n)
        print(f"tile span {TILE_SPAN_M:.0f} m, fine pixel {PIXEL_M} m, "
              f"gate-leak margin {CLEAR_M:.0f} m")
        for tx, ty in baked_tiles():
            # bake_ver comes from the TILE BYTES, never the directory mtime:
            # -bd3d0ddc7 is the newest fine directory here by mtime and is
            # bv8 with no water plane at all.
            raw = (WIRE / f"{tx}_{ty}.vxtl").read_bytes()
            import struct
            H = struct.Struct("<4sHQiiBH")
            bv = struct.unpack_from("<BBBBHH", raw, H.size)[4]
            _load(tx, ty)
            wet = int((~np.isnan(_W[(tx, ty)])).sum())
            print(f"  ({tx:>3},{ty:>3})  bake_ver {bv}  "
                  f"x [{tx*TILE_SPAN_M:>9.0f} ..{(tx+1)*TILE_SPAN_M:>9.0f}]  "
                  f"y [{ty*TILE_SPAN_M:>9.0f} ..{(ty+1)*TILE_SPAN_M:>9.0f}]  "
                  f"wet {wet:>9,} ({wet/FINE_TILE_PX**2*100:.4f}%)")
        return 0

    if args.cmd == "verify":
        # The doc's SITE A. If the axis convention here is wrong, these two
        # numbers are wrong, and everything downstream of them is too.
        x, y = cell_to_world(-12, -5, 850, 2627)
        g, w = ground_water(x, y)
        print(f"doc SITE A: tile (-12,-5) interior (row 850, col 2627) -> "
              f"world ({x:.2f}, {y:.2f})")
        print(f"  ground {g:.4f} m   (doc 348.044)")
        print(f"  water  {w:.4f} m   (doc 349.105)")
        ok = abs(g - 348.044) < 0.01 and abs(w - 349.105) < 0.01
        print("  CONVENTION", "VERIFIED" if ok else "WRONG -- do not trust any coordinate here")
        return 0 if ok else 1

    if args.cmd == "clearance":
        d, t = clearance_m(args.x, args.y)
        print(f"({args.x}, {args.y})  clearance {d:,.1f} m to unbaked tile {t}")
        print("  VERDICT:", f"OK (>= {CLEAR_M:.0f} m)" if d >= CLEAR_M else
              f"TOO CLOSE -- {CLEAR_M - d:,.0f} m short of the {CLEAR_M/1000:.1f} km rule")
        return 0 if d >= CLEAR_M else 1

    if args.cmd == "at":
        tx, ty, row, col = world_to_cell(args.x, args.y)
        g, w = ground_water(args.x, args.y)
        cl, ct = clearance_m(args.x, args.y)
        print(f"({args.x}, {args.y}) -> tile ({tx},{ty}) interior row {row} col {col}")
        print(f"  ground   {g:.3f} m")
        print(f"  water    " + ("DRY" if math.isnan(w) else
                                f"{w:.3f} m  (depth {w-g:.3f} m)"))
        print(f"  clearance {cl:,.1f} m to unbaked {ct}  "
              f"{'OK' if cl >= CLEAR_M else 'TOO CLOSE -- DO NOT SHOOT'}")
        return 0

    if args.cmd == "section":
        r = cross_section(args.x, args.y, args.half)
        if not r["wet"]:
            print("DRY at this point -- no cross-section")
            return 1
        d = depth_stats(args.x, args.y)
        cl, ct = clearance_m(args.x, args.y)
        print(f"cross-section at ({args.x}, {args.y})")
        print(f"  width      {r['width_m']:.2f} m at azimuth {r['azimuth_deg']} deg"
              f"{'  (CLIPPED)' if r.get('clipped') else ''}")
        print(f"  depth      p50 {d['p50_m']:.3f} m, mean {d['mean_m']:.3f} m, "
              f"max {d['max_m']:.3f} m  (over {d['n']:,} wet cells within 40 m)")
        print(f"  surface    {d['surface_m']:.3f} m")
        print(f"  clearance  {cl:,.1f} m to unbaked {ct}  "
              f"{'OK' if cl >= CLEAR_M else 'TOO CLOSE -- DO NOT SHOOT'}")
        return 0

    if args.cmd == "dry":
        # THE DRIEST LEGAL COLUMN, AND THE BUG THIS COMMAND EXISTS BECAUSE OF.
        #
        # The first version of this search ran the distance transform PER TILE
        # and reported the nearest wet cell as 4,684 m for a column that sits
        # 359 m from its own tile's edge, with a wet neighbour tile just past
        # it. The true answer was 2,700 m. A per-tile transform cannot see
        # across a tile boundary, and every interesting column in a four-tile
        # set is near a boundary -- so the error was not a corner case, it was
        # the normal case. It was caught only because the ribbon actor built 19
        # reaches at a pose whose stated nearest water was beyond its 4,000 m
        # scan radius, i.e. by two instruments disagreeing.
        #
        # So: mosaic every baked tile into ONE mask and transform that. Missing
        # tiles are left dry, which is sound only because a column is rejected
        # below unless its clearance exceeds the radius being claimed -- an
        # unbaked tile is then further away than the answer, and cannot change
        # it.
        from scipy import ndimage
        baked = baked_tiles()
        txs = sorted({t[0] for t in baked}); tys = sorted({t[1] for t in baked})
        D = 8                                    # 8 cells = 15 m, ample here
        th, tw = FINE_TILE_PX // D, FINE_TILE_PX // D
        mos = np.zeros((len(tys) * th, len(txs) * tw), bool)
        for tx, ty in baked:
            _load(tx, ty)
            wet = ~np.isnan(_W[(tx, ty)])
            # max-pool: a block is wet if ANY cell in it is, so the transform
            # can only UNDER-state the distance to water. Erring that way is
            # the safe direction for a "there is no water near here" claim.
            blk = wet.reshape(th, D, tw, D).any(axis=(1, 3))
            i, j = tys.index(ty), txs.index(tx)
            mos[i * th:(i + 1) * th, j * tw:(j + 1) * tw] = blk
        dist = ndimage.distance_transform_edt(~mos) * PIXEL_M * D
        print(f"driest legal columns (mosaic {mos.shape}, {PIXEL_M*D:.1f} m/cell, "
              f"clearance >= {CLEAR_M:.0f} m)")
        print(f"  {'dry_m':>8} {'clear_m':>9} {'x_m':>12} {'y_m':>12}  tile      "
              f"{'row':>5} {'col':>5} {'ground_m':>9}")
        out = []
        for i in range(0, mos.shape[0], 8):
            for j in range(0, mos.shape[1], 8):
                ty = tys[i // th]; tx = txs[j // tw]
                if (tx, ty) not in set(baked):
                    continue
                row, col = (i % th) * D, (j % tw) * D
                x, y = cell_to_world(tx, ty, row, col)
                cl, _ = clearance_m(x, y)
                # Never claim a dry radius larger than the clearance: past it
                # the mosaic simply has no data, and absence of data is not
                # absence of water.
                if cl < CLEAR_M:
                    continue
                out.append((min(float(dist[i, j]), cl), cl, x, y, tx, ty, row, col,
                            float(_G[(tx, ty)][row, col])))
        out.sort(reverse=True)
        for r in out[:args.top]:
            print(f"  {r[0]:>8.0f} {r[1]:>9,.0f} {r[2]:>12.1f} {r[3]:>12.1f}  "
                  f"({r[4]:>3},{r[5]:>3}) {r[6]:>5} {r[7]:>5} {r[8]:>9.1f}")
        return 0

    if args.cmd == "scan":
        # WIDE WATER, INSIDE THE MARGIN. The inscribed width from a Euclidean
        # distance transform is the cheap screen: 2*EDT is the diameter of the
        # largest disc that fits in the wet set at that cell, so it cannot be
        # inflated by a transect running ALONG the river the way a raw ray can.
        # Candidates that survive it are then measured with the real
        # azimuth-swept cross-section.
        from scipy import ndimage
        rows = []
        for tx, ty in baked_tiles():
            _load(tx, ty)
            wet = ~np.isnan(_W[(tx, ty)])
            if not wet.any():
                continue
            edt = ndimage.distance_transform_edt(wet)
            insc = 2.0 * edt * PIXEL_M
            cand = np.argwhere(insc >= args.min_width)
            if cand.size == 0:
                continue
            # thin to a coarse lattice so one wide pool does not fill the list
            seen = set()
            for row, col in cand:
                key = (row // 128, col // 128)
                if key in seen:
                    continue
                x, y = cell_to_world(tx, ty, int(row), int(col))
                cl, _ = clearance_m(x, y)
                if cl < CLEAR_M:
                    continue
                seen.add(key)
                rows.append((float(insc[row, col]), cl, x, y, tx, ty,
                             int(row), int(col)))
        rows.sort(reverse=True)
        print(f"{len(rows)} site(s): inscribed width >= {args.min_width} m AND "
              f"clearance >= {CLEAR_M:.0f} m")
        print(f"  {'insc_m':>8} {'clear_m':>9} {'x_m':>12} {'y_m':>12}  tile      "
              f"{'row':>5} {'col':>5}  {'xsec_m':>8} {'d_p50':>7} {'surf_m':>9}")
        for r in rows[:args.top]:
            xs = cross_section(r[2], r[3], 400.0)
            ds = depth_stats(r[2], r[3])
            print(f"  {r[0]:>8.1f} {r[1]:>9,.0f} {r[2]:>12.1f} {r[3]:>12.1f}  "
                  f"({r[4]:>3},{r[5]:>3}) {r[6]:>5} {r[7]:>5}  "
                  f"{xs.get('width_m',0):>8.2f} {ds.get('p50_m',0):>7.3f} "
                  f"{ds.get('surface_m',0):>9.2f}")
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
