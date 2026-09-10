#pragma once
#include "CoreMinimal.h"
#include "VoxelEnvironmentAsset.h"
#include "VoxelObjectGeometrySnapshot.h"
#include "voxelcore/assetcandidate.h"
class AVoxelEnvironmentLODPrototype;
namespace VoxelProductionCandidate
{
constexpr uint64 MaxCells = 8ull * 1024 * 1024;
constexpr int32 MaxResolved = 8192;
constexpr int32 MaxPages = 8192;
struct FWork
{
    TAtomic<bool> Cancelled{false}, WorkerReady{false};
    bool Success=false, ActorDone=false, ActorValid=false;
    bool RehearseHandoff=false; // explicit diagnostic request; game-thread only
    bool PublishVisualPilot=false; // standalone CPU-arena visual diagnostic only
    bool PrepareHeldGpuPages=false; // brick-only private diagnostic, never publication
    bool PrepareHeldCpuPages=false; // no GPU readiness/publication implied
    int32 Phase=0, PageCursor=0, VisiblePages=0, PendingPages=0, ParkedPages=0;
    uint64 EditEpoch=0, ResidencyEpoch=0;
    double Started=0;
    vxc::AssetField::ResolvedAssetInstance Candidate;
    vxc::AssetCandidateBounds Bounds;
    vxc::AssetProvenance Provenance;
    FVoxelEnvironmentAssetDescriptor Descriptor;
    FString CanonicalSourceHash, ClippedGeometryHash, Error;
    std::vector<vxc::AssetRenderPage> Pages;
    FVoxelImmutableGeometry Geometry;
    TArray<uint8> Dynamic;
    TWeakObjectPtr<AVoxelEnvironmentLODPrototype> Actor;
};
using FWorkRef=TSharedPtr<FWork,ESPMode::ThreadSafe>;
// Console requests only; no automatic production promotion or startup flag.
int32 ConsumeRequest(UWorld* World);
void ForgetWorld(UWorld* World);
bool IsCurrent(const FWork& Work,uint64 EditEpoch,uint64 ResidencyEpoch);
uint64 Fingerprint(const FString& Text);
FString GridContentHash(const vxc::AssetGrid& Grid);
bool EnumeratePages(const vxc::AssetCandidateBounds& Bounds,int32 Levels,std::vector<vxc::AssetRenderPage>& Out);
bool BuildImmutableSnapshot(FWork& Work,const std::vector<uint8>& Vxa);
void Cancel(FWorkRef& Work);
}
