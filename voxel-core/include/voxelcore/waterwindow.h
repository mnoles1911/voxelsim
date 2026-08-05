#pragma once
// INCREMENTAL WATER WINDOWS -- why the near-field disc must not rebuild whole.
//
// ---------------------------------------------------------------------------
// THE DEFECT, IN ONE SENTENCE
//
// `RefreshImplicitWater` keys its rebuild on the camera's BRICK CENTRE, so any
// camera movement that crosses a 0.8 m boundary throws the entire candidate
// list away and re-sweeps 65 x 65 = 4,225 brick columns. Flying, that is
// several full re-sweeps per second for a step that changes a one-brick-wide
// margin.
//
// ---------------------------------------------------------------------------
// THE OBSERVATION THAT MAKES THE FIX EXACT RATHER THAN APPROXIMATE
//
// Read the sweep's own predicate. A brick (bx, by, bz) is a candidate iff:
//
//     the column (bx, by) is wet          -- depends on (bx, by)
//     bz <= floodBrickZ(bx, by)           -- depends on (bx, by, bz)
//     not proven-interior at (bx, by, bz) -- depends on (bx, by, bz)
//
// NOT ONE OF THOSE CLAUSES MENTIONS THE CAMERA. The candidate predicate is a
// property of the WORLD; the camera contributes only the BOX that clips it.
// So the candidate set is
//
//     candidates(camera) = P  intersect  Box(camera)
//
// and for a camera step old -> new,
//
//     candidates(new) = (candidates(old) intersect Box(new))
//                       union (P intersect (Box(new) \ Box(old)))
//
// The first term is FREE -- those bricks are already meshed and their contents
// cannot have changed, because P does not depend on the camera. Only the second
// term is work, and for a one-brick step it is a single slab: 65 columns of
// 4,225, i.e. 1.5%.
//
// THIS IS AN IDENTITY, NOT A HEURISTIC. The incremental result is bit-identical
// to the full rebuild -- there is no staleness to trade against, no hysteresis
// to tune, and no correctness argument to make beyond the one above. That is
// what `window_difference_is_exactly_the_set_difference` and
// `incremental_window_equals_full_rebuild` pin.
//
// ---------------------------------------------------------------------------
// AND THE SAME IDENTITY IS WHERE PER-RING INVALIDATION COMES FROM
//
// A far-field cascade ring at LOD L is built from bricks (8 << L) voxels on a
// side. Its window is quantised to ITS OWN brick size, so a camera step smaller
// than one of that ring's bricks does not move the window AT ALL and the ring
// does nothing. Ring L therefore refreshes 2^L times less often than ring 0 --
// which is the rule the far-water plan asks for, arrived at as a consequence of
// quantisation rather than as a separate throttle with a separate tuning knob.
//
// `waterWindowCentreBrick` is the whole of it: floorDiv by the level's brick
// edge. Nothing else in this header knows about LODs, because nothing else has
// to.

#include <cstdint>

#include "voxelcore/core.h"
// WaterBrick8::kEdge lives here, not in brick.h.
#include "voxelcore/waterca.h"

namespace vxc {

// The brick a camera voxel coordinate falls in, in the bricks of level `lod`.
// A brick is 8 cells at every level and a cell is (kVoxelSizeMm << lod), so the
// brick edge in VOXELS is (8 << lod).
//
// THIS FUNCTION IS THE PER-RING INVALIDATION RULE. Ring L's window only moves
// when this value changes, and it changes 2^L times less often than ring 0's.
constexpr int64_t waterWindowCentreBrick(int64_t camVoxel, int lod) {
    return floorDiv(camVoxel, int64_t(WaterBrick8::kEdge) << lod);
}

// An INCLUSIVE box of brick coordinates, in the bricks of one level.
// `x1 < x0` (etc.) means empty, the same convention FarWaterBrickRange uses.
struct WaterWindow {
    int64_t x0 = 0, x1 = -1;
    int64_t y0 = 0, y1 = -1;
    int64_t z0 = 0, z1 = -1;

    constexpr bool empty() const { return x1 < x0 || y1 < y0 || z1 < z0; }
    constexpr int64_t spanX() const { return x1 >= x0 ? x1 - x0 + 1 : 0; }
    constexpr int64_t spanY() const { return y1 >= y0 ? y1 - y0 + 1 : 0; }
    constexpr int64_t spanZ() const { return z1 >= z0 ? z1 - z0 + 1 : 0; }
    constexpr int64_t count() const { return empty() ? 0 : spanX() * spanY() * spanZ(); }

