#include "VoxelSessionTravel.h"
#include "VoxelSaveLibrary.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"

namespace VoxelSessionTravel
{
namespace Detail
{
struct FPending { FRequest Request; TWeakObjectPtr<const UWorld> Source; };
TMap<TWeakObjectPtr<UGameInstance>,FPending> Pending;
void Prune()
{
    for(auto It=Pending.CreateIterator();It;++It) if(!It.Key().IsValid()) It.RemoveCurrent();
}
}
bool Queue(UWorld* World,const FRequest& Request)
{
    check(IsInGameThread());
    if(!World || !World->GetGameInstance() || World->GetNetMode()==NM_Client ||
        (Request.Action==EAction::Load && !VoxelSave::IsValidSlug(Request.Slug))) return false;
    const FString Map=UGameplayStatics::GetCurrentLevelName(World,true);
    if(Map.IsEmpty()) return false;
    Detail::Prune();
    if(Detail::Pending.Contains(World->GetGameInstance())) return false;
    Detail::FPending Pending; Pending.Request=Request; Pending.Source=World;
    Detail::Pending.Add(World->GetGameInstance(),MoveTemp(Pending));
    // Non-seamless replacement disconnects remote clients, so old baselines and
    // actor channels cannot mutate the restored session. They may reconnect.
    UGameplayStatics::OpenLevel(World,FName(*Map),true,
        World->GetNetMode()==NM_ListenServer?TEXT("listen"):TEXT(""));
    return true;
}
bool Peek(const UWorld* World,FRequest& Request)
{
    check(IsInGameThread()); Detail::Prune();
    const auto Pending=World?Detail::Pending.Find(World->GetGameInstance()):nullptr;
    if(!Pending || Pending->Source.Get()==World) return false;
    Request=Pending->Request; return true;
}
bool Take(const UWorld* World,FRequest& Request)
{
    if(!Peek(World,Request)) return false;
    Detail::Pending.Remove(World->GetGameInstance()); return true;
}
}
