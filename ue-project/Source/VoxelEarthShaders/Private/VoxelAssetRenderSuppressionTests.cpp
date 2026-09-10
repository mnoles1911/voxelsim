#include "VoxelGpuWorldGenGraph.h"
#include "VoxelGpuWorldGen.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetRenderSuppressionAdmissionTest,
    "Voxel.Objects.AssetRenderSuppressionAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FVoxelAssetRenderSuppressionAdmissionTest::RunTest(const FString& Parameters)
{
    FVoxelGpuRegionRequest Request;
    Request.DispatchColumns = FUintVector2(64, 64);
    Request.BricksZ = 32; // exactly 1 Mi cells / 4 MiB of optional claim scratch
    Request.bMeshChain = false;
    Request.RasterSize = FUintVector2(1, 1);
    Request.ElevationMm.Add(0);
    Request.ClimatePacked.Add(0);
    auto& Instance = Request.AssetInstances.AddDefaulted_GetRef();
    Instance.SizeX = Instance.SizeY = Instance.SizeZ = 1;
    Request.AssetColStarts = {0u, 1u};
    Request.AssetSpans = {256u | 16u};
    FString Error;
    TestEqual(TEXT("new requests preserve terrain rendering"), Instance.SuppressTerrainRender, 0u);
    TestTrue(TEXT("default request accepted"), VoxelGpuWorldGen::ValidateRegionRequest(Request, Error));
    Instance.SuppressTerrainRender = 1;
    TestTrue(TEXT("scratch boundary accepted"), VoxelGpuWorldGen::ValidateRegionRequest(Request, Error));
    Instance.SuppressTerrainRender = 2;
    TestFalse(TEXT("unknown marker rejected"), VoxelGpuWorldGen::ValidateRegionRequest(Request, Error));
    Instance.SuppressTerrainRender = 1;
    Request.BricksZ = 33;
    TestFalse(TEXT("scratch overflow rejected"), VoxelGpuWorldGen::ValidateRegionRequest(Request, Error));
    Request.DispatchColumns = FUintVector2(0xfffffff8u, 0xfffffff8u);
    Request.BricksZ = 0xffffffffu;
    TestFalse(TEXT("budget arithmetic cannot wrap"), VoxelGpuWorldGen::ValidateRegionRequest(Request, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetRenderSuppressionGpuTest,
    "Voxel.Objects.AssetRenderSuppressionGpu",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FVoxelAssetRenderSuppressionGpuTest::RunTest(const FString& Parameters)
{
    if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
    {
        AddWarning(TEXT("GPU winner suppression requires SM6; no GPU result established on this RHI"));
        return true;
    }
    // Real classic GPU readback, both level-zero scatter and coarse gather.
    // Two overlapping 8-cube banks use different material IDs. A promoted
    // first winner must leave air, while the second bank's exposed tail stays.
    for (int32 Level = 0; Level <= 1; ++Level)
    {
        FVoxelGpuRegionRequest Request;
        Request.DispatchColumns = FUintVector2(16, 8);
        Request.BricksZ = 1;
        Request.BrickZMin = 10000;
        Request.CoarseLevel = Level;
        Request.bMeshChain = false;
        Request.RasterSize = FUintVector2(1, 1);
        Request.ElevationMm.Add(0);
        Request.ClimatePacked.Add(0);
        const int32 Scale = 1 << Level;
        for (uint32 Bank = 0; Bank < 2; ++Bank)
        {
            auto& Instance = Request.AssetInstances.AddDefaulted_GetRef();
            Instance.AnchorRelVx = int32(Bank) * 4;
            Instance.AnchorVz = Request.BrickZMin * 8 * Scale;
            Instance.SizeX = Instance.SizeY = Instance.SizeZ = 8;
            Instance.ColStartsBase = uint32(Request.AssetColStarts.Num());
            for (uint32 Column = 0; Column < 64; ++Column)
            {
                Request.AssetColStarts.Add(uint32(Request.AssetSpans.Num()));
                Request.AssetSpans.Add((8u << 8) | (16u + Bank));
            }
            Request.AssetColStarts.Add(uint32(Request.AssetSpans.Num()));
        }
        for (int32 Owned = -1; Owned < 2; ++Owned)
        {
            Request.AssetInstances[0].SuppressTerrainRender = Owned == 0 ? 1u : 0u;
            Request.AssetInstances[1].SuppressTerrainRender = Owned == 1 ? 1u : 0u;
            const auto Result = VoxelGpuWorldGen::RunRegionBlocking(Request);
            if (!TestTrue(*Result.Error, Result.bOk)) return false;
            if (!TestEqual(TEXT("full region returned"), Result.Cells.Num(), 1024)) return false;
            for (int32 X = 0; X < 16; ++X)
            for (int32 Y = 0; Y < 8; ++Y)
            for (int32 Z = 0; Z < 8; ++Z)
            {
                const int32 Rx = X * Scale + Scale / 2;
                const int32 Ry = Y * Scale + Scale / 2;
                const int32 Rz = Z * Scale + Scale / 2;
                int32 Winner = -1;
                if (Ry < 8 && Rz < 8)
                {
                    if (Rx < 8) Winner = 0;
                    else if (Rx < 12) Winner = 1;
                }
                const uint32 Expected = Winner < 0 || Winner == Owned ? 0u : uint32(16 + Winner);
                const int32 Index = (X / 8) * 512 + (X % 8) + 8 * (Y + 8 * Z);
                if (!TestEqual(TEXT("canonical first winner is suppressed without revealing overlaps"),
                               Result.Cells[Index] & 255u, Expected)) return false;
            }
        }
    }
    return true;
}
#endif
