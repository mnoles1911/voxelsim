#pragma once
// Fetching a fine tile in PIECES -- the C++ half of task #52.
//
// docs/tile-slicing-2026-08-04.md is the measurement this implements, and the
// Python original (terrain-service/terrain_service/tile_slice.py) is the
// reference; the two are built against the same three layout facts and are
// cross-checked against the same real tiles, so a disagreement shows up as a
// byte mismatch rather than as terrain.
//
// WHAT THIS IS FOR, stated plainly, because the framing has moved once
// already. There is NO network fetch of fine tiles in the shipping client:
// tiles arrive as files in a directory and VoxelFineTileStreamer reads them
// with vxc::readFileBytes -- all of them, whole. So the saving this buys today
// is not transfer, it is:
//
//   * BYTES READ FROM DISK. A file supports ranges natively (seek + read), so
//     no transport change is needed to stop reading 32-56 MB to answer a query
//     that needs 60 KB of it.
//   * PEAK MEMORY, which is the larger of the two and the reason to care.
//     FineTile OWNS its bytes for the tile's whole resident life, so a
//     whole-file load is 32-56 MB held per tile, plus ~134 MB of decoded int16
//     lattice if the host warms every block. Slicing the fetch removes the
//     first; slicing the DECODE removes the second, and they are separate
//     changes -- see FineTileSampler::residentFileBytes/decodedBlockBytes,
//     which report them apart so nobody has to guess which one moved.
//
// The HTTP path is real (terrain-service answers 206 with Content-Range as of
// PR #217) but no client calls it for the fine tier, so nothing here claims a
// network saving. RangeSource is an ABSTRACT interface for exactly that
// reason: voxel-core has no HTTP client and must not grow one (same argument
// as the injected zstd decompressor in tilestore.h -- zero third-party
// dependencies, and it links into a UE binary). A host that acquires a
// transport implements the interface; everything above it already works.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "voxelcore/tilestore.h"

namespace vxc {

// --- constants, mirroring terrain_service/tile_slice.py ---------------------

// How much of the file's head to read on the first request. The header (43 B)
// plus a 7-entry section table (140 B) is 183 B, and ELEV_INDEX -- always the
// first section -- is 20,480 B for the shipped 32x32 block grid, so 32 KB lands
// header + section table + elevation index in ONE round trip on every tile in
// the shipped cache. Over-reading by ~11 KB costs nothing against a network
// bandwidth-delay product (below) and nothing at all against a local file.
//
// An OPTIMISATION, never a correctness assumption: readFineTilePreamble
// re-reads whatever the probe missed, so a differently laid out tile is slower
// here and never wrong.
inline constexpr uint64_t kFineHeadProbeBytes = 32 * 1024;

// Bridge a gap of at most this many bytes rather than split one request in two.
//
// This is the bandwidth-delay product, and that is the derivation rather than a
// coincidence: merging two spans separated by G bytes costs G wasted bytes and
// saves one round trip, G/B seconds against RTT seconds, so break-even is
// G = B*RTT. At the 160 ms RTT and ~3.9 Mbit/s the streaming work measured,
// that is ~77 KB -- a little over two median ZSTD blocks. Below the BDP the
// extra bytes are free in wall-clock terms; above it they are not.
//
// On the LOCAL FILE path, which is the only path the client uses today, the
// tradeoff is different in degree but the same in sign: a seek is cheap but not
// free, and 77 KB of sequential read is far cheaper than a second seek on any
// storage this runs on. Measured coalescing waste at this gap was <=0.6%.
inline constexpr uint64_t kFineCoalesceGap = 77 * 1024;

// --- byte ranges ------------------------------------------------------------

// A half-open [start, start+length) span of FILE bytes.
struct ByteRange {
    uint64_t start = 0;
    uint64_t length = 0;

    uint64_t end() const { return start + length; }
    bool empty() const { return length == 0; }

    friend bool operator==(const ByteRange&, const ByteRange&) = default;
};

// One request: the span to ask for, which blocks it satisfies, and how much of
// it is actually wanted. `span.length - usefulBytes` is what coalescing paid to
// save a seek/round trip, and is the number to look at when tuning the gap.
struct RangePlan {
    ByteRange span;
    // Block INDICES (by * blocksPerAxis + bx), the same key FineTileSampler's
    // block cache uses. Indices rather than (bx,by) pairs because the plan is
    // sorted by FILE OFFSET, which is not (by,bx) order -- see planBlockRanges.
    std::vector<uint32_t> blocks;
    uint64_t usefulBytes = 0;

