"""Measure the SHAPE OF THE BOLE up the stem, not just its thickness at 1.3 m.

`tools/plantprobe.py` reports `taper` = d(1.3 m) / d(0.1 m), which is the right
number to compare against a forester's caliper and the wrong one to judge a
taper term by: the bottom 1.3 m of a 20 m tree is 6% of its height, and a taper
curve that is doing its whole job up the bole moves that ratio by about 3%. Read
alone it would have said `trunk.taper` does nothing, which is exactly the
failure this repository keeps producing -- a mechanism that runs, reports
success and changes nothing.

So this walks the stem the way the trunk actually goes and reports d(z) at a
LADDER of heights, plus the two ratios that summarise it:

* `taper`  -- d(1.3) / d(0.1), the same number `plantprobe` gives, for continuity.
* `fq`     -- FORM QUOTIENT, d(H/2) / d(1.3). A cylinder is 1.00. This is the
              number that moves when a taper term works, and the number that
              stayed at 1.00 for the whole life of this generator.

Both are read off the BUILT VOXELS, not off the skeleton, so anything the
rasteriser, the flare or the fluting does to the bole is in the measurement.

Three modes:

    python tools/trunkform.py birch douglas-fir --seeds 1 2 3
        A/B every named spec against itself with `trunk.taper` forced to 0,
        which is an exact no-op path through `skeleton._radii` and therefore
        reproduces the generator as it was before the term existed. Taper is in
        `spec.SEED_EXCLUDED`, so both sides are the SAME INDIVIDUAL and the
        difference is the term and nothing else.

    python tools/trunkform.py --kind tree --seeds 1 2 3 --json out/x.json

    python tools/trunkform.py --hashes out/taper/hashes-before.json
        Every spec's seed hash against a snapshot. Adding a row to `spec.PARAMS`
        puts a new key in all 828 specs and normally moves every one of them to
        a different individual; `spec.SEED_EXCLUDED` is the mechanism that
        avoids that, and this is the measurement that says whether it worked.
        A reseed is invisible otherwise -- every asset still builds and still
        looks like itself -- and the library has been reseeded unnoticed twice.
"""
from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, spec as sm
from plantprobe import _stem_diameter

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"

# Where to caliper the stem. 0.1 m is inside the root flare, 1.3 m is breast
# height, and the rest are fractions of the tree's own built height so the
# ladder means the same thing on a 5 m rowan and a 90 m sequoia.
FIXED_M = (0.1, 1.3)
FRACS = (0.25, 0.50, 0.75)


def profile(data, voxel_m: float, height_m: float) -> dict:
    out = {}
    for z in FIXED_M:
        d, _ = _stem_diameter(data, voxel_m, z)
        out[f"d{z:g}"] = round(d * 100.0, 2)
    for f in FRACS:
        d, _ = _stem_diameter(data, voxel_m, f * height_m)
        out[f"d{int(f * 100)}pc"] = round(d * 100.0, 2)
    dbase, dbh, dhalf = out["d0.1"], out["d1.3"], out["d50pc"]
    out["taper"] = round(dbh / dbase, 4) if dbase else None
    out["fq"] = round(dhalf / dbh, 4) if dbh else None
    return out


def run(name: str, seed: int, taper=None) -> dict:
    """Build one individual, optionally with the taper forced to a value.

    BOTH SIDES GO THROUGH `sm.patch`, EVEN THE ONE THAT CHANGES NOTHING, and
    that is not tidiness -- it is working around a live bug that silently ruined
    the first run of this sweep.

    `spec.validate` is NOT IDEMPOTENT. `quad.eye` is declared `kind="int"` with a
    default of `1.0`, a float; a spec that does not carry the key on disk keeps
    the float through `load`, and the NEXT validate coerces it to `1`. The
    canonical JSON therefore changes from "1.0" to "1" the second time a spec is
    validated -- and the canonical JSON is what `seed_hash` hashes, so
    `sm.patch(spec, {})`, an EMPTY patch, hands back a different individual.
    Measured: 191 of 828 specs on disk are missing the key, 8 of them trees.

    So patching one side and not the other compares two different trees. It did:
    `hawthorn-scrub` came back 9,726 voxels against 18,274 and read as a huge
    taper effect on a species where taper does almost nothing. Patching both
    sides puts them through the same number of validations, which is all this
    needs; the bug itself is reported, not fixed here, because fixing it moves
    191 specs to different individuals and that is the owner's call.
    """
    s, _ = sm.load(SPECS / f"{name}.json")
    want = float(sm.get(s, "trunk.taper")) if taper is None else taper
    s, rep = sm.patch(s, {"trunk.taper": want})
    if rep.warnings:
        raise SystemExit(f"{name}: patch refused: {rep.warnings}")
    a = pipeline.build(s, seed)
    voxel_m = a.stats["voxel_cm"] / 100.0
    h = a.grid.data.shape[2] * voxel_m
    solid = a.grid.data != 0
    nz = solid.any(axis=(0, 1)).nonzero()[0]
    h = float(nz[-1] + 1) * voxel_m if nz.size else h
    row = profile(a.grid.data, voxel_m, h)
    row.update(name=name, seed=seed, height_m=round(h, 2),
               voxels=int(solid.sum()))
    return row


