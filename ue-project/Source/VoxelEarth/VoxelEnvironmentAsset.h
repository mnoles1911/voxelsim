#pragma once
#include "CoreMinimal.h"
#include "voxelcore/assetownership.h"

// Optional schema-2 source provenance. StableId is checked against every full
// source field; this does not bind an actor to the object registry.
struct FVoxelEnvironmentProductionProvenance {
    vxc::AssetProvenance Source;
    vxc::AssetObjectId StableId;
    FString CanonicalSourceHash;
};

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
    TOptional<FVoxelEnvironmentProductionProvenance> ProductionProvenance;
    uint32 SeedIndex=0;
    bool Fellable=false,Legacy=false;
    // Production preparation may store clipped occupancy under SourceHash.
    // Its canonical appearance still comes from the immutable source identity.
    const FString& AppearanceSourceHash() const {
        return ProductionProvenance.IsSet()?ProductionProvenance->CanonicalSourceHash:SourceHash;
    }
    uint8 AppearanceYawQuarter() const {
        return HasComposition()?SourceYawQuarter:(ProductionProvenance.IsSet()?ProductionProvenance->Source.yawQuarter:0);
    }
    bool IsValid() const;
    static FVoxelEnvironmentAssetDescriptor Prototype(const FString& Name);
};
namespace VoxelEnvironmentAsset {
// Legacy identities0..3 remain byte-compatible. Generic identity=-1/schema1
// carries bounded source metadata; schema2 retains main's full provenance layout.
// Schema3 adds composed geometry and an optional provenance payload.
// Legacy/schema1 layouts remain unchanged.
bool SerializeIdentity(FArchive& Ar,FVoxelEnvironmentAssetDescriptor& Descriptor);
// Production placement currently uses upright quarter turns. Restricting the
// transform preserves the exact cubic lattice and axis-aligned dig preview.
int32 QuarterYaw(const FTransform& Transform);
bool IsSupportedTransform(const FTransform& Transform);
FVector LocalPosition(const FTransform& Transform,const FVector& World);
FVector WorldPosition(const FTransform& Transform,const FVector& Local);
FVector LocalVector(const FTransform& Transform,const FVector& WorldVector);
}
