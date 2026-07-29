"""Content-addressed disk cache for tiles (plan §3.1 step 2).

Tiles are generated once and cached forever; the cache key includes the
provider identity+version so switching providers (synthetic dev tiles vs the
real diffusion worker) can never serve stale bytes. Writes are atomic
(tmp + rename) so a crashed worker never leaves a torn tile.

Three artifact kinds share one layout, all keyed by ``provider_id`` first:

    <root>/<provider_id>/<seed:016x>/s1/<x>_<y>.vxtl      coarse tile, 30 m/px
    <root>/<provider_id>/<seed:016x>/s16/<x>_<y>.vxtl     FINE TIER, 1.875 m/px
    <root>/<provider_id>/<seed:016x>/flow<L>/<sx>_<sy>.vxfl
                                                          flow superblock, level L

The fine tier reuses the scale slot rather than inventing a new axis: the
.vxtl v2 container is one fine tier per COARSE tile coordinate at scale 16
(docs/vxtl-v2-format.md section 1), so ``s16/<x>_<y>.vxtl`` addresses it with
the coarse (x, y) and no new path grammar. That also means the existing
``path``/``get``/``put`` already work for it; ``fine_path``/``get_fine``/
``put_fine`` exist so callers state which tier they mean instead of passing a
bare 16.

Flow superblocks (``terrain_service.bake.pipeline.FlowSuperblock``) are the
hydrology pyramid's cached intermediates: 30 m accumulation over a
world-anchored block of coarse tiles, plus the coarser levels above it. They
are keyed by SUPERBLOCK index, not tile index -- that is the point of them.
Two adjacent tiles that share an edge read the SAME superblock bytes, so they
cannot disagree about how much upstream area crosses that edge; a per-tile
neighbourhood computed on the fly would give each side its own answer.

Being under ``provider_id`` matters for the same reason it matters for tiles:
``provider_id`` now covers the bake version and constants (see
``providers/diffusion.py::_tile_format_fingerprint``), so a bake change lands
in a fresh namespace and can never mix with superblocks built by the old one.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

#: The scale slot the baked fine tier occupies. docs/vxtl-v2-format.md: one
#: fine container per coarse tile coordinate, 8192x8192 at 1.875 m/px.
FINE_SCALE = 16

#: Filename suffix for flow superblocks. Deliberately not ``.vxtl`` -- nothing
#: in voxel-core parses these, they are a server-side intermediate.
FLOW_SUFFIX = ".vxfl"


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


class TileCache:
    def __init__(self, root: str | os.PathLike[str]):
        self.root = Path(root)

    # -- coarse + fine tiles (both .vxtl, addressed by scale slot) ---------

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
        _atomic_write(self.path(provider_id, seed, x, y, scale), data)

    def fine_path(self, provider_id: str, seed: int, x: int, y: int) -> Path:
        """The baked fine tier for COARSE tile (x, y)."""
        return self.path(provider_id, seed, x, y, FINE_SCALE)

    def get_fine(self, provider_id: str, seed: int, x: int, y: int) -> bytes | None:
        return self.get(provider_id, seed, x, y, FINE_SCALE)

    def put_fine(self, provider_id: str, seed: int, x: int, y: int, data: bytes) -> None:
        self.put(provider_id, seed, x, y, FINE_SCALE, data)

    # -- flow superblocks --------------------------------------------------

    def flow_path(
        self, provider_id: str, seed: int, level: int, sx: int, sy: int
    ) -> Path:
        """One level-``level`` flow superblock at superblock index (sx, sy).

        (sx, sy) is a SUPERBLOCK index (``pipeline.superblock_index``), not a
        tile index, and the two grids are different at every level -- naming
        the level in the directory keeps that impossible to confuse.
        """
        return (
            self.root
            / provider_id
            / f"{seed:016x}"
            / f"flow{level}"
            / f"{sx}_{sy}{FLOW_SUFFIX}"
        )

    def get_flow(
        self, provider_id: str, seed: int, level: int, sx: int, sy: int
    ) -> bytes | None:
        try:
            return self.flow_path(provider_id, seed, level, sx, sy).read_bytes()
        except FileNotFoundError:
            return None

    def put_flow(
        self, provider_id: str, seed: int, level: int, sx: int, sy: int, data: bytes
    ) -> None:
        _atomic_write(self.flow_path(provider_id, seed, level, sx, sy), data)
