// Asynchronous GPU meshing of chunk regions (ADR-0006, G3 stage 1).
//
// WHAT THIS IS FOR. VoxelGpuWorldGen::RunRegionBlocking runs the same kernels,
// but it calls SubmitAndBlockUntilGPUIdle and FlushRenderingCommands and is
// documented as a verification tool. This class runs the identical seven-pass
// graph (literally the same AddRegionPasses function) with NO blocking call
// anywhere: jobs are dispatched into an RDG graph, their results are staged
// into FRHIGPUBufferReadback objects, and Tick() polls IsReady() until they
// land.
//
// TWO INVARIANTS ARE LOAD-BEARING, AND BOTH ARE THINGS THE BLOCKING PATH GETS
// AWAY WITH BECAUSE IT FLUSHES.
//
// 1. Every job OWNS everything it hands the render thread. RunRegionBlocking
//    captures its request and its output arrays BY REFERENCE into the render
//    command; that is only safe because the calling stack frame is parked in
//    FlushRenderingCommands until the command has run. Here the caller returns
//    immediately, so a job is a shared, ref-counted object that owns its
//    request, its readbacks and its staging arrays, and the render commands
//    hold a reference to it. Nothing is captured by reference.
//
// 2. Every dispatch produces EXACTLY ONE outcome. A streaming system that can
//    lose a job strands the terrain column that was waiting on it, and there is
//    no signal — the chunk simply never arrives. So there is no "returns false
//    on error" path here: Submit() always returns a job id, and every job id is
//    eventually delivered to OnJobComplete exactly once, whether it succeeded,
//    failed validation, failed to dispatch, timed out, or was cancelled by
//    shutdown. Callers cannot drop a job by forgetting to check a return value,
//    because there is no return value to forget.
//
// SCOPE. Stage 1 is the runner only. Nothing here is wired into DispatchJobs or
// the streaming path yet.
//
// WAVE D / D2 UPDATE. The brick-local -> chunk-local rebase no longer happens
// on the CPU by default: Submit sets FVoxelGpuRegionRequest::bChunkLocalQuads
// from voxel.GPU.MeshChunkLocal (default 1), which selects a MeshEmitMain
// permutation that bakes the offset in on the GPU. Setting that cvar to 0
// restores the CPU rebase, and it is a genuine control rather than a fallback:
// voxel.GPU.VerifyAsyncMesh byte-compares both against MeshChunkBricks, so the
// two paths agreeing is what makes the permutation trustworthy.
//
// The readbacks themselves are still here. Removing them needs the pool's
// buffers to be UAV-writable and RDG-registered (D1) and an allocation scheme
// that does not need the quad stream on the CPU at all (D3); until then this
// stages quads through system memory, which is correct but is NOT the
// throughput story Wave D exists for.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "VoxelGpuWorldGen.h"

// Geometry of the region that meshes exactly one 32^3 render chunk.
//
// A render chunk is 4x4x4 bricks. The mesher needs a one-brick halo on every
// side to read apron/AO neighbours, so the dispatch is 6x6x6 bricks = 48x48
// columns by 6 bricks in Z, and the 4x4x4 interior bricks it meshes ARE the
// chunk's bricks, in the chunk's own order.
//
// That gives MaskCount = 4*4*4*48 = 3,072, comfortably under the 65,536 masks
// the single-workgroup ScanSumsMain can scan, so a chunk never needs splitting
// into z-slabs the way the 64x64 bench fixtures would at depth.
namespace VoxelGpuChunkRegion
{
	inline constexpr uint32 kChunkEdgeVoxels = 32;
	inline constexpr uint32 kBrickEdge = 8;
	inline constexpr uint32 kColumns = 48;   // 32 + 2*8 halo
	inline constexpr uint32 kBricksZ = 6;    // 4 + 2 halo
	inline constexpr uint32 kInteriorBricks = 4;
	inline constexpr uint32 kMaskCount = kInteriorBricks * kInteriorBricks * kInteriorBricks * 48;

	// Sets DispatchColumns / OriginVx / OriginVy / BrickZMin / BricksZ on Req for
	// the given level-0 chunk key. The caller still has to fill the raster window
	// (RasterOriginPx, RasterSize, PixelSizeMm, ElevationMm, ClimatePacked) and
	// the seed -- that needs voxel-core, which this module deliberately does not
	// link.
	VOXELEARTHSHADERS_API void SetChunkFootprint(FVoxelGpuRegionRequest& Req,
	                                             int32 ChunkX, int32 ChunkY, int32 ChunkZ);
}

// Why a job ended. Exactly one of these is delivered per Submit().
enum class EVoxelGpuMeshJobStatus : uint8
{
	Success,
	// The request never reached the GPU: it failed ValidateRegionRequest, or the
	// RHI cannot run the kernels at all.
	Rejected,
	// The graph was built but something went wrong before the readback landed.
	DispatchFailed,
	// The readback was enqueued but Lock() failed or came back null.
	ReadbackFailed,
	// IsReady() never went true inside the timeout. This is what device loss
	// and a hung queue look like from here, and it is the reason a timeout
	// exists at all: without it those cases are indistinguishable from "still
	// working", which is exactly how a job gets silently lost.
	TimedOut,
	// The manager was shut down or destroyed with this job still outstanding.
	Cancelled,
};

VOXELEARTHSHADERS_API const TCHAR* LexToString(EVoxelGpuMeshJobStatus Status);

