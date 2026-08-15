"""Stand the land animals at the height their own biome file says they are.

THE DEFECT, AND IT IS A DEFINITION ERROR RATHER THAN A TUNING ONE.
`quad.shoulder_h` is documented as "height of the SHOULDER JOINT above the
ground, as a fraction of head-body length", and the values authored into 131
specs are published SHOULDER (withers) heights. The row's own help text gives
the game away: it offers "a horse 0.62, a bison 0.64, a moose 0.70, a giraffe
1.30" as real values, and a giraffe's shoulder JOINT is not at 1.3 of its
head-body length -- 1.3 is where the top of its withers is. `docs/biomes/*.md`
quote the same figures in metres, e.g. American bison `2.8 / 1.8 sh`, and
2.8 x 0.64 = 1.79.

The generator then does exactly what it says: it puts the JOINT at that height
and draws the trunk on top of it, so every animal in the library ends up
standing a half-trunk-depth taller than the number it was authored from.

MEASURED, BEFORE ANY OF THIS:

    measured withers / biome-documented shoulder height   1.31x  (n=30, 1 in 10%)
    measured withers / (spec shoulder_h x head-body)      1.35x  (n=131, 2 in 10%)
    spec shoulder_h x head-body / documented shoulder ht  0.995x (n=30, 21 within 5%)

The last line is the one that makes this a definition error and not a
disagreement: the authored numbers reproduce the documented shoulder heights
almost exactly. They were entered correctly, into a slot that means something
else.

AND THE SILHOUETTES AGREE, FROM A DIFFERENT DIRECTION. `tools/reffit.py report`
puts the library 1.26x too high at the belly and 1.22x too high at the back
against 80 species of CC0 reference silhouette. Two instruments that share no
code and no source of truth report the same defect.
See `docs/reference-fitting-research.md` and `docs/quadruped-stance-height.md`.

    python tools/refstance.py freeze          # capture the targets, once
    python tools/refstance.py report          # how far every species is
    python tools/refstance.py fit --dry       # what would move
    python tools/refstance.py fit             # write the specs
    python tools/refstance.py trunk           # then match the reference leg share

TWO PASSES, AND THE ORDER IS NOT OPTIONAL. `fit` puts the withers where the
biome file says, using `quad.shoulder_h`; `trunk` then matches the reference
silhouettes' LEG SHARE using `quad.depth`, re-solving the withers at every step
so the first pass is not quietly undone. Run the other way round, the trunk is
solved against a body that is about to drop by a quarter -- the same reason
`tools/retune_quad_bulk.py` insists on trunk before legs.

`quad.leg_thick` is touched in one circumstance only, and never downward: a
shorter limb is a thinner limb in voxels at the same thickness/length, and the
grid rounds, so `_repair_limb` puts back a ratio that a rounding took away.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
import quadprobe as qp
import refsil
from forge import parts as partslib, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
TARGETS = ROOT / "refs" / "withers-target.json"
BIOMES = ROOT / "docs" / "biomes"

SEED = 1

# ONLY THE ANIMALS THAT STAND ON FOUR LEGS. A sprawling lizard's limbs leave the
# FLANK, so the joint is not under the withers and the arithmetic above does not
# describe it; a kangaroo's withers is not a load-bearing height at all. Both are
# reported and left alone rather than silently included -- 15 sprawling and 8
# bipedal specs, and a stance fit that quietly moved a crocodile would be the
# kind of thing nobody notices until a render.
FITTED_STANCE = "standing"

# HOW FAR ONE PASS MAY LOWER A SPECIES, as a fraction of the withers height it
# already measures. The same reasoning as `reffit.MAX_LIFT`: the targets are
# read off authored numbers, one of which could be wrong, and no single wrong
# number should be able to fold an animal onto the floor before anybody sees a
# render. The measured gap is 1.35x, i.e. a drop of 26%, so 35% clears the whole
# distribution in one pass and still bounds a typo. IT IS A BOUND AND NOT A
# MEASUREMENT, and a species that reaches it is printed as having reached it.
MAX_DROP = 0.35


def _build(spec: dict, seed: int = SEED):
    flat, _ = sm.patch(spec, {"variation.amount": 0.0})
    return pipeline.build(flat, seed)


def _measure(spec: dict) -> dict:
    a = _build(spec)
    cm = float(sm.get(spec, "resolution_cm"))
    hb_v = float(sm.get(spec, "quad.length_m")) * 100.0 / cm
    w = qp.m_withers_h(a)
    g = qp.m_trunk_girth(a)
    # The silhouette measurement, off the SAME build, so a trunk pass costs no
    # extra assets. `refsil.from_grid` is a true orthographic projection of the
    # voxel grid and not a render -- see that file for why the render is wrong
    # for this.
    data = a.grid.data if hasattr(a, "grid") else a.data
    sil = refsil.measure(refsil.from_grid(data)) or {}
    # PARTS THAT TOUCH THEIR PARENT ONLY AT A CORNER. `forge.parts.joints`
    # returns `origin: None` for those, and `quadprobe --parts` fails on it,
    # because a limb or a head with no joint is a rigging defect that ships in
    # silence. Moving the shoulder moves where the neck leaves the trunk, so
    # this pass can create one: the first run of it did, on `fennec-fox`, whose
    # HEAD lost its joint. It is checked here rather than found afterwards.
    nojoint = []
    if a.parts is not None:
        nojoint = [j["part"] for j in partslib.joints(a.parts)
                   if j["origin"] is None]
    return {
        "nojoint": nojoint,
        "leg_share": float(sil.get("leg_share", float("nan"))),
        "sil_belly": float(sil.get("belly_over_length", float("nan"))),
        "withers": w,
        "withers_over_hb": w / hb_v if hb_v > 0 else float("nan"),
        "belly_clear": qp.m_belly_clear(a),
        "slender": qp.m_limb_slender(a),
        "dia": qp.m_limb_dia(a),
        "dia_min": qp.m_limb_dia_min(a),
        "limb_len": qp.m_limb_len(a),
        "girth": g,
        "gw": g / w if w > 0 else float("nan"),
        "fore_gap": qp.m_fore_gap(a),
        "hind_gap": qp.m_hind_gap(a),
    }


def _joints_ok(spec: dict, changes: dict) -> bool:
    """Does every part still meet its parent on a FACE, on the build the probe
    actually makes -- variation ON?

    THE SOLVE PINS THE VARIATION DRAW OFF AND `quadprobe --parts` DOES NOT, and
    that gap let a regression through the guard that was written to stop it.
    `fennec-fox` came out of the stance pass with its head touching the neck at
    a corner only; the pinned build was clean, so the check inside `_ok` saw
    nothing, and the failure appeared afterwards in the probe. Everything else
    here is solved with the draw off on purpose -- solving against one individual
    chases noise -- so this one check is made against the drawn build, once, on
    the candidate that is about to be written.
    """
    trial, _u = sm.patch(spec, changes)
    a = pipeline.build(trial, SEED)
    if a.parts is None:
        return True
    return not [j for j in partslib.joints(a.parts) if j["origin"] is None]


def _settle(spec: dict, changes: dict, key: str = "quad.shoulder_h") -> dict | None:
    """Back the change off, a step at a time, until the drawn build rigs."""
    if _joints_ok(spec, changes):
        return changes
    v0 = float(sm.get(spec, key))
    v1 = float(changes.get(key, v0))
    for k in range(1, 9):
        trial = dict(changes)
        trial[key] = round(v1 + (v0 - v1) * k / 8.0, 3)
        if abs(trial[key] - v0) < 5e-4:
            break
        if _joints_ok(spec, trial):
            return trial
    return None


def _quad_specs() -> list[str]:
    out = []
    for p in sorted(SPECS.glob("*.json")):
        s, _ = sm.load(p)
        if sm.get(s, "kind") == "quadruped":
            out.append(p.stem)
    return out


# ------------------------------------------------------------------- freeze

_SH_PATTERNS = (
    # "2.8 / 1.8 sh", "1.6 long / 1.0 shoulder", "1.3 head-body / 0.7 at the
    # shoulder". ONLY FORMS THAT SAY "SHOULDER" OUT LOUD. The bare "1.9 / 1.1"
    # form appears too and is NOT accepted: the same column also carries
    # "1.3 / 1.5 tall" and "0.6 (1.2 with tail)", so a bare second number is
    # not reliably a shoulder height, and guessing which it is on 30 rows to
    # save reading them is how this project got an orca eye patch measured off
    # dimensionless indices.
    re.compile(r"([\d.]+)\s*(?:long|head-body)?\s*/\s*([\d.]+)\s*"
               r"(?:sh\b|at the shoulder|shoulder)"),
)


def _biome_shoulders() -> dict[str, tuple[float, float, str]]:
    out: dict[str, tuple[float, float, str]] = {}
    for f in sorted(BIOMES.glob("*.md")):
        for line in f.read_text(encoding="utf-8").splitlines():
            if "gen: quadruped" not in line:
                continue
            cells = [c.strip() for c in line.split("|")]
            if len(cells) < 3:
                continue
            key = re.sub(r"[^a-z0-9]+", "-", cells[1].lower()).strip("-")
            for c in cells:
                for pat in _SH_PATTERNS:
                    m = pat.search(c)
                    if m:
                        out[key] = (float(m.group(1)), float(m.group(2)), f.name)
                        break
                if key in out:
                    break
    return out


def freeze(force: bool) -> int:
    """Write the targets ONCE, from the specs as they stand, and never again.

    THE TARGET CANNOT BE READ OUT OF THE SPEC AFTER THE FIRST RUN, and that is
    the whole reason this file exists. `quad.shoulder_h` is both the thing being
    solved and the thing that carries the authored withers height, so a second
    run of `fit` reading its own output would take the corrected joint height as
    a fresh withers target and lower the animal again, and again, and would
    print a tidy table every time. `tools/retune_quad_bulk.py` hit exactly this
    and its docstring records the fix: the baseline is a FILE.
    """
    if TARGETS.exists() and not force:
        print(f"{TARGETS} exists -- targets already frozen. --force to redo.")
        return 1
    doc = _biome_shoulders()
    out = {}
    agree = []
    for n in _quad_specs():
        s, _ = sm.load(SPECS / f"{n}.json")
        rec = {
            "withers_over_hb": round(float(sm.get(s, "quad.shoulder_h")), 4),
            "source": "spec quad.shoulder_h as authored at 17cc742",
            "stance": sm.get(s, "quad.stance"),
        }
        d = doc.get(n)
        if d:
            rec["biome_doc"] = {"head_body_m": d[0], "shoulder_m": d[1],
                                "file": d[2],
                                "ratio": round(d[1] / d[0], 4)}
            agree.append(rec["withers_over_hb"] * float(sm.get(s, "quad.length_m"))
                         / d[1])
        out[n] = rec
    TARGETS.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
    a = np.array(agree)
    print(f"froze {len(out)} targets to {TARGETS}")
    print(f"  {len(a)} corroborated by a biome file that names a shoulder "
          f"height in metres:")
    print(f"    spec shoulder_h x head-body, over the documented figure: "
          f"median {np.median(a):.3f}x, within 5% on "
          f"{int((np.abs(a - 1) <= 0.05).sum())} of {len(a)}")
    return 0


def _targets() -> dict:
    if not TARGETS.exists():
        raise SystemExit(f"no frozen targets at {TARGETS}\n"
                         "  run: python tools/refstance.py freeze")
    return json.loads(TARGETS.read_text(encoding="utf-8"))


# ------------------------------------------------------------------- report

def report(only: list[str] | None) -> int:
    tg = _targets()
    names = [n for n in _quad_specs() if not only or n in only]
    print("\nHOW HIGH THE LIBRARY STANDS, AGAINST WHAT ITS OWN SPECS SAY")
    print("  target = quad.shoulder_h x head-body length, which is the "
          "published\n  shoulder height the row was authored from. variation "
          "off, seed 1.\n")
    print(f"{'species':<26} {'stance':<10} {'want w/hb':>9} {'measured':>9} "
          f"{'x':>6} {'doc m':>6} {'meas m':>7}")
    rows = []
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        t = tg.get(n)
        if not t:
            continue
        m = _measure(s)
        cm = float(sm.get(s, "resolution_cm"))
        want = float(t["withers_over_hb"])
        x = m["withers_over_hb"] / want if want > 0 else float("nan")
        dm = (t.get("biome_doc") or {}).get("shoulder_m", float("nan"))
        rows.append((n, t["stance"], x))
        print(f"{n:<26} {t['stance']:<10} {want:>9.3f} "
              f"{m['withers_over_hb']:>9.3f} {x:>6.2f} {dm:>6.2f} "
              f"{m['withers'] * cm / 100:>7.2f}")
    for tag in (None, FITTED_STANCE):
        sel = [r for r in rows if tag is None or r[1] == tag]
        v = np.array([r[2] for r in sel])
        v = v[np.isfinite(v)]
        if not len(v):
            continue
        print(f"\n  {'all stances' if tag is None else tag} ({len(v)}): "
              f"measured withers / authored, median {np.median(v):.3f}x, "
              f"range {v.min():.2f}-{v.max():.2f}, "
              f"within 10% {int((np.abs(v - 1) <= 0.10).sum())}")
    return 0


# ---------------------------------------------------------------------- fit

def _ok(before: dict, after: dict, gw_floor: float | None = None,
        check_slender: bool = True) -> str | None:
    """Would this candidate make anything the proportion work established worse?

    EVERY ONE OF THESE IS A FLOOR SOMEBODY ELSE SET, and they are checked on the
    BUILT asset rather than argued about from the parameter, because the whole
    reason this exercise exists is a parameter that did not mean what it said.

    `check_slender` is off while a stance candidate is being SEARCHED and on
    when it is ACCEPTED. A shorter limb at the same thickness/length is a
    thinner limb in voxels, and the voxel grid rounds -- so a stance step that
    is right can still drop `t/L` by a rounding. That is repairable by thickening
    the limb (`_repair_limb`), and vetoing the stance for it would throw away the
    fix to defend a rounding error.
    """
    if not np.isfinite(after["withers"]) or after["withers"] <= 0:
        return "withers not measurable"
    if after["slender"] < qp.SLENDER_MIN:
        return f"foreleg {after['slender']:.3f} thick for its length"
    if check_slender and after["slender"] < before["slender"] * 0.98:
        return (f"foreleg thickness/length falls {before['slender']:.3f} -> "
                f"{after['slender']:.3f}")
    if after["dia"] < qp.LIMB_MIN_VOX:
        return f"foreleg {after['dia']:.1f} vox across"
    if after["dia_min"] < 2.0:
        return f"narrowest slab of the foreleg {after['dia_min']:.0f} vox"
    # THE GIRTH FLOOR, AND IT HAS TWO SETTINGS ON PURPOSE. With the stance solved
    # by the shoulder alone, girth/withers can only RISE -- withers is its
    # denominator -- so the floor is "not worse than it was", which catches a
    # regression rather than permitting one. When the trunk itself has to be
    # shallowed to get the animal down, girth necessarily falls, and the floor
    # becomes `GIRTH_WANT`: the number `tools/retune_quad_bulk.py` lifted every
    # trunk in this library to. Anything below that is undoing somebody's work.
    floor = qp.GIRTH_WANT if gw_floor is None else gw_floor
    if gw_floor is None:
        floor = max(qp.GIRTH_MIN, before["gw"] * 0.98)
    if after["gw"] < floor:
        return f"chest girth {after['gw']:.2f} of withers (floor {floor:.2f})"
    # THE FLOOR THE ANIMAL STANDS ON. Lowering the joint lowers the belly with
    # it, and a deep-chested short-legged species runs out of daylight before it
    # runs out of target. Three voxels is the house "three to read" rule applied
    # to the gap a leg is drawn in; below it there is no leg to look at.
    if after["belly_clear"] < 3.0:
        return f"belly {after['belly_clear']:.0f} vox off the ground"
    if after["limb_len"] < 3.0:
        return f"free foreleg {after['limb_len']:.0f} vox long"
    if after.get("nojoint") and not before.get("nojoint"):
        names = partslib.names()
        return ("a part loses its joint (corner contact only): "
                + ", ".join(names.get(p, str(p)) for p in after["nojoint"]))
    if after["fore_gap"] > 0.5 or after["hind_gap"] > 0.5:
        return (f"a foot leaves the floor "
                f"({after['fore_gap']:.1f}/{after['hind_gap']:.1f} vox)")
    return None


def _solve(spec: dict, want: float, before: dict, lo: float, hi: float,
           rounds: int = 6, gw_floor: float | None = None,
           check_slender: bool = True, max_v: float | None = None
           ) -> tuple[float, dict, str | None]:
    """Lower `quad.shoulder_h` until the MEASURED withers reaches `want`.

    THE STEP IS ADDITIVE AND NOT MULTIPLICATIVE, unlike the two other solvers in
    this repo, and the difference is not stylistic. `retune_quad_bulk._solve`
    and `reffit._solve` both step `x * want / got`, which is right when the
    measurement is proportional to the row -- a limb radius is a multiple of
    `leg_thick`. Withers height is `joint + the trunk that sits on top of it`,
    which is AFFINE, and the constant term is most of the error being solved
    here: a proportional step on an affine quantity converges toward the wrong
    place and takes its rounds getting there. `want - got` in the same units is
    exact for the model and lands in one step, and it is still measured again
    rather than trusted, because the trunk is rasterised and the ground is a
    whole voxel.

    THE ANSWER RETURNED IS THE BEST CANDIDATE THAT PASSES `_ok`, not the closest
    one. `retune_quad_bulk._solve` records why: taking the nearest try on a
    one-sided constraint can prefer a failing animal to a passing one, and both
    of the obvious rules -- last try, closest try -- shipped that bug once each.
    """
    v0 = float(sm.get(spec, "quad.shoulder_h"))
    tried: list[tuple[float, dict]] = []

    def probe(v: float) -> dict:
        m = _measure(sm.patch(spec, {"quad.shoulder_h": round(v, 4)})[0])
        tried.append((v, m))
        return m

    probe(v0)
    for _ in range(rounds):
        last, got = tried[-1]
        w = got["withers_over_hb"]
        if not np.isfinite(w) or w <= 0:
            break
        if abs(w - want) / want < 0.02:
            break
        nxt = float(np.clip(last + (want - w), lo, hi))
        if abs(nxt - last) < 5e-4:
            break
        probe(nxt)
    # NEVER ABOVE `max_v`, WHICH IS THE STARTING VALUE UNLESS SOMEONE SAYS
    # OTHERWISE. The stance pass may only lower an animal, and that guard lives
    # here. The TRUNK pass has to be allowed to raise the joint, because a
    # shallower trunk is a lower withers at the same joint and holding the
    # withers where it was put is the entire point of re-solving it.
    cap = v0 if max_v is None else max_v
    usable = [(v, m) for v, m in tried
              if v <= cap + 1e-9 and np.isfinite(m["withers_over_hb"])]
    if not usable:
        return v0, tried[0][1], "withers not measurable"
    v, m = min(usable, key=lambda t: abs(t[1]["withers_over_hb"] - want))
    return v, m, _ok(before, m, gw_floor, check_slender)


def _repair_limb(spec: dict, changes: dict, before: dict, after: dict
                 ) -> tuple[dict, dict]:
    """Thicken the limb back to the thickness/length it had, and never thin it.

    A SHORTER LIMB IS A THINNER LIMB IN VOXELS at the same thickness/length, and
    the grid rounds it to whole voxels, so a species can come out of a stance
    step with `t/L` a rounding lower than it went in. `quad.leg_thick` is a
    fraction OF THE LIMB'S OWN LENGTH, so it is the row that puts the ratio back
    without arguing with the stance -- and it can only go up here, which is the
    same direction `tools/reffit.py` and `tools/retune_quad_bulk.py` are allowed
    to move it in.
    """
    if after["slender"] >= before["slender"] * 0.98:
        return changes, after
    t0 = float(sm.get(spec, "quad.leg_thick"))
    want = before["slender"]
    best = (t0, after)
    t = t0
    for _ in range(4):
        got = best[1]["slender"]
        if not np.isfinite(got) or got <= 0:
            break
        t = float(np.clip(t * want / got, t0, float(sm.BY_PATH["quad.leg_thick"].hi)))
        if t <= t0 + 1e-4:
            break
        trial = dict(changes)
        trial["quad.leg_thick"] = round(t, 3)
        m = _measure(sm.patch(spec, trial)[0])
        if not np.isfinite(m["slender"]):
            break
        best = (t, m)
        if m["slender"] >= want * 0.98:
            break
    if best[0] <= t0 + 1e-4:
        return changes, after
    out = dict(changes)
    out["quad.leg_thick"] = round(best[0], 3)
    return out, best[1]


# HOW FAR THE TRUNK MAY BE SHALLOWED when the belly reaches the floor first, as
# multiples of the depth already authored, tried DEEPEST FIRST so the answer is
# the smallest change that works.
#
# A LADDER AND NOT A BISECTION, and that is a measured decision. Feasibility is
# NOT monotone in depth: on `hippopotamus` a trunk at 0.448 fails (belly on the
# floor) while 0.504 fails differently (foreleg 2 voxels across) and 0.392 and
# 0.336 both pass. The voxel grid is what makes it non-monotone -- a limb's
# measured span is its diameter rounded at whatever sub-voxel offset the
# centreline lands on -- and a bisection on a non-monotone predicate returns
# whichever side it happened to probe.
_DEPTH_LADDER = (1.0, 0.95, 0.90, 0.85, 0.80, 0.75, 0.70, 0.65, 0.60, 0.55, 0.50)


def _fit_one(spec: dict, want: float, before: dict, floor: float
             ) -> tuple[dict, dict, str | None, str]:
    """The best legal (`quad.shoulder_h`, `quad.depth`, `quad.leg_thick`).

    THE TWO-VARIABLE FIT, AND WHICH VARIABLE DOES THE WORK IS NOT THE ONE THE
    BRIEF EXPECTED. The expectation was that lowering an animal onto shorter legs
    would fight the girth gate. Measured, it does the opposite: withers is the
    DENOMINATOR of girth/withers, so lowering the animal RAISES that ratio, and
    carries it off the 0.95 the previous work stopped at and up into the live
    range of 1.14-1.38. `quad.depth` is not needed on the large majority of the
    library and is not touched there.

    The fight is real on one group, and the algebra says exactly which. Belly
    clearance is `withers - trunk depth` and nothing else: what the trunk hangs
    below its own axis is subtracted from what it stands above it, so `quad.belly`
    -- which looks like the obvious knob for a high belly -- cancels out of the
    answer entirely and cannot help. Once the withers is pinned to the documented
    height, only a shallower trunk can put daylight under the animal. That bites
    the short-legged deep-chested species and nothing else.
    """
    d0 = float(sm.get(spec, "quad.depth"))
    v0 = float(sm.get(spec, "quad.shoulder_h"))
    first = None
    for i, f in enumerate(_DEPTH_LADDER):
        d = round(d0 * f, 3)
        trial = spec if i == 0 else sm.patch(spec, {"quad.depth": d})[0]
        gwf = None if i == 0 else max(qp.GIRTH_MIN, qp.GIRTH_WANT)
        v, m, why = _solve(trial, want, before, floor, v0, gw_floor=gwf,
                           check_slender=False)
        ch = {"quad.shoulder_h": round(v, 3)}
        if i:
            ch["quad.depth"] = d
        if why is None:
            ch, m = _repair_limb(spec, ch, before, m)
            why = _ok(before, m, gwf)
        if first is None:
            first = (ch, m, why)
        if why is None:
            knob = "shoulder_h" if i == 0 else "shoulder_h + depth"
            if "quad.leg_thick" in ch:
                knob += " + leg_thick"
            return ch, m, None, knob
    ch, m, why = first
    return {}, m, why, "-"


def fit(only: list[str] | None, dry: bool) -> int:
    """Solve the stance so the MEASURED withers lands on the authored height."""
    tg = _targets()
    names = [n for n in _quad_specs() if not only or n in only]
    print("\nSTANCE: lower the shoulder JOINT until the WITHERS lands on the "
          "authored\nshoulder height. Nothing is ever raised.\n")
    print(f"{'species':<26} {'shoulder_h':>15} {'withers/hb':>19} "
          f"{'belly vox':>13} {'g/w':>13}  knob / outcome")
    moved = 0
    stuck: list[tuple[str, str]] = []
    skipped: list[tuple[str, str]] = []
    twoknob: list[str] = []
    deltas = []
    for n in names:
        path = SPECS / f"{n}.json"
        spec, _ = sm.load(path)
        t = tg.get(n)
        if not t:
            skipped.append((n, "no frozen target"))
            continue
        if t["stance"] != FITTED_STANCE:
            skipped.append((n, f"{t['stance']} -- withers is not a "
                               f"load-bearing height"))
            continue
        want = float(t["withers_over_hb"])
        before = _measure(spec)
        v0 = float(sm.get(spec, "quad.shoulder_h"))
        d0 = float(sm.get(spec, "quad.depth"))
        if before["withers_over_hb"] <= want * 1.02:
            print(f"{n:<26} {v0:>7.3f} -> {v0:<6.3f} "
                  f"{before['withers_over_hb']:>8.3f} vs {want:<8.3f} "
                  f"{'':>13} {'':>13}  already at or below the authored height")
            continue
        floor = max(0.12, v0 - MAX_DROP * before["withers_over_hb"])
        changes, after, why, knob = _fit_one(spec, want, before, floor)
        if not changes or changes.get("quad.shoulder_h", v0) >= v0 - 5e-4:
            print(f"{n:<26} {v0:>7.3f} -> {v0:<6.3f} "
                  f"{before['withers_over_hb']:>8.3f} vs {want:<8.3f} "
                  f"{before['belly_clear']:>13.0f} {before['gw']:>13.2f}  "
                  f"HELD: {why}")
            stuck.append((n, why or "?"))
            continue
        settled = _settle(spec, changes)
        if settled is None:
            print(f"{n:<26} {v0:>7.3f} -> {v0:<6.3f} "
                  f"{before['withers_over_hb']:>8.3f} vs {want:<8.3f} "
                  f"{before['belly_clear']:>13.0f} {before['gw']:>13.2f}  "
                  f"HELD: a part loses its joint on the drawn build")
            stuck.append((n, "a part loses its joint on the drawn build"))
            continue
        if settled != changes:
            changes = settled
            after = _measure(sm.patch(spec, changes)[0])
            knob += " (backed off to keep the head's joint)"
        v1 = changes["quad.shoulder_h"]
        moved += 1
        deltas.append(after["withers_over_hb"] / before["withers_over_hb"])
        note = f"  {knob}"
        if "depth" in knob:
            twoknob.append(f"{n} (depth {d0:.2f} -> "
                           f"{changes['quad.depth']:.2f}, girth/withers "
                           f"{before['gw']:.2f} -> {after['gw']:.2f})")
        if v1 <= floor + 5e-4:
            note += f"  AT THE {MAX_DROP:.0%} ONE-PASS FLOOR -- LOOK AT IT"
        elif after["withers_over_hb"] > want * 1.05:
            note += (f"  STOPPED SHORT at "
                     f"{(before['withers_over_hb'] - after['withers_over_hb']) / (before['withers_over_hb'] - want):.0%}"
                     f" of the way")
        print(f"{n:<26} {v0:>7.3f} -> {v1:<6.3f} "
              f"{before['withers_over_hb']:>8.3f} -> {after['withers_over_hb']:<8.3f} "
              f"{before['belly_clear']:>5.0f} -> {after['belly_clear']:<5.0f} "
              f"{before['gw']:>5.2f} -> {after['gw']:<5.2f}{note}")
        if not dry:
            _write(path, changes)
    print(f"\n  {moved} species lowered"
          f"{'  (DRY RUN -- nothing written)' if dry else ''}")
    if deltas:
        d = np.array(deltas)
        print(f"  withers came down by a median {(1 - np.median(d)) * 100:.0f}% "
              f"on the species that moved")
    if twoknob:
        print(f"  {len(twoknob)} needed the SECOND knob -- the belly reached "
              f"the floor before the withers reached the target:")
        for s in twoknob:
            print(f"    {s}")
    for n, why in stuck:
        print(f"  HELD: {n} -- {why}")
    if skipped:
        print(f"  {len(skipped)} not eligible:")
        for n, why in skipped:
            print(f"    {n:<26} {why}")
    return 0


# ------------------------------------------------------------------- trunk

# How far one pass may change the trunk's depth, either way.
MAX_TRUNK = 0.30

REFS = ROOT / "refs" / "quadruped-reference.json"
LATIN = ROOT / "refs" / "species-latin.json"


def _eligible_refs() -> dict[str, float]:
    """Species with a stance reference that is BOTH good enough and the right
    animal: fit quality on `leg_share`, a hand-verified scientific name, and
    that name matching what the silhouettes on disk were downloaded as."""
    if not REFS.exists():
        raise SystemExit(f"no reference file at {REFS}\n"
                         "  run: python tools/reffit.py extract")
    refs = json.loads(REFS.read_text(encoding="utf-8"))
    latin = json.loads(LATIN.read_text(encoding="utf-8")) if LATIN.exists() else {}
    out = {}
    for n, r in refs.items():
        if r.get("leg_share_quality") != "fit":
            continue
        info = latin.get(n) or {}
        if not info.get("checked_by_hand"):
            continue
        if info.get("latin") != r.get("scientific_name"):
            continue
        out[n] = float(r["leg_share"])
    return out


def trunk(only: list[str] | None, dry: bool) -> int:
    """Shallow the trunk until the LEG SHARE matches the reference silhouettes,
    holding the withers on its documented height.

    WHY THIS PASS EXISTS, AND IT IS THE HONEST HALF OF THE STANCE STORY. Putting
    the withers where the biome files say it goes moved the library from 1.26x
    the references' belly clearance to 0.72x -- it crossed the target instead of
    landing on it. On the length-free measurement the overshoot is smaller and
    real: leg share of the back height went from 1.067x the references to
    0.818x. The animal is now the right HEIGHT and the wrong SHAPE, because the
    trunk kept the depth it was given while the legs under it got shorter.

    AND THAT TRUNK DEPTH HAS A PROVENANCE THAT EXPLAINS IT. `retune_quad_bulk`
    deepened trunks until chest girth reached 0.95 of the withers -- of the
    INFLATED withers. Measured against the corrected one, the same trunks now
    read 1.30, at the top of the live 1.14-1.38 range. The trunks were absorbing
    the height error, and correcting the height is what exposed them.

    So: belly clearance is `withers - trunk depth`, exactly, and with the withers
    pinned the only remaining knob is the depth. Girth may not fall below
    `GIRTH_WANT`, which is the floor the previous work lifted every trunk to.
    """
    tg = _targets()
    want_ls = _eligible_refs()
    names = [n for n in _quad_specs()
             if (not only or n in only) and n in want_ls
             and (tg.get(n) or {}).get("stance") == FITTED_STANCE]
    print("\nTRUNK: shallow the trunk until the LEG SHARE matches the reference,"
          "\nholding the withers on its documented height.\n")
    print(f"  {len(want_ls)} species have a fit-quality stance reference; "
          f"{len(names)} of those stand on four legs and have a spec\n")
    print(f"{'species':<26} {'depth':>14} {'leg share':>19} {'shoulder_h':>15} "
          f"{'g/w':>13}  outcome")
    moved = 0
    held: list[tuple[str, str]] = []
    short: list[tuple[str, float, float, float]] = []
    for n in names:
        path = SPECS / f"{n}.json"
        spec, _ = sm.load(path)
        before = _measure(spec)
        d0 = float(sm.get(spec, "quad.depth"))
        v0 = float(sm.get(spec, "quad.shoulder_h"))
        want = want_ls[n]
        wither_want = float(tg[n]["withers_over_hb"])
        if abs(before["leg_share"] - want) / want < 0.03:
            print(f"{n:<26} {d0:>6.2f} -> {d0:<5.2f} "
                  f"{before['leg_share']:>8.3f} vs {want:<8.3f} "
                  f"{'':>15} {'':>13}  already on the reference")
            continue
        lo, hi = d0 * (1 - MAX_TRUNK), d0 * (1 + MAX_TRUNK)
        floor = max(0.12, v0 - MAX_DROP * before["withers_over_hb"])
        tried: list[tuple[float, dict, dict, str | None]] = []
        # A SEED THE SECANT CAN ACTUALLY START FROM. Seven species came out of
        # the stance pass with their legs no longer separating in an
        # orthographic side view -- `brown-bear` among them -- so `leg_share`
        # on our own asset is not a number and the step has nothing to divide
        # by. Those are precisely the animals this pass exists for, and
        # dropping them would leave the worst cases unfitted while the table
        # said "held". So walk the trunk in until the belly line comes back.
        d = d0
        if not np.isfinite(before["leg_share"]):
            for f in (0.90, 0.80, 0.75, 0.70):
                probe, _u = sm.patch(spec, {"quad.depth": round(d0 * f, 3)})
                if np.isfinite(_measure(probe)["leg_share"]):
                    d = d0 * f
                    break
        for _ in range(4):
            trial, _u = sm.patch(spec, {"quad.depth": round(d, 3)})
            # THE WITHERS IS RE-SOLVED AT EVERY DEPTH, not solved once and
            # trusted. A shallower trunk is a LOWER withers at the same joint,
            # so a trunk pass that did not re-solve would quietly undo the
            # stance pass and report a leg share it had bought by standing the
            # animal up again.
            v, m, _w = _solve(trial, wither_want, before, floor,
                              float(sm.BY_PATH["quad.shoulder_h"].hi),
                              gw_floor=max(qp.GIRTH_MIN, qp.GIRTH_WANT),
                              check_slender=False,
                              max_v=float(sm.BY_PATH["quad.shoulder_h"].hi))
            ch = {"quad.depth": round(d, 3), "quad.shoulder_h": round(v, 3)}
            ch, m = _repair_limb(spec, ch, before, m)
            tried.append((d, ch, m, _ok(before, m,
                                        max(qp.GIRTH_MIN, qp.GIRTH_WANT))))
            got = m["leg_share"]
            if not np.isfinite(got):
                break
            if abs(got - want) / want < 0.03:
                break
            # leg share is 1 - trunk/back, and the trunk is proportional to the
            # row, so this ratio is the step that lands it.
            nxt = float(np.clip(d * (1.0 - want) / max(1.0 - got, 1e-3), lo, hi))
            if abs(nxt - d) < 5e-4:
                break
            d = nxt
        good = [t for t in tried if t[3] is None and np.isfinite(t[2]["leg_share"])]
        # STOPPING SHORT MUST STILL MOVE. A secant lands on the target or on a
        # bound, and when the bound is illegal it comes back with nothing but
        # the value it started from -- which prints as "no better trunk in
        # range" on a species that could have gone MOST of the way. That is a
        # silent no-op wearing a decision's clothes, which is this project's
        # signature failure. So when the solve has found nothing better than
        # where it began, walk the trunk in one step at a time and keep the
        # furthest step that is still legal.
        if all(abs(t[0] - d0) < 5e-4 for t in good):
            step = -1 if want > before["leg_share"] else 1
            for k in range(1, 7):
                dk = round(d0 * (1.0 + step * 0.05 * k), 3)
                if not (lo - 1e-6 <= dk <= hi + 1e-6):
                    break
                trial, _u = sm.patch(spec, {"quad.depth": dk})
                v, m, _w = _solve(trial, wither_want, before, floor,
                                  float(sm.BY_PATH["quad.shoulder_h"].hi),
                                  gw_floor=max(qp.GIRTH_MIN, qp.GIRTH_WANT),
                                  check_slender=False,
                                  max_v=float(sm.BY_PATH["quad.shoulder_h"].hi))
                ch = {"quad.depth": dk, "quad.shoulder_h": round(v, 3)}
                ch, m = _repair_limb(spec, ch, before, m)
                why = _ok(before, m, max(qp.GIRTH_MIN, qp.GIRTH_WANT))
                # `continue`, NOT `break`. Feasibility is not monotone in the
                # trunk depth -- see `_DEPTH_LADDER` for the hippopotamus that
                # measured it -- so stopping at the first refusal loses the
                # steps behind it. `moose` and `reindeer` were reported as
                # having no legal trunk at all by a version that broke here.
                if why is not None or not np.isfinite(m["leg_share"]):
                    continue
                tried.append((dk, ch, m, None))
            good = [t for t in tried
                    if t[3] is None and np.isfinite(t[2]["leg_share"])]
        if not good:
            why = next((t[3] for t in tried if t[3]), "solver found nothing")
            held.append((n, why))
            print(f"{n:<26} {d0:>6.2f} -> {d0:<5.2f} "
                  f"{before['leg_share']:>8.3f} vs {want:<8.3f} "
                  f"{'':>15} {'':>13}  HELD: {why}")
            continue
        d1, ch, after, _ = min(good, key=lambda t: abs(t[2]["leg_share"] - want))
        if abs(d1 - d0) < 5e-4:
            print(f"{n:<26} {d0:>6.2f} -> {d0:<5.2f} "
                  f"{before['leg_share']:>8.3f} vs {want:<8.3f} "
                  f"{'':>15} {'':>13}  no better trunk in range")
            continue
        settled = _settle(spec, ch)
        if settled is None:
            held.append((n, "a part loses its joint on the drawn build"))
            print(f"{n:<26} {d0:>6.2f} -> {d0:<5.2f} "
                  f"{before['leg_share']:>8.3f} vs {want:<8.3f} "
                  f"{'':>15} {'':>13}  HELD: joint lost on the drawn build")
            continue
        if settled != ch:
            ch = settled
            after = _measure(sm.patch(spec, ch)[0])
        moved += 1
        note = ""
        if abs(d1 - lo) < 5e-4 or abs(d1 - hi) < 5e-4:
            note = f"  AT THE {MAX_TRUNK:.0%} ONE-PASS BOUND -- LOOK AT IT"
        elif abs(after["leg_share"] - want) / want > 0.05:
            done = ((after["leg_share"] - before["leg_share"])
                    / (want - before["leg_share"]))
            note = (f"  STOPPED SHORT at {done:.0%} of the way -- "
                    f"girth floor {max(qp.GIRTH_MIN, qp.GIRTH_WANT):.2f}")
            short.append((n, after["leg_share"], want, after["gw"]))
        print(f"{n:<26} {d0:>6.2f} -> {d1:<5.2f} "
              f"{before['leg_share']:>8.3f} -> {after['leg_share']:<8.3f} "
              f"{v0:>6.3f} -> {ch['quad.shoulder_h']:<6.3f} "
              f"{before['gw']:>5.2f} -> {after['gw']:<5.2f}{note}")
        if not dry:
            _write(path, ch)
    print(f"\n  {moved} trunks changed"
          f"{'  (DRY RUN -- nothing written)' if dry else ''}")
    for n, why in held:
        print(f"  HELD: {n} -- {why}")
    if short:
        print(f"  {len(short)} stopped short of the reference at the girth "
              f"floor -- a slimmer trunk than {qp.GIRTH_WANT:.2f} of withers is\n"
              f"  what these references ask for, and that floor is the "
              f"library's own, not this pass's to move:")
        for n, got, wnt, gw in short:
            print(f"    {n:<24} leg share {got:.3f}, reference {wnt:.3f}, "
                  f"girth/withers {gw:.2f}")
    return 0


def _write(path: Path, changes: dict) -> None:
    raw = json.loads(path.read_text(encoding="utf-8"))
    for dotted, value in changes.items():
        group, row = dotted.split(".", 1)
        raw.setdefault(group, {})[row] = value
    path.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("freeze", help="capture the targets, once")
    f.add_argument("--force", action="store_true")
    r = sub.add_parser("report", help="how high the library stands")
    r.add_argument("--only", nargs="*")
    t = sub.add_parser("fit", help="solve quad.shoulder_h")
    t.add_argument("--only", nargs="*")
    t.add_argument("--dry", action="store_true")
    k = sub.add_parser("trunk", help="solve quad.depth to the reference leg share")
    k.add_argument("--only", nargs="*")
    k.add_argument("--dry", action="store_true")
    a = ap.parse_args()
    if a.cmd == "freeze":
        return freeze(a.force)
    if a.cmd == "report":
        return report(a.only)
    if a.cmd == "fit":
        return fit(a.only, a.dry)
    if a.cmd == "trunk":
        return trunk(a.only, a.dry)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
