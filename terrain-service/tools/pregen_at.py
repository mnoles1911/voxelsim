#!/usr/bin/env python3
"""Pregen a launch grid at a chosen origin and package it, unattended.

Designed to be started with nohup and left alone: it pregens, verifies, writes
the tarball, and records every identity value needed later (provider_id,
checkpoint sha256, per-tile hashes) into a manifest beside the tarball -- so if
the terminal dies, or you come back tomorrow, nothing has to be recovered from
scrollback.

Restartable: tiles already in the cache are skipped, so re-running after an
interruption costs only the tiles that were missing.

Usage (from terrain-service/):
    python tools/pregen_at.py <checkpoint_dir> <sha256> --origin X,Y [--radius 2]

Typical unattended invocation:
    nohup python tools/pregen_at.py /workspace/ckpt/terrain-diffusion-30m <HASH> \
        --origin 6,-3 > /workspace/pregen.log 2>&1 &
    tail -f /workspace/pregen.log        # ctrl-C this safely; the job keeps going
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint_dir")
    ap.add_argument("checkpoint_sha256")
    ap.add_argument("--origin", required=True, help="X,Y tile coords, e.g. 6,-3")
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--radius", type=int, default=2)
    ap.add_argument("--cache", default="/workspace/tile-cache")
    ap.add_argument("--out", default="/workspace/tile-cache-seed20260719.tar.gz")
    args = ap.parse_args()

    if len(args.checkpoint_sha256) != 64 or args.checkpoint_sha256.startswith("<"):
        print("ERROR: sha256 must be 64 hex chars with no <> brackets.")
        return 2
    try:
        ox, oy = (int(v) for v in args.origin.split(","))
    except ValueError:
        print("ERROR: --origin must be X,Y (e.g. 6,-3)")
        return 2

    from terrain_service import tile_codec
    from terrain_service.cache import TileCache
    from terrain_service.providers.diffusion import DiffusionConfig, DiffusionProvider

    config = DiffusionConfig(checkpoint_id=args.checkpoint_dir,
                             checkpoint_sha256=args.checkpoint_sha256)
    provider = DiffusionProvider(config=config)
    cache = TileCache(args.cache)

    R, SEED, SCALE = args.radius, args.seed, 1
    coords = [(ox + dx, oy + dy)
              for dy in range(-R, R + 1) for dx in range(-R, R + 1)]

    print(f"origin ({ox},{oy})  radius {R}  -> {len(coords)} tiles")
    print(f"provider_id: {provider.provider_id}", flush=True)

    t0 = time.time()
    hashes = {}
    for i, (x, y) in enumerate(coords, 1):
        blob = cache.get(provider.provider_id, SEED, x, y, SCALE)
        if blob is None:
            blob = tile_codec.encode(provider.generate(SEED, x, y, SCALE))
            cache.put(provider.provider_id, SEED, x, y, SCALE, blob)
            state = "done"
        else:
            state = "cached"
        hashes[f"{x},{y}"] = hashlib.sha256(blob).hexdigest()
        print(f"[{i:3d}/{len(coords)}] ({x:+4d},{y:+4d}) {state:6s} "
              f"{len(blob)} bytes  {time.time() - t0:6.0f}s", flush=True)

    # Manifest first, so the identity survives even if tar fails.
    manifest = {
        "seed": SEED,
        "origin": [ox, oy],
        "radius": R,
        "scale": SCALE,
        "provider_id": provider.provider_id,
        "checkpoint_sha256": args.checkpoint_sha256,
        "tile_sha256": hashes,
    }
    manifest_path = os.path.join(args.cache, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    print(f"\nwrote {manifest_path}")

    parent, leaf = os.path.dirname(args.cache), os.path.basename(args.cache)
    subprocess.run(["tar", "czf", args.out, "-C", parent, leaf], check=True)
    size = os.path.getsize(args.out)
    print(f"wrote {args.out}  {size / 1e6:.1f} MB")

    print("\n=== RECORD THESE ===")
    print(f"provider_id       : {provider.provider_id}")
    print(f"checkpoint_sha256 : {args.checkpoint_sha256}")
    print(f"origin            : ({ox},{oy})  radius {R}")
    print(f"tarball           : {args.out}")
    print("\nNEXT: download the tarball, THEN destroy the instance")
    print("(Vast bills storage for as long as the instance exists).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
