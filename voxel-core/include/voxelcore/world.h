#pragma once
// World = deterministic generated function + edit overlay + edit log
// (doctrine §2.1): voxel(x,y,z) = f(seed, x, y, z) patched by replayed diffs.
// The overlay stores only bricks that have ever been edited; everything else
// is evaluated lazily from the amplifier.

#include "voxelcore/craftlattice.h"
#include "voxelcore/editlog.h"
#include "voxelcore/generator.h"

namespace vxc {

template <int B>
class World {
public:
    // THE CRAFT LATTICE NEEDS B == 8, AND World<16> IS A REAL CONFIGURATION.
    // A craft chunk is one terrain brick only when 4 * B == 32 (see
    // craftlattice.h). World<16> is instantiated in nineteen places across the
    // edit-log and compaction tests, and it exists to prove the template works
    // at a second brick size -- so the craft half is switched off there with
    // `if constexpr` rather than the whole class being made a compile error.
    //
    // Every craft ENTRY POINT below still static_asserts, so calling one on a
    // World<16> is a clear compile error naming this constant, not a silent
    // no-op that appends log entries nothing will ever apply.
    static constexpr bool kCraftSupported =
        (kCraftCellsPerVoxel * B == kCraftChunkEdgeCells);
    // `providerId` stamps the world's OWN log (see EditLog::providerId) with
    // the identity of the tile provider `tiles` was sourced from — "" (the
    // default) leaves it unstamped, matching pre-existing callers exactly.
    // providerId is COPIED into the terrain log and MOVED into the craft log,
    // in that order, because members initialise in declaration order and a
    // moved-from string would leave the craft log unstamped -- which
    // downgrades a hard provider mismatch into a warning on exactly the half
    // of the save that carries the player's building.
    World(uint64_t seed, ITileSampler& tiles, std::string providerId = {})
        : amp_(seed, tiles), gen_(amp_), log_(seed, B, providerId),
          craftLog_(seed, B, std::move(providerId), static_cast<uint32_t>(kCraftPitchMm)) {}

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

    // THE ASSET TERM, and the same narrow door for the same reason.
    //
    // `generated()` is const because nothing in normal operation may
    // reconfigure worldgen after construction, and the asset field is worldgen:
    // it feeds makeBrick, which is what applyToOverlay rebuilds an edited brick
    // from, and materialAt, which is what raycasts and digging read. Installing
    // one mid-session would make the same brick generate differently before and
    // after, so this must be called during bring-up, before any worker touches
    // the world -- exactly as setWaterMarker must.
    //
    // The field is NOT owned. It holds decoded species banks that outlive any
    // one world, which is the whole point of residency being per bank.
    void setAssetField(const AssetField* field) { gen_.setAssetField(field); }
    const AssetField* assetField() const { return gen_.assetField(); }
    // The channel source is worldgen exactly as the field is (see
    // GeneratedWorld::setAssetChannelSource): same narrow door, same bring-up
    // rule. assetChannelsAt is forwarded so host-side parallel samplers (the
    // UE meshers, exact admission) resolve facts through the SAME source the
    // brick/materialAt paths compose with -- one binding, no drift.
    void setAssetChannelSource(IAssetChannelSource* src) { gen_.setAssetChannelSource(src); }
    AssetColumnChannels assetChannelsAt(int64_t vx, int64_t vy) const {
        return gen_.assetChannelsAt(vx, vy);
    }
    // See Amplifier::waterMarkerColumnsMarked -- the two numbers that separate
    // "the marker is not wired up" from "the camera never looked at water" from
    // "the water is there and something downstream will not draw it".
    int64_t waterMarkerColumnsQueried() const { return amp_.waterMarkerColumnsQueried(); }
    int64_t waterMarkerColumnsMarked() const { return amp_.waterMarkerColumnsMarked(); }
    int64_t waterMarkerColumnsAboveGround() const { return amp_.waterMarkerColumnsAboveGround(); }
    const GeneratedWorld<B>& generated() const { return gen_; }
    const EditLog& log() const { return log_; }
    const ChunkMap<B>& editedBricks() const { return overlay_; }

    // --- the craft lattice (voxelcore/craftlattice.h) ----------------------
    //
    // A SECOND STREAM, NOT A SECOND WORLD. The craft log is the authority for
    // craft cells and for the coarse PROJECTION those cells imply; the terrain
    // log stays the authority for everything else. Replay order is terrain
    // first, then craft -- see replayCraft().
    const CraftLattice<B>& craftLattice() const { return craft_; }
    const EditLog& craftLog() const { return craftLog_; }
    uint64_t craftDigest() const { return craft_.digest(); }

