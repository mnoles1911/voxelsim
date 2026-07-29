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

// --- the carrier's control stencil (v9) -------------------------------------
//
// A column in tile-pixel cell px reads control points
// px + kCarrierStencilLo .. px + kCarrierStencilHi on each axis. v8's bilinear
// base read px..px+1; the cubic B-spline reads one further out on the low side
// and two on the high side.
//
// THIS IS A CONTRACT, NOT AN IMPLEMENTATION DETAIL, and it is stated here
// because two independent hosts have to honour it. Both the UE RDG pass
// (VoxelGpuRegionBuild.h) and the headless harness (bench/gpu_harness.cpp) copy
// a WINDOW of the tile raster to the GPU and the shader CLAMPS reads to that
// window. A window that is not dilated to match does not fault and does not
// error -- it silently returns an edge value where the CPU reads the real tile,
// i.e. different terrain on the two paths. That exact failure has already
// happened once here (see the D5 note in VoxelGpuRegionBuild.h), so the numbers
// live in one place and both hosts derive their margins from them.
inline constexpr int64_t kCarrierStencilLo = -1;
inline constexpr int64_t kCarrierStencilHi = 2;

// --- surface upper bound (the sky-band trim's proof obligation) -------------
//
// Amplifier::surfaceUpperBoundMm returns this when it declines to bound. It is
// deliberately INT64_MAX rather than a bool-out-param so a caller that forgets
// to check still gets the SAFE answer ("the terrain might reach arbitrarily
// high here"), never a false all-air verdict.
inline constexpr int64_t kSurfaceBoundDeclined = INT64_MAX;

// The mirror sentinel, for the LOWER bound and the all-solid bound below.
// INT64_MIN for exactly the same reason kSurfaceBoundDeclined is INT64_MAX: it
// is the safe answer. A caller that forgets to check gets "the terrain might
// reach arbitrarily low here" / "nothing is provably solid here", never a false
// all-solid verdict. The two sentinels are deliberately different values so a
// caller cannot pass one where the other is meant and still compile into
// something that looks like it works.
inline constexpr int64_t kSurfaceLowerBoundDeclined = INT64_MIN;

