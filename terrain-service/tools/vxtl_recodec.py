"""Re-frame a `.vxtl` v2 tile's block payloads under a different codec (§3).

    python terrain-service/tools/vxtl_recodec.py IN.vxtl --out OUT.vxtl [--level 19]

WHY THIS IS A CONTAINER OPERATION AND NOT A RE-ENCODE
-----------------------------------------------------
`codec` in docs/vxtl-v2-format.md §3 applies to BLOCK PAYLOADS and to nothing
else: the modes, the residual widths, the predictor and every reconstructed
control point are identical either side of it. So this tool never touches the
plane. It walks the §4 block index, replaces each block's stored bytes with the
same payload framed differently, and rewrites the offsets. Two consequences,
both of which are the point:

* **The values cannot change**, by construction rather than by test. There is
  no decode and no re-encode to get wrong; a CODED/32 block stays a CODED/32
  block with the same residuals in it. `--verify` (on by default) additionally
  decompresses every frame it wrote and asserts the payload came back byte for
  byte, which is a complete proof of value-equality without paying for the
  reference decoder's per-pixel MED inverse -- 67 M Python iterations per plane
  at production size, which is why `bake_real_tile.py` avoids it too.
* **The size difference is exactly the codec's**, so the MB/tile it prints is
  directly comparable to the zstd-19 numbers `bake_real_tile.py` records, which
  are measured the same way (compress the actual payloads, keep the container).

WHAT IT IS FOR
--------------
`bake_real_tile.py` writes CODEC_RAW because that is what a bake produces; the
fine tier ships CODEC_ZSTD. This is the missing step between them, and it is
also how a real production-size CODEC_ZSTD tile gets made for the C++ decoder to
read -- see `vxtl_v2_zstd_real_production_tile_equivalence` in
voxel-core/tests/test_tilestore.cpp, which decodes a RAW/ZSTD pair produced here
and digest-compares them. That pairing is the only end-to-end evidence that a
real 8192^2 entropy-coded tile decodes to the right numbers in the client's own
decoder; the committed 512-edge fixtures prove the format, not the scale.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from terrain_service import tile_codec as tc  # noqa: E402

_PLANES = (
    ("elev", tc.SECTION_ELEV_INDEX, tc.SECTION_ELEV_DATA),
    ("flow", tc.SECTION_FLOW_INDEX, tc.SECTION_FLOW_DATA),
)


def _read_container(data: bytes):
    """(header_bytes, ext_fields, {section_id: (offset, length)})."""
    magic, ver, _seed, _x, _y, _scale, _size = tc._HEADER.unpack_from(data, 0)
    if magic != b"VXTL":
        raise SystemExit("not a .vxtl file")
    if ver != 2:
        raise SystemExit(f"not a v2 tile (version {ver}); this tool only re-frames v2")
    ext = list(tc._V2_EXT.unpack_from(data, tc._HEADER.size))
    off = tc._HEADER.size + tc._V2_EXT.size
    table = {}
    order = []
    for i in range(ext[-1]):
        sid, soff, slen = tc._SECTION_ENTRY.unpack_from(data, off + i * tc._SECTION_ENTRY.size)
        table[sid] = (soff, slen)
        order.append(sid)
    return ext, table, order


def recodec(data: bytes, *, codec: int, compress, decompress=None):
    """Return (new file bytes, per-plane {payload_before, payload_after}).

    `compress` is payload -> stored bytes; None-safe identity is the caller's
    job. `decompress` is (stored, expected_len) -> payload and, when given, is
    used to verify every frame written round-trips byte for byte.
    """
    ext, table, order = _read_container(data)
    header = bytearray(data[: tc._HEADER.size + tc._V2_EXT.size])

    new_sections: dict[int, bytes] = {}
    report: dict[str, dict] = {}

    for name, index_id, data_id in _PLANES:
        if index_id not in table:
            continue
        ioff, ilen = table[index_id]
        doff, _dlen = table[data_id]
        n = ilen // tc._BLOCK_ENTRY.size
        index = bytearray()
        payloads = bytearray()
        before = after = 0
        for i in range(n):
            off, comp_len, mode, cp, rb, pad = tc._BLOCK_ENTRY.unpack_from(
                data, ioff + i * tc._BLOCK_ENTRY.size
            )
            if comp_len:
                payload = data[doff + off: doff + off + comp_len]
                stored = compress(payload)
                if decompress is not None:
                    back = decompress(stored, len(payload))
                    if back != payload:
                        raise SystemExit(
                            f"{name} block {i}: frame did not round-trip "
                            f"({len(back)} bytes back, {len(payload)} in)"
                        )
                before += len(payload)
                after += len(stored)
                new_off = len(payloads)
                payloads += stored
                index += tc._BLOCK_ENTRY.pack(new_off, len(stored), mode, cp, rb, pad)
            else:
                # CONSTANT: zero data bytes, and no frame to write (§4). The
                # offset is meaningless and is normalised to 0, as the encoder
                # writes it.
                index += tc._BLOCK_ENTRY.pack(0, 0, mode, cp, rb, pad)
        new_sections[index_id] = bytes(index)
        new_sections[data_id] = bytes(payloads)
        report[name] = {"blocks": n, "payload_before": before, "payload_after": after}

    # Re-lay the sections in their original order, back to back after the table.
    table_bytes = bytearray()
    body = bytearray()
    base = tc._HEADER.size + tc._V2_EXT.size + len(order) * tc._SECTION_ENTRY.size
    for sid in order:
        blob = new_sections[sid]
        table_bytes += tc._SECTION_ENTRY.pack(sid, base + len(body), len(blob))
        body += blob

    ext[3] = codec  # §3 `codec` is the 4th ext field: block_log2, predictor, quant, codec
    tc._V2_EXT.pack_into(header, tc._HEADER.size, *ext)
    return bytes(header) + bytes(table_bytes) + bytes(body), report


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", required=True)
    ap.add_argument("--level", type=int, default=19,
                    help="zstd level (default 19, the level every size number on "
                         "record was measured at)")
    ap.add_argument("--store", action="store_true",
                    help="write Raw_Block frames via tools/vxtl_zstd_store instead of "
                         "compressing. Real zstd frames, zero compression -- for "
                         "exercising the codec path with no compression library.")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip decompressing every frame back and comparing payloads")
    a = ap.parse_args()

    data = Path(a.input).read_bytes()
    if a.store:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from vxtl_zstd_store import zstd_store_frame, zstd_store_inflate

        compress, decompress, label = zstd_store_frame, zstd_store_inflate, "Raw_Block store"
    else:
        try:
            import zstandard
        except ImportError:
            raise SystemExit("zstandard is not installed; use --store, or pip install zstandard")
        cctx = zstandard.ZstdCompressor(level=a.level, write_content_size=True)
        dctx = zstandard.ZstdDecompressor()

        def compress(payload: bytes) -> bytes:
            return cctx.compress(payload)

        def decompress(frame: bytes, expected: int) -> bytes:
            return dctx.decompress(frame, max_output_size=expected)

        label = f"libzstd {'.'.join(map(str, zstandard.ZSTD_VERSION))} level {a.level}"

    out, report = recodec(
        data, codec=tc.CODEC_ZSTD, compress=compress,
        decompress=None if a.no_verify else decompress,
    )
    Path(a.out).write_bytes(out)

    _m, _v, _seed, tx, ty, _sc, size = tc._HEADER.unpack_from(data, 0)
    km2 = (size * tc.PIXEL_SIZE_MM[tc.FINE_SCALE] / 1e6) ** 2
    print(f"{a.input} -> {a.out}   tile ({tx},{ty}) {size}^2 over {km2:.1f} km2   [{label}]")
    tot_b = tot_a = 0
    for name, rep in report.items():
        tot_b += rep["payload_before"]
        tot_a += rep["payload_after"]
        ratio = rep["payload_before"] / max(rep["payload_after"], 1)
        print(f"  {name}: {rep['blocks']} blocks   payloads "
              f"{rep['payload_before']/1e6:8.2f} -> {rep['payload_after']/1e6:8.2f} MB   "
              f"{ratio:5.2f}x")
    print(f"  file:  {len(data)/1e6:8.2f} -> {len(out)/1e6:8.2f} MB   "
          f"{len(data)/max(len(out),1):5.2f}x   {len(out)/1024/km2:7.1f} KB/km2")
    if not a.no_verify:
        print("  every frame decompressed back to its payload byte for byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
