#pragma once
// VXA1 -- the baked environmental-asset container, read side.
//
// asset-forge (D:\voxelsim\asset-forge, forge/vxa.py) bakes one
// (species, seed) pair into a bounding box of voxels: a run-length body over
// the box in C order [x][y][z], so z varies fastest. That file is what ships;
// this is the engine's reader for it.
//
// WHAT THIS IS FOR, and it is not "load a model". The streaming layer cannot
// hold an asset as an object: a render chunk is 32 voxels (3.2 m) on a side
// and the largest baked asset is a 28 m jungle emergent, so any asset worth
// having spans up to ~9 chunks per axis and up to ~700 chunks in total. There
// is no design in which one chunk owns one asset. What every chunk CAN do is
// ask "what material is at this one voxel", independently of every other
// chunk, and get an answer that costs nothing to produce twice. That question
// is this file's entire API (`at`), and answering it in O(runs in one column)
// rather than O(runs in the file) is why the column index below exists.
//
// So the container is deliberately random-access, not streaming: an asset is
// decoded ONCE into a process-lifetime library keyed by (species, seed), and
// the chunk mesher reads through it. Residency is per SPECIES BANK, which is
// small and bounded (59 species x 64 seeds), never per instance and never per
// chunk -- an instance is a position, and positions are not data.
//
// DENSE DECODE WAS THE FIRST CUT AND IT DOES NOT FIT. A 28 m asset at 10 cm is
// 280^3 = 22 M cells; at one byte each that is 22 MB for ONE seed of ONE
// species, ~1.4 GB for a 64-seed bank, and the bank is the thing that has to
// stay resident. The runs are already the compact form (asset-forge measures a
// typical broadleaf at 3-5% of its dense size), so the reader keeps them and
// pays a short forward walk instead. The column index bounds that walk to the
// runs of a single z-column, which is what makes the walk short rather than
// merely finite.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "voxelcore/core.h"

namespace vxc {

// Header layout, fixed by forge/vxa.py's docstring and its struct format
// strings. Kept as named constants rather than literals sprinkled through the
// parser because the two sides are a wire contract: the Python writer packs
// "<I" + "<iii" + "<III" + "<I" + "<I" after a 4-byte magic, and the record
// body is a PACKED numpy structured dtype (u1 then u4, itemsize 5 -- numpy
// does not pad structured dtypes unless align=True is passed, and vxa.py does
// not pass it). A reader that assumed natural alignment would read 8-byte
// records and silently produce garbage from the second run onward.
//
// VERSION 2 ADDED THE VOXEL SIZE, and it is the only difference. Version 1
// recorded a box of voxels and never said how big a voxel was, which was
// survivable while every asset was on one lattice and stopped being survivable
// the moment they were not. The library now spans four: 1 cm birds and small
// fish, 2 cm tuna, 5 cm ground cover, 10 cm trees, rocks and large whales.
//
// This reader could not tell them apart and did not know it had a problem:
// `at()` takes plain integer local coordinates, so an asset is placed one
// asset voxel per terrain voxel whatever it was baked at. A 5 cm boulder read
// that way is not an error, it is a boulder at twice its size -- and there are
// no boulder-shaped diagnostics.
//
// v1 IS REFUSED rather than assumed to be terrain lattice. The two v1 files
// that existed when this changed (`granite-boulder`, `tundra-pine`) were both
// baked at 5 cm, so the tempting assumption is wrong for the entire actual
// corpus. Re-baking is seconds; a silently wrong scale is forever.
inline constexpr uint32_t kVxaMagic = 0x31415856u; // "VXA1", little-endian
inline constexpr uint32_t kVxaVersion = 2u;
inline constexpr size_t kVxaHeaderBytes = 40u;
inline constexpr size_t kVxaRunBytes = 5u; // uint8 material + uint32 count, packed

// Why an asset was rejected. A named reason rather than a bool because the
// only thing worse than refusing to load an asset is refusing to say which
// asset and why -- the bake step is a separate program in a separate language,
// and "the tree did not appear" is not a diagnosis.
enum class AssetParseError : uint8_t {
    kOk = 0,
    kTooSmall,        // fewer bytes than a header
    kBadMagic,        // not a VXA file at all
    kBadVersion,      // a VXA file this build does not understand
    kBadDimensions,   // zero, negative, or overflowing extent
    kTruncatedBody,   // header promises more runs than the blob carries
    kRunLengthSum,    // runs do not tile the box exactly
    kBadVoxelSize,    // voxel edge of zero, or one the lattice cannot hold
};

const char* assetParseErrorText(AssetParseError e);

// One baked (species, seed): a box of voxels plus where its base sits.
//
// `origin` is asset-forge's own offset from the asset's base, which it places
// at (0,0,0) with +z up. So a voxel at local (lx,ly,lz) sits at world voxel
// (anchorX + origin.x + lx, anchorY + origin.y + ly, anchorZ + origin.z + lz)
// where anchorZ is the ground voxel the instance is rooted on. Trees have a
// negative origin.z when they carry roots, and rocks have one when they are
// meant to sit buried; neither is a special case here, the offset is just
// carried.
class AssetGrid {
public:
    AssetGrid() = default;

