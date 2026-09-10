#pragma once
#include "CoreMinimal.h"
#include "voxelcore/assetecologybinding.h"
#include "voxelcore/assetfield.h"
class FVoxelAppearanceBankBinding;

// Parse/verify before worker startup; no publication or asset mutation.
namespace VoxelEcologicalPlacement {
bool Install(const vxc::EcoPlacementConfig& Config,const vxc::AssetManifest& Manifest,
    vxc::AssetField& Field,std::vector<vxc::AssetSpecies>& SpeciesTable);
bool Load(const FString& ConfigurationPath,const FString& AssetDirectory,
    const vxc::AssetManifest& Manifest,const vxc::AssetBankLibrary& Banks,
    const FVoxelAppearanceBankBinding& Binding,vxc::EcoPlacementConfig& Output,FString& Error);
}
