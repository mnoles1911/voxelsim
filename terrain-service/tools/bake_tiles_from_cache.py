#!/usr/bin/env python3
"""Bake a NAMED LIST of fine tiles from an existing cache, with no provider.

    python tools/bake_tiles_from_cache.py \
        --seed 20260719 --cache-dir D:/voxelsim/tile-cache \
        --provider-id terrain-diffusion-unlabeled-80b9ca451a23eae4 \
        --tiles="-14,-4 -13,-5 -14,-5 -14,-6 -14,-7"

WHY THIS EXISTS
---------------
``pregen --mode bake`` is the sanctioned path and this does not replace it: it
CALLS it (``pregen._run_bake``), so the coarse pass, the hydrology pyramid, the
publish gate, ``_encode_fine``'s refusal to drop a product, and the per-tile log
line are all the shipping ones. What it replaces is only how the run is
addressed:

1. **A corridor is a line, not a square.** ``--radius`` bakes (2r+1)^2 tiles. The
   showcase corridor is 5 tiles in an L; the smallest square containing it is 16.
   At ~300 CPU-s per tile that is 55 minutes of the wrong work.

2. **A cache-only run needs no checkpoint.** ``--provider diffusion`` without a
   real checkpoint must run ``--dry-run``, and a dry run correctly refuses to
   write into the real namespace (it tags its ``provider_id`` with
   ``-dryrun-``). That tag is right and is not defeated here: instead this tool
   presents a provider that CANNOT GENERATE AT ALL. Every coarse tile must
   already be in the cache; ``generate`` raises rather than inventing ground.

So the identity guarantee is kept in the form that matters for a re-bake: the
coarse bytes are whatever the world already has, and the fine namespace is
derived from the bake fingerprint exactly as production derives it. The world
identity record is written from the pinned values the caller states, and
``record_world_identity`` still refuses if they disagree with what the world
already claims.

WHAT IT WILL NOT DO. Generate a coarse tile. If the ring is incomplete the bake
is refused by the same gate ``pregen`` uses, because a tile baked over a missing
neighbour has an apron of sea level and its interior is not the
infinite-domain answer.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import pregen  # noqa: E402
from terrain_service.cache import TileCache  # noqa: E402
from terrain_service.providers.diffusion import (  # noqa: E402
    DiffusionConfig,
    fine_id_for,
)


class ReadOnlyFineCache:
    """The real cache, with the FINE tier hidden and write-protected.

    ``pregen._run_bake`` skips any tile already in the fine cache and writes
    every tile it bakes. Both are right for production and both are wrong for a
    DIAGNOSTIC re-bake of tiles that already shipped: the skip means the run
    does nothing, and the write means a diagnostic run can mutate the world.

    This wrapper delegates everything (coarse tiles, the flow pyramid, the
    world identity record) to the real ``TileCache`` and overrides exactly two
    methods: ``get_fine`` always misses, so every requested tile is baked, and
    ``put_fine`` drops the bytes on the floor. The npz dump, which is what the
    diagnosis reads, happens BEFORE the put and is unaffected.

    Deliberately not a flag on ``pregen``: the shipped bake path has no
    business growing a "bake it again but do not keep it" mode, and a wrapper
    here cannot be reached by a production run.
    """

    def __init__(self, inner):
        self._inner = inner
        self.suppressed = 0

    def __getattr__(self, name):
        return getattr(self._inner, name)

    def get_fine(self, *a, **k):
        return None

    def put_fine(self, provider_id, seed, x, y, data):
        self.suppressed += 1


class CacheOnlyProvider:
    """Everything ``_run_bake`` reads off a provider, and nothing that generates.

    ``provider_id`` / ``fine_provider_id`` address the cache; ``config`` feeds
    the world identity record. ``generate`` exists only to fail loudly: a
    re-bake that quietly synthesised a missing coarse tile would produce a fine
    tile describing ground no other tile in the world agrees with.
    """

    def __init__(self, provider_id: str, config: DiffusionConfig):
        self.provider_id = provider_id
        self.fine_provider_id = fine_id_for(provider_id)
        self.config = config

    def generate(self, *a, **k):
        raise RuntimeError(
            "bake_tiles_from_cache cannot generate coarse tiles -- it has no "
            "checkpoint. Every tile in the requested ring must already be in "
            "the cache."
        )


def _parse_tiles(s: str) -> list[tuple[int, int]]:
    """"-14,-7 -14,-6" -> [(-14,-7), (-14,-6)].

    ONE string rather than ``nargs="+"``, and the reason is not style: every
    interesting tile in this world has a negative x, and argparse reads a bare
    ``-14,-7`` as an option flag. The single-argument form must be written
    ``--tiles="-14,-7 ..."``, which parses.
    """
    out = []
    for part in s.replace(";", " ").replace(",,", " ").split():
        try:
            x, y = part.split(",")
            out.append((int(x), int(y)))
        except ValueError:
            raise argparse.ArgumentTypeError(
                f"tile must be X,Y with integer coords, got {part!r}")
    if not out:
        raise argparse.ArgumentTypeError("no tiles given")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--provider-id", required=True,
                    help="the COARSE namespace to read from")
    ap.add_argument("--tiles", type=_parse_tiles, required=True,
                    help='space-separated X,Y list, e.g. --tiles="-14,-7 -14,-6"')
    ap.add_argument("--codec", default="raw", choices=("raw", "zstd"))
    ap.add_argument("--checkpoint-label", default="unlabeled")
    ap.add_argument("--checkpoint-sha256", default="UNPINNED")
    ap.add_argument("--conditioning-digest", default="UNVERIFIED")
    ap.add_argument("--terrain-diffusion-version", default="unknown")
    ap.add_argument("--rebuild-superblocks", action="store_true",
                    help="ignore cached flow superblocks (an A/B needs this; "
                         "see pregen's own note on why reuse is the default)")
    ap.add_argument("--allow-incomplete-superblock", action="store_true")
    ap.add_argument("--npz-dir", default=None,
                    help="dump each baked tier as <x>_<y>.npz here (elevation, "
                         "accumulation, flow, and -- the point -- the "
                         "UNQUANTISED discharge and water plane)")
    ap.add_argument("--diagnostic", action="store_true",
                    help="re-bake tiles that are already in the fine cache and "
                         "DO NOT write the result back. Only useful with "
                         "--npz-dir; the run's whole output is the dump.")
    args = ap.parse_args()
    if args.diagnostic and not args.npz_dir:
        ap.error("--diagnostic without --npz-dir would bake and keep nothing")

    config = DiffusionConfig(
        checkpoint_label=args.checkpoint_label,
        checkpoint_sha256=args.checkpoint_sha256,
        conditioning_digest=args.conditioning_digest,
        terrain_diffusion_version=args.terrain_diffusion_version,
    )
    provider = CacheOnlyProvider(args.provider_id, config)
    cache = TileCache(Path(args.cache_dir))
    if args.diagnostic:
        cache = ReadOnlyFineCache(cache)
        print("DIAGNOSTIC: fine tier hidden and write-protected; the only "
              f"output is {args.npz_dir}", file=sys.stderr)

    print(f"coarse namespace: {provider.provider_id}", file=sys.stderr)
    print(f"fine   namespace: {provider.fine_provider_id}", file=sys.stderr)
    print(f"tiles: {args.tiles}", file=sys.stderr)

    bake_args = argparse.Namespace(
        seed=args.seed,
        radius=0,
        center_x=0,
        center_y=0,
        scale=1,
        bake_targets=list(args.tiles),
        bake_superblock_tiles=None,
        bake_max_level=None,
        bake_model_parent=False,
        bake_rebuild_superblocks=args.rebuild_superblocks,
        bake_no_verify_superblocks=False,
        bake_npz_dir=args.npz_dir,
        # Never generate. The provider above cannot anyway; this keeps the
        # refusal at the pregen layer where the message is legible.
        bake_no_coarse_generate=True,
        allow_incomplete_superblock=args.allow_incomplete_superblock,
        codec=args.codec,
    )
    rc = pregen._run_bake(bake_args, provider, cache)
    if args.diagnostic:
        print(f"DIAGNOSTIC: {cache.suppressed} fine tile write(s) suppressed",
              file=sys.stderr)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
