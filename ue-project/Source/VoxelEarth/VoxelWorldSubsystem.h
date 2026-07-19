#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelWorldSubsystem.generated.h"

// voxel-core owns the deterministic world + edit overlay (doctrine SS2.1 /
// SS2.4: vxc::World<8> + its sampler). Kept behind a PImpl so this header
// never includes a voxel-core header -- UHT-parsed headers stay
// voxel-core-free by doctrine; see VoxelWorldSubsystem.cpp for the bridge.
struct FVoxelWorldImpl;

// Owns the voxel world (docs/m1-plan.md decisions table: "All voxel-core
// access via this subsystem, game thread only in stage 1") and, in stage 1,
// synchronously generates + meshes + spawns the fixed-radius surface shell
// around the origin when a game world begins play.
UCLASS()
class VOXELEARTH_API UVoxelWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelWorldSubsystem();
	// Declared (not defaulted) here and defined in the .cpp: TUniquePtr<FVoxelWorldImpl>'s
	// destructor needs FVoxelWorldImpl's full definition, which this
	// UHT-parsed header must not see (voxel-core stays out of it).
	virtual ~UVoxelWorldSubsystem() override;
	// UHT auto-generates this hot-reload constructor unless one is already
	// declared; the auto-generated version lives in VoxelWorldSubsystem.gen.cpp,
	// which cannot see FVoxelWorldImpl's definition either (same PImpl reason
	// as the destructor above).
	UVoxelWorldSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem Interface

	// Dev-fixed seed (docs/m1-plan.md decisions table: "seed from config
	// (default 20260719)" -- config-driven arrives with streaming in stage 2).
	static constexpr uint64 DefaultSeed = 20260719;

	// Stage 1 fixed synchronous-generation radius in meters (decisions
	// table default; async budgeted streaming is stage 2).
	static constexpr double GenerationRadiusMeters = 24.0;

private:
	// Synchronous stage-1 generation: amplify + voxelize + greedy-mesh every
	// surface-shell render chunk within GenerationRadiusMeters of the
	// origin, then spawn one UVoxelChunkComponent per non-empty chunk on
	// ChunkOwner. Logs chunk/brick/quad totals + elapsed ms to LogVoxelEarth.
	void GenerateAndSpawnChunks(UWorld& InWorld);

	TUniquePtr<FVoxelWorldImpl> Impl;

	// Single actor hosting all render-chunk components (deliverable 2: "spawn
	// /attach one UVoxelChunkComponent per non-empty chunk on a single actor").
	UPROPERTY(Transient)
	TObjectPtr<AActor> ChunkOwner;
};
