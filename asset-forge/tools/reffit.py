"""Fit land-animal proportions to MEASURED references instead of to judgement.

THE GAP THIS CLOSES. `docs/quadruped-proportion-research.md` records, honestly,
that the limb target of 0.16 is a judgement call sitting between photoreal
(Infinigen 0.111-0.140), a shipping voxel RPG (Veloren 0.230) and a toy
(Minecraft 0.393). One constant was then applied to 131 species. Real animals do
not share a constant: measured off licence-clean scientific silhouettes, a brown
bear runs 0.57 and a red deer 0.12, which is a spread of nearly five to one.
This tool replaces the constant with a per-species measurement where a reference
exists, and says so plainly where one does not.

    python tools/reffit.py fetch            # ONLINE. downloads references
    python tools/reffit.py extract          # offline. silhouettes -> numbers
    python tools/reffit.py report           # offline. how far are we?
    python tools/reffit.py fit --dry        # offline. what would move
    python tools/reffit.py fit              # offline. write the specs
    python tools/reffit.py overlay          # offline. LOOK at the measurement

OFFLINE AT BUILD TIME, AND `fetch` IS THE ONLY COMMAND THAT TOUCHES A NETWORK.
Everything a build needs is checked in: the silhouettes under
`refs/silhouettes/`, their licences in `refs/silhouettes/SOURCES.json`, and the
extracted numbers in `refs/quadruped-reference.json`. `forge/language.py` is
fully local by owner instruction and this is held to the same standard. Nothing
under `forge/` imports this file.

WHAT IS NOT BUILT HERE, AND WHY. A visual hull from five silhouettes is the
obvious reading of "use photographs to drive generation" and it is a trap: it
yields one static mesh with no part tags, no joints, no sexes, no seeds and no
variation, which would discard the entire rig. The reference is a JUDGE here,
not a source of geometry. See `docs/reference-fitting-research.md` §6.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
import quadprobe as qp
import refsil
from forge import pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
REFS = ROOT / "refs"
SILS = REFS / "silhouettes"
SOURCES = SILS / "SOURCES.json"
LATIN = REFS / "species-latin.json"
EXTRACT = REFS / "quadruped-reference.json"
EXCLUDED = REFS / "excluded.json"
LIMBTARGET = REFS / "limb-target.json"
OUT = ROOT / "out" / "reffit"

SEED = 1

# LICENCES THIS PROJECT MAY CHECK IN. Recorded per FILE and never per site.
# This library has already rejected two datasets on licence grounds -- FishBase
# is non-commercial, and FishShapes contradicts itself between CC0 and CC BY-NC
# -- so the rule is that a licence is read off the artifact, not inferred from
# where it was found. NonCommercial, ShareAlike and NoDerivatives are all out:
# asset-forge feeds a commercial game, SA would reach into the output, and ND
# forbids the very act of reducing a picture to a measurement.
OK_LICENCES = {
    "https://creativecommons.org/publicdomain/zero/1.0/": "CC0 1.0",
    "https://creativecommons.org/publicdomain/mark/1.0/": "Public Domain Mark 1.0",
    "https://creativecommons.org/licenses/by/4.0/": "CC BY 4.0",
    "https://creativecommons.org/licenses/by/3.0/": "CC BY 3.0",
}

# A reference is only as good as its agreement with itself. One silhouette is an
# artist's opinion; four that agree are a measurement. Species below this many
# usable silhouettes are REPORTED but never FITTED.
MIN_SILHOUETTES = 2

# How far the usable silhouettes for one species may disagree before the species
# is treated as unmeasured.
#
# IT WAS (max-min)/median AND THAT PUNISHED A SPECIES FOR BEING WELL SAMPLED.
# The range of a sample can only grow as the sample grows, so a gate on the
# range is a gate on n as much as on disagreement. Measured over this corpus:
#
#     n = 2-3   (39 species)   median range/median 0.38
#     n = 4-6   (12 species)   median range/median 0.60
#     n = 7-11  ( 7 species)   median range/median 0.81
#     correlation of range/median with n: +0.67
#
# `plains-zebra` -- 7 silhouettes, and `docs/reference-fitting-research.md` §4
# calls it the best-sampled species in the corpus -- was thrown out by that, and
# so were `grey-wolf` (11), `reindeer` (9) and `wild-boar` (5). One striding or
# sitting outlier among eleven good drawings is exactly what a median is for,
# and the range gate handed the outlier the verdict instead.
#
# The interquartile spread does not do that (correlation +0.49, and flat from
# n=4 up: 0.25, 0.25). The number below is 0.275 rather than a fresh judgement
# because AT n = 2 AND n = 3 THE IQR IS EXACTLY HALF THE RANGE -- both quartiles
# fall between the same pair of order statistics -- so 0.275 reproduces the old
# 0.55 gate exactly on the sample sizes it was tuned against, and only changes
# the verdict for n >= 4. It admitted 5 species and dropped none.
MAX_SPREAD = 0.275

# THE MOST ANY ONE PASS MAY THICKEN A SPECIES, as a multiple of what it already
# measures. This is the general defence against a bad reference that no gate
# caught, and it is here because a specific one was found by eye and could not
# be found by any statistic: `refs/excluded.json` records the sitting fisher
# whose two silhouettes agreed with each other and asked for a limb 3.4x
# thicker than the spec draws.
#
# 1.5 is chosen so that a species can cross most of the distance to life in one
# pass -- run it twice and the cap compounds -- while no single undetected bad
# reference can rebuild an animal. Capped species are PRINTED as capped, which
# is the point: a 50% limb change is exactly the size of thing the owner should
# look at a render of rather than read about in a table.
MAX_LIFT = 1.5

# THE MOST ANY ONE PASS MAY THIN A SPECIES, and it is a SEPARATE number from
# MAX_LIFT because the two directions carry different risks.
#
# Thinning was forbidden outright until 2026-08-15 (see `fit`'s docstring and
# `docs/reference-fitting-research.md` §7 rejection 1) on the grounds that the
# references would undo the owner's own fix. That rejection was written when
# every authored `quad.leg_thick` was an owner-facing judgement. It is no longer
# true of most of them: measured, 49 of 131 specs carry the row's LOWER BOUND
# (0.05) and 11 carry values from 0.30 to 0.69 -- a limb two thirds as thick as
# it is long -- and both extremes were produced by solvers chasing the
# three-voxel readability gate at three different lattices, not by anybody
# looking at an animal. `docs/quadruped-limb-regression.md` has the working.
#
# So thinning is allowed, in one direction-symmetric bound, and it is defended
# by the SAME three things the owner's gates already are rather than by a
# prohibition: the drawn limb may never fall below `quadprobe.LIMB_MIN_VOX`
# voxels across, `quadprobe.SLENDER_MIN` still holds, and every species whose
# limb changes by more than a third is printed for a render.
MAX_THIN = 2.0


# ---------------------------------------------------------------- fetch (net)

def _curl(url: str, tries: int = 3) -> bytes:
    """`-L` IS NOT OPTIONAL. `https://api.phylopic.org/` answers 307 with an
    empty body, so without it the very first call of a fetch returns zero bytes
    and the JSON decoder raises on 'line 1 column 1' -- which reads like a
    corrupt API and is a missing flag."""
    last = b""
    for i in range(tries):
        r = subprocess.run(["curl", "-sSL", "-m", "60", url], capture_output=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout
        last = r.stdout or r.stderr
    raise RuntimeError(f"fetch failed after {tries} tries: {url}\n  {last[:200]!r}")


def _json(url: str):
    return json.loads(_curl(url))


def _quad_specs() -> list[str]:
    out = []
    for p in sorted(SPECS.glob("*.json")):
        try:
            s, _ = sm.load(p)
        except (OSError, ValueError):
            continue
        if sm.get(s, "kind") == "quadruped":
            out.append(p.stem)
    return out


def resolve_latin(names: list[str]) -> dict:
    """Common name -> scientific name, via GBIF, CONSTRAINED TO MAMMALIA.

    UNCONSTRAINED THIS RETURNS GARBAGE THAT LOOKS LIKE AN ANSWER. Verbatim, from
    `https://api.gbif.org/v1/species/search?q=brown%20bear&rank=SPECIES`:
    the top hit is *Protea speciosa*, a shrub. "red deer" returns "Red deerpox
    virus". Both come back as a confident single result with a clean scientific
    name, and a pipeline that trusted the first row would have fitted a deer to
    a virus and never printed anything odd.

    With `highertaxonKey=359` (Mammalia) the same six probes all resolve
    correctly. It is still a GUESS -- "moose" resolves to *Alces americanus*
    ahead of *Alces alces*, which is a genuine taxonomic split and not an error
    -- so the vernacular names GBIF returned are written into
    `refs/species-latin.json` beside each mapping. A wrong mapping is then
    visible on inspection instead of silently fitting one animal to another.
    """
    out = {}
    for n in names:
        q = n.replace("-", "%20")
        try:
            j = _json("https://api.gbif.org/v1/species/search?"
                      f"q={q}&rank=SPECIES&status=ACCEPTED&highertaxonKey=359&limit=1")
        except subprocess.CalledProcessError:
            continue
        res = j.get("results") or []
        if not res:
            continue
        r = res[0]
        out[n] = {
            "latin": r.get("canonicalName"),
            "gbif_key": r.get("key"),
            "gbif_vernacular": [v.get("vernacularName")
                                for v in (r.get("vernacularNames") or [])[:5]],
            "resolved_from": n,
            "checked_by_hand": False,
        }
    return out


def fetch(names: list[str], limit: int) -> int:
    """Download reference silhouettes and their licences. THE ONLY NET STEP."""
    SILS.mkdir(parents=True, exist_ok=True)
    latin = json.loads(LATIN.read_text(encoding="utf-8")) if LATIN.exists() else {}
    missing = [n for n in names if n not in latin]
    if missing:
        print(f"resolving {len(missing)} scientific names via GBIF (Mammalia only)")
        latin.update(resolve_latin(missing))
        LATIN.write_text(json.dumps(latin, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")

    src = json.loads(SOURCES.read_text(encoding="utf-8")) if SOURCES.exists() else {}
    build = _json("https://api.phylopic.org/")["build"]
    print(f"PhyloPic build {build}\n")
    print(f"{'species':<26} {'scientific name':<30} {'kept':>5} {'refused':>8}")
    for n in names:
        info = latin.get(n)
        if not info or not info.get("latin"):
            print(f"{n:<26} {'-- no scientific name --':<30}")
            continue
        # A CHANGED NAME MUST TAKE ITS OLD PICTURES WITH IT. `nile-monitor`
        # first resolved to *Mungos mungo* and downloaded two banded-mongoose
        # silhouettes, which then measured cleanly and were promoted to
        # "fit-quality". Correcting the name alone would have left the mongoose
        # on disk and still fitted a monitor lizard to it.
        #
        # Files only, one directory deep, never a recursive tree delete: this
        # machine has lost a cache to a recursive delete that followed a Windows
        # junction, and `refs/` is not worth repeating that on.
        old = (src.get(n) or {}).get("scientific_name")
        if old and old != info["latin"]:
            d = SILS / n
            gone = 0
            if d.is_dir() and not d.is_symlink():
                for f in d.glob("*.png"):
                    f.unlink()
                    gone += 1
            print(f"{n:<26} name changed {old} -> {info['latin']}: "
                  f"dropped {gone} stale silhouette(s)")
            src.pop(n, None)

        ln = info["latin"].lower().replace(" ", "+")
        try:
            j = _json(f"https://api.phylopic.org/images?build={build}"
                      f"&page=0&filter_name={ln}")
        except subprocess.CalledProcessError:
            print(f"{n:<26} {info['latin']:<30}  query failed")
            continue
        items = (j.get("_links") or {}).get("items") or []
        kept, refused = [], []
        for it in items[:limit]:
            uid = it["href"].split("/")[-1].split("?")[0]
            try:
                e = _json(f"https://api.phylopic.org/images/{uid}?build={build}")
            except subprocess.CalledProcessError:
                continue
            lic = ((e.get("_links") or {}).get("license") or {}).get("href", "")
            if lic not in OK_LICENCES:
                refused.append((it["title"], lic))
                continue
            rasters = (e["_links"] or {}).get("rasterFiles") or []
            big = max(rasters, key=lambda r: int(r["sizes"].split("x")[0]),
                      default=None)
            if not big:
                continue
            d = SILS / n
            d.mkdir(parents=True, exist_ok=True)
            path = d / f"{uid[:8]}.png"
            if not path.exists():
                path.write_bytes(_curl(big["href"]))
            kept.append({
                "file": f"{n}/{uid[:8]}.png",
                "uuid": uid,
                "phylopic_title": it["title"],
                "licence": OK_LICENCES[lic],
                "licence_url": lic,
                "contributor": ((e["_links"] or {}).get("contributor") or {}).get("title"),
                "attribution": e.get("attribution"),
                "source_url": f"https://www.phylopic.org/images/{uid}",
                "raster_url": big["href"],
                "raster_size": big["sizes"],
            })
        if kept:
            src[n] = {"scientific_name": info["latin"],
                      "phylopic_build": build, "images": kept,
                      "refused": [{"title": t, "licence_url": l} for t, l in refused]}
        print(f"{n:<26} {info['latin']:<30} {len(kept):>5} {len(refused):>8}")
    SOURCES.write_text(json.dumps(src, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
    _write_attribution(src)
    print(f"\nwrote {SOURCES}")
    return 0


def _write_attribution(src: dict) -> None:
    """CC BY files carry an obligation. Discharge it in a file, not in a promise."""
    lines = ["# Reference silhouettes -- attribution and licences",
             "",
             "Generated by `tools/reffit.py fetch`. Every line was read from the",
             "PhyloPic API's own per-image `_links.license.href` field, never",
             "inferred from the site. Files under a NonCommercial, ShareAlike or",
             "NoDerivatives licence are refused at download and are not present.",
             ""]
    for sp in sorted(src):
        rec = src[sp]
        lines.append(f"## {sp} -- *{rec['scientific_name']}*")
        lines.append("")
        for im in rec["images"]:
            who = im.get("attribution") or im.get("contributor") or "unattributed"
            lines.append(f"- `{im['file']}` -- {im['licence']} -- {who} -- "
                         f"<{im['source_url']}>")
        if rec.get("refused"):
            lines.append("")
            lines.append(f"  refused on licence: {len(rec['refused'])} file(s)")
        lines.append("")
    (SILS / "ATTRIBUTION.md").write_text("\n".join(lines), encoding="utf-8")


# ------------------------------------------------------------ extract (local)

def _spread(v: np.ndarray) -> float:
    """Interquartile spread over the median. See `MAX_SPREAD` for why not the
    range. At n = 2 and n = 3 this is exactly half the range, by construction of
    the quartiles, which is what makes the new gate continuous with the old."""
    med = float(np.median(v))
    if not np.isfinite(med) or med <= 0:
        return float("inf")
    q1, q3 = np.percentile(v, [25, 75])
    return float((q3 - q1) / med)


def _species_sils(sp: str) -> list[Path]:
    d = SILS / sp
    return sorted(d.glob("*.png")) if d.is_dir() else []


def extract(verbose: bool) -> int:
    """Measure every checked-in silhouette; aggregate the usable ones.

    THE REJECTIONS ARE THE POINT AND THEY ARE PRINTED. A silhouette corpus is
    not a clean dataset: it contains swimming hippopotamuses, rearing bears,
    head-on views and disembodied heads, and every one of those produces a
    NUMBER rather than an error. `refsil.usable` is what stands between those
    numbers and a spec file.
    """
    src = json.loads(SOURCES.read_text(encoding="utf-8")) if SOURCES.exists() else {}
    excl = {}
    if EXCLUDED.exists():
        excl = (json.loads(EXCLUDED.read_text(encoding="utf-8")) or {}).get("excluded", {})
    out = {}
    tot_ok = tot_no = tot_hand = 0
    print(f"{'species':<26} {'n':>3} {'used':>4} | {'t/L':>6} {'spread':>13} "
          f"{'belly/L':>8} {'H/L':>6}  status")
    for sp in sorted(src):
        rows, bad, hand = [], [], []
        for p in _species_sils(sp):
            key = f"{sp}/{p.name}"
            if key in excl:
                hand.append((p.name, excl[key]))
                continue
            o = refsil.measure(refsil.from_png(p))
            why = refsil.usable(o)
            if why:
                bad.append((p.name, why))
            else:
                o["file"] = key
                rows.append(o)
        tot_ok += len(rows)
        tot_no += len(bad)
        tot_hand += len(hand)
        if not rows:
            print(f"{sp:<26} {len(bad):>3} {0:>4} | {'--':>6} "
                  f"{'':>13} {'':>8} {'':>6}  NO USABLE SILHOUETTE")
            if verbose:
                for f, w in bad:
                    print(f"      refused {f}: {w}")
            continue
        s = np.array([r["slender"] for r in rows])
        b = np.array([r["belly_over_length"] for r in rows])
        h = np.array([r["height_over_length"] for r in rows])
        t = np.array([r["trunk_over_length"] for r in rows])
        g = np.array([r["leg_share"] for r in rows])
        med = float(np.median(s))
        spread = _spread(s)
        fit_ok = len(rows) >= MIN_SILHOUETTES and spread <= MAX_SPREAD
        status = "ok" if fit_ok else (
            f"n={len(rows)} < {MIN_SILHOUETTES}" if len(rows) < MIN_SILHOUETTES
            else f"spread {spread:.2f} > {MAX_SPREAD}")
        # LEG SHARE IS GATED SEPARATELY, AND IT EARNS IT. Where the legs join
        # the body is a topology fact that two artists agree about; how thick a
        # limb is drawn is an opinion. Measured over the same corpus, the
        # interquartile spread of `leg_share` has a median of 0.089 against
        # 0.25 for `slender`, so 53 species reach fit quality on stance where 42
        # do on thickness. Sharing one gate would have thrown away eleven
        # perfectly good stance references to defend a thickness nobody was
        # going to use.
        gspread = _spread(g)
        gfit = len(rows) >= MIN_SILHOUETTES and gspread <= MAX_SPREAD
        out[sp] = {
            "leg_share": round(float(np.median(g)), 4),
            "leg_share_spread": round(gspread, 4),
            "leg_share_quality": "fit" if gfit else "report-only",
            "scientific_name": src[sp]["scientific_name"],
            "n_usable": len(rows),
            "n_refused": len(bad),
            "n_refused_by_hand": len(hand),
            "refused": [{"file": f, "why": w} for f, w in bad],
            "refused_by_hand": [{"file": f, "why": w} for f, w in hand],
            "used": [r["file"] for r in rows],
            "slender": round(med, 4),
            "slender_min": round(float(s.min()), 4),
            "slender_max": round(float(s.max()), 4),
            "slender_spread": round(spread, 4),
            "belly_over_length": round(float(np.median(b)), 4),
            "height_over_length": round(float(np.median(h)), 4),
            "trunk_over_length": round(float(np.median(t)), 4),
            "fit_quality": "fit" if fit_ok else "report-only",
        }
        print(f"{sp:<26} {len(rows) + len(bad):>3} {len(rows):>4} | {med:>6.3f} "
              f"{s.min():>6.3f}-{s.max():<6.3f} {np.median(b):>8.2f} "
              f"{np.median(h):>6.2f}  {status}")
        if verbose:
            for f, w in bad:
                print(f"      refused {f}: {w}")
    EXTRACT.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
    fits = sum(1 for v in out.values() if v["fit_quality"] == "fit")
    print(f"\n  {tot_ok} silhouettes usable, {tot_no} refused")
    print(f"  {fits} species measurable well enough to fit, "
          f"{len(out) - fits} report-only")
    print(f"  wrote {EXTRACT}")
    return 0


# ------------------------------------------------------------- report / fit

def _build(spec: dict, seed: int = SEED):
    flat, _ = sm.patch(spec, {"variation.amount": 0.0})
    return pipeline.build(flat, seed)


def _asset_sil(spec: dict, seed: int = SEED) -> dict | None:
    a = _build(spec, seed)
    data = a.grid.data if hasattr(a, "grid") else a.data
    return refsil.measure(refsil.from_grid(data))


def _refs() -> dict:
    if not EXTRACT.exists():
        raise SystemExit(f"no reference file at {EXTRACT}\n"
                         "  run: python tools/reffit.py extract")
    return json.loads(EXTRACT.read_text(encoding="utf-8"))


def report(only: list[str] | None) -> int:
    """How far is the library from the references, per species and per ratio."""
    refs = _refs()
    names = [n for n in sorted(refs) if (not only or n in only)
             and (SPECS / f"{n}.json").exists()]
    if not names:
        print("no species with both a reference and a spec")
        return 0
    print("\nHOW FAR THE LIBRARY IS FROM THE REFERENCES")
    print("  reference and asset measured by the SAME code (tools/refsil.py),")
    print("  so the definition bias against quadprobe cancels. variation off, seed 1.\n")
    print(f"{'species':<24} {'q':<5}| {'ref t/L':>7} {'our t/L':>7} {'x':>5} | "
          f"{'ref b/L':>7} {'our b/L':>7} {'x':>5} | {'ref H/L':>7} {'our H/L':>7} {'x':>5}")
    rows = []
    for n in names:
        r = refs[n]
        spec, _ = sm.load(SPECS / f"{n}.json")
        o = _asset_sil(spec)
        if o is None:
            print(f"{n:<24} {'':<5}|  our own asset has no belly line -- not measurable")
            continue
        q = "fit" if r["fit_quality"] == "fit" else "rep"
        ts, bs, hs = (o["slender"] / r["slender"],
                      o["belly_over_length"] / r["belly_over_length"],
                      o["height_over_length"] / r["height_over_length"])
        rows.append((n, r, o, ts, bs, hs))
        print(f"{n:<24} {q:<5}| {r['slender']:>7.3f} {o['slender']:>7.3f} {ts:>5.2f} | "
              f"{r['belly_over_length']:>7.2f} {o['belly_over_length']:>7.2f} {bs:>5.2f} | "
              f"{r['height_over_length']:>7.2f} {o['height_over_length']:>7.2f} {hs:>5.2f}")
    if not rows:
        return 0
    for label, i in (("limb thickness / limb length", 3),
                     ("belly clearance / body length", 4),
                     ("total height / body length", 5)):
        v = np.array([r[i] for r in rows])
        print(f"\n  {label}: median {np.median(v):.2f}x reference, "
              f"range {v.min():.2f}-{v.max():.2f}x, "
              f"{int((np.abs(v - 1) <= 0.10).sum())} of {len(v)} within 10%")
    return 0


def _solve(spec: dict, row: str, want: float, read, lo: float, hi: float,
           rounds: int = 5) -> tuple[float, float]:
    """Move one spec row until a MEASURED quantity reaches `want`.

    Secant, on the assumption that the measurement is roughly proportional to
    the row -- which it is, both being a radius times a multiplier -- but
    measured again every step rather than trusted, because the voxel grid
    quantises a limb to whole voxels. Same shape as
    `tools/retune_quad_bulk._solve`, and deliberately so: two solvers that
    disagree about how to converge would make their two before/after tables
    incomparable.
    """
    value = float(sm.get(spec, row))
    tried = [(value, read(_asset_sil(sm.patch(spec, {row: value})[0])))]
    for _ in range(rounds):
        last, got = tried[-1]
        if not np.isfinite(got) or got <= 0:
            break
        if abs(got - want) / want < 0.03:
            break
        nxt = float(np.clip(last * want / got, lo, hi))
        if abs(nxt - last) < 1e-4:
            break
        tried.append((nxt, read(_asset_sil(sm.patch(spec, {row: nxt})[0]))))
    ok = [t for t in tried if np.isfinite(t[1]) and t[1] > 0]
    if not ok:
        return tried[0]
    return min(ok, key=lambda t: abs(t[1] - want))


def _read_slender(o) -> float:
    return float("nan") if o is None else o["slender"]


def _drawn(spec: dict, seed: int = SEED) -> tuple[float, float, float]:
    """What the OWNER'S OWN GATES see: limb voxels across, limb voxels long, and
    girth over withers, on the built asset rather than on a silhouette.

    `refsil` and `quadprobe` measure two different limbs on purpose -- the
    silhouette starts at the belly line, the probe starts at the part tag -- and
    the fit is solved against the silhouette because that is what the references
    are measured with. But the GATES are the probe's, so a thinning pass has to
    be checked against the probe or it would solve one ruler and fail the other.
    """
    a = _build(spec, seed)
    return qp.m_limb_dia(a), qp.m_limb_len(a), qp.m_trunk_girth(a) / max(
        qp.m_withers_h(a), 1e-6)


def _limb_floor(name: str) -> float:
    """The lowest `quadprobe` thickness/length this species may be taken to.

    NOT A JUDGEMENT. It is what the library actually measured at commit 17cc742
    -- the state whose `quadprobe --bulk` thickness/length median is **0.250**,
    which is the number the owner named when he said the ratio must not fall.
    Frozen in `refs/limb-target.json` rather than recomputed, because the row
    this file solves is the row that would otherwise carry the baseline: a
    solver reading its own output as its own floor thins the library a little
    further on every run and prints a clean table each time.
    `tools/refstance.py` hit exactly this and its fix was the same.

    THE FLOOR IS THE RATIO AND NOT THE DIAMETER, and that distinction was
    measured rather than assumed. Against the 17cc742 diameters, 38 of 131
    species are ALREADY thinner today -- the stance pass took a zebra from 7
    voxels across to 5 -- so a diameter floor would refuse every move including
    the ones that fix the regression. Against the 17cc742 RATIO, 0 of 131 are
    below, because the visible limb shortened faster than the limb thinned. The
    ratio is the quantity the owner's sentence is about and it is the one with
    headroom.

    Without this floor the references take twelve species to three voxels across
    -- `brown-bear` 7 -> 3, `wood-bison` 5 -> 3, `warthog` 5 -> 3 -- which is the
    wireframe library the owner rejected, rebuilt on the authority of a
    silhouette. `docs/reference-fitting-research.md` §7 rejection 1 is right
    about that, and this is how it survives the reference being allowed to thin.
    """
    if not LIMBTARGET.exists():
        return qp.SLENDER_MIN
    rec = (json.loads(LIMBTARGET.read_text(encoding="utf-8"))
           .get("species", {}).get(name) or {})
    # THE HIGHEST OF THE THREE, not the newest. The guarantee on record is that
    # the ratio "rose on every species and fell on none" against the
    # PRE-2026-08-15 library as well, and a floor taken from the newest state
    # alone let `roe-deer` fall 0.208 -> 0.167 against the older one. Two
    # baselines, one floor.
    ts = [float(rec[k]) for k in ("t_over_L", "t_over_L_040fb58",
                                  "t_over_L_90f512c") if rec.get(k)]
    return max([qp.SLENDER_MIN] + ts)


def _thin_ok(spec: dict, name: str = "") -> str | None:
    """Why this candidate may NOT be written, in the owner's own units."""
    dia, ln, gw = _drawn(spec)
    floor = _limb_floor(name) if name else qp.SLENDER_MIN
    if not np.isfinite(dia) or dia < qp.LIMB_MIN_VOX:
        return f"foreleg {dia:.0f} voxels across, floor {qp.LIMB_MIN_VOX:g}"
    if np.isfinite(ln) and ln > 0 and dia / ln < floor:
        return (f"thickness/length {dia / ln:.3f}, floor {floor:.3f}"
                + (" (the 0.250-median library)" if floor > qp.SLENDER_MIN
                   else ""))
    if np.isfinite(gw) and gw < qp.GIRTH_MIN:
        return f"girth/withers {gw:.2f}, floor {qp.GIRTH_MIN:.2f}"
    return None


