#include "VoxelWorldSubsystem.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelMeshTypes.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelWorldSubsystem.h / VoxelChunkComponent.h, both voxel-core-free).
#include "voxelcore/counters.h"
#include "voxelcore/hash.h"
#include "voxelcore/mesher.h"
#include "voxelcore/mips.h"
#include "voxelcore/raycast.h"
#include "voxelcore/tiles.h"
#include "voxelcore/world.h"

#include "Components/SceneComponent.h"
#include "Containers/Queue.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Materials/MaterialInterface.h"
#include "Stats/Stats.h"
#include "Tasks/Task.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
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
void MeshChunkBricks(const VoxelCoords::FVoxelChunkKey& ChunkKey, const MaterialFn& MaterialAt, TArray<FVoxelChunkQuad>& OutQuads,
                      vxc::Counters* PerfCounters = nullptr)
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

				// docs/debug-tooling-plan.md P1 "vxc::Counters": counted here
				// (around the UE layer's call into meshBrick), not inside
				// voxel-core itself -- bricksGenerated/cellsWritten count
				// every brick attempted (whether or not it emits quads: an
				// interior-solid brick still gets sampled/meshed), quads
				// counted separately below only when non-empty.
				if (PerfCounters)
				{
					PerfCounters->incBricksGenerated();
					PerfCounters->incCellsWritten(uint64_t(B) * B * B);
				}

				if (BrickQuads.empty())
				{
					continue;
				}

				if (PerfCounters)
				{
					PerfCounters->incQuadsEmitted(uint64_t(BrickQuads.size()));
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

// M2 mip sourcing (docs/m2-plan.md "Mip sourcing" row): a level-L (L>=1)
// chunk job builds its bricks via vxc::MipChain<8> over a PURE-generated
// level-0 source -- the lock-free rule holds unchanged (workers never touch
// World's overlay, only GeneratedWorld). A naive MipChain source that calls
// GeneratedWorld::makeBrick(key) fresh on every request would recompute the
// (8x8) amplifier column grid for every level-0 brick queried; within one
// job, a tall stack of level-0 bricks at the same (bx,by) XY footprint
// shares one column grid, so this small per-job LRU (capped, not one grid
// covering the whole job footprint -- that would front-load amplifier work
// for chunk regions the recursion may not even need) caches ColumnGrids by
// (bx,by) and is the thing that actually avoids the redundant recompute.
class FJobColumnGridCache
{
public:
	using GeneratedWorldT = vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>;

	FJobColumnGridCache(const GeneratedWorldT& InGen, vxc::Counters* InPerfCounters) : Gen(InGen), PerfCounters(InPerfCounters) {}

	const GeneratedWorldT::ColumnGrid& Get(int32 Bx, int32 By)
	{
		const uint64 PackedKey = (uint64(uint32(Bx)) << 32) | uint64(uint32(By));
		if (GeneratedWorldT::ColumnGrid* Found = Grids.Find(PackedKey))
		{
			Touch(PackedKey);
			return *Found;
		}
		if (Grids.Num() >= kMaxEntries)
		{
			Evict();
		}
		// docs/debug-tooling-plan.md P1 "vxc::Counters": one increment per
		// actual cache miss (LRU hits, the whole point of this cache, don't
		// recount) -- B*B amplifier column evaluations per miss, same unit as
		// the level-0 job's columnEvals counter.
		if (PerfCounters)
		{
			PerfCounters->incColumnEvals(uint64_t(VoxelCoords::BrickEdgeVoxels) * VoxelCoords::BrickEdgeVoxels);
		}
		GeneratedWorldT::ColumnGrid& NewGrid = Grids.Add(PackedKey, Gen.columns(Bx, By));
		RecencyOrder.Add(PackedKey);
		return NewGrid;
	}

private:
	void Touch(uint64 Key)
	{
		RecencyOrder.RemoveSingle(Key);
		RecencyOrder.Add(Key);
	}
	void Evict()
	{
		if (RecencyOrder.Num() == 0)
		{
			return;
		}
		const uint64 Oldest = RecencyOrder[0];
		RecencyOrder.RemoveAt(0);
		Grids.Remove(Oldest);
	}

	static constexpr int32 kMaxEntries = 64;
	const GeneratedWorldT& Gen;
	vxc::Counters* PerfCounters;
	TMap<uint64, GeneratedWorldT::ColumnGrid> Grids;
	TArray<uint64> RecencyOrder;
};

// Builds a per-job vxc::MipChain<8> whose level-0 source lazily materializes
// bricks from GeneratedWorld via the column-grid cache above, and returns a
// sampler closure (level-L absolute voxel coords -> MaterialId, valid on
// [-1,B]^3 across brick AND chunk borders, matching meshBrick's contract)
// bound to that chain at the requested Level. The returned std::function
// keeps the MipChain and column cache alive via shared_ptr captures, so the
// caller can pass the sampler straight into MeshChunkBricks without worrying
// about lifetime -- everything is freed once the sampler itself goes out of
// scope at the end of the worker job.
std::function<vxc::MaterialId(int64, int64, int64)> MakeLevelSampler(const vxc::GeneratedWorld<VoxelCoords::BrickEdgeVoxels>& Gen,
                                                                       int32 Level, vxc::Counters* PerfCounters)
{
	using namespace vxc;
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	struct FJobMipState
	{
		FJobMipState(const GeneratedWorld<B>& InGen, Counters* InPerfCounters)
			: ColumnCache(InGen, InPerfCounters)
			, Gen(InGen)
			, Chain(
				  [this](const BrickKey& Key) -> const Brick<B>*
				  {
					  auto Found = Level0Bricks.find(Key);
					  if (Found != Level0Bricks.end())
					  {
						  return &Found->second;
					  }
					  const auto& Grid = ColumnCache.Get(Key.x, Key.y);
					  auto [It, Inserted] = Level0Bricks.emplace(Key, Gen.makeBrick(Key, Grid));
					  (void)Inserted;
					  return &It->second;
				  })
		{
		}

		FJobColumnGridCache ColumnCache;
		const GeneratedWorld<B>& Gen;
		std::unordered_map<BrickKey, Brick<B>, BrickKeyHash> Level0Bricks;
		MipChain<B> Chain;
	};

	TSharedPtr<FJobMipState> State = MakeShared<FJobMipState>(Gen, PerfCounters);

	return [State, Level](int64 X, int64 Y, int64 Z) -> MaterialId
	{
		const BrickKey BKey{int32_t(floorDiv(X, B)), int32_t(floorDiv(Y, B)), int32_t(floorDiv(Z, B))};
		const Brick<B>* B0 = State->Chain.brick(Level, BKey);
		if (!B0)
		{
			return MAT_AIR;
		}
		const int Lx = int(floorMod(X, B));
		const int Ly = int(floorMod(Y, B));
		const int Lz = int(floorMod(Z, B));
		return B0->get(Lx, Ly, Lz);
	};
}

// Sized dig/place cube anchoring (m1-plan.md "Player experience decisions",
// "Dig sizes" / "Place" rows): the SizeVoxels^3 cube is centred on Anchor on
// the two axes tangent to the hit face (FaceAxis), and biased along the face
// axis by GrowAxisSign so the whole cube sits on one side of Anchor instead
// of straddling it -- dig grows INTO the terrain (GrowAxisSign ==
// hit.faceSign), place grows AWAY from it (GrowAxisSign == -hit.faceSign).
// FaceAxis == -1 (ray started inside solid; dig-only, TryPlace already
// rejects this case) falls back to centring on all three axes since there is
// no face to bias against.
void ComputeCubeMinCorner(int64 AnchorX, int64 AnchorY, int64 AnchorZ, int32 FaceAxis, int32 GrowAxisSign, int32 SizeVoxels,
                           int64& OutMinX, int64& OutMinY, int64& OutMinZ)
{
	const int64 Anchor[3] = {AnchorX, AnchorY, AnchorZ};
	int64 Min[3];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (Axis == FaceAxis && GrowAxisSign != 0)
		{
			Min[Axis] = (GrowAxisSign > 0) ? Anchor[Axis] : Anchor[Axis] - (int64)(SizeVoxels - 1);
		}
		else
		{
			Min[Axis] = Anchor[Axis] - (int64)(SizeVoxels / 2);
		}
	}
	OutMinX = Min[0];
	OutMinY = Min[1];
	OutMinZ = Min[2];
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

	// --- Chunk-state debug tint bookkeeping (docs/debug-tooling-plan.md P1,
	// mode 2 + voxel.Debug.ChunkStates) -- all in FVoxelWorldImpl::ElapsedSeconds
	// terms (a free-running clock since Initialize, not GetWorld()'s time).
	// Far-in-the-past defaults so a chunk that has never flashed reads as
	// fully decayed on the very first tint update.
	float LoadedAtSeconds = -1000.f;   // set when Component transitions null -> non-null (first apply)
	float RemeshedAtSeconds = -1000.f; // set on a genuine re-mesh (Component already existed, overlay-aware path)
	bool bHasOverlayBricks = false;    // this exact chunk (no border) currently owns >=1 edited brick
	int32 LastQuadCount = 0;           // for FVoxelWorldImpl::ResidentQuads bookkeeping on replace/unload
};

