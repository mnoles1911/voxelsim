#!/usr/bin/env python3
"""Why the baked wet mask fragments while the area network does not.

    python tools/river_break_probe.py --npz D:/tmp/frag-npz/-12_-5.npz \
        --trace-window 0,40,2450,2700

WHAT THIS IS FOR
----------------
On the stitched (-11,-4) (-11,-5) (-12,-5) (-11,-6) corridor the AREA network
at flow ``log2 >= 23`` is four components with a largest span of 4,216 m, while
the WET mask over the same ground is ~2,000 components with a longest span of
1,113 m and only 36.1% of above-threshold area cells wet. Accumulated area is
supposed to grow monotonically downstream, so a threshold on it cannot break a
river -- and that argument is what made the gap look like a defect in the step
from area to water. Three candidates were on the table:

    1. a hard ``q_drawable`` cut against the flow byte's factor-of-two log2 bin
    2. MFD splitting Q below the cut
    3. B5 basins written dry

They are separated here, on the UNQUANTISED intermediates from
``bake_tiles_from_cache.py --npz-dir --diagnostic``, because a probe that reads
only the shipped flow byte cannot tell 1 from the rest -- the byte IS suspect 1.

WHAT IT MEASURES, and which candidate each number rules in or out
-----------------------------------------------------------------
``basins``      shipped-wet vs ``q >= q_drawable``. The graded plane writes
                registered-basin cells dry and nothing else drops a cell, so
                the difference between those two masks is EXACTLY candidate 3,
                with no modelling in between.

``monotone``    for every above-threshold cell, step to its downstream
                continuation (the 8-neighbour of greatest accumulation -- see
                ``_downstream``) and compare. Reported for BOTH ``acc`` and
                ``q``, which come from the same ``accumulate_mfd`` over the
                same surface: if MFD dispersion is breaking Q it must break
                area at the same cells and by the same factor, and then the
                two networks can only differ by how they are thresholded.

``threshold``   the A/B that isolates candidate 1: the same field, labelled
                once at a SHARP cut and once at the flow byte's log2 bin. A
                log2 bin tolerates a factor of two before it opens a gap; a
                linear cut does not. Run on ``acc`` (where the shipped
                comparison lives) and on ``q``.

``trace``       one reach, walked downstream cell by cell, printing elevation,
                acc, q, q/q_drawable and wet. The gap is a claim about what
                happens along a channel and nothing else settles it.

The labeller is ``measure_corridor_fragmentation.label8`` -- the same one the
corridor numbers were taken with, so a span here is comparable to a span there.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402
from terrain_service.bake import pipeline as bp  # noqa: E402
from terrain_service.bake import water as _water  # noqa: E402
from terrain_service.bake import flow as _flow  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from measure_corridor_fragmentation import label8, max_pairwise_m  # noqa: E402

PIXEL_M = 30.0 / tc.FINE_SCALE  # 1.875

# The 8 neighbour offsets, in a fixed order so a trace is reproducible.
NBR = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]


def q_drawable() -> float:
    return _water.q_drawable_m3_yr(
        bp.PRODUCTION.fine_pixel_m,
        bp.CONSTANTS.water_min_width_px,
        bp.CONSTANTS.water_q_perennial_m3_yr,
    )


# --------------------------------------------------------------------- labelling

def label_stats(mask, name):
    """Components, sizes and spans of a boolean mask, the corridor tool's way."""
    r, c = np.nonzero(mask)
    if r.size == 0:
        return dict(name=name, cells=0, n_comp=0, max_span_m=0.0, top=[])
    lab = label8(r, c, mask.shape[0], mask.shape[1])
    sizes = np.bincount(lab)
    order = np.argsort(-sizes)
    top = []
    for k in order[:6]:
        m = lab == k
        top.append((int(sizes[k]), max_pairwise_m(r[m], c[m])))
    spans = np.array([t[1] for t in top], float)
    return dict(name=name, cells=int(r.size), n_comp=int(sizes.size),
                largest_px=int(sizes.max()),
                max_span_m=float(spans.max()) if spans.size else 0.0,
                top=top)


