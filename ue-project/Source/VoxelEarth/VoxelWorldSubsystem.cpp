#include "VoxelWorldSubsystem.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelEarth.h"
#include "VoxelMeshTypes.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelWorldSubsystem.h / VoxelChunkComponent.h, both voxel-core-free).
#include "voxelcore/mesher.h"
#include "voxelcore/raycast.h"
#include "voxelcore/tiles.h"
#include "voxelcore/world.h"

#include "Components/SceneComponent.h"
#include "Containers/Queue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/ThreadSafeCounter.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Stats/Stats.h"
#include "Tasks/Task.h"
#include "UObject/UObjectGlobals.h"

#include <unordered_map>
#include <vector>

// VoxelCoords.h intentionally duplicates this constant (in UU) so it stays
// voxel-core-free; check the two never drift apart.
static_assert(vxc::kVoxelSizeMm == int32(VoxelCoords::VoxelSizeUU) * 10,
              "VoxelCoords::VoxelSizeUU (UE units) must track vxc::kVoxelSizeMm (mm)");

namespace
{
// Meshes one render chunk's 4x4x4 bricks via MaterialAt and bakes each
// brick's quad coordinates into chunk-local ones. Shared, unchanged, by both
// halves of the lock-free split (docs/m1-plan.md Stage 2 decisions table,
// "Worker threading" row): the worker-job path calls this with a sampler
// closed over GeneratedWorld only (pure function of seed); the game-thread
// edited-chunk path calls it with a sampler closed over World::materialAt
// (overlay-aware). Only the sampler differs -- the mesh-and-bake logic never
// drifts between the two.
template <typename MaterialFn>
void MeshChunkBricks(const VoxelCoords::FVoxelChunkKey& ChunkKey, const MaterialFn& MaterialAt, TArray<FVoxelChunkQuad>& OutQuads)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;

	std::vector<Quad> BrickQuads;
	for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
	{
		for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
		{
			for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
			{
				const int64 OriginVX = (int64(ChunkKey.X) * BricksPerChunk + Dx) * int64(B);
				const int64 OriginVY = (int64(ChunkKey.Y) * BricksPerChunk + Dy) * int64(B);
				const int64 OriginVZ = (int64(ChunkKey.Z) * BricksPerChunk + Dz) * int64(B);

				// Sampler valid on [-1,B]^3 (mesher.h contract): MaterialAt
				// reads straight across brick AND render-chunk borders via
				// the same deterministic function, so no neighbor data needs
				// to be materialized just to mesh this one brick.
				const auto Sampler = [&](int X, int Y, int Z) -> MaterialId
				{ return MaterialAt(OriginVX + X, OriginVY + Y, OriginVZ + Z); };

				BrickQuads.clear();
				meshBrick<B>(Sampler, BrickQuads);
				if (BrickQuads.empty())
				{
					continue;
				}

				// Bake this brick's position within the chunk into the
				// (already chunk-scale, uint8-safe: max 31) quad fields so
				// the scene proxy never needs to know about bricks.
				const int32 AxisOffset[3] = {Dx * B, Dy * B, Dz * B};
				OutQuads.Reserve(OutQuads.Num() + (int32)BrickQuads.size());
				for (const Quad& Q : BrickQuads)
				{
					const int32 U = (Q.axis + 1) % 3;
					const int32 V = (Q.axis + 2) % 3;
					FVoxelChunkQuad CQ;
					CQ.Axis = Q.axis;
					CQ.Positive = Q.positive;
					CQ.Slice = (uint8)(Q.slice + AxisOffset[Q.axis]);
					CQ.U0 = (uint8)(Q.u0 + AxisOffset[U]);
					CQ.V0 = (uint8)(Q.v0 + AxisOffset[V]);
					CQ.W = Q.w;
					CQ.H = Q.h;
					CQ.Ao = Q.ao;
					CQ.Mat = Q.mat;
					OutQuads.Add(CQ);
				}
			}
		}
	}
}
} // namespace

// Streaming bookkeeping types. File-scope (not exposed to the UHT-parsed
// header -- see VoxelWorldSubsystem.h comment on FVoxelWorldImpl).
namespace VoxelStreaming
{
struct FChunkRecord
{
	// Weak: once registered, the component is owned (GC-rooted) by
	// ChunkOwner's component list; this is a lookup handle, not ownership.
	TWeakObjectPtr<UVoxelChunkComponent> Component;

