#pragma once
// Machine-checked twin of the allocation table at the top of hash.h. Read
// that comment first â€” this file is the enforcement, not the explanation.
//
// WHY THIS CANNOT LIVE IN hash.h. hash.h is the bottom of this dependency
// stack: caves.h, caverns.h, detail_rill.h and detail_bedding.h
// all `#include "voxelcore/hash.h"`, never the other way around, so none of
// their channel symbols (CH_CAVE_NODE, CH_CAVERN_SITE, CH_RILL, ...) can be
// named from inside hash.h itself. The uniqueness check has to live
// downstream of all of them, where every real symbol is visible at once â€”
// which is exactly this file.
//
// WHAT THIS CATCHES. Two subsystems claiming the same HashChannel id is not a
// cosmetic clash: the channel is hash2/hash3's only domain separator, so a
// shared id means the two subsystems are sampling the SAME noise field, only
// at different coordinate magnitudes â€” a correlation that stays invisible
// until someone changes a lattice size, at which point it shows up as an
// unexplained-looking correlation between two "unrelated" terrain features.
// That exact bug shipped once already: CH_CAVE_NODE/CH_CAVE_EDGE in caves.h
// reused ids 18/19, which CH_ECOTONE_TEMP/CH_ECOTONE_PRECIP in hash.h already
// owned, for a full worldgen version before anyone noticed. This header turns
// that class of mistake into a build failure instead of a code-review miss.
//
// HOW TO USE. Include this header (transitively or directly) from anywhere
// that should refuse to build on a duplicate id; voxelcore/amplifier.h does,
// so any real build of the engine gets the check. Adding a new channel
// elsewhere in the tree? Add its symbol to kChannelAllocs below (and a row to
// hash.h's table) â€” if it collides, the static_assert fails right here with a
// message that says why, instead of waiting to be noticed in a diff or, worse,
// in a rendered world.

#include "voxelcore/caverns.h"
#include "voxelcore/caves.h"
#include "voxelcore/detail_bedding.h"
#include "voxelcore/detail_rill.h"
#include "voxelcore/hash.h"

