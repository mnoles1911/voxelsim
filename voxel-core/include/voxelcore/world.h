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
    // `providerId` stamps the world's OWN log (see EditLog::providerId) with
    // the identity of the tile provider `tiles` was sourced from — "" (the
    // default) leaves it unstamped, matching pre-existing callers exactly.
    World(uint64_t seed, ITileSampler& tiles, std::string providerId = {})
        : amp_(seed, tiles), gen_(amp_), log_(seed, B, std::move(providerId)) {}

    const Amplifier& amplifier() const { return amp_; }

    // DEBUG WATER MARKER passthrough. `amplifier()` is deliberately const --
    // nothing in normal operation may reconfigure worldgen after construction
    // -- so this is the one narrow door, named for exactly what it does rather
    // than handing out a mutable Amplifier.
    //
    // MUST be called during bring-up, before any worker touches the world.
    // Amplifier::setWaterMarker documents why; the practical reason here is
    // that a session-lifetime brick cache would otherwise serve pre-marker
    // bricks alongside marked ones.
    void setWaterMarker(IWaterSampler* sampler, bool includeOcean = true) {
        amp_.setWaterMarker(sampler, includeOcean);
    }
    void setWaterMarkerFillPx(int64_t px) { amp_.setWaterMarkerFillPx(px); }
    // See Amplifier::waterMarkerColumnsMarked -- the two numbers that separate
    // "the marker is not wired up" from "the camera never looked at water" from
    // "the water is there and something downstream will not draw it".
    int64_t waterMarkerColumnsQueried() const { return amp_.waterMarkerColumnsQueried(); }
    int64_t waterMarkerColumnsMarked() const { return amp_.waterMarkerColumnsMarked(); }
    int64_t waterMarkerColumnsAboveGround() const { return amp_.waterMarkerColumnsAboveGround(); }
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
    // produced it. `currentProviderId`, if non-empty, is checked against the
    // log's stamped provider (EditLog::checkProvider): a real MISMATCH
    // refuses the replay (returns false) since the log's diffs were
    // recorded against different terrain; an UNSTAMPED log (recorded before
    // provider stamping existed) is not refused, only flagged — inspect
    // lastProviderCheck() to warn the caller. Passing "" (the default)
    // skips the check entirely, preserving old behavior for callers that
    // have no provider identity to check against yet.
    bool replay(const EditLog& log, const std::string& currentProviderId = {}) {
        if (log.seed() != amp_.seed() || log.brickEdge() != B) return false;
        lastProviderCheck_ = currentProviderId.empty()
            ? std::nullopt
            : std::optional<EditLog::ProviderCheck>(log.checkProvider(currentProviderId));
        if (lastProviderCheck_ == EditLog::ProviderCheck::kMismatch) return false;
        for (const EditEntry& e : log.entries()) {
            log_.append(e.key, e.cells);
            applyToOverlay(e);
        }
        return true;
    }

    // Result of the most recent replay()'s provider check: nullopt if no
    // currentProviderId was passed (check skipped), else the ProviderCheck
    // verdict (kMatch/kUnstamped/kMismatch — see EditLog::checkProvider).
    std::optional<EditLog::ProviderCheck> lastProviderCheck() const { return lastProviderCheck_; }

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
    std::optional<EditLog::ProviderCheck> lastProviderCheck_;
};

} // namespace vxc
