#include "VoxelWaterSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEditRelay.h"
#include "VoxelWaterChunkComponent.h"
#include "VoxelWorldSubsystem.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (same doctrine note as VoxelWorldSubsystem.cpp).
#include "voxelcore/bytes.h"
#include "voxelcore/mesher.h"
#include "voxelcore/waterca.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator (locating the world's single AVoxelEditRelay)
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "UObject/UObjectGlobals.h"

#include <vector>

// ADR-0003 item 2/4 (docs/adr/0003-hydrostatic-persistent-body.md): toggles
// vxc::WaterCA's cross-tick terrain-solidity memo. Checked every fixed step
// (see StepFixed below), same "read the cvar every call, no restart needed"
// pattern as VoxelWorldSubsystem.cpp's CVarVoxelDestructionEnabled/
// CVarVoxelCollapseEnabled -- so it can be flipped off in the field without a
// relaunch (task item 4) if the invalidation contract is ever suspected of a
// gap. Byte-identical for as long as UVoxelWorldSubsystem's edit paths honour
// the invalidation contract (NotifyTerrainRegionEdited, wired into
// TryDig/TryPlace/CarveSphere/island-and-collapse-removal -- see
// VoxelWorldSubsystem.cpp).
//
// DEFAULT ON as of ADR-0003 item 2 resolution: an in-engine cross-process A/B
// (-VoxelWaterMemoTest, VoxelEarthGameMode.cpp) ran the full dig/place/carve/
// M5-collapse edit vocabulary beneath and around a settled basin pool, once
// with this cvar forced 0 and once forced 1 (same seed, same edit sequence),
// and diffed EVERY logged checkpoint digest, not just the final one: pour,
// pre-edit settle, post-dig/-place/-carve/-collapse (each immediately after
// its edit AND again once re-settled). All of them matched byte-for-byte
// between the two runs, including intermediate values, confirming the
// invalidation wiring is complete for every edit path this cvar's contract
// depends on. See docs/status.md's "Water edit-notification completeness +
// memo enablement" entry for the full digest table and the perf numbers.
static TAutoConsoleVariable<bool> CVarVoxelWaterSolidCache(
	TEXT("voxel.Water.SolidCacheEnabled"), true,
	TEXT("ADR-0003: enable WaterCA's cross-tick terrain-solidity memo (~2.8-3.0x hydrostatic pass speedup). ")
	TEXT("Byte-identical for as long as every terrain edit invalidates it -- see docs/adr/0003-hydrostatic-persistent-body.md. ")
	TEXT("Set to 0 to force the uncached path (e.g. if an edit path is ever suspected of a missed invalidation)."),
	ECVF_Default);

namespace
{
// BrickKey <-> VoxelCoords::FVoxelCoord: this subsystem carries brick-grid
// coordinates (NOT voxel coordinates) through UE-visible plumbing as
// FVoxelCoord, per that struct's W2 doc comment (VoxelCoords.h) -- these two
// tiny helpers are the one place the reinterpretation happens.
VoxelCoords::FVoxelCoord ToCoord(const vxc::BrickKey& K)
{
	return VoxelCoords::FVoxelCoord{K.x, K.y, K.z};
}
vxc::BrickKey ToBrickKey(const VoxelCoords::FVoxelCoord& C)
{
	return vxc::BrickKey{static_cast<int32_t>(C.X), static_cast<int32_t>(C.Y), static_cast<int32_t>(C.Z)};
}

// --- Replication wire format (task item 1) ----------------------------------
//
// Brick-granularity raw-fill snapshots (u32 count, then per brick: i32 x,y,z
// + 512 raw fill bytes in the SAME fixed cell order WaterBrick8::digest uses)
// -- NOT a per-cell delta. Deliberately simple (task spec: "basic diff
// encoding -- full compression is W2-polish"); built from vxc::ByteWriter/
// ByteReader, the same primitives UVoxelWorldSubsystem's edit-log wire format
// uses (VoxelWorldSubsystem.cpp's SerializeEntries/ParseEntries).
//
// Processes Keys in order, stopping once MaxBytes would be exceeded (but
// always encoding at least one brick, guaranteeing forward progress even if
// a single brick's 524 bytes alone exceeds a pathologically small cap).
// OutConsumedKeyCount reports how many of the FRONT of Keys were fully
// handled (encoded OR determined-absent-so-nothing-to-send) -- the caller
// drops exactly those from its "dirty since last broadcast" set, leaving any
// remainder (past the byte cap) for the next ~5Hz round.
void SerializeWaterDiffs(const std::vector<vxc::BrickKey>& Keys, const vxc::WaterCA& CA, int32 MaxBytes, TArray<uint8>& OutBytes,
                          int32& OutEncodedBrickCount, size_t& OutConsumedKeyCount)
{
	std::vector<uint8_t> Bytes;
	vxc::ByteWriter W(Bytes);
	W.u32(0); // patched below once the final encoded count is known
	uint32_t Encoded = 0;
	size_t Consumed = 0;
	constexpr size_t EntrySize = 12 + size_t(vxc::WaterBrick8::kCells); // i32 x,y,z + 512 raw fill bytes

	for (; Consumed < Keys.size(); ++Consumed)
	{
		const vxc::BrickKey& K = Keys[Consumed];
		const vxc::WaterBrick8* Brick = CA.findBrick(K);
		if (!Brick)
		{
			continue; // emptied since being marked dirty -- nothing to send, but fully handled
		}
		if (Encoded > 0 && Bytes.size() + EntrySize > size_t(MaxBytes))
		{
			break; // cap reached -- this (and later) keys wait for next round
		}
		W.i32(K.x);
		W.i32(K.y);
		W.i32(K.z);
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) W.u8(Brick->get(x, y, z));
		++Encoded;
	}

