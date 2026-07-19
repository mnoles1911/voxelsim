"""terrain-diffusion provider — the CANONICAL tile source (plan §3.1 step 2).

Wraps https://github.com/xandergos/terrain-diffusion as an async GPU worker.
Diffusion output is NOT bit-deterministic across GPUs (doctrine §2.3), which
is exactly why tiles are generated once, cached forever, and distributed as
data. This module is a stub until we bring it up on a CUDA machine; the
provider interface and cache key discipline are already final.

Bring-up checklist (GPU machine):
  1. pip install terrain-diffusion's requirements (needs CUDA torch).
  2. Load the 30m checkpoint; map our (seed, x, y, scale) to its sampling API
     with a fixed sampler config — record sampler config + checkpoint hash in
     provider_id so regenerated caches never mix.
  3. Convert its elevation output to int16 metres and climate to uint8x4 per
     tile_codec conventions.
  4. Golden-tile test: pin sha256 of one generated tile ON THE GENERATING
     MACHINE (cache-distribution makes cross-GPU drift a non-issue, but the
     same machine + checkpoint must reproduce).
"""

from __future__ import annotations

from ..tile_codec import Tile


class DiffusionProvider:
    provider_id = "terrain-diffusion-UNPINNED"  # pin checkpoint hash at bring-up

    def __init__(self) -> None:
        raise NotImplementedError(
            "terrain-diffusion worker not wired up yet — needs a CUDA machine. "
            "Use TERRAIN_PROVIDER=synthetic for development, and see this "
            "module's docstring for the bring-up checklist."
        )

    def generate(self, seed: int, x: int, y: int, scale: int) -> Tile:
        raise NotImplementedError