struct FJobResult
{
	VoxelCoords::FVoxelLevelChunkKey Key;
	uint64 GenerationId = 0;
	TArray<FVoxelChunkQuad> Quads;
	float JobMs = 0.f; // wall time inside the worker task body (docs/debug-tooling-plan.md P1 "Worker timings")
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
		// Fixed-size ring buffer (docs/debug-tooling-plan.md P1 "Worker
		// timings"): sized once here so DrainResults never touches TArray
		// growth on its hot path.
		WorkerJobMsWindow.Init(0.f, WorkerJobMsWindowSize);
	}

	vxc::SyntheticTileSampler Tiles;
	vxc::World<VoxelCoords::BrickEdgeVoxels> Voxels;

	// --- Stage 2 streaming state (docs/m1-plan.md Stage 2 decisions table);
	// M2 (docs/m2-plan.md "Ring streaming" row) generalizes every key here
	// from FVoxelChunkKey to FVoxelLevelChunkKey (level, chunk) -- level 0
	// keys behave identically to the pre-M2 single-ring scheme. ---

	TMap<VoxelCoords::FVoxelLevelChunkKey, VoxelStreaming::FChunkRecord> ChunkRecords;

	// Nearest-first-within-level, lower-level-wins-ties priority queues
	// (docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance").
	// Sorted so Pop() (O(1), removes from the back) always yields the
	// highest-priority pending chunk -- see SortPendingQueues.
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingJobKeys;        // worker-dispatch pending (level 0 w/o edited brick, or any level>=1)
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingGameThreadKeys; // overlay-aware game-thread mesh pending (level 0 only --
	                                                                 // wave 1 limitation, see MarkChunkDirtyForRemesh doc comment)
	TArray<VoxelCoords::FVoxelLevelChunkKey> PendingUnloadKeys;
	TSet<VoxelCoords::FVoxelLevelChunkKey> PendingUnloadSet; // de-dupes PendingUnloadKeys across recomputes

	TQueue<VoxelStreaming::FJobResult, EQueueMode::Mpsc> ResultsQueue;
	FThreadSafeCounter JobsInFlightCounter;
	TArray<UE::Tasks::TTask<void>> InFlightTasks; // only for a clean Deinitialize() wait; see WaitForInFlightTasks

	bool bHasRecomputed = false;
	VoxelCoords::FVoxelChunkKey LastAnchorChunk{}; // level 0 anchor chunk; gates the whole RecomputeDesiredSet call
	FVector LastAnchorLocation = FVector::ZeroVector;

	// Per-level entry-scan gating (docs/m2-plan.md "Perf budget" row: "Ring
	// levels ... are ~constant cost by construction"): re-running the O(candidates)
	// entry scan for every level on every level-0 chunk crossing (every 3.2m
	// of movement) would waste work for outer levels whose own chunk edge is
	// much larger -- level L only re-scans once the anchor has crossed into a
	// new LEVEL-L chunk. The hysteresis/unload pass (cheap: iterates existing
	// ChunkRecords, no candidate generation) still runs every call.
	bool bHasRecomputedLevel[VoxelCoords::kNumLevels] = {};
	VoxelCoords::FVoxelChunkKey LastAnchorChunkPerLevel[VoxelCoords::kNumLevels] = {};

	// Cumulative counters, logged periodically (LogVoxelPerf, every ~5s) --
	// never per-chunk (docs/m1-plan.md: "log cumulative ... periodically").
	int64 TotalChunksLoaded = 0;
	int64 TotalChunksUnloaded = 0;
	int64 TotalQuadsLoaded = 0;
	float LogTimerAccumSeconds = 0.f;

	// --- Debug-tooling instrumentation (docs/debug-tooling-plan.md P1) -------

	// Engine-free perf counters (voxel-core/include/voxelcore/counters.h),
	// incremented around this file's own calls into voxel-core (worker mesh
	// jobs, the edit-log apply path) -- voxel-core's internals stay untouched.
	vxc::Counters PerfCounters;

	// Free-running clock since Initialize (NOT GetWorld()'s time -- this impl
	// has no UWorld& handy outside Tick/TickStreaming's parameters), used only
	// for chunk-state tint flash decay timing.
	float ElapsedSeconds = 0.f;

	int64 ResidentQuads = 0;         // sum of FChunkRecord::LastQuadCount across every tracked record with a live component
	int64 StaleResultsDiscarded = 0; // cumulative worker results dropped (chunk left the desired set, or superseded by an edit)

	// Rolling window of per-chunk worker mesh-job milliseconds (P1 "Worker
	// timings"), filled on the game thread in DrainResults as results are
	// drained -- no cross-thread access, so a plain ring buffer suffices.
	static constexpr int32 WorkerJobMsWindowSize = 256;
	TArray<float> WorkerJobMsWindow;
	int32 WorkerJobMsWindowNext = 0;
	int32 WorkerJobMsWindowCount = 0;

	// This-tick budget-saturation fractions (P1 "budget saturation (% of
	// per-frame apply/unload/re-mesh budgets used)"), set by the three Drain*
	// functions and blended together once per tick in UpdatePerfSnapshot.
	float LastAppliedFrac = 0.f;
	float LastRemeshFrac = 0.f;
	float LastUnloadFrac = 0.f;
	float BudgetSaturationAccum = 0.f;
	int32 BudgetSaturationSamples = 0;

	// 1Hz refresh window (P1 "1Hz refresh of text, per-frame collection"):
	// per-frame accumulation happens continuously above; the published
	// snapshot itself only updates once this timer rolls over.
	float PerfRefreshAccumSeconds = 0.f;
	int64 LoadedAtLastPerfRefresh = 0;
	int64 UnloadedAtLastPerfRefresh = 0;
	FVoxelPerfSnapshot LastPerfSnapshot;

	// Tracks the previous tick's VoxelDebug::IsChunkStatesEnabled() /
	// IsRingsEnabled() so the off-transition can drop every component's MID
	// exactly once (constraint: zero MID cost once both layers are off
	// again).
	bool bChunkStatesWasEnabled = false;
	bool bRingsWasEnabled = false;

	void TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime);
	void WaitForInFlightTasks();

	// Dig/place (edit-log authority path). Game thread only.
	bool TryDig(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels);
	bool TryPlace(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, uint8 MaterialId,
	              const FVector& PlayerActorLocation);

	// Explosives v1 (edit-log authority path). Game thread only.
	int32 CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU);

	FVoxelPerfSnapshot GetPerfSnapshot() const { return LastPerfSnapshot; }

private:
	void RecomputeDesiredSet(const FVector& Anchor);
	void ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const;
	void SortPendingQueues(const FVector& Anchor);
	void DispatchJobs();
	void DrainResults(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainGameThreadMesh(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material);
	void DrainUnloads();
	void ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
	                      const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec, TArray<FVoxelChunkQuad>&& Quads,
	                      bool bIsGameThreadMesh);
	// Level 0 only (m2-plan.md "Distant edits" limitation -- see
	// MarkChunkDirtyForRemesh doc comment): edits, the overlay, and re-mesh
	// dirtying are all level-0 concepts in wave 1, so these keep taking a
	// plain (level-implied-0) FVoxelChunkKey.
	void MarkChunkDirtyForRemesh(const VoxelCoords::FVoxelChunkKey& Key);
	void CollectDirtyChunks(int64 Vx, int64 Vy, int64 Vz, TSet<VoxelCoords::FVoxelChunkKey>& Out) const;
	bool ChunkHasEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const;
	// Strict (no 1-brick border) variant used only for the "edited-chunk
	// orange tint" test -- ChunkHasEditedBrick's border scan is correct for
	// MESH ROUTING (a pristine chunk beside an edited one must also take the
	// overlay-aware path) but would tint chunks that merely neighbor an edit,
	// not own one.
	bool ChunkOwnsEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const;
	vxc::RaycastHit CastFromCamera(const FVector& CameraLoc, const FVector& Dir) const;
	void MaybeLogCounters(float DeltaTime);

	// Shared tail of TryDig/TryPlace/CarveSphere (edit-log authority path):
	// applies every brick's grouped edits, marks every dirty chunk for
	// re-mesh, and re-sorts the pending queues. No-op if EditsByBrick is
	// empty.
	void ApplyGroupedEdits(std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash>& EditsByBrick,
	                        const TSet<VoxelCoords::FVoxelChunkKey>& DirtyChunks);

	// --- Debug-tooling helpers (docs/debug-tooling-plan.md P1) ----------------
	void UpdatePerfSnapshot(float DeltaTime, float TickMs);
	void UpdateChunkStateTints();
	// M2 item 4 (docs/m2-plan.md): tints every loaded chunk by its ring
	// level instead of chunk state; see VoxelDebug::IsRingsEnabled.
	void UpdateRingTints();
	void DrawDebugBoundsLayer(UWorld& World, const FVector& Anchor) const;
};

