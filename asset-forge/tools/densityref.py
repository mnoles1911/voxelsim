"""How many of this animal live in a square kilometre, measured rather than felt.

    python tools/densityref.py fetch      # ONLINE. downloads PanTHERIA
    python tools/densityref.py extract    # offline. -> refs/density-reference.json
    python tools/densityref.py report     # offline. coverage and how good it is

WHY THIS EXISTS. The spawn rows (`herd.per_hectare`, `flock.per_hectare`,
`detail.per_100m2`) were authored as NEAR-FIELD APPEARANCE numbers -- how thick
should this look from twenty metres -- and the owner has since set the spawn
radius to 1-2 km. Read literally over that bubble the library asks for ~131,000
land animals in grassland. Real animals are not distributed like that, and the
fix the owner asked for is the real numbers: **individuals per square
kilometre, per species, from the field literature.**

The point is NOT realism for its own sake. It is that real densities span SIX
ORDERS OF MAGNITUDE -- rock hyrax 5,752/km2, wolverine 0.008/km2 -- and that
spread does almost all of the thinning by itself, in a way no global multiplier
can. The owner has said these will be tweaked and some inflated to make the
world more interesting; this file is the baseline that tweak departs FROM, so
that "we doubled the deer" is a sentence with a meaning.

THE SOURCE, AND WHY THIS ONE. PanTHERIA (Jones et al. 2009, Ecology 90:2648),
column `21-1_PopulationDensity_n/km2`: 5,416 mammal species, 956 with a measured
density, compiled from the primary literature. The ESA archive states the data
are "free for scientific use" with no copyright restriction and asks only for
citation. It is the only compilation I know of that is open, species-level,
already in per-km2 units, and joinable to the hand-checked GBIF binomials in
`refs/species-latin.json`.

FOUR TIERS OF EVIDENCE, AND EVERY SPECIES SAYS WHICH IT IS. A number with no
provenance is the thing this project keeps getting hurt by, so:

  measured    PanTHERIA's own species-level figure, our binomial hand-checked.
  genus       median of congeners that do have one. A red fox standing in for
              a corsac fox is a real animal of about the right size and habits.
  allometric  predicted from body mass AND trophic level (below).
  none        no mammal source: reptiles and amphibians, which PanTHERIA does
              not cover at all. NOT estimated. Listed, and left for the owner.

THE ALLOMETRY, FITTED HERE RATHER THAN QUOTED. Damuth's law says density falls
with body mass to about the -0.75 power. Fitted over the 947 PanTHERIA species
carrying both figures, the slope comes out at **-0.741** -- the textbook value,
reproduced on this data.

But mass alone is not enough, and the residuals say so plainly: fitted against
body length the worst outliers are wolverine, wild dog, grey wolf, cheetah and
lynx, all far BELOW the line, and rock hyrax 592x ABOVE it. That is the energy
pyramid, not noise. Splitting the fit by PanTHERIA's own trophic level gives,
for a 10 kg animal:

    herbivore   16.91 /km2      log10(D) = 4.435 - 0.802*log10(mass_g)
    omnivore     5.20 /km2      log10(D) = 3.831 - 0.779*log10(mass_g)
    carnivore    0.89 /km2      log10(D) = 3.833 - 0.970*log10(mass_g)

A nineteen-fold spread at identical body size. Any model without that term puts
as many wolves on the hill as deer.

WHAT THIS IS HONEST ABOUT. The allometric tier has ~0.74 decades of residual
scatter, so an estimate is good to about a factor of seven. That is fine for
choosing between "one per square kilometre" and "a hundred", which is the
decision being made, and useless for anything finer. It is recorded per species
so nobody has to guess which numbers are which.

NOT COVERED HERE: birds, fish and cetaceans. PanTHERIA is mammals only, so fish
and birds need entirely different sources, and the cetaceans need scientific
names first -- `refs/species-latin.json` covers the 131 quadrupeds and nothing
else. `tools/refnames.py` explains at length why those names must be
hand-supplied and GBIF-verified rather than looked up in bulk: asked for "lion"
it returned a lion-tailed macaque, and for "nile-monitor" a banded mongoose,
each as a single confident result that then produced a valid, agreeing,
completely wrong reference.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

ROOT = Path(__file__).resolve().parents[1]
REFS = ROOT / "refs"
RAW = REFS / "raw" / "PanTHERIA_WR05.txt"
LATIN = REFS / "species-latin.json"
OUT = REFS / "density-reference.json"

PANTHERIA_URL = ("https://esapubs.org/archive/ecol/E090/184/"
                 "PanTHERIA_1-0_WR05_Aug2008.txt")

COL_NAME = "MSW05_Binomial"
COL_DENS = "21-1_PopulationDensity_n/km2"
COL_MASS = "5-1_AdultBodyMass_g"
COL_TROPHIC = "6-2_TrophicLevel"
# Carried because a density is not a sighting rate. It does NOT rescue the
# population arithmetic -- measured, the density leaders are mostly diurnal
# (rock hyrax, prairie dog, pika, grey squirrel) -- but a spawner that wants to
# know whether a species is out at noon should not have to guess.
COL_ACTIVITY = "1-1_ActivityCycle"

TROPHIC = {1: "herbivore", 2: "omnivore", 3: "carnivore"}
ACTIVITY = {1: "nocturnal", 2: "crepuscular/both", 3: "diurnal"}


# ---------------------------------------------------------------- fetch (net)

def fetch() -> int:
    """The ONLY command that touches a network, matching `reffit.py`'s rule.
    Everything a build needs is checked in afterwards."""
    RAW.parent.mkdir(parents=True, exist_ok=True)
    print(f"GET {PANTHERIA_URL}")
    with urllib.request.urlopen(PANTHERIA_URL, timeout=120) as r:
        blob = r.read()
    RAW.write_bytes(blob)
    print(f"  -> {RAW}  ({len(blob):,} bytes)")
    return 0


# ------------------------------------------------------------------- the data

def read_pantheria() -> dict:
    """binomial -> {density, mass_g, trophic}. -999 is PanTHERIA's null."""
    if not RAW.is_file():
        raise SystemExit(f"{RAW} missing -- run `densityref.py fetch` first")
    out = {}
    with RAW.open(encoding="utf-8") as f:
        hdr = f.readline().rstrip("\n").split("\t")
        iN, iD = hdr.index(COL_NAME), hdr.index(COL_DENS)
        iM, iT = hdr.index(COL_MASS), hdr.index(COL_TROPHIC)
        iA = hdr.index(COL_ACTIVITY)
        for line in f:
            c = line.rstrip("\n").split("\t")
            def num(i):
                v = float(c[i])
                return None if v == -999 else v
            out[c[iN]] = {"density": num(iD), "mass_g": num(iM),
                          "trophic": num(iT), "activity": num(iA)}
    return out


