"""Re-author the land animals' limb thickness and trunk bulk, by measurement.

WHY THIS IS A TOOL AND NOT A HAND EDIT. Two things changed on 2026-08-15 and
both of them have to land on 131 specs at once:

  1. `quad.leg_thick` CHANGED MEANING. It used to be limb diameter over the
     trunk's depth and it is now limb diameter over the LIMB'S OWN LENGTH --
     see `forge/quadruped.py` and `docs/quadruped-proportion-research.md` for
     why the old reference is what made every tall animal in the library look
     like a wireframe. Every authored value is stale, and 131 numbers cannot be
     converted by arithmetic because the old reference (trunk depth) and the
     new one (limb length) are in a different ratio on every species.
  2. `quad.depth` and `quad.width` are LIFTED where the trunk is too slight for
     the height the animal stands at, measured as chest girth over withers.

SO IT SOLVES, RATHER THAN CONVERTS. Each species is built, measured, adjusted
and built again until the measurement lands, which is the only method that
cannot be fooled by the chain of multiplications that caused the problem in the
first place. Two or three builds a species; the whole run is minutes.

NOTHING IS EVER REDUCED. Every species is lifted to a floor or left exactly
where it is. That is the property that answers the owner's own test -- "if your
fix makes the good ones worse, that is the thing to catch" -- by construction
rather than by inspection: `american-bison`, `brown-bear`, `wild-boar` and
`warthog` are already above both floors and this tool cannot touch them.

    python tools/retune_quad_bulk.py --dry          # print, change nothing
    python tools/retune_quad_bulk.py --trunk        # lift chest girth
    python tools/retune_quad_bulk.py --legs         # solve limb thickness
    python tools/retune_quad_bulk.py --trunk --legs # both, in that order

THE ORDER MATTERS AND IT IS TRUNK FIRST. A deeper chest hangs LOWER, so the
shoulder joint drops and the limb below it gets shorter -- which changes the
denominator the limb solve is working against. Run the other way round the leg
numbers are solved against a body that is about to move.

AND THEN RUN IT AGAIN, because the coupling goes both ways. A thicker limb
stands the animal HIGHER, and withers height is the denominator of the girth
ratio -- so a species that cleared the girth floor before its legs were solved
can be under it afterwards without its trunk having changed at all. `dingo`,
`giant-anteater` and `maned-wolf` all did exactly that: their girth ratios fell
from above the floor to 0.84, 0.90 and 0.83 while their trunks stood still.

It is a fixed point, not a pipeline, and it converges fast -- three rounds of
`--trunk --legs` took the library from 61 species short to one. Run it until a
`--trunk` pass reports nothing deepened, which is the stopping condition and is
printed.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
import quadprobe as qp
from forge import pipeline, quadruped as _q, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"

# The two floors, and both are in `tools/quadprobe.py` so that the tool that
# sets them and the tool that checks them cannot drift.
#
# LIMB_TARGET is not the same number as the probe's SLENDER_MIN and that is
# deliberate. 0.11 is the FAILURE line -- the thinnest-legged animal the owner
# looked at and accepted (wild boar, 0.118), and independently Infinigen's
# photoreal back leg at 0.111. Authoring everything onto a failure line leaves
# no room for the variation draw to push a species back under it.
#
# 0.16 is the working target and it sits between three independent readings:
# above every species the owner accepted at the thin end (boar 0.118, bison
# 0.143, warthog 0.137); above a real horse's fleshed foreleg averaged over its
# taper (~0.12, from a cannon at 0.08 to a forearm at 0.18); and well below
# Veloren's 0.230, which is a shipping voxel game's house style, and Minecraft's
# 0.393, which is a toy. See `docs/quadruped-proportion-research.md`.
LIMB_TARGET = 0.16

# Trunk girth at the middle of the barrel, over withers height.
#
# DELIBERATELY SHORT OF THE REAL ANIMAL, and the report says so. Live
# measurements put heart girth at 1.14 to 1.38 of withers height across cattle,
# horses, sika deer and goats, and a grazer's barrel is fuller than its heart
# girth -- so a faithful number here would be well above 1.2. The library is
# taken to 0.95 instead, because the owner looked at animals measuring 0.80 to
# 1.00 and called four of them solid: going straight to the real ratio would
# rebuild species he did not complain about, on the strength of a livestock
# tape measure rather than a render. 0.95 clears the entire group he rejected
# (0.65 to 0.76) and moves the group he accepted by a few per cent.
GIRTH_TARGET = 0.95

# Seed 1 with the variation draw pinned off, so the solve is against the
# species and not against one individual. Every land-animal spec authors a 9%
# length variation; solving on a varying draw would chase noise.
SEED = 1


def _build(spec: dict):
    flat, _ = sm.patch(spec, {"variation.amount": 0.0})
    return pipeline.build(flat, SEED)


def _measure(spec: dict) -> dict:
    a = _build(spec)
    return {
        "slender": qp.m_limb_slender(a),
        "dia": qp.m_limb_dia(a),
        "girth": qp.m_trunk_girth(a),
        "withers": qp.m_withers_h(a),
    }


def _gw(m: dict) -> float:
    return m["girth"] / m["withers"] if m["withers"] > 0 else float("nan")


# WHAT THE LIBRARY MEASURED BEFORE ANY OF THIS, read off a saved
# `quadprobe --bulk` table rather than recomputed.
BASELINE = ROOT / "out" / "quad-bulk-ab" / "bulk-BEFORE.txt"


def _baseline() -> dict[str, float]:
    """Species -> foreleg thickness over length, as the library shipped it.

    THE "NEVER REDUCE" RULE NEEDS A BASELINE AND THE SPEC IS NOT ONE once
    `quad.leg_thick` has changed what it is a fraction of. Read literally under
    the new meaning, `american-bison`'s authored 0.26 asks for a limb 26% as
    thick as it is long -- thicker than any Minecraft mob -- so a solver told
    to keep the larger of "as authored" and the target leaves the whole library
    enormous and reports that nothing needed doing.

    THE FIRST VERSION RECONSTRUCTED IT INSTEAD, by rebuilding each species at
    `leg_thick × depth_v / limb_v` -- the value that reproduces the old radius
    exactly. That is sound arithmetic and it was still the wrong design, for
    one reason: it is only correct while the specs are untouched, so the tool
    was silently single-use. Run a second time it fed itself its own output and
    produced nonsense (`hippopotamus` came out wanting a limb 1.44 times its
    own length). A re-run is exactly what a solver whose first pass had a bug
    needs to survive.

    So the baseline is a FILE now -- the saved `quadprobe --bulk` table from
    before the change -- which is the same number, is auditable, and does not
    move when this tool runs.
    """
    out: dict[str, float] = {}
    if not BASELINE.exists():
        raise SystemExit(
            f"no baseline table at {BASELINE}\n"
            "  This tool will not reduce a species below what it shipped with,\n"
            "  and it cannot know what that was without the table. Capture one\n"
            "  BEFORE changing anything:  python tools/quadprobe.py --bulk > "
            f"{BASELINE}")
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        f = line.split()
        if len(f) < 13 or not f[0].replace("-", "").isalpha():
            continue
        try:
            out[f[0]] = float(f[7])
        except ValueError:
            continue
    return out


def _write(path: Path, spec: dict, changes: dict) -> None:
    """Change only the named rows, and leave every other byte of the file
    alone -- including the key order, which is how `git diff` stays readable."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    for dotted, value in changes.items():
        group, row = dotted.split(".", 1)
        raw.setdefault(group, {})[row] = value
    path.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def _solve(spec: dict, row: str, want: float, read, lo: float, hi: float,
           rounds: int = 4) -> tuple[float, float]:
    """Move one parameter until a MEASURED quantity reaches `want`.

    A secant step on the assumption that the measurement is roughly
    proportional to the parameter, which it is -- both of these end up as a
    radius or a half-depth multiplied by the row. Roughly, not exactly: the
    voxel grid quantises a limb to whole voxels and the withers height moves
    when the chest deepens, so the loop measures again rather than trusting the
    step. It stops early when the measurement is within 3% of the target,
    because below that the next voxel is further away than the error.
    """
    value = float(sm.get(spec, row))
    tried = [(value, read(_measure(sm.patch(spec, {row: value})[0])))]
    for _ in range(rounds):
        last, got = tried[-1]
        if not np.isfinite(got) or got <= 0:
            break
        if abs(got - want) / want < 0.03:
            break
        value = float(np.clip(last * want / got, lo, hi))
        if abs(value - last) < 1e-4:
            break
        tried.append((value, read(_measure(sm.patch(spec, {row: value})[0]))))
    # THE CHEAPEST TRY THAT ACTUALLY CLEARS THE FLOOR -- not the last one, and
    # not the closest one either. Both of those were tried and both were wrong,
    # and the way they were wrong is the same both times: the species came out
    # unchanged and the run PRINTED IT AS LEFT ALONE, which is a silent no-op
    # wearing the costume of a deliberate decision.
    #
    #   * Returning the LAST try meant a step that overshot and came back down
    #     could land below where it started, so the caller's never-reduce guard
    #     fired and skipped the species.
    #   * Returning the CLOSEST try is worse, because these floors are one
    #     sided. `przewalskis-horse` sat at 0.875 of the girth floor and one
    #     step took it to 1.035; 0.875 is nearer to 0.95 than 1.035 is, so the
    #     closest-match rule chose to leave a FAILING animal alone in
    #     preference to a passing one.
    #
    # So: of the tries that reach the floor, take the smallest -- the least
    # change that does the job. If none reach it, take the one that got
    # furthest, and the caller reports how far short it stopped.
    ok = [t for t in tried if np.isfinite(t[1]) and t[1] > 0]
    if not ok:
        return tried[0]
    clears = [t for t in ok if t[1] >= want * 0.97]
    return min(clears, key=lambda t: t[0]) if clears else max(ok, key=lambda t: t[1])


