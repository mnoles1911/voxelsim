#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelDetachedReplication.generated.h"

class AVoxelEarthPlayerController;
struct FVoxelDetachedReplicationState;

// One immutable snapshot transfer per connection. Reliable delivery is paced
// and application-acknowledged so late-join geometry cannot fill UE's reliable
// queue. Physics, fracture, expiry and topology stay on the server.
UCLASS()
class VOXELEARTH_API UVoxelDetachedReplication : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
    void Receive(AVoxelEarthPlayerController* PC,const TArray<uint8>& Bytes);
    void ReceiveMotion(AVoxelEarthPlayerController* PC,const TArray<uint8>& Bytes);
    void Acknowledge(AVoxelEarthPlayerController* PC,uint32 Sequence,bool Accepted);
private:
    TSharedPtr<FVoxelDetachedReplicationState> State;
};

namespace VoxelDetachedWire
{
constexpr int32 ChunkBytes=16*1024;
constexpr int32 MaxSnapshotBytes=64*1024*1024;
// Ordered chunks, exact lengths and checksum before any actor mutation. A
// rejected fragment leaves the current assembly untouched.
struct FAssembly
{
    FGuid Id;
    uint64 Revision=0;
    int32 Total=0;
    uint32 Checksum=0;
    TArray<uint8> Data;
    bool Add(FGuid IncomingId,uint64 IncomingRevision,int32 IncomingTotal,uint32 IncomingChecksum,
             int32 Offset,const TArray<uint8>& Chunk);
    bool Complete() const;
    void Reset();
};
}
