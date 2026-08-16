"""Fit statistics for a `tools/plantprobe.py --kind tree --json` file.

The same arithmetic `docs/plant-proportion-research.md` §5.2 reports -- median
ours/target and mean absolute error against the open-grown stem-diameter target
-- read off a probe file that already exists, so measuring the fit again after a
generator change costs no extra builds. The species set is the same 52: has a
`dbh_vs_h` reference in `tools/plantref.json` and is not in `plantfit.EXCLUDED`.

    python tools/_fitstats.py out/taper/tree-before.json out/taper/tree-after.json
"""
from __future__ import annotations

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import _path  # noqa: F401
from plantfit import EXCLUDED, target_dbh_cm


def stats(path: str):
    rows = json.loads(Path(path).read_text(encoding="utf-8"))
    per = defaultdict(list)
    for r in rows:
        if r.get("side") in ("taper0",):      # trunkform A/B files carry both sides
            continue
        per[r["name"]].append(r)
    out = {}
    for name, rs in per.items():
        if name in EXCLUDED:
            continue
        h = statistics.mean(r["height_m"] for r in rs)
        dbh = statistics.mean(r["dbh_cm"] for r in rs if r.get("dbh_cm"))
        tgt = target_dbh_cm(name, h)
        if not tgt or not dbh:
            continue
        out[name] = (dbh / tgt, dbh, tgt, h)
    return out


def main():
    for path in sys.argv[1:]:
        s = stats(path)
        r = [v[0] for v in s.values()]
        err = [abs(x - 1.0) for x in r]
        print(f"{path}")
        print(f"  n {len(r)}   median ours/target {statistics.median(r):.2f}x   "
              f"mean abs error {statistics.mean(err):.1%}   "
              f"within 10% {sum(e <= .10 for e in err)}/{len(r)}   "
              f"within 25% {sum(e <= .25 for e in err)}/{len(r)}")
    if len(sys.argv) == 3:
        a, b = stats(sys.argv[1]), stats(sys.argv[2])
        both = sorted(set(a) & set(b), key=lambda n: -abs(b[n][0] - a[n][0]))
        print(f"\nlargest moves ({len(both)} species in both):")
        print(f"  {'spec':22s}{'DBH before':>11s}{'after':>8s}{'target':>8s}"
              f"{'x before':>10s}{'x after':>9s}")
        for n in both[:15]:
            print(f"  {n:22s}{a[n][1]:11.1f}{b[n][1]:8.1f}{b[n][2]:8.1f}"
                  f"{a[n][0]:10.2f}{b[n][0]:9.2f}")


if __name__ == "__main__":
    main()
