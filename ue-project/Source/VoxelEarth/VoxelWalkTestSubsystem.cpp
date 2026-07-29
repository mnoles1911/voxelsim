#include "VoxelWalkTestSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "VoxelCharacterMovement.h"
#include "VoxelEarthFlyPawn.h"
#include "VoxelMovementTuning.h"

DEFINE_LOG_CATEGORY(LogVoxelWalkTest);

using namespace VoxelMovementTuning;

namespace
{
// Phase durations (seconds). Generous rather than tight: this runs against
// live streaming, and a phase that ends early would assert on a half-finished
// motion and fail for the wrong reason.
constexpr double kSettleSeconds = 6.0;
constexpr double kFallSeconds = 4.0;
constexpr double kWalkSeconds = 3.0;
constexpr double kSprintGateSeconds = 4.0;
constexpr double kJumpSeconds = 3.0;
constexpr double kTapVsHoldSeconds = 3.0;
constexpr double kCrouchSeconds = 3.0;

const TCHAR* PhaseName(int32 P)
{
	static const TCHAR* kNames[] = {TEXT("Settle"),        TEXT("Fall"),           TEXT("WalkForward"),
	                                TEXT("SprintGate"),    TEXT("JumpApex"),       TEXT("JumpTapVsHold"),
	                                TEXT("CrouchGeometry"), TEXT("Done")};
	return kNames[FMath::Clamp(P, 0, int32(UE_ARRAY_COUNT(kNames)) - 1)];
}
} // namespace

void UVoxelWalkTestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Command line read at init, deliberately NOT -ExecCmds: cvars set that way
	// land after subsystems have already run (VoxelPerfRunSubsystem carries the
	// same note, for the same reason).
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWalkTest="), StartDelaySeconds) && StartDelaySeconds > 0.f)
	{
		bArmed = true;
		UE_LOG(LogVoxelWalkTest, Log,
		       TEXT("VoxelWalkTest: armed, scripted movement checks begin at t=%.1fs. Feel is NOT tested here -- ")
		       TEXT("this covers the mechanical claims only."),
		       StartDelaySeconds);
	}
}

bool UVoxelWalkTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UVoxelWalkTestSubsystem::IsTickable() const
{
	return bArmed && Phase != EPhase::Done;
}

TStatId UVoxelWalkTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelWalkTestSubsystem, STATGROUP_Tickables);
}

AVoxelEarthFlyPawn* UVoxelWalkTestSubsystem::GetPawn() const
{
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	return PC ? Cast<AVoxelEarthFlyPawn>(PC->GetPawn()) : nullptr;
}

UVoxelCharacterMovementComponent* UVoxelWalkTestSubsystem::GetMover() const
{
	AVoxelEarthFlyPawn* Pawn = GetPawn();
	return Pawn ? Pawn->GetWalkMovement() : nullptr;
}

void UVoxelWalkTestSubsystem::Report(const TCHAR* Check, bool bPass, const FString& Detail)
{
	bPass ? ++PassCount : ++FailCount;
	UE_LOG(LogVoxelWalkTest, Log, TEXT("VoxelWalkTest [%s] %s -- %s"), bPass ? TEXT("PASS") : TEXT("FAIL"), Check,
	       *Detail);
}

void UVoxelWalkTestSubsystem::EnterPhase(EPhase Next)
{
	Phase = Next;
	PhaseElapsedSeconds = 0.0;

	AVoxelEarthFlyPawn* Pawn = GetPawn();
	UVoxelCharacterMovementComponent* Mover = GetMover();
	if (!Pawn || !Mover)
	{
		return;
	}

	PhaseStartPos = Pawn->GetActorLocation();
	PhaseStartZ = PhaseStartPos.Z;
	PhaseStartXY = 0.0;
	PeakZ = PhaseStartZ;
	PeakSpeed = 0.0;
	bSawGrounded = false;
	bSawAirborne = false;
	bSawWaitingWhileAirborne = false;
	bForwardSprintSeen = false;

	// Neutral input at every boundary, so a phase never inherits the previous
	// one's held keys.
	Pawn->SetScriptedInput(0.f, 0.f);
	Mover->SetSprintHeld(false);
	Mover->SetCrouchHeld(false);

	switch (Phase)
	{
	case EPhase::WalkForward:
	case EPhase::SprintGate:
		Pawn->SetScriptedInput(1.f, 0.f);
		break;
	case EPhase::JumpApex:
		Mover->RequestJump(); // held: ReleaseJump is never called, so no variable-height cut
		break;
	case EPhase::JumpTapVsHold:
		Mover->RequestJump();
		Mover->ReleaseJump(); // tapped: cut on the same frame, the shortest possible hop
		break;
	case EPhase::CrouchGeometry:
		CrouchEntryFeetZ = PhaseStartPos.Z - Mover->GetHalfExtentZ();
		Mover->SetCrouchHeld(true);
		break;
	default:
		break;
	}

	if (Phase != EPhase::Done)
	{
		UE_LOG(LogVoxelWalkTest, Verbose, TEXT("VoxelWalkTest: phase %s"), PhaseName(int32(Phase)));
	}
}

