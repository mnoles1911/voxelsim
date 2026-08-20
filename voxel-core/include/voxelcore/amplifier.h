#pragma once
// Amplifier v0 (plan §3.1 step 3, M0 scope): bit-deterministic synthesis of
// 0.1m detail from 30m-class tiles. CPU reference implementation — the GPU
// compute port must match it bit-exactly (CI-enforced once GPU runners exist).
//
// v0 = bilinear tile base + slope-scaled integer-hash fractal detail + column
// stratigraphy (topsoil/subsoil/rock/bedrock, climate-conditioned surface
// material). Later versions add erosion stamps, riverbed carving, caves,
// vegetation placement.

#include <atomic> // the debug water marker's column counters

#include "voxelcore/biome.h" // BiomeId, carried on ColumnSample since the asset policy
#include "voxelcore/caverns.h"
#include "voxelcore/karst.h"
#include "voxelcore/caves.h"

#include "voxelcore/hash_channel_registry.h" // compile-time HashChannel id uniqueness guard
#include "voxelcore/tiles.h"

namespace vxc {

// Forward declaration ONLY. lakes.h pulls in tilestore.h, and amplifier.h is
// included almost everywhere; the marker stores a pointer and calls through it
// exclusively from the .cpp, so the heavy header stays out of this one.
class IWaterSampler;

//: The marker's own copy of lakes.h/tilestore.h's `kNoWaterMm`. Mirrored rather
//: than included for the reason above, and static_asserted equal in
//: amplifier.cpp so the two cannot drift.
inline constexpr int32_t kNoWaterMarkerMm = INT32_MIN;

//: Deepest water the marker will bound for. The shipped water plane is int16 at
//: a 10 mm LSB (tile_codec.WATER_DEPTH_LSB_MM), so no expressible depth exceeds
//: this. Used by surfaceUpperBoundMm; see the note there for why the bound is a
//: constant rather than a query.
inline constexpr int64_t kWaterMarkerMaxDepthMm = 32767 * 10;

//: HOW THICK THE MARKER IS, measured up from the ground -- NOT up to the water
//: surface.
//:
//: The marker used to fill every voxel between the ground and the baked water
//: surface, which is solid magenta all the way down a lake. That is 40 m of
//: voxels to show a surface nobody can see inside, and it cost the thing it was
//: meant to help: a 40 m column is FIFTY 8-voxel bricks where dry ground needs
//: one, so the deepest water -- the INTERIOR of every lake and river -- became
//: fifty times more expensive to generate than its own shoreline. Flown, that
//: read exactly as reported: "many voxel chunks unload when it gets close, then
//: the chunks reload except for the ones in the interior of the river and lake
//: water bodies", with the interiors never returning.
//:
//: A placement instrument does not need volume. It needs to say WHERE water is,
//: and 3 m of magenta hugging the ground says that from any angle a player will
//: ever see it from, at the cost of ~4 bricks instead of ~50.
//:
//: WHAT IT GIVES UP, stated: the marker no longer shows how DEEP the water is,
//: and its top is no longer the true water surface on anything deeper than this.
//: Depth is a number the probes report far better than an eye can judge from a
//: magenta block, and the owner's question was always placement.
inline constexpr int64_t kWaterMarkerHeightMm = 3000;

struct ColumnSample {
    int32_t surfaceMm = 0;      // terrain surface elevation, mm above sea level
    int32_t topsoilMm = 0;      // layer thickness below surface
    int32_t subsoilMm = 0;      // layer thickness below topsoil
    int32_t bedrockDepthMm = 0; // depth below surface where bedrock begins
    MaterialId surfaceMat = MAT_TOPSOIL; // biome surface material, voxelcore/biome.h

