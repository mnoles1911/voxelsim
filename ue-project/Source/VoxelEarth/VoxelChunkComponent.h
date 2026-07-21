#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "VoxelMeshTypes.h"
#include "VoxelChunkComponent.generated.h"

class UMaterialInstanceDynamic;

// Render chunk primitive (docs/m1-plan.md decisions table: one
// UVoxelChunkComponent + one FVoxelChunkSceneProxy per render chunk,
// re-mesh unit on edit). Doctrine
// (docs/voxel-earth-implementation-plan.md SS3.3 Band 1): a hand-rolled
// FPrimitiveSceneProxy, NOT ProceduralMeshComponent / UDynamicMesh.
//
// This component only carries CPU-side quad data (produced by
// UVoxelWorldSubsystem from vxc::meshBrick, chunk-local); the scene proxy
// (FVoxelChunkSceneProxy, defined in VoxelChunkComponent.cpp) converts it to
// GPU buffers on scene proxy creation, following the engine's
// CustomMeshComponent plugin
// (Engine/Plugins/Runtime/CustomMeshComponent/.../CustomMeshComponent.cpp)
// as the API-correct UE 5.7 template for FLocalVertexFactory /
// FStaticMeshVertexBuffers / FDynamicMeshIndexBuffer32.
UCLASS(ClassGroup = Voxel, meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelChunkComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UVoxelChunkComponent(const FObjectInitializer& ObjectInitializer);

	// Replace this chunk's mesh (chunk-local voxel-space quads) and mark
	// render state dirty so a fresh scene proxy is built next frame.
	// InChunkEdgeVoxels is the chunk edge length in voxels
	// (VoxelCoords::ChunkEdgeVoxels in stage 1), used for local bounds.
	void SetChunkQuads(TArray<FVoxelChunkQuad>&& InQuads, int32 InChunkEdgeVoxels);

	// M2 mip rings (docs/m2-plan.md decisions table): "one component type
	// serves all levels ... position scale = VoxelSizeUU << level". Set by
	// UVoxelWorldSubsystem right after acquiring a component (fresh or pooled
	// -- see FVoxelWorldImpl::AcquireChunkComponent) and always before the
	// SetChunkQuads call that actually bakes this into a scene proxy. M1
	// hitch-gap wave (component pooling): a single component's level CAN now
	// change across its lifetime -- once when first assigned, and again every
	// time a pooled/reused instance is handed to a DIFFERENT (level, key) --
	// this is safe to call repeatedly for exactly that reason; only the
	// value as of the NEXT SetChunkQuads call matters, nothing latches on
	// first-ever assignment. Quad coordinates in ChunkQuads stay in
	// level-relative voxel units (0..31); only world placement (this scale)
	// differs by level.
	//
	// M2 "Transitions" polish (docs/voxel-earth-implementation-plan.md SS3.3;
	// docs/m2-plan.md's "hard boundary v0" row, now upgraded): also drives the
	// always-on dithered ring cross-fade (ApplyRingFadeParams, VoxelChunkComponent.cpp)
	// -- a per-level static table derived from UVoxelWorldSubsystem::RingPresets
	// (read-only reference; this component never modifies the subsystem).
	// SetLevel runs BEFORE SetMaterial (see the NewObject<UVoxelChunkComponent>
	// call site in VoxelWorldSubsystem.cpp), so this alone can't create the MID
	// yet if no base material is assigned -- ApplyRingFadeParams no-ops until
	// both are known, whichever of SetLevel/SetMaterial lands second finishes
	// the job.
	void SetLevel(int32 InLevel);
	int32 GetLevel() const { return ChunkLevel; }

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* NewMaterial) override;
	virtual int32 GetNumMaterials() const override;
	// Required: the render-thread material verifier rejects any FMeshBatch
	// whose material is missing from this list (engine defaults are exempt,
	// which masked the omission until an authored material was used —
	// symptom: terrain renders with the default material, invisible with
	// M_VoxelTerrain, "not present in GetUsedMaterials" in the log).
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const override;
	//~ End UPrimitiveComponent Interface

	// --- Chunk-state debug tints (docs/debug-tooling-plan.md P1, mode 2 +
	// voxel.Debug.ChunkStates) ------------------------------------------------

	// Sets the DebugTint vector parameter (M_VoxelTerrain, multiplied into
	// BaseColor; see Tools/create_voxel_material.py) on ChunkMID. M2 update:
	// ChunkMID is no longer lazily created here -- it now always exists once a
	// base material is assigned, because ApplyRingFadeParams (SetLevel/
	// SetMaterial) needs one unconditionally for the always-on ring cross-fade
	// params, and the task constraint is "one MID per component maximum" --
	// so this just sets a parameter on that already-existing instance (a no-op
	// if no material has been assigned yet at all, which shouldn't happen in
	// practice since SetMaterial always runs before any debug call site).
	// Only ever called by FVoxelWorldImpl::UpdateChunkStateTints /
	// UpdateRingTints, which only run while voxel.Debug's mode>=2 layers are
	// live -- so this is the only per-component work that layer ever costs
	// (the MID itself is already paid for by the fade feature, not by debug).
	void SetDebugTint(const FLinearColor& Tint);

	// Resets DebugTint to the multiplicative identity (opaque white) rather
	// than dropping ChunkMID -- the MID must persist regardless (it carries
	// the always-on ring-fade params), so "zero-cost when off" now means "no
	// extra per-frame/per-toggle work", not "no MID": there was never a
	// second MID to drop once fades became always-on.
	void ClearDebugTint();