    uint64_t wastedBytes() const { return span.length - usefulBytes; }
};

// Turn a set of wanted blocks into the requests that fetch them.
//
// `index` is the plane's block index (FineTile::elevIndex() and friends);
// `dataOffset` is the FILE offset of that plane's DATA section, which is what
// makes each entry's `offset` addressable. `blockIds` are index positions;
// duplicates and out-of-range ids are ignored.
//
// THE THREE THINGS THIS GETS RIGHT, each of which produces a wrong fetcher if
// missed (docs/tile-slicing-2026-08-04.md §2):
//
//   1. CONSTANT blocks are DROPPED, not requested. Their (offset=0,comp_len=0)
//      is not a range -- byte 0 of the data section belongs to a different
//      block -- and they need no bytes at all, being served from `const_cp` in
//      the index. On the shipped water plane that is 72-87% of the tile, so
//      this is the common case and not a corner.
//   2. Sorting is by FILE OFFSET, not by (by,bx). The encoder decided the file
//      order; CONSTANT blocks punch holes in the coordinate sequence but not in
//      the file, so the two neighbours either side of one are still adjacent
//      bytes and still coalesce.
//   3. Spans are merged while the gap between them is at most `coalesceGap`.
//      Because the index is row-major with x fastest and blocks are written in
//      index order, a +x run inside one block-row merges at gap 0.
//
// Returns plans in ascending file order. Empty when every wanted block is
// CONSTANT or the set is empty -- which is a legitimate and common answer,
// NOT an error: it means the client already holds everything it needs.
std::vector<RangePlan> planBlockRanges(const std::vector<FineBlockEntry>& index,
                                       uint64_t dataOffset,
                                       const std::vector<uint32_t>& blockIds,
                                       uint64_t coalesceGap = kFineCoalesceGap);

// --- transport --------------------------------------------------------------

// Read byte ranges of ONE tile, and say honestly what that cost.
//
// ONE RANGE PER CALL, deliberately. Werkzeug's `make_conditional` -- and plenty
// of CDNs -- answer a multi-range request with 416 rather than
// multipart/byteranges, so a fetcher built on multi-range would pass a unit
// test and fall over in front of a cache. Coalescing is what buys the round
// trips back.
//
// `requests` and `bytesRead` are the measurement. An implementation that
// under-reports them is worse than useless: the entire point of this exercise
// is the request count and the byte count, and a transport that silently
// fetched more than it was asked for (an HTTP 200 answering a Range request is
// exactly this, and is what /tile did before PR #217) would report a saving
// that never happened.
class RangeSource {
public:
    virtual ~RangeSource() = default;

    // Appends `length` bytes at `offset` to `out`. Returns false on any I/O
    // failure OR any short read -- a short read is never silently accepted,
    // because a truncated payload decodes to plausible-looking wrong terrain
    // rather than to an error.
    //
    // The one legitimate short read is a probe that ran past the end of the
    // file, which readFineTilePreamble does on purpose without knowing the
    // length yet; `readClamped` is that case, spelled separately so the strict
    // version can stay strict.
    virtual bool read(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) = 0;

    // Like read(), but a range extending past the end of the file is satisfied
    // by clamping (RFC 9110's rule for HTTP, and what a file read does anyway)
    // and reports how many bytes actually came back.
    virtual bool readClamped(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) = 0;

    uint64_t requests = 0;
    uint64_t bytesRead = 0;
};

// The local cache mirror -- what the shipping client reads today, and the only
// fine-tier transport that exists. A file supports ranges natively, so this
// needs no server, no protocol and no change to how tiles are delivered: it is
// the same `<root>/<provider>/<seed:016x>/s16/<x>_<y>.vxtl` path
// VoxelFineTileStreamer::LocalPathFor already builds, opened once and seeked
// instead of read whole.
class FileRangeSource final : public RangeSource {
public:
    explicit FileRangeSource(const std::filesystem::path& path);