	Bytes[0] = uint8_t(Encoded);
	Bytes[1] = uint8_t(Encoded >> 8);
	Bytes[2] = uint8_t(Encoded >> 16);
	Bytes[3] = uint8_t(Encoded >> 24);
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}
	OutEncodedBrickCount = int32(Encoded);
	OutConsumedKeyCount = Consumed;
}
} // namespace

// FVoxelWaterImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWaterSubsystem.h (UHT-parsed) never sees a voxel-core header (same
// pattern as FVoxelWorldImpl in VoxelWorldSubsystem.cpp).
struct FVoxelWaterImpl
{
	explicit FVoxelWaterImpl(UVoxelWorldSubsystem& InTerrain)
		: Terrain(InTerrain)
		, CA([this](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
		     { return Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; })
	{
	}

	UVoxelWorldSubsystem& Terrain;
	vxc::WaterCA CA;

	// Reservoir v0 (docs/voxel-earth-implementation-plan.md SS3.7): breach-
	// boundary voxel coordinates that continuously top up to 255 fill units
	// every fixed step -- see UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared
	// for the seeding rule and UVoxelWaterSubsystem::StepFixed for the top-up.
	TSet<VoxelCoords::FVoxelCoord> ReservoirCells;

	// Fixed 10Hz accumulator (task item 3).
	float TickAccumSeconds = 0.f;
	static constexpr float FixedStepSeconds = 0.1f; // 10Hz
	// Fixed-timestep spiral-of-death guard: a frame hitch accumulates debt
	// but never demands more than this many catch-up steps in one Tick.
	static constexpr int32 MaxStepsPerFrame = 4;

	// One UWaterChunkComponent per resident vxc::WaterBrick8, keyed by its
	// BrickKey (carried as VoxelCoords::FVoxelCoord -- brick-grid
	// coordinates; see the file-scope ToCoord/ToBrickKey doc comment).
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ChunkComponents;

	// Bricks touched since the last re-mesh pass (task item 4): unioned from
	// vxc::WaterCA::activeBricks() after every fixed step, PLUS any brick an
	// addWater() call (spawn/breach/reservoir top-up) touched directly --
	// the latter needs marking immediately rather than waiting for the next
	// step() to fold it into the CA's own active set, so a fresh pour is
	// visible the same frame it's placed.
	TSet<VoxelCoords::FVoxelCoord> DirtyBricks;

	// --- Perf / HUD bookkeeping (task item 3) -------------------------------
	float PerfRefreshAccumSeconds = 0.f;
	int32 StepsThisWindow = 0;
	int32 LastSteppedBrickCount = 0;
	int32 ReplicatedBytesThisWindow = 0;
	FVoxelWaterPerfSnapshot LastSnapshot;

	// Log-throttle for the voxel.Water.MaxActiveBricks budget warning (task
	// item 3: "log-throttle warning (do not explode)").
	double LastBudgetWarnWorldSeconds = -1000.0;

	// --- Replication v1 (task item 1) ---------------------------------------
	float ReplicationAccumSeconds = 0.f;
	static constexpr float ReplicationIntervalSeconds = 0.2f; // ~5Hz
	static constexpr int32 MaxDiffBytesPerBroadcast = 32 * 1024; // documented cap (task item 1)
	TSet<VoxelCoords::FVoxelCoord> DirtySinceLastBroadcast;
};

namespace
{
// Marks every water brick spanning [VzStart, VzStart + ceil(PlacedAmount/255)]
// at (Vx,Vy) dirty for re-mesh AND replication -- addWater() stacks straight
// up a column, so this is the full set of bricks a single addWater() call
// could possibly have touched (a one-brick apron above the theoretical span
// costs nothing: findBrick() returning null there is a harmless no-op in
// RemeshDirtyBricks/SerializeWaterDiffs).
void MarkColumnDirty(FVoxelWaterImpl& Impl, int64_t Vx, int64_t Vy, int64_t VzStart, uint32_t PlacedAmount)
{
	const int64_t CellsSpan = int64_t(PlacedAmount) / 255 + 2;
	for (int64_t Dz = 0; Dz <= CellsSpan; ++Dz)
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(vxc::waterKeyForVoxel(Vx, Vy, VzStart + Dz));
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}
}
} // namespace