def _write(path: Path, changes: dict) -> None:
    raw = json.loads(path.read_text(encoding="utf-8"))
    for dotted, value in changes.items():
        group, row = dotted.split(".", 1)
        raw.setdefault(group, {})[row] = value
    path.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def fit(only: list[str] | None, dry: bool, thin: bool = False) -> int:
    """Solve `quad.leg_thick` to the measured reference.

    UPWARD ONLY BY DEFAULT, AND THAT DEFAULT IS DELIBERATE. Taken literally, the
    references said our red deer's legs were 41% TOO THICK and should come down
    from 0.174 to 0.123 -- and that was the exact animal the owner looked at,
    called a wireframe, and had rebuilt. A photoreal target would have undone
    that fix on the authority of a silhouette rather than a render. So the plain
    `fit` still only lifts, and `docs/reference-fitting-research.md` §7 records
    that as a rejection with its numbers.

    `--thin` IS THE OTHER DIRECTION AND IT NEEDED A REASON, WHICH ARRIVED. The
    stance fix lowered the whole library onto its true withers, which shortened
    every VISIBLE limb without thinning it, and the ratio the references judge
    went from 1.04x life to 1.46x with a mean absolute error of 69%. Chasing
    that with the row is only legitimate because the row turned out not to be
    carrying anybody's judgement on most species: 49 specs sit on the row's
    lower bound and 11 sit between 0.30 and 0.69. Those are solver artefacts of
    the three-voxel readability gate at three lattices, and thinning them is
    repairing a measurement, not overruling the owner.

    The prohibition is therefore replaced by the owner's OWN gates rather than
    lifted: `_thin_ok` refuses any candidate that would draw a limb under
    `quadprobe.LIMB_MIN_VOX` voxels across, or below `SLENDER_MIN`, or below the
    girth floor, and `MAX_THIN` bounds one pass. A species that cannot be thinned
    without breaking a gate is printed as held, with the gate that held it.
    """
    refs = _refs()
    # THE HAND-CHECK GATE, AND IT IS NOT CEREMONY. GBIF's common-name search
    # resolved `nile-monitor` to *Mungos mungo* and `lion` to *Macaca silenus*,
    # and the mongoose reference passed every quality test this file has: two
    # silhouettes, both measurable, in close agreement. Nothing downstream can
    # tell a good measurement of the wrong animal from a good measurement of the
    # right one. Only `tools/refnames.py` can, and only a name it has verified
    # against GBIF's own vernacular list is allowed to move a spec.
    latin = json.loads(LATIN.read_text(encoding="utf-8")) if LATIN.exists() else {}
    names, unchecked, stale = [], [], []
    for n in sorted(refs):
        if only and n not in only:
            continue
        if refs[n]["fit_quality"] != "fit" or not (SPECS / f"{n}.json").exists():
            continue
        if not (latin.get(n) or {}).get("checked_by_hand"):
            unchecked.append(n)
            continue
        # A VERIFIED NAME IS NOT THE SAME THING AS VERIFIED PICTURES, and that
        # gap is how the mongoose would have got in the second time.
        # `refs/species-latin.json` records what the name was checked to be;
        # `SOURCES.json` records what the files on disk were downloaded AS. Fix
        # a name by hand without re-fetching and they disagree, and everything
        # downstream reports a verified species measured off the wrong animal.
        # Found live, in this repository, on the run that added this check:
        # `common-frog` carried a verified *Rana temporaria* over silhouettes
        # downloaded as *Mustela lutreola*, a European mink.
        want = (latin.get(n) or {}).get("latin")
        got = (refs[n] or {}).get("scientific_name")
        if want and got and want != got:
            stale.append((n, want, got))
            continue
        names.append(n)
    print("\nFIT quad.leg_thick TO THE MEASURED REFERENCE (never reduced)\n")
    if unchecked:
        print(f"  {len(unchecked)} species have a fit-quality reference but an "
              f"UNVERIFIED scientific name and are skipped:")
        print(f"    {', '.join(unchecked)}")
        print("    run: python tools/refnames.py --write\n")
    if stale:
        print(f"  {len(stale)} species have a verified name that does NOT match "
              f"the silhouettes on disk and are REFUSED:")
        for n, want, got in stale:
            print(f"    {n:<24} verified {want}, files downloaded as {got}")
        print("    run: python tools/reffit.py fetch --only "
              f"{' '.join(n for n, _, _ in stale)}\n")
    if not names:
        print("  no species qualifies: needs a spec, a 'fit'-quality reference, "
              "and a hand-checked name")
        return 0
    print(f"{'species':<24} {'leg_thick':>16} {'limb t/L (silhouette)':>30}  outcome")
    moved = 0
    short: list[tuple[str, float, float]] = []
    cappedlist: list[tuple[str, float, float]] = []
    heldlist: list[tuple[str, str]] = []
    looklist: list[tuple[str, float, float]] = []
    for n in names:
        path = SPECS / f"{n}.json"
        spec, _ = sm.load(path)
        want = float(refs[n]["slender"])
        before = _asset_sil(spec)
        s0 = _read_slender(before)
        t0 = float(sm.get(spec, "quad.leg_thick"))
        if not np.isfinite(s0):
            print(f"{n:<24} {t0:>7.3f} {'':>8} {'not measurable':>30}  skipped")
            continue
        if thin and s0 > want * 1.03:
            d0, _, _ = _drawn(spec)
            aim = max(want, s0 / MAX_THIN)
            t1, s1 = _solve(spec, "quad.leg_thick", aim, _read_slender, 0.02, 0.90)
            # WALK IT BACK UNTIL IT PASSES THE OWNER'S GATES rather than
            # refusing outright. A species whose reference asks for less than
            # three voxels can still take part of the distance, and taking part
            # of it is the whole difference between a library at 1.46x life and
            # one at 1.2x. The candidate is rebuilt at every step because the
            # gates are measured on the DRAWN asset, not predicted from the row.
            why = None
            for frac in (1.0, 0.75, 0.5, 0.25):
                cand = t0 + (t1 - t0) * frac
                if cand >= t0 - 1e-4:
                    t1, why = t0, _thin_ok(sm.patch(spec, {"quad.leg_thick": t0})[0], n)
                    break
                why = _thin_ok(sm.patch(spec, {"quad.leg_thick": round(cand, 3)})[0], n)
                if why is None:
                    t1 = round(cand, 3)
                    s1 = _read_slender(_asset_sil(
                        sm.patch(spec, {"quad.leg_thick": t1})[0]))
                    break
            if why is not None or t1 >= t0 - 1e-4:
                heldlist.append((n, why or "solver found nothing thinner"))
                print(f"{n:<24} {t0:>7.3f} -> {t0:<6.3f} {s0:>13.3f} vs {want:<6.3f} "
                      f"{'':>3}  HELD: {why or 'solver found nothing thinner'}")
                continue
            moved += 1
            d1, _, _ = _drawn(sm.patch(spec, {"quad.leg_thick": t1})[0])
            note = ""
            if np.isfinite(d0) and d0 > 0 and abs(d1 - d0) / d0 > 0.33:
                looklist.append((n, d0, d1))
                note = f"  LIMB {d0:.0f} -> {d1:.0f} VOXELS ACROSS -- LOOK AT IT"
            elif s1 > want * 1.10:
                note = f"  still {s1 / want:.2f}x life"
            print(f"{n:<24} {t0:>7.3f} -> {t1:<6.3f} {s0:>13.3f} -> {s1:<6.3f} "
                  f"(want {want:.3f}){note}")
            if not dry:
                _write(path, {"quad.leg_thick": round(t1, 3)})
            continue
        if s0 >= want * 0.97:
            print(f"{n:<24} {t0:>7.3f} -> {t0:<6.3f} {s0:>13.3f} vs {want:<6.3f} "
                  f"{'':>3}  already at or above life")
            continue
        capped = want > s0 * MAX_LIFT
        aim = min(want, s0 * MAX_LIFT)
        t1, s1 = _solve(spec, "quad.leg_thick", aim, _read_slender, 0.05, 0.90)
        if t1 <= t0 + 1e-4:
            print(f"{n:<24} {t0:>7.3f} -> {t0:<6.3f} {s0:>13.3f} vs {want:<6.3f} "
                  f"{'':>3}  SOLVER FOUND NOTHING BETTER")
            continue
        moved += 1
        note = ""
        if t1 >= float(sm.BY_PATH["quad.leg_thick"].hi) - 1e-6:
            note = "  AT THE ROW'S CEILING"
        # STOPPING SHORT IS A REPORTED OUTCOME, NOT A BLANK. A thicker limb
        # STANDS THE ANIMAL HIGHER, and the animal's height is the denominator
        # of the very ratio being solved -- so on the stockiest species the
        # measurement chases the parameter and saturates well below the target.
        # `wolverine` wants 0.457 and stops at 0.235 for exactly that reason.
        # Printing the achieved number and nothing else would let a species that
        # reached half its target pass as done, which is this project's
        # signature failure wearing a solved-looking table row.
        elif capped:
            cappedlist.append((n, s1, want))
            note = f"  CAPPED at {MAX_LIFT:g}x (life wants {want:.3f}) -- LOOK AT IT"
        elif s1 < aim * 0.90:
            short.append((n, s1, aim))
            note = f"  STOPPED SHORT at {s1 / aim:.0%} of target"
        print(f"{n:<24} {t0:>7.3f} -> {t1:<6.3f} {s0:>13.3f} -> {s1:<6.3f} "
              f"(want {want:.3f}){note}")
        if not dry:
            _write(path, {"quad.leg_thick": round(t1, 3)})
    print(f"\n  {moved} species {'moved' if thin else 'thickened'}, "
          f"{len(names) - moved} left alone"
          f"{'  (DRY RUN -- nothing written)' if dry else ''}")
    for n, why in heldlist:
        print(f"  HELD: {n} is above life and cannot be thinned -- {why}")
    for n, d0, d1 in looklist:
        print(f"  LOOK AT IT: {n} went {d0:.0f} -> {d1:.0f} voxels across. "
              f"That is a render, not a table row.")
    for n, got, want in cappedlist:
        print(f"  CAPPED: {n} reached {got:.3f}; the reference asks {want:.3f}. "
              f"Re-run to go further, and look at a render before you do.")
    for n, got, want in short:
        print(f"  STILL SHORT: {n} reached {got:.3f} of a wanted {want:.3f} -- "
              f"a thicker limb also stands the animal higher, so this ratio "
              f"cannot be reached by limb thickness alone")
    return 0


