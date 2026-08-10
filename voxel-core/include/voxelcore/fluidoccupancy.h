#pragma once
// CPU reference for the GPU fluid occupancy volume and particle collision
// (docs/water-rearchitecture-plan-2026-08-09.md, Phase 0 spike (c)).
//
// WHAT LIVES HERE AND WHY. The volume is 16 MB of GPU-resident bits and the
// collision runs inside a compute kernel, so neither can be tested where it
// lives — the only way to look at either is to launch the editor and judge
// water. This header is the engine-free definition of both halves: the bit
// layout the builder writes and the collision reads, and the face
// determination and projection maths particles are pushed out with. vxc_tests
// exercises it; the HLSL mirrors it.
//
// It is a REFERENCE, not a shipping path. Nothing in the runtime calls it per
// frame. ue-project/Shaders/VoxelFluidOccupancy.usf and
// ue-project/Shaders/VoxelFluidCollision.ush mirror it by hand, the same way
// worldgen.ush mirrors amplifier.h, and every constant that could drift is
// named here and quoted there by name.
//
// FLOAT POLICY. The packing half is integer-only, per the library's rules: bit
// indices, word indices and the brick→volume transpose never touch a float.
// The collision half IS float, deliberately. Particles are PRESENTATION under
// the plan's authority split (authoritative water is the integer basin ledgers
// and the routing graph), so no gameplay consequence and nothing on the wire
// depends on a particle position being bit-reproducible. What still has to be
// exactly right is which FACE was entered, and that is decided by an integer
// DDA over integer voxel coordinates — the same decision raycast.h makes, with
// the same conventions, for the same reason.
//
// RELATIONSHIP TO raycast.h. raycast.h is the authoritative integer Amanatides
// & Woo walk for dig/place targeting, and it is exact (fractional boundary
// comparisons by cross-multiplication, no floats at all). This is deliberately
// NOT that: a PBF substep walks from a float position to a float position and
// has to run inside a compute kernel that has no int64 helpers to spare, so
// the walk here is the float form of the same algorithm. Everything raycast.h
// decides, this decides the same way and reports with the same conventions:
//
//   faceAxis  0=x, 1=y, 2=z; -1 when the segment STARTS inside a solid voxel
//             so no face was crossed (raycast.h:22-24).
//   faceSign  +1 means the walk entered moving in +axis, i.e. through the
//             voxel's −axis face (raycast.h:20-22).
//   ties      simultaneous boundary crossings step ONE axis at a time, lowest
//             axis first (raycast.h:74-80).
//   voxels    voxel v spans [v*kVoxelSizeMm, (v+1)*kVoxelSizeMm) per axis,
//             floored indexing, negatives behave like positives.
//
// A test that pins this header against raycastVoxels on the same geometry is
// in tests/test_fluidoccupancy.cpp, and it is the thing that keeps the two
// from drifting apart.

#include "voxelcore/core.h"

#include <cmath>
#include <cstdint>

