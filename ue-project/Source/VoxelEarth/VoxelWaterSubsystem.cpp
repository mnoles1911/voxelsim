#include "VoxelWaterSubsystem.h"

#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelEditRelay.h"
#include "VoxelWaterChunkComponent.h"
#include "VoxelWorldSubsystem.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (same doctrine note as VoxelWorldSubsystem.cpp).
#include "voxelcore/amplifier.h"
#include "voxelcore/bytes.h"
#include "voxelcore/caverns.h"
#include "voxelcore/mesher.h"
#include "voxelcore/tiles.h"
#include "voxelcore/waterca.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator (locating the world's single AVoxelEditRelay)
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"  // ADR-0005 water persistence: IFileManager (atomic rename, mkdir)
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h" // ADR-0005 water persistence: FFileHelper (blob read/write)
#include "Misc/Paths.h"      // ADR-0005 water persistence: FPaths::ProjectSavedDir (mirror the .vxlog path)
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
		, Tiles(InTerrain.GetSeed())
		, Amp(InTerrain.GetSeed(), Tiles)
		, Mob(
			  // The implicit static flood field (C7, docs/cavern-design.md SS5.1):
			  // worldgen-owned, deterministic, ZERO storage. caverns.h's
			  // cavernFloodedAt is exactly half the predicate -- "is this below
			  // the column's flood level" -- and WaterMobilizer applies the other
			  // half (is the cell actually open air) itself, so this callback
			  // stays a pure worldgen query.
			  [this](int64_t vx, int64_t vy, int64_t vz) -> uint8_t
			  { return vxc::cavernFloodedAt(Amp.columnCached(vx, vy).cavern, vz) ? uint8_t(255) : uint8_t(0); },
			  [this](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
			  { return Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; })
		// NOT the bare terrain query: makeSolidFn() layers the implicit-water
		// WALL on top, which is what makes the implicit/CA ownership partition
		// structural rather than a matter of call ordering (waterca.h).
		, CA(Mob.makeSolidFn())
	{
	}

	UVoxelWorldSubsystem& Terrain;

	// OUR OWN worldgen sampler, not UVoxelWorldSubsystem's. That subsystem is
	// another agent's file and exposes no column/cavern accessor, so — exactly
	// as AVoxelClipmapActor already does for terrain height — we build a second
	// Amplifier over the same seed. Worldgen is a pure function of (seed, tile
	// sampler), so this is bit-identical to the terrain's own for every run
	// that uses the synthetic sampler (the default).
	//
	// CAVEAT, and the reason for the warning logged in OnWorldBeginPlay: a run
	// launched with -VoxelTileDir uses a real tile-grid sampler over there and
	// this synthetic one over here, so the surfaces disagree and cavern flood
	// levels would be computed against the wrong terrain. Follow-up: a public
	// column accessor on UVoxelWorldSubsystem, which its owner must add.
	vxc::SyntheticTileSampler Tiles;
	vxc::Amplifier Amp;

	// MUST be declared before CA: makeSolidFn() hands the CA a callable that
	// captures the mobilizer, so the mobilizer has to outlive it.
	vxc::WaterMobilizer Mob;

	vxc::WaterCA CA;

	// One UWaterChunkComponent per rendered IMPLICIT (not-yet-mobilized) water
	// brick, keyed the same way ChunkComponents is. Kept separate from the CA's
	// map so mobilization is a clean handover: the implicit component is
	// destroyed and the CA's is created, and because implicitFillAt() returns 0
	// for a mobilized brick the two can never both draw the same water.
	TMap<VoxelCoords::FVoxelCoord, TObjectPtr<UWaterChunkComponent>> ImplicitChunkComponents;

	// Brick coordinate the implicit-water refresh last centred on, so a
	// stationary camera costs nothing. Set once the first refresh has run.
	VoxelCoords::FVoxelCoord LastImplicitCenterBrick = {0, 0, 0};
	bool bImplicitCenterValid = false;

	// Bricks the implicit pass still owes a mesh, drained under a per-tick
	// budget exactly like DirtyBricks.
	TArray<VoxelCoords::FVoxelCoord> PendingImplicitBricks;

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

// ADR-0005 water persistence: forward declarations. The definitions live in
// the big anonymous namespace below (they lean on MarkMobilizedBricksDirty /
// ToCoord, defined there), but OnWorldBeginPlay / Deinitialize / SaveWaterState
// above them need to call them.
namespace
{
FString GetWaterSaveFilePath(uint64 Seed);
bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed);
void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed);
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
	// ADR-0005: autosave the water blob on shutdown, mirroring
	// UVoxelWorldSubsystem::Deinitialize's edit-log autosave lifetime exactly --
	// authority only, and only for a world that genuinely began play (see
	// bWorldBegunPlay). Additionally gated on there being real water state to
	// persist (stored or mobilized bricks) so a run that never disturbed
	// underground water leaves no sibling .vxwater to shadow a later same-seed
	// session -- the edit log has no such gate because every world has a log,
	// but an all-implicit world has nothing to serialize and an empty blob would
	// only add a spurious file.
	if (Impl && bWorldBegunPlay)
	{
		UWorld* World = GetWorld();
		const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
		if (NetMode != NM_Client && (Impl->CA.storedBrickCount() > 0 || !Impl->Mob.mobilizedBricks().empty()))
		{
			SaveWaterState();
		}
	}

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

	// ADR-0005: from here on this is a genuine gameplay world (same reasoning as
	// UVoxelWorldSubsystem's bWorldBegunPlay) -- gates Deinitialize's autosave.
	bWorldBegunPlay = true;

	// ADR-0005 water persistence: the authority (server/listen/standalone) loads
	// Saved/VoxelWorlds/<seed>.vxwater, if present, before the first fixed step
	// ever runs (Tick() below). This is the earliest point OnWorldBeginPlay
	// reaches after the game-world/Impl guard, and it runs BEFORE the
	// dedicated-server early-return below so it applies to every authority role,
	// exactly mirroring UVoxelWorldSubsystem's LoadEditLogFromDisk placement. The
	// live CA/mob are still fresh here (no addWater/mobilize has run yet), which
	// is vxc::WaterState::applyTo's precondition. Fills-first load, three
	// failure modes handled loudly -- see LoadWaterStateFromDisk. NM_Client never
	// loads its own file: a joining client mirrors state via replication instead.
	if (InWorld.GetNetMode() != NM_Client)
	{
		LoadWaterStateFromDisk(*Impl, Impl->Terrain.GetSeed());
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

// --- C7/C8: implicit cavern water ------------------------------------------

// Drains the mobilizer's newly-converted-brick queue. A mobilized brick must
// stop drawing as implicit water and start drawing as CA water in the SAME
// frame or the handover visibly flickers, so this both marks it dirty for the
// CA re-mesh and destroys its implicit component.
void MarkMobilizedBricksDirty(FVoxelWaterImpl& Impl)
{
	for (const vxc::BrickKey& K : Impl.Mob.takeRecentlyMobilized())
	{
		const VoxelCoords::FVoxelCoord C = ToCoord(K);
		Impl.DirtyBricks.Add(C);
		Impl.DirtySinceLastBroadcast.Add(C);
		if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(C))
		{
			if (*Existing)
			{
				(*Existing)->DestroyComponent();
			}
			Impl.ImplicitChunkComponents.Remove(C);
		}
	}
}

