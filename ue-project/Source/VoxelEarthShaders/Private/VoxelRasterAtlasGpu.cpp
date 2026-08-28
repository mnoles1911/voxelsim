// FVoxelRasterAtlasGpu -- see the header for the division of labour. This file
// is resource plumbing only: three global shaders, four pooled buffers, one
// async readback. Every policy decision (which pages, what values, when to
// recentre) lives in FVoxelRasterAtlasCpu on the other side of
// FVoxelRasterAtlasGpuDelta.

#include "VoxelRasterAtlasGpu.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RHIBreadcrumbs.h"                         // RHI_BREADCRUMB_EVENT_STAT (5.8 spelling)

// ---------------------------------------------------------------------------
// STREAMING-SIDE GPU STATS -- the split for the unattributed +5.47 ms.
//
// THE SPELLING MATTERS AND THREE OF THEM ARE DEAD. In 5.8 SCOPED_GPU_STAT,
// RDG_GPU_STAT_SCOPE and RDG_RHI_GPU_STAT_SCOPE are UE_DEPRECATED_MACRO and
// expand to NOTHING -- they compile, they look armed, and they measure zero.
// RHI_BREADCRUMB_EVENT_STAT (RHIBreadcrumbs.h:1302) and RDG_EVENT_SCOPE_STAT
// (RenderGraphEvent.h:480) are the live spellings. These sites use the first;
// see the note at each one for why the RDG form cannot be used here.
//
// WHAT THE COLUMN IS. Every DECLARE_GPU_STAT_NAMED stat has its per-frame
// EXCLUSIVE (Busy + Wait) milliseconds written to the CSV profiler once per
// frame, end-of-pipe, as GPU/<StatName> (GPUProfiler.cpp:1065-1067). The
// engine also emits GPU/Unaccounted -- queue time inside NO stat scope
// (GPUProfiler.cpp:800, accumulated at :1556 only when the stat stack is
// empty). So the GPU/ columns of one row SUM to the frame's queue busy time
// and the decomposition checks itself.
//
// KEEP THESE SIBLINGS, NEVER NESTED. Exclusive time is charged to the
// innermost stat only; nesting would silently move a term and make "which
// number am I reading" a live question. Each scope below wraps one standalone
// FRDGBuilder in one ENQUEUE_RENDER_COMMAND, so they are siblings by
// construction.
//
// ARMING: -csvGpuStats on the command line (r.GPUCsvStatsEnabled defaults 0 --
// with it off the GPU/ columns are simply ABSENT, no error), plus
// `CsvProfile FRAMES=N`. A CSV with no GPU/ column measured nothing.
// ---------------------------------------------------------------------------
DECLARE_GPU_STAT_NAMED(VoxelStreamAtlas, TEXT("VoxelStreamAtlas"));

namespace
{
	constexpr uint32 kMissStatsDwords = 5; // [0] count, [1..4] first tags

	class FVoxelRasterAtlasInvalidateCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelRasterAtlasInvalidateCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelRasterAtlasInvalidateCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InvalidateSlots)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AtlasPageTagsRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AtlasMissStatsRW)
			SHADER_PARAMETER(uint32, InvalidateCount)
			SHADER_PARAMETER(uint32, ClearMissStats)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelRasterAtlasPayloadCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelRasterAtlasPayloadCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelRasterAtlasPayloadCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, UpsertPages)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, StagedElevationMm)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, StagedClimatePacked)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, AtlasElevationMmRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AtlasClimatePackedRW)
			SHADER_PARAMETER(uint32, UpsertCount)
			SHADER_PARAMETER(uint32, PixelsPerPage)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelRasterAtlasCommitCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelRasterAtlasCommitCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelRasterAtlasCommitCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, UpsertPages)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AtlasPageTagsRW)
			SHADER_PARAMETER(uint32, UpsertCount)
		END_SHADER_PARAMETER_STRUCT()
	};
}

