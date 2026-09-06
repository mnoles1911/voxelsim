#pragma once
// ============================================================================
// OCEAN CONNECTIVITY, AS A FLOOD FILL (water-ocean-tides plan Phase C1)
// ============================================================================
//
// WHAT THIS ANSWERS, IN ONE SENTENCE. Over an n x n grid of ground heights,
// which wet cells can the open sea actually REACH at the tide standing right
// now -- so that wave heights go to ocean-connected water only, and a rock
// pool cut off by a falling tide keeps its water instead of inheriting the
// sea's behaviour.
//
// WHY A FILL AND NOT A THRESHOLD, the same argument lakes.h's extent rule
// makes: "ground below the waterline" is true of every below-sea inland
// depression on the map (the documented IsOpenSeaAtWorld error class), and a
// hillside pool one ridge in from the beach satisfies it while sharing not a
// drop with the sea. Connectivity is a fact about PATHS, so it is computed as
// one: wet = ground strictly below the tide's datum, and connected = reachable
// from the open sea through 4-connected wet cells.
//
// FOUR-CONNECTED, NOT EIGHT, and the disagreement with lakeExtentFill's
// 8-connectivity is deliberate rather than sloppy. The extent fill is pinned
// to the bake's own definition (the registry measured 8-connected components,
// and an extent that disagrees with its row is a shoreline that does not
// close). Nothing here is pinned to the bake -- this grid is a runtime
// derivation with no wire twin -- so it takes the better physics: water does
// not flow through a diagonal pinch between two dry cells, and 8-connectivity
// would flood a pool through exactly that pinch. The two rules serve different
// masters and must not be "made consistent".
//
// THE SEED RULE IS THE GUARD. Seeds are BORDER cells that are wet AND whose
// ground lies below `seedLevelMm`, a level the caller sets a comfortable
// margin below the geological sea level (below sea minus the tide's full
// range, in the engine). Without the second clause, an inland below-sea playa
// whose bbox happens to touch the window edge would seed ITSELF and report
// its own water as ocean -- the exact self-certification the deep-margin
// guard exists to refuse. With it, a false connection requires a below-
// (sea - margin) channel running off the window edge, which is real seabed in
// every case that survives the arithmetic. The residual error is the already-
// documented IsOpenSeaAtWorld class, strictly narrowed.
//
// DETERMINISTIC BY CONSTRUCTION, not by promise: seeds are gathered in
// row-major order, the frontier is a FIFO walked with a head index, and
// neighbours are visited in one fixed order -- so two machines (or two runs)
// produce identical `outBits` byte for byte, and the test suite pins that by
// comparing runs rather than trusting this paragraph.
//
// ENGINE-FREE AND INTEGER-ONLY, like tide.h and for the same reason: what
// this feeds decides which basins ride the tide (TidalDatumSource's oracle),
// and a float in the wet test would be one machine's rock pool and another's
// tidal reach.

#include <cstdint>
#include <vector>

namespace vxc {

// What one fill did, so a leg log can say "seeds=0" and mean something: zero
// seeds on a coastal window is the guard eating the sea (margin too deep, or
// the window is not where the caller thinks it is), and it looks exactly like
// a calm day unless it is a number.
struct OceanConnectStats {
    uint32_t seedCells = 0;      // border cells that seeded the fill
    uint32_t wetCells = 0;       // cells with ground < tideNowMm
    uint32_t connectedCells = 0; // wet cells the fill reached (includes seeds)
};

// Fills `outBits` (n*n bytes, row-major y*n+x, 1 = ocean-connected wet cell)
// from `groundMm` (same layout, absolute mm). `tideNowMm` is the sea's datum
// RIGHT NOW -- kSeaLevelMm plus the quantised tide offset -- and `seedLevelMm`
// is the deep-margin seed threshold (see the header comment; the engine passes
// kSeaLevelMm - tideMax - margin so a seed stays wet at every state of the
// tide). n <= 0 clears nothing and answers zeros: this layer stays total for
// whatever a test hands it, same doctrine as tide.h's inert components.
inline OceanConnectStats oceanConnectivityFill(const int32_t* groundMm, int32_t n,
                                               int32_t tideNowMm, int32_t seedLevelMm,
                                               uint8_t* outBits) {
    OceanConnectStats stats;
    if (groundMm == nullptr || outBits == nullptr || n <= 0) return stats;
    const size_t cells = size_t(n) * size_t(n);
    for (size_t i = 0; i < cells; ++i) outBits[i] = 0;

    // FIFO frontier with a head index rather than std::queue: same walk, no
    // allocator churn, and the iteration order is visibly one thing.
    std::vector<int32_t> frontier;
    frontier.reserve(size_t(n) * 4);

    // Seeds: border cells, row-major scan, wet AND deep. Row-major is the
    // determinism contract's first half (the FIFO + fixed neighbour order is
    // the second).
    for (int32_t y = 0; y < n; ++y) {
        for (int32_t x = 0; x < n; ++x) {
            const bool border = x == 0 || y == 0 || x == n - 1 || y == n - 1;
            if (!border) continue;
            const int32_t g = groundMm[size_t(y) * size_t(n) + size_t(x)];
            if (g >= tideNowMm) continue;    // not even wet
            if (g >= seedLevelMm) continue;  // wet but shallow: the guard
            const int32_t i = y * n + x;
            outBits[size_t(i)] = 1;
            frontier.push_back(i);
            ++stats.seedCells;
        }
    }

    // The wet census is independent of the fill and counted in one pass, so
    // "wet but unreachable" (a held rock pool) is legible as wet - connected.
    for (size_t i = 0; i < cells; ++i) {
        if (groundMm[i] < tideNowMm) ++stats.wetCells;
    }

    // BFS. Neighbour order is fixed (-x, +x, -y, +y); with a FIFO this gives
    // one specific visit order for one specific input, which is what the
    // determinism test compares.
    static constexpr int32_t kDx[4] = {-1, 1, 0, 0};
    static constexpr int32_t kDy[4] = {0, 0, -1, 1};
    size_t head = 0;
    while (head < frontier.size()) {
        const int32_t c = frontier[head++];
        const int32_t cy = c / n, cx = c - cy * n;
        for (int k = 0; k < 4; ++k) {
            const int32_t nx = cx + kDx[k], ny = cy + kDy[k];
            if (nx < 0 || nx >= n || ny < 0 || ny >= n) continue;
            const size_t ni = size_t(ny) * size_t(n) + size_t(nx);
            if (outBits[ni]) continue;
            if (groundMm[ni] >= tideNowMm) continue; // dry: the fill stops here
            outBits[ni] = 1;
            frontier.push_back(int32_t(ni));
        }
    }
    stats.connectedCells = uint32_t(frontier.size());
    return stats;
}

} // namespace vxc
