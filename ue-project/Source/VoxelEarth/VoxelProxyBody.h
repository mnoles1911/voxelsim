#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "VoxelMovementTuning.h"
#include "VoxelProxyBody.generated.h"

class UStaticMesh;
class UStaticMeshComponent;

// docs/m1-plan.md "Player experience decisions" table, "Character proxy"
// row: a placeholder blocky voxel-style body (~1.8m: box torso/head/limbs)
// with basic code-driven walk/idle animation -- no skeletal mesh, no anim
// blueprint (that's M5's per-bone voxel bodies). Built entirely from the
// engine unit cube (/Engine/BasicShapes/Cube) scaled into box shapes, so it
// needs zero project content.
//
// Owned by AVoxelEarthFlyPawn: CreateDefaultSubobject'd and attached to the
// pawn's root, with this component's own origin at the walk-mode collision
// box's center (VoxelMovementTuning::StandHalfExtentZ) so the feet land on the
// box's bottom face. AVoxelEarthFlyPawn also owns visibility (hidden in first
// person, visible in third person -- SetVisibility on camera-mode toggle) and
// drives UpdateAnimation every tick; this component has no tick of its own so
// all animation timing stays centralized in the pawn.
//
// Crouch is a uniform vertical SQUASH rather than a posed bend: the scale
// factor is exactly the collision box's half-extent ratio
// (VoxelMovementTuning::ProxyCrouchScaleZ), so the body keeps filling the
// collision volume precisely in both stances instead of merely suggesting a
// crouch. A real knee-bend pose belongs with M5's per-bone voxel bodies.
UCLASS(ClassGroup = (VoxelEarth), meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelProxyBodyComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UVoxelProxyBodyComponent();

	// Poses the code-driven walk-cycle (legs/arms swing) and idle-breathing
	// bob. Called from AVoxelEarthFlyPawn::Tick every frame (fly and walk mode
	// alike).
	//
	// HorizontalSpeedUU is the pawn's current horizontal speed magnitude (UU/s,
	// sign ignored) and sets the swing AMPLITUDE. GaitPhase (radians) sets the
	// swing position and is owned by UVoxelCharacterMovementComponent -- the
	// same phase drives the first-person camera bob, so the visible footfalls
	// and the camera dip cannot drift apart. CrouchAlpha is 0 standing, 1
	// crouched.
	void UpdateAnimation(float DeltaTime, double HorizontalSpeedUU, float GaitPhase, float CrouchAlpha);

private:
	// Creates one box-shaped mesh component from the engine unit cube
	// (100x100x100 UU), scaled per-axis to SizeUU, positioned at
	// RelativeLocationUU under AttachTo, and tinted via a per-part MID
	// ("Color" vector parameter -- a no-op if the resolved base material
	// doesn't expose one, same placeholder spirit as the rest of this
	// component).
	UStaticMeshComponent* MakeBoxPart(FName Name, USceneComponent* AttachTo, UStaticMesh* CubeMesh, const FVector& SizeUU,
	                                   const FVector& RelativeLocationUU, const FLinearColor& Tint);

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> TorsoMesh;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> HeadMesh;

	// Limb meshes hang below their pivot by half their own length, so
	// rotating the pivot (shoulder/hip hinge) swings the whole limb about
	// that hinge instead of about the mesh's own center.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<USceneComponent> LeftArmPivot;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<USceneComponent> RightArmPivot;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<USceneComponent> LeftLegPivot;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<USceneComponent> RightLegPivot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> LeftArmMesh;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> RightArmMesh;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> LeftLegMesh;
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Proxy Body")
	TObjectPtr<UStaticMeshComponent> RightLegMesh;

	// Idle-breathing phase only. The WALK phase is no longer tracked here: it
	// now arrives as a parameter from the movement component, which owns the
	// one authoritative gait phase (see UpdateAnimation).
	float BobPhase = 0.f;

	// Smoothed crouch blend, so the squash eases in rather than snapping when
	// the collision box resizes instantly.
	float CrouchBlend = 0.f;

	// Every tuning value lives in VoxelMovementTuning.h. This class used to
	// carry its own copy of the reference walk speed with a comment admitting
	// it mirrored the pawn's -- exactly the drift the shared header removes.
	static constexpr float CrouchBlendRatePerSec = 12.f;
};