void UVoxelWalkTestSubsystem::Tick(float DeltaTime)
{
	if (!bArmed || Phase == EPhase::Done)
	{
		return;
	}

	ElapsedSeconds += DeltaTime;
	if (ElapsedSeconds < StartDelaySeconds)
	{
		return;
	}

	AVoxelEarthFlyPawn* Pawn = GetPawn();
	UVoxelCharacterMovementComponent* Mover = GetMover();
	if (!Pawn || !Mover)
	{
		return;
	}

	// First tick past the start delay: force walk mode on and take the Settle
	// phase's baseline. SetWalkMode is idempotent, so re-entering is harmless.
	if (PhaseElapsedSeconds == 0.0 && Phase == EPhase::Settle)
	{
		RunStartZ = Pawn->GetActorLocation().Z;
		Pawn->SetWalkMode(true);
		EnterPhase(EPhase::Settle);
		UE_LOG(LogVoxelWalkTest, Log, TEXT("VoxelWalkTest: walk mode on at z=%.1fm; gravity measured from here."),
		       RunStartZ / 100.0);
	}

	PhaseElapsedSeconds += DeltaTime;

	const FVector Pos = Pawn->GetActorLocation();
	const bool bGrounded = Mover->IsGrounded();
	const bool bWaiting = Mover->IsWaitingForTerrain();

	PeakZ = FMath::Max(PeakZ, Pos.Z);
	PeakSpeed = FMath::Max(PeakSpeed, Mover->GetHorizontalSpeedUU());
	bSawGrounded |= bGrounded;
	bSawAirborne |= !bGrounded;
	// THE REGRESSION GUARD. Airborne + "waiting for terrain" is the signature of
	// the gravity veto latching on an all-air settled chunk (the mid-air hang).
	// Watched in every phase, not just the jump ones.
	bSawWaitingWhileAirborne |= (!bGrounded && bWaiting);

	switch (Phase)
	{
	case EPhase::Settle:
		if (PhaseElapsedSeconds >= kSettleSeconds)
		{
			Report(TEXT("terrain-ready"), !bWaiting,
			       FString::Printf(TEXT("after %.1fs settle: waitingForTerrain=%s grounded=%s z=%.1fm"),
			                       kSettleSeconds, bWaiting ? TEXT("y") : TEXT("n"), bGrounded ? TEXT("y") : TEXT("n"),
			                       Pos.Z / 100.0));
			EnterPhase(EPhase::Fall);
		}
		break;

	case EPhase::Fall:
		if (PhaseElapsedSeconds >= kFallSeconds)
		{
			// The pawn spawns +5m up (AVoxelEarthGameMode::RestartPlayer) and fly
			// mode does not fall, so walk mode should have dropped it ~4m onto
			// the surface. Measured from RunStartZ, not the phase start, so this
			// witnesses the fall rather than inferring it from "it's on the
			// ground now" -- which was true even when gravity never ran.
			const double FellUU = RunStartZ - Pos.Z;
			const bool bFellFar = FellUU >= 200.0;
			Report(TEXT("gravity-lands"), bGrounded && !bWaiting && bFellFar,
			       FString::Printf(TEXT("grounded=%s waiting=%s fell %.2fm since walk mode (want >= 2.00m)"),
			                       bGrounded ? TEXT("y") : TEXT("n"), bWaiting ? TEXT("y") : TEXT("n"),
			                       FellUU / 100.0));
			EnterPhase(EPhase::WalkForward);
		}
		break;

	case EPhase::WalkForward:
		if (PhaseElapsedSeconds >= kWalkSeconds)
		{
			const double Dial = Mover->GetDialSpeedUU();
			const double TravelledUU = FVector::Dist2D(Pos, PhaseStartPos);
			// Reaching ~the dialled pace, and actually covering ground. Loose
			// bounds: terrain is uneven and the accel ramp eats the first frames.
			const bool bSpeedOk = PeakSpeed >= Dial * 0.85 && PeakSpeed <= Dial * 1.05;
			const bool bMovedOk = TravelledUU >= Dial * kWalkSeconds * 0.5;
			Report(TEXT("walk-reaches-dial"), bSpeedOk && bMovedOk,
			       FString::Printf(TEXT("dial=%.0f peak=%.0f UU/s (%s), travelled %.1fm in %.1fs (%s)"), Dial,
			                       PeakSpeed, bSpeedOk ? TEXT("ok") : TEXT("BAD"), TravelledUU / 100.0, kWalkSeconds,
			                       bMovedOk ? TEXT("ok") : TEXT("BAD")));
			EnterPhase(EPhase::SprintGate);
		}
		break;

	case EPhase::SprintGate:
	{
		// Two halves: forward+Shift must engage, strafe+Shift must not. The
		// forward-only gate (dot >= 0.7) is the claim under test.
		const bool bSecondHalf = PhaseElapsedSeconds >= kSprintGateSeconds * 0.5;
		Mover->SetSprintHeld(true);
		Pawn->SetScriptedInput(bSecondHalf ? 0.f : 1.f, bSecondHalf ? 1.f : 0.f);
		if (!bSecondHalf)
		{
			bForwardSprintSeen |= Mover->IsSprintEngaged();
		}

		if (PhaseElapsedSeconds >= kSprintGateSeconds)
		{
			const bool bForwardEngaged = bForwardSprintSeen;
			const bool bStrafeSuppressed = !Mover->IsSprintEngaged();
			Report(TEXT("sprint-forward-only"), bForwardEngaged && bStrafeSuppressed,
			       FString::Printf(TEXT("forward+Shift engaged=%s (want y), strafe+Shift engaged=%s (want n)"),
			                       bForwardEngaged ? TEXT("y") : TEXT("n"), bStrafeSuppressed ? TEXT("n") : TEXT("y")));
			EnterPhase(EPhase::JumpApex);
		}
		break;
	}

	case EPhase::JumpApex:
		if (PhaseElapsedSeconds >= kJumpSeconds)
		{
			const double ApexUU = PeakZ - PhaseStartZ;
			// ~1.0m apex by design (JumpSpeedUU sized for it). Wide band: the
			// landing may be on a different voxel than the launch.
			// Band follows JumpSpeedUU: 495.0 targets a 1.25 m apex, and the
			// landing may be on a different voxel than the launch, so this is
			// deliberately loose around it rather than tight.
			const bool bApexOk = ApexUU >= 95.0 && ApexUU <= 165.0;
			const bool bLanded = bGrounded;
			Report(TEXT("jump-apex"), bApexOk && bLanded && bSawAirborne,
			       FString::Printf(TEXT("apex %.2fm (want 0.70-1.40), airborne=%s, landed=%s"), ApexUU / 100.0,
			                       bSawAirborne ? TEXT("y") : TEXT("n"), bLanded ? TEXT("y") : TEXT("n")));
			// THE HYPOTHESIS THIS FIXTURE WAS BUILT FOR.
			Report(TEXT("no-midair-gravity-veto"), !bSawWaitingWhileAirborne,
			       bSawWaitingWhileAirborne
			           ? FString(TEXT("WAITING FOR TERRAIN latched while airborne -- the all-air-chunk veto fired mid-jump"))
			           : FString(TEXT("never entered waiting-for-terrain while off the ground")));
			EnterPhase(EPhase::JumpTapVsHold);
		}
		break;

	case EPhase::JumpTapVsHold:
		if (PhaseElapsedSeconds >= kTapVsHoldSeconds)
		{
			const double TapApexUU = PeakZ - PhaseStartZ;
			// Released on the launch frame, so the rise is scaled by
			// JumpReleaseCutScale (0.45): apex must be clearly under a full hop.
			//
			// BOTH bounds matter. An upper bound alone lets a jump that never
			// happened (apex 0.00) pass as a "short hop" -- which is exactly
			// what it did while the gravity veto was cancelling jumps outright,
			// reporting PASS for a completely broken jump. The lower bound is
			// what makes this check able to fail.
			const bool bShorter = TapApexUU < 70.0;
			const bool bActuallyJumped = TapApexUU >= 10.0;
			Report(TEXT("jump-variable-height"), bShorter && bActuallyJumped,
			       FString::Printf(TEXT("tapped apex %.2fm (want 0.10-0.70m: shorter than a held jump, but a jump)"),
			                       TapApexUU / 100.0));
			EnterPhase(EPhase::CrouchGeometry);
		}
		break;

	case EPhase::CrouchGeometry:
		if (PhaseElapsedSeconds >= kCrouchSeconds)
		{
			const double FeetZ = Pos.Z - Mover->GetHalfExtentZ();
			const bool bCrouched = Mover->IsCrouched();
			// The whole point of shrinking from the top: the bottom face does
			// not move, so the character never sinks into or pops off the floor.
			const bool bFeetPlanted = FMath::Abs(FeetZ - CrouchEntryFeetZ) < 5.0; // 5 UU tolerance
			const bool bHalfOk = FMath::IsNearlyEqual(Mover->GetHalfExtentZ(), CrouchHalfExtentZ, 0.01);
			Report(TEXT("crouch-feet-planted"), bCrouched && bFeetPlanted && bHalfOk,
			       FString::Printf(TEXT("crouched=%s halfZ=%.1f (want %.1f), feet moved %.2fm (want ~0)"),
			                       bCrouched ? TEXT("y") : TEXT("n"), Mover->GetHalfExtentZ(), CrouchHalfExtentZ,
			                       (FeetZ - CrouchEntryFeetZ) / 100.0));
			EnterPhase(EPhase::Done);
		}
		break;

	case EPhase::Done:
		break;
	}

	if (Phase == EPhase::Done)
	{
		Pawn->ClearScriptedInput();
		Mover->SetCrouchHeld(false);
		Mover->SetSprintHeld(false);
		UE_LOG(LogVoxelWalkTest, Log, TEXT("VoxelWalkTest COMPLETE: %d passed, %d FAILED."), PassCount, FailCount);

		if (UWorld* World = GetWorld())
		{
			FTimerHandle Quit;
			World->GetTimerManager().SetTimer(
				Quit, FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }), 2.f, false);
		}
	}
}
