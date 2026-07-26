#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuWorldGenGraph.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Misc/ScopeExit.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuMeshJob, Log, All);

// Wave D / D2. Which MeshEmitMain permutation the async runner dispatches.
//
// This is an A/B, not a feature flag, and it is the one that matters: at 1 the
// GPU emits chunk-local quads and RebaseQuadsToChunkLocal never runs; at 0 the
// GPU emits brick-local quads and the CPU rebases them exactly as it did
// before D2. Both are supposed to produce byte-identical results, and
// voxel.GPU.VerifyAsyncMesh compares BOTH against MeshChunkBricks — so
// flipping this and re-running is the whole correctness argument for the
// permutation, runnable in one session on one binary.
static int32 GVoxelGpuMeshChunkLocal = 1;
static FAutoConsoleVariableRef CVarVoxelGpuMeshChunkLocal(
	TEXT("voxel.GPU.MeshChunkLocal"),
	GVoxelGpuMeshChunkLocal,
	TEXT("1 = the GPU emits chunk-local quads (VXC_MESH_CHUNK_LOCAL permutation, no CPU rebase). ")
	TEXT("0 = the GPU emits brick-local quads and FVoxelGpuMeshJobManager rebases them on the CPU. ")
	TEXT("Default 1. Read once per Submit, so it takes effect on the next job."),
	ECVF_Default);

const TCHAR* LexToString(EVoxelGpuMeshJobStatus Status)
{
	switch (Status)
	{
	case EVoxelGpuMeshJobStatus::Success:        return TEXT("Success");
	case EVoxelGpuMeshJobStatus::Rejected:       return TEXT("Rejected");
	case EVoxelGpuMeshJobStatus::DispatchFailed: return TEXT("DispatchFailed");
	case EVoxelGpuMeshJobStatus::ReadbackFailed: return TEXT("ReadbackFailed");
	case EVoxelGpuMeshJobStatus::TimedOut:       return TEXT("TimedOut");
	case EVoxelGpuMeshJobStatus::Cancelled:      return TEXT("Cancelled");
	}
	return TEXT("<unknown>");
}

void VoxelGpuChunkRegion::SetChunkFootprint(FVoxelGpuRegionRequest& Req,
                                            int32 ChunkX, int32 ChunkY, int32 ChunkZ)
{
	Req.DispatchColumns = FUintVector2(kColumns, kColumns);

	// One brick of halo on the negative side; the 48-column width supplies the
	// other one on the positive side.
	Req.OriginVx = ChunkX * int32(kChunkEdgeVoxels) - int32(kBrickEdge);
	Req.OriginVy = ChunkY * int32(kChunkEdgeVoxels) - int32(kBrickEdge);

	// A chunk is 4 bricks tall, so its first brick is at brick-z ChunkZ*4; back
	// off one for the halo.
	Req.BrickZMin = ChunkZ * 4 - 1;
	Req.BricksZ = kBricksZ;

	Req.bMeshChain = true;
}

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------

namespace
{
	// Where a job is. Written by whichever thread owns the transition and read by
	// the other, so it is an atomic with acquire/release ordering rather than a
	// plain int -- Error and the staging arrays are published by the same
	// release-store that moves the state, and read after the matching acquire.
	// Where a job is.
	//
	// WAVE D / D3 SPLIT THIS IN TWO. There used to be one round trip: dispatch,
	// then read back Counts + Offsets + the whole upper-bound quad buffer and
	// work out afterwards how much of it was live. That is ~810 KB per chunk to
	// recover ~7-12 KB of quads (see VoxelQuadScan.usf for the arithmetic and
	// why it, not the kernel, sets streaming throughput).
	//
	// Now frame N reads back FOUR BYTES -- the scan total -- and frame N+k reads
	// back exactly that many quads. Two round trips instead of one, which costs
	// latency and saves ~70-100x the bandwidth. That is the right trade for a
	// streaming path whose in-flight cap already absorbs latency, and it is
	// transitional either way: once the pool's buffers are UAV-writable and
	// RDG-registered (D1), phase 2 becomes a GPU->GPU copy into the allocated
	// range and reads back NOTHING.
	enum class EJobState : int32
	{
		Queued = 0,      // game thread owns it
		Dispatched,      // graph executed; the 4-byte total readback is pending
		TotalDone,       // total has landed; the game thread must start phase 2
		QuadsDispatched, // phase 2 enqueued; the sized quad readback is pending
		ReadbackDone,    // render thread has copied everything out
		Failed,          // render thread gave up; Error says why
	};
}

