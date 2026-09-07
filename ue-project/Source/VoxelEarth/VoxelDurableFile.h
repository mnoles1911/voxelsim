#pragma once
#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace VoxelDurableFile
{
inline bool Publish(const FString& From,const FString& To,bool Replace)
{
#if PLATFORM_WINDOWS
    auto Native=[](const FString& Path)
    {
        FString Full=FPaths::ConvertRelativePathToFull(Path);
        FPaths::CollapseRelativeDirectories(Full);
        FPaths::MakePlatformFilename(Full);
        if(Full.StartsWith(TEXT("\\\\?\\"))) return Full;
        return Full.StartsWith(TEXT("\\\\"))?TEXT("\\\\?\\UNC\\")+Full.Mid(2):TEXT("\\\\?\\")+Full;
    };
    return ::MoveFileExW(*Native(From),*Native(To),MOVEFILE_WRITE_THROUGH|(Replace?MOVEFILE_REPLACE_EXISTING:0))!=0;
#else
    return FPlatformFileManager::Get().GetPlatformFile().MoveFile(*To,*From);
#endif
}
}
