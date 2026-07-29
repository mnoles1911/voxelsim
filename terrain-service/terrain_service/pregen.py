"""Pre-generate tiles for a given seed+radius offline (plan §3.4: "Pre-generate launch radius offline").

Run as: python -m terrain_service.pregen --seed <seed> --radius <r> [--center-x 0] [--center-y 0]
        [--scale 1] [--cache-dir ./tile-cache] [--provider synthetic]

Generates the (2*radius+1)^2 square of tiles around the center point. For each
tile, skips if already cached (by provider_id), else generates+encodes+caches.

TWO MODES
---------
``--mode coarse`` (default, unchanged) generates 30 m/px tiles from the
provider.

``--mode bake`` runs the Phase 2 geomorphic bake
(``terrain_service.bake.pipeline``) over already-generated coarse tiles and
writes the scale-16 fine tier (8192x8192 at 1.875 m/px) into the same
content-addressed namespace. It runs in three ordered passes, and the ORDER IS
LOAD-BEARING:

  1. coarse: every tile in the requested square PLUS a one-tile ring (the bake
     needs the 3x3 ring to fill its 960 m apron);
  2. hydrology: every flow superblock touching the square, top level down, from
     whatever coarse tiles are cached;
  3. bake: each target tile, reading its superblock for cross-tile inflow.

Doing hydrology before any bake is what makes a pregenerated world
order-independent. An on-demand frontier that bakes a tile the moment its ring
lands would build each superblock from whatever happened to exist at that
moment, and since a shipped tile is never regenerated, that choice is
permanent. See ``pipeline.HYDROLOGY_RESIDUALS`` #1.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import replace
from pathlib import Path

from . import tile_codec
from .cache import TileCache
from .app import _make_provider


#: The bake reads coarse tiles from the same namespace it writes the fine tier
#: into, and the coarse tier is always 30 m/px -- the learned cascade ends
#: there. Not a flag: a bake against anything else would be a bake against
#: interpolated data.
COARSE_SCALE = 1


def _coarse_elevation_m(cache: TileCache, provider, seed: int, x: int, y: int,
                        generate: bool):
    """Coarse tile (x, y)'s elevation in METRES, or None if unavailable.

    ``.vxtl`` v1 stores int16 whole metres (1 m vertical quantisation, 10x
    coarser than a voxel) -- the bake widens to float32 and everything below
    30 m of relief is synthesised from there.
    """
    import numpy as np

    data = cache.get(provider.provider_id, seed, x, y, COARSE_SCALE)
    if data is None:
        if not generate:
            return None
        tile = provider.generate(seed, x, y, COARSE_SCALE)
        data = tile_codec.encode(tile)
        cache.put(provider.provider_id, seed, x, y, COARSE_SCALE, data)
    return tile_codec.decode(data).elevation.astype(np.float32)


def _encode_fine(result, seed: int, provider_id: str):
    """Hand a BakeResult to tile_codec's v2 encoder, whatever it ended up called.

    ``tile_codec.py`` is owned by another workstream and its v2 entry point may
    not exist yet. Rather than guess a signature and silently write the wrong
    bytes, this probes for a plausible encoder and passes only the keyword
    arguments it actually accepts; if there is no v2 encoder it raises with an
    instruction, and ``--bake-npz-dir`` remains a usable output in the
    meantime. It never falls back to writing a v1 container -- a fine tier in a
    v1 wrapper is exactly the "silent disagreement between the two halves"
    docs/vxtl-v2-format.md opens by forbidding.
    """
    import inspect

    enc = None
    for name in ("encode_fine", "encode_v2", "encode_fine_tile"):
        enc = getattr(tile_codec, name, None)
        if enc is not None:
            break
    if enc is None:
        raise NotImplementedError(
            "tile_codec has no v2 fine-tier encoder yet (looked for "
            "encode_fine / encode_v2 / encode_fine_tile). The bake itself is "
            "done -- rerun with --bake-npz-dir to keep the output, and encode "
            "once docs/vxtl-v2-format.md's encoder lands."
        )
    candidates = {
        "seed": seed,
        "x": result.tile_x,
        "y": result.tile_y,
        "elevation_m": result.elevation_m,
        "elevation": result.elevation_m,
        "flow": result.flow,
        "flow_plane": result.flow,
        "provider_id": provider_id,
    }
    params = inspect.signature(enc).parameters
    kwargs = {k: v for k, v in candidates.items() if k in params}
    missing = [
        n
        for n, p in params.items()
        if p.default is inspect.Parameter.empty
        and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)
        and n not in kwargs
    ]
    if missing:
        raise NotImplementedError(
            f"tile_codec.{enc.__name__} needs arguments this CLI cannot supply "
            f"({missing}); wire it explicitly rather than letting pregen guess."
        )
    return enc(**kwargs)


def _run_bake(args, provider, cache: TileCache) -> int:
    """Bake mode: coarse pass, then hydrology pass, then the tiles.

    Reported in ``time.process_time()``. Wall-clock on this box reads exactly
    like a slow configuration when another session holds it; CPU-seconds
    cannot be stolen by a competing process.
    """
    import numpy as np

    from .bake import pipeline as bp

    if args.scale != COARSE_SCALE:
        print(
            f"note: --mode bake ignores --scale {args.scale}; it reads the "
            f"s{COARSE_SCALE} tier and writes s{bp.PRODUCTION.scale}",
            file=sys.stderr,
        )

    consts = bp.CONSTANTS
    if args.bake_superblock_tiles is not None:
        consts = replace(consts, superblock_tiles=args.bake_superblock_tiles)
    if args.bake_max_level is not None:
        consts = replace(consts, superblock_max_level=args.bake_max_level)
    geom = bp.PRODUCTION
    geom.assert_production()

    if consts is not bp.CONSTANTS:
        print(
            "warning: overridden bake constants roll the bake fingerprint but "
            "NOT provider_id unless the override is also made the default in "
            "pipeline.py -- these tiles would land in the same namespace as "
            "default-constant tiles. Use for experiments in a scratch "
            "--cache-dir only.",
            file=sys.stderr,
        )

    try:
        kernels = bp.load_kernels()
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    targets = [
        (args.center_x + dx, args.center_y + dy)
        for dx in range(-args.radius, args.radius + 1)
        for dy in range(-args.radius, args.radius + 1)
    ]
    generate_coarse = not args.bake_no_coarse_generate
    cpu0 = time.process_time()

    # -- pass 1: coarse tiles, target square plus the one-tile apron ring.
    ring = {
        (x + dx, y + dy)
        for (x, y) in targets
        for dx in (-1, 0, 1)
        for dy in (-1, 0, 1)
    }
    coarse_cache: dict[tuple[int, int], object] = {}

    def fetch(x: int, y: int, generate: bool = False):
        key = (x, y)
        if key not in coarse_cache:
            coarse_cache[key] = _coarse_elevation_m(
                cache, provider, args.seed, x, y, generate
            )
        return coarse_cache[key]

    have = 0
    for x, y in sorted(ring):
        if fetch(x, y, generate=generate_coarse) is not None:
            have += 1
    print(
        f"[coarse] {have}/{len(ring)} ring tiles available "
        f"(generate={generate_coarse}) cpu={time.process_time() - cpu0:.1f}s",
        file=sys.stderr,
    )
    if have == 0:
        print("error: no coarse tiles available to bake from", file=sys.stderr)
        return 1

    # -- pass 2: hydrology, TOP LEVEL FIRST so each level can inject into the
    # one below. Doing this before any bake is what makes a pregenerated world
    # order-independent (pipeline.HYDROLOGY_RESIDUALS #1).
    levels = [
        bp.FlowLevel(level=lv, geom=geom, consts=consts)
        for lv in range(consts.superblock_max_level, -1, -1)
    ]
    superblocks: dict[tuple[int, int, int], object] = {}
    for level in levels:
        needed = {bp.superblock_index(x, y, level) for (x, y) in ring}
        for sx, sy in sorted(needed):
            blob = cache.get_flow(provider.provider_id, args.seed, level.level, sx, sy)
            if blob is not None:
                sb, _ = bp.decode_flow_superblock(blob)
            else:
                parent = None
                if level.level < consts.superblock_max_level:
                    up = bp.FlowLevel(
                        level=level.level + 1, geom=geom, consts=consts
                    )
                    # The parent covering this block's own origin tile.
                    ptx, pty = sx * level.tiles_per_side, sy * level.tiles_per_side
                    parent = superblocks.get(
                        (up.level,) + bp.superblock_index(ptx, pty, up)
                    )
                sb = bp.build_flow_superblock(
                    lambda x, y: fetch(x, y, generate=False),
                    sx,
                    sy,
                    level,
                    kernels,
                    parent=parent,
                )
                cache.put_flow(
                    provider.provider_id,
                    args.seed,
                    level.level,
                    sx,
                    sy,
                    bp.encode_flow_superblock(sb, args.seed),
                )
            superblocks[(level.level, sx, sy)] = sb
        print(
            f"[flow L{level.level}] {len(needed)} superblock(s) "
            f"@ {level.cell_m:.0f} m/px, span {level.span_m / 1000:.0f} km  "
            f"cpu={time.process_time() - cpu0:.1f}s",
            file=sys.stderr,
        )

    # -- pass 3: the bakes.
    level0 = bp.FlowLevel(level=0, geom=geom, consts=consts)
    baked = skipped = failed = 0
    npz_dir = Path(args.bake_npz_dir) if args.bake_npz_dir else None
    if npz_dir:
        npz_dir.mkdir(parents=True, exist_ok=True)

    for i, (x, y) in enumerate(targets):
        if cache.get_fine(provider.provider_id, args.seed, x, y) is not None:
            skipped += 1
            continue
        sb = superblocks.get((0,) + bp.superblock_index(x, y, level0))
        t0 = time.process_time()
        result = bp.bake_tile(
            world_seed=args.seed,
            tile_x=x,
            tile_y=y,
            coarse_fetch=lambda cx, cy: fetch(cx, cy, generate=False),
            kernels=kernels,
            geom=geom,
            consts=consts,
            inflow_source=sb,
        )
        cpu = time.process_time() - t0
        if result.missing_coarse:
            print(
                f"  warning: tile ({x},{y}) baked with {len(result.missing_coarse)} "
                f"of its 9 ring tiles missing; the apron there is sea level and "
                f"the interior near that edge is NOT the infinite-domain answer",
                file=sys.stderr,
            )
        if result.stats["interior_dead_ends"]:
            # After an epsilon fill, receiver == -1 means "border cell draining
            # out of the domain" and nothing else. An interior one is a routing
            # bug, and a bug baked into a shipped tile is permanent.
            print(
                f"error: tile ({x},{y}) has "
                f"{int(result.stats['interior_dead_ends'])} interior cells with "
                "no receiver after the depression fill. That is a routing bug, "
                "not terrain -- refusing to ship it.",
                file=sys.stderr,
            )
            failed += 1
            continue
        if result.stats["basin_exceeds_apron"]:
            print(
                f"  warning: tile ({x},{y}) contains a flat/basin "
                f"{result.stats['max_basin_run_m']:.0f} m across, wider than the "
                f"{bp.PRODUCTION.apron_m:.0f} m apron. Elevations still agree "
                "across the seam (the effect is sub-ULP, below the 100 mm wire "
                "LSB) but its ROUTING may not -- see pipeline.APRON_BLIND_SPOT.",
                file=sys.stderr,
            )
        if npz_dir:
            np.savez(
                npz_dir / f"{x}_{y}.npz",
                elevation_m=result.elevation_m,
                accumulation_m2=result.accumulation_m2,
                flow=result.flow,
            )
        try:
            encoded = _encode_fine(result, args.seed, provider.provider_id)
        except NotImplementedError as e:
            print(f"error: {e}", file=sys.stderr)
            failed += 1
            if npz_dir is None:
                return 1
            continue
        cache.put_fine(provider.provider_id, args.seed, x, y, encoded)
        baked += 1
        print(
            f"[{i + 1}/{len(targets)}] baked ({x},{y}) cpu={cpu:.1f}s "
            f"max_catchment={result.stats['max_accumulation_km2']:.1f}km2 "
            f"incision_p99={result.stats['incision_p99_m']:.2f}m "
            f"channels={int(result.stats['channel_cells'])} "
            f"injected={result.stats['injected_inflow_km2']:.1f}km2 "
            f"basin={result.stats['basin_cells_frac']*100:.1f}%",
            file=sys.stderr,
        )

    print(
        f"Bake complete: baked={baked} skipped={skipped} unencoded={failed} "
        f"total={len(targets)} cpu_seconds={time.process_time() - cpu0:.1f}",
        file=sys.stderr,
    )
    return 0 if failed == 0 else 1


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
        help=(
            "Coarse-mode tile scale. Only 1 (30 m/px) generates real data: "
            "the learned cascade ends at 30 m and the bilinear scale-8 path "
            "was deleted (see providers/diffusion.py::_get_native). The "
            "sub-30 m tier comes from --mode bake, which always writes "
            "cache.FINE_SCALE (16) regardless of this flag."
        ),
    )
    parser.add_argument(
        "--mode",
        type=str,
        default="coarse",
        choices=["coarse", "bake"],
        help=(
            "coarse (default): generate 30 m/px tiles from the provider. "
            "bake: run the B0-B3 geomorphic bake over cached coarse tiles and "
            "write the scale-16 fine tier. See this module's docstring for why "
            "bake mode runs hydrology before any tile is baked."
        ),
    )
    parser.add_argument(
        "--bake-superblock-tiles",
        type=int,
        default=None,
        help=(
            "Bake mode: override BakeConstants.superblock_tiles (coarse tiles "
            "per side of a level-0 flow superblock). Rolls the bake identity, "
            "hence provider_id, hence the whole world -- for experiments only."
        ),
    )
    parser.add_argument(
        "--bake-max-level",
        type=int,
        default=None,
        help=(
            "Bake mode: override BakeConstants.superblock_max_level. Level L "
            "spans 4^(L+1) tiles; the top level receives no inflow at its own "
            "edges, so it is where catchment truncation happens."
        ),
    )
    parser.add_argument(
        "--bake-npz-dir",
        type=str,
        default=None,
        help=(
            "Bake mode: also dump each baked tier as an .npz here. Useful "
            "before tile_codec's v2 encoder lands, and for feeding "
            "tools/bake_seam_check.py. ~270 MB/tile uncompressed."
        ),
    )
    parser.add_argument(
        "--bake-no-coarse-generate",
        action="store_true",
        help=(
            "Bake mode: never call the provider. Bake only tiles whose full "
            "3x3 coarse ring is already cached, and build superblocks from "
            "cached tiles only. What a pod uses when coarse generation is "
            "another worker's job."
        ),
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
        help=(
            "Diffusion provider only: sha256 of --checkpoint-id's "
            "weights/snapshot. THIS, not the path, is what identifies the "
            "checkpoint in provider_id."
        ),
    )
    parser.add_argument(
        "--checkpoint-label",
        type=str,
        default=None,
        help=(
            "Diffusion provider only: human-readable checkpoint name (e.g. "
            "terrain-diffusion-30m) for legible cache dirs and edit-log "
            "stamps. Hashed into provider_id; must NOT be a path -- "
            "--checkpoint-id is the load location and is deliberately "
            "excluded from the identity."
        ),
    )
    parser.add_argument(
        "--conditioning-digest",
        type=str,
        default=None,
        help=(
            "Diffusion provider only: digest of the conditioning rasters "
            "(WorldClim bio + data/global/etopo_10m.tif) from "
            "compute_conditioning_digest(). They condition generation, so "
            "different copies mean different terrain under what would "
            "otherwise be one identity. Pass --print-conditioning-digest to "
            "compute it for this box."
        ),
    )
    parser.add_argument(
        "--terrain-diffusion-version",
        type=str,
        default=None,
        help="Diffusion provider only: terrain-diffusion package version/commit.",
    )
    parser.add_argument(
        "--provider-id-override",
        type=str,
        default=None,
        help=(
            "COMPATIBILITY ONLY: write into an existing cache namespace "
            "verbatim (e.g. resume tiles generated under the pre-v2 "
            "provider_id). Defeats every identity guarantee -- see "
            "DiffusionConfig.provider_id_override."
        ),
    )
    parser.add_argument(
        "--print-conditioning-digest",
        action="store_true",
        help=(
            "Compute and print this box's conditioning digest, then exit "
            "(nothing is generated). Run this at bring-up to get the value "
            "for --conditioning-digest / "
            "TERRAIN_DIFFUSION_CONDITIONING_DIGEST."
        ),
    )

    args = parser.parse_args()

    if args.print_conditioning_digest:
        from .providers.diffusion import (
            ConditioningDataMissing,
            compute_conditioning_digest,
            resolve_conditioning_root,
        )

        try:
            digest = compute_conditioning_digest()
        except ConditioningDataMissing as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
        print(f"conditioning_root:   {resolve_conditioning_root()}")
        print(f"conditioning_digest: {digest}")
        return 0

    # Validate seed
    if not 0 <= args.seed < 2**64:
        print(f"error: seed must fit in u64, got {args.seed}", file=sys.stderr)
        return 1

    if args.scale not in tile_codec.PIXEL_SIZE_MM:
        print(
            f"error: --scale must be one of {sorted(tile_codec.PIXEL_SIZE_MM)}, "
            f"got {args.scale}",
            file=sys.stderr,
        )
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
        for flag, fieldname in (
            (args.checkpoint_id, "checkpoint_id"),
            (args.checkpoint_label, "checkpoint_label"),
            (args.checkpoint_sha256, "checkpoint_sha256"),
            (args.conditioning_digest, "conditioning_digest"),
            (args.terrain_diffusion_version, "terrain_diffusion_version"),
            (args.provider_id_override, "provider_id_override"),
        ):
            if flag is not None:
                config_kwargs[fieldname] = flag
        try:
            config = DiffusionConfig(**config_kwargs)
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

    # Initialize provider and cache
    try:
        provider = _make_provider(args.provider, dry_run=args.dry_run, config=config)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    cache = TileCache(args.cache_dir)
    Path(args.cache_dir).mkdir(parents=True, exist_ok=True)

    # Echo the identity this run writes under: it is the cache namespace AND
    # the value stamped into edit logs, so a run that silently landed in the
    # wrong namespace (or under an UNPINNED/UNVERIFIEDDATA-marked id) should
    # be obvious from the first line of the log, not discovered later.
    print(f"provider_id: {provider.provider_id}", file=sys.stderr)

    if args.mode == "bake":
        return _run_bake(args, provider, cache)

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
