#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
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
// box's center (see AVoxelEarthFlyPawn::WalkBoxHalfExtentZ/XY) so the feet
// land on the box's bottom face. AVoxelEarthFlyPawn also owns visibility
// (hidden in first person, visible in third person -- SetVisibility on
// camera-mode toggle) and drives UpdateAnimation every tick; this component
// has no tick of its own so all animation timing stays centralized in the
// pawn.
UCLASS(ClassGroup = (VoxelEarth), meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelProxyBodyComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UVoxelProxyBodyComponent();

	// Advances the code-driven walk-cycle (legs/arms swing, rate
	// proportional to distance traveled) and idle-breathing bob. Called from
	// AVoxelEarthFlyPawn::Tick every frame (fly and walk mode alike).
	// HorizontalSpeedUU is the pawn's current horizontal speed magnitude
	// (UU/s, sign ignored).
	void UpdateAnimation(float DeltaTime, double HorizontalSpeedUU);

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

	float GaitPhase = 0.f;
	float BobPhase = 0.f;

	// Speed (UU/s) at which the walk-cycle swing reaches full amplitude;
	// matches AVoxelEarthFlyPawn::WalkSpeedUU (kept as an independent literal
	// here -- see file-ownership split in docs/m1-plan.md -- rather than a
	// cross-class constant reference).
	static constexpr double RefWalkSpeedUU = 450.0;
	static constexpr float MaxSwingDegrees = 35.f;
	// One full swing cycle per this many UU traveled (~1.4m stride).
	static constexpr float StrideLengthUU = 140.f;
	static constexpr float BobFrequencyHz = 0.8f;
	static constexpr double BobAmplitudeUU = 1.5;
};
