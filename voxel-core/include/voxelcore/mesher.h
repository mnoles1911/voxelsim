#pragma once
// Greedy mesher, CPU reference (plan §3.3 Band 1). Per-brick: for each of the
// six face directions, visible faces are greedily merged into maximal quads.
// Merge key = material + 4-corner voxel AO, so AO gradients never bleed across
// a merged quad. The GPU port must reproduce this output bit-exactly.
//
// The sampler must answer material queries for the brick interior AND a
// 1-voxel apron: coordinates in [-1, B] on every axis (neighbor bricks or the
// generator supply the apron).

#include <vector>

#include "voxelcore/core.h"

namespace vxc {

struct Quad {
    uint8_t axis;     // normal axis: 0=x, 1=y, 2=z
    uint8_t positive; // 1 if the face normal points toward +axis
    uint8_t slice;    // cell layer 0..B-1 owning the face
    uint8_t u0, v0;   // quad origin in slice coords (u=(axis+1)%3, v=(axis+2)%3)
    uint8_t w, h;     // quad extent along u, v (>=1)
    uint8_t ao;       // 2 bits per corner: (0,0),(1,0),(0,1),(1,1); 3 = unoccluded
    MaterialId mat;

    friend bool operator==(const Quad&, const Quad&) = default;
};

namespace detail {

// Classic 4-corner voxel AO (0=darkest, 3=open). s1/s2 are the two edge
// neighbors of the corner on the face plane, c the diagonal.
constexpr uint8_t aoCorner(bool s1, bool s2, bool c) {
    return (s1 && s2) ? 0 : static_cast<uint8_t>(3 - (int(s1) + int(s2) + int(c)));
}

} // namespace detail

// Sampler: MaterialId sampler(int x, int y, int z), valid on [-1, B]^3.
//
// Performance note (this is an implementation detail only — the emitted quad
// stream is byte-identical to the original straight-through version, and the
// HLSL port needs no corresponding change):
//
//  * The brick plus its 1-voxel apron is materialized ONCE into a flat array
//    before any face scan. The scan reads every voxel six times (once per
//    face direction) and reads nine more cells for each visible face's AO, so
//    a live sampler used to be re-evaluated ~10x per voxel. The apron is
//    exactly the sampler's documented domain, so no query outside the old
//    footprint is introduced and a pure sampler cannot observe the change.
//  * Addressing uses hoisted per-axis strides instead of a per-cell int c[3]
//    with runtime-varying subscripts. The old form forced the coordinate
//    triple to memory on every cell because axis/u/v are not compile-time
//    constants; strides make each neighbor a constant offset from the cell.
template <int B, typename Sampler>
void meshBrick(const Sampler& sampler, std::vector<Quad>& out) {
    constexpr int S = B + 2;               // apron-inclusive edge
    constexpr int kCells = S * S * S;
    constexpr int kStride[3] = {1, S, S * S};
    constexpr int kOrigin = 1 + S * (1 + S * 1); // index of voxel (0,0,0)

    MaterialId mat[kCells];
    {
        int idx = 0;
        for (int z = -1; z <= B; ++z)
            for (int y = -1; y <= B; ++y)
                for (int x = -1; x <= B; ++x) mat[idx++] = sampler(x, y, z);
    }

    // Early-out 1: no solid voxel in the brick interior means no face can have
    // a source material, whatever the apron holds.
    {
        bool anySolid = false;
        for (int z = 0; z < B && !anySolid; ++z)
            for (int y = 0; y < B && !anySolid; ++y) {
                const int rowBase = kOrigin + y * kStride[1] + z * kStride[2];
                for (int x = 0; x < B; ++x)
                    if (mat[rowBase + x] != MAT_AIR) {
                        anySolid = true;
                        break;
                    }
            }
        if (!anySolid) return;
    }

    // Early-out 2: a uniform apron+interior means every neighbor of every
    // interior voxel is solid, so nothing is visible. (The all-air case was
    // already caught above; this catches solid interior bricks, which mesh to
    // zero quads and should cost near nothing.)
    {
        const MaterialId m0 = mat[0];
        bool uniform = true;
        for (int i = 1; i < kCells; ++i)
            if (mat[i] != m0) {
                uniform = false;
                break;
            }
        if (uniform) return;
    }

    const auto solidAt = [&](int index) { return mat[index] != MAT_AIR; };

    // key: 0 = no face, else material | ao<<8 | 1<<16 (visible marker so
    // MAT_* values never collide with "no face").
    uint32_t mask[B * B];

    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const int sa = kStride[axis], su = kStride[u], sv = kStride[v];
        for (int dir = 0; dir < 2; ++dir) {
            const int nOff = dir ? sa : -sa;
            for (int slice = 0; slice < B; ++slice) {
                const int sliceBase = kOrigin + slice * sa;
                // Build the face mask for this slice.
                for (int j = 0; j < B; ++j) {
                    const int rowBase = sliceBase + j * sv;
                    for (int i = 0; i < B; ++i) {
                        const int c = rowBase + i * su;
                        const MaterialId m = mat[c];
                        uint32_t key = 0;
                        if (m != MAT_AIR) {
                            const int n = c + nOff;
                            if (!solidAt(n)) {
                                // AO from the 8 cells ringing the face on the
                                // neighbor plane.
                                const bool uNeg = solidAt(n - su), uPos = solidAt(n + su);
                                const bool vNeg = solidAt(n - sv), vPos = solidAt(n + sv);
                                uint8_t ao = 0;
                                ao |= detail::aoCorner(uNeg, vNeg, solidAt(n - su - sv));
                                ao |= detail::aoCorner(uPos, vNeg, solidAt(n + su - sv)) << 2;
                                ao |= detail::aoCorner(uNeg, vPos, solidAt(n - su + sv)) << 4;
                                ao |= detail::aoCorner(uPos, vPos, solidAt(n + su + sv)) << 6;
                                key = uint32_t(m) | uint32_t(ao) << 8 | 1u << 16;
                            }
                        }
                        mask[i + B * j] = key;
                    }
                }

                // Greedy rectangle merge over the mask.
                for (int j = 0; j < B; ++j) {
                    for (int i = 0; i < B;) {
                        const uint32_t key = mask[i + B * j];
                        if (!key) {
                            ++i;
                            continue;
                        }
                        int w = 1;
                        while (i + w < B && mask[i + w + B * j] == key) ++w;
                        int h = 1;
                        for (; j + h < B; ++h) {
                            bool rowOk = true;
                            for (int k = 0; k < w; ++k)
                                if (mask[i + k + B * (j + h)] != key) {
                                    rowOk = false;
                                    break;
                                }
                            if (!rowOk) break;
                        }
                        for (int dj = 0; dj < h; ++dj)
                            for (int di = 0; di < w; ++di) mask[i + di + B * (j + dj)] = 0;

                        Quad q;
                        q.axis = static_cast<uint8_t>(axis);
                        q.positive = static_cast<uint8_t>(dir);
                        q.slice = static_cast<uint8_t>(slice);
                        q.u0 = static_cast<uint8_t>(i);
                        q.v0 = static_cast<uint8_t>(j);
                        q.w = static_cast<uint8_t>(w);
                        q.h = static_cast<uint8_t>(h);
                        q.ao = static_cast<uint8_t>((key >> 8) & 0xff);
                        q.mat = static_cast<MaterialId>(key & 0xff);
                        out.push_back(q);
                        i += w;
                    }
                }
            }
        }
    }
}

inline void digestQuads(const std::vector<Quad>& quads, Digest& d) {
    for (const Quad& q : quads) {
        d.u8(q.axis);
        d.u8(q.positive);
        d.u8(q.slice);
        d.u8(q.u0);
        d.u8(q.v0);
        d.u8(q.w);
        d.u8(q.h);
        d.u8(q.ao);
        d.u8(q.mat);
    }
}

} // namespace vxc
