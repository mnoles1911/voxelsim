#pragma once
// Baked lakes on the client: the basin table turned into a water surface a
// column at a time.
//
// docs/watershed-system-plan.md work item 4 (§5.1, §11). This is the CLIENT
// half of the registry the bake writes (tilestore.h `BasinEntry`,
// SECTION_BASIN_TABLE, bake_ver 8).
//
// ZERO NEW MECHANISMS, which is the whole design. A baked lake datum has the
// SAME SHAPE as `CavernColumn::floodZMm` -- "water fills open air below this
// millimetre level in this column" -- which `WaterMobilizer` already consumes
// through the implicit field at VoxelWaterSubsystem.cpp's binding site. So
// nothing here places, meshes, replicates, persists or mobilises water. It
// answers ONE question, `waterSurfaceMmAtVoxel`, and the tested, shipping,
// already-budgeted path downstream does the rest: unmobilised lake water is a
// wall, digging the shore fires `NotifyTerrainVoxelsCleared` ->
// `mobilizeEditRegion`, fill replicates as diffs, the ledger audits to zero.
//
// THE EXTENT RULE IS SHARED WITH THE BAKE, not re-derived. `lakeExtentFill`
// below is `terrain_service/bake/basins.py:lake_extent_mask` in C++:
//
//     the 8-connected component of {elevation <= surfaceMm}, clipped to the
//     basin's bbox, that contains the basin's seed cell.
//
// A THRESHOLD ALONE WOULD BE WRONG and that is why this is a fill. Two basins
// can share a bbox, and any hillside below the water level also satisfies
// `elevation <= surfaceMm`; a client that drew that would flood dry ground on
// the far side of a ridge. The seed is the deepest cell of the component the
// registry recorded, so the fill can only ever return that component.
//
// EIGHT-CONNECTED, matching `depression_components`. Four is the better
// physics -- water does not squeeze through a diagonal pinch -- but the
// registry's area was measured on the 8-connected component, and an extent
// that disagrees with the row it ships beside is a shoreline that does not
// close. Physics loses to the single definition, on purpose, in both
// languages, and `tests/test_lakes.cpp` asserts the two agree on a fixture.
//
// WHICH ELEVATION THE CLIENT TESTS, corrected 2026-08-10. An earlier version
// of this comment claimed that "a shipped tile's elevation plane IS the
// re-opened surface these basins were measured on". It is not, quite: the bake
// measured the depression on its re-opened PER-PIXEL surface (`z_open`), and
// what the tile carries is the PREFILTERED B-SPLINE CONTROL LATTICE for that
// surface -- a control point is not a sample of it, and the prefilter stands
// one up to 5.6 m off the surface it interpolates (tile_codec.py; p99 4.4 m on
// this world). Measured over the bv26 wet block's 2,049 water-holding basins
// (vxc_lakeextentprobe), the per-CELL consequence is nil: the carrier is a
// uniform cubic B-spline, a CONVEX average of nearby control points, so the
// reconstructed surface cannot dip below a datum that no nearby control point
// dips below, and the lattice extent and the spline extent agree to 0.1% of
// area (970.1 vs 970.8 ha). The per-SEED consequence is not nil: the seed is
// ONE cell, prefilter ringing can strand exactly that cell's control point
// above the datum, and a fill whose seed reads dry returns an EMPTY mask for a
// basin whose component is sitting right there in the bbox -- 2 of the 2,049
// rendered nothing anywhere, that way. `lakeExtentFillRescued` below is the
// repair, and it is deliberately narrow: the lattice rule runs first and is
// byte-identical for every basin whose mask was already non-empty; only an
// EMPTY result triggers a refill that also accepts cells the RECONSTRUCTED
// SPLINE (`reconstructedGroundMm`, the same ground the river datum is defined
// against) puts at or under the datum, re-seeded at the v2 floor cell if even
// the wire seed stays dry.
//
// WHAT BOUNDS THE WATER FROM BELOW IS NOT THIS FILE. What the player sees is
// the AMPLIFIED surface: carrier spline plus detail bands, at 10 cm. The
// extent and the datum disagree with it by the detail band's amplitude near
// the shore. The composed predicate in §5.1 resolves that the only way that
// cannot produce floating water -- `zMm >= amplifiedGroundMm` -- so a column
// inside the extent whose amplified ground rises above the datum simply yields
// no water voxels, and the shoreline follows the ground's own contour instead
// of the lattice's staircase. The DATUM is authoritative; the extent says
// where to look; the ground says where to stop.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "voxelcore/caverns.h"
#include "voxelcore/core.h"
#include "voxelcore/tilestore.h"

