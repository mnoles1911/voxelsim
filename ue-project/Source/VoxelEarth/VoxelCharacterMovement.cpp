#include "VoxelCharacterMovement.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "VoxelCoords.h"
#include "VoxelWorldSubsystem.h"

namespace
{
// Converts a world-space interval [Lo, Hi] (UU) into the inclusive integer
// voxel-index range it overlaps. KINDA_SMALL_NUMBER keeps a box face that
// lands exactly on a voxel boundary from spuriously pulling in the next voxel
// over.
void AxisVoxelRange(double Lo, double Hi, int64& OutMin, int64& OutMax)
{
	OutMin = (int64)FMath::FloorToDouble(Lo / VoxelCoords::VoxelSizeUU);
	OutMax = (int64)FMath::FloorToDouble((Hi - KINDA_SMALL_NUMBER) / VoxelCoords::VoxelSizeUU);
	if (OutMax < OutMin)
	{
		OutMax = OutMin;
	}
}
} // namespace

using namespace VoxelMovementTuning;

UVoxelCharacterMovementComponent::UVoxelCharacterMovementComponent()
{
	// Driven externally via TickMovement, called from the owning pawn's Tick
	// (see the header): one movement clock, owned by the pawn.
	PrimaryComponentTick.bCanEverTick = false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void UVoxelCharacterMovementComponent::SetMoveInput(float Forward, float Right, float Up)
{
	CurrentForwardInput = Forward;
	CurrentRightInput = Right;
	CurrentUpInput = Up;
}

void UVoxelCharacterMovementComponent::RequestJump()
{
	bJumpKeyHeld = true;
	// BUFFERED, not applied: a jump pressed slightly before landing would
	// otherwise be silently dropped, which is the single most common "the game
	// ate my input" complaint. TickMovement spends this within
	// JumpBufferSeconds if a coyote-valid ground contact appears.
	JumpBufferRemainingSeconds = JumpBufferSeconds;
}

void UVoxelCharacterMovementComponent::ReleaseJump()
{
	bJumpKeyHeld = false;
	// Variable jump height: cut the remaining rise so a tap clears a 1-3 voxel
	// step without the full ~1 m apex. Only ever REDUCES upward velocity, so it
	// can never be used to gain height.
	if (VerticalVelocity > 0.0)
	{
		VerticalVelocity *= JumpReleaseCutScale;
	}
}

void UVoxelCharacterMovementComponent::AdjustSpeedTier(int32 Delta)
{
	SpeedTierIndex = FMath::Clamp(SpeedTierIndex + Delta, 0, kNumSpeedTiers - 1);
}

const TCHAR* UVoxelCharacterMovementComponent::GetSpeedTierName() const
{
	return NameForTier(SpeedTierIndex);
}

double UVoxelCharacterMovementComponent::GetEffectiveMaxSpeedUU() const
{
	const double Dial = GetDialSpeedUU();
	// Crouch CLAMPS rather than replaces: crouching while dialled below the cap
	// must not speed the character up.
	if (bCrouched)
	{
		return FMath::Min(Dial, CrouchSpeedCapUU);
	}
	return bSprintEngagedLastTick ? SpeedForTier(kSprintTierIndex) : Dial;
}

double UVoxelCharacterMovementComponent::ConsumeAbruptZJumpUU()
{
	const double Value = PendingAbruptZJumpUU;
	PendingAbruptZJumpUU = 0.0;
	return Value;
}

double UVoxelCharacterMovementComponent::ConsumeLandingImpactUU()
{
	const double Value = PendingLandingImpactUU;
	PendingLandingImpactUU = 0.0;
	return Value;
}

void UVoxelCharacterMovementComponent::ResetState()
{
	HorizontalVelocity = FVector::ZeroVector;
	VerticalVelocity = 0.0;
	JumpBufferRemainingSeconds = 0.0;
	TimeSinceGroundedSeconds = 0.0;
	bJumpKeyHeld = false;
	bWaitingForTerrain = false;
	bGroundedLastTick = false;
	bSwimmingLastTick = false;
	bSprintEngagedLastTick = false;
	PendingAbruptZJumpUU = 0.0;
	PendingLandingImpactUU = 0.0;

	// Stance is NOT reset here. bCrouched is a physical property of the
	// collision box, and the owner's location was already lowered to match; a
	// silent flip back to standing would embed the box in whatever is overhead.
	// Standing resumes through the ordinary CanStandAt path once the key is
	// released and there is room -- or via ForceStand when the caller knows
	// collision no longer applies.
	bStandBlocked = false;
}

// ---------------------------------------------------------------------------
// Voxel collision primitives
// ---------------------------------------------------------------------------

UVoxelWorldSubsystem* UVoxelCharacterMovementComponent::GetVoxelWorldSubsystem() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
}

