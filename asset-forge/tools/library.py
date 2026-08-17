"""What the published library IS: every species, its verdict, its seeds.

    python tools/library.py                 # the summary a human reads
    python tools/library.py --full          # one row per species
    python tools/library.py --json          # the same facts for a machine

This is the authoritative reading of the publish gate. The two exporters
apply the gate (via `spec.curation`, the one resolver); this tool is where a
person finds out what the gate WILL DO before running them -- who exports,
who is held back, which seeds each species publishes, and how much of
"approved" is the grandfather clause rather than a human having looked.
That last split is the number that matters during a curation drive:
`approved 828 (828 grandfathered)` means the gate exists and nobody has used
it yet, which is a different fact from a curated library, and the report
refuses to blur them.

The digest on the first line is a hash over every (name, spec_hash, status,
seeds) row, so two checkouts can compare libraries with one line of output
instead of a diff of 828 files. It moves when any species' identity OR
verdict moves, and not when a note is reworded.

WATER-GATED FLAGS are read off the same rows the manifest exports, not
re-derived: placement.water_max_m > 0 (only places within reach of water),
detail.water != any (salinity admission), placement.elev_min_m < 0 (lives
below sea level). Kind-agnostic on purpose -- the gate and this report treat
a rejected trout exactly like a rejected oak.

Exit 1 only if a spec fails to load; verdicts are never errors.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import Counter
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


def water_flags(body: dict) -> list[str]:
    """How this species' placement is gated by water, as short phrases."""
    flags: list[str] = []
    near = float(sm.get(body, "placement.water_max_m") or 0.0)
    if near > 0:
        flags.append(f"within {near:g} m of water")
    water = sm.get(body, "detail.water")
    if water and water != "any":
        flags.append(f"{water} water")
    below = float(sm.get(body, "placement.elev_min_m") or 0.0)
    if below < 0:
        flags.append(f"lives to {-below:g} m depth")
    return flags


def collect() -> list[dict]:
    """One row per spec: the facts the report and --json both print."""
    rows: list[dict] = []
    for p in sorted(SPECS.glob("*.json")):
        body, report = sm.load(p)
        cur = sm.curation(body)
        rows.append({
            "name": p.stem,
            "kind": sm.get(body, "kind"),
            "status": cur["status"],
            "curated": cur["curated"],
            "seeds": cur["seeds"],
            "spec_hash": sm.spec_hash(body),
            "water": water_flags(body),
            # A malformed curation block resolves to draft and validate names
            # the damage; surface those lines here, where a curator is looking.
            "warnings": [w for w in report.warnings if w.startswith("curation")],
        })
    return rows


def digest(rows: list[dict]) -> str:
    h = hashlib.blake2b(digest_size=8)
    for r in rows:
        h.update(f"{r['name']}|{r['spec_hash']}|{r['status']}|{r['seeds']}\n"
                 .encode())
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true",
                    help="one row per species instead of the summary")
    ap.add_argument("--json", action="store_true",
                    help="machine-readable output (implies every row)")
    args = ap.parse_args()

    try:
        rows = collect()
    except (OSError, json.JSONDecodeError) as e:
        print(f"library: FAILED to read a spec: {e}", file=sys.stderr)
        return 1

    by_status = Counter(r["status"] for r in rows)
    grandfathered = sum(1 for r in rows if not r["curated"])
    off_default = [r for r in rows if r["status"] == "approved"
                   and r["seeds"] != list(sm.CURATION_SEEDS)]

    if args.json:
        print(json.dumps({
            "digest": digest(rows),
            "counts": {s: by_status.get(s, 0) for s in sm.CURATION_STATUSES},
            "grandfathered": grandfathered,
            "species": rows,
        }, indent=1, sort_keys=True))
        return 0

    print(f"library: {len(rows)} species, digest {digest(rows)}")
    print(f"status:  {by_status.get('approved', 0)} approved "
          f"({grandfathered} grandfathered -- never curated by a human), "
          f"{by_status.get('draft', 0)} draft, "
          f"{by_status.get('rejected', 0)} rejected")

    kinds = sorted({r["kind"] for r in rows})
    parts = []
    for k in kinds:
        ours = [r for r in rows if r["kind"] == k]
        held = sum(1 for r in ours if r["status"] != "approved")
        parts.append(f"{k} {len(ours)}" + (f" ({held} held)" if held else ""))
    print(f"kinds:   {', '.join(parts)}")

    default = list(sm.CURATION_SEEDS)
    print(f"seeds:   {by_status.get('approved', 0) - len(off_default)} approved "
          f"species publish the default {default}; "
          f"{len(off_default)} curated seed lists differ")
    for r in off_default:
        print(f"    {r['name']}: {r['seeds']}")

    gated = [r for r in rows if r["water"]]
    print(f"water:   {len(gated)} species water-gated")

    held_rows = [r for r in rows if r["status"] != "approved"]
    for r in held_rows:
        print(f"  held: {r['name']} ({r['kind']}) -- {r['status']}")
    for r in rows:
        for w in r["warnings"]:
            print(f"  ! {r['name']}: {w}")

    if args.full:
        print()
        for r in rows:
            mark = "" if r["curated"] else " *"
            water = f"  [{'; '.join(r['water'])}]" if r["water"] else ""
            print(f"  {r['name']:<40} {r['kind']:<10} {r['status']:<9}"
                  f"{mark:<3} seeds {r['seeds']}  {r['spec_hash']}{water}")
        print("\n  * grandfathered: approved because no human has curated it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
