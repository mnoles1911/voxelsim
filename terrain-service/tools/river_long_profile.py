#!/usr/bin/env python3
"""The LONGITUDINAL PROFILE of one composed reach, head to mouth, in order.

    # stage 1: compose the drawn water (river plane U lake sheets), sparse
    python tools/river_long_profile.py compose \
        --npz-dir D:/tmp/seamwidth-npz --basins out/basins.json \
        --tiles="-11,-4 -11,-5 -12,-5 -11,-6" --out D:/tmp/composed-bv12.npz

    # stage 2: walk the longest component from its head to its mouth
    python tools/river_long_profile.py walk \
        --composed D:/tmp/composed-bv12.npz --interval 500

WHY THIS EXISTS, AND WHY IT IS NOT corridor_water_report.py
-----------------------------------------------------------
Every existing corridor instrument reports a DISTRIBUTION -- p50 width, max
depth, the span of the longest piece. A distribution cannot answer "where does
the river grow and where does it narrow", because it has thrown the ordering
away. This tool keeps the ordering: it extracts one component's thalweg as a
PATH and reports width, depth and surface elevation as functions of distance
along that path.

THREE THINGS IT DOES DIFFERENTLY, EACH BECAUSE THE NAIVE VERSION IS WRONG.

1. IT COMPOSES THE WAY THE CLIENT DRAWS. `CompositeWaterSampler` unions the
   river plane with the lake sheets `lakes.h:lakeExtentFill` reconstructs from
   the basin registry. The bake writes registered-basin cells DRY on purpose,
   so the river plane alone is a different, shorter network -- on this corridor
   the raster-only longest piece stops 0.79 km short of the shoreline while the
   composed one runs out onto the seafloor. Composing is not a refinement here;
   it decides which reach is "the river".

   The lake fill is `scipy.ndimage.label` on {elev <= surfaceMm} clipped to the
   basin bbox, taking the component containing the seed -- which is what
   lakes.h's stack fill computes, and what basins.py:lake_extent_mask computes.
   A bare threshold would flood every hillside below the water level.

2. WIDTH IS MEASURED PERPENDICULAR TO THE LOCAL FLOW DIRECTION, not along a
   raster row and not as the four-axis minimum. `corridor_water_report.py`'s
   ribbon width is the right acceptance statistic -- it is what the client
   draws, minimised over the four axes so a diagonal is not inflated by sqrt2
   -- but on a channel running at 30 degrees it quantises to whichever of the
   four axes happens to be closest, which is a +-sqrt2 error, and it cannot see
   an asymmetric section at all. Here the tangent is fitted over a +-24 px
   window of the extracted path, the normal is taken from it, and the wet run
   is walked outward from the centreline along that normal in half-pixel steps.
   The reported width is the true perpendicular chord.

3. DISTANCE IS ALONG THE CHANNEL, not end to end. The "20.3 km" in the
   seam-and-width record is `max_pairwise_m` -- the EUCLIDEAN span between the
   two furthest cells of the component, via the convex hull. A sinuous river is
   longer than its own span. Both are reported and neither is called the other.

THE HEAD AND THE MOUTH. The head is the wet cell with the highest WATER SURFACE
(not the highest ground: a wet cell's ground can sit metres below its own water
surface, and on a widened bank it does). The mouth is the cell at maximum
geodesic distance from the head, which on a trunk with tributaries is the one
that is actually downstream rather than merely far away -- checked against the
minimum water surface, which must be the same cell or the walk is reported as
suspect rather than quietly used.
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

FINE_PX = tc.TILE_SIZE * tc.FINE_SCALE   # 8192
PIXEL_M = 30.0 / tc.FINE_SCALE           # 1.875
DEC = tc.FINE_SCALE                      # 16, to the 30 m coarse pitch

SRC_RIVER = 0
SRC_LAKE = 1


# ------------------------------------------------------------------ stage 1

def lake_sheets(elev_m, basins, log=print):
    """Every registered basin's extent, as lakes.h reconstructs it.

    Returns (rows, cols, surface_m) in TILE-interior pixels. Basins are
    disjoint by construction (each is a depression component), but the union
    is taken through a dict keyed on the flat index anyway, so an overlap
    cannot double-count a cell -- it is reported instead.
    """
    from scipy import ndimage
    st8 = np.ones((3, 3), bool)
    H, W = elev_m.shape
    elev_mm = elev_m.astype(np.float64) * 1000.0

    seen: dict[int, float] = {}
    n_empty = 0
    overlaps = 0
    for b in basins:
        sx, sy = b["seed"]
        x0, y0, x1, y1 = b["bbox"]
        surf_mm = float(b["surface_mm"])
        if not (x0 <= sx <= x1 and y0 <= sy <= y1):
            n_empty += 1
            continue
        sub = elev_mm[y0:y1 + 1, x0:x1 + 1] <= surf_mm
        if not sub[sy - y0, sx - x0]:
            # lakes.h returns 0 here: the seed itself stands above the datum.
            n_empty += 1
            continue
        lab, _ = ndimage.label(sub, structure=st8)
        keep = lab == lab[sy - y0, sx - x0]
        r, c = np.nonzero(keep)
        if r.size == 0:
            n_empty += 1
            continue
        flat = (r + y0) * W + (c + x0)
        for f in flat.tolist():
            if f in seen:
                overlaps += 1
            seen[f] = surf_mm / 1000.0
    if n_empty:
        log(f"      {n_empty} basins returned an EMPTY extent")
    if overlaps:
        log(f"      {overlaps} cells claimed by more than one basin")
    if not seen:
        return (np.zeros(0, np.int64),) * 2 + (np.zeros(0, float),)
    flat = np.fromiter(seen.keys(), np.int64, len(seen))
    surf = np.fromiter(seen.values(), float, len(seen))
    return flat // W, flat % W, surf


def compose(npz_dir: Path, basins_by_tile, tiles, log=print):
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    nc, nr = xs[-1] - x0 + 1, ys[-1] - y0 + 1
    H, W = nr * FINE_PX, nc * FINE_PX

    coarse = np.full((H // DEC, W // DEC), np.nan, np.float32)
    Rs, Cs, WSs, ELs, Qs, SRCs = [], [], [], [], [], []
    for (x, y) in tiles:
        p = npz_dir / f"{x}_{y}.npz"
        if not p.exists():
            raise SystemExit(f"missing dump {p}")
        z = np.load(p)
        elev = z["elevation_m"]
        ro, co = (y - y0) * FINE_PX, (x - x0) * FINE_PX
        coarse[ro // DEC:(ro + FINE_PX) // DEC,
               co // DEC:(co + FINE_PX) // DEC] = elev[::DEC, ::DEC]

        ws = z["water_surface_m"]
        q_full = z["discharge_m3_yr"]
        m = np.isfinite(ws)
        r, c = np.nonzero(m)
        log(f"  {x},{y}: river plane {r.size:,} wet px "
            f"({100.0 * m.mean():.4f}%wet)")
        Rs.append(r.astype(np.int64) + ro)
        Cs.append(c.astype(np.int64) + co)
        WSs.append(ws[m].astype(np.float64))
        ELs.append(elev[m].astype(np.float64))
        Qs.append(q_full[m].astype(np.float64))
        SRCs.append(np.full(r.size, SRC_RIVER, np.uint8))

        bl = basins_by_tile.get(f"{x}_{y}", [])
        lr, lc, lsurf = lake_sheets(elev, bl, log=log)
        log(f"      {len(bl)} basins -> {lr.size:,} lake-sheet px")
        # The bake writes registered-basin cells dry, so these sets should be
        # disjoint. Measured, not assumed -- an overlap would mean the union
        # double-counts and every "lake share" below would be wrong.
        rl_key = (lr.astype(np.int64) << 20) | lc.astype(np.int64)
        rv_key = (r.astype(np.int64) << 20) | c.astype(np.int64)
        both = np.intersect1d(rl_key, rv_key, assume_unique=False)
        if both.size:
            log(f"      OVERLAP: {both.size:,} cells are BOTH river and lake")
            drop = np.isin(rl_key, both)
            lr, lc, lsurf = lr[~drop], lc[~drop], lsurf[~drop]
        Rs.append(lr + ro)
        Cs.append(lc + co)
        WSs.append(lsurf)
        ELs.append(elev[lr, lc].astype(np.float64))
        Qs.append(np.zeros(lr.size))
        SRCs.append(np.full(lr.size, SRC_LAKE, np.uint8))
        del z, elev, ws, q_full, m

    return dict(H=H, W=W, x0=x0, y0=y0, coarse=coarse,
                R=np.concatenate(Rs), C=np.concatenate(Cs),
                WS=np.concatenate(WSs), EL=np.concatenate(ELs),
                Q=np.concatenate(Qs), SRC=np.concatenate(SRCs))


# ------------------------------------------------------------------ stage 2

def pick_component(st, log=print):
    from measure_corridor_fragmentation import label8, max_pairwise_m
    R, C = st["R"], st["C"]
    lab = label8(R, C, int(st["H"]), int(st["W"]))
    n = int(lab.max()) + 1
    sizes = np.bincount(lab)
    log(f"  composed cells {R.size:,}   components {n:,}")
    spans = np.array([max_pairwise_m(R[lab == k], C[lab == k])
                      for k in range(n)], float)
    order = np.argsort(-spans)
    for k in order[:4]:
        m = lab == k
        share = 100.0 * float((st["SRC"][m] == SRC_LAKE).mean())
        log(f"    span {spans[k]:8.0f} m  {int(sizes[k]):>8,} px  "
            f"elev {st['EL'][m].min():8.1f} -> {st['EL'][m].max():7.1f} m  "
            f"lake share {share:5.1f}%")
    return lab, spans, int(order[0])


def _local_raster(R, C, SRC):
    r0, c0 = int(R.min()), int(C.min())
    h, w = int(R.max()) - r0 + 1, int(C.max()) - c0 + 1
    grid = np.zeros((h, w), bool)
    grid[R - r0, C - c0] = True
    # THE RIVER-ONLY MASK, and it is not a nicety. A perpendicular ray walked
    # over the COMPOSED mask measures the lake it runs into, so "the widest
    # section" on the composed mask is a lake's short axis wherever the reach
    # crosses a basin. Both are reported; neither is called the other.
    riv = np.zeros((h, w), bool)
    sel = SRC == SRC_RIVER
    riv[R[sel] - r0, C[sel] - c0] = True
    idx = np.full((h, w), -1, np.int64)
    idx[R - r0, C - c0] = np.arange(R.size)
    return grid, riv, idx, r0, c0


def geodesic(grid, start_rc, cost=None):
    """8-connected least-cost path from `start_rc`. Returns (dist, prev).

    `dist` is the accumulated COST, which equals metres only when `cost` is
    None. The geometric length of a returned path is always recomputed from
    the path itself, never read out of here.

    WHY A COST AND NOT PLAIN DISTANCE. After the width law the ribbon is 1-5
    pixels wide, so an unweighted shortest path hugs the INSIDE of every bend
    and spends long stretches on widened bank cells whose own discharge is a
    hillside trickle. Geometrically that is within two pixels of the channel
    and harmless; as a longitudinal profile it is not, because the Q, the bed
    elevation and the depth all get read off the flank instead of the thalweg.
    Passing a cost that is 1 on a centreline (or lake) cell and several times
    that on a widened one buys a path that stays on the channel and still
    crosses a widened gap where it has to.
    """
    import heapq
    h, w = grid.shape
    dist = np.full((h, w), np.inf)
    prev = np.full((h, w), -1, np.int64)
    sr, sc = start_rc
    dist[sr, sc] = 0.0
    pq = [(0.0, sr * w + sc)]
    d1, d2 = PIXEL_M, PIXEL_M * np.sqrt(2.0)
    nbr = ((-1, 0, d1), (1, 0, d1), (0, -1, d1), (0, 1, d1),
           (-1, -1, d2), (-1, 1, d2), (1, -1, d2), (1, 1, d2))
    unit = cost is None
    while pq:
        d, node = heapq.heappop(pq)
        r, c = divmod(node, w)
        if d > dist[r, c]:
            continue
        kr = 1.0 if unit else cost[r, c]
        for dr, dc, wt in nbr:
            nr, ncc = r + dr, c + dc
            if nr < 0 or nr >= h or ncc < 0 or ncc >= w or not grid[nr, ncc]:
                continue
            nd = d + wt * (1.0 if unit else 0.5 * (kr + cost[nr, ncc]))
            if nd < dist[nr, ncc]:
                dist[nr, ncc] = nd
                prev[nr, ncc] = node
                heapq.heappush(pq, (nd, nr * w + ncc))
    return dist, prev


def backtrack(prev, w, end_rc):
    r, c = end_rc
    out = [(r, c)]
    node = prev[r, c]
    while node >= 0:
        r, c = divmod(node, w)
        out.append((r, c))
        node = prev[r, c]
    out.reverse()
    return np.array(out, np.int64)


def _tangent(path, i, half_win):
    n = len(path)
    a = path[max(0, i - half_win)]
    b = path[min(n - 1, i + half_win)]
    ty, tx = float(b[0] - a[0]), float(b[1] - a[1])
    L = np.hypot(ty, tx)
    return None if L == 0.0 else (ty / L, tx / L)


def path_normal(path, i, half_win=24, short_win=5):
    """Unit normal to the local flow direction at path[i], and a STRAIGHTNESS.

    The tangent is the chord over +-`half_win` path samples (45 m each side at
    1.875 m/px), which smooths the raster's staircase without cutting a real
    bend.

    THE STRAIGHTNESS EXISTS BECAUSE A PERPENDICULAR CAN POINT DOWNSTREAM. At a
    tight bend the smoothed tangent and the local one disagree, and a ray cast
    on the smoothed normal can run ALONG the channel instead of across it and
    return a chord of tens of metres on a 4 m river. That is the single way
    this measurement can be spectacularly wrong, so it is measured rather than
    hoped away: `straight` is cos(angle between the +-9 m tangent and the
    +-45 m one), and every width headline below is quoted both raw and
    restricted to sections where it exceeds 0.90 (25 degrees).
    """
    long_t = _tangent(path, i, half_win)
    if long_t is None:
        return None, 0.0
    short_t = _tangent(path, i, short_win)
    straight = (0.0 if short_t is None
                else abs(long_t[0] * short_t[0] + long_t[1] * short_t[1]))
    ty, tx = long_t
    return (-tx, ty), straight


def chord(mask, path, i, normal, max_px=400):
    """Contiguous wet run through path[i] along `normal`, in metres.

    Half-pixel steps with nearest-neighbour sampling: the ray leaves the
    lattice, so a whole-pixel step can jump a one-pixel ribbon and report a
    2 px channel as 1 px.
    """
    if normal is None:
        return 0.0, 0.0, 0.0
    ny, nx = normal
    h, w = mask.shape
    r0, c0 = float(path[i][0]), float(path[i][1])
    if not mask[int(r0), int(c0)]:
        return 0.0, 0.0, 0.0
    runs = []
    for sgn in (+1.0, -1.0):
        t, last_ok = 0.0, 0.0
        while t < max_px:
            t += 0.5
            rr = int(round(r0 + sgn * ny * t))
            cc = int(round(c0 + sgn * nx * t))
            if rr < 0 or rr >= h or cc < 0 or cc >= w or not mask[rr, cc]:
                break
            last_ok = t
        runs.append(last_ok)
    plus, minus = runs[0], runs[1]
    # +1 px for the centre cell itself; the ray measures from cell CENTRES.
    # Returned in the SIGN CONVENTION of `normal`, not as "left"/"right":
    # getting those two the wrong way round silently samples outside the wet
    # run on one flank and stops short on the other, and the first version of
    # this file did exactly that.
    return (plus + minus + 1.0) * PIXEL_M, plus, minus


def section_stats(idx, path, i, WS, EL, SRC, normal, plus_px, minus_px):
    """Depth statistics over exactly the cells the chord covered.

    Deduplicated on the cell index: half-pixel stepping visits some cells
    twice, and a mean over visits is a mean weighted by how obliquely the ray
    crossed each pixel, which is not the cross-section mean.
    """
    if normal is None:
        return 0.0, 0.0, 0.0
    h, w = idx.shape
    ny, nx = normal
    r0, c0 = float(path[i][0]), float(path[i][1])
    seen = set()
    t = -minus_px
    while t <= plus_px + 1e-9:
        rr = int(round(r0 + ny * t))
        cc = int(round(c0 + nx * t))
        t += 0.5
        if 0 <= rr < h and 0 <= cc < w:
            j = idx[rr, cc]
            if j >= 0:
                seen.add(int(j))
    if not seen:
        return 0.0, 0.0, 0.0
    j = np.fromiter(seen, np.int64, len(seen))
    d = WS[j] - EL[j]
    return (float(d.max()), float(d.mean()),
            float((SRC[j] == SRC_LAKE).mean()))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("compose")
    c.add_argument("--npz-dir", required=True)
    c.add_argument("--basins", required=True)
    c.add_argument("--tiles", required=True)
    c.add_argument("--out", required=True)

    wlk = sub.add_parser("walk")
    wlk.add_argument("--composed", required=True)
    wlk.add_argument("--interval", type=float, default=500.0)
    wlk.add_argument("--rank", type=int, default=0,
                     help="0 = longest-span component, 1 = second, ...")
    wlk.add_argument("--mouth", choices=("lowest", "furthest"),
                     default="lowest")
    wlk.add_argument("--straightness", type=float, default=0.98,
                     help="minimum cos(angle) between the local and smoothed "
                          "tangents for a perpendicular section to count")
    wlk.add_argument("--flank-cost", type=float, default=6.0,
                     help="cost multiplier for a widened (non-centreline) "
                          "cell when routing the thalweg")
    wlk.add_argument("--csv-out", default=None)

    args = ap.parse_args()

    if args.cmd == "compose":
        tiles = [tuple(int(v) for v in p.split(",")) for p in args.tiles.split()]
        basins = json.loads(Path(args.basins).read_text())
        st = compose(Path(args.npz_dir), basins, tiles)
        np.savez_compressed(
            args.out, H=st["H"], W=st["W"], x0=st["x0"], y0=st["y0"],
            coarse=st["coarse"], R=st["R"], C=st["C"], WS=st["WS"],
            EL=st["EL"], Q=st["Q"], SRC=st["SRC"])
        print(f"wrote {args.out}: {st['R'].size:,} composed cells "
              f"({int((st['SRC'] == SRC_LAKE).sum()):,} lake, "
              f"{int((st['SRC'] == SRC_RIVER).sum()):,} river)")
        return 0

    st = dict(np.load(args.composed))
    return walk(st, args)


def walk(st, args) -> int:
    from measure_corridor_fragmentation import max_pairwise_m
    R, C, WS, EL, Q, SRC = (st["R"], st["C"], st["WS"], st["EL"],
                            st["Q"], st["SRC"])
    lab, spans, _ = pick_component(st)
    k = int(np.argsort(-spans)[args.rank])
    m = lab == k
    R, C, WS, EL, Q, SRC = R[m], C[m], WS[m], EL[m], Q[m], SRC[m]
    span = spans[k]
    print(f"\n=== component rank {args.rank}: euclidean span {span:,.0f} m, "
          f"{R.size:,} cells, {100.0 * float((SRC == SRC_LAKE).mean()):.1f}% "
          f"lake sheet ===")

    grid, rivgrid, idx, r0, c0 = _local_raster(R, C, SRC)
    print(f"  local raster {grid.shape[0]:,} x {grid.shape[1]:,} px")

    qd = _water.q_drawable_m3_yr(PIXEL_M,
                                 bp.CONSTANTS.water_min_width_px,
                                 bp.CONSTANTS.water_q_perennial_m3_yr)
    centre = (Q >= qd) | (SRC == SRC_LAKE)
    print(f"  q_drawable {qd:.4e} m3/yr; {int((Q >= qd).sum()):,} centreline "
          f"cells, {int((SRC == SRC_LAKE).sum()):,} lake, "
          f"{int((~centre).sum()):,} widened")
    cost = np.full(grid.shape, float(args.flank_cost))
    cost[R[centre] - r0, C[centre] - c0] = 1.0

    head = int(np.argmax(WS))
    print(f"  head: water surface {WS[head]:.1f} m, ground {EL[head]:.1f} m, "
          f"{'LAKE' if SRC[head] == SRC_LAKE else 'river'}")

    # THE PLAIN geodesic first, only to answer "is the furthest cell the
    # lowest one". On a trunk with a long tributary it is NOT, and taking the
    # furthest as the mouth walks up the tributary -- which is what the first
    # run of this tool did, and the profile came back rising for its whole
    # second half. The mouth is the lowest WATER SURFACE.
    dplain, _ = geodesic(grid, (int(R[head] - r0), int(C[head] - c0)))
    d_at = dplain[R - r0, C - c0]
    far = int(np.argmax(d_at))
    lowest = int(np.argmin(WS))
    print(f"  furthest cell: {d_at[far]:,.0f} m along, surface {WS[far]:.1f} m")
    print(f"  lowest   cell: surface {WS[lowest]:.1f} m, ground "
          f"{EL[lowest]:.1f} m, {d_at[lowest]:,.0f} m along")
    if far != lowest:
        print(f"  NOTE: the furthest cell is NOT the lowest. It sits "
              f"{WS[far] - WS[lowest]:,.0f} m ABOVE the outlet, so it is up a "
              f"tributary. The MOUTH is the lowest cell.")
    mouth = lowest if args.mouth == "lowest" else far

    _, prev = geodesic(grid, (int(R[head] - r0), int(C[head] - c0)), cost)
    path = backtrack(prev, grid.shape[1], (int(R[mouth] - r0),
                                           int(C[mouth] - c0)))
    pj = idx[path[:, 0], path[:, 1]]
    chain = np.concatenate([[0.0], np.cumsum(
        np.hypot(np.diff(path[:, 0]).astype(float),
                 np.diff(path[:, 1]).astype(float)) * PIXEL_M)])
    print(f"  thalweg path {len(path):,} cells, along-channel length "
          f"{chain[-1]:,.0f} m  (sinuosity {chain[-1] / span:.2f})")

    # ---- per-node section
    print("  measuring perpendicular sections ...")
    n_p = len(path)
    wid = np.zeros(n_p)       # composed chord: what the client draws
    wid_r = np.zeros(n_p)     # river-plane chord only
    dmax = np.zeros(n_p)
    dmean = np.zeros(n_p)
    lakef = np.zeros(n_p)
    strt = np.zeros(n_p)
    for i in range(n_p):
        nvec, strt[i] = path_normal(path, i)
        w_m, p_px, m_px = chord(grid, path, i, nvec)
        wid[i] = w_m
        wid_r[i] = chord(rivgrid, path, i, nvec)[0]
        dmax[i], dmean[i], lakef[i] = section_stats(
            idx, path, i, WS, EL, SRC, nvec, p_px, m_px)
    # THE SECOND, INDEPENDENT WIDTH, and the reason there are two.
    # `ribbon_width_m` (corridor_water_report.py, imported not re-implemented)
    # is the shortest contiguous wet run through a cell over the four raster
    # axes. It is BEND-IMMUNE -- it never looks along the channel -- but it
    # quantises the direction to the nearest of four, so it is coarse. The
    # perpendicular chord is precise but fails at a bend. Requiring the two to
    # AGREE is what makes a single "widest point" defensible: a bend artefact
    # inflates the perpendicular and leaves the four-axis measure alone.
    from corridor_water_report import ribbon_width_m
    rib_all = ribbon_width_m(R, C)
    rib_r = np.zeros(R.size)
    sel_r = SRC == SRC_RIVER
    rib_r[sel_r] = ribbon_width_m(R[sel_r], C[sel_r])
    rib = rib_all[pj]
    ribr = rib_r[pj]
    ok = (strt >= float(args.straightness)) & (wid <= rib + 2.0 * PIXEL_M)
    okr = (strt >= float(args.straightness)) & (wid_r <= ribr + 2.0 * PIXEL_M)

    surf = WS[pj]
    grnd = EL[pj]
    qq = Q[pj]
    src = SRC[pj]
    # THE LAW WIDTH, because the drawn one is quantised to 1.875 m and the
    # question is how the river GROWS. `channel_width_m(Q)` is continuous, and
    # it is the quantity the bake's widening is trying to realise -- the drawn
    # ribbon agrees with it to within one pixel on 93.1% of this corridor
    # (docs/measurements/river-seam-and-width-2026-08-04.txt). Between two
    # sections both drawn 7.50 m wide, this is the only thing that can say
    # which is the bigger river.
    w_law = np.where(qq > 0.0,
                     _water.channel_width_m(np.maximum(qq, 1.0),
                                            bp.CONSTANTS.water_q_perennial_m3_yr),
                     np.nan)

    print("\n--- THE FOUR NUMBERS ---")
    # DEPTH BY CLASS, because "the deepest point" has three different honest
    # answers and quoting the largest one alone would be a lie about the
    # channel. A LAKE cell's depth is a lake's depth. A WIDENED river cell is
    # within two pixels of the centreline and its depth is the flat water
    # surface standing over ground that drops away -- a cut bank, which is the
    # class the seam-and-width record flagged (91 cells corridor-wide deeper
    # than 5 m). Only the CENTRELINE number is the river's own channel depth.
    dep_all = WS - EL
    centre = Q >= qd
    riv = SRC == SRC_RIVER
    classes = (("river centreline", riv & centre),
               ("river widened/bank", riv & ~centre),
               ("lake sheet", SRC == SRC_LAKE))
    print(f"  depth, metres, by class over the {R.size:,} cells of the reach:")
    for nm, s in classes:
        if not s.any():
            continue
        d = dep_all[s]
        print(f"    {nm:<20} n={int(s.sum()):>7,}  min {d.min():.3f}  "
              f"p50 {np.median(d):.3f}  p90 {np.percentile(d, 90):.3f}  "
              f"p99 {np.percentile(d, 99):.3f}  max {d.max():.3f}")
    print(f"    cells deeper than 2 m: {int((dep_all > 2).sum()):,}; "
          f"deeper than 5 m: {int((dep_all > 5).sum()):,}")
    for nm, s in (("ANYWHERE on the reach", np.ones(R.size, bool)),) + classes:
        if not s.any():
            continue
        i = int(np.argmax(np.where(s, dep_all, -1.0)))
        print(f"  deepest {nm:<22}: {dep_all[i]:.3f} m  (bed {EL[i]:8.1f} m, "
              f"surface {WS[i]:8.1f} m, Q {Q[i]:.3e}, "
              f"{d_at[i]:,.0f} m along the network)")
    print(f"  sections passing the guard (straightness >= "
          f"{float(args.straightness):.2f} AND perpendicular within 2 px of "
          f"the four-axis ribbon): composed {int(ok.sum()):,}, river "
          f"{int(okr.sum()):,}, of {n_p:,}")
    for tag, arr, g, rb in (
            ("COMPOSED (river U lake, what the client draws)", wid, ok, rib),
            ("RIVER-PLANE only", wid_r, okr, ribr)):
        i_raw = int(np.argmax(arr))
        i_ok = int(np.argmax(np.where(g, arr, -1)))
        print(f"  widest {tag}:")
        print(f"      raw      {arr[i_raw]:7.2f} m at {chain[i_raw]:>7,.0f} m "
              f"(straightness {strt[i_raw]:.2f}, four-axis {rb[i_raw]:.2f} m, "
              f"{100.0 * lakef[i_raw]:.0f}% lake in chord)")
        print(f"      GUARDED  {arr[i_ok]:7.2f} m at {chain[i_ok]:>7,.0f} m "
              f"(straightness {strt[i_ok]:.2f}, four-axis {rb[i_ok]:.2f} m, "
              f"{100.0 * lakef[i_ok]:.0f}% lake in chord)")
        print(f"      guarded p50 {np.median(arr[g]):.2f}  p90 "
              f"{np.percentile(arr[g], 90):.2f}  p99 "
              f"{np.percentile(arr[g], 99):.2f}")
    print(f"  four-axis ribbon width over ALL {R.size:,} reach cells: "
          f"composed p50 {np.median(rib_all):.2f} p90 "
          f"{np.percentile(rib_all, 90):.2f} max {rib_all.max():.2f} m; "
          f"river p50 {np.median(rib_r[sel_r]):.2f} p90 "
          f"{np.percentile(rib_r[sel_r], 90):.2f} max {rib_r.max():.2f} m")
    clean = okr & (lakef < 1e-9)
    if clean.any():
        iwc = int(np.argmax(np.where(clean, wid_r, -1)))
        print(f"  widest guarded RIVER section with NO lake sheet in the "
              f"chord: {wid_r[iwc]:.2f} m at {chain[iwc]:,.0f} m along")
    print(f"  descent, water surface: {surf[0]:.1f} -> {surf[-1]:.1f} m "
          f"= {surf[0] - surf[-1]:.1f} m")
    print(f"  descent, ground at the thalweg: {grnd[0]:.1f} -> "
          f"{grnd[-1]:.1f} m = {grnd[0] - grnd[-1]:.1f} m")
    print(f"  mean gradient {1000.0 * (surf[0] - surf[-1]) / chain[-1]:.2f} m/km")

    # ---- monotonicity: the surface must never rise downstream
    #
    # READ THIS BEFORE CALLING A RISE A DEFECT. The bake's guarantee is that
    # the surface descends along the D8 RECEIVER CHAIN. This path is a
    # least-cost route through the drawn mask, which is not the same object: at
    # a confluence, on a braid, or where the ribbon is wide enough to hold two
    # threads, the route can step SIDEWAYS from one thread onto a neighbouring
    # one that stands slightly higher. So the test that means something is
    # whether the profile is monotone at a scale larger than the ribbon --
    # a single-step rise inside a 4 px ribbon is a lateral hop, a rise that
    # survives 100 m of smoothing is a real reversal.
    rise = np.diff(surf)
    up = rise > 1e-6
    print("\n--- MONOTONICITY (the surface must not rise downstream) ---")
    print(f"  path steps {len(rise):,}; steps that RISE: {int(up.sum()):,} "
          f"({100.0 * float(up.mean()):.2f}%)")
    if up.any():
        print(f"  largest single rise {rise[up].max() * 1000:.1f} mm; "
              f"total rise {rise[up].sum():.2f} m against "
              f"{-rise[~up].sum():.1f} m of fall")
        onc = centre[pj]
        print(f"  of the rising steps, {100.0 * float(onc[:-1][up].mean()):.1f}% "
              f"start on a CENTRELINE cell "
              f"({100.0 * float(onc.mean()):.1f}% of all path cells are)")
    for win_m in (25.0, 100.0, 250.0):
        step = max(1, int(win_m / PIXEL_M))
        s = surf[::step]
        d = np.diff(s)
        n_up = int((d > 1e-6).sum())
        print(f"  resampled every {win_m:>5.0f} m ({len(s):,} samples): "
              f"{n_up:,} rises, largest {d.max() * 1000 if d.size else 0:.0f} mm,"
              f" net rise {d[d > 0].sum() if n_up else 0.0:.2f} m")
    exc = surf + np.maximum.accumulate(-surf)
    ie = int(np.argmax(exc))
    print(f"  largest excursion ABOVE the running minimum: {exc[ie]:.2f} m, "
          f"at {chain[ie]:,.0f} m along (surface {surf[ie]:.2f} m against a "
          f"running minimum of {surf[ie] - exc[ie]:.2f} m)")
    big = np.flatnonzero(exc > 0.5)
    if big.size:
        # Contiguous excursions, so one bump is reported once.
        brk = np.flatnonzero(np.diff(big) > 1)
        starts = np.concatenate([[0], brk + 1])
        ends = np.concatenate([brk, [big.size - 1]])
        print(f"  excursions above 0.5 m: {len(starts)} stretches "
              f"covering {100.0 * big.size / n_p:.2f}% of the path")
        for a, b2 in list(zip(starts, ends))[:6]:
            print(f"    {chain[big[a]]:,.0f}-{chain[big[b2]]:,.0f} m, "
                  f"peak {exc[big[a]:big[b2]+1].max():.2f} m")

    # ---- the traverse
    print(f"\n--- TRAVERSE, every {args.interval:.0f} m ---")
    hdr = (f"{'km':>7} {'surf m':>8} {'bed m':>8} {'wDrawn':>7} "
           f"{'wLaw m':>7} {'w p90':>7} {'depth m':>8} {'dmax m':>7} "
           f"{'Q m3/yr':>10} {'lake%':>6} {'m/km':>7}")
    print(hdr)
    edges = np.arange(0.0, chain[-1] + args.interval, args.interval)
    bins = np.clip(np.searchsorted(edges, chain, "right") - 1, 0, len(edges) - 2)
    for b in range(len(edges) - 1):
        s = bins == b
        if not s.any():
            continue
        i_lo = int(np.flatnonzero(s)[0])
        i_hi = int(np.flatnonzero(s)[-1])
        grad = ((surf[i_lo] - surf[i_hi]) / max(chain[i_hi] - chain[i_lo], 1e-9)
                * 1000.0)
        sw = s & okr if (s & okr).any() else s   # widths from guarded sections
        row = dict(
            km=edges[b] / 1000.0, surf=surf[i_lo], grnd=grnd[i_lo],
            wriv=float(np.median(wid_r[sw])),
            wlaw=float(np.nanmedian(w_law[s])) if np.isfinite(w_law[s]).any()
            else float("nan"),
            width=float(np.median(wid[sw])),
            w90=float(np.percentile(wid[sw], 90)),
            depth=float(np.median(dmean[s])), dmax=float(dmax[s].max()),
            q=float(np.median(qq[s])),
            lake=100.0 * float(lakef[s].mean()), grad=grad)
        print(f"{row['km']:7.2f} {row['surf']:8.1f} {row['grnd']:8.1f} "
              f"{row['wriv']:7.2f} {row['wlaw']:7.2f} {row['w90']:7.2f} "
              f"{row['depth']:8.3f} {row['dmax']:7.2f} {row['q']:10.3e} "
              f"{row['lake']:6.1f} {row['grad']:7.2f}")

    # ---- steepest and gentlest kilometre
    print("\n--- GRADIENT BY KILOMETRE ---")
    kms = []
    for s_km in np.arange(0.0, chain[-1] - 1000.0 + 1.0, 1000.0):
        i_lo = int(np.searchsorted(chain, s_km))
        i_hi = int(np.searchsorted(chain, s_km + 1000.0)) - 1
        if i_hi <= i_lo:
            continue
        drop = surf[i_lo] - surf[i_hi]
        kms.append((drop / (chain[i_hi] - chain[i_lo]) * 1000.0,
                    s_km, drop, float(np.median(wid[i_lo:i_hi + 1]))))
    kms.sort()
    for tag, sel in (("gentlest", kms[:3]), ("steepest", kms[-3:][::-1])):
        for g, s_km, drop, w in sel:
            print(f"  {tag:>9}: km {s_km/1000:5.1f}-{s_km/1000+1:4.1f}  "
                  f"{g:7.2f} m/km  ({drop:6.1f} m drop, width p50 {w:.2f} m)")

    # ---- where does it cross sea level
    below = np.flatnonzero(grnd <= 0.0)
    if below.size:
        i0 = int(below[0])
        print(f"\n--- THE SHORELINE ---")
        print(f"  bed first reaches 0 m at {chain[i0]:,.0f} m along "
              f"({100.0 * chain[i0] / chain[-1]:.0f}% of the reach); "
              f"{chain[-1] - chain[i0]:,.0f} m of the walk is SUBMARINE, "
              f"descending a further {surf[i0] - surf[-1]:,.0f} m")

    # ---- does width grow downstream?
    print("\n--- DOES WIDTH GROW DOWNSTREAM? ---")
    from scipy import stats as sps
    lw = np.isfinite(w_law)
    print(f"  Spearman(LAW width,      distance along) = "
          f"{sps.spearmanr(chain[lw], w_law[lw]).statistic:+.3f} "
          f"(n={int(lw.sum()):,})")
    print(f"  law width along the reach: head {np.nanmin(w_law[:200]):.2f} m, "
          f"mouth {np.nanmax(w_law[-200:]):.2f} m, "
          f"max {np.nanmax(w_law):.2f} m")
    print(f"  Spearman(river width,    distance along) = "
          f"{sps.spearmanr(chain, wid_r).statistic:+.3f}")
    print(f"  Spearman(composed width, distance along) = "
          f"{sps.spearmanr(chain, wid).statistic:+.3f}")
    q_ok = qq > 0
    if q_ok.sum() > 10:
        print(f"  Spearman(Q,              distance along) = "
              f"{sps.spearmanr(chain[q_ok], qq[q_ok]).statistic:+.3f} "
              f"(river-plane nodes only, n={int(q_ok.sum()):,})")
        print(f"  Spearman(river width, Q)                 = "
              f"{sps.spearmanr(wid_r[q_ok], qq[q_ok]).statistic:+.3f}")
    dq = np.diff(qq)
    fall = np.flatnonzero(dq < -1e-6)
    print(f"  Q falls downstream on {fall.size:,} of {dq.size:,} steps:")
    tile_r = (R[pj] // FINE_PX) + int(st["y0"])
    tile_c = (C[pj] // FINE_PX) + int(st["x0"])
    for i in fall[:10]:
        seam = ("  SEAM" if (tile_r[i], tile_c[i]) != (tile_r[i + 1],
                                                       tile_c[i + 1])
                else "")
        near = min(int(R[pj][i]) % FINE_PX, FINE_PX - 1 - int(R[pj][i]) % FINE_PX,
                   int(C[pj][i]) % FINE_PX, FINE_PX - 1 - int(C[pj][i]) % FINE_PX)
        print(f"    at {chain[i]:,.0f} m: {qq[i]:.4e} -> {qq[i+1]:.4e} "
              f"({'lake' if src[i] == SRC_LAKE else 'river'} -> "
              f"{'lake' if src[i+1] == SRC_LAKE else 'river'}) "
              f"tile ({tile_c[i]},{tile_r[i]}), {near * PIXEL_M:,.0f} m from "
              f"the nearest tile edge{seam}")
    # monotone in 1 km blocks?
    for tag, arr in (("river", wid_r), ("composed", wid)):
        blk = []
        for s_km in np.arange(0.0, chain[-1], 1000.0):
            s = (chain >= s_km) & (chain < s_km + 1000.0)
            sw = s & okr if (s & okr).any() else s
            if s.sum() > 5:
                blk.append((s_km / 1000.0, float(np.median(arr[sw])),
                            float(np.median(qq[s]))))
        falls = [(blk[i][0], blk[i][1], blk[i + 1][1], blk[i][2], blk[i + 1][2])
                 for i in range(len(blk) - 1) if blk[i + 1][1] < blk[i][1] - 1e-9]
        print(f"  km-block median {tag} width falls at {len(falls)} of "
              f"{len(blk) - 1} steps:")
        for a, w1, w2, q1, q2 in falls:
            print(f"    km {a:4.1f} -> {a+1:4.1f}:  {w1:5.2f} -> {w2:5.2f} m"
                  f"   (Q {q1:.3e} -> {q2:.3e})")

    if args.csv_out:
        import csv
        with open(args.csv_out, "w", newline="") as fh:
            wcsv = csv.writer(fh)
            wcsv.writerow(["chain_m", "row_px", "col_px", "tile_x", "tile_y",
                           "surface_m", "bed_m", "depth_m",
                           "width_river_m", "width_composed_m", "width_law_m",
                           "straightness", "depth_max_section_m", "q_m3_yr",
                           "lake_frac", "src"])
            for i in range(len(path)):
                wcsv.writerow([f"{chain[i]:.3f}", int(R[pj][i]), int(C[pj][i]),
                               int(tile_c[i]), int(tile_r[i]),
                               f"{surf[i]:.3f}",
                               f"{grnd[i]:.3f}", f"{surf[i]-grnd[i]:.4f}",
                               f"{wid_r[i]:.3f}", f"{wid[i]:.3f}",
                               f"{w_law[i]:.4f}", f"{strt[i]:.3f}",
                               f"{dmax[i]:.4f}", f"{qq[i]:.6e}",
                               f"{lakef[i]:.4f}", int(src[i])])
        print(f"\nwrote {args.csv_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
