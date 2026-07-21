#pragma once
// Amplifier v0 (plan §3.1 step 3, M0 scope): bit-deterministic synthesis of
// 0.1m detail from 30m-class tiles. CPU reference implementation — the GPU
// compute port must match it bit-exactly (CI-enforced once GPU runners exist).
//
// v0 = bilinear tile base + slope-scaled integer-hash fractal detail + column
// stratigraphy (topsoil/subsoil/rock/bedrock, climate-conditioned surface
// material). Later versions add erosion stamps, riverbed carving, caves,
// vegetation placement.

#include "voxelcore/caves.h"
#include "voxelcore/tiles.h"

namespace vxc {

struct ColumnSample {
    int32_t surfaceMm = 0;      // terrain surface elevation, mm above sea level
    int32_t topsoilMm = 0;      // layer thickness below surface
    int32_t subsoilMm = 0;      // layer thickness below topsoil
    int32_t bedrockDepthMm = 0; // depth below surface where bedrock begins
    MaterialId surfaceMat = MAT_TOPSOIL; // biome surface material, voxelcore/biome.h

    // M4 cave pass (voxelcore/caves.h): the tube axes passing near this column,
    // already reduced to the per-voxel test's two numbers. Carried in the
    // ColumnSample — rather than recomputed per voxel or bolted onto
    // materialAt's signature — so that every existing consumer of the column
    // cache (GeneratedWorld::makeBrick, the UE column grid cache, gpu_harness,
    // bench) picks caves up with no API change and pays the 34 cave hashes
    // once per column instead of once per voxel.
    CaveColumn cave;
};

class Amplifier {
public:
    Amplifier(uint64_t seed, ITileSampler& tiles)
        : seed_(seed), tiles_(&tiles), id_(nextId()) {}

    uint64_t seed() const { return seed_; }

    // Full stratigraphy for the column through voxel (vx, vy).
    ColumnSample column(int64_t vx, int64_t vy) const;

    // Material of voxel (vx, vy, vz) given its precomputed column. A voxel is
    // solid iff its centre (vz*100+50 mm) is at or below the surface, MINUS
    // whatever the M4 cave pass carves out of it (voxelcore/caves.h). Defined
    // to unbounded depth (implicit-solid underground, doctrine §3.1 step 4) —
    // note that underground is no longer UNIFORMLY solid: caves are the one
    // source of air below the surface shell.
    static MaterialId materialAt(const ColumnSample& col, int64_t vz);

    // Stratigraphy only, cave pass NOT applied — the pre-M4 definition, kept
    // for tests and tooling that need "what would be here if no cave crossed
    // it". Production paths want materialAt().
    static MaterialId stratigraphyAt(const ColumnSample& col, int64_t vz);

    // Per-voxel query. This is the path taken by World::materialAt ->
    // UVoxelWorldSubsystem::IsSolidAtVoxel -> the region-graph MaterialFn and
    // by collapse.h's CarveSphere, which walk voxels rather than columns and
    // so re-derive the SAME column once per voxel of its height. It goes
    // through the memo below; batch callers that already hold a ColumnGrid
    // (GeneratedWorld::columns, gpu_harness, bench) use the two-argument
    // materialAt and never pay for it.
    MaterialId materialAt(int64_t vx, int64_t vy, int64_t vz) const {
        return materialAt(columnCached(vx, vy), vz);
    }

    // column(), served from a per-thread memo. Identical value to column();
    // the reference is valid until this thread's next columnCached call.
    const ColumnSample& columnCached(int64_t vx, int64_t vy) const;

private:
    // Identity for the per-thread tile-raster memo in amplifier.cpp. Drawn from
    // a never-reused counter rather than using `this` or `tiles_`, because a
    // destroyed Amplifier's address can be recycled by the allocator and a
    // stale memo entry would then be served to a DIFFERENT world. Copies share
    // the id, which is correct: Amplifier is immutable after construction, so
    // a copy samples the same (seed, tiles) and must produce the same columns.
    static uint64_t nextId();

    uint64_t seed_;
    ITileSampler* tiles_;
    uint64_t id_;
};

} // namespace vxc