#define VOXEL_RASTER_ATLAS_USF "/VoxelEarth/VoxelRasterAtlasUpsert.usf"
IMPLEMENT_GLOBAL_SHADER(FVoxelRasterAtlasInvalidateCS, VOXEL_RASTER_ATLAS_USF, "RasterAtlasInvalidateMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelRasterAtlasPayloadCS,    VOXEL_RASTER_ATLAS_USF, "RasterAtlasPayloadMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelRasterAtlasCommitCS,     VOXEL_RASTER_ATLAS_USF, "RasterAtlasCommitMain",     SF_Compute);

void FVoxelRasterAtlasGpu::Init(uint32 InPagePx, uint32 InPagesDim)
{
	check(IsInGameThread());
	check(PagePx == 0);       // once, before any delta or bind -- the pool-layout rule
	check(InPagePx > 0 && InPagesDim > 0);
	PagePx = InPagePx;
	PagesDim = InPagesDim;

	// Buffers are created HERE, eagerly, in their own render command -- not
	// lazily on first Register. Register is called once per JOB inside the
	// batch graph, and RDG's RegisterExternalBuffer is idempotent per graph
	// (FindExternalBuffer returns the existing handle), so eager creation
	// makes every later call a pure lookup. A lazy first-touch inside a batch
	// graph would create a SECOND set of buffers for the second job of the
	// same graph, because extraction into the pooled handles only happens at
	// Execute -- a double-allocation that renders as every job sampling a
	// different, mostly-invalid atlas.
	ENQUEUE_RENDER_COMMAND(VoxelRasterAtlasCreate)(
		[this](FRHICommandListImmediate& RHICmdList)
	{
		const uint32 Slots = PagesDim * PagesDim;
		const uint32 Pixels = Slots * PagePx * PagePx;
		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamAtlas, "VoxelStreamAtlas");
		FRDGBuilder GraphBuilder(RHICmdList);

		// Tags are cleared to the SENTINEL, not zero: zero packs page
		// (-32768,-32768), which no real query reaches but which is a real
		// page coordinate -- an all-zero tag table would be a table full of
		// one absurd but VALID page. All-sentinel means every tap before the
		// first upsert is a COUNTED miss, which is the honest state.
		FRDGBufferRef Tags = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Slots),
			TEXT("Voxel.RasterAtlasTags"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Tags), 0xffffffffu);
		// PAYLOADS ARE ALLOCATED OUTSIDE THE GRAPH, and that is forced by the
		// decision not to clear them. A payload behind a sentinel tag is
		// unreachable by construction (atlasResolve checks the tag first), so
		// clearing hundreds of MiB here would be a hitch bought for nothing --
		// but an RDG buffer that no pass writes cannot be extracted:
		//
		//   Assertion failed: Resource->bProduced || Resource->bExternal ||
		//   Resource->bQueuedForUpload -- "Unable to queue the extraction of
		//   the resource Voxel.RasterAtlasElev because it has not been
		//   produced by any pass."
		//
		// AllocatePooledBuffer hands back the pooled buffer directly, with no
		// producing pass required and no clear. The alternative -- creating
		// them in the graph and clearing to satisfy RDG -- would reintroduce
		// exactly the hitch this comment exists to avoid, at up to ~610 MiB
		// under ring 6 at fine pitch.
		PooledElevation = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), Pixels),
			TEXT("Voxel.RasterAtlasElev"));
		PooledClimate = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Pixels),
			TEXT("Voxel.RasterAtlasClimate"));
		FRDGBufferRef Miss = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), kMissStatsDwords),
			TEXT("Voxel.RasterAtlasMiss"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Miss), 0u);

		// Only the two CLEARED buffers go through extraction -- the payloads
		// above are already pooled and never entered the graph.
		GraphBuilder.QueueBufferExtraction(Tags, &PooledTags);
		GraphBuilder.QueueBufferExtraction(Miss, &PooledMissStats);
		GraphBuilder.Execute();
	});
}