namespace vxc {

// Declared here and defined below, so IWaterSampler's ribbon half can hand one
// back without moving RiverSampler above the interface it implements.
class RiverSampler;

// `kNoWaterMm` moved to tilestore.h at bake_ver 9 -- the water PLANE is decoded
// there and must answer "dry" in the same currency this file's lake sampler
// does. Still one constant, one meaning; only its home changed.

// Fill units (0..255, the CA's own currency) for the voxel whose BOTTOM is at
// `zBottomMm`, under a water surface at `surfaceMm`.
//
// PARTIAL TOP FILL (§5.1's "nicety", and it is not one): without it the
// surface snaps to the 10 cm lattice, so a lake whose datum sits at x.37 of a
// voxel renders 6.3 cm too high or too low and two adjacent lakes 9 cm apart
// render at the same height. The CA expresses fractional fill natively, so
// carrying it costs nothing and buys a surface that sits AT the datum.
//
// The bottom, not the centre (which is what `cavernFloodedAt` uses): a
// fraction needs the distance from the bottom face, and rounding rather than
// truncating keeps the mean error at zero instead of half a fill unit low.
//
// NO CLAMP TO 1 IS NEEDED and one was written and then deleted, because the
// arithmetic already forbids the case it guarded: 255 units span 100 mm, so
// one millimetre of water rounds to 3 units, and the only way to reach 0 is
// `rem <= 0`, which returns above. A defensive clamp here would have been
// unreachable code with a plausible-sounding comment attached, which is worse
// than none.
constexpr uint8_t waterFillUnits(int64_t zBottomMm, int32_t surfaceMm) {
    if (surfaceMm == kNoWaterMm) return 0;
    const int64_t rem = static_cast<int64_t>(surfaceMm) - zBottomMm;
    if (rem <= 0) return 0;
    if (rem >= kVoxelSizeMm) return 255;
    return static_cast<uint8_t>((rem * 255 + kVoxelSizeMm / 2) / kVoxelSizeMm);
}

// The extent of one basin, as a bitmask over its own bbox.
//
// `elev(lx, ly)` returns the tile-local control point elevation in absolute
// mm. Templated rather than taking an interface so the bake fixture, the unit
// tests and `FineTileSampler` all drive the SAME code -- a second copy for
// testing is how the two languages would drift.
//
// Returns the number of cells set. `out` is resized to bboxW * bboxH bytes,
// one per cell, 1 = wet.
//
// Explicit stack, no recursion: a basin can be 2.5 million cells (the survey's
// largest is 865 ha at 1.875 m/px) and a recursive fill on that is a stack
// overflow, not a slow function.
//
// `lakeExtentFillFrom` is the same fill with an EXPLICIT seed, for the rescue
// path below: the wire seed is `b.seedX/seedY` and `lakeExtentFill` keeps that
// as the one spelling every existing caller and the Python fixture pin.
template <class ElevFn>
size_t lakeExtentFillFrom(const BasinEntry& b, int32_t seedX, int32_t seedY, ElevFn&& elev,
                          std::vector<uint8_t>& out) {
    const int32_t x0 = b.bboxX0, y0 = b.bboxY0, x1 = b.bboxX1, y1 = b.bboxY1;
    const int32_t w = x1 - x0 + 1, h = y1 - y0 + 1;
    out.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    if (w <= 0 || h <= 0) return 0;
    if (seedX < x0 || seedX > x1 || seedY < y0 || seedY > y1) return 0;
    if (b.surfaceMm == kNoWaterMm) return 0;

    const int32_t sx = seedX - x0, sy = seedY - y0;
    if (elev(seedX, seedY) > b.surfaceMm) return 0;

    std::vector<int32_t> stack;
    stack.reserve(256);
    out[size_t(sy * w + sx)] = 1;
    stack.push_back(sy * w + sx);
    size_t n = 1;
    static constexpr int32_t kDy[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
    static constexpr int32_t kDx[8] = {0, 0, -1, 1, -1, 1, -1, 1};
    while (!stack.empty()) {
        const int32_t c = stack.back();
        stack.pop_back();
        const int32_t cy = c / w, cx = c - cy * w;
        for (int k = 0; k < 8; ++k) {
            const int32_t ny = cy + kDy[k], nx = cx + kDx[k];
            if (ny < 0 || ny >= h || nx < 0 || nx >= w) continue;
            const int32_t ni = ny * w + nx;
            if (out[size_t(ni)]) continue;
            if (elev(x0 + nx, y0 + ny) > b.surfaceMm) continue;
            out[size_t(ni)] = 1;
            ++n;
            stack.push_back(ni);
        }
    }
    return n;
}

template <class ElevFn>
size_t lakeExtentFill(const BasinEntry& b, ElevFn&& elev, std::vector<uint8_t>& out) {
    return lakeExtentFillFrom(b, int32_t(b.seedX), int32_t(b.seedY), std::forward<ElevFn>(elev),
                              out);
}

// THE SEED RESCUE (2026-08-10), for the 2-in-2,049 case measured on the bv26
// wet block (see the extent-rule comment at the top of this file): a
// water-holding basin whose wire seed's CONTROL POINT rings above the datum,
// so the lattice fill dies at its very first test and the whole lake renders
// nothing, anywhere, forever -- while the wet component sits in the bbox one
// cell away. The two measured casualties: tile (-3,-5) basin 391 (seed cp
// +37 mm over the datum, spline -1,228 mm under it) and tile (-4,-5) basin 538
// (cp +921 mm over, spline -1,094 mm under).
//
// THE PRIMARY RULE IS UNTOUCHED. `latticeElev` runs first, and a basin whose
// mask comes out non-empty gets EXACTLY the mask it always got -- byte for
// byte, 2,047 of 2,049 on the measured block -- because a fill rule that moved
// for everyone to serve two basins would re-litigate every shoreline in the
// world. Only an EMPTY mask triggers the refill, whose accessor accepts a cell
// when EITHER ground puts it at or under the datum: the lattice (the shipped
// rule), or `groundElev` -- the RECONSTRUCTED SPLINE, which is the surface the
// bake actually measured the depression on, to ~42 mm. That union can only
// ever be a superset of the lattice component, so the rescue cannot lose a
// cell the old rule would have kept.
//
// `groundElev` must return INT32_MAX where it cannot be evaluated (a stencil
// that would cross into an unloaded tile); the lattice test alone then decides,
// which is the pre-rescue behaviour. `floorLx/floorLy` is the basin's v2 floor
// cell in the same tile-local pixels as the bbox, or -1 when the row is v1 or
// the floor lies in a neighbouring tile: if even the wire seed stays dry under
// both grounds, the fill re-seeds there -- the deepest cell of the WHOLE
// basin, which for a spanning row can rescue the tile whose clipped seed was
// the casualty.
//
// COST, with the arithmetic: the happy path pays nothing (one lattice fill,
// as before). A rescued basin pays one extra fill over its bbox in which only
// the lattice-REJECTED cells it visits gather the 4x4 spline stencil --
// measured across all 2,049 bv26 basins forced through the hybrid accessor,
// 0.95 M spline gathers over 8.06 M bbox cells and 0.30 s total, i.e. ~0.15 ms
// for a median basin and 8 ms for the block's largest (1.49 M-cell bbox). Two
// basins pay it today.
template <class ElevFn, class GroundFn>
size_t lakeExtentFillRescued(const BasinEntry& b, ElevFn&& latticeElev, GroundFn&& groundElev,
                             int32_t floorLx, int32_t floorLy, std::vector<uint8_t>& out) {
    const size_t n = lakeExtentFill(b, latticeElev, out);
    if (n != 0 || b.surfaceMm == kNoWaterMm) return n;
    auto either = [&](int32_t x, int32_t y) -> int32_t {
        const int32_t cp = latticeElev(x, y);
        if (cp <= b.surfaceMm) return cp;
        const int32_t g = groundElev(x, y);
        return g == INT32_MAX ? cp : g;
    };
    const size_t r = lakeExtentFillFrom(b, int32_t(b.seedX), int32_t(b.seedY), either, out);
    if (r != 0) return r;
    if (floorLx < 0 || floorLy < 0 ||
        (floorLx == int32_t(b.seedX) && floorLy == int32_t(b.seedY)))
        return r;
    return lakeExtentFillFrom(b, floorLx, floorLy, either, out);
}

// What the composed ImplicitFn asks, and the only thing this layer answers.
//
// ---------------------------------------------------------------------------
// THE LEDGER-DELTA HOOK (water re-architecture Phase 2)
// ---------------------------------------------------------------------------
//
// A basin's datum on the wire is its BAKED EQUILIBRIUM surface -- where the
// climate says the lake stands when nothing has happened to it. Runtime
// hydrology moves it: rain routed down the graph fills a lake, a player
// breaches a sill and drains one. The authoritative "how far from equilibrium
// is this basin now" scalar is a per-basin int64 volume delta living in
// voxelcore/basinledger.h, and THIS interface is the one seam through which the
// drawn water asks about it.
//
// ONE SEAM, not two, and that is the whole reason it is here rather than at the
// two call sites. `LakeSampler::surfaceAtPixel` feeds the near-field implicit
// fill, and the sheet gather feeds the far-field rectangles; if each applied
// the delta itself, the near field and the far field could disagree about how
// high a lake stands, which is the exact seam the sheet was careful to close
// when it shipped.
//
// THE DEFAULT IS THE BAKED SURFACE. An implementation that holds no delta for a
// basin must return `baked.surfaceMm` unchanged, and a sampler with no datum
// source at all behaves bit-identically to the version before this existed --
// which is what keeps every shipped lake, and every pinned test, where it was.
//
// KNOWN v0 LIMIT, written down because it is visible rather than theoretical:
// the EXTENT does not move with the datum. Masks are built once against the
// baked `surfaceMm` (see `maskFor`), so a lake credited up towards its sill
// renders at the correct HEIGHT inside the outline it had when it was at
// equilibrium, and a drained one keeps the wider outline. In the near field the
// per-voxel `zMm >= amplifiedGroundMm` bound hides the second case entirely and
// most of the first; the far-field sheet shows both. The fix is a mask taken at
// the sill, which is cheap to build and expensive to keep for every basin --
// deferred until basin table v2 ships an extent the client does not have to
// reconstruct.
//
// SECOND KNOWN v0 LIMIT: a DRY basin credited water does not draw. `indexFor`
// skips every row where `holdsWater()` is false, so a playa or a salt flat is
// not in the bucket index at all and `surfaceAtPixel` never reaches its datum.
// The LEDGER still tracks it correctly -- the volume is real, it spills at the
// right level, and it persists -- it simply is not rendered. Admitting dry rows
// to the index would build an extent mask for every playa in every tile whether
// or not one ever fills, which is the cost the index exists to avoid; the right
// fix is to admit a row when the ledger reports a non-zero delta for it, and it
// needs the index to be invalidated on the first credit rather than only on a
// tile change.
class IBasinDatumSource {
public:
    virtual ~IBasinDatumSource() = default;
    // Absolute mm the basin's surface stands at RIGHT NOW. `baked` is the row
    // as shipped; returning `baked.surfaceMm` is the correct answer for a basin
    // this source knows nothing about.
    virtual int32_t basinDatumMm(int32_t tx, int32_t ty, const BasinEntry& baked) = 0;
};

// An interface rather than a concrete class because two implementations
// already matter: `LakeSampler` over baked tiles, and "nothing is baked here"
// -- a client with no fine tier must still run, with the ocean and the caverns
// unaffected, rather than crash or invent a lake.
class IWaterSampler {
public:
    virtual ~IWaterSampler() = default;
    // Absolute mm of the baked water surface over this world VOXEL column, or
    // kNoWaterMm where there is none. NOT const: implementations decode and
    // cache lazily, and pretending otherwise would be a lie the caller might
    // thread on.
    virtual int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) = 0;

    // Phase 5 of the water re-architecture: the engine flips this each tick
    // from voxel.Water.RetireBakedRivers. When retired, an implementation
    // that composes rivers and lakes must answer with LAKES ONLY -- rivers
    // near the player are the fluid sim's job and the plane is data, not
    // shape. Default no-op so null/test samplers need not care.
    virtual void setRetireBakedRivers(bool) {}

    // ---- THE SHEET HALF (watershed plan work item 5, §5.2) ----------------
    //
    // `waterSurfaceMmAtVoxel` answers "is there water over THIS column", which
    // is the only question the near field's brick sweep asks. A sheet asks the
    // other one: "which lakes are near me, and where does each one END". That
    // cannot be reconstructed from a per-column query without walking every
    // column in a 10 km disc, so the registry the sampler already holds is
    // exposed rather than re-derived.
    //
    // DEFAULTED TO "NOTHING HERE", not pure virtual, for the same reason
    // NullWaterSampler exists at all: a client with no fine tier is a supported
    // configuration, and it should draw no sheets by construction rather than
    // by a null check at the call site.
    //
    // The pointers are borrowed and stay valid until the sampler is destroyed
    // or the underlying tile is evicted; a caller that holds one across a tile
    // load is holding a dangling pointer, which is why the UE actor copies what
    // it needs into world space in the same call.
    virtual const std::vector<BasinEntry>* basinsForTile(int32_t /*tx*/, int32_t /*ty*/) { return nullptr; }
    // The 1-byte-per-cell wet mask over `basinsForTile(tx,ty)[id]`'s bbox, in
    // the layout `lakeExtentFill` writes. nullptr when the tile or the basin is
    // not resolvable -- which must NOT be read as "this basin is dry".
    virtual const std::vector<uint8_t>* extentMaskFor(int32_t /*tx*/, int32_t /*ty*/, uint16_t /*id*/) {
        return nullptr;
    }
    // Tile edge in fine pixels, and mm per fine pixel: what turns a tile-local
    // bbox into world millimetres. 0 means "this sampler has no tiles".
    virtual uint32_t tilePixels() const { return 0; }
    virtual int32_t pixelSizeMm() const { return 0; }

    // The LEDGER-ADJUSTED datum for one row of `basinsForTile` (see
    // IBasinDatumSource above). Every consumer of a basin's height must ask
    // this instead of reading `BasinEntry::surfaceMm` directly, or the far
    // field will draw a lake at a height the near field has already left.
    //
    // Defaulted to the baked surface, so a sampler that has no ledger -- and
    // every existing caller that has not been taught about one -- behaves
    // exactly as it did before Phase 2.
    virtual int32_t basinDatumMm(int32_t /*tx*/, int32_t /*ty*/, const BasinEntry& baked) {
        return baked.surfaceMm;
    }

    // Binds the ledger, and drops whatever the sampler memoised against the
    // datum it had before.
    //
    // THESE ARE ON THE INTERFACE, not on LakeSampler, for the reason
    // ribbonTiles()/ribbonRivers() are: UE modules build with /GR- so there is
    // no dynamic_cast, a static_cast onto the wrong sampler is silent memory
    // corruption, and the host holds only an IWaterSampler*. Defaulted to no-ops
    // so a world with no fine tier -- or with a sampler that has no basins --
    // ignores a ledger it could not use anyway.
    virtual void setBasinDatumSource(IBasinDatumSource* /*source*/) {}
    virtual void invalidateBasinDatumMemo() {}

    // ---- THE RIBBON HALF (far-field FLOWING water, riverribbon.h) ----------
    //
    // The sheet half above is structurally lake-only -- it is a basin registry
    // and a per-basin extent mask, and a river reach is neither. The far-field
    // river producer needs the two objects underneath instead: the tile
    // sampler, because riverRibbonFillWet scans the raw depth raster block by
    // block rather than asking per pixel, and the river sampler, because
    // riverRibbonResolveDatum takes the datum from surfaceAtPixel -- the SAME
    // call the near field reaches through waterSurfaceMmAtVoxel, which is what
    // makes near and far agree on height by construction rather than by tuning.
    //
    // EXPOSED AS BORROWED POINTERS RATHER THAN A SECOND SAMPLER, and that is
    // the whole point: a caller that built its own FineTileSampler would have
    // a second tile set, a second residency state and a second answer to "is
    // this pixel wet". These hand back the ones this sampler already owns.
    //
    // DEFAULTED TO nullptr for the reason the sheet half is defaulted to
    // nullptr: a client with no fine tier is supported, and it should draw no
    // ribbons by construction. A caller must treat null as "no far-field river
    // here", never as "this valley is dry".
    virtual FineTileSampler* ribbonTiles() { return nullptr; }
    virtual RiverSampler* ribbonRivers() { return nullptr; }
};

// Answers kNoWaterMm everywhere. The client's default, so "no fine tier
// loaded" is a supported configuration and not a null check at every call.
class NullWaterSampler final : public IWaterSampler {
public:
    int32_t waterSurfaceMmAtVoxel(int64_t, int64_t) override { return kNoWaterMm; }
};

// A water sampler that is safe to query from many threads at once.
//
// WHY IT HAS TO EXIST. Every real IWaterSampler here MUTATES on query -- the
// first read inside a block decodes it, the first read inside a basin builds
// that basin's extent mask -- so the contract is "prewarm from one thread, then
// read from many". That contract is satisfiable for the near-field sweep, which
// knows its region in advance. It is NOT satisfiable for the debug water
// marker: `Amplifier::column` is called from the mesher worker pool at whatever
// columns the streamer happens to want, so there is no moment at which the
// region of interest is known and no thread on which to prewarm it.
//
// Rather than make every sampler thread-safe -- which would put a lock on the
// near-field path, the hottest loop in the water system, to serve a debug mode
// -- the lock lives here and only the debug path pays it.
//
// Contention is real and accepted: marker mode serialises one tile-block decode
// across the mesher pool. It is an instrument, not a shipping path.
class LockedWaterSampler final : public IWaterSampler {
public:
    explicit LockedWaterSampler(IWaterSampler& inner) : inner_(&inner) {}

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        std::lock_guard<std::mutex> guard(m_);
        return inner_->waterSurfaceMmAtVoxel(vx, vy);
    }