    // WHAT classifyBiome ACTUALLY SAID, and the slope it said it about.
    //
    // Both of these were computed inside column() and thrown away. surfaceMat
    // is a lossy reduction of the first and is the only thing that used to
    // survive: MAT_SAND is BOTH beach and desert, MAT_ROCK is BOTH bare rock
    // and alpine above the rock line (biomeSurfaceMaterial, biome.h:239), so
    // a consumer that needs the biome cannot recover it from the material.
    // slopeMmPerM was not exposed at all -- evalSurface is private.
    //
    // Asset placement is the consumer that needs both: a species carries a
    // weight per biome (all 828 asset-forge specs do) and a slope band, and
    // "steep, but not a cliff" is what puts scree below a cliff rather than
    // on top of it.
    //
    // NEITHER FIELD CAN CHANGE A VOXEL. materialAt and stratigraphyAt are
    // static functions of (ColumnSample, vz) and read neither, so the solid
    // set is untouched; this is carried on the sample for exactly the reason
    // `cave` and `cavern` below are, and it costs two stores in a function
    // that already computed both values. Verified rather than argued: the
    // worldgen digest (vxc_bench --radius 8 --digest) is unchanged.
    //
    // The GPU mirror (shaders/worldgen.ush ColumnMain) does NOT need these:
    // it mirrors materialAt's inputs, and these are not among them. Adding a
    // field materialAt reads WOULD carry that obligation.
    BiomeId biome = TEMPERATE_FOREST;
    int64_t slopeMmPerM = 0; // mm of rise per metre of run, same currency as
                             // kBiomeCliffSlopeMmPerM (biome.h:71)

    // DEBUG WATER MARKER (off by default). Absolute mm of the baked water
    // surface over this column, or kNoWaterMm. Populated only when the
    // Amplifier has been given a water sampler; `kNoWaterMm` therefore means
    // BOTH "marker mode is off" and "no water here", which is deliberate --
    // stratigraphyAt is a static function of (ColumnSample, vz), so the mode
    // has to ride on the sample rather than widen the signature, exactly as
    // `cave` and `cavern` do.
    int32_t waterSurfaceMm = kNoWaterMarkerMm;

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

    // KARST CONDUITS (voxelcore/karst.h), carried for the same reason `cave`
    // and `cavern` are: materialAt is a static function of (ColumnSample, vz),
    // so anything the per-voxel test needs and cannot derive from vz has to
    // arrive on the sample.
    //
    // IT IS EMPTY UNTIL A TABLE IS INSTALLED, AND THAT IS DELIBERATE. This
    // member and its carve land as a PROVEN NO-OP: with no table the reduction
    // returns count == 0, karstCarveAt's first compare rejects, and the world
    // is byte-identical -- verified by vxc_bench --digest before and after. The
    // repo has done this before, deliberately (core.h's v28 note: "THE FORMAT
    // LANDED AS A PROVEN NO-OP FIRST"), because it separates "the plumbing is
    // correct" from "the new geometry is good", and those fail differently.
    //
    // The table itself will be baked (docs/karst-phase1-carve.md). Until the
    // HLSL mirror in voxel-core/shaders/karst.ush is wired into worldgen.ush,
    // installing a real table would make the CPU and GPU disagree -- which
    // ADR-0006 makes a desync vector -- so the source stays null on purpose.
    KarstColumn karst;

