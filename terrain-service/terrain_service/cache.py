"""Content-addressed disk cache for tiles (plan §3.1 step 2).

Tiles are generated once and cached forever; the cache key includes the
provider identity+version so switching providers (synthetic dev tiles vs the
real diffusion worker) can never serve stale bytes. Writes are atomic
(tmp + rename) so a crashed worker never leaves a torn tile.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path


class TileCache:
    def __init__(self, root: str | os.PathLike[str]):
        self.root = Path(root)

    def path(self, provider_id: str, seed: int, x: int, y: int, scale: int) -> Path:
        return (
            self.root
            / provider_id
            / f"{seed:016x}"
            / f"s{scale}"
            / f"{x}_{y}.vxtl"
        )

    def get(self, provider_id: str, seed: int, x: int, y: int, scale: int) -> bytes | None:
        p = self.path(provider_id, seed, x, y, scale)
        try:
            return p.read_bytes()
        except FileNotFoundError:
            return None

    def put(
        self, provider_id: str, seed: int, x: int, y: int, scale: int, data: bytes
    ) -> None:
        p = self.path(provider_id, seed, x, y, scale)
        p.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=p.parent, suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as f:
                f.write(data)
            os.replace(tmp, p)
        except BaseException:
            os.unlink(tmp)
            raise
