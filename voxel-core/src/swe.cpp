// Fixed-point shallow-water reference core + CA coupling. The full design,
// the numerics justification and the coupling contract live in
// voxelcore/swe.h; this file is the implementation of exactly what that
// header specifies and adds no rules of its own.

#include "voxelcore/swe.h"

#include <algorithm>

namespace vxc {
namespace {

// swe.h §3 phase 1. Damping is applied with shiftSym (swe.h's note on why a
// plain >> would leave a permanent one-unit current on a settled lake); the
// gain term promotes the head difference into Q8 BEFORE the gain shift, which
// is what keeps an arbitrarily small head difference from truncating to a
// zero increment (swe.h §2, "WHY Q8 ON THE FLUX").
int32_t fluxUpdate(int32_t f, int64_t headDiff, const SweConfig& cfg, int64_t maxQ8) {
    int64_t nf = shiftSym(int64_t{f} * cfg.dampingQ8, 8) +
                 shiftSym(headDiff << 8, cfg.gainShift);
    if (nf > maxQ8) nf = maxQ8;
    else if (nf < -maxQ8) nf = -maxQ8;
    return static_cast<int32_t>(nf);
}

// Bound on the demote pre-flight's upward capacity walk (swe.h §5(c)).
// Generous because a demote is rare and the alternative to finding the room is
// deferring the whole hand-over another tick.
constexpr int64_t kDemoteScanVoxels = 4096;

} // namespace

// ---------------------------------------------------------------------------
// SweGrid
// ---------------------------------------------------------------------------

SweGrid::SweGrid(int64_t originVx, int64_t originVy, int32_t sizeX, int32_t sizeY,
                 const SweConfig& cfg)
    : originVx_(originVx), originVy_(originVy),
      sizeX_(sizeX < 0 ? 0 : sizeX), sizeY_(sizeY < 0 ? 0 : sizeY), cfg_(cfg) {
    // swe.h §4: the CFL analogue is CLAMPED, not trusted. Weakening the gain
    // (raising gainShift) is the safe direction to move in — it costs
    // convergence speed and widens the settle deadband, but both of those are
    // observable and bounded, whereas an unstable config rings forever.
    while (!cfg_.stableIn2D() && cfg_.gainShift < 30) ++cfg_.gainShift;
    if (cfg_.dampingQ8 < 0) cfg_.dampingQ8 = 0;
    if (cfg_.dampingQ8 > 255) cfg_.dampingQ8 = 255;
    if (cfg_.maxFluxPerTick < 0) cfg_.maxFluxPerTick = 0;
    if (cfg_.maxColumnDepth < 0) cfg_.maxColumnDepth = 0;

    const size_t n = static_cast<size_t>(sizeX_) * static_cast<size_t>(sizeY_);
    bed_.assign(n, 0);
    depth_.assign(n, 0);
    active_.assign(n, 1);
    fluxXQ8_.assign(n, 0);
    fluxYQ8_.assign(n, 0);
    desiredXQ8_.assign(n, 0);
    desiredYQ8_.assign(n, 0);
    magX_.assign(n, 0);
    magY_.assign(n, 0);
    delta_.assign(n, 0);
}

void SweGrid::setBed(int64_t vx, int64_t vy, int32_t bedZ) {
    if (!inBounds(vx, vy)) return;
    bed_[static_cast<size_t>(index(vx, vy))] = bedZ;
}

int32_t SweGrid::bedAt(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return 0;
    return bed_[static_cast<size_t>(index(vx, vy))];
}

void SweGrid::setColumnActive(int64_t vx, int64_t vy, bool on) {
    if (!inBounds(vx, vy)) return;
    active_[static_cast<size_t>(index(vx, vy))] = on ? uint8_t{1} : uint8_t{0};
}

bool SweGrid::columnActive(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return false;
    return active_[static_cast<size_t>(index(vx, vy))] != 0;
}

int32_t SweGrid::depthAt(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return 0;
    return depth_[static_cast<size_t>(index(vx, vy))];
}

int64_t SweGrid::headAt(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return 0;
    const size_t i = static_cast<size_t>(index(vx, vy));
    return int64_t{bed_[i]} * 255 + depth_[i];
}

int32_t SweGrid::addWater(int64_t vx, int64_t vy, int32_t amount) {
    if (!inBounds(vx, vy) || amount <= 0) return 0;
    const size_t i = static_cast<size_t>(index(vx, vy));
    const int32_t room = cfg_.maxColumnDepth - depth_[i];
    const int32_t add = amount < room ? amount : room;
    if (add <= 0) return 0;
    depth_[i] += add;
    totalVolume_ += add;
    return add;
}

int32_t SweGrid::removeWater(int64_t vx, int64_t vy, int32_t amount) {
    if (!inBounds(vx, vy) || amount <= 0) return 0;
    const size_t i = static_cast<size_t>(index(vx, vy));
    const int32_t take = amount < depth_[i] ? amount : depth_[i];
    if (take <= 0) return 0;
    depth_[i] -= take;
    totalVolume_ -= take;
    return take;
}

int32_t SweGrid::faceFluxX(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return 0;
    return static_cast<int32_t>(shiftSym(fluxXQ8_[static_cast<size_t>(index(vx, vy))], 8));
}

int32_t SweGrid::faceFluxY(int64_t vx, int64_t vy) const {
    if (!inBounds(vx, vy)) return 0;
    return static_cast<int32_t>(shiftSym(fluxYQ8_[static_cast<size_t>(index(vx, vy))], 8));
}

void SweGrid::step() {
    std::vector<int32_t> identity(static_cast<size_t>(sizeX_) * static_cast<size_t>(sizeY_));
    for (size_t i = 0; i < identity.size(); ++i) identity[i] = static_cast<int32_t>(i);
    runTick(identity);
}

void SweGrid::stepWithColumnOrder(const std::vector<int32_t>& order) { runTick(order); }

void SweGrid::runTick(const std::vector<int32_t>& order) {
    const int32_t n = static_cast<int32_t>(bed_.size());
    if (n == 0 || sizeX_ == 0) return;

    // Dedup `order` (swe.h: "duplicates are ignored"). Phase 1 and 2 are
    // idempotent per column, but phase 3 REDUCES the stored magnitudes in
    // place against a budget, so visiting a column twice would double-charge
    // its headroom. Deduping once here is cheaper and far clearer than making
    // every phase re-entrant.
    std::vector<uint8_t> seen(static_cast<size_t>(n), 0);
    std::vector<int32_t> cols;
    cols.reserve(static_cast<size_t>(n));
    for (int32_t i : order) {
        if (i < 0 || i >= n) continue;
        if (seen[static_cast<size_t>(i)]) continue;
        seen[static_cast<size_t>(i)] = 1;
        cols.push_back(i);
    }

    std::fill(desiredXQ8_.begin(), desiredXQ8_.end(), 0);
    std::fill(desiredYQ8_.begin(), desiredYQ8_.end(), 0);
    std::fill(magX_.begin(), magX_.end(), 0);
    std::fill(magY_.begin(), magY_.end(), 0);
    std::fill(delta_.begin(), delta_.end(), 0);

    const int64_t maxQ8 = int64_t{cfg_.maxFluxPerTick} << 8;

    // --- PHASE 1: flux update, per face, from tick-start state only --------
    for (int32_t i : cols) {
        const size_t si = static_cast<size_t>(i);
        const int32_t cx = i % sizeX_;
        const int32_t cy = i / sizeX_;
        if (!active_[si]) continue; // hard wall: owns no faces (swe.h setColumnActive)
        const int64_t headA = int64_t{bed_[si]} * 255 + depth_[si];
        if (cx + 1 < sizeX_ && active_[si + 1]) {
            const size_t sj = si + 1;
            const int64_t headB = int64_t{bed_[sj]} * 255 + depth_[sj];
            desiredXQ8_[si] = fluxUpdate(fluxXQ8_[si], headA - headB, cfg_, maxQ8);
        }
        if (cy + 1 < sizeY_ && active_[si + static_cast<size_t>(sizeX_)]) {
            const size_t sj = si + static_cast<size_t>(sizeX_);
            const int64_t headB = int64_t{bed_[sj]} * 255 + depth_[sj];
            desiredYQ8_[si] = fluxUpdate(fluxYQ8_[si], headA - headB, cfg_, maxQ8);
        }
    }

    // Fixed face order for both caps: +x, -x, +y, -y (swe.h §3 phases 2/3).
    // faceIsX[k] selects which array; faceIdx[k] < 0 means "no such face".
    const bool faceIsX[4] = {true, true, false, false};

    // --- PHASE 2: source-side cap -----------------------------------------
    for (int32_t i : cols) {
        const size_t si = static_cast<size_t>(i);
        const int32_t cx = i % sizeX_;
        const int32_t cy = i / sizeX_;

        int64_t faceIdx[4] = {-1, -1, -1, -1};
        int64_t want[4] = {0, 0, 0, 0};

        if (cx + 1 < sizeX_) { // +x: face i, outgoing when positive
            const int64_t t = shiftSym(desiredXQ8_[si], 8);
            if (t > 0) { faceIdx[0] = static_cast<int64_t>(si); want[0] = t; }
        }
        if (cx > 0) { // -x: face i-1, outgoing when negative
            const int64_t t = shiftSym(desiredXQ8_[si - 1], 8);
            if (t < 0) { faceIdx[1] = static_cast<int64_t>(si) - 1; want[1] = -t; }
        }
        if (cy + 1 < sizeY_) { // +y: face i, outgoing when positive
            const int64_t t = shiftSym(desiredYQ8_[si], 8);
            if (t > 0) { faceIdx[2] = static_cast<int64_t>(si); want[2] = t; }
        }
        if (cy > 0) { // -y: face i-sizeX, outgoing when negative
            const size_t sj = si - static_cast<size_t>(sizeX_);
            const int64_t t = shiftSym(desiredYQ8_[sj], 8);
            if (t < 0) { faceIdx[3] = static_cast<int64_t>(sj); want[3] = -t; }
        }

        const int64_t sum = want[0] + want[1] + want[2] + want[3];
        int64_t give[4] = {want[0], want[1], want[2], want[3]};
        const int64_t have = depth_[si];
        if (sum > have) {
            // Integer floor plus explicit remainder in the fixed face order:
            // sum(give) == have EXACTLY, so depth can never go negative and
            // no unit is lost to rounding (swe.h §3 phase 2).
            int64_t allocated = 0;
            for (int k = 0; k < 4; ++k) {
                give[k] = sum > 0 ? want[k] * have / sum : 0;
                allocated += give[k];
            }
            int64_t rem = have - allocated;
            for (int k = 0; k < 4 && rem > 0; ++k) {
                if (want[k] > 0) { ++give[k]; --rem; }
            }
        }

        for (int k = 0; k < 4; ++k) {
            if (faceIdx[k] < 0) continue;
            const size_t f = static_cast<size_t>(faceIdx[k]);
            (faceIsX[k] ? magX_ : magY_)[f] = static_cast<int32_t>(give[k]);
        }
    }

    // --- PHASE 3: target-side cap -----------------------------------------
    for (int32_t i : cols) {
        const size_t si = static_cast<size_t>(i);
        const int32_t cx = i % sizeX_;
        const int32_t cy = i / sizeX_;

        int64_t faceIdx[4] = {-1, -1, -1, -1};

        if (cx + 1 < sizeX_ && shiftSym(desiredXQ8_[si], 8) < 0) faceIdx[0] = static_cast<int64_t>(si);
        if (cx > 0 && shiftSym(desiredXQ8_[si - 1], 8) > 0) faceIdx[1] = static_cast<int64_t>(si) - 1;
        if (cy + 1 < sizeY_ && shiftSym(desiredYQ8_[si], 8) < 0) faceIdx[2] = static_cast<int64_t>(si);
        if (cy > 0) {
            const size_t sj = si - static_cast<size_t>(sizeX_);
            if (shiftSym(desiredYQ8_[sj], 8) > 0) faceIdx[3] = static_cast<int64_t>(sj);
        }

        int64_t budget = int64_t{cfg_.maxColumnDepth} - depth_[si];
        if (budget < 0) budget = 0;
        for (int k = 0; k < 4; ++k) {
            if (faceIdx[k] < 0) continue;
            const size_t f = static_cast<size_t>(faceIdx[k]);
            int32_t& m = (faceIsX[k] ? magX_ : magY_)[f];
            const int64_t admit = m < budget ? m : budget;
            m = static_cast<int32_t>(admit);
            budget -= admit;
        }
    }

    // --- PHASE 4: apply ---------------------------------------------------
    // Every face's magnitude is subtracted from one column and added to
    // another IN THE SAME STATEMENT PAIR, so sum(delta_) is exactly 0 by
    // construction — conservation is a property of this loop (swe.h §3).
    for (int32_t j = 0; j < n; ++j) {
        const size_t sj = static_cast<size_t>(j);
        const int32_t cx = j % sizeX_;
        const int32_t cy = j / sizeX_;

        if (cx + 1 < sizeX_ && active_[sj] && active_[sj + 1]) {
            const int64_t t = shiftSym(desiredXQ8_[sj], 8);
            const int32_t m = magX_[sj];
            if (m > 0) {
                const size_t lo = sj, hi = sj + 1;
                const size_t src = t > 0 ? lo : hi;
                const size_t dst = t > 0 ? hi : lo;
                delta_[src] -= m;
                delta_[dst] += m;
            }
            // Uncapped: keep the full-precision accumulator (sub-unit
            // momentum survives, which is what makes the sheet slosh rather
            // than creep). Capped: momentum is cut to the water that actually
            // moved.
            const int64_t absT = t < 0 ? -t : t;
            if (m == absT) {
                fluxXQ8_[sj] = desiredXQ8_[sj];
            } else {
                fluxXQ8_[sj] = static_cast<int32_t>((t >= 0 ? int64_t{m} : -int64_t{m}) << 8);
            }
        } else {
            fluxXQ8_[sj] = 0; // outer boundary is a hard wall (swe.h §3 phase 1)
        }

        if (cy + 1 < sizeY_ && active_[sj] && active_[sj + static_cast<size_t>(sizeX_)]) {
            const int64_t t = shiftSym(desiredYQ8_[sj], 8);
            const int32_t m = magY_[sj];
            if (m > 0) {
                const size_t lo = sj, hi = sj + static_cast<size_t>(sizeX_);
                const size_t src = t > 0 ? lo : hi;
                const size_t dst = t > 0 ? hi : lo;
                delta_[src] -= m;
                delta_[dst] += m;
            }
            const int64_t absT = t < 0 ? -t : t;
            if (m == absT) {
                fluxYQ8_[sj] = desiredYQ8_[sj];
            } else {
                fluxYQ8_[sj] = static_cast<int32_t>((t >= 0 ? int64_t{m} : -int64_t{m}) << 8);
            }
        } else {
            fluxYQ8_[sj] = 0;
        }
    }

    for (int32_t j = 0; j < n; ++j) depth_[static_cast<size_t>(j)] += delta_[static_cast<size_t>(j)];
}

SweVelocity SweGrid::velocityAt(int64_t vx, int64_t vy) const {
    SweVelocity v;
    if (!inBounds(vx, vy)) return v;
    const int32_t i = index(vx, vy);
    const size_t si = static_cast<size_t>(i);
    const int32_t d = depth_[si];
    if (d <= 0) return v;
    const int32_t cx = i % sizeX_;
    const int32_t cy = i / sizeX_;

    // Average the two opposite face fluxes in Q8, then convert once. swe.h §6:
    // v = f * kVoxelSizeMm * ticksPerSecond / depth, with the Q8 scale folded
    // into the divisor so the division happens at full precision. C++ integer
    // division truncates toward zero, which is already sign-symmetric.
    const int64_t fl = cx > 0 ? fluxXQ8_[si - 1] : 0;
    const int64_t fr = cx + 1 < sizeX_ ? fluxXQ8_[si] : 0;
    const int64_t fd = cy > 0 ? fluxYQ8_[si - static_cast<size_t>(sizeX_)] : 0;
    const int64_t fu = cy + 1 < sizeY_ ? fluxYQ8_[si] : 0;

    const int64_t scale = int64_t{kVoxelSizeMm} * cfg_.ticksPerSecond;
    const int64_t divisor = int64_t{d} * 512; // 256 (Q8) * 2 (two-face average)
    v.xMmPerSec = static_cast<int32_t>((fl + fr) * scale / divisor);
    v.yMmPerSec = static_cast<int32_t>((fd + fu) * scale / divisor);
    return v;
}

int64_t SweGrid::recomputeVolume() const {
    int64_t sum = 0;
    for (int32_t d : depth_) sum += d;
    return sum;
}

void SweGrid::digest(Digest& d) const {
    d.u32(kSweVersion);
    d.u32(static_cast<uint32_t>(sizeX_));
    d.u32(static_cast<uint32_t>(sizeY_));
    for (size_t i = 0; i < bed_.size(); ++i) {
        d.u8(active_[i]);
        d.u32(static_cast<uint32_t>(bed_[i]));
        d.u32(static_cast<uint32_t>(depth_[i]));
        d.u32(static_cast<uint32_t>(fluxXQ8_[i]));
        d.u32(static_cast<uint32_t>(fluxYQ8_[i]));
    }
}

// ---------------------------------------------------------------------------
// SweCaCoupler (swe.h §5)
// ---------------------------------------------------------------------------

SweCaCoupler::SweCaCoupler(SweGrid& grid, WaterCA& ca, SolidFn solid, const SweCoupleConfig& cfg)
    : grid_(grid), ca_(ca), solid_(std::move(solid)), cfg_(cfg) {
    const size_t n = static_cast<size_t>(grid.sizeX()) * static_cast<size_t>(grid.sizeY());
    member_.assign(n, 0);
    eligDwell_.assign(n, 0);
    inelDwell_.assign(n, 0);
    if (cfg_.enabled) deactivateAll();
}

// The SWE domain IS the promoted set: with a coupler attached, a column that
// has not been promoted is CA-owned and must be a hard wall in the numerics
// (swe.h setColumnActive). A disabled coupler touches nothing, which is what
// keeps swe_coupler_is_a_total_no_op_when_disabled true.
void SweCaCoupler::deactivateAll() {
    for (int32_t cy = 0; cy < grid_.sizeY(); ++cy)
        for (int32_t cx = 0; cx < grid_.sizeX(); ++cx)
            grid_.setColumnActive(grid_.originVx() + cx, grid_.originVy() + cy,
                                  member_[static_cast<size_t>(cx + grid_.sizeX() * cy)] != 0);
}

bool SweCaCoupler::isSweColumn(int64_t vx, int64_t vy) const {
    if (!grid_.inBounds(vx, vy)) return false;
    const size_t i = static_cast<size_t>((vx - grid_.originVx()) +
                                         int64_t{grid_.sizeX()} * (vy - grid_.originVy()));
    return member_[i] != 0;
}

bool SweCaCoupler::eligible(int64_t vx, int64_t vy) const {
    const int64_t bed = grid_.bedAt(vx, vy);
    if (!solidAt(vx, vy, bed)) return false; // no bed to rest on (or punctured)
    // No lid: a flooded tunnel is not a free surface, so it is not
    // depth-averageable and must stay with the CA.
    for (int32_t k = 1; k <= cfg_.openClearanceVoxels; ++k) {
        if (solidAt(vx, vy, bed + k)) return false;
    }
    // Open, not a one-voxel pipe: horizontal scale must exceed depth.
    int32_t open = 0;
    if (!solidAt(vx + 1, vy, bed + 1)) ++open;
    if (!solidAt(vx - 1, vy, bed + 1)) ++open;
    if (!solidAt(vx, vy + 1, bed + 1)) ++open;
    if (!solidAt(vx, vy - 1, bed + 1)) ++open;
    return open >= cfg_.minOpenNeighbours;
}

int32_t SweCaCoupler::absorbColumn(int64_t vx, int64_t vy, int32_t budget) {
    if (budget <= 0) return 0;
    const int64_t bed = grid_.bedAt(vx, vy);
    int32_t moved = 0;
    for (int32_t k = 1; k <= cfg_.sheetScanVoxels && moved < budget; ++k) {
        const uint8_t fill = ca_.fillAt(vx, vy, bed + k);
        if (fill == 0) continue;
        const int32_t want = budget - moved;
        const uint32_t took = ca_.removeWaterAt(vx, vy, bed + k,
                                                static_cast<uint32_t>(want < fill ? want : fill));
        if (took == 0) continue;
        // Only what the grid actually accepted is debited from the CA... and
        // the CA has already given it up, so if the grid is at capacity the
        // remainder must go back. Placing first and refunding keeps the
        // two-sided ledger exact with no pre-flight capacity query.
        const int32_t placed = grid_.addWater(vx, vy, static_cast<int32_t>(took));
        if (placed < static_cast<int32_t>(took)) {
            ca_.addWaterAt(vx, vy, bed + k, static_cast<uint32_t>(static_cast<int32_t>(took) - placed));
        }
        moved += placed;
    }
    toSWE_ += moved;
    return moved;
}

void SweCaCoupler::drainColumn(int32_t /*i*/, int64_t vx, int64_t vy) {
    const int64_t bed = grid_.bedAt(vx, vy);
    const int32_t have = grid_.depthAt(vx, vy);
    const int32_t want = have < cfg_.drainPerTick ? have : cfg_.drainPerTick;
    if (want <= 0) return;
    const uint32_t placed = ca_.addWaterAt(vx, vy, bed, static_cast<uint32_t>(want));
    if (placed == 0) return; // CA cell full: back-pressure, nothing is lost
    const int32_t removed = grid_.removeWater(vx, vy, static_cast<int32_t>(placed));
    toCA_ += removed;
}

void SweCaCoupler::promote(int32_t i, int64_t vx, int64_t vy) {
    member_[static_cast<size_t>(i)] = 1;
    grid_.setColumnActive(vx, vy, true);
    ++sweColumns_;
    // Channel (c): the column's existing CA fill becomes sheet depth at full
    // rate, which is also what establishes the no-shared-cell invariant for
    // this column from its very first owned tick.
    absorbColumn(vx, vy, cfg_.sheetScanVoxels * 255);
}

void SweCaCoupler::demote(int32_t i, int64_t vx, int64_t vy) {
    const int64_t bed = grid_.bedAt(vx, vy);
    const int32_t have = grid_.depthAt(vx, vy);
    if (have > 0) {
        // Stack back down into the CA from the first non-solid voxel. A
        // punctured column starts AT the bed (that voxel is open by
        // definition); an intact one starts just above it.
        const int64_t startZ = solidAt(vx, vy, bed) ? bed + 1 : bed;

        // PRE-FLIGHT, and it is load-bearing for the ownership partition, not
        // an optimization. Demoting is ALL-OR-NOTHING: if the CA cannot take
        // the whole column this tick we do not move a single unit, because a
        // half-demoted column would hold sheet depth AND CA fill in the same
        // z-range — the one thing swe.h §5's partition forbids. The column
        // simply stays cleanly SWE-owned, and since the ineligible dwell
        // counter is left standing the demotion retries every following tick
        // until the CA has drained enough room.
        int64_t capacity = 0;
        for (int64_t z = startZ; capacity < have; ++z) {
            if (z - startZ >= kDemoteScanVoxels) break;
            if (solidAt(vx, vy, z)) break;
            capacity += 255 - ca_.fillAt(vx, vy, z);
        }
        if (capacity < have) return;

        const uint32_t placed = ca_.addWater(vx, vy, startZ, static_cast<uint32_t>(have));
        toCA_ += grid_.removeWater(vx, vy, static_cast<int32_t>(placed));
        if (grid_.depthAt(vx, vy) > 0) return; // belt-and-braces; unreachable given the pre-flight
    }
    member_[static_cast<size_t>(i)] = 0;
    grid_.setColumnActive(vx, vy, false);
    --sweColumns_;
    inelDwell_[static_cast<size_t>(i)] = 0;
}

void SweCaCoupler::forcePromote(int64_t vx, int64_t vy) {
    if (!grid_.inBounds(vx, vy)) return;
    const int32_t i = static_cast<int32_t>((vx - grid_.originVx()) +
                                           int64_t{grid_.sizeX()} * (vy - grid_.originVy()));
    if (member_[static_cast<size_t>(i)]) return;
    promote(i, vx, vy);
}

void SweCaCoupler::forceDemote(int64_t vx, int64_t vy) {
    if (!grid_.inBounds(vx, vy)) return;
    const int32_t i = static_cast<int32_t>((vx - grid_.originVx()) +
                                           int64_t{grid_.sizeX()} * (vy - grid_.originVy()));
    if (!member_[static_cast<size_t>(i)]) return;
    demote(i, vx, vy);
}

void SweCaCoupler::step() {
    if (!cfg_.enabled) return; // ADR-0004 PENDING: default is a total no-op
    lastPunctured_ = 0;

    const int32_t sx = grid_.sizeX(), sy = grid_.sizeY();

    // Pass 1: membership hysteresis (swe.h §5, mechanism 1).
    for (int32_t cy = 0; cy < sy; ++cy) {
        for (int32_t cx = 0; cx < sx; ++cx) {
            const int32_t i = cx + sx * cy;
            const int64_t vx = grid_.originVx() + cx, vy = grid_.originVy() + cy;
            const size_t si = static_cast<size_t>(i);
            if (eligible(vx, vy)) {
                ++eligDwell_[si];
                inelDwell_[si] = 0;
            } else {
                ++inelDwell_[si];
                eligDwell_[si] = 0;
            }
            if (!member_[si] && eligDwell_[si] >= cfg_.promoteDwellTicks) promote(i, vx, vy);
            else if (member_[si] && inelDwell_[si] >= cfg_.demoteDwellTicks) demote(i, vx, vy);
        }
    }

    // Pass 2: exchange. The direction is chosen ONCE per column per tick from
    // the puncture test, so drain and absorb can never both run on the same
    // column in the same tick (swe.h §5, mechanism 2).
    for (int32_t cy = 0; cy < sy; ++cy) {
        for (int32_t cx = 0; cx < sx; ++cx) {
            const int32_t i = cx + sx * cy;
            if (!member_[static_cast<size_t>(i)]) continue;
            const int64_t vx = grid_.originVx() + cx, vy = grid_.originVy() + cy;
            if (!solidAt(vx, vy, grid_.bedAt(vx, vy))) {
                ++lastPunctured_;
                drainColumn(i, vx, vy);
            } else {
                absorbColumn(vx, vy, cfg_.absorbPerTick);
            }
        }
    }
}

} // namespace vxc