// UVoxelWaterSubsystem ------------------------------------------------------

UVoxelWaterSubsystem::UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::~UVoxelWaterSubsystem() = default;
UVoxelWaterSubsystem::UVoxelWaterSubsystem(FVTableHelper& Helper)
	: Super(Helper)
{
}

void UVoxelWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Direct dependency on the terrain subsystem (task spec item 1): both are
	// UWorldSubsystem-derived, so InitializeDependency handles ordering --
	// UVoxelWorldSubsystem::Impl is guaranteed constructed by the time this
	// call returns, so IsSolidAtVoxel is safe to bind into the CA's SolidFn
	// right away (it isn't actually CALLED until streaming/breach/spawn
	// touches a voxel, well after both subsystems' Initialize has run).
	UVoxelWorldSubsystem* Terrain = Collection.InitializeDependency<UVoxelWorldSubsystem>();
	if (!Terrain)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("UVoxelWaterSubsystem::Initialize: no UVoxelWorldSubsystem -- water CA cannot bridge to terrain ")
		       TEXT("solidity; water is disabled for this run."));
		return;
	}

	Impl = MakeUnique<FVoxelWaterImpl>(*Terrain);
}

void UVoxelWaterSubsystem::Deinitialize()
{
	ChunkRoot = nullptr;
	ChunkOwner = nullptr;
	WaterMaterial = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWaterSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Same scope as UVoxelWorldSubsystem: game worlds only.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWaterSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld() || !Impl)
	{
		return;
	}

	// Dedicated server: no viewport, so no render chunks -- but the CA still
	// ticks authoritatively regardless (Tick() below doesn't check
	// ChunkOwner for the simulation half, only for the re-mesh half). Same
	// reasoning as UVoxelWorldSubsystem's identical dedicated-server branch.
	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Water rendering DISABLED (NM_DedicatedServer has no viewport): the pressure CA still ticks ")
		       TEXT("authoritatively at %.0fHz."),
		       1.f / FVoxelWaterImpl::FixedStepSeconds);
		return;
	}

	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
	if (WaterMaterial == nullptr)
	{
		WaterMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelWater, Warning, TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- using engine default material."));
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		UE_LOG(LogVoxelWater, Error, TEXT("Failed to spawn the water chunk owner actor; water rendering will not start."));
		return;
	}

	ChunkRoot = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelWaterChunkRoot"));
	ChunkOwner->SetRootComponent(ChunkRoot);
	ChunkRoot->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelWaterChunkOwner"));
#endif

	UE_LOG(LogVoxelWater, Log, TEXT("Water subsystem initialized: fixed step %.0fHz, MaxActiveBricks budget=%d"),
	       1.f / FVoxelWaterImpl::FixedStepSeconds, VoxelDebug::GetWaterMaxActiveBricks());
}

namespace
{
// Doctrine SS2.5 ("everything expensive is budgeted, never demand-driven"):
// per-brick vxc::meshBrick<8> (re-mesh of an existing component OR meshing
// for a brand-new one -- both cost the same greedy-mesh pass) is the
// expensive per-tick operation, NOT just NewObject+RegisterComponent as
// first assumed. Verification testing measured this progressively:
//   1. Every apron sample routed through vxc::WaterCA::fillAt()'s full
//      hash+unordered_map lookup: ~90 active bricks cost 10-40ms/tick.
//   2. Fixed by indexing the already-fetched WaterBrick8 directly for
//      in-bounds samples (see the Sampler below) -- but a NEW-component
//      creation cap alone (kMaxNewComponentsPerTick, tried at 32 then 4)
//      still left 15-69ms spikes: re-meshing ~90-150 ALREADY-EXISTING
//      components every tick (uncapped) was the remaining cost, not
//      creation.
// Fix: ONE unified per-tick budget over the actual expensive operation
// (the meshBrick<8> call itself), covering both re-mesh and create alike;
// only cheap destroy-on-empty is unbudgeted (see below). A sustained flood
// larger than this budget spreads its update cost over several 10Hz ticks
// (each dirty-but-deferred brick keeps its last-known mesh on screen for
// one extra tick at most) instead of spiking one of them.
constexpr int32 kMaxBrickMeshesPerTick = 8;

// Rebuilds (or destroys) the render component for every brick in
// Impl.DirtyBricks, then clears that set (except for any budget-deferred
// entries, left in place for the next call). Shared by the authority tick
// path and the client replication-receive path (task item 4's mesher applies
// identically regardless of which side produced the fill data).
void RemeshDirtyBricks(FVoxelWaterImpl& Impl, AActor* ChunkOwner, USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		// No viewport (NM_DedicatedServer) -- simulate only, never render;
		// skip the meshBrick<8> work entirely rather than pay its cost for
		// output nobody will ever see, and drop the backlog so it can't grow
		// unbounded over a long-running dedicated server session.
		Impl.DirtyBricks.Reset();
		return;
	}