	// Bumped by MarkChunkDirtyForRemesh whenever an edit dirties this chunk.
	// A worker job snapshots the id at dispatch time; if it no longer
	// matches this value when the result is drained, the result is stale
	// (superseded by an edit while the job was in flight) and is discarded
	// rather than applied -- this IS the stale-result discard mechanism.
	uint64 GenerationId = 1;

	bool bJobInFlight = false;
};

struct FJobResult
{
	VoxelCoords::FVoxelChunkKey Key;
	uint64 GenerationId = 0;
	TArray<FVoxelChunkQuad> Quads;
};
} // namespace VoxelStreaming

// FVoxelWorldImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWorldSubsystem.h (UHT-parsed) never sees a voxel-core header. Also
// owns ALL Stage 2 streaming state (chunk records, pending-work queues, the
// worker-result MPSC queue, in-flight task handles): none of it is
// UE-reflection-visible, so it belongs behind the same PImpl boundary.
struct FVoxelWorldImpl
{
	explicit FVoxelWorldImpl(uint64 Seed)
		: Tiles(Seed)
		, Voxels(Seed, Tiles)
	{
	}

	vxc::SyntheticTileSampler Tiles;
	vxc::World<VoxelCoords::BrickEdgeVoxels> Voxels;

	// --- Stage 2 streaming state (docs/m1-plan.md Stage 2 decisions table) ---

	TMap<VoxelCoords::FVoxelChunkKey, VoxelStreaming::FChunkRecord> ChunkRecords;

	// Nearest-first priority queues. Sorted FARTHEST-first so Pop() (O(1),
	// removes from the back) always yields the nearest pending chunk.
	TArray<VoxelCoords::FVoxelChunkKey> PendingJobKeys;        // worker-dispatch pending (no edited brick)
	TArray<VoxelCoords::FVoxelChunkKey> PendingGameThreadKeys; // overlay-aware game-thread mesh pending (first load of an
	                                                            // edited chunk, or a post-edit dirty re-mesh)
	TArray<VoxelCoords::FVoxelChunkKey> PendingUnloadKeys;
	TSet<VoxelCoords::FVoxelChunkKey> PendingUnloadSet; // de-dupes PendingUnloadKeys across recomputes

	TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc> ResultsQueue;
	FThreadSafeCounter JobsInFlightCounter;
	TArray<UE::Tasks::TTask<void>> InFlightTasks; // only for a clean Deinitialize() wait; see WaitForInFlightTasks

	bool bHasRecomputed = false;
	VoxelCoords::FVoxelChunkKey LastAnchorChunk{};
	FVector LastAnchorLocation = FVector::ZeroVector;

	// Cumulative counters, logged periodically (LogVoxelEarth, every ~5s) --
	// never per-chunk (docs/m1-plan.md: "log cumulative ... periodically").
	int64 TotalChunksLoaded = 0;
	int64 TotalChunksUnloaded = 0;
	int64 TotalQuadsLoaded = 0;
	float LogTimerAccumSeconds = 0.f;

	void TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime);
	void WaitForInFlightTasks();

	// Dig/place (edit-log authority path). Game thread only.
	bool TryDig(const FVector& CameraLoc, const FVector& CameraDir);
	bool TryPlace(const FVector& CameraLoc, const FVector& CameraDir);

private:
	void RecomputeDesiredSet(const FVector& Anchor);
	void ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32& OutChunkZMin, int32& OutChunkZMax) const;
	void SortPendingQueues(const FVector& Anchor);
	void DispatchJobs();
	void DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainUnloads();
	void ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
	                      const VoxelCoords::FVoxelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec, TArray<FVoxelChunkQuad>&& Quads);
	void MarkChunkDirtyForRemesh(const VoxelCoords::FVoxelChunkKey& Key);
	void CollectDirtyChunks(int64 Vx, int64 Vy, int64 Vz, TSet<VoxelCoords::FVoxelChunkKey>& Out) const;
	bool ChunkHasEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const;
	vxc::RaycastHit CastFromCamera(const FVector& CameraLoc, const FVector& Dir) const;
	void MaybeLogCounters(float DeltaTime);
};

// --- streaming tick orchestration -------------------------------------------