// Tile-pixel corners the bound will read per axis before declining. A level-4
// chunk (51.2 m) needs 4 corners against a 30 m pixel and ~15 against the
// 3.75 m scale-8 pixel; 16 leaves generous headroom at scale 1 while keeping
// the read count and the on-stack corner grid bounded (16x16 int64 = 2 KB).
//
// Re-checked when the ring cascade grew to level 5 (2 km edge): a level-5
// footprint is 102.4 m plus a 6.4 m apron = 108.8 m, needing ~6 corners at
// 30 m -- still inside the cap, so at SCALE 1 (the only scale any tile has
// ever been generated at) the bound is COMPUTED, not declined, at every level
// the cascade now uses. The bound does get looser with a bigger footprint,
// because a larger rectangle spans more pixel cells and so admits a wider base
// range and a higher max slope. That is the SAFE direction for both consumers:
// surfaceUpperBoundMm rising and solidBelowBoundMm falling each mean FEWER
// chunks are provably skippable, never more, so a looser bound costs
// optimization and cannot open a hole in the world.
//
// SCALE 8 IS DIFFERENT, and this comment previously got it wrong by 3x because
// it used the superseded 11.25 m pixel (90 m / 8) instead of the real 3.75 m
// (30 m / 8) -- see tilestore.h's tilePixelSizeMm. At 3.75 m a level-5
// footprint needs ~30 corners and a level-4 one ~15, so the bound would
// DECLINE from level 5 up rather than at level 6. Declining is safe (it skips
// nothing), but it means the sky-band and all-solid trims would quietly stop
// paying off above level 4 on scale-8 tiles. Raise this cap, and re-check the
// 2 KB stack grid, before generating scale-8 tiles or extending the cascade
// past level 5.
// v9 RAISED THIS FROM 16 TO 34, and the reason is the carrier's stencil. A
// cubic B-spline on cell c reads control points c-1..c+2, so the grid the bound
// must see is the cells the footprint touches DILATED by one on the low side
// and two on the high side: three more control points per axis than v8's
// bilinear needed. A level-5 footprint (108.8 m incl. apron) spans ~4 cells at
// 30 m -> 7 control points, and ~29 cells at the 3.75 m scale-8 pixel -> 32.
// 34 covers both with headroom; the on-stack grid is 34x34 int64 = 9.2 KB, up
// from 2 KB.
//
// This also discharges the v8 warning that used to live here: at 3.75 m the old
// cap of 16 would have made the bound DECLINE from level 4 up, so the sky-band
// and all-solid trims would have quietly stopped paying off on scale-8 tiles.
// Declining is safe -- it skips nothing -- but it is a silent performance
// cliff, and the baked fine tier (docs/terrain-amplification-plan.md Phase 2)
// is about to make scale-8 the normal case.
inline constexpr int64_t kSurfaceBoundMaxCornersPerAxis = 34;

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

    // A PROVABLE LOWER BOUND on surfaceMm(vx, vy) over the same inclusive
    // rectangle — the exact mirror of surfaceUpperBoundMm:
    //
    //     surfaceLowerBoundMm(...) <= surfaceMm(vx, vy)   for all such columns
    //
    // and returns kSurfaceLowerBoundDeclined if it will not bound this
    // footprint. Same cost, same corner budget, same decline conditions; it
    // shares surfaceUpperBoundMm's implementation body, so the two cannot drift.
    //
    // On its own this proves nothing about solidity — knowing the surface is at
    // least X does not make everything below X solid, because caves and caverns
    // carve. It is the input to solidBelowBoundMm, which adds the carve
    // envelope. Exposed separately because it is independently meaningful and
    // independently testable.
    int64_t surfaceLowerBoundMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const;

    // A PROVABLE ALL-SOLID FLOOR for the inclusive voxel-index rectangle: every
    // voxel in that footprint whose centre is strictly below the returned mm
    // elevation is guaranteed non-air. Returns kSurfaceLowerBoundDeclined when
    // it will not bound the footprint.
    //
    // THIS IS THE MIRROR OF THE SKY-BAND TRIM AND IT IS STRICTLY HARDER.
    // Proving a chunk all-AIR needs only the surface, because nothing in the
    // amplifier adds solid above it. Proving a chunk all-SOLID needs the
    // surface AND the full depth envelope of everything that carves, because
    // caves, crevices, sinkhole shafts and caverns all remove solid from below
    // it. Get the envelope wrong and the result is not a cosmetic pop-in: it is
    // a cave the player can see into but never reach, or a chunk that was never
    // tracked at all sitting where someone is about to dig.
    //
    // THE DERIVATION (the static_asserts that pin every constant it uses live
    // in amplifier.cpp, in the same coupling block surfaceUpperBoundMm uses):
    //
    //   materialAt returns air below the surface for exactly three reasons, and
    //   this enumeration is closed — there is no MAT_WATER, and every non-air
    //   material is solid:
    //
    //   1. Nothing: below every column's own surface, stratigraphyAt is solid
    //      at every depth (MAT_ROCK, then the unbounded MAT_BEDROCK floor).
    //      Handled by taking a LOWER bound on the surface over the footprint.
    //
    //   2. caveCarveAt — tunnels, crevices and sinkhole shafts. Every one of
    //      these is bounded in the QUERYING COLUMN'S OWN depth space by
    //      compile-time constants (deepest tube axis + widest radius, plus the
    //      crevice's downward reach), so they cannot reach below
    //      surfaceLowerBound - kMaxCaveCarveBelowSurfaceMm.
    //
    //   3. cavernCarveAt — the hard one, and the reason this bound needs a
    //      dilated footprint. A cavern chain is anchored at ABSOLUTE z, derived
    //      from the surface at the SITE'S OWN anchor xy, which is a different
    //      column from the one being queried. Its depth below THAT surface is
    //      constant-bounded (caveNode's own depth ceiling, three chain steps,
    //      and the flat-floor clamp), but the site's surface can be far above
    //      the querying column's. Since a site only reaches columns within
    //      kCavernMaxReachMm of its anchor, dilating the footprint by that
    //      radius and taking the surface lower bound over the DILATED rectangle
    //      bounds every site that can possibly carve into it.
    //
    // So the bound is: lower-bound the surface over the footprint dilated by
    // the cavern reach, then subtract the deepest carve envelope. Conservative
    // twice over — the dilated lower bound is never above the tight one, and
    // the envelope is the worst case over every hash draw rather than the draw
    // actually taken.
    //
    // Cost: one surfaceLowerBoundMm call over the dilated rectangle. No
    // hashing, no cave or cavern lattice evaluation, no column() — that is the
    // entire point, since this runs per candidate chunk on the streaming
    // admission path where a single column() would already be too expensive.
    int64_t solidBelowBoundMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const;

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
        // v9: MM PER METRE, from the carrier's analytic gradient. Was mm per
        // tile PIXEL from a per-cell forward difference, which both stepped on
        // the pixel grid and meant a different grade at every tile scale.
        int64_t slopeMmPerM = 0;
        int64_t px = 0, py = 0; // tile pixel the column falls in
    };
    SurfaceEval evalSurface(int64_t vx, int64_t vy) const;

    // Shared body of surfaceUpperBoundMm / surfaceLowerBoundMm: one traversal
    // of the footprint's tile-pixel corners producing BOTH bounds, so the two
    // cannot drift apart in which cells they visit or what maximum slope they
    // feed slopeScaleQ10. Returns false when it declines to bound.
    bool surfaceBoundsMm(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1, int64_t& outLowerMm,
                         int64_t& outUpperMm) const;

    // True if any cavern site reachable by any column of the rect has its
    // anchor within kCavernMaxReachMm of the rect. Conservative in the safe
    // direction: returns true when unsure. Selects which carve envelope
    // solidBelowBoundMm subtracts; see its definition in amplifier.cpp.
    bool cavernMayReachFootprint(int64_t vx0, int64_t vy0, int64_t vx1, int64_t vy1) const;

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
