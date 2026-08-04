"""Fetch one 480 m block of a fine tile instead of all 32-56 MB of it (task #52).

WHY THIS IS A FETCH PROBLEM AND NOT A FORMAT CHANGE
---------------------------------------------------
``tile_codec._encode_plane`` already compresses every block on its own "with no
state carried across a block boundary", and the block index already carries
``(offset u64, comp_len u32, mode u8, const_cp i16, resid_bits u8, pad)``. So a
block is standalone bytes at a known file offset, and every plane -- elevation,
flow, water -- goes through that same encoder. Nothing about the container has
to change; what was missing is the addressing and the transport.

THE THREE THINGS THAT ARE EASY TO GET WRONG
-------------------------------------------
1. **CONSTANT blocks have no data-section entry at all.** ``_encode_plane``
   appends an index entry with ``offset=0, comp_len=0`` and ``continue``s
   without pushing a chunk. Asking a server for ``bytes=<data_start>-<...-1>``
   because "offset 0, length 0" looked like a range is a request for bytes that
   belong to a *different* block. ``PlaneIndex.file_range`` returns ``None`` for
   these and ``plan_block_ranges`` drops them before it plans anything --
   a CONSTANT block is served from the index alone, at zero transport cost.
   On the shipped bv12 water plane this is most of the tile.

2. **The preamble is FOUR disjoint regions, not one.** ``encode_v2`` emits
   sections in the order ELEV_INDEX, ELEV_DATA, FLOW_INDEX, FLOW_DATA,
   WATER_INDEX, WATER_DATA, BASIN_TABLE, so each plane's index sits immediately
   *before* its own multi-megabyte data section and the basin table is last.
   Only the header, section table and ELEV_INDEX are contiguous. Anything that
   assumes "the first 65 KB is the preamble" reads 20 KB of index and 44 KB of
   compressed elevation.

3. **The index is row-major, x fastest** (``_decode_plane``: ``by, bx =
   divmod(i, nb)``), and blocks are written in index order, so a run of blocks
   along +x within one block-row is one contiguous byte range.
   ``plan_block_ranges`` coalesces those, and will also bridge a small gap --
   see ``DEFAULT_COALESCE_GAP``.

WHAT THE TRANSPORT HAS TO PROVIDE
---------------------------------
A ``RangeSource``: read N byte ranges, count the requests. ``FileRangeSource``
is the local cache mirror the UE client reads today; ``HttpRangeSource`` is the
``/tile`` endpoint, which serves 206 as of this change and is *verified* to do
so -- an ``HttpRangeSource`` that silently accepted a 200 would download the
whole tile per block and report a saving that never happened, so it raises.
"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Protocol, Sequence

import numpy as np

from . import tile_codec as tc

__all__ = [
    "ByteRange",
    "BlockEntry",
    "PlaneIndex",
    "TilePreamble",
    "RangePlan",
    "RangeSource",
    "FileRangeSource",
    "BytesRangeSource",
    "HttpRangeSource",
    "RangeNotSupported",
    "HEAD_PROBE_BYTES",
    "DEFAULT_COALESCE_GAP",
    "CARRIER_STENCIL_LO",
    "CARRIER_STENCIL_HI",
    "read_preamble",
    "plan_block_ranges",
    "fetch_blocks",
    "blocks_covering_rect",
    "dilated_block_coverage",
]


#: How much of the file's head to ask for on the first request. The header
#: (25 B) + v2 extension (18 B) + section table (7 x 20 B) is 183 B, and
#: ELEV_INDEX -- which always follows it, being the first section -- is
#: 20,480 B for the shipped 32x32 block grid. 32 KB therefore lands the header,
#: the section table AND the elevation index in ONE round trip on every tile in
#: the shipped cache, and degrades gracefully if a future tile is laid out
#: differently: `read_preamble` re-reads whatever the probe missed.
HEAD_PROBE_BYTES = 32 * 1024

#: Bridge a gap of at most this many bytes rather than split a range in two.
#:
#: This is the bandwidth-delay product and that is not a coincidence. Merging
#: two ranges separated by G bytes costs G wasted bytes and saves one round
#: trip: G/B seconds against RTT seconds, so the break-even gap is G = B*RTT,
#: which IS the BDP. At the 160 ms RTT and ~3.9 Mbit/s the streaming work
#: measured, that is ~77 KB -- a little over two median ZSTD blocks. Below the
#: BDP a request is latency-bound and the extra bytes are free; above it they
#: are not.
DEFAULT_COALESCE_GAP = 77 * 1024

#: voxel-core/include/voxelcore/tiles.h:
#:     static_assert(kCarrierStencilLo == -17 && kCarrierStencilHi == 17, ...)
#: Duplicated here so the measurement can size a realistic working set without
#: a UE build. tests/test_tile_slice.py greps that static_assert and fails if
#: these two drift from it -- a silently undersized dilation is exactly the
#: failure tilestreaming.h's own comment warns about ("does not fault ...
#: silently returns an edge value").
CARRIER_STENCIL_LO = -17
CARRIER_STENCIL_HI = 17


class RangeNotSupported(RuntimeError):
    """The transport ignored a Range request and answered with the whole body."""


# --------------------------------------------------------------- byte ranges

@dataclass(frozen=True)
class ByteRange:
    """A half-open [start, start+length) span of file bytes."""

    start: int
    length: int

    def __post_init__(self) -> None:
        if self.start < 0 or self.length < 0:
            raise ValueError(f"nonsensical range {self!r}")

    @property
    def end(self) -> int:
        return self.start + self.length

    def header(self) -> str:
        """The HTTP Range header value for this span (inclusive-inclusive)."""
        if self.length == 0:
            raise ValueError("cannot express an empty range as a Range header")
        return f"bytes={self.start}-{self.end - 1}"


# --------------------------------------------------------------- block index

@dataclass(frozen=True)
class BlockEntry:
    """One ``tile_codec._BLOCK_ENTRY``, minus the pad."""

    offset: int       # into the plane's DATA section, not the file
    comp_len: int
    mode: int
    const_cp: int
    resid_bits: int

    @property
    def is_constant(self) -> bool:
        return self.mode == tc.MODE_CONSTANT


@dataclass(frozen=True)
class PlaneIndex:
    """A decoded block index plus where its DATA section starts in the file."""

    entries: tuple[BlockEntry, ...]
    blocks_per_axis: int
    block_dim_px: int
    data_offset: int          # file offset of the plane's DATA section
    data_length: int
    elem_dtype: str           # "<i2" for elevation/water, "u1" for flow
    out_dtype: type

    @property
    def index_bytes(self) -> int:
        return len(self.entries) * tc._BLOCK_ENTRY.size

    def entry(self, bx: int, by: int) -> BlockEntry:
        if not (0 <= bx < self.blocks_per_axis and 0 <= by < self.blocks_per_axis):
            raise IndexError(f"block ({bx},{by}) outside {self.blocks_per_axis}^2")
        return self.entries[by * self.blocks_per_axis + bx]  # row-major, x fastest

    def file_range(self, bx: int, by: int) -> ByteRange | None:
        """Where block (bx, by) lives in the FILE, or None if it costs nothing.

        None means MODE_CONSTANT: trap #1 in the module docstring. The caller
        must serve the block from ``entry(bx, by).const_cp`` and issue no
        request -- the (offset=0, comp_len=0) an encoder wrote for it is not a
        range, and byte 0 of the data section belongs to a different block.
        """
        e = self.entry(bx, by)
        if e.is_constant:
            return None
        return ByteRange(self.data_offset + e.offset, e.comp_len)

    def constant_share(self) -> float:
        n = len(self.entries)
        return sum(1 for e in self.entries if e.is_constant) / n if n else 0.0


def _parse_plane_index(
    index_bytes: bytes, *, size: int, block_log2: int, data_offset: int,
    data_length: int, elem_dtype: str, out_dtype,
) -> PlaneIndex:
    bs = 1 << block_log2
    nb = size // bs
    n_blocks = nb * nb
    if len(index_bytes) != n_blocks * tc._BLOCK_ENTRY.size:
        raise ValueError("block index has the wrong length")
    entries = []
    for i in range(n_blocks):
        offset, comp_len, mode, const_cp, resid_bits, pad = tc._BLOCK_ENTRY.unpack_from(
            index_bytes, i * tc._BLOCK_ENTRY.size
        )
        if pad != tc._ZERO_PAD4:
            raise ValueError("nonzero reserved pad bytes in block index")
        if mode == tc.MODE_CONSTANT:
            if comp_len != 0 or offset != 0:
                raise ValueError("CONSTANT block must have offset 0 and comp_len 0")
        elif mode not in (tc.MODE_CODED, tc.MODE_RAW):
            raise ValueError(f"bad block mode {mode}")
        elif offset + comp_len > data_length:
            raise ValueError("block payload extends past the end of the data section")
        entries.append(BlockEntry(offset, comp_len, mode, const_cp, resid_bits))
    return PlaneIndex(
        entries=tuple(entries), blocks_per_axis=nb, block_dim_px=bs,
        data_offset=data_offset, data_length=data_length,
        elem_dtype=elem_dtype, out_dtype=out_dtype,
    )


# ------------------------------------------------------------------ preamble

@dataclass
class TilePreamble:
    """Everything a client needs before it can ask for a single block.

    ~62-67 KB against a 32-56 MB tile, in four disjoint file regions (trap #2).
    """

    seed: int
    x: int
    y: int
    size: int
    block_log2: int
    predictor: int
    quant: int
    codec: int
    bake_ver: int
    flags: int
    base_offset_mm: int
    sections: dict[int, tuple[int, int]]   # id -> (offset, length)
    file_size: int
    elevation: PlaneIndex
    flow: PlaneIndex | None = None
    water: PlaneIndex | None = None
    basins: "list[tc.BasinEntry] | None" = None

    @property
    def block_dim_px(self) -> int:
        return 1 << self.block_log2

    @property
    def blocks_per_axis(self) -> int:
        return self.size // self.block_dim_px

    @property
    def preamble_bytes(self) -> int:
        """What reading this preamble cost, ignoring probe over-read."""
        fixed = tc._HEADER.size + tc._V2_EXT.size + len(self.sections) * tc._SECTION_ENTRY.size
        idx = sum(
            self.sections[s][1] for s in
            (tc.SECTION_ELEV_INDEX, tc.SECTION_FLOW_INDEX, tc.SECTION_WATER_INDEX)
            if s in self.sections
        )
        basin = self.sections.get(tc.SECTION_BASIN_TABLE, (0, 0))[1]
        return fixed + idx + basin

    def plane(self, section_index_id: int) -> PlaneIndex | None:
        return {
            tc.SECTION_ELEV_INDEX: self.elevation,
            tc.SECTION_FLOW_INDEX: self.flow,
            tc.SECTION_WATER_INDEX: self.water,
        }[section_index_id]


def _parse_fixed_head(head: bytes) -> dict:
    """Header + v2 extension + section table out of the head probe."""
    if len(head) < tc._HEADER.size:
        raise ValueError("truncated header")
    magic, version, seed, x, y, scale, size = tc._HEADER.unpack_from(head, 0)
    if magic != tc.MAGIC:
        raise ValueError("bad magic")
    if version != tc.VERSION_V2:
        raise ValueError(f"unsupported tile version {version}")
    if scale != tc.FINE_SCALE:
        raise ValueError(f"unsupported v2 scale {scale}")
    off = tc._HEADER.size
    if len(head) < off + tc._V2_EXT.size:
        raise ValueError("truncated v2 header extension")
    (block_log2, predictor, quant, codec, bake_ver, flags,
     base_offset_mm, parent_scale, reserved, n_sections) = tc._V2_EXT.unpack_from(head, off)
    off += tc._V2_EXT.size
    if reserved != tc._ZERO_RESERVED3:
        raise ValueError("nonzero reserved header bytes")
    if predictor != tc.PRED_MED:
        raise ValueError(f"unsupported predictor {predictor}")
    if quant not in tc.QUANT_MM:
        raise ValueError(f"unsupported quant {quant}")
    if codec not in (tc.CODEC_RAW, tc.CODEC_ZSTD):
        raise ValueError(f"unsupported codec {codec}")
    if parent_scale != 0:
        raise ValueError("parent_scale != 0 (residual ladder) is not supported")
    bs = 1 << block_log2
    if size == 0 or size % bs != 0:
        raise ValueError(f"size {size} not a multiple of the block edge {bs}")
    if flags & ~(tc.FLAG_FLOW_PRESENT | tc.FLAG_BASINS_PRESENT | tc.FLAG_WATER_PRESENT):
        raise ValueError(f"unknown header flag bits set: 0x{flags:04x}")

    table_len = n_sections * tc._SECTION_ENTRY.size
    if len(head) < off + table_len:
        raise ValueError(
            f"head probe of {len(head)} B did not reach the end of the section "
            f"table ({off + table_len} B); raise HEAD_PROBE_BYTES"
        )
    sections: dict[int, tuple[int, int]] = {}
    file_size = off + table_len
    for i in range(n_sections):
        sid, soff, slen = tc._SECTION_ENTRY.unpack_from(head, off + i * tc._SECTION_ENTRY.size)
        sections[sid] = (soff, slen)
        file_size = max(file_size, soff + slen)
    return dict(
        seed=seed, x=x, y=y, size=size, block_log2=block_log2, predictor=predictor,
        quant=quant, codec=codec, bake_ver=bake_ver, flags=flags,
        base_offset_mm=base_offset_mm, sections=sections, file_size=file_size,
    )


def read_preamble(source: "RangeSource", *, want_flow: bool = True,
                  want_water: bool = True, want_basins: bool = True) -> TilePreamble:
    """Header + section table + the requested plane indices + basin table.

    Two rounds at most. The first is a single ``HEAD_PROBE_BYTES`` read, which
    on every shipped tile already contains the header, the section table AND
    ELEV_INDEX. The second asks for whichever of FLOW_INDEX / WATER_INDEX /
    BASIN_TABLE the caller wants, each its own range because ``encode_v2`` puts
    a multi-megabyte data section between them (trap #2).

    A client that only needs ground can pass ``want_flow=want_water=False`` and
    get its whole preamble in ONE request -- 20,663 B of content on the shipped
    tiles, out of a ``HEAD_PROBE_BYTES`` read.
    """
    head = source.read_ranges([ByteRange(0, HEAD_PROBE_BYTES)])[0]
    fields = _parse_fixed_head(head)
    sections = fields["sections"]

    wanted = [tc.SECTION_ELEV_INDEX]
    if want_flow and (fields["flags"] & tc.FLAG_FLOW_PRESENT):
        wanted.append(tc.SECTION_FLOW_INDEX)
    if want_water and (fields["flags"] & tc.FLAG_WATER_PRESENT):
        wanted.append(tc.SECTION_WATER_INDEX)
    if want_basins and (fields["flags"] & tc.FLAG_BASINS_PRESENT):
        wanted.append(tc.SECTION_BASIN_TABLE)

    have: dict[int, bytes] = {}
    todo: list[int] = []
    for sid in wanted:
        if sid not in sections:
            raise ValueError(f"flag set but section {sid} is missing")
        soff, slen = sections[sid]
        if soff + slen <= len(head):          # already in the head probe
            have[sid] = head[soff:soff + slen]
        else:
            todo.append(sid)
    if todo:
        got = source.read_ranges([ByteRange(*sections[s]) for s in todo])
        for sid, blob in zip(todo, got):
            have[sid] = blob

    def plane_for(index_id: int, data_id: int, elem_dtype: str, out_dtype):
        if index_id not in have:
            return None
        if data_id not in sections:
            raise ValueError(f"section {index_id} present but its data section is missing")
        doff, dlen = sections[data_id]
        return _parse_plane_index(
            have[index_id], size=fields["size"], block_log2=fields["block_log2"],
            data_offset=doff, data_length=dlen,
            elem_dtype=elem_dtype, out_dtype=out_dtype,
        )

    elevation = plane_for(tc.SECTION_ELEV_INDEX, tc.SECTION_ELEV_DATA, "<i2", np.int16)
    if elevation is None:
        raise ValueError("missing required ELEV_INDEX")
    return TilePreamble(
        seed=fields["seed"], x=fields["x"], y=fields["y"], size=fields["size"],
        block_log2=fields["block_log2"], predictor=fields["predictor"],
        quant=fields["quant"], codec=fields["codec"], bake_ver=fields["bake_ver"],
        flags=fields["flags"], base_offset_mm=fields["base_offset_mm"],
        sections=sections, file_size=fields["file_size"],
        elevation=elevation,
        flow=plane_for(tc.SECTION_FLOW_INDEX, tc.SECTION_FLOW_DATA, "u1", np.uint8),
        water=plane_for(tc.SECTION_WATER_INDEX, tc.SECTION_WATER_DATA, "<i2", np.int16),
        basins=(tc.decode_basin_table(have[tc.SECTION_BASIN_TABLE])
                if tc.SECTION_BASIN_TABLE in have else None),
    )


# ------------------------------------------------------------------ planning

@dataclass(frozen=True)
class RangePlan:
    """One request: a byte span, the blocks it satisfies, and the waste.

    ``useful_bytes`` is what the requested blocks actually occupy;
    ``span.length - useful_bytes`` is what coalescing paid to save a round
    trip, and is the number to look at when tuning ``coalesce_gap``.
    """

    span: ByteRange
    blocks: tuple[tuple[int, int], ...]
    useful_bytes: int

    @property
    def wasted_bytes(self) -> int:
        return self.span.length - self.useful_bytes


def plan_block_ranges(
    plane: PlaneIndex,
    blocks: Iterable[tuple[int, int]],
    *,
    coalesce_gap: int = DEFAULT_COALESCE_GAP,
) -> list[RangePlan]:
    """Turn a set of wanted blocks into the requests that fetch them.

    CONSTANT blocks are dropped (trap #1) -- they are already answered by the
    index, and a plan that included them would ask for bytes that are not
    theirs. Everything else is sorted by file offset and coalesced while the
    gap between consecutive payloads is at most ``coalesce_gap``. Because the
    index is row-major and blocks are written in index order, a +x run inside
    one block-row coalesces to a single span with gap 0 (trap #3).

    Sorting by offset rather than by (by, bx) is deliberate: it is the encoder,
    not the caller, that decided the file order, and CONSTANT blocks punch
    holes in the (by, bx) sequence that offset order closes.
    """
    if coalesce_gap < 0:
        raise ValueError("coalesce_gap must be >= 0")
    wanted: list[tuple[ByteRange, tuple[int, int]]] = []
    for bx, by in blocks:
        r = plane.file_range(bx, by)
        if r is None or r.length == 0:
            continue                       # CONSTANT: costs nothing, no request
        wanted.append((r, (bx, by)))
    if not wanted:
        return []
    wanted.sort(key=lambda t: (t[0].start, t[0].length))

    plans: list[RangePlan] = []
    cur_start = wanted[0][0].start
    cur_end = wanted[0][0].end
    cur_blocks = [wanted[0][1]]
    cur_useful = wanted[0][0].length
    for r, b in wanted[1:]:
        if r.start <= cur_end + coalesce_gap:
            cur_useful += r.length
            cur_end = max(cur_end, r.end)
            cur_blocks.append(b)
        else:
            plans.append(RangePlan(ByteRange(cur_start, cur_end - cur_start),
                                   tuple(cur_blocks), cur_useful))
            cur_start, cur_end, cur_blocks, cur_useful = r.start, r.end, [b], r.length
    plans.append(RangePlan(ByteRange(cur_start, cur_end - cur_start),
                           tuple(cur_blocks), cur_useful))
    return plans


# ----------------------------------------------------------- range transports

class RangeSource(Protocol):
    """Read byte ranges of one tile, and say what that cost.

    ``read_ranges`` returns one ``bytes`` per requested range, in order. An
    implementation may satisfy several ranges in one round trip, but it MUST
    count what it actually issued in ``requests`` -- the whole point of the
    exercise is the request count, so a source that under-reports it is worse
    than useless.
    """

    requests: int
    bytes_fetched: int

    def read_ranges(self, ranges: Sequence[ByteRange]) -> list[bytes]: ...


class _CountingSource:
    def __init__(self) -> None:
        self.requests = 0
        self.bytes_fetched = 0

    def reset(self) -> None:
        self.requests = 0
        self.bytes_fetched = 0


class BytesRangeSource(_CountingSource):
    """An in-memory tile. For tests, and for measuring against a known blob."""

    def __init__(self, data: bytes) -> None:
        super().__init__()
        self._data = data

    def read_ranges(self, ranges: Sequence[ByteRange]) -> list[bytes]:
        out = []
        for r in ranges:
            self.requests += 1
            chunk = self._data[r.start:r.end]
            self.bytes_fetched += len(chunk)
            out.append(chunk)
        return out


class FileRangeSource(_CountingSource):
    """The local cache mirror -- what ``vxc::readFileBytes`` reads today.

    This IS the fine tier's serving path as shipped: the UE client
    (VoxelFineTileStreamer::LocalPathFor) builds the same
    ``<root>/<provider>/<seed:016x>/s16/<x>_<y>.vxtl`` this opens, and reads
    all of it. A file supports ranges natively, so on this transport slicing
    needs no server change at all -- only the addressing above.
    """

    def __init__(self, path: str | os.PathLike[str]) -> None:
        super().__init__()
        self.path = Path(path)
        self._f: io.BufferedReader | None = None

    def __enter__(self) -> "FileRangeSource":
        self._f = open(self.path, "rb")
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        if self._f is not None:
            self._f.close()
            self._f = None

    def read_ranges(self, ranges: Sequence[ByteRange]) -> list[bytes]:
        if self._f is None:
            self._f = open(self.path, "rb")
        out = []
        for r in ranges:
            self.requests += 1
            self._f.seek(r.start)
            chunk = self._f.read(r.length)
            self.bytes_fetched += len(chunk)
            out.append(chunk)
        return out


class HttpRangeSource(_CountingSource):
    """``GET /tile`` with a Range header, one request per span.

    ONE range per request on purpose. Werkzeug's ``make_conditional`` -- and a
    good many CDNs -- answer a multi-range request with 416 rather than
    ``multipart/byteranges``, so a fetcher built on multi-range would work in
    a unit test and fall over in front of a cache. Coalescing (above) is what
    buys the round trips back, not multipart.

    A 200 response is treated as a HARD ERROR, not as a slow path. That is the
    behaviour ``/tile`` had before this change: the Range header was ignored
    and the full 32-56 MB body came back with status 200. Accepting it here
    would turn "slicing works" into "slicing downloads the whole tile once per
    block" while every byte counter still looked plausible.
    """

    def __init__(self, get, url: str) -> None:
        """`get(url, headers=...)` -> response with .status_code/.headers/.content.

        Deliberately a callable rather than a hard dependency on requests or a
        Flask test client, so this is exercisable in CI with neither a network
        nor an extra package.
        """
        super().__init__()
        self._get = get
        self.url = url

    def read_ranges(self, ranges: Sequence[ByteRange]) -> list[bytes]:
        out = []
        for r in ranges:
            self.requests += 1
            resp = self._get(self.url, headers={"Range": r.header()})
            status = getattr(resp, "status_code", None)
            if status == 200:
                raise RangeNotSupported(
                    f"{self.url} answered 200 with {len(self._body(resp))} B for "
                    f"{r.header()}; the transport ignored the Range header"
                )
            if status != 206:
                raise RangeNotSupported(f"{self.url} answered {status} for {r.header()}")
            body = self._body(resp)
            self.bytes_fetched += len(body)
            # A short body is legitimate in exactly one case: the range ran past
            # the end of the file, which the head probe does on purpose (it asks
            # for HEAD_PROBE_BYTES without knowing the length yet) and which
            # RFC 9110 says to satisfy by clamping. Content-Range carries the
            # complete length, so we can tell that apart from a truncated
            # response rather than guessing.
            expected = r.length
            total = self._complete_length(resp)
            if total is not None:
                expected = min(expected, max(0, total - r.start))
            if len(body) != expected:
                raise RangeNotSupported(
                    f"{self.url} returned {len(body)} B for a {r.length} B range"
                    + (f" of a {total} B body" if total is not None else "")
                )
            out.append(body)
        return out

    @staticmethod
    def _body(resp) -> bytes:
        # requests -> .content; Flask test client -> .get_data()
        data = getattr(resp, "content", None)
        return data if data is not None else resp.get_data()

    @staticmethod
    def _complete_length(resp) -> int | None:
        headers = getattr(resp, "headers", None) or {}
        cr = headers.get("Content-Range")
        if not cr or "/" not in cr:
            return None
        tail = cr.rsplit("/", 1)[1].strip()
        return int(tail) if tail.isdigit() else None


# ------------------------------------------------------------ block decoding

def decode_block(plane: PlaneIndex, bx: int, by: int, payload: bytes | None,
                 *, codec: int, decompressor=None) -> np.ndarray:
    """One block's (block_dim, block_dim) values.

    ``payload`` must be ``None`` exactly when ``file_range`` returned None.
    """
    e = plane.entry(bx, by)
    bs = plane.block_dim_px
    n = bs * bs
    if e.is_constant:
        if payload:
            raise ValueError("payload supplied for a CONSTANT block")
        lo, hi = np.iinfo(plane.out_dtype).min, np.iinfo(plane.out_dtype).max
        if e.const_cp < lo or e.const_cp > hi:
            raise ValueError(f"const_cp {e.const_cp} out of range [{lo}, {hi}]")
        return np.full((bs, bs), e.const_cp, dtype=plane.out_dtype)
    if payload is None or len(payload) != e.comp_len:
        raise ValueError("block payload missing or the wrong length")
    if e.mode == tc.MODE_CODED:
        if e.resid_bits not in (16, 32):
            raise ValueError(f"bad resid_bits {e.resid_bits}")
        expected = n * (2 if e.resid_bits == 16 else 4)
    else:
        expected = n * np.dtype(plane.elem_dtype).itemsize
    raw = tc._decompress(payload, codec, expected, decompressor)
    if e.mode == tc.MODE_CODED:
        resid = tc._unpack_residuals(raw, n, e.resid_bits).reshape(bs, bs)
        return tc._med_reconstruct(resid).astype(plane.out_dtype)
    return np.frombuffer(raw, dtype=plane.elem_dtype, count=n).reshape(bs, bs).astype(
        plane.out_dtype, copy=True
    )


def fetch_blocks(
    source: "RangeSource",
    plane: PlaneIndex,
    blocks: Iterable[tuple[int, int]],
    *,
    codec: int,
    coalesce_gap: int = DEFAULT_COALESCE_GAP,
    decompressor=None,
) -> tuple[dict[tuple[int, int], np.ndarray], list[RangePlan]]:
    """Fetch and decode exactly the named blocks. Returns (blocks, the plan)."""
    blocks = list(dict.fromkeys(blocks))       # de-dup, keep order
    plans = plan_block_ranges(plane, blocks, coalesce_gap=coalesce_gap)
    spans = source.read_ranges([p.span for p in plans]) if plans else []

    payloads: dict[tuple[int, int], bytes] = {}
    for plan, blob in zip(plans, spans):
        if len(blob) != plan.span.length:
            raise ValueError("short read for a planned range")
        for bx, by in plan.blocks:
            r = plane.file_range(bx, by)
            assert r is not None
            payloads[(bx, by)] = blob[r.start - plan.span.start:
                                      r.end - plan.span.start]

    out = {}
    for bx, by in blocks:
        out[(bx, by)] = decode_block(
            plane, bx, by, payloads.get((bx, by)),
            codec=codec, decompressor=decompressor,
        )
    return out, plans


# ------------------------------------------------------------ block coverage

def blocks_covering_rect(px0: int, py0: int, px1: int, py1: int, *,
                         block_dim_px: int, blocks_per_axis: int
                         ) -> list[tuple[int, int]]:
    """Blocks of ONE tile that a closed fine-pixel rect overlaps, clamped.

    The single-tile half of ``vxc::blocksCoveringRect``
    (voxel-core/include/voxelcore/tilestreaming.h); the multi-tile routing that
    function also does is the caller's business here, because each tile is a
    separate file and therefore a separate fetch.
    """
    if px1 < px0 or py1 < py0:
        return []
    bx0 = max(0, px0 // block_dim_px)
    bx1 = min(blocks_per_axis - 1, px1 // block_dim_px)
    by0 = max(0, py0 // block_dim_px)
    by1 = min(blocks_per_axis - 1, py1 // block_dim_px)
    if bx1 < bx0 or by1 < by0:
        return []
    return [(bx, by) for by in range(by0, by1 + 1) for bx in range(bx0, bx1 + 1)]


def dilated_block_coverage(px0: int, py0: int, px1: int, py1: int, *,
                           block_dim_px: int, blocks_per_axis: int,
                           extra_margin_px: int = 0) -> list[tuple[int, int]]:
    """``dilateForCarrierStencil`` then ``blocks_covering_rect``.

    ``extra_margin_px`` is the caller's own generation reach on top of the
    carrier stencil -- the UE host passes ``kRasterCavernMarginMm`` converted
    to fine pixels (``vxc::kCavernMaxReachMm`` = 36,394 mm = 20 px at
    1.875 m/px). It is a parameter rather than a constant here for the reason
    ``fineReadPixelRect`` takes one: the carrier stencil is the format's, the
    generation reach is the consumer's, and conflating them is how the two came
    to disagree once already.
    """
    m = extra_margin_px
    return blocks_covering_rect(
        px0 + CARRIER_STENCIL_LO - m, py0 + CARRIER_STENCIL_LO - m,
        px1 + CARRIER_STENCIL_HI + m, py1 + CARRIER_STENCIL_HI + m,
        block_dim_px=block_dim_px, blocks_per_axis=blocks_per_axis,
    )
