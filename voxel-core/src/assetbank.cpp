#include "voxelcore/assetbank.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace vxc {

const char* assetBankErrorText(AssetBankError e) {
    switch (e) {
        case AssetBankError::kOk: return "ok";
        case AssetBankError::kUnreadable: return "bank file missing or unreadable";
        case AssetBankError::kParse: return "VXA parse refused";
        case AssetBankError::kMaterialsBeyond:
            return "material id beyond kMaterialCount: every material-indexed consumer "
                   "would read past its own array and it would still render as something";
        case AssetBankError::kWrongVoxelSize:
            return "file's voxel size differs from the manifest's for this species";
        case AssetBankError::kWrongLattice:
            return "voxel size does not match the layer's lattice";
        case AssetBankError::kTooTall:
            return "baked grid taller than its layer's maxHeightMm: the streaming bound "
                   "would prove its crown to be air -- a hole in the world";
        case AssetBankError::kTooDeep: return "baked grid reaches below the layer's maxDepthMm";
        case AssetBankError::kTooWide:
            return "baked grid reaches past the layer's maxRadiusMm: rect queries would "
                   "miss its edge and slice the crown";
    }
    return "unknown";
}

namespace {

bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    out.clear();
#ifdef _MSC_VER
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || f == nullptr) return false;
#else
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (f == nullptr) return false;
#endif
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        const size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return true;
}

// One seed file against its species' layer contract. kOk or the FIRST reason
// it fails -- the order below runs the cheap structural refusals before the
// geometric ones so a truncated file never reaches a box measurement.
AssetBankError validateGrid(const AssetGrid& g, const AssetManifestSpecies& sp,
                            const AssetLayer& layer) {
    if (!g.materialsWithinEngine()) return AssetBankError::kMaterialsBeyond;
    if (g.voxelSizeMm() != sp.voxelSizeMm) return AssetBankError::kWrongVoxelSize;
    if (!assetLayerAdmitsVoxelSize(layer, g.voxelSizeMm())) return AssetBankError::kWrongLattice;
    // The baked box's own extents, in mm, measured the way assetfield.h will
    // read them: height above the anchor is originZ + sizeZ, depth below it is
    // -originZ, and horizontal reach is the furthest box corner on either axis
    // in either sign.
    const int64_t vs = int64_t(g.voxelSizeMm());
    const int64_t heightMm = (int64_t(g.originZ()) + int64_t(g.sizeZ())) * vs;
    const int64_t depthMm = -int64_t(g.originZ()) * vs;
    if (heightMm > int64_t(layer.maxHeightMm)) return AssetBankError::kTooTall;
    if (depthMm > int64_t(layer.maxDepthMm)) return AssetBankError::kTooDeep;
    const int64_t ox = int64_t(g.originX()), oy = int64_t(g.originY());
    int64_t reach = 0;
    const int64_t corners[4] = {ox < 0 ? -ox : ox, oy < 0 ? -oy : oy,
                                ox + int64_t(g.sizeX()) < 0 ? -(ox + int64_t(g.sizeX()))
                                                            : ox + int64_t(g.sizeX()),
                                oy + int64_t(g.sizeY()) < 0 ? -(oy + int64_t(g.sizeY()))
                                                            : oy + int64_t(g.sizeY())};
    for (int i = 0; i < 4; ++i) reach = corners[i] > reach ? corners[i] : reach;
    if (reach * vs > int64_t(layer.maxRadiusMm)) return AssetBankError::kTooWide;
    return AssetBankError::kOk;
}

} // namespace

void AssetBankLibrary::configure(const AssetManifest* manifest, std::string root) {
    std::lock_guard<std::mutex> lock(mu_);
    manifest_ = manifest;
    root_ = std::move(root);
    banks_.clear();
    stats_ = Stats{};
}

const AssetBankLibrary::Bank& AssetBankLibrary::bankFor(uint16_t bankId) const {
    // Caller holds mu_.
    Bank& bank = banks_[bankId];
    if (bank.loaded) return bank;
    bank.loaded = true;

    const AssetManifestSpecies& sp = manifest_->species()[bankId];
    const AssetLayer& layer =
        manifest_->layers()[sp.layer == kAssetLayerNotScattered ? 0 : sp.layer];

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::path(root_) / sp.name;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& f = it->path();
        if (f.extension() == ".vxa") files.push_back(f);
    }
    // SORTED FILENAME ORDER is the seed order. Directory iteration order is
    // filesystem-dependent; the sort is what makes "seed 3 of the oak bank"
    // mean the same grid on every machine that has the same files.
    std::sort(files.begin(), files.end());

    std::vector<uint8_t> bytes;
    for (const fs::path& f : files) {
        if (!readFile(f, bytes)) {
            ++stats_.filesRefused;
            ++stats_.refusedBy[size_t(AssetBankError::kUnreadable)];
            continue;
        }
        auto g = std::make_unique<AssetGrid>();
        const AssetParseError pe = g->parse(bytes);
        if (pe != AssetParseError::kOk) {
            ++stats_.filesRefused;
            ++stats_.refusedBy[size_t(AssetBankError::kParse)];
            std::fprintf(stderr, "assetbank: %s: %s\n", f.string().c_str(),
                         assetParseErrorText(pe));
            continue;
        }
        const AssetBankError ve = validateGrid(*g, sp, layer);
        if (ve != AssetBankError::kOk) {
            ++stats_.filesRefused;
            ++stats_.refusedBy[size_t(ve)];
            std::fprintf(stderr, "assetbank: %s: %s\n", f.string().c_str(),
                         assetBankErrorText(ve));
            continue;
        }
        stats_.bytesResident += g->footprintBytes();
        ++stats_.filesLoaded;
        bank.seeds.push_back(std::move(g));
    }
    if (bank.seeds.size() != size_t(sp.seedsBaked)) ++stats_.seedCountMismatch;
    return bank;
}

const AssetGrid* AssetBankLibrary::bankGrid(uint16_t bankId, uint16_t seedIndex) const {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.requests;
    if (manifest_ == nullptr || size_t(bankId) >= manifest_->species().size()) {
        ++stats_.missNoSpecies;
        return nullptr;
    }
    const Bank& bank = bankFor(bankId);
    if (bank.seeds.empty()) {
        ++stats_.missNoSeeds;
        return nullptr;
    }
    // Reduced modulo the VALID seed count: a partially-baked bank serves every
    // draw rather than dropping the seeds it does not have. Which files exist
    // is worldgen input -- see the header.
    const AssetGrid* g = bank.seeds[size_t(seedIndex) % bank.seeds.size()].get();
    ++stats_.servedGrids;
    return g;
}

int AssetBankLibrary::loadSpecies(uint16_t bankId) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (manifest_ == nullptr || size_t(bankId) >= manifest_->species().size()) return 0;
    return int(bankFor(bankId).seeds.size());
}

AssetBankLibrary::Stats AssetBankLibrary::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

} // namespace vxc
