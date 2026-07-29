#pragma once

// Unattended movement fixture: -VoxelWalkTest=<seconds>.
//
// WHY THIS EXISTS. The walk mover (UVoxelCharacterMovementComponent) shipped
// with every one of its behaviours verified by reading rather than running --
// PRs #156/#166/#167 all state plainly that "the UE side is not built here".
// The first in-engine runs could only check what a screenshot shows: that it
// boots, engages, and renders. Everything that makes the rig a rig -- gravity,
// landing, jump feel, the step-up, crouch clearance, the ledge stop -- needs
// a keypress, and an unattended run has no keyboard.
//
// So this drives the mover directly on a scripted timeline and asserts on the
// RESULT, in the same spirit as -VoxelPerfRun's scripted flight and the water
// track's -VoxelWaterParityTest: the fixture IS the instrument, and it says
// PASS/FAIL rather than leaving a human to squint at a capture.
//
// WHAT IT CANNOT DO. Feel. Whether the tiers read as distinct paces, whether
// the jump is forgiving, whether the head bob sells the gait -- none of that
// is a number, and none of it is tested here. This covers the mechanical
// claims only, so that a human play-test can spend its attention on the part
// that actually needs a human.
//
// Phases run in order, each with a fixed duration, and each logs one PASS/FAIL
// line under LogVoxelWalkTest. The run self-quits when the script ends.

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelWalkTestSubsystem.generated.h"

class AVoxelEarthFlyPawn;
class UVoxelCharacterMovementComponent;

VOXELEARTH_API DECLARE_LOG_CATEGORY_EXTERN(LogVoxelWalkTest, Log, All);

UCLASS()
class VOXELEARTH_API UVoxelWalkTestSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;

private:
	// The script. Each phase gets Setup() once on entry, Sample() every tick,
	// and Judge() once on exit -- the shape every one of these checks wants
	// (drive an input, watch what the mover does, then assert on the extremum).
	enum class EPhase : uint8
	{
		Settle,        // wait for terrain + a stable ground contact before asserting anything
		Fall,          // gravity runs and the character actually lands
		WalkForward,   // reaches the dialled speed and covers ground
		SprintGate,    // Shift forward engages; Shift strafing does not
		JumpApex,      // apex height, and gravity NEVER latches off mid-air
		JumpTapVsHold, // a tapped jump apexes lower than a held one
		CrouchGeometry,// box shrinks from the top: feet stay planted
		Done,
	};

	void EnterPhase(EPhase Next);
	void Report(const TCHAR* Check, bool bPass, const FString& Detail);

	AVoxelEarthFlyPawn* GetPawn() const;
	UVoxelCharacterMovementComponent* GetMover() const;

	bool bArmed = false;
	float StartDelaySeconds = 0.f;
	double ElapsedSeconds = 0.0;

	EPhase Phase = EPhase::Settle;
	double PhaseElapsedSeconds = 0.0;

	int32 PassCount = 0;
	int32 FailCount = 0;

	// Per-phase scratch. Reset by EnterPhase.
	// Z at the instant walk mode is switched on, before anything has moved.
	// gravity-lands measures from HERE rather than from its own phase start:
	// the Settle phase already absorbs the fall, so a phase-local measurement
	// reports 0.00 m and passes without ever witnessing gravity act.
	double RunStartZ = 0.0;

	double PhaseStartZ = 0.0;
	double PhaseStartXY = 0.0;
	FVector PhaseStartPos = FVector::ZeroVector;
	double PeakZ = 0.0;
	double PeakSpeed = 0.0;
	// SprintGate: did forward+Shift ever actually engage during the first half?
	// Sampled rather than read once at the boundary, because the gate is a
	// per-tick verdict and the exact frame the phase flips is arbitrary.
	bool bForwardSprintSeen = false;
	bool bSawGrounded = false;
	bool bSawAirborne = false;
	bool bSawWaitingWhileAirborne = false;
	double CrouchEntryFeetZ = 0.0;
};