// --- ADR-0005 water persistence -------------------------------------------
//
// The blob is a SIBLING of the terrain edit log, written into the same
// directory with the same per-seed naming and the same atomic tmp+rename write
// UVoxelWorldSubsystem uses (GetWorldSaveFilePath / WriteBytesAtomic in
// VoxelWorldSubsystem.cpp). Only the extension differs: <seed>.vxwater beside
// <seed>.vxlog. See waterca.h's WaterState comment for why it is a separate
// file rather than a section of the log (it invalidates on kWaterCAVersion and
// is discardable without discarding terrain edits).
FString GetWaterSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxwater"), (unsigned long long)Seed);
}

// The .vxlog the terrain edit log saves to -- used only to decide how LOUD a
// missing water blob should be (a missing blob beside an existing terrain save
// means a world that had edits, some possibly drains, will refill every cavern;
// a missing blob with no terrain save is just a fresh world).
FString GetTerrainSaveFilePath(uint64 Seed)
{
	return FPaths::ProjectSavedDir() / TEXT("VoxelWorlds") / FString::Printf(TEXT("%llu.vxlog"), (unsigned long long)Seed);
}

// Atomic tmp+rename write, identical shape to VoxelWorldSubsystem.cpp's
// WriteBytesAtomic: a process dying mid-write leaves only the .tmp behind,
// never a truncated/corrupt blob a later load would have to reject.
bool WriteWaterBytesAtomic(const FString& Path, const TArray<uint8>& Bytes)
{
	const FString TmpPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveArrayToFile(Bytes, *TmpPath))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to write temp file %s"), *TmpPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TmpPath, /*Replace*/ true))
	{
		UE_LOG(LogVoxelWater, Error, TEXT("SaveWaterState: failed to rename %s -> %s"), *TmpPath, *Path);
		IFileManager::Get().Delete(*TmpPath);
		return false;
	}
	return true;
}

