"""Regenerate the CODEC_ZSTD conformance fixtures from the CODEC_RAW goldens.

    python terrain-service/tools/make_v2_zstd_fixture.py

Every fixture it writes carries the SAME lattice (and flow plane) as the
CODEC_RAW golden beside it, block mode for block mode. That equivalence is the
actual proof the C++ side needs: decoding a zstd fixture must produce exactly
the digest the CODEC_RAW golden already produces, so a codec that quietly
changed a value would show up as a digest mismatch rather than as
plausible-looking terrain.

TWO fixtures, because they prove different things and neither substitutes.

`vxtl_v2_golden_zstd_512.vxtl` is framed by tools/vxtl_zstd_store (real zstd
frames built from Raw_Blocks, no compression library needed), so it can be
regenerated anywhere, its bytes are reproducible, and CI -- which installs no
compression library on purpose -- can decode it with the C++ test's own
~30-line Raw_Block reader. Read that module's header for why that is the right
trade. What it does NOT exercise is a single bit of entropy decoding: a
Raw_Block frame has no Huffman literals and no FSE sequences.

`vxtl_v2_golden_zstd_real_512.vxtl` closes exactly that gap. It is the same
lattice compressed by a REAL libzstd at level 19, so its blocks are
Compressed_Blocks and reading it requires the whole entropy stage. It is
decoded by the opt-in `-DVXC_WITH_ZSTD=ON` build of vxc_tests and by nothing
else; the default build asserts only that its Raw_Block reader REFUSES the
frames, which is what proves the fixture really is entropy-coded rather than
quietly stored -- a check CI can make without owning a zstd.

Regenerating the real-zstd fixture needs `zstandard`; without it that half is
skipped and the committed file is left alone. The Raw_Block fixture never needs
it, and when it IS present this script additionally verifies with the real
decoder that every Raw_Block frame it wrote is conformant, which is what stops
"reproducible" drifting into "wrong".
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

# Same source golden, real entropy coding. Level 19 because that is the level
# every size number on record was measured at (tools/bake_real_tile.py), so the
# committed file's size is directly comparable to them.
REAL_PAIRS = [
    ("vxtl_v2_golden_512.vxtl", "vxtl_v2_golden_zstd_real_512.vxtl"),
]
REAL_LEVEL = 19


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


def frame_block_types(frame: bytes) -> list[int]:
    """Block_Type of every block in one zstd frame (RFC 8878 §3.1.1.2).

    Only the frame-header shapes this repo writes are walked -- Single_Segment
    with a 4-byte Frame_Content_Size, which is what both `zstd_store_frame` and
    `zstandard`'s one-shot compressor emit for these payload sizes. It exists to
    answer one question the fixture's SIZE cannot: are these frames actually
    entropy-coded (type 2, Compressed_Block), or merely stored (type 0)? A
    "real zstd" fixture that happened to store its blocks would silently prove
    nothing more than the Raw_Block one already does.
    """
    if len(frame) < 9 or int.from_bytes(frame[:4], "little") != 0xFD2FB528:
        raise ValueError("not a zstd frame this walker understands")
    if frame[4] != 0xA0:
        raise ValueError(f"frame header descriptor 0x{frame[4]:02x} not walkable here")
    out, off = [], 9
    while True:
        header = int.from_bytes(frame[off:off + 3], "little")
        off += 3
        out.append((header >> 1) & 3)
        size = header >> 3
        off += size
        if header & 1:
            break
    return out


def convert(src: Path, dst: Path, compressor, decompressor, *, expect_compressed=False,
            note: str = "") -> None:
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
        compressor=compressor,
    )

    # Round-trip through our own decoder, then confirm the lattice is
    # IDENTICAL to the source golden's. Same bytes in the plane, different
    # codec on the wire — that is the whole claim being committed here.
    back = tc.decode_v2(out, decompressor=decompressor)
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

    # What kind of zstd blocks did we actually write? `expect_compressed` turns
    # the answer into a hard requirement, because "we used a real compressor" is
    # a claim about the OUTPUT, not about which library was called: a payload
    # zstd declines to compress comes back as stored blocks and would leave the
    # entropy path as untested as before.
    types = _all_block_types(out)
    kinds = {0: "raw", 1: "rle", 2: "compressed", 3: "reserved"}
    hist = ", ".join(f"{kinds[k]} {types.count(k)}" for k in sorted(set(types)))
    if expect_compressed and 2 not in types:
        raise SystemExit(
            f"{dst.name}: no Compressed_Block in any frame ({hist}); this fixture "
            "exists to exercise entropy decoding and would exercise none"
        )

    dst.write_bytes(out)
    print(
        f"{dst.name}: {len(out)} bytes (source {src.name} {len(data)} bytes, "
        f"{len(data) / len(out):.2f}x smaller){note}\n  zstd blocks: {hist}\n  {verdict}"
    )


def _section_table(data: bytes):
    off = tc._HEADER.size
    ext = tc._V2_EXT.unpack_from(data, off)
    n_sections = ext[-1]
    off += tc._V2_EXT.size
    return [
        tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
        for i in range(n_sections)
    ]


def _all_block_types(data: bytes) -> list[int]:
    """Block_Types across every frame in the file, both planes."""
    sec = {sid: (o, ln) for sid, o, ln in _section_table(data)}
    out: list[int] = []
    for index_id, data_id in ((tc.SECTION_ELEV_INDEX, tc.SECTION_ELEV_DATA),
                              (tc.SECTION_FLOW_INDEX, tc.SECTION_FLOW_DATA)):
        if index_id not in sec:
            continue
        ioff, ilen = sec[index_id]
        doff, _ = sec[data_id]
        for i in range(ilen // tc._BLOCK_ENTRY.size):
            off, comp_len, _mode, _cp, _rb, _pad = tc._BLOCK_ENTRY.unpack_from(
                data, ioff + i * tc._BLOCK_ENTRY.size
            )
            if comp_len:
                out += frame_block_types(data[doff + off:doff + off + comp_len])
    return out


def main() -> None:
    for src_name, dst_name in PAIRS:
        src = FIXTURES / src_name
        if not src.exists():
            print(f"SKIP {src_name}: not present")
            continue
        convert(src, FIXTURES / dst_name, zstd_store_frame, zstd_store_inflate)

    try:
        import zstandard
    except ImportError:
        print("SKIP the real-zstd fixtures: zstandard is not installed. The "
              "committed ones are left alone; they are only regenerated when the "
              "source golden changes.")
        return
    cctx = zstandard.ZstdCompressor(level=REAL_LEVEL, write_content_size=True)

    def real_compress(payload: bytes) -> bytes:
        return cctx.compress(payload)

    def real_inflate(frame: bytes, expected_len: int) -> bytes:
        return zstandard.ZstdDecompressor().decompress(frame, max_output_size=expected_len)

    for src_name, dst_name in REAL_PAIRS:
        src = FIXTURES / src_name
        if not src.exists():
            print(f"SKIP {src_name}: not present")
            continue
        convert(src, FIXTURES / dst_name, real_compress, real_inflate,
                expect_compressed=True,
                note=f"  [real libzstd {'.'.join(map(str, zstandard.ZSTD_VERSION))} "
                     f"level {REAL_LEVEL}]")


if __name__ == "__main__":
    main()