def show(st):
    line = (f"  {st['name']:<34} cells {st['cells']:>8,}  comps {st['n_comp']:>7,}"
            f"  largest {st.get('largest_px', 0):>7,} px"
            f"  span_max {st['max_span_m']:8.0f} m")
    if st["top"]:
        line += "\n" + " " * 6 + "top: " + "  ".join(
            f"{s:,}px/{sp:.0f}m" for s, sp in st["top"][:5])
    return line


# --------------------------------------------------------------------- topology

def _downstream(elev, acc, r, c):
    """The downstream continuation: lower ground, and among that the main stem.

    THE OBVIOUS RULE DOES NOT WORK, and finding that out was the first result.
    "Step to the neighbour of greatest accumulation" assumes a donor always
    carries less than the cell it feeds, which is true under D8 and FALSE under
    MFD: a cell hands its water to a fan, so the next cell down can hold less
    than the one above it. The rule oscillates between two mutual maxima at the
    first divergence -- measured on (-12,-5), it stopped after two steps.

    So the direction comes from the GROUND (strictly lower elevation, the
    steepest-descent stem picked by accumulation among the descents) and the
    fields are read ALONG that path rather than steering it. In a pit -- the
    dump carries the shipped post-B5 surface, not the filled routing one -- the
    walk escapes to the lowest neighbour and says so, rather than stopping.
    """
    h, w = elev.shape
    best_acc = np.full(r.size, -np.inf, np.float64)
    best_low = np.full(r.size, np.inf, np.float64)
    br = r.copy(); bc = c.copy()
    lr = r.copy(); lc = c.copy()
    z0 = elev[r, c]
    for dr, dc in NBR:
        rr, cc = r + dr, c + dc
        ok = (rr >= 0) & (rr < h) & (cc >= 0) & (cc < w)
        zz = np.full(r.size, np.inf, np.float64)
        aa = np.full(r.size, -np.inf, np.float64)
        zz[ok] = elev[rr[ok], cc[ok]]
        aa[ok] = acc[rr[ok], cc[ok]]
        desc = ok & (zz < z0)
        take = desc & (aa > best_acc)
        best_acc = np.where(take, aa, best_acc)
        br = np.where(take, rr, br); bc = np.where(take, cc, bc)
        takel = ok & (zz < best_low)
        best_low = np.where(takel, zz, best_low)
        lr = np.where(takel, rr, lr); lc = np.where(takel, cc, lc)
    pit = ~np.isfinite(best_acc)
    return (np.where(pit, lr, br), np.where(pit, lc, bc), pit)


def monotonicity(elev, acc, q, sel, qd, label):
    """How often does the field FALL going downstream, and by how much?

    A drop is what breaks a component under a threshold, so this is the direct
    measurement of candidate 2. Reported for acc and q side by side: they are
    two sweeps of the same kernel over the same surface differing only in the
    source term, so if MFD dispersion is the mechanism the two drop TOGETHER.
    """
    r, c = np.nonzero(sel)
    dr, dc, pit = _downstream(elev, acc, r, c)
    keep = ~((dr == r) & (dc == c)) & ~pit
    r, c, dr, dc = r[keep], c[keep], dr[keep], dc[keep]
    a0 = acc[r, c].astype(np.float64)
    a1 = acc[dr, dc].astype(np.float64)
    q0 = q[r, c].astype(np.float64)
    q1 = q[dr, dc].astype(np.float64)
    ratio_a = a1 / np.maximum(a0, 1e-9)
    ratio_q = q1 / np.maximum(q0, 1e-9)
    out = [f"  {label}: {r.size:,} cells with a downstream step"]
    for nm, rt in (("acc", ratio_a), ("q  ", ratio_q)):
        out.append(
            f"    {nm}  falls {100.0 * float((rt < 1.0).mean()):6.2f}%   "
            f"falls >2x {100.0 * float((rt < 0.5).mean()):6.2f}%   "
            f"ratio p1 {np.percentile(rt, 1):.3f} p5 {np.percentile(rt, 5):.3f} "
            f"p50 {np.percentile(rt, 50):.3f}  min {rt.min():.4f}")
    # The cell that matters: above the cut, whose downstream continuation is not.
    above = q0 >= qd
    if above.any():
        drops = above & (q1 < qd)
        out.append(
            f"    q crosses BACK DOWN through the cut at "
            f"{int(drops.sum()):,} of {int(above.sum()):,} above-cut cells "
            f"({100.0 * float(drops.mean() if above.size else 0):.2f}% of all steps, "
            f"{100.0 * drops.sum() / max(int(above.sum()), 1):.2f}% of above-cut)")
        if drops.any():
            rr = q1[drops] / qd
            out.append(f"    ...and lands at q/q_drawable p50 {np.percentile(rr, 50):.3f}"
                       f"  p90 {np.percentile(rr, 90):.3f}  max {rr.max():.3f}")
    return "\n".join(out)