struct FVoxelGpuMeshJobManager::FJob
{
	uint64 JobId = 0;
	uint64 UserTag = 0;

	// OWNED. The render thread reads this; nothing else may mutate it after
	// Submit. This is the field that would be a dangling reference if it were
	// captured the way RunRegionBlocking captures its request.
	FVoxelGpuRegionRequest Region;

	VoxelGpuWorldGen::FRegionGraphSizes Sizes;
	uint32 InteriorX = 0;
	uint32 InteriorY = 0;
	uint32 InteriorZ = 0;

	// Created and destroyed on the render thread, kept alive by this object's
	// refcount for as long as any render command can still touch them.
	//
	// TotalReadback is phase 1 and always present. QuadsReadback is phase 2.
	// Counts/Offsets are only created for the brick-local control path, which
	// is the one that still has to rebase on the CPU and therefore still needs
	// the per-mask tables; the chunk-local path never reads them.
	TUniquePtr<FRHIGPUBufferReadback> TotalReadback;
	TUniquePtr<FRHIGPUBufferReadback> CountsReadback;
	TUniquePtr<FRHIGPUBufferReadback> OffsetsReadback;
	TUniquePtr<FRHIGPUBufferReadback> QuadsReadback;

	// The quad buffer, kept alive ACROSS GRAPHS. Today's Voxel.Quads is an RDG
	// transient and dies at GraphBuilder.Execute(); phase 2 runs in a different
	// graph a frame or more later, so it has to outlive the first one. This is
	// the reference that makes that true, and dropping it is what frees the
	// memory.
	TRefCountPtr<FRDGPooledBuffer> QuadBuffer;

	// Staged by the render thread, consumed by the game thread after State goes
	// to ReadbackDone.
	TArray<uint32> Counts;
	TArray<uint32> Offsets;
	TArray<uint64> RawQuads;
	// What QuadTotalMain said, i.e. how many quads phase 2 actually fetches.
	uint32 NumQuads = 0;
	FString Error;

	std::atomic<int32> State{ int32(EJobState::Queued) };
	// Set by the game thread before enqueuing a poll, cleared by the render
	// thread when that poll has looked at this job. Stops polls from stacking up
	// when the render thread is behind.
	std::atomic<int32> PollPending{ 0 };
	// Set by the game thread when it has given up on this job (timeout /
	// cancellation). The render thread checks it so a late poll does no work.
	std::atomic<int32> Abandoned{ 0 };
	// Set by the game thread when it has enqueued phase 2 for this job. The
	// state does not move to QuadsDispatched until the render command runs, so
	// without this the next tick would see TotalDone again and enqueue a second
	// fetch — two readbacks racing to fill one array.
	std::atomic<int32> QuadFetchStarted{ 0 };

	double SubmitSeconds = 0.0;
	double DispatchSeconds = 0.0;
	double ReadySeconds = 0.0;

	void SetState(EJobState New) { State.store(int32(New), std::memory_order_release); }
	EJobState GetState() const { return EJobState(State.load(std::memory_order_acquire)); }
};

namespace
{
	using FJobPtr = TSharedPtr<FVoxelGpuMeshJobManager::FJob, ESPMode::ThreadSafe>;

