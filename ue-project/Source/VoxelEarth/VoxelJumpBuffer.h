#pragma once
#include <algorithm>

namespace VoxelMovement
{
// Input collected since the previous movement update has not spent that
// update's elapsed time waiting. Give it one consumption opportunity before
// aging it; older buffered input still expires normally during a hitch.
inline double AdvanceJumpBuffer(double Remaining, bool& Fresh, double DeltaSeconds)
{
    if (Fresh)
    {
        Fresh = false;
        return Remaining;
    }
    return std::max(0.0, Remaining - DeltaSeconds);
}
}