	const TArray<VoxelCoords::FVoxelCoord> ToProcess = Impl.DirtyBricks.Array();
	Impl.DirtyBricks.Reset();
	int32 MeshesThisTick = 0;
	int32 DeferredCount = 0;

	for (const VoxelCoords::FVoxelCoord& BrickCoord : ToProcess)
	{
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const vxc::WaterBrick8* Brick = Impl.CA.findBrick(Key);

		if (!Brick)
		{
			// Brick fully drained/collapsed (WaterMap's homogeneous-empty
			// collapse == absence, per waterca.h) -- drop its component
			// entirely. Cheap (no meshBrick call) -- unbudgeted, and capping
			// it would leak stale geometry.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			continue;
		}

		if (MeshesThisTick >= kMaxBrickMeshesPerTick)
		{
			// Budget reached -- defer to the NEXT RemeshDirtyBricks call
			// rather than pay for every dirty brick's meshBrick<8> pass in
			// one frame. Whatever this brick's component currently shows
			// (its previous mesh, or nothing if brand new) stays as-is for
			// one extra tick at most (10Hz CA cadence >> this budget's
			// drain rate for any v0-scale event).
			Impl.DirtyBricks.Add(BrickCoord);
			++DeferredCount;
			continue;
		}
		++MeshesThisTick;

		// Task item 4: "mesh active water cells (fill>0) via vxc::meshBrick
		// over a water-occupancy sampler (fill>=128 solid for meshing v0;
		// surface cells with partial fill render at full cube v0 --
		// partial-height mesh is W5 polish)". The material id written into
		// the sampler's output is an arbitrary fixed nonzero placeholder
		// (MAT_ROCK) -- water's translucent material doesn't branch on it,
		// unlike terrain's per-material vertex-color tint.
		// Perf: meshBrick<8> samples a padded [-1,8] range per axis, but the
		// overwhelming majority of those samples land INSIDE this brick
		// (0..7 on every axis) -- for those, index straight into the
		// already-fetched Brick pointer instead of paying vxc::WaterCA::
		// fillAt()'s full waterKeyForVoxel-hash + unordered_map-lookup cost
		// per cell. Only the apron (a neighbor brick, or empty space) falls
		// back to fillAt(). Measured necessary in verification testing: with
		// every sample going through fillAt(), a single RemeshDirtyBricks
		// pass over ~90 active bricks cost 10-40ms/tick -- far over the v0
		// <2ms/frame budget -- purely from redundant hashmap lookups within
		// a brick whose contents were already in hand.
		std::vector<vxc::Quad> RawQuads;
		const auto Sampler = [&Impl, &Key, Brick](int x, int y, int z) -> vxc::MaterialId
		{
			uint8_t Fill;
			if (x >= 0 && x < vxc::WaterBrick8::kEdge && y >= 0 && y < vxc::WaterBrick8::kEdge && z >= 0 && z < vxc::WaterBrick8::kEdge)
			{
				Fill = Brick->get(x, y, z);
			}
			else
			{
				const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
				const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
				const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
				Fill = Impl.CA.fillAt(Vx, Vy, Vz);
			}
			return Fill >= 128 ? vxc::MAT_ROCK : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		TArray<FVoxelChunkQuad> Quads;
		Quads.Reserve(RawQuads.size());
		for (const vxc::Quad& Q : RawQuads)
		{
			FVoxelChunkQuad CQ;
			CQ.Axis = Q.axis;
			CQ.Positive = Q.positive;
			CQ.Slice = Q.slice;
			CQ.U0 = Q.u0;
			CQ.V0 = Q.v0;
			CQ.W = Q.w;
			CQ.H = Q.h;
			CQ.Ao = Q.ao;
			CQ.Mat = Q.mat;
			Quads.Add(CQ);
		}

		if (Quads.Num() == 0)
		{
			// Fully solid or fully empty per the >=128 threshold, but the
			// brick itself is still stored (e.g. all cells sit at exactly
			// 100 fill, below the meshing threshold, but nonzero) -- no
			// faces to draw; drop any stale component rather than register
			// a proxy-less one.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ChunkComponents.Remove(BrickCoord);
			}
			continue;
		}

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			// Already inside this tick's kMaxBrickMeshesPerTick budget (the
			// meshBrick<8> call above already happened) -- NewObject +
			// RegisterComponent is comparatively cheap next to that, so no
			// separate cap needed here.
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			const FVector RelLoc(double(Key.x) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
			                      double(Key.y) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU,
			                      double(Key.z) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU);
			Comp->SetRelativeLocation(RelLoc);
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads));
	}

	if (DeferredCount > 0)
	{
		UE_LOG(LogVoxelWater, Verbose, TEXT("RemeshDirtyBricks: deferred %d brick mesh(es) to next tick (budget %d/tick)."),
		       DeferredCount, kMaxBrickMeshesPerTick);
	}
}