	// Copies one readback out. Returns false and fills OutError on any failure,
	// so a partial copy can never be mistaken for a good one.
	bool CopyReadback(FRHIGPUBufferReadback& Readback, void* Dest, uint32 Bytes,
	                  const TCHAR* Name, FString& OutError)
	{
		if (Bytes == 0)
		{
			return true;
		}
		const void* Src = Readback.Lock(Bytes);
		if (Src == nullptr)
		{
			OutError = FString::Printf(TEXT("%s readback lock returned null"), Name);
			return false;
		}
		FMemory::Memcpy(Dest, Src, Bytes);
		Readback.Unlock();
		return true;
	}

	// Releases a job's readbacks on the render thread. Capturing the job by
	// shared pointer is what keeps it alive until the command runs -- the
	// manager may well have forgotten about it by then.
	void ReleaseReadbacksOnRenderThread(FJobPtr Job)
	{
		if (!Job.IsValid())
		{
			return;
		}
		ENQUEUE_RENDER_COMMAND(VoxelGpuMeshReleaseReadbacks)(
			[Job](FRHICommandListImmediate&)
		{
			Job->TotalReadback.Reset();
			Job->CountsReadback.Reset();
			Job->OffsetsReadback.Reset();
			Job->QuadsReadback.Reset();
			// Frees the persistent quad buffer back to RDG's pool. Deliberately
			// on the render thread with everything else: this is an RHI resource
			// reference and it is the last thing holding it.
			Job->QuadBuffer.SafeRelease();
		});
	}

	// maskIndex = meshBrickIndex * 48 + axis * 16 + dir * 8 + slice, and the
	// interior bricks of a chunk-sized region ARE the chunk's 4x4x4 bricks in
	// the same x-fastest order MeshChunkBricks walks. So interior brick
	// (ix,iy,iz) sits at chunk-local voxel origin (ix*8, iy*8, iz*8) and the
	// rebase is a straight add on the three packed position fields.
	//
	// Ported from RebaseQuadsToRegionLocal in VoxelGpuVerify.cpp, with the
	// difference that this targets CHUNK-local coordinates (0..31, what the pool
	// and the CPU mesher use) rather than region-local: the chunk's first brick
	// is interior brick (0,0,0), whose region-local origin is (8,8,8), so the
	// +1-brick term that version adds is exactly what has to come back out.
	TArray<uint64> RebaseQuadsToChunkLocal(const FVoxelGpuMeshJobManager::FJob& Job)
	{
		TArray<uint64> Rebased;
		Rebased.Reserve(Job.RawQuads.Num());

		const uint32 InteriorX = Job.InteriorX;
		const uint32 InteriorY = Job.InteriorY;

		for (int32 MaskIndex = 0; MaskIndex < Job.Counts.Num(); ++MaskIndex)
		{
			const uint32 Count = Job.Counts[MaskIndex];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = Job.Offsets[MaskIndex];

			const uint32 MeshBrickIndex = uint32(MaskIndex) / 48u;
			const uint32 Ix = MeshBrickIndex % InteriorX;
			const uint32 Iy = (MeshBrickIndex / InteriorX) % InteriorY;
			const uint32 Iz = MeshBrickIndex / (InteriorX * InteriorY);

			const uint32 BrickOrigin[3] = { Ix * 8u, Iy * 8u, Iz * 8u };

			for (uint32 Q = 0; Q < Count; ++Q)
			{
				const int32 SrcIndex = int32(Start + Q);
				if (!Job.RawQuads.IsValidIndex(SrcIndex))
				{
					// Cannot happen unless the scan is corrupt; the caller checks
					// NumQuads against MaxQuads first. Bail rather than read past
					// the end.
					return Rebased;
				}
				const uint64 Packed = Job.RawQuads[SrcIndex];
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);

				const uint32 Axis  =  W0        & 0xfu;
				const uint32 Dir   = (W0 >>  4) & 0xfu;
				const uint32 Slice = (W0 >>  8) & 0xffu;
				const uint32 U0    = (W0 >> 16) & 0xffu;
				const uint32 V0    = (W0 >> 24) & 0xffu;

				const uint32 U = (Axis + 1u) % 3u;
				const uint32 V = (Axis + 2u) % 3u;

				const uint32 NewW0 = Axis | (Dir << 4)
				                   | ((Slice + BrickOrigin[Axis]) << 8)
				                   | ((U0    + BrickOrigin[U])    << 16)
				                   | ((V0    + BrickOrigin[V])    << 24);
				Rebased.Add(uint64(NewW0) | (uint64(W1) << 32));
			}
		}
		return Rebased;
	}
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

