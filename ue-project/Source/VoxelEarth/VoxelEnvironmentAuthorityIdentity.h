#pragma once
#include "CoreMinimal.h"
#include "voxelcore/assetauthority.h"
namespace VoxelEnvironmentAuthority {
// Session-owned immutable registration. Field must remain alive/frozen during
// calls; an unknown address or changed placement table answers no identity.
class FIdentity final : public vxc::IAssetAuthorityIdentity {
public:
    static TUniquePtr<FIdentity> Create(const vxc::AssetField& Field,TConstArrayView<uint8> ManifestBytes,FString& Error);
    std::string catalogIdentity(const vxc::AssetField& Field) const override;
    std::string contentHash(const vxc::AssetGrid& Grid) const override;
    FIdentity(const FIdentity&)=delete;
    FIdentity& operator=(const FIdentity&)=delete;
private:
    FIdentity()=default;
    const vxc::AssetField* Registered=nullptr;
    uint64 Seed=0;
    std::vector<vxc::AssetLayer> Layers;
    std::vector<vxc::AssetSpecies> Species;
    std::string Catalog;
};
}
