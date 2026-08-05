#pragma once
// FAR-FIELD VOXEL WATER -- the rules that let water be drawn as VOXELS out to a
// kilometre instead of as the flat quads that stop at 25.6 m.
//
// ---------------------------------------------------------------------------
// WHAT WAS WRONG, IN NUMBERS
//
// `RefreshImplicitWater` finds water by SWEEPING: 65 x 65 = 4,225 brick columns
// in a box around the camera, each one asked "are you wet". The box is
// +/-25.6 m horizontally and +/-12.8 m vertically. Beyond it rivers are flat
// ribbon quads and lakes are flat sheet rects, which is the thing the owner
// rejected.
//
// The box cannot simply be grown. Columns swept go as the SQUARE of the radius:
//
//      25.6 m       4,225 columns      1x
//     500   m   1,563,001 columns    370x
//   1,000   m   6,255,001 columns  1,480x
//
// and each column costs an amplifier evaluation and a datum resolve.
//
// ---------------------------------------------------------------------------
// THE FIRST FIX: ENUMERATE, DO NOT SEARCH
//
// The sweep is dense only because it does not know where the water is. The
// baked water plane does. Measured on the bv13 tiles, per `vxc_farwaterprobe`:
//
//   wet-country (6 tiles)   68.7% of water blocks are CONSTANT-dry
//   arid corridor (4 tiles) 79.0% of water blocks are CONSTANT-dry
//
// and a CONSTANT block owns no data-section entry at all, so those are rejected
// from the block index with zero bytes fetched and zero decodes. Of what
// remains, 0.53% (wet country) and 1.08% (arid) of fine pixels are wet. So the
// candidate set comes from the plane block-major -- exactly as
// `riverRibbonFillWet` already does it -- and the cost stops scaling with SWEPT
// AREA and starts scaling with WATER AREA.
//
// ---------------------------------------------------------------------------
// THE SECOND FIX, AND IT IS NOT OPTIONAL: LOD
//
// Enumerating instead of sweeping fixes the SEARCH cost. It does not fix the
// DRAW cost, and measurement says the draw cost is the binding one. Surface
// bricks (bricks that emit at least one face) and real `meshBrick<8>` quads
// within 1 km at full 0.1 m resolution, against what today's 25.6 m box
// actually meshes at the same camera:
//
//   site                          today          full-res 1 km
//   wet-country braided reach     8,409 brk        630,523 brk / 1.14 M quads
//   wet-country lake reach            0 brk        346,185 brk / 0.88 M quads
//   arid corridor, 100%-wet block 8,450 brk      7,232,575 brk / 10.9 M quads
//
// That is 75x to 860x, and 166 MB to 1.57 GB of quad data. FULL-RESOLUTION
// VOXEL WATER TO 1 KM IS NOT AFFORDABLE. It is not close.
//
// A distance-doubling cascade is. LOD L covers [base << (L-1), base << L) and
// uses a (0.1 << L) m voxel, so each ring quadruples in area and quarters in
// resolution and the cost per ring is FLAT. Measured, base 32 m, six rings out
// to 1,024 m, surface bricks per ring:
//
//   arid 100%-wet block   10,026  7,536  3,768  3,760  3,628  2,341
//   wet-country braided   10,012  3,009  2,334  1,016    408     93
//
// Totals to 1,024 m: 31,059 and 16,872 surface bricks, 49,528 and 38,282
// quads. Against today's 8,450 and 8,409 bricks at 25.6 m, that is 2.0x-3.7x
// the brick count for FORTY TIMES the radius. The flatness of those rows is
// the whole argument: it is what "the cost is bounded by the ring count, not
// by the radius" looks like when it is measured rather than asserted.
//
// ---------------------------------------------------------------------------
// WHAT THIS HEADER IS FOR
//
// The rules only, integer-only, no sampler and no streaming: the fill, the ring
// selection, the coarse-column aggregation, the brick z range and the interior
// test. They live here for the reason `implicitWaterFill` and
// `implicitWaterCeilingMm` live in lakes.h -- so the client's binding site and
// the tests cannot express them differently. Enumeration stays at the binding
// site, where residency and the tile streamer already are.

#include <cstdint>

#include "voxelcore/brick.h"
#include "voxelcore/core.h"
#include "voxelcore/lakes.h"
// WaterBrick8::kEdge lives here, not in brick.h. The far field reuses the
// water brick exactly -- 8 cells at every level -- so meshBrick<8> and the
// whole upload path are untouched.
#include "voxelcore/waterca.h"