void FVoxelWorldImpl::TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime)
{
	using namespace VoxelCoords;

	LastAnchorLocation = Anchor;

	// Recompute the desired set only when the anchor crosses into a new
	// render-chunk XY column (every ChunkEdgeUU = 3.2m of movement), not
	// every tick -- the ring-membership test itself (docs/m1-plan.md: "render
	// chunks (XY footprints) within 64m") is cheap per-candidate but touches
	// ~1300+ candidate footprints at a 64m/3.2m ratio, so gating it behind
	// anchor movement is what keeps this off the hot per-frame path.
	const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxel(Anchor));
	if (!bHasRecomputed || AnchorChunk.X != LastAnchorChunk.X || AnchorChunk.Y != LastAnchorChunk.Y)
	{
		RecomputeDesiredSet(Anchor);
		LastAnchorChunk = AnchorChunk;
		bHasRecomputed = true;
	}

	// Budgets (docs/m1-plan.md Stage 2 decisions table): jobs in flight
	// <=2xLogicalCores, chunk component applies <=8/frame, unloads <=4/frame,
	// edit re-meshes <=4/frame (also covers first load of an edited chunk --
	// see PendingGameThreadKeys comment above).
	DispatchJobs();
	DrainResults(Owner, Root, Material);
	DrainGameThreadMesh(Owner, Root, Material);
	DrainUnloads();

	InFlightTasks.RemoveAllSwap([](const UE::Tasks::TTask<void>& T) { return T.IsCompleted(); }, EAllowShrinking::No);

	MaybeLogCounters(DeltaTime);
}

void FVoxelWorldImpl::WaitForInFlightTasks()
{
	// Called from Deinitialize before Impl is torn down: worker jobs capture
	// raw pointers into Voxels.generated() and the results queue (see
	// DispatchJobs) rather than a ref-counted handle, on the assumption that
	// no job outlives Impl. This is what makes that assumption true.
	for (UE::Tasks::TTask<void>& Task : InFlightTasks)
	{
		Task.Wait();
	}
	InFlightTasks.Empty();
}

void FVoxelWorldImpl::MaybeLogCounters(float DeltaTime)
{
	LogTimerAccumSeconds += DeltaTime;
	if (LogTimerAccumSeconds < 5.0f)
	{
		return;
	}
	LogTimerAccumSeconds = 0.f;

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Voxel streaming: loaded=%lld unloaded=%lld quads=%lld tracked=%d jobsInFlight=%d pendingJobs=%d ")
	       TEXT("pendingGameThread=%d pendingUnload=%d"),
	       (long long)TotalChunksLoaded, (long long)TotalChunksUnloaded, (long long)TotalQuadsLoaded, ChunkRecords.Num(),
	       JobsInFlightCounter.GetValue(), PendingJobKeys.Num(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num());
}

// --- desired-set / hysteresis ------------------------------------------------

void FVoxelWorldImpl::ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32& OutChunkZMin, int32& OutChunkZMax) const
{
	using namespace VoxelCoords;

	// Cheap proxy for stage 1's exhaustive per-brick surfaceBrickRange (which
	// would cost B*B amplifier samples per candidate -- prohibitive across
	// ~1300+ candidate footprints per recompute): sample the amplifier
	// column at this footprint's 4 corners and take a +-1 render-chunk
	// margin around the resulting elevation range as slope safety. Generous
	// on purpose; the Z-extent algorithm isn't pinned by the decisions table
	// (only the XY radius test is), and this keeps recompute cost bounded.
	const int64 Vx0 = int64(ChunkX) * ChunkEdgeVoxels;
	const int64 Vx1 = Vx0 + ChunkEdgeVoxels - 1;
	const int64 Vy0 = int64(ChunkY) * ChunkEdgeVoxels;
	const int64 Vy1 = Vy0 + ChunkEdgeVoxels - 1;

	int64 TopVoxelMin = INT64_MAX, TopVoxelMax = INT64_MIN;
	const int64 CornersX[2] = {Vx0, Vx1};
	const int64 CornersY[2] = {Vy0, Vy1};
	for (int64 Cx : CornersX)
	{
		for (int64 Cy : CornersY)
		{
			const vxc::ColumnSample Col = Voxels.amplifier().column(Cx, Cy);
			const int64 Top = vxc::floorDiv(Col.surfaceMm, vxc::kVoxelSizeMm);
			TopVoxelMin = FMath::Min(TopVoxelMin, Top);
			TopVoxelMax = FMath::Max(TopVoxelMax, Top);
		}
	}

	// Corner-only sampling under-estimates interior extremes; the amplifier's
	// slope-scaled detail can add metres between corners. Missing chunks
	// BELOW the range are invisible (buried), so -1 suffices; missing chunks
	// ABOVE the range are holes in peaks, so take +2 (=6.4m) headroom. The
	// exact fix (workers compute their own z-range per footprint) is a stage
	// 3 refactor.
	OutChunkZMin = (int32)FloorDiv(TopVoxelMin, ChunkEdgeVoxels) - 1;
	OutChunkZMax = (int32)FloorDiv(TopVoxelMax, ChunkEdgeVoxels) + 2;
}