# --------------------------------------------------------------------- mechanism

def d8_accumulation(filled, cell_m):
    """Catchment area by SINGLE steepest descent -- monotone by construction.

    THE CONTROL FOR MFD DISPERSION. Under D8 every cell hands its entire
    accumulation to one receiver, so a receiver holds at least what its donor
    held and a threshold on the field CANNOT break a downstream chain. Under
    MFD a cell pays out to a fan and the next cell down can hold less. Running
    both over the SAME filled surface with the same cell area isolates that one
    difference; anything the two networks do differently is dispersion and
    nothing else.
    """
    import numba

    rec, _ = _flow.d8_receivers(filled, cell_m)
    rec = np.asarray(rec, np.int64).ravel()
    order = np.argsort(filled.ravel(), kind="stable")[::-1]  # descending
    acc = np.full(filled.size, float(cell_m) * float(cell_m), np.float64)

    @numba.njit(cache=True)
    def _sweep(order, rec, acc):
        for i in range(order.size):
            k = order[i]
            r = rec[k]
            if r >= 0:
                acc[r] += acc[k]
        return acc

    return _sweep(np.ascontiguousarray(order, np.int64), rec,
                  acc).reshape(filled.shape)


def dispersion_local_max(elev, acc, sel):
    """MFD dispersion on the BAKE'S OWN field, with nothing reconstructed.

    Water leaves a cell downhill, so every strictly-lower 8-neighbour is a
    place its accumulation can have gone. Under any SINGLE-receiver rule one of
    them must therefore hold at least what this cell holds -- the receiver
    inherits the whole of it, plus its own. A cell that has a lower neighbour
    and whose lower neighbours ALL hold less than it does is proof that its
    accumulation was split: no recipient inherited it.

    That is candidate 2 stated as a property of the shipped field alone. It
    needs no routing surface, no refill and no receiver forest, so unlike the
    D8 control below it cannot be argued with on the grounds that the
    reconstruction is not the bake's. Under D8 the count is identically zero
    away from pits and the domain rim.
    """
    h, w = elev.shape
    r, c = np.nonzero(sel)
    z0 = elev[r, c]
    a0 = acc[r, c]
    best = np.full(r.size, -np.inf, np.float64)
    has_lower = np.zeros(r.size, bool)
    for dr, dc in NBR:
        rr, cc = r + dr, c + dc
        ok = (rr >= 0) & (rr < h) & (cc >= 0) & (cc < w)
        zz = np.full(r.size, np.inf, np.float64)
        aa = np.full(r.size, -np.inf, np.float64)
        zz[ok] = elev[rr[ok], cc[ok]]
        aa[ok] = acc[rr[ok], cc[ok]]
        low = ok & (zz < z0)
        has_lower |= low
        best = np.where(low & (aa > best), aa, best)
    keep = has_lower
    ratio = best[keep] / np.maximum(a0[keep], 1e-9)
    split = ratio < 1.0
    return (f"  dispersion on the bake's own field, no reconstruction:\n"
            f"    {int(keep.sum()):,} of {r.size:,} network cells have a "
            f"strictly lower neighbour\n"
            f"    of those, {int(split.sum()):,} ({100.0 * split.mean():.2f}%) "
            f"have NO lower neighbour holding as much as they do\n"
            f"      -- their whole accumulation was split. Under any "
            f"single-receiver rule this count is 0.\n"
            f"    best surviving share  p5 {np.percentile(ratio, 5):.3f}  "
            f"p50 {np.percentile(ratio, 50):.3f}  "
            f"p95 {np.percentile(ratio, 95):.3f}")


