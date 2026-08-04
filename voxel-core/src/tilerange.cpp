#include "voxelcore/tilerange.h"

#include <algorithm>
#include <cstddef>  // std::ptrdiff_t
#include <utility>  // std::pair, std::move

// See voxelcore/tilerange.h for the contract and for why the saving this buys
// today is disk bytes and peak memory rather than network transfer.
//
// Nothing here uses floating point (doctrine: voxel-core src/ and include/ are
// integer-only) and nothing here allocates a whole tile.

namespace vxc {

namespace {

// One block's file span, or length 0 when it owns no bytes (CONSTANT). The
// bounds check against the data section is repeated here even though
// FineTile::parse already made it, so the planner is safe on its own terms --
// it is handed an index and an offset, not a validated tile.
ByteRange blockSpan(const FineBlockEntry& e, uint64_t dataOffset) {
    if (e.mode == kBlockConstant || e.compLen == 0) return ByteRange{0, 0};
    return ByteRange{dataOffset + e.offset, e.compLen};
}

} // namespace

std::vector<RangePlan> planBlockRanges(const std::vector<FineBlockEntry>& index,
                                       uint64_t dataOffset,
                                       const std::vector<uint32_t>& blockIds,
                                       uint64_t coalesceGap) {
    // De-duplicate FIRST. A repeated id would otherwise be counted twice in
    // usefulBytes and make wastedBytes -- the number that justifies the
    // coalescing gap -- read low, which is the one direction a measurement must
    // not be wrong in.
    std::vector<uint32_t> ids(blockIds);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    // (span, blockId) for every block that actually costs bytes.
    std::vector<std::pair<ByteRange, uint32_t>> wanted;
    wanted.reserve(ids.size());
    for (uint32_t id : ids) {
        if (id >= index.size()) continue;
        const ByteRange r = blockSpan(index[id], dataOffset);
        if (r.length == 0) continue;   // CONSTANT: served from the index, no request
        wanted.push_back({r, id});
    }
    if (wanted.empty()) return {};

    // BY FILE OFFSET. Not by (by,bx) -- see the header: CONSTANT blocks hole
    // the coordinate sequence but not the file.
    std::sort(wanted.begin(), wanted.end(),
              [](const std::pair<ByteRange, uint32_t>& a, const std::pair<ByteRange, uint32_t>& b) {
                  if (a.first.start != b.first.start) return a.first.start < b.first.start;
                  return a.first.length < b.first.length;
              });

    std::vector<RangePlan> plans;
    RangePlan cur;
    cur.span = wanted[0].first;
    cur.blocks.push_back(wanted[0].second);
    cur.usefulBytes = wanted[0].first.length;

    for (size_t i = 1; i < wanted.size(); ++i) {
        const ByteRange& r = wanted[i].first;
        const uint64_t curEnd = cur.span.end();
        // `<=` on purpose: a gap of exactly coalesceGap is at break-even and
        // merging it saves the seek.
        if (r.start <= curEnd + coalesceGap) {
            cur.usefulBytes += r.length;
            cur.span.length = std::max(curEnd, r.end()) - cur.span.start;
            cur.blocks.push_back(wanted[i].second);
        } else {
            plans.push_back(std::move(cur));
            cur = RangePlan{};
            cur.span = r;
            cur.blocks.push_back(wanted[i].second);
            cur.usefulBytes = r.length;
        }
    }
    plans.push_back(std::move(cur));
    return plans;
}

// --- transport --------------------------------------------------------------

FileRangeSource::FileRangeSource(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    if (ec) return;
    f_.open(path, std::ios::binary);
    if (!f_.is_open()) return;
    size_ = static_cast<uint64_t>(sz);
}

bool FileRangeSource::readClamped(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    if (!f_.is_open()) return false;
    ++requests;
    if (offset >= size_) return length == 0;
    const uint64_t n = std::min(length, size_ - offset);
    const size_t base = out.size();
    out.resize(base + static_cast<size_t>(n));
    f_.clear();
    f_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!f_) return false;
    f_.read(reinterpret_cast<char*>(out.data() + base), static_cast<std::streamsize>(n));
    const uint64_t got = static_cast<uint64_t>(f_.gcount());
    out.resize(base + static_cast<size_t>(got));
    bytesRead += got;
    return got == n;
}

