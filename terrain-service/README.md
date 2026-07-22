# terrain-service

Tile generation + serving for Voxel Earth (plan §3.1 steps 1–2). Serves
512×512 tiles of int16 elevation + uint8×4 climate over
`GET /tile?seed&x&y&scale`, cache-first, via pluggable providers:

- **synthetic** (default): deterministic integer value-noise terrain. DEV
  ONLY — not canonical world data. Exists so the whole pipeline (codec, cache,
  API, voxel-core consumption) works without a GPU.
- **diffusion**: the canonical provider wrapping
  [terrain-diffusion](https://github.com/xandergos/terrain-diffusion).
  Config/adapter/validation are fully implemented and tested in
  `terrain_service/providers/diffusion.py`; only the actual model call
  (`DiffusionProvider._call_model`) needs a CUDA machine (the project
  targets one rented 4090-class worker in production, plan §3.4). Run with
  `dry_run=True` (or `TERRAIN_DIFFUSION_DRY_RUN=1` / `pregen --dry-run`) to
  exercise the full config→adapter→validate→encode path with
  synthetic-fallback rasters, no GPU needed. See
  `terrain-service/docs/diffusion-bringup.md` for the cloud bring-up
  runbook.

## Run

```sh
python3 -m pip install -r requirements.txt
python3 -m pytest                                  # incl. golden-tile regression
python3 -m flask --app terrain_service.app:create_app run
curl -s "localhost:5000/tile?seed=1&x=0&y=0&scale=1" | xxd | head
```

Environment: `TERRAIN_PROVIDER` (`synthetic`|`diffusion`),
`TERRAIN_CACHE_DIR` (default `./tile-cache`), `TERRAIN_DIFFUSION_DRY_RUN`
(`1` => diffusion provider runs in dry-run/synthetic-fallback mode, no GPU
needed).

## Pre-generate tiles offline

Pre-generate tiles for a launch radius to warm the cache before deployment
(plan §3.4: "Pre-generate launch radius offline"):

```sh
python3 -m terrain_service.pregen --seed 42 --radius 8 --cache-dir ./tile-cache
python3 -m terrain_service.pregen --seed 42 --radius 8 --center-x 100 --center-y -50 --scale 1
python3 -m terrain_service.pregen --help
```

For each tile in the (2*radius+1)² square around the center point, the tool
skips if already cached, else generates+encodes+caches. Prints progress every
10 tiles.

## Docker

Build and run the service in a container:

```sh
docker build -t terrain-service terrain-service/
docker run -p 8000:8000 -v tile-cache:/data/tile-cache terrain-service

# Pre-generate tiles in container
docker run -v tile-cache:/data/tile-cache terrain-service \
  python -m terrain_service.pregen --seed 42 --radius 8

# Fetch a tile via the running server
curl -s "localhost:8000/tile?seed=42&x=0&y=0&scale=1" | xxd | head
```

`Dockerfile.diffusion` is a separate CUDA-base image for the real
terrain-diffusion GPU worker (unbuildable without GPU cloud network access
here — build it on the rented GPU box). See `docs/diffusion-bringup.md`.

## Contracts that matter

- Tile bytes for a given (provider_id, seed, x, y, scale) are **immutable
  forever** — the cache key embeds the provider version, and golden-sha256
  tests pin the synthetic output. Any output change requires a provider_id
  bump.
- Wire format is defined in `terrain_service/tile_codec.py` and mirrored by
  `voxel-core/include/voxelcore/tiles.h` (`PIXEL_SIZE_MM` ↔ `pixelSizeMm`).
- Scale semantics follow terrain-diffusion: scale 1 = 30 m/px, scale 8
  (supersampled) = 3.75 m/px (30 m / 8).
