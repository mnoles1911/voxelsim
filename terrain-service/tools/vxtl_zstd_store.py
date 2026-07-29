"""A dependency-free zstd frame writer/reader restricted to RAW blocks.

WHY THIS EXISTS, because "hand-rolled zstd" deserves an explanation.

`.vxtl` v2's CODEC_ZSTD (docs/vxtl-v2-format.md §3) is decoded on the client by
a decompressor INJECTED at the host boundary — voxel-core vendors no
third-party code, and the UE build must use the engine's own zstd rather than a
second static copy. That leaves one awkward gap in the conformance story:

  * CI deliberately does not install `zstandard` (see tile_codec.HAVE_ZSTD), so
    the committed CODEC_ZSTD conformance fixture cannot be *generated* there;
  * voxel-core's standalone test binary has no zstd at all, so it cannot
    *decode* an arbitrary compressed fixture either.

Both are solved by the same observation: zstd's frame format defines a
`Raw_Block` — literal, uncompressed bytes — and a frame made only of raw blocks
is a fully conformant zstd frame that any real zstd decoder reads correctly,
while being ~30 lines to write and ~30 lines to parse. So the committed fixture
is a real zstd stream, the C++ test's injected decompressor is a real (if
minimal) zstd reader, and neither side needs a compression library.

It compresses nothing, and it is not meant to: this file's job is to exercise
the INJECTION BOUNDARY and the length validation either side of it. The
production encoder still uses `zstandard` (tile_codec._compress); real
compression ratios are measured with that, not with this.

Reference: RFC 8878 §3.1.1 (frame header), §3.1.1.2 (blocks).
"""

from __future__ import annotations

import struct

ZSTD_MAGIC = 0xFD2FB528

#: Frame_Header_Descriptor: Frame_Content_Size_flag = 2 (4-byte FCS, value
#: stored directly), Single_Segment_flag = 1 (no Window_Descriptor byte;
#: Window_Size == Frame_Content_Size), no dictionary id, no content checksum.
FRAME_HEADER_DESCRIPTOR = 0xA0

#: Block_Maximum_Size is min(Window_Size, 128 KB) (RFC 8878 §3.1.1.2). A
#: 256x256 block at 4 bytes/px is 256 KB, so payloads DO span several blocks —
#: which is worth having in the fixture, since a reader that assumes one block
#: per frame passes on the small blocks and fails on the wide-residual one.
BLOCK_MAX = 128 * 1024

_RAW_BLOCK = 0  # Block_Type


def zstd_store_frame(payload: bytes) -> bytes:
    """Wrap `payload` in a conformant zstd frame of Raw_Blocks."""
    out = bytearray()
    out += struct.pack("<I", ZSTD_MAGIC)
    out.append(FRAME_HEADER_DESCRIPTOR)
    out += struct.pack("<I", len(payload))

    i = 0
    while True:
        chunk = payload[i:i + BLOCK_MAX]
        i += len(chunk)
        last = 1 if i >= len(payload) else 0
        header = last | (_RAW_BLOCK << 1) | (len(chunk) << 3)
        out += header.to_bytes(3, "little")
        out += chunk
        if last:
            break
    return bytes(out)


def zstd_store_inflate(frame: bytes, expected_len: int) -> bytes:
    """Inverse of zstd_store_frame, with the same strictness the C++ side has.

    Rejects — rather than best-effort reads — anything outside the subset this
    module writes, and rejects a frame whose content is not exactly
    `expected_len` bytes. That exactness is the whole point: under CODEC_ZSTD
    `comp_len` is the compressed size and constrains nothing, so this is the
    only length check standing between a corrupt frame and plausible terrain.
    """
    if len(frame) < 9:
        raise ValueError("zstd frame too short")
    magic, descriptor, fcs = struct.unpack_from("<IBI", frame, 0)
    if magic != ZSTD_MAGIC:
        raise ValueError("not a zstd frame")
    if descriptor != FRAME_HEADER_DESCRIPTOR:
        raise ValueError(
            f"frame header descriptor 0x{descriptor:02x} outside the Raw_Block subset"
        )
    if fcs != expected_len:
        raise ValueError(f"frame declares {fcs} bytes, header implies {expected_len}")

    out = bytearray()
    off = 9
    while True:
        if off + 3 > len(frame):
            raise ValueError("truncated block header")
        header = int.from_bytes(frame[off:off + 3], "little")
        off += 3
        last = header & 1
        block_type = (header >> 1) & 3
        size = header >> 3
        if block_type != _RAW_BLOCK:
            raise ValueError(f"block type {block_type} outside the Raw_Block subset")
        if off + size > len(frame):
            raise ValueError("truncated block content")
        out += frame[off:off + size]
        off += size
        if last:
            break
    if off != len(frame):
        raise ValueError("trailing bytes after the last block")
    if len(out) != expected_len:
        raise ValueError(f"frame expanded to {len(out)} bytes, header implies {expected_len}")
    return bytes(out)