    // The sheet half is NOT forwarded, deliberately. It hands out BORROWED
    // pointers into the inner sampler's own storage (see IWaterSampler), and a
    // lock held only for the duration of this call would not protect the caller
    // while it reads through them. The marker never asks these questions; a
    // future caller that needs them must solve that lifetime problem rather
    // than inherit a lock that looks like it helps.

private:
    IWaterSampler* inner_;
    std::mutex m_;
};

// Baked lakes over a `FineTileSampler`.
//
// THREADING mirrors `FineTileSampler`'s exactly, and for the same reason: the
// first query inside a basin decodes elevation blocks and builds that basin's
// extent mask, so queries MUTATE. Call `prewarmTile` from one thread over the
// region of interest; after that every touched basin is resident and queries
// are pure reads.
//
// COST. Building a basin's mask is O(bbox) once, and the bbox is on the wire
// precisely so it is not O(tile): the survey's median basin is 0.59 ha (about
// 1,700 cells) and its 90th percentile 2.27 ha. The bucket index below turns a
// column query into a handful of bbox tests rather than a scan of up to 266
// basins, and a one-entry column memo collapses the inner z loop of a brick
// sweep -- which is the access pattern the ImplicitFn actually has -- to one
// lookup per column instead of one per voxel.
class LakeSampler final : public IWaterSampler {
public:
    // Bucket edge in fine pixels for the per-tile basin index. 256 gives a
    // 32x32 index over an 8192 tile: small enough that a bucket holds a couple
    // of basins, big enough that the index itself is 1 KB of pointers.
    static constexpr int32_t kBucketPx = 256;

    explicit LakeSampler(FineTileSampler& tiles) : tiles_(tiles) {}

    // --- the ledger-delta hook (see IBasinDatumSource) ----------------------
    //
    // Borrowed and optional; null (the default) means every basin stands at its
    // baked equilibrium and this class runs the arithmetic it always ran. The
    // source must outlive the sampler.
    void setBasinDatumSource(IBasinDatumSource* source) override {
        datum_ = source;
        memoValid_ = false; // the memo below caches a DATUM, not a mask
    }
    IBasinDatumSource* datumSource() const { return datum_; }

    // Drops the one-entry column memo. A host must call this whenever it moves
    // a basin's ledger delta, because the memo answers "the surface over this
    // pixel" and a credit changes that answer without changing the pixel. One
    // entry deep, so this is not a cache flush, it is a single bool -- and the
    // alternative (a generation counter threaded through the ledger) buys
    // nothing at this size.
    void invalidateBasinDatumMemo() override { memoValid_ = false; }

    int32_t basinDatumMm(int32_t tx, int32_t ty, const BasinEntry& baked) override {
        return datum_ == nullptr ? baked.surfaceMm : datum_->basinDatumMm(tx, ty, baked);
    }