// One fixed 10Hz step: Reservoir v0 top-up, then vxc::WaterCA::step(),
// folding the CA's own post-step active set into the dirty-for-remesh /
// dirty-for-replication sets (task items 2 & 3).
void StepFixed(FVoxelWaterImpl& Impl, double NowWorldSeconds)
{
	// ADR-0003 item 4: re-check the cvar every fixed step (cheap -- a bool
	// compare, plus a memo clear only on the OFF-transition, see
	// WaterCA::setSolidCacheEnabled) so voxel.Water.SolidCacheEnabled can be
	// flipped live without restarting. Must happen before step() below --
	// hydrostaticPass reads solidCacheEnabled_ at the top of its own call.
	Impl.CA.setSolidCacheEnabled(CVarVoxelWaterSolidCache.GetValueOnGameThread());

	// Reservoir v0 (SS3.7: "the implicit ocean acts as an infinite
	// reservoir: boundary cells refill to 255 each tick while exposed"). v0
	// simplification: tops up unconditionally once registered (no support
	// for detecting a plugged/re-solidified breach), documented in
	// NotifyTerrainVoxelsCleared's own doc comment.
	for (const VoxelCoords::FVoxelCoord& Cell : Impl.ReservoirCells)
	{
		const uint8_t Cur = Impl.CA.fillAt(Cell.X, Cell.Y, Cell.Z);
		if (Cur < 255)
		{
			Impl.CA.addWater(Cell.X, Cell.Y, Cell.Z, uint32_t(255 - Cur));
			MarkColumnDirty(Impl, Cell.X, Cell.Y, Cell.Z, uint32_t(255 - Cur));
		}
	}

	Impl.CA.step();
	++Impl.StepsThisWindow;
	Impl.LastSteppedBrickCount = int32(Impl.CA.steppedBrickCount());

	for (const vxc::BrickKey& K : Impl.CA.activeBricks())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
	}

	// Task item 3: "if steppedBrickCount exceeds a cvar cap ... log-throttle
	// warning (do not explode)". The CA's tick contract (waterca.h) is
	// atomic over its whole active-set snapshot -- there is no safe mid-step
	// cutoff that wouldn't break volume conservation/determinism -- so this
	// is purely a monitoring signal, not a clamp.
	const int32 Cap = VoxelDebug::GetWaterMaxActiveBricks();
	if (Cap > 0 && Impl.LastSteppedBrickCount > Cap && (NowWorldSeconds - Impl.LastBudgetWarnWorldSeconds) > 5.0)
	{
		Impl.LastBudgetWarnWorldSeconds = NowWorldSeconds;
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("WaterCA over budget: steppedBrickCount=%d > voxel.Water.MaxActiveBricks=%d (tick ran in full -- ")
		       TEXT("no safe mid-step cutoff exists; raise the cap or investigate runaway spread)."),
		       Impl.LastSteppedBrickCount, Cap);
	}
}

// Server -> client replication (task item 1), fixed ~5Hz cadence,
// byte-capped per broadcast. No-op (and clears nothing) if there's nothing
// dirty this round.
void BroadcastWaterDiffs(FVoxelWaterImpl& Impl, UWorld& World, float DeltaTime)
{
	Impl.ReplicationAccumSeconds += DeltaTime;
	if (Impl.ReplicationAccumSeconds < FVoxelWaterImpl::ReplicationIntervalSeconds)
	{
		return;
	}
	Impl.ReplicationAccumSeconds = 0.f;

	if (Impl.DirtySinceLastBroadcast.Num() == 0)
	{
		return;
	}

	std::vector<vxc::BrickKey> Keys;
	Keys.reserve(Impl.DirtySinceLastBroadcast.Num());
	for (const VoxelCoords::FVoxelCoord& C : Impl.DirtySinceLastBroadcast) Keys.push_back(ToBrickKey(C));

	TArray<uint8> Bytes;
	int32 EncodedBrickCount = 0;
	size_t ConsumedKeyCount = 0;
	SerializeWaterDiffs(Keys, Impl.CA, FVoxelWaterImpl::MaxDiffBytesPerBroadcast, Bytes, EncodedBrickCount, ConsumedKeyCount);

	for (size_t i = 0; i < ConsumedKeyCount; ++i)
	{
		Impl.DirtySinceLastBroadcast.Remove(ToCoord(Keys[i]));
	}

	if (EncodedBrickCount == 0)
	{
		return; // every dirty brick this round had already emptied again -- nothing to actually tell clients
	}

	if (ConsumedKeyCount < Keys.size())
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("BroadcastWaterDiffs: byte cap reached -- sent %d/%d dirty bricks this round (%d bytes); remainder ")
		       TEXT("deferred to next ~5Hz broadcast."),
		       EncodedBrickCount, (int32)Keys.size(), Bytes.Num());
	}

	AVoxelEditRelay* Relay = nullptr;
	for (TActorIterator<AVoxelEditRelay> It(&World); It; ++It)
	{
		Relay = *It;
		break;
	}
	if (!Relay)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("BroadcastWaterDiffs: no AVoxelEditRelay in the world -- %d brick diffs not broadcast."),
		       EncodedBrickCount);
		return;
	}

	Relay->MulticastWaterDiffs(Bytes);
	Impl.ReplicatedBytesThisWindow += Bytes.Num();
}
} // namespace

