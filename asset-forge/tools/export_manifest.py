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
