#pragma once
// ============================================================================
// THE TIDE, AS ARITHMETIC (docs/water-ocean-tides-plan-2026-09-04.md Phase A1)
// ============================================================================
//
// WHAT THIS IS, IN ONE SENTENCE. Given a tide specification and a moment on
// the sky clock, this file says how far the sea's datum stands from
// kSeaLevelMm right now, and it gives the same answer on every machine.
//
// A RUNTIME OFFSET, NEVER A CHANGE TO kSeaLevelMm. The geological datum is a
// constexpr with derived gates hanging off it (caves.h:165, karst.h:116, the
// biome beach band, the bake contract test), and any bake output change
// re-keys the fine namespace. So the tide is a number ADDED at the engine's
// composition seams (`oceanSurfaceMmAt`'s seaLevelNowMm argument, lakes.h)
// and nothing in worldgen ever sees it. Everything in this header is relative
// millimetres about zero.
//
// PURE f(epoch), NOT SAVED STATE, and that is a design decision rather than a
// shortcut. The tide is driven from the same clock as the sun and the moon
// (FVoxelSkyState::EpochSeconds); a client that knows the epoch knows the
// tide, exactly as it knows where the moon is. Nothing is added to save
// games, nothing accumulates, nothing can drift: two machines that agree on
// the clock agree on the waterline BY CONSTRUCTION, and a machine that
// crashes and reloads is at the right tide the frame it wakes.
//
// WHY AN INTEGER PHASE AND A SINE TABLE, NOT libm. The datum this offset
// feeds decides which voxels are water (the ImplicitFn wall, mobilization
// debits, the ledger). `sin()` is not bit-specified across libms, compilers
// or FMA settings, so a floating tide could put ONE VOXEL of disagreement
// between two machines at the same (spec, epoch) -- the exact class of drift
// weather.h's doctrine note banned floats from world-derivation for, and this
// repo has been bitten by before. Everything below is int64 arithmetic over a
// compile-time table; constexpr integer evaluation is exact by the standard,
// so the table itself is identical on every compiler that builds it.
//
// TWO CADENCES LIVE DOWNSTREAM OF THIS FILE, and it serves both from one
// clock: the CONTINUOUS offset (`tideOffsetMm`) feeds material parameters
// every tick and never re-meshes anything; the DATUM steps only when
// `quantiseTideMm` crosses a quantum, which is what bounds
// waterline-through-voxel churn. The quantum also absorbs the table's
// residue: 4096 entries put adjacent-index steps at ~1.2 mm for a
// spring-tide-sized amplitude, far below the 25 mm default quantum, so LUT
// granularity can never be the thing that flips a datum step.

#include <cstdint>

#include "voxelcore/core.h" // floorDiv/floorMod -- the one division idiom