    // Material at a craft cell. Falls back to the TERRAIN voxel containing the
    // cell when its brick is not promoted, so a caller can read the world at
    // 25 mm everywhere without first asking whether anyone has chiselled here.
    // Inside a promoted brick the craft lattice is authoritative.
    MaterialId craftMaterialAt(int64_t cx, int64_t cy, int64_t cz) const {
        const BrickKey tb = terrainBrickOfCraftBrick(craftBrickKeyOfCell(cx, cy, cz));
        if (!craft_.isPromoted(tb))
            return materialAt(voxelOfCraftCell(cx), voxelOfCraftCell(cy), voxelOfCraftCell(cz));
        return craft_.materialAt(cx, cy, cz);
    }

    bool isPromoted(const BrickKey& terrainBrick) const { return craft_.isPromoted(terrainBrick); }

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
    // the assigned sequence number IN WHICHEVER STREAM TOOK THE EDIT.
    //
    // ---------------------------------------------------------------------
    // A TERRAIN EDIT INTO A PROMOTED BRICK IS ROUTED TO THE CRAFT STREAM
    // ---------------------------------------------------------------------
    // Two things go wrong if it is not, and both are silent.
    //
    // 1. THE INVARIANT. For a promoted brick the overlay holds project(craft).
    //    A terrain edit written straight to the overlay makes the two disagree
    //    until something reprojects, and the next craft edit anywhere in that
    //    brick silently reverts it.
    //
    // 2. CHRONOLOGY. Two append-only streams have independent sequence
    //    numbers, so a replay cannot interleave them -- it can only run one
    //    then the other. If a dig and a chisel touch the same voxel, whichever
    //    stream replays last wins, and that is not necessarily the one that
    //    happened last. Keeping the craft log the SOLE authority for promoted
    //    bricks removes the interleaving question entirely: terrain-then-craft
    //    replays the real order by construction.
    //
    // A 10 cm edit becomes the 4^3 craft cells of each voxel it touches, which
    // projects back to exactly the material asked for (the same identity that
    // makes promotion a no-op), so the caller sees no difference IN THE WORLD.
    //
    // IT DOES CHANGE WHAT THE RETURN VALUE MEANS, AND THAT IS A TRAP.
    // Unrouted, the result is a sequence number in the TERRAIN log. Routed, it
    // is a sequence number in the CRAFT log -- a different stream with its own
    // independent numbering, starting from its own 0. Same type, same name, two
    // meanings, and which one you get depends on whether a player has ever
    // chiselled that brick.
    //
    // No caller uses it today (the engine's sole call site discards it, and
    // setVoxel only forwards it), so this is a trap for the NEXT caller rather
    // than a live defect. Anything that wants to index the terrain log by this
    // -- replication catch-up via SerializeLogEntriesFrom is the obvious one --
    // must not, because a routed edit puts NO entry in the terrain log at all.
    // Read log().size() / craftLog().size() explicitly instead of inferring a
    // stream from a number that does not name one.
    uint64_t applyEdit(const BrickKey& key, std::vector<EditCell> cells) {
        if constexpr (kCraftSupported) {
            if (craft_.isPromoted(key)) return routeTerrainEditToCraft(key, cells);
        }
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

    // AUTHORITY PATH FOR CRAFT EDITS. `craftBrick` is in CRAFT-brick
    // coordinates (8 craft cells = 20 cm); `cells` are Brick<8> cell indices
    // within it, exactly as a terrain entry is.
    //
    // Promotion is IMPLICIT and happens here, from the terrain brick as it
    // stands right now. That is why replay order matters: the terrain log must
    // already be applied, or a brick promotes from ungraded generated terrain
    // and every later craft cell sits on the wrong base.
    uint64_t applyCraftEdit(const BrickKey& craftBrick, std::vector<EditCell> cells) {
        static_assert(kCraftSupported, "the craft lattice needs terrain brick edge 8");
        const uint64_t seq = applyCraftEditNoProject(craftBrick, std::move(cells));
        writeProjection(terrainBrickOfCraftBrick(craftBrick));
        return seq;
    }

    // Convenience for a single craft cell, in global craft-cell coordinates.
    uint64_t setCraftCell(int64_t cx, int64_t cy, int64_t cz, MaterialId mat) {
        static_assert(kCraftSupported, "the craft lattice needs terrain brick edge 8");
        const BrickKey key = craftBrickKeyOfCell(cx, cy, cz);
        const int64_t e = kMarchBrickEdge;
        const uint16_t cell = static_cast<uint16_t>(Brick<kMarchBrickEdge>::cellIndex(
            static_cast<int>(floorMod(cx, e)), static_cast<int>(floorMod(cy, e)),
            static_cast<int>(floorMod(cz, e))));
        return applyCraftEdit(key, {{cell, mat}});
    }

    // Replay a parsed CRAFT log. MUST run after replay() of the terrain log --
    // promotion reads the terrain brick as it stands, so a craft log replayed
    // first would promote from unedited generated terrain.
    //
    // Refuses a log recorded against a different lattice. brickEdge is 8 on
    // BOTH lattices, so without the pitch check a terrain log would replay
    // here happily and land 10 cm diffs at 25 mm coordinates.
    bool replayCraft(const EditLog& log) {
        static_assert(kCraftSupported, "the craft lattice needs terrain brick edge 8");
        if (log.seed() != amp_.seed() || log.brickEdge() != B) return false;
        if (log.latticePitchMm() != static_cast<uint32_t>(kCraftPitchMm)) return false;
        for (const EditEntry& e : log.entries()) {
            ensurePromoted(terrainBrickOfCraftBrick(e.key));
            craftLog_.append(e.key, e.cells);
            applyCraftToOverlay(e);
        }
        // Project ONCE per touched brick at the end rather than per entry: a
        // long carve session is many entries over few bricks, and a projection
        // is a 64-brick fold.
        for (const BrickKey& tb : craft_.promotedSorted()) writeProjection(tb);
        return true;
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
        // Symmetric to replayCraft's guard, and needed for the same reason:
        // both lattices use 8^3 bricks, so the pitch is the only thing that
        // stops a craft log being replayed as terrain.
        if (log.latticePitchMm() != static_cast<uint32_t>(kVoxelSizeMm)) return false;
        lastProviderCheck_ = currentProviderId.empty()
            ? std::nullopt
            : std::optional<EditLog::ProviderCheck>(log.checkProvider(currentProviderId));
        if (lastProviderCheck_ == EditLog::ProviderCheck::kMismatch) return false;

        // OUT-OF-ORDER GUARD, AND IT IS NOT DEFENSIVE PADDING.
        //
        // applyEdit() routes a terrain edit into the craft stream once its
        // brick is promoted, so a terrain log produced by this engine can
        // never contain an entry for a brick that was promoted at the time it
        // was recorded. Therefore, if a brick named here is ALREADY promoted,
        // the caller has replayed the craft log first -- and applyToOverlay
        // below would write straight over the projection, leaving
        // overlay != project(craft) with no error anywhere. The next chisel in
        // that brick would then silently revert the dig.
        //
        // Checked BEFORE anything is applied, so a refusal leaves the world
        // untouched rather than half-replayed.
        if constexpr (kCraftSupported) {
            for (const EditEntry& e : log.entries())
                if (craft_.isPromoted(e.key)) return false;
        }

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

    // Expand a terrain-lattice edit into craft cells and submit it through
    // the craft authority path. All kCraftCellsPerVoxel^3 cells of one voxel
    // fall inside a SINGLE craft brick (a craft brick is exactly 2 voxels per
    // axis), so this groups by craft brick and emits one entry each.
    uint64_t routeTerrainEditToCraft(const BrickKey& terrainBrick,
                                     const std::vector<EditCell>& cells) {
        std::vector<std::pair<BrickKey, std::vector<EditCell>>> byCraftBrick;
        for (const EditCell& c : cells) {
            const int lx = c.cell % B, ly = (c.cell / B) % B, lz = c.cell / (B * B);
            const int64_t vx = int64_t(terrainBrick.x) * B + lx;
            const int64_t vy = int64_t(terrainBrick.y) * B + ly;
            const int64_t vz = int64_t(terrainBrick.z) * B + lz;
            const int64_t c0x = craftCellOfVoxelMin(vx);
            const int64_t c0y = craftCellOfVoxelMin(vy);
            const int64_t c0z = craftCellOfVoxelMin(vz);
            const BrickKey cb = craftBrickKeyOfCell(c0x, c0y, c0z);

            std::vector<EditCell>* bucket = nullptr;
            for (auto& kv : byCraftBrick)
                if (kv.first == cb) { bucket = &kv.second; break; }
            if (bucket == nullptr) {
                byCraftBrick.emplace_back(cb, std::vector<EditCell>{});
                bucket = &byCraftBrick.back().second;
            }
            const int64_t e = kMarchBrickEdge;
            for (int64_t dz = 0; dz < kCraftCellsPerVoxel; ++dz)
                for (int64_t dy = 0; dy < kCraftCellsPerVoxel; ++dy)
                    for (int64_t dx = 0; dx < kCraftCellsPerVoxel; ++dx)
                        bucket->push_back(EditCell{
                            static_cast<uint16_t>(Brick<kMarchBrickEdge>::cellIndex(
                                static_cast<int>(floorMod(c0x + dx, e)),
                                static_cast<int>(floorMod(c0y + dy, e)),
                                static_cast<int>(floorMod(c0z + dz, e)))),
                            c.mat});
        }
        // Deterministic order, because the craft log's sequence numbers are
        // part of the save and two hosts must agree on them.
        std::sort(byCraftBrick.begin(), byCraftBrick.end(),
                  [](const auto& a, const auto& b) { return BrickKeyLess{}(a.first, b.first); });
        uint64_t last = 0;
        for (auto& kv : byCraftBrick)
            last = applyCraftEditNoProject(kv.first, std::move(kv.second));
        // ONCE, after every group has been applied. Every group here belongs to
        // the same terrain brick by construction -- routeTerrainEditToCraft is
        // called with one terrain brick's cells -- so one fold covers them all.
        writeProjection(terrainBrick);
        return last;
    }

    // The apply half of applyCraftEdit, WITHOUT the projection.
    //
    // Exists because projecting is per TERRAIN BRICK while applying is per
    // CRAFT BRICK, and one terrain edit routed into the craft stream produces
    // up to 27 craft-brick groups inside a single terrain brick. Projecting
    // inside the loop redid the same ~36,864-cell fold once per group -- always
    // correct, since each fold is right for the state at that moment and the
    // last one wins, which is exactly why no test saw it until one counted the
    // folds (a_routed_dig_projects_its_brick_exactly_once).
    //
    // Anything calling this MUST call writeProjection for every terrain brick
    // it touched, or the overlay keeps the pre-edit projection while the craft
    // lattice has moved -- the two disagreeing about the same voxel, which is
    // the defect the routing exists to prevent.
    uint64_t applyCraftEditNoProject(const BrickKey& craftBrick, std::vector<EditCell> cells) {
        ensurePromoted(terrainBrickOfCraftBrick(craftBrick));
        const EditEntry& e = craftLog_.append(craftBrick, std::move(cells));
        applyCraftToOverlay(e);
        return e.seq;
    }

    void ensurePromoted(const BrickKey& terrainBrick) {
        if (craft_.isPromoted(terrainBrick)) return;
        craft_.promote(terrainBrick, brickAt(terrainBrick));
    }

    void applyCraftToOverlay(const EditEntry& e) {
        // e.key is a CRAFT-brick key, so its cells start at key * 8 in craft
        // cells -- the same arithmetic a terrain entry uses, one lattice down.
        const int64_t baseX = int64_t(e.key.x) * kMarchBrickEdge;
        const int64_t baseY = int64_t(e.key.y) * kMarchBrickEdge;
        const int64_t baseZ = int64_t(e.key.z) * kMarchBrickEdge;
        for (const EditCell& c : e.cells) {
            const int x = c.cell % kMarchBrickEdge;
            const int y = (c.cell / kMarchBrickEdge) % kMarchBrickEdge;
            const int z = c.cell / (kMarchBrickEdge * kMarchBrickEdge);
            craft_.setCell(baseX + x, baseY + y, baseZ + z, c.mat);
        }
    }

    // THE PROJECTION IS WRITTEN, NOT DERIVED AT READ TIME. Everything that
    // reads the 10 cm world -- collision, pathfinding, water, the coarse ring
    // meshes, the digest -- goes on reading the overlay and gets a correct
    // answer with no knowledge that a craft lattice exists.
    //
    // It goes into the OVERLAY and never into the terrain LOG: the craft log
    // is the authority for these cells, so writing them to both would
    // double-count them on replay and make the two streams disagree about who
    // owns the brick.
    void writeProjection(const BrickKey& terrainBrick) {
        Brick<B> projected;
        if (!craft_.project(terrainBrick, projected)) return; // counted in the lattice
        overlay_.insert(terrainBrick, std::move(projected));
    }

    Amplifier amp_;
    GeneratedWorld<B> gen_;
    ChunkMap<B> overlay_;
    CraftLattice<B> craft_;
    EditLog log_;
    EditLog craftLog_;
    std::optional<EditLog::ProviderCheck> lastProviderCheck_;
};

} // namespace vxc