namespace vxc {

// ---------------------------------------------------------------------------
// The volume
// ---------------------------------------------------------------------------
//
// 1 bit per 10 cm voxel over the active region, packed x-major into uint32
// words: 32 voxels per word along x, rows along x, then y, then z. This is the
// layout VoxelFluidContract.ush:65-69 specifies and the ONLY layout that
// exists — VoxelFluidCollision.ush reads it and VoxelFluidOccupancy.usf writes
// it, and both quote these names.
//
// 512 voxels is 51.2 m, the active-simulation cube the plan sizes PBF against
// (plan §5, "a GPU occupancy volume over the active region, 512^3 bits ≈ 16 MB
// for the 51.2 m cube"). It is a constant rather than a parameter because the
// buffer allocation, the recentre policy and the rebuild cost are all sized
// from it; a second value would have to be justified against all three.
inline constexpr int32_t kFluidVolumeDimVoxels = 512;

// One packed word. 32 rather than 64 because HLSL's RWBuffer<uint> is a 32-bit
// typed buffer and 64-bit typed loads are not universally available; the
// collision path reads one word per voxel test and must not need SM6-only ops
// (unlike worldgen.ush, which does — see VoxelGpuWorldGen.cpp:64-70).
inline constexpr int32_t kFluidBitsPerWord = 32;

// 16 words per 512-voxel row along x.
inline constexpr int32_t kFluidVolumeWordsPerRow =
    kFluidVolumeDimVoxels / kFluidBitsPerWord;

// 4,194,304 words = 16 MiB exactly.
inline constexpr int64_t kFluidVolumeWords =
    static_cast<int64_t>(kFluidVolumeWordsPerRow) * kFluidVolumeDimVoxels *
    kFluidVolumeDimVoxels;
static_assert(kFluidVolumeWords * 4 == 16 * 1024 * 1024,
              "the plan's 16 MB budget for the occupancy volume");
static_assert(kFluidVolumeDimVoxels % kFluidBitsPerWord == 0,
              "a row must be a whole number of words or the x-major packing "
              "straddles rows and every index below is wrong");

// The source grain. Terrain lives in 8^3 bricks everywhere in this codebase
// (Brick<B> with B=8, VoxelCoords::BrickEdgeVoxels), the dirty-brick
// notifications the incremental rebuild hangs off are per brick, and the
// mesher's own unit of work is a brick. So the region uploader speaks bricks.
inline constexpr int32_t kFluidBrickEdge = 8;
// 8^3 bits = 512 bits = 16 words.
inline constexpr int32_t kFluidBrickWords =
    (kFluidBrickEdge * kFluidBrickEdge * kFluidBrickEdge) / kFluidBitsPerWord;

// One 10 cm voxel expressed in Unreal units (1 UU = 1 cm). This is
// kVoxelSizeMm (core.h:263) in UU and NOT the contract's
// VOXEL_FLUID_REST_SPACING_UU: the two are numerically equal today and mean
// different things (one is the terrain lattice, the other is PBF's rest
// spacing), so they are separate names and neither is defined in terms of the
// other.
inline constexpr float kFluidVoxelUU = static_cast<float>(kVoxelSizeMm) / 10.0f;
static_assert(kVoxelSizeMm == 100, "kFluidVoxelUU's conversion assumes 10 cm voxels");

// ---------------------------------------------------------------------------
// Solidity
// ---------------------------------------------------------------------------

// Does this terrain material stop a water particle?
//
// Terrain bricks store no water at all ("Water is implicit … never stored in
// terrain bricks", core.h:298-299), so every material except air is ground and
// stops a particle. The one exception is the water MARKER: MAT_WATERMARK is a
// debug instrument that puts SOLID voxels exactly where the bake says water
// is (core.h:322-334), so treating it as solid would wall off every river the
// moment -VoxelWaterMarker=1 is passed — i.e. it would break collision only
// in the diagnostic mode somebody turns on to look at water. It is not world
// content and it is not ground.
//
// This predicate is the ONLY place solidity is decided, and it is decided on
// the CPU: the GPU never sees a material id, only the bit this produced. That
// is the whole reason the region uploader packs bits rather than materials —
// one definition, no HLSL mirror of the material enum, and 64 bytes per brick
// on the bus instead of 512.
constexpr bool isSolidForFluid(MaterialId m) {
    return m != MAT_AIR && m != MAT_WATERMARK;
}

// ---------------------------------------------------------------------------
// Brick bit packing
// ---------------------------------------------------------------------------

// Bit index of a voxel inside a brick's 512-bit block. IDENTICAL to
// Brick<8>::cellIndex (brick.h:46) — x + B*(y + B*z) — so a brick's bits are
// in its own cell order and packBrickSolidBits can walk cells linearly.
constexpr int32_t fluidBrickBitIndex(int32_t bx, int32_t by, int32_t bz) {
    return bx + kFluidBrickEdge * (by + kFluidBrickEdge * bz);
}

// Word and shift for that bit. Both derived from the bit index so there is one
// definition of the packing and not three.
constexpr int32_t fluidBrickWordOf(int32_t bitIndex) { return bitIndex / kFluidBitsPerWord; }
constexpr int32_t fluidBrickShiftOf(int32_t bitIndex) { return bitIndex % kFluidBitsPerWord; }

// Packs one brick's solidity into 16 words. MaterialFn: MaterialId(int bx,
// int by, int bz), brick-local 0..7.
//
// Callers pass whatever they have: a vxc::Brick<8>'s get(), World::materialAt
// with the brick origin added, or a flat array. The uploader in
// VoxelFluidOccupancy.cpp uses the SAME call the mesher makes for the same
// brick, which is what "the occupancy source is the mesher's source" means in
// practice.
template <typename MaterialFn>
void packBrickSolidBits(const MaterialFn& materialAt, uint32_t out[kFluidBrickWords]) {
    for (int32_t w = 0; w < kFluidBrickWords; ++w) out[w] = 0u;
    for (int32_t bz = 0; bz < kFluidBrickEdge; ++bz) {
        for (int32_t by = 0; by < kFluidBrickEdge; ++by) {
            for (int32_t bx = 0; bx < kFluidBrickEdge; ++bx) {
                if (!isSolidForFluid(materialAt(bx, by, bz))) continue;
                const int32_t bit = fluidBrickBitIndex(bx, by, bz);
                out[fluidBrickWordOf(bit)] |= 1u << fluidBrickShiftOf(bit);
            }
        }
    }
}

// Reads a bit back out of a packed brick. Round-trip partner of the above; the
// test asserts pack→get is the identity over all 512 cells.
constexpr bool brickSolidBit(const uint32_t* brickWords, int32_t bx, int32_t by, int32_t bz) {
    const int32_t bit = fluidBrickBitIndex(bx, by, bz);
    return ((brickWords[fluidBrickWordOf(bit)] >> fluidBrickShiftOf(bit)) & 1u) != 0u;
}

// ---------------------------------------------------------------------------
// Volume bit indexing — TOROIDAL
// ---------------------------------------------------------------------------
//
// THREE coordinate spaces, and confusing any two of them is the whole class of
// bug this section exists to prevent:
//
//   WORLD    (wx, wy, wz)  planet voxel coordinates, int64, unbounded.
//   LOCAL    (lx, ly, lz)  window coordinates, 0..511: world minus the volume
//                          origin. THIS IS THE GEOMETRIC SPACE. Particle
//                          positions are local UU (contract item 1), the walk's
//                          boundary planes are at local voxel edges, and the
//                          collision projection puts a particle back at a local
//                          plane. Nothing below changes any of that.
//   STORAGE  (sx, sy, sz)  where the bit physically lives in the 16 MiB buffer,
//                          0..511. Local plus a rolling offset, modulo 512.
//
// WHY STORAGE IS NOT LOCAL (contract item 4, ratified 2026-08-09). The volume
// used to be flat: storage == local, so moving the origin by one voxel renamed
// every bit in the buffer and the only correct response was to throw all 16 MiB
// away and refill — 262,144 bricks to pack, against a mesher with ~893
// bricks/s of spare capacity, i.e. a multi-second refill during which unbuilt
// space is solid and the water in it is frozen. That cost is why v0 latched the
// origin once and never moved it.
//
// Toroidal addressing makes the buffer a ROLLING WINDOW. Storage addresses are
// anchored to the world, not to the window, so when the window slides the bits
// that stay in view stay exactly where they are; only the newly exposed slab is
// invalidated. The slab the window left behind is not cleared or copied — it is
// simply reinterpreted, because the entering slab lands on precisely those
// addresses (that is what "modulo" means here).
//
// THE OFFSET, AND WHY IT IS NOT JUST `world % 512`. The obvious anchor is
// storage = world & 511, and it works — but only if the origin's x is a
// multiple of 32, or a 32-voxel output word straddles the wrap seam and the
// builder's whole-word write (which is what lets it run with no atomics and no
// pass ordering; see fluidFillRegion below) becomes a read-modify-write race.
// The origin is the camera's voxel minus 256 and is arbitrary. So the anchor is
// instead the window's ACCUMULATED MOTION:
//
//     storage = (local + wrapOffset) mod 512
//     wrapOffset = (origin - originAtCreation) mod 512
//
// which is the same rolling window, with the alignment property moved onto a
// quantity the host controls: every recentre step is a multiple of
// kFluidRecentreStepVoxels, so wrapOffset is always a multiple of 64 and no
// word ever straddles the seam, whatever the initial origin was. It costs one
// extra int3 uniform — exactly the cost the volume's own note predicted
// (VoxelFluidOccupancy.h, SetOriginVoxel).
//
// The MIRROR is ue-project/Shaders/VoxelFluidCollision.ush (the reader) and
// VoxelFluidOccupancy.usf (the writer), and the in-editor verify gate
// (voxel.Fluid.Occupancy.Verify) byte-compares the GPU volume against
// fluidFillRegion below — so the wrap has to be identical in three places or
// the gate says so.

// 511. The wrap is an AND, not a %, which is why the dimension must be a power
// of two — asserted rather than commented, because a future 384-voxel volume
// would otherwise wrap wrong and silently, in a way that reads as terrain.
inline constexpr int32_t kFluidVolumeWrapMask = kFluidVolumeDimVoxels - 1;
static_assert((kFluidVolumeDimVoxels & kFluidVolumeWrapMask) == 0,
              "toroidal addressing masks instead of dividing: the volume "
              "dimension must be a power of two");

// THE RECENTRE QUANTUM. The origin may only move in whole multiples of this on
// every axis, and three separate requirements land on the number:
//
//   * 32 in x, minimum, or an output word straddles the wrap seam (above).
//   * 8 in y and z, minimum, or an entering slab is not brick-aligned and
//     cannot be packed by the region uploader at all.
//   * 64 makes an entering slab a whole number of the 64^3 cells the host
//     already fills the volume with (UVoxelFluidSubsystem's kFillCellVoxels),
//     so a recentre refill is the SAME unit of work as the initial fill and
//     rides the same per-tick budget. That is what makes the refill cost
//     legible: one 64-voxel step exposes 8x8 = 64 cells out of the 512 the
//     initial fill queues, i.e. 1/8 of the volume, 2 MiB of bits.
//
// (The volume's old cost note guessed "~1/50 of the volume for a one-chunk
// step". That was optimistic about the step size, not about the mechanism: at
// this 64-voxel quantum it is 1/8, and it would be 1/16 at the 32-voxel floor.
// Either way it is a bounded slab instead of all 16 MiB, which is the point.)
inline constexpr int32_t kFluidRecentreStepVoxels = 64;
static_assert(kFluidRecentreStepVoxels % kFluidBitsPerWord == 0,
              "a recentre step must keep the wrap seam on an output-word "
              "boundary, or the builder's whole-word write races");
static_assert(kFluidRecentreStepVoxels % kFluidBrickEdge == 0,
              "a recentre step must keep entering slabs brick-aligned");
static_assert(kFluidVolumeDimVoxels % kFluidRecentreStepVoxels == 0,
              "the window must be a whole number of recentre cells");
// 8 cells per axis, 512 in the volume — the same grid the host's initial fill
// walks centre-out.
inline constexpr int32_t kFluidRecentreCellsPerAxis =
    kFluidVolumeDimVoxels / kFluidRecentreStepVoxels;

// LOCAL -> STORAGE, one axis. `local` is 0..511 and `wrapOffset` is 0..511, so
// the sum is below 1024 and the mask is one conditional subtract; no negative
// operand ever reaches it, which is why this needs no floorMod.
constexpr int32_t fluidVolumeStorageAxis(int32_t local, int32_t wrapOffset) {
    return (local + wrapOffset) & kFluidVolumeWrapMask;
}

// The rolling offset after moving the origin by `delta` voxels on one axis.
// floorMod, not %, because the window slides in both directions and C's % would
// hand back a negative storage coordinate for a westward move.
constexpr int32_t fluidWrapOffsetAfterMove(int32_t wrapOffset, int64_t delta) {
    return static_cast<int32_t>(floorMod(wrapOffset + delta, kFluidVolumeDimVoxels));
}

// A recentre delta the addressing can actually honour: whole recentre steps on
// every axis. Refused rather than rounded — a rounded delta would put the
// window somewhere other than where the caller thinks it is, and every world
// coordinate the caller then packs would be off by the remainder.
inline bool fluidRecentreDeltaIsAligned(const int64_t delta[3]) {
    for (int a = 0; a < 3; ++a) {
        if (delta[a] % kFluidRecentreStepVoxels != 0) return false;
    }
    return true;
}

// The invariant the builder's whole-word write depends on: the seam sits on an
// output-word boundary. True by construction if every move was aligned, and
// therefore worth checking at the point of use — it is the one thing that turns
// a silent torn word into a refused dispatch.
inline bool fluidWrapOffsetIsAligned(const int32_t wrapOffset[3]) {
    for (int a = 0; a < 3; ++a) {
        if (wrapOffset[a] < 0 || wrapOffset[a] >= kFluidVolumeDimVoxels) return false;
        if (wrapOffset[a] % kFluidRecentreStepVoxels != 0) return false;
    }
    return true;
}

// (sx, sy, sz) are STORAGE voxel coordinates, 0..511 — where the bit lives, not
// where it is. Callers that hold LOCAL coordinates want
// fluidVolumeWordIndexLocal below; this is the raw addressing underneath both.
constexpr int64_t fluidVolumeWordIndex(int32_t sx, int32_t sy, int32_t sz) {
    return (static_cast<int64_t>(sz) * kFluidVolumeDimVoxels + sy) * kFluidVolumeWordsPerRow +
           (sx / kFluidBitsPerWord);
}
constexpr uint32_t fluidVolumeBitMask(int32_t sx) {
    return 1u << (sx % kFluidBitsPerWord);
}

// LOCAL (window) coordinates -> the word that holds them. This is the function
// the verify gate must use to compare a GPU word against a CPU word: both sides
// name a voxel by where it is in the WINDOW, and only this maps that to where
// the bit was stored.
constexpr int64_t fluidVolumeWordIndexLocal(const int32_t wrapOffset[3], int32_t lx, int32_t ly,
                                            int32_t lz) {
    return fluidVolumeWordIndex(fluidVolumeStorageAxis(lx, wrapOffset[0]),
                                fluidVolumeStorageAxis(ly, wrapOffset[1]),
                                fluidVolumeStorageAxis(lz, wrapOffset[2]));
}
constexpr uint32_t fluidVolumeBitMaskLocal(const int32_t wrapOffset[3], int32_t lx) {
    return fluidVolumeBitMask(fluidVolumeStorageAxis(lx, wrapOffset[0]));
}

constexpr bool fluidVolumeInBounds(int32_t lx, int32_t ly, int32_t lz) {
    return lx >= 0 && ly >= 0 && lz >= 0 && lx < kFluidVolumeDimVoxels &&
           ly < kFluidVolumeDimVoxels && lz < kFluidVolumeDimVoxels;
}

// STORAGE-space accessors: (sx, sy, sz) name a slot in the buffer, not a place
// in the world. Correct for whole-buffer work (the clear, a raw dump) and for
// callers that already wrapped; anything holding window coordinates wants the
// Local forms below.
inline bool fluidVolumeGetBit(const uint32_t* words, int32_t sx, int32_t sy, int32_t sz) {
    return (words[fluidVolumeWordIndex(sx, sy, sz)] & fluidVolumeBitMask(sx)) != 0u;
}

inline void fluidVolumeSetBit(uint32_t* words, int32_t sx, int32_t sy, int32_t sz, bool solid) {
    const int64_t i = fluidVolumeWordIndex(sx, sy, sz);
    const uint32_t m = fluidVolumeBitMask(sx);
    words[i] = solid ? (words[i] | m) : (words[i] & ~m);
}

// LOCAL-space accessors: (lx, ly, lz) name a place in the window, 0..511. The
// wrap happens here and nowhere else in the caller.
inline bool fluidVolumeGetBitLocal(const uint32_t* words, const int32_t wrapOffset[3], int32_t lx,
                                   int32_t ly, int32_t lz) {
    return fluidVolumeGetBit(words, fluidVolumeStorageAxis(lx, wrapOffset[0]),
                             fluidVolumeStorageAxis(ly, wrapOffset[1]),
                             fluidVolumeStorageAxis(lz, wrapOffset[2]));
}

inline void fluidVolumeSetBitLocal(uint32_t* words, const int32_t wrapOffset[3], int32_t lx,
                                   int32_t ly, int32_t lz, bool solid) {
    fluidVolumeSetBit(words, fluidVolumeStorageAxis(lx, wrapOffset[0]),
                      fluidVolumeStorageAxis(ly, wrapOffset[1]),
                      fluidVolumeStorageAxis(lz, wrapOffset[2]), solid);
}

// THE RULE THE WHOLE COLLISION MODEL RESTS ON: anything outside the built
// volume is SOLID. A particle can never escape through an unbuilt edge, which
// is the same "blocked, never guessed" rule the fine terrain tier uses
// (VoxelFluidContract.ush:70-73). The failure mode of the opposite convention
// is a river that drains into the void the first time the volume lags the
// camera by one frame, and it would look exactly like a solver bug.
//
// originVoxel is the world voxel coordinate of LOCAL (0,0,0) — the window's
// minimum corner. wrapOffset is the STORAGE coordinate of that same corner (all
// zeros for a window that has never moved).
//
// THE WINDOW TEST IS UNCHANGED BY THE WRAP, and that is the safety property:
// the bounds check runs on the LOCAL coordinate, so a voxel outside the window
// is still solid even though its storage address is perfectly valid and holds
// somebody else's terrain. Dropping the check and trusting the modulo would
// make a particle one voxel past the west face collide against the terrain
// 51.2 m to its east — an aliasing bug that reads as terrain, not as a bug.
inline bool fluidSolidAtVoxel(const uint32_t* words, const int32_t originVoxel[3],
                              const int32_t wrapOffset[3], int64_t wx, int64_t wy, int64_t wz) {
    const int64_t lx = wx - originVoxel[0];
    const int64_t ly = wy - originVoxel[1];
    const int64_t lz = wz - originVoxel[2];
    if (lx < 0 || ly < 0 || lz < 0 || lx >= kFluidVolumeDimVoxels ||
        ly >= kFluidVolumeDimVoxels || lz >= kFluidVolumeDimVoxels) {
        return true;
    }
    return fluidVolumeGetBitLocal(words, wrapOffset, static_cast<int32_t>(lx),
                                  static_cast<int32_t>(ly), static_cast<int32_t>(lz));
}

// The window that has never moved. Named rather than a brace-initialised
// literal at every call site, so "this code assumes a flat volume" is greppable.
inline constexpr int32_t kFluidWrapOffsetNone[3] = {0, 0, 0};

// An unbuilt volume is ALL SOLID for the same reason as outside-is-solid above,
// and this is what the builder's clear pass writes. A volume that has just been
// created freezes the particles inside it until the regions land, which is a
// visible stall; the alternative is particles falling through terrain that
// simply has not arrived yet, which is a leak that looks like physics. It is
// also the value fluidMarkRegionUnbuilt writes over an entering slab, which is
// how a recentre pays the same stall over 1/8 of the volume instead of all of
// it.
inline constexpr uint32_t kFluidVolumeUnbuiltWord = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// Region fill: the brick→volume transpose
// ---------------------------------------------------------------------------
//
// This is the mirror of FluidOccupancyFillMain. A region is a box of bricks
// whose packed bits the host uploaded contiguously; the transpose turns
// brick-local bit order into the volume's x-major order.
//
// ALIGNMENT, AND WHY IT IS A REQUIREMENT RATHER THAN A CLAMP. One output word
// spans 32 voxels along x, which is FOUR bricks. If a region began or ended
// part-way through a word the kernel would have to read-modify-write it, and
// two regions dispatched in the same graph could then race on the shared word.
// Requiring 32-voxel alignment along x makes every write a whole word owned by
// exactly one thread, so the builder needs no atomics and no pass ordering
// between regions. Snapping outward costs at most 3 extra brick columns per
// region and the host can always produce them (World::brickAt generates any
// brick on demand).
struct FluidRegion {
    // Volume-local voxel coordinates of the region's minimum corner.
    int32_t minVoxel[3] = {0, 0, 0};
    // Extent in voxels.
    int32_t sizeVoxels[3] = {0, 0, 0};
};

// x must be 32-aligned; y and z must be brick-aligned (8).
inline constexpr int32_t kFluidRegionAlignX = kFluidBitsPerWord;   // 32
inline constexpr int32_t kFluidRegionAlignYZ = kFluidBrickEdge;    // 8
// Four bricks per output word along x.
inline constexpr int32_t kFluidBricksPerWordX = kFluidRegionAlignX / kFluidBrickEdge;

inline bool fluidRegionIsAligned(const FluidRegion& r) {
    if (r.minVoxel[0] % kFluidRegionAlignX != 0) return false;
    if (r.sizeVoxels[0] % kFluidRegionAlignX != 0) return false;
    for (int a = 1; a < 3; ++a) {
        if (r.minVoxel[a] % kFluidRegionAlignYZ != 0) return false;
        if (r.sizeVoxels[a] % kFluidRegionAlignYZ != 0) return false;
    }
    return true;
}

inline bool fluidRegionInBounds(const FluidRegion& r) {
    for (int a = 0; a < 3; ++a) {
        if (r.minVoxel[a] < 0 || r.sizeVoxels[a] <= 0) return false;
        if (r.minVoxel[a] + r.sizeVoxels[a] > kFluidVolumeDimVoxels) return false;
    }
    return true;
}

// Snaps an arbitrary volume-local voxel box OUTWARD onto the update grid and
// clips it to the volume. minVoxelIn/maxVoxelIn are INCLUSIVE, matching the
// dirty-brick notifications this is fed from (a brick key names the brick, not
// the gap after it). Returns false when the box does not intersect the volume
// at all, so a caller cannot mistake an empty result for a zero-sized update
// it should still dispatch.
//
// Outward, never inward: a region that snapped inward would leave a strip of
// the dirty box holding pre-edit bits, and that strip is invisible until water
// walks through the wall somebody just dug.
inline bool fluidSnapRegion(const int32_t minVoxelIn[3], const int32_t maxVoxelIn[3],
                            FluidRegion& out) {
    for (int a = 0; a < 3; ++a) {
        const int32_t align = (a == 0) ? kFluidRegionAlignX : kFluidRegionAlignYZ;
        int64_t lo = floorDiv(minVoxelIn[a], align) * align;
        int64_t hi = (floorDiv(maxVoxelIn[a], align) + 1) * align; // exclusive
        if (lo < 0) lo = 0;
        if (hi > kFluidVolumeDimVoxels) hi = kFluidVolumeDimVoxels;
        if (hi <= lo) return false;
        out.minVoxel[a] = static_cast<int32_t>(lo);
        out.sizeVoxels[a] = static_cast<int32_t>(hi - lo);
    }
    return true;
}

// Bricks along each axis, and the total word count the host must upload.
inline constexpr int32_t fluidRegionBricks(int32_t sizeVoxels) {
    return sizeVoxels / kFluidBrickEdge;
}
inline int64_t fluidRegionBrickWordCount(const FluidRegion& r) {
    return static_cast<int64_t>(fluidRegionBricks(r.sizeVoxels[0])) *
           fluidRegionBricks(r.sizeVoxels[1]) * fluidRegionBricks(r.sizeVoxels[2]) *
           kFluidBrickWords;
}

// Index of a brick's first word in the uploaded region buffer. x fastest, then
// y, then z — the same ordering GpuColumnSample dispatches use and the same
// ordering Brick<B>::cellIndex uses one level down, so there is one convention
// in the file rather than two.
constexpr int64_t fluidRegionBrickWordBase(int32_t ibx, int32_t iby, int32_t ibz,
                                           int32_t bricksX, int32_t bricksY) {
    return (static_cast<int64_t>(ibx) +
            static_cast<int64_t>(bricksX) * (iby + static_cast<int64_t>(bricksY) * ibz)) *
           kFluidBrickWords;
}

// ONE OUTPUT WORD, gathered from the four bricks that share it. This is the
// exact body of FluidOccupancyFillMain's inner loop and the reason the fill is
// a compute pass at all: the host has bricks, the volume wants x-major words,
// and nobody should be doing that transpose with a scatter of 64-byte
// lock/memcpy uploads on the render thread (the cost pattern D1 spent a wave
// removing — see VoxelQuadPoolWrite.usf:28-40).
//
// (wordX, ry, rz) are region-relative: wordX indexes 32-voxel columns.
inline uint32_t fluidRegionWordFromBricks(const uint32_t* regionBrickWords, int32_t bricksX,
                                          int32_t bricksY, int32_t wordX, int32_t ry,
                                          int32_t rz) {
    const int32_t iby = ry / kFluidBrickEdge, by = ry % kFluidBrickEdge;
    const int32_t ibz = rz / kFluidBrickEdge, bz = rz % kFluidBrickEdge;

    // Bit index of (bx=0, by, bz) inside a brick. A brick row along x is 8
    // consecutive bits by construction (fluidBrickBitIndex has bx as the
    // fastest term), so one shifted byte IS the row — no per-voxel loop.
    const int32_t rowBit = fluidBrickBitIndex(0, by, bz);
    const int32_t rowWord = fluidBrickWordOf(rowBit);
    const int32_t rowShift = fluidBrickShiftOf(rowBit);

    uint32_t out = 0u;
    for (int32_t k = 0; k < kFluidBricksPerWordX; ++k) {
        const int32_t ibx = wordX * kFluidBricksPerWordX + k;
        const int64_t base = fluidRegionBrickWordBase(ibx, iby, ibz, bricksX, bricksY);
        const uint32_t row = (regionBrickWords[base + rowWord] >> rowShift) & 0xFFu;
        out |= row << (k * kFluidBrickEdge);
    }
    return out;
}

// The whole region, word by word. `words` is the full kFluidVolumeWords volume;
// the region is in LOCAL (window) coordinates and `wrapOffset` maps it to
// storage. Mirror of FluidOccupancyFillMain.
//
// WHY A WHOLE-WORD WRITE SURVIVES THE WRAP. One output word is 32 voxels of x
// owned by one thread. The wrap could in principle split that word across the
// seam — and would, for an arbitrary offset. It cannot here: wrapOffset is
// always a multiple of kFluidRecentreStepVoxels (64), the region's x is
// 32-aligned by fluidRegionIsAligned, so a local word column maps to exactly
// one storage word column. Both halves of that are checkable —
// fluidWrapOffsetIsAligned and fluidRegionIsAligned — and the host checks them
// before every dispatch, because the failure they prevent is a torn word at the
// seam: a 32-voxel stripe of wrong terrain, once per recentre, somewhere in the
// volume — findable only by the verify gate and only by luck.
inline void fluidFillRegion(uint32_t* words, const int32_t wrapOffset[3], const FluidRegion& r,
                            const uint32_t* regionBrickWords) {
    const int32_t bricksX = fluidRegionBricks(r.sizeVoxels[0]);
    const int32_t bricksY = fluidRegionBricks(r.sizeVoxels[1]);
    const int32_t wordsX = r.sizeVoxels[0] / kFluidBitsPerWord;
    for (int32_t rz = 0; rz < r.sizeVoxels[2]; ++rz) {
        for (int32_t ry = 0; ry < r.sizeVoxels[1]; ++ry) {
            for (int32_t wx = 0; wx < wordsX; ++wx) {
                const int32_t lx = r.minVoxel[0] + wx * kFluidBitsPerWord;
                const int32_t ly = r.minVoxel[1] + ry;
                const int32_t lz = r.minVoxel[2] + rz;
                words[fluidVolumeWordIndexLocal(wrapOffset, lx, ly, lz)] =
                    fluidRegionWordFromBricks(regionBrickWords, bricksX, bricksY, wx, ry, rz);
            }
        }
    }
}

// Marks a region UNBUILT (all solid) without an upload. Mirror of
// FluidOccupancyMarkUnbuiltMain.
//
// WHY THIS EXISTS AND THE WHOLE-VOLUME CLEAR STILL DOES NOT. The builder
// deliberately has no "store a constant" kernel for the whole volume, because
// RDG already has a clear and a second mechanism is a second place for the
// value to be wrong (VoxelFluidOccupancy.usf, "THE CLEAR IS NOT HERE"). That
// argument does not reach a SUB-BOX: there is no ranged buffer clear, and the
// alternative is uploading 64 bytes per brick of nothing but ones — 2 MiB on
// the bus per recentre to say "I don't know yet".
//
// It is not optional. When the window slides, the entering slab's storage words
// still hold the terrain from the slab that just left, 51.2 m away and
// perfectly plausible. Leaving them would not freeze water (the safe failure);
// it would collide water against terrain from somewhere else, which is the
// failure that reads as worldgen.
inline void fluidMarkRegionUnbuilt(uint32_t* words, const int32_t wrapOffset[3],
                                   const FluidRegion& r) {
    const int32_t wordsX = r.sizeVoxels[0] / kFluidBitsPerWord;
    for (int32_t rz = 0; rz < r.sizeVoxels[2]; ++rz) {
        for (int32_t ry = 0; ry < r.sizeVoxels[1]; ++ry) {
            for (int32_t wx = 0; wx < wordsX; ++wx) {
                const int32_t lx = r.minVoxel[0] + wx * kFluidBitsPerWord;
                const int32_t ly = r.minVoxel[1] + ry;
                const int32_t lz = r.minVoxel[2] + rz;
                words[fluidVolumeWordIndexLocal(wrapOffset, lx, ly, lz)] = kFluidVolumeUnbuiltWord;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Recentring: which cells enter the window
// ---------------------------------------------------------------------------
//
// The window is a grid of kFluidRecentreCellsPerAxis^3 cells of
// kFluidRecentreStepVoxels^3 voxels (8^3 cells of 64^3 voxels — the same cells
// the host's initial fill queues centre-out). After a move, a cell of the NEW
// window either (a) was wholly inside the OLD window, in which case its bits
// are already correct and already at the right storage address and there is
// nothing to do, or (b) was not, in which case it must be marked unbuilt and
// refilled.
//
// ENUMERATED BY CONTAINMENT, NOT BY SLAB ARITHMETIC. The obvious
// implementation is "the entering slab is delta voxels thick on the moved
// axis"; it is also wrong for a diagonal move, where up to three slabs overlap
// and the overlap gets marked (and refilled) two or three times. Testing each
// cell costs 512 comparisons once per recentre and cannot double-count, and it
// degenerates correctly with no special case when the move is larger than the
// window (every cell enters — the full rebuild, reached rather than branched
// to).
inline bool fluidRecentreCellIsResident(const int64_t delta[3], const int32_t cell[3]) {
    for (int a = 0; a < 3; ++a) {
        // The cell's local box under the NEW origin, expressed under the OLD
        // one: local_old = local_new + delta.
        const int64_t lo = static_cast<int64_t>(cell[a]) * kFluidRecentreStepVoxels + delta[a];
        if (lo < 0 || lo + kFluidRecentreStepVoxels > kFluidVolumeDimVoxels) return false;
    }
    return true;
}

// Calls fn(const FluidRegion&) for every cell of the new window that must be
// invalidated and refilled, in NEW-origin LOCAL coordinates. Returns the count,
// so a caller can tell "nothing entered" from "the enumeration did not run" —
// the standing ran-flag rule.
template <typename CellFn>
int32_t fluidForEachEnteringCell(const int64_t delta[3], const CellFn& fn) {
    int32_t count = 0;
    for (int32_t cz = 0; cz < kFluidRecentreCellsPerAxis; ++cz) {
        for (int32_t cy = 0; cy < kFluidRecentreCellsPerAxis; ++cy) {
            for (int32_t cx = 0; cx < kFluidRecentreCellsPerAxis; ++cx) {
                const int32_t cell[3] = {cx, cy, cz};
                if (fluidRecentreCellIsResident(delta, cell)) continue;
                FluidRegion r;
                for (int a = 0; a < 3; ++a) {
                    r.minVoxel[a] = cell[a] * kFluidRecentreStepVoxels;
                    r.sizeVoxels[a] = kFluidRecentreStepVoxels;
                }
                fn(r);
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Recentring: rebasing the particles
// ---------------------------------------------------------------------------
//
// THE DECISION, RATIFIED AS CONTRACT ITEM 8. Particle positions stay
// ORIGIN-RELATIVE and are rebased by one kernel when the origin moves; they do
// not become world-anchored.
//
// The alternative was tempting because it makes recentring free for the solver:
// leave positions in world UU and let every pass subtract the origin. It is not
// available. The world spans tens of km and a float32 ULP at 30 km is 2 mm —
// two orders of magnitude coarser than the 0.01 UU collision skin, and the PBF
// density estimate differences positions at 10 UU spacing, where 2 mm of
// quantisation is 0.02 % noise injected into every neighbour term. That is the
// exact reason the contract put positions in [0, 5120] in the first place
// (VoxelFluidContract.ush:20-25), and a recentre must not undo it.
//
// So: subtract the delta from every stored position, once, at the move.
//
// HOW MUCH ERROR THAT COSTS, EXACTLY. The delta is a whole number of voxels, so
// deltaUU is an integer multiple of 10 and is itself exact well past any
// plausible window motion (below). The subtraction p - deltaUU is then EXACT
// whenever |p - deltaUU| <= |p|, i.e. whenever the particle moves toward the
// new origin: deltaUU is a multiple of 640 (the recentre quantum in UU), every
// float in [0, 5120] is a multiple of a power of two no coarser than 2^-11,
// and the difference is therefore a multiple of the smaller operand's ULP and
// representable. Moving away from the origin can round, by at most half a ULP
// at 5120 UU = 2^-12 UU = 2.4 um — one fortieth of the collision skin and one
// forty-thousandth of a voxel. Pinned by test rather than asserted here.
inline constexpr float fluidRebaseDeltaUU(int32_t deltaVoxels) {
    return static_cast<float>(deltaVoxels) * kFluidVoxelUU;
}

// Whole voxels times 10 UU stops being exactly representable in float32 above
// 2^24 UU, i.e. 1,677,721 voxels = 167.7 km of window motion IN ONE STEP. A
// jump that far is a teleport, not a recentre, and the host takes the
// full-rebuild path for anything past the window anyway — but the bound is
// named and tested so "float is fine here" is a checked claim and not a habit.
inline constexpr int64_t kFluidRebaseExactMaxVoxels = (1LL << 24) / 10;
inline constexpr bool fluidRebaseIsExactlyRepresentable(int64_t deltaVoxels) {
    return deltaVoxels >= -kFluidRebaseExactMaxVoxels && deltaVoxels <= kFluidRebaseExactMaxVoxels;
}

// ---------------------------------------------------------------------------
// Collision: the voxel-line walk and the face projection
// ---------------------------------------------------------------------------

// How many voxel boundaries one resolve may cross. THE ANTI-TUNNELLING BUDGET,
// and the number the Phase 0 spike exists to justify.
//
//   Motion per substep at speed v is v*dt UU. A walk crosses one boundary per
//   step, and a diagonal segment crosses up to sqrt(3) times as many
//   boundaries as an axis-aligned one of the same length, so the budget covers
//
//       v_max = kFluidMaxCollisionSteps * kFluidVoxelUU / (dt * sqrt(3))
//
//   at worst case, and sqrt(3) times more along an axis.
//
//   Waterfall requirement (the plan's own worst case): a 40 m fall reaches
//   v = sqrt(2*g*h) = sqrt(2*9.81*40) = 28.0 m/s = 2800 UU/s.
//
//       steps=8,  dt=1/60  →  2771 UU/s  — FAILS the 40 m waterfall
//       steps=16, dt=1/60  →  5543 UU/s  = 55.4 m/s ≡ a 157 m fall
//       steps=16, dt=1/120 → 11086 UU/s  = 111 m/s  ≡ a 627 m fall
//       steps=16, dt=1/240 → 22170 UU/s  = 222 m/s  ≡ terminal velocity ×3.5
//
//   8 is the obvious budget and it misses by 1 %, at the slow end of the
//   plan's substep range, on the exact case the plan names. 16 clears it by
//   1.98× at the worst substep rate — a 157 m fall — and is still bounded
//   work: the loop exits at the first solid voxel or when the segment is
//   exhausted, so a particle drifting at 1 m/s costs ONE iteration, not
//   sixteen.
//
//   RAISING IT IS ALMOST FREE if a taller drop ever needs covering. The loop
//   is not unrolled and typical particles exit on the first step, so the only
//   cost is worst-case latency for particles that are already over budget: 24
//   steps covers 83 m/s (a 352 m fall), 32 covers 111 m/s (a 627 m fall).
//   16 is chosen because the plan's stated requirement is 40 m and going over
//   budget clamps rather than leaks (see below), not because 16 is a limit.
inline constexpr int32_t kFluidMaxCollisionSteps = 16;

// AND THE BUDGET IS NOT A LEAK. When the walk runs out of steps with segment
// left, the particle is clamped to the last boundary it verified rather than
// let through: above the supported speed a particle is SLOWED, never passed
// through a wall. So the number above is a speed limit, not a tunnelling
// threshold, and the failure mode of getting it wrong is visibly sluggish
// water instead of invisibly leaking water.

// One resolve pass clamps one axis. A particle entering a corner needs up to
// three, and cannot need a fourth — there are three axes and each is clamped
// at most once (the loop tracks which, and stops if an axis repeats).
inline constexpr int32_t kFluidMaxResolveIterations = 3;

// How far outside the face the projected position is placed.
//
// 0.01 UU = 0.1 mm. Sized against float precision, not against physics: the
// volume is 5120 UU across so origin-relative positions reach ~5120, where a
// float32 ULP is ~4.9e-4 UU. The skin is ~20 ULP — large enough that
// re-flooring the corrected position lands in the free voxel and not back on
// the boundary, small enough to be invisible at 10 cm voxels (1/1000 of one).
inline constexpr float kFluidCollisionSkinUU = 0.01f;

// Stands in for "this axis never crosses a boundary". Any value larger than 1
// works (the walk clamps at t>1); 1e30 is far outside the segment parameter's
// range and finite, so it survives arithmetic that an inf would poison.
inline constexpr float kFluidNoCrossingT = 1e30f;

// Worst-case (diagonal) speed the budget supports at a given substep. Exposed
// so the test asserts the waterfall case rather than trusting the comment.
inline float fluidMaxTraversalSpeedUU(float dtSeconds) {
    return static_cast<float>(kFluidMaxCollisionSteps) * kFluidVoxelUU /
           (dtSeconds * 1.7320508f);
}

struct FluidWalkHit {
    bool hit = false;
    // The first solid voxel entered, in WORLD voxel coordinates.
    int64_t vx = 0, vy = 0, vz = 0;
    // raycast.h's conventions exactly: axis 0/1/2, sign +1 when the walk
    // entered moving in +axis (through the voxel's −axis face), and
    // faceAxis == -1 when the segment started inside solid.
    int32_t faceAxis = -1;
    int32_t faceSign = 0;
    // Segment parameter in [0,1] at which the face was crossed.
    float tEnter = 0.0f;
    // The segment started inside a solid voxel. hit is true and faceAxis is -1.
    bool startedInside = false;
    // The step budget ran out with segment remaining. tEnter holds the last
    // boundary the walk verified; the caller clamps there.
    bool budgetExhausted = false;
};

// Float Amanatides & Woo from prevPosUU to posUU, both in UU relative to the
// volume origin's own corner (see VoxelFluidCollision.ush for why the fluid
// origin and the volume origin are required to be the same point).
//
// SolidFn: bool(int64_t vx, int64_t vy, int64_t vz) over WORLD voxels, so the
// caller supplies the origin offset — same shape as raycast.h's MaterialFn.
template <typename SolidFn>
FluidWalkHit fluidWalkVoxelLine(const SolidFn& solidAt, const int32_t originVoxel[3],
                                const float prevPosUU[3], const float posUU[3]) {
    FluidWalkHit out;

    int64_t v[3];
    for (int a = 0; a < 3; ++a) {
        v[a] = static_cast<int64_t>(std::floor(prevPosUU[a] / kFluidVoxelUU)) + originVoxel[a];
    }

    if (solidAt(v[0], v[1], v[2])) {
        // raycast.h:42-48's case: no face was crossed, so no projection
        // direction can be derived. Reported, never guessed.
        out.hit = true;
        out.startedInside = true;
        out.vx = v[0];
        out.vy = v[1];
        out.vz = v[2];
        return out;
    }

    float d[3], tMax[3], tDelta[3];
    int32_t step[3];
    for (int a = 0; a < 3; ++a) {
        d[a] = posUU[a] - prevPosUU[a];
        // Boundary planes are at world voxel edges; prevPosUU is volume-local
        // UU, so the local voxel index is v[a] - originVoxel[a].
        const float localV = static_cast<float>(v[a] - originVoxel[a]);
        if (d[a] > 0.0f) {
            step[a] = 1;
            tMax[a] = ((localV + 1.0f) * kFluidVoxelUU - prevPosUU[a]) / d[a];
            tDelta[a] = kFluidVoxelUU / d[a];
        } else if (d[a] < 0.0f) {
            step[a] = -1;
            tMax[a] = (localV * kFluidVoxelUU - prevPosUU[a]) / d[a];
            tDelta[a] = -kFluidVoxelUU / d[a];
        } else {
            step[a] = 0;
            tMax[a] = kFluidNoCrossingT;
            tDelta[a] = kFluidNoCrossingT;
        }
    }

    float tLast = 0.0f;
    for (int32_t i = 0; i < kFluidMaxCollisionSteps; ++i) {
        // Ties step ONE axis at a time, lowest axis first — raycast.h:74-80.
        int axis;
        if (tMax[0] <= tMax[1] && tMax[0] <= tMax[2]) axis = 0;
        else if (tMax[1] <= tMax[2]) axis = 1;
        else axis = 2;

        if (tMax[axis] > 1.0f) return out;  // segment exhausted, no hit

        tLast = tMax[axis];
        v[axis] += step[axis];
        tMax[axis] += tDelta[axis];

        if (solidAt(v[0], v[1], v[2])) {
            out.hit = true;
            out.vx = v[0];
            out.vy = v[1];
            out.vz = v[2];
            out.faceAxis = axis;
            out.faceSign = step[axis];
            out.tEnter = tLast;
            return out;
        }
    }

    // Budget spent with segment left. The last voxel the walk entered was
    // tested and is empty, so tLast is a verified-safe place to stop.
    out.budgetExhausted = true;
    out.tEnter = tLast;
    return out;
}

struct FluidCollisionResult {
    float posUU[3] = {0.0f, 0.0f, 0.0f};
    // Outward normal of the last blocking face: one axis ±1, zero when nothing
    // blocked. The PBF solver zeroes the velocity component along this.
    float normal[3] = {0.0f, 0.0f, 0.0f};
    bool blocked = false;
    bool startedInside = false;
    bool speedClamped = false;
    int32_t faceAxis = -1;
    int32_t faceSign = 0;
    // How many resolve passes ran. 0 means the motion was free.
    int32_t iterations = 0;
};

// Walks the voxel line and projects a penetrating particle out along the face
// normal of the voxel it entered.
//
// WHY IT ITERATES. One pass clamps one axis, and the clamped position can
// still lie inside a different solid voxel — a particle driven into a corner
// has to be pushed out of two or three faces. Each pass clamps a NEW axis (the
// mask refuses a repeat) so the loop is bounded by three and terminates by
// construction, not by a tolerance.
//
// KILLING THE NORMAL VELOCITY. PBF derives velocity as (x - x_prev)/dt, so the
// caller gets a normal-free velocity for free IF it also lifts x_prev's normal
// component to the projected plane. `normal` is returned for exactly that; see
// VoxelFluidCollision.ush's VoxelFluidResolveCollisionEx, which is the entry
// point the solver should use. The contract's bare VoxelFluidResolveCollision
// returns only the position, and leaves a residual approach velocity of
// (penetration depth)/dt along the normal.
template <typename SolidFn>
FluidCollisionResult fluidResolveCollision(const SolidFn& solidAt, const int32_t originVoxel[3],
                                           const float posUU[3], const float prevPosUU[3]) {
    FluidCollisionResult out;
    for (int a = 0; a < 3; ++a) out.posUU[a] = posUU[a];

    uint32_t clampedAxes = 0u;
    for (int32_t iter = 0; iter < kFluidMaxResolveIterations; ++iter) {
        const FluidWalkHit h = fluidWalkVoxelLine(solidAt, originVoxel, prevPosUU, out.posUU);
        out.iterations = iter + 1;

        if (h.startedInside) {
            // Nowhere sound to project to. Hold position: the particle stops
            // where it was rather than being teleported somewhere plausible.
            for (int a = 0; a < 3; ++a) out.posUU[a] = prevPosUU[a];
            out.blocked = true;
            out.startedInside = true;
            out.faceAxis = -1;
            return out;
        }

        if (h.budgetExhausted) {
            for (int a = 0; a < 3; ++a) {
                out.posUU[a] = prevPosUU[a] + (out.posUU[a] - prevPosUU[a]) * h.tEnter;
            }
            out.speedClamped = true;
            return out;
        }

        if (!h.hit) {
            out.iterations = iter;  // this pass found nothing; motion is free
            return out;
        }

        const int32_t axis = h.faceAxis;
        if ((clampedAxes >> static_cast<uint32_t>(axis) & 1u) != 0u) {
            // The same axis blocked twice. Cannot happen with the projection
            // below (the clamp puts the position in the free voxel along that
            // axis and the skin keeps it there), so reaching here means the
            // volume changed under us mid-resolve or the skin is too small.
            // Hold at prev rather than loop.
            for (int a = 0; a < 3; ++a) out.posUU[a] = prevPosUU[a];
            out.blocked = true;
            return out;
        }
        clampedAxes |= 1u << static_cast<uint32_t>(axis);

        // The entered voxel's blocking face, in volume-local UU. faceSign +1
        // means the walk came in through the voxel's LOW face, so the free
        // side is below it.
        const float localV = static_cast<float>(h.faceAxis == 0   ? h.vx - originVoxel[0]
                                                : h.faceAxis == 1 ? h.vy - originVoxel[1]
                                                                  : h.vz - originVoxel[2]);
        if (h.faceSign > 0) {
            out.posUU[axis] = localV * kFluidVoxelUU - kFluidCollisionSkinUU;
            out.normal[axis] = -1.0f;
        } else {
            out.posUU[axis] = (localV + 1.0f) * kFluidVoxelUU + kFluidCollisionSkinUU;
            out.normal[axis] = 1.0f;
        }
        out.blocked = true;
        out.faceAxis = axis;
        out.faceSign = h.faceSign;
    }
    return out;
}

} // namespace vxc
