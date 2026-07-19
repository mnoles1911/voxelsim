#include "VoxelWorldSubsystem.h"

#include "VoxelChunkComponent.h"
#include "VoxelCoords.h"
#include "VoxelEarth.h"
#include "VoxelMeshTypes.h"

// voxel-core is UE-header-free C++20; safe to include directly from a UE
// module .cpp (doctrine: never from a header UHT parses -- see
// VoxelWorldSubsystem.h / VoxelChunkComponent.h, both voxel-core-free).
#include "voxelcore/mesher.h"
#include "voxelcore/tiles.h"
#include "voxelcore/world.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

#include <vector>

// VoxelCoords.h intentionally duplicates this constant (in UU) so it stays
// voxel-core-free; check the two never drift apart.
static_assert(vxc::kVoxelSizeMm == int32(VoxelCoords::VoxelSizeUU) * 10,
              "VoxelCoords::VoxelSizeUU (UE units) must track vxc::kVoxelSizeMm (mm)");

// FVoxelWorldImpl -- the voxel-core side of the subsystem, defined only here
// so VoxelWorldSubsystem.h (UHT-parsed) never sees a voxel-core header.
struct FVoxelWorldImpl
{
	explicit FVoxelWorldImpl(uint64 Seed)
		: Tiles(Seed)
		, Voxels(Seed, Tiles)
	{
	}

	vxc::SyntheticTileSampler Tiles;
	vxc::World<VoxelCoords::BrickEdgeVoxels> Voxels;
};

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
	// -- fixed for stage 1, becomes config-driven with stage 2 streaming.
	Impl = MakeUnique<FVoxelWorldImpl>(DefaultSeed);
}

void UVoxelWorldSubsystem::Deinitialize()
{
	ChunkOwner = nullptr;
	Impl.Reset();

	Super::Deinitialize();
}

bool UVoxelWorldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Stage 1 scope (deliverable 2): "On world begin play (game worlds
	// only)" -- skip Editor/Inactive/EditorPreview worlds so simply opening
	// the level editor never triggers generation.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GamePreview;
}

void UVoxelWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (!InWorld.IsGameWorld())
	{
		return;
	}

	GenerateAndSpawnChunks(InWorld);
}

void UVoxelWorldSubsystem::GenerateAndSpawnChunks(UWorld& InWorld)
{
	using namespace vxc;

	if (!Impl)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	constexpr int32 B = VoxelCoords::BrickEdgeVoxels;
	constexpr int32 BricksPerChunk = VoxelCoords::ChunkEdgeBricks;

	const int64 RadiusMm = (int64)(GenerationRadiusMeters * 1000.0);
	const int64 RadiusVox = RadiusMm / kVoxelSizeMm;
	const int32 BrickMin = (int32)floorDiv(-RadiusVox, B);
	const int32 BrickMax = (int32)floorDiv(RadiusVox - 1, B);

	// Pass 1: surface shell at brick granularity -- identical approach to
	// voxel-core/bench/bench_main.cpp (GeneratedWorld::columns +
	// surfaceBrickRange per brick footprint) -- to find which render chunks
	// are touched. Only the brick containing each column's topmost solid
	// voxel needs meshing: everything below is uniformly solid (no exposed
	// faces) and everything above is air.
	TSet<VoxelCoords::FVoxelChunkKey> TouchedChunks;
	for (int32 By = BrickMin; By <= BrickMax; ++By)
	{
		for (int32 Bx = BrickMin; Bx <= BrickMax; ++Bx)
		{
			const auto Grid = Impl->Voxels.generated().columns(Bx, By);
			int32 BzMin, BzMax;
			Impl->Voxels.generated().surfaceBrickRange(Grid, BzMin, BzMax);

			for (int32 Bz = BzMin; Bz <= BzMax; ++Bz)
			{
				VoxelCoords::FVoxelChunkKey Key;
				Key.X = (int32)floorDiv(Bx, BricksPerChunk);
				Key.Y = (int32)floorDiv(By, BricksPerChunk);
				Key.Z = (int32)floorDiv(Bz, BricksPerChunk);
				TouchedChunks.Add(Key);
			}
		}
	}

	// Resolve the terrain material once (deliverable 4: load by path,
	// fallback to the engine default material, never crash).
	UMaterialInterface* ResolvedMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_VoxelTerrain.M_VoxelTerrain")));
	if (ResolvedMaterial == nullptr)
	{
		ResolvedMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_VoxelTerrain not found at /Game/Voxel/M_VoxelTerrain -- using engine default material."));
	}

	// Single actor hosting every render-chunk component (deliverable 2).
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ChunkOwner = InWorld.SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (ChunkOwner == nullptr)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("Failed to spawn the voxel chunk owner actor; aborting generation."));
		return;
	}

	USceneComponent* Root = NewObject<USceneComponent>(ChunkOwner, TEXT("VoxelChunkRoot"));
	ChunkOwner->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	ChunkOwner->SetActorLabel(TEXT("VoxelChunkOwner"));
