#!/usr/bin/env python3
"""What should this run pass as ``-VoxelFineTileProviderId``? READ, not derived.

    python tools/resolve_fine_namespace.py \
        --cache-dir D:/voxelsim/tile-cache --seed 20260719 \
        --tiles="-3,-4 -4,-4"
    # -> terrain-diffusion-unlabeled-80b9ca451a23eae4-b19d281fd

WHY THIS EXISTS
---------------
The fine tier has TWO sides that name a namespace and NOTHING that reconciles
them:

  bake   COMPUTES it. ``fine_id_for(provider_id())`` -- the ``-bXXXXXXXX``
         suffix is a sha256 over ``bake_identity_payload()`` +
         ``product_identity_payload()``, so it MOVES whenever a bake constant
         moves. ``bake_ver`` 27 -> 28 moved it -bdcab4bed -> -b19d281fd.
  engine COMPUTES NOTHING. ``FVoxelFineTileStreamer`` takes a literal string
         from ``-VoxelFineTileProviderId``, or failing that from
         ``DefaultFineTileProviderId`` in ue-project/Config/DefaultGame.ini.

Between them sat a human copying nine hex characters into an ini after every
bake change. On 2026-08-18 that failed three times in one night: the ini still
pinned the ``bake_ver`` 27 namespace, the run overrode ``-VoxelFineTileDir``
without overriding the id (they resolve INDEPENDENTLY), and the fatal residency
gate reported a path that had never existed on any disk.

This tool is the reconciliation. It reads the record the bake already writes --
``world-identity.json``, whose ``namespace_id`` is the directory it was written
into -- and prints the id. It derives nothing and it hashes nothing, so it
cannot become a second answer to the question content addressing exists to have
one answer to.

USE IT IN HARNESSES rather than pinning by hand::

    $Id = python terrain-service/tools/resolve_fine_namespace.py `
              --cache-dir $CacheDir --seed $Seed --tiles="$Tiles"
    ... -VoxelFineTileProviderId=$Id

EXIT CODES, so a harness can branch:
  0  exactly one namespace qualifies -- its id is on stdout, alone, no trailing
     newline, ready to interpolate into a flag.
  2  NONE qualifies. Either nothing is baked for these tiles or the cache root
     is wrong. A harness must NOT fall through to a default here: the fine-tier
     residency gate is fatal in unattended runs, so guessing costs the whole run.
  3  SEVERAL qualify and ``--newest`` was not given. Ambiguity is reported, not
     resolved silently: two namespaces holding the same tile hold two different
     bakes of it, and picking one by luck is how a measurement ends up
     describing a world nobody meant to look at. Pass --newest to take the most
     recently created, which is what a capture after a fresh bake wants.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service.world_manifest import find_fine_namespaces  # noqa: E402


def _parse_tiles(s: str) -> list[tuple[int, int]]:
    """``"-3,-4 -4,-4"`` -> ``[(-3,-4), (-4,-4)]``.

    One string rather than ``nargs="+"`` for the same reason
    ``bake_tiles_from_cache`` uses one: every interesting tile in this world has
    a negative x, and argparse reads a bare ``-3,-4`` as an option flag.
    """
    out = []
    for part in s.replace(";", " ").split():
        try:
            x, y = part.split(",")
            out.append((int(x), int(y)))
        except ValueError:
            raise argparse.ArgumentTypeError(
                f"tile must be X,Y with integer coords, got {part!r}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cache-dir", required=True,
                    help="the cache ROOT -- the directory holding <namespace>/ "
                         "dirs, i.e. the same value as -VoxelFineTileDir")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--tiles", type=_parse_tiles, default=[],
                    help='COARSE tile coords the run needs resident, e.g. '
                         '--tiles="-3,-4 -4,-4". Namespaces missing any of them '
                         'are rejected -- which is the difference between a '
                         'stale pin and a coverage gap, and the harness needs '
                         'to know which one it has.')
    ap.add_argument("--provider-id", default=None,
                    help="restrict to fine namespaces of this COARSE world")
    ap.add_argument("--newest", action="store_true",
                    help="on ambiguity take the most recently created instead "
                         "of failing")
    ap.add_argument("--list", action="store_true",
                    help="print every candidate with its date and coverage to "
                         "stderr, for a human deciding by hand")
    args = ap.parse_args()

    found = find_fine_namespaces(args.cache_dir, args.seed, tiles=args.tiles,
                                 coarse_provider_id=args.provider_id)

    if args.list:
        for f in found:
            cover = (f"{len(f['tiles_present'])}/"
                     f"{len(f['tiles_present']) + len(f['tiles_missing'])} tiles"
                     if args.tiles else "no tile filter")
            print(f"  {f['namespace_id']}  created={f['created_utc'] or '?'}  "
                  f"{cover}  manifest={'yes' if f['has_manifest'] else 'NO'}",
                  file=sys.stderr)

    # A namespace qualifies when it holds EVERY requested tile. With no --tiles
    # every namespace carrying anything for this seed qualifies, which is why
    # --tiles is what a harness should pass: it is the only filter that reflects
    # what the run will actually touch.
    ok = [f for f in found if not f["tiles_missing"]]

    if not ok:
        what = f"tiles {args.tiles}" if args.tiles else "anything"
        print(f"error: no fine namespace under {args.cache_dir} holds {what} "
              f"for seed {args.seed:016x} ({len(found)} namespace(s) present "
              f"for this seed). Do NOT fall back to a default id: the "
              f"fine-tier residency gate is fatal in unattended runs, so an "
              f"unbaked tile kills the run at the first query rather than "
              f"degrading. Bake the tiles, or check --cache-dir.",
              file=sys.stderr)
        for f in found:
            print(f"  candidate {f['namespace_id']} is missing "
                  f"{f['tiles_missing']}", file=sys.stderr)
        return 2

    if len(ok) > 1 and not args.newest:
        print(f"error: {len(ok)} fine namespaces under {args.cache_dir} hold "
              f"these tiles for seed {args.seed:016x}. They are different BAKES "
              f"of the same ground; picking one by luck is how a measurement "
              f"ends up describing a world nobody meant to look at. Pass "
              f"--newest, or name one explicitly:", file=sys.stderr)
        for f in ok:
            print(f"  {f['namespace_id']}  created={f['created_utc'] or '?'}",
                  file=sys.stderr)
        return 3

    chosen = ok[0]
    # A record that names a namespace other than the directory holding it has
    # been MOVED. The path is what the client opens, so the path wins -- but
    # silently preferring it would hide exactly the kind of hand-copied cache
    # surgery that produces two planets in one namespace.
    recorded = chosen.get("recorded_namespace_id")
    if recorded and recorded != chosen["namespace_id"]:
        print(f"warning: {chosen['path']}/world-identity.json records "
              f"namespace_id={recorded!r} but sits in directory "
              f"{chosen['namespace_id']!r}. This namespace has been moved or "
              f"copied by hand. Using the DIRECTORY, since that is what the "
              f"client opens -- but the record no longer describes where it is.",
              file=sys.stderr)
    if not chosen["has_manifest"]:
        print(f"warning: {chosen['namespace_id']} has no world-identity.json, "
              f"so nothing records what made it. Usable, but its provenance is "
              f"unverified (see terrain_service/world_manifest.py).",
              file=sys.stderr)

    sys.stdout.write(chosen["namespace_id"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