void UVoxelWaterSubsystem::Tick(float DeltaTime)
{
	if (!Impl)
	{
		return;
	}
	// docs/debug-tooling-plan.md P3 / task item 5c perf budget ("<2ms/frame
	// at v0 scale"): wall-clock this whole Tick() body, published unthrottled
	// (same convention as UVoxelWorldSubsystem::FVoxelPerfSnapshot::SubsystemTickMs).
	const double TickStartSeconds = FPlatformTime::Seconds();

	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;

	if (NetMode != NM_Client)
	{
		Impl->TickAccumSeconds += DeltaTime;
		const double NowWorldSeconds = World ? World->GetTimeSeconds() : 0.0;
		int32 StepsThisFrame = 0;
		while (Impl->TickAccumSeconds >= FVoxelWaterImpl::FixedStepSeconds && StepsThisFrame < FVoxelWaterImpl::MaxStepsPerFrame)
		{
			Impl->TickAccumSeconds -= FVoxelWaterImpl::FixedStepSeconds;
			StepFixed(*Impl, NowWorldSeconds);
			++StepsThisFrame;
		}

		if (World && NetMode != NM_Standalone)
		{
			BroadcastWaterDiffs(*Impl, *World, DeltaTime);
		}
	}
	// NM_Client: authority-only simulation (task item 1) -- never steps its
	// own CA; rendering is kept current purely by ApplyReplicatedWaterDiffs.

	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);

	// --- Perf snapshot (task item 3), 1Hz -----------------------------------
	Impl->PerfRefreshAccumSeconds += DeltaTime;
	if (Impl->PerfRefreshAccumSeconds >= 1.0f)
	{
		const float Window = Impl->PerfRefreshAccumSeconds;
		Impl->PerfRefreshAccumSeconds = 0.f;

		FVoxelWaterPerfSnapshot Snap;
		Snap.ActiveBricks = int64(Impl->CA.activeBrickCount());
		Snap.StoredBricks = int64(Impl->CA.storedBrickCount());
		Snap.TotalVolume = Impl->CA.totalVolume();
		Snap.StepsPerSec = float(Impl->StepsThisWindow) / Window;
		Snap.LastSteppedBrickCount = Impl->LastSteppedBrickCount;
		Snap.ReplicatedBytesPerSec = float(Impl->ReplicatedBytesThisWindow) / Window;
		Snap.ReservoirCells = Impl->ReservoirCells.Num();

		Snap.TickMs = Impl->LastSnapshot.TickMs; // preserve the always-fresh field across this reassignment
		Impl->LastSnapshot = Snap;
		Impl->StepsThisWindow = 0;
		Impl->ReplicatedBytesThisWindow = 0;

		// Task item 5c perf-budget evidence ("water tick must stay <2ms/frame
		// at v0 scale; report actual"): a 1Hz log-line trail while water is
		// actually active, readable alongside -VoxelPerfRun's own p50/p95
		// frame-time JSON summary. Skipped while the sim is fully idle (no
		// stored bricks, zero volume) so a normal no-water run stays quiet.
		if (Snap.StoredBricks > 0 || Snap.TotalVolume > 0)
		{
			UE_LOG(LogVoxelPerf, Log,
			       TEXT("WaterPerf: activeBricks=%lld stored=%lld volume=%llu steps/s=%.1f tickMs=%.3f replKB/s=%.2f"),
			       Snap.ActiveBricks, Snap.StoredBricks, (unsigned long long)Snap.TotalVolume, Snap.StepsPerSec, Snap.TickMs,
			       Snap.ReplicatedBytesPerSec / 1024.0);
		}
	}

	Impl->LastSnapshot.TickMs = float((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0);
}

TStatId UVoxelWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWaterSubsystem, STATGROUP_Tickables);
}

