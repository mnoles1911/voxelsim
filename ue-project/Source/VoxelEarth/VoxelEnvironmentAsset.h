#pragma once
#include "CoreMinimal.h"

// Source identity is distinct from the registry's placed-object GUID/revisions.
// Render ownership and collision are supplied by the placement adapter, never
// inferred from the generator kind (large bushes/grass can be terrain assets).
struct FVoxelEnvironmentAssetDescriptor {
    FString SpecId,Kind,Category;
    FString SourceHash,SpecHash,CatalogHash,ProviderHash;
    // Optional composed occupancy identity. SourceHash always identifies the
    // original canonical source/appearance, never the clipped VXA bytes.
    FString ClippedGeometryHash;
    uint8 SourceYawQuarter=0;
    bool HasComposition() const {return !ClippedGeometryHash.IsEmpty();}
    uint32 SeedIndex=0;
    bool Fellable=false,Legacy=false;
    bool IsValid() const;
    static FVoxelEnvironmentAssetDescriptor Prototype(const FString& Name);
};
namespace VoxelEnvironmentAsset {
// Legacy identities0..3 remain byte-compatible. Generic identity=-1/schema1
// carries bounded source metadata; schema2 appends composed geometry metadata.
// Uncomposed schema1 and legacy markers retain their original byte layout.
bool SerializeIdentity(FArchive& Ar,FVoxelEnvironmentAssetDescriptor& Descriptor);
// Production placement currently uses upright quarter turns. Restricting the
// transform preserves the exact cubic lattice and axis-aligned dig preview.
int32 QuarterYaw(const FTransform& Transform);
bool IsSupportedTransform(const FTransform& Transform);
FVector LocalPosition(const FTransform& Transform,const FVector& World);
FVector WorldPosition(const FTransform& Transform,const FVector& Local);
FVector LocalVector(const FTransform& Transform,const FVector& WorldVector);
}
