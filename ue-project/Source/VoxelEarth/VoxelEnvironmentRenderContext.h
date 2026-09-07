#pragma once
#include "VoxelProductionEnvironmentAdapter.h"
#include "voxelcore/assetfield.h"

namespace VoxelEnvironmentRender {
// Bindings come from the current canonical bank, never a clipped actor grid.
// The bank must remain immutable/alive through every worker using this context.
struct FSourceBinding {
    uint16 BankId=0,SeedIndex=0;
    const vxc::AssetGrid* Grid=nullptr;
    FString CanonicalSourceHash;
};
class FContext;
using FContextRef=TSharedPtr<const FContext,ESPMode::ThreadSafe>;
// Bounded admission, intended for a preparation worker, not per-chunk hashing.
// Empty ownership requires no source bindings and performs no grid hashing.
FContextRef Build(VoxelProductionEnvironment::FSnapshot Snapshot,uint64 WorldSeed,
    const FString& Provider,const FString& Catalog,const TArray<FSourceBinding>& Bindings,FString& Error);
class FContext {
public:
    uint64 Generation() const {return Snapshot->generation;}
    // Inclusive base-voxel AABB; conservative for coarse representative/apron sampling.
    bool AffectsBounds(int64 X0,int64 Y0,int64 Z0,int64 X1,int64 Y1,int64 Z1) const;
    bool HasOwnedSources() const {return !Sources.IsEmpty();}
    // Input MUST already be job-private; even an empty context clears old flags.
    // Preserves order and storage. False means a stale/wrong bank binding; no
    // partial marker changes. Empty contexts clear flags without hashing/copies.
    bool MarkPrivate(std::vector<vxc::AssetField::ResolvedAssetInstance>& Instances) const;
private:
    friend FContextRef Build(VoxelProductionEnvironment::FSnapshot,uint64,const FString&,const FString&,const TArray<FSourceBinding>&,FString&);
    struct FOwned {vxc::AssetProvenance Provenance;const vxc::AssetGrid* Grid=nullptr;};
    VoxelProductionEnvironment::FSnapshot Snapshot;
    TArray<FOwned> Sources;
};
}
