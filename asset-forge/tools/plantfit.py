"""Fit tree trunk thickness to measured stem-diameter allometry.

The defect this exists to fix, measured in `docs/plant-proportion-research.md`
§4: the library's trunks are a **median 3.05x thicker** than the stem diameter
a real tree of that species carries at that height, across 55 species with a
reference, and **not one of the 55 was within 10%**. It is the only one of the
four tree ratios that is wrong in one direction for essentially everything,
which is what makes it worth a bulk pass rather than a species-by-species
judgement.

**The reference.** `tools/plantref.json`, built from two independent sources
that agree with each other to 4% (§2.4): the Tallo database (CC BY 4.0) for the
stem-diameter-on-height power law per species, and the USDA FIA database
(public domain) as the cross-check. The target is

    target DBH = a * H^b * OPEN_GROWN

evaluated at the asset's OWN BUILT height, not at its authored `height_m` --
the two differ, see §3.2.

**`OPEN_GROWN = 1.50` and where it comes from.** Both reference datasets are
mostly forest plots, and a forest tree is thin for its height because it is
racing its neighbours for light. These assets are scattered detail entities, so
the open-grown figure is the honest target. It is measured, not assumed: FIA
records a crown class per tree and code 1 is "open grown", so the same power
law was fitted to open-grown trees alone and to all trees, for the 19 species
with at least 80 open-grown stems. At 15 m the open-grown tree is **1.50x**
thicker (range 1.27-2.19). Even after that allowance the library is still
about 2x too thick.

**What is deliberately NOT fitted.** Five species carry a fat bole because the
fat bole IS the species, and each says so in its own notes -- see EXCLUDED
below. Correcting those to a forest average would be the same mistake as
applying one limb thickness to 131 animals, in the other direction.

    python tools/plantfit.py report          # measure, change nothing
    python tools/plantfit.py fit             # dry run, print what would move
    python tools/plantfit.py fit --apply     # write the specs
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm
from plantprobe import probe

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
REF = json.loads((Path(__file__).resolve().parent / "plantref.json")
                 .read_text(encoding="utf-8"))

# FIA crown class 1 ("open grown") against all crown classes, 19 species,
# median of the per-species ratio of predicted DBH at 15 m. See the module
# docstring and docs/plant-proportion-research.md §2.5.
OPEN_GROWN = 1.50

# No single pass may move a trunk by more than this, in either direction. The
# same guard as `tools/reffit.py`'s MAX_LIFT and for the same reason: a
# reference can be wrong about one species, and a capped species gets printed
# as capped so a person looks at it.
MAX_MOVE = 2.5

# At 10 cm a trunk radius below 0.10 m is a single voxel and the bole stops
# being a cylinder. Anything the reference wants thinner than this cannot be
# drawn -- reported, not silently clamped away.
MIN_RADIUS_M = 0.10

EXCLUDED = {
    "baobab": "the water-storing trunk IS the species; its own notes say 'the "
              "trunk is most of the tree'",
    "european-yew": "notes author the 1.1 m radius deliberately, as what many "
                    "fused stems read like from outside; ancient yews do reach it",
    "olive": "notes: 'THE GNARL IS THE SPECIES'; the heavy flared hollow base "
             "is the whole silhouette",
    "strangler-fig": "the base is fused root, not stem -- notes put roots.count "
                     "and roots.rise both at their ceiling to get it",
    "hero-sequoia": "a hero asset the owner sized; its 12.1 m base is close to "
                    "General Sherman's and is not an accident. Its taper is a "
                    "separate finding, see research §5.3",
}


def target_dbh_cm(spec_name: str, built_h_m: float) -> float | None:
    r = REF.get(spec_name, {})
    f = r.get("dbh_vs_h")
    if not f or not built_h_m:
        return None
    return f["a"] * built_h_m ** f["b"] * OPEN_GROWN


def measure(name: str, seeds) -> dict:
    rows = probe(name, seeds)
    def m(k):
        v = [r[k] for r in rows if r.get(k) is not None]
        return statistics.mean(v) if v else None
    return dict(h=m("height_m"), dbh=m("dbh_cm"), taper=m("taper"),
                cwh=m("cwmean_over_h"), cr=m("crown_ratio"))


def tree_specs():
    out = []
    for p in sorted(SPECS.glob("*.json")):
        d = json.loads(p.read_text(encoding="utf-8"))
        if d.get("kind") == "tree":
            out.append(p.stem)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["report", "fit"])
    ap.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3])
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--only", nargs="*", default=None)
    a = ap.parse_args()

    names = a.only or tree_specs()
    moved, capped, floored, skipped = [], [], [], []
    print(f"{'spec':22s} {'H_m':>6s} {'DBH':>7s} {'target':>7s} {'x':>5s} "
          f"{'r_base':>7s} {'->':>7s}")
    for n in names:
        if n in EXCLUDED:
            skipped.append((n, EXCLUDED[n]))
            continue
        me = measure(n, a.seeds)
        tgt = target_dbh_cm(n, me["h"])
        if tgt is None or not me["dbh"]:
            skipped.append((n, "no stem-diameter reference in plantref.json"))
            continue
        scale = tgt / me["dbh"]
        note = ""
        if 0.9 <= scale <= 1.1:
            # Already inside the band. Do not write: DBH quantises to whole
            # voxels at 10 cm, so a species sitting on a boundary would
            # otherwise be nudged back and forth by every re-run.
            print(f"{n:22s} {me['h']:6.1f} {me['dbh']:7.1f} {tgt:7.1f} "
                  f"{scale:5.2f} {'':7s} {'':7s} in band")
            continue
        if scale < 1.0 / MAX_MOVE:
            scale, note = 1.0 / MAX_MOVE, " CAPPED"
        elif scale > MAX_MOVE:
            scale, note = MAX_MOVE, " CAPPED"
        s, _ = sm.load(SPECS / f"{n}.json")
        old = float(sm.get(s, "trunk.radius_base_m"))
        new = round(old * scale, 3)
        if new < MIN_RADIUS_M:
            new, note = MIN_RADIUS_M, note + " FLOORED (1 voxel at 10 cm)"
        print(f"{n:22s} {me['h']:6.1f} {me['dbh']:7.1f} {tgt:7.1f} "
              f"{tgt/me['dbh']:5.2f} {old:7.3f} {new:7.3f}{note}")
        if abs(new - old) < 1e-4:
            continue
        (capped if "CAPPED" in note else
         floored if "FLOORED" in note else moved).append((n, old, new))
        if a.cmd == "fit" and a.apply:
            s2, rep = sm.patch(s, {"trunk.radius_base_m": new})
            # `validate` never raises -- it clamps and reports -- so a value
            # out of range comes back as a warning on a spec that still saves.
            # Refuse rather than write a spec that does not say what was asked.
            if rep.warnings:
                print(f"    REFUSED: {rep.warnings}")
                continue
            got = float(sm.get(s2, "trunk.radius_base_m"))
            if abs(got - new) > 1e-6:
                print(f"    REFUSED: clamped to {got} instead of {new}")
                continue
            sm.save(s2, SPECS / f"{n}.json")

    print(f"\nmoved {len(moved)}   capped {len(capped)}   "
          f"floored {len(floored)}   skipped {len(skipped)}")
    for n, why in skipped:
        print(f"  skip {n:22s} {why}")
    if a.cmd == "fit" and not a.apply:
        print("\n(dry run -- pass --apply to write)")


if __name__ == "__main__":
    main()
