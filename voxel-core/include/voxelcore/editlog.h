#pragma once
// The edit log (doctrine §2.1/§2.4): the ONE authority path for world changes.
// Append-only, versioned, RLE-compressed brick diffs. Everything — player
// digs, NPC builds, water, settlements — becomes entries here; replication,
// persistence, LOD invalidation and rendering all hang off this mechanism.
//
// Invariant (replay test): world(tiles) + log == world(tiles) + log, always.

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voxelcore/brick.h"
#include "voxelcore/bytes.h"

namespace vxc {

struct EditCell {
    uint16_t cell; // Brick<B>::cellIndex
    MaterialId mat;
    friend bool operator==(const EditCell&, const EditCell&) = default;
};

struct EditEntry {
    uint64_t seq = 0; // server-assigned, contiguous from 0
    BrickKey key;
    std::vector<EditCell> cells; // sorted by cell index, unique
    friend bool operator==(const EditEntry&, const EditEntry&) = default;
};

// --- the log ---

class EditLog {
public:
    // Format history:
    //   1 - magic, fmt, wgen, seed, brickEdge, count, entries.
    //   2 - adds providerId (u16 length + bytes) right after brickEdge, so
    //       a replay knows which tile-content-addressed provider (checkpoint
    //       sha256 + sampler + scale, see terrain-service's
    //       DiffusionConfig.provider_id()) the log's diffs were recorded
    //       against. parse() still reads version-1 files (providerId reads
    //       as "" -- see checkProvider()'s kUnstamped result) so existing
    //       logs on disk keep loading; only NEW logs are written as v2.
    static constexpr uint32_t kFormatVersion = 2;
    static constexpr uint32_t kMinReadableFormatVersion = 1;
    static constexpr uint32_t kMagic = 0x4C455856; // "VXEL" little-endian

    // Result of comparing a log's stamped providerId against the provider
    // that is about to replay it. kUnstamped is NOT a failure: version-1
    // logs (and any v2 log deliberately written with providerId="") predate
    // or opt out of the stamp, so callers should warn loudly and proceed --
    // only kMismatch should refuse a replay outright (see doc comment on
    // checkProvider()).
    enum class ProviderCheck { kMatch, kUnstamped, kMismatch };

    EditLog(uint64_t seed, uint8_t brickEdge, std::string providerId = {})
        : seed_(seed), brickEdge_(brickEdge), providerId_(std::move(providerId)) {}

    uint64_t seed() const { return seed_; }
    uint8_t brickEdge() const { return brickEdge_; }
    // Content-addressed identity of the tile provider this log's diffs were
    // recorded against ("" means unstamped: a pre-v2 log, or a caller that
    // never had a provider identity, e.g. tests). See ProviderCheck.
    const std::string& providerId() const { return providerId_; }
    const std::vector<EditEntry>& entries() const { return entries_; }
    size_t size() const { return entries_.size(); }

    // Compare this log's stamped providerId against `currentProviderId` (the
    // identity of the world/tile-set about to replay it). Doctrine: an
    // edit-log is a diff against a specific base world, so replaying it
    // against a DIFFERENT tile set silently lands edits on terrain that no
    // longer matches. kMismatch means the caller MUST refuse the replay;
    // kUnstamped means the log predates provider stamping and the caller
    // should warn but MAY proceed (refusing outright would hard-fail every
    // log recorded before this feature existed).
    ProviderCheck checkProvider(const std::string& currentProviderId) const {
        if (providerId_.empty()) return ProviderCheck::kUnstamped;
        return providerId_ == currentProviderId ? ProviderCheck::kMatch : ProviderCheck::kMismatch;
    }

    // Appends with the next sequence number. Cells are normalized to
    // sorted-unique, last write wins.
    const EditEntry& append(const BrickKey& key, std::vector<EditCell> cells) {
        normalize(cells);
        entries_.push_back(EditEntry{entries_.size(), key, std::move(cells)});
        return entries_.back();
    }

