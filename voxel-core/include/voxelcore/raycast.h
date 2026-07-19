#pragma once
// Deterministic integer voxel raycast (Amanatides & Woo DDA, exact fractional
// boundary comparisons via cross-multiplication — no floats, doctrine §2.3).
// Serves dig/place targeting on clients AND server-side validation/NPC line
// checks; both must agree bit-exactly, which is why this lives in voxel-core
// and not in engine code.
//
// The ray is a segment: origin O (mm) to O+D (mm). Voxels are kVoxelSizeMm
// cubes; voxel v spans [v*100, (v+1)*100) per axis (floored indexing, so
// negative coordinates behave identically to positive ones).

#include "voxelcore/core.h"

namespace vxc {

struct RaycastHit {
    bool hit = false;
    int64_t vx = 0, vy = 0, vz = 0; // first solid voxel along the segment
    // Face of the solid voxel that was entered: axis (0=x,1=y,2=z) and sign
    // (+1 means the ray entered moving in +axis, i.e. through the voxel's
    // -axis face). faceAxis == -1 when the segment STARTS inside a solid
    // voxel (no face was crossed).
    int32_t faceAxis = -1;
    int32_t faceSign = 0;
    // Voxel adjacent to the entered face (the last empty voxel before the
    // hit) — the placement target. Equal to the hit voxel when faceAxis==-1.
    int64_t px = 0, py = 0, pz = 0;
};

// MaterialFn: MaterialId(int64 vx, int64 vy, int64 vz).
template <typename MaterialFn>
RaycastHit raycastVoxels(const MaterialFn& materialAt, int64_t oxMm, int64_t oyMm,
                         int64_t ozMm, int64_t dxMm, int64_t dyMm, int64_t dzMm,
                         int32_t maxSteps = 4096) {
    RaycastHit out;

    int64_t v[3] = {floorDiv(oxMm, kVoxelSizeMm), floorDiv(oyMm, kVoxelSizeMm),
                    floorDiv(ozMm, kVoxelSizeMm)};
    const int64_t o[3] = {oxMm, oyMm, ozMm};
    const int64_t d[3] = {dxMm, dyMm, dzMm};

    if (materialAt(v[0], v[1], v[2]) != MAT_AIR) {
        out.hit = true;
        out.vx = out.px = v[0];
        out.vy = out.py = v[1];
        out.vz = out.pz = v[2];
        return out;
    }

    // Per axis: step direction, and tMax as the exact fraction num[i]/|d[i]|
    // (parametric t in [0,1] along the segment to the next boundary crossing).
    // num[i] is the remaining mm to the next boundary along axis i, scaled to
    // the axis' own denominator; comparisons cross-multiply, additions add
    // kVoxelSizeMm to the numerator. All values stay far below int64 range
    // for any sane segment (|d| < 10^7 mm ⇒ products < 10^14).
    int step[3];
    int64_t num[3], den[3]; // den[i] = |d[i]|, 0 ⇒ axis never crosses
    for (int i = 0; i < 3; ++i) {
        if (d[i] > 0) {
            step[i] = 1;
            den[i] = d[i];
            num[i] = (v[i] + 1) * kVoxelSizeMm - o[i]; // in (0, 100]
        } else if (d[i] < 0) {
            step[i] = -1;
            den[i] = -d[i];
            num[i] = o[i] - v[i] * kVoxelSizeMm; // in [0, 100)
        } else {
            step[i] = 0;
            den[i] = 0;
            num[i] = 0;
        }
    }

    // a "beats" b when tMax[a] <= tMax[b] (ties broken by axis order x<y<z
    // for determinism; simultaneous crossings step one axis at a time).
    const auto lessEq = [&](int a, int b) {
        if (den[a] == 0) return false;
        if (den[b] == 0) return true;
        return num[a] * den[b] <= num[b] * den[a];
    };

    for (int32_t i = 0; i < maxSteps; ++i) {
        int axis;
        if (lessEq(0, 1) && lessEq(0, 2)) axis = 0;
        else if (lessEq(1, 2)) axis = 1;
        else axis = 2;

        if (den[axis] == 0) return out;              // degenerate zero ray
        if (num[axis] > den[axis]) return out;       // t > 1: segment exhausted

        const int64_t prev[3] = {v[0], v[1], v[2]};
        v[axis] += step[axis];
        num[axis] += kVoxelSizeMm;

        if (materialAt(v[0], v[1], v[2]) != MAT_AIR) {
            out.hit = true;
            out.vx = v[0];
            out.vy = v[1];
            out.vz = v[2];
            out.faceAxis = axis;
            out.faceSign = step[axis];
            out.px = prev[0];
            out.py = prev[1];
            out.pz = prev[2];
            return out;
        }
    }
    return out;
}

} // namespace vxc
