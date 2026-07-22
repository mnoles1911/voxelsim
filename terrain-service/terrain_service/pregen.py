"""Pre-generate tiles for a given seed+radius offline (plan §3.4: "Pre-generate launch radius offline").

Run as: python -m terrain_service.pregen --seed <seed> --radius <r> [--center-x 0] [--center-y 0]
        [--scale 1] [--cache-dir ./tile-cache] [--provider synthetic]

Generates the (2*radius+1)^2 square of tiles around the center point. For each
tile, skips if already cached (by provider_id), else generates+encodes+caches.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from . import tile_codec
from .cache import TileCache
from .app import _make_provider


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pre-generate tiles for a given seed and launch radius"
    )
    parser.add_argument(
        "--seed", type=int, required=True, help="Tile generation seed (u64)"
    )
    parser.add_argument(
        "--radius",
        type=int,
        required=True,
        help="Tile radius: generates (2*radius+1)^2 tiles around center",
    )
    parser.add_argument(
        "--center-x", type=int, default=0, help="Center tile x coordinate (default 0)"
    )
    parser.add_argument(
        "--center-y", type=int, default=0, help="Center tile y coordinate (default 0)"
    )
    parser.add_argument(
        "--scale",
        type=int,
        default=1,
        choices=[1, 8],
        help="Tile scale: 1 (30m/px) or 8 (11.25m/px) (default 1)",
    )
    parser.add_argument(
        "--cache-dir",
        type=str,
        default="./tile-cache",
        help="Cache directory (default ./tile-cache)",
    )
    parser.add_argument(
        "--provider",
        type=str,
        default="synthetic",
        choices=["synthetic", "diffusion"],
        help="Tile provider (default synthetic)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Diffusion provider only: run in dry-run mode (synthetic-fallback "
            "rasters through the real config/adapter/validate path, no GPU "
            "needed). Lets you exercise/pregen the pipeline before a real "
            "checkpoint is wired up. See docs/diffusion-bringup.md."
        ),
    )
    parser.add_argument(
        "--checkpoint-id",
        type=str,
        default=None,
        help=(
            "Diffusion provider only: pinned checkpoint id/local snapshot "
            "path. Together with --checkpoint-sha256, builds a pinned "
            "DiffusionConfig instead of letting the provider fall back to "
            "its UNPINNED default -- the sha256 gate (verify_checkpoint_"
            "sha256) refuses real inference against an unpinned checkpoint "
            "regardless, but only deep inside the call stack; pinning here "
            "makes the CLI itself explicit about which checkpoint it is "
            "using. See docs/pod-bringup-commands.md Block 5."
        ),
    )
    parser.add_argument(
        "--checkpoint-sha256",
        type=str,
        default=None,
        help="Diffusion provider only: sha256 of --checkpoint-id's weights/snapshot.",
    )

    args = parser.parse_args()

    # Validate seed
    if not 0 <= args.seed < 2**64:
        print(f"error: seed must fit in u64, got {args.seed}", file=sys.stderr)
        return 1

    # Build a pinned DiffusionConfig for the diffusion provider so this CLI
    # can never silently fall back to DiffusionConfig()'s UNPINNED default
    # (docs/pod-bringup-commands.md Block 5's documented gap) -- --scale is
    # threaded through unconditionally so a pregen at --scale 8 doesn't hit
    # DiffusionProvider.generate's scale-mismatch guard against a config
    # that defaulted to scale=1.
    config = None
    if args.provider == "diffusion":
        from .providers.diffusion import DiffusionConfig

        config_kwargs: dict[str, object] = {"scale": args.scale}
        if args.checkpoint_id is not None:
            config_kwargs["checkpoint_id"] = args.checkpoint_id
        if args.checkpoint_sha256 is not None:
            config_kwargs["checkpoint_sha256"] = args.checkpoint_sha256
        config = DiffusionConfig(**config_kwargs)

    # Initialize provider and cache
    try:
        provider = _make_provider(args.provider, dry_run=args.dry_run, config=config)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    cache = TileCache(args.cache_dir)
    Path(args.cache_dir).mkdir(parents=True, exist_ok=True)

    # Generate tile coordinates in (2*radius+1)^2 square
    tiles_to_generate = []
    for dx in range(-args.radius, args.radius + 1):
        for dy in range(-args.radius, args.radius + 1):
            x = args.center_x + dx
            y = args.center_y + dy
            tiles_to_generate.append((x, y))

    start_time = time.time()
    generated = 0
    skipped = 0
    total_bytes = 0

    for i, (x, y) in enumerate(tiles_to_generate):
        # Check if already cached
        if cache.get(provider.provider_id, args.seed, x, y, args.scale) is not None:
            skipped += 1
        else:
            # Generate, encode, cache
            tile = provider.generate(args.seed, x, y, args.scale)
            encoded = tile_codec.encode(tile)
            cache.put(provider.provider_id, args.seed, x, y, args.scale, encoded)
            generated += 1
            total_bytes += len(encoded)

        # Print progress every 10 tiles (or at end)
        if (i + 1) % 10 == 0 or i + 1 == len(tiles_to_generate):
            elapsed = time.time() - start_time
            print(
                f"[{i + 1}/{len(tiles_to_generate)}] generated={generated} skipped={skipped}",
                file=sys.stderr,
            )

    elapsed = time.time() - start_time
    print(
        f"Pre-generation complete: generated={generated} skipped={skipped} total={len(tiles_to_generate)} "
        f"bytes={total_bytes} seconds={elapsed:.1f}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
