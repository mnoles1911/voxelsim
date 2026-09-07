#pragma once
#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "VoxelPlantMeshComponent.generated.h"

// Fixed WPO envelope, independent of the size of an individual edit section.
UCLASS()
class UVoxelPlantMeshComponent : public UProceduralMeshComponent
{
    GENERATED_BODY()
public:
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override
    {
        FBoxSphereBounds ExpandedBounds = Super::CalcBounds(LocalToWorld);
        ExpandedBounds.BoxExtent += FVector(30.0);
        ExpandedBounds.SphereRadius = ExpandedBounds.BoxExtent.Size();
        return ExpandedBounds;
    }
};
