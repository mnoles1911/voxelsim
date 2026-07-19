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
template <int B, typename Sampler>
void meshBrick(const Sampler& sampler, std::vector<Quad>& out) {
    const auto solid = [&](int x, int y, int z) { return sampler(x, y, z) != MAT_AIR; };

    // key: 0 = no face, else material | ao<<8 | 1<<16 (visible marker so
    // MAT_* values never collide with "no face").
    uint32_t mask[B * B];

    for (int axis = 0; axis < 3; ++axis) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        for (int dir = 0; dir < 2; ++dir) {
            const int nOff = dir ? 1 : -1;
            for (int slice = 0; slice < B; ++slice) {
                // Build the face mask for this slice.
                for (int j = 0; j < B; ++j) {
                    for (int i = 0; i < B; ++i) {
                        int c[3];
                        c[axis] = slice;
                        c[u] = i;
                        c[v] = j;
                        const MaterialId m = sampler(c[0], c[1], c[2]);
                        uint32_t key = 0;
                        if (m != MAT_AIR) {
                            int n[3] = {c[0], c[1], c[2]};
                            n[axis] += nOff;
                            if (!solid(n[0], n[1], n[2])) {
                                // AO from the 8 cells ringing the face on the
                                // neighbor plane.
                                const auto planeSolid = [&](int du, int dv) {
                                    int p[3] = {n[0], n[1], n[2]};
                                    p[u] += du;
                                    p[v] += dv;
                                    return solid(p[0], p[1], p[2]);
                                };
                                const bool uNeg = planeSolid(-1, 0), uPos = planeSolid(1, 0);
                                const bool vNeg = planeSolid(0, -1), vPos = planeSolid(0, 1);
                                uint8_t ao = 0;
                                ao |= detail::aoCorner(uNeg, vNeg, planeSolid(-1, -1));
                                ao |= detail::aoCorner(uPos, vNeg, planeSolid(1, -1)) << 2;
                                ao |= detail::aoCorner(uNeg, vPos, planeSolid(-1, 1)) << 4;
                                ao |= detail::aoCorner(uPos, vPos, planeSolid(1, 1)) << 6;
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
