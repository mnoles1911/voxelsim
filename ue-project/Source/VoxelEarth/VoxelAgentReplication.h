#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelAgentReplication.generated.h"

// M6 gap closure (docs/status.md M6 section "Tier-1 hierarchical planning +
// NPC replication"): the ONE replicated transport for NPC swarm state,
// mirroring AVoxelEditRelay's (VoxelEditRelay.h) "single unowned world-scoped
// actor, server -> everyone NetMulticast" shape -- deliberately a SEPARATE
// actor/file rather than adding a third stream to AVoxelEditRelay, since that
// file is owned by another slice this run (VoxelEditRelay.h's own doctrine
// note: "the relay generalizes to non-edit authoritative streams later," but
// this slice's owned-files list is VoxelAgent*/VoxelAgentSubsystem* + "a new
// file if you need one for replication").
//
// IMPORTANT doctrine distinction from the edit-log relay: agents are NORMAL
// replicated game state, NOT deterministically re-derived on clients, and
// NOT part of the edit log -- only their terrain EDITS (already wired,
// UVoxelWorldSubsystem::TryDig/TryPlace via UVoxelAgentSubsystem::
// TryExecuteWaypointEdit) go through that path. This actor exists solely to
// tell a remote client WHERE the server's agents currently are and what tier
// they're in, so it can draw them -- the server remains the sole authority
// for every path/dig decision (UVoxelAgentSubsystem::Tick's NM_Client early-out
// is unchanged: a client never simulates).
//
// Replication design (see UVoxelAgentSubsystem::CollectReplicationSnapshot/
// ApplyReplicatedAgentSnapshot for the wire format/client-side details):
//   - Rate: BroadcastIntervalSeconds (10Hz) via this actor's own Tick,
//     independent of and much coarser than the swarm SIMULATION's own tick
//     rate -- "batch/compress ... send at a sensible rate" (task doctrine),
//     not naive every-tick replication.
//   - Batching: ALL currently-relevant agents go out in ONE NetMulticast call
//     per broadcast, not one call/actor per agent.
//   - Compression: positions are sent as int16 offsets (UU) relative to
//     OriginWorld (a replicated FVector re-anchored near the players every
//     broadcast, keeping the offsets small and LWC-safe regardless of how
//     far from the engine origin the swarm currently is), plus a uint8 tier
//     -- 11 bytes/agent, vs. 24+ for a raw per-agent double FVector.
//   - Relevancy/culling: Tier 2 agents (always farther than Tier1ExitUU from
//     EVERY player already, per the tier scheduler's own hysteresis, and the
//     least cosmetically important tier to begin with) are NEVER included.
//     Tier 0/1 agents are included only within RelevancyRadiusUU of
//     OriginWorld, which this actor re-anchors every broadcast at the FIRST
//     connected player's pawn location (docs/status.md M6 section --
//     matches this module's existing "first player controller" precedent,
//     e.g. UVoxelAgentSubsystem::Tick's own pursuit-target lookup). This is a
//     server-side traffic filter (which AGENTS go out), not a per-connection
//     actor-relevancy filter (which CONNECTIONS receive the actor) -- see
//     the constructor's bAlwaysRelevant comment (.cpp) for why the latter
//     isn't used here.
//   - Interpolation: client-side (UVoxelAgentSubsystem::TickClientReplicated
//     Agents) -- each tracked agent eases from its last known render position
//     toward the freshly received one over ClientAgentInterpolationWindowSeconds,
//     so 10Hz updates don't read as visible teleport-snapping.
//
// Transport: NetMulticast Reliable, exactly mirroring AVoxelEditRelay::
// MulticastWaterDiffs's already-proven precedent in this codebase (a fixed-
// cadence, byte-capped broadcast of ephemeral simulation state). A truly
// optimal wire choice would be Unreliable (a stale position snapshot is
// worthless once a newer one lands, so guaranteed delivery buys nothing) --
// Reliable is used here to match the existing proven precedent and avoid
// introducing a new reliability-edge-case class in this slice; documented as
// a follow-up for a shipped version.
UCLASS()
class VOXELEARTH_API AVoxelAgentReplicator : public AActor
{
	GENERATED_BODY()

public:
	AVoxelAgentReplicator();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Wire cadence (class comment "Rate"): independent of, and much coarser
	// than, both the swarm simulation's own tick rate and this actor's own
	// Actor tick (which just accumulates time toward this).
	static constexpr double BroadcastIntervalSeconds = 0.1; // 10Hz

	// Relevancy/culling radius (class comment "Relevancy/culling"), UU:
	// comfortably covers the full Tier 0 + Tier 1 band (UVoxelAgentSubsystem::
	// Tier0EnterUU/ExitUU, Tier1EnterUU/ExitUU top out at Tier1ExitUU=10,000UU
	// = 100m) plus margin, so an agent doesn't flicker in/out of replication
	// right at the tier boundary the same way the tier scheduler itself uses
	// hysteresis to avoid flickering tiers.
	static constexpr double RelevancyRadiusUU = 12000.0; // 120m

private:
	// Re-anchored every broadcast to the first connected player's pawn
	// location (see class comment "Relevancy/culling") -- every agent
	// position in the matching MulticastAgentSnapshot payload is relative to
	// THIS value at THIS broadcast, not the engine origin.
	UPROPERTY(Replicated)
	FVector OriginWorld = FVector::ZeroVector;

	// Server -> every client: reliable broadcast of the current relevant-agent
	// snapshot (UVoxelAgentSubsystem::CollectReplicationSnapshot's wire
	// format). No-ops on the calling (authority) instance itself -- same
	// reasoning as AVoxelEditRelay::MulticastAppliedEntries/MulticastWaterDiffs
	// (the server's own Agents pool is already authoritative; only remote
	// instances need this to know where to draw anything at all).
	UFUNCTION(NetMulticast, Reliable)
	void MulticastAgentSnapshot(FVector InOriginWorld, const TArray<uint8>& SnapshotBytes);

	double TimeSinceLastBroadcastSeconds = 0.0;
};