    constexpr bool contains(int64_t bx, int64_t by, int64_t bz) const {
        return !empty() && bx >= x0 && bx <= x1 && by >= y0 && by <= y1 && bz >= z0 && bz <= z1;
    }
    constexpr bool operator==(const WaterWindow& o) const {
        if (empty() && o.empty()) return true;
        return x0 == o.x0 && x1 == o.x1 && y0 == o.y0 && y1 == o.y1 && z0 == o.z0 && z1 == o.z1;
    }
    constexpr bool operator!=(const WaterWindow& o) const { return !(*this == o); }
};

// The camera-centred window: radius `rXY` bricks on each horizontal axis and
// `rZ` on the vertical, inclusive both ends -- i.e. exactly the shape
// `RefreshImplicitWater` sweeps today (rXY = 32, rZ = 16 gives 65 x 65 x 33).
constexpr WaterWindow waterWindowAt(int64_t cx, int64_t cy, int64_t cz, int64_t rXY, int64_t rZ) {
    WaterWindow w;
    if (rXY < 0 || rZ < 0) return w; // stays empty
    w.x0 = cx - rXY;
    w.x1 = cx + rXY;
    w.y0 = cy - rXY;
    w.y1 = cy + rXY;
    w.z0 = cz - rZ;
    w.z1 = cz + rZ;
    return w;
}

// The intersection, which is the set of bricks that SURVIVE a camera step --
// already meshed, provably unchanged, and the whole saving.
constexpr WaterWindow waterWindowIntersect(const WaterWindow& a, const WaterWindow& b) {
    WaterWindow r;
    if (a.empty() || b.empty()) return r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    r.z0 = a.z0 > b.z0 ? a.z0 : b.z0;
    r.z1 = a.z1 < b.z1 ? a.z1 : b.z1;
    if (r.x1 < r.x0 || r.y1 < r.y0 || r.z1 < r.z0) return WaterWindow{};
    return r;
}

// `waterWindowDifference` writes at most this many boxes.
//
// SIX, and the number is structural rather than a safe over-estimate: the
// difference is peeled one axis at a time, and each axis contributes the slab
// below the overlap and the slab above it. Three axes, two slabs each.
inline constexpr int kWaterWindowMaxRegions = 6;

// Decompose `nw \ old` into DISJOINT boxes whose union is exactly that set
// difference. Returns how many were written (0 .. kWaterWindowMaxRegions).
//
// This is the "shift and fill" in its entirety. Sweep these boxes and nothing
// else; every brick outside them that is in `nw` was already in `old`.
//
// The peel order is x, then y within the surviving x, then z within the
// surviving x and y -- which is what keeps the boxes disjoint. Sweeping the
// naive "three slabs, full extent each" instead double-counts every edge and
// every corner, and double-counting here means re-meshing a brick that did not
// change, which is the exact cost this is removing.
inline int waterWindowDifference(const WaterWindow& nw, const WaterWindow& old, WaterWindow* out) {
    int n = 0;
    if (nw.empty()) return 0;
    if (old.empty()) {
        out[n++] = nw;
        return n;
    }

    // The surviving x band, then the surviving (x, y) column band.
    const int64_t ox0 = nw.x0 > old.x0 ? nw.x0 : old.x0;
    const int64_t ox1 = nw.x1 < old.x1 ? nw.x1 : old.x1;
    const bool xOverlaps = ox0 <= ox1;

    // Below and above the overlap on x: full y and z extent of the new window.
    {
        const int64_t lo = nw.x0;
        const int64_t hi = xOverlaps ? (ox0 - 1) : nw.x1;
        if (lo <= hi) out[n++] = WaterWindow{lo, hi, nw.y0, nw.y1, nw.z0, nw.z1};
    }
    if (xOverlaps && ox1 + 1 <= nw.x1) {
        out[n++] = WaterWindow{ox1 + 1, nw.x1, nw.y0, nw.y1, nw.z0, nw.z1};
    }
    if (!xOverlaps) return n; // the whole new window is new; y and z peel nothing

    const int64_t oy0 = nw.y0 > old.y0 ? nw.y0 : old.y0;
    const int64_t oy1 = nw.y1 < old.y1 ? nw.y1 : old.y1;
    const bool yOverlaps = oy0 <= oy1;
    {
        const int64_t lo = nw.y0;
        const int64_t hi = yOverlaps ? (oy0 - 1) : nw.y1;
        if (lo <= hi) out[n++] = WaterWindow{ox0, ox1, lo, hi, nw.z0, nw.z1};
    }
    if (yOverlaps && oy1 + 1 <= nw.y1) {
        out[n++] = WaterWindow{ox0, ox1, oy1 + 1, nw.y1, nw.z0, nw.z1};
    }
    if (!yOverlaps) return n;

    const int64_t oz0 = nw.z0 > old.z0 ? nw.z0 : old.z0;
    const int64_t oz1 = nw.z1 < old.z1 ? nw.z1 : old.z1;
    const bool zOverlaps = oz0 <= oz1;
    {
        const int64_t lo = nw.z0;
        const int64_t hi = zOverlaps ? (oz0 - 1) : nw.z1;
        if (lo <= hi) out[n++] = WaterWindow{ox0, ox1, oy0, oy1, lo, hi};
    }
    if (zOverlaps && oz1 + 1 <= nw.z1) {
        out[n++] = WaterWindow{ox0, ox1, oy0, oy1, oz1 + 1, nw.z1};
    }
    return n;
}

// The COLUMN footprint of the newly-entered region, which is a smaller set than
// the brick region and is the one that actually costs.
//
// WHY IT IS SEPARATE. The expensive half of the sweep is per-COLUMN -- the
// datum resolve and the amplified-ground bound, one query per (bx, by) -- and
// the vertical loop over it is nearly free. A pure-ALTITUDE camera step enters
// a new z slab across the whole footprint, so it enters thousands of new BRICK
// slots while entering ZERO new columns: every column's datum and ground are
// already known and only the z clip moved. Counting columns and bricks with the
// same number would report that step as a full rebuild when it is free.
//
// This is the 2D footprint difference and it is computed DIRECTLY, not filtered
// out of `waterWindowDifference`'s output. Inferring it from the 3D peel by
// asking which regions span the full z extent is wrong in one real case: when
// the camera jumps more than the window's own z height (26.4 m at rZ = 16 --
// a teleport, or a fast dive) the z ranges do not overlap at all, the z peel
// then spans the full new z extent too, and the filter counts every surviving
// column as new. Those columns are not new; only their z clip moved. The 2D
// difference cannot express that mistake.
//
// Writes at most 4 boxes. The z fields carry the new window's extent so the
// caller can sweep them directly, but it is the x/y extents that are the claim.
inline constexpr int kWaterWindowMaxColumnRegions = 4;

inline int waterWindowColumnDifference(const WaterWindow& nw, const WaterWindow& old,
                                       WaterWindow* out) {
    int n = 0;
    if (nw.empty()) return 0;
    if (old.empty()) {
        out[n++] = nw;
        return n;
    }
    const int64_t ox0 = nw.x0 > old.x0 ? nw.x0 : old.x0;
    const int64_t ox1 = nw.x1 < old.x1 ? nw.x1 : old.x1;
    const bool xOverlaps = ox0 <= ox1;
    {
        const int64_t lo = nw.x0;
        const int64_t hi = xOverlaps ? (ox0 - 1) : nw.x1;
        if (lo <= hi) out[n++] = WaterWindow{lo, hi, nw.y0, nw.y1, nw.z0, nw.z1};
    }
    if (xOverlaps && ox1 + 1 <= nw.x1) {
        out[n++] = WaterWindow{ox1 + 1, nw.x1, nw.y0, nw.y1, nw.z0, nw.z1};
    }
    if (!xOverlaps) return n;
    const int64_t oy0 = nw.y0 > old.y0 ? nw.y0 : old.y0;
    const int64_t oy1 = nw.y1 < old.y1 ? nw.y1 : old.y1;
    const bool yOverlaps = oy0 <= oy1;
    {
        const int64_t lo = nw.y0;
        const int64_t hi = yOverlaps ? (oy0 - 1) : nw.y1;
        if (lo <= hi) out[n++] = WaterWindow{ox0, ox1, lo, hi, nw.z0, nw.z1};
    }
    if (yOverlaps && oy1 + 1 <= nw.y1) {
        out[n++] = WaterWindow{ox0, ox1, oy1 + 1, nw.y1, nw.z0, nw.z1};
    }
    return n;
}

} // namespace vxc