FRDGBufferRef FVoxelRasterAtlasGpu::EnsureBuffers_RenderThread(FRDGBuilder& GraphBuilder,
                                                               FRDGBufferRef& OutElev,
                                                               FRDGBufferRef& OutClimate,
                                                               FRDGBufferRef& OutMiss)
{
	// Render commands run in enqueue order and Init's creation command was
	// enqueued before any upsert or dispatch could be (game-thread program
	// order), so the pooled handles are valid here by construction.
	check(IsInitialized());
	check(PooledTags.IsValid());
	FRDGBufferRef Tags = GraphBuilder.RegisterExternalBuffer(PooledTags, TEXT("Voxel.RasterAtlasTags"));
	OutElev = GraphBuilder.RegisterExternalBuffer(PooledElevation, TEXT("Voxel.RasterAtlasElev"));
	OutClimate = GraphBuilder.RegisterExternalBuffer(PooledClimate, TEXT("Voxel.RasterAtlasClimate"));
	OutMiss = GraphBuilder.RegisterExternalBuffer(PooledMissStats, TEXT("Voxel.RasterAtlasMiss"));
	return Tags;
}

void FVoxelRasterAtlasGpu::EnqueueUpsert(FVoxelRasterAtlasGpuDelta&& Delta)
{
	check(IsInGameThread());
	check(IsInitialized());
	check(Delta.PageMeta.Num() % 3 == 0);
	const uint32 PixelsPerPage = PagePx * PagePx;
	const uint32 NumUpserts = uint32(Delta.PageMeta.Num() / 3);
	check(Delta.StagedElevationMm.Num() == int64(NumUpserts) * PixelsPerPage);
	check(Delta.StagedClimatePacked.Num() == int64(NumUpserts) * PixelsPerPage);
	if (NumUpserts == 0 && Delta.InvalidateSlots.Num() == 0 && !Delta.bClearMissStats)
	{
		return;
	}

	const bool bWantMissRead = bMissReadbackArmed.exchange(false);

	ENQUEUE_RENDER_COMMAND(VoxelRasterAtlasUpsert)(
		[this, Delta = MoveTemp(Delta), PixelsPerPage, NumUpserts,
		 bWantMissRead](FRHICommandListImmediate& RHICmdList) mutable
	{
		// Drain any readback that landed since the last upsert, HERE, on the
		// render thread. The game thread's PollMissStats only consumes what
		// this publishes -- doing the RHI IsReady()/Lock() in Tick asserted
		// IsInRenderingThread() and killed the run at 37 s.
		PollMissStats_RenderThread();

		// ON THE RHI COMMAND LIST, NOT THE GRAPH, AND THAT IS FORCED. An
		// RDG_EVENT_SCOPE_STAT here asserts at FRDGBuilder::Execute --
		// RenderGraphBuilder.cpp:1770 checks the graph's breadcrumb is back at
		// Sentinel -- because these builders Execute inside the scope rather than
		// after it. Measured: it crashed the first leg at VoxelRasterAtlasGpu.
		// RHI_BREADCRUMB_EVENT_STAT is the same stat on the RHI timeline, feeds
		// the same GPU/<name> CSV column, and outlives the graph by construction
		// (declared before it, destroyed after it).
		RHI_BREADCRUMB_EVENT_STAT(RHICmdList, VoxelStreamAtlas, "VoxelStreamAtlas");
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGBufferRef Elev = nullptr;
		FRDGBufferRef Climate = nullptr;
		FRDGBufferRef Miss = nullptr;
		FRDGBufferRef Tags = EnsureBuffers_RenderThread(GraphBuilder, Elev, Climate, Miss);

		const uint32 NumInvalidate = uint32(Delta.InvalidateSlots.Num());

		// Miss readback FIRST, before the invalidate pass that may clear the
		// stats: RDG orders same-resource passes by submission, so copying
		// here reads the window that just ENDED, and the clear below opens
		// the next one. Copy-after-clear would read zeros forever -- a meter
		// that can never fire, the exact dead-gate shape this project keeps
		// paying for.
		if (bWantMissRead && !bMissReadbackInFlight.load())
		{
			if (!MissReadback.IsValid())
			{
				MissReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.RasterAtlasMissRead"));
			}
			AddEnqueueCopyPass(GraphBuilder, MissReadback.Get(), Miss,
			                   kMissStatsDwords * sizeof(uint32));
			bMissReadbackInFlight.store(true);
		}

		// Pass 1: invalidate leaving pages (and clear the miss window if asked).
		if (NumInvalidate > 0 || Delta.bClearMissStats)
		{
			// CreateStructuredBuffer refuses zero elements; a lone stats clear
			// still needs a bound (never-read) buffer.
			static const uint32 ZeroSlot = 0xffffffffu;
			const uint32* SlotData = NumInvalidate > 0 ? Delta.InvalidateSlots.GetData() : &ZeroSlot;
			const uint32 SlotCount = NumInvalidate > 0 ? NumInvalidate : 1u;
			FRDGBufferRef SlotsBuf = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.RasterAtlasInvalidate"), sizeof(uint32),
				SlotCount, SlotData, SlotCount * sizeof(uint32));

			FVoxelRasterAtlasInvalidateCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelRasterAtlasInvalidateCS::FParameters>();
			Params->InvalidateSlots = GraphBuilder.CreateSRV(SlotsBuf);
			Params->AtlasPageTagsRW = GraphBuilder.CreateUAV(Tags);
			Params->AtlasMissStatsRW = GraphBuilder.CreateUAV(Miss);
			Params->InvalidateCount = NumInvalidate;
			Params->ClearMissStats = Delta.bClearMissStats ? 1u : 0u;
			TShaderMapRef<FVoxelRasterAtlasInvalidateCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.RasterAtlasInvalidate(%u)", NumInvalidate),
				Shader, Params,
				FIntVector(FMath::DivideAndRoundUp(FMath::Max(NumInvalidate, 1u), 64u), 1, 1));
		}

		// Passes 2+3: payload, then tags -- tag-after-payload is the torn-page
		// guarantee, see the .usf header.
		if (NumUpserts > 0)
		{
			FRDGBufferRef MetaBuf = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.RasterAtlasUpsertMeta"), sizeof(uint32),
				Delta.PageMeta.Num(), Delta.PageMeta.GetData(),
				Delta.PageMeta.Num() * sizeof(uint32));
			FRDGBufferRef StagedElev = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.RasterAtlasStagedElev"), sizeof(int32),
				Delta.StagedElevationMm.Num(), Delta.StagedElevationMm.GetData(),
				Delta.StagedElevationMm.Num() * sizeof(int32));
			FRDGBufferRef StagedClimate = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.RasterAtlasStagedClimate"), sizeof(uint32),
				Delta.StagedClimatePacked.Num(), Delta.StagedClimatePacked.GetData(),
				Delta.StagedClimatePacked.Num() * sizeof(uint32));

			{
				FVoxelRasterAtlasPayloadCS::FParameters* Params =
					GraphBuilder.AllocParameters<FVoxelRasterAtlasPayloadCS::FParameters>();
				Params->UpsertPages = GraphBuilder.CreateSRV(MetaBuf);
				Params->StagedElevationMm = GraphBuilder.CreateSRV(StagedElev);
				Params->StagedClimatePacked = GraphBuilder.CreateSRV(StagedClimate);
				Params->AtlasElevationMmRW = GraphBuilder.CreateUAV(Elev);
				Params->AtlasClimatePackedRW = GraphBuilder.CreateUAV(Climate);
				Params->UpsertCount = NumUpserts;
				Params->PixelsPerPage = PixelsPerPage;
				TShaderMapRef<FVoxelRasterAtlasPayloadCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("Voxel.RasterAtlasPayload(%u pages)", NumUpserts),
					Shader, Params, FIntVector(int32(NumUpserts), 1, 1));
			}
			{
				FVoxelRasterAtlasCommitCS::FParameters* Params =
					GraphBuilder.AllocParameters<FVoxelRasterAtlasCommitCS::FParameters>();
				Params->UpsertPages = GraphBuilder.CreateSRV(MetaBuf);
				Params->AtlasPageTagsRW = GraphBuilder.CreateUAV(Tags);
				Params->UpsertCount = NumUpserts;
				TShaderMapRef<FVoxelRasterAtlasCommitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("Voxel.RasterAtlasCommit(%u)", NumUpserts),
					Shader, Params,
					FIntVector(FMath::DivideAndRoundUp(NumUpserts, 64u), 1, 1));
			}
		}

		GraphBuilder.Execute();
	});
}

