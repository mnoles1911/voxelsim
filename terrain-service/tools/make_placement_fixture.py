#!/usr/bin/env python3
"""Regenerate the cross-language placement-plane fixture (bake_ver 28).

    python tools/make_placement_fixture.py

Writes ``voxel-core/tests/fixtures/vxtl_v2_golden_placement_512.vxtl``: a
512-px v2 tile whose five SECTION_PLACE_* planes carry FORMULA-DEFINED values
the C++ test (test_tilestore.cpp, the placement fixture tests) recomputes
independently -- same posture as make_basin_fixture.py: the two halves share
the format document and no code.

The plane content exercises every mode the decoder must handle: a CONSTANT
region (the whole talus plane is zero), CODED regions (the smooth gradient
planes), and values at both ends of each encoding (a wet pixel at distance 0,
the saturated/unknown 255, curvature both sides of 128).
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

OUT = (Path(__file__).resolve().parents[2] / "voxel-core" / "tests" / "fixtures"
       / "vxtl_v2_golden_placement_512.vxtl")

SIZE = 512
SUB = SIZE // tc.PLACEMENT_SUBSAMPLE  # 128


def planes() -> dict[str, np.ndarray]:
    """The normative fixture content. Mirrored in the C++ test BY FORMULA."""
    y, x = np.mgrid[0:SUB, 0:SUB]
    return {
        # A shoreline along x = 16: wet (0) to the left, 2 m steps rightward,
        # saturating to the unknown code far from it.
        "place_dist_water": np.minimum(np.maximum(x - 16, 0) * 3, 255).astype(np.uint8),
        # A smooth diagonal moisture gradient, 255 (unknown) in one corner
        # block so the sentinel is exercised on the wire.
        "place_twi": np.where((x >= 112) & (y >= 112), 255,
                              np.minimum((x + y) // 2, 254)).astype(np.uint8),
        # All-zero: the whole plane is one CONSTANT block (or a few), the
        # commonest real-world case for a tile with no cliffs.
        "place_talus": np.zeros((SUB, SUB), np.uint8),
        # Both sides of flat 128.
        "place_curv": np.clip(128 + (x.astype(np.int32) - y.astype(np.int32)) % 64 - 32,
                              0, 255).astype(np.uint8),
        # A hillshade-ish sweep over the full range.
        "place_heat": ((x * 2 + y) % 255).astype(np.uint8),
    }


def main() -> int:
    elev = ((np.arange(SIZE)[:, None] + np.arange(SIZE)[None, :]) % 200 - 100).astype(np.int16)
    blob = tc.encode_v2(tc.TileV2(
        seed=20260719, x=-7, y=3, size=SIZE, elevation_cp=elev,
        base_offset_mm=1_000_000, bake_ver=28, **planes(),
    ))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    print(f"wrote {OUT} ({len(blob)} bytes)")
    d = tc.decode_v2(blob)
    for name, p in planes().items():
        assert np.array_equal(getattr(d, name), p), name
    print("python round-trip verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