bool FVoxelWorldImpl::ChunkHasEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const
{
	// docs/m1-plan.md Stage 2 decisions table: "Chunks that contain ANY
	// edited brick (query World's editedBricks map by chunk range) are never
	// meshed by workers." The scan extends 1 brick beyond the chunk on every
	// axis because the mesher's apron reads 1 voxel across the chunk border:
	// a pristine chunk NEXT TO an edited brick must also take the
	// overlay-aware path, or its border faces cull against pre-edit data
	// (visible as a seam after unload/reload of a neighbor-of-edit chunk).
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;
	const vxc::ChunkMap<VoxelCoords::BrickEdgeVoxels>& Overlay = Voxels.editedBricks();
	if (Overlay.size() == 0)
	{
		return false; // fast path: nothing has ever been edited
	}
	for (int32 Dz = -1; Dz <= BricksPerChunk; ++Dz)
	{
		for (int32 Dy = -1; Dy <= BricksPerChunk; ++Dy)
		{
			for (int32 Dx = -1; Dx <= BricksPerChunk; ++Dx)
			{
				const vxc::BrickKey Key{ChunkKey.X * BricksPerChunk + Dx, ChunkKey.Y * BricksPerChunk + Dy,
				                         ChunkKey.Z * BricksPerChunk + Dz};
				if (Overlay.find(Key) != nullptr)
				{
					return true;
				}
			}
		}
	}
	return false;
}

void FVoxelWorldImpl::SortPendingQueues(const FVector& Anchor)
{
	const auto DistSq = [&Anchor](const VoxelCoords::FVoxelChunkKey& Key)
	{
		const double CenterX = (double(Key.X) + 0.5) * VoxelCoords::ChunkEdgeUU;
		const double CenterY = (double(Key.Y) + 0.5) * VoxelCoords::ChunkEdgeUU;
		const double CenterZ = (double(Key.Z) + 0.5) * VoxelCoords::ChunkEdgeUU;
		return FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) + FMath::Square(CenterZ - Anchor.Z);
	};
	// Descending: nearest ends up at the back, so Pop() (O(1)) always yields
	// the nearest-first priority order (decisions table: "Nearest-first
	// priority by distance^2").
	const auto Farthest = [&](const VoxelCoords::FVoxelChunkKey& A, const VoxelCoords::FVoxelChunkKey& B) { return DistSq(A) > DistSq(B); };
	PendingJobKeys.Sort(Farthest);
	PendingGameThreadKeys.Sort(Farthest);
}

void FVoxelWorldImpl::RecomputeDesiredSet(const FVector& Anchor)
{
	using namespace VoxelCoords;

	const double LoadRadiusUU = UVoxelWorldSubsystem::LoadRadiusMeters * 100.0;   // m -> UU (1m = 100UU)
	const double UnloadRadiusUU = UVoxelWorldSubsystem::UnloadRadiusMeters * 100.0;
	const double LoadRadiusSq = LoadRadiusUU * LoadRadiusUU;
	const double UnloadRadiusSq = UnloadRadiusUU * UnloadRadiusUU;

	// 1. Hysteresis exit: currently-tracked chunks that drifted beyond the
	// unload ring (80m) get queued for unload. Chunks that merely left the
	// load ring (64m) but are still inside 80m stay tracked untouched.
	for (const auto& Pair : ChunkRecords)
	{
		const FVoxelChunkKey& Key = Pair.Key;
		if (PendingUnloadSet.Contains(Key))
		{
			continue;
		}
		const double CenterX = (double(Key.X) + 0.5) * ChunkEdgeUU;
		const double CenterY = (double(Key.Y) + 0.5) * ChunkEdgeUU;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		if (DistSq > UnloadRadiusSq)
		{
			PendingUnloadKeys.Add(Key);
			PendingUnloadSet.Add(Key);
			PendingJobKeys.RemoveSingle(Key);
			PendingGameThreadKeys.RemoveSingle(Key);
		}
	}

	// 2. Hysteresis entry: XY footprints within the load ring (64m) that
	// aren't already tracked become newly tracked chunks, routed to the
	// worker queue or the game-thread queue depending on whether they
	// already contain an edited brick.
	const int32 ChunkSpan = FMath::CeilToInt32(LoadRadiusUU / ChunkEdgeUU) + 1;
	const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(WorldToVoxel(Anchor));

	for (int32 Cy = AnchorChunk.Y - ChunkSpan; Cy <= AnchorChunk.Y + ChunkSpan; ++Cy)
	{
		for (int32 Cx = AnchorChunk.X - ChunkSpan; Cx <= AnchorChunk.X + ChunkSpan; ++Cx)
		{
			const double CenterX = (double(Cx) + 0.5) * ChunkEdgeUU;
			const double CenterY = (double(Cy) + 0.5) * ChunkEdgeUU;
			const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
			if (DistSq > LoadRadiusSq)
			{
				continue;
			}

			int32 ChunkZMin, ChunkZMax;
			ComputeFootprintChunkZRange(Cx, Cy, ChunkZMin, ChunkZMax);

			for (int32 Cz = ChunkZMin; Cz <= ChunkZMax; ++Cz)
			{
				const FVoxelChunkKey Key{Cx, Cy, Cz};
				if (ChunkRecords.Contains(Key))
				{
					continue;
				}

				ChunkRecords.Add(Key);
				if (ChunkHasEditedBrick(Key))
				{
					PendingGameThreadKeys.Add(Key);
				}
				else
				{
					PendingJobKeys.Add(Key);
				}
			}
		}
	}

	SortPendingQueues(Anchor);
}