void FVoxelRasterAtlasGpu::EnqueueMissStatsRead()
{
	check(IsInGameThread());
	bMissReadbackArmed.store(true);
}

void FVoxelRasterAtlasGpu::PollMissStats_RenderThread()
{
	check(IsInRenderingThread());
	if (!bMissReadbackInFlight.load() || !MissReadback.IsValid() || !MissReadback->IsReady())
	{
		return;
	}
	uint32 Data[kMissStatsDwords] = {};
	if (const void* Src = MissReadback->Lock(sizeof(Data)))
	{
		FMemory::Memcpy(Data, Src, sizeof(Data));
		MissReadback->Unlock();
	}
	bMissReadbackInFlight.store(false);
	PublishedMisses.store(Data[0], std::memory_order_relaxed);
	for (int32 I = 0; I < 4; ++I)
	{
		PublishedFirstPageTags[I].store(Data[1 + I], std::memory_order_relaxed);
	}
	bMissStatsPublished.store(true, std::memory_order_release);
}

bool FVoxelRasterAtlasGpu::PollMissStats(FVoxelRasterAtlasMissStats& Out)
{
	check(IsInGameThread());
	// THE ACTUAL READBACK RUNS ON THE RENDER THREAD. IsReady() and Lock() are
	// RHI calls that assert IsInRenderingThread(); calling them from
	// TickStreaming crashed the run at 37 s with
	//   Assertion failed: IsInRenderingThread() [RHICommandList.h:5316]
	// This function now only consumes what the render thread published.
	if (!bMissStatsPublished.load(std::memory_order_acquire))
	{
		return false;
	}
	Out.Misses = PublishedMisses.load(std::memory_order_relaxed);
	for (int32 I = 0; I < 4; ++I)
	{
		Out.FirstPageTags[I] = PublishedFirstPageTags[I].load(std::memory_order_relaxed);
	}
	return true;
}

FVoxelRasterAtlasBindings FVoxelRasterAtlasGpu::Register(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	FRDGBufferRef Elev = nullptr;
	FRDGBufferRef Climate = nullptr;
	FRDGBufferRef Miss = nullptr;
	FRDGBufferRef Tags = EnsureBuffers_RenderThread(GraphBuilder, Elev, Climate, Miss);

	FVoxelRasterAtlasBindings Out;
	Out.ElevationMm = GraphBuilder.CreateSRV(Elev);
	Out.ClimatePacked = GraphBuilder.CreateSRV(Climate);
	Out.PageTags = GraphBuilder.CreateSRV(Tags);
	Out.MissStats = GraphBuilder.CreateUAV(Miss);
	Out.PagePx = PagePx;
	Out.PagesDim = PagesDim;
	return Out;
}

void FVoxelRasterAtlasGpu::ReleaseResources_RenderThread()
{
	check(IsInRenderingThread());
	PooledElevation.SafeRelease();
	PooledClimate.SafeRelease();
	PooledTags.SafeRelease();
	PooledMissStats.SafeRelease();
	MissReadback.Reset();
	bMissReadbackInFlight.store(false);
}