namespace vxc {
namespace channel_registry_detail {

// One entry per allocation. `count` is the number of consecutive ids the
// symbol reserves â€” 1 for an ordinary single-purpose channel, 16 for the two
// per-index bases (CH_DETAIL_OCTAVE_BASE reserves 0..15, CH_SYNTH_TILE_BASE
// reserves 32..47) so the whole reserved block, not just its first id, is
// checked for overlap against every other entry.
struct ChannelAlloc {
    uint32_t first;
    uint32_t count;
    const char* symbol;
};

inline constexpr ChannelAlloc kChannelAllocs[] = {
    // hash.h
    {CH_DETAIL_OCTAVE_BASE, 16, "CH_DETAIL_OCTAVE_BASE (reserves +0..+15)"},
    {CH_TOPSOIL_JITTER, 1, "CH_TOPSOIL_JITTER"},
    {CH_BEDROCK_JITTER, 1, "CH_BEDROCK_JITTER"},
    {CH_ECOTONE_TEMP, 1, "CH_ECOTONE_TEMP"},
    {CH_ECOTONE_PRECIP, 1, "CH_ECOTONE_PRECIP"},
    {CH_SYNTH_TILE_BASE, 16, "CH_SYNTH_TILE_BASE (reserves +0..+15)"},
    // caves.h
    {CH_CAVE_NODE, 1, "CH_CAVE_NODE"},
    {CH_CAVE_EDGE, 1, "CH_CAVE_EDGE"},
    {CH_CAVE_RADIUS, 1, "CH_CAVE_RADIUS"},
    {CH_CAVE_SHAFT, 1, "CH_CAVE_SHAFT"},
    {CH_CREVICE, 1, "CH_CREVICE"},
    // caverns.h
    {CH_CAVERN_SITE, 1, "CH_CAVERN_SITE"},
    {CH_CAVERN_ROUGH, 1, "CH_CAVERN_ROUGH"},
    {CH_CAVERN_FLOOD, 1, "CH_CAVERN_FLOOD"},
    // detail_rill.h
    {CH_RILL, 1, "CH_RILL"},
    // detail_bedding.h
    {CH_BEDDING_STRIKE, 1, "CH_BEDDING_STRIKE"},
    {CH_BEDDING, 1, "CH_BEDDING"},
    // hash.h -- v16 horizontal carrier warp, one channel per axis.
    {CH_CARRIER_WARP_X, 1, "CH_CARRIER_WARP_X"},
    {CH_CARRIER_WARP_Y, 1, "CH_CARRIER_WARP_Y"},
    // density3.h allocated NOTHING and is deleted at kWorldGenVersion 20. It
    // held CH_POCKET = 29 for the pocket term until v12 removed that; 29, 30 and
    // 31 are free. Everything the 3D density band hashed, it hashed through
    // detail_bedding.h's two channels above -- deliberately, so the volumetric
    // recesses stayed in phase with the banding on the face -- so removing the
    // band frees no channel and leaves this table unchanged.
    // hash.h -- environmental-asset scatter (voxelcore/assetplacement.h). Each
    // of the three draws reserves one id PER LAYER (kAssetLayerCount = 4), so
    // the blocks are 50..53, 54..57 and 58..61; the reserve is declared here at
    // its true width rather than as three single ids, or a fifth layer would
    // silently collide with the next draw's base instead of failing the build.
    {CH_ASSET_SITE, 4, "CH_ASSET_SITE (reserves +0..+3, one per asset layer)"},
    {CH_ASSET_JITTER, 4, "CH_ASSET_JITTER (reserves +0..+3, one per asset layer)"},
    {CH_ASSET_PICK, 4, "CH_ASSET_PICK (reserves +0..+3, one per asset layer)"},
    // hash.h -- placement policy (voxelcore/assetpolicy.h). Same per-layer
    // reserve as the three above and declared at its true width for the same
    // reason.
    {CH_ASSET_CLUSTER, 4, "CH_ASSET_CLUSTER (reserves +0..+3, one per asset layer)"},
    {CH_ASSET_SPECIES, 4, "CH_ASSET_SPECIES (reserves +0..+3, one per asset layer)"},
    // hash.h -- detail entities (voxelcore/assetdetail.h). NOT per-layer; a
    // detail group lattice is chosen by the caller rather than being one of
    // the four size classes, so there is no index to reserve against.
    {CH_DETAIL_GROUP, 1, "CH_DETAIL_GROUP"},
    {CH_DETAIL_MEMBER, 1, "CH_DETAIL_MEMBER"},
};

inline constexpr int kChannelAllocCount =
    static_cast<int>(sizeof(kChannelAllocs) / sizeof(kChannelAllocs[0]));

constexpr bool rangesOverlap(const ChannelAlloc& a, const ChannelAlloc& b) {
    return a.first < b.first + b.count && b.first < a.first + a.count;
}

// Returns -1 if every allocation is disjoint, otherwise 100*i + j (i < j) for
// the first colliding pair found â€” encoded as a plain int, not a struct,
// purely so it can be handed to static_assert's message-adjacent constexpr
// context without dragging in <utility>.
constexpr int firstCollisionOrNegativeOne() {
    for (int i = 0; i < kChannelAllocCount; ++i)
        for (int j = i + 1; j < kChannelAllocCount; ++j)
            if (rangesOverlap(kChannelAllocs[i], kChannelAllocs[j])) return 100 * i + j;
    return -1;
}

} // namespace channel_registry_detail

static_assert(channel_registry_detail::firstCollisionOrNegativeOne() == -1,
              "voxelcore HashChannel id collision: two entries in "
              "hash_channel_registry.h's kChannelAllocs claim overlapping "
              "ids. The channel is hash2/hash3's only domain separator, so "
              "this means two subsystems would sample the exact same noise "
              "field (just at different coordinate magnitudes) rather than "
              "provably independent ones -- that is a correlation bug, not a "
              "coincidence to leave alone. Check the allocation table at the "
              "top of hash.h, find the two colliding ids, and move one of "
              "them to a free id (48+, or check hash.h's table for gaps).");

} // namespace vxc
