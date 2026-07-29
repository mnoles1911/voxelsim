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

// --- opt-in memo instrumentation (VXC_MEMO_STATS) ---------------------------
//
// Counts, not clocks. The block memos in amplifier.cpp exist to turn 16
// per-column tile probes into 1, and that is a COUNT -- deterministic, and
// immune to whatever else happens to be running on the machine. Wall-clock is
// neither. Build with -DVXC_MEMO_STATS=ON to get these; the shipping path
// compiles them out entirely.
#ifdef VXC_MEMO_STATS
struct MemoStats {
    uint64_t elevProbes = 0;    // cachedElevationMm calls
    uint64_t elevMisses = 0;    // ... that reached the sampler
    uint64_t stencilProbes = 0; // cachedStencil calls (one per column)
    uint64_t stencilMisses = 0; // ... that had to gather 16 control points
};
// Per-thread by construction; read it from the thread that did the work.
MemoStats& memoStats();
void resetMemoStats();
#endif

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

// Tile CONTROL POINTS the bound will read per axis before declining.
//
// THE ARITHMETIC, spelled out so the next pixel-size change re-derives it
// instead of trusting it. Both previous values were sized against a pixel that
// then moved underneath them, which is how this constant became a cliff twice.
//
// The largest footprint the streaming layer ever asks about is the ALL-SOLID
// trim's level-5 chunk WITH the mesher apron (VoxelWorldSubsystem's
// FootprintSolidFloorMmCached): 32 * 2^5 = 1024 level-0 voxels of chunk plus
// one level-scale voxel-run either side = 1088 voxels = 108.8 m, so the
// INCLUSIVE span (x1Mm - x0Mm) is 108700 mm. The sky-band trim asks about the
// same chunk without the apron, which is strictly smaller.
//
// A footprint at an arbitrary phase against a pxMm grid touches at most
// floor(span/pxMm) + 2 cells per axis, and the cubic B-spline carrier dilates
// that by one cell low and two high (a spline on cell c reads control points
// c-1..c+2), so the grid this function must materialise is
//
//     nx <= floor(108700 / pxMm) + 2 + 3
//
//     30 m/px    (scale 1)  ->  nx <=  8
//     3.75 m/px  (scale 8)  ->  nx <= 33
//     1.875 m/px (scale 16) ->  nx <= 62   <-- the .vxtl v2 fine tier
//
// 64 covers all three, and it is NOT generous at 1.875 m: two control points
// of headroom, i.e. the cap admits a 112.5 m footprint where the cascade asks
// for 108.8 m. A level-6 footprint (204.8 m + apron) would need ~114 and
// declines -- which it does at every pixel size, including 30 m, and is
// unchanged by this constant.
//
// WHY THE CAP IS SIZED RATHER THAN JUST REMOVED. Declining is SAFE: the
// sentinels mean "no information", so a decline skips no chunk and can never
// open a hole in the world. It is a PERFORMANCE cliff, not a correctness one --
// the sky-band and all-solid trims quietly stop paying off above the level
// where the cap first bites. Sizing the cap to the cascade keeps the trims
// alive at every level the streamer actually uses, while still refusing the
// unbounded read that an arbitrarily large footprint would ask for.
//
// Note also that a bigger cap costs nothing when it is not needed: the read
// loop and the first-difference scans in surfaceBoundsMm are bounded by nx/ny,
// which come from the footprint, never by this constant. At 30 m the work is
// identical at 16, 34 or 64 -- only the address space reserved for the grid
// changes. Raising it cannot move worldgen output either: the only thing it
// governs is WHEN the bound declines, and the bound is a derived query that no
// generation path reads.
//
// HISTORY:
//  * v8 used 16, sized for a 30 m pixel and a BILINEAR carrier (4 corners for
//    a level-4 chunk). Its own comment warned that scale 8 would decline.
//  * v9 raised it to 34 for the cubic carrier's three extra points per axis,
//    sized for the 3.75 m pixel then planned for scale 8 -- nx <= 33, one
//    point of headroom.
//  * 64 is the .vxtl v2 fine tier (docs/vxtl-v2-format.md 1) moving that pixel
//    from 3.75 m to 1.875 m. At 34 a 1.875 m level-5 footprint needs 62 and
//    would decline -- exactly the cliff v9 had just removed at 3.75 m.
//
// THE STACK. surfaceBoundsMm materialises the control grid in one on-stack
// int64 array: 64*64*8 = 32 KB, up from 34*34*8 = 9.2 KB (2 KB at v8). That is
// acceptable in the two contexts that call it -- the game thread's desired-set
// pass and the streaming worker pool -- for three reasons that are properties
// of this code rather than assumptions about the host:
//
//   * it is ONE fixed frame, not a per-level or per-recursion cost.
//     surfaceBoundsMm is a leaf as far as stack goes: it calls only
//     cachedElevationMm and evalCarrier, neither of which recurses or holds a
//     comparable frame, and the amplifier's own per-thread state (the memo
//     tables) is thread_local, not stack. 32 KB is the peak of this chain, not
//     a term in a sum.
//   * 32 KB is ~3% of the 1 MB default thread stack reserve on Win64, and the
//     array is not initialised -- only the nx*ny prefix is ever written or
//     read -- so the larger cap costs reserved stack, not touched pages.
//   * MSVC emits a __chkstk probe for any frame over one 4 KB page, so the
//     guard page is walked in order rather than skipped. That was already true
//     at 9.2 KB; 32 KB does not change the mechanism.
//
// If a future tier ever pushes this past ~128 (128*128*8 = 128 KB) the grid
// should move to a static thread_local scratch buffer instead of growing
// further on the stack. At 64 it does not need to.
inline constexpr int64_t kSurfaceBoundMaxCornersPerAxis = 64;

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