FVoxelGpuMeshJobManager::FVoxelGpuMeshJobManager(FVoxelGpuMeshJobComplete OnComplete,
                                                 int32 InMaxInFlight,
                                                 double InTimeoutSeconds)
	: OnJobComplete(MoveTemp(OnComplete))
	, MaxInFlight(FMath::Max(1, InMaxInFlight))
	, TimeoutSeconds(FMath::Max(0.1, InTimeoutSeconds))
{
	// The "cannot lose a job" guarantee is only a guarantee if there is somewhere
	// to deliver to. Failing here is far better than discovering it as a chunk
	// that never arrives.
	checkf(OnJobComplete.IsBound(),
	       TEXT("FVoxelGpuMeshJobManager requires a bound completion delegate — every job must be deliverable"));
}

FVoxelGpuMeshJobManager::~FVoxelGpuMeshJobManager()
{
	CancelAll();
}

uint64 FVoxelGpuMeshJobManager::Submit(FVoxelGpuRegionRequest&& Region, uint64 UserTag)
{
	check(IsInGameThread());

	FJobPtr Job = MakeShared<FJob, ESPMode::ThreadSafe>();
	Job->JobId = NextJobId++;
	Job->UserTag = UserTag;
	Job->Region = MoveTemp(Region);
	// Latched per job rather than read at delivery: a cvar flip between
	// dispatch and readback would otherwise rebase quads that were already
	// rebased on the GPU, which is silent and looks like a mesher bug.
	Job->Region.bChunkLocalQuads = GVoxelGpuMeshChunkLocal != 0;
	Job->SubmitSeconds = FPlatformTime::Seconds();

	Queued.Add(Job);
	return Job->JobId;
}

void FVoxelGpuMeshJobManager::Deliver(const FJobPtr& Job, EVoxelGpuMeshJobStatus Status, const FString& Error)
{
	FVoxelGpuMeshJobResult Result;
	Result.JobId = Job->JobId;
	Result.UserTag = Job->UserTag;
	Result.Status = Status;
	Result.Error = Error;
	Result.SubmitToDeliverMs = (FPlatformTime::Seconds() - Job->SubmitSeconds) * 1000.0;
	if (Job->ReadySeconds > 0.0 && Job->DispatchSeconds > 0.0)
	{
		Result.DispatchToReadyMs = (Job->ReadySeconds - Job->DispatchSeconds) * 1000.0;
	}

	if (Status == EVoxelGpuMeshJobStatus::Success)
	{
		// Under the D2 permutation the shader has already baked each brick's
		// chunk-local origin into the quad positions, so the stream is
		// pool-ready and the CPU rebase would double-apply the offset. The
		// brick-local branch below is kept, not vestigial: it is the control
		// voxel.GPU.MeshChunkLocal 0 selects, and the two must agree.
		Result.Quads = Job->Region.bChunkLocalQuads
			? MoveTemp(Job->RawQuads)
			: RebaseQuadsToChunkLocal(*Job);
	}

	OnJobComplete.ExecuteIfBound(MoveTemp(Result));
}

