"""How far the BIRD and FISH specs are from measured references.

`tools/reffit.py` did this for land animals against CC0 silhouettes. Nothing
outside the quadrupeds had ever been fitted to anything: `docs/biomes/README.md`
section 8 says outright that every size in the library is an unsourced
approximation, and 127 bird specs and 124 fish and cetacean specs were authored
the same way -- one agent's judgement against other shipped specs.

WHY THIS IS A TABLE COMPARISON AND NOT A SILHOUETTE PIPELINE. `tools/refsil.py`
finds a BELLY LINE -- the lowest row where two runs of silhouette become one --
and then tracks legs through a band above it. A fish has no legs and a bird in a
PhyloPic drawing is usually flying, wings spread, which is a shape whose lateral
extent is the wing and not the animal. So the quadruped instrument does not
transfer, and building a second one is real work that should be done against a
measurement that says it is needed.

What DOES transfer is the source discipline. Both kinds already have measured
proportions in this repository, read from real morphometric datasets and typed
out by hand precisely because the datasets themselves are not licence-clean:

  * fish -- FishShapes v1 (16,523 specimens) joined to FishBase's shape classes,
    confirmed by FISHMORPH (8,342 species); plus 7,452 landmarked specimens for
    fin positions. `docs/fish-shape-research.md` sections 1, 2 and 4.
  * birds -- 94 Cornell species accounts joined to AVONET, 88 species; plus
    Alerstam et al. 2007 PLoS Biology Protocol S1, 129 species with complete
    biometry from 33,610 individual measurements. `docs/bird-shape-research.md`
    sections 3 and 5.

Those medians are checked in as `refs/fish-reference.json` and
`refs/bird-reference.json`, per-source, with the group or class of every species
assigned BY HAND and the unassigned ones printed rather than defaulted.

    python tools/refshape.py fish report
    python tools/refshape.py birds report
    python tools/refshape.py fish fit --dry
    python tools/refshape.py birds fit --dry

THE MEDIAN ALONE WOULD CONCLUDE THE LIBRARY IS FINE. The land-animal pass
measured a median of 1.04x life and a MEAN ABSOLUTE ERROR of 37% on the same
ratio, and reporting only the first would have closed the exercise with a
sentence that was true and useless. Every table below prints both.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

import _path  # noqa: F401
from forge import pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
REFS = ROOT / "refs"
BIRDREF = REFS / "bird-reference.json"
FISHREF = REFS / "fish-reference.json"

SEED = 1

# HOW FAR A SPEC MAY BE MOVED IN ONE PASS, as a multiple of what it holds. Same
# shape and same reason as `reffit.MAX_LIFT`: a species should be able to cross
# most of the distance to the reference in one pass, and no single wrong row in
# a hand-assigned group table may rebuild an animal. It is a bound, not a
# measurement, and it is recorded as such.
MAX_MOVE = 1.6

# A GROUP MEDIAN IS NOT A SPECIES MEASUREMENT, and this is the whole reason the
# fit below is narrow. The bird table has nineteen groups for 8,000-odd birds
# and the fish table has four classes for 30,000-odd fish, so the reference for
# one species is the middle of its class and not the animal. A spec inside the
# published p10-p90 band for its class is INSIDE THE DATA and is left alone;
# only species outside the band are moved, and they are moved TO THE EDGE OF THE
# BAND rather than to the median. Fitting every species to a class median would
# collapse a real range onto a constant, which is the exact defect
# `docs/reference-fitting-research.md` section 4 found in `quad.leg_thick`:
# "the defect is not the value of the constant; it is that it is a constant."
FIT_TO_BAND = True


def _specs(kinds: tuple[str, ...]) -> list[tuple[str, Path]]:
    out = []
    for p in sorted(SPECS.glob("*.json")):
        try:
            raw = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if raw.get("kind") in kinds:
            out.append((p.stem, p))
    return out


def _build(spec: dict, seed: int = SEED):
    flat, _ = sm.patch(spec, {"variation.amount": 0.0})
    return pipeline.build(flat, seed)


def _stats(name: str, ours: list[float], want: list[float]) -> str:
    """Median AND mean absolute error, always both. See the module docstring."""
    if not ours:
        return f"  {name:<34} no species measurable"
    r = np.array(ours) / np.array(want)
    return (f"  {name:<34} median {np.median(r):>5.2f}x   "
            f"MAE {100 * np.mean(np.abs(r - 1)):>4.0f}%   "
            f"within 10% {int((np.abs(r - 1) <= 0.10).sum()):>3} of {len(r)}   "
            f"range {r.min():.2f}-{r.max():.2f}x")


# ------------------------------------------------------------------- fish

def _fish_rows(spec: dict) -> dict:
    """Every fish quantity that has a reference, read off the SPEC.

    READ OFF THE SPEC AND NOT OFF THE BUILD, deliberately, and the reason is in
    `refs/fish-reference.json`: `fish.depth_ratio` is depth over `fish.length_m`,
    and `fish.length_m` is the body before the caudal fin is added, which is
    exactly the standard length FishShapes and the landmark table are measured
    against. A built asset's x extent is nose to tail TIP, a different quantity,
    and dividing by it would put a 20% caudal fin into every ratio. `_fish_drawn`
    below checks separately that the rows are actually honoured.
    """
    g = lambda r: float(sm.get(spec, r))                       # noqa: E731
    return {
        "depth_over_length": g("fish.depth_ratio"),
        "depth_over_width": 1.0 / max(g("fish.width_ratio"), 1e-6),
        "deepest": g("fish.depth_at"),
        "dorsal": g("fish.dorsal_start"),
        "operculum": g("fish.head_frac"),
        "peduncle": g("fish.peduncle"),
        "caudal_height": g("fish.caudal_span") * g("fish.depth_ratio"),
    }


# A class with no published numbers is a class that is REPORTED AND NEVER
# FITTED. See `refs/fish-reference.json`'s `depressiform` entry for the move
# that was caught by looking at it: a manta ray fitted to the short-deep band
# comes out as a disc stood on its edge.
NO_REFERENCE_CLASS = "depressiform"

# THE SMALLEST MOVE WORTH WRITING A SPEC FILE FOR. Below this the fit is
# churning a file to chase the third decimal of a band edge -- `golden-carp`
# 0.420 -> 0.417 -- and every such write costs a spec its seed salt, so every
# individual of that species changes for a change nobody can see.
MIN_MOVE = 0.03

# HOW FAR OUTSIDE ITS CLASS BAND A SPECIES HAS TO BE BEFORE IT IS MOVED.
#
# The band is already a p10-p90 over thousands of specimens, so a species just
# outside it is a species in the tail of its class -- which is where real
# outliers live and is not an authoring error. `ocean-sunfish` measures a
# length:depth of 1.3 against a short-deep p10 of 1.5, and a Mola mola really is
# as deep as it is long: pulling it to the p10 would have made the most
# distinctive silhouette in the library more ordinary, on the authority of a
# percentile. 1.20 is a judgement and it is recorded as one; it is set so that
# the species outside the DATA move and the species merely in the tail of their
# class do not.
BAND_TOL = 1.20


def fish_report(fit_mode: bool = False, dry: bool = True) -> int:
    ref = json.loads(FISHREF.read_text(encoding="utf-8"))
    cls, lm = ref["classes"], ref["landmarks"]
    rows, unassigned, cet = [], [], []
    for name, path in _specs(("fish", "cetacean")):
        raw = json.loads(path.read_text(encoding="utf-8"))
        if raw.get("kind") == "cetacean":
            cet.append(name)
            continue
        c = ref["species"].get(name)
        if not c:
            unassigned.append(name)
            continue
        spec, _ = sm.load(path)
        rows.append((name, c, _fish_rows(spec)))

    print("\nHOW FAR THE FISH SPECS ARE FROM THE MEASURED MEDIANS")
    print("  references: FishShapes v1 / FishBase shape classes (16,523 specimens),")
    print("  7,452 landmarked specimens for fin positions. Hand-typed medians only;")
    print("  see refs/fish-reference.json for why neither dataset is vendored.\n")
    print(f"{'species':<28}{'class':<11}{'L:D ours':>9}{'L:D ref':>8}{'x':>6}"
          f"{'deep@':>7}{'dors@':>7}{'operc':>7}{'pedu':>6}{'caud':>6}")
    got: dict[str, tuple[list, list]] = {k: ([], []) for k in
                                         ("length_over_depth", "depth_over_width",
                                          "deepest", "dorsal", "operculum",
                                          "peduncle", "caudal_height")}
    for name, c, o in rows:
        ld = 1.0 / max(o["depth_over_length"], 1e-6)
        want_ld = cls[c]["length_over_depth"]
        if want_ld:
            got["length_over_depth"][0].append(ld)
            got["length_over_depth"][1].append(want_ld)
        if cls[c]["depth_over_width"]:
            got["depth_over_width"][0].append(o["depth_over_width"])
            got["depth_over_width"][1].append(cls[c]["depth_over_width"])
        for k, key in (("deepest", "deepest"), ("dorsal", "dorsal"),
                       ("operculum", "operculum"), ("caudal_height", "caudal_height")):
            got[k][0].append(o[key])
            got[k][1].append(lm[key]["median"])
        got["peduncle"][0].append(o["peduncle"])
        got["peduncle"][1].append(ref["all_teleost"]["peduncle_over_depth"])
        print((f"{name:<28}{c:<11}{ld:>9.1f}"
               + (f"{want_ld:>8.1f}{ld / want_ld:>6.2f}" if want_ld
                  else f"{'-':>8}{'-':>6}"))
              .replace("nan", "  -")
              + f"{o['deepest']:>7.2f}{o['dorsal']:>7.2f}{o['operculum']:>7.2f}"
              + f"{o['peduncle']:>6.2f}{o['caudal_height']:>6.2f}")
    print(f"\n  reference medians: deep@ {lm['deepest']['median']:.3f}  "
          f"dors@ {lm['dorsal']['median']:.3f}  "
          f"operc {lm['operculum']['median']:.3f}  "
          f"pedu {ref['all_teleost']['peduncle_over_depth']:.2f}  "
          f"caud {lm['caudal_height']['median']:.3f}\n")
    for k in got:
        print(_stats(k, got[k][0], got[k][1]))
    print(f"\n  {len(rows)} fish measured, {len(unassigned)} with no hand-assigned "
          f"class{': ' + ', '.join(unassigned) if unassigned else ''}")
    print(f"  {len(cet)} cetaceans NOT measured -- they are mammals and none of "
          f"these medians apply to them")
    if fit_mode:
        return _fish_fit(rows, ref, dry)
    return 0


def _band(lo: float, hi: float, v: float) -> float | None:
    """Where a value should move to, or None if it is already inside the band."""
    if v < lo:
        return lo
    if v > hi:
        return hi
    return None


def _fish_fit(rows, ref, dry: bool) -> int:
    """Move the two rows the measurement says are worth moving, to the BAND.

    ONE ROW ONLY, AND THE SHORTLIST IS THE FINDING RATHER THAN A SCOPING
    DECISION. `length:depth` is the ratio the sources say separates one fish
    from another -- it spans a factor of eight across the four classes -- and it
    is the one the library is worst on: median 1.07x the class median but a MEAN
    ABSOLUTE ERROR of 46%, with only 27 of 106 species inside 10% and a range
    from 0.30x to 5.56x. That is the same shape of defect the land animals had,
    and the median alone would have closed the exercise.

    `fish.depth_at` is NOT fitted even though it is second on the list, and the
    reason is in `honoured`: there is no fin-free measurement of where the body
    is deepest, so a fit could write a number and never be checked. It measures
    0.87x with a 14% mean absolute error, which is the closest ratio in the set
    anyway.

    `fish.head_frac`, `fish.peduncle`, `fish.dorsal_start` and the caudal height
    are all reported and none is fitted: three of them are inside the published
    p10-p90 band for most species, and a row inside the data is a row a fit can
    only damage.
    """
    cls, lm = ref["classes"], ref["landmarks"]
    print("\nFIT fish.depth_ratio TO THE PUBLISHED p10-p90 BAND OF ITS CLASS\n")
    print(f"{'species':<28}{'row':<20}{'was':>8}{'->':>4}{'now':>8}   why")
    moved = 0
    for name, c, o in rows:
        path = SPECS / f"{name}.json"
        changes = {}
        if c == NO_REFERENCE_CLASS:
            continue
        ld = 1.0 / max(o["depth_over_length"], 1e-6)
        edge = _band(cls[c]["p10"] / BAND_TOL, cls[c]["p90"] * BAND_TOL, ld)
        if edge is not None:
            edge = float(np.clip(edge, cls[c]["p10"], cls[c]["p90"]))
            want = 1.0 / edge
            cur = o["depth_over_length"]
            lo, hi = cur / MAX_MOVE, cur * MAX_MOVE
            new = round(float(np.clip(want, lo, hi)), 3)
            p = sm.BY_PATH["fish.depth_ratio"]
            new = round(float(np.clip(new, p.lo, p.hi)), 3)
            if abs(new - cur) > MIN_MOVE * cur:
                changes["fish.depth_ratio"] = new
                print(f"{name:<28}{'fish.depth_ratio':<20}{cur:>8.3f}{'->':>4}"
                      f"{new:>8.3f}   L:D {ld:.1f} outside {c} "
                      f"{cls[c]['p10']:.1f}-{cls[c]['p90']:.1f}")
        if changes:
            moved += 1
            if not dry:
                raw = json.loads(path.read_text(encoding="utf-8"))
                for dotted, value in changes.items():
                    grp, row = dotted.split(".", 1)
                    raw.setdefault(grp, {})[row] = value
                path.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                                encoding="utf-8")
    print(f"\n  {moved} species moved"
          f"{'  (DRY RUN -- nothing written)' if dry else ''}")
    return 0


# ------------------------------------------------------------------ birds

def _bird_rows(spec: dict) -> dict:
    """Span over total length, tail over total length, and the aspect ratio.

    `bird.wing_span` IS span over total length, by `forge/bird.py`'s own
    definition (`wing_half = 0.5 * bird.wing_span * length_v`), so it compares
    directly to the source table. The tail share has to be NORMALISED first --
    `forge/bird.py` divides all five shares by their sum -- and a raw
    `bird.tail_frac` read straight out of the spec would be wrong by whatever
    the other four happen to add up to.
    """
    g = lambda r: float(sm.get(spec, r))                       # noqa: E731
    shares = {k: g(f"bird.{k}_frac") for k in
              ("bill", "head", "neck", "body", "tail")}
    total = max(sum(shares.values()), 1e-6)
    return {
        "span_over_length": g("bird.wing_span"),
        "tail_over_length": shares["tail"] / total,
        "wing_aspect": g("bird.wing_aspect"),
    }


def birds_report(fit_mode: bool = False, dry: bool = True) -> int:
    ref = json.loads(BIRDREF.read_text(encoding="utf-8"))
    groups = ref["groups"]
    rows, unassigned = [], []
    for name, path in _specs(("bird",)):
        g = ref["species"].get(name)
        if not g:
            unassigned.append(name)
            continue
        spec, _ = sm.load(path)
        rows.append((name, g, _bird_rows(spec)))

    print("\nHOW FAR THE BIRD SPECS ARE FROM THE MEASURED MEDIANS")
    print("  references: 94 Cornell accounts joined to AVONET (88 species) for span")
    print("  and tail; Alerstam et al. 2007 PLoS Biology Protocol S1 (129 species,")
    print("  33,610 measurements) for aspect ratio. Hand-typed medians only.\n")
    print(f"{'species':<28}{'group':<17}{'span ours':>10}{'ref':>6}{'x':>6}"
          f"{'tail ours':>10}{'ref':>6}{'x':>6}{'AR ours':>8}{'ref':>6}{'x':>6}")
    got: dict[str, tuple[list, list]] = {k: ([], []) for k in
                                         ("span_over_length", "tail_over_length",
                                          "wing_aspect")}
    for name, g, o in rows:
        r = groups[g]
        cells = []
        for k in ("span_over_length", "tail_over_length", "wing_aspect"):
            w = r.get(k)
            if w:
                got[k][0].append(o[k])
                got[k][1].append(w)
                cells.append(f"{o[k]:>10.2f}{w:>6.2f}{o[k] / w:>6.2f}")
            else:
                cells.append(f"{o[k]:>10.2f}{'-':>6}{'-':>6}")
        print(f"{name:<28}{g:<17}" + "".join(cells))
    print()
    for k in got:
        print(_stats(k, got[k][0], got[k][1]))
    print(f"\n  {len(rows)} birds measured, {len(unassigned)} with no group in a "
          f"19-group table built from 88 north-temperate species:")
    print(f"    {', '.join(unassigned)}")
    print("  Those are reported as unmeasured rather than fitted against the "
          "nearest-looking row.")
    if fit_mode:
        return _birds_fit(rows, groups, dry)
    return 0


def _birds_fit(rows, groups, dry: bool) -> int:
    """Move `bird.wing_span` and `bird.wing_aspect` toward the group median.

    NO BAND HERE, BECAUSE THE SOURCE PUBLISHES NONE. The bird table carries a
    group median and a group range but no per-group percentiles, so there is no
    p10-p90 to fit to the edge of -- and a range over n = 2 is not a
    distribution. The rule used instead is a factor: a species more than 40%
    away from its group's median is brought to within 40% of it, and one inside
    that is left alone. 40% IS A JUDGEMENT and it is recorded as one; it is set
    so that the genuinely mis-authored (a 4x span) moves and the ordinary
    between-species spread inside a group does not.

    `bird.tail_frac` is deliberately NOT fitted. It is one of five shares that
    are normalised against their own sum, so moving it silently rescales the
    bill, head, neck and body of the same bird -- the change would land
    somewhere nobody asked for it. That needs a solver against the built asset,
    which is the next piece of work and not this one.
    """
    print("\nFIT bird.wing_span AND bird.wing_aspect TOWARD THE GROUP MEDIAN\n")
    print(f"{'species':<28}{'row':<20}{'was':>8}{'->':>4}{'now':>8}   why")
    tol, moved = 0.40, 0
    for name, g, o in rows:
        path = SPECS / f"{name}.json"
        changes = {}
        for k, row in (("span_over_length", "bird.wing_span"),
                       ("wing_aspect", "bird.wing_aspect")):
            w = groups[g].get(k)
            if not w:
                continue
            cur = o[k]
            if abs(cur - w) <= tol * w:
                continue
            want = w * (1 + tol) if cur > w else w * (1 - tol)
            p = sm.BY_PATH[row]
            new = round(float(np.clip(want, max(p.lo, cur / MAX_MOVE),
                                      min(p.hi, cur * MAX_MOVE))), 2)
            if abs(new - cur) > 1e-3:
                changes[row] = new
                print(f"{name:<28}{row:<20}{cur:>8.2f}{'->':>4}{new:>8.2f}   "
                      f"{g} median {w:.2f}, was {cur / w:.2f}x")
        if changes:
            moved += 1
            if not dry:
                raw = json.loads(path.read_text(encoding="utf-8"))
                for dotted, value in changes.items():
                    grp, r = dotted.split(".", 1)
                    raw.setdefault(grp, {})[r] = value
                path.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                                encoding="utf-8")
    print(f"\n  {moved} species moved"
          f"{'  (DRY RUN -- nothing written)' if dry else ''}")
    return 0


def honoured(kind: str) -> int:
    """Is the row the spec carries the thing the generator actually draws?

    THE SAME CHECK `reffit dead` EXISTS FOR. `quad.leg_thick` sat on its own
    lower bound in 49 specs for a day and drew identical animals, because a
    floor in `forge/quadruped.py` was doing the work. A fit that writes a number
    nothing reads is this project's signature failure, so before any bird or
    fish row is fitted it is swept: the row is set to 0.7x and 1.4x of itself
    and the asset is measured. A row that does not move the measurement is
    printed DEAD.
    """
    import fishprobe as fp
    import birdprobe as bp
    if kind == "fish":
        pairs = [("fish.depth_ratio", fp.m_body_depth, 1.0),
                 ("fish.depth_at", fp.m_depth, 1.0),
                 ("fish.dorsal_start", fp.m_dorsal_at, 0.03),
                 ("fish.head_frac", fp.m_head_span, 1.0),
                 ("fish.peduncle", fp.m_wrist, 1.0),
                 ("fish.caudal_span", fp.m_tail_span, 1.0)]
        probe_spec = "brown-trout"
    else:
        pairs = [("bird.wing_span", bp.m_span, 1.0),
                 ("bird.wing_aspect", bp.m_wing_chord, 1.0),
                 ("bird.tail_frac", bp.m_tail_run, 1.0)]
        probe_spec = "european-robin"
    spec, _ = sm.load(SPECS / f"{probe_spec}.json")
    if kind == "birds":
        spec, _ = sm.patch(spec, {"bird.pose": "flying", "bird.wing_thick": 2})
    print(f"\nIS THE ROW READ? sweeping on {probe_spec}\n")
    print(f"{'row':<24}{'x0.7':>9}{'x1.0':>9}{'x1.4':>9}   verdict")
    dead = 0
    for row, measure, floor in pairs:
        v0 = float(sm.get(spec, row))
        vals = []
        for k in (0.7, 1.0, 1.4):
            p = sm.BY_PATH[row]
            flat, _ = sm.patch(spec, {row: float(np.clip(v0 * k, p.lo, p.hi)),
                                      "variation.amount": 0.0})
            vals.append(measure(pipeline.build(flat, SEED)))
        span = max(vals) - min(vals)
        verdict = "moves" if span >= floor else "DEAD -- nothing reads this row"
        dead += int(span < floor)
        print(f"{row:<24}{vals[0]:>9.2f}{vals[1]:>9.2f}{vals[2]:>9.2f}   {verdict}")
    print(f"\n  {dead} of {len(pairs)} rows do not move a measurement")
    return 1 if dead else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=("fish", "birds"))
    ap.add_argument("cmd", choices=("report", "fit", "honoured"))
    ap.add_argument("--dry", action="store_true", help="print, write nothing")
    a = ap.parse_args()
    if a.cmd == "honoured":
        return honoured(a.kind)
    fit_mode = a.cmd == "fit"
    if a.kind == "fish":
        return fish_report(fit_mode, a.dry)
    return birds_report(fit_mode, a.dry)


if __name__ == "__main__":
    raise SystemExit(main())