bool UVoxelCharacterMovementComponent::IsGroundedAt(const FVector& Pos) const
{
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		return false;
	}

	// Downward 1-voxel probe under the box: solid anywhere in the voxel layer
	// immediately beneath the box's footprint counts as grounded.
	const double BottomZ = Pos.Z - GetHalfExtentZ();
	const int64 ProbeVZ = (int64)FMath::FloorToDouble(BottomZ / VoxelCoords::VoxelSizeUU) - 1;

	int64 VXMin, VXMax, VYMin, VYMax;
	AxisVoxelRange(Pos.X - BoxHalfExtentXY, Pos.X + BoxHalfExtentXY, VXMin, VXMax);
	AxisVoxelRange(Pos.Y - BoxHalfExtentXY, Pos.Y + BoxHalfExtentXY, VYMin, VYMax);

	for (int64 VX = VXMin; VX <= VXMax; ++VX)
	{
		for (int64 VY = VYMin; VY <= VYMax; ++VY)
		{
			if (Subsystem->IsSolidAtVoxel(VX, VY, ProbeVZ))
			{
				return true;
			}
		}
	}
	return false;
}

bool UVoxelCharacterMovementComponent::CanStandAt(const FVector& CrouchCenterPos) const
{
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		// World not ready: never TRAP the character crouched against a world
		// that cannot answer. Standing into unstreamed space is harmless (the
		// sweep has nothing to collide with either way).
		return true;
	}

	// Only the slab the taller box would NEWLY occupy needs testing -- from the
	// crouched box's top face up by StandClearanceUU. The volume below that is
	// already occupied by the crouched box, so re-testing it would report the
	// floor the character is standing on and never allow standing at all.
	const double CrouchTopZ = CrouchCenterPos.Z + CrouchHalfExtentZ;
	const double StandTopZ = CrouchTopZ + StandClearanceUU;

	int64 VXMin, VXMax, VYMin, VYMax, VZMin, VZMax;
	AxisVoxelRange(CrouchCenterPos.X - BoxHalfExtentXY, CrouchCenterPos.X + BoxHalfExtentXY, VXMin, VXMax);
	AxisVoxelRange(CrouchCenterPos.Y - BoxHalfExtentXY, CrouchCenterPos.Y + BoxHalfExtentXY, VYMin, VYMax);
	AxisVoxelRange(CrouchTopZ, StandTopZ, VZMin, VZMax);

	for (int64 VZ = VZMin; VZ <= VZMax; ++VZ)
	{
		for (int64 VX = VXMin; VX <= VXMax; ++VX)
		{
			for (int64 VY = VYMin; VY <= VYMax; ++VY)
			{
				if (Subsystem->IsSolidAtVoxel(VX, VY, VZ))
				{
					return false;
				}
			}
		}
	}
	return true;
}

bool UVoxelCharacterMovementComponent::UpdateCrouchState(FVector& InOutPos)
{
	// Both transitions keep the FEET planted: the box shrinks and grows from
	// the top only, so its bottom face never moves and the grounded probe stays
	// valid across the transition.
	if (bCrouchHeld && !bCrouched)
	{
		bCrouched = true;
		bStandBlocked = false;
		InOutPos.Z -= CrouchCenterDropUU;
		PendingAbruptZJumpUU += -CrouchCenterDropUU;
		return true;
	}

	if (!bCrouchHeld && bCrouched)
	{
		// Auto-stand: the key is released but standing is only permitted when
		// there is room. While blocked the character simply stays crouched and
		// this retries every tick, so walking out from under a ledge stands you
		// up on its own with no second key press.
		if (!CanStandAt(InOutPos))
		{
			bStandBlocked = true;
			return false;
		}
		bCrouched = false;
		bStandBlocked = false;
		InOutPos.Z += CrouchCenterDropUU;
		PendingAbruptZJumpUU += CrouchCenterDropUU;
		return true;
	}

	bStandBlocked = false;
	return false;
}