    // A THIRD MEMBER, Density3Column d3, lived here from v12 to v19 for the
    // same reason `cave` and `cavern` do: stratigraphyAt is a static function of
    // (ColumnSample, vz), so anything the per-voxel test needs and cannot derive
    // from vz has to arrive on the sample. v20 removed the term (core.h) and the
    // member with it, so a column is once more exactly the layer model plus the
    // two carve passes.
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
// kSurfaceBoundDeclined and kSurfaceLowerBoundDeclined MOVED TO core.h, values
// and meanings unchanged. They are still the sentinels
// Amplifier::surfaceUpperBoundMm / surfaceLowerBoundMm / solidBelowBoundMm
// return, and every existing `vxc::kSurfaceBoundDeclined` reads the same
// constant it always did -- core.h is already included here, so nothing about
// using them from this header changed.
//
// They moved because they stopped being the amplifier's private vocabulary. A
// bound that COMPOSES with the surface bound has to speak the same decline
// convention (voxelcore/assetplacement.h is the first: an asset height added to
// INT64_MAX wraps negative, which reads as "provably air" for the whole sky),
// and making that module include this one -- caves, caverns, biome, climate and
// all -- to learn one integer would be the wrong dependency edge entirely.

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
//  * 72 is v13's PREFILTER. On a tier that ships raster samples the bound has to
//    materialise the same control lattice evalSurface does, which means reading
//    a halo of kCarrierPrefilterRadius raw samples around the control grid --
//    nx + 8 rather than nx. Keeping the CONTROL budget where it was (the 1.7 km
//    footprint that must be accepted and the 2.0 km one that must decline are
//    both pinned in test_amplifier.cpp) therefore needs the array to hold
//    nx + 8 = 68. 72 is that, rounded. The 1.875 m fine tier is unaffected: it
//    ships control points, so it takes no halo and still needs 62.
//    The haloed raw grid is a thread_local scratch, not a second stack array --
//    see surfaceBoundsMm -- so only `elev` grows, from 32 KB to 41.5 KB.
inline constexpr int64_t kSurfaceBoundMaxCornersPerAxis = 72;

class Amplifier {
public:
    Amplifier(uint64_t seed, ITileSampler& tiles)
        : seed_(seed), tiles_(&tiles), id_(nextId()) {}

    uint64_t seed() const { return seed_; }

    // --- DEBUG WATER MARKER -------------------------------------------------
    //
    // Give the amplifier a water sampler and every column between its ground
    // and its water surface voxelises as MAT_WATERMARK. Pass nullptr (the
    // default) and nothing changes -- `waterSurfaceMm` stays kNoWaterMm and
    // stratigraphyAt takes its historical path.
    //
    // AN IWaterSampler RATHER THAN A NEW ITileSampler METHOD, because
    // CompositeWaterSampler already composes the river plane, the lake table
    // and the sea datum, and re-deriving any of that here would be a fourth
    // copy of a shipped fact. `lakes.h:implicitWaterDatumMm` is the same
    // composition the near-field sweep uses.
    //
    // BORROWED, NOT OWNED, and NOT thread-safe to install: set it once during
    // bring-up, before any worker touches the amplifier. The sampler itself
    // mutates on query (it decodes blocks lazily), which is why the member is
    // mutable and why `column()` can stay const.
    // `includeOcean` composes the sea datum in, the same way lakes.h's
    // `implicitWaterDatumMm` does for the near-field sweep -- the sampler alone
    // carries only the baked lakes and rivers, because the ocean is not on the
    // wire (it is `ground < kSeaLevelMm`, decided at the call site). Pass false
    // to mark inland water only: near a coast a marked ocean fills the frame
    // and a river shot is unreadable.
    void setWaterMarker(IWaterSampler* sampler, bool includeOcean = true) {
        waterMarker_ = sampler;
        waterMarkerOcean_ = includeOcean;
    }
    // How far the marker searches sideways for a water level, in FINE PIXELS.
    // ZERO IS THE DEFAULT AND IT IS NOT A TASTE DECISION -- see
    // waterMarkerFillPx_ for the measurement. Non-zero costs 8*n LOCKED water
    // sampler queries on EVERY column in the world, wet or dry, and dry is
    // 99.4% of them.
    void setWaterMarkerFillPx(int64_t px) { waterMarkerFillPx_ = px < 0 ? 0 : px; }

    // Install the region's baked conduit table. The Amplifier does NOT own it:
    // the caller (the tile streamer, or a test) owns the arrays and must
    // outlive this. Passing an empty table restores the no-op default.
    //
    // NOT YET CALLED BY ANYTHING SHIPPING. Installing a real table makes the
    // CPU carve conduits the GPU mirror does not, and ADR-0006 makes that
    // divergence a desync vector -- so this stays unwired until
    // voxel-core/shaders/karst.ush is included by worldgen.ush and vxc_gpu has
    // shown the two agree.
    void setKarstTable(const KarstTable& t) { karstTable_ = t; }
    const KarstTable& karstTable() const { return karstTable_; }
    IWaterSampler* waterMarker() const { return waterMarker_; }
    bool waterMarkerEnabled() const { return waterMarker_ != nullptr; }