namespace vxc {

// The near field is LOD 0 and MUST stay LOD 0. Everything in this header is
// written so that lod == 0 reduces exactly to what the client already does.
inline constexpr int kFarWaterLod0 = 0;

// A practical ceiling. At base 32 m, LOD 5 is a 3.2 m voxel reaching 1,024 m;
// past that the ring holds so few wet columns that another level buys nothing
// (measured: 93 surface bricks in the 512-1,024 m ring on the wet-country
// braided reach, and 0 in the 1,024-2,048 m ring at base 64).
inline constexpr int kFarWaterMaxLod = 5;

// The voxel edge at a level, in mm. LOD 0 is kVoxelSizeMm by construction.
constexpr int64_t farWaterCellMm(int lod) { return int64_t(kVoxelSizeMm) << lod; }

// The brick edge at a level, in mm -- still 8 cells, just bigger ones. A brick
// stays 8^3 at every level so `meshBrick<8>`, `WaterBrick8` and the whole mesh
// and upload path are untouched; only the scale the brick is drawn at changes.
// That is deliberately the same trick the terrain rings use
// (`VoxelChunkComponent.cpp`'s `VoxelSizeUU * (1 << ChunkLevel)`).
constexpr int64_t farWaterBrickMm(int lod) { return farWaterCellMm(lod) * WaterBrick8::kEdge; }

// How many LOD-0 brick columns a LOD-L column covers on each axis.
constexpr int64_t farWaterStep(int lod) { return int64_t(1) << lod; }

// THE RING RULE. A column `distBricks` LOD-0 bricks from the camera is drawn at
// the level whose ring contains it: LOD 0 inside `baseBricks`, then a doubling
// per level, clamped to `maxLod`.
//
// EXPRESSED AS A COMPARISON LADDER, not a log2, because the answer must be
// exact at the boundary and a float log2 at 2^k is the classic off-by-one. Six
// iterations at most.
constexpr int farWaterLodForDistance(int64_t distBricks, int64_t baseBricks, int maxLod) {
    if (baseBricks <= 0) return 0;
    if (distBricks < baseBricks) return 0;
    int lod = 0;
    int64_t edge = baseBricks;
    while (lod < maxLod && distBricks >= edge) {
        edge *= 2;
        ++lod;
    }
    return lod;
}

// THE FILL RULE, and one branch of it is a measurement, not a derivation.
//
// At LOD 0 this IS `implicitWaterFill`, called rather than reimplemented. The
// near field must not move by one unit and the only way to guarantee that is to
// run the same function the client runs.
//
// ABOVE LOD 0 THE GROUND TEST CHANGES, and the reason is that the fine rule
// fails TOTALLY at coarse scale rather than gracefully. `implicitWaterFill`
// rejects a cell whose BOTTOM is below the ground, which is right at 100 mm --
// the ground is flat across the cell, so a cell starting under it is inside
// rock. It is wrong at 1.6 m. The water this world actually has is p50 0.75 m
// deep (wet-country braided reach) to p50 1.18 m (wet-country lake reach), so
// at LOD 4 the ENTIRE water column fits inside one cell; and if the ground
// falls in that cell's interior, the cell below is rejected for starting under
// the ground while the cell above starts above the datum. Every cell reads dry.
//
// Measured, before this branch existed: LOD 4 offered 138 surface bricks at
// 100 m and meshed ZERO QUADS. The river did not get coarser with distance, it
// DISAPPEARED -- silently, and in exactly the direction ("there is no water
// out there") that this whole feature exists to fix.
//
// So above LOD 0 a cell is rejected only when it lies ENTIRELY below the
// ground; a cell the ground passes through still takes its fill from the datum.
// That is also the physically right reading at this scale -- the ground is
// inside the cell and the water sits on top of it -- and it is safe to DRAW
// because the surface's true height is carried by the 8-bit corner heights
// (`BuildWaterCornerField` / `EmitWaterQuads`), not by which cell the quad
// lands in. A 1.6 m cell still puts its surface at the right millimetre.
constexpr uint8_t farWaterFill(int64_t zBottomMm, int32_t groundMm, int32_t surfaceMm,
                               int64_t cellMm) {
    if (cellMm == int64_t(kVoxelSizeMm)) {
        return implicitWaterFill(zBottomMm / int64_t(kVoxelSizeMm), groundMm, surfaceMm, false);
    }
    if (surfaceMm == kNoWaterMm) return 0;
    if (zBottomMm + cellMm <= int64_t(groundMm)) return 0; // entirely inside rock
    const int64_t rem = int64_t(surfaceMm) - zBottomMm;
    if (rem <= 0) return 0;
    if (rem >= cellMm) return 255;
    return static_cast<uint8_t>((rem * 255 + cellMm / 2) / cellMm);
}

// One water column: the amplified ground (#3, what the renderer draws) and the
// composed datum. `datumMm == kNoWaterMm` is dry, and it is NOT depth zero.
struct FarWaterColumn {
    int32_t groundMm = 0;
    int32_t datumMm = kNoWaterMm;

