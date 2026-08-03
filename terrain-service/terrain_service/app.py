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
    TERRAIN_DIFFUSION_CHECKPOINT_ID
                       WHERE to load the checkpoint from (local snapshot
                       path). NOT part of provider_id -- identity is
                       content-addressed, see DiffusionConfig.
    TERRAIN_DIFFUSION_CHECKPOINT_LABEL
                       Human-readable checkpoint name (e.g.
                       terrain-diffusion-30m). Hashed into provider_id;
                       must not be a path.
    TERRAIN_DIFFUSION_CHECKPOINT_SHA256
                       Pin the checkpoint's content hash (doctrine §2.3: it
                       must be pinned before ANY real inference, or
                       verify_checkpoint_sha256 refuses).
    TERRAIN_DIFFUSION_CONDITIONING_DIGEST
                       Pin the conditioning rasters' content hash (WorldClim
                       bio + data/global/etopo_10m.tif -- they condition
                       generation, so different copies mean different
                       terrain). From compute_conditioning_digest().
    TERRAIN_DIFFUSION_VERSION
                       terrain-diffusion package version/commit, when known.
    TERRAIN_CONDITIONING_ROOT
                       Where the conditioning rasters live (default
                       data/global relative to CWD, matching upstream).
    TERRAIN_DIFFUSION_PROVIDER_ID_OVERRIDE
                       COMPATIBILITY ONLY: serve an existing cache namespace
                       verbatim (e.g. tiles generated under the v1 id).
                       Defeats every identity guarantee above -- see
                       DiffusionConfig.provider_id_override.

                       Leaving the pinning vars unset keeps DiffusionConfig's
                       UNPINNED/UNVERIFIED defaults, which is fine for
                       TERRAIN_DIFFUSION_DRY_RUN=1 but is refused for a real
                       (non-dry-run) call.

The first tile this server GENERATES for a world (as opposed to serves from
cache) writes or confirms that world's ``world-identity.json`` -- what
checkpoint, what conditioning bytes, what version made it. A request that
would add tiles under an identity the world disagrees with fails with 500
instead of appending them. See world_manifest.py.
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


#: env var -> DiffusionConfig field. Every identity-bearing field the server
#: can pin without a code change; see this module's docstring for what each
#: means and diffusion.py for why the load path is NOT among them.
_DIFFUSION_ENV_FIELDS = {
    "TERRAIN_DIFFUSION_CHECKPOINT_ID": "checkpoint_id",
    "TERRAIN_DIFFUSION_CHECKPOINT_LABEL": "checkpoint_label",
    "TERRAIN_DIFFUSION_CHECKPOINT_SHA256": "checkpoint_sha256",
    "TERRAIN_DIFFUSION_CONDITIONING_DIGEST": "conditioning_digest",
    "TERRAIN_DIFFUSION_VERSION": "terrain_diffusion_version",
    "TERRAIN_DIFFUSION_PROVIDER_ID_OVERRIDE": "provider_id_override",
}


def _diffusion_config_from_env() -> "DiffusionConfig | None":
    """Build a pinned DiffusionConfig from the environment, or None if none of
    the pinning vars are set (caller falls back to DiffusionProvider's own
    UNPINNED/UNVERIFIED default -- unchanged prior behavior, and still fine
    for TERRAIN_DIFFUSION_DRY_RUN=1)."""
    kwargs = {
        fieldname: os.environ[var]
        for var, fieldname in _DIFFUSION_ENV_FIELDS.items()
        if var in os.environ
    }
    if not kwargs:
        return None
    from .providers.diffusion import DiffusionConfig

    return DiffusionConfig(**kwargs)


def create_app(provider=None, cache: TileCache | None = None) -> Flask:
    app = Flask(__name__)
    provider = provider or _make_provider(
        os.environ.get("TERRAIN_PROVIDER", "synthetic"),
        dry_run=os.environ.get("TERRAIN_DIFFUSION_DRY_RUN", "") == "1",
        config=_diffusion_config_from_env(),
    )
    cache = cache or TileCache(os.environ.get("TERRAIN_CACHE_DIR", "tile-cache"))

    #: (provider_id, seed) -> the verdict from world_manifest, computed once.
    _identity_verdicts: dict[tuple[str, int], tuple[bool, str]] = {}

    def _world_identity(seed: int) -> tuple[bool, str]:
        key = (provider.provider_id, seed)
        if key not in _identity_verdicts:
            from .world_manifest import record_world_identity

            verdict = record_world_identity(
                cache, provider, seed, provider.provider_id
            )
            if verdict[1]:
                app.logger.warning("world identity: %s", verdict[1])
            _identity_verdicts[key] = verdict
        return _identity_verdicts[key]

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
            # ADDING to a world, which is the moment its identity has to be on
            # record and has to agree. This server is the "keeps generating for
            # years" half of docs/world-generation-architecture.md: the tile it
            # writes in 2027 must have come from the same inputs as the one it
            # wrote in 2026, and nothing else downstream can tell. Checked only
            # on the write path (a cache hit adds nothing to the world) and
            # memoised per (namespace, seed), so it costs one hash pass over
            # the conditioning rasters per seed per process.
            ok, msg = _world_identity(seed)
            if not ok:
                abort(500, msg)
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
