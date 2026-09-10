#pragma once
#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "VoxelTerrainAppearanceUpload.h"
class FRDGBuilder;
class FRHICommandListImmediate;
// Construction/destruction may follow the pool holder's lifetime on any thread.
// All mutation and Register calls are render-thread only. No geometry ownership.
class VOXELEARTHSHADERS_API FVoxelTerrainAppearanceGpuState {
public:
    struct FEntry {uint32 Slot=0;TSharedPtr<const FVoxelTerrainAppearanceUpload,ESPMode::ThreadSafe> Upload;};
    struct FBuffers {FRDGBufferRef Pages=nullptr,Sources=nullptr,Ranges=nullptr,Slots=nullptr;};
    FVoxelTerrainAppearanceGpuState();
    ~FVoxelTerrainAppearanceGpuState();
    bool ApplyBatch(FRHICommandListImmediate& RHI,uint32 ChunkCapacity,TConstArrayView<uint32> ClearSlots,TConstArrayView<FEntry> Entries,FString& Error);
    FBuffers Register(FRDGBuilder& GraphBuilder);
    // Persistent bindings for raster vertex-factory uniform buffers. Refresh
    // after Reset or the first data admission: empty and populated bindings
    // intentionally use separate buffers. Render-thread only, like Register.
    struct FViews {FShaderResourceViewRHIRef Pages,Sources,Ranges,Slots;};
    FViews GetViews(FRHICommandListImmediate& RHI);
    void Reset();
private:
    struct FImpl;TUniquePtr<FImpl> Impl;
};
