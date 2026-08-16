#pragma once
// THE SPECIES BANK LIBRARY -- decoded VXA v3 grids, keyed (species, seed),
// resident PER SPECIES BANK and never per instance.
//
// assetgrid.h states the residency model this implements: "an asset is decoded
// ONCE into a process-lifetime library keyed by (species, seed) ... Residency
// is per SPECIES BANK, which is small and bounded, never per instance and
// never per chunk -- an instance is a position, and positions are not data."
// This class is that library, and it is the production implementation of the
// IAssetBankSource seam assetfield.h keeps deliberately abstract.
//
// LAYOUT ON DISK is what asset-forge's tools/export_banks.py writes:
//
//     <root>/<species-name>/<species-name>-NNNN.vxa
//
// where <species-name> is the manifest's bank reference. The seed files are
// taken in SORTED FILENAME ORDER and a site's seedIndex is reduced modulo the
// number of valid files, so a partially-baked bank still serves every draw.
// WHICH FILES EXIST IS THEREFORE WORLDGEN INPUT: adding a seed to a bank
// remaps every (seedIndex % count) draw of that species, which is a
// kWorldGenVersion bump like any other manifest change.
//
// EVERY FILE IS VALIDATED AGAINST ITS SPECIES' LAYER AT LOAD, and a file that
// fails is refused BY NAME with a count -- never stamped. The checks are the
// ones whose failure modes are silent:
//
//   * assetLayerAdmitsHeight / depth / radius against the BAKED grid's own
//     box, not the spec's nominal numbers. A grid taller than its layer's
//     maxHeightMm is a hole in the world at its own crown (the bound proves
//     the crown's chunks are air and they never generate); a grid wider than
//     maxRadiusMm is a crown whose edge rect queries MISS -- a sliced tree.
//   * assetLayerAdmitsVoxelSize and the manifest's own voxelSizeMm: a 5 cm
//     boulder stamped on the 10 cm lattice is a boulder at twice its size,
//     and there are no boulder-shaped diagnostics (assetgrid.h's own words).
//   * materialsWithinEngine: an id past kMaterialCount reads past every
//     material-indexed array in every consumer, and renders as SOMETHING,
//     which is the worst available outcome.
//
// MISSES ARE COUNTED, NOT SWALLOWED. bankGrid() answering nullptr composes as
// air by design (assetfield.h: "a bank that failed to load must leave the
// world it was going to stand in intact") -- which is exactly why the miss has
// to be a number somewhere. A species whose bank never loads is a species
// placement resolved, counted, and rendered as nothing; `stats()` is how that
// stops being invisible. Zero stamped voxels against thousands of requests is
// a wiring fault, and the counters make it a one-line diagnosis.
//
// THREADING. bankGrid() is called from chunk-generation workers; loads are
// serialised under one mutex and a bank, once loaded, is immutable -- returned
// pointers stay valid for the library's lifetime. The counters are plain
// integers mutated under the same mutex; they are diagnostics, not worldgen.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "voxelcore/assetfield.h"
#include "voxelcore/assetgrid.h"
#include "voxelcore/assetmanifest.h"

namespace vxc {

// Why a bank FILE was refused at load. Mirrors AssetParseError's philosophy:
// the only thing worse than refusing an asset is refusing to say which and why.
enum class AssetBankError : uint8_t {
    kOk = 0,
    kUnreadable,       // missing directory or unreadable file
    kParse,            // AssetGrid::parse refused (its own named reason logged)
    kMaterialsBeyond,  // material id >= kMaterialCount
    kWrongVoxelSize,   // file's lattice differs from the manifest's for this species
    kWrongLattice,     // assetLayerAdmitsVoxelSize refused against the layer
    kTooTall,          // baked height above the layer's maxHeightMm
    kTooDeep,          // baked depth below the layer's maxDepthMm
    kTooWide,          // baked reach past the layer's maxRadiusMm
};

const char* assetBankErrorText(AssetBankError e);

class AssetBankLibrary : public IAssetBankSource {
public:
    AssetBankLibrary() = default;

    // `manifest` must outlive this library; `root` is the banks directory.
    // Nothing is read here -- banks load on first touch, so a library over
    // 180 species costs only what the terrain actually asks for.
    void configure(const AssetManifest* manifest, std::string root);

    // The seam assetfield.h consumes. nullptr for anything absent or refused,
    // which composes as "no asset voxels here"; the miss is counted.
    const AssetGrid* bankGrid(uint16_t bankId, uint16_t seedIndex) const override;

    // Force-load one species bank (benches, load-time validation sweeps).
    // Returns the number of seed grids that survived validation.
    int loadSpecies(uint16_t bankId) const;

    struct Stats {
        uint64_t requests = 0;      // bankGrid calls
        uint64_t servedGrids = 0;   // non-null answers
        uint64_t missNoSpecies = 0; // bankId outside the manifest
        uint64_t missNoSeeds = 0;   // bank loaded, zero valid seed files
        uint64_t filesLoaded = 0;
        uint64_t filesRefused = 0;  // sum of refusedBy
        uint64_t refusedBy[9] = {}; // by AssetBankError value
        uint64_t seedCountMismatch = 0; // banks whose valid-file count differs
                                        // from the manifest's seedsBaked: the
                                        // world moved under the manifest
        uint64_t bytesResident = 0;     // decoded footprint, all banks
    };
    Stats stats() const;

private:
    struct Bank {
        bool loaded = false;
        // unique_ptr per grid so pointers survive the vector's growth during
        // load; the vector itself never changes after `loaded` flips.
        std::vector<std::unique_ptr<AssetGrid>> seeds;
    };

    const Bank& bankFor(uint16_t bankId) const;

    const AssetManifest* manifest_ = nullptr;
    std::string root_;
    mutable std::mutex mu_;
    mutable std::unordered_map<uint16_t, Bank> banks_;
    mutable Stats stats_;
};

} // namespace vxc
