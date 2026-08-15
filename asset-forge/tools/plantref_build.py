"""Rebuild `tools/plantref.json` from the published sources.

`plantref.json` is a derived file — 78 rows of fitted allometry — and a derived
file nobody can rebuild is a number without a source, which is the failure mode
`docs/plant-proportion-research.md` is written against. So this is the
derivation, with the URLs in it.

    python tools/plantref_build.py fetch --dir <cache>    # ~3 GB, network
    python tools/plantref_build.py build --dir <cache>    # writes plantref.json

`fetch` is the only command that touches a network and nothing under `forge/`
imports any of this. The cache is deliberately NOT inside the repo: Tallo is
49 MB and the nine FIA state tables are about 3 GB unpacked.

Sources, licences read per file — see the research doc §2 for the verbatim
licence fields:

* **Tallo**, `https://zenodo.org/api/records/6637599`, **CC BY 4.0**
  (`metadata.license.id == "cc-by-4.0"`). Jucker, Fischer, Chave et al. 2022,
  doi:10.5281/zenodo.6637599; paper doi:10.1111/gcb.16302.
  Gives the crown-radius-on-height and stem-diameter-on-height power laws.
* **USDA FIA DataMart**, `https://apps.fs.usda.gov/fia/datamart/CSV/`,
  a work of the US Government, **public domain**. Gives uncompacted live crown
  ratio per species, and the independent cross-check on stem diameter.

Two column traps are handled here and neither is cosmetic:

* FIA `SPCD` ships as `"134.0"`, not `"134"`. `int()` on that raises, and with
  the raise in front of a row counter the whole scan silently reports zero rows
  — see research §3.3.
* FIA carries `CR` (**compacted**) and `UNCRCD` (**uncompacted**) crown ratio
  and they differ by a median factor of 1.35. Only `UNCRCD` is comparable to
  "where do the leaves start" on a voxel asset — research §3.5.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
import urllib.request
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from plantnames import TREES

TOOLS = Path(__file__).resolve().parent
ZENODO = "https://zenodo.org/api/records/6637599/files/{}/content"
FIA = "https://apps.fs.usda.gov/fia/datamart/CSV/{}"
STATES = ("AZ", "VT", "NH", "WV", "IN", "WA", "CA", "FL", "NM")
UA = {"User-Agent": "Mozilla/5.0 asset-forge-research"}

# A fit is used only above these; below them the slope is noise. `white-poplar`
# (r2 0.03 over a 5 m height range) is the species this gate exists for.
MIN_N, MIN_R2 = 40, 0.20


# --------------------------------------------------------------------- fetch

def _get(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 0:
        print(f"  have {dest.name}")
        return
    print(f"  get  {dest.name} <- {url}")
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=900) as r, open(dest, "wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)


def fetch(cache: Path) -> None:
    import zipfile
    for name in ("Tallo.csv", "Tallo_metadata.csv", "Tallo_references.csv"):
        _get(ZENODO.format(name), cache / "tallo" / name)
    _get(FIA.format("REF_SPECIES.csv"), cache / "fia" / "REF_SPECIES.csv")
    for st in STATES:
        z = cache / "fia" / f"{st}_TREE.zip"
        if not (cache / "fia" / st).exists():
            _get(FIA.format(f"{st}_TREE.zip"), z)
            with zipfile.ZipFile(z) as zf:
                zf.extractall(cache / "fia" / st)
            z.unlink()


# ----------------------------------------------------------------- allometry

def ols_loglog(pairs) -> dict | None:
    """Fit y = a*x**b by OLS in log-log, with the Baskerville correction."""
    pts = [(math.log(x), math.log(y)) for x, y in pairs if x > 0 and y > 0]
    n = len(pts)
    if n < 8:
        return None
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    if sxx <= 0:
        return None
    b = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx
    a = my - b * mx
    resid = [p[1] - (a + b * p[0]) for p in pts]
    ss_res = sum(r * r for r in resid)
    ss_tot = sum((p[1] - my) ** 2 for p in pts)
    sd = statistics.pstdev(resid)
    return dict(a=math.exp(a) * math.exp(sd * sd / 2), b=b, n=n,
                r2=1 - ss_res / ss_tot if ss_tot > 0 else float("nan"))


def pct(v, q):
    if not v:
        return None
    v = sorted(v)
    i = (len(v) - 1) * q
    lo, hi = int(math.floor(i)), int(math.ceil(i))
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


# ------------------------------------------------------------------- sources

def read_tallo(cache: Path):
    """-> {species: [(dbh_cm, h_m, cr_m, h_out, cr_out)]}, and the same by genus."""
    sp, gen = {}, {}
    with open(cache / "tallo" / "Tallo.csv", encoding="utf-8-sig", newline="") as f:
        for row in csv.DictReader(f):
            def g(k):
                v = row[k]
                return None if v in ("NA", "") else float(v)
            rec = (g("stem_diameter_cm"), g("height_m"), g("crown_radius_m"),
                   row["height_outlier"], row["crown_radius_outlier"])
            sp.setdefault(row["species"], []).append(rec)
            gen.setdefault(row["genus"], []).append(rec)
    return sp, gen


def read_fia_crown_ratio(cache: Path):
    """-> {binomial: (median uncompacted crown ratio %, n)}"""
    ref = {}
    with open(cache / "fia" / "REF_SPECIES.csv", encoding="utf-8-sig",
              newline="") as f:
        for row in csv.DictReader(f):
            try:
                ref[int(row["SPCD"])] = (row["GENUS"] + " " + row["SPECIES"]).strip()
            except (ValueError, KeyError):
                pass
    acc = {}
    for st in STATES:
        for path in (cache / "fia" / st).glob("*_TREE.csv"):
            with open(path, encoding="utf-8-sig", newline="") as f:
                for row in csv.DictReader(f):
                    if row.get("STATUSCD") != "1":
                        continue
                    try:
                        # "134.0", not "134" -- see the module docstring.
                        b = ref.get(int(float(row["SPCD"])))
                    except (ValueError, KeyError):
                        continue
                    v = row.get("UNCRCD", "")
                    if not b or v in ("", "NA"):
                        continue
                    try:
                        fv = float(v)
                    except ValueError:
                        continue
                    if 0 < fv <= 100:
                        acc.setdefault(b, []).append(fv)
    return {b: (statistics.median(v), len(v))
            for b, v in acc.items() if len(v) >= 100}


# --------------------------------------------------------------------- build

def build(cache: Path) -> None:
    sp, gen = read_tallo(cache)
    cr = read_fia_crown_ratio(cache)
    out = {}
    for spec, (binom, note) in TREES.items():
        r = dict(binomial=binom, note=note)
        if binom:
            level, recs = "species", sp.get(binom, [])
            g = binom.split()[0]
            if len(recs) < 20 and len(gen.get(g, [])) >= 20:
                level, recs = "genus:" + g, gen[g]
            if recs:
                H = [x[1] for x in recs if x[1] and x[3] == "N"]
                r.update(tallo_level=level, tallo_n_h=len(H),
                         h_p90=pct(H, .90), h_p95=pct(H, .95))
                for key, pairs in (
                        ("crown_radius_vs_h",
                         [(x[1], x[2]) for x in recs
                          if x[1] and x[2] and x[3] == "N" and x[4] == "N"]),
                        ("dbh_vs_h",
                         [(x[1], x[0]) for x in recs
                          if x[1] and x[0] and x[3] == "N"])):
                    f = ols_loglog(pairs)
                    if f and f["n"] >= MIN_N and f["r2"] >= MIN_R2:
                        r[key] = dict(a=round(f["a"], 5), b=round(f["b"], 5),
                                      n=f["n"], r2=round(f["r2"], 3))
            if binom in cr:
                r["fia_uncr"] = cr[binom][0] / 100.0
                r["fia_uncr_n"] = cr[binom][1]
        out[spec] = r
    (TOOLS / "plantref.json").write_text(json.dumps(out, indent=1), encoding="utf-8")
    print(f"wrote {TOOLS / 'plantref.json'}: {len(out)} rows, "
          f"{sum('dbh_vs_h' in v for v in out.values())} with a stem-diameter law, "
          f"{sum('crown_radius_vs_h' in v for v in out.values())} with a crown law, "
          f"{sum('fia_uncr' in v for v in out.values())} with a species-level crown ratio")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["fetch", "build"])
    ap.add_argument("--dir", required=True,
                    help="cache directory for the downloaded sources (~3 GB)")
    a = ap.parse_args()
    cache = Path(a.dir)
    (fetch if a.cmd == "fetch" else build)(cache)


if __name__ == "__main__":
    main()