void FVoxelGpuMeshJobManager::Tick()
{
	check(IsInGameThread());

	// --- 1. promote queued jobs, one RDG graph for the whole batch ----------
	//
	// Rejections are collected rather than delivered inline, for the same
	// reentrancy reason as the harvest below.
	TArray<FJobPtr> Batch;
	TArray<TPair<FJobPtr, FString>> Rejected;
	while (Queued.Num() > 0 && InFlight.Num() + Batch.Num() < MaxInFlight)
	{
		FJobPtr Job = Queued[0];
		Queued.RemoveAt(0, EAllowShrinking::No);

		FString ValidationError;
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			Rejected.Emplace(Job, TEXT("Requires SM6 (64-bit integer shader ops). Relaunch with -sm6."));
			continue;
		}
		if (!VoxelGpuWorldGen::ValidateRegionRequest(Job->Region, ValidationError))
		{
			Rejected.Emplace(Job, ValidationError);
			continue;
		}
		if (!Job->Region.bMeshChain)
		{
			Rejected.Emplace(Job, TEXT("bMeshChain must be true — this manager exists to produce quads"));
			continue;
		}

		Job->Sizes = VoxelGpuWorldGen::ComputeRegionGraphSizes(Job->Region);
		Job->InteriorX = Job->Sizes.BricksX - 2;
		Job->InteriorY = Job->Sizes.BricksY - 2;
		Job->InteriorZ = Job->Sizes.BricksZ - 2;

		Batch.Add(MoveTemp(Job));
	}

	if (Batch.Num() > 0)
	{
		InFlight.Append(Batch);
		DispatchBatch(MoveTemp(Batch));
	}

	for (const TPair<FJobPtr, FString>& R : Rejected)
	{
		Deliver(R.Key, EVoxelGpuMeshJobStatus::Rejected, R.Value);
	}

	// --- 2. poll and harvest ------------------------------------------------
	PollInFlight();
}

void FVoxelGpuMeshJobManager::DispatchBatch(TArray<FJobPtr>&& Batch)
{
	// ONE graph for the whole batch. Every job's seven passes plus its three
	// readback copies go into the same FRDGBuilder, which is executed once.
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshDispatchBatch)(
		[Jobs = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// Jobs that made it into the graph. Anything that falls out below is
		// terminated here and now with a Failed state, so no job can leave this
		// command still sitting in Queued.
		TArray<FJobPtr> Built;
		Built.Reserve(Jobs.Num());

		for (const FJobPtr& Job : Jobs)
		{
			if (Job->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}

			const VoxelGpuWorldGen::FRegionGraphResources Graph =
				VoxelGpuWorldGen::AddRegionPasses(GraphBuilder, Job->Region);

			if (Graph.Quads == nullptr || Graph.Counts == nullptr ||
			    Graph.Offsets == nullptr || Graph.Total == nullptr)
			{
				Job->Error = TEXT("mesh chain produced no quad buffers");
				Job->SetState(EJobState::Failed);
				continue;
			}

			// The quad buffer has to survive this graph: phase 2 fetches from
			// it in a later one. Everything else here dies at Execute, as it
			// should.
			Job->QuadBuffer = GraphBuilder.ConvertToExternalBuffer(Graph.Quads);

			// PHASE 1 READS FOUR BYTES. That is the whole point of D3 — see the
			// EJobState comment.
			Job->TotalReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.QuadTotal"));
			AddEnqueueCopyPass(GraphBuilder, Job->TotalReadback.Get(), Graph.Total, sizeof(uint32));

			// ...except on the brick-local control path, which rebases on the
			// CPU and so genuinely needs the per-mask tables. Kept honest
			// rather than lean: voxel.GPU.MeshChunkLocal 0 is meant to be the
			// PREVIOUS behaviour, and that included these reads.
			if (!Job->Region.bChunkLocalQuads)
			{
				Job->CountsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Counts"));
				Job->OffsetsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Offsets"));

				AddEnqueueCopyPass(GraphBuilder, Job->CountsReadback.Get(), Graph.Counts,
				                   Job->Sizes.CountsBytes());
				AddEnqueueCopyPass(GraphBuilder, Job->OffsetsReadback.Get(), Graph.Offsets,
				                   Job->Sizes.CountsBytes());
			}

			Built.Add(Job);
		}

		// No SubmitAndBlockUntilGPUIdle. Execute records the work and returns;
		// the readbacks land whenever the GPU gets to them, and Tick's poll is
		// what notices.
		GraphBuilder.Execute();

		const double Now = FPlatformTime::Seconds();
		for (const FJobPtr& Job : Built)
		{
			Job->DispatchSeconds = Now;
			Job->SetState(EJobState::Dispatched);
		}
	});
}

