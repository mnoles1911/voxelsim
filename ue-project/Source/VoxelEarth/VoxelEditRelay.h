#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoxelEditRelay.generated.h"

// M3 wave 1 (docs/m3-plan.md): the ONE replicated transport for the edit-log
// authority stream -- server-authored entries broadcast to every client
// after each authoritative edit batch, plus the seed/worldgen/digest
// handshake clients verify against on join. Deliberately generic (m3-plan.md
// wave 3 note: "the relay generalizes to non-edit authoritative streams
// later -- do not build it narrower than 'seq-stamped opaque entry
// stream'") so later systems (water CA, NPC blueprint diffs) can reuse the
// same broadcast path instead of inventing a new one.
//
// Spawned once by AVoxelEarthGameMode::BeginPlay on authority, ONLY when
// actually networked (NM_Standalone spawns nothing at all -- single-player
// must stay byte-identical to pre-M3 behavior). Standard UE actor
// replication then auto-spawns a proxy of this same actor on every client
// that joins; bAlwaysRelevant keeps it visible to every connection
// regardless of ownership/location.
//
// IMPORTANT -- what does NOT live here: client-initiated traffic (submit
// edit intent, request join-sync log replay). UE Server RPCs are only
// callable by the connection that OWNS the target actor
// (AActor::GetNetConnection() walks the Owner chain; an actor with no Owner
// has no net connection, so no client can successfully call a Server RPC on
// it -- the engine silently rejects the call). This relay is deliberately a
// single, unowned, world-scoped actor (one instance for everyone, not one
// per player), so it CANNOT receive client-called Server RPCs. Those calls
// instead live on AVoxelEarthPlayerController (ServerSubmitDigIntent /
// ServerSubmitPlaceIntent / ServerSubmitCarveIntent / ServerRequestJoinSync /
// ClientReceiveJoinSyncChunk), which IS reliably owned by its own client
// connection. This relay carries only server -> everyone traffic
// (MulticastAppliedEntries, the replicated handshake fields), which
// NetMulticast/property replication deliver regardless of ownership.
UCLASS()
class VOXELEARTH_API AVoxelEditRelay : public AActor
{
	GENERATED_BODY()

public:
	AVoxelEditRelay();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Determinism-guard handshake (m3-plan.md decisions table "Determinism
	// guard"): set once, authority-side, in BeginPlay from this world's
	// UVoxelWorldSubsystem; replicated to every client as part of the
	// actor's initial replication (arrives before the client's own BeginPlay
	// runs -- standard UE actor-channel ordering). Client-side BeginPlay
	// compares these against its own locally-computed values and hard-
	// disconnects on mismatch (see the .cpp).
	UPROPERTY(Replicated)
	uint64 ServerSeed = 0;
	UPROPERTY(Replicated)
	uint32 ServerWorldGenVersion = 0;
	UPROPERTY(Replicated)
	uint64 ServerProbeDigest = 0;

	// F7 sky-epoch replication (docs/water-ocean-tides-plan-2026-09-04.md
	// Phase F): the server's world clock, as TWO scalars -- the epoch itself and
	// the rate it is advancing at. This is the reuse this class's header comment
	// promised ("the relay generalizes to non-edit authoritative streams later")
	// and the exact design VoxelSkySubsystem.h's old TODO named: two fields on
	// the actor that already exists, already replicates, and is already
	// bAlwaysRelevant -- NOT a second replicated actor, which would be a channel,
	// a relevancy question and a spawn-ordering race bought for two numbers.
	//
	// WHY THE RATE RIDES ALONG: the epoch is pushed at a low fixed cadence
	// (UVoxelSkySubsystem's kSkyEpochPushPeriodSeconds, ~5 s), so between pushes
	// the client dead-reckons the server clock forward at this rate. Without it
	// a client whose local voxel.Sky.TimeScale disagreed with the server's would
	// saw-tooth: drift for 5 s, get yanked back, drift again.
	//
	// WRITER: UVoxelSkySubsystem::Tick on the authority, via AuthoritySetSkyClock
	// below -- the relay itself never computes a clock, exactly as it never
	// computes an edit. READER: OnRep_SkyClock forwards both scalars to the
	// client's UVoxelSkySubsystem, which owns the smooth-correction policy
	// (AdoptReplicatedEpoch -- rate bound, snap threshold, and the gate log line
	// all live there, beside the clock they correct).
	//
	// RepNotify rides on the EPOCH only. Both fields are written together by
	// AuthoritySetSkyClock and the epoch changes on every push while the clock
	// runs, so one notify per push is the normal case. The one theoretical miss
	// -- a time-scale change landing while the epoch is frozen at an identical
	// value (TimeScale 0) -- self-heals on the next push and is bounded-corrected
	// meanwhile; a second notify on the scale would instead double-invoke the
	// adopt path (UE fires one RepNotify per changed property) and double-count
	// its receipt evidence.
	UPROPERTY(ReplicatedUsing = OnRep_SkyClock)
	double ServerSkyEpochSeconds = 0.0;
	UPROPERTY(Replicated)
	float ServerSkyTimeScale = 1.0f;

	// Authority-side setter for the pair above; no-op on non-authority instances
	// so a confused caller cannot fork a client's proxy from the server's truth.
	void AuthoritySetSkyClock(double EpochSeconds, float TimeScale);

	UFUNCTION()
	void OnRep_SkyClock();

	// Server -> every client: reliable broadcast of newly-applied edit-log
	// entries (wire format: UVoxelWorldSubsystem::SerializeLogEntriesFrom /
	// ApplyReplicatedEntries), called by UVoxelWorldSubsystem's
	// BroadcastNewEntries helper after any authoritative TryDig/TryPlace/
	// CarveSphere -- both from the server's own local player (listen
	// server) and from a client's forwarded intent (AVoxelEarthPlayerController's
	// ServerSubmit*Intent RPCs), one code path either way. No-ops on the
	// calling (authority) instance itself -- NetMulticast functions execute
	// locally on the server too, but the server already applied these
	// entries directly; only remote (client) instances need to replay them.
	UFUNCTION(NetMulticast, Reliable)
	void MulticastAppliedEntries(const TArray<uint8>& EntryBytes);

	// W2 (docs/voxel-earth-implementation-plan.md SS3.7 Layer B replication):
	// exactly the reuse this class's header comment called for -- "the relay
	// generalizes to non-edit authoritative streams later ... water CA" --
	// so this is a second seq-stamped opaque entry stream on the SAME relay
	// actor rather than a new one. Server -> every client: reliable broadcast
	// of dirty water-brick fill-diffs, called by UVoxelWaterSubsystem at a
	// fixed ~5Hz cadence (never per-CA-tick -- the CA ticks at 10Hz, the wire
	// cadence is independently throttled and byte-capped, see
	// UVoxelWaterSubsystem::BroadcastWaterDiffs). Wire format: flat
	// brick-key + raw-512-byte-fill pairs (UVoxelWaterSubsystem::
	// SerializeWaterDiffs/ParseWaterDiffs) -- brick-granularity snapshots,
	// NOT a per-cell delta; full compression is W2-polish per the task spec.
	// No-ops on the calling (authority) instance itself, same reasoning as
	// MulticastAppliedEntries above (the server's own CA is already
	// authoritative; only remote instances need to mirror it).
	UFUNCTION(NetMulticast, Reliable)
	void MulticastWaterDiffs(const TArray<uint8>& DiffBytes);
};
