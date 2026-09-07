"""ONE VERB: publish the library to the game (plan P2, keep-driven).

    python tools/publish.py [--check-only]

Owner ruling 2026-09-05: "Keep should be only human action in the Forge
phase of the process. Once it's 'kept' it goes to the library and then the
library contents are used in our game world." So this tool:

  1. RE-SYNCS verdicts from the library (library wins): every species with
     kept entries gets its curation block's seed list re-derived through
     `manifest.kept_seeds` -- the same derivation the server applies on
     every keep/unkeep, run once more here so a hand-deleted library folder
     cannot leave a stale seed list driving the exporters. Species without
     keeps are NEVER touched: the 828 grandfathered specs' bytes must not
     move (the no-unpublish rule), and a legacy explicit approval keeps its
     recorded list until the owner keeps seeds. A REJECTED species with
     kept entries still on disk is a conflict between two explicit human
     gestures; rejection wins and the conflict is printed, never silently
     resolved.
  2. Runs the exporters in dependency order: categories, banks, manifest
     (the manifest COUNTS baked seeds off the disk, so banks must land
     first), then the two checks: enginecheck and categories --check.
  3. Prints the merged CurationSummary -- who exported, who was held back
     BY NAME -- plus the remaining never-reviewed count (the burn-down,
     printed here until it reaches zero, per ruling 6).
  4. Says LOUDLY when species.vxm's bytes changed: that is worldgen input,
     and the v24 contract makes it a kWorldGenVersion bump with goldens
     re-blessed (voxelcore/core.h; docs/asset-placement-architecture.md §9).

Exit is non-zero if any exporter or check failed. `--check-only` runs the
re-sync report and the two checks without exporting anything.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import manifest, publishing, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
LIBRARY = ROOT / "library"
OUT = ROOT / "out" / "engine"


def resync_from_library(*, dry_run: bool = False) -> tuple[list[str], int]:
    """The library-wins pass. Returns (report lines, conflict count)."""
    lines: list[str] = []
    conflicts = 0
    for p in sorted(SPECS.glob("*.json")):
        name = p.stem
        kept = manifest.kept_seeds(LIBRARY, name)
        if not kept:
            continue    # no keeps: grandfather/legacy bytes never move
        raw = json.loads(p.read_text(encoding="utf-8"))
        prior = raw.get("curation") or {}
        if prior.get("status") == "rejected":
            conflicts += 1
            lines.append(
                f"  CONFLICT {name}: rejected, but {len(kept)} kept "
                f"entr{'y' if len(kept) == 1 else 'ies'} remain in the "
                f"library -- rejection wins (held back); delete the kept "
                f"entries or lift the rejection")
            continue
        block = {"status": "approved", "seeds": kept,
                 "notes": str(prior.get("notes", "") or "")}
        if prior == block:
            continue
        raw["curation"] = block
        if not dry_run:
            p.write_text(json.dumps(raw, indent=2, sort_keys=True) + "\n",
                         encoding="utf-8")
        was = f"seeds {prior.get('seeds')}" if prior else "never reviewed"
        lines.append(f"  {name}: bank re-derived from keeps -> {kept} (was {was})")
    return lines, conflicts


def run_tool(tool: str, *args: str) -> int:
    print(f"\n== {tool} {' '.join(args)}".rstrip())
    sys.stdout.flush()
    r = subprocess.run([sys.executable, str(ROOT / "tools" / tool), *args],
                       cwd=str(ROOT))
    if r.returncode:
        print(f"== {tool}: FAILED (exit {r.returncode})")
    return r.returncode


def _digest(path: Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check-only", action="store_true",
                    help="re-sync report + checks, no exporting")
    args = ap.parse_args()
    t0 = time.time()

    print("publish: library -> game (keep-driven)")
    lines, conflicts = resync_from_library(dry_run=args.check_only)
    print(f"re-sync from library: {len(lines)} line(s)"
          if lines else "re-sync from library: everything already in sync")
    for ln in lines:
        print(ln)

    rc = int(bool(conflicts or (args.check_only and lines)))
    vxm = OUT / "species.vxm"
    before = _digest(vxm)
    if not args.check_only:
        rc |= run_tool("export_categories.py")
        rc |= run_tool("export_banks.py")
        rc |= run_tool("export_manifest.py")
    try:
        count = publishing.publish_craft(SPECS, LIBRARY, OUT / "craft", check_only=args.check_only)
        print(f"craft: {count} approved saved models {'verified' if args.check_only else 'published'}")
    except (OSError, ValueError) as exc:
        print(f"craft: FAILED: {exc}")
        rc = 1
    rc |= run_tool("enginecheck.py")
    rc |= run_tool("export_categories.py", "--check")
    after = _digest(vxm)

    if before != after:
        print("\n" + "!" * 72)
        print("! species.vxm BYTES CHANGED. That table is worldgen input:")
        print("! bump kWorldGenVersion (voxelcore/core.h) and re-bless the")
        print("! goldens before this export ships -- see")
        print("! docs/asset-placement-architecture.md section 9.")
        print("!" * 72)

    # The merged curation summary + the burn-down, from the ONE gate.
    specs, seeds_baked, summary = manifest.curated_inputs(SPECS, OUT / "banks")
    print()
    for ln in summary.lines():
        print(ln)
    unreviewed = sum(
        1 for p in sorted(SPECS.glob("*.json"))
        if not sm.curation(sm.load(p)[0])["curated"])
    if unreviewed:
        print(f"burn-down: {unreviewed} species never reviewed -- open the "
              f"library's Never-reviewed queue and keep the good seeds")
    else:
        print("burn-down: every species has been reviewed")
    if conflicts:
        print(f"conflicts: {conflicts} rejected species still hold kept "
              f"library entries (see above)")

    print(f"\npublish: {'FAIL' if rc else 'PASS'} in {time.time() - t0:,.0f} s")
    return 1 if rc else 0


if __name__ == "__main__":
    sys.exit(main())