bool SaveWaterStateToDisk(const FVoxelWaterImpl& Impl, uint64 Seed)
{
	// vxc::WaterState::serialize appends the whole blob (magic + kFormatVersion +
	// kWaterCAVersion + totalVolume integrity check + per-brick fill + active set
	// + mobilized keys) into a caller-owned buffer, exactly like EditLog.
	std::vector<uint8_t> Bytes;
	vxc::WaterState::serialize(Impl.CA, Impl.Mob, Bytes);

	TArray<uint8> OutBytes;
	OutBytes.SetNumUninitialized((int32)Bytes.size());
	if (!Bytes.empty())
	{
		FMemory::Memcpy(OutBytes.GetData(), Bytes.data(), Bytes.size());
	}

	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VoxelWorlds");
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = GetWaterSaveFilePath(Seed);
	if (!WriteWaterBytesAtomic(Path, OutBytes))
	{
		return false;
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("SaveWaterState: wrote %d bytes (%llu fill units, %llu stored brick(s), %llu mobilized brick(s)) to %s -- waterDigest=0x%016llX"),
	       OutBytes.Num(), (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
	return true;
}

void LoadWaterStateFromDisk(FVoxelWaterImpl& Impl, uint64 Seed)
{
	// Honor -VoxelNoLoad exactly as LoadEditLogFromDisk does: a fresh-start
	// verification aid must start the water field fully implicit too, or terrain
	// would start fresh while water came back from a stale blob.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelNoLoad")))
	{
		UE_LOG(LogVoxelWater, Log,
		       TEXT("LoadWaterState: -VoxelNoLoad passed -- skipping water-state load; underground water starts fully implicit."));
		return;
	}

	const FString Path = GetWaterSaveFilePath(Seed);
	if (!FPaths::FileExists(Path))
	{
		// FAILURE MODE 1 -- MISSING. Coherent (the world reverts to the implicit,
		// full field) but every drained cavern refills, so it is LOUD whenever a
		// terrain save exists (that world had edits; some may have been drains)
		// and merely informational for a genuinely fresh world with no log either.
		if (FPaths::FileExists(GetTerrainSaveFilePath(Seed)))
		{
			UE_LOG(LogVoxelWater, Warning,
			       TEXT("LoadWaterState: a terrain save exists but there is NO water blob at %s -- falling back to a fully ")
			       TEXT("implicit world. EVERY drained cavern in this world WILL REFILL (ADR-0005). Expected only for a save ")
			       TEXT("that predates water persistence; otherwise the water blob was lost."),
			       *Path);
		}
		else
		{
			UE_LOG(LogVoxelWater, Log,
			       TEXT("LoadWaterState: no water blob at %s and no terrain save -- fresh world, underground water is fully implicit."),
			       *Path);
		}
		return;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s exists but could NOT be READ -- falling back to a fully implicit world; ")
		       TEXT("every drained cavern WILL REFILL (ADR-0005)."),
		       *Path);
		return;
	}

	// FAILURE MODES 2 (stale kWaterCAVersion) and 3 (refused: corrupt/truncated,
	// integrity cross-check mismatch, non-fresh target) collapse into the single
	// signal the ADR-0005 handoff names: vxc::WaterState::load returning false.
	// All three take the SAME loud fallback, because handling any of them quietly
	// silently loses every drained lake. load() applies fills FIRST, then the
	// active set, then markMobilized (waterca.h) -- so the world is never even
	// momentarily in the both-accountants-report-zero state that reads as open air.
	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), Impl.CA, Impl.Mob))
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("LoadWaterState: water blob %s (%d bytes) was REFUSED by vxc::WaterState::load -- stale kWaterCAVersion ")
		       TEXT("(engine is now v%u), corrupt/truncated, or a failed totalVolume integrity cross-check. Falling back to a ")
		       TEXT("fully implicit world; EVERY drained cavern in this world WILL REFILL (ADR-0005)."),
		       *Path, Bytes.Num(), vxc::kWaterCAVersion);
		return;
	}

	// Success. Every restored brick needs meshing on the first frame: the
	// mobilized ones must stop drawing as implicit water and start drawing as CA
	// water (drain takeRecentlyMobilized via MarkMobilizedBricksDirty, tearing
	// down any implicit component -- there are none yet this early, so this just
	// queues the CA re-mesh), and every stored CA brick must mesh its fill.
	MarkMobilizedBricksDirty(Impl);
	for (const auto& Entry : Impl.CA.bricks())
	{
		Impl.DirtyBricks.Add(ToCoord(Entry.first));
	}

	vxc::Digest D;
	Impl.CA.digest(D);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("LoadWaterState: restored %llu fill units across %llu stored brick(s), %llu mobilized brick(s) from %s -- waterDigest=0x%016llX"),
	       (unsigned long long)Impl.CA.totalVolume(), (unsigned long long)Impl.CA.storedBrickCount(),
	       (unsigned long long)Impl.Mob.mobilizedBricks().size(), *Path, (unsigned long long)D.h);
}

// How far around the camera implicit lake surfaces are built, in water bricks
// (8 voxels = 0.8 m each). 32 bricks is ~25 m, comfortably more than a cavern
// room's half-width, so standing in a flooded chamber shows its whole surface.
constexpr int32 kImplicitRadiusBricks = 32;
constexpr int32 kImplicitRadiusBricksZ = 16;
// Same budget discipline as kMaxBrickMeshesPerTick: the candidate box is large
// and a first refresh must never land as one frame's worth of work.
constexpr int32 kMaxImplicitMeshesPerTick = 192;

