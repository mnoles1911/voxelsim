#include "VoxelGpuWorldGen.h"
#include "VoxelGpuWorklist.h"
#include "VoxelRasterAtlasGpu.h"
#include "Misc/AutomationTest.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr uint32 kSuppressionFixtureCapacity = 8;
constexpr uint32 kSuppressionFixtureCells = 32768;

FVoxelGpuRegionRequest SuppressionWorklistRequest(int32 Level, int32 Owned)
{
    FVoxelGpuRegionRequest Request;
    Request.DispatchColumns = FUintVector2(32, 32);
    Request.BricksZ = 4;
    Request.BrickZMin = 10000;
    Request.CoarseLevel = Level;
    Request.bMeshChain = false;
    Request.RasterSize = FUintVector2(1, 1);
    Request.ElevationMm.Add(0);
    Request.ClimatePacked.Add(0);
    for (uint32 Bank = 0; Bank < 2; ++Bank)
    {
        auto& Instance = Request.AssetInstances.AddDefaulted_GetRef();
        Instance.AnchorRelVx = 4 + int32(Bank) * 4;
        Instance.AnchorRelVy = 4;
        Instance.AnchorVz = Request.BrickZMin * 8 * (1 << Level);
        Instance.SizeX = Instance.SizeY = 8;
        Instance.SizeZ = 32; // exercises the mask's top bit at level zero
        Instance.SuppressTerrainRender = Owned == int32(Bank) ? 1u : 0u;
        Instance.ColStartsBase = uint32(Request.AssetColStarts.Num());
        for (uint32 Column = 0; Column < 64; ++Column)
        {
            Request.AssetColStarts.Add(uint32(Request.AssetSpans.Num()));
            Request.AssetSpans.Add((32u << 8) | (16u + Bank));
        }
        Request.AssetColStarts.Add(uint32(Request.AssetSpans.Num()));
    }
    return Request;
}

FVoxelWorklistAssetPayload SuppressionWorklistPayload(const FVoxelGpuRegionRequest& Request)
{
    FVoxelWorklistAssetPayload Payload;
    Payload.ColStarts = Request.AssetColStarts;
    Payload.Spans = Request.AssetSpans;
    for (const auto& Source : Request.AssetInstances)
    {
        auto& Dest = Payload.Instances.AddDefaulted_GetRef();
        Dest.AnchorRelVx = Source.AnchorRelVx;
        Dest.AnchorRelVy = Source.AnchorRelVy;
        Dest.AnchorVz = Source.AnchorVz;
        Dest.GridOriginZ = Source.GridOriginZ;
        Dest.RotOriginX = Source.RotOriginX;
        Dest.RotOriginY = Source.RotOriginY;
        Dest.YawQuarter = Source.YawQuarter;
        Dest.SizeX = Source.SizeX;
        Dest.SizeY = Source.SizeY;
        Dest.SizeZ = Source.SizeZ;
        Dest.ColStartsBase = Source.ColStartsBase;
        Dest.SuppressTerrainRender = Source.SuppressTerrainRender;
    }
    return Payload;
}

struct FSuppressionWorklistReadback
{
    TArray<uint32> Cells;
    TArray<FVoxelGpuChunkWorkRecord> Records;
    uint32 AtlasMisses = 0;
    FString Error;
};

struct FSuppressionWorklistFixture
{
    FVoxelRasterAtlasGpu Atlas;
    TUniquePtr<FVoxelGpuWorklist> Worklist;
    uint32 NextGeneration = 1;

    FSuppressionWorklistFixture()
    {
        // 128 KiB of owned raster payload. Four-by-four pages cover [-1920,1920)m,
        // including the flat fixture's terrain sampling and cavern lookups.
        Atlas.Init(32, 4);
        FVoxelRasterAtlasGpuDelta Delta;
        Delta.bClearMissStats = true;
        for (int32 Py = -2; Py < 2; ++Py)
        for (int32 Px = -2; Px < 2; ++Px)
        {
            const uint32 Slot = uint32((Py + 4) % 4) * 4 + uint32((Px + 4) % 4);
            const uint32 Tag = (uint32(Py + 32768) << 16) | uint32(Px + 32768);
            Delta.PageMeta.Append({Slot, Tag, uint32(Delta.StagedElevationMm.Num())});
            Delta.StagedElevationMm.AddZeroed(1024);
            Delta.StagedClimatePacked.AddZeroed(1024);
        }
        Atlas.EnqueueUpsert(MoveTemp(Delta));
        Worklist = MakeUnique<FVoxelGpuWorklist>();
        Worklist->Init(kSuppressionFixtureCapacity);
        Worklist->SetColumnStageInputs(&Atlas, 0, 30000);
        Worklist->SetVoxelizeStageArmed(true, 3);
        Worklist->SetAssetStampStageArmed(true);
    }

