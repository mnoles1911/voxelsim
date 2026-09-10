#include "VoxelEnvironmentPageBarrier.h"

namespace VoxelEnvironmentPages
{
FBarrier::FBarrier(int32 MaxFrozenKeys, int32 MaxLeases, int32 MaxTickets)
    : Identity(FGuid::NewGuid()), KeyLimit(FMath::Max(0, MaxFrozenKeys)),
      LeaseLimit(FMath::Max(0, MaxLeases)), TicketLimit(FMath::Max(0, MaxTickets))
{ check(IsInGameThread()); }

FLease FBarrier::TryAcquire(const FKey& Key, EProducer Producer)
{
    check(IsInGameThread());
    if (Key.Level < 0 || Key.Level >= VoxelCoords::kNumLevels ||
        (Producer != EProducer::Cpu && Producer != EProducer::Gpu) ||
        Frozen.Contains(Key) || Work.Num() >= LeaseLimit || NextLease == MAX_uint64) return {};
    FLease Lease; Lease.Owner = Identity; Lease.Serial = NextLease++;
    Work.Add(Lease.Serial, FWork{Key, Producer});
    ++Counts.FindOrAdd(Key);
    return Lease;
}
bool FBarrier::Retire(const FLease& Lease)
{
    check(IsInGameThread());
    if (Lease.Owner != Identity || !Lease.Serial) return false;
    const auto Entry = Work.Find(Lease.Serial);
    if (!Entry) return false;
    const FKey Key = Entry->Key;
    auto Count = Counts.Find(Key); check(Count && *Count > 0);
    if (--*Count == 0) Counts.Remove(Key);
    Work.Remove(Lease.Serial);
    return true;
}
FTicket FBarrier::TryFreeze(const TArray<FKey>& CompleteKeys)
{
    check(IsInGameThread());
    if (CompleteKeys.IsEmpty() || CompleteKeys.Num() > KeyLimit - Frozen.Num() ||
        Tickets.Num() >= TicketLimit || NextTicket == MAX_uint64) return {};
    TSet<FKey> Unique;
    for (const FKey& Key : CompleteKeys) {
        if (Key.Level < 0 || Key.Level >= VoxelCoords::kNumLevels ||
            Frozen.Contains(Key) || Unique.Contains(Key)) return {};
        Unique.Add(Key);
    }
    FTicket Ticket; Ticket.Owner = Identity; Ticket.Serial = NextTicket++;
    Tickets.Add(Ticket.Serial, CompleteKeys);
    for (const FKey& Key : CompleteKeys) Frozen.Add(Key, Ticket.Serial);
    return Ticket;
}
bool FBarrier::Owns(const FTicket& Ticket) const
{ return Ticket.Owner == Identity && Ticket.Serial && Tickets.Contains(Ticket.Serial); }
bool FBarrier::IsQuiescent(const FTicket& Ticket) const
{
    check(IsInGameThread());
    if (!Owns(Ticket)) return false;
    for (const FKey& Key : Tickets.FindChecked(Ticket.Serial)) if (Counts.Contains(Key)) return false;
    return true;
}
bool FBarrier::IsFrozen(const FKey& Key) const
{ check(IsInGameThread()); return Frozen.Contains(Key); }
bool FBarrier::Release(const FTicket& Ticket)
{
    check(IsInGameThread());
    if (!Owns(Ticket)) return false;
    for (const FKey& Key : Tickets.FindChecked(Ticket.Serial)) Frozen.Remove(Key);
    Tickets.Remove(Ticket.Serial);
    return true;
}
int32 FBarrier::ActiveLeases() const { check(IsInGameThread()); return Work.Num(); }
int32 FBarrier::FrozenKeys() const { check(IsInGameThread()); return Frozen.Num(); }
}
