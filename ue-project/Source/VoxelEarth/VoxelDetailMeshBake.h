#pragma once
#include "CoreMinimal.h"
#if WITH_EDITOR
namespace vxc { class AssetGrid; }
class FVoxelAssetAppearance;
class UStaticMesh;
class UPackage;
class UMaterialInterface;
// Editor-only adapter to the actual runtime geometry/appearance/LOD builder.
UStaticMesh* VoxelBakePersistentDetailMesh(const vxc::AssetGrid& Grid,
    TSharedPtr<const FVoxelAssetAppearance,ESPMode::ThreadSafe> Appearance,
    UPackage* Package,FName Name,UMaterialInterface* Material,FString& Error);
#endif