#endif

	// Pass 2: mesh every touched chunk's 4x4x4 bricks and spawn a component
	// for each chunk that came out non-empty.
	int32 TotalChunksSpawned = 0;
	int64 TotalBricksMeshed = 0;
	int64 TotalQuads = 0;

	std::vector<Quad> BrickQuads;
	for (const VoxelCoords::FVoxelChunkKey& ChunkKey : TouchedChunks)
	{
		TArray<FVoxelChunkQuad> ChunkQuads;

		for (int32 Dz = 0; Dz < BricksPerChunk; ++Dz)
		{
			for (int32 Dy = 0; Dy < BricksPerChunk; ++Dy)
			{
				for (int32 Dx = 0; Dx < BricksPerChunk; ++Dx)
				{
					const int32 Bx = ChunkKey.X * BricksPerChunk + Dx;
					const int32 By = ChunkKey.Y * BricksPerChunk + Dy;
					const int32 Bz = ChunkKey.Z * BricksPerChunk + Dz;
					const int64 OriginVX = int64(Bx) * B;
					const int64 OriginVY = int64(By) * B;
					const int64 OriginVZ = int64(Bz) * B;

					// Sampler valid on [-1,B]^3 (mesher.h contract): reads
					// World::materialAt, which is overlay-aware and samples
					// straight across brick borders via the same
					// deterministic function -- doctrine SS2.4, and why no
					// neighbor bricks need to be materialized just to mesh
					// this one.
					const auto Sampler = [&](int X, int Y, int Z) -> MaterialId
					{ return Impl->Voxels.materialAt(OriginVX + X, OriginVY + Y, OriginVZ + Z); };

					BrickQuads.clear();
					meshBrick<B>(Sampler, BrickQuads);
					++TotalBricksMeshed;
					if (BrickQuads.empty())
					{
						continue;
					}

					// Bake this brick's position within the chunk into the
					// (already chunk-scale, uint8-safe: max 31) quad fields
					// so the scene proxy never needs to know about bricks.
					const int32 AxisOffset[3] = {Dx * B, Dy * B, Dz * B};
					ChunkQuads.Reserve(ChunkQuads.Num() + (int32)BrickQuads.size());
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
						ChunkQuads.Add(CQ);
					}
					TotalQuads += (int64)BrickQuads.size();
				}
			}
		}

		if (ChunkQuads.Num() == 0)
		{
			continue;
		}

		UVoxelChunkComponent* ChunkComp = NewObject<UVoxelChunkComponent>(ChunkOwner);
		ChunkComp->SetupAttachment(Root);
		ChunkComp->SetRelativeLocation(VoxelCoords::ChunkOriginWorld(ChunkKey));
		ChunkComp->SetMaterial(0, ResolvedMaterial);
		ChunkComp->SetChunkQuads(MoveTemp(ChunkQuads), VoxelCoords::ChunkEdgeVoxels);
		ChunkComp->RegisterComponent();

		++TotalChunksSpawned;
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Voxel gen (seed %llu, radius %.0fm): chunks=%d bricks=%lld quads=%lld time=%.1fms"),
	       (unsigned long long)DefaultSeed, GenerationRadiusMeters, TotalChunksSpawned, (long long)TotalBricksMeshed,
	       (long long)TotalQuads, ElapsedMs);
}
