#pragma once
#include "CoreMinimal.h"
// Compare one readback with the host work included in that exact flush.
namespace VoxelGpuClaimProof {
inline int64 Expected(int64 Staged,uint32 Deferred){return Staged-int64(Deferred);}
inline bool Dark(int64 Expected,uint32 Claimed){return Expected>0&&Claimed==0;}
inline bool Ahead(int64 Expected,uint32 Claimed){return int64(Claimed)>Expected;}
}
