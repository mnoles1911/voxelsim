#pragma once

#include "CoreMinimal.h"

// M6 NPC swarm (docs/voxel-earth-implementation-plan.md SS3.6, navigation-
// only v0 -- agents pursue but never dig/edit terrain; that's the next
// slice). This header defines FVoxelAgent, the per-agent "cheap body"
// record for the pool owned by UVoxelAgentSubsystem.
//
// Deliberately a PLAIN struct, NOT a UCLASS/AActor: "hundreds of NPCs"
// means the pool must be cheap to store/iterate/move, so it lives as one
// contiguous TArray<FVoxelAgent> (UVoxelAgentSubsystem.h) rather than
// hundreds of heavy UObjects each carrying tick/replication/component
// overhead. Rendering mirrors AVoxelDebris' "engine cubes" style
// (VoxelDebris.h): a single UInstancedStaticMeshComponent, one instance per
// agent (FVoxelAgent::InstanceIndex), never N actors.
//
// Deliberately voxel-core-free (no vxc:: types): this keeps the header safe
// to include from UVoxelAgentSubsystem.h, a UHT-parsed UCLASS header --
// same "UE reflection headers stay voxel-core-free" doctrine
// VoxelWorldSubsystem.h/VoxelMeshTypes.h already follow. The actual
// vxc::PathResult driving Tier 0/1's Waypoints below lives PIMPL-side, in
// UVoxelAgentSubsystem.cpp's FVoxelAgentImpl (a parallel array, same index
// as the pool) -- see that file's class comment for why the split is there
// and not here.

// Simulation LOD tier (plan SS3.6 "NPCs & AI (simulation LOD)"): bucketed
// by distance-to-player every subsystem tick via ComputeNextVoxelAgentTier
// below, which applies hysteresis so an agent sitting near a tier boundary
// doesn't flip back and forth every frame.
enum class EVoxelAgentTier : uint8
{
	Tier0_Embodied = 0,     // full windowed A*, replan whenever needed, ground-snapped every tick
	Tier1_Abstract = 1,     // same A*, coarser replan cadence + staggered ground-snap/validity checks
	Tier2_Statistical = 2,  // no A* at all -- bounded steering toward the player, infrequent ground snap
};

struct FVoxelAgent
{
	// World-space (UU = cm, LWC-safe) position of the agent's FEET (ground
	// contact point, not the body's visual center -- UVoxelAgentSubsystem::
	// UpdateInstanceTransform offsets the ISM instance up by
	// UVoxelAgentSubsystem::AgentHalfHeightUU to place the visual body).
	FVector Position = FVector::ZeroVector;

	// Tier 2 steering velocity only (world-space UU/s) -- Tier 0/1 move by
	// advancing along Waypoints (AdvanceAlongWaypoints below) instead and
	// leave this at zero.
	FVector Velocity = FVector::ZeroVector;

	// Tier 0/1 only: world-space centers of the CURRENT windowed path's
	// steps, rebuilt from a fresh vxc::PathResult every replan (see
	// UVoxelAgentSubsystem::PlanPath). Empty when the agent holds no path
	// right now (freshly spawned, path just exhausted, or demoted to
	// Tier 2 -- Tier 2 never populates this).
	TArray<FVector> Waypoints;
	int32 WaypointIndex = 0;

	EVoxelAgentTier Tier = EVoxelAgentTier::Tier1_Abstract;

	// UWorld::GetTimeSeconds() at the last replan. Tier 1's coarser replan
	// cadence gates on this (UVoxelAgentSubsystem::Tier1ReplanIntervalSeconds);
	// Tier 0 ignores it and replans purely on path-exhausted/invalid/stale
	// conditions instead (see the subsystem Tick doc comment).
	double LastReplanTimeSeconds = -1.0;

	// The (ground-projected -- see UVoxelAgentSubsystem::Tick's class
	// comment on why) player goal column the CURRENT Waypoints were planned
	// toward. A replan is forced once the live goal drifts more than
	// UVoxelAgentSubsystem::ReplanGoalDriftThresholdUU away from this
	// cached value, without waiting for the path to exhaust first.
	FVector LastPlanGoalWorld = FVector::ZeroVector;

	// True once the agent has closed to within
	// UVoxelAgentSubsystem::StandoffRadiusUU of the player: movement stops
	// (no piling into the player's own cell) until the player moves back
	// out beyond StandoffResumeUU (hysteresis, same shape as the tier
	// bands above).
	bool bIdleAtStandoff = false;

	// UWorld::GetTimeSeconds() at the last ground-snap raycast (VoxelAgent
	// Subsystem::GroundSnap). Tier 0 snaps every tick and updates this
	// unconditionally; Tier 1/2 gate their (coarser) ground-snap cadence on
	// it -- see UVoxelAgentSubsystem::Tier2GroundSnapIntervalSeconds.
	double LastGroundSnapTimeSeconds = -1.0;

	// Index into UVoxelAgentSubsystem::AgentISM's instance array, or
	// INDEX_NONE on a dedicated server (no ISM exists there at all -- no
	// viewport to render into, see UVoxelAgentSubsystem::OnWorldBeginPlay).
	int32 InstanceIndex = INDEX_NONE;
};

// Hysteresis hand-off between adjacent tiers: an agent keeps its CURRENT
// tier until distance crosses the boundary AWAY from it, using a wider
// (farther) threshold to leave a tier than to enter it -- see
// UVoxelAgentSubsystem's Tier0Enter/ExitUU, Tier1Enter/ExitUU constants for
// the concrete numbers (kept there, not here, since they are the
// subsystem's scheduler tuning, documented alongside the rest of the LOD
// scheduler design). Pure function of (current tier, distance) -- no
// subsystem/world dependency, easy to reason about/test in isolation.
EVoxelAgentTier ComputeNextVoxelAgentTier(EVoxelAgentTier CurrentTier, double DistanceToPlayerUU,
                                           double Tier0EnterUU, double Tier0ExitUU, double Tier1EnterUU,
                                           double Tier1ExitUU);

// Tier 0/1 movement: advances Agent.Position toward Agent.Waypoints[Agent.
// WaypointIndex] at SpeedUUPerSec (horizontal/XY only -- Z is ground-
// snapped separately via the voxel raycast, UVoxelAgentSubsystem::
// GroundSnap, so waypoint Z from the path's voxel centers is never used
// directly for movement). Advances WaypointIndex once within
// AdvanceThresholdUU (XY) of the current target. Returns true once the
// path is exhausted (WaypointIndex has passed the last waypoint, or there
// was nothing to follow), signalling the caller a replan is due.
bool AdvanceAlongWaypoints(FVoxelAgent& Agent, double DeltaSeconds, double SpeedUUPerSec,
                            double AdvanceThresholdUU);

// Tier 2 movement: NO pathfinding at all -- bounded steering straight
// toward PlayerWorldPos (XY only) plus a small deterministic-per-agent
// perpendicular wander (AgentSeed varies the phase so a crowd doesn't
// march in a perfectly straight line), integrated directly into
// Agent.Position. This is intentionally the cheapest possible per-agent
// update: no world/voxel query at all (ground snap is handled separately,
// on its own infrequent cadence, by the caller).
void SteerVoxelAgentTier2(FVoxelAgent& Agent, const FVector& PlayerWorldPos, double DeltaSeconds,
                           double SpeedUUPerSec, double WanderAmplitudeUUPerSec, double TimeSeconds,
                           int32 AgentSeed);
