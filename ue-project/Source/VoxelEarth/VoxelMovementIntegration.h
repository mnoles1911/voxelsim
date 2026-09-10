#pragma once
#include "CoreMinimal.h"

namespace VoxelMovement
{
// Bound work, never discard elapsed time. Ordinary frames and the engine's
// 400ms hitch cap use steps no larger than 1/120s.
inline int32 IntegrationSteps(double Seconds)
{
    return FMath::Clamp(FMath::CeilToInt(FMath::Min(Seconds, 256.0 / 120.0) * 120.0), 1, 256);
}
inline double GravityDisplacement(double Velocity, double Gravity, double Seconds)
{
    return Velocity * Seconds - .5 * Gravity * Seconds * Seconds;
}
}
