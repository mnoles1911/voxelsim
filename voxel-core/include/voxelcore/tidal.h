#pragma once
// ============================================================================
// THE ROCK-POOL DATUM (water-ocean-tides plan Phase C3)
// ============================================================================
//
// WHAT THIS IS, IN ONE SENTENCE. A decorator on THE one datum seam
// (lakes.h `IBasinDatumSource`) that makes a qualified tidal basin's surface
// ride the tide while the sea reaches over its sill, and hold FULL AT ITS
// SILL when the falling tide cuts it off -- which is the whole rock-pool
// behaviour of the reference game, expressed as three integer comparisons.
//
// WHY A DECORATOR ON THE SEAM AND NOT LOGIC AT THE CALL SITES. Every consumer
// of a basin's height -- the near-field implicit fill, the far-field sheet
// gather, SubmergedDepth, the surface contract -- already reads through
// `basinDatumMm` (that seam was closed twice: when the sheet shipped and when
// the ledger landed). Wrapping the seam moves ALL of them at once by
// construction; logic at any call site would reopen the exact near/far
// disagreement the seam exists to forbid.
//
// STATELESS AND DETERMINISTIC, and that is a design decision rather than a
// simplification. A disconnected pool answers `spillMm` -- full to its sill --
// with no memory of when it disconnected or how much it has "evaporated",
// because a tidal basin's sill is below astronomical high water BY
// QUALIFICATION (the oracle's first clause), so the sea genuinely refills it
// to the brim every cycle; holding it there between floods is the honest
// equilibrium, not an approximation of one. Nothing is saved, nothing
// accumulates, and two machines at the same (spec, epoch) agree on every
// pool's surface exactly as they agree on the tide (tide.h's contract).
//
// "CONNECTED" IS ARITHMETIC HERE, NOT A GRID READ: the pool is connected to
// the sea exactly while the tide stands at or over its sill (tideNow >=
// spillMm). The BFS grid (oceanconnect.h) decides WHICH basins are tidal at
// all -- through the oracle, once per window recentre -- not where today's
// water reaches minute by minute; conflating the two would make a pool's
// surface flicker with the sampling of a 7.5 m grid.
//
// THE LEDGER ALWAYS KEEPS ITS SAY: the tidal answer is max()ed with the inner
// source, so a rain-credited pool standing ABOVE the tide's answer stays at
// the ledger's level (the risk-register row: "tidal datum masks rain-credited
// pool -- max() with ledger, ledger wins when higher"). The tide is a datum
// OVERLAY, never a credit: no spill, no refund, no save-blob row ever moves
// because of this class.
//
// NON-TIDAL BASINS PASS THROUGH BIT-EXACT -- not "approximately", not "after
// a no-op max": the inner source's answer is RETURNED UNTOUCHED, which is
// what makes the inland-lake gate (`InlandDatumDeltaMm == 0`) a structural
// property with a counter that can still fail if this wiring is ever bent.

#include <cstdint>

#include "voxelcore/lakes.h" // IBasinDatumSource, ITidalBasinOracle, BasinEntry

namespace vxc {

class TidalDatumSource final : public IBasinDatumSource {
public:
    // Both borrowed; both must outlive this object (the engine owns all three
    // in declaration order, same discipline as the ledger stack).
    TidalDatumSource(IBasinDatumSource& inner, ITidalBasinOracle& oracle)
        : inner_(&inner), oracle_(&oracle) {}

    // The sea's datum RIGHT NOW, absolute mm: kSeaLevelMm plus the QUANTISED
    // tide offset -- the same one number the ImplicitFn's ocean term reads, set
    // by the engine on every datum step. Quantised on purpose: this feeds the
    // extent memo's key and the drawn surface, and a continuous value here
    // would re-fill masks at tick rate.
    void setTideNowMm(int32_t tideNowMm) { tideNowMm_ = tideNowMm; }
    int32_t tideNowMm() const { return tideNowMm_; }

    int32_t basinDatumMm(int32_t tx, int32_t ty, const BasinEntry& baked) override {
        const int32_t ledgerMm = inner_->basinDatumMm(tx, ty, baked);
        if (!oracle_->isTidal(tx, ty, baked)) {
            // BIT-EXACT PASSTHROUGH -- the inland gate's whole foundation.
            return ledgerMm;
        }
        // Connected (tide at or over the sill): the pool is one body with the
        // sea and rides it. Disconnected: held full at the sill it was cut off
        // at. >= not >: at exact equality the two answers are the same number,
        // so the boundary cannot flicker.
        const int32_t tidalMm = tideNowMm_ >= baked.spillMm ? tideNowMm_ : baked.spillMm;
        // The ledger wins when higher (rain-credited pool); the tide never
        // DRAINS anything, it only explains where the sea put the surface.
        return ledgerMm > tidalMm ? ledgerMm : tidalMm;
    }

private:
    IBasinDatumSource* inner_;
    ITidalBasinOracle* oracle_;
    int32_t tideNowMm_ = kSeaLevelMm;
};

} // namespace vxc
