#pragma once
#include "CoreMinimal.h"
#include "VoxelPublishedAppearanceCatalog.h"

// Editor-source cache only. This is not authorization for loading uncooked
// packages in a packaged game. A cooked index needs a separate contract.
struct FVoxelDetailCacheIdentity {
    FString EngineVersion,HostPlatform,Settings,BuilderIdentity,MaterialSourceSHA256;
    bool bAllowPreview=false;
    bool bActivePublicationIsPreview=false;
};
class FVoxelDetailMeshCacheIndex {
public:
    struct FEntry {
        struct FLod {uint32 Vertices=0,Triangles=0,UVChannels=0;double ScreenSize=0;};
        uint32 Resource=0;
        FString DerivedKey,ObjectPath,MaterialObjectPath,PackageSHA256,MaterialSHA256,PackageFile,MaterialFile,AttributeSHA256;
        FString GeometrySHA256,AppearanceSHA256;
        uint32 PitchUm=0;
        TArray<FLod> Lods;
        FString Bounds;
    };
    // PublicationBytes must be the exact bytes belonging to the retained
    // verified catalog snapshot. This function additionally cross-checks every
    // cache row against that publication and catalog. No filesystem mutations.
    static TSharedPtr<const FVoxelDetailMeshCacheIndex,ESPMode::ThreadSafe> ParseEditorSource(
        const FString& IndexJson,const TArray<uint8>& PublicationBytes,
        TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Catalog,
        const FVoxelDetailCacheIdentity& Expected,FString& Error);
    const FEntry* Find(uint32 Resource) const {return Entries.Find(Resource);}
    int32 Num() const {return Entries.Num();}
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> SourceSnapshot() const {return Snapshot;}
private:
    TMap<uint32,FEntry> Entries;
    TSharedPtr<const FVoxelPublishedAppearanceCatalog,ESPMode::ThreadSafe> Snapshot;
};
