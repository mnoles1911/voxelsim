#pragma once
#include "CoreMinimal.h"
class UWorld;

// A launch request survives map replacement, scoped to its game instance.
// The source world retains its slot until the final checkpoint is captured.
namespace VoxelSessionTravel
{
enum class EAction : uint8 { Menu, NewGame, Load };
struct FRequest
{
    EAction Action=EAction::Menu;
    uint64 Seed=0;
    FString Slug;
};
VOXELEARTH_API bool Queue(UWorld* World,const FRequest& Request);
VOXELEARTH_API bool Peek(const UWorld* World,FRequest& Request);
VOXELEARTH_API bool Take(const UWorld* World,FRequest& Request);
}
