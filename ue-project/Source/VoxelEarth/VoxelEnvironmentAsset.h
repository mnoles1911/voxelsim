#pragma once
#include "CoreMinimal.h"

// Source identity is distinct from the registry's placed-object GUID/revisions.
// Render ownership and collision are supplied by the placement adapter, never
// inferred from the generator kind (large bushes/grass can be terrain assets).
struct FVoxelEnvironmentAssetDescriptor {
    FString SpecId,Kind,Category;
    FString SourceHash,SpecHash,CatalogHash,ProviderHash;
    uint32 SeedIndex=0;
    bool Fellable=false,Legacy=false;
    bool IsValid() const;
    static FVoxelEnvironmentAssetDescriptor Prototype(const FString& Name);
};
namespace VoxelEnvironmentAsset {
// Legacy identities0..3 remain byte-compatible. Generic identity=-1/schema1
// carries bounded source metadata; occupancy stays in the immutable buffer.
bool SerializeIdentity(FArchive& Ar,FVoxelEnvironmentAssetDescriptor& Descriptor);
// Production placement currently uses upright quarter turns. Restricting the
// transform preserves the exact cubic lattice and axis-aligned dig preview.
int32 QuarterYaw(const FTransform& Transform);
bool IsSupportedTransform(const FTransform& Transform);
FVector LocalPosition(const FTransform& Transform,const FVector& World);
FVector WorldPosition(const FTransform& Transform,const FVector& Local);
FVector LocalVector(const FTransform& Transform,const FVector& WorldVector);
}