def _stance_gaps(spec: dict, seeds=(1, 2, 3)) -> tuple[float, float]:
    """Fore and hind foot clearance, averaged, on the VARIED draw."""
    f, h = [], []
    for s in seeds:
        a = pipeline.build(spec, s)
        f.append(qp.m_fore_gap(a))
        h.append(qp.m_hind_gap(a))
    return float(np.nanmean(f)), float(np.nanmean(h))


def _settle_dead(name: str, old: float, honest: float) -> tuple[float, bool]:
    """Back the honest value off until the VARIED draw is unchanged too.

    THE HOLE THIS PLUGS IS ALREADY WRITTEN DOWN IN THIS REPOSITORY, in
    `docs/quadruped-stance-height.md` §4: *"the solver pins the variation draw
    off and the probe does not, so the pinned build was clean and the failure
    only existed on the drawn one."* It cost `fennec-fox` a `--parts` failure
    then and it cost `lesser-egyptian-jerboa` a `--stance` failure here.

    `2.0 / limb_v` puts the radius exactly on the one-voxel floor AT THE PINNED
    SIZE. `quad.shoulder_h`, `quad.hip_h` and `quad.length_m` are all varied, so
    an individual drawn a few per cent larger has a longer limb, and at exactly
    the floor that individual's radius crosses it -- which is fine on most
    species and lifted the jerboa's hind feet two voxels off the ground. So the
    value is walked down in eighths until the varied draw measures what it did
    before, and a species that cannot be settled keeps the value it had.
    """
    path = SPECS / f"{name}.json"
    spec, _ = sm.load(path)
    f0, h0 = _stance_gaps(sm.patch(spec, {"quad.leg_thick": old})[0])
    for k in range(9):
        cand = honest * (1.0 - k / 8.0)
        if cand <= old + 1e-9:
            return old, k > 0
        f1, h1 = _stance_gaps(sm.patch(spec, {"quad.leg_thick": round(cand, 3)})[0])
        if abs(f1 - f0) <= 0.01 and abs(h1 - h0) <= 0.01:
            return round(cand, 3), k > 0
    return old, True


