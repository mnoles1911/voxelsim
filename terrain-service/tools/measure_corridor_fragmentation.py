#!/usr/bin/env python3
"""Label the WET MASK of a stitched multi-tile corridor and describe its pieces.

WHY STITCH BEFORE LABELLING. A river that leaves one tile and enters the next
is one river. Labelling per tile and summing counts it twice and caps its
length at the tile edge, which is exactly the quantity under test here -- so
the four tiles are pasted into one grid on their true (tile_x, tile_y) lattice
and labelled once. Tiles absent from the corridor read as dry, which is
conservative: it can only BREAK a component, never join one.

WHY SPARSE UNION-FIND rather than ``ndimage.label``. The stitched grid is
16384 x 24576 px; an int32 label image of it is 1.6 GB, while the wet set is a
few tens of thousands of pixels. Union-find over the wet cells alone is the
same 8-connected labelling at 1/10000 the memory.

Geometry is read off the codec, not assumed: a fine tile is
``TILE_SIZE * FINE_SCALE`` px at ``30.0 / FINE_SCALE`` m, and
``pipeline.BakeGeometry.padded_origin_cells`` fixes the axis order --
row index tracks tile_y, column index tracks tile_x, both increasing.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

FINE_PX = tc.TILE_SIZE * tc.FINE_SCALE          # 8192
PIXEL_M = 30.0 / tc.FINE_SCALE                  # 1.875


def area_network(ns_dir: Path, seed_hex: str, tiles, log2_thr: int):
    """The same stitch and the same labeller, on ACCUMULATED AREA instead.

    The flow plane's low 5 bits are log2 of catchment area in m^2. Area is
    monotone downstream, so a threshold on it CANNOT fragment a river -- which
    makes this the control the wet mask is fragmented against. Reported
    alongside is the share of above-threshold cells that are actually wet: the
    gap between the two networks is the whole of the fragmentation.
    """
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    H = (ys[-1] - y0 + 1) * FINE_PX
    W = (xs[-1] - x0 + 1) * FINE_PX
    ar, ac, wet_flag = [], [], []
    for (x, y) in tiles:
        t = load_tile(ns_dir, seed_hex, x, y)
        if t is None or t.flow is None:
            continue
        m = (np.asarray(t.flow) & tc.FLOW_LOG2_MASK) >= log2_thr
        r, c = np.nonzero(m)
        wetm = (np.asarray(t.water_cp) >= 0) if t.water_cp is not None \
            else np.zeros_like(m)
        wet_flag.append(wetm[m])
        ar.append(r.astype(np.int64) + (y - y0) * FINE_PX)
        ac.append(c.astype(np.int64) + (x - x0) * FINE_PX)
    if not ar:
        return None
    R, C = np.concatenate(ar), np.concatenate(ac)
    Wf = np.concatenate(wet_flag)
    lab = label8(R, C, H, W)
    n = int(lab.max()) + 1
    sizes = np.bincount(lab)
    order = np.argsort(-sizes)
    spans = []
    for k in order[:10]:
        m = lab == k
        spans.append((int(sizes[k]), max_pairwise_m(R[m], C[m])))
    return dict(cells=int(R.size), n_comp=n,
                largest_frac=float(sizes.max()) / float(R.size),
                top=spans, wet_share=float(Wf.mean()))


def load_tile(ns_dir: Path, seed_hex: str, x: int, y: int):
    p = ns_dir / seed_hex / "s16" / f"{x}_{y}.vxtl"
    if not p.exists():
        return None
    t = tc.decode_v2(p.read_bytes())
    return t


def stitch(ns_dir: Path, seed_hex: str, tiles):
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    ncol, nrow = xs[-1] - x0 + 1, ys[-1] - y0 + 1
    H, W = nrow * FINE_PX, ncol * FINE_PX

    wet_r: list[np.ndarray] = []
    wet_c: list[np.ndarray] = []
    depth_m: list[np.ndarray] = []
    elev_m: list[np.ndarray] = []
    per_tile = {}
    bake_vers = set()

    for (x, y) in tiles:
        t = load_tile(ns_dir, seed_hex, x, y)
        if t is None:
            per_tile[(x, y)] = None
            continue
        bake_vers.add(t.bake_ver)
        if t.water_cp is None:
            per_tile[(x, y)] = {"wet_px": 0, "water_plane": False,
                                "bake_ver": t.bake_ver}
            continue
        w = np.asarray(t.water_cp)
        m = w >= 0
        r, c = np.nonzero(m)
        d = w[m].astype(np.float64) * tc.WATER_DEPTH_LSB_MM / 1000.0
        # Elevation of the wet cells, absolute metres (control lattice; good
        # enough to say "this end is at sea level", not a surface reconstruction).
        e = (np.asarray(t.elevation_cp)[m].astype(np.float64)
             * tc.QUANT_MM[t.quant] + t.base_offset_mm) / 1000.0
        ro = (y - y0) * FINE_PX
        co = (x - x0) * FINE_PX
        wet_r.append(r.astype(np.int64) + ro)
        wet_c.append(c.astype(np.int64) + co)
        depth_m.append(d)
        elev_m.append(e)
        per_tile[(x, y)] = {
            "wet_px": int(m.sum()),
            "pct_wet": 100.0 * float(m.mean()),
            "water_plane": True,
            "bake_ver": t.bake_ver,
            "depth_p50": float(np.median(d)) if d.size else 0.0,
            "depth_max": float(d.max()) if d.size else 0.0,
        }

    if wet_r:
        R = np.concatenate(wet_r)
        C = np.concatenate(wet_c)
        D = np.concatenate(depth_m)
        E = np.concatenate(elev_m)
    else:
        R = C = np.zeros(0, np.int64)
        D = E = np.zeros(0, np.float64)
    return dict(H=H, W=W, x0=x0, y0=y0, R=R, C=C, D=D, E=E,
                per_tile=per_tile, bake_vers=sorted(bake_vers))


def label8(R, C, H, W):
    """8-connected labels over a sparse wet set. Returns int array of labels."""
    n = R.size
    if n == 0:
        return np.zeros(0, np.int64)
    key = R * W + C
    order = np.argsort(key, kind="stable")
    key_s = key[order]
    # index lookup: position of a key in key_s, or -1
    parent = np.arange(n, dtype=np.int64)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    # Only forward neighbours: (0,+1), (+1,-1), (+1,0), (+1,+1). Covers all 8
    # adjacencies exactly once.
    for dr, dc in ((0, 1), (1, -1), (1, 0), (1, 1)):
        nr = R[order] + dr
        nc = C[order] + dc
        ok = (nr >= 0) & (nr < H) & (nc >= 0) & (nc < W)
        nk = nr * W + nc
        pos = np.searchsorted(key_s, nk)
        pos_c = np.clip(pos, 0, n - 1)
        hit = ok & (key_s[pos_c] == nk)
        ia = np.flatnonzero(hit)
        ib = pos_c[hit]
        for a, b in zip(ia.tolist(), ib.tolist()):
            union(a, b)

    roots = np.array([find(i) for i in range(n)], dtype=np.int64)
    lab_sorted = np.empty(n, np.int64)
    uniq, inv = np.unique(roots, return_inverse=True)
    lab_sorted[:] = inv
    lab = np.empty(n, np.int64)
    lab[order] = lab_sorted
    return lab


def max_pairwise_m(r, c):
    """Exact max pairwise distance, in metres, via the convex hull."""
    pts = np.stack([c.astype(np.float64), r.astype(np.float64)], 1) * PIXEL_M
    if pts.shape[0] <= 2:
        if pts.shape[0] < 2:
            return 0.0
        return float(np.hypot(*(pts[1] - pts[0])))
    try:
        from scipy.spatial import ConvexHull
        h = ConvexHull(pts)
        p = pts[h.vertices]
    except Exception:
        p = pts
    if p.shape[0] > 4000:
        p = p[:: max(1, p.shape[0] // 4000)]
    d = np.hypot(p[:, 0][:, None] - p[:, 0][None, :],
                 p[:, 1][:, None] - p[:, 1][None, :])
    return float(d.max())


def analyse(st, name):
    R, C, D, E = st["R"], st["C"], st["D"], st["E"]
    lab = label8(R, C, st["H"], st["W"])
    ncomp = int(lab.max()) + 1 if lab.size else 0
    comps = []
    for k in range(ncomp):
        m = lab == k
        r, c = R[m], C[m]
        span = max_pairwise_m(r, c)
        comps.append(dict(
            k=k, size=int(m.sum()), span_m=span,
            bbox_r=(int(r.min()), int(r.max())),
            bbox_c=(int(c.min()), int(c.max())),
            elev_min=float(E[m].min()), elev_max=float(E[m].max()),
            depth_p50=float(np.median(D[m])), depth_max=float(D[m].max()),
        ))
    comps.sort(key=lambda d: -d["span_m"])
    return dict(name=name, wet_px=int(R.size), n_comp=ncomp, comps=comps,
                D=D, per_tile=st["per_tile"], bake_vers=st["bake_vers"],
                x0=st["x0"], y0=st["y0"])


def pct(a, q):
    return float(np.percentile(a, q)) if len(a) else 0.0


def report(a):
    sizes = np.array([c["size"] for c in a["comps"]], float)
    spans = np.array([c["span_m"] for c in a["comps"]], float)
    tot = sizes.sum() if sizes.size else 1.0
    lines = []
    lines.append(f"--- {a['name']}  (bake_ver {a['bake_vers']}) ---")
    lines.append(f"  total wet px       {a['wet_px']:,}")
    lines.append(f"  components         {a['n_comp']:,}")
    if not a["n_comp"]:
        return "\n".join(lines)
    lines.append(f"  size px  p50 {pct(sizes,50):.0f}  p90 {pct(sizes,90):.0f}"
                 f"  max {sizes.max():.0f}")
    lines.append(f"  span m   p50 {pct(spans,50):.0f}  p90 {pct(spans,90):.0f}"
                 f"  max {spans.max():.0f}")
    lines.append("  span >= X:  count   px-share")
    for thr in (200, 500, 1000, 2000, 5000):
        sel = spans >= thr
        lines.append(f"    {thr:>6} m   {int(sel.sum()):>6}   "
                     f"{100.0 * sizes[sel].sum() / tot:6.2f}%")
    d = a["D"]
    if d.size:
        lines.append(f"  depth m  min {d.min():.2f}  p50 {pct(d,50):.2f}  "
                     f"p90 {pct(d,90):.2f}  max {d.max():.2f}")
    lines.append("  largest by span:")
    for c in a["comps"][:5]:
        lines.append(f"    span {c['span_m']:8.0f} m  {c['size']:>7,} px  "
                     f"elev {c['elev_min']:7.1f}..{c['elev_max']:7.1f} m  "
                     f"depth p50 {c['depth_p50']:.2f} max {c['depth_max']:.2f}")
    lines.append("  per tile:")
    for k, v in sorted(a["per_tile"].items()):
        if v is None:
            lines.append(f"    {k}: MISSING")
        elif not v["water_plane"]:
            lines.append(f"    {k}: NO WATER PLANE (bv{v['bake_ver']})")
        else:
            lines.append(f"    {k}: {v['wet_px']:>7,} px  {v['pct_wet']:.4f}%wet"
                         f"  depth p50 {v['depth_p50']:.2f} max {v['depth_max']:.2f}"
                         f"  (bv{v['bake_ver']})")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache-dir", default="D:/voxelsim/tile-cache")
    ap.add_argument("--seed-hex", default="000000000135276f")
    ap.add_argument("--ns", action="append", required=True,
                    help="LABEL=namespace-dir-name, repeatable")
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--json-out", default=None)
    ap.add_argument("--area-log2", type=int, default=None,
                    help="also label the flow plane's accumulated-area network "
                         "at this log2(m^2) threshold, as the un-fragmentable "
                         "control")
    args = ap.parse_args()

    tiles = []
    for part in args.tiles.split():
        x, y = part.split(",")
        tiles.append((int(x), int(y)))

    out = []
    dumps = {}
    for spec in args.ns:
        label, ns = spec.split("=", 1)
        st = stitch(Path(args.cache_dir) / ns, args.seed_hex, tiles)
        a = analyse(st, label)
        out.append(report(a))
        if args.area_log2 is not None:
            an = area_network(Path(args.cache_dir) / ns, args.seed_hex,
                              tiles, args.area_log2)
            if an is not None:
                lines = [f"  AREA network, flow log2 >= {args.area_log2}:",
                         f"    cells {an['cells']:,}  components {an['n_comp']:,}"
                         f"  largest {100 * an['largest_frac']:.1f}%"
                         f"  wet share {100 * an['wet_share']:.1f}%",
                         "    top by size:  " + "  ".join(
                             f"{s:,}px/{sp:.0f}m" for s, sp in an["top"][:5])]
                out[-1] += "\n" + "\n".join(lines)
        dumps[label] = dict(
            wet_px=a["wet_px"], n_comp=a["n_comp"],
            comps=[{kk: vv for kk, vv in c.items()} for c in a["comps"][:50]],
            per_tile={f"{k[0]},{k[1]}": v for k, v in a["per_tile"].items()},
        )
    print("\n\n".join(out))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(dumps, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