void UVoxelCharacterMovementComponent::ForceStand()
{
	if (!bCrouched)
	{
		return;
	}
	bCrouched = false;
	bStandBlocked = false;
	if (AActor* Owner = GetOwner())
	{
		// Same feet-planted rule as the ordinary transition, so the character
		// grows upward out of the crouch rather than rising off the floor.
		Owner->SetActorLocation(Owner->GetActorLocation() + FVector(0.0, 0.0, CrouchCenterDropUU));
		PendingAbruptZJumpUU += CrouchCenterDropUU;
	}
}

bool UVoxelCharacterMovementComponent::SweepAxis(int32 Axis, double Delta, FVector& InOutPos) const
{
	if (FMath::IsNearlyZero(Delta))
	{
		return false;
	}

	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		// World not ready (shouldn't happen once walking, defensive only): move
		// freely rather than getting stuck against nothing.
		InOutPos[Axis] += Delta;
		return false;
	}

	const double HalfExtent[3] = {BoxHalfExtentXY, BoxHalfExtentXY, GetHalfExtentZ()};

	const double OldCenter = InOutPos[Axis];
	const double NewCenter = OldCenter + Delta;

	// Voxel range the box overlaps across its full path along Axis this step
	// (covers both the start and end box, not just the destination).
	const double SweptLo = FMath::Min(OldCenter, NewCenter) - HalfExtent[Axis];
	const double SweptHi = FMath::Max(OldCenter, NewCenter) + HalfExtent[Axis];
	int64 VMin[3], VMax[3];
	AxisVoxelRange(SweptLo, SweptHi, VMin[Axis], VMax[Axis]);

	// The other two axes use the box's static extent at the (already
	// axis-resolved) position -- this is what makes the sweep axis-separated:
	// each axis is tested independently, in turn.
	const int32 Other1 = (Axis + 1) % 3;
	const int32 Other2 = (Axis + 2) % 3;
	AxisVoxelRange(InOutPos[Other1] - HalfExtent[Other1], InOutPos[Other1] + HalfExtent[Other1], VMin[Other1], VMax[Other1]);
	AxisVoxelRange(InOutPos[Other2] - HalfExtent[Other2], InOutPos[Other2] + HalfExtent[Other2], VMin[Other2], VMax[Other2]);

	// Walk the moving axis from the near side (closest to OldCenter) toward the
	// far side so the first hit found is the nearest blocking voxel.
	const int32 Step = Delta > 0 ? 1 : -1;
	const int64 AxisStart = Step > 0 ? VMin[Axis] : VMax[Axis];
	const int64 AxisEnd = Step > 0 ? VMax[Axis] : VMin[Axis];

	int64 V[3];
	bool bBlocked = false;
	int64 BlockingVoxel = 0;
	for (int64 AV = AxisStart; Step > 0 ? AV <= AxisEnd : AV >= AxisEnd; AV += Step)
	{
		V[Axis] = AV;
		bool bSliceSolid = false;
		for (int64 O1 = VMin[Other1]; O1 <= VMax[Other1] && !bSliceSolid; ++O1)
		{
			V[Other1] = O1;
			for (int64 O2 = VMin[Other2]; O2 <= VMax[Other2] && !bSliceSolid; ++O2)
			{
				V[Other2] = O2;
				if (Subsystem->IsSolidAtVoxel(V[0], V[1], V[2]))
				{
					bSliceSolid = true;
				}
			}
		}
		if (bSliceSolid)
		{
			bBlocked = true;
			BlockingVoxel = AV;
			break;
		}
	}

	if (!bBlocked)
	{
		InOutPos[Axis] = NewCenter;
		return false;
	}

	// Clamp the box's leading face to the blocking voxel's near face, with
	// CollisionEpsilonUU clearance so the box doesn't sit flush (and
	// potentially re-trigger a hit next step due to floating point noise).
	double ClampedCenter;
	if (Step > 0)
	{
		const double FaceMin = double(BlockingVoxel) * VoxelCoords::VoxelSizeUU;
		ClampedCenter = FMath::Min(NewCenter, FaceMin - HalfExtent[Axis] - CollisionEpsilonUU);
	}
	else
	{
		const double FaceMax = double(BlockingVoxel + 1) * VoxelCoords::VoxelSizeUU;
		ClampedCenter = FMath::Max(NewCenter, FaceMax + HalfExtent[Axis] + CollisionEpsilonUU);
	}
	InOutPos[Axis] = ClampedCenter;
	return true;
}

