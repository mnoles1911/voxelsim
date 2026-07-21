#pragma once
// Amplifier v0 (plan §3.1 step 3, M0 scope): bit-deterministic synthesis of
// 0.1m detail from 30m-class tiles. CPU reference implementation — the GPU
// compute port must match it bit-exactly (CI-enforced once GPU runners exist).
//
// v0 = bilinear tile base + slope-scaled integer-hash fractal detail + column
// stratigraphy (topsoil/subsoil/rock/bedrock, climate-conditioned surface
// material). Later versions add erosion stamps, riverbed carving, caves,
// vegetation placement.

#include "voxelcore/tiles.h"

namespace vxc {

struct ColumnSample {
    int32_t surfaceMm = 0;      // terrain surface elevation, mm above sea level
    int32_t topsoilMm = 0;      // layer thickness below surface
    int32_t subsoilMm = 0;      // layer thickness below topsoil
    int32_t bedrockDepthMm = 0; // depth below surface where bedrock begins
    MaterialId surfaceMat = MAT_TOPSOIL; // biome surface material, voxelcore/biome.h
};

class Amplifier {
public:
    Amplifier(uint64_t seed, ITileSampler& tiles) : seed_(seed), tiles_(&tiles) {}

    uint64_t seed() const { return seed_; }

    // Full stratigraphy for the column through voxel (vx, vy).
    ColumnSample column(int64_t vx, int64_t vy) const;

    // Material of voxel (vx, vy, vz) given its precomputed column. A voxel is
    // solid iff its centre (vz*100+50 mm) is at or below the surface. Defined
    // to unbounded depth (implicit-solid underground, doctrine §3.1 step 4).
    static MaterialId materialAt(const ColumnSample& col, int64_t vz);

    MaterialId materialAt(int64_t vx, int64_t vy, int64_t vz) const {
        return materialAt(column(vx, vy), vz);
    }

private:
    uint64_t seed_;
    ITileSampler* tiles_;
};

} // namespace vxc