// --- budgeted drains ----------------------------------------------------

void FVoxelWorldImpl::DispatchJobs()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=2xLogicalCores jobs in
	// flight."
	const int32 MaxJobsInFlight = 2 * FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	while (JobsInFlightCounter.GetValue() < MaxJobsInFlight && PendingJobKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelChunkKey Key = PendingJobKeys.Pop(EAllowShrinking::No); // nearest (see SortPendingQueues)

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Key);
		if (!Rec)
		{
			continue; // left the desired set between recompute and dispatch
		}

		// Defensive re-check: an edit landing in this same tick, between
		// recompute and dispatch, may have made this chunk edited-only.
		if (ChunkHasEditedBrick(Key))
		{
			PendingGameThreadKeys.Add(Key);
			continue;
		}

		Rec->bJobInFlight = true;
		JobsInFlightCounter.Increment();

		const uint64 GenId = Rec->GenerationId;
		// Worker threading (decisions table): the job touches ONLY
		// GeneratedWorld (pure function of seed, lock-free) -- never World
		// (the overlay is not thread-safe and workers must never touch it).
		// GenPtr, QueuePtr and CounterPtr are raw pointers into Impl-owned
		// data; safe because Deinitialize waits for every in-flight task
		// (WaitForInFlightTasks) before Impl is destroyed.
		const vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>* GenPtr = &Voxels.generated();
		TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc>* QueuePtr = &ResultsQueue;
		FThreadSafeCounter* CounterPtr = &JobsInFlightCounter;

		UE::Tasks::TTask<void> Task = UE::Tasks::Launch(
			TEXT("VoxelChunkMeshJob"),
			[GenPtr, Key, GenId, QueuePtr, CounterPtr]()
			{
				VoxelStreaming::FJobResult Result;
				Result.Key = Key;
				Result.GenerationId = GenId;
				MeshChunkBricks(
					Key, [GenPtr](int64 X, int64 Y, int64 Z) { return GenPtr->materialAt(X, Y, Z); }, Result.Quads);
				QueuePtr->Enqueue(MoveTemp(Result));
				CounterPtr->Decrement();
			},
			UE::Tasks::ETaskPriority::BackgroundNormal);
		InFlightTasks.Add(MoveTemp(Task));
	}
}

void FVoxelWorldImpl::ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
                                       const VoxelCoords::FVoxelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec,
                                       TArray<FVoxelChunkQuad>&& Quads)
{
	if (Quads.Num() == 0)
	{
		// No visible geometry (fully buried chunk, or an edit carved away
		// the last exposed faces): drop any stale component instead of
		// spawning/keeping an empty one.
		if (UVoxelChunkComponent* Existing = Rec.Component.Get())
		{
			Existing->DestroyComponent();
		}
		Rec.Component = nullptr;
		return;
	}

	UVoxelChunkComponent* Comp = Rec.Component.Get();
	if (!Comp)
	{
		Comp = NewObject<UVoxelChunkComponent>(&Owner);
		Comp->SetupAttachment(&Root);
		Comp->SetRelativeLocation(VoxelCoords::ChunkOriginWorld(Key));
		Comp->SetMaterial(0, Material);
		Comp->RegisterComponent();
		Rec.Component = Comp;
		++TotalChunksLoaded;
	}
	TotalQuadsLoaded += Quads.Num();
	Comp->SetChunkQuads(MoveTemp(Quads), VoxelCoords::ChunkEdgeVoxels);
}