    // Decodes and indexes the tile's registry. Optional -- queries do it on
    // demand -- but doing it up front is what makes the query path a pure
    // read. False when the tile is not loaded.
    bool prewarmTile(int32_t tx, int32_t ty) { return indexFor(tx, ty) != nullptr; }

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        const int32_t pxMm = tiles_.pixelSizeMm();
        if (pxMm <= 0) return kNoWaterMm;
        const int64_t px = floorDiv(vx * kVoxelSizeMm, pxMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, pxMm);
        if (memoValid_ && px == memoPx_ && py == memoPy_) return memoMm_;
        const int32_t mm = surfaceAtPixel(px, py);
        memoPx_ = px;
        memoPy_ = py;
        memoMm_ = mm;
        memoValid_ = true;
        return mm;
    }

    // Same question in tile-pixel space, for tests and for the clipmap band
    // (work item 5), which walks pixels rather than voxels.
    int32_t surfaceAtPixel(int64_t px, int64_t py) {
        const uint32_t size = tiles_.tileSize();
        if (size == 0) return kNoWaterMm;
        const int32_t tx = int32_t(floorDiv(px, size)), ty = int32_t(floorDiv(py, size));
        TileIndex* idx = indexFor(tx, ty);
        if (idx == nullptr || idx->basins == nullptr) return kNoWaterMm;
        const int32_t lx = int32_t(px - int64_t(tx) * size);
        const int32_t ly = int32_t(py - int64_t(ty) * size);
        const int32_t bxi = lx / kBucketPx, byi = ly / kBucketPx;
        const std::vector<uint16_t>& cand =
            idx->buckets[size_t(byi) * idx->bucketsPerAxis + size_t(bxi)];
        // Highest surface wins where two basins somehow overlap. They should
        // not -- depression components are disjoint -- but "should not" is not
        // "cannot" across a bbox index, and picking the LOWER would drain a
        // lake into its neighbour's answer.
        int32_t best = kNoWaterMm;
        for (uint16_t id : cand) {
            const BasinEntry& b = (*idx->basins)[id];
            if (lx < b.bboxX0 || lx > b.bboxX1 || ly < b.bboxY0 || ly > b.bboxY1) continue;
            const std::vector<uint8_t>& mask = maskFor(*idx, id, tx, ty);
            if (mask.empty()) continue;
            const int32_t w = int32_t(b.bboxX1) - int32_t(b.bboxX0) + 1;
            if (!mask[size_t(ly - b.bboxY0) * size_t(w) + size_t(lx - b.bboxX0)]) continue;
            // THE LEDGER-ADJUSTED DATUM, not the bare wire field. With no datum
            // source this is `b.surfaceMm` and the comparison below is
            // byte-identical to what shipped; with one, a credited basin stands
            // higher here and therefore in the implicit fill, the marker and
            // every test that reads through this sampler, all at once.
            const int32_t datumMm = basinDatumMm(tx, ty, b);
            if (datumMm > best) best = datumMm;
        }
        return best;
    }

    // ---- IWaterSampler's sheet half, over the index this class already builds.
    const std::vector<BasinEntry>* basinsForTile(int32_t tx, int32_t ty) override {
        TileIndex* idx = indexFor(tx, ty);
        return idx == nullptr ? nullptr : idx->basins;
    }
    const std::vector<uint8_t>* extentMaskFor(int32_t tx, int32_t ty, uint16_t id) override {
        TileIndex* idx = indexFor(tx, ty);
        if (idx == nullptr || idx->basins == nullptr || id >= idx->basins->size()) return nullptr;
        const std::vector<uint8_t>& m = maskFor(*idx, id, tx, ty);
        // An EMPTY mask is `maskFor`'s "could not resolve" (it already counted
        // an unresolved basin), not a dry one -- a dry basin has a mask full of
        // zeroes, not no mask. Collapsing the two here would let the sheet draw
        // nothing for a tile that failed to decode and call it a shoreline.
        return m.empty() ? nullptr : &m;
    }
    uint32_t tilePixels() const override { return tiles_.tileSize(); }
    int32_t pixelSizeMm() const override { return tiles_.pixelSizeMm(); }

    // Diagnostics, so a bug is a number rather than an impression.
    size_t residentMaskCount() const { return maskCount_; }
    // Basins skipped because their tile is not loaded or a block failed to
    // decode. A non-zero value means water is MISSING, which looks exactly
    // like "there is no lake here" and must not.
    uint64_t unresolvedBasins() const { return unresolved_; }

private:
    struct TileIndex {
        const std::vector<BasinEntry>* basins = nullptr;
        size_t bucketsPerAxis = 0;
        std::vector<std::vector<uint16_t>> buckets;
        std::unordered_map<uint16_t, std::vector<uint8_t>> masks;
    };

    static uint64_t key(int32_t tx, int32_t ty) {
        return (uint64_t(uint32_t(tx)) << 32) | uint64_t(uint32_t(ty));
    }

    TileIndex* indexFor(int32_t tx, int32_t ty) {
        const FineTile* t = tiles_.findTile(tx, ty);
        auto it = index_.find(key(tx, ty));
        if (it != index_.end()) {
            // RESIDENCY IS RE-CHECKED ON EVERY HIT, not only on the miss that
            // built the entry, and this is a lifetime rule rather than a
            // freshness one. `TileIndex::basins` is a pointer BORROWED from a
            // resident FineTile -- while `FineTileSampler::unloadTile` destroys
            // that tile. A cached entry outliving its tile does not answer
            // stale, it answers FREED MEMORY, and what that draws is a lake
            // sheet at an arbitrary Z. (The masks go with it: they are indexed
            // by that tile's own bbox.)
            //
            // NOT REACHABLE TODAY, and that is exactly why it is worth one hash
            // lookup: FLakeWaterSampler owns a private FineTileSampler that
            // never unloads, so nothing calls unloadTile underneath this map --
            // but the STREAMER already calls it, and the day the lake tier gets
            // a byte budget this becomes live with no other change. A floating
            // sheet at an arbitrary height is the defect this project spent
            // 2026-08-04 chasing from the other end.
            //
            // THE THIRD STATE IS WHY THIS IS NOT JUST A NULL CHECK. A partial
            // tile (bake_ver 12) can be resident with its basin table NOT yet
            // fetched, and can then gain it. `basins == nullptr` cached against
            // a tile that now has a resident registry is a tile whose lakes
            // would stay invisible forever, so that transition invalidates too.
            const bool bExpectBasins = t != nullptr && t->hasBasins() && t->basinsResident();
            const bool bValid = t != nullptr && (bExpectBasins ? it->second.basins == &t->basins()
                                                              : it->second.basins == nullptr);
            if (bValid) return &it->second;
            index_.erase(it);
            // The column memo answered from the entry just dropped.
            memoValid_ = false;
        }
        if (t == nullptr) return nullptr;
        TileIndex idx;
        // hasBasins() false means "baked before the registry existed", which
        // is NOT "no basins": leaving `basins` null keeps those two apart, and
        // a tile with an empty-but-present table indexes to zero candidates.
        //
        // basinsResident() is the THIRD state, and it arrived with partial
        // tiles: the tile declares a registry that this client has not fetched.
        // That is also not "no basins", and it routes to the same null -- which
        // makes extentMaskFor answer nullptr and bumps unresolvedBasins, i.e.
        // water MISSING and counted, rather than water absent and believed.
        if (t->hasBasins() && t->basinsResident()) {
            idx.basins = &t->basins();
            const uint32_t size = t->size();
            idx.bucketsPerAxis = size_t((size + kBucketPx - 1) / kBucketPx);
            idx.buckets.resize(idx.bucketsPerAxis * idx.bucketsPerAxis);
            for (size_t i = 0; i < idx.basins->size(); ++i) {
                const BasinEntry& b = (*idx.basins)[i];
                if (!b.holdsWater()) continue;  // a dry playa has no surface
                for (int32_t byi = b.bboxY0 / kBucketPx; byi <= b.bboxY1 / kBucketPx; ++byi)
                    for (int32_t bxi = b.bboxX0 / kBucketPx; bxi <= b.bboxX1 / kBucketPx; ++bxi)
                        idx.buckets[size_t(byi) * idx.bucketsPerAxis + size_t(bxi)]
                            .push_back(uint16_t(i));
            }
        }
        auto ins = index_.emplace(key(tx, ty), std::move(idx));
        return &ins.first->second;
    }

    const std::vector<uint8_t>& maskFor(TileIndex& idx, uint16_t id, int32_t tx, int32_t ty) {
        auto it = idx.masks.find(id);
        if (it != idx.masks.end()) return it->second;
        const BasinEntry& b = (*idx.basins)[id];
        const uint32_t size = tiles_.tileSize();
        const int64_t ox = int64_t(tx) * size, oy = int64_t(ty) * size;
        // Every cell of the bbox is about to be read, so decode the blocks
        // once here instead of block-faulting inside the fill.
        if (!tiles_.prewarm(ox + b.bboxX0, oy + b.bboxY0, ox + b.bboxX1, oy + b.bboxY1)) {
            ++unresolved_;
            auto ins = idx.masks.emplace(id, std::vector<uint8_t>{});
            return ins.first->second;
        }
        auto lattice = [&](int32_t lx, int32_t ly) {
            return tiles_.elevationMm(ox + lx, oy + ly);
        };
        // The rescue path's spline ground (see lakeExtentFillRescued). The 4x4
        // carrier stencil of cell (lx, ly) spans pixels [lx-1, lx+2] x
        // [ly-1, ly+2], so a cell within two pixels of the tile edge gathers
        // from a neighbouring tile; where that neighbour is NOT loaded the
        // stencil would mix the missing-tile default into the carrier and
        // manufacture a cliff, so those cells answer INT32_MAX -- "no spline
        // here" -- and the lattice test alone decides, exactly as before the
        // rescue existed. Neighbour pixels of a LOADED tile decode lazily on
        // first touch, which is fine on this path: maskFor is the mutating
        // half of this class's threading contract already.
        bool nb[3][3];
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                nb[dy + 1][dx + 1] =
                    (dx == 0 && dy == 0) || tiles_.findTile(tx + dx, ty + dy) != nullptr;
        const int64_t edge = int64_t(size);
        auto ground = [&](int32_t lx, int32_t ly) -> int32_t {
            const int xLo = (lx - 1 < 0) ? -1 : 0, xHi = (lx + 2 >= edge) ? 1 : 0;
            const int yLo = (ly - 1 < 0) ? -1 : 0, yHi = (ly + 2 >= edge) ? 1 : 0;
            if (!nb[yLo + 1][xLo + 1] || !nb[yLo + 1][xHi + 1] || !nb[yHi + 1][xLo + 1] ||
                !nb[yHi + 1][xHi + 1])
                return INT32_MAX;
            return reconstructedGroundMm(tiles_, ox + lx, oy + ly);
        };
        // The v2 floor cell, tile-local; -1 when v1 or when a spanning basin's
        // floor lies in the neighbour (the neighbour's own row covers it).
        int32_t fLx = -1, fLy = -1;
        if (b.hasV2()) {
            const int64_t wx = int64_t(b.globalIdWorldX()) - ox;
            const int64_t wy = int64_t(b.globalIdWorldY()) - oy;
            if (wx >= b.bboxX0 && wx <= b.bboxX1 && wy >= b.bboxY0 && wy <= b.bboxY1) {
                fLx = int32_t(wx);
                fLy = int32_t(wy);
            }
        }
        std::vector<uint8_t> mask;
        lakeExtentFillRescued(b, lattice, ground, fLx, fLy, mask);
        ++maskCount_;
        auto ins = idx.masks.emplace(id, std::move(mask));
        return ins.first->second;
    }

    FineTileSampler& tiles_;
    IBasinDatumSource* datum_ = nullptr;
    std::unordered_map<uint64_t, TileIndex> index_;
    size_t maskCount_ = 0;
    uint64_t unresolved_ = 0;
    int64_t memoPx_ = 0, memoPy_ = 0;
    int32_t memoMm_ = kNoWaterMm;
    bool memoValid_ = false;
};