uint32 UVoxelWaterSubsystem::SpawnWaterAt(const FVector& WorldLocationUU, uint32 Amount)
{
	if (!Impl)
	{
		return 0;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SpawnWaterAt refused on NM_Client -- water sim is authority-only (task item 1)."));
		return 0;
	}

	const VoxelCoords::FVoxelCoord Vc = VoxelCoords::WorldToVoxel(WorldLocationUU);
	const uint32 Placed = Impl->CA.addWater(Vc.X, Vc.Y, Vc.Z, Amount);
	if (Placed > 0)
	{
		MarkColumnDirty(*Impl, Vc.X, Vc.Y, Vc.Z, Placed);
		UE_LOG(LogVoxelWater, Log, TEXT("SpawnWaterAt: placed %u/%u fill units at voxel (%lld,%lld,%lld)"), Placed, Amount,
		       (long long)Vc.X, (long long)Vc.Y, (long long)Vc.Z);
	}
	return Placed;
}

void UVoxelWaterSubsystem::NotifyTerrainVoxelsCleared(const TArray<VoxelCoords::FVoxelCoord>& ClearedVoxels)
{
	if (!Impl || ClearedVoxels.Num() == 0)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // breach seeding is simulation state, authority-only (same rule as the CA tick)
	}

	TSet<VoxelCoords::FVoxelCoord> ClearedSet(ClearedVoxels);

	static const VoxelCoords::FVoxelCoord kNeighborOffsets[6] = {
		VoxelCoords::FVoxelCoord{1, 0, 0}, VoxelCoords::FVoxelCoord{-1, 0, 0}, VoxelCoords::FVoxelCoord{0, 1, 0},
		VoxelCoords::FVoxelCoord{0, -1, 0}, VoxelCoords::FVoxelCoord{0, 0, 1}, VoxelCoords::FVoxelCoord{0, 0, -1}};

	int32 NewlyBreachedCount = 0;
	for (const VoxelCoords::FVoxelCoord& V : ClearedVoxels)
	{
		if (V.Z >= 0)
		{
			continue; // Reservoir v0/implicit ocean is a below-sea-level concept only (SS3.7)
		}

		// "adjacent cell is implicit-ocean (i.e., non-solid, z<0, outside
		// active water)" -- crucially, a neighbor that's ALSO part of
		// ClearedSet is newly-dug-by-this-same-edit, not pre-existing ocean;
		// excluding it is what stops an isolated dry pocket (all neighbors
		// solid before the dig, hence also cleared-by-this-dig, hence
		// excluded here) from spontaneously flooding itself.
		bool bTouchesOcean = false;
		for (const VoxelCoords::FVoxelCoord& Off : kNeighborOffsets)
		{
			const VoxelCoords::FVoxelCoord N{V.X + Off.X, V.Y + Off.Y, V.Z + Off.Z};
			if (ClearedSet.Contains(N)) continue;
			if (N.Z >= 0) continue;
			if (Impl->Terrain.IsSolidAtVoxel(N.X, N.Y, N.Z)) continue;
			if (Impl->CA.fillAt(N.X, N.Y, N.Z) != 0) continue; // already-tracked active water, not "implicit ocean"
			bTouchesOcean = true;
			break;
		}
		if (!bTouchesOcean)
		{
			continue;
		}

		if (!Impl->ReservoirCells.Contains(V))
		{
			Impl->ReservoirCells.Add(V);
			++NewlyBreachedCount;
		}
		const uint8 Cur = Impl->CA.fillAt(V.X, V.Y, V.Z);
		if (Cur < 255)
		{
			Impl->CA.addWater(V.X, V.Y, V.Z, uint32(255 - Cur));
		}
		MarkColumnDirty(*Impl, V.X, V.Y, V.Z, 255);
	}

	if (NewlyBreachedCount > 0)
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Dig breach: seeded %d Reservoir v0 boundary cell(s) (full fill, continuous top-up) out of %d cleared ")
		       TEXT("voxel(s) examined."),
		       NewlyBreachedCount, ClearedVoxels.Num());
	}
}

void UVoxelWaterSubsystem::NotifyTerrainRegionEdited(const VoxelCoords::FVoxelCoord& MinVoxelIncl,
                                                       const VoxelCoords::FVoxelCoord& MaxVoxelIncl)
{
	if (!Impl)
	{
		return;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		return; // a client's local mirror never runs step()/hydrostaticPass -- nothing to invalidate
	}
	Impl->CA.invalidateSolidRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X, MaxVoxelIncl.Y,
	                               MaxVoxelIncl.Z);
}

bool UVoxelWaterSubsystem::IsSolidCacheEnabled() const
{
	return Impl && Impl->CA.solidCacheEnabled();
}

FVoxelWaterPerfSnapshot UVoxelWaterSubsystem::GetPerfSnapshot() const
{
	return Impl ? Impl->LastSnapshot : FVoxelWaterPerfSnapshot{};
}

uint64 UVoxelWaterSubsystem::GetWaterDigest() const
{
	if (!Impl)
	{
		return 0;
	}
	vxc::Digest D;
	Impl->CA.digest(D);
	return D.h;
}

