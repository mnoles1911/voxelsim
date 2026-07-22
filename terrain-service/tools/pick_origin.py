#!/usr/bin/env python3
"""Choose a launch origin from a ``scan_land.py --json`` result.

Why this is not just ``max(land_fraction)``: the highest land fraction in a
scan is, by construction, the tile FURTHEST from any water. That is usually
the worst possible spawn -- a featureless plateau interior with no coast, no
river mouth, no bay, nothing to navigate by. Tonight's other failure mode is
the mirror image: tile (0,0) of seed 20260719 is entirely underwater
(max -8.9 m), so "just use the origin" is also wrong.

What we actually want for a launch grid is a COASTAL tile with a substantial
land majority, sitting in a neighbourhood that is also mostly land (so the
5x5 contiguous pregen around it does not run off into open ocean). That is
three separate terms, so it gets a scoring function and a unit test rather
than a one-line ``sorted()``.

Scoring, per candidate tile:

  land term    triangular, peaks at ``IDEAL_LAND`` (0.70) -- a tile that is
               70% land and 30% water is very likely to contain shoreline.
               Pure land (1.0) still scores respectably (~0.57) because it is
               a usable if boring spawn; open ocean scores ~0.
  relief term  small bonus for elevation range, capped -- distinguishes real
               topography from a flat shelf, but capped so a lone spire
               cannot buy its way past the land term.
  neighbour    mean land fraction of the scan's adjacent samples, which
  term         guards against isolated islands: the scan is STRIDED (~46 km
               per step at stride 3) while the pregen is CONTIGUOUS (~77 km
               across at radius 2), so neighbouring samples straddle the
               pregen box and are a fair proxy for what it will contain.

Tiles below ``MIN_LAND`` are rejected outright rather than ranked -- an
almost-underwater tile is never the right answer, however good its relief.

Usage:
    python tools/pick_origin.py scan.json            # prints "X,Y"
    python tools/pick_origin.py scan.json --explain  # full ranking table
"""

import argparse
import json
import sys

IDEAL_LAND = 0.70
MIN_LAND = 0.10
RELIEF_FULL_SCORE_M = 1500.0
RELIEF_WEIGHT = 0.25
NEIGHBOUR_WEIGHT = 0.35


def _land_term(land: float) -> float:
    """Triangular preference peaking at IDEAL_LAND, clipped to [0, 1]."""
    return max(0.0, 1.0 - abs(land - IDEAL_LAND) / IDEAL_LAND)


def _relief_term(lo: float, hi: float) -> float:
    """Capped bonus for genuine topographic range."""
    relief = max(0.0, hi - max(lo, 0.0))
    return min(relief / RELIEF_FULL_SCORE_M, 1.0)


def _neighbour_term(coord: tuple[int, int], samples: dict, stride: int) -> float:
    """Mean land fraction of the 8 strided neighbours that were sampled.

    Missing neighbours (scan edge) simply do not contribute, rather than
    counting as ocean -- absence of a sample is not evidence of water.
    """
    x, y = coord
    vals = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            n = samples.get((x + dx * stride, y + dy * stride))
            if n is not None:
                vals.append(n["land"])
    if not vals:
        return 0.0
    return sum(vals) / len(vals)


def score_candidates(scan: dict) -> list[dict]:
    """Rank scan tiles best-first. Pure function -- this is the tested part."""
    stride = int(scan.get("stride", 1))
    samples = {
        (int(t["x"]), int(t["y"])): t for t in scan["tiles"]
    }

    ranked = []
    for coord, t in samples.items():
        land = float(t["land"])
        if land < MIN_LAND:
            continue
        land_s = _land_term(land)
        relief_s = _relief_term(float(t["min"]), float(t["max"]))
        neigh_s = _neighbour_term(coord, samples, stride)
        total = land_s + RELIEF_WEIGHT * relief_s + NEIGHBOUR_WEIGHT * neigh_s
        ranked.append({
            "x": coord[0], "y": coord[1], "land": land,
            "min": float(t["min"]), "max": float(t["max"]),
            "land_score": land_s, "relief_score": relief_s,
            "neighbour_score": neigh_s, "score": total,
        })

    # Deterministic tie-break: score desc, then coordinate, so two runs of the
    # same scan always name the same origin.
    ranked.sort(key=lambda r: (-r["score"], r["x"], r["y"]))
    return ranked


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("scan_json")
    ap.add_argument("--explain", action="store_true")
    args = ap.parse_args()

    with open(args.scan_json) as f:
        scan = json.load(f)

    ranked = score_candidates(scan)
    if not ranked:
        print("ERROR: no tile in the scan had a land fraction above "
              f"{MIN_LAND:.0%}. Widen --radius/--stride and rescan.",
              file=sys.stderr)
        return 1

    if args.explain:
        print(f"{'tile':>12}  {'land':>6}  {'land_s':>7}  {'relief_s':>8}  "
              f"{'nbr_s':>6}  {'SCORE':>6}", file=sys.stderr)
        for r in ranked[:10]:
            print(f"  ({r['x']:+4d},{r['y']:+4d})  {r['land'] * 100:5.1f}%  "
                  f"{r['land_score']:7.3f}  {r['relief_score']:8.3f}  "
                  f"{r['neighbour_score']:6.3f}  {r['score']:6.3f}",
                  file=sys.stderr)

    best = ranked[0]
    # stdout is ONLY the answer, so the caller can do ORIGIN=$(pick_origin ...)
    print(f"{best['x']},{best['y']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