def receiver_monotonicity(acc, q, rec, sel, qd, label):
    """The drop along the TRUE routing topology, not a guessed one.

    ``rec`` is ``d8_receivers(filled)`` -- the same forest the bake grades the
    water plane down and the same one ``water_head_mask`` calls a head against.
    A cell's receiver is where its water goes, so ``field[rec] < field[cell]``
    is exactly "this field is not monotone downstream", with no walking rule
    of this tool's own invention in the way.
    """
    idx = np.flatnonzero(sel.ravel())
    r = rec.ravel()[idx]
    keep = r >= 0
    idx, r = idx[keep], r[keep]
    a0, a1 = acc.ravel()[idx], acc.ravel()[r]
    q0, q1 = q.ravel()[idx], q.ravel()[r]
    ra = a1 / np.maximum(a0, 1e-9)
    rq = q1 / np.maximum(q0, 1e-9)
    out = [f"  {label}: {idx.size:,} cells with a receiver"]
    for nm, rt in (("acc", ra), ("q  ", rq)):
        out.append(
            f"    {nm}  falls {100.0 * float((rt < 1.0).mean()):6.2f}%   "
            f"falls >2x {100.0 * float((rt < 0.5).mean()):6.2f}%   "
            f"ratio p1 {np.percentile(rt, 1):.3f} p5 {np.percentile(rt, 5):.3f} "
            f"p50 {np.percentile(rt, 50):.3f}  min {rt.min():.4f}")
    above = q0 >= qd
    if above.any():
        drops = above & (q1 < qd)
        out.append(f"    q falls back below the cut at {int(drops.sum()):,} of "
                   f"{int(above.sum()):,} above-cut cells "
                   f"({100.0 * drops.sum() / max(int(above.sum()), 1):.2f}%)")
    return "\n".join(out)


# --------------------------------------------------------------------- the trace

