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

    args = parser.parse_args()

    # Validate seed
    if not 0 <= args.seed < 2**64:
        print(f"error: seed must fit in u64, got {args.seed}", file=sys.stderr)
        return 1

    # Initialize provider and cache
    try:
        provider = _make_provider(args.provider)
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
