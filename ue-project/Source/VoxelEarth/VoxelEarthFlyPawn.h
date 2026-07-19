#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "VoxelEarthFlyPawn.generated.h"

class UCameraComponent;
class UFloatingPawnMovement;

// docs/m1-plan.md Stage 2 decisions table: "Spectator fly pawn + legacy
// input bindings (Enhanced Input assets deferred)." A minimal from-scratch
// fly pawn rather than ADefaultPawn/ASpectatorPawn: those bind to NAMED axis
// mappings ("MoveForward" etc.) that must come from an Input Settings config
// this project doesn't have; binding raw FKeys directly (BindAxisKey) here
// needs zero ini/config and stays fully "legacy input, no assets."
UCLASS()
class VOXELEARTH_API AVoxelEarthFlyPawn : public APawn
{
	GENERATED_BODY()

public:
	AVoxelEarthFlyPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UFloatingPawnMovement> Movement;

private:
	void MoveForward(float Value);
	void MoveBackward(float Value);
	void MoveRight(float Value);
	void MoveLeft(float Value);
	void MoveUp(float Value);
	void MoveDown(float Value);
	void Turn(float Value);
	void LookUp(float Value);

	// Degrees/sec applied per unit of raw mouse-delta axis value.
	static constexpr float MouseLookSpeed = 2.5f;
};
