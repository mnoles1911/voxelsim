#pragma once
#include "CoreMinimal.h"

// Presentation only: the registry always retains the newest server transform.
// No extrapolation or local physics is used for detached client replicas.
namespace VoxelReplicaMotion
{
struct FBlend
{
    FTransform From, To;
    double Start = 0., Duration = .1;
    FTransform Sample(double Now) const
    {
        const double Alpha = Duration > 0. ? FMath::Clamp((Now-Start)/Duration,0.,1.) : 1.;
        FTransform Result;
        Result.Blend(From,To,Alpha);
        return Result;
    }
    bool Complete(double Now) const { return Duration<=0. || Now>=Start+Duration; }
};
// Standing environment assets must retain exact quarter-yaw lattice transforms.
// Large corrections are teleports, not a sweep through intervening terrain.
inline bool ShouldBlend(uint8 Kind,const FTransform& From,const FTransform& To)
{
    return Kind!=3 && From.IsValid() && To.IsValid() &&
        From.GetScale3D().Equals(To.GetScale3D(),1.e-6) &&
        FVector::DistSquared(From.GetLocation(),To.GetLocation())<=FMath::Square(1000.);
}
}
