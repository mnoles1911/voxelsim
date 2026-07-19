# terrain-service

Tile generation + serving for Voxel Earth (plan §3.1 steps 1–2). Serves
512×512 tiles of int16 elevation + uint8×4 climate over
`GET /tile?seed&x&y&scale`, cache-first, via pluggable providers:

- **synthetic** (default): deterministic integer value-noise terrain. DEV
  ONLY — not canonical world data. Exists so the whole pipeline (codec, cache,
  API, voxel-core consumption) works without a GPU.
- **diffusion**: the canonical provider wrapping
  [terrain-diffusion](https://github.com/xandergos/terrain-diffusion).
  Currently a stub with a bring-up checklist in
  `terrain_service/providers/diffusion.py` — needs a CUDA machine (the
  project targets one rented 4090-class worker in production, plan §3.4).

## Run

```sh
python3 -m pip install -r requirements.txt
python3 -m pytest                                  # incl. golden-tile regression
python3 -m flask --app terrain_service.app:create_app run
curl -s "localhost:5000/tile?seed=1&x=0&y=0&scale=1" | xxd | head
```

Environment: `TERRAIN_PROVIDER` (`synthetic`|`diffusion`),
`TERRAIN_CACHE_DIR` (default `./tile-cache`).

## Contracts that matter

- Tile bytes for a given (provider_id, seed, x, y, scale) are **immutable
  forever** — the cache key embeds the provider version, and golden-sha256
  tests pin the synthetic output. Any output change requires a provider_id
  bump.
- Wire format is defined in `terrain_service/tile_codec.py` and mirrored by
  `voxel-core/include/voxelcore/tiles.h` (`PIXEL_SIZE_MM` ↔ `pixelSizeMm`).
- Scale semantics follow terrain-diffusion: scale 1 = 30 m/px, scale 8
  (supersampled) = 11.25 m/px.