    // Parses `blob` in place. Returns kOk and fills this grid, or a reason and
    // leaves the grid empty. Never throws, never partially fills: a grid that
    // failed to parse reads as all-air rather than as half an asset, because a
    // half-decoded tree is the failure mode that looks like a shape bug.
    AssetParseError parse(const uint8_t* blob, size_t bytes);
    AssetParseError parse(const std::vector<uint8_t>& blob) {
        return parse(blob.data(), blob.size());
    }

    bool valid() const { return sizeX_ > 0 && sizeY_ > 0 && sizeZ_ > 0; }

    int32_t sizeX() const { return sizeX_; }
    int32_t sizeY() const { return sizeY_; }
    int32_t sizeZ() const { return sizeZ_; }
    int32_t originX() const { return originX_; }
    int32_t originY() const { return originY_; }
    int32_t originZ() const { return originZ_; }

    // The edge of ONE VOXEL OF THIS ASSET, in millimetres, as baked.
    //
    // Read it before placing anything. It is `kVoxelSizeMm` for a terrain
    // asset -- rocks and trees, which join the world grid and are destructible
    // as terrain is -- and it is anything the species chose for a detail
    // entity, which carries its own grid and its own transform and never
    // enters the world lattice at all. `at()` deliberately does NOT scale by
    // it: local coordinates are indices into the baked box, and what a caller
    // does with a box of 10 mm voxels is a placement decision, not a decode
    // one.
    uint32_t voxelSizeMm() const { return voxelSizeMm_; }

    // True when this asset is on the world's own lattice and so can be stamped
    // into terrain. A detail entity answers false and must be placed as its
    // own object.
    bool onTerrainLattice() const { return voxelSizeMm_ == uint32_t(kVoxelSizeMm); }

    // Number of run records, exposed for tests and for the library's byte
    // accounting (an LRU that charges decoded size needs a decoded size).
    size_t runCount() const { return runMat_.size(); }
    size_t footprintBytes() const;

    // Material at LOCAL coordinates, i.e. indices into the baked box.
    // Out-of-range reads answer MAT_AIR rather than asserting: the mesher's
    // 1-voxel apron reads one cell outside every brick it meshes, so a brick
    // flush with the asset's bounding box WILL read outside it, on every axis,
    // on every load. That is not a caller error, it is the apron doing its job,
    // and answering it with air is what makes the asset's outer faces get
    // emitted instead of culled against a phantom neighbour.
    MaterialId at(int32_t lx, int32_t ly, int32_t lz) const;

