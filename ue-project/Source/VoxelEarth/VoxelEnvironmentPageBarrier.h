#pragma once
#include "CoreMinimal.h"
#include "VoxelCoords.h"

namespace VoxelEnvironmentPages
{
using FKey = VoxelCoords::FVoxelLevelChunkKey;
enum class EProducer : uint8 { Cpu, Gpu };
class FBarrier;

// Copy tokens into worker input/results; only the owning game-thread ledger mutates.
// Valid means nonempty, not necessarily still active. Retire/Release validate ownership.
class FLease
{
public:
    bool IsValid() const { return Serial != 0; }
private:
    friend class FBarrier;
    FGuid Owner;
    uint64 Serial = 0;
};
class FTicket
{
public:
    bool IsValid() const { return Serial != 0; }
private:
    friend class FBarrier;
    FGuid Owner;
    uint64 Serial = 0;
};

// This is a publication-lifetime ledger, NOT a task-running counter. A lease
// survives worker completion, result enqueue, chunk unload, and record removal.
// Retire only after final application/discard (and any earlier GPU publication
// has drained). Callers must enumerate the COMPLETE touched page set themselves.
// Freezing prevents new ordinary work, not old GPU publication or renderer reads.
class FBarrier
{
public:
    explicit FBarrier(int32 MaxFrozenKeys = 8192, int32 MaxLeases = 65536, int32 MaxTickets = 8);
    FBarrier(const FBarrier&) = delete;
    FBarrier& operator=(const FBarrier&) = delete;
    FLease TryAcquire(const FKey& Key, EProducer Producer);
    bool Retire(const FLease& Lease);
    // Empty, duplicate, overlapping, invalid or over-capacity requests change nothing.
    FTicket TryFreeze(const TArray<FKey>& CompleteKeys);
    bool IsQuiescent(const FTicket& Ticket) const;
    bool IsFrozen(const FKey& Key) const;
    // Cancellation and successful handoff both release the freeze. Active leases
    // are never cancelled by this operation; stale tokens cannot affect retries.
    bool Release(const FTicket& Ticket);
    int32 ActiveLeases() const;
    int32 FrozenKeys() const;
private:
    bool Owns(const FTicket& Ticket) const;
    struct FWork { FKey Key; EProducer Producer; };
    const FGuid Identity;
    const int32 KeyLimit, LeaseLimit, TicketLimit;
    uint64 NextLease = 1, NextTicket = 1;
    TMap<uint64, FWork> Work;
    TMap<FKey, int32> Counts;
    TMap<FKey, uint64> Frozen;
    TMap<uint64, TArray<FKey>> Tickets;
};
}
