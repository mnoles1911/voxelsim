#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "VoxelEarthFlyPawn.generated.h"

class UCameraComponent;
class UFloatingPawnMovement;
class UVoxelWorldSubsystem;

// docs/m1-plan.md Stage 2 decisions table: "Spectator fly pawn + legacy
// input bindings (Enhanced Input assets deferred)." A minimal from-scratch
// fly pawn rather than ADefaultPawn/ASpectatorPawn: those bind to NAMED axis
// mappings ("MoveForward" etc.) that must come from an Input Settings config
// this project doesn't have; binding raw FKeys directly (BindAxisKey) here
// needs zero ini/config and stays fully "legacy input, no assets."
//
// Stage 3b (docs/m1-plan.md stage 3b, plan SS3.3 "no Chaos for terrain")
// adds a walk mode toggled with G: fly mode (default) is unchanged
// UFloatingPawnMovement flight; walk mode is a from-scratch kinematic
// mover -- gravity, jump, and an axis-separated swept-AABB sweep against
// UVoxelWorldSubsystem::IsSolidAtVoxel -- driven from this pawn's own Tick
// (only enabled while walking). No CharacterMovementComponent, no Chaos:
// this is client-side presentation only (the doctrine determinism boundary
// covers world DERIVATION; authoritative movement arrives with the M3
// server), so plain floats are fine here.
UCLASS()
class VOXELEARTH_API AVoxelEarthFlyPawn : public APawn
{
	GENERATED_BODY()

public:
	AVoxelEarthFlyPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth")
	TObjectPtr<UFloatingPawnMovement> Movement;

private:
	void MoveForward(float Value);
	void MoveRight(float Value);
	void MoveUp(float Value);
	void Turn(float Value);
	void LookUp(float Value);
	void ToggleWalkMode();
	void OnJumpPressed();

	// Walk-mode kinematic step, called from Tick only while bWalkMode is set.
	void TickWalkMode(float DeltaTime);

	// Sweeps the walk-mode collision box along a single world axis (0=X,
	// 1=Y, 2=Z) by Delta UU, starting from InOutPos; on a blocking voxel,
	// clamps InOutPos[Axis] to the hit face (CollisionEpsilonUU clearance)
	// instead of the full Delta. Returns true if the sweep was blocked.
	bool SweepAxis(int32 Axis, double Delta, FVector& InOutPos) const;

	// True if a voxel is solid immediately (1 voxel) under the walk-mode
	// collision box's footprint at Pos.
	bool IsGroundedAt(const FVector& Pos) const;

	UVoxelWorldSubsystem* GetVoxelWorldSubsystem() const;

	// Degrees/sec applied per unit of raw mouse-delta axis value.
	static constexpr float MouseLookSpeed = 2.5f;

	// --- Walk mode (stage 3b) -----------------------------------------

	bool bWalkMode = false;

	// Cached WASD axis state (fly mode applies these via AddMovementInput
	// directly in the callback; walk mode instead re-projects them onto the
	// horizontal plane once per Tick -- see TickWalkMode).
	float CurrentForwardInput = 0.f;
	float CurrentRightInput = 0.f;

	float VerticalVelocity = 0.f;
	bool bJumpRequested = false;

	// 60x60x180 UU box, actor location at box center.
	static constexpr double WalkBoxHalfExtentXY = 30.0;
	static constexpr double WalkBoxHalfExtentZ = 90.0;

	static constexpr double WalkSpeedUU = 600.0;         // UU/s, horizontal
	static constexpr double GravityUUPerSec2 = 980.0;    // UU/s^2
	static constexpr double JumpSpeedUU = 450.0;         // UU/s, upward
	static constexpr double StepUpHeightUU = 30.0;       // 3 voxels
	static constexpr double CollisionEpsilonUU = 0.1;    // face clamp clearance
};
