# Fetching a fine tile in pieces (task #52)

Written 2026-08-04. Measured on the four bv12 corridor tiles at
`tile-cache/terrain-diffusion-unlabeled-80b9ca451a23eae4-b52995abb/000000000135276f/s16/`.

## 1. The transport answer, first, because it decides the shape

**Before this change there were two serving paths and neither did ranges.**

| path | what it is | ranges? |
|---|---|---|
| the shipping client | `VoxelFineTileStreamer::LoadTile` -> `vxc::readFileBytes(<root>/<provider>/<seed:016x>/s16/<x>_<y>.vxtl)` | it is a **local file read of the whole file**. No HTTP anywhere in `voxel-core` or `ue-project`. |
| `terrain-service` | Flask, two routes: `/healthz` and `/tile?seed&x&y&scale`. `scale=16` is the fine tier. | **no.** `Response(bytes)` never gets Werkzeug's conditional handling, so a `Range:` header was **silently ignored** |

Measured against the shipped 30.6 MB tile `-11_-6.vxtl`, before the change:

```
GET /tile?...&scale=16   Range: bytes=0-182
  -> 200,  body 32,118,243 B,  Accept-Ranges: <absent>,  Content-Range: <absent>
```

That is the worst of the three possible answers: not a refusal, which a client
could detect, but a full 32 MB body with a success status. A fetcher that
trusted it would have "sliced" the tile by downloading all of it once per block.

**This does not stop the work, because we own both ends.** Ranges were absent,
not unavailable:

* the file path supports ranges natively -- `seek`/`read` -- so on the transport
  the client actually uses today, slicing needs **no transport change at all**,
  only the addressing;
* the HTTP path needed one line. `resp.make_conditional(request,
  accept_ranges=True, complete_length=len(data))` in `app.py`. Same request now
  returns `206`, `Content-Range: bytes 0-182/32118243`, 183 bytes.

`Cache-Control: public, max-age=31536000, immutable` is what makes ranges safe
here: the endpoint's own contract is that the response is byte-identical forever
for a given `(provider, seed, x, y, scale)`. Ranging a body that could change
under one URL is how a client assembles two different tiles into one file.

One deliberate limitation: **one range per request.** `make_conditional`
answers a multi-range request with 416, as do plenty of CDNs, so nothing in
this work sends one. Round trips are bought back by coalescing, not by
`multipart/byteranges`.

## 2. What the container already gave us

Nothing about `.vxtl` v2 changed, and nothing needed to. `_encode_plane`
already compresses each block standalone and the index already carries
`(offset u64, comp_len u32, mode u8, const_cp i16, resid_bits u8, pad)`.
Confirmed on all four tiles: `size=8192`, `block_log2=8`, so **32x32 = 1024
blocks of 256 px = 480 m**, and 20 B x 1024 = 20,480 B of index per plane.

Three things the layout forces, all of which are traps if missed:

**CONSTANT blocks have no data-section entry.** `_encode_plane` writes
`(offset=0, comp_len=0)` and `continue`s without appending a chunk. That row is
not a range -- byte 0 of the data section belongs to a different block. On these
tiles the water plane is **72-87% CONSTANT**, so this is the common case, not
the corner. `PlaneIndex.file_range` returns `None`; the block is served from
`const_cp` with no request.

**The preamble is four disjoint regions, not a prefix.** `encode_v2` emits
ELEV_INDEX, ELEV_DATA, FLOW_INDEX, FLOW_DATA, WATER_INDEX, WATER_DATA,
BASIN_TABLE, so each index sits immediately before its own multi-megabyte data
section:

```
-11_-4.vxtl   ELEV_INDEX      183 +   20,480
              ELEV_DATA    20,663 + 38,869,635
              FLOW_INDEX 38,890,298 +   20,480
              FLOW_DATA  38,910,778 + 12,601,793
              WATER_INDEX 51,512,571 +   20,480
              WATER_DATA  51,533,051 +   67,672
              BASIN_TABLE 51,600,723 +    1,768
```

Only the header, section table and ELEV_INDEX are contiguous. "The first 65 KB"
would be 20 KB of index and 44 KB of compressed elevation. Preamble **content**
is 62,303-66,559 B across the four tiles, in **4 requests**; a client that wants
ground only takes ELEV_INDEX alone -- **one request, 20,663 B**.

**The index is row-major, x fastest**, and blocks are written in index order, so
a +x run within one block-row is one contiguous span. `plan_block_ranges` sorts
by *file offset* rather than by `(by, bx)` -- which matters because CONSTANT
blocks punch holes in the coordinate sequence but not in the file, so the
neighbours either side of one are still adjacent bytes.

## 3. Coalescing: the gap tolerance is the bandwidth-delay product