// Rebuilds the implicit-water candidate list when the camera crosses into a new
// brick, then meshes a budgeted slice of it every tick.
//
// WHY THIS MESHES BRICKS RATHER THAN DROPPING IN A FLAT PLANE. A plane at
// floodZ (the AVoxelOceanActor trick) would be cheaper and the design doc even
// suggests the same material family — but mobilization would then be a visible
// seam, a flat sheet abruptly becoming voxel water. Going through the very same
// meshBrick<8> path the CA uses means an implicit brick and a mobilized brick
// are pixel-identical, so a draining lake reads as one continuous body with a
// hole appearing in it. That is precisely the shot this feature has to sell.
void RefreshImplicitWater(FVoxelWaterImpl& Impl, const FVector& CameraUU, AActor* ChunkOwner,
                          USceneComponent* ChunkRoot, UMaterialInterface* Material)
{
	if (!ChunkOwner || !ChunkRoot)
	{
		return; // dedicated server: never render
	}

	const int64 CamVx = int64(FMath::FloorToDouble(CameraUU.X / VoxelCoords::VoxelSizeUU));
	const int64 CamVy = int64(FMath::FloorToDouble(CameraUU.Y / VoxelCoords::VoxelSizeUU));
	const int64 CamVz = int64(FMath::FloorToDouble(CameraUU.Z / VoxelCoords::VoxelSizeUU));
	const VoxelCoords::FVoxelCoord Center{
		int32(vxc::floorDiv(CamVx, vxc::WaterBrick8::kEdge)), int32(vxc::floorDiv(CamVy, vxc::WaterBrick8::kEdge)),
		int32(vxc::floorDiv(CamVz, vxc::WaterBrick8::kEdge))};

	if (!Impl.bImplicitCenterValid || Center != Impl.LastImplicitCenterBrick)
	{
		Impl.LastImplicitCenterBrick = Center;
		Impl.bImplicitCenterValid = true;
		Impl.PendingImplicitBricks.Reset();

		// Cheap reject, one column query per brick COLUMN rather than 512 per
		// brick: the flood level is a per-site constant across its whole reach
		// disc, so a column that is dry has no flooded brick anywhere in its
		// stack and the entire vertical run is skipped.
		for (int32 By = Center.Y - kImplicitRadiusBricks; By <= Center.Y + kImplicitRadiusBricks; ++By)
		{
			for (int32 Bx = Center.X - kImplicitRadiusBricks; Bx <= Center.X + kImplicitRadiusBricks; ++Bx)
			{
				const int64 Vx = int64(Bx) * vxc::WaterBrick8::kEdge;
				const int64 Vy = int64(By) * vxc::WaterBrick8::kEdge;
				const int32 FloodZMm = Impl.Amp.columnCached(Vx, Vy).cavern.floodZMm;
				if (FloodZMm == INT32_MIN)
				{
					continue; // dry column
				}
				// Only bricks whose bottom sits below the flood level can hold
				// any water at all.
				const int64 FloodBrickZ = vxc::floorDiv(int64(FloodZMm) / vxc::kVoxelSizeMm, vxc::WaterBrick8::kEdge);
				for (int32 Bz = Center.Z - kImplicitRadiusBricksZ; Bz <= Center.Z + kImplicitRadiusBricksZ; ++Bz)
				{
					if (int64(Bz) > FloodBrickZ)
					{
						continue;
					}
					Impl.PendingImplicitBricks.Add(VoxelCoords::FVoxelCoord{Bx, By, Bz});
				}
			}
		}

		// Farthest first, because the drain below Pop()s from the BACK: the
		// water actually in front of the camera has to mesh in the first few
		// ticks, not after the whole 25 m disc has been walked.
		UE_LOG(LogVoxelWater, Log,
		       TEXT("RefreshImplicitWater: rebuilt at brick (%d,%d,%d) [cam (%.0f,%.0f,%.0f) UU] -- %d candidate brick(s)"),
		       Center.X, Center.Y, Center.Z, CameraUU.X, CameraUU.Y, CameraUU.Z, Impl.PendingImplicitBricks.Num());
		Impl.PendingImplicitBricks.Sort(
			[Center](const VoxelCoords::FVoxelCoord& A, const VoxelCoords::FVoxelCoord& B)
			{
				const int64 Da = int64(A.X - Center.X) * (A.X - Center.X) + int64(A.Y - Center.Y) * (A.Y - Center.Y) +
				                 int64(A.Z - Center.Z) * (A.Z - Center.Z);
				const int64 Db = int64(B.X - Center.X) * (B.X - Center.X) + int64(B.Y - Center.Y) * (B.Y - Center.Y) +
				                 int64(B.Z - Center.Z) * (B.Z - Center.Z);
				return Da > Db;
			});
	}

	int32 MeshesThisTick = 0;
	int32 Built = 0;
	while (Impl.PendingImplicitBricks.Num() > 0 && MeshesThisTick < kMaxImplicitMeshesPerTick)
	{
		const VoxelCoords::FVoxelCoord BrickCoord = Impl.PendingImplicitBricks.Pop(EAllowShrinking::No);
		const vxc::BrickKey Key = ToBrickKey(BrickCoord);
		const int64_t Ox = int64_t(Key.x) * vxc::WaterBrick8::kEdge;
		const int64_t Oy = int64_t(Key.y) * vxc::WaterBrick8::kEdge;
		const int64_t Oz = int64_t(Key.z) * vxc::WaterBrick8::kEdge;

		++MeshesThisTick;

		// implicitFillAt() already returns 0 for a mobilized brick, so a
		// converted brick simply meshes to nothing here and the CA's own
		// component takes over — the ownership partition does the bookkeeping.
		std::vector<vxc::Quad> RawQuads;
		const auto Sampler = [&Impl, Ox, Oy, Oz](int x, int y, int z) -> vxc::MaterialId
		{
			return Impl.Mob.implicitFillAt(Ox + x, Oy + y, Oz + z) >= 128 ? vxc::MAT_ROCK : vxc::MAT_AIR;
		};
		vxc::meshBrick<vxc::WaterBrick8::kEdge>(Sampler, RawQuads);

		if (RawQuads.empty())
		{
			// Dry, fully submerged (interior bricks emit no faces — which is
			// exactly what keeps a large lake affordable), or mobilized.
			if (TObjectPtr<UWaterChunkComponent>* Existing = Impl.ImplicitChunkComponents.Find(BrickCoord))
			{
				if (*Existing)
				{
					(*Existing)->DestroyComponent();
				}
				Impl.ImplicitChunkComponents.Remove(BrickCoord);
			}
			continue;
		}

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

		TObjectPtr<UWaterChunkComponent>* ExistingPtr = Impl.ImplicitChunkComponents.Find(BrickCoord);
		UWaterChunkComponent* Comp = ExistingPtr ? ExistingPtr->Get() : nullptr;
		if (!Comp)
		{
			Comp = NewObject<UWaterChunkComponent>(ChunkOwner);
			Comp->SetupAttachment(ChunkRoot);
			Comp->SetRelativeLocation(FVector(double(Ox) * VoxelCoords::VoxelSizeUU, double(Oy) * VoxelCoords::VoxelSizeUU,
			                                   double(Oz) * VoxelCoords::VoxelSizeUU));
			Comp->SetMaterial(0, Material);
			Comp->RegisterComponent();
			Impl.ImplicitChunkComponents.Add(BrickCoord, Comp);
		}
		Comp->SetChunkQuads(MoveTemp(Quads));
		++Built;
	}

	if (MeshesThisTick > 0 && Impl.PendingImplicitBricks.Num() == 0)
	{
		UE_LOG(LogVoxelWater, Verbose, TEXT("RefreshImplicitWater: candidate list drained; %d implicit water component(s) live."),
		       Impl.ImplicitChunkComponents.Num());
	}
	(void)Built;
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

	// C8 mobilize-on-approach: advance the implicit->CA front BEFORE stepping,
	// so any brick this tick's water could flow into is already CA-owned. Over
	// budget simply defers — a deferred brick is still a wall to the CA, so
	// deferring can never leak water (waterca.h). Cheap when nothing is active:
	// a settled or empty CA has no active bricks, so no front to advance.
	if (Impl.Mob.advanceFront(Impl.CA) > 0)
	{
		MarkMobilizedBricksDirty(Impl);
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

	// C7 static cavern lakes. Runs on clients too: the implicit field is pure
	// worldgen, so a client derives WHERE the water is straight from the seed
	// with no replication at all. What does have to replicate is which bricks
	// have MOBILIZED, because that is simulation state -- see
	// ApplyReplicatedWaterDiffs.
	if (World)
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			FVector CameraUU = FVector::ZeroVector;
			FRotator UnusedRot = FRotator::ZeroRotator;
			PC->GetPlayerViewPoint(CameraUU, UnusedRot);
			RefreshImplicitWater(*Impl, CameraUU, ChunkOwner, ChunkRoot, WaterMaterial);
		}
	}

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

	// ...and then actually WAKE the water this edit can affect. The memo
	// invalidation above only fixes what the CA believes about terrain; without
	// this call a settled body never ticks again and simply ignores the edit
	// (see the header comment on this function, and voxelcore/waterca.h
	// "Terrain-edit reactivation"). Scheduling only: no fill is written, so the
	// conservation ledger is untouched. Water-bearing bricks only, within one
	// brick of the edit -- an edit nowhere near water costs one bounded probe
	// and wakes nothing.
	// C8 mobilize-on-approach, THE SEED (docs/cavern-design.md SS5.2). This has
	// to happen here and not only in the CA-activity front, because digging into
	// the wall of a static lake produces no CA activity whatsoever -- there is no
	// CA water yet -- so without an edit hook the advancing front would have
	// nothing to grow from and the player would face a frozen wall of water.
	// Costs a bounded scan of the edit's own bricks when they hold no implicit
	// water, which is every edit outside a flooded cavern.
	const size_t Converted = Impl->Mob.mobilizeEditRegion(Impl->CA, MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z,
	                                                      MaxVoxelIncl.X, MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Converted > 0)
	{
		MarkMobilizedBricksDirty(*Impl);
		UE_LOG(LogVoxelWater, Log,
		       TEXT("Mobilized %d cavern water brick(s) on edit [%d,%d,%d]..[%d,%d,%d] (implicit -> CA; ledger %llu debited / %llu credited, shortfall %llu)"),
		       static_cast<int32>(Converted), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z, (unsigned long long)Impl->Mob.debitedVolume(),
		       (unsigned long long)Impl->Mob.creditedVolume(), (unsigned long long)Impl->Mob.shortfallVolume());
	}

	const size_t Woken = Impl->CA.wakeRegion(MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
	                                         MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	if (Woken > 0)
	{
		UE_LOG(LogVoxelWater, Verbose,
		       TEXT("NotifyTerrainRegionEdited: woke %d water brick(s) for edit [%d,%d,%d]..[%d,%d,%d]"),
		       static_cast<int32>(Woken), MinVoxelIncl.X, MinVoxelIncl.Y, MinVoxelIncl.Z, MaxVoxelIncl.X,
		       MaxVoxelIncl.Y, MaxVoxelIncl.Z);
	}
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

// --- C7/C8 underground water ------------------------------------------------

bool UVoxelWaterSubsystem::GetCavernFloodZUU(double WorldXUU, double WorldYUU, double& OutFloodZUU) const
{
	if (!Impl)
	{
		return false;
	}
	const int64 Vx = int64(FMath::FloorToDouble(WorldXUU / VoxelCoords::VoxelSizeUU));
	const int64 Vy = int64(FMath::FloorToDouble(WorldYUU / VoxelCoords::VoxelSizeUU));
	const int32 FloodZMm = Impl->Amp.columnCached(Vx, Vy).cavern.floodZMm;
	if (FloodZMm == INT32_MIN)
	{
		return false;
	}
	OutFloodZUU = double(FloodZMm) / 10.0; // mm -> UU (1 UU = 10 mm)
	return true;
}

bool UVoxelWaterSubsystem::FindFloodedCavernNear(const FVector& OriginUU, double SearchRadiusUU,
                                                  FVector& OutWaterSurfaceUU) const
{
	if (!Impl)
	{
		return false;
	}
	// 30 m grid: under a cavern site's ~36 m reach radius, so a site inside the
	// search radius cannot be stepped over.
	constexpr double StepUU = 3000.0;
	const int32 Steps = FMath::Max(1, int32(SearchRadiusUU / StepUU));

	// Contiguous open voxels spanning the waterline at this column: how much
	// lake there is below and how much headroom above. This, not a two-point
	// probe, is what tells the middle of a chamber from its tapering edge.
	constexpr int32 kProbeVoxels = 60; // +/- 6 m
	constexpr int32 kMinAirSpanVoxels = 40; // 4 m of open water + headroom
	// Scored as 2*min(headroom, depth) rather than the plain sum: a column with
	// 6 m of water and no roof clearance scores the same as one with 6 m of
	// roof and no water under the plain sum, and only the BALANCED one gives a
	// camera both a lake to look at and somewhere to stand.
	const auto AirSpanAround = [this](int64 Vx, int64 Vy, int64 Vz) -> int32
	{
		int32 Above = 0, Below = 0;
		for (int32 D = 0; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz + D)) break;
			++Above;
		}
		for (int32 D = 1; D < kProbeVoxels; ++D)
		{
			if (Impl->Terrain.IsSolidAtVoxel(Vx, Vy, Vz - D)) break;
			++Below;
		}
		return 2 * FMath::Min(Above, Below);
	};

	// Expanding rings, so the NEAREST flooded cavern wins rather than whichever
	// one a raster scan happens to reach first.
	for (int32 Ring = 0; Ring <= Steps; ++Ring)
	{
		for (int32 Iy = -Ring; Iy <= Ring; ++Iy)
		{
			for (int32 Ix = -Ring; Ix <= Ring; ++Ix)
			{
				if (Ring > 0 && FMath::Abs(Ix) != Ring && FMath::Abs(Iy) != Ring)
				{
					continue; // interior of the ring: already tested on an earlier ring
				}
				const double Wx = OriginUU.X + double(Ix) * StepUU;
				const double Wy = OriginUU.Y + double(Iy) * StepUU;
				const int64 Vx = int64(FMath::FloorToDouble(Wx / VoxelCoords::VoxelSizeUU));
				const int64 Vy = int64(FMath::FloorToDouble(Wy / VoxelCoords::VoxelSizeUU));

				// BY VALUE, not by reference. Amplifier::columnCached returns a
				// reference into a per-thread memo that the NEXT columnCached
				// call overwrites, and the refinement loop below makes hundreds
				// of them -- reading Col afterwards is a dangling read (it was
				// silently reporting the wrong cavern seg count before this).
				const vxc::ColumnSample Col = Impl->Amp.columnCached(Vx, Vy);
				if (Col.cavern.floodZMm == INT32_MIN || Col.cavern.count == 0)
				{
					continue; // dry, or no room actually reaches this column
				}

				// A column can carry a site's flood level while its own rooms are
				// truncated away by the roof/bedrock clamps, so require the
				// column to be genuinely open here: water below the flood level
				// and real headroom above it.
				const int64 FloodVz = int64(Col.cavern.floodZMm) / vxc::kVoxelSizeMm;
				if (AirSpanAround(Vx, Vy, FloodVz) < kMinAirSpanVoxels)
				{
					continue;
				}

				// REFINE. The search grid is 30 m and a room is 24-56 m across,
				// so the first hit is usually somewhere on the reach disc's edge,
				// where the roof closes down and a camera placed "just above the
				// water" ends up inside rock -- which renders as a see-through
				// world, because the surrounding geometry is all backfaces.
				// Walk a local grid and take the column with the most open air
				// around the waterline, i.e. the middle of the chamber.
				double BestWx = Wx, BestWy = Wy;
				int32 BestSpan = AirSpanAround(Vx, Vy, FloodVz);
				constexpr double RefineStepUU = 200.0; // 2 m
				constexpr int32 RefineSteps = 12;      // +/- 24 m
				for (int32 Ry = -RefineSteps; Ry <= RefineSteps; ++Ry)
				{
					for (int32 Rx = -RefineSteps; Rx <= RefineSteps; ++Rx)
					{
						const double Cx = Wx + double(Rx) * RefineStepUU;
						const double Cy = Wy + double(Ry) * RefineStepUU;
						const int64 Cvx = int64(FMath::FloorToDouble(Cx / VoxelCoords::VoxelSizeUU));
						const int64 Cvy = int64(FMath::FloorToDouble(Cy / VoxelCoords::VoxelSizeUU));
						// Must belong to the SAME lake, not a neighbouring site.
						if (Impl->Amp.columnCached(Cvx, Cvy).cavern.floodZMm != Col.cavern.floodZMm)
						{
							continue;
						}
						const int32 Span = AirSpanAround(Cvx, Cvy, FloodVz);
						if (Span > BestSpan)
						{
							BestSpan = Span;
							BestWx = Cx;
							BestWy = Cy;
						}
					}
				}

				OutWaterSurfaceUU = FVector(BestWx, BestWy, double(Col.cavern.floodZMm) / 10.0);
				UE_LOG(LogVoxelWater, Log,
				       TEXT("FindFloodedCavernNear: flooded cavern at (%.0f, %.0f) UU, water surface z=%.0f UU (floodZ %d mm), %d seg(s), open air span %d voxels (%.1f m) around the waterline"),
				       BestWx, BestWy, OutWaterSurfaceUU.Z, Col.cavern.floodZMm, Col.cavern.count, BestSpan,
				       double(BestSpan) * VoxelCoords::VoxelSizeUU / 100.0);
				return true;
			}
		}
	}
	UE_LOG(LogVoxelWater, Warning, TEXT("FindFloodedCavernNear: no flooded cavern within %.0f UU of (%.0f, %.0f)."),
	       SearchRadiusUU, OriginUU.X, OriginUU.Y);
	return false;
}