struct FVoxelGpuMeshJobResult
{
	uint64 JobId = 0;
	// Whatever the caller passed to Submit, echoed back untouched. The streaming
	// integration will put a packed chunk key here.
	uint64 UserTag = 0;

	EVoxelGpuMeshJobStatus Status = EVoxelGpuMeshJobStatus::Cancelled;
	FString Error;

	// CHUNK-LOCAL packed quads, byte-identical in layout to
	// PackVoxelChunkQuad(FVoxelChunkQuad) -- i.e. exactly what
	// UVoxelGpuPoolComponent::AddChunk consumes. Produced chunk-local by the
	// GPU under voxel.GPU.MeshChunkLocal 1, or rebased on the CPU in Tick under
	// 0; the contract on this field is the same either way.
	TArray<uint64> Quads;

	// Wall-clock, all measured from the game thread's point of view except
	// DispatchToReadyMs.
	//
	// DispatchToReadyMs is render-thread timestamps: from the moment
	// GraphBuilder.Execute() returned to the moment a poll first saw IsReady()
	// true. That is the number that describes the GPU, and it is quantised by
	// the poll interval (one game-thread tick).
	//
	// *** DO NOT COMPARE ACROSS D3; QUOTE SubmitToDeliverMs. ***
	//
	// D3 changed what this measures. It is now the moment the 4-BYTE TOTAL
	// landed — when the GPU finished the mesh chain. It used to be the moment
	// the whole ~810 KB readback landed, so it included a transfer that no
	// longer happens on this path.
	//
	// So this number WILL be lower after D3, and lower for a reason that has
	// nothing to do with the GPU being faster. A before/after quoted from this
	// field is a flattering artefact of a metric that changed definition, which
	// is exactly how this programme has previously published figures it then
	// had to retract. SubmitToDeliverMs covers both phases and is the honest
	// end-to-end figure; quote that one.
	double DispatchToReadyMs = 0.0;
	// Submit() to the OnJobComplete call. Includes queueing behind the in-flight
	// cap, the render thread being a frame behind, and the poll quantisation.
	double SubmitToDeliverMs = 0.0;

	bool IsOk() const { return Status == EVoxelGpuMeshJobStatus::Success; }
};

DECLARE_DELEGATE_OneParam(FVoxelGpuMeshJobComplete, FVoxelGpuMeshJobResult&&);

class VOXELEARTHSHADERS_API FVoxelGpuMeshJobManager
{
public:
	// OnComplete is required and is called for EVERY submitted job, on the game
	// thread, from inside Tick() (or from the destructor / Shutdown() for jobs
	// still outstanding at that point).
	//
	// MaxInFlight caps how many jobs have GPU work outstanding at once. Queued
	// jobs beyond that wait; they are not rejected.
	explicit FVoxelGpuMeshJobManager(FVoxelGpuMeshJobComplete OnComplete,
	                                 int32 InMaxInFlight = 8,
	                                 double InTimeoutSeconds = 10.0);

	// Delivers Cancelled for anything still outstanding. Does NOT flush the
	// render thread: any render command still in flight holds its own reference
	// to the job it touches, so it stays valid after this returns.
	~FVoxelGpuMeshJobManager();

	FVoxelGpuMeshJobManager(const FVoxelGpuMeshJobManager&) = delete;
	FVoxelGpuMeshJobManager& operator=(const FVoxelGpuMeshJobManager&) = delete;

	// Takes ownership of Region. Always succeeds in the sense that the returned
	// id WILL be reported to OnComplete exactly once -- a request that cannot
	// possibly run is reported as Rejected on the next Tick rather than refused
	// here. Game thread only.
	uint64 Submit(FVoxelGpuRegionRequest&& Region, uint64 UserTag = 0);

	// Promotes queued jobs, polls readbacks, delivers finished ones. Game thread
	// only. Cheap and safe to call every frame with nothing outstanding.
	void Tick();

	// Delivers Cancelled for every queued and in-flight job. Idempotent.
	void CancelAll();

	int32 NumQueued() const { return Queued.Num(); }
	int32 NumInFlight() const { return InFlight.Num(); }
	bool HasWork() const { return NumQueued() > 0 || NumInFlight() > 0; }

	int32 GetMaxInFlight() const { return MaxInFlight; }
	void SetMaxInFlight(int32 InMaxInFlight) { MaxInFlight = FMath::Max(1, InMaxInFlight); }

	// Defined entirely in the .cpp. Public only so the file-local rebase helper
	// can name it; nothing outside the implementation can do anything with it.
	struct FJob;

private:
	void DispatchBatch(TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>>&& Batch);
	// D3 phase 2: fetch exactly the quads the 4-byte total said exist. Becomes
	// a GPU->GPU copy into a pool range, with no readback, once D1 lands.
	void DispatchQuadFetch(TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>>&& Batch);
	void PollInFlight();
	void Deliver(const TSharedPtr<FJob, ESPMode::ThreadSafe>& Job,
	             EVoxelGpuMeshJobStatus Status, const FString& Error);

	FVoxelGpuMeshJobComplete OnJobComplete;
	int32 MaxInFlight = 8;
	double TimeoutSeconds = 10.0;
	uint64 NextJobId = 1;

	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> Queued;
	TArray<TSharedPtr<FJob, ESPMode::ThreadSafe>> InFlight;
};