def fit_allometry(pan: dict) -> dict:
    """One Damuth line per trophic level, fitted on PanTHERIA itself.

    Returned per level: slope, intercept, n, and the residual scatter in
    decades -- which is the honest error bar on every allometric estimate and
    is written into the reference file next to each one."""
    fits = {}
    for lv, label in TROPHIC.items():
        X, Y = [], []
        for rec in pan.values():
            d, m, t = rec["density"], rec["mass_g"], rec["trophic"]
            if d and m and t == lv and d > 0 and m > 0:
                X.append(math.log10(m))
                Y.append(math.log10(d))
        if len(X) < 10:
            continue
        n = len(X)
        mx, my = sum(X) / n, sum(Y) / n
        b1 = (sum((x - mx) * (y - my) for x, y in zip(X, Y))
              / sum((x - mx) ** 2 for x in X))
        b0 = my - b1 * mx
        sd = statistics.pstdev([y - (b0 + b1 * x) for x, y in zip(X, Y)])
        fits[label] = {"intercept": round(b0, 4), "slope": round(b1, 4),
                       "n": n, "scatter_decades": round(sd, 3)}
    return fits


def predict(fits: dict, mass_g: float, trophic: float | None) -> tuple:
    label = TROPHIC.get(int(trophic)) if trophic else "omnivore"
    f = fits.get(label) or fits["omnivore"]
    d = 10 ** (f["intercept"] + f["slope"] * math.log10(mass_g))
    return d, label, f["scatter_decades"]


# ----------------------------------------------------------------- extraction