    // HOW MANY COLUMNS THE MARKER ACTUALLY MARKED, because "I see no magenta"
    // is otherwise unfalsifiable.
    //
    // 2026-08-06 cost the session twice over: a capture whose marker had been
    // silently disabled by the GPU mesh fork, and then a capture where the
    // marker was genuinely installed, genuinely queried, and still put nothing
    // on screen. Neither the log nor the image could tell those apart from
    // "the bake has no water here" -- which is the ONE question the instrument
    // exists to answer, so the instrument was unable to fail honestly.
    //
    // `queried` counts columns that reached the marker at all; `marked` counts
    // those that came back with water over them. marked == 0 against a large
    // queried is the signature of a wiring fault; both small is a camera that
    // never looked at water; marked large with nothing on screen is a DRAWING
    // problem (thin band, coarse LOD sampling) and not a data one.
    //
    // Relaxed atomics: they are incremented from the mesher pool, nothing
    // branches on them, and they are read once for a log line.
    int64_t waterMarkerColumnsQueried() const {
        return markerQueried_.load(std::memory_order_relaxed);
    }
    int64_t waterMarkerColumnsMarked() const {
        return markerMarked_.load(std::memory_order_relaxed);
    }
    // Of the marked columns, how many carry water ABOVE their own amplified
    // surface -- i.e. how many can actually emit a magenta voxel. See the
    // increment site: stratigraphyAt returns MAT_WATERMARK only above the
    // surface, so `marked - aboveGround` is water the marker knows about and
    // structurally cannot draw.
    int64_t waterMarkerColumnsAboveGround() const {
        return markerAboveGround_.load(std::memory_order_relaxed);
    }

    // Full stratigraphy for the column through voxel (vx, vy).
    ColumnSample column(int64_t vx, int64_t vy) const;

    // WHAT THE GROUND IS LIKE HERE, without what is underneath it.
    //
    // Surface, slope magnitude AND signed per-axis gradient, biome and climate
    // in one call: one evalSurface plus the climate read and the biome gates.
    // What it does NOT pay for is the cave reduction (34 hashes) and the
    // cavern reduction that column() also does -- which a caller asking "is
    // this deer country" has no use for. That caller is the detail-entity
    // spawner (classes 3-4 of docs/asset-placement-architecture.md §1), which
    // has a position and nothing else; the chunk-generation path should keep
    // using the ColumnSample it already holds, where `biome` and `slopeMmPerM`
    // ride for free.
    //
    // THE GRADIENT DIRECTION is the carrier's analytic gradient at the
    // (warped) sample position -- the same quantity whose L1 magnitude is
    // slopeMmPerM, which until now was the only part that survived
    // evalSurface. Sign convention: positive slopeXMmPerM means the ground
    // RISES toward +x, so uphill is (+slopeX, +slopeY) and downhill is its
    // negation. This is what directional placement needs: scree lies where a
    // probe ~15 m UPHILL reads past the BARE_ROCK gate, and "uphill" was not
    // answerable from a magnitude.
    //
    // ROUNDING: slopeXMmPerM and slopeYMmPerM are each scaled to mm-per-metre
    // separately, so |slopeX| + |slopeY| can differ from slopeMmPerM by the
    // truncation of one division (carrierSlopeMmPerM divides the SUM once).
    // slopeMmPerM here is BIT-IDENTICAL to ColumnSample::slopeMmPerM; the
    // per-axis fields are new information, not a recomposition of it.
    //
    // `biome` and `climate` are BIT-IDENTICAL to what column() computes for
    // the same (vx, vy) -- same blended channels, same ecotone dither -- via
    // one shared private helper, so a spawner and the world it spawns into
    // cannot disagree about where the taiga is.
    struct SurfaceInfo {
        int32_t surfaceMm = 0;
        int64_t slopeMmPerM = 0;   // L1 magnitude, ColumnSample::slopeMmPerM
        int64_t slopeXMmPerM = 0;  // signed: + rises toward +x
        int64_t slopeYMmPerM = 0;  // signed: + rises toward +y
        BiomeId biome = TEMPERATE_FOREST;
        ClimateSample climate;     // blended channels, UNDITHERED (the dither
                                   // is a per-column boundary treatment and is
                                   // already inside `biome`)
    };
    SurfaceInfo surfaceInfo(int64_t vx, int64_t vy) const;

