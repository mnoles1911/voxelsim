#!/usr/bin/env python3
"""Find a land origin for the launch grid.

Tile (0,0) of seed 20260719 is entirely underwater (elevation max -8.9 m), so a
pregen centred on the origin produces 25 tiles of ocean. This samples tiles on a
STRIDED grid and reports where the land is, so the launch origin can be chosen
before spending an hour on a real pregen.

Strided, not contiguous, on purpose: one tile is 512 px x 30 m = 15.4 km across,
so stride 3 covers ~46 km per step and a 5x5 scan reaches ~230 km in each
direction for 25 tiles (~9 min at 22.5 s/tile). Contiguous sampling would search
a 77 km box for the same cost, and the whole problem is that the neighbourhood of
the origin is ocean.

Note `scale` is a SUPERSAMPLE knob (1 => 30 m/px, 8 => 3.75 m/px), so a larger
scale covers LESS ground, not more. There is no cheap coarse query; sparse
sampling is the lever.

Usage (from terrain-service/):
    python tools/scan_land.py <checkpoint_dir> <sha256> [--radius 2] [--stride 3]

Prints a per-tile line plus an ASCII map, and ranks candidate origins by land
fraction so the choice is data, not vibes.
"""

import argparse
import json
import os
import sys
import time

import numpy as np


def _write_json(path, args, results) -> None:
    """Atomically dump scan state for tools/pick_origin.py to consume."""
    payload = {
        "seed": args.seed,
        "radius": args.radius,
        "stride": args.stride,
        "checkpoint_dir": args.checkpoint_dir,
        "tiles": [
            {"x": x, "y": y, "land": land, "min": lo, "max": hi}
            for (x, y), (land, lo, hi) in sorted(results.items())
        ],
    }
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(payload, f, indent=2)
    os.replace(tmp, path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("checkpoint_dir")
    ap.add_argument("checkpoint_sha256")
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--radius", type=int, default=2, help="scan radius in strided steps")
    ap.add_argument("--stride", type=int, default=3, help="tiles skipped between samples")
    ap.add_argument("--json", help="write raw results here for tools/pick_origin.py")
    ap.add_argument("--conditioning-digest", default=None,
                    help="pin explicitly; default is to hash this box's rasters")
    args = ap.parse_args()

    if len(args.checkpoint_sha256) != 64 or args.checkpoint_sha256.startswith("<"):
        print("ERROR: sha256 must be 64 hex chars with no <> brackets.")
        return 2

    from terrain_service.providers.diffusion import (
        ConditioningDataMissing,
        DiffusionConfig,
        DiffusionProvider,
        compute_conditioning_digest,
        resolve_conditioning_root,
    )

    # Inference refuses to run against UNVERIFIED conditioning data (the
    # WorldClim/ETOPO rasters condition generation, so two boxes with
    # different copies produce different terrain under one identity). This is
    # a scan, not a canonical generate, so hashing whatever this box actually
    # has is the honest pin -- it records what produced these numbers rather
    # than asserting what should have.
    digest = args.conditioning_digest
    if digest is None:
        try:
            digest = compute_conditioning_digest()
        except ConditioningDataMissing as e:
            print(f"ERROR: {e}")
            print(f"Run from the directory containing "
                  f"{resolve_conditioning_root()}, and build ETOPO first:")
            print("  python tools/fetch_etopo.py")
            return 2

    config = DiffusionConfig(checkpoint_id=args.checkpoint_dir,
                             checkpoint_sha256=args.checkpoint_sha256,
                             conditioning_digest=digest)
    provider = DiffusionProvider(config=config)

    R, S = args.radius, args.stride
    coords = [(x * S, y * S) for y in range(-R, R + 1) for x in range(-R, R + 1)]
    print(f"scanning {len(coords)} tiles, stride {S} "
          f"(~{S * 15.36:.0f} km per step, ~{R * S * 15.36:.0f} km reach)\n")

    results = {}
    t0 = time.time()
    for x, y in coords:
        elev = provider._call_model(seed=args.seed, x=x, y=y, scale=1)["elevation"]
        # Land fraction is the decision variable: a tile whose MAX is above sea
        # level can still be 99% ocean with one rock in it, which is not a
        # spawn. Mean elevation is likewise misleading next to a trench.
        land = float((elev > 0).mean())
        results[(x, y)] = (land, float(elev.min()), float(elev.max()))
        print(f"({x:+4d},{y:+4d})  land={land * 100:5.1f}%  "
              f"min={elev.min():9.1f}  max={elev.max():8.1f}   "
              f"[{time.time() - t0:5.0f}s]", flush=True)
        # Rewrite after EVERY tile, not at the end: a 25-tile scan is ~9 min
        # of GPU time and a dropped web terminal must not cost all of it.
        if args.json:
            _write_json(args.json, args, results)

    # ASCII map, north up. Symbols are coarse on purpose -- this is for picking
    # a region to look at, not for measuring one.
    print("\nland fraction map (north up):")
    for y in range(-R, R + 1):
        row = ""
        for x in range(-R, R + 1):
            land = results[(x * S, y * S)][0]
            row += "#" if land > 0.75 else "+" if land > 0.40 else "." if land > 0.05 else "~"
        print("  " + row)
    print("  ~ ocean   . coast   + mixed   # land")

    ranked = sorted(results.items(), key=lambda kv: -kv[1][0])
    print("\nbest candidates:")
    for (x, y), (land, lo, hi) in ranked[:5]:
        print(f"  ({x:+4d},{y:+4d})  land={land * 100:5.1f}%  elev {lo:.0f} .. {hi:.0f} m")

    best, (bland, _, _) = ranked[0]
    if bland < 0.05:
        print("\nNo land found. Widen --radius/--stride and rerun; the scan is")
        print("restartable and each tile is ~22.5 s.")
        return 1
    # NB: this "best" is raw land fraction, which over-rewards inland
    # plateaus. tools/pick_origin.py scores the same data for a COASTAL
    # origin and is what generate_world.sh actually uses; this line is a
    # human-readable cross-check, not the decision.
    print(f"\nHighest land fraction: {best}  (see tools/pick_origin.py "
          f"for the coastal-weighted pick actually used)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