    // The same query in the 4 free cubic-lattice orientations. Yaw is
    // quarter-turns about +z, 0..3.
    //
    // WHY ONLY FOUR. A cubic voxel lattice has no cheap arbitrary rotation:
    // anything that is not a multiple of 90 degrees has to RESAMPLE, which
    // means a different voxelization per instance, which means the baked file
    // stops being the thing that ships and the whole (species, seed)
    // determinism argument goes with it. Four orientations against a 64-seed
    // bank is 256 distinct appearances per species, which is past the point
    // repetition reads; an arbitrary yaw would buy a fifth significant figure
    // of variety for a resampler on the streaming path. If a stand of trees
    // ever looks gridded, the fix is more seeds, not more angles.
    //
    // `w`/`h` are the rotated box extents -- see rotatedSizeX/Y.
    MaterialId atYaw(int32_t rx, int32_t ry, int32_t rz, uint8_t yawQuarter) const;

    int32_t rotatedSizeX(uint8_t yawQuarter) const {
        return (yawQuarter & 1u) ? sizeY_ : sizeX_;
    }
    int32_t rotatedSizeY(uint8_t yawQuarter) const {
        return (yawQuarter & 1u) ? sizeX_ : sizeY_;
    }
    // The rotated box's own origin offset, so a caller can place a yawed
    // instance without re-deriving the rotation convention.
    int32_t rotatedOriginX(uint8_t yawQuarter) const;
    int32_t rotatedOriginY(uint8_t yawQuarter) const;

    // Count of non-air cells. Walks the runs once; used by tests and by the
    // bake-verification tooling, not on any hot path.
    uint64_t solidCount() const;

    // Highest material id appearing anywhere in this asset.
    //
    // READ THIS BEFORE WIRING ANY OF THIS UP. Every asset asset-forge has baked
    // so far carries materials the ENGINE DOES NOT HAVE. Its own
    // forge/materials.py says so in as many words -- "SLOTS 16+ ARE PROPOSED
    // AND DO NOT EXIST IN THE ENGINE YET" -- and the two fixtures in
    // tests/fixtures bear it out: the pine is built from 16 (bark), 17
    // (heartwood) and 20 (needle), the daisy from 8 (grass) and 24 (blossom),
    // against a vxc::kMaterialCount of 16.
    //
    // Loading one of those today is not a cosmetic problem. MaterialId is a
    // uint8_t so nothing overflows or faults on decode; what happens instead is
    // that every consumer indexed by material reads past its own array --
    // kMineCostByMaterial[vxc::kMaterialCount] in VoxelAgentSubsystem.cpp:64
    // first among them -- and the mesher's merge key treats an unknown id as a
    // distinct material and cheerfully emits quads for it. The result renders
    // as something, which is the worst available outcome, because it means the
    // append was never noticed as missing.
    //
    // So: a host must gate on this against kMaterialCount and refuse the asset
    // otherwise. It is exposed as a value rather than enforced in parse()
    // because the fix is an enum append with three known tails (mine costs, the
    // mips.h majority arrays, and test_mesher.cpp's golden-digest trap -- all
    // three written up in docs/tree-asset-generator-research.md section 8A), and
    // a reader that refused every existing file would simply be dead code until
    // that lands rather than a check anyone could act on.
    MaterialId maxMaterialId() const;

    // True iff every material in this asset is one vxc::Material defines.
    bool materialsWithinEngine() const {
        return uint32_t(maxMaterialId()) < uint32_t(kMaterialCount);
    }

private:
    void clear();

    // Index of the run containing the first cell of column (lx,ly), and how
    // many cells of that run precede the column. Two flat arrays of nx*ny
    // rather than a struct-of-two so the common read (find the run) touches
    // one cache line per column instead of straddling.
    int32_t sizeX_ = 0, sizeY_ = 0, sizeZ_ = 0;
    int32_t originX_ = 0, originY_ = 0, originZ_ = 0;
    uint32_t voxelSizeMm_ = 0;   // 0 until a successful parse; see voxelSizeMm()
    std::vector<MaterialId> runMat_;
    std::vector<uint32_t> runLen_;
    std::vector<uint32_t> colRun_;
    std::vector<uint32_t> colOff_;
};

} // namespace vxc