// ---------------------------------------------------------------------------
// THE SHEET (watershed plan work item 5, §5.2)
// ---------------------------------------------------------------------------
//
// WHAT PROBLEM THIS SOLVES, precisely. `RefreshImplicitWater` meshes water only
// inside a 65-brick disc -- 52 m across. A lake is 2 km across. So beyond 26 m
// a baked lake is simply ABSENT: not dim, not low-detail, absent. Every vista,
// every screenshot from a ridge, every flight over the basin shows a dry hole
// where the water is. That is the whole of work item 5's first half.
//
// WHY RECTANGLES AND NOT A HEIGHTFIELD. The sheet is FLAT by construction --
// the datum is one number for the whole basin (§5.1) -- so the only thing its
// geometry has to express is its OUTLINE. A rectangle decomposition of the wet
// mask says exactly that and nothing else: no vertex carries a height, no
// vertex can disagree with its neighbour, and the whole basin is a few hundred
// triangles instead of the 1.6 million a per-cell grid over an 800k-cell extent
// would be.
//
// WHY THE MASK IS DECIMATED RATHER THAN MESHED AT 1.875 m. At the range this
// exists for, the shoreline's own pixel is far below the screen-space error of
// the terrain it meets: the clipmap draws that ground at 256 m per vertex. A
// decimation that keeps the outline within ~20 m is therefore invisible against
// its own backdrop while costing 1/100th of the triangles. `step` is the
// caller's, not a constant here, because the right value is a function of range
// and the caller is the only one who knows it.
//
// THE DECIMATED CELL IS WET IF ANY CELL IN THE BLOCK IS WET. This REVERSES the
// original centre-sample rule, and the reversal was forced by a screenshot:
// eroding left a jagged staircase of empty air between each lake and its own
// bank, which is what a player actually sees from the air.
//
// The old rule's argument was that "any wet" floats water over dry ground at
// the shore, and that erosion only loses a sliver "the near-field voxels draw
// anyway". Both halves turned out to be wrong in practice:
//   - Near-field water meshes only within ~25.6 m of the camera, so from any
//     altitude nothing draws the eroded rim. The sliver is the artefact.
//   - Dilated water does not float over dry ground where it matters, because
//     the ground it runs into is the BANK -- opaque terrain standing above lake
//     level, which occludes the overhang. The visible waterline becomes the
//     terrain's own intersection with the lake plane, which is smooth and
//     correctly shaped, rather than the decimation grid's staircase.
// The failure modes are not symmetric: over-cover hides, under-cover glares.

// One wet rectangle, in tile-local FINE PIXELS, inclusive on all four sides.
struct LakeSheetRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Decomposes a basin's extent mask into axis-aligned rectangles at `step` fine
// pixels, merging along X. Appends to `out`; returns the number appended.
//
// ROW RUNS, NOT A FULL RECTANGLE COVER. Merging in one axis is O(cells) and
// gives a convex lake ~one rectangle per row; a full 2D cover would give fewer
// primitives for a cost (and a bug surface) this does not need, since the
// rectangles are coplanar and abut exactly -- there is no crack to open between
// two of them however they are cut.
template <class MaskT>
size_t lakeSheetRects(const BasinEntry& b, const MaskT& mask, int32_t step,
                      std::vector<LakeSheetRect>& out) {
    if (step < 1) step = 1;
    const int32_t w = int32_t(b.bboxX1) - int32_t(b.bboxX0) + 1;
    const int32_t h = int32_t(b.bboxY1) - int32_t(b.bboxY0) + 1;
    if (w <= 0 || h <= 0) return 0;
    if (mask.size() != size_t(w) * size_t(h)) return 0;
    const size_t before = out.size();
    // ANY WET PIXEL IN THE BLOCK, not the block's centre sample, and the owner's
    // screenshot is why. The centre sample ERODES: a block more than half wet
    // whose middle pixel happens to be dry is dropped whole, so the sheet stops
    // up to a full step short of the shore and the lake is ringed by a jagged
    // staircase of EMPTY AIR between the water and its own bank -- at a 500 m
    // basin that step is ~4 fine pixels, 7.5 m of visible gap from the air.
    //
    // The erosion was justified above by "the near-field voxels draw the rim
    // anyway". That is no longer true: near-field water meshes only within
    // ~25.6 m of the camera, so anyone looking at a lake from more than a few
    // dozen metres up sees the eroded rim and nothing filling it.
    //
    // Dilating instead is free to look at: the extra half-block of water runs
    // INTO the bank, and the bank is opaque ground standing above lake level,
    // so the terrain occludes it. An over-covered rim is invisible; an
    // under-covered one is the artefact. Cost stays O(mask) overall -- each
    // pixel is visited at most once across all blocks -- and the scan stops at
    // the first wet pixel, so open water (the common case) exits immediately.
    const auto blockWet = [&](int32_t cx, int32_t cy) -> bool {
        const int32_t yEnd = (cy + step < h) ? (cy + step) : h;
        const int32_t xEnd = (cx + step < w) ? (cx + step) : w;
        for (int32_t y = cy; y < yEnd; ++y) {
            const size_t row = size_t(y) * size_t(w);
            for (int32_t x = cx; x < xEnd; ++x) {
                if (mask[row + size_t(x)] != 0) return true;
            }
        }
        return false;
    };
    for (int32_t cy = 0; cy < h; cy += step) {
        int32_t runStart = -1;
        for (int32_t cx = 0; cx < w; cx += step) {
            const bool wet = blockWet(cx, cy);
            if (wet && runStart < 0) {
                runStart = cx;
            } else if (!wet && runStart >= 0) {
                out.push_back(LakeSheetRect{b.bboxX0 + runStart, b.bboxY0 + cy,
                                            b.bboxX0 + cx - 1,
                                            b.bboxY0 + std::min(cy + step, h) - 1});
                runStart = -1;
            }
        }
        if (runStart >= 0) {
            out.push_back(LakeSheetRect{b.bboxX0 + runStart, b.bboxY0 + cy, b.bboxX0 + w - 1,
                                        b.bboxY0 + std::min(cy + step, h) - 1});
        }
    }
    return out.size() - before;
}

