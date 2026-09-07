#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace vxc {
// A footprint bin's lowest occupied asset voxel and sampled terrain envelope.
// Units are millimetres. The caller owns terrain sampling/residency and caching.
struct AssetSupportBin {
    double bottomMm = 0;
    double groundLowMm = 0;
    double groundHighMm = 0;
};
struct AssetSlopeFit {
    bool accepted = false;
    double originZMm = 0;
    double sinkMm = 0;
    double maximumBurialMm = 0;
};

// Upright rigid seating: choose the HIGHEST lattice-aligned origin for which
// all sampled support bins enter terrain. Never progressively sink an existing
// transform: referenceZ is a fresh terrain-derived anchor on every resolve.
inline AssetSlopeFit fitAssetSupports(const AssetSupportBin* bins, size_t count,
                                     double referenceZMm, double latticeMm,
                                     double maximumSinkMm, double maximumBurialMm,
                                     double embedMm = 0) {
    AssetSlopeFit out;
    if (!bins || !count || !std::isfinite(referenceZMm) ||
        !std::isfinite(latticeMm) || latticeMm <= 0 ||
        !std::isfinite(maximumSinkMm) || maximumSinkMm < 0 ||
        !std::isfinite(maximumBurialMm) || maximumBurialMm < 0 ||
        !std::isfinite(embedMm) || embedMm < 0) return out;
    double target = referenceZMm;
    for (size_t i = 0; i < count; ++i) {
        const auto& b = bins[i];
        if (!std::isfinite(b.bottomMm) || !std::isfinite(b.groundLowMm) ||
            !std::isfinite(b.groundHighMm) || b.groundLowMm > b.groundHighMm) return out;
        target = std::min(target, b.groundLowMm - b.bottomMm - embedMm);
    }
    out.originZMm = std::floor(target / latticeMm) * latticeMm;
    out.sinkMm = referenceZMm - out.originZMm;
    for (size_t i = 0; i < count; ++i)
        out.maximumBurialMm = std::max(out.maximumBurialMm,
            bins[i].groundHighMm - (out.originZMm + bins[i].bottomMm));
    out.accepted = out.sinkMm <= maximumSinkMm && out.maximumBurialMm <= maximumBurialMm;
    return out;
}
} // namespace vxc