namespace vxc {

// Up to four superposed oscillators. Four is enough to express the reference
// behaviour (a principal lunar-ish period plus a solar-ish beat gives spring/
// neap envelopes for free) and small enough that the whole spec is a value
// type an engine cvar string can carry.
inline constexpr int32_t kMaxTideComponents = 4;

struct TideComponent {
    int32_t periodS = 0;         // full cycle, in seconds of sky-clock time; <= 0 = inert
    int32_t amplitudeMm = 0;     // peak offset this component contributes, either side of 0
    int32_t phaseMilliTurns = 0; // phase at epoch 0, in 1/1000 of a turn (wraps mod 1000)
};

struct TideSpec {
    TideComponent comp[kMaxTideComponents] = {};
    int32_t count = 0; // 0..kMaxTideComponents; 0 == no tide, offset is exactly 0
};

// --- the sine table ---------------------------------------------------------
//
// 4096 entries over one turn, Q16 (kTideSinOne == one). 4096 is the phase
// resolution of the whole system: fine enough that the coarsest visible step
// (max amplitude x 2pi / 4096) sits under 2 mm for any sane spec, coarse
// enough that the table is 16 KB and a cache-resident read.
//
// GENERATED AT COMPILE TIME, NOT HAND-WRITTEN, but by the same rule as the
// hand-written tables (detail_bedding.h:57-61): the entries are ordinary
// literal integer data by the time anything runs, computed by an integer
// Taylor series in Q30 over the first quarter wave and reflected. Constexpr
// integer arithmetic is exact and mandated, so every compiler produces the
// identical 4096 numbers -- the cross-machine agreement is the standard's
// promise, not a build setting's.
//
// THE QUARTER-WAVE SYMMETRY IS EXACT BY CONSTRUCTION, and that is
// load-bearing for tests and for the velocity function: sin[k + 2048] ==
// -sin[k] and sin[2048 - k] == sin[k] identically, with no rounding error,
// because the other three quadrants are copies of the first rather than
// separate evaluations. cos is then sin at (k + 1024) with no second table.
inline constexpr int32_t kTideLutSize = 4096;
inline constexpr int32_t kTideSinOne = 65536; // Q16

namespace tidedetail {

// sin(theta) for theta in [0, pi/2], Q30 in, Q30 out. Taylor through the
// theta^11 term, factored Horner-style so every intermediate is positive and
// every product fits int64 (widest: t * thetaSq <= 2^30 * 2.65e9 ~= 2.85e18
// < 2^63). Truncation error of the series is theta^13/13! ~= 5.7e-8 at
// theta = pi/2 -- three orders below one Q16 LSB, so the table's accuracy is
// set by the Q30->Q16 rounding, not by the mathematics.
constexpr int64_t tideSinQ30(int64_t thetaQ30) {
    constexpr int64_t kQ = int64_t(1) << 30;
    const int64_t t2 = (thetaQ30 * thetaQ30) / kQ; // theta^2, Q30
    int64_t s = kQ - t2 / 110;                     // 1 - t2/(10*11)
    s = kQ - (t2 * s / kQ) / 72;                   // 1 - t2/(8*9) * s
    s = kQ - (t2 * s / kQ) / 42;                   // 1 - t2/(6*7) * s
    s = kQ - (t2 * s / kQ) / 20;                   // 1 - t2/(4*5) * s
    s = kQ - (t2 * s / kQ) / 6;                    // 1 - t2/(2*3) * s
    return (thetaQ30 * s) / kQ;                    // theta * s
}

struct TideSinTable {
    int32_t v[kTideLutSize] = {};
};

constexpr TideSinTable makeTideSinTable() {
    TideSinTable t{};
    // 2*pi in Q30, truncated. The truncation is 4e-11 relative -- invisible
    // under the Q16 rounding below -- and truncation rather than rounding
    // keeps the constant derivable by eye from the digits of pi.
    constexpr int64_t kTwoPiQ30 = 6746518852;
    // First quadrant by the series; the endpoint pinned exactly. sin(pi/2) is
    // 1 by identity, and pinning it (rather than trusting the series' last
    // few Q30 counts through the rounding) is what makes "a lone component's
    // sweep attains exactly its amplitude" a testable equality downstream.
    for (int32_t k = 0; k < kTideLutSize / 4; ++k) {
        const int64_t thetaQ30 = (kTwoPiQ30 * k) / kTideLutSize;
        const int64_t sQ30 = tideSinQ30(thetaQ30);
        t.v[k] = int32_t((sQ30 + (int64_t(1) << 13)) >> 14); // Q30 -> Q16, round-half-up
    }
    t.v[kTideLutSize / 4] = kTideSinOne;
    // Remaining three quadrants are exact reflections -- no arithmetic, so no
    // new rounding and the symmetries hold to the bit.
    for (int32_t k = kTideLutSize / 4 + 1; k <= kTideLutSize / 2; ++k) {
        t.v[k] = t.v[kTideLutSize / 2 - k];
    }
    for (int32_t k = kTideLutSize / 2 + 1; k < kTideLutSize; ++k) {
        t.v[k] = int32_t(-int64_t(t.v[k - kTideLutSize / 2]));
    }
    return t;
}

} // namespace tidedetail

inline constexpr tidedetail::TideSinTable kTideSin = tidedetail::makeTideSinTable();

// --- phase ------------------------------------------------------------------
//
// MILLISECONDS IN, and that is the one place this header refines the plan's
// sketch: the continuous offset feeds material parameters every tick, and at
// whole-second phase a spring tide moves in ~7 mm pops once a second --
// visible on a shaded plane. Milliseconds put the granularity at the table's
// own (~1.2 mm), below anything a material can show. The engine derives
// epochMs from FVoxelSkyState::EpochSeconds once per tick; the DATUM path
// then quantises so hard that the sub-second refinement cannot move a step.
//
// floorMod BEFORE the multiply, so the product t * kTideLutSize is bounded by
// periodMs * 4096 (< 2^43 for any int32 period) regardless of how large the
// epoch grows -- a session's clock can run for years without this overflowing.
constexpr int32_t tidePhaseIndex(int32_t periodS, int32_t phaseMilliTurns, int64_t epochMs) {
    const int64_t periodMs = int64_t(periodS) * 1000;
    const int64_t t = floorMod(epochMs, periodMs);
    const int64_t base = floorDiv(t * kTideLutSize, periodMs);
    const int64_t phase = floorDiv(floorMod(int64_t(phaseMilliTurns), 1000) * kTideLutSize, 1000);
    return int32_t(floorMod(base + phase, kTideLutSize));
}

// --- the offset -------------------------------------------------------------
//
// Sum of the components' contributions, each floor-divided out of Q16
// independently. Per-component flooring (rather than one divide of the summed
// Q16 total) costs at most kMaxTideComponents-1 mm of downward bias and buys
// the property the determinism test pins: a spec's offset equals the sum of
// its components evaluated alone, so no composition order, grouping or
// incremental evaluation strategy can ever disagree with another.
//
// A component with periodS <= 0 contributes nothing rather than dividing by
// zero: the engine's spec parser refuses such a component loudly, and this
// arithmetic layer must still be total for whatever a test hands it.
constexpr int32_t tideOffsetMm(const TideSpec& spec, int64_t epochMs) {
    int64_t sum = 0;
    for (int32_t i = 0; i < spec.count && i < kMaxTideComponents; ++i) {
        const TideComponent& c = spec.comp[i];
        if (c.periodS <= 0) continue;
        const int32_t idx = tidePhaseIndex(c.periodS, c.phaseMilliTurns, epochMs);
        sum += floorDiv(int64_t(c.amplitudeMm) * kTideSin.v[idx], kTideSinOne);
    }
    return int32_t(sum);
}

// --- the velocity -----------------------------------------------------------
//
// Analytic d/dt of the offset -- amplitude * (2*pi/T) * cos -- via the same
// table read a quarter turn ahead, NOT a finite difference of two offsets: a
// difference of floored values stutters between 0 and 2x at exactly the slow
// speeds a material would show it at.
//
// MICROMETRES PER SECOND, because millimetres are too coarse for the number
// itself: a full spring tide peaks near 7 mm/s, and an int mm/s would step
// through seven visible values per half-cycle. This is the one COSMETIC
// output of the header (it feeds a material parameter and nothing else), so
// 2*pi is carried at 1e3 scale (0.003% low) -- far inside what any consumer
// of a flow hint can resolve, and it keeps the widest product under 2^60.
constexpr int64_t tideVelocityUmPerS(const TideSpec& spec, int64_t epochMs) {
    constexpr int64_t kTwoPiE3 = 6283; // 2*pi * 1000, truncated
    int64_t sum = 0;
    for (int32_t i = 0; i < spec.count && i < kMaxTideComponents; ++i) {
        const TideComponent& c = spec.comp[i];
        if (c.periodS <= 0) continue;
        const int32_t idx =
            int32_t(floorMod(tidePhaseIndex(c.periodS, c.phaseMilliTurns, epochMs) +
                                 int64_t(kTideLutSize) / 4,
                             kTideLutSize));
        sum += floorDiv(int64_t(c.amplitudeMm) * kTideSin.v[idx] * kTwoPiE3,
                        int64_t(kTideSinOne) * c.periodS);
    }
    return sum;
}

// --- the bound --------------------------------------------------------------
//
// Sum of |amplitude| -- the astronomical-high-water bound. It IS attained
// (every component hits +-amplitude exactly at its table cardinals, and
// phases align at some epoch for commensurable periods), and more
// importantly nothing can exceed it, which is what Phase C's rock-pool
// qualification ("sill below astronomical high water") and the arm log lean
// on. Inert components are excluded so the bound describes the spec that
// actually runs, not the string that was typed.
constexpr int32_t tideMaxMm(const TideSpec& spec) {
    int64_t sum = 0;
    for (int32_t i = 0; i < spec.count && i < kMaxTideComponents; ++i) {
        const TideComponent& c = spec.comp[i];
        if (c.periodS <= 0) continue;
        sum += c.amplitudeMm < 0 ? -int64_t(c.amplitudeMm) : int64_t(c.amplitudeMm);
    }
    return int32_t(sum);
}

// --- the quantiser ----------------------------------------------------------
//
// FLOOR TO THE QUANTUM, in the floorDiv sense, and the choice of floor over
// truncation is the whole content of this function. Truncation's bins are
// quantum-wide everywhere except around zero, where -q+1..q-1 all collapse to
// 0 -- a double-width dead band straddling the geological datum, which is
// exactly where every shoreline lives. Floor's bins are uniform, so the datum
// steps at the same offset spacing on the ebb as on the flood and a rising
// tide crosses each threshold exactly once (monotone: x <= y implies
// quantise(x) <= quantise(y), which the tests sweep).
//
// quantum <= 0 disables quantisation (identity) rather than dividing by zero;
// the engine clamps its cvar, this layer stays total.
constexpr int32_t quantiseTideMm(int32_t offsetMm, int32_t quantumMm) {
    if (quantumMm <= 0) return offsetMm;
    return int32_t(floorDiv(offsetMm, quantumMm) * quantumMm);
}

} // namespace vxc
