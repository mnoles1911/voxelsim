#!/usr/bin/env python3
"""The acceptance report for a corridor's DRAWN WATER, off the .npz dumps.

    python tools/corridor_water_report.py --npz-dir D:/tmp/frag-npz \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6"

WHY OFF THE NPZ AND NOT THE CACHE. ``measure_corridor_fragmentation.py`` and
``corridor_coast_reach.py`` read shipped ``.vxtl`` tiles, which is right for
comparing two namespaces that both shipped. A tuning run bakes with
``--diagnostic``, which write-protects the fine tier precisely so it cannot
mutate the world -- so there is no ``.vxtl`` to read and the only output is the
npz dump. This is the same stitch, the same 8-connected sparse union-find
labeller (imported, not re-implemented) and the same coastline arithmetic,
against ``water_surface_m`` instead of the quantised ``water_cp``.

Those two masks are the same set: ``isfinite(water_surface_m)`` reproduced the
shipped wet counts on all four corridor tiles (5,354 / 9,623 / 5,914 / 6,456),
so a number here is comparable to one taken there. The npz is additionally
UNQUANTISED, which is what makes the width and depth distributions below
meaningful below the wire's 100 mm LSB.

WHAT IT ADDS OVER THE TWO EXISTING TOOLS
-----------------------------------------
``width``   the thing a wider network can quietly trade away. Two widths are
            reported and they answer different questions:

              * RASTER ribbon width -- for each wet pixel, the shortest
                contiguous wet run through it over the four axes (row, column,
                both diagonals), at that axis' own pitch. Exact for a straight
                ribbon aligned to any of the four, and it is the width the
                client will actually draw.
              * LAW width -- ``channel_width_m(Q)`` at the same pixels, which is
                what hydraulic geometry says the channel is. The pair is the
                honest statement: the raster cannot draw below its pitch, so
                reporting only the first would make every headwater look
                1.875 m wide by construction.

``head``    for the longest component: the elevation at its HIGH end and
            whether its low end is at the shoreline. "The longest reach" and
            "a river from the mountains to the sea" are different claims and
            the second is the one that was asked for.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from terrain_service import tile_codec as tc  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402
from terrain_service.bake import water as _water  # noqa: E402
from measure_corridor_fragmentation import label8, max_pairwise_m  # noqa: E402

FINE_PX = tc.TILE_SIZE * tc.FINE_SCALE   # 8192
PIXEL_M = 30.0 / tc.FINE_SCALE           # 1.875
DEC = tc.FINE_SCALE                      # 16, to the 30 m coarse pitch


# --------------------------------------------------------------------- width

def _runs_along(R, C, axis):
    """Contiguous-run length through every point, along one of four axes.

    ``axis`` is one of ``"row"`` (0,+1), ``"col"`` (+1,0), ``"diag"`` (+1,+1),
    ``"anti"`` (+1,-1). Returns an int array parallel to ``R``.

    Sparse by construction: the wet set is tens of thousands of pixels on a
    16384 x 24576 grid, so this sorts the points into lines and walks them
    rather than materialising a raster.
    """
    if axis == "row":
        line, along = R, C
    elif axis == "col":
        line, along = C, R
    elif axis == "diag":
        line, along = R - C, R
    elif axis == "anti":
        line, along = R + C, R
    else:
        raise ValueError(axis)
    order = np.lexsort((along, line))
    l_s, a_s = line[order], along[order]
    # A new run starts at a change of line, or a gap of more than one step.
    new = np.empty(l_s.size, bool)
    new[0] = True
    new[1:] = (l_s[1:] != l_s[:-1]) | (a_s[1:] != a_s[:-1] + 1)
    grp = np.cumsum(new) - 1
    lengths = np.bincount(grp)
    out = np.empty(R.size, np.int64)
    out[order] = lengths[grp]
    return out


def ribbon_width_m(R, C):
    """Narrowest contiguous wet cross-section through each pixel, in metres."""
    diag = PIXEL_M * np.sqrt(2.0)
    w = np.minimum(
        np.minimum(_runs_along(R, C, "row"), _runs_along(R, C, "col")) * PIXEL_M,
        np.minimum(_runs_along(R, C, "diag"), _runs_along(R, C, "anti")) * diag,
    )
    return w


# --------------------------------------------------------------------- stitch

def stitch(npz_dir: Path, tiles):
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    nc, nr = xs[-1] - x0 + 1, ys[-1] - y0 + 1
    H, W = nr * FINE_PX, nc * FINE_PX

    coarse = np.full((H // DEC, W // DEC), np.nan, np.float32)
    Rs, Cs, Ds, Es, Qs = [], [], [], [], []
    per_tile = {}
    for (x, y) in tiles:
        p = npz_dir / f"{x}_{y}.npz"
        if not p.exists():
            per_tile[(x, y)] = None
            continue
        z = np.load(p)
        elev = z["elevation_m"]
        ro, co = (y - y0) * FINE_PX, (x - x0) * FINE_PX
        coarse[ro // DEC:(ro + FINE_PX) // DEC,
               co // DEC:(co + FINE_PX) // DEC] = elev[::DEC, ::DEC]
        if "water_surface_m" not in z.files:
            per_tile[(x, y)] = {"wet_px": 0, "water_plane": False}
            del z
            continue
        ws = z["water_surface_m"]
        m = np.isfinite(ws)
        r, c = np.nonzero(m)
        d = (ws[m] - elev[m]).astype(np.float64)
        e = elev[m].astype(np.float64)
        q = (z["discharge_m3_yr"][m].astype(np.float64)
             if "discharge_m3_yr" in z.files else np.zeros(r.size))
        Rs.append(r.astype(np.int64) + ro)
        Cs.append(c.astype(np.int64) + co)
        Ds.append(d)
        Es.append(e)
        Qs.append(q)
        per_tile[(x, y)] = {
            "wet_px": int(m.sum()), "pct_wet": 100.0 * float(m.mean()),
            "water_plane": True,
            "depth_p50": float(np.median(d)) if d.size else 0.0,
            "depth_max": float(d.max()) if d.size else 0.0,
        }
        del z, elev, ws, m
    cat = (lambda a, dt: np.concatenate(a) if a else np.zeros(0, dt))
    return dict(H=H, W=W, x0=x0, y0=y0, coarse=coarse, per_tile=per_tile,
                R=cat(Rs, np.int64), C=cat(Cs, np.int64),
                D=cat(Ds, float), E=cat(Es, float), Q=cat(Qs, float))


def pct(a, q):
    return float(np.percentile(a, q)) if len(a) else 0.0


def report(st, label, spans_at=(500, 1000, 2000, 5000, 10000), top=5):
    R, C, D, E, Q = st["R"], st["C"], st["D"], st["E"], st["Q"]
    lines = [f"=== {label} ===",
             f"  wet pixels          {R.size:,}"]
    if R.size == 0:
        return "\n".join(lines), {}

    lab = label8(R, C, st["H"], st["W"])
    ncomp = int(lab.max()) + 1
    sizes = np.bincount(lab)
    spans = np.array([max_pairwise_m(R[lab == k], C[lab == k])
                      for k in range(ncomp)], float)

    lines.append(f"  components (8-conn) {ncomp:,}")
    lines.append(f"  size px   p50 {pct(sizes,50):.0f}  p90 {pct(sizes,90):.0f}"
                 f"  max {sizes.max():.0f}")
    lines.append(f"  span m    p50 {pct(spans,50):.0f}  p90 {pct(spans,90):.0f}"
                 f"  max {spans.max():.0f}")
    for thr in spans_at:
        sel = spans >= thr
        lines.append(f"    span >= {thr:>6} m : {int(sel.sum()):>6} components,"
                     f" {100.0 * sizes[sel].sum() / sizes.sum():6.2f}% of wet px")

    lines.append(f"  depth m   min {D.min():.3f}  p50 {pct(D,50):.3f}"
                 f"  p90 {pct(D,90):.3f}  max {D.max():.3f}")

    w_ras = ribbon_width_m(R, C)
    lines.append(f"  raster ribbon width m  min {w_ras.min():.2f}"
                 f"  p50 {pct(w_ras,50):.2f}  p90 {pct(w_ras,90):.2f}"
                 f"  p99 {pct(w_ras,99):.2f}  max {w_ras.max():.2f}")
    for thr in (1.9, 2.7, 3.8, 7.5):
        lines.append(f"    ribbon <= {thr:>4.1f} m : "
                     f"{100.0 * float((w_ras <= thr).mean()):5.2f}% of wet px")
    if Q.any():
        w_law = _water.channel_width_m(Q, bp.CONSTANTS.water_q_perennial_m3_yr)
        lines.append(f"  law width m (channel.h at Q)  min {w_law.min():.2f}"
                     f"  p50 {pct(w_law,50):.2f}  p90 {pct(w_law,90):.2f}"
                     f"  max {w_law.max():.2f}")

    # -- the coastline half, the way corridor_coast_reach.py asks it.
    from scipy import ndimage
    sea = np.nan_to_num(st["coarse"], nan=1e9) <= 0.0
    coast = {}
    if sea.any():
        dist_px = ndimage.distance_transform_edt(~sea)
        d_all_km = dist_px[R // DEC, C // DEC] * 30.0 / 1000.0
        lines.append(f"  sea cells (elev <= 0) at 30 m: {int(sea.sum()):,}"
                     f" ({100.0 * sea.mean():.2f}% of the corridor box)")
        lines.append(f"  wet px distance to sea km  min {d_all_km.min():.2f}"
                     f"  p50 {pct(d_all_km,50):.2f}  max {d_all_km.max():.2f}")
        near_km = np.array([float(dist_px[R[lab == k] // DEC,
                                          C[lab == k] // DEC].min()) * 0.03
                            for k in range(ncomp)], float)
        for reach in (1.0, 2.0):
            sel = near_km <= reach
            n = int(sel.sum())
            best = float(spans[sel].max()) if n else 0.0
            lines.append(f"  pieces within {reach:.0f} km of the sea: {n:,}"
                         f" ({int(sizes[sel].sum()):,} px); longest span"
                         f" {best:.0f} m")
            if reach == 1.0:
                coast = {"pieces_within_1km": n,
                         "longest_span_at_coast_m": best}
    else:
        lines.append("  NO SEA IN THE CORRIDOR BOX")

    # -- the number that matters, and where its ends are.
    order = np.argsort(-spans)
    lines.append("  longest components:")
    for k in order[:top]:
        m = lab == k
        e = E[m]
        hi, lo = float(e.max()), float(e.min())
        extra = ""
        if sea.any():
            dl = float(dist_px[R[m][int(np.argmin(e))] // DEC,
                               C[m][int(np.argmin(e))] // DEC]) * 0.03
            dh = float(dist_px[R[m][int(np.argmax(e))] // DEC,
                               C[m][int(np.argmax(e))] // DEC]) * 0.03
            extra = (f"  low end {dl:.2f} km from sea, high end {dh:.2f} km")
        lines.append(f"    span {spans[k]:8.0f} m  {int(sizes[k]):>7,} px"
                     f"  elev {lo:7.1f} -> {hi:7.1f} m"
                     f"  depth p50 {np.median(D[m]):.2f}"
                     f"  width p50 {np.median(w_ras[m]):.2f} m{extra}")

    lines.append("  per tile:")
    for k, v in sorted(st["per_tile"].items()):
        if v is None:
            lines.append(f"    {k}: MISSING")
        elif not v["water_plane"]:
            lines.append(f"    {k}: NO WATER PLANE")
        else:
            lines.append(f"    {k}: {v['wet_px']:>8,} px  {v['pct_wet']:.4f}%wet"
                         f"  depth p50 {v['depth_p50']:.2f}"
                         f" max {v['depth_max']:.2f}")

    summary = {
        "wet_px": int(R.size), "components": ncomp,
        "longest_span_m": float(spans.max()),
        "components_ge_2km": int((spans >= 2000).sum()),
        "depth_min_m": float(D.min()), "depth_p50_m": pct(D, 50),
        "depth_p90_m": pct(D, 90), "depth_max_m": float(D.max()),
        "ribbon_width_p50_m": pct(w_ras, 50),
        "ribbon_width_p90_m": pct(w_ras, 90),
        "ribbon_width_max_m": float(w_ras.max()),
        **coast,
    }
    return "\n".join(lines), summary


def decompose(arms, widths, tiles):
    """The 2x2 that apportions the change between its two halves, for free.

    A shipped wet mask is ``(Q >= q_drawable) & ~basin``, and the npz dump
    carries the UNQUANTISED ``Q``. So the four arms of

        {MFD, D8} x {2.0 px, 1.5 px}

    are three re-thresholdings of two discharge fields that are already on
    disk, not four corridors of bakes. Eight tiles of bake buys the full
    apportionment instead of one number.

    THE BASIN EXCLUSION IS NOT APPLIED HERE and the numbers are therefore the
    RAW cut, not the shipped mask. That is deliberate rather than a shortcut:
    a basin mask recovered from the old wet set cannot say whether a cell the
    LOWER cut newly admits is inside a basin, so applying it would be a guess
    dressed as a measurement. The diagnosis measured what it costs -- 6.2% of
    cells and 0 m of span -- and the headline arm below is the real shipped
    mask, so the raw cut is used only where all four arms are raw and the
    comparison is like for like.
    """
    out = []
    fields = {}
    lo_q = min(_water.q_drawable_m3_yr(PIXEL_M, w,
                                       bp.CONSTANTS.water_q_perennial_m3_yr)
               for w in widths)
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    H = (ys[-1] - y0 + 1) * FINE_PX
    W = (xs[-1] - x0 + 1) * FINE_PX
    for label, d in arms:
        Rs, Cs, Qs = [], [], []
        for (x, y) in tiles:
            p = Path(d) / f"{x}_{y}.npz"
            if not p.exists():
                continue
            z = np.load(p)
            q = z["discharge_m3_yr"]
            m = q >= lo_q
            r, c = np.nonzero(m)
            Rs.append(r.astype(np.int64) + (y - y0) * FINE_PX)
            Cs.append(c.astype(np.int64) + (x - x0) * FINE_PX)
            Qs.append(q[m].astype(np.float64))
            del z, q, m
        fields[label] = (np.concatenate(Rs), np.concatenate(Cs),
                         np.concatenate(Qs))

    out.append("=== apportionment: the raw q >= q_drawable cut, no basin "
               "exclusion ===")
    out.append(f"{'arm':<28} {'cut m3/yr':>11} {'cells':>9} {'comps':>8}"
               f" {'longest m':>10} {'>=2km':>6}")
    for label, _ in arms:
        R, C, Q = fields[label]
        for wpx in widths:
            qd = _water.q_drawable_m3_yr(
                PIXEL_M, wpx, bp.CONSTANTS.water_q_perennial_m3_yr)
            sel = Q >= qd
            r, c = R[sel], C[sel]
            if r.size == 0:
                out.append(f"{label} @ {wpx} px: empty")
                continue
            lab = label8(r, c, H, W)
            n = int(lab.max()) + 1
            spans = np.array([max_pairwise_m(r[lab == k], c[lab == k])
                              for k in range(n)], float)
            out.append(f"{label + ' @ ' + str(wpx) + ' px':<28} {qd:11.4e}"
                       f" {r.size:>9,} {n:>8,} {spans.max():>10.0f}"
                       f" {int((spans >= 2000).sum()):>6}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--npz-dir", action="append", required=True,
                    help="LABEL=dir, or a bare dir (repeatable)")
    ap.add_argument("--decompose", action="store_true",
                    help="also run the {arm} x {min_width_px} apportionment "
                         "over the same npz dirs, on the raw Q cut")
    ap.add_argument("--widths", default="2.0 1.5",
                    help="min_width_px values for --decompose")
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
    arms = []
    out, dumps = [], {}
    for spec in args.npz_dir:
        label, _, d = spec.partition("=")
        if not d:
            label, d = spec, spec
        arms.append((label, d))
        st = stitch(Path(d), tiles)
        text, summary = report(st, label)
        out.append(text)
        dumps[label] = summary
    if args.decompose:
        out.append(decompose(arms, [float(v) for v in args.widths.split()],
                             tiles))
    print("\n\n".join(out))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(dumps, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
