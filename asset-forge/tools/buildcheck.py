"""Build every spec and fail if any of them would ship something broken.

This is the regression net for the library, and it is what CI runs. It exists
because everything else here reports and nothing enforces: `healthpass.py`
prints, `check` prints, `pipeline.build` measures `airborne_kept` and
`orphans_removed` into stats that nothing reads. Every floating-rock defect
this project has found was found by a person looking at a render, which is
another way of saying it was found by luck.

Three failure classes, all of which have already shipped here:

* **More than one piece.** One generation produces ONE asset: 1 tree, 1 rock,
  1 grass clump, 1 clump of reeds, 1 clump of flowers (owner, 2026-08-11).
  Sizes are separate assets, saved separately and placed together later by
  placement logic, so a secondary boulder or a scree ring arriving in the same
  grid is a second asset the library cannot address, cost or place. The rule
  is `forge.cli.pieces` == 1 at 26-connectivity.

* **A spec that no longer means what it says.** `spec.validate` never raises --
  a slider drag and the plain-language box both write specs and neither should
  be able to hard-fail a batch -- so an unknown parameter, a clamped range or a
  wrong type comes back as a *warning* on a spec that still builds fine. On
  disk that is a species quietly authored against a parameter table that moved.

* **A build that raises, or an asset `pipeline.health` rejects** -- an empty
  grid, nothing touching the ground, wood that came off the trunk.

Usage:

    python tools/buildcheck.py                    # every spec, seed 1
    python tools/buildcheck.py --seeds 1 2 3      # three individuals each
    python tools/buildcheck.py --kind rock        # rocks only
    python tools/buildcheck.py --skip-heavy       # leave out the four big heroes
    python tools/buildcheck.py --only-heavy       # just those four
    python tools/buildcheck.py --only-heavy --res 20   # ... on a coarser lattice
    python tools/buildcheck.py --no-allow         # ignore the known-failure list

The whole library at seed 1 and authored resolution is about half an hour, and
`hero-arch-colossal` alone is 12 minutes of it (40M voxels, a measured 24 GB
peak). That is why `--skip-heavy` / `--only-heavy` exist and why CI runs the
heavy four at 20 cm; see `forge.cli.HEAVY_SPECS`.

`--no-allow` is how a fix gets verified and how the true count is read at any
time: the allow-list in `forge/cli.py` keeps CI green on defects that are
already written down and being worked, and it hides nothing from anyone who
asks. Its length is the honest measure of how far this has to go.
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import Counter
from pathlib import Path

import _path  # noqa: F401  (sys.path bootstrap)
from forge import cli, pipeline, spec as sm

SPECS = Path(__file__).resolve().parents[1] / "specs"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seeds", type=int, nargs="+", default=[1],
                    help="which individuals to build of each species (default: 1)")
    ap.add_argument("--kind", action="append",
                    help="only these kinds (repeatable), e.g. --kind rock")
    ap.add_argument("--spec", action="append",
                    help="only these species by name (repeatable)")
    ap.add_argument("--no-allow", action="store_true",
                    help="report the known-failing specs as failures too")
    ap.add_argument("--res", type=float, default=None,
                    help="voxel size in cm, overriding the spec. A COARSER lattice is a "
                         "weaker check -- pieces that touch at one voxel can merge, and "
                         "thin necks can part -- but it is the only way the heaviest "
                         "heroes fit in a CI runner (hero-arch-colossal peaks at 24 GB "
                         "and 12 minutes at its authored 10 cm)")
    heavy = ap.add_mutually_exclusive_group()
    heavy.add_argument("--skip-heavy", action="store_true",
                       help=f"leave out the few specs that dominate the wall clock "
                            f"({', '.join(sorted(cli.HEAVY_SPECS))})")
    heavy.add_argument("--only-heavy", action="store_true",
                       help="run ONLY those specs")
    args = ap.parse_args()

    paths = sorted(SPECS.glob("*.json"))
    if args.skip_heavy:
        paths = [p for p in paths if p.stem not in cli.HEAVY_SPECS]
    if args.only_heavy:
        paths = [p for p in paths if p.stem in cli.HEAVY_SPECS]
        missing = cli.HEAVY_SPECS - {p.stem for p in paths}
        if missing:
            # A renamed or deleted hero must not turn its CI leg into a no-op
            # that passes. This project has shipped that failure more than once.
            print(f"forge.cli.HEAVY_SPECS names specs that do not exist: "
                  f"{', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    if args.spec:
        want = set(args.spec)
        paths = [p for p in paths if p.stem in want]
        missing = want - {p.stem for p in paths}
        if missing:
            print(f"no such spec: {', '.join(sorted(missing))}", file=sys.stderr)
            return 2
    if not paths:
        print("no specs found", file=sys.stderr)
        return 2

    failures: list[str] = []
    excused: list[str] = []
    by_kind: Counter[str] = Counter()      # specs failing the one-piece rule
    seen_kind: Counter[str] = Counter()
    broke: dict[str, bool] = {}            # spec name -> shipped more than one piece
    built = 0
    t0 = time.perf_counter()

    for fp in paths:
        s, rep = sm.load(fp)
        kind = sm.get(s, "kind")
        if args.kind and kind not in args.kind:
            continue
        seen_kind[kind] += 1
        allowed = None if args.no_allow else cli.KNOWN_MULTIPIECE.get(fp.stem)
        spec_failed = False

        for w in rep.warnings:
            failures.append(f"{fp.stem}: spec warning: {w}")
            print(f"{fp.stem:<26} {kind:<6}       FAIL  spec warning: {w}", flush=True)

        for seed in args.seeds:
            t = time.perf_counter()
            try:
                asset = pipeline.build(s, seed, resolution_cm=args.res)
            except Exception as e:                      # noqa: BLE001 -- report any
                failures.append(f"{fp.stem} seed {seed}: build raised {type(e).__name__}: {e}")
                print(f"{fp.stem:<26} {kind:<6} s{seed}   FAIL  build raised "
                      f"{type(e).__name__}: {e}", flush=True)
                spec_failed = True
                continue
            built += 1
            ms = (time.perf_counter() - t) * 1e3
            extra, _frac, why = cli.loose_summary(asset)
            problems = pipeline.health(asset)
            note = ""

            broke[fp.stem] = broke.get(fp.stem, False) or bool(extra)
            if extra:
                if allowed:
                    excused.append(f"{fp.stem} seed {seed}: {why}")
                    note = f"  known ({allowed}): {why}"
                else:
                    failures.append(f"{fp.stem} seed {seed}: {why}")
                    note = f"  NOT ONE PIECE: {why}"
                    spec_failed = True
            for p in problems:
                failures.append(f"{fp.stem} seed {seed}: {p}")
                note += f"  HEALTH: {p}"
                spec_failed = True

            state = "FAIL" if (extra and not allowed) or problems else (
                "known" if extra else "ok")
            # flushed: this job is minutes long and a CI log that only appears
            # at the end cannot be watched, or read at all if the runner is
            # killed for time.
            print(f"{fp.stem:<26} {kind:<6} s{seed} {asset.stats['voxels']:>9,} vox "
                  f"{asset.stats.get('grid_mb', 0):>7,.0f} MB {ms:>7.0f} ms  "
                  f"{state}{note}", flush=True)

        if spec_failed:
            by_kind[kind] += 1

    if not args.no_allow:
        for name in cli.stale_allowances(broke):
            failures.append(f"{name}: allow-listed in forge/cli.KNOWN_MULTIPIECE but it "
                            f"now builds as one piece -- delete the entry "
                            f"({cli.KNOWN_MULTIPIECE[name]})")

    elapsed = time.perf_counter() - t0
    print()
    print(f"{built} builds in {elapsed:.0f} s"
          + (f", at {args.res:g} cm rather than the authored size -- a weaker check, "
             f"see --res" if args.res else ""))
    for e in excused:
        print(f"  known-failing (allow-listed in forge/cli.py): {e}")
    if by_kind:
        print("  failing specs by kind: " + ", ".join(
            f"{k} {by_kind[k]}/{seen_kind[k]}" for k in sorted(seen_kind) if by_kind[k]))
    if failures:
        print(f"buildcheck: FAIL -- {len(failures)} problem"
              f"{'s' if len(failures) != 1 else ''}")
        for f in failures:
            print(f"  ! {f}")
        return 1
    print("buildcheck: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
