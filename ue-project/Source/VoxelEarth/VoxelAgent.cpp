#include "VoxelAgent.h"

EVoxelAgentTier ComputeNextVoxelAgentTier(EVoxelAgentTier CurrentTier, double DistanceToPlayerUU,
                                           double Tier0EnterUU, double Tier0ExitUU, double Tier1EnterUU,
                                           double Tier1ExitUU)
{
	// Asymmetric enter/exit thresholds per boundary is the whole hysteresis
	// trick: crossing INTO a tier requires getting closer than *EnterUU,
	// crossing OUT requires getting farther than *ExitUU (> EnterUU) --  an
	// agent sitting exactly between the two thresholds never flips, no
	// matter how much per-frame noise its distance has.
	switch (CurrentTier)
	{
	case EVoxelAgentTier::Tier0_Embodied:
		return DistanceToPlayerUU > Tier0ExitUU ? EVoxelAgentTier::Tier1_Abstract : EVoxelAgentTier::Tier0_Embodied;

	case EVoxelAgentTier::Tier1_Abstract:
		if (DistanceToPlayerUU < Tier0EnterUU)
		{
			return EVoxelAgentTier::Tier0_Embodied;
		}
		if (DistanceToPlayerUU > Tier1ExitUU)
		{
			return EVoxelAgentTier::Tier2_Statistical;
		}
		return EVoxelAgentTier::Tier1_Abstract;

	case EVoxelAgentTier::Tier2_Statistical:
		return DistanceToPlayerUU < Tier1EnterUU ? EVoxelAgentTier::Tier1_Abstract : EVoxelAgentTier::Tier2_Statistical;
	}
	return CurrentTier;
}

bool AdvanceAlongWaypoints(FVoxelAgent& Agent, double DeltaSeconds, double SpeedUUPerSec,
                            double AdvanceThresholdUU)
{
	if (Agent.Waypoints.Num() == 0 || Agent.WaypointIndex >= Agent.Waypoints.Num())
	{
		return true; // nothing to follow -- caller should replan
	}

	const FVector Target = Agent.Waypoints[Agent.WaypointIndex];
	FVector ToTarget = Target - Agent.Position;
	ToTarget.Z = 0.0; // horizontal-only follow; Z comes from GroundSnap, not the path

	const double Dist = ToTarget.Size();
	if (Dist <= AdvanceThresholdUU)
	{
		++Agent.WaypointIndex;
		return Agent.WaypointIndex >= Agent.Waypoints.Num();
	}

	const double StepLen = FMath::Min(SpeedUUPerSec * DeltaSeconds, Dist);
	Agent.Position += ToTarget.GetSafeNormal() * StepLen;
	return false;
}

void SteerVoxelAgentTier2(FVoxelAgent& Agent, const FVector& PlayerWorldPos, double DeltaSeconds,
                           double SpeedUUPerSec, double WanderAmplitudeUUPerSec, double TimeSeconds,
                           int32 AgentSeed)
{
	FVector ToPlayer = PlayerWorldPos - Agent.Position;
	ToPlayer.Z = 0.0;
	const FVector Dir = ToPlayer.GetSafeNormal();

	// Bounded perpendicular wander so a Tier 2 crowd doesn't read as a
	// perfectly straight-line march -- phase varies per agent (AgentSeed)
	// so the crowd doesn't wander in lockstep either. NOT required to be
	// bit-reproducible: this is cosmetic-only steering (plan SS3.6's
	// "statistical" tier), unlike pathfind.h it is never part of
	// authoritative world state and never touches the edit log.
	const FVector Perp(-Dir.Y, Dir.X, 0.0);
	const double Wander = FMath::Sin(TimeSeconds * 0.6 + double(AgentSeed)) * WanderAmplitudeUUPerSec;

	Agent.Velocity = Dir * SpeedUUPerSec + Perp * Wander;
	Agent.Position += Agent.Velocity * DeltaSeconds;
}