    constexpr bool wet() const {
        return datumMm != kNoWaterMm && int64_t(datumMm) > int64_t(groundMm);
    }
};

// Accumulates the LOD-0 columns under one coarse column.
//
// THE MAJORITY RULE IS THE SAME SHAPE AS mips.h's `solidThreshold = 4 of 8`,
// and both alternatives are wrong in a way that shows on screen. A strict-ANY
// rule grows every river outward by one coarse cell per level, so a 1.9 m
// ribbon is 3.2 m wide at LOD 5 and the water climbs its own banks. A
// strict-ALL rule erases every river narrower than the cell, which at LOD 5 is
// most of them -- 99.21% of wet pixels were a single 1.875 m pixel before the
// width law landed, and 7.50 m at p90 after it.
//
// GROUND AND DATUM ARE MEANS OVER THE WET CHILDREN, not extremes. A max datum
// lifts the coarse surface to the highest point of a descending reach and makes
// every ring boundary a visible step; a min ground digs the water into the
// bank. The mean holds the coarse surface at the same height as the fine one,
// which is what makes the ring boundary invisible.
class FarWaterAccumulator {
public:
    constexpr void add(const FarWaterColumn& c) {
        ++seen_;
        if (!c.wet()) return;
        ++wet_;
        sumGroundMm_ += c.groundMm;
        sumDatumMm_ += c.datumMm;
    }

    // `step` is farWaterStep(lod); the coarse column covers step*step children.
    constexpr bool resolve(int64_t step, FarWaterColumn& out) const {
        const int64_t children = step * step;
        if (wet_ == 0) return false;
        if (wet_ * 2 < children) return false; // strictly fewer than half: dry
        out.groundMm = static_cast<int32_t>(sumGroundMm_ / wet_);
        out.datumMm = static_cast<int32_t>(sumDatumMm_ / wet_);
        return true;
    }

    constexpr int64_t wetChildren() const { return wet_; }
    constexpr int64_t seenChildren() const { return seen_; }

private:
    int64_t seen_ = 0;
    int64_t wet_ = 0;
    int64_t sumGroundMm_ = 0;
    int64_t sumDatumMm_ = 0;
};

// The inclusive brick z range a column's water occupies at a level.
//
// THIS IS THE VERTICAL BOUND, AND IT REPLACES `kImplicitRadiusBricksZ`. The
// near field's +/-12.8 m box around the CAMERA means water stops existing when
// the camera climbs 13 m, which is why a capture at 90 m logs `0 candidate
// brick(s)` and why flying loses the river. The range here is a property of the
// WATER COLUMN -- ground to datum -- so a camera at 200 m or 5 km still sees
// every reach below it, and a column 1 m deep still costs the one or two bricks
// it actually needs rather than 33.
struct FarWaterBrickRange {
    int64_t z0 = 0;
    int64_t z1 = -1; // z1 < z0 means "no bricks"
    constexpr bool any() const { return z1 >= z0; }
    constexpr int64_t count() const { return any() ? (z1 - z0 + 1) : 0; }
};

constexpr FarWaterBrickRange farWaterBrickRange(const FarWaterColumn& c, int64_t cellMm) {
    FarWaterBrickRange r;
    if (!c.wet()) return r;
    const int64_t gz = floorDiv(int64_t(c.groundMm), cellMm);
    const int64_t dz = floorDiv(int64_t(c.datumMm) - 1, cellMm);
    r.z0 = floorDiv(gz, int64_t(WaterBrick8::kEdge));
    r.z1 = floorDiv(dz, int64_t(WaterBrick8::kEdge));
    return r;
}

// Is this brick provably faceless?
//
// The near field's own interior proof, generalised from a single column to the
// 3 x 3 the mesh apron can read. A brick emits no face iff EVERY padded cell is
// full water: every one of the nine columns must have its ground at or below
// the pad bottom and its datum a full cell above the pad top. A dry neighbour
// is an immediate no -- that is the shoreline, and it is the one place a wrong
// skip punches a hole in a water surface.
//
// `col(dx, dy)` returns the coarse column at that offset, dry-by-default
// outside the grid.
template <class ColFn>
constexpr bool farWaterBrickIsInterior(int64_t bz, int64_t cellMm, ColFn&& col) {
    const int64_t edge = int64_t(WaterBrick8::kEdge);
    const int64_t padBottomMm = (bz * edge - 1) * cellMm;
    const int64_t padTopMm = (bz * edge + edge) * cellMm;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const FarWaterColumn n = col(dx, dy);
            if (!n.wet()) return false;
            if (int64_t(n.groundMm) > padBottomMm) return false;
            if (padTopMm + cellMm > int64_t(n.datumMm)) return false;
        }
    }
    return true;
}

// The outer radius of the whole cascade, in LOD-0 bricks -- i.e. where the far
// field stops and the flat ribbon and sheet quads take over again.
//
// The far/flat handover has to be cut against THIS, not against the 25.6 m
// implicit disc, or the flat quads are drawn underneath the voxel water for the
// whole cascade and every one of the two-renderer tone problems shows up over a
// kilometre instead of over 52 m.
constexpr int64_t farWaterOuterBricks(int64_t baseBricks, int maxLod) {
    return baseBricks <= 0 ? 0 : baseBricks * (int64_t(1) << maxLod);
}

} // namespace vxc
