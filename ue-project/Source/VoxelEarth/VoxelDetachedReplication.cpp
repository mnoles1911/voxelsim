#include "VoxelDetachedReplication.h"
#include "VoxelDetachedPersistence.h"
#include "VoxelEnvironmentAsset.h"
#include "VoxelReplicaMotion.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelEarth.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/Crc.h"
#include "Misc/Compression.h"
#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
namespace VoxelDetachedNetworkFixture { void Tick(UWorld* World,float Delta); }

namespace VoxelDetachedWire
{
bool FAssembly::Add(FGuid NewId,uint64 NewRevision,int32 NewTotal,uint32 NewChecksum,int32 Offset,const TArray<uint8>& Chunk)
{
    if(!NewId.IsValid()||!NewRevision||NewTotal<=0||NewTotal>MaxSnapshotBytes||Chunk.IsEmpty()||
       Chunk.Num()>ChunkBytes||Offset<0||int64(Offset)+Chunk.Num()>NewTotal)return false;
    if(Offset==0){
        if(!Data.IsEmpty())return false;
        Id=NewId;Revision=NewRevision;Total=NewTotal;Checksum=NewChecksum;
    } else if(Id!=NewId||Revision!=NewRevision||Total!=NewTotal||Checksum!=NewChecksum)return false;
    if(Offset!=Data.Num())return false;
    Data.Append(Chunk);return true;
}
bool FAssembly::Complete() const
{return Total>0&&Data.Num()==Total&&FCrc::MemCrc32(Data.GetData(),Data.Num())==Checksum;}
void FAssembly::Reset(){Id.Invalidate();Revision=0;Total=0;Checksum=0;Data.Reset();}
}
namespace
{
enum class EMessage:uint8 { Reset,Geometry,Motion,Evict,Remove };
constexpr uint8 Protocol=1;
constexpr double InterestCm=25000.;
constexpr int32 MaxRawBytes=512*1024*1024;
TAutoConsoleVariable<float> CVarNetMiB(TEXT("voxel.Objects.NetMiBPerSecond"),2.f,
    TEXT("Detached geometry bandwidth per connection; paced reliable chunks, also limited by acknowledgement RTT."));
struct FEncoded {TArray<uint8> Bytes;int32 RawBytes=0;uint32 Checksum=0;};
struct FEncodeJob {TFuture<FEncoded> Result;};
struct FDecoded {bool Valid=false;VoxelDetachedPersistence::FSnapshot Entries;};
struct FStamp{uint64 Revision=0,Geometry=0;};
struct FPeer
{
    FGuid Epoch=FGuid::NewGuid();
    uint32 Sent=0,Acked=0;
    bool ResetSent=false;
    double Budget=0.;
    int32 Cursor=0,Offset=0,MotionCursor=0;
    TMap<FGuid,FStamp> Known;
    TMap<FGuid,uint64> Failed;
    TArray<uint8> Transfer;
    TSharedPtr<FEncodeJob> Encoding;
    int32 RawBytes=0;
    VoxelObjects::FEntry Entry;
    uint32 Checksum=0;
    FGuid PendingId;
    uint64 PendingRevision=0;
    uint64 PendingGeometry=0;
};
// No arrays on the motion path: mutable physics state never retransmits mesh
// buffers. Every value is checked before touching the replica registry.
void Motion(FArchive& Ar,VoxelObjects::FEntry& E)
{
    Ar<<E.Id<<E.Revision<<E.GeometryRevision<<E.Kind<<E.Transform<<E.Velocity<<E.AngularVelocity<<E.BoundsExtent;
    uint8 Life=uint8(E.Lifetime.Kind);Ar<<Life<<E.Lifetime.RemainingSeconds<<E.OwnerId<<E.bRetained<<E.bActivePhysics;
    E.Lifetime.Kind=EVoxelDebrisLifetime(Life);
}
bool Valid(const VoxelObjects::FEntry& E)
{
    return E.Id.IsValid()&&E.Revision>0&&E.Kind>=1&&E.Kind<=3&&(E.Kind!=3||VoxelEnvironmentAsset::IsSupportedTransform(E.Transform))&&E.Transform.IsValid()&&!E.Transform.ContainsNaN()&&
      !E.Velocity.ContainsNaN()&&!E.AngularVelocity.ContainsNaN()&&!E.BoundsExtent.ContainsNaN()&&
      FMath::IsFinite(E.Lifetime.RemainingSeconds)&&E.Lifetime.RemainingSeconds>=0&&
      uint8(E.Lifetime.Kind)<=uint8(EVoxelDebrisLifetime::Retained);
}
void DestroyReplica(VoxelObjects::FEntry& E)
{
    // Clear the binding BEFORE EndPlay so replacing/unloading a visual replica
    // cannot publish an authoritative tombstone for the new binding.
    AActor* Actor=E.Actor.Get();E.Actor.Reset();
    if(IsValid(Actor))Actor->Destroy();
}
bool ApplyGeometry(UWorld* W,VoxelObjects::FEntry E,AActor* Actor)
{
    const FGuid IncomingId=E.Id;
    auto& Registry=VoxelObjects::Get(W);auto Old=Registry.Find(E.Id);
    if(!Valid(E))return false;
    if(Old&&(Old->Residency==VoxelObjects::EResidency::Tombstone||Old->Revision>E.Revision))return false;
    if(!IsValid(Actor)||!VoxelDetachedPersistence::PublishRestoredObject(Actor,E))return false;
    Old=Registry.Find(E.Id);AActor* Previous=Old?Old->Actor.Get():nullptr;E.Actor=Actor;
    bool Accepted=true;
    if(Old&&Old->Revision==E.Revision){*Old=MoveTemp(E);Old->Residency=VoxelObjects::EResidency::Live;}
    else Accepted=Registry.Import(MoveTemp(E));
    if(Accepted){
        if(IsValid(Previous))Previous->Destroy();
        UE_LOG(LogVoxelEarth,Log,TEXT("DetachedNet installed id=%s pos=%s"),*IncomingId.ToString(),*Actor->GetActorLocation().ToString());
    }
    return Accepted;
}
}
struct FVoxelDetachedReplicationState
{
    struct FVisualMotion {TWeakObjectPtr<AActor> Actor;VoxelReplicaMotion::FBlend Blend;};
    TMap<FGuid,FVisualMotion> VisualMotion;
    FGuid RemoteEpoch;
    TSet<FGuid> RetiredEpochs;
    uint32 Received=0;
    TMap<TWeakObjectPtr<AVoxelEarthPlayerController>,FPeer> Peers;
    VoxelDetachedWire::FAssembly Assembly;
    TArray<VoxelObjects::FEntry> Snapshot;
    double ScanAge=1.;
    TSharedPtr<FEncodeJob> ActiveEncode;
    TUniquePtr<TFuture<FDecoded>> Decoding;
    int32 IncomingRawBytes=0;
    uint32 DecodeSequence=0;
    TWeakObjectPtr<AVoxelEarthPlayerController> DecodeController;
    TUniquePtr<VoxelObjects::FEntry> WaitingRestore, LatestMotion;
    FGuid IncomingId;
    uint64 RestoreHandle=0;
};
namespace
{
void ApplyReplicaMotion(FVoxelDetachedReplicationState& State,UWorld* World,const VoxelObjects::FEntry& Entry)
{
    auto Actor=Entry.Actor.Get();if(!IsValid(Actor))return;
    if(VoxelReplicaMotion::ShouldBlend(Entry.Kind,Actor->GetActorTransform(),Entry.Transform)){
        auto& Visual=State.VisualMotion.FindOrAdd(Entry.Id);
        Visual.Actor=Actor;
        Visual.Blend={Actor->GetActorTransform(),Entry.Transform,World->GetTimeSeconds(),.1};
    }else{
        State.VisualMotion.Remove(Entry.Id);
        Actor->SetActorTransform(Entry.Transform,false,nullptr,ETeleportType::TeleportPhysics);
    }
    if(auto Life=Actor->FindComponentByClass<UVoxelDebrisLifecycle>())Life->RestoreState(Entry.Lifetime);
}
void TickReplicaMotion(FVoxelDetachedReplicationState& State,UWorld* World)
{
    const auto Registry=VoxelObjects::Find(World);const double Now=World->GetTimeSeconds();
    for(auto It=State.VisualMotion.CreateIterator();It;++It){
        const auto Entry=Registry?Registry->Find(It.Key()):nullptr;auto Actor=It.Value().Actor.Get();
        if(!Entry||Entry->Residency!=VoxelObjects::EResidency::Live||!IsValid(Actor)||Entry->Actor.Get()!=Actor){It.RemoveCurrent();continue;}
        Actor->SetActorTransform(It.Value().Blend.Sample(Now),false,nullptr,ETeleportType::TeleportPhysics);
        if(It.Value().Blend.Complete(Now))It.RemoveCurrent();
    }
}
}
void UVoxelDetachedReplication::Initialize(FSubsystemCollectionBase& Collection)
{Super::Initialize(Collection);State=MakeShared<FVoxelDetachedReplicationState>();}
void UVoxelDetachedReplication::Deinitialize(){if(State){State->RemoteEpoch.Invalidate();if(State->RestoreHandle)VoxelDetachedPersistence::CancelRestoreObject(State->RestoreHandle);}State.Reset();Super::Deinitialize();}
bool UVoxelDetachedReplication::DoesSupportWorldType(EWorldType::Type Type) const
{return Type==EWorldType::Game||Type==EWorldType::PIE;}
TStatId UVoxelDetachedReplication::GetStatId() const
{RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelDetachedReplication,STATGROUP_Tickables);}
void UVoxelDetachedReplication::Acknowledge(AVoxelEarthPlayerController* PC,uint32 Sequence,bool Accepted)
{
    if(!State||!PC||!PC->HasAuthority())return;
    auto Peer=State->Peers.Find(PC);
    if(!Peer||Sequence!=Peer->Sent||Sequence<=Peer->Acked)return;
    Peer->Acked=Sequence;
    if(!Accepted){
        if(Peer->PendingId.IsValid()){
            Peer->Failed.Add(Peer->PendingId,Peer->PendingGeometry);
        }
        Peer->Transfer.Reset();Peer->Offset=0;
        UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNet replica rejected packet %u for %s; this revision is withheld"),Sequence,*PC->GetName());
    }
}
void UVoxelDetachedReplication::Tick(float Delta)
{
    UWorld* W=GetWorld();if(!State||!W)return;
    VoxelDetachedNetworkFixture::Tick(W,Delta);
    if(W->GetNetMode()==NM_Client){
        TickReplicaMotion(*State,W);
        if(State->Decoding&&State->Decoding->IsReady()){
            auto Result=State->Decoding->Get();State->Decoding.Reset();
            if(Result.Valid&&Result.Entries.Num()==1)State->WaitingRestore=MakeUnique<VoxelObjects::FEntry>(MoveTemp(Result.Entries[0]));
            else {
                State->Received=State->DecodeSequence;
                if(auto PC=State->DecodeController.Get())PC->ServerAcknowledgeDetachedPacket(State->DecodeSequence,false);
                UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNet async snapshot decode failed"));
            }
        }
        if(State->WaitingRestore&&!State->RestoreHandle&&!W->bIsTearingDown){
            const auto Incoming=*State->WaitingRestore;const FGuid Epoch=State->RemoteEpoch;const uint32 Sequence=State->DecodeSequence;
            TWeakPtr<FVoxelDetachedReplicationState> Weak=State;TWeakObjectPtr<UWorld> World=W;
            auto Completion=[Weak,World,Incoming,Epoch,Sequence](AActor* Actor){
                auto S=Weak.Pin();auto CurrentWorld=World.Get();if(!S||!CurrentWorld||CurrentWorld->bIsTearingDown||S->RemoteEpoch!=Epoch)return false;
                S->RestoreHandle=0;S->WaitingRestore.Reset();
                auto& Registry=VoxelObjects::Get(CurrentWorld);auto Existing=Registry.Find(Incoming.Id);
                const bool Obsolete=Existing&&(Existing->Residency==VoxelObjects::EResidency::Tombstone||Existing->GeometryRevision>Incoming.GeometryRevision);
                auto Current=Incoming;
                auto Merge=[&](const VoxelObjects::FEntry* Motion){if(Motion&&Motion->GeometryRevision==Current.GeometryRevision&&Motion->Revision>Current.Revision){auto Geometry=Current.Geometry;auto Dynamic=MoveTemp(Current.Dynamic);const uint32 Format=Current.GeometryFormat;Current=*Motion;Current.Geometry=MoveTemp(Geometry);Current.Dynamic=MoveTemp(Dynamic);Current.GeometryFormat=Format;Current.Actor.Reset();}};
                Merge(Existing);Merge(S->LatestMotion.Get());
                const bool Published=!Obsolete&&IsValid(Actor)&&ApplyGeometry(CurrentWorld,MoveTemp(Current),Actor);
                S->Received=Sequence;S->IncomingId.Invalidate();S->LatestMotion.Reset();
                if(auto PC=S->DecodeController.Get())PC->ServerAcknowledgeDetachedPacket(Sequence,Published||Obsolete);
                if(!Published&&!Obsolete)UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNet staged replica publication failed"));
                return Published;
            };
            // A full restore queue is temporary: retain the decoded snapshot
            // and retry next tick without rejecting its authoritative revision.
            State->RestoreHandle=VoxelDetachedPersistence::BeginRestoreObject(W,Incoming,MoveTemp(Completion),[Weak,World,Epoch,Id=Incoming.Id,Geometry=Incoming.GeometryRevision](){
                auto S=Weak.Pin();if(!S||!World.IsValid()||World->bIsTearingDown||S->RemoteEpoch!=Epoch)return false;
                auto Registry=VoxelObjects::Find(World.Get());auto Existing=Registry?Registry->Find(Id):nullptr;
                return !Existing||(Existing->Residency!=VoxelObjects::EResidency::Tombstone&&Existing->GeometryRevision<=Geometry);
            });
        }
        return;
    }
    if(W->GetNetMode()==NM_Standalone)return;
    if(State->ActiveEncode&&State->ActiveEncode->Result.IsReady())State->ActiveEncode.Reset();
    State->ScanAge+=Delta;const bool NewSnapshot=State->ScanAge>=.1;
    if(NewSnapshot){
        VoxelDetachedPersistence::RefreshObjects(W);
        State->Snapshot=VoxelObjects::Get(W).Snapshot();State->ScanAge=0.;
    }
    for(auto It=State->Peers.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
    for(auto It=W->GetPlayerControllerIterator();It;++It){
        auto PC=Cast<AVoxelEarthPlayerController>(It->Get());
        if(!PC||PC->IsLocalController()||!PC->GetNetConnection())continue;
        auto& P=State->Peers.FindOrAdd(PC);
        if(NewSnapshot&&P.ResetSent){
            TArray<uint8> Moving;FMemoryWriter MW(Moving);uint8 V=Protocol;FGuid Epoch=P.Epoch;
            MW<<V<<Epoch;const int64 CountOffset=MW.Tell();uint8 N=0;MW<<N;
            const int32 Count=State->Snapshot.Num();
            for(int32 I=0;I<FMath::Min(Count,128)&&N<32;++I){
                P.MotionCursor%=Count;auto E=State->Snapshot[P.MotionCursor++];auto Known=P.Known.Find(E.Id);
                if(!Known||Known->Revision>=E.Revision||Known->Geometry!=E.GeometryRevision)continue;
                uint8 Removed=E.Residency==VoxelObjects::EResidency::Tombstone?1:0;MW<<Removed;Motion(MW,E);++N;
            }
            if(N){MW.Seek(CountOffset);MW<<N;PC->ClientReceiveDetachedMotion(Moving);}
        }
        const double BytesPerSecond=FMath::Clamp(double(CVarNetMiB.GetValueOnGameThread()),.01,32.)*1024.*1024.;
        P.Budget=FMath::Min(double(VoxelDetachedWire::ChunkBytes+1024),P.Budget+FMath::Max(0.f,Delta)*BytesPerSecond);
        if(P.Encoding){
            if(!P.Encoding->Result.IsReady())continue;
            auto Encoded=P.Encoding->Result.Get();
            if(State->ActiveEncode==P.Encoding)State->ActiveEncode.Reset();P.Encoding.Reset();
            if(Encoded.Bytes.IsEmpty()){
                P.Failed.Add(P.Entry.Id,P.Entry.GeometryRevision);
                UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNet async encoding failed: %s"),*P.Entry.Id.ToString());continue;
            }
            P.Transfer=MoveTemp(Encoded.Bytes);P.RawBytes=Encoded.RawBytes;P.Checksum=Encoded.Checksum;P.Offset=0;
        }
        if(P.Sent!=P.Acked||P.Budget<VoxelDetachedWire::ChunkBytes+512)continue;
        TArray<uint8> Packet;FMemoryWriter Ar(Packet);
        uint8 Version=Protocol;uint32 Sequence=P.Sent+1;FGuid Epoch=P.Epoch;
        Ar<<Version<<Sequence<<Epoch;
        const int64 HeaderBytes=Ar.Tell();
        auto Tag=[&](EMessage Message){uint8 Type=uint8(Message);Ar<<Type;};
        P.PendingId.Invalidate();P.PendingRevision=0;P.PendingGeometry=0;
        if(!P.ResetSent){Tag(EMessage::Reset);P.ResetSent=true;}
        else {
            // If an object is deleted while its snapshot is being transmitted,
            // finish no more geometry: publish its tombstone immediately.
            if(!P.Transfer.IsEmpty()){
                const auto Current=VoxelObjects::Get(W).Find(P.Entry.Id);
                if(!Current||Current->Residency==VoxelObjects::EResidency::Tombstone){
                    P.Transfer.Reset();P.Offset=0;
                    auto E=Current?*Current:P.Entry;E.Revision=FMath::Max(E.Revision,P.Entry.Revision+1);
                    Tag(EMessage::Remove);Motion(Ar,E);P.Known.Remove(E.Id);P.PendingId=E.Id;P.PendingRevision=E.Revision;
                }
            }
            if(Ar.Tell()==HeaderBytes){ // header only: choose one bounded state operation
                if(P.Transfer.IsEmpty()){
                    const APawn* Pawn=PC->GetPawn();if(!Pawn)continue;
                    bool Found=false;
                    const int32 Count=State->Snapshot.Num();
                    for(int32 I=0;I<FMath::Min(Count,128);++I){
                        P.Cursor%=Count;auto E=State->Snapshot[P.Cursor++];
                        auto Known=P.Known.Find(E.Id);
                        const bool Removed=E.Residency==VoxelObjects::EResidency::Tombstone;
                        const FBox Bounds(E.Transform.GetLocation()-E.BoundsExtent,E.Transform.GetLocation()+E.BoundsExtent);
                        const bool Relevant=!Removed&&Bounds.ComputeSquaredDistanceToPoint(Pawn->GetActorLocation())<=FMath::Square(InterestCm);
                        if(Relevant)if(auto Failed=P.Failed.Find(E.Id))if(*Failed>=E.GeometryRevision)continue;
                        if(!Relevant){
                            if(!Known)continue;
                            Tag(Removed?EMessage::Remove:EMessage::Evict);Motion(Ar,E);P.Known.Remove(E.Id);Found=true;
                        } else if(!Known||Known->Geometry!=E.GeometryRevision){
                            if(!E.Geometry&&!E.Page.IsValid())continue;
                            if(State->ActiveEncode.IsValid())continue;
                            P.Entry=E;E.Actor.Reset();P.Encoding=MakeShared<FEncodeJob>();State->ActiveEncode=P.Encoding;
                            P.Encoding->Result=Async(EAsyncExecution::ThreadPool,[E=MoveTemp(E)]() mutable {
                                FEncoded Out;VoxelDetachedPersistence::FSnapshot One;One.Add(MoveTemp(E));TArray<uint8> Raw;
                                if(!VoxelDetachedPersistence::EncodeSnapshot(One,Raw)||Raw.IsEmpty()||Raw.Num()>MaxRawBytes)return Out;
                                Out.RawBytes=Raw.Num();int32 Size=FCompression::CompressMemoryBound(NAME_Zlib,Raw.Num());
                                Out.Bytes.SetNumUninitialized(Size);
                                if(!FCompression::CompressMemory(NAME_Zlib,Out.Bytes.GetData(),Size,Raw.GetData(),Raw.Num())||Size>VoxelDetachedWire::MaxSnapshotBytes){Out.Bytes.Reset();return Out;}
                                Out.Bytes.SetNum(Size);Out.Checksum=FCrc::MemCrc32(Out.Bytes.GetData(),Size);return Out;
                            });
                            break;
                        } else if(Known->Revision<E.Revision){
                            Tag(EMessage::Motion);Motion(Ar,E);Known->Revision=E.Revision;Found=true;
                        }
                        if(Found){P.PendingId=E.Id;P.PendingRevision=E.Revision;P.PendingGeometry=E.GeometryRevision;break;}
                    }
                    if(!Found)continue;
                }
                if(!P.Transfer.IsEmpty()){
                    Tag(EMessage::Geometry);auto E=P.Entry;
                    int32 Total=P.Transfer.Num(),Offset=P.Offset;
                    int32 N=FMath::Min(VoxelDetachedWire::ChunkBytes,Total-Offset);
                    Ar<<E.Id<<E.Revision<<Total<<P.RawBytes<<P.Checksum<<Offset<<N;Ar.Serialize(P.Transfer.GetData()+Offset,N);
                    P.PendingId=E.Id;P.PendingRevision=E.Revision;P.PendingGeometry=E.GeometryRevision;P.Offset+=N;
                    if(P.Offset==Total){P.Known.Add(E.Id,{E.Revision,E.GeometryRevision});P.Transfer.Reset();P.Offset=0;}
                }
            }
        }
        if(Ar.IsError()||Packet.Num()>VoxelDetachedWire::ChunkBytes+512)continue;
        P.Sent=Sequence;P.Budget-=Packet.Num();PC->ClientReceiveDetachedPacket(Packet);
    }
}

void UVoxelDetachedReplication::ReceiveMotion(AVoxelEarthPlayerController* PC,const TArray<uint8>& Packet)
{
    UWorld* W=GetWorld();if(!State||!W||W->GetNetMode()!=NM_Client||!PC||!PC->IsLocalController()||Packet.Num()>VoxelDetachedWire::ChunkBytes+512)return;
    FMemoryReader Ar(Packet);uint8 Version=0,N=0;FGuid Epoch;Ar<<Version<<Epoch<<N;
    if(Ar.IsError()||Version!=Protocol||Epoch!=State->RemoteEpoch||N>32)return;
    TArray<VoxelObjects::FEntry> Updates;TArray<uint8> Removed;
    for(uint8 I=0;I<N;++I){uint8 Gone=0;VoxelObjects::FEntry E;Ar<<Gone;Motion(Ar,E);if(Ar.IsError()||Gone>1||!Valid(E))return;Removed.Add(Gone);Updates.Add(MoveTemp(E));}
    if(Ar.Tell()!=Ar.TotalSize())return;
    auto& R=VoxelObjects::Get(W);
    for(int32 I=0;I<Updates.Num();++I){
        auto E=MoveTemp(Updates[I]);auto Old=R.Find(E.Id);
        if(E.Id==State->IncomingId){
            if(!State->LatestMotion||State->LatestMotion->Revision<E.Revision)State->LatestMotion=MakeUnique<VoxelObjects::FEntry>(E);
            if(Removed[I]&&!Old){E.Residency=VoxelObjects::EResidency::Tombstone;R.Import(MoveTemp(E));continue;}
        }
        if(!Old||Old->Residency==VoxelObjects::EResidency::Tombstone||Old->Revision>=E.Revision)continue;
        if(Removed[I]){DestroyReplica(*Old);E.Residency=VoxelObjects::EResidency::Tombstone;R.Import(MoveTemp(E));}
        else if(Old->GeometryRevision==E.GeometryRevision){
            E.Actor=Old->Actor;E.Geometry=Old->Geometry;E.Dynamic=Old->Dynamic;E.GeometryFormat=Old->GeometryFormat;E.Residency=Old->Residency;
            ApplyReplicaMotion(*State,W,E);
            R.Import(MoveTemp(E));
        }
    }
}

void UVoxelDetachedReplication::Receive(AVoxelEarthPlayerController* PC,const TArray<uint8>& Packet)
{
    UWorld* W=GetWorld();if(!State||!W||W->GetNetMode()!=NM_Client||!PC||!PC->IsLocalController()||Packet.Num()>VoxelDetachedWire::ChunkBytes+512)return;
    FMemoryReader Ar(Packet);uint8 Version=0,Type=255;uint32 Sequence=0;FGuid Epoch;
    Ar<<Version<<Sequence<<Epoch<<Type;
    if(Ar.IsError()||Version!=Protocol||!Epoch.IsValid()||Type>uint8(EMessage::Remove))return;
    const EMessage Message=EMessage(Type);
    if(Message==EMessage::Reset){
        if(Sequence!=1||State->RetiredEpochs.Contains(Epoch)||Ar.Tell()!=Ar.TotalSize())return;
        if(State->RemoteEpoch==Epoch){PC->ServerAcknowledgeDetachedPacket(Sequence,true);return;}
        if(State->RemoteEpoch.IsValid())State->RetiredEpochs.Add(State->RemoteEpoch);
        // Cancel currently invokes completion(nullptr); invalidate its captured
        // epoch before cancellation so it cannot ACK an old-world sequence.
        State->RemoteEpoch=Epoch;
        if(State->RestoreHandle)VoxelDetachedPersistence::CancelRestoreObject(State->RestoreHandle);
        State->RestoreHandle=0;State->WaitingRestore.Reset();State->LatestMotion.Reset();State->IncomingId.Invalidate();
        if(auto R=VoxelObjects::Find(W))for(auto E:R->Snapshot())if(auto Current=R->Find(E.Id))DestroyReplica(*Current);
        VoxelObjects::Forget(W);State->VisualMotion.Reset();State->Assembly.Reset();State->Decoding.Reset();State->DecodeController.Reset();
        State->RemoteEpoch=Epoch;State->Received=Sequence;
        PC->ServerAcknowledgeDetachedPacket(Sequence,true);return;
    }
    if(Epoch!=State->RemoteEpoch)return;
    if(Sequence<=State->Received){PC->ServerAcknowledgeDetachedPacket(Sequence,true);return;}
    if(Sequence!=State->Received+1)return;
    if(Sequence==State->DecodeSequence&&(State->WaitingRestore||State->RestoreHandle))return;
    auto& Registry=VoxelObjects::Get(W);bool Accepted=false;
    if(Message==EMessage::Geometry){
        FGuid Id;uint64 Revision=0;int32 Total=0,RawBytes=0,Offset=0,N=0;uint32 Checksum=0;
        Ar<<Id<<Revision<<Total<<RawBytes<<Checksum<<Offset<<N;
        if(!Ar.IsError()&&!State->Decoding&&RawBytes>0&&RawBytes<=MaxRawBytes&&N>0&&N<=VoxelDetachedWire::ChunkBytes&&N==Ar.TotalSize()-Ar.Tell()&&
           (Offset==0||RawBytes==State->IncomingRawBytes)){
            if(Offset==0){State->IncomingRawBytes=RawBytes;State->IncomingId=Id;State->LatestMotion.Reset();}
            TArray<uint8> Chunk;Chunk.SetNumUninitialized(N);Ar.Serialize(Chunk.GetData(),N);
            Accepted=State->Assembly.Add(Id,Revision,Total,Checksum,Offset,Chunk);
            if(Accepted&&State->Assembly.Data.Num()==Total){
                auto Compressed=MoveTemp(State->Assembly.Data);
                State->DecodeSequence=Sequence;State->DecodeController=PC;
                State->Decoding=MakeUnique<TFuture<FDecoded>>(Async(EAsyncExecution::ThreadPool,
                  [Compressed=MoveTemp(Compressed),RawBytes,Checksum,Id,Revision]() mutable {
                    FDecoded Result;if(FCrc::MemCrc32(Compressed.GetData(),Compressed.Num())!=Checksum)return Result;
                    TArray<uint8> Raw;Raw.SetNumUninitialized(RawBytes);
                    if(!FCompression::UncompressMemory(NAME_Zlib,Raw.GetData(),RawBytes,Compressed.GetData(),Compressed.Num()))return Result;
                    Result.Valid=VoxelDetachedPersistence::DecodeSnapshot(Raw,Result.Entries)&&Result.Entries.Num()==1&&
                        Result.Entries[0].Id==Id&&Result.Entries[0].Revision==Revision;
                    return Result;
                }));
                State->Assembly.Reset();
                return; // Final acknowledgement waits for worker decode + actor installation.
            }
        }
    } else {
        VoxelObjects::FEntry E;Motion(Ar,E);
        if(!Ar.IsError()&&Ar.Tell()==Ar.TotalSize()&&Valid(E)){
            auto Old=Registry.Find(E.Id);Accepted=true;
            if(Message==EMessage::Remove){
                State->Assembly.Reset();
                if(!Old||Old->Revision<E.Revision){
                    if(Old)DestroyReplica(*Old);E.Residency=VoxelObjects::EResidency::Tombstone;
                    Registry.Import(MoveTemp(E));
                }
            } else if(Message==EMessage::Evict){
                if(Old&&Old->Revision<=E.Revision){DestroyReplica(*Old);Old->Residency=VoxelObjects::EResidency::Dormant;}
            } else if(Old&&Old->Residency!=VoxelObjects::EResidency::Tombstone&&Old->Revision<E.Revision&&Old->GeometryRevision==E.GeometryRevision){
                E.Actor=Old->Actor;E.Geometry=Old->Geometry;E.Dynamic=Old->Dynamic;E.GeometryFormat=Old->GeometryFormat;E.Residency=Old->Residency;
                ApplyReplicaMotion(*State,W,E);
                Registry.Import(MoveTemp(E));
            }
        }
    }
    if(!Accepted){State->Assembly.Reset();UE_LOG(LogVoxelEarth,Error,TEXT("DetachedNet rejected packet %u; no further chunks of this revision will be sent"),Sequence);}
    State->Received=Sequence;PC->ServerAcknowledgeDetachedPacket(Sequence,Accepted);
}
