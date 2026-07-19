#pragma once
// World = deterministic generated function + edit overlay + edit log
// (doctrine §2.1): voxel(x,y,z) = f(seed, x, y, z) patched by replayed diffs.
// The overlay stores only bricks that have ever been edited; everything else
// is evaluated lazily from the amplifier.

#include "voxelcore/editlog.h"
#include "voxelcore/generator.h"

namespace vxc {

template <int B>
class World {
public:
    World(uint64_t seed, ITileSampler& tiles)
        : amp_(seed, tiles), gen_(amp_), log_(seed, B) {}

    const Amplifier& amplifier() const { return amp_; }
    const GeneratedWorld<B>& generated() const { return gen_; }
    const EditLog& log() const { return log_; }
    const ChunkMap<B>& editedBricks() const { return overlay_; }

    MaterialId materialAt(int64_t vx, int64_t vy, int64_t vz) const {
        const BrickKey key = ChunkMap<B>::keyForVoxel(vx, vy, vz);
        if (const Brick<B>* b = overlay_.find(key))
            return b->get(static_cast<int>(floorMod(vx, B)),
                          static_cast<int>(floorMod(vy, B)),
                          static_cast<int>(floorMod(vz, B)));
        return gen_.materialAt(vx, vy, vz);
    }

    // Materialized brick: edited version if present, else generated.
    Brick<B> brickAt(const BrickKey& key) const {
        if (const Brick<B>* b = overlay_.find(key)) return *b;
        return gen_.makeBrick(key);
    }

    // Authority path: append to the log, then apply to the overlay. Returns
    // the assigned sequence number.
    uint64_t applyEdit(const BrickKey& key, std::vector<EditCell> cells) {
        const EditEntry& e = log_.append(key, std::move(cells));
        applyToOverlay(e);
        return e.seq;
    }

    // Convenience for single-voxel edits in world coordinates.
    uint64_t setVoxel(int64_t vx, int64_t vy, int64_t vz, MaterialId mat) {
        const BrickKey key = ChunkMap<B>::keyForVoxel(vx, vy, vz);
        const uint16_t cell = static_cast<uint16_t>(
            Brick<B>::cellIndex(static_cast<int>(floorMod(vx, B)),
                                static_cast<int>(floorMod(vy, B)),
                                static_cast<int>(floorMod(vz, B))));
        return applyEdit(key, {{cell, mat}});
    }

    // Replay a parsed log into a fresh world. Fails (returns false) on
    // seed/brick-size mismatch — a log only replays against the world that
    // produced it.
    bool replay(const EditLog& log) {
        if (log.seed() != amp_.seed() || log.brickEdge() != B) return false;
        for (const EditEntry& e : log.entries()) {
            log_.append(e.key, e.cells);
            applyToOverlay(e);
        }
        return true;
    }

    // Deterministic digest over the edited bricks (sorted key order) — used by
    // the replay identity test.
    uint64_t editedDigest() const {
        std::vector<BrickKey> keys;
        keys.reserve(overlay_.size());
        for (const auto& [key, brick] : overlay_) keys.push_back(key);
        std::sort(keys.begin(), keys.end(), BrickKeyLess{});
        Digest d;
        for (const BrickKey& key : keys) {
            d.u64(static_cast<uint32_t>(key.x));
            d.u64(static_cast<uint32_t>(key.y));
            d.u64(static_cast<uint32_t>(key.z));
            overlay_.find(key)->digest(d);
        }
        return d.h;
    }

private:
    void applyToOverlay(const EditEntry& e) {
        Brick<B>* b = overlay_.find(e.key);
        if (!b) b = &overlay_.insert(e.key, gen_.makeBrick(e.key));
        for (const EditCell& c : e.cells) {
            const int x = c.cell % B, y = (c.cell / B) % B, z = c.cell / (B * B);
            b->set(x, y, z, c.mat);
        }
        b->tryCollapse();
    }

    Amplifier amp_;
    GeneratedWorld<B> gen_;
    ChunkMap<B> overlay_;
    EditLog log_;
};

} // namespace vxc