private:
	TArray<FVoxelChunkQuad> ChunkQuads;
	int32 ChunkEdgeVoxels = 32;
	int32 ChunkLevel = 0;

	// The material actually used for rendering (CreateSceneProxy reads this
	// directly) -- either the plain base material passed to SetMaterial(),
	// or ChunkMID once ApplyRingFadeParams has wrapped it (which happens as
	// soon as both a base material and a level are known; see SetLevel).
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ChunkMaterial;

	// Single MID per component (docs/m2-plan.md "Transitions" row: "one MID
	// per component maximum -- set both param groups on the same instance").
	// Created by ApplyRingFadeParams (VoxelChunkComponent.cpp) the first time
	// a base material is available; carries the four always-on
	// RingInnerFadeStart/End + RingOuterFadeStart/End scalar params, and
	// reused by SetDebugTint/ClearDebugTint above for the P1 DebugTint vector
	// param when that separate debug layer is engaged. Never destroyed for
	// the lifetime of the component once created (fades never turn off), only
	// recreated if SetMaterial() is called again with a different base.
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ChunkMID;

	// M1 hitch-gap wave: true iff ChunkMID's DebugTint param currently
	// differs from the identity (opaque white) -- set by SetDebugTint,
	// cleared by ClearDebugTint. Lets ClearDebugTint (now called
	// unconditionally on every pool-park by ReturnChunkComponentToPool, see
	// VoxelWorldSubsystem.cpp) skip its SetVectorParameterValue call --  a
	// real, non-coalescing render-thread command, unlike MarkRenderStateDirty
	// -- whenever there is genuinely nothing to clear (the common case
	// whenever voxel.Debug's tint layers are off).
	bool bDebugTintDirty = false;

	// Applies this chunk's per-level ring cross-fade params (docs/m2-plan.md
	// "Transitions" row upgrade; docs/voxel-earth-implementation-plan.md
	// SS3.3) to ChunkMID, lazily creating it over the current base material
	// (ChunkMID's existing Parent, or ChunkMaterial if no MID exists yet) the
	// first time both ChunkLevel and a base material are known. No-ops if no
	// base material has been assigned yet (SetLevel runs before SetMaterial
	// in the current construction order -- see SetLevel's doc comment).
	// Always-on: unlike SetDebugTint/ClearDebugTint, there is no cvar gate.
	void ApplyRingFadeParams();

	friend class FVoxelChunkSceneProxy;
};
