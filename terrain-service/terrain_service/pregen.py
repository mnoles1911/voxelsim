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

EVERY MODE RECORDS THE WORLD'S IDENTITY
---------------------------------------
Before the first tile, each mode writes (or confirms) ``world-identity.json``
in the world directory it is about to write into -- coarse mode under
``provider_id``, bake mode under ``fine_provider_id``, since those are two
different worlds' worth of artifacts. It carries the checkpoint sha256, the
conditioning digest, the sha256 of each conditioning file, the
terrain-diffusion version and a timestamp, and a run whose identity disagrees
with a world that already has one is refused before it generates anything.

Automatic, with no flag to forget: the 289-tile world that can never be
extended was lost because nobody wrote this down at the time. See
``world_manifest.py``,
docs/measurements/world-identity-not-reproducible-2026-08-03.txt (the
cross-machine finding) and
docs/measurements/etopo-build-not-reproducible-2026-08-02.txt (why the two
built conditioning files have no builder, and why that turned out NOT to be
what froze that particular world).
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
from .world_manifest import record_world_identity


#: The bake reads coarse tiles from the same namespace it writes the fine tier
#: into, and the coarse tier is always 30 m/px -- the learned cascade ends
#: there. Not a flag: a bake against anything else would be a bake against
#: interpolated data.
COARSE_SCALE = 1