// `r` minus `hole`, as up to four rectangles written to `out`; returns how many.
// 0 means `r` is entirely inside `hole` and vanishes.
//
// THIS IS THE NEAR/FAR HANDOVER, and it is a subtraction rather than a fade
// because the two water surfaces are COPLANAR: the sheet and the near field's
// voxel water both sit at the datum, so an overlap is a z-fight AND a doubled
// translucent blend, and a gap is a ring of missing water. Only an exact cut
// gives neither, and an exact cut is available precisely because the near
// field's disc is an axis-aligned box in brick space.
inline size_t subtractRect(const LakeSheetRect& r, const LakeSheetRect& hole, LakeSheetRect out[4]) {
    // Disjoint (inclusive bounds, so touching edges do NOT overlap).
    if (hole.x1 < r.x0 || hole.x0 > r.x1 || hole.y1 < r.y0 || hole.y0 > r.y1) {
        out[0] = r;
        return 1;
    }
    size_t n = 0;
    if (hole.y0 > r.y0) out[n++] = LakeSheetRect{r.x0, r.y0, r.x1, hole.y0 - 1};        // below
    if (hole.y1 < r.y1) out[n++] = LakeSheetRect{r.x0, hole.y1 + 1, r.x1, r.y1};        // above
    const int32_t my0 = std::max(r.y0, hole.y0), my1 = std::min(r.y1, hole.y1);
    if (hole.x0 > r.x0) out[n++] = LakeSheetRect{r.x0, my0, hole.x0 - 1, my1};          // left
    if (hole.x1 < r.x1) out[n++] = LakeSheetRect{hole.x1 + 1, my0, r.x1, my1};          // right
    return n;
}

// ---------------------------------------------------------------------------
// THE FLUID SINK'S EXTENT BITMASK (PBF basin sink, VoxelFluidContract item 6)
// ---------------------------------------------------------------------------
//
// WHAT WENT WRONG WITHOUT THIS. The GPU basin sink shipped testing a basin's
// BOUNDING BOX against the lake datum: a particle inside the box at or below
// the surface counted as having reached standing water, so it despawned and
// credited the scalar ledger. At the owner's pinned spawn the picked basin was
// a 0.29 ha pond inside an 88 x 56 m bbox, and the fluid's active window is
// only 51.2 m across -- so the bbox STRICTLY CONTAINED the entire window, and
// every particle anywhere in it below the pond's surface was deleted as
// "standing water", including river water tens of metres from the pond.
// Measured in the round-17 playtest: 104,503 spawned, 104,277 despawned to the
// basin, 226 alive. That is a mean residence of 61 ms -- about 9 cm of travel
// at the 150 UU/s emit speed. The owner saw no flowing water at all, because
// the sink ate the river at the faucet mouth.
//
// So the sink must test the TRUE EXTENT: the same 8-connected wet component
// `lakeExtentFill` builds, which the lake sheet already draws from. This
// resamples that extent onto an N x N bit grid over the window the sim
// actually simulates, which is what makes it affordable on the GPU: at N = 32
// over 51.2 m the whole mask is 32 uints -- 128 bytes, a uniform array and one
// bit test per particle, no texture and no buffer.
//
// GEOMETRY. Cell (cx, cy) covers [winMinXMm + cx*cellMm, + cellMm) and is wet
// iff the fine pixel under its CENTRE is wet. Centre sampling rather than "any
// wet pixel in the cell", because growing the wet set here would resume
// DELETING RIVER WATER along the shoreline -- the exact defect this grid exists
// to end. The error is at most half a cell either way -- 80 cm at N = 32 over
// 51.2 m, well inside the 1.875 m pixel the extent itself is quantised to.
//
// NOTE THAT `lakeSheetRects` DELIBERATELY DOES THE OPPOSITE (any wet pixel),
// and the disagreement is the point rather than an oversight: these two
// decimate the same extent for opposite consequences. Drawing wants to
// OVER-cover, because a sheet that falls short leaves visible air between a
// lake and its bank while an overhang is hidden by the bank itself. Despawning
// wants to UNDER-cover, because a sink that reaches too far eats the river. So
// each rounds toward its own harmless side; do not "make them consistent".
//
// BITS. `rowsOut[cy]` bit `cx`, LSB = smallest x. All N rows are cleared
// first, so a cell whose pixel falls outside the basin's bbox stays 0.
//
// RETURNS the number of wet cells. Zero is a REAL answer -- "this basin has no
// wet cell in this window" -- and is not the same as "the extent could not be
// resolved", which this function cannot report because it never loads
// anything. The caller gets that distinction from `extentMaskFor` returning
// nullptr, and must keep the two apart for the same reason that accessor does:
// a lake that failed to decode must not read as a lake with no water in it.
template <class MaskT>
size_t basinExtentBits(const BasinEntry& b, const MaskT& mask, int64_t tileOxPx, int64_t tileOyPx,
                       int32_t pixelMm, int64_t winMinXMm, int64_t winMinYMm, int64_t cellMm,
                       int32_t n, uint32_t* rowsOut) {
    if (rowsOut == nullptr || n <= 0 || n > 32 || cellMm <= 0 || pixelMm <= 0) return 0;
    for (int32_t i = 0; i < n; ++i) rowsOut[i] = 0;
    const int64_t w = int64_t(b.bboxX1) - int64_t(b.bboxX0) + 1;
    const int64_t h = int64_t(b.bboxY1) - int64_t(b.bboxY0) + 1;
    if (w <= 0 || h <= 0) return 0;
    if (mask.size() != size_t(w) * size_t(h)) return 0;
    const int64_t halfMm = cellMm / 2;
    size_t setCells = 0;
    for (int32_t cy = 0; cy < n; ++cy) {
        const int64_t py =
            floorDiv(winMinYMm + int64_t(cy) * cellMm + halfMm, int64_t(pixelMm)) - tileOyPx;
        if (py < b.bboxY0 || py > b.bboxY1) continue;
        const size_t row = size_t(py - b.bboxY0) * size_t(w);
        uint32_t bits = 0;
        for (int32_t cx = 0; cx < n; ++cx) {
            const int64_t px =
                floorDiv(winMinXMm + int64_t(cx) * cellMm + halfMm, int64_t(pixelMm)) - tileOxPx;
            if (px < b.bboxX0 || px > b.bboxX1) continue;
            if (mask[row + size_t(px - b.bboxX0)] == 0) continue;
            bits |= (1u << uint32_t(cx));
            ++setCells;
        }
        rowsOut[cy] = bits;
    }
    return setCells;
}

// Baked RIVERS over a `FineTileSampler`, from the P2 water plane.
//
// The counterpart of LakeSampler and deliberately much simpler, because the
// plane is already the answer: the bake did the non-local work (a
// runoff-weighted accumulation sweep and a descent-enforcing pass down the D8
// forest) and wrote the water surface per pixel, so a query here is one block
// decode and one lookup. No flood fill, no extent mask, no registry -- the
// asymmetry is the point, and it is why rivers cost a plane while lakes cost a
// table.
//
// THREADING and COST mirror FineTileSampler exactly: the first query in a
// block decodes it, so queries MUTATE. Call `prewarmTile` over the region of
// interest from one thread, then read from many.
class RiverSampler final : public IWaterSampler {
public:
    explicit RiverSampler(FineTileSampler& tiles) : tiles_(tiles) {}

    bool prewarmTile(int32_t tx, int32_t ty) {
        const FineTile* t = tiles_.findTile(tx, ty);
        return t != nullptr && t->hasWater();
    }

