// voxel.GPU.VerifyAsyncMesh — the gate on FVoxelGpuMeshJobManager (ADR-0006, G3
// stage 1).
//
// WHAT THIS PROVES, AND WHAT IT DELIBERATELY DOES NOT.
//
// The GPU mesher itself is already proven: voxel.GPU.VerifyRegion shows Unreal's
// compilation of the kernels is bit-exact against the CPU mesher, and
// voxel.GPU.SpawnPool shows the quads it emits are format-compatible with the
// pool. Neither of those says anything about the ASYNC runner, which is new code
// with new failure modes -- lifetime bugs in what the render command captured,
// a readback harvested before it landed, a rebase that lost a brick offset.
//
// So this meshes K render chunks through FVoxelGpuMeshJobManager, meshes the
// same K chunks with MeshChunkBricks (the actual shipping CPU mesher, not a
// transcription of it -- that is why it lives in VoxelChunkMesher.h now), packs
// the CPU output with PackVoxelChunkQuad, and BYTE-COMPARES the two uint64
// arrays. Same count, same order, same bits, or it fails and says which quad.
//
// It also reports the two numbers that decide whether the path is worth
// integrating: per-job dispatch-to-ready latency, and jobs completed per second
// at a given in-flight cap.
//
// Deliberately synthetic-sampler and headless: same seed and same sampler as
// voxel.GPU.VerifyRegion, so a failure here cannot be blamed on tile data.
//
// Usage:
//   voxel.GPU.VerifyAsyncMesh              16 chunks, 4 in flight
//   voxel.GPU.VerifyAsyncMesh 64 8         64 chunks, 8 in flight

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "VoxelChunkMesher.h"
#include "VoxelCoords.h"
#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuRegionBuild.h"
#include "VoxelGpuWorldGen.h"
#include "VoxelMeshTypes.h"

#include "voxelcore/amplifier.h"
#include "voxelcore/core.h"
#include "voxelcore/tiles.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpuAsync, Log, All);

namespace
{
	// Same fixture seed as voxel.GPU.VerifyRegion / the bench, so anything this
	// finds is about the runner and not about which world we are in.
	constexpr uint64 kSeed = 20260719;

	// The CPU reference for one render chunk, produced exactly the way a level-0
	// streaming job produces it (VoxelWorldSubsystem.cpp DispatchJobs): a
	// (32+2)^2 column grid, a sampler over it, MeshChunkBricks, no ring skirt.
	TArray<uint64> CpuMeshChunkPacked(const vxc::Amplifier& Amp,
	                                  const VoxelCoords::FVoxelChunkKey& Key)
	{
		constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
		constexpr int32 GridEdge = ChunkVox + 2;
		const int64 BaseVX = int64(Key.X) * ChunkVox;
		const int64 BaseVY = int64(Key.Y) * ChunkVox;

		TArray<vxc::ColumnSample> Columns;
		Columns.SetNumUninitialized(GridEdge * GridEdge);
		for (int32 LY = 0; LY < GridEdge; ++LY)
		{
			for (int32 LX = 0; LX < GridEdge; ++LX)
			{
				Columns[LX + GridEdge * LY] = Amp.column(BaseVX + LX - 1, BaseVY + LY - 1);
			}
		}

		const auto GridSampler = [&Columns, BaseVX, BaseVY](int64 X, int64 Y, int64 Z)
		{
			const int32 LX = int32(X - BaseVX) + 1;
			const int32 LY = int32(Y - BaseVY) + 1;
			check(LX >= 0 && LX < GridEdge && LY >= 0 && LY < GridEdge);
			return vxc::Amplifier::materialAt(Columns[LX + GridEdge * LY], Z);
		};

		TArray<FVoxelChunkQuad> Quads;
		MeshChunkBricks(Key, GridSampler, Quads);

		TArray<uint64> Packed;
		Packed.Reserve(Quads.Num());
		for (const FVoxelChunkQuad& Q : Quads)
		{
			Packed.Add(PackVoxelChunkQuad(Q));
		}
		return Packed;
	}

