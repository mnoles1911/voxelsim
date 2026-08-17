#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelDetailAssetSubsystem.generated.h"

// TASK #7: render what already places -- the detail-lattice (L3) ground cover.
//
// The placement audit (docs/asset-placement-audit.md section 5.3) measured that
// 85% of all resolved asset instances -- every grass tuft, fern, flower and
// small rock authored on the 5 cm detail lattice -- are resolved by voxel-core
// and then never drawn: every UE composition call passes terrainOnly=true, and
// detail grids never enter the world voxel lattice by design (they carry their
// own voxel pitch; vxc::AssetGrid::onTerrainLattice() answers false and
// materialOfInstance refuses them as a category error). This subsystem is the
// missing presentation half: it consumes the SAME deterministic resolver
// (vxc::AssetField::instancesForRect with terrainOnly=false), converts each
// (species, seed) detail grid ONCE into a small static mesh, and draws the
// instances as per-(species, seed) HISM batches in a ring around the streaming
// anchor.
//
// WHAT IS DELIBERATELY EXCLUDED: the detail ENTITIES -- fish, birds,
// quadrupeds, cetaceans (owner decision: they need animation first). The
// exclusion is structural, not a filter this file could forget: those species
// carry layer 255 (vxc::kAssetLayerNotScattered), are dropped by
// assetSpeciesTableFromManifest, and therefore can never come out of
// instancesForRect at all. A defensive hasParts() guard in the worker backs
// the structural argument up (only animals carry rig parts).
//
// NO NEW PLACEMENT LOGIC. Same seed + tiles => same instances, because this
// subsystem only ever CONSUMES the resolver -- it computes nothing about where
// anything stands, and it puts nothing in the world lattice, so worldgen
// digests, streaming bounds and multiplayer determinism are untouched by
// construction (rendering is presentation, not worldgen state).
//
// This header is UHT-parsed and stays voxel-core-free by the same doctrine as
// UVoxelWorldSubsystem: all vxc types live behind the PImpl in the .cpp.
//
// See docs/detail-asset-rendering.md for the architecture write-up and the
// capture checklist.

struct FVoxelDetailAssetImpl;

class AActor;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;

UCLASS()
class VOXELEARTH_API UVoxelDetailAssetSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UVoxelDetailAssetSubsystem();
	// Declared (not defaulted) here and defined in the .cpp:
	// TUniquePtr<FVoxelDetailAssetImpl>'s destructor needs the full definition,
	// which this UHT-parsed header must not see (same PImpl reason as
	// UVoxelWorldSubsystem).
	virtual ~UVoxelDetailAssetSubsystem() override;
	UVoxelDetailAssetSubsystem(FVTableHelper& Helper);

	//~ Begin USubsystem Interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	//~ End USubsystem Interface

	//~ Begin UWorldSubsystem Interface
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	//~ End UWorldSubsystem Interface

	//~ Begin FTickableGameObject / UTickableWorldSubsystem Interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	//~ End FTickableGameObject / UTickableWorldSubsystem Interface

private:
	TUniquePtr<FVoxelDetailAssetImpl> Impl;

	// Single actor hosting every detail HISM component (same one-owner shape as
	// UVoxelWorldSubsystem's ChunkOwner). Spawned lazily on the first tick that
	// finds an installed asset field.
	UPROPERTY(Transient)
	TObjectPtr<AActor> DetailOwner;

	// Root component every detail HISM attaches under; kept at the world
	// origin (each HISM sets its own nearby world location for instance-buffer
	// float precision -- see FMeshEntry::OriginUU in the .cpp).
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> DetailRoot;

	// M_VoxelDetailAsset (authored by Tools/create_detail_asset_material.py:
	// BaseColor = sRGB-decoded vertex colour), or the engine default material
	// when the asset is missing -- grey cover with correct SHAPES, plus a loud
	// warning naming the script, never a crash.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> DetailMaterial;

	// GC roots for the runtime-built meshes and their HISM components. The
	// components are also owned by DetailOwner and the meshes referenced by the
	// components, but these arrays make the ownership explicit rather than
	// transitively inferred.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> BuiltMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> HismComponents;
};
