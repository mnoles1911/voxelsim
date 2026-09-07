#pragma once
#include "CoreMinimal.h"

// Pure L1/L2 surface-lighting derivation. No world/RHI/actor access. Operations
// retain the marcher's existing float expression order and fallback constants.
namespace VoxelSurfaceLighting
{
struct FVectors
{
    FVector4f AmbientSkyAndGround=FVector4f(0,0,0,0);
    FVector4f SunDirAndWrapFloor=FVector4f(0,0,1,-1);
    FVector4f WrapColorAndSkyBoost=FVector4f(0,0,0,0);
    bool AssetDiagnosticEnabled=false;
};
inline float DuskFade(float SunZ)
{
    const float T=FMath::Clamp((SunZ+0.02f)/0.17f,0.0f,1.0f);
    return T*T*(3.0f-2.0f*T);
}
inline FVector4f Ambient(float RawIntensity,float RawGroundMix,float RawWrapFloor,
    float Engagement,const FVector3f& Tint)
{
    float Intensity=FMath::Max(RawIntensity,0.0f);
    const float Floor=FMath::Clamp(RawWrapFloor,0.0f,0.95f);
    Intensity*=1.0f-Floor*Engagement;
    return FVector4f(Tint.X*Intensity,Tint.Y*Intensity,Tint.Z*Intensity,FMath::Clamp(RawGroundMix,0.0f,1.0f));
}
inline FVectors Wrap(bool Enabled,bool SunPublished,const FVector3f& Direction,
    float RawFloor,float RawSkyBoost,float RawGain,const FVector3f& Tint)
{
    FVectors Out;
    if(!Enabled||!SunPublished)return Out;
    const float Floor=FMath::Clamp(RawFloor,0.0f,0.95f);
    const float SkyBoost=FMath::Clamp(RawSkyBoost,0.0f,2.0f);
    const float Gain=FMath::Max(RawGain,0.0f)*DuskFade(Direction.Z);
    Out.SunDirAndWrapFloor=FVector4f(Direction.X,Direction.Y,Direction.Z,Floor);
    Out.WrapColorAndSkyBoost=FVector4f(Gain*Tint.X,Gain*Tint.Y,Gain*Tint.Z,SkyBoost);
    return Out;
}
inline float SunDeficit(const FVector3f& Normal,const FVector4f& DirectionAndFloor,float SkyBoost)
{
    if(DirectionAndFloor.W<0)return 0;
    const float Dot=Normal.X*DirectionAndFloor.X+Normal.Y*DirectionAndFloor.Y+Normal.Z*DirectionAndFloor.Z;
    const float Term=FMath::Max(FMath::Max(DirectionAndFloor.W,0.5f+0.5f*Dot),Normal.Z*SkyBoost);
    return FMath::Max(Term-FMath::Clamp(Dot,0.0f,1.0f),0.0f);
}
}
// GT snapshot of the same published sun/colours and cvars used by the marcher.
// AssetDiagnosticEnabled stays false unless explicitly opted in AND both volume
// modifiers are disabled. L3 propagation/GI texture bindings are not implemented.
VOXELEARTHSHADERS_API VoxelSurfaceLighting::FVectors VoxelSurfaceLightingSnapshot_GameThread();
