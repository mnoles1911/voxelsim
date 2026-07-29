"""Regenerate the CODEC_ZSTD conformance fixtures from the CODEC_RAW goldens.

    python terrain-service/tools/make_v2_zstd_fixture.py

The two fixtures it writes carry the SAME lattice (and flow plane) as the
CODEC_RAW goldens beside them, block mode for block mode. That equivalence is
the actual proof the C++ side needs: decoding the zstd fixture must produce
exactly the digest the CODEC_RAW golden already produces, so a codec that
quietly changed a value would show up as a digest mismatch rather than as
plausible-looking terrain.

Frames are written by tools/vxtl_zstd_store (real zstd frames built from
Raw_Blocks, no compression library needed) so this runs anywhere and the
committed bytes are reproducible byte-for-byte. Read that module's header for
why that is the right trade here; if `zstandard` happens to be installed, this
script additionally verifies with the REAL decoder that every frame it wrote is
conformant, which is what stops "reproducible" drifting into "wrong".
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "terrain-service"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

from terrain_service import tile_codec as tc  # noqa: E402
from vxtl_zstd_store import zstd_store_frame, zstd_store_inflate  # noqa: E402

FIXTURES = REPO / "voxel-core" / "tests" / "fixtures"

# (source CODEC_RAW golden, CODEC_ZSTD fixture to write)
#
# ONLY the elevation golden is mirrored, on purpose. A Raw_Block frame is the
# same size as its payload, so a CODEC_ZSTD twin of the flow golden would add
# another 850 KB of committed binary to prove what the hand-built C++ bytes and
# the Python round-trip already prove about the u8 plane. The elevation twin is
# the one worth its weight: it shares the CODEC_RAW golden's whole-lattice
# digest, and its CODED/32 block is a 256 KB payload, so its frame necessarily
# spans several zstd blocks — the case a "one block per frame" reader passes
# everything else on and fails here.
PAIRS = [
    ("vxtl_v2_golden_512.vxtl", "vxtl_v2_golden_zstd_512.vxtl"),
]


def _forced_raw_blocks(data: bytes, section_id: int, n_sections: int, nb: int):
    """Which (bx, by) blocks the source file stored as MODE_RAW.

    Re-encoding has to reproduce them explicitly: the reference encoder never
    auto-selects RAW (see _encode_plane's docstring), so without this the RAW
    block would come back CODED and the fixture would stop covering the mode
    it exists to cover.
    """
    off = tc._HEADER.size + tc._V2_EXT.size
    table = [
        tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
        for i in range(n_sections)
    ]
    index_off = next(o for sid, o, _ in table if sid == section_id)
    out = set()
    for i in range(nb * nb):
        entry = tc._BLOCK_ENTRY.unpack_from(data, index_off + i * tc._BLOCK_ENTRY.size)
        if entry[2] == tc.MODE_RAW:
            by, bx = divmod(i, nb)
            out.add((bx, by))
    return out


def convert(src: Path, dst: Path) -> None:
    data = src.read_bytes()
    tile = tc.decode_v2(data)
    nb = tile.size // (1 << tile.block_log2)
    n_sections = 4 if tile.flow is not None else 2

    raw_blocks = _forced_raw_blocks(data, tc.SECTION_ELEV_INDEX, n_sections, nb)
    flow_raw_blocks = (
        _forced_raw_blocks(data, tc.SECTION_FLOW_INDEX, n_sections, nb)
        if tile.flow is not None
        else None
    )

    tile.codec = tc.CODEC_ZSTD
    out = tc.encode_v2(
        tile,
        raw_blocks=raw_blocks,
        flow_raw_blocks=flow_raw_blocks,
        compressor=zstd_store_frame,
    )

    # Round-trip through our own decoder, then confirm the lattice is
    # IDENTICAL to the source golden's. Same bytes in the plane, different
    # codec on the wire — that is the whole claim being committed here.
    back = tc.decode_v2(out, decompressor=zstd_store_inflate)
    np.testing.assert_array_equal(back.elevation_cp, tile.elevation_cp)
    if tile.flow is not None:
        np.testing.assert_array_equal(back.flow, tile.flow)

    try:
        import zstandard  # noqa: F401

        real = tc.decode_v2(out)  # no injected decompressor: uses real zstd
        np.testing.assert_array_equal(real.elevation_cp, tile.elevation_cp)
        if tile.flow is not None:
            np.testing.assert_array_equal(real.flow, tile.flow)
        verdict = "verified against the REAL zstandard decoder"
    except ImportError:
        verdict = "zstandard not installed: frame conformance NOT independently verified"

    dst.write_bytes(out)
    print(
        f"{dst.name}: {len(out)} bytes (source {src.name} {len(data)} bytes, "
        f"{len(data) / len(out):.2f}x smaller)\n  {verdict}"
    )


def main() -> None:
    for src_name, dst_name in PAIRS:
        src = FIXTURES / src_name
        if not src.exists():
            print(f"SKIP {src_name}: not present")
            continue
        convert(src, FIXTURES / dst_name)


if __name__ == "__main__":
    main()