// --- streaming tick orchestration -------------------------------------------

void FVoxelWorldImpl::TickStreaming(const FVector& Anchor, AActor& Owner, USceneComponent& Root, UMaterialInterface* Material, float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_VoxelSubsystemTick);
	const double TickStartSeconds = FPlatformTime::Seconds();

	using namespace VoxelCoords;

	LastAnchorLocation = Anchor;
	ElapsedSeconds += DeltaTime;

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

	// docs/debug-tooling-plan.md P1 "Stats group": always-on DWORD counters
	// (STATS macros already compile to nothing in Shipping; no extra gate).
	SET_DWORD_STAT(STAT_VoxelChunksLoaded, ChunkRecords.Num());
	SET_DWORD_STAT(STAT_VoxelChunksInFlight, JobsInFlightCounter.GetValue());

	MaybeLogCounters(DeltaTime);

	// Constraint: "keep all debug work zero-cost when voxel.Debug=0 (branch
	// out early)" -- FVoxelPerfSnapshot collection (array iteration, once/sec
	// sort) is real work beyond the always-on STATS macros above, so it's
	// gated on mode >= 1 (the perf HUD's own activation threshold) rather
	// than running unconditionally.
	if (VoxelDebug::GetDebugMode() >= 1)
	{
		const float TickMs = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
		UpdatePerfSnapshot(DeltaTime, TickMs);
	}

	// Visualizations (mode 2 only; every helper early-outs on its own cvar
	// check so this call is a single cheap branch when debug is off). M2
	// item 4: voxel.Debug.Rings tints by mip level instead of chunk state --
	// the two layers share one MID per component (UVoxelChunkComponent::
	// SetDebugTint has no concept of "which layer"), so Rings takes priority
	// when both happen to be enabled at once rather than fighting over the
	// tint every tick.
	const bool bRingsNow = VoxelDebug::IsRingsEnabled();
	const bool bChunkStatesNow = VoxelDebug::IsChunkStatesEnabled();
	if (bRingsNow)
	{
		UpdateRingTints();
	}
	else if (bChunkStatesNow)
	{
		UpdateChunkStateTints();
	}
	if (!bRingsNow && !bChunkStatesNow && (bRingsWasEnabled || bChunkStatesWasEnabled))
	{
		// Off-transition (both layers now off): drop every MID once
		// (constraint: zero MID cost with the layer off) rather than paying
		// for one until the chunk happens to re-mesh naturally.
		for (auto& Pair : ChunkRecords)
		{
			if (UVoxelChunkComponent* Comp = Pair.Value.Component.Get())
			{
				Comp->ClearDebugTint();
			}
		}
	}
	bRingsWasEnabled = bRingsNow;
	bChunkStatesWasEnabled = bChunkStatesNow;

	if (UWorld* World = Owner.GetWorld())
	{
		DrawDebugBoundsLayer(*World, Anchor);
	}
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

	// docs/debug-tooling-plan.md P1 "Log split": periodic counter line moved
	// off LogVoxelEarth (module/general) onto LogVoxelPerf.
	UE_LOG(LogVoxelPerf, Log,
	       TEXT("Voxel streaming: loaded=%lld unloaded=%lld quads=%lld tracked=%d jobsInFlight=%d pendingJobs=%d ")
	       TEXT("pendingGameThread=%d pendingUnload=%d"),
	       (long long)TotalChunksLoaded, (long long)TotalChunksUnloaded, (long long)TotalQuadsLoaded, ChunkRecords.Num(),
	       JobsInFlightCounter.GetValue(), PendingJobKeys.Num(), PendingGameThreadKeys.Num(), PendingUnloadKeys.Num());

	// M2 item 1: "Per-level loaded/pending counters into the perf snapshot/
	// HUD" -- also into this periodic log line, so a headless run's log file
	// alone (no HUD to screenshot) is enough to verify every ring level is
	// actually loading chunks.
	int32 LevelLoaded[VoxelCoords::kNumLevels] = {};
	int32 LevelPending[VoxelCoords::kNumLevels] = {};
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.Component.IsValid())
		{
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingJobKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingGameThreadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	UE_LOG(LogVoxelPerf, Log, TEXT("Voxel rings: R0 loaded=%d pending=%d | R1 loaded=%d pending=%d | R2 loaded=%d pending=%d | ")
	                          TEXT("R3 loaded=%d pending=%d | R4 loaded=%d pending=%d"),
	       LevelLoaded[0], LevelPending[0], LevelLoaded[1], LevelPending[1], LevelLoaded[2], LevelPending[2], LevelLoaded[3],
	       LevelPending[3], LevelLoaded[4], LevelPending[4]);
}

// --- desired-set / hysteresis ------------------------------------------------