def extract() -> int:
    pan = read_pantheria()
    fits = fit_allometry(pan)
    latin = json.loads(LATIN.read_text(encoding="utf-8"))

    by_genus = defaultdict(list)
    for b, rec in pan.items():
        if rec["density"]:
            by_genus[b.split()[0]].append(rec["density"])

    species, counts = {}, defaultdict(int)
    for name in sorted(latin):
        rec = latin[name]
        binom = rec.get("latin")
        checked = bool(rec.get("checked_by_hand"))
        p = pan.get(binom or "")
        entry = {"latin": binom, "name_checked_by_hand": checked}
        if p:
            if p["mass_g"]:
                entry["mass_g"] = p["mass_g"]
            if p["trophic"]:
                entry["trophic"] = TROPHIC.get(int(p["trophic"]))
            if p["activity"]:
                entry["activity"] = ACTIVITY.get(int(p["activity"]))

        if p and p["density"]:
            entry.update(per_km2=round(p["density"], 4), tier="measured",
                         source="PanTHERIA 21-1_PopulationDensity_n/km2")
            if not checked:
                entry["warning"] = ("binomial is NOT hand-checked; the density "
                                    "may belong to a different animal")
        elif p and p["mass_g"]:
            d, lab, sd = predict(fits, p["mass_g"], p["trophic"])
            entry.update(per_km2=round(d, 4), tier="allometric",
                         source=f"Damuth fit, {lab}, from PanTHERIA body mass",
                         mass_g=p["mass_g"], trophic=lab,
                         factor_uncertainty=round(10 ** sd, 1))
        elif binom and by_genus.get(binom.split()[0]):
            sibs = by_genus[binom.split()[0]]
            entry.update(per_km2=round(statistics.median(sibs), 4),
                         tier="genus",
                         source=f"median of {len(sibs)} congener(s) in PanTHERIA")
        else:
            entry.update(per_km2=None, tier="none",
                         source="no mammal source; PanTHERIA does not cover "
                                "reptiles or amphibians")
        counts[entry["tier"]] += 1
        species[name] = entry

    doc = {
        "version": 1,
        "units": "individuals per square kilometre of suitable habitat",
        "source": {
            "name": "PanTHERIA",
            "citation": ("Jones K.E. et al. (2009) PanTHERIA: a species-level "
                         "database of life history, ecology, and geography of "
                         "extant and recently extinct mammals. Ecology 90:2648."),
            "url": PANTHERIA_URL,
            "terms": "ESA archive states free for scientific use; cite the paper.",
        },
        "allometry": fits,
        "tier_counts": dict(counts),
        "species": species,
    }
    OUT.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n",
                   encoding="utf-8")
    print(f"wrote {OUT}")
    for t, c in sorted(counts.items()):
        print(f"   {t:<12}{c:>5}")
    return 0


# --------------------------------------------------------------------- report

def report() -> int:
    if not OUT.is_file():
        raise SystemExit(f"{OUT} missing -- run `densityref.py extract`")
    doc = json.loads(OUT.read_text(encoding="utf-8"))
    sp = doc["species"]
    print(f"{'species':<26}{'per km2':>12}  tier")
    ranked = sorted((v for v in sp.items() if v[1]["per_km2"]),
                    key=lambda kv: -kv[1]["per_km2"])
    for n, v in ranked[:10]:
        print(f"{n:<26}{v['per_km2']:>12,.2f}  {v['tier']}")
    print("   ...")
    for n, v in ranked[-10:]:
        print(f"{n:<26}{v['per_km2']:>12,.4f}  {v['tier']}")

    vals = [v["per_km2"] for v in sp.values() if v["per_km2"]]
    print(f"\n{len(vals)} of {len(sp)} species have a number.")
    print(f"   spread: {min(vals):,.4f} to {max(vals):,.2f} per km2 "
          f"({math.log10(max(vals) / min(vals)):.1f} orders of magnitude)")
    print(f"   median: {statistics.median(vals):,.2f} per km2")
    for t, c in sorted(doc["tier_counts"].items()):
        print(f"   {t:<12}{c:>5}")
    unsourced = [n for n, v in sp.items() if not v["per_km2"]]
    if unsourced:
        print(f"\n   NO SOURCE ({len(unsourced)}), left for the owner rather "
              f"than invented:")
        print("     " + ", ".join(sorted(unsourced)))
    risky = [n for n, v in sp.items() if v.get("warning")]
    if risky:
        print(f"\n   density found under an UNVERIFIED binomial ({len(risky)}): "
              + ", ".join(sorted(risky)))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=("fetch", "extract", "report"))
    a = ap.parse_args()
    return {"fetch": fetch, "extract": extract, "report": report}[a.cmd]()


if __name__ == "__main__":
    sys.exit(main())