void FVoxelWorldImpl::DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=8 chunk component
	// applies/frame."
	constexpr int32 MaxApplies = 8;
	int32 Applied = 0;
	VoxelStreaming::FJobResult Result;
	while (Applied < MaxApplies && ResultsQueue.Dequeue(Result))
	{
		++Applied;

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Result.Key);
		// Stale-result discard: the chunk left the desired set entirely (no
		// record any more), or an edit re-mesh superseded this job while it
		// was in flight (GenerationId no longer matches the id the job was
		// dispatched with -- MarkChunkDirtyForRemesh bumped it).
		if (!Rec || Rec->GenerationId != Result.GenerationId)
		{
			continue;
		}

		Rec->bJobInFlight = false;
		ApplyMeshResult(Owner, Root, Material, Result.Key, *Rec, MoveTemp(Result.Quads));
	}
}

void FVoxelWorldImpl::DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material)
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 edit re-meshes/frame."
	// This budget covers both first-time load of a chunk that already
	// contains an edited brick (ChunkHasEditedBrick routed it here in
	// RecomputeDesiredSet/DispatchJobs) and a post-edit dirty re-mesh
	// (MarkChunkDirtyForRemesh) -- both use the identical overlay-aware
	// game-thread mesh path, so they share one budget rather than two.
	constexpr int32 MaxRemeshes = 4;
	int32 Count = 0;
	while (Count < MaxRemeshes && PendingGameThreadKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelChunkKey Key = PendingGameThreadKeys.Pop(EAllowShrinking::No); // nearest

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Key);
		if (!Rec)
		{
			continue; // left the desired set; doesn't consume the budget
		}
		++Count;

		TArray<FVoxelChunkQuad> Quads;
		MeshChunkBricks(
			Key, [this](int64 X, int64 Y, int64 Z) { return Voxels.materialAt(X, Y, Z); }, Quads);
		ApplyMeshResult(Owner, Root, Material, Key, *Rec, MoveTemp(Quads));
	}
}

void FVoxelWorldImpl::DrainUnloads()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 unloads/frame."
	constexpr int32 MaxUnloads = 4;
	int32 Count = 0;
	while (Count < MaxUnloads && PendingUnloadKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelChunkKey Key = PendingUnloadKeys.Pop(EAllowShrinking::No);
		PendingUnloadSet.Remove(Key);
		++Count;

		VoxelStreaming::FChunkRecord Rec;
		if (ChunkRecords.RemoveAndCopyValue(Key, Rec))
		{
			// Any worker job still in flight for this key keeps running to
			// completion (it can't be cancelled); when its result arrives,
			// DrainResults finds no record for the key and discards it.
			if (UVoxelChunkComponent* Comp = Rec.Component.Get())
			{
				Comp->DestroyComponent();
			}
			++TotalChunksUnloaded;
		}
	}
}

// --- dig / place (edit-log authority path) -----------------------------

vxc::RaycastHit FVoxelWorldImpl::CastFromCamera(const FVector& CameraLoc, const FVector& Dir) const
{
	const int64 OxMm = VoxelCoords::WorldToMm(CameraLoc.X);
	const int64 OyMm = VoxelCoords::WorldToMm(CameraLoc.Y);
	const int64 OzMm = VoxelCoords::WorldToMm(CameraLoc.Z);
	const double RangeMm = UVoxelWorldSubsystem::DigPlaceRangeMeters * 1000.0;
	const int64 DxMm = (int64)FMath::RoundToDouble(Dir.X * RangeMm);
	const int64 DyMm = (int64)FMath::RoundToDouble(Dir.Y * RangeMm);
	const int64 DzMm = (int64)FMath::RoundToDouble(Dir.Z * RangeMm);

	// World::materialAt is overlay-aware (docs/m1-plan.md Stage 2 decisions
	// table: "call vxc::raycastVoxels against the subsystem's
	// World::materialAt (game thread)"). Game thread only -- Voxels.overlay_
	// is not thread-safe.
	const auto MaterialFn = [this](int64 X, int64 Y, int64 Z) { return Voxels.materialAt(X, Y, Z); };
	return vxc::raycastVoxels(MaterialFn, OxMm, OyMm, OzMm, DxMm, DyMm, DzMm);
}