int32 UVoxelWaterSubsystem::CarveCavernOutflow(const FVector& LakeSurfaceUU)
{
	if (!Impl)
	{
		return 0;
	}

	// Walk +X until the flood field runs out -- that is the edge of this site's
	// reach disc, and the first place water can actually go.
	constexpr double ProbeStepUU = 200.0;  // 2 m
	constexpr double MaxProbeUU = 8000.0;  // 80 m, comfortably past a site's reach
	double ExitX = LakeSurfaceUU.X;
	bool bFoundExit = false;
	for (double D = ProbeStepUU; D <= MaxProbeUU; D += ProbeStepUU)
	{
		double Unused = 0.0;
		if (!GetCavernFloodZUU(LakeSurfaceUU.X + D, LakeSurfaceUU.Y, Unused))
		{
			ExitX = LakeSurfaceUU.X + D;
			bFoundExit = true;
			break;
		}
	}
	if (!bFoundExit)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("CarveCavernOutflow: no dry column within %.0f UU -- lake has nowhere to drain."), MaxProbeUU);
		return 0;
	}

	// A tunnel just under the water surface, so it takes the top of the lake
	// and the level visibly drops, then a shaft down past the dry edge so the
	// water keeps running away instead of backing up.
	constexpr double TunnelRadiusUU = 250.0; // 2.5 m
	constexpr double StepUU = 150.0;         // 1.5 m, well under the radius: no gaps
	const double TunnelZ = LakeSurfaceUU.Z - 100.0; // 1 m below the surface

	int32 Removed = 0;
	int32 Spheres = 0, ProductiveSpheres = 0;
	for (double X = LakeSurfaceUU.X; X <= ExitX + 400.0; X += StepUU)
	{
		// Three stacked passes rather than one: a single-radius tunnel at a
		// fixed z can thread a chamber that is open at that exact height for
		// its whole length and remove NOTHING, which is a silent no-drain.
		// Spanning 5 m vertically guarantees it meets the chamber's rock.
		for (double DZ = -200.0; DZ <= 200.0; DZ += 200.0)
		{
			const int32 R = Impl->Terrain.CarveSphere(FVector(X, LakeSurfaceUU.Y, TunnelZ + DZ), TunnelRadiusUU, 0.0);
			++Spheres;
			if (R > 0) ++ProductiveSpheres;
			Removed += R;
		}
	}
	// Drop shaft at the dry end.
	for (double Z = TunnelZ; Z >= TunnelZ - 1500.0; Z -= StepUU)
	{
		Removed += Impl->Terrain.CarveSphere(FVector(ExitX + 400.0, LakeSurfaceUU.Y, Z), TunnelRadiusUU, 0.0);
	}

	int32 MobBricks = 0;
	uint64 Deb = 0, Cred = 0, Short = 0;
	GetMobilizationStats(MobBricks, Deb, Cred, Short);
	UE_LOG(LogVoxelWater, Log,
	       TEXT("CarveCavernOutflow: removed %d voxels via %d/%d productive sphere(s), exit at x=%.0f UU. Mobilized %d brick(s), ledger %llu debited / %llu credited, shortfall %llu."),
	       Removed, ProductiveSpheres, Spheres, ExitX, MobBricks, (unsigned long long)Deb, (unsigned long long)Cred,
	       (unsigned long long)Short);
	if (Removed == 0)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("CarveCavernOutflow: carved nothing -- every target cell was already air, so no edit fired and nothing mobilized."));
	}
	return Removed;
}