    bool ok() const { return f_.is_open(); }
    // The file's size on disk, or 0 if it could not be opened. This is the
    // `fileSize` FineTileBytes needs, and taking it from the filesystem rather
    // than from the section table is what lets a truncated file be caught: the
    // table says how long the tile SHOULD be, this says how long it IS.
    uint64_t fileSize() const { return size_; }

    bool read(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) override;
    bool readClamped(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) override;

private:
    mutable std::ifstream f_;
    uint64_t size_ = 0;
};

// An in-memory tile, for tests and for measuring against a known blob.
class BytesRangeSource final : public RangeSource {
public:
    explicit BytesRangeSource(std::vector<uint8_t> data) : data_(std::move(data)) {}

    uint64_t fileSize() const { return data_.size(); }
    bool read(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) override;
    bool readClamped(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) override;

private:
    std::vector<uint8_t> data_;
};

// --- the fetch --------------------------------------------------------------

// Which planes a client wants the INDEX for. Skipping a plane skips its index
// (20,480 B) and, more to the point, means the client never asks for a byte of
// its data section: on the shipped tiles FLOW_DATA alone is 12.6 MB of a
// 51.6 MB tile, and nothing in ue-project reads the flow plane at all.
struct FinePreambleRequest {
    bool wantFlow = true;
    bool wantWater = true;
    bool wantBasins = true;
    uint64_t headProbeBytes = kFineHeadProbeBytes;
};

// Reads header + section table + the requested plane indices + basin table into
// a FineTileBytes, ready for FineTile::parsePartial. Two rounds at most: one
// head probe, then one request per section the probe did not already cover.
//
// The four regions are DISJOINT and that is the trap this function exists to
// absorb (docs/tile-slicing-2026-08-04.md §2): encode_v2 emits ELEV_INDEX,
// ELEV_DATA, FLOW_INDEX, FLOW_DATA, WATER_INDEX, WATER_DATA, BASIN_TABLE, so
// each index sits immediately before its own multi-megabyte data section. "The
// first 65 KB" is 20 KB of index and 44 KB of compressed elevation, not the
// preamble. A ground-only client (wantFlow=wantWater=false) gets its whole
// preamble in ONE request of 20,663 B of content.
//
// `fileSize` must be the file's true length (FileRangeSource::fileSize(), or a
// 206's Content-Range total). Returns false, with `err` set, if the tile is not
// a v2 .vxtl, if a declared section is missing, or on any read failure.
//
// `facts` (optional) comes back filled from the head probe whenever there was a
// header to read -- INCLUDING on failure, which is the case it exists for. A
// refusal that cannot say what the file claimed to be cannot distinguish a
// reader that is too old from a file that is broken, and the two want opposite
// responses from whoever reads the log. Pair it with fineDescribeRejection.
bool readFineTilePreamble(RangeSource& src, uint64_t fileSize, const FinePreambleRequest& want,
                          FineTileBytes& out, FineError* err = nullptr,
                          FineHeaderFacts* facts = nullptr);

// Which plane a fetch is for. The three share all their machinery -- one block
// index, one data section, the same entry layout -- so this selects rather than
// branches.
enum class FinePlane : uint8_t { kElevation, kFlow, kWater };

// Fetches (and splices into `tile`) exactly the named blocks of one plane that
// are not already resident. CONSTANT blocks cost no request. Returns false on a
// read failure or if a fetched span overlaps bytes already held; on success
// every requested block satisfies <plane>BlockResident().
//
// Idempotent: blocks already held are filtered out before planning, so calling
// it twice costs zero requests the second time.
bool fetchFineTileBlocks(RangeSource& src, FineTile& tile, FinePlane plane,
                         const std::vector<uint32_t>& blockIds,
                         uint64_t coalesceGap = kFineCoalesceGap);

// The plane's index, or an empty vector when the tile does not carry it.
const std::vector<FineBlockEntry>& finePlaneIndex(const FineTile& tile, FinePlane plane);

// Every block id of `plane` that is NOT CONSTANT -- i.e. the set that costs
// bytes. Handed to fetchFineTileBlocks it makes a plane fully resident, which is
// what a "fetch the whole water plane" refresh wants: 104-231 KB in one
// coalesced span on the shipped tiles, because most of the plane is CONSTANT
// and the rest is contiguous.
std::vector<uint32_t> fineNonConstantBlocks(const FineTile& tile, FinePlane plane);

} // namespace vxc
