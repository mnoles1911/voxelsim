#pragma once
// Offline edit-log compaction (plan §3.2: "edit log is append-only,
// compacted offline"; doctrine §2.1: "only diffs are ever stored").
//
// Compaction collapses each touched brick's edit history into a single
// last-write-wins entry over the cells that were actually touched. It never
// synthesizes untouched cells (no full-brick bake), so compacted logs stay
// small even for bricks whose whole content happens to be edited by many
// separate small edits over time. Replaying a compacted log must reproduce
// the exact same brick contents as replaying the source log.

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "voxelcore/brick.h"
#include "voxelcore/editlog.h"

namespace vxc {

// Produces a new EditLog where every brick touched anywhere in `source` has
// exactly one entry: the union of that brick's touched cells, each holding
// its last-written value. Entries are ordered by BrickKeyLess and sequence
// numbers are renumbered contiguously from 0. Seed and brickEdge are
// preserved from `source`.
inline EditLog compactLog(const EditLog& source) {
    // Accumulate every cell write per brick in original seq order so that,
    // once normalized, "last write wins" resolves to the most recent write
    // in time: EditLog::normalize() stable-sorts by cell index and keeps the
    // LAST of any equal-index run, and a stable sort preserves the relative
    // (i.e. chronological) order of writes to the same cell.
    std::unordered_map<BrickKey, std::vector<EditCell>, BrickKeyHash> byBrick;
    for (const EditEntry& e : source.entries()) {
        std::vector<EditCell>& cells = byBrick[e.key];
        cells.insert(cells.end(), e.cells.begin(), e.cells.end());
    }

    std::vector<BrickKey> keys;
    keys.reserve(byBrick.size());
    for (const auto& [key, cells] : byBrick) keys.push_back(key);
    std::sort(keys.begin(), keys.end(), BrickKeyLess{});

    EditLog out(source.seed(), source.brickEdge());
    for (const BrickKey& key : keys) {
        // append() normalizes (sorted-unique, last-write-wins) internally
        // and assigns the next contiguous sequence number.
        out.append(key, std::move(byBrick[key]));
    }
    return out;
}

} // namespace vxc
