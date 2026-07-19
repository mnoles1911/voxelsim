"""Tile provider interface."""

from __future__ import annotations

from typing import Protocol

from ..tile_codec import Tile


class TileProvider(Protocol):
    #: Stable identity+version string, part of the cache key. Bump the version
    #: suffix on ANY output-changing modification.
    provider_id: str

    def generate(self, seed: int, x: int, y: int, scale: int) -> Tile: ...
