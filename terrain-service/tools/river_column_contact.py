#!/usr/bin/env python3
"""Does the drawn water column TOUCH the one below it? The staircase probe.

    python tools/river_column_contact.py \
        --cache-dir D:/voxelsim/tile-cache \
        --namespace terrain-diffusion-unlabeled-80b9ca451a23eae4-ba9c62170 \
        --seed-hex 000000000135276f --tiles="-11,-6" \
        --at=-161003.5,-85434.3

WHAT THIS MEASURES AND WHY THE EXISTING PROBES CANNOT
-----------------------------------------------------
``measure_corridor_fragmentation`` labels the wet mask IN PLAN, 8-CONNECTED. A
river that is one blob there can still render as a row of disconnected cubes,
because 8-connected plan adjacency is not what the renderer draws. Two things
it ignores are exactly the two ways the drawn water comes apart:

  * a DIAGONAL neighbour shares an EDGE, not a face. Two voxel columns touching
    at a corner are two objects with air between them;
  * two face-adjacent columns at different heights may not OVERLAP in z at all.

The owner's report is about both at once -- "several cubes of water placed in a
general direction but disconnected going down the slope ... obvious empty space
and air all around" -- so the statistic has to be voxel 6-connectivity, which
is what this computes.

THE RENDERING MODEL, taken from the client rather than assumed (``lakes.h``):

    RiverSampler::waterSurfaceMmAtVoxel -> floorDiv(vx * 100, 1875)  NEAREST
        fine pixel, so the surface is CONSTANT across all 18.75 voxels of a
        pixel: one flat slab per pixel, by construction.
    surfaceMm = reconstructedGroundMm(px, py) + depth * WATER_DEPTH_LSB_MM
    implicitWaterFill(vz, groundMm, surfaceMm) -> wet iff
        vz*100 >= groundMm  and  vz*100 < surfaceMm

so a pixel's water is the interval [g, g + d] and two face-adjacent pixels
share a voxel face iff those intervals overlap:

    contact(a, b)  <=>  d_lo > (g_hi - g_lo)

-- the lower cell's water must reach the upper cell's bed. That is the vertical
half of the defect, and its threshold is a GRADIENT: d / PIXEL_M.

WHICH GROUND. ``reconstructedGroundMm`` -- the cubic B-spline over the shipped
control lattice at the pixel centre, which at t=0 is the separable [1,4,1]/6
stencil (``carrier.h::evalCarrier`` via ``tilestore.h:1095``). NOT the raw
control point, which stands up to 5.6 m off the surface it interpolates. The
client's fill FLOOR is a third surface (the amplified one); this probe does not
model it, and since the amplifier only adds relief to the bed, every number here
is an UPPER BOUND on how connected the drawn water really is.

WHAT IS REPORTED, and what each number decides
----------------------------------------------
``face_components`` / ``plan8_components``
        the same wet cells labelled under voxel 6-connectivity and under the
        plan-only 8-connectivity the old probe used. The ratio IS the defect:
        plan-connected, voxel-disconnected.
``isolated_frac``
        wet cells with NO face-contact neighbour at all -- literally the cubes
        in the screenshot.
``break_diagonal`` / ``break_zgap``
        the two mechanisms, separated. A pair 8-adjacent but not 4-adjacent is
        a diagonal break (a 1 px wide reach on a diagonal run is nothing else);
        a pair 4-adjacent whose columns miss is a z-gap break.
``by_gradient``
        every statistic above, binned by the cell's own local bed gradient, so
        "the defect is a function of slope" is a measurement rather than a
        reading of one screenshot.
``profile``
        one reach walked downstream from ``--at``, bed and surface per step.
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
TILE_M = FINE_PX * PIXEL_M                      # 15360.0
VOXEL_M = 0.1

#: Bed-gradient bin edges (rise/run, dimensionless). The interesting scale is
#: set by the water itself: contact fails at gradient = depth / PIXEL_M, and the
#: measured p50 depth is ~0.43 m, so ~0.23 is where the vertical mechanism
#: switches on. The bins straddle it by an order of magnitude either side.
GRAD_BINS = [0.0, 0.02, 0.05, 0.10, 0.15, 0.20, 0.30, 0.50, 1e9]


def reconstructed_ground_m(t) -> np.ndarray:
    """The spline the client evaluates, in metres. float32, (size, size).

    ``evalCarrier(cp, 0, 0)`` is the cubic B-spline at a knot, whose weights are
    (1, 4, 1)/6 over cp[i-1..i+1] -- separable, so this is two 3-tap passes. The
    edge row/column keeps its own control point (the client reaches into the
    neighbouring tile; a one-pixel apron is not what this probe is about).
    """
    cp = (np.asarray(t.elevation_cp, np.float32) * np.float32(tc.QUANT_MM[t.quant])
          + np.float32(t.base_offset_mm)) / np.float32(1000.0)
    out = cp.copy()
    out[1:-1, :] = (cp[:-2, :] + 4.0 * cp[1:-1, :] + cp[2:, :]) / 6.0
    mid = out.copy()
    out[:, 1:-1] = (mid[:, :-2] + 4.0 * mid[:, 1:-1] + mid[:, 2:]) / 6.0
    return out


def load(ns_dir: Path, seed_hex: str, x: int, y: int):
    p = ns_dir / seed_hex / "s16" / f"{x}_{y}.vxtl"
    if not p.exists():
        return None
    return tc.decode_v2(p.read_bytes())


def _pct(a, ps=(5, 25, 50, 75, 90, 95, 99)):
    a = np.asarray(a)
    if a.size == 0:
        return {}
    v = np.percentile(a, ps)
    return {f"p{p}": round(float(x), 4) for p, x in zip(ps, v)}


class Sparse:
    """The wet set of one tile as sparse arrays, with neighbour lookup."""

    def __init__(self, g, wet, d):
        self.H, self.W = wet.shape
        self.r, self.c = np.nonzero(wet)
        self.n = self.r.size
        self.key = self.r.astype(np.int64) * self.W + self.c
        self.g = g[self.r, self.c].astype(np.float64)
        self.d = d[self.r, self.c].astype(np.float64)
        # Local bed gradient, central difference on the reconstructed ground.
        gy = np.zeros_like(g)
        gx = np.zeros_like(g)
        gy[1:-1, :] = (g[2:, :] - g[:-2, :]) / (2.0 * PIXEL_M)
        gx[:, 1:-1] = (g[:, 2:] - g[:, :-2]) / (2.0 * PIXEL_M)
        self.grad = np.hypot(gy[self.r, self.c], gx[self.r, self.c]).astype(np.float64)

    def neighbours(self, dr, dc):
        """(ia, ib) index pairs for wet cells offset by (dr, dc). Each pair once."""
        nk = (self.r.astype(np.int64) + dr) * self.W + (self.c.astype(np.int64) + dc)
        inb = ((self.r + dr >= 0) & (self.r + dr < self.H)
               & (self.c + dc >= 0) & (self.c + dc < self.W))
        pos = np.clip(np.searchsorted(self.key, nk), 0, max(self.n - 1, 0))
        hit = inb & (self.key[pos] == nk) if self.n else np.zeros(0, bool)
        return np.flatnonzero(hit), pos[hit]

    def contact(self, ia, ib):
        """Do these two columns share a voxel face? Vectorised."""
        ga, gb, da, db = self.g[ia], self.g[ib], self.d[ia], self.d[ib]
        up_a = ga >= gb
        g_hi = np.where(up_a, ga, gb)
        g_lo = np.where(up_a, gb, ga)
        d_lo = np.where(up_a, db, da)
        return d_lo > (g_hi - g_lo), (g_hi - g_lo), d_lo


def union_find(n, pairs):
    parent = np.arange(n, dtype=np.int64)

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for ia, ib in pairs:
        for a, b in zip(ia.tolist(), ib.tolist()):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[max(ra, rb)] = min(ra, rb)
    roots = np.array([find(i) for i in range(n)], np.int64)
    _, lab = np.unique(roots, return_inverse=True)
    return lab


def comp_summary(lab, prefix):
    if lab.size == 0:
        return {}
    sizes = np.bincount(lab)
    return {
        f"{prefix}_components": int(sizes.size),
        f"{prefix}_largest_frac": round(float(sizes.max()) / float(lab.size), 4),
        f"{prefix}_singletons": int((sizes == 1).sum()),
        f"{prefix}_cells_in_comps_under_4": int(sizes[sizes < 4].sum()),
    }


FACE4 = ((0, 1), (1, 0))
DIAG4 = ((1, 1), (1, -1))


def measure_tile(t):
    g = reconstructed_ground_m(t)
    w = np.asarray(t.water_cp)
    wet = w >= 0
    d = np.where(wet, w.astype(np.float32) * (tc.WATER_DEPTH_LSB_MM / 1000.0),
                 np.float32(0.0))
    out = {"bake_ver": int(t.bake_ver), "wet_cells": int(wet.sum())}
    if out["wet_cells"] == 0:
        return out, None
    s = Sparse(g, wet, d)
    out["depth_m"] = _pct(s.d)
    out["bed_gradient"] = _pct(s.grad)
    #: The gradient at which THIS cell's own water can no longer bridge one
    #: pixel of bed drop.  d / PIXEL_M, per cell.
    out["contact_break_gradient"] = _pct(s.d / PIXEL_M)

    face_pairs, all_pairs = [], []
    n_face_adj = n_face_ok = n_diag_adj = 0
    drops, dlos, ok_all = [], [], []
    has_face = np.zeros(s.n, bool)
    for dr, dc in FACE4:
        ia, ib = s.neighbours(dr, dc)
        ok, drop, d_lo = s.contact(ia, ib)
        n_face_adj += ia.size
        n_face_ok += int(ok.sum())
        drops.append(drop)
        dlos.append(d_lo)
        ok_all.append(ok)
        face_pairs.append((ia[ok], ib[ok]))
        all_pairs.append((ia, ib))
        has_face[ia[ok]] = True
        has_face[ib[ok]] = True
    for dr, dc in DIAG4:
        ia, ib = s.neighbours(dr, dc)
        n_diag_adj += ia.size
        all_pairs.append((ia, ib))

    drop = np.concatenate(drops) if drops else np.zeros(0)
    d_lo = np.concatenate(dlos) if dlos else np.zeros(0)
    ok = np.concatenate(ok_all) if ok_all else np.zeros(0, bool)

    out.update({
        "face_adjacencies": int(n_face_adj),
        "face_adjacencies_in_contact": int(n_face_ok),
        "break_zgap_pairs": int(n_face_adj - n_face_ok),
        "break_zgap_frac_of_face_adj": round(
            float(n_face_adj - n_face_ok) / n_face_adj, 6) if n_face_adj else 0.0,
        "diag_adjacencies": int(n_diag_adj),
        "pair_bed_drop_m": _pct(drop),
    })
    if (~ok).any():
        out["zgap_break_gradient"] = _pct((drop / PIXEL_M)[~ok])
        out["zgap_air_gap_m"] = _pct((drop - d_lo)[~ok])

    # WHICH MECHANISM. Two ablations against the same wet cells, so the
    # diagonal half and the vertical half are separated rather than argued
    # about: `face4_anyz` keeps 4-adjacency and DROPS the z test (whatever it
    # still breaks is the diagonal), `diag_withz` keeps the z test and ADDS the
    # diagonals (whatever it still breaks is the z gap).
    lab_face = union_find(s.n, face_pairs)
    lab_plan = union_find(s.n, all_pairs)
    lab_f4 = union_find(s.n, all_pairs[:2])
    diag_ok = []
    for dr, dc in DIAG4:
        ia, ib = s.neighbours(dr, dc)
        okd, _, _ = s.contact(ia, ib)
        diag_ok.append((ia[okd], ib[okd]))
    lab_d8 = union_find(s.n, face_pairs + diag_ok)
    out.update(comp_summary(lab_face, "face"))
    out.update(comp_summary(lab_plan, "plan8"))
    out.update(comp_summary(lab_f4, "face4_anyz"))
    out.update(comp_summary(lab_d8, "diag8_withz"))
    # The cells the VERTICAL mechanism alone would strand: local bed gradient
    # above the cell's own contact-break gradient (its depth over the pitch).
    out["cells_over_own_break_gradient_frac"] = round(
        float((s.grad > (s.d / PIXEL_M)).mean()), 6)

    # -- MONOTONICITY, at the finest scale there is.
    #
    # The shipped gate resamples one composed reach every 100 m and 250 m and
    # requires zero rises. This is the same question asked PER ADJACENT PAIR
    # over the whole tile, which is strictly stronger -- a rise that a 100 m
    # resample would smooth away is counted here -- and it needs no re-bake,
    # because both namespaces' bytes are already on disk. Reported for both
    # sides of an A/B rather than gated at zero: a fill that hands two reaches
    # of different level to one bank cell can put a legitimate local step in
    # either direction, and what matters is that a change does not add them.
    for name, dirs in (("face", FACE4), ("diag", DIAG4)):
        n_ord = n_bad = 0
        worst = 0.0
        for dr, dc in dirs:
            ia, ib = s.neighbours(dr, dc)
            if ia.size == 0:
                continue
            hi_a = s.g[ia] > s.g[ib]
            lo_a = s.g[ia] < s.g[ib]
            # "upstream" = the higher bed. A rise is the LOWER cell standing
            # higher than the upper one.
            rise = np.where(hi_a, s.d[ib] + s.g[ib] - (s.d[ia] + s.g[ia]),
                            np.where(lo_a, s.d[ia] + s.g[ia] - (s.d[ib] + s.g[ib]),
                                     0.0))
            ordered = hi_a | lo_a
            n_ord += int(ordered.sum())
            bad = ordered & (rise > 1e-6)
            n_bad += int(bad.sum())
            if bad.any():
                worst = max(worst, float(rise[bad].max()))
        out[f"descent_{name}_ordered_pairs"] = n_ord
        out[f"descent_{name}_rises"] = n_bad
        out[f"descent_{name}_rise_frac"] = round(n_bad / n_ord, 8) if n_ord else 0.0
        out[f"descent_{name}_largest_rise_m"] = round(worst, 4)
    out["isolated_cells"] = int((~has_face).sum())
    out["isolated_frac"] = round(float((~has_face).mean()), 6)

    # THE SLOPE DEPENDENCE. Every headline number again, binned by the cell's
    # own local bed gradient -- this is what makes "the defect is a function of
    # slope" a measurement instead of a reading of one screenshot.
    bins = np.digitize(s.grad, GRAD_BINS) - 1
    sz_face = np.bincount(lab_face)
    per_cell_comp = sz_face[lab_face]
    rows = []
    for b in range(len(GRAD_BINS) - 1):
        m = bins == b
        if not m.any():
            continue
        rows.append({
            "grad_lo": GRAD_BINS[b],
            "grad_hi": GRAD_BINS[b + 1] if GRAD_BINS[b + 1] < 1e8 else None,
            "wet_cells": int(m.sum()),
            "wet_frac_of_tile": round(float(m.mean()), 4),
            "isolated_frac": round(float((~has_face)[m].mean()), 4),
            "in_comp_under_4_frac": round(float((per_cell_comp[m] < 4).mean()), 4),
            "median_face_component_cells": float(np.median(per_cell_comp[m])),
            "depth_p50_m": round(float(np.median(s.d[m])), 3),
        })
    out["by_gradient"] = rows
    return out, (g, wet, d, s, has_face)


def annotate_path(rows, g, wet, d, tag):
    """Report bed, surface and contact for an ALREADY CHOSEN cell path.

    The before/after profile has to walk the SAME cells or it is comparing two
    routes, not two bakes -- and it can, because a water-only change leaves the
    bed bit-identical (``tools/verify_water_only_change.py``). The path is taken
    from the bake that HAS a continuous reach; the other one is asked what it
    put on those cells, which is where its holes show up as ``dry``.
    """
    for i, s in enumerate(rows):
        r, c = s["row"], s["col"]
        w = bool(wet[r, c])
        s[f"{tag}_wet"] = w
        s[f"{tag}_surface_m"] = round(float(g[r, c] + d[r, c]), 3) if w else None
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        step_face = (abs(a["row"] - b["row"]) + abs(a["col"] - b["col"])) == 1
        a["step"] = "face" if step_face else "diagonal"
        sa, sb = a[f"{tag}_surface_m"], b[f"{tag}_surface_m"]
        if sa is None or sb is None:
            a[f"{tag}_touches"] = False
            a[f"{tag}_air_gap_m"] = None
            continue
        hi, lo = ((a, sa), (b, sb)) if a["bed_m"] >= b["bed_m"] else ((b, sb), (a, sa))
        a[f"{tag}_touches"] = bool(step_face and lo[1] > hi[0]["bed_m"])
        a[f"{tag}_air_gap_m"] = round(max(0.0, hi[0]["bed_m"] - lo[1]), 3)
    return rows


def walk_downstream(g, wet, d, r0, c0, steps):
    """Walk downstream over WET cells, steepest bed descent, staying on water."""
    H, W = wet.shape
    seen, rows = set(), []
    r, c = int(r0), int(c0)
    for _ in range(steps):
        if not (0 <= r < H and 0 <= c < W) or (r, c) in seen:
            break
        seen.add((r, c))
        rows.append({"row": r, "col": c, "bed_m": round(float(g[r, c]), 3),
                     "wet": bool(wet[r, c]),
                     "depth_m": round(float(d[r, c]), 2),
                     "surface_m": round(float(g[r, c] + d[r, c]), 3)
                     if wet[r, c] else None})
        best = None
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                if dr == 0 and dc == 0:
                    continue
                rr, cc = r + dr, c + dc
                if not (0 <= rr < H and 0 <= cc < W) or (rr, cc) in seen:
                    continue
                if not wet[rr, cc]:
                    continue          # STAY ON THE WATER: this is a river walk
                slope = (g[r, c] - g[rr, cc]) / float(np.hypot(dr, dc))
                if best is None or slope > best[0]:
                    best = (slope, rr, cc)
        if best is None:
            break
        r, c = best[1], best[2]
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        a["step"] = "face" if (abs(a["row"] - b["row"]) + abs(a["col"] - b["col"])) == 1 \
            else "diagonal"
        hi, lo = (a, b) if a["bed_m"] >= b["bed_m"] else (b, a)
        zok = lo["surface_m"] > hi["bed_m"]
        a["z_overlap"] = bool(zok)
        a["touches_next"] = bool(zok and a["step"] == "face")
        a["air_gap_m"] = round(max(0.0, hi["bed_m"] - lo["surface_m"]), 3)
    return rows


def long_profile(rows, tag, label):
    """Rises down one reach, raw and resampled -- the shipped monotonicity gate.

    ``river_long_profile.py`` resamples a composed reach every 100 m and 250 m
    and requires zero rises; the argument for resampling is its own ("a
    single-step rise inside a 4 px ribbon is a lateral hop, a rise that survives
    100 m of smoothing is a real reversal"), and it is quoted rather than
    re-derived. This computes the same quantity on a path taken from the shipped
    bytes, so an A/B needs no re-bake -- and the SAME path is used for both
    sides, which a water-only change permits because the bed is bit-identical.
    """
    surf = np.array([r[f"{tag}_surface_m"] for r in rows
                     if r[f"{tag}_surface_m"] is not None], np.float64)
    out = {"label": label, "path_cells": len(rows),
           f"{tag}_wet_cells_on_path": int(surf.size),
           f"{tag}_dry_cells_on_path": int(len(rows) - surf.size)}
    if surf.size < 3:
        return out
    for win_m in (0.0, 100.0, 250.0):
        step = max(1, int(win_m / PIXEL_M))
        d = np.diff(surf[::step])
        up = d > 1e-6
        key = "raw" if step == 1 else f"{win_m:.0f}m"
        out[f"{tag}_rises_{key}"] = int(up.sum())
        out[f"{tag}_samples_{key}"] = int(d.size + 1)
        out[f"{tag}_largest_rise_{key}_mm"] = round(
            float(d[up].max() * 1000.0) if up.any() else 0.0, 1)
    out[f"{tag}_fall_m"] = round(float(surf[0] - surf[-1]), 2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--namespace", required=True)
    ap.add_argument("--seed-hex", required=True)
    ap.add_argument("--tiles", required=True)
    ap.add_argument("--at", default=None, help="world x,y metres")
    ap.add_argument("--profile-steps", type=int, default=60)
    ap.add_argument("--long-profile", type=int, default=0, metavar="STEPS",
                    help="walk the longest FACE component head to foot for up "
                         "to STEPS cells and report rises raw / 100 m / 250 m, "
                         "which is the shipped monotonicity gate's quantity")
    ap.add_argument("--vs-namespace", default=None,
                    help="a second namespace to report on the SAME cell path, "
                         "for a before/after profile. Refuses if the two "
                         "disagree about the bed.")
    ap.add_argument("--map", action="store_true",
                    help="print an ASCII wet map around --at")
    ap.add_argument("--json-out", default=None)
    args = ap.parse_args()

    ns = Path(args.cache_dir) / args.namespace
    tiles = [tuple(int(v) for v in tok.split(",")) for tok in args.tiles.split()]
    report = {"namespace": args.namespace, "pixel_m": PIXEL_M, "tiles": {}}
    agg = {"wet": 0, "iso": 0, "face_adj": 0, "zgap": 0, "diag": 0}

    for (x, y) in tiles:
        t = load(ns, args.seed_hex, x, y)
        if t is None:
            report["tiles"][f"{x},{y}"] = {"missing": True}
            continue
        stats, arrays = measure_tile(t)
        report["tiles"][f"{x},{y}"] = stats
        if arrays:
            agg["wet"] += stats["wet_cells"]
            agg["iso"] += stats["isolated_cells"]
            agg["face_adj"] += stats["face_adjacencies"]
            agg["zgap"] += stats["break_zgap_pairs"]
            agg["diag"] += stats["diag_adjacencies"]
        print(f"[{x},{y}] wet={stats['wet_cells']} "
              f"face_comps={stats.get('face_components')} "
              f"plan8_comps={stats.get('plan8_components')} "
              f"isolated={stats.get('isolated_frac')} "
              f"zgap_pairs={stats.get('break_zgap_pairs')}", flush=True)

        if args.long_profile and arrays:
            # THE LONGEST REACH THIS TILE DRAWS, head to foot. The head is the
            # highest-surfaced cell of the largest FACE component -- the largest
            # object the client actually draws, not the largest mask blob.
            g, wet, d, s, has_face = arrays
            lab_f = union_find(s.n, [(np.array([], np.int64),
                                      np.array([], np.int64))])
            face_pairs = []
            for dr, dc in FACE4:
                ia, ib = s.neighbours(dr, dc)
                ok, _, _ = s.contact(ia, ib)
                face_pairs.append((ia[ok], ib[ok]))
            lab_f = union_find(s.n, face_pairs)
            big = int(np.argmax(np.bincount(lab_f)))
            sel = np.flatnonzero(lab_f == big)
            head = sel[int(np.argmax((s.g + s.d)[sel]))]
            rows = walk_downstream(g, wet, d, int(s.r[head]), int(s.c[head]),
                                   args.long_profile)
            annotate_path(rows, g, wet, d, "after")
            lp = [long_profile(rows, "after", args.namespace)]
            if args.vs_namespace:
                t2 = load(Path(args.cache_dir) / args.vs_namespace,
                          args.seed_hex, x, y)
                if t2 is not None:
                    g2 = reconstructed_ground_m(t2)
                    assert np.array_equal(g2, g), "the two bakes disagree on bed"
                    w2 = np.asarray(t2.water_cp)
                    wet2 = w2 >= 0
                    d2 = np.where(wet2, w2.astype(np.float32)
                                  * (tc.WATER_DEPTH_LSB_MM / 1000.0),
                                  np.float32(0.0))
                    annotate_path(rows, g, wet2, d2, "before")
                    lp.append(long_profile(rows, "before", args.vs_namespace))
                    del t2, g2, w2, wet2, d2
            report["tiles"][f"{x},{y}"]["long_profile"] = lp
            for e in lp:
                print("   long profile", json.dumps(e), flush=True)

        if args.at and arrays:
            g, wet, d, s, has_face = arrays
            wx, wy = (float(v) for v in args.at.split(","))
            tx, ty = int(np.floor(wx / TILE_M)), int(np.floor(wy / TILE_M))
            if (tx, ty) == (x, y):
                col = int(round((wx - tx * TILE_M) / PIXEL_M))
                row = int(round((wy - ty * TILE_M) / PIXEL_M))
                rr, cc = np.nonzero(wet[max(0, row - 25):row + 26,
                                        max(0, col - 25):col + 26])
                if rr.size:
                    rr += max(0, row - 25)
                    cc += max(0, col - 25)
                    k = np.argmin((rr - row) ** 2 + (cc - col) ** 2)
                    row, col = int(rr[k]), int(cc[k])
                steps = walk_downstream(g, wet, d, row, col, args.profile_steps)
                annotate_path(steps, g, wet, d, "after")
                if args.vs_namespace:
                    t2 = load(Path(args.cache_dir) / args.vs_namespace,
                              args.seed_hex, x, y)
                    if t2 is not None:
                        g2 = reconstructed_ground_m(t2)
                        assert np.array_equal(g2, g), (
                            "the two namespaces disagree about the BED; a "
                            "before/after profile over one path is only "
                            "meaningful if the ground did not move")
                        w2 = np.asarray(t2.water_cp)
                        wet2 = w2 >= 0
                        d2 = np.where(wet2, w2.astype(np.float32)
                                      * (tc.WATER_DEPTH_LSB_MM / 1000.0),
                                      np.float32(0.0))
                        annotate_path(steps, g, wet2, d2, "before")
                        del t2, g2, w2, wet2, d2
                report["profile"] = {
                    "tile": [x, y], "start_row": row, "start_col": col,
                    "steps": steps}
                if args.map:
                    R = slice(max(0, row - 8), row + 45)
                    C = slice(max(0, col - 25), col + 20)
                    print("\nWET MAP  # wet   . dry")
                    sub = wet[R, C]
                    for i in range(sub.shape[0]):
                        print("%5d " % (max(0, row - 8) + i)
                              + "".join("#" if v else "." for v in sub[i]))
        del t

    report["corridor"] = {
        "wet_cells": agg["wet"], "isolated_cells": agg["iso"],
        "isolated_frac": round(agg["iso"] / agg["wet"], 6) if agg["wet"] else 0.0,
        "face_adjacencies": agg["face_adj"], "break_zgap_pairs": agg["zgap"],
        "diag_adjacencies": agg["diag"],
    }
    print(json.dumps(report["corridor"], indent=2))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2))
        print("wrote " + args.json_out)


if __name__ == "__main__":
    main()