def _fmt(r: dict) -> str:
    return (f"{r['height_m']:6.1f} {r['d0.1']:7.1f} {r['d1.3']:7.1f} "
            f"{r['d25pc']:7.1f} {r['d50pc']:7.1f} {r['d75pc']:7.1f} "
            f"{(r['taper'] or 0):6.3f} {(r['fq'] or 0):6.3f} {r['voxels']:9,d}")


def check_hashes(path: Path) -> int:
    old = json.loads(path.read_text(encoding="utf-8"))
    moved_seed, moved_spec = [], 0
    for p in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(p)
        was = old.get(p.stem)
        if was is None:
            print(f"  {p.stem}: not in the snapshot")
            continue
        if was[0] != sm.seed_hash(s):
            moved_seed.append(p.stem)
        if was[1] != sm.spec_hash(s):
            moved_spec += 1
    print(f"seed hashes moved: {len(moved_seed)} of {len(old)}  "
          f"(a moved seed hash is a different individual of the same species)")
    for n in moved_seed[:20]:
        print(f"  ! {n}")
    print(f"spec hashes moved: {moved_spec} of {len(old)}  "
          f"(expected to move on any PARAMS change; identity, not individual)")
    return 1 if moved_seed else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--kind", action="append", default=None)
    ap.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3])
    ap.add_argument("--json", default=None)
    ap.add_argument("--hashes", default=None,
                    help="path to a seed-hash snapshot to check against")
    ap.add_argument("--no-ab", action="store_true",
                    help="measure as authored only, skip the taper-0 side")
    a = ap.parse_args()

    if a.hashes:
        raise SystemExit(check_hashes(Path(a.hashes)))

    names = list(a.names)
    if a.kind:
        for p in sorted(SPECS.glob("*.json")):
            if json.loads(p.read_text(encoding="utf-8")).get("kind") in a.kind:
                names.append(p.stem)
    if not names:
        ap.error("give spec names or --kind")

    rows = []
    print(f"{'spec':22s} {'':8s} {'H_m':>6s} {'d0.1':>7s} {'d1.3':>7s} "
          f"{'d25%':>7s} {'d50%':>7s} {'d75%':>7s} {'taper':>6s} {'fq':>6s} "
          f"{'voxels':>9s}")
    for n in sorted(set(names)):
        for seed in a.seeds:
            try:
                new = run(n, seed)
            except Exception as exc:
                print(f"{n:22s} seed {seed} BUILD FAILED: {exc}")
                continue
            if a.no_ab:
                print(f"{n:22s} s{seed} {'as-is':5s} {_fmt(new)}")
                rows.append(dict(new, side="authored"))
                continue
            old = run(n, seed, taper=0.0)
            print(f"{n:22s} s{seed} {'cyl':5s} {_fmt(old)}")
            print(f"{'':22s} {'':2s} {'taper':5s} {_fmt(new)}")
            rows.append(dict(old, side="taper0"))
            rows.append(dict(new, side="authored"))

    if not a.no_ab and rows:
        for side in ("taper0", "authored"):
            fq = [r["fq"] for r in rows if r["side"] == side and r["fq"]]
            tp = [r["taper"] for r in rows if r["side"] == side and r["taper"]]
            if fq:
                print(f"\n{side:9s} n={len(fq):4d}  form quotient d(H/2)/DBH "
                      f"median {statistics.median(fq):.3f}  "
                      f"min {min(fq):.3f} max {max(fq):.3f}   |   "
                      f"taper d(1.3)/d(0.1) median {statistics.median(tp):.3f}")
    if a.json:
        Path(a.json).parent.mkdir(parents=True, exist_ok=True)
        Path(a.json).write_text(json.dumps(rows, indent=1), encoding="utf-8")
        print(f"\nwrote {a.json}  ({len(rows)} rows)")


if __name__ == "__main__":
    main()
