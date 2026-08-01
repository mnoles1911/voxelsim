#!/usr/bin/env python3
"""Move an existing tile cache onto the split coarse/fine namespace.

WHY A TOOL AND NOT A ONE-LINER. The namespace split gave bake-derived artifacts
their own identity (``fine_provider_id`` = inference identity + bake digest) so
that retuning a bake stops discarding coarse tiles that cost ~22.5 s of GPU each.
That is a one-time re-key of BOTH namespaces, and it cannot be done by pattern-
matching directory names: the old ``provider_id`` folded the bake in, so the name
on disk is not derivable from today's code, and the CONFIG that produced it is
not derivable from the cache either -- no checkpoint sha and no conditioning
digest is stored anywhere under the cache root. The ids have to be computed from
the same config that generated the tiles, which is why this reuses pregen's own
argument parsing instead of inventing its own.

WHAT MAKES IT CHEAP, and it was checked rather than assumed: a .vxtl v2 fine tile
carries seed, tileX and tileY in its header and NOTHING about the provider
(voxel-core/src/tilestreaming.cpp::validateAndParseFineTile -- kIdentityMismatch
is "seed/x/y in the header != what was requested"). The provider id is a PATH
SEGMENT only. So this is a directory move, not a re-encode: the bytes are
untouched and a 201 MB fine tile does not have to be decoded and rewritten.

    <root>/<old_id>/<seed>/s1     ->  <root>/<provider_id>/<seed>/s1
    <root>/<old_id>/<seed>/s16    ->  <root>/<fine_provider_id>/<seed>/s16
    <root>/<old_id>/<seed>/flow*  ->  <root>/<fine_provider_id>/<seed>/flow*

Everything else under the old seed directory (bake_*.json manifests and the like)
stays put, because nothing reads it by path.

SAFE TO INTERRUPT AND SAFE TO RE-RUN. Each move is a single os.replace of a
directory on one filesystem, which is atomic; a run that dies half way leaves
some directories moved and the rest where they were, and re-running finishes the
job. Nothing is deleted, ever -- if a destination already exists the move is
refused rather than merged, because merging two generations of tiles is the exact
failure the split exists to prevent.

    # see what it would do, touching nothing
    python -m tools.migrate_cache_namespace --root D:/voxelsim/tile-cache \\
        --old-id terrain-diffusion-unlabeled-3e11cf157a836c70 --seed 20260719 \\
        --checkpoint-sha256 <sha> --conditioning-digest <digest>

    # ... and again with --apply once the plan reads right
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, type=Path,
                    help="cache root: the directory CONTAINING <provider_id>/")
    ap.add_argument("--old-id", required=True,
                    help="the existing provider_id directory name to migrate out of")
    ap.add_argument("--seed", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--apply", action="store_true",
                    help="actually move. Without it this only prints the plan.")
    # The identity inputs. Defaults come from DiffusionConfig, so a config that
    # only differs in the pinned digests needs just these two.
    ap.add_argument("--checkpoint-label")
    ap.add_argument("--checkpoint-sha256")
    ap.add_argument("--conditioning-digest")
    ap.add_argument("--conditioning-file", action="append", dest="conditioning_files")
    ap.add_argument("--terrain-diffusion-version")
    a = ap.parse_args()

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from terrain_service.providers.diffusion import DiffusionConfig

    over = {k: v for k, v in {
        "checkpoint_label": a.checkpoint_label,
        "checkpoint_sha256": a.checkpoint_sha256,
        "conditioning_digest": a.conditioning_digest,
        "conditioning_files": tuple(a.conditioning_files) if a.conditioning_files else None,
        "terrain_diffusion_version": a.terrain_diffusion_version,
    }.items() if v is not None}
    cfg = DiffusionConfig(**over)
    coarse, fine = cfg.provider_id(), cfg.fine_provider_id()

    seed_dir = f"{a.seed:016x}"
    src_root = a.root / a.old_id / seed_dir
    if not src_root.is_dir():
        print(f"error: {src_root} is not a directory", file=sys.stderr)
        return 1

    print(f"root        {a.root}")
    print(f"from        {a.old_id}/{seed_dir}")
    print(f"coarse ->   {coarse}/{seed_dir}")
    print(f"fine   ->   {fine}/{seed_dir}")
    if "UNPINNED" in coarse or "UNVERIFIEDDATA" in coarse:
        print("\nWARNING: the computed id carries UNPINNED / UNVERIFIEDDATA, which means this\n"
              "         run did not supply the checkpoint sha and conditioning digest the\n"
              "         tiles were generated with. Migrating under this id will put real\n"
              "         tiles in a namespace marked unverified, and a later pregen with the\n"
              "         REAL config will not find them. Supply --checkpoint-sha256 and\n"
              "         --conditioning-digest.", file=sys.stderr)
    print()

    moves: list[tuple[Path, Path]] = []
    for child in sorted(src_root.iterdir()):
        if not child.is_dir():
            continue
        if child.name == "s1":
            dst_id = coarse
        elif child.name == "s16" or child.name.startswith("flow"):
            dst_id = fine
        else:
            print(f"  skip  {child.name}  (not a tier or flow directory)")
            continue
        moves.append((child, a.root / dst_id / seed_dir / child.name))

    if not moves:
        print("nothing to migrate.")
        return 0

    problems = 0
    for src, dst in moves:
        n = sum(1 for _ in src.rglob("*") if _.is_file())
        if dst.exists():
            print(f"  REFUSE {src.name:12s} -> {dst}  (destination exists; merging two "
                  f"generations is what the split exists to prevent)")
            problems += 1
        else:
            print(f"  move   {src.name:12s} -> {dst}  ({n} file(s))")
    if problems:
        return 1
    if not a.apply:
        print("\ndry run. re-run with --apply to perform the moves.")
        return 0

    for src, dst in moves:
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.replace(src, dst)          # atomic within a filesystem
        except OSError:
            shutil.move(str(src), str(dst))  # across filesystems; slower, still safe
        print(f"  moved  {src.name} -> {dst}")
    print("\ndone. Point the client at the new names:")
    print(f"  DefaultTileDir=../../tile-cache/{coarse}/{seed_dir}/s1")
    print(f"  DefaultFineTileDir=../../tile-cache")
    print(f"  DefaultFineTileProviderId={fine}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