bool UVoxelCharacterMovementComponent::IsTerrainReadyAt(const FVector& Pos) const
{
	// THE PROBLEM. Terrain streams in asynchronously, so a walking character can
	// exist before the ground beneath it does. IsSolidAtVoxel answers "is there
	// a solid voxel here" and CANNOT distinguish "empty air" from "not generated
	// yet" -- both are false. Left alone, a character that spawns or toggles
	// into walk mode a fraction of a second early falls forever, and (worse) it
	// does so silently and looks like a collision bug.
	//
	// THE RULE. Gravity runs unless the character's OWN chunk is TRACKED (in the
	// streaming desired set) but does not yet own a COMPONENT -- precisely
	// "queued, not here yet", the boot case and the reason it must not fall the
	// moment it spawns. Anything else counts as ready:
	//
	//   * tracked WITH a component -- the ground is really there (or the chunk
	//     is genuinely empty air, a legitimate reason to fall);
	//   * not tracked at all -- outside the streaming footprint, e.g. an all-air
	//     chunk the sky-band optimisation never admits. Vetoing there would
	//     leave the character hovering wherever it stepped into open sky.
	//
	// ONLY the character's own chunk is probed. An earlier version also probed
	// the chunk one below, to stop a fall into terrain a frame away -- but that
	// held it hovering a metre above real ground whenever the solid rock
	// directly beneath was un-meshed (underground rock is never meshed, so its
	// chunk is legitimately "tracked, no component"). The genuine fall-through
	// case -- dropping into space that was never generated -- is caught instead
	// by the analytic-surface backstop at the end of TickMovement, which needs
	// no streaming at all. So this probe stays local and the caller additionally
	// short-circuits it when actually grounded.
	//
	// NOT snapshot-based. FVoxelPerfSnapshot looked like the obvious global "has
	// anything loaded yet" signal, but the subsystem only refreshes it while
	// voxel.Debug >= 1 (a deliberate zero-cost-at-mode-0 gate), so a snapshot
	// test would read all-zero in a normal session and hold the character in the
	// air forever. DebugChunkStatusAt is a live query with no such gate.
	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		return false;
	}

	bool bTracked = false;
	bool bHasComponent = false;
	int32 Quads = 0;
	if (Subsystem->DebugChunkStatusAt(Pos, bTracked, bHasComponent, Quads) && bTracked && !bHasComponent)
	{
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void UVoxelCharacterMovementComponent::TickMovement(float DeltaTime)
{
	AActor* Owner = GetOwner();
	if (!Owner || DeltaTime <= 0.f)
	{
		return;
	}

	UVoxelWorldSubsystem* Subsystem = GetVoxelWorldSubsystem();
	if (!Subsystem)
	{
		bWaitingForTerrain = true;
		return; // world not streamed in yet; hold position rather than fall through nothing
	}

	FVector Pos = Owner->GetActorLocation();

	// Stance first: it resizes the collision box and moves the box centre, so
	// every probe below (grounded, sweeps, clearance) must see the final shape.
	UpdateCrouchState(Pos);

	// Being GROUNDED on a real solid voxel is proof the terrain here has
	// arrived -- it trumps the streaming-readiness probe unconditionally. This
	// ordering matters: IsTerrainReadyAt also inspects the chunk one below, and
	// when standing on the floor of a resident chunk that lower chunk is solid
	// underground rock, which is legitimately never meshed (so "tracked, no
	// component"). Without this short-circuit that would read as "not ready" and
	// hold a character that is plainly standing on the ground.
	const bool bGroundedNow = IsGroundedAt(Pos);

	// Jump-feel timers, advanced before the jump is considered so a landing
	// this tick can immediately spend a buffered press.
	if (bGroundedNow)
	{
		TimeSinceGroundedSeconds = 0.0;
	}
	else
	{
		TimeSinceGroundedSeconds += DeltaTime;
	}
	JumpBufferRemainingSeconds = FMath::Max(0.0, JumpBufferRemainingSeconds - DeltaTime);

	// Terrain not streamed underneath (see IsTerrainReadyAt): suspend gravity
	// and hold Z. Horizontal movement is deliberately still allowed -- freezing
	// completely would strand the player with no way to walk back to loaded
	// ground -- but with no voxels to sweep against it is unobstructed, which is
	// the honest behaviour: there is nothing there yet. The HUD shows "WAITING
	// FOR TERRAIN" so this reads as a state, not a bug.
	bWaitingForTerrain = !bGroundedNow && !IsTerrainReadyAt(Pos);
	if (bWaitingForTerrain)
	{
		VerticalVelocity = 0.0;
		JumpBufferRemainingSeconds = 0.0;
		bGroundedLastTick = false;
		bSwimmingLastTick = false;
		bSprintEngagedLastTick = false;

		const FRotator HoldYawOnly(0.0, Owner->GetActorRotation().Yaw, 0.0);
		const FRotationMatrix HoldYawMatrix(HoldYawOnly);
		FVector HoldWish =
			HoldYawMatrix.GetUnitAxis(EAxis::X) * CurrentForwardInput + HoldYawMatrix.GetUnitAxis(EAxis::Y) * CurrentRightInput;
		if (HoldWish.SizeSquared() > 1.0)
		{
			HoldWish.Normalize();
		}
		HorizontalVelocity = HoldWish * GetEffectiveMaxSpeedUU();
		Owner->SetActorLocation(Pos + HorizontalVelocity * DeltaTime);
		return;
	}

	// Water track W1 swimming placeholder (docs/voxel-earth-implementation-
	// plan.md SS3.7): the box counts as "in the water" once it is entirely below
	// sea level (z=0, matching AVoxelOceanActor's implicit ocean --
	// VoxelCoords.h: voxel z=0 == UE world z=0). Binary swim/walk switch only --
	// no buoyancy, drag or currents (those are W4).
	const bool bSwimming = (Pos.Z + GetHalfExtentZ()) < 0.0;
	const bool bGrounded = !bSwimming && bGroundedNow;
	bSwimmingLastTick = bSwimming;
	bGroundedLastTick = bGrounded;

	// Horizontal wish direction from the WASD inputs, projected onto the
	// yaw-only horizontal plane -- ignores pitch (mouse look still pitches the
	// camera; it must not tilt walking into the ground).
	const FRotator YawOnly(0.0, Owner->GetActorRotation().Yaw, 0.0);
	const FRotationMatrix YawMatrix(YawOnly);
	const FVector YawForward = YawMatrix.GetUnitAxis(EAxis::X);
	FVector WishDir = YawForward * CurrentForwardInput + YawMatrix.GetUnitAxis(EAxis::Y) * CurrentRightInput;
	if (WishDir.SizeSquared() > 1.0)
	{
		WishDir.Normalize();
	}

	// Forward-only sprint: Shift is a momentary override to the TOP dial tier,
	// but only while actually heading forward, so strafing and backpedalling
	// stay at the dialled pace. Suppressed entirely while crouched (the crouch
	// cap wins) and while swimming.
	bSprintEngagedLastTick = false;
	if (bSprintHeld && !bCrouched && !bSwimming && !WishDir.IsNearlyZero())
	{
		bSprintEngagedLastTick = (WishDir | YawForward) >= SprintForwardDot;
	}

	if (bSwimming)
	{
		// Fly-style while submerged: no gravity, no jump, no step-up -- just
		// reduced-speed 3-axis movement, still swept against voxel collision
		// (below the DDA-swept ground, e.g. a lakebed).
		VerticalVelocity = 0.0;
		JumpBufferRemainingSeconds = 0.0;
	}
	else
	{
		// Coyote time + input buffering. A buffered press fires when a
		// coyote-valid ground contact exists; on success the coyote window is
		// spent outright so one airtime can never yield two jumps.
		const bool bCoyoteValid = TimeSinceGroundedSeconds <= CoyoteTimeSeconds;
		if (JumpBufferRemainingSeconds > 0.0 && bCoyoteValid)
		{
			VerticalVelocity = JumpSpeedUU;
			JumpBufferRemainingSeconds = 0.0;
			TimeSinceGroundedSeconds = CoyoteTimeSeconds + 1.0;

			// Holding the key from a previous jump must not carry into this
			// one; ReleaseJump only cuts a rise it was actually held through.
			if (!bJumpKeyHeld)
			{
				VerticalVelocity *= JumpReleaseCutScale;
			}
		}

		// Client-presentation kinematics only (see the header): plain gravity
		// integration, not part of the deterministic world derivation and not
		// authoritative -- M3's server owns real player movement.
		VerticalVelocity -= GravityUUPerSec2 * DeltaTime;
	}

	FVector HorizDelta;
	if (bSwimming)
	{
		// Fly-style while submerged (W1 placeholder, unchanged): instant
		// velocity from input, no accel/friction model. Kept synced into
		// HorizontalVelocity so ground movement resumes smoothly from this
		// value on exit instead of snapping.
		HorizontalVelocity = WishDir * SwimSpeedUU;
		HorizDelta = HorizontalVelocity * DeltaTime;
	}
	else
	{
		// Accel-based ground movement with friction-style damping when there is
		// no input (WishDir == 0 pulls HorizontalVelocity toward zero the same
		// way), and air control at 30% of ground accel while airborne -- this is
		// what makes direction changes feel weighted instead of binary.
		const double MaxSpeed = GetEffectiveMaxSpeedUU();
		const double Accel = bGrounded ? GroundAccelUUPerSec2 : (GroundAccelUUPerSec2 * AirControlFactor);

		const FVector TargetVelocity = WishDir * MaxSpeed;
		const FVector VelDelta = TargetVelocity - HorizontalVelocity;
		const double MaxStep = Accel * DeltaTime;
		if (VelDelta.SizeSquared() <= FMath::Square(MaxStep))
		{
			HorizontalVelocity = TargetVelocity;
		}
		else
		{
			HorizontalVelocity += VelDelta.GetSafeNormal() * MaxStep;
		}
		HorizDelta = HorizontalVelocity * DeltaTime;
	}

	// Swimming: vertical motion comes directly from the Space/LeftControl axis
	// (fly-style), not integrated velocity -- there's no "falling" underwater in
	// this placeholder.
	const double VertDelta = bSwimming ? (CurrentUpInput * SwimSpeedUU * DeltaTime) : (VerticalVelocity * DeltaTime);

	FVector NewPos = Pos;
	const bool bBlockedX = SweepAxis(0, HorizDelta.X, NewPos);
	const bool bBlockedY = SweepAxis(1, HorizDelta.Y, NewPos);

	bool bDidStepSnap = false;
	if (!bSwimming && (bBlockedX || bBlockedY) && bGrounded)
	{
		// Step-up: retry the same horizontal move 30 UU (3 voxels) higher; if
		// that clears, snap back down onto the ground with a downward sweep.
		// Handles ledges/stairs up to 3 voxels without a full physics-style step
		// solver.
		FVector StepPos = Pos + FVector(0.0, 0.0, StepUpHeightUU);
		const bool bStepBlockedX = SweepAxis(0, HorizDelta.X, StepPos);
		const bool bStepBlockedY = SweepAxis(1, HorizDelta.Y, StepPos);
		if (!bStepBlockedX && !bStepBlockedY)
		{
			SweepAxis(2, -(StepUpHeightUU + CollisionEpsilonUU * 2.0), StepPos);
			NewPos = StepPos;
			bDidStepSnap = true;
			VerticalVelocity = 0.0;
		}
		// Else: the raised retry also hit something (a wall, not a step) -- keep
		// the normal (blocked/clamped) horizontal result in NewPos.
	}

	// Ledge-safe edge stop, crouched only. Crouching is how you work at the lip
	// of a dug void, so a crouched character refuses to walk off a ledge into
	// open air.
	//
	// Runs AFTER the step-up retry deliberately: a step-up legitimately changes
	// Z and lands on new ground, and validating before it would reject the step
	// as "not grounded" and break crouched stair-climbing entirely.
	//
	// Per-axis rather than all-or-nothing so movement can still slide ALONG an
	// edge instead of sticking to it. The full revert restores Z as well, which
	// matters when a step snap raised it: keeping a stepped-up Z with reverted
	// X/Y would leave the character floating.
	if (bCrouched && bGrounded && !bSwimming && !IsGroundedAt(NewPos))
	{
		const FVector KeepX(NewPos.X, Pos.Y, NewPos.Z);
		const FVector KeepY(Pos.X, NewPos.Y, NewPos.Z);
		if (IsGroundedAt(KeepX))
		{
			NewPos = KeepX;
			HorizontalVelocity.Y = 0.0;
		}
		else if (IsGroundedAt(KeepY))
		{
			NewPos = KeepY;
			HorizontalVelocity.X = 0.0;
		}
		else
		{
			NewPos = Pos;
			bDidStepSnap = false;
			HorizontalVelocity = FVector::ZeroVector;
		}
	}

	if (!bDidStepSnap)
	{
		// Capture the pre-sweep velocity: a blocked downward sweep IS the
		// landing, and the sweep zeroes the velocity that describes how hard it
		// was.
		const double PreSweepVerticalVelocity = VerticalVelocity;
		if (SweepAxis(2, VertDelta, NewPos))
		{
			if (PreSweepVerticalVelocity < 0.0)
			{
				PendingLandingImpactUU = FMath::Max(PendingLandingImpactUU, -PreSweepVerticalVelocity);
			}
			VerticalVelocity = 0.0;
		}
	}
	else
	{
		// The step-up snapped Z abruptly (up to StepUpHeightUU = 30 UU); report
		// it so the camera absorbs the jump instead of teleporting with it.
		PendingAbruptZJumpUU += NewPos.Z - Pos.Z;
	}

	// --- Backstop: never fall through the surface into unstreamed space -----
	//
	// IsTerrainReadyAt (above) catches "the chunk I am in is queued", but it
	// cannot catch this: the streaming set does not ADMIT underground chunks
	// until the anchor itself goes underground (FVoxelWorldImpl's deep-set
	// policy). So a character that starts falling from a few metres up can
	// outrun the surface chunk, cross into space that was never generated at all
	// -- where IsSolidAtVoxel honestly answers "air" for every voxel -- and keep
	// accelerating downwards forever. Measured directly: entering walk mode 1 s
	// after launch put the pawn 14.9 m BELOW the surface and still airborne.
	//
	// The fix uses the one height that needs no streaming at all:
	// GetSurfaceHeightUU is the analytic amplifier surface, available
	// everywhere, immediately. If the box has sunk below it and the chunk it is
	// in has no component, it is falling through terrain that does not exist yet
	// -- so park it on the analytic surface and report "waiting for terrain"
	// until the real voxels arrive.
	//
	// This deliberately does NOT fire when the chunk IS resident, which is what
	// makes caves, dug holes and cave mouths work normally: those are all below
	// the analytic surface, but their chunks are loaded, so the real voxel sweep
	// stays in charge and this never intervenes.
	if (!bSwimming)
	{
		const double SurfaceUU = Subsystem->GetSurfaceHeightUU(NewPos.X, NewPos.Y);
		if ((NewPos.Z - GetHalfExtentZ()) < (SurfaceUU - SurfaceBackstopToleranceUU))
		{
			bool bTracked = false;
			bool bHasComponent = false;
			int32 Quads = 0;
			const bool bResident = Subsystem->DebugChunkStatusAt(NewPos, bTracked, bHasComponent, Quads) && bHasComponent;
			if (!bResident)
			{
				NewPos.Z = SurfaceUU + GetHalfExtentZ();
				VerticalVelocity = 0.0;
				bWaitingForTerrain = true;
				bGroundedLastTick = false;
				// Parking is a teleport, not a landing: suppress the view punch
				// that the fall would otherwise have earned.
				PendingLandingImpactUU = 0.0;
			}
		}
	}

	Owner->SetActorLocation(NewPos);

	// Gait phase advances with DISTANCE travelled, not time, so the limb swing
	// and camera bob speed up with the character rather than running at a fixed
	// rate. Owned here so the proxy body and the first-person bob share one
	// phase (see GetGaitPhase).
	GaitPhase += (float)(HorizontalVelocity.Size() * DeltaTime) * (2.f * PI / (float)StrideLengthUU);
	GaitPhase = FMath::Fmod(GaitPhase, 2.f * PI);
}
