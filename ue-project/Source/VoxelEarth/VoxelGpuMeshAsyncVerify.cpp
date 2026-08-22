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

#include "VoxelBrickPool.h"
#include "VoxelChunkMesher.h"
#include "VoxelCoords.h"
#include "VoxelGpuMeshJobManager.h"
#include "VoxelGpuPoolComponent.h"
#include "VoxelGpuRegionBuild.h"
#include "VoxelGpuWorldGen.h"
#include "VoxelMeshTypes.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

#include "voxelcore/amplifier.h"
#include "voxelcore/brickpack.h"
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
	                                  const VoxelCoords::FVoxelChunkKey& Key,
	                                  uint8 RingSkirtMask = 0)
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
		MeshChunkBricks(Key, GridSampler, Quads, /*PerfCounters*/ nullptr, RingSkirtMask);

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
	                                         const VoxelCoords::FVoxelChunkKey& Key,
	                                         uint8 RingSkirtMask = 0)
	{
		FVoxelGpuRegionRequest Req;
		VoxelGpuChunkRegion::SetChunkFootprint(Req, Key.X, Key.Y, Key.Z);
		Req.Seed = kSeed;
		Req.RingSkirtMask = RingSkirtMask;
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

			// The caveats are printed NEXT TO the numbers, not left in a doc.
			// Both of these describe ways a reader could quote a figure from
			// this block and be wrong about what it means, and this programme
			// has already had to retract one set of numbers that way.
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  dispatch->ready latency ms: min %.2f  p50 %.2f  p95 %.2f  max %.2f"),
			       Percentile(ReadyLatenciesMs, 0.0), Percentile(ReadyLatenciesMs, 0.5),
			       Percentile(ReadyLatenciesMs, 0.95), Percentile(ReadyLatenciesMs, 1.0));
			UE_LOG(LogVoxelGpuAsync, Warning,
			       TEXT("    ^ DO NOT COMPARE ACROSS D3; QUOTE submit->deliver. Since D3 this "
			            "is when the 4-BYTE TOTAL landed (GPU work done). It used to be when "
			            "the whole ~810 KB readback landed, so it WILL be lower for a reason "
			            "that has nothing to do with the GPU being faster."));

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  submit->deliver latency ms: min %.2f  p50 %.2f  p95 %.2f  max %.2f"),
			       Percentile(DeliverLatenciesMs, 0.0), Percentile(DeliverLatenciesMs, 0.5),
			       Percentile(DeliverLatenciesMs, 0.95), Percentile(DeliverLatenciesMs, 1.0));
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("    ^ the honest end-to-end number: covers both D3 phases."));

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("  throughput: %.1f chunks/s at an in-flight cap of %d "
			            "(cpu reference meshed %.1f chunks/s single-threaded)"),
			       Throughput, MaxInFlight,
			       double(NumChunks) / FMath::Max(1e-6, CpuTotalMs / 1000.0));
			UE_LOG(LogVoxelGpuAsync, Warning,
			       TEXT("    ^ D3 IS A TRADE, quote it with this number: bandwidth down "
			            "~70-100x (786 KB -> ~7-12 KB per chunk), latency-to-delivery roughly "
			            "DOUBLED (two round trips instead of one). Also: the cpu reference is "
			            "SINGLE-THREADED and the shipping path uses ~24 workers, so it is a "
			            "per-chunk cost comparison, NOT a throughput ratio."));

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

	// D5.3: the ring skirt, over its ENTIRE input domain.
	//
	// WHY EXHAUSTIVE RATHER THAN SAMPLED, and it is not thoroughness for its own
	// sake. The skirt makes a chunk's mesh a function of the CAMERA ANCHOR --
	// ComputeRingSkirtMask asks which neighbours belong to a finer ring -- and
	// the anchor is not part of the world seed. So the determinism digest
	// STRUCTURALLY cannot cover this feature: there is no fixed expected output
	// to pin. The only gate available is a direct comparison against
	// MeshChunkBricks called with the same mask.
	//
	// The saving grace is that the domain is 16 values. Four lateral faces, each
	// on or off. Sixteen comparisons is not a sample of the input space, it IS
	// the input space, and that is a rare enough luxury to take rather than
	// spend on picking a representative subset.
	//
	// Mask 0 is included and is the control: it must reproduce the unskirted
	// mesh exactly, which is what proves the skirt block is genuinely dead when
	// no face is flagged rather than merely usually harmless.
	void VerifyRingSkirtCommand(const TArray<FString>& Args)
	{
		if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
		{
			UE_LOG(LogVoxelGpuAsync, Error, TEXT("GPU worldgen needs SM6. Relaunch with -sm6."));
			return;
		}

		vxc::SyntheticTileSampler Tiles(kSeed);
		vxc::SyntheticTileSampler CpuTiles(kSeed);
		vxc::Amplifier CpuAmp(kSeed, CpuTiles);

		const int32 Cx = (Args.Num() > 0) ? FCString::Atoi(*Args[0]) : 0;
		const int32 Cy = (Args.Num() > 1) ? FCString::Atoi(*Args[1]) : 0;
		const VoxelCoords::FVoxelChunkKey Key = SurfaceChunkKey(CpuAmp, Cx, Cy);

		UE_LOG(LogVoxelGpuAsync, Log,
		       TEXT("voxel.GPU.VerifyRingSkirt: chunk (%d, %d, %d), all 16 mask values against "
		            "MeshChunkBricks with the same mask. The digest cannot cover this -- the skirt "
		            "is a function of the camera anchor, which is not part of the seed."),
		       Key.X, Key.Y, Key.Z);

		int32 Failures = 0;
		int32 MasksThatChangedGeometry = 0;
		for (uint8 Mask = 0; Mask < 16; ++Mask)
		{
			const TArray<uint64> Cpu = CpuMeshChunkPacked(CpuAmp, Key, Mask);
			const TArray<uint64> CpuUnskirted = (Mask == 0) ? Cpu : CpuMeshChunkPacked(CpuAmp, Key, 0);

			FVoxelGpuRegionRequest Req = BuildChunkRequest(Tiles, Key, Mask);
			// CHUNK-LOCAL, because the CPU reference is. PackVoxelChunkQuad's
			// Slice/U0/V0 are chunk-local (0..31); the DEFAULT emit permutation
			// writes them BRICK-local (0..7). Comparing the two gives byte
			// mismatches from the very first quad while the counts agree
			// exactly -- which is what this gate reported on its first run, for
			// every mask INCLUDING zero. Mask 0 failing is what identified it as
			// a harness fault rather than a skirt one: the skirt block is
			// provably dead at mask 0.
			Req.bChunkLocalQuads = true;
			const FVoxelGpuRegionResult Gpu = VoxelGpuWorldGen::RunRegionBlocking(Req);
			if (!Gpu.bOk)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("[D5.3 skirt] FAIL — mask %u: dispatch failed: %s"), Mask, *Gpu.Error);
				++Failures;
				continue;
			}

			// The GPU emits chunk-local under voxel.GPU.MeshChunkLocal, which is
			// the same form PackVoxelChunkQuad produces, so this is a direct
			// byte comparison of two quad streams in the same order.
			bool bMatch = (Gpu.Quads.Num() == Cpu.Num());
			int32 FirstDiff = -1;
			if (bMatch)
			{
				for (int32 I = 0; I < Cpu.Num(); ++I)
				{
					if (Gpu.Quads[I] != Cpu[I]) { bMatch = false; FirstDiff = I; break; }
				}
			}

			// Did this mask actually DO anything? A gate where every case is
			// vacuously equal to the control proves nothing, and the D6 sweep
			// spent its whole life in exactly that state.
			const bool bChanged = (Mask != 0) && (Cpu != CpuUnskirted);
			if (bChanged) { ++MasksThatChangedGeometry; }

			if (bMatch)
			{
				UE_LOG(LogVoxelGpuAsync, Log,
				       TEXT("[D5.3 skirt] PASS — mask %2u (%s): %d quads, byte-identical to "
				            "MeshChunkBricks%s"),
				       Mask,
				       *FString::Printf(TEXT("%s%s%s%s"),
				                        (Mask & 1) ? TEXT("-X ") : TEXT(""),
				                        (Mask & 2) ? TEXT("+X ") : TEXT(""),
				                        (Mask & 4) ? TEXT("-Y ") : TEXT(""),
				                        (Mask & 8) ? TEXT("+Y") : TEXT("")),
				       Cpu.Num(),
				       (Mask == 0) ? TEXT(" [control: skirt block must be dead]")
				                   : (bChanged ? TEXT(" [changed geometry vs mask 0]")
				                               : TEXT(" [WARNING: identical to mask 0 -- this face "
				                                      "had no apron to rewrite, so the case is "
				                                      "vacuous here]")));
			}
			else
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("[D5.3 skirt] FAIL — mask %u: gpu %d quads vs cpu %d, first differing "
				            "quad %d"),
				       Mask, Gpu.Quads.Num(), Cpu.Num(), FirstDiff);
				++Failures;
			}
		}

		if (Failures == 0 && MasksThatChangedGeometry == 0)
		{
			UE_LOG(LogVoxelGpuAsync, Warning,
			       TEXT("[D5.3 skirt] all 16 masks matched, but NOT ONE changed the geometry at "
			            "this chunk -- every non-zero case was vacuously equal to mask 0. Pick a "
			            "chunk whose lateral aprons actually contain solid voxels: "
			            "voxel.GPU.VerifyRingSkirt <cx> <cy>."));
		}
		else if (Failures == 0)
		{
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("[D5.3 skirt] PASS — all 16 mask values byte-identical to MeshChunkBricks; "
			            "%d of 15 non-zero masks changed the geometry."),
			       MasksThatChangedGeometry);
		}
		else
		{
			UE_LOG(LogVoxelGpuAsync, Error,
			       TEXT("[D5.3 skirt] FAIL — %d of 16 mask values disagree."), Failures);
		}
	}

	// =====================================================================
	// voxel.GPU.VerifyPoolWrite — the gate on D1 (no-readback GPU meshing)
	// =====================================================================
	//
	// WHAT THIS PROVES THAT NOTHING ELSE CAN. voxel.GPU.VerifyAsyncMesh compares
	// the quads the manager DELIVERS against MeshChunkBricks, and under
	// voxel.GPU.MeshDirectToPool the manager delivers no quads at all -- so it
	// goes quiet on exactly the path D1 adds. Everything the CPU still knows
	// about a direct write (the allocation, the chunk-table entry, the run, the
	// count) would be identical if the compute pass had written nothing, or
	// written to the wrong offset, or written someone else's quads. The only
	// honest check is to read the POOL BUFFER back and look.
	//
	// So this meshes K chunks through the manager in direct mode, applies each
	// through UVoxelGpuPoolComponent::AddChunkFromGpu, then reads back the used
	// prefix of both pool buffers and checks THREE things per chunk:
	//
	//   1. the quads at its allocated range are byte-identical to
	//      MeshChunkBricks + PackVoxelChunkQuad -- the same CPU reference
	//      voxel.GPU.VerifyAsyncMesh uses, and the shipping mesher rather than a
	//      transcription of it;
	//   2. every chunk id across that range is the id the pool issued -- a quad
	//      with the wrong id draws at another chunk's origin, which is the
	//      failure a picture is worst at showing;
	//   3. the quads immediately OUTSIDE the range are untouched. Writing past
	//      the end is the one new failure this design can produce and the only
	//      one of the three that a per-chunk comparison would miss entirely.
	//
	// Run it with voxel.GPU.MeshDirectToPool 0 as well: the readback path must
	// produce the same bytes through AddChunk. A gate with no control says
	// nothing about either path.
	struct FPoolWriteVerifyRun : public TSharedFromThis<FPoolWriteVerifyRun>
	{
		struct FChunkUnderTest
		{
			VoxelCoords::FVoxelChunkKey Key;
			TArray<uint64> CpuQuads;
			int32 PoolHandle = INDEX_NONE;
			uint32 PoolFirst = 0;
			uint32 PoolCount = 0;
			uint32 ChunkId = 0;
			bool bDelivered = false;
			bool bApplied = false;
			bool bWasDirect = false;
		};

		explicit FPoolWriteVerifyRun(int32 InNumChunks, int32 InMaxInFlight, double InStartDelay)
			: Tiles(kSeed)
			, CpuTiles(kSeed)
			, CpuAmp(kSeed, CpuTiles)
			, NumChunks(InNumChunks)
			, MaxInFlight(InMaxInFlight)
			, StartDelaySeconds(InStartDelay)
			, Manager(FVoxelGpuMeshJobComplete::CreateRaw(this, &FPoolWriteVerifyRun::OnJobComplete),
			          InMaxInFlight)
		{
		}

		~FPoolWriteVerifyRun()
		{
			if (PoolOwner.IsValid())
			{
				PoolOwner->Destroy();
			}
		}

		bool CreatePool()
		{
			UWorld* World = nullptr;
			if (GEngine != nullptr)
			{
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.World() != nullptr && Context.World()->IsGameWorld())
					{
						World = Context.World();
						break;
					}
				}
			}
			if (World == nullptr)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("voxel.GPU.VerifyPoolWrite: no game world. This gate needs a live world — the "
				            "pool's buffers are created by its first SCENE PROXY, which needs a registered "
				            "component in a ticking world. Run it in PIE or with -game."));
				return false;
			}

			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector,
			                                          FRotator::ZeroRotator, Params);
			if (Owner == nullptr)
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("voxel.GPU.VerifyPoolWrite: could not spawn a pool actor"));
				return false;
			}
			PoolOwner = Owner;

			Pool = NewObject<UVoxelGpuPoolComponent>(Owner);
			Pool->SetPoolName(TEXT("VoxelGpuPool.VerifyPoolWrite"));
			Pool->SetChunkTableCapacity(FMath::Max(64, NumChunks * 4));
			Owner->SetRootComponent(Pool.Get());
			Pool->RegisterComponent();
			// Generous: the whole point is that no chunk is refused for space, so
			// an alloc failure here is unambiguously a bug rather than capacity.
			Pool->InitPool(uint32(NumChunks + 1) * 98304u / 8u + 65536u);

			// THE SEED CHUNK, and it is not padding. A pool's persistent buffers
			// are created by its first scene proxy, and CreateSceneProxy returns
			// null until something is in the pool -- so a direct write has nowhere
			// to land until at least one chunk has arrived by the CPU path. That
			// is the same bootstrap the streaming path has, exercised here on
			// purpose rather than papered over: it is also the guard band the
			// out-of-range check below reads.
			TArray<uint64> Seed;
			Seed.SetNumZeroed(kSeedQuads);
			for (int32 I = 0; I < kSeedQuads; ++I)
			{
				Seed[I] = kGuardPattern ^ uint64(I);
			}
			SeedHandle = Pool->AddChunk(Seed, FVector3f::ZeroVector, 0);
			return SeedHandle != INDEX_NONE;
		}

		void Start()
		{
			Chunks.SetNum(NumChunks);
			const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));
			for (int32 I = 0; I < NumChunks; ++I)
			{
				const int32 Cx = (I % Side) - Side / 2;
				const int32 Cy = (I / Side) - Side / 2;
				Chunks[I].Key = SurfaceChunkKey(CpuAmp, Cx, Cy);
				Chunks[I].CpuQuads = CpuMeshChunkPacked(CpuAmp, Chunks[I].Key);
			}

			const IConsoleVariable* DirectCVar =
				IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GPU.MeshDirectToPool"));
			const IConsoleVariable* ChunkLocalCVar =
				IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.GPU.MeshChunkLocal"));
			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("voxel.GPU.VerifyPoolWrite: %d chunks, %d in flight, seed %llu, "
			            "MeshDirectToPool=%d MeshChunkLocal=%d, pool GPU-writable=%d. "
			            "A PASS that does not say which path it tested is not evidence about either."),
			       NumChunks, MaxInFlight, kSeed,
			       DirectCVar ? DirectCVar->GetInt() : -1,
			       ChunkLocalCVar ? ChunkLocalCVar->GetInt() : -1,
			       Pool.IsValid() && Pool->IsGpuWritable() ? 1 : 0);

			FirstSubmitSeconds = FPlatformTime::Seconds();
			for (int32 I = 0; I < NumChunks; ++I)
			{
				Manager.Submit(BuildChunkRequest(Tiles, Chunks[I].Key), uint64(I),
				               /*bRequestGpuResidentQuads*/ true);
			}
		}

		void OnJobComplete(FVoxelGpuMeshJobResult&& Result)
		{
			const int32 Index = int32(Result.UserTag);
			if (!Chunks.IsValidIndex(Index))
			{
				++NumDelivered;
				return;
			}
			FChunkUnderTest& Chunk = Chunks[Index];
			if (Chunk.bDelivered)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d (%d,%d,%d) delivered TWICE"), Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z);
				++NumDoubleDelivered;
				return;
			}
			Chunk.bDelivered = true;
			++NumDelivered;

			if (!Result.IsOk())
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("  chunk %d FAILED: %s — %s"),
				       Index, LexToString(Result.Status), *Result.Error);
				++NumFailed;
				return;
			}

			// The count must agree BEFORE anything is allocated from it: a wrong
			// count would size the pool range and the comparison alike, and the
			// two errors would cancel into a PASS.
			if (Result.NumQuads != uint32(Chunk.CpuQuads.Num()))
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d (%d,%d,%d): quad COUNT differs — gpu %u, cpu %d"),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z, Result.NumQuads, Chunk.CpuQuads.Num());
				++NumMismatched;
				return;
			}
			if (Result.NumQuads == 0)
			{
				// SurfaceChunkKey picks chunks that contain the surface, so this
				// should not happen -- and a run of empty chunks would pass every
				// check below vacuously, which is the failure mode this whole
				// harness is written to avoid.
				UE_LOG(LogVoxelGpuAsync, Warning,
				       TEXT("  chunk %d (%d,%d,%d) meshed to ZERO quads — nothing to verify here"),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z);
				return;
			}

			Chunk.bWasDirect = Result.GpuQuads.IsValid();
			if (!Chunk.bWasDirect)
			{
				// The control path (voxel.GPU.MeshDirectToPool 0, or the manager
				// refusing the request). Apply it the way the streaming path does
				// so the SAME pool-content check below covers both.
				Chunk.PoolHandle = Pool->AddChunk(Result.Quads, FVector3f::ZeroVector, 0);
			}
			else
			{
				++NumDirect;
				Chunk.PoolHandle = Pool->AddChunkFromGpu(Result.GpuQuads, Result.NumQuads,
				                                         FVector3f::ZeroVector, 0);
			}

			if (Chunk.PoolHandle == INDEX_NONE)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d: the pool refused %u quads (free %u, largest run %u, gpuWritable=%d)"),
				       Index, Result.NumQuads, Pool->GetFreeQuads(), Pool->GetLargestFreeRun(),
				       Pool->IsGpuWritable() ? 1 : 0);
				++NumFailed;
				return;
			}
			Chunk.bApplied = true;
			Chunk.PoolCount = Result.NumQuads;
			// Recovered from the runs the pool publishes rather than guessed: the
			// run list is exactly what the renderer culls and draws from, so
			// checking the bytes at the run's offset is checking what will be
			// drawn.
			for (const UVoxelGpuPoolComponent::FChunkRun& Run : Pool->DebugGetChunkRuns())
			{
				if (Run.NumQuads == Chunk.PoolCount && !UsedOffsets.Contains(Run.FirstQuad))
				{
					Chunk.PoolFirst = Run.FirstQuad;
					Chunk.ChunkId = Run.ChunkId;
					UsedOffsets.Add(Run.FirstQuad);
					break;
				}
			}
		}

		// Reads the whole used prefix of both pool buffers back, once.
		void BeginReadback()
		{
			const FVoxelGpuPoolBuffersRef Buffers = Pool->DebugGetPoolBuffers();
			HighWater = Pool->GetHighWaterMarkQuads();
			if (!Buffers.IsValid() || !Buffers->QuadPooled.IsValid() || HighWater == 0)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("voxel.GPU.VerifyPoolWrite: the pool has no GPU buffers to read back "
				            "(highWater=%u). Nothing was written."), HighWater);
				bReadbackFailed = true;
				return;
			}

			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> QuadRb =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyPool.Quads"));
			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> IdRb =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyPool.Ids"));
			QuadReadback = QuadRb;
			IdReadback = IdRb;

			const uint32 Quads = HighWater;
			ENQUEUE_RENDER_COMMAND(VoxelVerifyPoolReadback)(
				[Buffers, QuadRb, IdRb, Quads](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FRDGBufferRef Q = GraphBuilder.RegisterExternalBuffer(Buffers->QuadPooled, TEXT("VerifyPool.Quads"));
				FRDGBufferRef I = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkIdPooled, TEXT("VerifyPool.Ids"));
				AddEnqueueCopyPass(GraphBuilder, QuadRb.Get(), Q, Quads * uint32(sizeof(uint64)));
				AddEnqueueCopyPass(GraphBuilder, IdRb.Get(), I, Quads * uint32(sizeof(uint32)));
				GraphBuilder.Execute();
			});
		}

		bool HarvestReadback()
		{
			if (!QuadReadback.IsValid() || !IdReadback.IsValid())
			{
				return false;
			}
			bool bReady = false;
			// IsReady is render-thread state; the flush is acceptable here because
			// this is a verification tool and nothing about the measurement depends
			// on not blocking (unlike the runner itself, which must never flush).
			ENQUEUE_RENDER_COMMAND(VoxelVerifyPoolPoll)(
				[Q = QuadReadback, I = IdReadback, &bReady](FRHICommandListImmediate&)
			{
				bReady = Q->IsReady() && I->IsReady();
			});
			FlushRenderingCommands();
			if (!bReady)
			{
				return false;
			}

			PoolQuads.SetNumUninitialized(int32(HighWater));
			PoolIds.SetNumUninitialized(int32(HighWater));
			ENQUEUE_RENDER_COMMAND(VoxelVerifyPoolCopy)(
				[Q = QuadReadback, I = IdReadback, QuadsOut = PoolQuads.GetData(),
				 IdsOut = PoolIds.GetData(), Count = HighWater](FRHICommandListImmediate&)
			{
				if (const void* Src = Q->Lock(Count * uint32(sizeof(uint64))))
				{
					FMemory::Memcpy(QuadsOut, Src, SIZE_T(Count) * sizeof(uint64));
					Q->Unlock();
				}
				if (const void* Src = I->Lock(Count * uint32(sizeof(uint32))))
				{
					FMemory::Memcpy(IdsOut, Src, SIZE_T(Count) * sizeof(uint32));
					I->Unlock();
				}
			});
			FlushRenderingCommands();
			return true;
		}

		void Compare()
		{
			int32 Checked = 0, QuadFail = 0, IdFail = 0, GuardFail = 0;

			for (int32 I = 0; I < Chunks.Num(); ++I)
			{
				const FChunkUnderTest& C = Chunks[I];
				if (!C.bApplied || C.PoolCount == 0)
				{
					continue;
				}
				if (uint64(C.PoolFirst) + C.PoolCount > uint64(HighWater))
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d: range [%u, %u) is outside the drawn prefix (%u)"),
					       I, C.PoolFirst, C.PoolFirst + C.PoolCount, HighWater);
					++QuadFail;
					continue;
				}
				++Checked;

				// (1) the bytes.
				for (uint32 Q = 0; Q < C.PoolCount; ++Q)
				{
					if (PoolQuads[int32(C.PoolFirst + Q)] == C.CpuQuads[int32(Q)])
					{
						continue;
					}
					++QuadFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d (%d,%d,%d): pool quad [%u] of %u differs — pool %016llx vs cpu %016llx "
					            "(pool offset %u)"),
					       I, C.Key.X, C.Key.Y, C.Key.Z, Q, C.PoolCount,
					       PoolQuads[int32(C.PoolFirst + Q)], C.CpuQuads[int32(Q)], C.PoolFirst);
					break;
				}

				// (2) the ids. A quad with the wrong id draws at another chunk's
				// origin, which looks like a mesher bug and is not one.
				for (uint32 Q = 0; Q < C.PoolCount; ++Q)
				{
					if (PoolIds[int32(C.PoolFirst + Q)] == C.ChunkId)
					{
						continue;
					}
					++IdFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d: chunk id at pool quad [%u] is %u, expected %u"),
					       I, C.PoolFirst + Q, PoolIds[int32(C.PoolFirst + Q)], C.ChunkId);
					break;
				}
			}

			// (3) the guard band. The seed chunk sits at pool offset 0 and holds a
			// known pattern that nothing under test should ever write. A pass that
			// only compared each chunk's own range would be blind to a write that
			// ran past the end of a neighbour's.
			for (int32 Q = 0; Q < kSeedQuads && Q < int32(HighWater); ++Q)
			{
				if (PoolQuads[Q] != (kGuardPattern ^ uint64(Q)))
				{
					++GuardFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  GUARD BAND CORRUPTED at pool quad %d: %016llx, expected %016llx. "
					            "Something wrote outside its allocation."),
					       Q, PoolQuads[Q], kGuardPattern ^ uint64(Q));
					break;
				}
			}

			const bool bPass = (QuadFail == 0 && IdFail == 0 && GuardFail == 0 && NumFailed == 0
			                    && NumMismatched == 0 && NumDoubleDelivered == 0 && Checked > 0);
			const FString Summary = FString::Printf(
				TEXT("voxel.GPU.VerifyPoolWrite: %s — %d/%d chunks checked in the pool buffer "
				     "(%d written GPU-side, %d via the readback control), quadFail=%d idFail=%d "
				     "guardFail=%d failed=%d countMismatch=%d doubleDelivered=%d, "
				     "poolDirectWrites=%lld dropped=%d"),
				bPass ? TEXT("PASS — pool contents are byte-identical to MeshChunkBricks") : TEXT("FAIL"),
				Checked, NumChunks, NumDirect, Checked - NumDirect,
				QuadFail, IdFail, GuardFail, NumFailed, NumMismatched, NumDoubleDelivered,
				(long long)Pool->GetGpuDirectWrites(), Pool->GetGpuDirectWritesDropped());
			// Verbosity is a compile-time argument to UE_LOG, so the branch is on
			// the call rather than inside it.
			if (bPass)
			{
				UE_LOG(LogVoxelGpuAsync, Log, TEXT("%s"), *Summary);
			}
			else
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("%s"), *Summary);
			}

			if (Checked > 0 && NumDirect == 0)
			{
				UE_LOG(LogVoxelGpuAsync, Warning,
				       TEXT("  NOT ONE chunk took the direct path. This run says nothing about D1 — check "
				            "voxel.GPU.MeshDirectToPool and voxel.GPU.MeshChunkLocal are both 1."));
			}
		}

		bool Tick(float DeltaSeconds)
		{
			if (!bStarted)
			{
				ElapsedBeforeStart += double(DeltaSeconds);
				if (ElapsedBeforeStart < StartDelaySeconds)
				{
					return true;
				}
				if (!CreatePool())
				{
					return false;
				}
				bStarted = true;
				return true;   // one frame for the proxy (and its buffers) to exist
			}
			if (!bSubmitted)
			{
				if (!Pool->IsGpuWritable())
				{
					// The proxy has not been created yet. Wait rather than submit,
					// or every job would silently take the readback path and the run
					// would test nothing.
					if (++SettleTicks > 120)
					{
						UE_LOG(LogVoxelGpuAsync, Error,
						       TEXT("voxel.GPU.VerifyPoolWrite: the pool never became GPU-writable — its scene "
						            "proxy was never created. Nothing to test."));
						return false;
					}
					return true;
				}
				bSubmitted = true;
				Start();
				return true;
			}

			Manager.Tick();

			if (NumDelivered < NumChunks)
			{
				if (FPlatformTime::Seconds() - FirstSubmitSeconds > 120.0)
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("ABANDONING: %d of %d jobs never reported back."), NumChunks - NumDelivered, NumChunks);
					return false;
				}
				return true;
			}

			// Everything is delivered and applied. The writes are render commands
			// that may not have run yet, so let a couple of frames pass before
			// reading -- and read through the same command stream, which orders
			// the readback behind them.
			if (!bReadbackStarted)
			{
				if (++DrainTicks < 3)
				{
					return true;
				}
				bReadbackStarted = true;
				BeginReadback();
				return !bReadbackFailed;
			}
			if (!HarvestReadback())
			{
				if (++HarvestTicks > 600)
				{
					UE_LOG(LogVoxelGpuAsync, Error, TEXT("voxel.GPU.VerifyPoolWrite: the readback never landed"));
					return false;
				}
				return true;
			}

			Compare();
			return false;
		}

		static constexpr int32 kSeedQuads = 256;
		static constexpr uint64 kGuardPattern = 0x5645524946594242ull;   // "VERIFYBB"

		vxc::SyntheticTileSampler Tiles;
		vxc::SyntheticTileSampler CpuTiles;
		vxc::Amplifier CpuAmp;

		TArray<FChunkUnderTest> Chunks;
		TSet<uint32> UsedOffsets;
		TWeakObjectPtr<AActor> PoolOwner;
		TWeakObjectPtr<UVoxelGpuPoolComponent> Pool;
		int32 SeedHandle = INDEX_NONE;

		TArray<uint64> PoolQuads;
		TArray<uint32> PoolIds;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> QuadReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> IdReadback;
		uint32 HighWater = 0;

		int32 NumChunks = 0;
		int32 MaxInFlight = 4;
		double StartDelaySeconds = 25.0;
		double ElapsedBeforeStart = 0.0;
		double FirstSubmitSeconds = 0.0;
		int32 NumDelivered = 0, NumFailed = 0, NumMismatched = 0, NumDoubleDelivered = 0, NumDirect = 0;
		int32 SettleTicks = 0, DrainTicks = 0, HarvestTicks = 0;
		bool bStarted = false, bSubmitted = false, bReadbackStarted = false, bReadbackFailed = false;

		FVoxelGpuMeshJobManager Manager;
	};

	void VerifyPoolWriteCommand(const TArray<FString>& Args)
	{
		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 512) : 16;
		const int32 InFlight = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 256) : 4;
		const double Delay = (Args.Num() > 2) ? FMath::Max(0.0, FCString::Atod(*Args[2])) : 25.0;

		TSharedPtr<FPoolWriteVerifyRun> Run = MakeShared<FPoolWriteVerifyRun>(NumChunks, InFlight, Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float Dt) -> bool { return Run->Tick(Dt); }), 0.0f);
	}

	// =====================================================================
	// voxel.GPU.VerifyBrickPack — the gate on P1-C / P2 (the resident brick volume)
	// =====================================================================
	//
	// WHAT THIS PROVES THAT NOTHING ELSE CAN. P1-B byte-compared brickpack.ush
	// against vxc::packChunkBricksCanonical over 1,024 bricks with an offline
	// DXC harness. That says the KERNEL is right. It says nothing about Unreal's
	// compilation of it, nothing about the dispatch this phase added, and
	// nothing at all about the pool: the allocation, the three copies, the
	// descriptor rebase and the chunk record are all new code, and every one of
	// them can be wrong while producing a pool that looks populated.
	//
	// So this meshes K chunks through FVoxelGpuMeshJobManager with
	// voxel.GPU.BrickPack on, publishes each into a PRIVATE FVoxelBrickPool,
	// reads the pool back, and checks SIX things per chunk against
	// vxc::packChunkBricksCanonical over the same chunk:
	//
	//   1. CONTROL, and it is the one that cannot be wrong about the base: the
	//      SCRATCH descriptors, read straight out of the producer's own output.
	//      Those dispatch with all four write bases at ZERO, so they are the
	//      chunk-relative form the contract defines and they are compared with
	//      no arithmetic whatsoever.
	//   2. DIRECT: the POOL descriptors, which are the same 64 slots with the
	//      arena bases folded into their offset fields. Compared against the CPU
	//      reference REBASED BY THE ALLOCATION THE POOL ACTUALLY HANDED OUT --
	//      not by a base this code assumed. docs/brick-volume-format.md section
	//      6b: a base compared against a chunk offset reports a mismatch that is
	//      not one, and it looks exactly like the format being wrong. Checking
	//      both forms is what makes that indistinguishable pair distinguishable.
	//   3. the occupancy dwords at the chunk's arena offset, byte for byte;
	//   4. the material dwords likewise (palette and payload);
	//   5. the 32 B chunk record: origin, ring level, the 64-bit L1 mask, and
	//      BOTH FLAGS -- including allSolid, which is NOT derivable from the
	//      descriptors and looks as though it is, so a kernel that derived it
	//      would pass every other check here;
	//   6. GUARD BAND: every dword of every arena that is not inside a live
	//      allocation must still be ZERO. The arenas are created zero-filled and
	//      this gate never frees, so a write that ran past the end of its
	//      allocation has nowhere to hide. That is the one new failure this
	//      design can produce and the only one a per-chunk comparison misses.
	//
	// It forces voxel.GPU.BrickPack 1 and voxel.GPU.BrickPackResident 0 for the
	// duration and restores both, and it says so in the log -- a PASS that does
	// not name the path it tested is not evidence about any path. Resident 0 is
	// what keeps the GLOBAL pool out of the measurement: the payload comes back
	// on the result and this gate publishes it into its own.
	struct FBrickPackVerifyRun : public TSharedFromThis<FBrickPackVerifyRun>
	{
		struct FChunkUnderTest
		{
			VoxelCoords::FVoxelChunkKey Key;
			vxc::ChunkBrickPack Cpu;
			FVoxelGpuBrickPayloadRef Payload;
			FVoxelBrickPool::FResidentChunk Res;
			TUniquePtr<FRHIGPUBufferReadback> ScratchDescReadback;
			TArray<uint32> ScratchDesc;     // 2 dwords per brick slot
			bool bDelivered = false;
			bool bResident = false;
		};

		explicit FBrickPackVerifyRun(int32 InNumChunks, int32 InMaxInFlight, double InStartDelay)
			: Tiles(kSeed)
			, CpuTiles(kSeed)
			, CpuAmp(kSeed, CpuTiles)
			, NumChunks(InNumChunks)
			, MaxInFlight(InMaxInFlight)
			, StartDelaySeconds(InStartDelay)
			, Manager(FVoxelGpuMeshJobComplete::CreateRaw(this, &FBrickPackVerifyRun::OnJobComplete),
			          InMaxInFlight)
		{
		}

		~FBrickPackVerifyRun()
		{
			RestoreCvars();
		}

		// The CPU reference for one render chunk: the canonical packing over the
		// same 32^3 of world the GPU region covers. THE SHIPPING REFERENCE, not
		// a transcription of it -- vxc::packChunkBricksCanonical is the same
		// function 726 unit tests and the P1-B byte comparison run against.
		static vxc::ChunkBrickPack CpuPackChunk(const vxc::Amplifier& Amp,
		                                        const VoxelCoords::FVoxelChunkKey& Key)
		{
			constexpr int32 ChunkVox = vxc::kMarchChunkEdgeVoxels;
			const int64 BaseVX = int64(Key.X) * ChunkVox;
			const int64 BaseVY = int64(Key.Y) * ChunkVox;
			const int64 BaseVZ = int64(Key.Z) * ChunkVox;

			// NO HALO. A marcher reads neighbours by index, so the producer
			// dispatches 32x32x4 and the reference samples exactly the same
			// 32^3 -- one column per voxel column of the chunk and not one more.
			TArray<vxc::ColumnSample> Columns;
			Columns.SetNumUninitialized(ChunkVox * ChunkVox);
			for (int32 LY = 0; LY < ChunkVox; ++LY)
			{
				for (int32 LX = 0; LX < ChunkVox; ++LX)
				{
					Columns[LX + ChunkVox * LY] = Amp.column(BaseVX + LX, BaseVY + LY);
				}
			}

			return vxc::packChunkBricksCanonical(
				[&Columns, BaseVZ](int32 X, int32 Y, int32 Z) -> vxc::MaterialId
			{
				return vxc::Amplifier::materialAt(Columns[X + ChunkVox * Y], BaseVZ + Z);
			});
		}

		// FORCING A CVAR AT ECVF_SetByCode IS A SILENT NO-OP AGAINST THE COMMAND
		// LINE. Every leg on this project sets cvars through -ExecCmds, and the
		// console records those at ECVF_SetByConsole -- the HIGHEST priority
		// there is (see FConsoleVariableBase::CanChange: a Set is kept only when
		// NewPri >= OldPri). A Set at SetByCode is then discarded with nothing
		// but a LogConsoleManager warning, while this gate goes on to log
		// "Forced ..." and measure the path it did not force.
		//
		// That is harmless when the forced value is what the leg already set. It
		// is FATAL FOR THE CONTROL ARM: forcing voxel.GPU.BrickPack 0 would be
		// dropped, and the "control" would be a byte-identical rerun of the
		// direct arm -- a control that agrees with the thing it controls, for the
		// worst possible reason.
		//
		// So force at max(the priority the cvar already carries, SetByCode).
		// Equal priority IS accepted, so this beats -ExecCmds; and because the
		// force and the restore use the SAME priority, the restore puts the old
		// value back where the force left it instead of clobbering the recorded
		// priority to something the next writer of this cvar cannot beat.
		//
		// It is still only a request, so the value is READ BACK. A gate that
		// cannot establish its own preconditions must refuse to report, not
		// report anyway.
		struct FForcedCvar
		{
			int32 SavedValue = 0;
			EConsoleVariableFlags Priority = ECVF_SetByCode;
			bool bForced = false;
		};

		static bool ForceOneCvar(const TCHAR* Name, int32 Value, FForcedCvar& OutState)
		{
			IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Var == nullptr)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("voxel.GPU.VerifyBrickPack: ABORTING -- the console variable '%s' does "
				            "not exist, so this run cannot select the path it claims to test."),
				       Name);
				return false;
			}

			const uint32 CurrentPriority = uint32(Var->GetFlags()) & uint32(ECVF_SetByMask);
			OutState.Priority = EConsoleVariableFlags(
				FMath::Max<uint32>(CurrentPriority, uint32(ECVF_SetByCode)));
			OutState.SavedValue = Var->GetInt();

			Var->Set(Value, OutState.Priority);
			OutState.bForced = true;

			const int32 ReadBack = Var->GetInt();
			if (ReadBack != Value)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("voxel.GPU.VerifyBrickPack: ABORTING -- '%s' still reads %d after being "
				            "forced to %d at SetBy%s (it now sits at SetBy%s). This run would have "
				            "measured a path it did not select, so it reports nothing at all."),
				       Name, ReadBack, Value,
				       GetConsoleVariableSetByName(OutState.Priority),
				       GetConsoleVariableSetByName(
					       EConsoleVariableFlags(uint32(Var->GetFlags()) & uint32(ECVF_SetByMask))));
				return false;
			}
			return true;
		}

		static void RestoreOneCvar(const TCHAR* Name, FForcedCvar& State)
		{
			if (!State.bForced)
			{
				return;
			}
			State.bForced = false;
			if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				// The priority the force used, so this is accepted for the same
				// reason the force was, and the recorded priority is left alone.
				Var->Set(State.SavedValue, State.Priority);
			}
		}

		// false => the preconditions do not hold and the caller MUST abandon the
		// run. Anything already forced stays recorded, so RestoreCvars() still
		// puts it back.
		bool ForceCvars()
		{
			bCvarsForced = true;
			if (!ForceOneCvar(TEXT("voxel.GPU.BrickPack"), 1, PackCvarState))
			{
				return false;
			}
			// This gate owns its own pool. Publishing into the global one as
			// well would make the run's residency depend on whatever the
			// streaming path had already put there.
			if (!ForceOneCvar(TEXT("voxel.GPU.BrickPackResident"), 0, ResidentCvarState))
			{
				return false;
			}
			return true;
		}

		void RestoreCvars()
		{
			if (!bCvarsForced)
			{
				return;
			}
			bCvarsForced = false;
			RestoreOneCvar(TEXT("voxel.GPU.BrickPack"), PackCvarState);
			RestoreOneCvar(TEXT("voxel.GPU.BrickPackResident"), ResidentCvarState);
		}

		void Start()
		{
			// Small on purpose: the whole arena is read back, and a private pool
			// sized to the run makes the guard-band check cover every byte the
			// run could possibly have touched rather than a prefix of a 300 MiB
			// buffer.
			FVoxelBrickPoolConfig Config;
			Config.ChunkCapacity = uint32(NumChunks) + 4;
			Config.OccWordCapacity = uint32(NumChunks + 1) * 64u * 16u + 4096u;
			Config.MatWordCapacity = uint32(NumChunks + 1) * 64u * 132u + 4096u;
			Pool.Init(Config);

			Chunks.SetNum(NumChunks);
			const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumChunks)));
			for (int32 I = 0; I < NumChunks; ++I)
			{
				const int32 Cx = (I % Side) - Side / 2;
				const int32 Cy = (I / Side) - Side / 2;
				Chunks[I].Key = SurfaceChunkKey(CpuAmp, Cx, Cy);
				Chunks[I].Cpu = CpuPackChunk(CpuAmp, Chunks[I].Key);
			}

			UE_LOG(LogVoxelGpuAsync, Log,
			       TEXT("voxel.GPU.VerifyBrickPack: %d chunks, %d in flight, seed %llu. Forced "
			            "voxel.GPU.BrickPack 1 (was %d) and voxel.GPU.BrickPackResident 0 (was %d) "
			            "for this run AND READ BOTH BACK AT THE FORCED VALUE (a force that does not "
			            "take aborts the run); both are restored afterwards at the priority they "
			            "were forced at. The pool is PRIVATE to this run."),
			       NumChunks, MaxInFlight, kSeed, PackCvarState.SavedValue,
			       ResidentCvarState.SavedValue);

			FirstSubmitSeconds = FPlatformTime::Seconds();
			for (int32 I = 0; I < NumChunks; ++I)
			{
				Manager.Submit(BuildChunkRequest(Tiles, Chunks[I].Key), uint64(I),
				               /*bRequestGpuResidentQuads*/ false);
			}
		}

		void OnJobComplete(FVoxelGpuMeshJobResult&& Result)
		{
			const int32 Index = int32(Result.UserTag);
			if (!Chunks.IsValidIndex(Index))
			{
				++NumDelivered;
				return;
			}
			FChunkUnderTest& Chunk = Chunks[Index];
			if (Chunk.bDelivered)
			{
				++NumDoubleDelivered;
				return;
			}
			Chunk.bDelivered = true;
			++NumDelivered;

			if (!Result.IsOk())
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("  chunk %d FAILED: %s — %s"),
				       Index, LexToString(Result.Status), *Result.Error);
				++NumFailed;
				return;
			}
			if (!Result.BrickVolume.IsValid())
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d (%d,%d,%d) came back with NO BRICK VOLUME. The job meshed but "
				            "did not pack — check that voxel.GPU.BrickPack took effect."),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z);
				++NumFailed;
				return;
			}

			Chunk.Payload = Result.BrickVolume;

			// THE COUNTS MUST AGREE BEFORE ANYTHING IS ALLOCATED FROM THEM. A
			// wrong total would size the pool range AND the comparison alike,
			// and the two errors would cancel into a PASS.
			const uint32 CpuOccWords = uint32(Chunk.Cpu.occ.size());
			const uint32 CpuMatWords = uint32(Chunk.Cpu.mat.size());
			if (Chunk.Payload->OccWords != CpuOccWords || Chunk.Payload->MatWords != CpuMatWords)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d (%d,%d,%d): arena SIZES differ — gpu occ %u mat %u, cpu occ %u "
				            "mat %u"),
				       Index, Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z,
				       Chunk.Payload->OccWords, Chunk.Payload->MatWords, CpuOccWords, CpuMatWords);
				++NumMismatched;
				return;
			}

			const FVoxelBrickChunkKey PoolKey{ Chunk.Key.X, Chunk.Key.Y, Chunk.Key.Z, 0 };
			if (Pool.AddChunkFromGpu(Chunk.Payload, PoolKey,
			                         FVoxelBrickChunkShading::Neutral()) == INDEX_NONE)
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("  chunk %d: the brick pool refused it (occ %u, mat %u dwords)"),
				       Index, Chunk.Payload->OccWords, Chunk.Payload->MatWords);
				++NumFailed;
				return;
			}
			Chunk.bResident = Pool.DebugGetResidentChunk(PoolKey, Chunk.Res);
		}

		void BeginReadback()
		{
			const FVoxelBrickPoolBuffersRef Buffers = Pool.DebugGetBuffers();
			if (!Buffers.IsValid() || !Buffers->IsValid())
			{
				UE_LOG(LogVoxelGpuAsync, Error,
				       TEXT("voxel.GPU.VerifyBrickPack: the pool has no GPU buffers — nothing was "
				            "written."));
				bReadbackFailed = true;
				return;
			}

			DescSlots = Buffers->DescSlots;
			OccWords = Buffers->OccWords;
			MatWords = Buffers->MatWords;
			ChunkSlots = Buffers->ChunkSlots;

			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> D =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyBrick.Desc"));
			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> O =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyBrick.Occ"));
			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> M =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyBrick.Mat"));
			TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> T =
				MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("Voxel.VerifyBrick.Table"));
			PoolDescReadback = D;
			PoolOccReadback = O;
			PoolMatReadback = M;
			PoolTableReadback = T;

			// The SCRATCH descriptors, per chunk: 512 B each, and the whole
			// control experiment. They are the producer's own output with every
			// write base at zero, so they are compared with no rebasing at all.
			TArray<TRefCountPtr<FRDGPooledBuffer>> ScratchDescs;
			TArray<FRHIGPUBufferReadback*> ScratchReadbacks;
			for (FChunkUnderTest& Chunk : Chunks)
			{
				if (!Chunk.bResident || !Chunk.Payload.IsValid() || !Chunk.Payload->Desc.IsValid())
				{
					continue;
				}
				Chunk.ScratchDescReadback =
					MakeUnique<FRHIGPUBufferReadback>(TEXT("Voxel.VerifyBrick.ScratchDesc"));
				ScratchDescs.Add(Chunk.Payload->Desc);
				ScratchReadbacks.Add(Chunk.ScratchDescReadback.Get());
			}

			const uint32 InDescSlots = DescSlots;
			const uint32 InOccWords = OccWords;
			const uint32 InMatWords = MatWords;
			const uint32 InChunkSlots = ChunkSlots;
			ENQUEUE_RENDER_COMMAND(VoxelVerifyBrickReadback)(
				[Buffers, D, O, M, T, ScratchDescs, ScratchReadbacks,
				 InDescSlots, InOccWords, InMatWords, InChunkSlots](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);
				FRDGBufferRef Desc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled,
				                                                        TEXT("VerifyBrick.Desc"));
				FRDGBufferRef Occ = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled,
				                                                       TEXT("VerifyBrick.Occ"));
				FRDGBufferRef Mat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled,
				                                                       TEXT("VerifyBrick.Mat"));
				FRDGBufferRef Table = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled,
				                                                          TEXT("VerifyBrick.Table"));
				AddEnqueueCopyPass(GraphBuilder, D.Get(), Desc, InDescSlots * 8u);
				AddEnqueueCopyPass(GraphBuilder, O.Get(), Occ, InOccWords * 4u);
				AddEnqueueCopyPass(GraphBuilder, M.Get(), Mat, InMatWords * 4u);
				// The GPU-side half of the same sizing. Both this and the CPU-side
				// Copy below must use the record stride; the pair is why the
				// literal survived -- they agreed with each other at 8, so the
				// gate was self-consistent and wrong together.
				AddEnqueueCopyPass(GraphBuilder, T.Get(), Table,
				                   InChunkSlots * uint32(FVoxelBrickPool::kChunkRecordDwords) * 4u);

				for (int32 I = 0; I < ScratchDescs.Num(); ++I)
				{
					FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(ScratchDescs[I],
					                                                        TEXT("VerifyBrick.Scratch"));
					AddEnqueueCopyPass(GraphBuilder, ScratchReadbacks[I], Src, 64u * 8u);
				}
				GraphBuilder.Execute();
			});
		}

		bool HarvestReadback()
		{
			if (!PoolDescReadback.IsValid())
			{
				return false;
			}
			bool bReady = false;
			ENQUEUE_RENDER_COMMAND(VoxelVerifyBrickPoll)(
				[D = PoolDescReadback, O = PoolOccReadback, M = PoolMatReadback,
				 T = PoolTableReadback, &bReady](FRHICommandListImmediate&)
			{
				bReady = D->IsReady() && O->IsReady() && M->IsReady() && T->IsReady();
			});
			FlushRenderingCommands();
			if (!bReady)
			{
				return false;
			}

			PoolDesc.SetNumUninitialized(int32(DescSlots) * 2);
			PoolOcc.SetNumUninitialized(int32(OccWords));
			PoolMat.SetNumUninitialized(int32(MatWords));
			// kChunkRecordDwords, not 8: this sizes the readback the record
			// comparison and the guard band both index into, so a literal here
			// would truncate BOTH of them the moment the record grows -- and a
			// truncated readback reads as zeros, which the guard band scores as
			// a PASS.
			PoolTable.SetNumUninitialized(int32(ChunkSlots) * FVoxelBrickPool::kChunkRecordDwords);
			for (FChunkUnderTest& Chunk : Chunks)
			{
				if (Chunk.ScratchDescReadback.IsValid())
				{
					Chunk.ScratchDesc.SetNumUninitialized(128);
				}
			}

			ENQUEUE_RENDER_COMMAND(VoxelVerifyBrickCopy)(
				[this](FRHICommandListImmediate&)
			{
				const auto Copy = [](FRHIGPUBufferReadback& Rb, void* Dst, uint32 Bytes)
				{
					if (const void* Src = Rb.Lock(Bytes))
					{
						FMemory::Memcpy(Dst, Src, SIZE_T(Bytes));
						Rb.Unlock();
					}
				};
				Copy(*PoolDescReadback, PoolDesc.GetData(), DescSlots * 8u);
				Copy(*PoolOccReadback, PoolOcc.GetData(), OccWords * 4u);
				Copy(*PoolMatReadback, PoolMat.GetData(), MatWords * 4u);
				// kChunkRecordDwords, NOT a literal 8. This copy sizes the bytes
				// pulled back from the chunk table, and the array it fills and
				// both consumers that index it are already parameterised -- so a
				// literal here does not error, it copies HALF of each record and
				// leaves the rest as whatever the array allocation contained.
				// Caught by the very gate this change was made to strengthen:
				// "RECORD differs at dword 0 -- got 98c4fc20 want ffffffc0" with
				// the got-record showing repeated 4-dword groups, i.e. descriptor
				// payload read at the wrong stride, plus a guard-band hit on a
				// slot past the copied region.
				Copy(*PoolTableReadback, PoolTable.GetData(),
				     ChunkSlots * uint32(FVoxelBrickPool::kChunkRecordDwords) * 4u);
				for (FChunkUnderTest& Chunk : Chunks)
				{
					if (Chunk.ScratchDescReadback.IsValid() && Chunk.ScratchDesc.Num() == 128)
					{
						Copy(*Chunk.ScratchDescReadback, Chunk.ScratchDesc.GetData(), 64u * 8u);
					}
				}
			});
			FlushRenderingCommands();
			return true;
		}

		void Compare()
		{
			int32 Checked = 0;
			int32 ScratchFail = 0, PoolDescFail = 0, OccFail = 0, MatFail = 0, RecordFail = 0;

			// Which dwords are legitimately non-zero, for the guard band. Built
			// from the allocations the POOL handed out, not from what the writes
			// were asked to do -- so a write that landed outside its allocation
			// shows up here even if its own bytes are perfect.
			TArray<bool> OccLive;
			TArray<bool> MatLive;
			TArray<bool> DescLive;
			OccLive.Init(false, int32(OccWords));
			MatLive.Init(false, int32(MatWords));
			DescLive.Init(false, int32(DescSlots));
			TSet<uint32> LiveSlots;

			for (int32 I = 0; I < Chunks.Num(); ++I)
			{
				const FChunkUnderTest& C = Chunks[I];
				if (!C.bResident)
				{
					continue;
				}
				++Checked;
				LiveSlots.Add(C.Res.ChunkSlot);
				for (uint32 W = 0; W < 64u; ++W)
				{
					DescLive[int32(C.Res.BrickBase + W)] = true;
				}
				for (uint32 W = 0; W < C.Res.OccWords; ++W)
				{
					OccLive[int32(C.Res.OccBase + W)] = true;
				}
				for (uint32 W = 0; W < C.Res.MatWords; ++W)
				{
					MatLive[int32(C.Res.MatBase + W)] = true;
				}

				// (1) CONTROL. The scratch descriptors, chunk-relative, compared
				// with no arithmetic at all.
				if (C.ScratchDesc.Num() == 128)
				{
					for (int32 B = 0; B < 64; ++B)
					{
						const uint32 GpuOcc = C.ScratchDesc[B * 2 + 0];
						const uint32 GpuMat = C.ScratchDesc[B * 2 + 1];
						const vxc::BrickDesc& Ref = C.Cpu.descs[B];
						if (GpuOcc == Ref.OccWord && GpuMat == Ref.MatWord)
						{
							continue;
						}
						++ScratchFail;
						UE_LOG(LogVoxelGpuAsync, Error,
						       TEXT("  chunk %d (%d,%d,%d) brick %d: SCRATCH descriptor differs — gpu "
						            "%08x/%08x, cpu %08x/%08x. Chunk-relative on both sides; no base is "
						            "involved, so this is the FORMAT."),
						       I, C.Key.X, C.Key.Y, C.Key.Z, B, GpuOcc, GpuMat, Ref.OccWord, Ref.MatWord);
						break;
					}
				}

				// (2) DIRECT. The pool descriptors, against the reference rebased
				// by the allocation the pool actually handed out. UNIFORM BRICKS
				// ARE NOT REBASED -- their offset fields are zero by contract and
				// adding a base would invent an address into somebody else's
				// payload.
				for (int32 B = 0; B < 64; ++B)
				{
					const uint32 GpuOcc = PoolDesc[int32(C.Res.BrickBase + uint32(B)) * 2 + 0];
					const uint32 GpuMat = PoolDesc[int32(C.Res.BrickBase + uint32(B)) * 2 + 1];
					const vxc::BrickDesc& Ref = C.Cpu.descs[B];
					uint32 WantOcc = Ref.OccWord;
					uint32 WantMat = Ref.MatWord;
					if (Ref.kind() == 2u)   // MIXED
					{
						WantOcc = ((Ref.occDwordOffset() + C.Res.OccBase) & vxc::kBrickOffsetMask)
						        | (Ref.OccWord & ~vxc::kBrickOffsetMask);
						WantMat = ((Ref.matDwordOffset() + C.Res.MatBase) & vxc::kBrickOffsetMask)
						        | (Ref.MatWord & ~vxc::kBrickOffsetMask);
					}
					if (GpuOcc == WantOcc && GpuMat == WantMat)
					{
						continue;
					}
					++PoolDescFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d (%d,%d,%d) brick %d: POOL descriptor differs — pool "
					            "%08x/%08x, expected %08x/%08x (chunk-relative %08x/%08x rebased by "
					            "occ+%u mat+%u)"),
					       I, C.Key.X, C.Key.Y, C.Key.Z, B, GpuOcc, GpuMat, WantOcc, WantMat,
					       Ref.OccWord, Ref.MatWord, C.Res.OccBase, C.Res.MatBase);
					break;
				}

				// (3) occupancy, byte for byte at the arena offset.
				for (uint32 W = 0; W < C.Res.OccWords; ++W)
				{
					const uint32 Got = PoolOcc[int32(C.Res.OccBase + W)];
					const uint32 Want = C.Cpu.occ[W];
					if (Got == Want)
					{
						continue;
					}
					++OccFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d (%d,%d,%d): occupancy dword %u of %u differs — pool %08x, "
					            "cpu %08x (arena offset %u)"),
					       I, C.Key.X, C.Key.Y, C.Key.Z, W, C.Res.OccWords, Got, Want, C.Res.OccBase);
					break;
				}

				// (4) materials: the 16 B local palette and the payload.
				for (uint32 W = 0; W < C.Res.MatWords; ++W)
				{
					const uint32 Got = PoolMat[int32(C.Res.MatBase + W)];
					const uint32 Want = C.Cpu.mat[W];
					if (Got == Want)
					{
						continue;
					}
					++MatFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  chunk %d (%d,%d,%d): material dword %u of %u differs — pool %08x, "
					            "cpu %08x (arena offset %u)"),
					       I, C.Key.X, C.Key.Y, C.Key.Z, W, C.Res.MatWords, Got, Want, C.Res.MatBase);
					break;
				}

				// (5) THE WHOLE RECORD, BUILT BY THE WRITER AND COMPARED AS BYTES.
				//
				// THIS USED TO EXTRACT FIVE NAMED FIELDS AND COMPARE THEM ONE BY
				// ONE, WHICH MEANT THE GATE ONLY CHECKED WHAT SOMEBODY HAD
				// REMEMBERED TO LIST. Dword 7 was never read at all. That was
				// survivable only while dword 7 was reserved-and-zero; the moment
				// the record carries per-chunk shading (climate, the surface
				// plane) it stops being survivable, because those fields are
				// written by TWO INDEPENDENT PRODUCERS -- BuildChunkRecord on the
				// CPU arm and BrickChunkRecordMain on the GPU fork -- and a
				// disagreement between them has no error, no null and no counter.
				// The fork takes a small share of chunks and the worker the rest,
				// so the symptom would be patchy colour whose distribution
				// follows queue saturation. It would look exactly like a biome.
				//
				// Building `Want` through FVoxelBrickPool::BuildChunkRecord --
				// the same function the CPU arm's own upload path calls -- means
				// every field is gated, including every field added after this
				// line was written, for free and forever. This is the shape
				// VoxelCoverVerify.cpp:230-249 already uses.
				//
				// CONVERTED ON THE 8-DWORD FORMAT, DELIBERATELY, AND RUN BEFORE
				// THE FORMAT GREW. A gate converted in the same change it is
				// meant to catch is a gate nobody has tested.
				{
					uint32 Want[FVoxelBrickPool::kChunkRecordDwords] = {};
					const FIntVector WantOrigin(
						C.Key.X * int32(vxc::kMarchChunkEdgeVoxels),
						C.Key.Y * int32(vxc::kMarchChunkEdgeVoxels),
						C.Key.Z * int32(vxc::kMarchChunkEdgeVoxels));
					// RingLevel 0: this harness meshes and admits at level 0 only,
					// and the GPU kernel masks the level the same way, so the
					// LevelAndFlags dword must agree bit for bit.
					FVoxelBrickPool::BuildChunkRecord(WantOrigin, /*RingLevel=*/0u,
					                                  C.Cpu.anySolid, C.Cpu.allSolid,
					                                  C.Res.BrickBase, C.Cpu.brickSolid,
					                                  // This harness submits its own requests and
					                                  // plumbs no shading, so both sides are neutral.
					                                  // If that ever stops being true the whole-record
					                                  // compare below is what will say so.
					                                  FVoxelBrickChunkShading::Neutral(), Want);

					const int32 R = int32(C.Res.ChunkSlot) * FVoxelBrickPool::kChunkRecordDwords;
					int32 BadDword = INDEX_NONE;
					for (int32 W = 0; W < FVoxelBrickPool::kChunkRecordDwords; ++W)
					{
						if (PoolTable[R + W] != Want[W])
						{
							BadDword = W;
							break;
						}
					}

					if (BadDword != INDEX_NONE)
					{
						++RecordFail;
						// The offending dword is named because "the record
						// differs" is not actionable on a record that is about to
						// carry three unrelated field groups.
						UE_LOG(LogVoxelGpuAsync, Error,
						       TEXT("  chunk %d (%d,%d,%d): RECORD differs at dword %d — got %08x "
						            "want %08x. Full record got/want: "
						            "[%08x %08x %08x %08x %08x %08x %08x %08x] / "
						            "[%08x %08x %08x %08x %08x %08x %08x %08x]"),
						       I, C.Key.X, C.Key.Y, C.Key.Z, BadDword,
						       PoolTable[R + BadDword], Want[BadDword],
						       PoolTable[R + 0], PoolTable[R + 1], PoolTable[R + 2], PoolTable[R + 3],
						       PoolTable[R + 4], PoolTable[R + 5], PoolTable[R + 6], PoolTable[R + 7],
						       Want[0], Want[1], Want[2], Want[3],
						       Want[4], Want[5], Want[6], Want[7]);
					}
				}
			}

			// (6) THE GUARD BAND. The arenas were created zero-filled and this
			// run never frees, so every dword outside a live allocation must
			// still be zero. A per-chunk comparison is blind to a write that ran
			// past the end of its own range; this is not.
			int32 GuardFail = 0;
			for (int32 W = 0; W < PoolOcc.Num() && GuardFail == 0; ++W)
			{
				if (!OccLive[W] && PoolOcc[W] != 0)
				{
					++GuardFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  GUARD BAND: occupancy dword %d is %08x and belongs to no allocation. "
					            "Something wrote outside its range."), W, PoolOcc[W]);
				}
			}
			for (int32 W = 0; W < PoolMat.Num() && GuardFail == 0; ++W)
			{
				if (!MatLive[W] && PoolMat[W] != 0)
				{
					++GuardFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  GUARD BAND: material dword %d is %08x and belongs to no allocation."),
					       W, PoolMat[W]);
				}
			}
			for (int32 Slot = 0; Slot < int32(DescSlots) && GuardFail == 0; ++Slot)
			{
				if (!DescLive[Slot] && (PoolDesc[Slot * 2] != 0 || PoolDesc[Slot * 2 + 1] != 0))
				{
					++GuardFail;
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("  GUARD BAND: descriptor slot %d is %08x/%08x and belongs to no chunk."),
					       Slot, PoolDesc[Slot * 2], PoolDesc[Slot * 2 + 1]);
				}
			}
			for (int32 Slot = 0; Slot < int32(ChunkSlots) && GuardFail == 0; ++Slot)
			{
				if (LiveSlots.Contains(uint32(Slot)))
				{
					continue;
				}
				// kChunkRecordDwords, NOT 8. A literal here is one of the ways
				// this record's length can grow while a checker silently keeps
				// inspecting the old half: the guard would then check dwords 0-7
				// of every dead slot and never look at 8-15, so a stale tail
				// would be invisible. A slot freed and re-allocated would carry
				// the PREVIOUS TENANT'S per-chunk shading, and the symptom is
				// tint that follows slot reuse -- with this gate reporting pass.
				for (int32 W = 0; W < FVoxelBrickPool::kChunkRecordDwords; ++W)
				{
					const int32 D = Slot * FVoxelBrickPool::kChunkRecordDwords + W;
					if (PoolTable[D] != 0)
					{
						++GuardFail;
						UE_LOG(LogVoxelGpuAsync, Error,
						       TEXT("  GUARD BAND: chunk record %d dword %d is %08x and belongs to no "
						            "chunk."), Slot, W, PoolTable[D]);
						break;
					}
				}
			}

			const bool bPass = (ScratchFail == 0 && PoolDescFail == 0 && OccFail == 0 && MatFail == 0
			                    && RecordFail == 0 && GuardFail == 0 && NumFailed == 0
			                    && NumMismatched == 0 && NumDoubleDelivered == 0 && Checked > 0);
			const FString Summary = FString::Printf(
				TEXT("voxel.GPU.VerifyBrickPack: %s — %d/%d chunks checked against ")
				TEXT("packChunkBricksCanonical. scratchDescFail=%d poolDescFail=%d occFail=%d ")
				TEXT("matFail=%d recordFail=%d guardFail=%d failed=%d sizeMismatch=%d ")
				TEXT("doubleDelivered=%d. Pool: %d resident, %u occ + %u mat dwords used, ")
				TEXT("largest free run occ %u mat %u, allocFail %lld, evictions %lld."),
				bPass ? TEXT("PASS — the resident volume is byte-identical to the CPU reference")
				      : TEXT("FAIL"),
				Checked, NumChunks, ScratchFail, PoolDescFail, OccFail, MatFail, RecordFail,
				GuardFail, NumFailed, NumMismatched, NumDoubleDelivered,
				Pool.GetNumResidentChunks(), Pool.GetUsedOccWords(), Pool.GetUsedMatWords(),
				Pool.GetLargestFreeOccRun(), Pool.GetLargestFreeMatRun(),
				Pool.GetAllocFailures(), Pool.GetEvictions());
			if (bPass)
			{
				UE_LOG(LogVoxelGpuAsync, Log, TEXT("%s"), *Summary);
			}
			else
			{
				UE_LOG(LogVoxelGpuAsync, Error, TEXT("%s"), *Summary);
			}

			// A run where every chunk packed to nothing would pass all six checks
			// vacuously. SurfaceChunkKey picks chunks containing the surface, so
			// this should never fire -- which is exactly why it is worth saying
			// when it does.
			if (Checked > 0 && Pool.GetUsedMatWords() == 0)
			{
				UE_LOG(LogVoxelGpuAsync, Warning,
				       TEXT("  NOT ONE chunk allocated a material dword. Every chunk under test packed "
				            "to uniform bricks, so this run says nothing about the payload."));
			}
		}

		bool Tick(float DeltaSeconds)
		{
			if (!bStarted)
			{
				ElapsedBeforeStart += double(DeltaSeconds);
				if (ElapsedBeforeStart < StartDelaySeconds)
				{
					return true;
				}
				if (!VoxelGpuWorldGen::IsSupportedOnCurrentRHI())
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("voxel.GPU.VerifyBrickPack: this RHI cannot run the kernels (SM6 with "
					            "64-bit integer ops required)."));
					return false;
				}
				if (!ForceCvars())
				{
					// The gate could not put the engine into the configuration it
					// is about to describe. A PASS or a FAIL from here would be a
					// reading of an unknown path.
					RestoreCvars();
					return false;
				}
				bStarted = true;
				Start();
				return true;
			}

			Manager.Tick();

			if (NumDelivered < NumChunks)
			{
				if (FPlatformTime::Seconds() - FirstSubmitSeconds > 120.0)
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("ABANDONING: %d of %d jobs never reported back."),
					       NumChunks - NumDelivered, NumChunks);
					RestoreCvars();
					return false;
				}
				return true;
			}

			// The pool writes are render commands the manager's Tick enqueued;
			// let a couple of frames pass, then read through the same command
			// stream, which orders the readback behind them.
			if (!bReadbackStarted)
			{
				Pool.Flush();
				if (++DrainTicks < 3)
				{
					return true;
				}
				bReadbackStarted = true;
				BeginReadback();
				if (bReadbackFailed)
				{
					RestoreCvars();
					return false;
				}
				return true;
			}
			if (!HarvestReadback())
			{
				if (++HarvestTicks > 600)
				{
					UE_LOG(LogVoxelGpuAsync, Error,
					       TEXT("voxel.GPU.VerifyBrickPack: the readback never landed"));
					RestoreCvars();
					return false;
				}
				return true;
			}

			Compare();
			RestoreCvars();
			return false;
		}

		vxc::SyntheticTileSampler Tiles;
		vxc::SyntheticTileSampler CpuTiles;
		vxc::Amplifier CpuAmp;

		TArray<FChunkUnderTest> Chunks;
		FVoxelBrickPool Pool;

		TArray<uint32> PoolDesc;
		TArray<uint32> PoolOcc;
		TArray<uint32> PoolMat;
		TArray<uint32> PoolTable;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> PoolDescReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> PoolOccReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> PoolMatReadback;
		TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> PoolTableReadback;
		uint32 DescSlots = 0, OccWords = 0, MatWords = 0, ChunkSlots = 0;

		int32 NumChunks = 0;
		int32 MaxInFlight = 4;
		double StartDelaySeconds = 25.0;
		double ElapsedBeforeStart = 0.0;
		double FirstSubmitSeconds = 0.0;
		int32 NumDelivered = 0, NumFailed = 0, NumMismatched = 0, NumDoubleDelivered = 0;
		int32 DrainTicks = 0, HarvestTicks = 0;
		FForcedCvar PackCvarState, ResidentCvarState;
		bool bStarted = false, bReadbackStarted = false, bReadbackFailed = false, bCvarsForced = false;

		FVoxelGpuMeshJobManager Manager;
	};

	void VerifyBrickPackCommand(const TArray<FString>& Args)
	{
		const int32 NumChunks = (Args.Num() > 0) ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 256) : 16;
		const int32 InFlight = (Args.Num() > 1) ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 64) : 4;
		const double Delay = (Args.Num() > 2) ? FMath::Max(0.0, FCString::Atod(*Args[2])) : 25.0;

		TSharedPtr<FBrickPackVerifyRun> Run = MakeShared<FBrickPackVerifyRun>(NumChunks, InFlight, Delay);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Run](float Dt) -> bool { return Run->Tick(Dt); }), 0.0f);
	}

	FAutoConsoleCommand GVoxelGpuVerifyBrickPackCmd(
		TEXT("voxel.GPU.VerifyBrickPack"),
		TEXT("P1-C / P2 gate: pack K chunks into the resident brick volume through the streaming "
		     "dispatch, publish them into a private FVoxelBrickPool, read the POOL back and "
		     "byte-compare it against vxc::packChunkBricksCanonical. Checks the scratch (chunk-relative, "
		     "no rebase) and pool (rebased by the real allocation) descriptors, both arenas, the 32 B "
		     "chunk record including allSolid, and a guard band over every unallocated dword. Forces "
		     "voxel.GPU.BrickPack 1 / BrickPackResident 0 and restores them. Usage: [K=16] [InFlight=4] "
		     "[delaySeconds=25]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyBrickPackCommand));

	FAutoConsoleCommand GVoxelGpuVerifyPoolWriteCmd(
		TEXT("voxel.GPU.VerifyPoolWrite"),
		TEXT("D1 gate: mesh K chunks through FVoxelGpuMeshJobManager with the quads left in GPU memory, "
		     "write them into a real UVoxelGpuPoolComponent with no readback, then read the POOL BUFFER "
		     "back and byte-compare it against MeshChunkBricks. Also checks every chunk id in the range "
		     "and a guard band against out-of-range writes. Run with voxel.GPU.MeshDirectToPool 0 for the "
		     "control. Needs a live game world. Usage: [K=16] [InFlight=4] [delaySeconds=25]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyPoolWriteCommand));

	FAutoConsoleCommand GVoxelGpuVerifyRingSkirtCmd(
		TEXT("voxel.GPU.VerifyRingSkirt"),
		TEXT("D5.3: byte-compare the GPU ring skirt against MeshChunkBricks for ALL 16 mask "
		     "values -- the feature's entire input domain, which the determinism digest cannot "
		     "cover because the skirt depends on the camera anchor. Usage: [chunkX=0] [chunkY=0]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&VerifyRingSkirtCommand));

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
