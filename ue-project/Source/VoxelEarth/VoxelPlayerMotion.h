#pragma once
#include "CoreMinimal.h"
struct FVoxelPlayerMotion
{
    FVector Velocity=FVector::ZeroVector;
    bool Walk=false,Crouched=false,JumpHeld=false;
    int32 WalkSpeed=0,FlySpeed=0;
    double GroundAge=0,JumpRemaining=0;
};