def _build_model_parent(args, provider, geom, consts, kernels, cache, blocks):
    """The MODEL-BACKED top of the hydrology pyramid, or None.

    WHAT PROBLEM. ``superblock_max_level`` (1 by default, 246 km) gets no
    inflow at its own edges, so a river draining more than 246 km arrives with
    ZERO upstream area and carves nothing -- ``pipeline.HYDROLOGY_RESIDUALS``
    #2. The fix is a parent above it, and the cheapest parent available is the
    diffusion model's own coarse stage: one cell is 7.68 km, so a 512^2 window
    spans 3,932 km and costs coarse-stage inference only. Buying the same reach
    as an EXACT pyramid level would need 4,096 coarse tiles (~6 GB) to exist
    first, and only reach 983 km.

    WHY HERE. ``bake/pipeline.py`` receives a ``CoarseFetch`` callable and must
    not learn that a diffusion model exists -- that is a design property, not
    an accident. ``pregen`` holds the provider AND calls
    ``build_flow_superblock``, so it is the one place both halves are already
    in scope. ``pipeline.build_model_superblock`` takes an ARRAY.

    WHY THE WINDOW IS WORLD-ANCHORED. Same reason ``superblock_index`` floors:
    two top-level blocks that share an edge must see the same parent, or the
    two sides of that edge disagree about how much water crosses it. The window
    grid is anchored at multiples of ``--bake-model-window`` coarse cells and
    the window is required to be a whole number of top-level blocks, so a block
    can never straddle two windows and get half a parent.

    ``blocks`` is the set of top-level ``(sx, sy)`` this run needs. They must
    all land in ONE window: a run spanning two windows would need two parents,
    which is supportable but has never been needed (one window is 3,932 km) and
    is refused rather than half-implemented.
    """
    import numpy as np

    from .bake import pipeline as bp

    if not hasattr(provider, "coarse_elevation_m"):
        print(
            f"error: --bake-model-parent needs a provider that can serve its "
            f"own coarse stage; {type(provider).__name__} cannot. Use "
            "--provider diffusion against a real checkpoint (not --dry-run).",
            file=sys.stderr,
        )
        return None

    fine_px = provider.coarse_cell_fine_px(args.seed)
    cells_per_tile, rem = divmod(geom.coarse_tile_px, fine_px)
    if rem or cells_per_tile < 1:
        print(
            f"error: a {geom.coarse_tile_px}px coarse tile is not a whole "
            f"number of {fine_px}px coarse cells; the model window cannot be "
            "aligned to the tile grid.",
            file=sys.stderr,
        )
        return None

    top = bp.FlowLevel(level=consts.superblock_max_level, geom=geom, consts=consts)
    block_cells = top.tiles_per_side * cells_per_tile
    window = int(getattr(args, "bake_model_window", 512))
    if window % block_cells:
        print(
            f"error: --bake-model-window {window} is not a multiple of the "
            f"{block_cells} coarse cells a level-{top.level} superblock spans. "
            "A block straddling two windows would get half a parent and no "
            "warning; pick a multiple.",
            file=sys.stderr,
        )
        return None

    # World-anchored: the window index comes from the block's own world
    # position, never from where this run happens to be centred.
    windows = {(sx * block_cells // window, sy * block_cells // window)
               for sx, sy in blocks}
    if len(windows) != 1:
        print(
            f"error: this run's level-{top.level} superblocks span "
            f"{len(windows)} model windows ({sorted(windows)}). One window is "
            f"{window * fine_px * geom.coarse_pixel_m / 1000:.0f} km; a run "
            "wider than that needs one parent per window, which is not wired.",
            file=sys.stderr,
        )
        return None
    mx, my = windows.pop()

    cell_m = fine_px * geom.coarse_pixel_m
    origin_m = (mx * window * cell_m, my * window * cell_m)
    cpu0 = time.process_time()

    # Reuse a cached window only when it is the SAME window. The cache key
    # (fine_provider_id, seed, level, index) covers the checkpoint, the
    # conditioning data and the world shape, but NOT the window size -- and
    # unlike a tile-backed block this one's fingerprint cannot be re-verified
    # without re-running inference, which is the cost the cache exists to
    # avoid. Geometry is the part that can be checked for free, so check it.
    sb = None
    blob = cache.get_flow(provider.fine_provider_id, args.seed,
                          bp.MODEL_FLOW_LEVEL, mx, my)
    if blob is not None:
        try:
            cached, _ = bp.decode_flow_superblock(blob)
        except ValueError as e:
            print(f"  warning: cached model window is unreadable ({e}); "
                  "rebuilding", file=sys.stderr)
        else:
            if (cached.size_px, cached.cell_m, cached.origin_m) == (
                window, cell_m, origin_m
            ):
                sb = cached
            else:
                print(
                    f"  warning: cached model window ({mx},{my}) is "
                    f"{cached.size_px}px @ {cached.cell_m:.0f}m from "
                    f"{cached.origin_m}, this run wants {window}px @ "
                    f"{cell_m:.0f}m from {origin_m}; rebuilding",
                    file=sys.stderr,
                )

    if sb is None:
        ci0, cj0 = my * window, mx * window
        elev = provider.coarse_elevation_m(
            args.seed, ci0, ci0 + window, cj0, cj0 + window
        )
        sb = bp.build_model_superblock(
            np.asarray(elev, np.float32),
            origin_m=origin_m,
            cell_m=cell_m,
            kernels=kernels,
            sx=mx,
            sy=my,
            consts=consts,
        )
        cache.put_flow(
            provider.fine_provider_id, args.seed, bp.MODEL_FLOW_LEVEL, mx, my,
            bp.encode_flow_superblock(sb, args.seed),
        )

    # A parent that only half-covers its child injects at the edges it reaches
    # and truncates the rest -- silently, with fewer entry cells and smaller
    # rivers as the only symptom. The alignment arithmetic above should make
    # this impossible; assert it rather than trust it.
    for sx, sy in sorted(blocks):
        child_origin = (sx * top.span_m, sy * top.span_m)
        if not bp.superblock_covers(sb, child_origin, top.span_m):
            print(
                f"error: model window ({mx},{my}) does not cover level-"
                f"{top.level} superblock ({sx},{sy}) at {child_origin}",
                file=sys.stderr,
            )
            return None

    print(
        f"[flow M] model window ({mx},{my}) {window}x{window} @ "
        f"{cell_m:.0f} m/px, span {window * cell_m / 1000:.0f} km, "
        f"origin {origin_m}  max_acc={float(sb.acc.max()) / 1e6:,.0f} km2  "
        f"fp={sb.fingerprint_hex[:12]}  cpu={time.process_time() - cpu0:.1f}s",
        file=sys.stderr,
    )
    return sb


def _coarse_planes(cache: TileCache, provider, seed: int, x: int, y: int,
                   generate: bool):
    """Coarse tile (x, y) as ``(elevation_m float32, climate uint8)``, or None.

    ``.vxtl`` v1 stores int16 whole metres (1 m vertical quantisation, 10x
    coarser than a voxel) -- the bake widens to float32 and everything below
    30 m of relief is synthesised from there.

    CLIMATE IS NOW KEPT (bake_ver 7). This used to be ``_coarse_elevation_m``
    and threw the ``(4, 512, 512)`` uint8 climate plane away at the decode,
    which is why the bake could only ever vary its physics with SHAPE. The
    landform-province partition (``bake.province``) needs temperature and
    precipitation to tell a glacial mountain from a fluvial one, and the
    shipped cache tiles are already the full 1,572,889-byte form -- the data
    was there the whole time and cost nothing more to read.
    """
    import numpy as np

    data = cache.get(provider.provider_id, seed, x, y, COARSE_SCALE)
    if data is None:
        if not generate:
            return None
        tile = provider.generate(seed, x, y, COARSE_SCALE)
        data = tile_codec.encode(tile)
        cache.put(provider.provider_id, seed, x, y, COARSE_SCALE, data)
    tile = tile_codec.decode(data)
    return tile.elevation.astype(np.float32), tile.climate


def _encode_fine(result, seed: int, provider_id: str, codec: int | None = None):
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
        # THE CODEC HAS TO BE PASSED, and it was not until 2026-08-01.
        #
        # tile_codec.encode_fine defaults to CODEC_RAW, and that default is
        # RIGHT for the library: CODEC_RAW must never depend on an optional
        # compression package, and CI deliberately does not install zstandard.
        # But this dict is the ONLY channel through which pregen reaches the
        # encoder, and `codec` was absent from it -- so there was no way to
        # produce a compressed fine tile through the production path at all.
        # Every tile pregen has ever written is uncompressed.
        #
        # MEASURED on tile (-5,2): 201.4 MB RAW against 33.4 MB CODEC_ZSTD,
        # 6.0x, with elevation and flow planes both bit-identical on round
        # trip. At 4,200 tiles per 1M km^2 that is ~845 GB against ~140 GB of
        # storage and of wire, and the client has decoded zstd since the
        # runtime binder landed.
        #
        # None means "let the encoder choose", which keeps the library's own
        # default reachable for a box with no zstandard installed.
        **({"codec": codec} if codec is not None else {}),
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


def _record_identity(cache: TileCache, provider, seed: int, namespace_id) -> bool:
    """Record (or confirm) what made this world, and say so on stderr.

    Not a flag. The world that cannot be extended today was lost to a bring-up
    that had no reason to think of this, and any switch this needed would have
    been off that day too -- see world_manifest.py.
    """
    ok, msg = record_world_identity(cache, provider, seed, namespace_id)
    if msg:
        print(msg, file=sys.stderr)
    if not ok:
        print(
            "Refusing to write into a world whose identity this run does not "
            "match. Nothing has been generated.",
            file=sys.stderr,
        )
    return ok


def _run_bake(args, provider, cache: TileCache) -> int:
    """Bake mode: coarse pass, then hydrology pass, then the tiles.

    Reported in ``time.process_time()``. Wall-clock on this box reads exactly
    like a slow configuration when another session holds it; CPU-seconds
    cannot be stolen by a competing process.
    """
    import numpy as np

    from .bake import pipeline as bp

    # EVERYTHING THIS FUNCTION WRITES IS BAKE-DERIVED, so it all keys on
    # fine_provider_id (inference identity + bake digest), never on
    # provider_id (inference only). The coarse tiles it READS are still under
    # provider_id -- see _coarse_planes, which is left alone deliberately.
    #
    # That asymmetry is the entire point of the split: retuning a bake constant
    # must re-key the fine tier and the flow pyramid together while leaving the
    # ~22.5 s/tile coarse inference output exactly where it is. See
    # providers/diffusion.py::_bake_fingerprint for what the old single-id
    # policy cost.
    # An ATTRIBUTE on providers, not a method -- same shape as provider_id,
    # per the TileProvider protocol. (DiffusionConfig.fine_provider_id() IS a
    # method; the provider snapshots it at construction the way it does
    # provider_id, so a config mutated afterwards cannot repoint a live cache.)
    fine_provider_id = provider.fine_provider_id

    # The fine tier is its OWN world directory (bake identity included), so it
    # carries its own identity record -- and it records the coarse provider_id
    # it was baked from, which the directory name alone only half tells you.
    if not _record_identity(cache, provider, args.seed, fine_provider_id):
        return 1

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
    verify_superblocks = not args.bake_no_verify_superblocks
    cpu0 = time.process_time()

    # -- pass 1: coarse tiles, target square plus the one-tile apron ring.
    ring = {
        (x + dx, y + dy)
        for (x, y) in targets
        for dx in (-1, 0, 1)
        for dy in (-1, 0, 1)
    }
    coarse_cache: dict[tuple[int, int], object] = {}

    def _planes(x: int, y: int, generate: bool = False):
        key = (x, y)
        if key not in coarse_cache:
            coarse_cache[key] = _coarse_planes(
                cache, provider, args.seed, x, y, generate
            )
        return coarse_cache[key]

    def fetch(x: int, y: int, generate: bool = False):
        """ELEVATION only -- this is the ``CoarseFetch`` the hydrology hashes.

        Kept byte-identical to what it returned before climate was plumbed
        through: ``superblock_inputs_fingerprint`` digests this array directly,
        so widening it would have rewritten every cached flow superblock's
        fingerprint for a plane the pyramid does not read.
        """
        p = _planes(x, y, generate)
        return None if p is None else p[0]

    def fetch_climate(x: int, y: int):
        """The ``(4, n, n)`` uint8 climate planes, for the province partition.

        Never generates: a tile that does not already exist here has no climate
        to offer, and the elevation pass above has already decided what the ring
        contains.
        """
        p = _planes(x, y, generate=False)
        return None if p is None else p[1]

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
    stale_superblocks = 0
    incomplete_superblocks = 0

    # THE TOP LEVEL'S OWN PARENT, from the model rather than from tiles.
    # Without it the top level receives nothing at its edges and every
    # catchment larger than its span (246 km) is truncated to zero upstream
    # area -- pipeline.HYDROLOGY_RESIDUALS #2. Built BEFORE the level loop so
    # the loop below stays a plain "parent is the level above".
    # getattr, like allow_incomplete_superblock below: _run_bake is called with
    # hand-built argument objects by tools outside this module, and a new flag
    # must not break them.
    model_parent = None
    if getattr(args, "bake_model_parent", False):
        top = levels[0]
        model_parent = _build_model_parent(
            args, provider, geom, consts, kernels, cache,
            {bp.superblock_index(x, y, top) for (x, y) in ring},
        )
        if model_parent is None:
            return 1

    for level in levels:
        needed = {bp.superblock_index(x, y, level) for (x, y) in ring}
        for sx, sy in sorted(needed):
            entry_mode = bp.ENTRY_FOOTPRINT
            parent = None
            if level.level < consts.superblock_max_level:
                up = bp.FlowLevel(level=level.level + 1, geom=geom, consts=consts)
                # The parent covering this block's own origin tile.
                ptx, pty = sx * level.tiles_per_side, sy * level.tiles_per_side
                parent = superblocks.get((up.level,) + bp.superblock_index(ptx, pty, up))
            elif model_parent is not None:
                parent = model_parent
                # The entry mode is a per-HOP choice because the ratio is. At
                # L1 -> L0 the footprint is 4x4 and the two modes barely
                # differ; at M -> L1 it is 64x64 and ENTRY_FOOTPRINT can put a
                # continental river 3.8 km inside the domain, possibly across a
                # divide. See pipeline.ENTRY_CROSSING.
                entry_mode = getattr(
                    args, "bake_model_entry_mode", bp.ENTRY_CROSSING
                )
            parent_fp = parent.inputs_fingerprint if parent is not None else b""

            blob = cache.get_flow(fine_provider_id, args.seed, level.level, sx, sy)
            sb = None
            if blob is not None:
                try:
                    sb, _ = bp.decode_flow_superblock(blob)
                except ValueError as e:
                    # A superblock is a DERIVED artifact, unlike a shipped
                    # tile: an unreadable one is rebuilt rather than fatal.
                    print(
                        f"  warning: cached flow superblock L{level.level} "
                        f"({sx},{sy}) is unreadable ({e}); rebuilding",
                        file=sys.stderr,
                    )
                    sb = None
            if sb is not None and getattr(args, "bake_rebuild_superblocks", False):
                # A cached block is normally REUSED even when its fingerprint
                # says the world moved, because it is what the tiles already
                # baked against (ORDER_DEPENDENCE). That policy makes an A/B
                # impossible: turning the model parent on changes only the
                # fingerprint, so the parentless block would be reused and the
                # experiment would silently measure nothing.
                sb = None
            if sb is not None and verify_superblocks:
                # THE ORDER-DEPENDENCE CHECK (pipeline.ORDER_DEPENDENCE). A
                # cached superblock was built from whatever coarse tiles
                # existed then; recomputing the digest from the tiles that
                # exist NOW is the only way to see that it froze a smaller
                # world. Reusing it is still correct -- it is what the tiles
                # already baked against -- but it must not be silent.
                now_fp = bp.superblock_inputs_fingerprint(
                    lambda x, y: fetch(x, y, generate=False),
                    sx,
                    sy,
                    level,
                    parent_fingerprint=parent_fp,
                    entry_mode=entry_mode,
                )
                if now_fp != sb.inputs_fingerprint:
                    stale_superblocks += 1
                    print(
                        f"  warning: flow superblock L{level.level} ({sx},{sy}) "
                        f"was built against a DIFFERENT coarse world "
                        f"(stored {sb.fingerprint_hex[:16]}, now "
                        f"{now_fp.hex()[:16]}). Tiles already baked against it "
                        "are frozen to the older one and are never "
                        "regenerated; new tiles here inherit that choice. See "
                        "pipeline.ORDER_DEPENDENCE.",
                        file=sys.stderr,
                    )
            if sb is None:
                sb = bp.build_flow_superblock(
                    lambda x, y: fetch(x, y, generate=False),
                    sx,
                    sy,
                    level,
                    kernels,
                    parent=parent,
                    entry_mode=entry_mode,
                )
                cache.put_flow(
                    fine_provider_id,
                    args.seed,
                    level.level,
                    sx,
                    sy,
                    bp.encode_flow_superblock(sb, args.seed),
                )
            if not sb.complete:
                incomplete_superblocks += 1
                print(
                    f"  warning: flow superblock L{level.level} ({sx},{sy}) is "
                    f"INCOMPLETE ({len(sb.missing_tiles)} of "
                    f"{level.tiles_per_side ** 2} coarse tiles absent). Rivers "
                    "entering from those tiles contribute nothing, permanently, "
                    "to every tile baked against it.",
                    file=sys.stderr,
                )
            superblocks[(level.level, sx, sy)] = sb
        fps = ",".join(
            sorted(
                superblocks[(level.level, sx, sy)].fingerprint_hex[:12]
                for sx, sy in needed
            )
        )
        print(
            f"[flow L{level.level}] {len(needed)} superblock(s) "
            f"@ {level.cell_m:.0f} m/px, span {level.span_m / 1000:.0f} km  "
            f"fp={fps}  cpu={time.process_time() - cpu0:.1f}s",
            file=sys.stderr,
        )

    # -- pass 3: the bakes.
    allow_incomplete = bool(getattr(args, "allow_incomplete_superblock", False))
    codec = _resolve_codec(getattr(args, "codec", "raw"))
    level0 = bp.FlowLevel(level=0, geom=geom, consts=consts)
    baked = skipped = failed = 0
    npz_dir = Path(args.bake_npz_dir) if args.bake_npz_dir else None
    if npz_dir:
        npz_dir.mkdir(parents=True, exist_ok=True)

    for i, (x, y) in enumerate(targets):
        if cache.get_fine(fine_provider_id, args.seed, x, y) is not None:
            skipped += 1
            continue
        sb = superblocks.get((0,) + bp.superblock_index(x, y, level0))
        t0 = time.process_time()
        result = bp.bake_tile(
            world_seed=args.seed,
            tile_x=x,
            tile_y=y,
            coarse_fetch=lambda cx, cy: fetch(cx, cy, generate=False),
            climate_fetch=fetch_climate,
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
        publishable, gate_msg = superblock_gate_verdict(
            result.stats["superblock_missing_tiles"], allow_incomplete
        )
        if gate_msg:
            print(f"  {gate_msg.format(x=x, y=y)}", file=sys.stderr)
        if not publishable:
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
            encoded = _encode_fine(
                result, args.seed, fine_provider_id, codec=codec
            )
        except NotImplementedError as e:
            print(f"error: {e}", file=sys.stderr)
            failed += 1
            if npz_dir is None:
                return 1
            continue
        cache.put_fine(fine_provider_id, args.seed, x, y, encoded)
        baked += 1
        print(
            f"[{i + 1}/{len(targets)}] baked ({x},{y}) cpu={cpu:.1f}s "
            f"max_catchment={result.stats['max_accumulation_km2']:.1f}km2 "
            f"incision_p99={result.stats['incision_p99_m']:.2f}m "
            f"channels={int(result.stats['channel_cells'])} "
            f"injected={result.stats['injected_inflow_km2']:.1f}km2 "
            f"basin={result.stats['basin_cells_frac']*100:.1f}%/"
            f"{result.stats['basin_max_depth_m']:.0f}m "
            f"hydro={result.superblock_fingerprint[:12] or 'none'}",
            file=sys.stderr,
        )

    print(
        f"Bake complete: baked={baked} skipped={skipped} unencoded={failed} "
        f"total={len(targets)} incomplete_superblocks={incomplete_superblocks} "
        f"stale_superblocks={stale_superblocks} "
        f"cpu_seconds={time.process_time() - cpu0:.1f}",
        file=sys.stderr,
    )
    return 0 if failed == 0 else 1


def _resolve_codec(name: str) -> int | None:
    """``--codec`` name -> ``tile_codec`` constant, refusing early if unusable.

    WHY THIS EXISTS. ``_encode_fine`` has accepted a ``codec`` argument since
    2026-08-01, but no caller ever supplied one, so the encoder's own
    ``CODEC_RAW`` default won every time: **every fine tile pregen has ever
    written is uncompressed**, including all 17 in the cache today. The library
    default is right for the library -- CODEC_RAW must never depend on an
    optional package, and CI deliberately does not install ``zstandard`` -- but
    it left the production path with no way to reach compression at all.

    It matters because the ratio is large and already measured: 201.4 MB RAW
    against 33.4 MB CODEC_ZSTD on tile (-5,2), 6.0x, elevation and flow planes
    bit-identical on round trip. Across a world that is the difference between
    ~58 GB and ~9.7 GB of storage and of wire.

    ``auto`` is deliberately NOT offered. A flag that silently degrades to RAW
    when ``zstandard`` is missing would write uncompressed tiles into a cache
    whose operator believes they are compressed, and the size only shows up
    later as a bandwidth bill. Ask for zstd and not have it: fail here.
    """
    from . import tile_codec as tc

    name = (name or "raw").lower()
    if name == "raw":
        return tc.CODEC_RAW
    if name == "zstd":
        if not tc.HAVE_ZSTD:
            raise SystemExit(
                "error: --codec zstd needs the 'zstandard' package, which is "
                "not installed here. Install it, or pass --codec raw. Refusing "
                "to silently write uncompressed tiles under a compressed flag."
            )
        return tc.CODEC_ZSTD
    raise SystemExit(f"error: unknown --codec {name!r} (want 'raw' or 'zstd')")


def superblock_gate_verdict(
    missing_tiles: float, allow_incomplete: bool
) -> tuple[bool, str]:
    """May a tile baked against this superblock be PUBLISHED?

    ``missing_tiles`` is ``BakeResult.stats["superblock_missing_tiles"]``:
    ``0`` complete, ``> 0`` that many coarse tiles absent, ``< 0`` no superblock
    at all. Returns ``(publishable, message)``; the message takes ``{x}``/``{y}``.

    WHY THIS IS A HARD GATE AND NOT A WARNING.

    Stream-power incision scales with discharge, and discharge is the whole
    upstream catchment -- which routinely lies outside the tile. A superblock
    supplies that context. When it is incomplete, rivers entering from the
    absent tiles contribute nothing, so the tile is carved by less water than
    really flows through it.

    THE ELEVATION ARGUMENT FOR THIS GATE WAS MEASURED AND IS FALSE. An earlier
    version of this docstring claimed "you do not get wrong water, you get wrong
    mountains". Tested by baking a tile with and without its injected inflow:
    ZERO elevation cells moved past the 100 mm wire LSB, and 5 of 67M flow cells
    changed. Do not restore that claim, and do not justify this gate on visible
    terrain damage -- if someone re-measures and finds the same nulls, the gate
    will look unfounded and get removed.

    The gate stands on DETERMINISM instead, which the same test confirms: those
    5 cells are 5 cells of disagreement between two clients that baked the same
    coordinates against different neighbour sets, and it never heals. ``pipeline.ORDER_DEPENDENCE`` is explicit: "A tile baked
    against an incomplete superblock stays baked that way", because a shipped
    tile is never regenerated. So the defect is permanent in a way an ordinary
    warning is not, and it was previously emitted as a warning that pregen then
    ignored -- the 2026-08-02 world was baked with "INCOMPLETE (102 of 256
    coarse tiles absent)" scrolling past.

    In MULTIPLAYER it is worse than a quality problem. Terrain must be identical
    on every client, and a frontier tile baked against a thin superblock differs
    from the same coordinates baked later against a full one. Two players then
    disagree about the ground they are standing on and building on. Because
    ``waterca`` re-simulates from terrain, their water desyncs too.

    ``--allow-incomplete-superblock`` exists because development needs to bake
    single tiles without first generating 256 coarse neighbours. It must stay
    OFF for anything a player will ever see.
    """
    n = int(missing_tiles)
    if n == 0:
        return True, ""
    what = (
        "NO flow superblock at all"
        if n < 0
        else f"an INCOMPLETE flow superblock ({n} coarse tiles absent)"
    )
    if allow_incomplete:
        return True, (
            f"warning: tile ({{x}},{{y}}) baked against {what}; its river "
            "network is frozen to this exploration order and the tile is never "
            "regenerated (pipeline.ORDER_DEPENDENCE). Published anyway because "
            "--allow-incomplete-superblock was passed -- do NOT ship this tile."
        )
    return False, (
        f"error: tile ({{x}},{{y}}) baked against {what}. Its upstream "
        "catchment is truncated, so its routing depends on which neighbours "
        "happened to exist, and a shipped tile is never regenerated -- two "
        "clients would disagree about this ground forever -- refusing to "
        "publish it. Generate the missing coarse tiles and re-run, or pass "
        "--allow-incomplete-superblock for a throwaway development bake."
    )


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
        "--bake-model-parent",
        action="store_true",
        help=(
            "Bake mode: give the TOP flow superblock level a parent built from "
            "the diffusion model's own coarse stage (7.68 km/px), so a "
            "catchment larger than that level's 246 km span is no longer "
            "truncated to zero upstream area (pipeline.HYDROLOGY_RESIDUALS "
            "#2). Costs one coarse-stage inference over "
            "--bake-model-window^2 cells and ZERO coarse tiles. Needs "
            "--provider diffusion against a real checkpoint. OPT-IN: it "
            "changes every top-level superblock's inputs fingerprint, hence "
            "the rivers, hence the terrain."
        ),
    )
    parser.add_argument(
        "--bake-model-window",
        type=int,
        default=512,
        help=(
            "Bake mode: edge of the model coarse-map window, in coarse cells "
            "(7.68 km each). This is the largest catchment the pyramid can "
            "resolve: 512 spans 3,932 km (Mississippi-scale) for one "
            "coarse-stage inference. Must be a whole number of top-level "
            "superblocks (32 cells each at the default max level) so no block "
            "straddles two windows."
        ),
    )
    parser.add_argument(
        "--bake-model-entry-mode",
        choices=("footprint", "crossing"),
        default="crossing",
        help=(
            "Bake mode: where a model-parent cell's through-flow lands inside "
            "its child. 'footprint' is the pyramid's historical rule (lowest "
            "cell anywhere in the parent cell's footprint) and is fine at the "
            "4x4 ratio between tile-backed levels; at the model parent's 64x64 "
            "it can deposit a continental river ~3.8 km INTO the domain, "
            "possibly across a divide. 'crossing' (default) restricts the "
            "search to the face the flow actually crosses. Affects the M -> "
            "top hop only; see pipeline.ENTRY_CROSSING."
        ),
    )
    parser.add_argument(
        "--bake-rebuild-superblocks",
        action="store_true",
        help=(
            "Bake mode: rebuild every flow superblock, ignoring cached ones. "
            "Normally a cached block is reused even when its fingerprint says "
            "the world has moved, because it is what the tiles already baked "
            "against (pipeline.ORDER_DEPENDENCE) -- which makes an A/B of a "
            "hydrology change impossible, since the 'after' arm would silently "
            "reuse the 'before' block. Use for experiments, in a scratch "
            "--cache-dir."
        ),
    )
    parser.add_argument(
        "--bake-no-verify-superblocks",
        action="store_true",
        help=(
            "Bake mode: skip the order-dependence check on CACHED flow "
            "superblocks. The check recomputes each cached superblock's "
            "inputs fingerprint from the coarse tiles that exist now and warns "
            "when it disagrees with the world the superblock was built "
            "against -- see pipeline.ORDER_DEPENDENCE. It costs one read+hash "
            "pass over the block's coarse tiles (16 MB at level 0, 256 MB at "
            "level 1); turn it off only when that I/O is the bottleneck and "
            "you already know the world is static."
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
            "DiffusionConfig.provider_id_override. It is NOT a way to resolve "
            "an identity mismatch: the world's own world-identity.json still "
            "records the checkpoint and conditioning hashes this run actually "
            "has, and still refuses when they disagree. Forcing two different "
            "generations into one namespace gives one world with a seam in it "
            "and no error anywhere -- see world_manifest.py."
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
    parser.add_argument(
        "--codec",
        choices=("raw", "zstd"),
        default="raw",
        help=(
            "Fine-tier block codec. 'raw' (default, and what every tile in "
            "existence was written with) never depends on a compression "
            "library. 'zstd' compresses ~6x (measured 201.4 -> 33.4 MB on tile "
            "(-5,2), planes bit-identical on round trip) and needs the "
            "'zstandard' package here plus a client that can decode it -- see "
            "tools/fetch-zstd.ps1. There is deliberately no 'auto': a flag "
            "that quietly fell back to raw would fill a cache with "
            "uncompressed tiles its operator believed were compressed."
        ),
    )
    parser.add_argument(
        "--allow-incomplete-superblock",
        action="store_true",
        help=(
            "DEVELOPMENT ONLY: publish fine tiles whose flow superblock is "
            "incomplete. Such a tile is carved by less water than really flows "
            "through it (its upstream catchment is truncated) and is never "
            "regenerated, so the defect is permanent -- and in multiplayer two "
            "players baking the same ground at different frontier sizes get "
            "different terrain. Use only for throwaway bakes; see "
            "pregen.superblock_gate_verdict."
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

    if not _record_identity(cache, provider, args.seed, provider.provider_id):
        return 1

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