// PHASE 2. The total has landed, so fetch exactly that many quads out of the
// buffer phase 1 kept alive.
//
// THIS IS THE TRANSITIONAL HALF OF D3. Once the pool's buffers are UAV-writable
// and RDG-registered (D1), this stops being a readback at all: it becomes
// Pool.Alloc(NumQuads) plus a GPU->GPU copy into the allocated range, the quad
// stream never touches system memory, and delivery happens off the 4-byte total
// alone. The structure here is deliberately the shape that change slots into —
// one place that turns a known quad count into a destination.
void FVoxelGpuMeshJobManager::DispatchQuadFetch(TArray<FJobPtr>&& Batch)
{
	ENQUEUE_RENDER_COMMAND(VoxelGpuMeshFetchQuads)(
		[Jobs = MoveTemp(Batch)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		TArray<FJobPtr> Built;
		Built.Reserve(Jobs.Num());

		for (const FJobPtr& Job : Jobs)
		{
			if (Job->Abandoned.load(std::memory_order_acquire) != 0)
			{
				continue;
			}
			if (!Job->QuadBuffer.IsValid())
			{
				Job->Error = TEXT("quad buffer did not survive phase 1");
				Job->SetState(EJobState::Failed);
				continue;
			}

			FRDGBufferRef Quads = GraphBuilder.RegisterExternalBuffer(
				Job->QuadBuffer, TEXT("Voxel.Async.QuadsPersistent"));

			// Exactly the live range. Offsets are exclusive-scanned from zero,
			// so the live quads are [QuadWriteBase, QuadWriteBase + NumQuads)
			// and the base is zero on every path today — the copy starts at 0
			// and the harvest trims any prefix.
			const uint32 Bytes =
				(Job->Sizes.QuadWriteBase + Job->NumQuads) * uint32(sizeof(uint64));

			Job->QuadsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Quads"));
			AddEnqueueCopyPass(GraphBuilder, Job->QuadsReadback.Get(), Quads, Bytes);

			Built.Add(Job);
		}

		GraphBuilder.Execute();

		for (const FJobPtr& Job : Built)
		{
			Job->SetState(EJobState::QuadsDispatched);
		}
	});
}