    static void normalize(std::vector<EditCell>& cells) {
        // Stable sort by cell index, then keep the LAST write per cell.
        std::stable_sort(cells.begin(), cells.end(),
                         [](const EditCell& a, const EditCell& b) { return a.cell < b.cell; });
        auto out = cells.begin();
        for (auto it = cells.begin(); it != cells.end(); ++it) {
            auto next = it + 1;
            if (next != cells.end() && next->cell == it->cell) continue; // superseded
            *out++ = *it;
        }
        cells.erase(out, cells.end());
    }

    void serialize(std::vector<uint8_t>& out) const {
        ByteWriter w(out);
        w.u32(kMagic);
        w.u32(kFormatVersion);
        w.u32(kWorldGenVersion);
        w.u64(seed_);
        w.u8(brickEdge_);
        writeString(w, providerId_);
        w.u64(entries_.size());
        const uint32_t cellsPerBrick =
            uint32_t(brickEdge_) * brickEdge_ * brickEdge_;
        for (const EditEntry& e : entries_) {
            w.u64(e.seq);
            w.i32(e.key.x);
            w.i32(e.key.y);
            w.i32(e.key.z);
            writePayload(w, e, cellsPerBrick);
        }
    }

    static std::optional<EditLog> parse(const uint8_t* data, size_t size) {
        ByteReader r(data, size);
        uint32_t magic, fmt, wgen;
        uint64_t seed, count;
        uint8_t edge;
        if (!r.u32(magic) || magic != kMagic) return std::nullopt;
        if (!r.u32(fmt) || fmt < kMinReadableFormatVersion || fmt > kFormatVersion)
            return std::nullopt;
        if (!r.u32(wgen) || wgen != kWorldGenVersion) return std::nullopt;
        if (!r.u64(seed) || !r.u8(edge)) return std::nullopt;
        if (edge != 8 && edge != 16) return std::nullopt;
        std::string providerId; // stays "" for fmt==1 (pre-provider-stamp logs)
        if (fmt >= 2 && !readString(r, providerId)) return std::nullopt;
        if (!r.u64(count)) return std::nullopt;
        EditLog log(seed, edge, std::move(providerId));
        const uint32_t cellsPerBrick = uint32_t(edge) * edge * edge;
        for (uint64_t i = 0; i < count; ++i) {
            EditEntry e;
            if (!r.u64(e.seq) || !r.i32(e.key.x) || !r.i32(e.key.y) || !r.i32(e.key.z))
                return std::nullopt;
            if (e.seq != i) return std::nullopt; // contiguous from 0
            if (!readPayload(r, e, cellsPerBrick)) return std::nullopt;
            log.entries_.push_back(std::move(e));
        }
        if (!r.atEnd()) return std::nullopt;
        return log;
    }

    // WHAT A REFUSED FILE IS CARRYING.
    //
    // parse() answers exactly one bit -- usable or not -- which is the right
    // answer for a loader and the WRONG answer for the human who has to decide
    // what to do next. "Written by an older kWorldGenVersion" and "the bytes
    // are damaged" both arrive as std::nullopt, and they want OPPOSITE
    // responses: the first wants a migration written against intact data, the
    // second wants a restore. A caller that cannot tell them apart logs "corrupt
    // or unrecognized" for both and loses the distinction forever.
    //
    // peekHeader reads ONLY the fixed-size prefix, allocates nothing, and
    // reports each field it MANAGED to read (haveX false = the file ended
    // before that field). It is total: any byte sequence, including an empty
    // one, produces an answer.
    //
    // IT LIVES BESIDE parse() ON PURPOSE. The prefix layout is written down
    // twice -- once here, once in parse() -- and putting the two within a
    // screen of each other is what keeps them from detaching. Anything that
    // changes serialize()'s header must change both, and will see both.
    struct HeaderPeek {
        bool haveMagic = false;
        uint32_t magic = 0;
        bool haveFormat = false;
        uint32_t format = 0;
        bool haveWorldGen = false;
        uint32_t worldGen = 0;
        bool haveSeed = false;
        uint64_t seed = 0;
        bool haveBrickEdge = false;
        uint8_t brickEdge = 0;
    };

