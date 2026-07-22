"""Flask tile API (plan §5 task 1): GET /tile?seed&x&y&scale -> binary tile.

Cache-first: every generated tile is written through the disk cache, and the
response is byte-identical forever for a given (provider, seed, x, y, scale).
Configuration via environment:

    TERRAIN_PROVIDER   synthetic (default) | diffusion
    TERRAIN_CACHE_DIR  cache root (default: ./tile-cache)
    TERRAIN_DIFFUSION_DRY_RUN
                       1 => diffusion provider runs in dry-run mode
                       (synthetic-fallback rasters through the real
                       config/adapter/validate path, no GPU needed).
                       See providers/diffusion.py and
                       docs/diffusion-bringup.md. Ignored for
                       TERRAIN_PROVIDER=synthetic.
    TERRAIN_DIFFUSION_CHECKPOINT_ID / TERRAIN_DIFFUSION_CHECKPOINT_SHA256
                       Pin the diffusion provider's DiffusionConfig (doctrine
                       §2.3: checkpoint sha256 must be pinned before ANY real
                       inference, or verify_checkpoint_sha256 refuses).
                       Leaving these unset keeps DiffusionConfig's UNPINNED
                       default, which is fine for TERRAIN_DIFFUSION_DRY_RUN=1
                       but is refused for a real (non-dry-run) call.
"""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

from flask import Flask, Response, abort, request

from . import tile_codec
from .cache import TileCache
from .providers.synthetic import SyntheticProvider

if TYPE_CHECKING:
    from .providers.diffusion import DiffusionConfig


def _make_provider(name: str, dry_run: bool = False, config: "DiffusionConfig | None" = None):
    if name == "synthetic":
        return SyntheticProvider()
    if name == "diffusion":
        from .providers.diffusion import DiffusionProvider

        # `config`, when given, is the CALLER's pinned DiffusionConfig
        # (checkpoint id/sha256/sampler/scale) -- passing it through here
        # (rather than letting DiffusionProvider fall back to its own
        # UNPINNED-default DiffusionConfig()) is exactly what closes the gap
        # documented in docs/pod-bringup-commands.md Block 5: a caller that
        # built a pinned config must not have it silently discarded.
        return DiffusionProvider(config=config, dry_run=dry_run)
    raise ValueError(f"unknown TERRAIN_PROVIDER {name!r}")


def _diffusion_config_from_env() -> "DiffusionConfig | None":
    """Build a pinned DiffusionConfig from TERRAIN_DIFFUSION_CHECKPOINT_ID /
    _SHA256 if either is set in the environment, else None (caller falls
    back to DiffusionProvider's own UNPINNED default -- unchanged prior
    behavior, and still fine for TERRAIN_DIFFUSION_DRY_RUN=1)."""
    checkpoint_id = os.environ.get("TERRAIN_DIFFUSION_CHECKPOINT_ID")
    checkpoint_sha256 = os.environ.get("TERRAIN_DIFFUSION_CHECKPOINT_SHA256")
    if checkpoint_id is None and checkpoint_sha256 is None:
        return None
    from .providers.diffusion import DiffusionConfig

    kwargs = {}
    if checkpoint_id is not None:
        kwargs["checkpoint_id"] = checkpoint_id
    if checkpoint_sha256 is not None:
        kwargs["checkpoint_sha256"] = checkpoint_sha256
    return DiffusionConfig(**kwargs)


def create_app(provider=None, cache: TileCache | None = None) -> Flask:
    app = Flask(__name__)
    provider = provider or _make_provider(
        os.environ.get("TERRAIN_PROVIDER", "synthetic"),
        dry_run=os.environ.get("TERRAIN_DIFFUSION_DRY_RUN", "") == "1",
        config=_diffusion_config_from_env(),
    )
    cache = cache or TileCache(os.environ.get("TERRAIN_CACHE_DIR", "tile-cache"))

    @app.get("/healthz")
    def healthz() -> dict:
        return {"ok": True, "provider": provider.provider_id}

    @app.get("/tile")
    def tile() -> Response:
        try:
            seed = int(request.args["seed"])
            x = int(request.args["x"])
            y = int(request.args["y"])
            scale = int(request.args.get("scale", "1"))
        except (KeyError, ValueError):
            abort(400, "required: integer seed, x, y (and optional scale)")
        if scale not in tile_codec.PIXEL_SIZE_MM:
            abort(400, f"scale must be one of {sorted(tile_codec.PIXEL_SIZE_MM)}")
        if not 0 <= seed < 2**64:
            abort(400, "seed must fit in u64")

        data = cache.get(provider.provider_id, seed, x, y, scale)
        if data is None:
            data = tile_codec.encode(provider.generate(seed, x, y, scale))
            cache.put(provider.provider_id, seed, x, y, scale, data)
        return Response(
            data,
            mimetype="application/octet-stream",
            headers={
                "X-Provider": provider.provider_id,
                "Cache-Control": "public, max-age=31536000, immutable",
            },
        )

    return app
