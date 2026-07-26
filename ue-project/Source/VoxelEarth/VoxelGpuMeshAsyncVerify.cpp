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

// A NAMED namespace, not an anonymous one. VoxelGpuVerify.cpp has its own
// file-local kSeed, and UE's unity build concatenates the two translation units
// -- at which point two anonymous namespaces are one namespace and the names
// collide. Naming this one keeps it file-local in intent without depending on
// which unity group the file lands in.
namespace VoxelGpuMeshAsyncVerify
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

	// Builds the region request for one chunk. Shared by the async harness and
	// the blocking control below, so the control is genuinely the SAME request.
	FVoxelGpuRegionRequest BuildChunkRequest(vxc::SyntheticTileSampler& Tiles,
	                                         const VoxelCoords::FVoxelChunkKey& Key)
	{
		FVoxelGpuRegionRequest Req;
		VoxelGpuChunkRegion::SetChunkFootprint(Req, Key.X, Key.Y, Key.Z);
		Req.Seed = kSeed;
		// Sized from the dispatch footprint just set, by the same code the
		// blocking digest gate uses.
		VoxelGpuRegionBuild::FillRasterWindow(Req, Tiles);
		return Req;
	}

	// The rebase the manager does internally, duplicated here ONLY so the
	// blocking control can be compared on equal terms. Not used by the async path.
	TArray<uint64> RebaseToChunkLocal(const FVoxelGpuRegionResult& Gpu, uint32 InteriorX, uint32 InteriorY)
	{
		TArray<uint64> Rebased;
		Rebased.Reserve(Gpu.Quads.Num());
		for (int32 MaskIndex = 0; MaskIndex < Gpu.QuadCounts.Num(); ++MaskIndex)
		{
			const uint32 Count = Gpu.QuadCounts[MaskIndex];
			if (Count == 0)
			{
				continue;
			}
			const uint32 Start = Gpu.QuadOffsets[MaskIndex];
			const uint32 MeshBrickIndex = uint32(MaskIndex) / 48u;
			const uint32 Ix = MeshBrickIndex % InteriorX;
			const uint32 Iy = (MeshBrickIndex / InteriorX) % InteriorY;
			const uint32 Iz = MeshBrickIndex / (InteriorX * InteriorY);
			const uint32 BrickOrigin[3] = { Ix * 8u, Iy * 8u, Iz * 8u };

			for (uint32 Q = 0; Q < Count; ++Q)
			{
				const uint64 Packed = Gpu.Quads[int32(Start + Q)];
				const uint32 W0 = uint32(Packed & 0xffffffffull);
				const uint32 W1 = uint32(Packed >> 32);
				const uint32 Axis  =  W0        & 0xfu;
				const uint32 Dir   = (W0 >>  4) & 0xfu;
				const uint32 Slice = (W0 >>  8) & 0xffu;
				const uint32 U0    = (W0 >> 16) & 0xffu;
				const uint32 V0    = (W0 >> 24) & 0xffu;
				const uint32 U = (Axis + 1u) % 3u;
				const uint32 V = (Axis + 2u) % 3u;
				const uint32 NewW0 = Axis | (Dir << 4)
				                   | ((Slice + BrickOrigin[Axis]) << 8)
				                   | ((U0 + BrickOrigin[U]) << 16)
				                   | ((V0 + BrickOrigin[V]) << 24);
				Rebased.Add(uint64(NewW0) | (uint64(W1) << 32));
			}
		}
		return Rebased;
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

		explicit FAsyncMeshVerifyRun(int32 InNumChunks, int32 InMaxInFlight, double InStartDelay)
			: Tiles(kSeed)
			, CpuTiles(kSeed)
			, CpuAmp(kSeed, CpuTiles)
			, NumChunks(InNumChunks)
			, MaxInFlight(InMaxInFlight)
			, StartDelaySeconds(InStartDelay)
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

			// Which emit permutation this run is about. Without it a PASS here
			// says nothing about WHICH of the two paths passed, and the whole
			// point of D2's control is that both are run.
			const IConsoleVariable* ChunkLocalCVar =
				IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GPU.MeshChunkLocal"));
			const bool bChunkLocal = ChunkLocalCVar == nullptr || ChunkLocalCVar->GetInt() != 0;

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("voxel.GPU.VerifyAsyncMesh: %d chunks, %d in flight, seed %llu, emit=%s. "
			            "CPU reference: %d quads in %.1f ms (%.2f ms/chunk)."),
			       NumChunks, MaxInFlight, kSeed,
			       bChunkLocal ? TEXT("GPU chunk-local (voxel.GPU.MeshChunkLocal 1)")
			                   : TEXT("GPU brick-local + CPU rebase (voxel.GPU.MeshChunkLocal 0)"),
			       TotalCpuQuads, CpuTotalMs,
			       CpuTotalMs / double(FMath::Max(1, NumChunks)));

			FirstSubmitSeconds = FPlatformTime::Seconds();
			for (int32 I = 0; I < NumChunks; ++I)
			{
				Manager.Submit(BuildChunkRequest(Tiles, Chunks[I].Key), uint64(I));
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
		bool Tick(float DeltaSeconds)
		{
			// DO NOT MEASURE DURING STARTUP. The first run of this measured a
			// 15.6 s "dispatch to ready" latency and timed out 13 of 16 jobs --
			// not because the GPU was slow, but because -ExecCmds fires on frame
			// zero and the game thread then spends 15 s loading the world and
			// precaching PSOs without ticking. Every number that comes out of
			// that is meaningless, and it reads exactly like a broken runner.
			if (!bStarted)
			{
				ElapsedBeforeStart += double(DeltaSeconds);
				if (ElapsedBeforeStart < StartDelaySeconds)
				{
					return true;
				}
				bStarted = true;
				Start();
				return true;
			}

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
		double StartDelaySeconds = 0.0;
		double ElapsedBeforeStart = 0.0;
		bool bStarted = false;
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
		// See FAsyncMeshVerifyRun::Tick for why this is not optional under
		// -ExecCmds.
		const double StartDelay = (Args.Num() > 2) ? FMath::Max(0.0, FCString::Atod(*Args[2])) : 25.0;

		TSharedPtr<FAsyncMeshVerifyRun> Run =
			MakeShared<FAsyncMeshVerifyRun>(NumChunks, MaxInFlight, StartDelay);
		GActiveRuns.Add(Run);
		UE_LOG(LogVoxelGpuAsync, Log,
		       TEXT("voxel.GPU.VerifyAsyncMesh queued: %d chunks, %d in flight, starting in %.0f s "
		            "(so the measurement is not taken while the world is still loading)."),
		       NumChunks, MaxInFlight, StartDelay);

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

	// THE CONTROL EXPERIMENT (docs/gpu-pool-rendering-notes.md: "prefer a control
	// experiment to a bisect").
	//
	// Runs the IDENTICAL chunk region request through RunRegionBlocking -- the
	// path the digest gate already proves -- and compares it to the same CPU
	// reference the async harness uses. That splits the space in one run:
	//
	//   control fails too  -> the chunk region setup is wrong (footprint, raster
	//                         window, z-range), and the async runner is innocent.
	//   control passes     -> the async runner is what differs.
	//
	// It also dumps the first few differing COLUMN fields, because a stratigraphy
	// mismatch with byte-identical geometry can only come from the column stage.
	void RunControl(int32 NumChunks)
	{
		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		constexpr int32 ChunkVox = VoxelCoords::ChunkEdgeVoxels;
		const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));
		int32 NumMatched = 0;

		for (int32 I = 0; I < NumChunks; ++I)
		{
			const VoxelCoords::FVoxelChunkKey Key =
				SurfaceChunkKey(CpuAmp, (I % Side) - Side / 2, (I / Side) - Side / 2);

			const FVoxelGpuRegionRequest Req = BuildChunkRequest(Tiles, Key);
			const double Start = FPlatformTime::Seconds();
			const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
			const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;

			if (!Gpu.bOk)
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("  [control] chunk (%d,%d,%d) dispatch FAILED: %s"),
				       Key.X, Key.Y, Key.Z, *Gpu.Error);
				continue;
			}

			// --- column stage --------------------------------------------------
			// Dispatch column (8+x, 8+y) is chunk-local (x,y): the footprint has a
			// one-brick halo on the negative side.
			int32 ColumnMismatches = 0;
			for (int32 Y = 0; Y < ChunkVox && ColumnMismatches < 4; ++Y)
			{
				for (int32 X = 0; X < ChunkVox && ColumnMismatches < 4; ++X)
				{
					const int32 Idx = (8 + X) + (8 + Y) * int32(VoxelGpuChunkRegion::kColumns);
					const FVoxelGpuColumnSample& G = Gpu.Columns[Idx];
					const vxc::ColumnSample C =
						CpuAmp.column(int64(Key.X) * ChunkVox + X, int64(Key.Y) * ChunkVox + Y);

					if (G.SurfaceMm == C.surfaceMm && G.TopsoilMm == C.topsoilMm &&
					    G.SubsoilMm == C.subsoilMm && G.BedrockDepthMm == C.bedrockDepthMm &&
					    G.SurfaceMat == uint32(C.surfaceMat))
					{
						continue;
					}
					++ColumnMismatches;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  [control] chunk (%d,%d,%d) column (%d,%d): "
					            "surface cpu %d gpu %d | topsoil cpu %d gpu %d | subsoil cpu %d gpu %d | "
					            "bedrock cpu %d gpu %d | mat cpu %u gpu %u"),
					       Key.X, Key.Y, Key.Z, X, Y,
					       C.surfaceMm, G.SurfaceMm, C.topsoilMm, G.TopsoilMm,
					       C.subsoilMm, G.SubsoilMm, C.bedrockDepthMm, G.BedrockDepthMm,
					       uint32(C.surfaceMat), G.SurfaceMat);
				}
			}

			// --- quad stage ----------------------------------------------------
			const TArray<uint64> Rebased = RebaseToChunkLocal(
				Gpu, VoxelGpuChunkRegion::kInteriorBricks, VoxelGpuChunkRegion::kInteriorBricks);
			const TArray<uint64> CpuQuads = CpuMeshChunkPacked(CpuAmp, Key);

			const bool bSameCount = Rebased.Num() == CpuQuads.Num();
			const bool bSameBytes = bSameCount &&
				(Rebased.IsEmpty() || FMemory::Memcmp(Rebased.GetData(), CpuQuads.GetData(),
				                                      SIZE_T(Rebased.Num() * sizeof(uint64))) == 0);

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  [control] chunk (%d,%d,%d): blocking gpu %d quads, cpu %d quads, "
			            "columns differing (first 4 shown) %d, bytes %s, %.1f ms"),
			       Key.X, Key.Y, Key.Z, Rebased.Num(), CpuQuads.Num(), ColumnMismatches,
			       bSameBytes ? TEXT("IDENTICAL") : TEXT("DIFFER"), Ms);

			if (bSameBytes)
			{
				++NumMatched;
			}
		}

		UE_LOG(LogVoxelGpuAsync, Log, TEXT("[control] %d of %d chunks byte-identical through "
		                                   "RunRegionBlocking"), NumMatched, NumChunks);
	}

	void VerifyAsyncMeshControlCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuAsync, Error, TEXT("Needs SM6. Relaunch with -sm6."));
			return;
		}

		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 64) : 2;
		// Deferred for the same reason the async run is, but harder: this path
		// calls FlushRenderingCommands, and running that from -ExecCmds on frame
		// zero wedges the process outright (observed: no output, no crash, CPU
		// spinning). It must not run until the world is up.
		const double Delay = (Args.Num() > 1) ? FMath::Max(0.0, FCString::Atod(*Args[1])) : 25.0;

		TSharedPtr<double> Elapsed = MakeShared<double>(0.0);
		UE_LOG(LogVoxelGpuAsync, Log, TEXT("[control] queued: %d chunks, starting in %.0f s"),
		       NumChunks, Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Elapsed, NumChunks, Delay](float DeltaTime)
		{
			*Elapsed += double(DeltaTime);
			if (*Elapsed < Delay)
			{
				return true;
			}
			RunControl(NumChunks);
			return false;
		}), 0.0f);
	}

	FAutoConsoleCommand GVoxelGpuVerifyAsyncMeshControlCmd(
		TEXT("voxel.GPU.VerifyAsyncMesh.Control"),
		TEXT("Control experiment: mesh the SAME chunk regions through the blocking "
		     "RunRegionBlocking path and compare against the CPU mesher. If this fails too, the "
		     "region setup is wrong, not the async runner. Usage: [K=2] [delaySeconds=25]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyAsyncMeshControlCommand));

	FAutoConsoleCommand GVoxelGpuVerifyAsyncMeshCmd(
		TEXT("voxel.GPU.VerifyAsyncMesh"),
		TEXT("Mesh K render chunks through FVoxelGpuMeshJobManager (no blocking calls) and "
		     "byte-compare the packed quads against MeshChunkBricks. Reports dispatch-to-ready "
		     "latency and chunks/s. Usage: voxel.GPU.VerifyAsyncMesh [K=16] [InFlight=4]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyAsyncMeshCommand));
}