    // The terrain surface elevation at (vx, vy) on its own — bit-identical to
    // column(vx, vy).surfaceMm (it is literally the same evalSurface call), but
    // without the climate read, stratigraphy, biome classification, cave pass
    // and cavern pass that column() also does. For callers that want only the
    // height: the surface-bound tests, and anything bounding or probing terrain
    // height without needing materials.
    int32_t surfaceMm(int64_t vx, int64_t vy) const;

    // A PROVABLE UPPER BOUND on the DISPLACED surface over every column in the
    // inclusive voxel-index rectangle [vx0, vx1] x [vy0, vy1] — i.e.
    //
    //     surfaceUpperBoundMm(...) >= surfaceMm(vx, vy) + D(vx, vy, z)
    //
    // for all such columns and all z, and returns kSurfaceBoundDeclined if it
    // will not bound this footprint.
    //
    // v12 widened this by kDensity3MaxAbsMm and redefined it onto the DISPLACED
    // surface; v20 removed the 3D density band, so it is a bound on surfaceMm
    // again and the 350 mm came back off (core.h's v20 entry). The reason the
    // v12 note is kept rather than deleted is its argument, which outlives the
    // term: callers use this as "everything above here is air", so if a
    // displacement is ever reintroduced, this bound must move onto the displaced
    // surface WITH it. A bound whose contract silently stopped matching its only
    // use is a hole in the world one refactor away.
    //
    // WHY IT IS THE ONLY QUERY A SKY-BAND TRIM NEEDS. materialAt is
    // unconditionally MAT_AIR above the surface (stratigraphyAt's
    // `depthMm < 0` test), and the cave and
    // cavern passes only ever CARVE — no pass in the amplifier can turn air into
    // solid. So a chunk whose lowest voxel centre sits above this bound is
    // provably all air, and skipping it can never hide geometry.
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

    // A PROVABLE LOWER BOUND on the surface over the same inclusive rectangle —
    // the exact mirror of surfaceUpperBoundMm, and it lost the same
    // kDensity3MaxAbsMm at v20 for the same reason:
    //
    //     surfaceLowerBoundMm(...) <= surfaceMm(vx, vy)
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
    //   materialAt returns air below the surface for exactly four reasons, and
    //   this enumeration is closed — there is no MAT_WATER, and every non-air
    //   material is solid:
    //
    //   1. Nothing: below every column's own surface, stratigraphyAt is solid at
    //      every depth (MAT_ROCK, then the unbounded MAT_BEDROCK floor). Handled
    //      by taking a LOWER bound on the surface over the footprint.
    //
    //      A reason 1b stood here from v12 to v19 — the bounded 3D density band,
    //      which let a voxel up to 350 mm BELOW a column's own surfaceMm be air.
    //      It never needed a term of its own, because surfaceLowerBoundMm was
    //      widened by the same constant and so bounded the DISPLACED surface,
    //      which is the surface reason 1 is stated against. v20 removed the term
    //      and the widening together, which is why this enumeration went back to
    //      three reasons without any other line changing. Anything reintroducing
    //      a displacement must restore BOTH halves or reason 1 stops being true.
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
    // solid iff its centre (vz*100+50 mm) is at or below surfaceMm — v12 to v19
    // added a 3D displacement here, making this the one solidity test in the
    // amplifier that was not a heightfield, and v20 removed it — MINUS whatever
    // the M4 cave pass carves out of it (voxelcore/caves.h). Defined
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
        // The same gradient's two SIGNED components, mm per metre. Computed
        // beside the magnitude from the identical CarrierEval (they were being
        // thrown away); consumed only by surfaceInfo. Nothing on the voxel
        // path reads them, which is why carrying them cannot move the digest.
        int64_t slopeXMmPerM = 0;
        int64_t slopeYMmPerM = 0;
        int64_t px = 0, py = 0; // tile pixel the column falls in
    };
    SurfaceEval evalSurface(int64_t vx, int64_t vy) const;