uint8 UVoxelWaterSubsystem::GetImplicitFillAtWorld(const FVector& WorldUU) const
{
	if (!Impl)
	{
		return 0;
	}
	return Impl->Mob.implicitFillAt(int64(FMath::FloorToDouble(WorldUU.X / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Y / VoxelCoords::VoxelSizeUU)),
	                                 int64(FMath::FloorToDouble(WorldUU.Z / VoxelCoords::VoxelSizeUU)));
}

void UVoxelWaterSubsystem::GetMobilizationStats(int32& OutMobilizedBricks, uint64& OutDebited, uint64& OutCredited,
                                                 uint64& OutShortfall) const
{
	OutMobilizedBricks = Impl ? int32(Impl->Mob.mobilizedBricks().size()) : 0;
	OutDebited = Impl ? Impl->Mob.debitedVolume() : 0;
	OutCredited = Impl ? Impl->Mob.creditedVolume() : 0;
	OutShortfall = Impl ? Impl->Mob.shortfallVolume() : 0;
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

		// HOW A CLIENT AGREES WITH THE SERVER ON WHAT HAS MOBILIZED (C8).
		// Mobilization is authority-only: it depends on where players dug and
		// when, so a client that ran its own front off its replication-mirror
		// CA would drift the instant a packet was late. Instead the authority
		// mobilizes and the client learns it from THIS stream, at zero extra
		// bytes -- mobilizing a brick writes fill into it, which dirties it,
		// which is exactly what puts it in this batch. So "the server sent me
		// authoritative fill for this brick" IS "this brick has mobilized",
		// and marking it here flips the client's own ownership partition in
		// the same packet that delivers the water.
		//
		// markMobilized credits nothing, which is the point: those units are
		// already in the replicated fill below, and crediting them again would
		// duplicate them (waterca.h).
		Impl->Mob.markMobilized(Key);

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

	// Tear down any implicit-water components for bricks this batch converted,
	// so the client's handover looks identical to the authority's.
	MarkMobilizedBricksDirty(*Impl);

	// Clients render too (v0: same re-mesh path, driven by replicated state
	// instead of a locally-stepped CA).
	RemeshDirtyBricks(*Impl, ChunkOwner, ChunkRoot, WaterMaterial);
	return true;
}

