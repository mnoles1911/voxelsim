#pragma once
#include "CoreMinimal.h"

// GT builds a packet once; only its position in the render command stream makes
// it visible. Never replace a queued packet with a newer mutable mailbox value.
struct FVoxelMarchPreparedCredit;

namespace VoxelMarchOrdered
{
struct FPacket
{
    uint64 Epoch=0, BaseGeneration=0, Generation=0;
    TSharedPtr<FVoxelMarchPreparedCredit,ESPMode::ThreadSafe> PreparedCredit;
    TArray<uint32> Full, Pairs, Occupied, AnyAbsent, AllSky;
    bool Delta=false, GpuResident=false, Verify=false, GpuCompatible=true;
    int32 MaxDeltaCells=0;
    uint64 Hash=0;
    TArray<uint32> GpuEntries;
    int32 GpuRemoves=0, GpuAdds=0;
    uint64 Bytes() const { return (uint64(Full.Num())+uint64(Pairs.Num())+uint64(Occupied.Num())+uint64(AnyAbsent.Num())+uint64(AllSky.Num())+uint64(GpuEntries.Num()))*sizeof(uint32); }
};
struct FState
{
    uint64 Epoch=1, Generation=0;
    TArray<uint32> Cells, Occupied, AnyAbsent, AllSky;
    TSet<uint32> DirtyCells;
    bool FullPending=false, DeltaPending=false, Verify=false;
    uint64 Hash=0;
    // RT only; payload shape is validated before mutating any part of the state.
    // Selective consumption: only Full and the three coarse arrays are moved.
    // Pairs, scalar policy/hash and GpuEntries remain valid for the caller's
    // inline GPU publication after installation; no whole-Packet move occurs.
    bool Apply(FPacket&& P, int32 CellCount, int32 BlockWords)
    {
        if(P.Epoch==0 || P.Generation==0 || P.Epoch!=Epoch || P.BaseGeneration!=Generation || P.Generation!=Generation+1 ||
           P.Occupied.Num()!=BlockWords || P.AnyAbsent.Num()!=BlockWords || P.AllSky.Num()!=BlockWords ||
           (P.Pairs.Num()%2)!=0 || (!P.Full.IsEmpty() && P.Full.Num()!=CellCount) ||
           (P.Full.IsEmpty() && Cells.Num()!=CellCount)) return false;
        for(int32 I=0;I<P.Pairs.Num();I+=2) if(P.Pairs[I]>=uint32(CellCount))return false;
        if(!P.Full.IsEmpty()) { Cells=MoveTemp(P.Full); DirtyCells.Reset(); FullPending=true; }
        for(int32 I=0;I<P.Pairs.Num();I+=2) { const uint32 C=P.Pairs[I]; Cells[C]=P.Pairs[I+1]; if(!FullPending)DirtyCells.Add(C); }
        Occupied=MoveTemp(P.Occupied); AnyAbsent=MoveTemp(P.AnyAbsent); AllSky=MoveTemp(P.AllSky);
        Generation=P.Generation; Verify=P.Verify; Hash=P.Hash;
        if(!P.Delta || DirtyCells.Num()>P.MaxDeltaCells) { FullPending=true; DirtyCells.Reset(); }
        DeltaPending=!FullPending && !DirtyCells.IsEmpty();
        return true;
    }
    void Reset(uint64 NewEpoch)
    {
        Epoch=NewEpoch; Generation=0; Cells.Reset(); Occupied.Reset(); AnyAbsent.Reset(); AllSky.Reset();
        DirtyCells.Reset(); FullPending=false; DeltaPending=false; Verify=false; Hash=0;
    }
};
}