    static HeaderPeek peekHeader(const uint8_t* data, size_t size) {
        HeaderPeek h;
        if (data == nullptr) return h;
        ByteReader r(data, size);
        if (!(h.haveMagic = r.u32(h.magic))) return h;
        if (!(h.haveFormat = r.u32(h.format))) return h;
        if (!(h.haveWorldGen = r.u32(h.worldGen))) return h;
        if (!(h.haveSeed = r.u64(h.seed))) return h;
        h.haveBrickEdge = r.u8(h.brickEdge);
        return h;
    }

private:
    enum PayloadMode : uint8_t { kSparse = 0, kRle = 1 };

    // ByteWriter/ByteReader (voxelcore/bytes.h) are shared with the tile
    // wire format and only speak fixed-width integers; providerId is the
    // only string field in this format, so it gets its own tiny
    // length-prefixed (uint16) codec here rather than growing the shared
    // helper for one caller.
    static void writeString(ByteWriter& w, const std::string& s) {
        w.u16(static_cast<uint16_t>(s.size()));
        for (unsigned char c : s) w.u8(c);
    }

    static bool readString(ByteReader& r, std::string& s) {
        uint16_t n;
        if (!r.u16(n)) return false;
        s.clear();
        s.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            uint8_t c;
            if (!r.u8(c)) return false;
            s.push_back(static_cast<char>(c));
        }
        return true;
    }

    static void writePayload(ByteWriter& w, const EditEntry& e, uint32_t cellsPerBrick) {
        // RLE only encodes full-brick rewrites; pick it when smaller.
        if (e.cells.size() == cellsPerBrick) {
            std::vector<std::pair<MaterialId, uint16_t>> runs;
            for (const EditCell& c : e.cells) {
                if (!runs.empty() && runs.back().first == c.mat && runs.back().second < 0xffff)
                    ++runs.back().second;
                else
                    runs.push_back({c.mat, 1});
            }
            if (2 + 3 * runs.size() < 2 + 3 * e.cells.size()) {
                w.u8(kRle);
                w.u16(static_cast<uint16_t>(runs.size()));
                for (auto& [mat, len] : runs) {
                    w.u8(mat);
                    w.u16(len);
                }
                return;
            }
        }
        w.u8(kSparse);
        w.u16(static_cast<uint16_t>(e.cells.size()));
        for (const EditCell& c : e.cells) {
            w.u16(c.cell);
            w.u8(c.mat);
        }
    }

    static bool readPayload(ByteReader& r, EditEntry& e, uint32_t cellsPerBrick) {
        uint8_t mode;
        uint16_t n;
        if (!r.u8(mode) || !r.u16(n)) return false;
        if (mode == kSparse) {
            e.cells.resize(n);
            uint16_t prev = 0;
            for (uint16_t i = 0; i < n; ++i) {
                if (!r.u16(e.cells[i].cell) || !r.u8(e.cells[i].mat)) return false;
                if (e.cells[i].cell >= cellsPerBrick) return false;
                if (i > 0 && e.cells[i].cell <= prev) return false; // sorted unique
                prev = e.cells[i].cell;
            }
            return true;
        }
        if (mode == kRle) {
            uint32_t cell = 0;
            for (uint16_t i = 0; i < n; ++i) {
                uint8_t mat;
                uint16_t len;
                if (!r.u8(mat) || !r.u16(len)) return false;
                for (uint16_t k = 0; k < len; ++k) {
                    if (cell >= cellsPerBrick) return false;
                    e.cells.push_back({static_cast<uint16_t>(cell++), mat});
                }
            }
            return cell == cellsPerBrick;
        }
        return false;
    }

    uint64_t seed_;
    uint8_t brickEdge_;
    std::string providerId_;
    std::vector<EditEntry> entries_;
};

} // namespace vxc