def _quad_specs() -> list[str]:
    out = []
    for p in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(p)
        if sm.get(s, "kind") == "quadruped":
            out.append(p.stem)
    return out


def trunk_pass(names: list[str], dry: bool) -> None:
    """Lift `quad.depth` until chest girth reaches the floor. Never lowers it.

    `quad.width` rides along at its authored ratio to the depth, because that
    row is width OVER DEPTH and a deeper chest at the same ratio is a wider one
    -- which is what a real barrel does. Only the depth is solved.
    """
    print(f"\nTRUNK: lift chest girth to {GIRTH_TARGET:.2f} of withers "
          f"(nothing is reduced)\n")
    print(f"{'species':<26} {'depth':>13} {'girth/withers':>22}")
    moved = 0
    stuck: list[tuple[str, float, float]] = []
    for n in names:
        path = SPECS / f"{n}.json"
        spec, _ = sm.load(path)
        before = _measure(spec)
        gw0 = _gw(before)
        # WITHIN THE SOLVER'S OWN TOLERANCE COUNTS AS THERE. `_solve` stops
        # inside 3% of the target -- below that the next whole voxel of trunk
        # is further away than the error -- so a species sitting at 0.93 is
        # converged, not short, and calling it short would put forty rows of
        # false alarm under a heading that is meant to name real ones.
        if not np.isfinite(gw0) or gw0 >= GIRTH_TARGET * 0.97:
            continue
        d0 = float(sm.get(spec, "quad.depth"))
        d1, g1 = _solve(spec, "quad.depth", GIRTH_TARGET,
                        lambda m: _gw(m), 0.15, 0.90)
        if d1 <= d0 + 1e-4:
            # UNDER THE FLOOR AND NOT LIFTED IS A REPORTED OUTCOME, not a
            # blank line. Skipping quietly here is how a species stays broken
            # across three runs of a tool whose whole output says it fixed
            # everything.
            stuck.append((n, gw0, d0))
            continue
        moved += 1
        # A PARAMETER THAT STOPPED AT ITS OWN CEILING IS SAID SO, OUT LOUD.
        # Silently clamping at authoring time is this project's documented
        # trap -- a `fish.length_m` ceiling of 3 m held every whale in the
        # library and the only sign was a warning nobody read. A maned wolf
        # genuinely cannot reach the girth floor: it is 0.95 of its own body
        # length at the shoulder, and a trunk deeper than 0.90 of its length
        # is not a mammal.
        ceiling = " AT THE ROW'S CEILING -- cannot reach the floor" \
            if d1 >= float(sm.BY_PATH["quad.depth"].hi) - 1e-6 else ""
        print(f"{n:<26} {d0:>6.2f} -> {d1:<5.2f} {gw0:>10.2f} -> {g1:<10.2f}"
              f"{ceiling}")
        if not dry:
            _write(path, spec, {"quad.depth": round(d1, 3)})
    print(f"\n  {moved} species deepened, "
          f"{len(names) - moved - len(stuck)} already above the floor")
    for n, gw0, d0 in stuck:
        print(f"  STILL SHORT: {n} at {gw0:.2f} of withers, quad.depth {d0:.2f}"
              f" -- no deeper trunk this row allows reaches the floor")