def dead(only: list[str] | None, write: bool) -> int:
    """Find every spec whose `quad.leg_thick` the generator CANNOT HONOUR.

    THIS IS THE SILENT NO-OP THIS PROJECT KEEPS PAYING FOR, and it shipped for a
    day inside the fix that was meant to end it. `forge/quadruped.py` computes

        leg_r = max(1.0, 0.5 * quad.leg_thick * limb_v)

    so on any species where `0.5 * leg_thick * limb_v` is under one voxel THE
    ROW DOES NOTHING: the floor draws the limb, three voxels across, whatever
    the spec says. Measured on 2026-08-15 that was 68 of 131 species, and 49 of
    them carry the row's own LOWER BOUND, 0.05 -- which is not an authored
    number at all. It is what the 90f512c conversion clamped to when its
    arithmetic went out of range, and the floor then hid the clamp perfectly:
    every one of those 49 renders exactly as it would with any other value, so
    no render, no gate and no sweep could ever have shown it. `red-fox` went
    0.176 -> 0.05 in that commit and nothing changed on screen.

    `--write` replaces a dead value with the ratio THE FLOOR IS ACTUALLY
    DRAWING, `2.0 / limb_v`, which changes no voxel and makes the row state what
    is on screen. That is deliberately not a fit: where a reference exists,
    `fit` should do the work and this should not pre-empt it. It is the
    difference between a spec that lies and a spec that is merely coarse.
    """
    from forge import quadruped as quad
    rows = []
    for name in _quad_specs():
        if only and name not in only:
            continue
        path = SPECS / f"{name}.json"
        spec, _ = sm.load(path)
        flat, _ = sm.patch(spec, {"variation.amount": 0.0})
        p = quad._params(flat, np.random.default_rng(SEED),
                         pipeline.resolution_m(flat))
        limb_v = max(1.0, 0.5 * (p["fore_len"] + p["hind_len"]))
        t0 = float(sm.get(flat, "quad.leg_thick"))
        want = 0.5 * t0 * limb_v
        if want >= 1.0 - 1e-9:
            continue
        # FLOORED TO THREE DECIMALS, NOT ROUNDED. `2.0 / limb_v` puts the radius
        # exactly on the one-voxel floor, and rounding UP would put it a
        # thousandth above -- at which point `max(1.0, ...)` stops governing and
        # the capsule rasteriser is free to add a corner voxel. A repair that
        # claims to change no geometry has to be arithmetically incapable of it.
        honest = np.floor(min(0.90, max(0.02, 2.0 / limb_v)) * 1000.0) / 1000.0
        rows.append((name, t0, limb_v, want, float(honest)))
    lo = float(sm.BY_PATH["quad.leg_thick"].lo)
    at_lo = [r for r in rows if abs(r[1] - lo) < 1e-9]
    print("\nSPEC ROWS THE GENERATOR CANNOT HONOUR (quad.leg_thick)\n")
    print(f"{'species':<28} {'authored':>9} {'limb_v':>8} {'radius asked':>13} "
          f"{'drawn radius':>13} {'honest value':>13}")
    for name, t0, limb_v, want, hon in rows:
        print(f"{name:<28} {t0:>9.3f} {limb_v:>8.2f} {want:>13.2f} "
              f"{1.0:>13.2f} {hon:>13.3f}"
              f"{'   AT THE ROW LOW BOUND' if abs(t0 - lo) < 1e-9 else ''}")
    print(f"\n  {len(rows)} of {len(_quad_specs())} species draw their limb from "
          f"the one-voxel radius floor, not from the row")
    print(f"  {len(at_lo)} of those carry the row's own lower bound {lo:g}, "
          f"which is a clamp and not an authored value")
    if write:
        n_back = 0
        for name, t0, _, _, hon in rows:
            hon, backed = _settle_dead(name, t0, hon)
            n_back += int(backed)
            _write(SPECS / f"{name}.json", {"quad.leg_thick": hon})
        print(f"  rewrote {len(rows)} specs to the ratio the floor already draws "
              f"-- NO VOXEL CHANGES at the pinned size; verify with "
              f"tools/quadprobe.py --bulk")
        if n_back:
            print(f"  {n_back} backed off below it because the VARIED draw does "
                  f"leave the floor -- see `_settle_dead`")
    else:
        print("  --write rewrites them to the ratio already on screen "
              "(no voxel changes)")
    return 0


