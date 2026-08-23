#pragma once
// FVoxelRasterAtlasGpu -- render-side owner of the persistent elevation/climate
// raster atlas the VXC_RASTER_ATLAS worldgen permutation samples
// (docs/gpu-streaming-architecture.md; the FillRasterWindow arithmetic blocker:
// ~46 KB re-sampled per chunk on the game thread, 94% of it identical to the
// lateral neighbour's window and 100% identical between Z-siblings).
//
// DIVISION OF LABOUR, mirroring the march index and the brick pool:
//   * FVoxelRasterAtlasCpu (VoxelEarth module) owns the POLICY -- which pages
//     exist, filling them through the vxc sampler funnel, recentring with the
//     anchor, the coverage check against ComputeRasterWindowPx -- because
//     policy needs voxel-core and the tile samplers, which this module must
//     not include.
//   * THIS class owns the RESOURCES -- four pooled buffers and the upsert /
//     readback machinery -- because persistent GPU buffers live where the
//     graphs that read them are built.
// The two meet at exactly one type, FVoxelRasterAtlasGpuDelta, and one call,
// EnqueueUpsert.
//
// LIFETIME AND THREADS. Config is written once by Init on the game thread
// before any delta or bind can exist, immutable after (the pool-layout rule).
// The pooled buffers are created and touched only on the render thread.
// EnqueueUpsert and EnqueueMissStatsRead are game-thread; Register and
// GetBindings are render-thread.

#include "CoreMinimal.h"
#include "RenderGraphResources.h"
#include "RHIGPUReadback.h"

class FRDGBuilder;
class FRDGPooledBuffer;

// One flush of page traffic, staged on the game thread. Element i of an
// entering page's pixels is StagedElevationMm[base+i] / StagedClimatePacked
// [base+i]; PageMeta is [slot, tag, base] per page -- the layout
// VoxelRasterAtlasUpsert.usf documents.
struct FVoxelRasterAtlasGpuDelta
{
	TArray<uint32> PageMeta;            // 3 dwords per entering page
	TArray<int32> StagedElevationMm;
	TArray<uint32> StagedClimatePacked;
	TArray<uint32> InvalidateSlots;     // 1 dword per leaving page
	bool bClearMissStats = false;
};

// What a region graph needs to bind the atlas permutation's parameters.
struct FVoxelRasterAtlasBindings
{
	FRDGBufferSRVRef ElevationMm = nullptr;
	FRDGBufferSRVRef ClimatePacked = nullptr;
	FRDGBufferSRVRef PageTags = nullptr;
	FRDGBufferUAVRef MissStats = nullptr;
	uint32 PagePx = 0;
	uint32 PagesDim = 0;
};

// The miss stats, once a readback completes. THE COUNTER THAT MUST BE ABLE TO
// COME OUT THE OTHER WAY: Misses > 0 is a FAIL -- a generation pass tapped a
// page the tags called non-resident and got the documented missing-tile answer
// (sea level / bland climate) instead of terrain. The failing reading in the
// log is `[raster-atlas] gpuMiss=N firstPages=...`; the visible symptom is
// sea-level-flat terrain at the miss site. Zero with the atlas armed and
// requests flowing is the healthy reading; zero with ZERO requests is the DEAD
// reading and the CPU side's window line calls it out separately.
struct FVoxelRasterAtlasMissStats
{
	uint32 Misses = 0;
	uint32 FirstPageTags[4] = {0, 0, 0, 0};
};

class VOXELEARTHSHADERS_API FVoxelRasterAtlasGpu
{
public:
	// Game thread, once, before any other call. PagesDim*PagePx pixels of
	// torus per axis; sizes the payload buffers at
	// PagesDim^2 * PagePx^2 * 8 bytes -- the caller (FVoxelRasterAtlasCpu)
	// logs the MiB at init so the allocation is never a surprise.
	void Init(uint32 InPagePx, uint32 InPagesDim);
	bool IsInitialized() const { return PagePx != 0; }

	uint32 GetPagePx() const { return PagePx; }
	uint32 GetPagesDim() const { return PagesDim; }

	// Game thread. Enqueues one render command that applies the delta through
	// the three upsert passes. Commands run in enqueue order, so a delta
	// enqueued before the job manager's dispatch command in the same tick is
	// visible to every region graph that tick dispatches.
	void EnqueueUpsert(FVoxelRasterAtlasGpuDelta&& Delta);

	// Game thread. Arms one async miss-stats readback (a no-op if one is still
	// in flight); PollMissStats returns the result when it lands. Never blocks.
	void EnqueueMissStatsRead();
	bool PollMissStats(FVoxelRasterAtlasMissStats& Out);

	// Render thread, from inside a graph that wants to SAMPLE the atlas.
	// Registers the pooled buffers (creating them empty -- all tags sentinel --
	// if no upsert ever ran, so a mis-ordered first dispatch reads as counted
	// misses, never as a crash or a silent pass) and returns the bindings.
	FVoxelRasterAtlasBindings Register(FRDGBuilder& GraphBuilder);

	// Render thread. Drops the pooled buffers (PIE teardown).
	void ReleaseResources_RenderThread();

private:
	FRDGBufferRef EnsureBuffers_RenderThread(FRDGBuilder& GraphBuilder,
	                                         FRDGBufferRef& OutElev,
	                                         FRDGBufferRef& OutClimate,
	                                         FRDGBufferRef& OutMiss);

	uint32 PagePx = 0;
	uint32 PagesDim = 0;

	// Render thread only.
	TRefCountPtr<FRDGPooledBuffer> PooledElevation;
	TRefCountPtr<FRDGPooledBuffer> PooledClimate;
	TRefCountPtr<FRDGPooledBuffer> PooledTags;
	TRefCountPtr<FRDGPooledBuffer> PooledMissStats;

	// Miss readback: armed on the game thread (flag), serviced on the render
	// thread, polled on the game thread. One in flight at a time.
	TUniquePtr<FRHIGPUBufferReadback> MissReadback;
	std::atomic<bool> bMissReadbackArmed{false};
	std::atomic<bool> bMissReadbackInFlight{false};
};
