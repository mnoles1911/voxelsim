"""Write the species placement manifest the engine loads.

    python tools/export_manifest.py [--out out/engine/species.vxm]

Reads every spec in specs/, counts the baked bank seeds already exported under
the manifest's own directory (banks/<name>/<name>-NNNN.vxa), and writes one
versioned binary table -- see forge/manifest.py for the format and for why the
layer table travels inside it.

Prints the export report to stdout and REFUSES (exit 1) only on structural
failure (a spec that cannot be represented at all). Underserved spacing and
too-rare-to-express species are reported, not fatal: they are authored facts
the format cannot fully serve, and hiding the export behind them would just
stop the 800 species it does serve.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import manifest, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"

_SEED_FILE = re.compile(r"-(\d{4})\.vxa$")


def count_baked_seeds(banks_dir: Path) -> dict[str, int]:
    """How many bank seeds each species has on disk. COUNTED, not assumed:
    the manifest's seeds_baked is the number a loader can trust before it
    opens a single file, so it must be the number of files."""
    out: dict[str, int] = {}
    if not banks_dir.is_dir():
        return out
    for d in sorted(banks_dir.iterdir()):
        if not d.is_dir():
            continue
        n = sum(1 for f in d.iterdir() if _SEED_FILE.search(f.name))
        if n:
            out[d.name] = n
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "out" / "engine" / "species.vxm"))
    args = ap.parse_args()

    out_path = Path(args.out)
    banks_dir = out_path.parent / "banks"

    specs = []
    for p in sorted(SPECS.glob("*.json")):
        body, _report = sm.load(p)
        specs.append((p.stem, body))

    report = manifest.ExportReport()
    blob = manifest.encode(specs, seeds_baked=count_baked_seeds(banks_dir),
                           report=report)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)

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
