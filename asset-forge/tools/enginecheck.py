"""Is what the engine loads still what the specs say? Answer with a number.

    python tools/enginecheck.py                  # exit 1 if anything is stale
    python tools/enginecheck.py --out out/engine # a different export dir
    python tools/enginecheck.py --quiet          # just the verdict

WHY THIS EXISTS. `voxelcore/core.h`'s v24 contract says the manifest bytes, the
bank bytes and the layer table are worldgen input: change any of them and it is
a version bump with goldens re-blessed. That is a rule, and the export is a
DERIVED COPY of 828 specs that nothing checked against its source. Two ways it
detached, both real, both silent:

* `export_banks.py` skipped a (species, seed) whenever the FILE EXISTED. A spec
  could move any distance -- a new trunk taper, a re-fitted diameter -- and the
  bank kept serving the old tree forever, because the old tree was still on
  disk. Nothing raised. The world composed a stale tree and rendered it
  perfectly.
* `species.vxm` is written once per export and read every run. A weight, a
  spacing, a biome assignment edited afterwards lives in the spec and not in
  the file the engine actually reads.

Both are the project's signature shape -- see the memory note "derived, not
verified, detaches", and the five bugs behind it. The repair is the same one
every time: stop deriving the fact twice and start CHECKING it. This tool is
the check, and `export_banks.py` now records `spec_hash` per species so it can
re-bake on a changed spec instead of skipping on a present file.

WHY `spec_hash` AND NOT A FILE TIMESTAMP. `spec.spec_hash` is library identity:
it covers the authored body and deliberately excludes `notes`, so rewording a
comment does not invalidate 688 baked files, and changing a single trunk
parameter does. A timestamp would do the opposite of both.

WHAT IT CANNOT SEE. A bank baked by a DIFFERENT VERSION OF THE GENERATOR from
the same unchanged spec. The hash covers the spec, not `forge/`, so a change to
`rasterize.py` that moves every tree leaves every hash equal and every bank
stale. That is a real hole and it is named here rather than papered over: the
honest fix is to re-bake on any `forge/` change, which is what a release does.
`--deep` rebuilds each species and compares the bytes it WOULD write, which
closes the hole at the cost of a full bake's runtime.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import manifest, parts as partslib, pipeline, spec as sm, vxa

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
BAKED = "BAKED.json"

_SEED_FILE = re.compile(r"-(\d{4})\.vxa$")


def bank_record(banks: Path) -> dict:
    p = banks / BAKED
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8")).get("species", {})
    except Exception:  # noqa: BLE001 -- an unreadable record is "no record"
        return {}


def check_banks(banks: Path, deep: bool = False
                ) -> tuple[list[str], int, int, list[str]]:
    """(problems, checked, unrecorded, orphans).

    A species with a bank directory but no recorded hash is UNRECORDED, not
    clean: nothing knows which spec its files came from. After the first
    `--force` pass that is not "predates the record" any more -- it means the
    exporter no longer bakes that species, so the files are left over from a
    library it has since dropped. Reported, never deleted."""
    rec = bank_record(banks)
    problems: list[str] = []
    orphans: list[str] = []
    checked = unrecorded = 0
    if not banks.is_dir():
        return ([f"no bank directory at {banks}"], 0, 0, [])

    for d in sorted(banks.iterdir()):
        if not d.is_dir():
            continue
        name = d.name
        spec_path = SPECS / f"{name}.json"
        files = sorted(f for f in d.iterdir() if _SEED_FILE.search(f.name))
        if not spec_path.is_file():
            problems.append(f"{name}: {len(files)} bank files, NO SPEC -- the "
                            f"species was renamed or deleted and the bank "
                            f"outlived it")
            continue
        body, _ = sm.load(spec_path)
        now = sm.spec_hash(body)
        was = (rec.get(name) or {}).get("spec_hash")
        checked += 1
        if was is None:
            # A bank directory with no record after a --force pass is not
            # "predates the record" -- the exporter would have stamped it. It is
            # a bank the exporter no longer bakes: a species that folds to zero
            # per-mille, is refused by its layer, or was renamed. The files stay
            # (deleting a baked hero is not this tool's call) but they are dead
            # bytes of unknown vintage, and saying so is the point.
            unrecorded += 1
            orphans.append(f"{name}: {len(files)} bank file(s), no recorded "
                           f"spec_hash -- the exporter does not bake this "
                           f"species, so these are stale bytes of unknown age")
            continue
        if was != now:
            problems.append(f"{name}: spec_hash {was[:12]} -> {now[:12]} -- "
                            f"{len(files)} bank files serve the OLD species")
            continue
        if deep:
            for f in files:
                m = _SEED_FILE.search(f.name)
                seed = int(m.group(1))
                a = pipeline.build(body, seed)
                want = vxa.encode(a.grid, a.parts, partslib.joints(a.parts))
                if want != f.read_bytes():
                    problems.append(f"{name} seed {seed}: rebuilt bytes differ "
                                    f"from the baked file though the spec_hash "
                                    f"matches -- the GENERATOR moved")
    return (problems, checked, unrecorded, orphans)


def check_manifest(out: Path) -> list[str]:
    """The manifest on disk against the one today's specs would produce.

    Re-derived through `manifest.curated_inputs`, the SAME function
    export_manifest writes from -- the publish gate and the approved-seed
    count are one reading, applied twice, so this check cannot certify a
    manifest the exporter would not write."""
    p = out / "species.vxm"
    if not p.is_file():
        return [f"no manifest at {p}"]

    specs, seeds, _curation = manifest.curated_inputs(SPECS, out / "banks")
    # The exporter mutates rock placement from the baked grids before
    # encoding (manifest.apply_rock_classification); comparing without the
    # same mutation certifies nothing and fails everything -- see the
    # function's own docstring for the day that proved it.
    manifest.apply_rock_classification(specs, out / "banks")
    fresh = manifest.encode(specs, seeds, manifest.ExportReport())
    have = p.read_bytes()
    if fresh == have:
        return []
    return [f"species.vxm is {len(have):,} B on disk and would be "
            f"{len(fresh):,} B from today's specs -- the engine is reading a "
            f"table the library no longer describes"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "out" / "engine"))
    ap.add_argument("--deep", action="store_true",
                    help="rebuild every bank and compare bytes (slow)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out = Path(args.out)
    problems = check_manifest(out)
    bank_problems, checked, unrecorded, orphans = check_banks(out / "banks", args.deep)
    problems += bank_problems

    if not args.quiet:
        print(f"manifest: {out / 'species.vxm'}")
        print(f"banks:    {checked} species checked, {unrecorded} with no "
              f"recorded spec_hash")
        for w in problems:
            print(f"  ! {w}")

    if problems:
        print(f"enginecheck: FAIL -- {len(problems)} stale, re-export with "
              f"tools/export_banks.py and tools/export_manifest.py "
              f"(a deliberate worldgen change: bump vxc::kWorldGenVersion "
              f"and re-bless goldens)")
        return 1
    if unrecorded:
        for w in orphans:
            print(f"  ~ {w}")
        print(f"enginecheck: PASS with {unrecorded} unrecorded -- reported "
              f"above, not fatal: they are not in the manifest, so nothing "
              f"loads them")
        return 0
    print(f"enginecheck: PASS -- {checked} species, manifest matches the specs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