// --- ADR-0005 water persistence: public save + round-trip verification ------

bool UVoxelWaterSubsystem::SaveWaterState() const
{
	if (!Impl)
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("SaveWaterState: no water Impl -- nothing to save."));
		return false;
	}
	UWorld* World = GetWorld();
	const ENetMode NetMode = World ? World->GetNetMode() : NM_Standalone;
	if (NetMode == NM_Client)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("SaveWaterState: refused on NM_Client -- only the authority (server/listen/standalone) has an authoritative CA to persist."));
		return false;
	}
	return SaveWaterStateToDisk(*Impl, Impl->Terrain.GetSeed());
}

bool UVoxelWaterSubsystem::VerifyWaterDiskRoundTrip(uint64& OutLiveDigest, uint64& OutReloadedDigest, uint64& OutLiveVolume,
                                                     uint64& OutReloadedVolume, int32& OutLiveMobilized, int32& OutReloadedMobilized)
{
	OutLiveDigest = OutReloadedDigest = 0;
	OutLiveVolume = OutReloadedVolume = 0;
	OutLiveMobilized = OutReloadedMobilized = 0;
	if (!Impl)
	{
		return false;
	}

	// Live side.
	{
		vxc::Digest D;
		Impl->CA.digest(D);
		OutLiveDigest = D.h;
		OutLiveVolume = Impl->CA.totalVolume();
		OutLiveMobilized = int32(Impl->Mob.mobilizedBricks().size());
	}

	// Read back the ACTUAL on-disk blob (proving the file wiring, not just the
	// in-memory serializer) and apply it into a FRESH CA + mobilizer pair built
	// over the same implicit-flood / terrain-solidity callbacks the live pair
	// uses (FVoxelWaterImpl's constructor) -- this is the load path a genuine
	// reload runs, isolated so it never touches live state.
	const FString Path = GetWaterSaveFilePath(Impl->Terrain.GetSeed());
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: could not read %s -- run SaveWaterState first."), *Path);
		return false;
	}

	FVoxelWaterImpl& I = *Impl;
	vxc::WaterMobilizer FreshMob(
		[&I](int64_t vx, int64_t vy, int64_t vz) -> uint8_t
		{ return vxc::cavernFloodedAt(I.Amp.columnCached(vx, vy).cavern, vz) ? uint8_t(255) : uint8_t(0); },
		[&I](int64_t vx, int64_t vy, int64_t vz) -> vxc::MaterialId
		{ return I.Terrain.IsSolidAtVoxel(vx, vy, vz) ? vxc::MAT_ROCK : vxc::MAT_AIR; });
	vxc::WaterCA FreshCA(FreshMob.makeSolidFn());

	if (!vxc::WaterState::load(Bytes.GetData(), (size_t)Bytes.Num(), FreshCA, FreshMob))
	{
		UE_LOG(LogVoxelWater, Warning, TEXT("VerifyWaterDiskRoundTrip: vxc::WaterState::load REFUSED the on-disk blob %s."), *Path);
		return false;
	}

	vxc::Digest RD;
	FreshCA.digest(RD);
	OutReloadedDigest = RD.h;
	OutReloadedVolume = FreshCA.totalVolume();
	OutReloadedMobilized = int32(FreshMob.mobilizedBricks().size());
	return true;
}

// --- voxel.SpawnWater console command (task item 2, dev tool) --------------

namespace
{
// ADR-0005 dev/verification convenience: force a water-state save without
// waiting for shutdown. Mirrors the voxel.SaveWorld console command
// (VoxelWorldSubsystem.cpp) -- same manual-save-alongside-autosave shape.
FAutoConsoleCommandWithWorld CVarVoxelSaveWater(
	TEXT("voxel.SaveWater"),
	TEXT("ADR-0005: write the water-state blob (Saved/VoxelWorlds/<seed>.vxwater) now, beside the terrain edit log."),
	FConsoleCommandWithWorldDelegate::CreateLambda(
		[](UWorld* World)
		{
			if (!World)
			{
				return;
			}
			if (UVoxelWaterSubsystem* Water = World->GetSubsystem<UVoxelWaterSubsystem>())
			{
				Water->SaveWaterState();
			}
			else
			{
				UE_LOG(LogVoxelWater, Warning, TEXT("voxel.SaveWater: no UVoxelWaterSubsystem in this world."));
			}
		}));

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