    // The ribbon half. Hands back the tile set this sampler is already reading
    // rather than a second one, so riverRibbonFillWet's fast wet test and this
    // class's surfaceAtPixel cannot disagree about which tiles are resident --
    // the property riverribbonprobe measures at 0 disagreements.
    FineTileSampler* ribbonTiles() override { return &tiles_; }
    RiverSampler* ribbonRivers() override { return this; }

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        const int32_t pxMm = tiles_.pixelSizeMm();
        if (pxMm <= 0) return kNoWaterMm;
        const int64_t px = floorDiv(vx * kVoxelSizeMm, pxMm);
        const int64_t py = floorDiv(vy * kVoxelSizeMm, pxMm);
        if (memoValid_ && px == memoPx_ && py == memoPy_) return memoMm_;
        const int32_t mm = surfaceAtPixel(px, py);
        memoPx_ = px;
        memoPy_ = py;
        memoMm_ = mm;
        memoValid_ = true;
        return mm;
    }

    // Same question in tile-pixel space, for tests and for the clipmap band.
    int32_t surfaceAtPixel(int64_t px, int64_t py) {
        const uint32_t size = tiles_.tileSize();
        if (size == 0) return kNoWaterMm;
        const int32_t tx = int32_t(floorDiv(px, size)), ty = int32_t(floorDiv(py, size));
        const FineTile* t = tiles_.findTile(tx, ty);
        // A tile that is not resident, or one baked before the water plane
        // existed, answers DRY -- and the residency gate
        // (FVoxelFineTileStreamer, block-until-ready) is what makes that safe:
        // no chunk generates over a non-resident footprint, so "dry because not
        // loaded" can never be voxelised into a desync.
        if (t == nullptr || !t->hasWater()) return kNoWaterMm;
        const uint32_t lx = uint32_t(px - int64_t(tx) * size);
        const uint32_t ly = uint32_t(py - int64_t(ty) * size);
        const uint32_t log2 = t->blockLog2();
        const uint32_t dim = t->blockDim();
        const uint64_t k = blockKey(tx, ty, lx >> log2, ly >> log2);
        auto it = blocks_.find(k);
        if (it == blocks_.end()) {
            std::vector<int16_t> depth;
            if (!t->decodeWaterBlock(lx >> log2, ly >> log2, depth)) {
                ++unresolvedBlocks_;
                return kNoWaterMm;
            }
            it = blocks_.emplace(k, std::move(depth)).first;
        }
        const size_t i = size_t(ly & (dim - 1)) * dim + size_t(lx & (dim - 1));
        const int16_t d = it->second[i];
        // DRY IS DECIDED BEFORE THE SPLINE, not after. The reconstruction below
        // costs a 4x4 stencil gather, and ~99% of a production tile is dry --
        // paying it to compute a ground we are about to throw away would put a
        // 16-probe carrier evaluation on every voxel column of the near-field
        // sweep, the hottest loop in the water path.
        // Only the two SENTINELS are dry. A band cp is negative but carries a
        // level, and it is exactly the collar of pixels where the waterline
        // needs resolving -- returning kNoWaterMm for it would throw away the
        // whole feature at the one place it matters. The early-out's argument
        // survives: the sentinels still cover everything outside the collar,
        // which is ~97-99% of a tile, so only the collar pays the spline.
        if (d == kWaterDryDepth || d == kWaterNoLevel) return kNoWaterMm;
        // THE RECONSTRUCTED SURFACE, `spline(cp)` -- not this pixel's control
        // point, which is what this function used to pass and what the whole
        // signature of `waterMmFromDepth` exists to prevent.
        //
        // The lattice was chosen here originally on the argument that a
        // per-pixel sampler has no spline and that a sub-voxel datum offset
        // cannot change a coarse "is there water near here" answer. The first
        // half is no longer true (`reconstructedGroundMm` is the spline, and it
        // is the shipped one), and the second half was never the whole story:
        // THIS SAMPLER IS THE NEAR-FIELD DATUM. `FVoxelWaterImpl`'s ImplicitFn
        // takes `waterSurfaceMmAtVoxel` and hands it straight to
        // `implicitWaterFill` as the water surface, so an offset here is an
        // offset in the rendered waterline -- and |cp - surface| reaches 5.6 m
        // on this world, which is 56 voxels.
        //
        // Having ONE datum, correct, is deliberate: the alternative on the
        // table was to keep the cheap lattice answer here and add a
        // ground-taking overload for the near field, which is exactly the shape
        // of the conflation that has now been made three times in this
        // codebase. A caller cannot pick the wrong one if there is only one.
        return FineTile::waterMmFromDepth(d, reconstructedGroundMm(tiles_, px, py));
    }

    uint32_t tilePixels() const override { return tiles_.tileSize(); }
    int32_t pixelSizeMm() const override { return tiles_.pixelSizeMm(); }
    // No basins here, so no datum to adjust -- but this sampler DOES hold a
    // one-entry column memo over the same composite query, and a caller
    // invalidating the lake half must be able to invalidate this one in the
    // same call or a stale answer survives a pixel longer than it should.
    void invalidateBasinDatumMemo() override { memoValid_ = false; }

    size_t residentBlockCount() const { return blocks_.size(); }
    // Blocks whose payload failed to decode. Non-zero means water is MISSING,
    // which looks exactly like "there is no river here" and must not.
    uint64_t unresolvedBlocks() const { return unresolvedBlocks_; }

private:
    static uint64_t blockKey(int32_t tx, int32_t ty, uint32_t bx, uint32_t by) {
        return (uint64_t(uint32_t(tx)) << 44) ^ (uint64_t(uint32_t(ty)) << 24) ^
               (uint64_t(bx) << 12) ^ uint64_t(by);
    }

    // THE DEPTH PLANE ONLY. This used to cache the elevation block beside it,
    // on the argument that a water query always needs both -- true, but the
    // ground it needs is `spline(cp)`, whose 4x4 stencil straddles block and
    // tile boundaries and so cannot come from one cached block anyway.
    // `FineTileSampler` already caches decoded elevation blocks, and
    // `reconstructedGroundMm` reads through it, so keeping a second copy here
    // bought nothing and cost a decode per water block.
    FineTileSampler& tiles_;
    std::unordered_map<uint64_t, std::vector<int16_t>> blocks_;
    uint64_t unresolvedBlocks_ = 0;
    int64_t memoPx_ = 0, memoPy_ = 0;
    int32_t memoMm_ = kNoWaterMm;
    bool memoValid_ = false;
};

// Lakes and rivers as ONE query, which is what §5.1 means by "river water goes
// through the same ImplicitFn as lakes".
//
// The plan asked for this by putting lake surfaces INTO the water plane so the
// client had one uniform query. It is done here instead, and the difference is
// deliberate: writing a basin's surface into the plane as well would put a
// second copy of an already-shipped fact (SECTION_BASIN_TABLE's `surfaceMm`)
// on the wire, free to disagree with the first. Composing two samplers gives
// the same single query with no duplicated bytes and nothing to keep in step
// -- and the bake writes the plane DRY inside registered basins precisely so
// the two never both answer.
//
// HIGHEST WINS where they overlap anyway. They should not overlap; "should
// not" is not "cannot", and taking the lower would drain a lake into the river
// that feeds it.
class CompositeWaterSampler final : public IWaterSampler {
public:
    CompositeWaterSampler(IWaterSampler& lakes, IWaterSampler& rivers)
        : lakes_(lakes), rivers_(rivers) {}

    int32_t waterSurfaceMmAtVoxel(int64_t vx, int64_t vy) override {
        const int32_t a = lakes_.waterSurfaceMmAtVoxel(vx, vy);
        const int32_t b = rivers_.waterSurfaceMmAtVoxel(vx, vy);
        if (a == kNoWaterMm) return b;
        if (b == kNoWaterMm) return a;
        return a > b ? a : b;
    }

    // The sheet half belongs to the lakes: a river reach is not a flat disc
    // and cannot be drawn as one. Forwarded rather than merged so the sheet
    // actor keeps seeing exactly the registry it already consumes.
    const std::vector<BasinEntry>* basinsForTile(int32_t tx, int32_t ty) override {
        return lakes_.basinsForTile(tx, ty);
    }
    const std::vector<uint8_t>* extentMaskFor(int32_t tx, int32_t ty, uint16_t id) override {
        return lakes_.extentMaskFor(tx, ty, id);
    }
    uint32_t tilePixels() const override { return lakes_.tilePixels(); }
    int32_t pixelSizeMm() const override { return lakes_.pixelSizeMm(); }
    // The ledger datum belongs to the lakes for the same reason the registry
    // does: a river reach has no basin row to adjust.
    int32_t basinDatumMm(int32_t tx, int32_t ty, const BasinEntry& baked) override {
        return lakes_.basinDatumMm(tx, ty, baked);
    }
    void setBasinDatumSource(IBasinDatumSource* source) override {
        lakes_.setBasinDatumSource(source);
    }
    void invalidateBasinDatumMemo() override {
        // BOTH halves. The river sampler holds no basins, but it holds its own
        // one-entry column memo over the SAME composite query, so leaving it
        // warm would let a stale lake answer survive one pixel longer than the
        // lake sampler's own.
        lakes_.invalidateBasinDatumMemo();
        rivers_.invalidateBasinDatumMemo();
    }