void FVoxelWorldImpl::CollectDirtyChunks(int64 Vx, int64 Vy, int64 Vz, TSet<VoxelCoords::FVoxelChunkKey>& Out) const
{
	using namespace VoxelCoords;

	// The mesher apron reads 1 voxel across a brick's (and therefore a
	// render chunk's) boundary. An edited voxel sitting on a render-chunk
	// border therefore also invalidates whichever neighbor chunk(s) share
	// that border -- including diagonal neighbors when the voxel sits on an
	// edge or corner of the chunk, since meshBrick's AO sampling reads the
	// diagonal apron cell too (voxelcore/mesher.h planeSolid(-1,-1) etc).
	const FVoxelChunkKey Base = ChunkKeyForVoxel(FVoxelCoord{Vx, Vy, Vz});
	const int64 Lx = FloorMod(Vx, ChunkEdgeVoxels);
	const int64 Ly = FloorMod(Vy, ChunkEdgeVoxels);
	const int64 Lz = FloorMod(Vz, ChunkEdgeVoxels);
	const int32 Ox = (Lx == 0) ? -1 : (Lx == ChunkEdgeVoxels - 1 ? 1 : 0);
	const int32 Oy = (Ly == 0) ? -1 : (Ly == ChunkEdgeVoxels - 1 ? 1 : 0);
	const int32 Oz = (Lz == 0) ? -1 : (Lz == ChunkEdgeVoxels - 1 ? 1 : 0);

	const int32 XSteps = Ox != 0 ? 2 : 1;
	const int32 YSteps = Oy != 0 ? 2 : 1;
	const int32 ZSteps = Oz != 0 ? 2 : 1;
	for (int32 Sx = 0; Sx < XSteps; ++Sx)
	{
		for (int32 Sy = 0; Sy < YSteps; ++Sy)
		{
			for (int32 Sz = 0; Sz < ZSteps; ++Sz)
			{
				FVoxelChunkKey Key = Base;
				if (Sx == 1) Key.X += Ox;
				if (Sy == 1) Key.Y += Oy;
				if (Sz == 1) Key.Z += Oz;
				Out.Add(Key);
			}
		}
	}
}

void FVoxelWorldImpl::MarkChunkDirtyForRemesh(const VoxelCoords::FVoxelChunkKey& Key)
{
	VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Key);
	if (!Rec)
	{
		// Not currently streamed/tracked: nothing to re-mesh right now. On a
		// later load, ChunkHasEditedBrick routes it correctly — its scan
		// extends one brick beyond the chunk, so border-neighbors of edits
		// also take the overlay-aware path (no stale-seam-on-reload case).
		return;
	}
	++Rec->GenerationId; // invalidates any in-flight/queued worker result for this chunk
	Rec->bJobInFlight = false;
	PendingJobKeys.RemoveSingle(Key); // a dirtied chunk is never worker-dispatched
	PendingGameThreadKeys.AddUnique(Key);
}

bool FVoxelWorldImpl::TryDig(const FVector& CameraLoc, const FVector& CameraDir)
{
	const FVector Dir = CameraDir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	const vxc::RaycastHit Hit = CastFromCamera(CameraLoc, Dir);
	if (!Hit.hit)
	{
		return false;
	}

	constexpr int64 R = UVoxelWorldSubsystem::DigSphereRadiusVoxels;
	constexpr double RadiusSq = double(R) * double(R);
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	// One World::applyEdit per touched brick (docs/m1-plan.md Stage 2
	// decisions table): collect every voxel in the sphere, grouped by brick.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (int64 Dz = -R; Dz <= R; ++Dz)
	{
		for (int64 Dy = -R; Dy <= R; ++Dy)
		{
			for (int64 Dx = -R; Dx <= R; ++Dx)
			{
				const double DistSq = double(Dx * Dx + Dy * Dy + Dz * Dz);
				if (DistSq > RadiusSq)
				{
					continue;
				}

				const int64 Vx = Hit.vx + Dx, Vy = Hit.vy + Dy, Vz = Hit.vz + Dz;
				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, vxc::MAT_AIR});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
			}
		}
	}

	if (EditsByBrick.empty())
	{
		return false;
	}

	for (auto& Entry : EditsByBrick)
	{
		Voxels.applyEdit(Entry.first, std::move(Entry.second));
	}

	for (const VoxelCoords::FVoxelChunkKey& Key : DirtyChunks)
	{
		MarkChunkDirtyForRemesh(Key);
	}
	SortPendingQueues(LastAnchorLocation);

	return true;
}