void FVoxelGpuMeshJobManager::PollInFlight()
{
	const double Now = FPlatformTime::Seconds();

	// --- poll both pending phases in one render command ---------------------
	TArray<FJobPtr> ToPoll;
	for (const FJobPtr& Job : InFlight)
	{
		const EJobState State = Job->GetState();
		if (State != EJobState::Dispatched && State != EJobState::QuadsDispatched)
		{
			continue;
		}
		int32 Expected = 0;
		if (Job->PollPending.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel))
		{
			ToPoll.Add(Job);
		}
	}

	if (ToPoll.Num() > 0)
	{
		ENQUEUE_RENDER_COMMAND(VoxelGpuMeshPoll)(
			[Jobs = MoveTemp(ToPoll)](FRHICommandListImmediate&)
		{
			for (const FJobPtr& Job : Jobs)
			{
				ON_SCOPE_EXIT { Job->PollPending.store(0, std::memory_order_release); };

				if (Job->Abandoned.load(std::memory_order_acquire) != 0)
				{
					continue;
				}

				const EJobState State = Job->GetState();

				// --- phase 1: the 4-byte total (+ the control path's tables) --
				if (State == EJobState::Dispatched)
				{
					const bool bNeedTables = !Job->Region.bChunkLocalQuads;
					if (!Job->TotalReadback.IsValid() ||
					    (bNeedTables && (!Job->CountsReadback.IsValid() ||
					                     !Job->OffsetsReadback.IsValid())))
					{
						Job->Error = TEXT("phase 1 readback objects missing");
						Job->SetState(EJobState::Failed);
						continue;
					}
					// All or none: a total that landed without its tables would
					// have the control path rebasing against stale offsets.
					if (!Job->TotalReadback->IsReady() ||
					    (bNeedTables && (!Job->CountsReadback->IsReady() ||
					                     !Job->OffsetsReadback->IsReady())))
					{
						continue;
					}

					// The GPU number. This is what D3 exists to fetch, and it
					// is 4 bytes.
					Job->ReadySeconds = FPlatformTime::Seconds();

					FString Error;
					bool bOk = CopyReadback(*Job->TotalReadback, &Job->NumQuads,
					                        sizeof(uint32), TEXT("QuadTotal"), Error);

					if (bOk && bNeedTables)
					{
						const uint32 MaskCount = Job->Sizes.MaskCount;
						Job->Counts.SetNumUninitialized(int32(MaskCount));
						Job->Offsets.SetNumUninitialized(int32(MaskCount));
						bOk = CopyReadback(*Job->CountsReadback, Job->Counts.GetData(),
						                   Job->Sizes.CountsBytes(), TEXT("Counts"), Error) &&
						      CopyReadback(*Job->OffsetsReadback, Job->Offsets.GetData(),
						                   Job->Sizes.CountsBytes(), TEXT("Offsets"), Error);
					}

					if (!bOk)
					{
						Job->Error = Error;
						Job->SetState(EJobState::Failed);
						continue;
					}
					Job->SetState(EJobState::TotalDone);
					continue;
				}

				// --- phase 2: exactly NumQuads quads --------------------------
				if (State == EJobState::QuadsDispatched)
				{
					if (!Job->QuadsReadback.IsValid())
					{
						Job->Error = TEXT("phase 2 readback object missing");
						Job->SetState(EJobState::Failed);
						continue;
					}
					if (!Job->QuadsReadback->IsReady())
					{
						continue;
					}

					const uint32 Elements = Job->Sizes.QuadWriteBase + Job->NumQuads;
					Job->RawQuads.SetNumUninitialized(int32(Elements));

					FString Error;
					if (!CopyReadback(*Job->QuadsReadback, Job->RawQuads.GetData(),
					                  Elements * uint32(sizeof(uint64)), TEXT("Quads"), Error))
					{
						Job->Error = Error;
						Job->SetState(EJobState::Failed);
						continue;
					}
					Job->SetState(EJobState::ReadbackDone);
				}
			}
		});
	}

	// --- start phase 2 for anything whose total has landed -------------------
	//
	// Game thread, because it is the thread that owns the decision and because
	// it is where the "is this count even sane" check belongs. Batched into one
	// render command for the same reason phase 1 is.
	{
		TArray<FJobPtr> ToFetch;
		for (const FJobPtr& Job : InFlight)
		{
			if (Job->GetState() != EJobState::TotalDone)
			{
				continue;
			}
			int32 Expected = 0;
			if (!Job->QuadFetchStarted.compare_exchange_strong(Expected, 1, std::memory_order_acq_rel))
			{
				continue;
			}

			if (Job->NumQuads > Job->Sizes.MaxQuads)
			{
				// Caught HERE rather than after a fetch, because the fetch would
				// be sized from this number: a corrupt total would ask for a
				// copy far larger than the buffer.
				Job->Error = FString::Printf(
					TEXT("QuadTotalMain reports %u quads but the buffer holds at most %u"),
					Job->NumQuads, Job->Sizes.MaxQuads);
				Job->SetState(EJobState::Failed);
				continue;
			}

			if (Job->NumQuads == 0)
			{
				// An all-air or all-solid chunk. Nothing to fetch, so it skips
				// phase 2 entirely and delivers a frame earlier — which is not
				// an edge case at level 0, where most of the vertical stack is
				// one or the other.
				Job->RawQuads.Reset();
				Job->SetState(EJobState::ReadbackDone);
				continue;
			}

			ToFetch.Add(Job);
		}

		if (ToFetch.Num() > 0)
		{
			DispatchQuadFetch(MoveTemp(ToFetch));
		}
	}

	// Harvest, in two phases.
	//
	// PHASE 1 decides and detaches; PHASE 2 delivers. They are separate because a
	// completion callback is free to call back into the manager -- Submit is the
	// obvious one, but CancelAll or destroying the manager are both legitimate
	// reactions to a failure, and either would mutate InFlight underneath a loop
	// that was still iterating it. Detaching first means the delivery loop owns
	// its jobs outright and nothing it triggers can invalidate them.
	struct FPending
	{
		FJobPtr Job;
		EVoxelGpuMeshJobStatus Status;
		FString Error;
	};
	TArray<FPending> Finished;

	for (int32 I = InFlight.Num() - 1; I >= 0; --I)
	{
		const FJobPtr Job = InFlight[I];
		const EJobState State = Job->GetState();

		if (State == EJobState::ReadbackDone)
		{
			// NumQuads came off the GPU in phase 1 and was range-checked before
			// phase 2 was sized from it, so there is nothing left to derive
			// here. On the brick-local control path the per-mask tables are
			// present too and RebaseQuadsToChunkLocal walks them; on the
			// chunk-local path they were never read.
			InFlight.RemoveAt(I, EAllowShrinking::No);

			// Drop the QuadWriteBase prefix, leaving exactly this job's live
			// quads. The prefix is empty on every path today.
			if (Job->Sizes.QuadWriteBase > 0 && Job->RawQuads.Num() > 0)
			{
				Job->RawQuads.RemoveAt(0, int32(Job->Sizes.QuadWriteBase), EAllowShrinking::No);
			}
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::Success, FString() });
			continue;
		}

		if (State == EJobState::Failed)
		{
			InFlight.RemoveAt(I, EAllowShrinking::No);
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::DispatchFailed, Job->Error });
			continue;
		}

		if (Now - Job->SubmitSeconds > TimeoutSeconds)
		{
			// Device loss, a wedged queue, or a render thread that never ran the
			// command. Give up on it, but keep the job object alive for any
			// render command still holding a reference.
			Job->Abandoned.store(1, std::memory_order_release);
			InFlight.RemoveAt(I, EAllowShrinking::No);
			Finished.Add({ Job, EVoxelGpuMeshJobStatus::TimedOut,
				FString::Printf(TEXT("readback not ready after %.1f s"), TimeoutSeconds) });
		}
	}

	for (const FPending& P : Finished)
	{
		Deliver(P.Job, P.Status, P.Error);
		ReleaseReadbacksOnRenderThread(P.Job);
	}
}

void FVoxelGpuMeshJobManager::CancelAll()
{
	TArray<FJobPtr> Outstanding;
	Outstanding.Append(Queued);
	Outstanding.Append(InFlight);
	Queued.Reset();
	InFlight.Reset();

	for (const FJobPtr& Job : Outstanding)
	{
		Job->Abandoned.store(1, std::memory_order_release);
		Deliver(Job, EVoxelGpuMeshJobStatus::Cancelled, TEXT("manager shut down"));
		ReleaseReadbacksOnRenderThread(Job);
	}
}
