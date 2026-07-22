#pragma once
// Amplifier v0 (plan §3.1 step 3, M0 scope): bit-deterministic synthesis of
// 0.1m detail from 30m-class tiles. CPU reference implementation — the GPU
// compute port must match it bit-exactly (CI-enforced once GPU runners exist).
//
// v0 = bilinear tile base + slope-scaled integer-hash fractal detail + column
// stratigraphy (topsoil/subsoil/rock/bedrock, climate-conditioned surface
// material). Later versions add erosion stamps, riverbed carving, caves,
// vegetation placement.

#include "voxelcore/caverns.h"
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

    // M4 cave pass v2 (voxelcore/caverns.h): the cavern rooms whose ellipsoids
    // reach this column, reduced the same way `cave` is and carried here for
    // the same reason — every consumer of the column cache picks caverns up
    // with no API change. Unlike `cave`, the cavern reduction needs a terrain
    // surface at the SITE's own xy (caverns anchor at absolute z so their
    // floors and water tables are level, not draped); Amplifier::column
    // supplies that as a callback over its own surface function, and the GPU
    // mirror recomputes it inside VoxelizeMain rather than widening
    // GpuColumnSample (docs/cavern-design.md §3.5).
    CavernColumn cavern;
};

// --- surface upper bound (the sky-band trim's proof obligation) -------------
//
// Amplifier::surfaceUpperBoundMm returns this when it declines to bound. It is
// deliberately INT64_MAX rather than a bool-out-param so a caller that forgets
// to check still gets the SAFE answer ("the terrain might reach arbitrarily
// high here"), never a false all-air verdict.
inline constexpr int64_t kSurfaceBoundDeclined = INT64_MAX;

// Tile-pixel corners the bound will read per axis before declining. A level-4
// chunk (51.2 m) needs 4 corners against a 30 m pixel and 6 against the 11.25 m
// scale-8 pixel; 16 leaves generous headroom while keeping the read count and
// the on-stack corner grid bounded (16x16 int64 = 2 KB).
inline constexpr int64_t kSurfaceBoundMaxCornersPerAxis = 16;

class Amplifier {
public:
    Amplifier(uint64_t seed, ITileSampler& tiles)
        : seed_(seed), tiles_(&tiles), id_(nextId()) {}

    uint64_t seed() const { return seed_; }

    // Full stratigraphy for the column through voxel (vx, vy).
    ColumnSample column(int64_t vx, int64_t vy) const;

    // The terrain surface elevation at (vx, vy) on its own — bit-identical to
    // column(vx, vy).surfaceMm (it is literally the same evalSurface call), but
    // without the climate read, stratigraphy, biome classification, cave pass
    // and cavern pass that column() also does. For callers that want only the
    // height: the surface-bound tests, and anything bounding or probing terrain
    // height without needing materials.
    int32_t surfaceMm(int64_t vx, int64_t vy) const;

    // A PROVABLE UPPER BOUND on surfaceMm(vx, vy) over every column in the
    // inclusive voxel-index rectangle [vx0, vx1] x [vy0, vy1] — i.e.
    //
    //     surfaceUpperBoundMm(...) >= surfaceMm(vx, vy)   for all such columns
    //
    // and returns kSurfaceBoundDeclined if it will not bound this footprint.
    //
    // WHY IT IS THE ONLY QUERY A SKY-BAND TRIM NEEDS. materialAt is
    // unconditionally MAT_AIR above surfaceMm (stratigraphyAt's `depthMm < 0`
    // test), and the cave and cavern passes only ever CARVE — no pass in the
    // amplifier can turn air into solid. So a chunk whose lowest voxel centre
    // sits above this bound is provably all air, and skipping it can never hide
    // geometry.
    //
    // This is a BOUND, not the maximum: it is sound but conservative, and the
    // conservatism is deliberate. It lives here, next to kDetailOctaves and
    // slopeScaleQ10 in amplifier.cpp, precisely so that a worldgen tweak and the
    // bound that depends on it cannot drift apart across a module boundary —
    // which is exactly what happened while this logic lived UE-side. See
    // amplifier.cpp for the derivation and the static_asserts that pin the
    // couplings the derivation relies on.
    //
    // Cost: at most one 16x16 block of ITileSampler::elevationMm reads (served
    // from the same per-thread tile memo column() uses) and no hashing at all —
    // it never evaluates a single detail octave. Cheaper than ONE column.
    int64_t surfaceUpperBoundMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const;

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
    // The surface half of column() — the bilinear tile base plus the
    // slope-scaled detail octaves — plus the tile pixel and tile slope the
    // rest of column() derives from the same reads. Exposed as its own step
    // only so the cavern pass's `surfaceAt` callback (caverns.h, which needs a
    // surface at the SITE's xy rather than the querying column's) can be
    // literally this function instead of a second copy of it.
    struct SurfaceEval {
        int32_t surfaceMm = 0;
        int64_t slopeMmPerPx = 0;
        int64_t px = 0, py = 0; // tile pixel the column falls in
    };
    SurfaceEval evalSurface(int64_t vx, int64_t vy) const;

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