void FVoxelWorldImpl::ComputeFootprintChunkZRange(int32 ChunkX, int32 ChunkY, int32 Level, int32& OutChunkZMin, int32& OutChunkZMax) const
{
	using namespace VoxelCoords;

	// Cheap proxy for stage 1's exhaustive per-brick surfaceBrickRange (which
	// would cost B*B amplifier samples per candidate -- prohibitive across
	// ~1300+ candidate footprints per recompute): sample the amplifier
	// column at this footprint's 4 corners and take a +-1 render-chunk
	// margin around the resulting elevation range as slope safety. Generous
	// on purpose; the Z-extent algorithm isn't pinned by the decisions table
	// (only the XY radius test is), and this keeps recompute cost bounded.
	//
	// M2: the amplifier always operates in LEVEL-0 (10cm) voxel units, so a
	// level-L footprint's corners are converted up to level-0 voxel units
	// before querying it, and the resulting level-0 top-voxel range is
	// converted back down to level-L chunk units afterward.
	const int64 LevelScale = int64(1) << Level;
	const int64 Vx0 = (int64(ChunkX) * ChunkEdgeVoxels) * LevelScale;
	const int64 Vx1 = Vx0 + ChunkEdgeVoxels * LevelScale - 1;
	const int64 Vy0 = (int64(ChunkY) * ChunkEdgeVoxels) * LevelScale;
	const int64 Vy1 = Vy0 + ChunkEdgeVoxels * LevelScale - 1;

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
	// ABOVE the range are holes in peaks, so take +2 headroom (in level-L
	// chunks -- generous by construction, since a level-L chunk is (1<<L)
	// times taller than a level-0 one). The exact fix (workers compute their
	// own z-range per footprint) is a stage 3 refactor.
	const int64 TopVoxelMinAtLevel = FloorDiv(TopVoxelMin, LevelScale);
	const int64 TopVoxelMaxAtLevel = FloorDiv(TopVoxelMax, LevelScale);
	OutChunkZMin = (int32)FloorDiv(TopVoxelMinAtLevel, ChunkEdgeVoxels) - 1;
	OutChunkZMax = (int32)FloorDiv(TopVoxelMaxAtLevel, ChunkEdgeVoxels) + 2;
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

bool FVoxelWorldImpl::ChunkOwnsEditedBrick(const VoxelCoords::FVoxelChunkKey& ChunkKey) const
{
	// docs/debug-tooling-plan.md P1 chunk-state tint: "edited-chunk orange
	// (persistent while it has overlay bricks)" -- unlike ChunkHasEditedBrick
	// (mesh-routing correctness, needs the 1-brick border), this is purely a
	// tint decision, so it only asks whether THIS chunk owns an edited brick.
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;
	const vxc::ChunkMap<VoxelCoords::BrickEdgeVoxels>& Overlay = Voxels.editedBricks();
	if (Overlay.size() == 0)
	{
		return false;
	}
	for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
	{
		for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
		{
			for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
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
	const auto DistSq = [&Anchor](const VoxelCoords::FVoxelLevelChunkKey& LevelKey)
	{
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(LevelKey.Level);
		const double CenterX = (double(LevelKey.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(LevelKey.Key.Y) + 0.5) * ChunkEdge;
		const double CenterZ = (double(LevelKey.Key.Z) + 0.5) * ChunkEdge;
		return FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y) + FMath::Square(CenterZ - Anchor.Z);
	};
	// docs/m2-plan.md item 1: "Budgets shared across levels, nearest-first
	// within level, lower level (finer) wins priority at equal distance."
	// Distance is the PRIMARY key -- "nearest-first" is the dominant clause,
	// and "lower level wins ... at equal distance" is explicitly a tie-break
	// ("at equal distance"), not an override. Level-as-primary-key was tried
	// first and measured wrong: it dispatches every level-0 job before a
	// single level-1 job, every level-1 before level-2, etc., so outer rings
	// never get a turn until the (much larger, in absolute chunk count)
	// inner rings fully drain -- R3/R4 stayed at 0 loaded chunks through a
	// 90s verification run despite thousands queued. Distance-primary lets
	// every ring populate outward from the camera roughly together, which is
	// both the visually-correct LOD behavior and the literal reading of the
	// decisions-table wording. Sort() wants a "less than" predicate
	// producing ascending order, and Pop() removes from the back, so the
	// LOWEST-priority element must sort first (front) and the
	// HIGHEST-priority element (nearest, level 0 on an exact tie) must sort
	// last (back) -- this predicate returns true when A has lower priority
	// than B.
	const auto Farthest = [&](const VoxelCoords::FVoxelLevelChunkKey& A, const VoxelCoords::FVoxelLevelChunkKey& B)
	{
		const double DistA = DistSq(A);
		const double DistB = DistSq(B);
		if (DistA != DistB)
		{
			return DistA > DistB; // farther = lower priority = sorts toward the front
		}
		return A.Level > B.Level; // exact-distance tie: higher level number = lower priority
	};
	PendingJobKeys.Sort(Farthest);
	PendingGameThreadKeys.Sort(Farthest);
}

void FVoxelWorldImpl::RecomputeDesiredSet(const FVector& Anchor)
{
	using namespace VoxelCoords;

	// 1. Hysteresis exit: currently-tracked chunks (any level) that drifted
	// beyond their level's unload ring, OR drifted inside their level's inner
	// edge (the next finer level has taken over -- no hysteresis on this
	// side in wave 1, see the doc comment on RingPresets in
	// VoxelWorldSubsystem.h: "hard boundary" per m2-plan.md's v0 decision),
	// get queued for unload. Chunks that merely left the load ring but are
	// still inside the unload ring stay tracked untouched -- this is the
	// same hysteresis gap the original single-ring code had, now evaluated
	// per level.
	for (const auto& Pair : ChunkRecords)
	{
		const FVoxelLevelChunkKey& LevelKey = Pair.Key;
		if (PendingUnloadSet.Contains(LevelKey))
		{
			continue;
		}
		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[LevelKey.Level];
		const double ChunkEdge = ChunkEdgeUUForLevel(LevelKey.Level);
		const double CenterX = (double(LevelKey.Key.X) + 0.5) * ChunkEdge;
		const double CenterY = (double(LevelKey.Key.Y) + 0.5) * ChunkEdge;
		const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
		const double UnloadOuterUU = Preset.OuterMeters * 100.0 * UVoxelWorldSubsystem::UnloadRingMultiplier;
		const double InnerUU = Preset.InnerMeters * 100.0;
		const bool bBeyondOuter = DistSq > FMath::Square(UnloadOuterUU);
		const bool bInsideInner = LevelKey.Level > 0 && DistSq < FMath::Square(InnerUU);
		if (bBeyondOuter || bInsideInner)
		{
			PendingUnloadKeys.Add(LevelKey);
			PendingUnloadSet.Add(LevelKey);
			PendingJobKeys.RemoveSingle(LevelKey);
			PendingGameThreadKeys.RemoveSingle(LevelKey);
		}
	}

	// 2. Hysteresis entry, per level: XY footprints within level L's annulus
	// [Inner(L), Outer(L)) that aren't already tracked become newly tracked
	// chunks. Level 0 routes to the worker queue or the game-thread queue
	// depending on whether it already contains an edited brick (unchanged
	// M1 behavior); levels >=1 always take the worker/MipChain path (m2-plan.md
	// "Distant edits" wave-1 limitation: higher levels render pure-generated
	// even where level-0 edits exist underneath them -- see MakeLevelSampler).
	//
	// Gated per level (bHasRecomputedLevel/LastAnchorChunkPerLevel): a level-L
	// chunk is (1<<L) times larger, so re-running this O(candidates) scan on
	// every level-0 chunk crossing (every 3.2m) would be wasted work for
	// outer levels once their own chunk hasn't actually changed.
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		const FVoxelCoord AnchorVoxel = WorldToVoxelForLevel(Anchor, Level);
		const FVoxelChunkKey AnchorChunk = ChunkKeyForVoxel(AnchorVoxel);
		if (bHasRecomputedLevel[Level] && AnchorChunk.X == LastAnchorChunkPerLevel[Level].X &&
		    AnchorChunk.Y == LastAnchorChunkPerLevel[Level].Y)
		{
			continue; // nothing new can have entered this level's annulus
		}
		LastAnchorChunkPerLevel[Level] = AnchorChunk;
		bHasRecomputedLevel[Level] = true;

		const UVoxelWorldSubsystem::FRingPreset& Preset = UVoxelWorldSubsystem::RingPresets[Level];
		const double InnerUU = Preset.InnerMeters * 100.0;
		const double OuterUU = Preset.OuterMeters * 100.0;
		const double ChunkEdge = ChunkEdgeUUForLevel(Level);
		const int32 ChunkSpan = FMath::CeilToInt32(OuterUU / ChunkEdge) + 1;

		for (int32 Cy = AnchorChunk.Y - ChunkSpan; Cy <= AnchorChunk.Y + ChunkSpan; ++Cy)
		{
			for (int32 Cx = AnchorChunk.X - ChunkSpan; Cx <= AnchorChunk.X + ChunkSpan; ++Cx)
			{
				const double CenterX = (double(Cx) + 0.5) * ChunkEdge;
				const double CenterY = (double(Cy) + 0.5) * ChunkEdge;
				const double DistSq = FMath::Square(CenterX - Anchor.X) + FMath::Square(CenterY - Anchor.Y);
				if (DistSq >= FMath::Square(OuterUU) || (Level > 0 && DistSq < FMath::Square(InnerUU)))
				{
					continue; // outside this level's annulus
				}

				int32 ChunkZMin, ChunkZMax;
				ComputeFootprintChunkZRange(Cx, Cy, Level, ChunkZMin, ChunkZMax);

				for (int32 Cz = ChunkZMin; Cz <= ChunkZMax; ++Cz)
				{
					const FVoxelLevelChunkKey LevelKey{Level, FVoxelChunkKey{Cx, Cy, Cz}};
					if (ChunkRecords.Contains(LevelKey))
					{
						continue;
					}

					ChunkRecords.Add(LevelKey);
					if (Level == 0 && ChunkHasEditedBrick(LevelKey.Key))
					{
						PendingGameThreadKeys.Add(LevelKey);
					}
					else
					{
						PendingJobKeys.Add(LevelKey);
					}
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
		const VoxelCoords::FVoxelLevelChunkKey LevelKey = PendingJobKeys.Pop(EAllowShrinking::No); // highest priority (see SortPendingQueues)

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
		if (!Rec)
		{
			continue; // left the desired set between recompute and dispatch
		}

		// Defensive re-check (level 0 only -- see MakeLevelSampler doc
		// comment on the wave-1 "higher levels never take the overlay path"
		// limitation): an edit landing in this same tick, between recompute
		// and dispatch, may have made this chunk edited-only.
		if (LevelKey.Level == 0 && ChunkHasEditedBrick(LevelKey.Key))
		{
			PendingGameThreadKeys.Add(LevelKey);
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
		vxc::Counters* PerfCountersPtr = &PerfCounters;

		UE::Tasks::TTask<void> Task = UE::Tasks::Launch(
			TEXT("VoxelChunkMeshJob"),
			[GenPtr, LevelKey, GenId, QueuePtr, CounterPtr, PerfCountersPtr]()
			{
				SCOPE_CYCLE_COUNTER(STAT_VoxelWorkerJob);
				const double JobStartSeconds = FPlatformTime::Seconds();

				VoxelStreaming::FJobResult Result;
				Result.Key = LevelKey;
				Result.GenerationId = GenId;

				const VoxelCoords::FVoxelChunkKey& Key = LevelKey.Key;
				if (LevelKey.Level == 0)
				{
					// Column-cache the whole job (docs/m1-plan.md stage 3a): a
					// naive GeneratedWorld::materialAt sampler recomputes the
					// full amplifier column per VOXEL query (~130k column
					// evaluations per chunk); this grid computes each of the
					// (32+2)^2 columns exactly once (~100x less amplifier work,
					// measured ~5 -> ~50+ chunks/s). The +1 apron matches the
					// mesher's [-1,B] sampler contract across chunk borders.
					// Level 0 keeps this exact, already-tuned fast path
					// unchanged rather than routing it through MipChain too
					// (a level-0 MipChain::brick(0,key) is just source_(key)
					// directly, so it would be equivalent but strictly slower
					// -- an extra Brick<8> materialization + get() indirection
					// per voxel instead of Amplifier::materialAt directly).
					constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
					constexpr int32 GridEdge = ChunkVox + 2;
					const int64 BaseVX = int64(Key.X) * ChunkVox;
					const int64 BaseVY = int64(Key.Y) * ChunkVox;
					TArray<vxc::ColumnSample> Columns;
					Columns.SetNumUninitialized(GridEdge * GridEdge);
					const vxc::Amplifier& Amp = GenPtr->amplifier();
					for (int32 LY = 0; LY < GridEdge; ++LY)
					{
						for (int32 LX = 0; LX < GridEdge; ++LX)
						{
							Columns[LX + GridEdge * LY] =
								Amp.column(BaseVX + LX - 1, BaseVY + LY - 1);
						}
					}
					const auto GridSampler = [&Columns, BaseVX, BaseVY](int64 X, int64 Y, int64 Z)
					{
						const int32 LX = int32(X - BaseVX) + 1;
						const int32 LY = int32(Y - BaseVY) + 1;
						checkSlow(LX >= 0 && LX < GridEdge && LY >= 0 && LY < GridEdge);
						return vxc::Amplifier::materialAt(Columns[LX + GridEdge * LY], Z);
					};
					// docs/debug-tooling-plan.md P1 "vxc::Counters": columnEvals
					// counts the explicit Amp.column() calls this cache-build loop
					// makes -- the ~100x-cheaper number the doc comment above
					// references, NOT a count of voxel-core-internal column work
					// (that stays uninstrumented this pass, per P1 scope).
					PerfCountersPtr->incColumnEvals(uint64_t(GridEdge) * GridEdge);
					MeshChunkBricks(Key, GridSampler, Result.Quads, PerfCountersPtr);
				}
				else
				{
					// M2 mip sourcing (docs/m2-plan.md "Mip sourcing" row):
					// level-L (L>=1) bricks come from a per-job vxc::MipChain<8>
					// over a pure-GeneratedWorld level-0 source -- see
					// MakeLevelSampler's doc comment for the column-grid LRU
					// that avoids the naive-recompute perf trap. Still lock-free
					// (GenPtr only, never World/the overlay); wave-1 limitation:
					// this NEVER checks for edited bricks, so higher levels
					// render pure-generated even where level-0 edits exist
					// underneath them (distant-edit mip propagation is a later
					// M2 item, docs/m2-plan.md "Distant edits" row).
					const auto LevelSampler = MakeLevelSampler(*GenPtr, LevelKey.Level, PerfCountersPtr);
					MeshChunkBricks(Key, LevelSampler, Result.Quads, PerfCountersPtr);
				}

				Result.JobMs = float((FPlatformTime::Seconds() - JobStartSeconds) * 1000.0);
				QueuePtr->Enqueue(MoveTemp(Result));
				CounterPtr->Decrement();
			},
			UE::Tasks::ETaskPriority::BackgroundNormal);
		InFlightTasks.Add(MoveTemp(Task));
	}
}

void FVoxelWorldImpl::ApplyMeshResult(AActor& Owner, USceneComponent& Root, UMaterialInterface* Material,
                                       const VoxelCoords::FVoxelLevelChunkKey& Key, VoxelStreaming::FChunkRecord& Rec,
                                       TArray<FVoxelChunkQuad>&& Quads, bool bIsGameThreadMesh)
{
	// docs/debug-tooling-plan.md P1 "Memory" row: ResidentQuads tracks
	// currently-loaded quads (not the cumulative TotalQuadsLoaded below), so
	// it must be decremented by whatever this record held before, regardless
	// of which branch below runs.
	ResidentQuads -= Rec.LastQuadCount;
	Rec.LastQuadCount = 0;

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
	const bool bWasFirstLoad = (Comp == nullptr);
	if (!Comp)
	{
		Comp = NewObject<UVoxelChunkComponent>(&Owner);
		Comp->SetupAttachment(&Root);
		Comp->SetLevel(Key.Level); // M2: fixed for the component's lifetime, see SetLevel doc comment
		Comp->SetRelativeLocation(VoxelCoords::ChunkOriginWorldForLevel(Key.Key, Key.Level));
		Comp->SetMaterial(0, Material);
		Comp->RegisterComponent();
		Rec.Component = Comp;
		++TotalChunksLoaded;
	}
	TotalQuadsLoaded += Quads.Num();
	Rec.LastQuadCount = Quads.Num();
	ResidentQuads += Rec.LastQuadCount;
	Comp->SetChunkQuads(MoveTemp(Quads), VoxelCoords::ChunkEdgeVoxels);

	// docs/debug-tooling-plan.md P1 chunk-state tints: "just-loaded" fires on
	// any first component creation (worker or game-thread path alike);
	// "re-mesh" only fires when this was a genuine re-mesh of an
	// already-resident chunk via the overlay-aware path (never the worker
	// path -- workers only ever run a chunk's FIRST mesh).
	if (bWasFirstLoad)
	{
		Rec.LoadedAtSeconds = ElapsedSeconds;
	}
	else if (bIsGameThreadMesh)
	{
		Rec.RemeshedAtSeconds = ElapsedSeconds;
	}
	// Overlay ownership is a level-0-only concept (m2-plan.md wave-1
	// limitation): higher levels never own edited bricks in their own
	// right (MakeLevelSampler never consults the overlay).
	Rec.bHasOverlayBricks = (Key.Level == 0) && ChunkOwnsEditedBrick(Key.Key);
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

		// docs/debug-tooling-plan.md P1 "Worker timings": recorded for every
		// drained result, even ones about to be discarded as stale below --
		// the worker did real, representative work regardless of whether the
		// result is still wanted.
		WorkerJobMsWindow[WorkerJobMsWindowNext] = Result.JobMs;
		WorkerJobMsWindowNext = (WorkerJobMsWindowNext + 1) % WorkerJobMsWindowSize;
		WorkerJobMsWindowCount = FMath::Min(WorkerJobMsWindowCount + 1, WorkerJobMsWindowSize);

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(Result.Key);
		// Stale-result discard: the chunk left the desired set entirely (no
		// record any more), or an edit re-mesh superseded this job while it
		// was in flight (GenerationId no longer matches the id the job was
		// dispatched with -- MarkChunkDirtyForRemesh bumped it).
		if (!Rec || Rec->GenerationId != Result.GenerationId)
		{
			++StaleResultsDiscarded;
			continue;
		}

		Rec->bJobInFlight = false;
		ApplyMeshResult(Owner, Root, Material, Result.Key, *Rec, MoveTemp(Result.Quads), /*bIsGameThreadMesh*/ false);
	}
	LastAppliedFrac = float(Applied) / float(MaxApplies);
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
		// Level 0 only in wave 1 (docs/m2-plan.md "Distant edits" limitation
		// -- see MarkChunkDirtyForRemesh): nothing ever pushes a level>=1 key
		// onto this queue.
		const VoxelCoords::FVoxelLevelChunkKey LevelKey = PendingGameThreadKeys.Pop(EAllowShrinking::No); // nearest
		checkSlow(LevelKey.Level == 0);

		VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
		if (!Rec)
		{
			continue; // left the desired set; doesn't consume the budget
		}
		++Count;

		SCOPE_CYCLE_COUNTER(STAT_VoxelGameThreadMesh);
		TArray<FVoxelChunkQuad> Quads;
		MeshChunkBricks(
			LevelKey.Key, [this](int64 X, int64 Y, int64 Z) { return Voxels.materialAt(X, Y, Z); }, Quads, &PerfCounters);
		ApplyMeshResult(Owner, Root, Material, LevelKey, *Rec, MoveTemp(Quads), /*bIsGameThreadMesh*/ true);
	}
	LastRemeshFrac = float(Count) / float(MaxRemeshes);
}

void FVoxelWorldImpl::DrainUnloads()
{
	// docs/m1-plan.md Stage 2 decisions table: "<=4 unloads/frame."
	constexpr int32 MaxUnloads = 4;
	int32 Count = 0;
	while (Count < MaxUnloads && PendingUnloadKeys.Num() > 0)
	{
		const VoxelCoords::FVoxelLevelChunkKey Key = PendingUnloadKeys.Pop(EAllowShrinking::No);
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
			ResidentQuads -= Rec.LastQuadCount;
			++TotalChunksUnloaded;
		}
	}
	LastUnloadFrac = float(Count) / float(MaxUnloads);
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
	// Level 0 only (docs/m2-plan.md "Distant edits" row, wave-1 limitation):
	// edits, the overlay, and re-mesh dirtying only ever touch the level-0
	// (true-voxel) ring in this wave. Higher levels render pure-generated
	// even where a level-0 edit exists underneath them -- propagating an
	// edit up the mip chain to invalidate/re-mesh overlapping level 1-4
	// chunks is a later M2 item (see MakeLevelSampler's doc comment).
	const VoxelCoords::FVoxelLevelChunkKey LevelKey{0, Key};
	VoxelStreaming::FChunkRecord* Rec = ChunkRecords.Find(LevelKey);
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
	PendingJobKeys.RemoveSingle(LevelKey); // a dirtied chunk is never worker-dispatched
	PendingGameThreadKeys.AddUnique(LevelKey);
}

void FVoxelWorldImpl::ApplyGroupedEdits(std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash>& EditsByBrick,
                                         const TSet<VoxelCoords::FVoxelChunkKey>& DirtyChunks)
{
	if (EditsByBrick.empty())
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_VoxelEditApply);
	for (auto& Entry : EditsByBrick)
	{
		Voxels.applyEdit(Entry.first, std::move(Entry.second));
		// docs/debug-tooling-plan.md P1 "vxc::Counters": one increment per
		// World::applyEdit call, matching the doc comment's existing "one
		// World::applyEdit per touched brick" invariant exactly.
		PerfCounters.incEditsApplied();
	}

	for (const VoxelCoords::FVoxelChunkKey& Key : DirtyChunks)
	{
		MarkChunkDirtyForRemesh(Key);
	}
	SortPendingQueues(LastAnchorLocation);
}

// --- debug-tooling helpers (docs/debug-tooling-plan.md P1) -----------------

void FVoxelWorldImpl::UpdatePerfSnapshot(float DeltaTime, float TickMs)
{
	// Per-frame collection into accumulators ("1Hz refresh of text, per-frame
	// collection"); the published snapshot (LastPerfSnapshot, read via
	// GetPerfSnapshot) only updates once the 1Hz window below rolls over.
	const float ThisTickSaturation = (LastAppliedFrac + LastRemeshFrac + LastUnloadFrac) / 3.f;
	BudgetSaturationAccum += ThisTickSaturation;
	++BudgetSaturationSamples;

	PerfRefreshAccumSeconds += DeltaTime;
	LastPerfSnapshot.SubsystemTickMs = TickMs; // always fresh; cheap, no reason to gate behind 1Hz

	if (PerfRefreshAccumSeconds < 1.0f)
	{
		return;
	}
	const float Window = PerfRefreshAccumSeconds;
	PerfRefreshAccumSeconds = 0.f;

	const int32 MaxJobsInFlight = 2 * FPlatformMisc::NumberOfCoresIncludingHyperthreads();

	LastPerfSnapshot.TotalChunksLoaded = TotalChunksLoaded;
	LastPerfSnapshot.TotalChunksUnloaded = TotalChunksUnloaded;
	LastPerfSnapshot.ChunksLoadedPerSec = float(TotalChunksLoaded - LoadedAtLastPerfRefresh) / Window;
	LastPerfSnapshot.ChunksUnloadedPerSec = float(TotalChunksUnloaded - UnloadedAtLastPerfRefresh) / Window;
	LoadedAtLastPerfRefresh = TotalChunksLoaded;
	UnloadedAtLastPerfRefresh = TotalChunksUnloaded;

	LastPerfSnapshot.JobsInFlight = JobsInFlightCounter.GetValue();
	LastPerfSnapshot.JobsInFlightCap = MaxJobsInFlight;
	LastPerfSnapshot.PendingJobQueueDepth = PendingJobKeys.Num();
	LastPerfSnapshot.PendingGameThreadQueueDepth = PendingGameThreadKeys.Num();
	LastPerfSnapshot.PendingUnloadQueueDepth = PendingUnloadKeys.Num();
	LastPerfSnapshot.StaleResultsDiscarded = StaleResultsDiscarded;

	LastPerfSnapshot.BudgetSaturationPct =
		BudgetSaturationSamples > 0 ? (BudgetSaturationAccum / float(BudgetSaturationSamples)) * 100.f : 0.f;
	BudgetSaturationAccum = 0.f;
	BudgetSaturationSamples = 0;

	// Worker mesh-job ms percentiles over the rolling window (sort a small
	// <=256-float copy once/sec -- negligible even though this runs whenever
	// voxel.Debug >= 1, not just under mode 2).
	if (WorkerJobMsWindowCount > 0)
	{
		TArray<float> Sorted;
		Sorted.Append(WorkerJobMsWindow.GetData(), WorkerJobMsWindowCount);
		Sorted.Sort();
		const int32 P50Index = FMath::Clamp(int32(Sorted.Num() * 0.50f), 0, Sorted.Num() - 1);
		const int32 P95Index = FMath::Clamp(int32(Sorted.Num() * 0.95f), 0, Sorted.Num() - 1);
		LastPerfSnapshot.WorkerMsP50 = Sorted[P50Index];
		LastPerfSnapshot.WorkerMsP95 = Sorted[P95Index];
		LastPerfSnapshot.WorkerMsMax = Sorted.Last();
	}
	else
	{
		LastPerfSnapshot.WorkerMsP50 = LastPerfSnapshot.WorkerMsP95 = LastPerfSnapshot.WorkerMsMax = 0.f;
	}

	int32 ResidentComponents = 0;
	// M2 item 1: "Per-level loaded/pending counters into the perf
	// snapshot/HUD." Loaded = has a live component right now; pending =
	// queued across all three pending arrays (job dispatch, game-thread
	// mesh, unload) for that level.
	int32 LevelLoaded[VoxelCoords::kNumLevels] = {};
	int32 LevelPending[VoxelCoords::kNumLevels] = {};
	for (const auto& Pair : ChunkRecords)
	{
		if (Pair.Value.Component.IsValid())
		{
			++ResidentComponents;
			++LevelLoaded[FMath::Clamp(Pair.Key.Level, 0, VoxelCoords::kNumLevels - 1)];
		}
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingJobKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingGameThreadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (const VoxelCoords::FVoxelLevelChunkKey& K : PendingUnloadKeys)
	{
		++LevelPending[FMath::Clamp(K.Level, 0, VoxelCoords::kNumLevels - 1)];
	}
	for (int32 Level = 0; Level < VoxelCoords::kNumLevels; ++Level)
	{
		LastPerfSnapshot.LevelLoadedCount[Level] = LevelLoaded[Level];
		LastPerfSnapshot.LevelPendingCount[Level] = LevelPending[Level];
	}
	LastPerfSnapshot.ResidentComponents = ResidentComponents;
	LastPerfSnapshot.ResidentQuads = ResidentQuads;
	LastPerfSnapshot.OverlayBrickCount = int64(Voxels.editedBricks().size());
	LastPerfSnapshot.EditLogEntries = int64(Voxels.log().size());

	LastPerfSnapshot.BricksGenerated = PerfCounters.getBricksGenerated();
	LastPerfSnapshot.CellsWritten = PerfCounters.getCellsWritten();
	LastPerfSnapshot.QuadsEmitted = PerfCounters.getQuadsEmitted();
	LastPerfSnapshot.EditsApplied = PerfCounters.getEditsApplied();
	LastPerfSnapshot.ColumnEvals = PerfCounters.getColumnEvals();
}

void FVoxelWorldImpl::UpdateChunkStateTints()
{
	// docs/debug-tooling-plan.md P1 chunk-state tints: just-loaded flash blue
	// (1s decay), edited-chunk orange (persistent while it owns overlay
	// bricks), game-thread re-mesh flash purple (1s decay). Whichever flash
	// is more recent (higher decay alpha) wins over the persistent base tint;
	// the two flashes are mutually exclusive in practice (LoadedAtSeconds only
	// fires on first creation, RemeshedAtSeconds only on an already-resident
	// chunk) but a fresh chunk that's edited again within the same second
	// could see both windows open at once, so pick explicitly rather than
	// relying on ordering.
	static const FLinearColor BlueFlash(0.1f, 0.4f, 1.0f, 1.0f);
	static const FLinearColor PurpleFlash(0.6f, 0.05f, 0.9f, 1.0f);
	static const FLinearColor OrangePersistent(1.0f, 0.45f, 0.05f, 1.0f);
	static const FLinearColor White(1.0f, 1.0f, 1.0f, 1.0f);
	constexpr float FlashDecaySeconds = 1.0f;

	for (auto& Pair : ChunkRecords)
	{
		UVoxelChunkComponent* Comp = Pair.Value.Component.Get();
		if (!Comp)
		{
			continue;
		}
		const VoxelStreaming::FChunkRecord& Rec = Pair.Value;
		const FLinearColor BaseTint = Rec.bHasOverlayBricks ? OrangePersistent : White;

		const float SinceLoaded = ElapsedSeconds - Rec.LoadedAtSeconds;
		const float SinceRemeshed = ElapsedSeconds - Rec.RemeshedAtSeconds;
		const float LoadedAlpha = (SinceLoaded >= 0.f && SinceLoaded < FlashDecaySeconds) ? (1.f - SinceLoaded / FlashDecaySeconds) : 0.f;
		const float RemeshedAlpha = (SinceRemeshed >= 0.f && SinceRemeshed < FlashDecaySeconds) ? (1.f - SinceRemeshed / FlashDecaySeconds) : 0.f;

		FLinearColor Tint = BaseTint;
		if (LoadedAlpha > 0.f || RemeshedAlpha > 0.f)
		{
			const bool bLoadedWins = LoadedAlpha >= RemeshedAlpha;
			Tint = FMath::Lerp(BaseTint, bLoadedWins ? BlueFlash : PurpleFlash, bLoadedWins ? LoadedAlpha : RemeshedAlpha);
		}

		Comp->SetDebugTint(Tint);
	}
}

void FVoxelWorldImpl::UpdateRingTints()
{
	// docs/m2-plan.md first implementation wave item 4: "when voxel.Debug
	// >= 2 and voxel.Debug.Rings is set, tint components by level." Simple
	// and unconditional -- unlike UpdateChunkStateTints there is no flash/
	// decay here, just a flat per-level color.
	for (auto& Pair : ChunkRecords)
	{
		if (UVoxelChunkComponent* Comp = Pair.Value.Component.Get())
		{
			Comp->SetDebugTint(VoxelDebug::RingLevelTint(Pair.Key.Level));
		}
	}
}

void FVoxelWorldImpl::DrawDebugBoundsLayer(UWorld& World, const FVector& Anchor) const
{
	if (!VoxelDebug::IsBoundsEnabled())
	{
		return;
	}

	// Cap ~200 nearest tracked chunks (docs/debug-tooling-plan.md P1 "Bounds
	// layer"). ChunkRecords can hold several thousand entries at M1 streaming
	// radii; the full sort below only runs while this layer is switched on.
	struct FEntry
	{
		VoxelCoords::FVoxelLevelChunkKey Key;
		double DistSq;
	};
	TArray<FEntry> Entries;
	Entries.Reserve(ChunkRecords.Num());
	for (const auto& Pair : ChunkRecords)
	{
		const double ChunkEdge = VoxelCoords::ChunkEdgeUUForLevel(Pair.Key.Level);
		const FVector Center = VoxelCoords::ChunkOriginWorldForLevel(Pair.Key.Key, Pair.Key.Level) + FVector(ChunkEdge * 0.5);
		Entries.Add(FEntry{Pair.Key, FVector::DistSquared(Center, Anchor)});
	}
	Entries.Sort([](const FEntry& A, const FEntry& B) { return A.DistSq < B.DistSq; });

	constexpr int32 MaxBoxes = 200;
	const int32 Count = FMath::Min(MaxBoxes, Entries.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const VoxelCoords::FVoxelLevelChunkKey& LevelKey = Entries[Index].Key;
		const float Extent = float(VoxelCoords::ChunkEdgeUUForLevel(LevelKey.Level)) * 0.5f;
		const FVector Center = VoxelCoords::ChunkOriginWorldForLevel(LevelKey.Key, LevelKey.Level) + FVector(Extent);
		DrawDebugBox(&World, Center, FVector(Extent), FColor::Cyan, /*bPersistentLines*/ false, /*LifeTime*/ -1.f,
		             /*DepthPriority*/ 0, /*Thickness*/ 1.5f);
	}
}

bool FVoxelWorldImpl::TryDig(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels)
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

	const int32 N = FMath::Clamp(SizeVoxels, UVoxelWorldSubsystem::MinCubeSizeVoxels, UVoxelWorldSubsystem::MaxCubeSizeVoxels);
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	// Cube biased into the terrain (m1-plan.md "Dig sizes" row): grow along
	// the hit-face axis in the SAME direction the ray was travelling when it
	// entered the solid voxel (Hit.faceSign) -- that direction points deeper
	// into the surface, away from the camera.
	int64 MinX, MinY, MinZ;
	ComputeCubeMinCorner(Hit.vx, Hit.vy, Hit.vz, Hit.faceAxis, Hit.faceSign, N, MinX, MinY, MinZ);

	// One World::applyEdit per touched brick (docs/m1-plan.md Stage 2
	// decisions table): collect every voxel in the cube, grouped by brick.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (int64 Dz = 0; Dz < N; ++Dz)
	{
		for (int64 Dy = 0; Dy < N; ++Dy)
		{
			for (int64 Dx = 0; Dx < N; ++Dx)
			{
				const int64 Vx = MinX + Dx, Vy = MinY + Dy, Vz = MinZ + Dz;
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
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return true;
}

bool FVoxelWorldImpl::TryPlace(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, uint8 MaterialId,
                                const FVector& PlayerActorLocation)
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

	const int32 N = FMath::Clamp(SizeVoxels, UVoxelWorldSubsystem::MinCubeSizeVoxels, UVoxelWorldSubsystem::MaxCubeSizeVoxels);
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;

	// Cube grid-snapped against the hit face, biased AWAY from the surface
	// (m1-plan.md "Place" row): grow along the face axis opposite the ray's
	// entry direction (-Hit.faceSign), starting from Hit.px/py/pz (already
	// the empty voxel adjacent to the face) -- mirrors TryDig's bias.
	int64 MinX, MinY, MinZ;
	ComputeCubeMinCorner(Hit.px, Hit.py, Hit.pz, Hit.faceAxis, -Hit.faceSign, N, MinX, MinY, MinZ);

	// Reject if the placement cube would overlap the player's collision box
	// (m1-plan.md "Place" row: "Placement must not intersect the player's
	// collision box"). Box half-extents mirror AVoxelEarthFlyPawn's walk-mode
	// collision box (60x60x180 UU) -- duplicated here (rather than reaching
	// into the pawn) since the subsystem must not depend on a specific pawn
	// class.
	constexpr double PlayerHalfExtentXYUU = 30.0;
	constexpr double PlayerHalfExtentZUU = 90.0;
	const FVector CubeMinUU = VoxelCoords::VoxelToWorldCorner(VoxelCoords::FVoxelCoord{MinX, MinY, MinZ});
	const FVector CubeMaxUU =
		VoxelCoords::VoxelToWorldCorner(VoxelCoords::FVoxelCoord{MinX + N, MinY + N, MinZ + N});
	const FVector PlayerMinUU = PlayerActorLocation - FVector(PlayerHalfExtentXYUU, PlayerHalfExtentXYUU, PlayerHalfExtentZUU);
	const FVector PlayerMaxUU = PlayerActorLocation + FVector(PlayerHalfExtentXYUU, PlayerHalfExtentXYUU, PlayerHalfExtentZUU);
	const bool bOverlapsPlayer = CubeMinUU.X < PlayerMaxUU.X && CubeMaxUU.X > PlayerMinUU.X && CubeMinUU.Y < PlayerMaxUU.Y &&
	                              CubeMaxUU.Y > PlayerMinUU.Y && CubeMinUU.Z < PlayerMaxUU.Z && CubeMaxUU.Z > PlayerMinUU.Z;
	if (bOverlapsPlayer)
	{
		// docs/debug-tooling-plan.md P1 "Log split": edit-log rejections move
		// to LogVoxelEdit.
		UE_LOG(LogVoxelEdit, Log, TEXT("TryPlace rejected: %dx%dx%d cube at (%lld,%lld,%lld) would intersect the player."),
		       N, N, N, (long long)MinX, (long long)MinY, (long long)MinZ);
		return false;
	}

	// One World::applyEdit per touched brick, same grouping as TryDig.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;

	for (int64 Dz = 0; Dz < N; ++Dz)
	{
		for (int64 Dy = 0; Dy < N; ++Dy)
		{
			for (int64 Dx = 0; Dx < N; ++Dx)
			{
				const int64 Vx = MinX + Dx, Vy = MinY + Dy, Vz = MinZ + Dz;
				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, (vxc::MaterialId)MaterialId});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
			}
		}
	}

	if (EditsByBrick.empty())
	{
		return false;
	}
	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return true;
}

int32 FVoxelWorldImpl::CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU)
{
	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	const double VoxelSize = VoxelCoords::VoxelSizeUU;
	const uint64 Seed = Voxels.amplifier().seed();
	const double MaxExtentUU = RadiusUU + FMath::Abs(JitterUU);

	// Bounding box of voxels that could possibly be inside RadiusUU +
	// JitterUU of CenterUU, in voxel-lattice coordinates.
	const int64 VXMin = (int64)FMath::FloorToDouble((CenterUU.X - MaxExtentUU) / VoxelSize);
	const int64 VXMax = (int64)FMath::CeilToDouble((CenterUU.X + MaxExtentUU) / VoxelSize);
	const int64 VYMin = (int64)FMath::FloorToDouble((CenterUU.Y - MaxExtentUU) / VoxelSize);
	const int64 VYMax = (int64)FMath::CeilToDouble((CenterUU.Y + MaxExtentUU) / VoxelSize);
	const int64 VZMin = (int64)FMath::FloorToDouble((CenterUU.Z - MaxExtentUU) / VoxelSize);
	const int64 VZMax = (int64)FMath::CeilToDouble((CenterUU.Z + MaxExtentUU) / VoxelSize);

	// One World::applyEdit per touched brick, same grouping as TryDig/TryPlace.
	std::unordered_map<vxc::BrickKey, std::vector<vxc::EditCell>, vxc::BrickKeyHash> EditsByBrick;
	TSet<VoxelCoords::FVoxelChunkKey> DirtyChunks;
	int32 RemovedCount = 0;

	for (int64 Vz = VZMin; Vz <= VZMax; ++Vz)
	{
		for (int64 Vy = VYMin; Vy <= VYMax; ++Vy)
		{
			for (int64 Vx = VXMin; Vx <= VXMax; ++Vx)
			{
				const FVector VoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Vx, Vy, Vz});
				const double DistUU = (VoxelCenterUU - CenterUU).Size();

				// Deterministic per-voxel jitter (m1-plan.md "Explosives v1"
				// row): vxc::hash3(seed, vx,vy,vz, channel 40), top 16 bits
				// signed and scaled into [-JitterUU, +JitterUU] -- gives a
				// ragged, reproducible blast edge instead of a perfect sphere.
				const uint64 H = vxc::hash3(Seed, Vx, Vy, Vz, /*channel*/ 40);
				const double JitterHereUU = (double(vxc::hashToSigned16(H)) / 32768.0) * JitterUU;
				if (DistUU >= RadiusUU + JitterHereUU)
				{
					continue;
				}

				if (Voxels.materialAt(Vx, Vy, Vz) == vxc::MAT_AIR)
				{
					continue; // nothing to remove; also keeps the edit-log entry minimal
				}

				const vxc::BrickKey BKey = vxc::ChunkMap<B>::keyForVoxel(Vx, Vy, Vz);
				const int LocalX = (int)vxc::floorMod(Vx, B);
				const int LocalY = (int)vxc::floorMod(Vy, B);
				const int LocalZ = (int)vxc::floorMod(Vz, B);
				const uint16_t Cell = (uint16_t)vxc::Brick<B>::cellIndex(LocalX, LocalY, LocalZ);
				EditsByBrick[BKey].push_back(vxc::EditCell{Cell, vxc::MAT_AIR});

				CollectDirtyChunks(Vx, Vy, Vz, DirtyChunks);
				++RemovedCount;
			}
		}
	}

	ApplyGroupedEdits(EditsByBrick, DirtyChunks);
	return RemovedCount;
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
	// -VoxelDefaultMaterial: diagnostic switch — skip the authored material
	// and use the engine default, to isolate material bugs from geometry
	// bugs (an invisible-terrain failure with the authored material and a
	// visible one with the default indicts the asset, not the mesh).
	if (!FParse::Param(FCommandLine::Get(), TEXT("VoxelDefaultMaterial")))
	{
		ChunkMaterial = Cast<UMaterialInterface>(
			StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
	}
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
		// docs/debug-tooling-plan.md P1 "Log split": streaming-lifecycle lines
		// move to LogVoxelStream (module/general lines stay on LogVoxelEarth).
		UE_LOG(LogVoxelStream, Error, TEXT("Failed to spawn the voxel chunk owner actor; streaming will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelChunkOwner"));
#endif

	UE_LOG(LogVoxelStream, Log,
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

bool UVoxelWorldSubsystem::TryDig(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels)
{
	return Impl ? Impl->TryDig(CameraWorldLocation, CameraWorldDirection, SizeVoxels) : false;
}

bool UVoxelWorldSubsystem::TryPlace(const FVector& CameraWorldLocation, const FVector& CameraWorldDirection, int32 SizeVoxels,
                                     uint8 MaterialId, const FVector& PlayerActorLocation)
{
	return Impl ? Impl->TryPlace(CameraWorldLocation, CameraWorldDirection, SizeVoxels, MaterialId, PlayerActorLocation) : false;
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

bool UVoxelWorldSubsystem::IsSolidAtVoxel(int64 Vx, int64 Vy, int64 Vz) const
{
	if (!Impl)
	{
		return false;
	}
	// Overlay-aware (World::materialAt, not GeneratedWorld::materialAt): a
	// dug voxel must read back as non-solid immediately, and a placed one as
	// solid, for walk-mode collision to agree with what dig/place just did.
	return Impl->Voxels.materialAt(Vx, Vy, Vz) != vxc::MAT_AIR;
}

bool UVoxelWorldSubsystem::RaycastVoxelWorld(const FVector& StartUU, const FVector& DirUU, double MaxDistUU,
                                              FVector& OutHitVoxelCenterUU, FVector& OutPrevVoxelCenterUU) const
{
	if (!Impl || MaxDistUU <= 0.0)
	{
		return false;
	}

	const FVector Dir = DirUU.GetSafeNormal();
	if (Dir.IsNearlyZero())
	{
		return false;
	}

	// Same mm conversion as the dig/place raycast (CastFromCamera above);
	// kept independent (not shared) so this method stays self-contained for
	// the parallel file-ownership split noted in docs/m1-plan.md.
	const int64 OxMm = VoxelCoords::WorldToMm(StartUU.X);
	const int64 OyMm = VoxelCoords::WorldToMm(StartUU.Y);
	const int64 OzMm = VoxelCoords::WorldToMm(StartUU.Z);
	const double RangeMm = MaxDistUU * 10.0; // UU -> mm (1 UU = 10 mm)
	const int64 DxMm = (int64)FMath::RoundToDouble(Dir.X * RangeMm);
	const int64 DyMm = (int64)FMath::RoundToDouble(Dir.Y * RangeMm);
	const int64 DzMm = (int64)FMath::RoundToDouble(Dir.Z * RangeMm);

	const auto MaterialFn = [this](int64 X, int64 Y, int64 Z) { return Impl->Voxels.materialAt(X, Y, Z); };
	const vxc::RaycastHit Hit = vxc::raycastVoxels(MaterialFn, OxMm, OyMm, OzMm, DxMm, DyMm, DzMm);
	if (!Hit.hit)
	{
		return false;
	}

	OutHitVoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Hit.vx, Hit.vy, Hit.vz});
	OutPrevVoxelCenterUU = VoxelCoords::VoxelToWorldCenter(VoxelCoords::FVoxelCoord{Hit.px, Hit.py, Hit.pz});
	return true;
}

int32 UVoxelWorldSubsystem::CarveSphere(const FVector& CenterUU, double RadiusUU, double JitterUU)
{
	return Impl ? Impl->CarveSphere(CenterUU, RadiusUU, JitterUU) : 0;
}

FVoxelPerfSnapshot UVoxelWorldSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->GetPerfSnapshot() : FVoxelPerfSnapshot{};
}