    // The climate half of column(): faded-bilinear channel blend plus the
    // ecotone dither, at a column whose evalSurface already produced (px, py).
    // ONE function used by both column() and surfaceInfo(), so the two cannot
    // disagree about where a biome boundary falls -- a spawner keyed to a
    // biome that placement disagrees with puts deer in the wrong valley
    // deterministically, which is worse than randomly.
    struct ClimateAtColumn {
        ClimateSample cl;        // blended, undithered
        int32_t tempDithered = 0;
        int32_t precipDithered = 0;
    };
    ClimateAtColumn climateAtColumn(int64_t vx, int64_t vy, int64_t px, int64_t py) const;

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
    // The baked conduit table for the region being amplified. EMPTY BY DEFAULT
    // and empty in every shipping configuration today, which is what makes the
    // karst carve a proven no-op: see setKarstTable and the ColumnSample member.
    KarstTable karstTable_{};
    // Debug only; nullptr in every shipping configuration. See setWaterMarker.
    IWaterSampler* waterMarker_ = nullptr;
    bool waterMarkerOcean_ = true;
    // See waterMarkerColumnsMarked(). Touched only when waterMarker_ is set, so
    // a shipping run pays nothing.
    mutable std::atomic<int64_t> markerQueried_{0};
    mutable std::atomic<int64_t> markerMarked_{0};
    mutable std::atomic<int64_t> markerAboveGround_{0};
    // How far the marker's lateral fill searches for a water level, in FINE
    // PIXELS. See the search in amplifier.cpp.
    //
    // ZERO BY DEFAULT, MEASURED. Same pose, 90 s of streaming, marker only:
    //
    //   marker off                 51,063 chunks, settled (0 in flight, 0 pending)
    //   marker on, search 8 px     19,162 chunks, NOT settled (96 in flight, 860 pending)
    //   marker on, search off      51,059 chunks, settled
    //
    // The whole 2.7x regression was this search and nothing else -- with it off
    // the marker is free. It costs 8*n LOCKED water-sampler queries on EVERY
    // column, and 99.4% of columns are dry, so the world pays for a waterline
    // refinement it has no water to refine.
    //
    // Turn it on with -VoxelWaterMarkerFillPx=<n> when inspecting a waterline
    // and accept the slowdown; leave it off to fly the world.
    //
    // EIGHT WAS A MEASURED DISASTER for a second reason worth keeping. The search hands a dry column the water
    // level of the nearest wet cell within the radius, and the per-voxel test
    // then wets it wherever its ground is below that level. At 8 px (15 m) on a
    // valley floor that is almost every column, so the marker stopped being a
    // waterline and became a flood: 35.6 MILLION quads against ~1.8 M on the
    // same site with the marker working correctly, with the streamer unable to
    // keep up with the player and terrain chunks arriving minutes late.
    //
    // One pixel is what an edge actually needs. The defect being fixed is that
    // the baked wet mask is quantised to 1,875 mm, so the waterline snaps to a
    // cell boundary; reaching ONE cell past the mask is enough for the
    // amplified 10 cm ground to decide where the water really ends. Anything
    // beyond that is not resolving an edge, it is inventing water.
    int64_t waterMarkerFillPx_ = 0;
};

} // namespace vxc
