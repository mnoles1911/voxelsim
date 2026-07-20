#include "VoxelEditRelay.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "VoxelEarth.h"
#include "VoxelWorldSubsystem.h"

AVoxelEditRelay::AVoxelEditRelay()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true; // every client needs the broadcast stream, not just those "near" it -- there is no near
	SetReplicatingMovement(false);
}

void AVoxelEditRelay::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AVoxelEditRelay, ServerSeed);
	DOREPLIFETIME(AVoxelEditRelay, ServerWorldGenVersion);
	DOREPLIFETIME(AVoxelEditRelay, ServerProbeDigest);
}

void AVoxelEditRelay::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogVoxelEarth, Error, TEXT("AVoxelEditRelay::BeginPlay: no UVoxelWorldSubsystem -- handshake cannot proceed."));
		return;
	}

	if (HasAuthority())
	{
		// docs/m3-plan.md "Determinism guard": publish this run's seed +
		// worldgen version + fixed-probe digest for every joining client to
		// compare against. Simply setting these UPROPERTY(Replicated) fields
		// is enough -- UE pushes them to clients automatically.
		ServerSeed = Subsystem->GetSeed();
		ServerWorldGenVersion = Subsystem->GetWorldGenVersion();
		ServerProbeDigest = Subsystem->ComputeHandshakeDigest();
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelEditRelay: authority handshake state seed=%llu wgen=%u digest=0x%016llX"),
		       (unsigned long long)ServerSeed, ServerWorldGenVersion, (unsigned long long)ServerProbeDigest);
		return;
	}

	// Client: this actor's replicated properties are guaranteed populated by
	// the time BeginPlay runs (initial actor-channel replication applies
	// every property before invoking BeginPlay) -- safe to compare right
	// here rather than waiting on a RepNotify.
	const uint64 LocalSeed = Subsystem->GetSeed();
	const uint32 LocalWorldGenVersion = Subsystem->GetWorldGenVersion();
	const uint64 LocalDigest = Subsystem->ComputeHandshakeDigest();

	if (LocalSeed != ServerSeed || LocalWorldGenVersion != ServerWorldGenVersion || LocalDigest != ServerProbeDigest)
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("VoxelEditRelay: HANDSHAKE MISMATCH -- local(seed=%llu wgen=%u digest=0x%016llX) != ")
		       TEXT("server(seed=%llu wgen=%u digest=0x%016llX). This client's terrain would silently diverge from the ")
		       TEXT("authoritative World -- disconnecting."),
		       (unsigned long long)LocalSeed, LocalWorldGenVersion, (unsigned long long)LocalDigest, (unsigned long long)ServerSeed,
		       ServerWorldGenVersion, (unsigned long long)ServerProbeDigest);
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			PC->ConsoleCommand(TEXT("disconnect"));
		}
		return;
	}

	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelEditRelay: handshake OK (seed=%llu wgen=%u digest=0x%016llX)"), (unsigned long long)LocalSeed,
	       LocalWorldGenVersion, (unsigned long long)LocalDigest);
}

void AVoxelEditRelay::MulticastAppliedEntries_Implementation(const TArray<uint8>& EntryBytes)
{
	if (HasAuthority())
	{
		// The server already applied these entries directly (that's how
		// they got into the log to broadcast in the first place); this
		// multicast body only matters for remote (client) instances.
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}
	Subsystem->ReceiveLiveEntries(EntryBytes);
}