    // The ribbon half is the exact mirror of the sheet half above: the sheet
    // belongs to the lakes because a reach is not a disc, and the ribbon
    // belongs to the rivers because a basin is not a centreline. Forwarded to
    // the river side, so a host holding only the composite can still reach the
    // producer without knowing how the composition was built.
    FineTileSampler* ribbonTiles() override { return rivers_.ribbonTiles(); }
    RiverSampler* ribbonRivers() override { return rivers_.ribbonRivers(); }

private:
    IWaterSampler& lakes_;
    IWaterSampler& rivers_;
};
// ---------------------------------------------------------------------------
// THE OCEAN (watershed plan §6.4, work item 8)
// ---------------------------------------------------------------------------
//
// THE SEA IS A LAKE WHOSE DATUM IS kSeaLevelMm AND WHOSE EXTENT IS "every
// column whose ground lies below that datum". That sentence is the whole
// feature, and writing it as one function beside `implicitWaterFill` rather
// than as a predicate of its own is the point: the ocean becomes the THIRD
// TERM of the same ImplicitFn, so it inherits — with no ocean-specific code
// anywhere downstream — the wall (unmobilized sea is solid to the CA), the
// budgeted one-way mobilization, the ledger, the replication and the
// persistence that lakes already got.
//
// WHAT THIS REPLACES is `UVoxelWaterSubsystem`'s Reservoir v0: a set of
// breach-seeded voxels pinned to 255 fill units every fixed step forever. That
// mechanism had three defects this one does not have by construction, and
// tests/test_ocean.cpp measures all three:
//
//   1. IT COULD NOT TELL A PIT FROM THE SEA. Its test was "this cleared voxel
//      is below z=0 and touches a non-solid cell that is also below z=0".
//      Dig into a hillside, below the datum, in two passes — which is just
//      "keep digging" — and the second pass sees the first pass's own air as
//      ocean and seeds an infinite spring inside dry rock. Measured:
//      `reservoir_v0_floods_an_inland_pit_the_datum_test_leaves_dry`.
//   2. ITS HEAD WAS THE BREACH, NOT THE DATUM. A cell pinned at 255 at
//      z = -10 voxels is a pressure source at z = -10, so a shaft rising out
//      of the tunnel equalizes to the tunnel's own roof instead of to sea
//      level. The sea fills to the sea's surface; a pinned cell fills to its
//      own. Measured: `breach_parity_open_shaft_*`.
//   3. IT NEVER STOPPED. "No support for detecting a plugged/re-solidified
//      breach" is its own doc comment; plug the hole and the spring keeps
//      running. Mobilized water is ordinary CA water and a plugged hole simply
//      stops feeding it.
//
// THE GROUND MUST BE THE WORLDGEN AMPLIFIED SURFACE, NOT THE EDITED OVERLAY,
// and that is what makes defect 1 structurally impossible rather than merely
// fixed. A pit a player digs into land does not lower its column's worldgen
// ground, so `oceanSurfaceMmAt` keeps answering kNoWaterMm however deep the
// pit goes. This is the same choice, for the same reason, that
// `UVoxelWaterSubsystem::IsUnderwaterAtWorld` already made for the underwater
// test in work item 1 — one rule, now shared by the fog and the water.
//
// STRICTLY BELOW, not at-or-below: a column whose ground sits exactly on the
// datum is a beach with zero depth of water over it, and answering
// kSeaLevelMm there would ask `waterFillUnits` for a zero remainder it already
// documents as unreachable.
constexpr int32_t oceanSurfaceMmAt(int32_t groundMm) {
    return groundMm < kSeaLevelMm ? kSeaLevelMm : kNoWaterMm;
}

// The one datum for a column: the highest of what the bake put there (a lake
// today, a river reach once item 7 lands) and what the sea puts there.
//
// MAX, and it is not arbitrary. The two can genuinely overlap — a coastal lake
// whose surface stands above sea level on a column whose ground is still below
// it, and, once rivers land, every river mouth, where the reach's own datum
// descends to meet kSeaLevelMm. Taking the LOWER there would cut a step down
// into the river at the exact frame the owner most wants to look at; taking
// the higher makes the two surfaces coplanar at the mouth and the join
// invisible, because at that point they are the same number.
//
// This is also why the ocean is composed HERE and not by giving the sea its
// own `IWaterSampler`. A sampler answers "what did the bake put over this
// column"; the sea is not baked, it is the datum itself, and a second sampler
// would have to be merged with the first by exactly this max() anyway — one
// mechanism, expressed once.
constexpr int32_t implicitWaterDatumMm(int32_t bakedSurfaceMm, int32_t groundMm) {
    const int32_t sea = oceanSurfaceMmAt(groundMm);
    if (bakedSurfaceMm == kNoWaterMm) return sea;
    if (sea == kNoWaterMm) return bakedSurfaceMm;
    return bakedSurfaceMm > sea ? bakedSurfaceMm : sea;
}

// The composed predicate of §5.1, as one function so the client's binding site
// and the tests cannot express it differently.
//
//   cavern flood      -> as today, unchanged
//   baked lake        -> open air between the AMPLIFIED ground and the datum
//   the ocean         -> the same, with the datum (§6.4; pass
//                        `implicitWaterDatumMm` as `waterSurfaceMm`)
//   otherwise         -> dry
//
// `groundMm` is the amplified surface for this column (Amplifier's
// `columnCached(vx, vy).surfaceMm`); it is what excludes a cave under the
// lakebed, and the mobilizer's own terrain half re-checks solidity per cell
// anyway. `waterSurfaceMm` is kNoWaterMm for a dry column.
// "This column can hold no implicit water at any height", for
// `implicitWaterCeilingMm` below. NOT kNoWaterMm: a ceiling is an int64 because
// it is compared against absolute voxel/brick millimetres, and reusing an int32
// sentinel in an int64 comparison is exactly the shape of trap this file's
// `waterSurfaceMm == kNoWaterMm` guards exist to avoid.
inline constexpr int64_t kNoImplicitWaterMm = INT64_MIN;

// THE NEAR-FIELD BRICK SWEEP'S PER-COLUMN CEILING (VoxelWaterSubsystem.cpp's
// `RefreshImplicitWater`), as one function so the sweep and the tests cannot
// express it differently -- the same reason `implicitWaterFill` lives here
// rather than at the binding site.
//
// WHY THE SWEEP NEEDS ITS OWN RULE AT ALL. The sweep does not consult the
// ImplicitFn; it re-derives the water ceiling per column and offers every brick
// at or below it. So it can offer bricks the fill would decline (wasted work)
// and, far worse, it decides what the fill is ever ASKED about -- a brick the
// sweep never offers is water that silently does not exist. Both directions
// have now cost a diagnosis each.
//
// THE TWO TERMS ARE NOT SYMMETRIC, and that asymmetry is the whole content of
// this function. Cavern water is bounded ABOVE by the ground: it is underground
// by construction (see `cavernWaterCeilingMm`). Lake, river and sea water is
// bounded BELOW by it: the datum stands above the bed, so a datum above the
// ground is the ordinary WET case and clamping it would empty every lake in the
// world. One max(), two opposite relationships to the same number.
//
// THE OCEAN IS DELIBERATELY ABSENT, matching the sweep: an untouched sea is
// already drawn by AVoxelOceanActor's plane, and offering it here would be tens
// of thousands of candidates at every shoreline. See RefreshImplicitWater's own
// comment for the two independent reasons.
constexpr int64_t implicitWaterCeilingMm(int32_t cavernFloodZMm, int32_t bakedSurfaceMm,
                                         int64_t groundMm) {
    const int64_t cav = cavernWaterCeilingMm(cavernFloodZMm, groundMm);
    const int64_t baked =
        bakedSurfaceMm == kNoWaterMm ? kNoImplicitWaterMm : static_cast<int64_t>(bakedSurfaceMm);
    return cav > baked ? cav : baked;
}
constexpr int64_t implicitWaterCeilingMm(const CavernColumn& cavern, int32_t bakedSurfaceMm,
                                         int64_t groundMm) {
    return implicitWaterCeilingMm(cavern.floodZMm, bakedSurfaceMm, groundMm);
}

constexpr uint8_t implicitWaterFill(int64_t vz, int32_t groundMm, int32_t waterSurfaceMm,
                                    bool cavernFlooded) {
    if (cavernFlooded) return 255;
    if (waterSurfaceMm == kNoWaterMm) return 0;
    const int64_t zMm = vz * kVoxelSizeMm;
    if (zMm < groundMm) return 0;  // below the ground surface: not open air
    return waterFillUnits(zMm, waterSurfaceMm);
}

} // namespace vxc