def trace(acc, q, elev, wet_shipped, rec, start_rc, steps, qd):
    """Walk the D8 receiver forest from a start cell and print the fields.

    ``rec`` is the forest over the DEPRESSION-FILLED surface, so every step is
    where the routing actually sends the water; ``rec < 0`` is the domain
    border and ends the walk. Reported elevation is the shipped one, which is
    why it can rise: the filled surface is what routes.
    """
    h, w = elev.shape
    k = start_rc[0] * w + start_rc[1]
    rows = []
    seen = set()
    for i in range(steps):
        if k in seen:
            rows.append((i, k // w, k % w, None, None, None, None, "LOOP"))
            break
        seen.add(k)
        r, c = k // w, k % w
        rows.append((i, r, c, float(elev[r, c]), float(acc[r, c]),
                     float(q[r, c]), float(q[r, c]) / qd,
                     "WET" if wet_shipped[r, c] else "dry"))
        nk = int(rec.ravel()[k])
        if nk < 0:
            rows.append((i + 1, r, c, None, None, None, None, "OUT"))
            break
        k = nk
    return rows


def trace_report(rows, qd):
    out = ["  step     r     c    elev_m        acc_m2          q_m3yr   q/qdraw  state"]
    prev = None
    for t in rows:
        if t[7] in ("LOOP", "OUT"):
            out.append(f"  {t[0]:>4}  {t[1]:>5} {t[2]:>5}   "
                       + ("(revisited -- stop)" if t[7] == "LOOP"
                          else "(receiver -1: leaves the domain)"))
            break
        now = t[7].split()[0]
        out.append(f"  {t[0]:>4}  {t[1]:>5} {t[2]:>5}  {t[3]:8.2f}  {t[4]:12.3e}"
                   f"  {t[5]:14.5e}  {t[6]:7.3f}  {t[7]:<8}"
                   + ("  <== BREAK" if prev == "WET" and now == "dry"
                      else ("  <== RESUME" if prev == "dry" and now == "WET"
                            else "")))
        prev = now
    return "\n".join(out)


# --------------------------------------------------------------------- stitched

FINE_PX = tc.TILE_SIZE * tc.FINE_SCALE


def stitch_mode(paths, area_log2, qd):
    """The corridor headline, re-measured with the two cuts LEVEL-MATCHED.

    The recorded gap -- area network 4 components and 4,216 m against a wet
    mask of ~2,000 components and 1,113 m -- compares a cut at
    ``flow log2 >= 23`` with a cut at ``q >= q_drawable``. Those are not the
    same height. This stitches the same four tiles on their true lattice, as
    ``measure_corridor_fragmentation`` does, and labels five sets with the same
    labeller: the two as reported, and then each field re-cut at the OTHER's
    level. If the gap is the currency the re-cuts change nothing; if it is the
    level they swap.
    """
    tiles = {}
    for p in paths:
        x, y = (int(v) for v in Path(p).stem.split("_"))
        tiles[(x, y)] = p
    xs = sorted({x for x, _ in tiles})
    ys = sorted({y for _, y in tiles})
    x0, y0 = xs[0], ys[0]
    H = (ys[-1] - y0 + 1) * FINE_PX
    W = (xs[-1] - x0 + 1) * FINE_PX

    # Pass 1: every cell above a floor two octaves under the lowest cut, which
    # is far below anything either threshold can select. Sparse by construction
    # -- the stitched grid is 16384 x 24576 and no dense array of it is built.
    floor_a = float(2 ** (area_log2 - 1))
    recs = []
    ks = []
    for (x, y), p in sorted(tiles.items()):
        z = np.load(p)
        acc = np.asarray(z["accumulation_m2"], np.float64)
        q = np.asarray(z["discharge_m3_yr"], np.float64)
        wet = np.isfinite(np.asarray(z["water_surface_m"], np.float64))
        sel = (acc >= floor_a) | (q >= 0.5 * qd) | wet
        r, c = np.nonzero(sel)
        recs.append((r.astype(np.int64) + (y - y0) * FINE_PX,
                     c.astype(np.int64) + (x - x0) * FINE_PX,
                     acc[r, c], q[r, c], wet[r, c]))
        ks.append(np.median(q[acc >= float(2 ** area_log2)]
                            / acc[acc >= float(2 ** area_log2)]))
        print(f"  {(x, y)}: {r.size:,} candidate cells, "
              f"median runoff {ks[-1]:.4f} m/yr", flush=True)
        del acc, q, wet, sel
        z.close()
    R = np.concatenate([t[0] for t in recs])
    C = np.concatenate([t[1] for t in recs])
    A = np.concatenate([t[2] for t in recs])
    Q = np.concatenate([t[3] for t in recs])
    Wt = np.concatenate([t[4] for t in recs])
    del recs
    k50 = float(np.median(np.concatenate([np.full(1, v) for v in ks])))
    print(f"  corridor median implied runoff {k50:.4f} m/yr; "
          f"q_drawable = area {qd / k50:.4e} m2 = 2^{np.log2(qd / k50):.2f}")

    def lab(mask, name):
        r, c, = R[mask], C[mask]
        if r.size == 0:
            return dict(name=name, cells=0, n_comp=0, largest_px=0,
                        max_span_m=0.0, top=[], long=(0, 0))
        l = label8(r, c, H, W)
        sizes = np.bincount(l)
        # SPAN OF EVERY COMPONENT, not just the biggest few. The reported gap
        # is "four area components over 1.8 km against nothing over 2 km in
        # the wet mask", and a long thin reach need not be a large one -- a
        # top-5-by-size shortlist can miss exactly the component under test.
        # Components under 40 px cannot span 1.8 km at a 1.875 m pitch even in
        # a straight line, so they are skipped rather than hulled.
        spans = np.zeros(sizes.size)
        for k in np.flatnonzero(sizes >= 40):
            m = l == k
            spans[k] = max_pairwise_m(r[m], c[m])
        order = np.argsort(-sizes)
        top = [(int(sizes[k]), float(spans[k])) for k in order[:5]]
        return dict(name=name, cells=int(r.size), n_comp=int(sizes.size),
                    largest_px=int(sizes.max()), max_span_m=float(spans.max()),
                    top=top,
                    long=(int((spans >= 1800).sum()), int((spans >= 2000).sum())))

    thr_a = float(2 ** area_log2)
    n_q = int((Q >= qd).sum())
    srt = np.sort(A)
    a_match = float(srt[-n_q])
    del srt
    print(f"  equal-cell-count area threshold {a_match:.4e} m2 "
          f"= 2^{np.log2(a_match):.2f}   (n = {n_q:,})")
    for st in (lab(A >= thr_a, f"AREA as reported  acc>=2^{area_log2}"),
               lab(Q >= qd, "Q as reported     q>=q_drawable"),
               lab(Wt, "WET shipped plane"),
               lab(A >= a_match, "AREA re-cut at the wet level"),
               lab(Q >= k50 * thr_a, f"Q    re-cut at the 2^{area_log2} level")):
        print(show(st))
        print(f"      components spanning >=1.8 km: {st['long'][0]}"
              f"   >=2 km: {st['long'][1]}")
    on = A >= thr_a
    print(f"  wet share of the area network: {100.0 * Wt[on].mean():.1f}%   "
          f"(reported 36.1%)")
    print(f"  wet share of the LEVEL-MATCHED area network: "
          f"{100.0 * Wt[A >= a_match].mean():.1f}%")


# --------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--npz", required=True, action="append",
                    help="a dump from bake_tiles_from_cache --npz-dir, repeatable")
    ap.add_argument("--area-log2", type=int, default=23)
    ap.add_argument("--trace-window", default=None,
                    help="r0,r1,c0,c1 -- start the downstream trace at the "
                         "maximum-accumulation cell in this window")
    ap.add_argument("--trace-steps", type=int, default=1200)
    ap.add_argument("--no-mechanism", action="store_true",
                    help="skip the fill + MFD/D8 control (the expensive half)")
    ap.add_argument("--stitch", action="store_true",
                    help="paste the given tiles on their true lattice and "
                         "re-measure the corridor headline with the two cuts "
                         "level-matched")
    args = ap.parse_args()

    qd = q_drawable()
    thr_area = float(2 ** args.area_log2)
    print(f"q_drawable = {qd:.6e} m3/yr    area threshold 2^{args.area_log2} "
          f"= {thr_area:.4e} m2    pixel {PIXEL_M} m")

    if args.stitch:
        print(f"\n================ STITCHED CORRIDOR, {len(args.npz)} tiles "
              "================")
        stitch_mode(args.npz, args.area_log2, qd)
        return 0

    for path in args.npz:
        z = np.load(path)
        acc = np.asarray(z["accumulation_m2"], np.float64)
        q = np.asarray(z["discharge_m3_yr"], np.float64)
        elev = np.asarray(z["elevation_m"], np.float64)
        flow = np.asarray(z["flow"])
        ws = np.asarray(z["water_surface_m"], np.float64)
        wet_shipped = np.isfinite(ws)
        print(f"\n================ {Path(path).name} "
              f"{acc.shape[0]}x{acc.shape[1]} ================")

        # ---- CANDIDATE 3: what the plane drops relative to the raw cut.
        wet_q = q >= qd
        only_q = wet_q & ~wet_shipped
        only_ship = wet_shipped & ~wet_q
        print("\n[basins] shipped wet mask vs the raw q >= q_drawable cut")
        print(f"  q >= q_drawable            {int(wet_q.sum()):>8,}")
        print(f"  shipped wet (plane finite) {int(wet_shipped.sum()):>8,}")
        print(f"  dropped by the plane       {int(only_q.sum()):>8,}   "
              f"({100.0 * only_q.sum() / max(int(wet_q.sum()), 1):.3f}% of the cut)")
        print(f"  wet but below the cut      {int(only_ship.sum()):>8,}")

        # ---- the two networks, sharp and binned, on the SAME field.
        print("\n[threshold] the same field, cut sharply and cut in the "
              "flow byte's log2 bin")
        binned_area = (flow & tc.FLOW_LOG2_MASK) >= args.area_log2
        sharp_area = acc >= thr_area
        # The byte's own rule, reproduced from the unquantised field, so the
        # binned/sharp difference is not confounded by how the byte was written.
        with np.errstate(divide="ignore"):
            acc_log2 = np.floor(np.log2(np.maximum(acc, 1e-30)))
        recon_area = acc_log2 >= args.area_log2
        qd_bin = float(2 ** np.floor(np.log2(qd)))
        with np.errstate(divide="ignore"):
            q_log2 = np.floor(np.log2(np.maximum(q, 1e-30)))
        sharp_q = q >= qd
        binned_q = q_log2 >= np.floor(np.log2(qd))
        for st in (label_stats(binned_area, f"AREA  binned  flow log2>={args.area_log2}"),
                   label_stats(recon_area, f"AREA  binned  floor(log2 acc)>={args.area_log2}"),
                   label_stats(sharp_area, f"AREA  sharp   acc>=2^{args.area_log2}"),
                   label_stats(binned_q, f"Q     binned  floor(log2 q)>={np.floor(np.log2(qd)):.0f}"
                                         f" (={qd_bin:.3e})"),
                   label_stats(sharp_q, "Q     sharp   q>=q_drawable"),
                   label_stats(wet_shipped, "WET   shipped plane")):
            print(show(st))

        # ---- IS Q ANYTHING BUT A RESCALED ACC? The corridor comparison sets
        # area against discharge as if they were different quantities. Within
        # ONE tile the runoff field is a heavily smoothed coarse plane, so
        # q/acc is the implied runoff in m/yr and its SPREAD is the entire
        # information content of the currency change at this scale. If it is
        # narrow, "area network" and "discharge network" are the same network
        # at two different threshold LEVELS, and comparing them at levels
        # nobody matched measures the levels.
        on = sharp_area
        k = q[on] / np.maximum(acc[on], 1e-9)
        print("\n[currency] implied runoff q/acc over the area network, m/yr")
        print(f"  p1 {np.percentile(k,1):.4f}  p50 {np.percentile(k,50):.4f}"
              f"  p99 {np.percentile(k,99):.4f}   p99/p1 "
              f"{np.percentile(k,99)/max(np.percentile(k,1),1e-12):.3f}")
        k50 = float(np.percentile(k, 50))
        acc_equiv = qd / k50
        print(f"  q_drawable at the median runoff is an AREA of {acc_equiv:.4e} m2 "
              f"= 2^{np.log2(acc_equiv):.2f}, i.e. {acc_equiv / thr_area:.2f}x the "
              f"2^{args.area_log2} the control was taken at")

        # ---- THE FAIR CONTROL. Two ways to match the levels, because either
        # alone can be argued with: the area threshold that admits the same
        # NUMBER of cells as the wet cut, and the area threshold the wet cut
        # implies through the median runoff.
        print("\n[matched] the area network re-cut at levels matched to the wet cut")
        n_wet = int(sharp_q.sum())
        vals = np.sort(acc[acc > 0].ravel())
        acc_same_n = float(vals[-n_wet]) if n_wet and vals.size >= n_wet else thr_area
        del vals
        print(f"  same-cell-count area threshold {acc_same_n:.4e} m2 "
              f"(2^{np.log2(acc_same_n):.2f})")
        for st in (label_stats(acc >= acc_same_n, "AREA  cut for equal cell count"),
                   label_stats(acc >= acc_equiv, "AREA  cut at q_drawable/runoff"),
                   label_stats(q >= k50 * thr_area,
                               f"Q     cut at 2^{args.area_log2} x runoff")):
            print(show(st))

        # ---- CANDIDATE 2: is the field monotone downstream at all?
        print("\n[monotone] downstream steps, on cells above the area threshold")
        print(monotonicity(elev, acc, q, sharp_area, qd, "area>=2^%d" % args.area_log2))

        # ---- HOW STEEPLY DOES CONNECTEDNESS DEPEND ON THE LEVEL? If the
        # answer is "very", then any two networks cut at unmatched levels differ
        # for that reason alone and the comparison says nothing about currency.
        print("\n[sweep] longest component vs threshold, on acc and on q")
        print("    level(2^)   AREA cells   comps  span_m  |  "
              "Q cells   comps  span_m   (Q at level x runoff)")
        for e in np.arange(22.0, 25.01, 0.25):
            t = float(2.0 ** e)
            a = label_stats(acc >= t, "")
            b = label_stats(q >= k50 * t, "")
            print(f"      {e:5.2f}   {a['cells']:>10,} {a['n_comp']:>7,} "
                  f"{a['max_span_m']:7.0f}  |  {b['cells']:>8,} {b['n_comp']:>7,} "
                  f"{b['max_span_m']:7.0f}")

        # ---- CANDIDATE 1, restated as a number: how far below the cut do the
        # gaps sit? A gap shallower than a factor of two is one a log2 bin
        # would have closed and a linear cut does not.
        print("\n[gapdepth] cells on the area network that are NOT wet")
        off = sharp_area & ~wet_q
        if off.any():
            rr = q[off] / qd
            for p in (50, 75, 90, 95, 99):
                print(f"    q/q_drawable p{p:<3} {np.percentile(rr, p):.4f}")
            print(f"    within a factor of 2 of the cut: "
                  f"{100.0 * float((rr >= 0.5).mean()):.2f}%")
            print(f"    within a factor of 10:           "
                  f"{100.0 * float((rr >= 0.1).mean()):.2f}%")

        # ---- CANDIDATE 2, on the true routing topology and against its own
        # control. Both need the filled surface and the receiver forest, which
        # are not in the dump; recomputed here on the shipped tile with the
        # bake's OWN kernels, so MFD and D8 are compared over identical ground.
        rec = None
        if not args.no_mechanism:
            cm = bp.PRODUCTION.fine_pixel_m
            filled = _flow.fill_depressions(np.asarray(elev, np.float32))
            rec, _ = _flow.d8_receivers(filled, cm)
            print("\n[mechanism] MFD dispersion, measured and then controlled")
            print(dispersion_local_max(elev, acc, sharp_area))
            print(receiver_monotonicity(acc, q, np.asarray(rec, np.int64),
                                        sharp_area, qd,
                                        f"area>=2^{args.area_log2}"))
            a_mfd = np.asarray(_flow.accumulate_mfd(filled, cm,
                                                    p=bp.CONSTANTS.mfd_p), np.float64)
            a_d8 = d8_accumulation(filled, cm)
            # HOW FAITHFUL IS THE RECONSTRUCTION? The bake accumulated over the
            # PADDED, PRE-B5 filled surface with the pyramid's inflow at its
            # rim; this refills the shipped, post-B5, interior-only one. The
            # A/B below is still controlled -- MFD and D8 run over the SAME
            # surface here -- but its absolute numbers are not the bake's, and
            # that has to be a printed number rather than a footnote.
            both = sharp_area & (a_mfd > 0)
            lr = np.log(acc[both] / a_mfd[both])
            print(f"  reconstruction vs the bake's own acc on the network: "
                  f"log-ratio p5 {np.percentile(lr, 5):+.2f} "
                  f"p50 {np.percentile(lr, 50):+.2f} "
                  f"p95 {np.percentile(lr, 95):+.2f}  "
                  f"(0 = identical; the D8/MFD A/B below is internal to the "
                  f"reconstruction and does not depend on this)")
            print("  the two networks at MATCHED cell counts, same surface, "
                  "same kernel family:")
            for nm, f in (("MFD p=%.1f" % bp.CONSTANTS.mfd_p, a_mfd),
                          ("D8 (monotone)", a_d8)):
                v = np.sort(f[f > 0].ravel())
                for n in (int(sharp_area.sum()), int(sharp_q.sum())):
                    t = float(v[-n])
                    print(show(label_stats(f >= t, f"{nm} @ n={n:,}")))
                del v
            del a_mfd, a_d8, filled

        # ---- the walk.
        if args.trace_window:
            r0, r1, c0, c1 = (int(v) for v in args.trace_window.split(","))
            win = acc[r0:r1, c0:c1]
            k = int(np.argmax(win))
            sr, sc = r0 + k // win.shape[1], c0 + k % win.shape[1]
            if rec is None:
                cm = bp.PRODUCTION.fine_pixel_m
                rec, _ = _flow.d8_receivers(
                    _flow.fill_depressions(np.asarray(elev, np.float32)), cm)
            print(f"\n[trace] downstream from ({sr},{sc}), the maximum-accumulation "
                  f"cell of rows {r0}:{r1} cols {c0}:{c1}")
            rows = trace(acc, q, elev, wet_shipped, np.asarray(rec, np.int64),
                         (sr, sc), args.trace_steps, qd)
            print(trace_report(rows, qd))
            st = [t[7] for t in rows if t[7] in ("WET", "dry")]
            runs = []
            for s in st:
                if runs and runs[-1][0] == s:
                    runs[-1][1] += 1
                else:
                    runs.append([s, 1])
            print(f"  {len(rows)} steps: " + " ".join(
                f"{n}{'W' if s == 'WET' else 'd'}" for s, n in runs))
        z.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