bool FVoxelWorldImpl::TryPlace(const FVector& CameraLoc, const FVector& CameraDir)
{
	const FVector Dir = CameraDir.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	const vxc::RaycastHit Hit = CastFromCamera(CameraLoc, Dir);
	if (!Hit.hit || Hit.faceAxis < 0)
	{
		return false; // no hit, or the ray started inside solid (no valid face)
	}

	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	const int64 Vx = Hit.px, Vy = Hit.py, Vz = Hit.pz;
	const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
	const int LocalX = (int)vxc::floorMod(Vx, B);
	const int LocalY = (int)vxc::floorMod(Vy, B);
	const int LocalZ = (int)vxc::floorMod(Vz, B);
	const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);

	Voxels.applyEdit(BKey, {vxc::EditCell{Cell, vxc::MAT_ROCK}});

	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
	CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
	for (const VoxelCoords::FVoxelChunkKey& Key : DirtyChunks)
	{
		MarkChunkDirtyForRemesh(Key);
	}
	SortPendingQueues(LastAnchorLocation);

	return true;
}

// UVoxelWorldSubsystem ----------------------------------------------------

UVoxelWorldSubsystem::UVoxelWorldSubsystem() = default;
UVoxelWorldSubsystem::~UVoxelWorldSubsystem() = default;
UVoxelWorldSubsystem::UVoxelWorldSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// docs/m1-plan.md decisions table: "seed from config (default 20260719)"
	// -- still fixed; config-driven seed selection is a later milestone.
	Impl = MakeUnique<FVoxelWorldImpl>(DefaultSeed);
}

void UVoxelWorldSubsystem::Deinitialize()
{
	if (Impl)
	{
		// Worker jobs hold raw pointers into Impl-owned data (DispatchJobs);
		// block until every in-flight job has finished before freeing it.
		Impl->WaitForInFlightTasks();
	}

	ChunkRoot = nullptr;
	ChunkOwner = nullptr;
	ChunkMaterial = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Stage 1 scope (deliverable 2): "On world begin play (game worlds
	// only)" -- skip Editor/Inactive/EditorPreview worlds so simply opening
	// the level editor never triggers generation or ticking.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// Resolve the terrain material once (deliverable 4: load by path,
	// fallback to the engine default material, never crash).
	ChunkMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
	if (ChunkMaterial == nullptr)
	{
		ChunkMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_VoxelTerrain not found at /Game/Voxel/M_VoxelTerrain -- using engine default material."));
	}

	// Single actor hosting every render-chunk component (unchanged from
	// stage 1); streaming now spawns/destroys UVoxelChunkComponents on it
	// every tick instead of a one-shot fixed-radius pass.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("Failed to spawn the voxel chunk owner actor; streaming will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelChunkOwner"));
#endif

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Voxel streaming initialized (seed %llu): load=%.0fm unload=%.0fm digRange=%.0fm"),
	       (unsigned long long)DefaultSeed, LoadRadiusMeters, UnloadRadiusMeters, DigPlaceRangeMeters);
}

void UVoxelWorldSubsystem::Tick(float DeltaTime)
{
	if (!Impl || !ChunkOwner || !ChunkRoot)
	{
		return;
	}

	// Streaming anchor (decisions table: "Track a streaming anchor each
	// tick"): the first local player's possessed pawn, falling back to the
	// world origin if there is none yet (e.g. before RestartPlayer runs).
	FVector Anchor = FVector::ZeroVector;
	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				Anchor = Pawn->GetActorLocation();
			}
		}
	}

	Impl->TickStreaming(Anchor, *ChunkOwner, *ChunkRoot, ChunkMaterial, DeltaTime);
}

TStatId UVoxelWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWorldSubsystem, STATGROUP_Tickables);
}

bool UVoxelWorldSubsystem::TryDig(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection)
{
	return Impl ? Impl->TryDig(CameraWorldLocation, CameraWorldDirection) : false;
}

bool UVoxelWorldSubsystem::TryPlace(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection)
{
	return Impl ? Impl->TryPlace(CameraWorldLocation, CameraWorldDirection) : false;
}

double UVoxelWorldSubsystem::GetSurfaceHeightUU(double WorldX, double WorldY) const
{
	if (!Impl)
	{
		return 0.0;
	}
	const int64 Vx = (int64)FMath::FloorToDouble(WorldX / VoxelCoords::VoxelSizeUU);
	const int64 Vy = (int64)FMath::FloorToDouble(WorldY / VoxelCoords::VoxelSizeUU);
	const vxc::ColumnSample Col = Impl->Voxels.amplifier().column(Vx, Vy);
	return double(Col.surfaceMm) / 10.0; // mm -> UU (1 UU = 10 mm)
}
