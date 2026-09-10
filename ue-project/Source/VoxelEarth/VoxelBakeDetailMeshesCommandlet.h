#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "VoxelBakeDetailMeshesCommandlet.generated.h"

UCLASS()
class UVoxelBakeDetailMeshesCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UVoxelBakeDetailMeshesCommandlet();
    virtual int32 Main(const FString& Params) override;
};
