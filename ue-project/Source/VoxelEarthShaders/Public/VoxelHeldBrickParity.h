#pragma once
#include "CoreMinimal.h"
#include "VoxelBrickPool.h"

enum class EVoxelHeldBrickParityStatus : uint8 { Pending, Passed, Failed, Cancelled };
struct FVoxelHeldBrickParityResult
{
    uint64 OwnershipGeneration=0;
    uint32 Mismatches=0;
    FIntVector FirstMismatch=FIntVector(-1);
    uint8 CpuMaterial=0, GpuMaterial=0;
    FString Error;
};

// Private validation only: no pool admission or terrain publication. One globally
// admitted readback, including cancelled work awaiting GPU retirement. GT API.
class VOXELEARTHSHADERS_API FVoxelHeldBrickParity
{
public:
    FVoxelHeldBrickParity() = default;
    ~FVoxelHeldBrickParity();
    FVoxelHeldBrickParity(const FVoxelHeldBrickParity&)=delete;
    FVoxelHeldBrickParity& operator=(const FVoxelHeldBrickParity&)=delete;
    bool Begin(const FVoxelBrickCpuPackRef& Cpu, const FVoxelGpuBrickPayloadRef& Gpu,
        const FIntVector& ExpectedOrigin, uint64 OwnershipGeneration, FString& OutError);
    EVoxelHeldBrickParityStatus Poll(FVoxelHeldBrickParityResult& Out) const;
    void Cancel();
    // Shutdown only. Enqueues a render-thread GPU wait and retirement; caller's
    // final FlushRenderingCommands must follow. Ordinary Cancel never waits.
    static void DrainForShutdown();
    static bool IsRetirementPending();
    // Bounded validation/semantic comparison, also used after GPU readback.
    static bool ComparePacks(const FVoxelBrickCpuPack& Cpu, const FVoxelBrickCpuPack& Gpu,
        uint32 GpuOccBase, uint32 GpuMatBase, FVoxelHeldBrickParityResult& Out);
private:
    struct FState;
    TSharedPtr<FState, ESPMode::ThreadSafe> State;
    static TSharedPtr<FState, ESPMode::ThreadSafe> Active;
};
