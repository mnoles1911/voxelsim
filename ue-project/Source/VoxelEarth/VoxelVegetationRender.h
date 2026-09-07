#pragma once
#include "CoreMinimal.h"

// Presentation metadata only. The asset/world material IDs and collision stay
// authoritative. Alpha is NOT opacity: see Tools/vegetation_material_common.py.
namespace VoxelVegetationRender
{
inline bool IsLeaf(uint8 M) { return (M >= 19 && M <= 22) || M == 24 || M == 25; }
inline bool IsWood(uint8 M) { return M == 16 || M == 17 || M == 18 || M == 23; }
inline float MaterialClass(uint8 M)
{
    if (IsLeaf(M)) return .5f;
    if (IsWood(M)) return .75f;
    // Grass/reeds use grass IDs; petals use the shared appearance palette.
    // This metadata is only emitted by environment mesh paths, never creatures.
    if (M == 8 || M == 10 || M >= 26) return .25f;
    return 1.f;
}
inline FVector2f WindData(float LocalZ, float HeightUU)
{
    // Root plane is the asset origin Z=0, not its cropped grid minimum.
    return FVector2f(FMath::Clamp(LocalZ / FMath::Max(HeightUU, 1.f), 0.f, 1.f), HeightUU);
}
}