bool FileRangeSource::read(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    // Strict: a range past the end of the file is a planning bug, not something
    // to paper over by returning fewer bytes than the caller will index into.
    if (offset > size_ || length > size_ - offset) {
        ++requests;
        return false;
    }
    return readClamped(offset, length, out);
}

bool BytesRangeSource::readClamped(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    ++requests;
    const uint64_t size = data_.size();
    if (offset >= size) return length == 0;
    const uint64_t n = std::min(length, size - offset);
    out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset),
               data_.begin() + static_cast<std::ptrdiff_t>(offset + n));
    bytesRead += n;
    return true;
}

bool BytesRangeSource::read(uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    const uint64_t size = data_.size();
    if (offset > size || length > size - offset) {
        ++requests;
        return false;
    }
    return readClamped(offset, length, out);
}

// --- preamble ---------------------------------------------------------------

bool readFineTilePreamble(RangeSource& src, uint64_t fileSize, const FinePreambleRequest& want,
                          FineTileBytes& out, FineError* err) {
    const auto reject = [err](FineError e) {
        if (err) *err = e;
        return false;
    };
    if (err) *err = FineError::kNone;
    if (fileSize < kFineHeaderBytes) return reject(FineError::kNotVxtl);

    out = FineTileBytes::forFile(fileSize);

    // Round one: the head probe. Clamped, because it deliberately asks for more
    // than it knows to be there.
    std::vector<uint8_t> head;
    if (!src.readClamped(0, std::min(want.headProbeBytes, fileSize), head)) {
        return reject(FineError::kFileUnreadable);
    }
    uint16_t flags = 0;
    uint64_t tableFileSize = 0;
    std::vector<FineSectionEntry> sections;
    if (!readFineSectionTable(head.data(), head.size(), flags, tableFileSize, sections)) {
        // Either not a v2 .vxtl, or the probe was too small to reach the end of
        // the section table. The second is recoverable by re-probing larger, but
        // it cannot happen with any layout this format permits at the shipped
        // block grid (183 B of header+table against a 32 KB probe), so it is
        // reported rather than silently retried -- a silent retry would hide a
        // format change behind a doubled request count.
        return reject(FineError::kNotVxtl);
    }
    // The section table's own idea of the file length must match the transport's.
    // A mismatch is a TRUNCATED (or overlong) file, which is the failure a
    // whole-file read catches for free and a ranged read otherwise would not:
    // every individual range would succeed and the missing tail would simply
    // never be asked for.
    if (tableFileSize != fileSize) return reject(FineError::kBadSectionTable);

    const auto find = [&sections](uint32_t id) -> const FineSectionEntry* {
        for (const FineSectionEntry& s : sections) {
            if (s.id == id) return &s;
        }
        return nullptr;
    };

    std::vector<uint32_t> wantIds{kSectionElevIndex};
    if (want.wantFlow && (flags & kFineFlagFlowPresent)) wantIds.push_back(kSectionFlowIndex);
    if (want.wantWater && (flags & kFineFlagWaterPresent)) wantIds.push_back(kSectionWaterIndex);
    if (want.wantBasins && (flags & kFineFlagBasinsPresent)) wantIds.push_back(kSectionBasinTable);

    // The header and section table themselves, straight out of the probe.
    const uint64_t fixedBytes =
        kFineHeaderBytes + static_cast<uint64_t>(sections.size()) * kFineSectionEntryBytes;
    if (head.size() < fixedBytes) return reject(FineError::kNotVxtl);
    if (!out.addSegment(0, std::vector<uint8_t>(head.begin(),
                                                head.begin() + static_cast<std::ptrdiff_t>(
                                                                   fixedBytes)))) {
        return reject(FineError::kBadSectionTable);
    }

    // Round two: whatever the probe did not already cover. Each its own request
    // because encode_v2 puts a multi-megabyte data section between them.
    for (uint32_t id : wantIds) {
        const FineSectionEntry* s = find(id);
        if (s == nullptr) return reject(FineError::kBadSectionTable);
        if (s->length == 0) continue;
        if (s->offset + s->length <= head.size()) {
            // Already paid for by the probe -- no request at all.
            std::vector<uint8_t> blob(
                head.begin() + static_cast<std::ptrdiff_t>(s->offset),
                head.begin() + static_cast<std::ptrdiff_t>(s->offset + s->length));
            if (!out.addSegment(s->offset, std::move(blob))) {
                return reject(FineError::kBadSectionTable);
            }
            continue;
        }
        std::vector<uint8_t> blob;
        if (!src.read(s->offset, s->length, blob)) return reject(FineError::kFileUnreadable);
        if (!out.addSegment(s->offset, std::move(blob))) {
            return reject(FineError::kBadSectionTable);
        }
    }
    return true;
}