At ~160 ms RTT a 34 KB block request is latency-bound, not throughput-bound.
Merging two spans separated by G bytes costs G wasted bytes and saves one round
trip: `G/B` seconds against `RTT` seconds, so break-even is `G = B*RTT`, which
**is** the BDP. `DEFAULT_COALESCE_GAP = 77 KB` accordingly -- a little over two
median ZSTD blocks. Below the BDP the extra bytes are free in wall-clock terms;
above it they are not.

The same argument sets `HEAD_PROBE_BYTES = 32 KB`: reading 32 KB to get a
20,663 B preamble over-reads by 11 KB, which is well under the BDP and so costs
no time, and buys header + section table + elevation index in **one** round
trip. The probe is an optimisation and never a correctness assumption -- if it
misses a section, `read_preamble` fetches it in round two.

## 4. What it buys, measured

`python tools/tile_slice_measure.py <s16-dir> --verify --http`. Bytes and
requests are what the fetcher actually issued, preamble included.

| working set | blocks | bytes / tile | req | vs whole tile |
|---|---|---|---|---|
| `block-raw` one 480 m block, undilated | 1 | 61,640-78,729 | 2 | **521-703x** |
| `ground-1blk` the same block, correctly dilated | 9 | 284,036-414,121 | 4 | **113-139x** |
| `full-1blk` that block, all three planes | 27 | 412,650-567,687 | 10-11 | **78-91x** |
| `ring-3x3` 1.44 km neighbourhood, three planes | 75 | 1,000,186-1,423,423 | 15 | **32-36x** |
| `water-only` every wet block + index + basins | 130-285 | 106,444-236,465 | 4 | **136-451x** |

Whole tiles for comparison: 32,118,243 / 47,721,389 / 47,969,189 / 51,602,491 B.

Over all four tiles: **171.1 MB** of whole tiles against **1.34 MB in 16
requests** for a `ground-1blk` working set on each (99.22% less), or **4.87 MB
in 60 requests** for `ring-3x3` (97.15% less).

Two readings that matter more than the table:

**The preamble dominates a single-block fetch, and it amortises.** The 480 m
block's own payload is 30-46 KB; the rest of `block-raw`'s 62-79 KB is the head
probe, paid **once per tile** however many blocks are then fetched. So the
marginal cost of the next block is ~34 KB against a 32-56 MB tile -- the
~1000x the task's framing assumed, reached once the client holds more than one
block.

**A water-only refresh is one request.** WATER_INDEX, WATER_DATA and
BASIN_TABLE are the last three sections and are contiguous, and WATER_DATA is
only 51-174 KB because most of the plane is CONSTANT, so **every wet block in a
tile coalesces into a single span**: 104-231 KB, 4 requests including the
preamble, against re-downloading 30-50 MB. This is the composition with the
`ELEV_DATA`-digest work: that work is about whether a water-only re-bake has to
change the tile's identity at all; this one makes the churn cheap *even if it
does*. Neither replaces the other.

**Coalescing waste is negligible at 77 KB.** Worst case across the four tiles
was 7,797 B on `ring-3x3` (0.6% of that fetch); three of the twenty runs wasted
nothing at all.

## 5. Verified

* Every block of every plane, both codecs, byte-identical to the same slab out
  of a full `tile_codec.decode_v2` -- `tests/test_tile_slice.py`, and 84 blocks
  per real tile under `--verify`.
* The `ground-1blk` fetch run over the real Flask `/tile` with `Range:` on all
  four 30-50 MB tiles: 206 throughout, bytes identical to the file-read path.
* `pytest` 568 passed / 2 skipped (546/2 before).

## 6. The client half (added the same day)

Items 1 and 2 below are now built and tested; item 3 turned out not to be the
point.

**There is no network fetch of fine tiles in the shipping client, and there is
no plan for one to appear soon.** Tiles arrive as files in a directory. So the
saving this buys today is *not* transfer, and quoting the 100-700x of §4 as a
bandwidth number would be quoting a saving the game cannot take. What it buys
is **bytes read from disk** -- a file supports ranges natively, so `seek` +
`read` needs no server, no protocol and no change to delivery -- and **peak
memory**, which is the larger of the two.

Built:

* **`FineTileBytes`** (`voxelcore/tilestore.h`) -- a sparse, file-offset-addressed
  byte store. `FineTileBytes::whole()` is the pre-existing shape, so every
  whole-file caller is byte-for-byte unchanged; a sliced client holds the
  preamble plus whatever blocks it fetched. `span()` returns **nullptr** for
  bytes that were not fetched. It does not return zeroes and it does not return
  a short buffer, which is what forces every caller to have a not-resident
  branch.
