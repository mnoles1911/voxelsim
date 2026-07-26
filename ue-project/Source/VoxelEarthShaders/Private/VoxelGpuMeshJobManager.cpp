#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuWorldGenGraph.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Misc/ScopeExit.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuMeshJob, Log, All);

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
	enum class EJobState : int32
	{
		Queued = 0,      // game thread owns it
		Dispatched,      // render thread has executed the graph; readbacks pending
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
	TUniquePtr<FRHIGPUBufferReadback> CountsReadback;
	TUniquePtr<FRHIGPUBufferReadback> OffsetsReadback;
	TUniquePtr<FRHIGPUBufferReadback> QuadsReadback;

	// Staged by the render thread, consumed by the game thread after State goes
	// to ReadbackDone.
	TArray<uint32> Counts;
	TArray<uint32> Offsets;
	TArray<uint64> RawQuads;
	FString Error;

	std::atomic<int32> State{ int32(EJobState::Queued) };
	// Set by the game thread before enqueuing a poll, cleared by the render
	// thread when that poll has looked at this job. Stops polls from stacking up
	// when the render thread is behind.
	std::atomic<int32> PollPending{ 0 };
	// Set by the game thread when it has given up on this job (timeout /
	// cancellation). The render thread checks it so a late poll does no work.
	std::atomic<int32> Abandoned{ 0 };

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
			Job->CountsReadback.Reset();
			Job->OffsetsReadback.Reset();
			Job->QuadsReadback.Reset();
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
		Result.Quads = RebaseQuadsToChunkLocal(*Job);
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

			if (Graph.Quads == nullptr || Graph.Counts == nullptr || Graph.Offsets == nullptr)
			{
				Job->Error = TEXT("mesh chain produced no quad buffers");
				Job->SetState(EJobState::Failed);
				continue;
			}

			Job->CountsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Counts"));
			Job->OffsetsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Offsets"));
			Job->QuadsReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.Async.Quads"));

			AddEnqueueCopyPass(GraphBuilder, Job->CountsReadback.Get(), Graph.Counts,
			                   Job->Sizes.CountsBytes());
			AddEnqueueCopyPass(GraphBuilder, Job->OffsetsReadback.Get(), Graph.Offsets,
			                   Job->Sizes.CountsBytes());
			AddEnqueueCopyPass(GraphBuilder, Job->QuadsReadback.Get(), Graph.Quads,
			                   Job->Sizes.QuadsBytes());

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

void FVoxelGpuMeshJobManager::PollInFlight()
{
	const double Now = FPlatformTime::Seconds();

	// Ask the render thread to look at every job that is dispatched and does not
	// already have a poll outstanding. One command for all of them.
	TArray<FJobPtr> ToPoll;
	for (const FJobPtr& Job : InFlight)
	{
		if (Job->GetState() != EJobState::Dispatched)
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

				if (Job->Abandoned.load(std::memory_order_acquire) != 0 ||
				    Job->GetState() != EJobState::Dispatched)
				{
					continue;
				}
				if (!Job->CountsReadback.IsValid() || !Job->OffsetsReadback.IsValid() ||
				    !Job->QuadsReadback.IsValid())
				{
					Job->Error = TEXT("readback objects missing");
					Job->SetState(EJobState::Failed);
					continue;
				}
				// All three or none: a partially-landed set would produce a quad
				// stream indexed by stale offsets, which is worse than waiting.
				if (!Job->CountsReadback->IsReady() || !Job->OffsetsReadback->IsReady() ||
				    !Job->QuadsReadback->IsReady())
				{
					continue;
				}

				Job->ReadySeconds = FPlatformTime::Seconds();

				const uint32 MaskCount = Job->Sizes.MaskCount;
				const uint32 MaxQuads = Job->Sizes.MaxQuads;

				Job->Counts.SetNumUninitialized(int32(MaskCount));
				Job->Offsets.SetNumUninitialized(int32(MaskCount));
				Job->RawQuads.SetNumUninitialized(int32(MaxQuads));

				FString Error;
				const bool bOk =
					CopyReadback(*Job->CountsReadback, Job->Counts.GetData(),
					             Job->Sizes.CountsBytes(), TEXT("Counts"), Error) &&
					CopyReadback(*Job->OffsetsReadback, Job->Offsets.GetData(),
					             Job->Sizes.CountsBytes(), TEXT("Offsets"), Error) &&
					CopyReadback(*Job->QuadsReadback, Job->RawQuads.GetData(),
					             Job->Sizes.QuadsBytes(), TEXT("Quads"), Error);

				if (!bOk)
				{
					Job->Error = Error;
					Job->SetState(EJobState::Failed);
					continue;
				}
				Job->SetState(EJobState::ReadbackDone);
			}
		});
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
			// The scan is exclusive, so the live quad count is the last mask's
			// offset plus its own count.
			const uint32 MaskCount = Job->Sizes.MaskCount;
			const uint32 NumQuads = (MaskCount > 0)
				? Job->Offsets[int32(MaskCount) - 1] + Job->Counts[int32(MaskCount) - 1]
				: 0;

			InFlight.RemoveAt(I, EAllowShrinking::No);

			if (NumQuads > Job->Sizes.MaxQuads)
			{
				Finished.Add({ Job, EVoxelGpuMeshJobStatus::ReadbackFailed,
					FString::Printf(TEXT("scan reports %u quads but the buffer holds at most %u"),
					                NumQuads, Job->Sizes.MaxQuads) });
			}
			else
			{
				Job->RawQuads.SetNum(int32(NumQuads), EAllowShrinking::No);
				Finished.Add({ Job, EVoxelGpuMeshJobStatus::Success, FString() });
			}
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