// --- block fetch ------------------------------------------------------------

const std::vector<FineBlockEntry>& finePlaneIndex(const FineTile& tile, FinePlane plane) {
    switch (plane) {
    case FinePlane::kFlow: return tile.flowIndex();
    case FinePlane::kWater: return tile.waterIndex();
    case FinePlane::kElevation: break;
    }
    return tile.elevIndex();
}

namespace {

uint64_t planeDataOffset(const FineTile& tile, FinePlane plane) {
    switch (plane) {
    case FinePlane::kFlow: return tile.flowDataOffset();
    case FinePlane::kWater: return tile.waterDataOffset();
    case FinePlane::kElevation: break;
    }
    return tile.elevDataOffset();
}

bool planeBlockResident(const FineTile& tile, FinePlane plane, uint32_t id) {
    const uint32_t perAxis = tile.blocksPerAxis();
    if (perAxis == 0) return false;
    const uint32_t bx = id % perAxis, by = id / perAxis;
    switch (plane) {
    case FinePlane::kFlow: return tile.flowBlockResident(bx, by);
    case FinePlane::kWater: return tile.waterBlockResident(bx, by);
    case FinePlane::kElevation: break;
    }
    return tile.elevBlockResident(bx, by);
}

} // namespace

std::vector<uint32_t> fineNonConstantBlocks(const FineTile& tile, FinePlane plane) {
    const std::vector<FineBlockEntry>& index = finePlaneIndex(tile, plane);
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < index.size(); ++i) {
        if (index[i].mode != kBlockConstant && index[i].compLen != 0) out.push_back(i);
    }
    return out;
}

bool fetchFineTileBlocks(RangeSource& src, FineTile& tile, FinePlane plane,
                         const std::vector<uint32_t>& blockIds, uint64_t coalesceGap) {
    const std::vector<FineBlockEntry>& index = finePlaneIndex(tile, plane);
    if (index.empty()) return blockIds.empty();   // plane not carried by this tile

    // Drop what is already held BEFORE planning, which is what makes this
    // idempotent and what keeps a second call from re-requesting bytes and
    // being refused by the overlap check in FineTileBytes::addSegment.
    std::vector<uint32_t> todo;
    todo.reserve(blockIds.size());
    for (uint32_t id : blockIds) {
        if (id >= index.size()) continue;
        if (planeBlockResident(tile, plane, id)) continue;
        todo.push_back(id);
    }
    if (todo.empty()) return true;

    const std::vector<RangePlan> plans =
        planBlockRanges(index, planeDataOffset(tile, plane), todo, coalesceGap);
    for (const RangePlan& p : plans) {
        std::vector<uint8_t> blob;
        if (!src.read(p.span.start, p.span.length, blob)) return false;
        if (blob.size() != p.span.length) return false;
        if (!tile.addFetchedBytes(p.span.start, std::move(blob))) return false;
    }
    return true;
}

} // namespace vxc
