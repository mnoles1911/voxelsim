"""Write the species placement manifest the engine loads.

    python tools/export_manifest.py [--out out/engine/species.vxm]

Reads every spec in specs/, applies the publish gate (`spec.curation`: a
species is exported only if its verdict is approved, and absent means
approved -- the grandfather clause is documented on the resolver), counts the
baked APPROVED bank seeds already exported under the manifest's own directory
(banks/<name>/<name>-NNNN.vxa), and writes one versioned binary table -- see
forge/manifest.py for the format and for why the layer table travels inside
it. The gate and the seed count both come from `manifest.curated_inputs`,
shared with tools/enginecheck.py so the exporter and its checker cannot read
the verdicts differently.

Prints the export report to stdout -- including who the gate held back, by
name -- and REFUSES (exit 1) only on structural failure (a spec that cannot
be represented at all). Underserved spacing and too-rare-to-express species
are reported, not fatal: they are authored facts the format cannot fully
serve, and hiding the export behind them would just stop the 800 species it
does serve. A draft or rejected species is different again: held back on
purpose, by a person, and reported so the absence is legible.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import manifest

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "out" / "engine" / "species.vxm"))
    args = ap.parse_args()

    out_path = Path(args.out)
    banks_dir = out_path.parent / "banks"

    specs, seeds_baked, curation = manifest.curated_inputs(SPECS, banks_dir)

    # --- ROCK SELF-CLASSIFICATION (owner directive, 2026-08-18) --------------
    #
    # Rocks are allocated by what they physically ARE, measured from the baked
    # grid, so a newly generated rock species falls into the system without
    # hand-tuning: bake it, export, and its size has already priced its rarity.
    #
    #   volume  = solid voxels x pitch^3 of seed 1's bank (the library
    #             individual; seeds vary a few percent, rarity doesn't care)
    #   spacing = max(authored, 6.0 x volume^0.35 m) -- the size-frequency law.
    #             Real rock populations follow N(>V) ~ V^-1.8..-2 (see
    #             docs/placement-research.md; same power-law family as the
    #             Damuth scaling the wildlife densities use), so nominal
    #             spacing must grow with volume or a 100 m^3 tor is as common
    #             as a cobble. Calibration: 1 m^3 boulder -> 6 m, 100 m^3
    #             tor -> ~30 m, cobbles keep their authored spacing (the
    #             derived floor sits below it). AUTHORED ALWAYS WINS UPWARD --
    #             a designer may make a species rarer than physics, never
    #             more common than its size allows.
    #   cluster = floor of 0.7 for slope-banded (talus-class) rocks whose
    #             authored cluster is lower: fragmentation debris clusters,
    #             and the authored library median (0.35) reads as scattered
    #             gravel where a talus fan should be.
    #
    # Mass (volume x ~2600 kg/m^3) is derived and REPORTED but not yet a
    # placement input; the talus/water transport channels (bake_ver 28) are
    # what will consume it. Extension point documented in
    # docs/rock-placement-system.md.
    try:
        sys.path.insert(0, str(ROOT))
        from forge import vxa as _vxa
        import glob as _glob
        derived_n = 0
        for name, body in specs:
            if body.get("kind") != "rock":
                continue
            bank = sorted(_glob.glob(str(banks_dir / name / f"{name}-*.vxa")))
            if not bank:
                continue
            g = _vxa.read(bank[0])
            solid = int((g.data != 0).sum())
            pitch = float(body.get("resolution_cm", 10.0)) / 100.0
            vol = solid * pitch ** 3
            pl = body.setdefault("placement", {})
            derived_sp = 6.0 * (vol ** 0.35)
            if derived_sp > float(pl.get("spacing_m") or 0.0):
                pl["spacing_m"] = round(derived_sp, 2)
                derived_n += 1
            if float(pl.get("slope_min_pct") or 0.0) > 0 and float(pl.get("cluster") or 0.0) < 0.7:
                pl["cluster"] = 0.7
        print(f"rock classifier: size-frequency spacing raised on {derived_n} species "
              f"(measured from banks; authored-wins-upward)")
    except Exception as e:  # loud, never silent -- a classifier that half-runs
        raise SystemExit(f"rock classifier failed: {e}")

    report = manifest.ExportReport()
    blob = manifest.encode(specs, seeds_baked=seeds_baked, report=report)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)

    for line in curation.lines():
        print(line)
    for line in report.lines():
        print(line)
    print(f"wrote {out_path} ({len(blob):,} bytes)")

    # Round-trip through the independent decoder so a struct drift in encode
    # is caught by the tool that wrote the file, not by the engine refusing it.
    back = manifest.decode(blob)
    if len(back["species"]) != report.species:
        print("ROUND TRIP FAILED: species count mismatch", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