bool UVoxelWaterSubsystem::GetStoredWaterCentroidUU(FVector& OutCenterUU) const
{
	if (!Impl || Impl->CA.storedBrickCount() == 0)
	{
		return false;
	}
	double SumX = 0.0, SumY = 0.0, SumZ = 0.0;
	int64 Count = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::BrickKey& Key = Entry.first;
		// Brick-center world position (UU): brick origin + half an edge.
		SumX += (double(Key.x) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumY += (double(Key.y) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		SumZ += (double(Key.z) + 0.5) * double(vxc::WaterBrick8::kEdge) * VoxelCoords::VoxelSizeUU;
		++Count;
	}
	OutCenterUU = FVector(SumX / double(Count), SumY / double(Count), SumZ / double(Count));
	return true;
}

uint8 UVoxelWaterSubsystem::GetMaxStoredFill() const
{
	if (!Impl)
	{
		return 0;
	}
	uint8_t Max = 0;
	for (const auto& Entry : Impl->CA.bricks())
	{
		const vxc::WaterBrick8& B = Entry.second;
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x) Max = FMath::Max(Max, B.get(x, y, z));
	}
	return Max;
}

bool UVoxelWaterSubsystem::ApplyReplicatedWaterDiffs(const TArray<uint8>& Bytes)
{
	if (!Impl)
	{
		return false;
	}
	vxc::ByteReader R(Bytes.GetData(), size_t(Bytes.Num()));
	uint32_t Count = 0;
	if (!R.u32(Count))
	{
		return Bytes.Num() == 0; // an empty batch is valid (nothing to apply)
	}

	for (uint32_t i = 0; i < Count; ++i)
	{
		int32_t X = 0, Y = 0, Z = 0;
		if (!R.i32(X) || !R.i32(Y) || !R.i32(Z))
		{
			return false;
		}
		const vxc::BrickKey Key{X, Y, Z};
		for (int z = 0; z < vxc::WaterBrick8::kEdge; ++z)
		{
			for (int y = 0; y < vxc::WaterBrick8::kEdge; ++y)
			{
				for (int x = 0; x < vxc::WaterBrick8::kEdge; ++x)
				{
					uint8_t Fill = 0;
					if (!R.u8(Fill))
					{
						return false;
					}
					const int64_t Vx = int64_t(Key.x) * vxc::WaterBrick8::kEdge + x;
					const int64_t Vy = int64_t(Key.y) * vxc::WaterBrick8::kEdge + y;
					const int64_t Vz = int64_t(Key.z) * vxc::WaterBrick8::kEdge + z;
					Impl->CA.setReplicatedFill(Vx, Vy, Vz, Fill);
				}
			}
		}
		Impl->DirtyBricks.Add(ToCoord(Key));
	}

	// Clients render too (v0: same re-mesh path, driven by replicated state
	// instead of a locally-stepped CA).
	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);
	return true;
}

// --- voxel.SpawnWater console command (task item 2, dev tool) --------------

namespace
{
FAutoConsoleCommandWithWorldAndArgs CVarVoxelSpawnWater(
	TEXT("voxel.SpawnWater"),
	TEXT("voxel.SpawnWater <amount> -- dev tool: dumps <amount> water fill units at the local player's crosshair (W2)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (Args.Num() < 1)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SpawnWater <amount> -- missing amount argument."));
				return;
			}
			const uint32 Amount = uint32(FMath::Max(0, FCString::Atoi(*Args[0])));

			UVoxelWaterSubsystem* WaterSubsystem = World->GetSubsystem<UVoxelWaterSubsystem>();
			UVoxelWorldSubsystem* Terrain = World->GetSubsystem<UVoxelWorldSubsystem>();
			APlayerController* PC = World->GetFirstPlayerController();
			if (!WaterSubsystem || !Terrain || !PC || !PC->PlayerCameraManager)
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SpawnWater: missing subsystem/camera; nothing spawned."));
				return;
			}

			const FVector CamLoc = PC->PlayerCameraManager->GetCameraLocation();
			const FVector CamDir = PC->PlayerCameraManager->GetCameraRotation().Vector();

			FVector HitVoxelCenterUU, PrevVoxelCenterUU;
			constexpr double MaxDistUU = 6400.0; // 64m, generous crosshair reach
			FVector TargetUU;
			if (Terrain->RaycastVoxelWorld(CamLoc, CamDir, MaxDistUU, HitVoxelCenterUU, PrevVoxelCenterUU))
			{
				TargetUU = PrevVoxelCenterUU; // last empty voxel before the hit surface -- mirrors TryPlace's aim
			}
			else
			{
				TargetUU = CamLoc + CamDir * 500.0; // no hit: 5m in front of the camera
			}

			const uint32 Placed = WaterSubsystem->SpawnWaterAt(TargetUU, Amount);
			UE_LOG(LogVoxelWater, Log, TEXT("voxel.SpawnWater: requested %u, placed %u at (%.0f,%.0f,%.0f)"), Amount, Placed,
			       TargetUU.X, TargetUU.Y, TargetUU.Z);
		}));
} // namespace