* **`FineTile::parsePartial`**, `elevBlockResident` / `flowBlockResident` /
  `waterBlockResident`, and `FineError::kBlockNotResident`. A CONSTANT block is
  resident with zero bytes fetched -- it owns none. An unfetched block's decoder
  **leaves the caller's buffer untouched** and returns false; there is a test
  that fills it with a sentinel and asserts the sentinel survives, because
  "returns false" and "does not write sea level into your buffer" are different
  guarantees and only the second one is safe.
* **`flowIndexResident()` / `waterIndexResident()` / `basinsResident()`** -- the
  third state that partial tiles introduced. `hasBasins()` false means "baked
  before the registry existed"; `hasBasins() && basins().empty()` means
  "surveyed, no lakes"; and now `hasBasins() && !basinsResident()` means "we do
  not know". `lakes.h` consults it and routes the third case to the same
  `unresolvedBasins` counter the others use, i.e. water **missing and counted**
  rather than water absent and believed.
* **`voxelcore/tilerange.h`** -- `planBlockRanges` (offset-ordered, coalescing,
  CONSTANT-dropping), `readFineTilePreamble` (the four disjoint regions),
  `fetchFineTileBlocks`, and a `RangeSource` interface with `FileRangeSource`
  and `BytesRangeSource`. Deliberately **no HTTP client**: voxel-core has zero
  third-party dependencies and links into a UE binary, so the transport is
  injected exactly as the zstd decompressor is. The interface is the
  HTTP-readiness; when a fetch exists, it implements `RangeSource` and
  everything above it already works.
* **`vxc_sliceprobe`** -- the C++ measurement, and an independent check on §4:
  it issues byte-identical requests to the Python fetcher (`ground-1blk`
  284,036-414,121 B in 4 requests, matching §4's table exactly) and verifies
  every fetched block against a whole-file decode.

Measured over the four bv12 corridor tiles, per tile, with `--verify`:

| working set | req | disk bytes | held = file + decoded |
|---|---|---|---|
| whole file (today) | 1 | 32.1-51.6 MB | **166-186 MB** = 32-52 + 134 |
| `streamer-load` (what the streamer now does) | 5 | 25.9-39.0 MB | 160-173 MB = 26-39 + 134 |
| `ground-1blk` 9 blocks, dilated | 4 | 284-414 KB | **1.45-1.58 MB** = 0.27-0.40 + 1.18 |
| `water-only` every wet block + basins | 4 | 106-236 KB | — |

**Peak memory is dominated by DECODE, not by the read** -- 134 MB of int16
lattice against a 32-52 MB file, 72% of the total. That is the single most
important number here, because it means slicing the fetch alone moves the
smaller half. The two are separate changes and
`FineTileSampler::residentFileBytes()` / `decodedBlockBytes()` report them
apart so nobody has to guess which one a change moved.

## 7. What is still not built

**The UE streamer's residency is still per-TILE.** `EnsureTileResident_Locked`
now reads in ranges and skips the flow plane entirely (nothing in `ue-project`
decodes flow; `FLOW_DATA` is 12.6 MB of a 51.6 MB tile), which takes the four
tiles from 179.4 MB to 133.7 MB read and held -- 25%. But **rule 1** (the
header's threading note: a tile is fully decoded at load, which is what makes
the worker-facing query path a pure read under a shared lock) still forces every
elevation block to be fetched and decoded.

Retiring rule 1 is where the remaining 100x is, and it is a bigger change than
it looks:

* `FVoxelFineTileSamplerProxy::elevationMm`'s residency test must become
  per-BLOCK. `vxc::FineTileSampler::blockDecoded()` exists for exactly this and
  is a pure const lookup, so the shared lock survives -- but this is the funnel
  that ~100 unguarded consumers pass through, and it is the one line standing
  between the fine tier and a silent desync.
* `IsFootprintResident` / `RequestFootprint` must walk blocks, not tiles.
* the LRU must evict blocks rather than tiles, and "prefetch the ring" must come
  to mean something other than "fetch whole tiles".

It is not done here for a reason worth writing down: **no CI job builds
`ue-project`** (`.github/workflows/ci.yml` builds voxel-core only, on three
compilers), and the editor was unavailable. Making that change blind, on the
desync gate, in the same commit as the read change, is not a trade worth taking.
The voxel-core half it needs is built and tested.

**Server-side, a range still reads the whole file into memory.** `TileCache.get`
is `path.read_bytes()`, so a 183-byte range costs a 32 MB read on the server.
Correct but wasteful; the fix is `send_file(..., conditional=True)` or a
`TileCache.get_range`, and it is invisible to clients.

**The section order is not optimised.** Moving the three indices and the basin
table adjacent would make the whole preamble one range instead of four. It
would also change every tile's bytes and therefore its identity, which is
precisely the churn the sibling work is trying to avoid, so it is not worth it
for three round trips.