    ~FSuppressionWorklistFixture()
    {
        FlushRenderingCommands();
        Worklist.Reset();
        ENQUEUE_RENDER_COMMAND(SuppressionFixtureReleaseAtlas)([this](FRHICommandListImmediate&)
        {
            Atlas.ReleaseResources_RenderThread();
        });
        FlushRenderingCommands();
    }

    bool Append(int32 Level, const TArray<int32>& Owners, TArray<uint32>& Indices)
    {
        TArray<FVoxelGpuChunkWorkRecord> Records;
        TArray<FVoxelWorklistAssetPayload> Payloads;
        for (int32 Owned : Owners)
        {
            const auto Request = SuppressionWorklistRequest(Level, Owned);
            auto& Record = Records.AddDefaulted_GetRef();
            Record.OriginVx = Request.OriginVx;
            Record.OriginVy = Request.OriginVy;
            Record.BrickZMin = Request.BrickZMin;
            Record.GenId = NextGeneration++; // consume proof rejects generation zero
            Record.LevelFlags = uint32(Level) | (1u << 8);
            Record.AssetCount = uint32(Request.AssetInstances.Num());
            Payloads.Add(SuppressionWorklistPayload(Request));
        }
        return Worklist->Append(Records, &Indices, &Payloads) == Owners.Num();
    }