	// The chunk containing the surface at this chunk column's centre, so every
	// chunk under test actually has geometry in it. An all-air or all-solid chunk
	// meshes to zero quads on both sides and would pass vacuously.
	VoxelCoords::FVoxelChunkKey SurfaceChunkKey(const vxc::Amplifier& Amp, int32 Cx, int32 Cy)
	{
		constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
		const int64 Vx = int64(Cx) * ChunkVox + ChunkVox / 2;
		const int64 Vy = int64(Cy) * ChunkVox + ChunkVox / 2;
		const vxc::ColumnSample C = Amp.column(Vx, Vy);
		// Topmost solid voxel: its centre (vz*100 + 50) <= surfaceMm.
		const int64 Top = vxc::floorDiv(int64(C.surfaceMm) - vxc::kVoxelSizeMm / 2, vxc::kVoxelSizeMm);
		return VoxelCoords::FVoxelChunkKey{ Cx, Cy, int32(vxc::floorDiv(Top, int64(ChunkVox))) };
	}

	double Percentile(TArray<double> Values, double Fraction)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(int32(Fraction * double(Values.Num() - 1) + 0.5),
		                                 0, Values.Num() - 1);
		return Values[Index];
	}

	// One run of the harness. Lives past the console command that made it,
	// because the whole point is that the GPU path does NOT block: it is pumped
	// from FTSTicker until every job has reported back.
	//
	// The manager is declared LAST so that if this object is torn down early it
	// is destroyed FIRST -- its destructor delivers Cancelled for anything still
	// outstanding, and that callback needs the rest of this object alive.
	struct FAsyncMeshVerifyRun : public TSharedFromThis<FAsyncMeshVerifyRun>
	{
		struct FChunkUnderTest
		{
			VoxelCoords::FVoxelChunkKey Key;
			TArray<uint64> CpuQuads;
			bool bDelivered = false;
		};

		explicit FAsyncMeshVerifyRun(int32 InNumChunks, int32 InMaxInFlight)
			: Tiles(kSeed)
			, CpuTiles(kSeed)
			, CpuAmp(kSeed, CpuTiles)
			, NumChunks(InNumChunks)
			, MaxInFlight(InMaxInFlight)
			, Manager(FVoxelGpuMeshJobComplete::CreateRaw(this, &FAsyncMeshVerifyRun::OnJobComplete),
			          InMaxInFlight)
		{
		}

		void Start()
		{
			Chunks.SetNum(NumChunks);

			// A square-ish block of adjacent chunk columns around the fixture
			// origin, so neighbouring jobs share raster pixels the way streaming
			// jobs would.
			const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));

			const double CpuStart = FPlatformTime::Seconds();
			for (int32 I = 0; I < NumChunks; ++I)
			{
				const int32 Cx = (I % Side) - Side / 2;
				const int32 Cy = (I / Side) - Side / 2;
				Chunks[I].Key = SurfaceChunkKey(CpuAmp, Cx, Cy);
				Chunks[I].CpuQuads = CpuMeshChunkPacked(CpuAmp, Chunks[I].Key);
				TotalCpuQuads += Chunks[I].CpuQuads.Num();
			}
			CpuTotalMs = (FPlatformTime::Seconds() - CpuStart) * 1000.0;

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("voxel.GPU.VerifyAsyncMesh: %d chunks, %d in flight, seed %llu. "
			            "CPU reference: %d quads in %.1f ms (%.2f ms/chunk)."),
			       NumChunks, MaxInFlight, kSeed, TotalCpuQuads, CpuTotalMs,
			       CpuTotalMs / double(FMath::Max(1, NumChunks)));

			FirstSubmitSeconds = FPlatformTime::Seconds();
			for (int32 I = 0; I < NumChunks; ++I)
			{
				FVoxelGpuRegionRequest Req;
				VoxelGpuChunkRegion::SetChunkFootprint(Req, Chunks[I].Key.X, Chunks[I].Key.Y,
				                                       Chunks[I].Key.Z);
				Req.Seed = kSeed;
				// Sized from the dispatch footprint that was just set, by the same
				// code the blocking digest gate uses.
				VoxelGpuRegionBuild::FillRasterWindow(Req, Tiles);

				Manager.Submit(MoveTemp(Req), uint64(I));
			}
		}

		void OnJobComplete(FVoxelGpuMeshJobResult&& Result)
		{
			const int32 Index = int32(Result.UserTag);
			if (!Chunks.IsValidIndex(Index))
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("  job %llu delivered an unknown tag %llu"),
				       Result.JobId, Result.UserTag);
				++NumDelivered;
				return;
			}

			FChunkUnderTest& Chunk = Chunks[Index];
			if (Chunk.bDelivered)
			{
				// The "exactly one outcome" guarantee, checked rather than assumed.
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d (%d,%d,%d) delivered TWICE — the manager broke its own contract"),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z);
				++NumDoubleDelivered;
				return;
			}
			Chunk.bDelivered = true;
			++NumDelivered;
			LastDeliverSeconds = FPlatformTime::Seconds();

			if (!Result.IsOk())
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("  chunk %d (%d,%d,%d) FAILED: %s — %s"),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z,
				       LexToString(Result.Status), *Result.Error);
				++NumFailed;
				return;
			}

			ReadyLatenciesMs.Add(Result.DispatchToReadyMs);
			DeliverLatenciesMs.Add(Result.SubmitToDeliverMs);
			TotalGpuQuads += Result.Quads.Num();

			// --- the deliverable: a byte comparison ---------------------------
			if (Result.Quads.Num() != Chunk.CpuQuads.Num())
			{
				if (NumMismatched < kMaxReported)
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d (%d,%d,%d): quad COUNT differs — gpu %d, cpu %d"),
					       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z,
					       Result.Quads.Num(), Chunk.CpuQuads.Num());
				}
				++NumMismatched;
				return;
			}

			const int32 Bytes = Result.Quads.Num() * int32(sizeof(uint64));
			if (Bytes > 0 && FMemory::Memcmp(Result.Quads.GetData(), Chunk.CpuQuads.GetData(),
			                                 SIZE_T(Bytes)) != 0)
			{
				++NumMismatched;
				if (NumMismatched <= kMaxReported)
				{
					// Find and print the first differing quad, decoded, because
					// "the bytes differ" on its own says nothing about which field.
					for (int32 Q = 0; Q < Result.Quads.Num(); ++Q)
					{
						if (Result.Quads[Q] == Chunk.CpuQuads[Q])
						{
							continue;
						}
						const auto Describe = [](uint64 P)
						{
							const uint32 W0 = uint32(P & 0xffffffffull);
							const uint32 W1 = uint32(P >> 32);
							return FString::Printf(
								TEXT("ax%u d%u s%u u%u v%u w%u h%u ao%u m%u"),
								W0 & 0xfu, (W0 >> 4) & 0xfu, (W0 >> 8) & 0xffu,
								(W0 >> 16) & 0xffu, (W0 >> 24) & 0xffu,
								W1 & 0xffu, (W1 >> 8) & 0xffu,
								(W1 >> 16) & 0xffu, (W1 >> 24) & 0xffu);
						};
						UE_LOG(LogVoxelGpuAsync, Error,
						       TEXT("  chunk %d (%d,%d,%d): first differing quad is [%d] of %d — ")
						       TEXT("gpu %016llx (%s) vs cpu %016llx (%s)"),
						       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z, Q,
						       Result.Quads.Num(),
						       Result.Quads[Q], *Describe(Result.Quads[Q]),
						       Chunk.CpuQuads[Q], *Describe(Chunk.CpuQuads[Q]));
						break;
					}
				}
				return;
			}

			++NumMatched;
		}

		// Returns false to unregister the ticker.
		bool Tick(float)
		{
			Manager.Tick();

			++NumTicks;
			if (NumDelivered < NumChunks)
			{
				// The manager's own timeout is the backstop for a wedged GPU; this
				// one only catches "the manager stopped delivering", which would be
				// the bug this whole design exists to prevent.
				if (FPlatformTime::Seconds() - FirstSubmitSeconds > 120.0)
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("ABANDONING: %d of %d jobs never reported back after 120 s. "
					            "That is the lost-job failure mode."),
					       NumChunks - NumDelivered, NumChunks);
					Report();
					return false;
				}
				return true;
			}

			Report();
			return false;
		}

		void Report() const
		{
			const double WallSeconds = FMath::Max(1e-6, LastDeliverSeconds - FirstSubmitSeconds);
			const double Throughput = double(NumDelivered) / WallSeconds;

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  delivered %d/%d jobs in %d ticks (%.3f s wall)"),
			       NumDelivered, NumChunks, NumTicks, WallSeconds);
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  quads: gpu %d, cpu %d"), TotalGpuQuads, TotalCpuQuads);

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  dispatch->ready latency ms: min %.2f  p50 %.2f  p95 %.2f  max %.2f"),
			       Percentile(ReadyLatenciesMs, 0.0), Percentile(ReadyLatenciesMs, 0.5),
			       Percentile(ReadyLatenciesMs, 0.95), Percentile(ReadyLatenciesMs, 1.0));
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  submit->deliver latency ms: min %.2f  p50 %.2f  p95 %.2f  max %.2f"),
			       Percentile(DeliverLatenciesMs, 0.0), Percentile(DeliverLatenciesMs, 0.5),
			       Percentile(DeliverLatenciesMs, 0.95), Percentile(DeliverLatenciesMs, 1.0));
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  throughput: %.1f chunks/s at an in-flight cap of %d "
			            "(cpu reference meshed %.1f chunks/s single-threaded)"),
			       Throughput, MaxInFlight,
			       double(NumChunks) / FMath::Max(1e-6, CpuTotalMs / 1000.0));

			const bool bPass = NumFailed == 0 && NumMismatched == 0 && NumDoubleDelivered == 0
			                && NumDelivered == NumChunks && NumMatched == NumChunks;
			if (bPass)
			{
				UE_LOG(LogVoxelGpuAsync, Log,
				       TEXT("PASS: all %d chunks meshed through the async GPU path are BYTE-IDENTICAL "
				            "to MeshChunkBricks + PackVoxelChunkQuad."),
				       NumMatched);
			}
			else
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("FAIL: %d matched, %d differed, %d job failures, %d double deliveries, "
				            "%d of %d delivered."),
				       NumMatched, NumMismatched, NumFailed, NumDoubleDelivered,
				       NumDelivered, NumChunks);
			}
		}

		static constexpr int32 kMaxReported = 5;

		vxc::SyntheticTileSampler Tiles;
		// A second, independent sampler for the CPU reference, same as
		// voxel.GPU.VerifyRegion: sharing one would let a stateful bug in the
		// sampler cancel itself out on both sides of the comparison.
		vxc::SyntheticTileSampler CpuTiles;
		vxc::Amplifier CpuAmp;

		int32 NumChunks = 0;
		int32 MaxInFlight = 0;
		TArray<FChunkUnderTest> Chunks;

		int32 NumDelivered = 0;
		int32 NumMatched = 0;
		int32 NumMismatched = 0;
		int32 NumFailed = 0;
		int32 NumDoubleDelivered = 0;
		int32 NumTicks = 0;
		int32 TotalCpuQuads = 0;
		int32 TotalGpuQuads = 0;

		double CpuTotalMs = 0.0;
		double FirstSubmitSeconds = 0.0;
		double LastDeliverSeconds = 0.0;
		TArray<double> ReadyLatenciesMs;
		TArray<double> DeliverLatenciesMs;

		// Must be last: see the struct comment.
		FVoxelGpuMeshJobManager Manager;
	};

	// Keeps the run alive for as long as its ticker is registered.
	TArray<TSharedPtr<FAsyncMeshVerifyRun>> GActiveRuns;

	void VerifyAsyncMeshCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuAsync, Error,
			       TEXT("GPU worldgen needs SM6 (64-bit integer shader ops). Relaunch with -sm6 and ")
			       TEXT("make sure +D3D12TargetedShaderFormats=PCD3D_SM6 is in DefaultEngine.ini."));
			return;
		}

		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 4096) : 16;
		const int32 MaxInFlight = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 256) : 4;

		TSharedPtr<FAsyncMeshVerifyRun> Run = MakeShared<FAsyncMeshVerifyRun>(NumChunks, MaxInFlight);
		GActiveRuns.Add(Run);
		Run->Start();

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float DeltaTime)
		{
			if (Run->Tick(DeltaTime))
			{
				return true;
			}
			GActiveRuns.Remove(Run);
			return false;
		}), 0.0f);
	}

	FAutoConsoleCommand GVoxelGpuVerifyAsyncMeshCmd(
		TEXT("voxel.GPU.VerifyAsyncMesh"),
		TEXT("Mesh K render chunks through FVoxelGpuMeshJobManager (no blocking calls) and "
		     "byte-compare the packed quads against MeshChunkBricks. Reports dispatch-to-ready "
		     "latency and chunks/s. Usage: voxel.GPU.VerifyAsyncMesh [K=16] [InFlight=4]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyAsyncMeshCommand));
}