def overlay(only: list[str] | None) -> int:
    """Draw every measurement on its own silhouette, reference and ours."""
    refs = _refs()
    d = OUT / "overlay"
    d.mkdir(parents=True, exist_ok=True)
    n_out = 0
    for sp in sorted(refs):
        if only and sp not in only:
            continue
        for p in _species_sils(sp):
            m = refsil.from_png(p)
            o = refsil.measure(m)
            if refsil.usable(o):
                continue
            refsil.overlay(m, o).save(d / f"{sp}__ref__{p.stem}.png")
            n_out += 1
        sp_path = SPECS / f"{sp}.json"
        if sp_path.exists():
            spec, _ = sm.load(sp_path)
            a = _build(spec)
            data = a.grid.data if hasattr(a, "grid") else a.data
            m = refsil.from_grid(data)
            o = refsil.measure(m)
            if o and not refsil.usable(o):
                refsil.overlay(m, o, scale=4).save(d / f"{sp}__ours.png")
                n_out += 1
    print(f"wrote {n_out} overlays to {d}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="ONLINE: download reference silhouettes")
    f.add_argument("--only", nargs="*")
    f.add_argument("--limit", type=int, default=12,
                   help="most PhyloPic images to consider per species")
    e = sub.add_parser("extract", help="silhouettes -> checked-in numbers")
    e.add_argument("-v", "--verbose", action="store_true",
                   help="print why each silhouette was refused")
    r = sub.add_parser("report", help="how far the library is from the references")
    r.add_argument("--only", nargs="*")
    t = sub.add_parser("fit", help="solve quad.leg_thick to the reference")
    t.add_argument("--only", nargs="*")
    t.add_argument("--dry", action="store_true", help="print, write nothing")
    t.add_argument("--thin", action="store_true",
                   help="also bring species DOWN to life, gated on the probe's "
                        "own limb, slenderness and girth floors")
    d = sub.add_parser("dead", help="specs whose quad.leg_thick the floor overrides")
    d.add_argument("--only", nargs="*")
    d.add_argument("--write", action="store_true",
                   help="rewrite dead rows to the ratio already drawn "
                        "(changes no voxels)")
    o = sub.add_parser("overlay", help="draw the measurement on the silhouette")
    o.add_argument("--only", nargs="*")
    a = ap.parse_args()

    if a.cmd == "fetch":
        return fetch(a.only or _quad_specs(), a.limit)
    if a.cmd == "extract":
        return extract(a.verbose)
    if a.cmd == "report":
        return report(a.only)
    if a.cmd == "fit":
        return fit(a.only, a.dry, a.thin)
    if a.cmd == "dead":
        return dead(a.only, a.write)
    if a.cmd == "overlay":
        return overlay(a.only)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