    FSuppressionWorklistReadback Read()
    {
        FSuppressionWorklistReadback Result;
        ENQUEUE_RENDER_COMMAND(SuppressionFixtureReadback)([this, &Result](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);
            const auto Columns = Worklist->RegisterColumnStage(GraphBuilder);
            const auto Ring = Worklist->Register(GraphBuilder);
            const auto Raster = Atlas.Register(GraphBuilder);
            if (!Columns.CellArena || !Ring.Records)
            {
                Result.Error = TEXT("Worklist cell arena or ring was not produced");
                GraphBuilder.Execute();
                return;
            }
            FRHIGPUBufferReadback CellsReadback(TEXT("Suppression.WorklistCells"));
            FRHIGPUBufferReadback RecordsReadback(TEXT("Suppression.WorklistRecords"));
            FRHIGPUBufferReadback MissReadback(TEXT("Suppression.AtlasMisses"));
            const uint32 CellBytes = 3 * kSuppressionFixtureCells * sizeof(uint32);
            const uint32 RecordBytes = kSuppressionFixtureCapacity * sizeof(FVoxelGpuChunkWorkRecord);
            AddEnqueueCopyPass(GraphBuilder, &CellsReadback, Columns.CellArena, CellBytes);
            AddEnqueueCopyPass(GraphBuilder, &RecordsReadback, Ring.Records->Desc.Buffer, RecordBytes);
            AddEnqueueCopyPass(GraphBuilder, &MissReadback, Raster.MissStats->Desc.Buffer, sizeof(uint32));
            GraphBuilder.Execute();
            RHICmdList.SubmitAndBlockUntilGPUIdle();
            const auto Copy = [&Result](FRHIGPUBufferReadback& Readback, void* Dest, uint32 Bytes)
            {
                if (!Readback.IsReady()) { Result.Error = TEXT("Readback not ready after GPU idle"); return; }
                const void* Source = Readback.Lock(Bytes);
                if (!Source) { Result.Error = TEXT("Readback lock failed"); return; }
                FMemory::Memcpy(Dest, Source, Bytes);
                Readback.Unlock();
            };
            Result.Cells.SetNumUninitialized(3 * kSuppressionFixtureCells);
            Result.Records.SetNumUninitialized(kSuppressionFixtureCapacity);
            Copy(CellsReadback, Result.Cells.GetData(), CellBytes);
            Copy(RecordsReadback, Result.Records.GetData(), RecordBytes);
            Copy(MissReadback, &Result.AtlasMisses, sizeof(uint32));
        });
        FlushRenderingCommands();
        return Result;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelAssetRenderSuppressionWorklistTest,
    "Voxel.Objects.AssetRenderSuppressionWorklist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FVoxelAssetRenderSuppressionWorklistTest::RunTest(const FString& Parameters)
{
    if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
    {
        AddWarning(TEXT("Worklist suppression requires SM6; no GPU parity established on this RHI"));
        return true;
    }
    // One production-style worklist lifetime also exercises ring wraparound
    // between levels; separate mailbox lifetimes have their own regression.
    FSuppressionWorklistFixture Fixture;
    for (int32 Level = 0; Level <= 1; ++Level)
    {
        const TArray<int32> Owners = {-1, 0, 1};
        TArray<FVoxelGpuRegionResult> Classic;
        for (int32 Owned : Owners)
        {
            Classic.Add(VoxelGpuWorldGen::RunRegionBlocking(SuppressionWorklistRequest(Level, Owned)));
            if (!TestTrue(*Classic.Last().Error, Classic.Last().bOk)) return false;
            if (!TestEqual(TEXT("classic reference covers one chunk"), Classic.Last().Cells.Num(), int32(kSuppressionFixtureCells))) return false;
        }
        const auto Compare = [this, Level, &Classic](const FSuppressionWorklistReadback& Data, int32 Slice, int32 OwnerIndex)
        {
            if (!TestTrue(*Data.Error, Data.Error.IsEmpty())) return false;
            if (!TestEqual(TEXT("fixture raster taps are resident"), Data.AtlasMisses, 0u)) return false;
            if (!TestEqual(TEXT("worklist arena has three bounded slices"), Data.Cells.Num(), int32(3 * kSuppressionFixtureCells))) return false;
            const int32 Owned = OwnerIndex - 1;
            const int32 Scale = 1 << Level;
            for (int32 X = 0; X < 32; ++X)
            for (int32 Y = 0; Y < 32; ++Y)
            for (int32 Z = 0; Z < 32; ++Z)
            {
                const int32 Rx = X * Scale + Scale / 2, Ry = Y * Scale + Scale / 2, Rz = Z * Scale + Scale / 2;
                int32 Winner = -1;
                if (Ry >= 4 && Ry < 12 && Rz < 32)
                {
                    if (Rx >= 4 && Rx < 12) Winner = 0;
                    else if (Rx >= 8 && Rx < 16) Winner = 1;
                }
                const uint32 Expected = Winner < 0 || Winner == Owned ? 0u : uint32(16 + Winner);
                const int32 Brick = ((X / 8) + 4 * (Y / 8)) * 4 + Z / 8;
                const int32 Index = Brick * 512 + X % 8 + 8 * (Y % 8 + 8 * (Z % 8));
                const uint32 Cell = Data.Cells[Slice * kSuppressionFixtureCells + Index];
                if (!TestEqual(TEXT("worklist agrees with analytic canonical winner"), Cell & 255u, Expected)) return false;
                if (!TestEqual(TEXT("all packed cell bits agree with classic GPU"), Cell, Classic[OwnerIndex].Cells[Index])) return false;
            }
            return true;
        };
        TArray<uint32> Initial;
        if (!TestTrue(TEXT("three ownership variants admitted"), Fixture.Append(Level, Owners, Initial))) return false;
        Fixture.Worklist->Flush(3);
        const auto InitialData = Fixture.Read();
        for (int32 Variant = 0; Variant < 3; ++Variant)
            if (!Compare(InitialData, Variant, Variant)) return false;

        // Deferred asset payloads intentionally lose stampsStaged and fall back
        // classic; they must never sample a later flush's unrelated payload blob.
        TArray<uint32> Limited;
        if (!TestTrue(TEXT("budget-limited variants admitted"), Fixture.Append(Level, Owners, Limited))) return false;
        Fixture.Worklist->Flush(1);
        const auto LimitedData = Fixture.Read();
        if (!Compare(LimitedData, 0, 0)) return false;
        for (int32 Variant = 0; Variant < 3; ++Variant)
        {
            const bool Staged = (LimitedData.Records[Limited[Variant] % kSuppressionFixtureCapacity].LevelFlags & (1u << 9)) != 0u;
            if (!TestEqual(TEXT("only same-flush consumed assets are staged"), Staged, Variant == 0)) return false;
        }
        Fixture.Worklist->Flush(2);
        const auto DeferredData = Fixture.Read();
        if (!TestTrue(*DeferredData.Error, DeferredData.Error.IsEmpty())) return false;
        if (!TestTrue(TEXT("deferred records cannot stamp stale arena data"), DeferredData.Cells == LimitedData.Cells)) return false;
        TArray<uint32> Resubmitted;
        if (!TestTrue(TEXT("fresh owned variants admitted"), Fixture.Append(Level, {0, 1}, Resubmitted))) return false;
        Fixture.Worklist->Flush(2);
        const auto ResubmittedData = Fixture.Read();
        if (!Compare(ResubmittedData, 0, 1) || !Compare(ResubmittedData, 1, 2)) return false;
    }
    // Read() waits for GPU idle. Two empty flushes first land the outstanding
    // readback on the render thread, then consume its proof on the game thread.
    Fixture.Worklist->Flush(0);
    FlushRenderingCommands();
    Fixture.Worklist->Flush(0);
    FlushRenderingCommands();
    const auto Proof = Fixture.Worklist->GetProofStatus();
    TestTrue(TEXT("production consume proof actually landed"), Proof.Landed > 0);
    TestEqual(TEXT("production consume fold matches host"), Proof.Failed, uint64(0));
    TestEqual(TEXT("fixture records satisfy production record contract"), Proof.MalformedOnGpu, uint64(0));
    return true;
}
#endif