def leg_pass(names: list[str], dry: bool) -> None:
    """Solve `quad.leg_thick` under its NEW meaning.

    Every authored value is stale after the reference changed, so this runs on
    all 131 whether or not they are thin -- but the TARGET is
    `max(what the species already measured, the floor)`, so a species that was
    already thick enough keeps its own number and only the thin ones move.
    """
    print(f"\nLEGS: solve limb thickness to max(as shipped, {LIMB_TARGET:.2f}) "
          f"of limb length\n")
    print(f"  baseline: {BASELINE}\n")
    print(f"{'species':<26} {'leg_thick':>15} {'thickness/length':>24} "
          f"{'vox across':>13}")
    base = _baseline()
    for n in names:
        path = SPECS / f"{n}.json"
        spec, _ = sm.load(path)
        s0 = base.get(n, float("nan"))
        if not np.isfinite(s0):
            print(f"{n:<26}   not in the baseline table -- left alone")
            continue
        want = max(s0, LIMB_TARGET)
        t0 = float(sm.get(spec, "quad.leg_thick"))
        t1, s1 = _solve(spec, "quad.leg_thick", want,
                        lambda m: m["slender"], 0.05, 0.90)
        after = _measure(sm.patch(spec, {"quad.leg_thick": round(t1, 3)})[0])
        print(f"{n:<26} {t0:>6.2f} -> {t1:<6.3f} {s0:>10.3f} -> {s1:<10.3f} "
              f"{'':>5} -> {after['dia']:<5.1f}")
        if not dry:
            _write(path, spec, {"quad.leg_thick": round(t1, 3)})


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry", action="store_true", help="print, write nothing")
    ap.add_argument("--trunk", action="store_true")
    ap.add_argument("--legs", action="store_true")
    ap.add_argument("--only", nargs="*", help="restrict to these species")
    args = ap.parse_args()
    if not (args.trunk or args.legs):
        ap.error("pick --trunk, --legs or both")

    names = args.only or _quad_specs()
    if not names:
        print("no quadruped specs found", file=sys.stderr)
        return 2
    if args.trunk:
        trunk_pass(names, args.dry)
    if args.legs:
        leg_pass(names, args.dry)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
