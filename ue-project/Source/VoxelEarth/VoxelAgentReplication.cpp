#include "VoxelAgentReplication.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "VoxelAgentSubsystem.h"
#include "VoxelEarth.h"

AVoxelAgentReplicator::AVoxelAgentReplicator()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	// Same reasoning as AVoxelEditRelay's identical bAlwaysRelevant (VoxelEditRelay.cpp):
	// this is a single, unowned, world-scoped actor (one instance for
	// everyone), and every connected client needs the swarm broadcast --
	// there is no per-connection "near" to filter actor relevancy on for a
	// singleton like this. RELEVANCY in the "which agents get sent" sense
	// (class comment) is instead implemented server-side, inside the
	// broadcast payload itself (see Tick below / UVoxelAgentSubsystem::
	// CollectReplicationSnapshot), not by limiting which connections this
	// actor's RPCs reach.
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
}

void AVoxelAgentReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AVoxelAgentReplicator, OriginWorld);
}

void AVoxelAgentReplicator::BeginPlay()
{
	Super::BeginPlay();
	// Unlike AVoxelEditRelay, there is no determinism handshake to run here
	// -- agent state is normal replicated game state (class comment), not
	// something a client independently derives and cross-checks. Nothing
	// else to do at BeginPlay; Tick drives everything.
}

void AVoxelAgentReplicator::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return; // a remote instance only ever receives MulticastAgentSnapshot; it never originates one
	}

	TimeSinceLastBroadcastSeconds += double(DeltaSeconds);
	if (TimeSinceLastBroadcastSeconds < BroadcastIntervalSeconds)
	{
		return;
	}
	TimeSinceLastBroadcastSeconds = 0.0;

	UWorld* World = GetWorld();
	UVoxelAgentSubsystem* AgentSubsystem = World ? World->GetSubsystem<UVoxelAgentSubsystem>() : nullptr;
	if (!AgentSubsystem || AgentSubsystem->GetAgentCount() == 0)
	{
		return; // nothing to broadcast yet
	}

	// Relevancy origin (class comment "Relevancy/culling"): the first
	// connected player's pawn location, matching UVoxelAgentSubsystem::Tick's
	// own "first player controller" pursuit-target precedent. A future
	// multi-player-aware version could union several players' nearby agents;
	// this run's verification is a single-player-per-process two-client
	// scenario (docs/status.md M6 section), where this is exactly correct.
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* PlayerPawn = PC ? PC->GetPawn() : nullptr;
	if (!PlayerPawn)
	{
		return;
	}
	OriginWorld = PlayerPawn->GetActorLocation();

	TArray<uint8> SnapshotBytes;
	AgentSubsystem->CollectReplicationSnapshot(OriginWorld, RelevancyRadiusUU, SnapshotBytes);
	if (SnapshotBytes.Num() == 0)
	{
		return; // nothing within relevancy range this broadcast
	}

	MulticastAgentSnapshot(OriginWorld, SnapshotBytes);
}

void AVoxelAgentReplicator::MulticastAgentSnapshot_Implementation(FVector InOriginWorld, const TArray<uint8>& SnapshotBytes)
{
	if (HasAuthority())
	{
		// The server's own Agents pool is already authoritative -- this body
		// only matters for remote (client) instances, same reasoning as
		// AVoxelEditRelay::MulticastAppliedEntries_Implementation.
		return;
	}
	UWorld* World = GetWorld();
	UVoxelAgentSubsystem* AgentSubsystem = World ? World->GetSubsystem<UVoxelAgentSubsystem>() : nullptr;
	if (!AgentSubsystem)
	{
		return;
	}
	AgentSubsystem->ApplyReplicatedAgentSnapshot(InOriginWorld, SnapshotBytes);
}
