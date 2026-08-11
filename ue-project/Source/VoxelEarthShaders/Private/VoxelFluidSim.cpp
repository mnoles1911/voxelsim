// VoxelFluidSim.cpp -- RDG pass builder for the Phase 0 PBF solver spike.
// See VoxelFluidSim.h for the ownership/lifetime doctrine and
// VoxelFluidSim.usf for the solver itself; this file is the plumbing between
// them, following VoxelGpuWorldGen.cpp's shape (FGlobalShader compute passes,
// per-kernel parameter structs, IMPLEMENT_GLOBAL_SHADER off a virtual path).

#include "VoxelFluidSim.h"

#include "VoxelFluidOccupancy.h" // the collision volume (AddPasses + parameter binding)
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderingThread.h"
#include "Misc/AutomationTest.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED

DEFINE_LOG_CATEGORY_STATIC(LogVoxelFluid, Log, All);

// `stat GPU` line for the whole solver graph.
//
// NOT RDG_GPU_STAT_SCOPE: UE 5.8 removed the legacy GPU profiler and that macro
// is deprecated to a no-op (RenderGraphEvent.h:520 says so in its own message).
// The live spelling is the _STAT variant of the event scope, which puts the
// same span on the breadcrumb timeline AND attributes it to this stat -- so one
// scope now feeds ProfileGPU and `stat GPU` both, where before it fed only the
// former. The name is kept identical to the RDG event name so a ProfileGPU tree
// and a stat line cannot be read as two different measurements.
DECLARE_GPU_STAT_NAMED(VoxelFluidSim, TEXT("VoxelFluidSim"));

// Macro, not a const TCHAR*, for the same reason VoxelGpuWorldGen.cpp:24
// gives: IMPLEMENT_GLOBAL_SHADER stringizes its path argument.
#define VOXEL_FLUID_SIM_USF "/VoxelEarth/VoxelFluidSim.usf"

// The HLSL contract struct is two float4s (VoxelFluidContract.ush:48-52). If
// the CPU mirror ever stops being 32 bytes the debug readback silently
// misaligns, so assert it here rather than debug it as "the points draw in
// the wrong place" (the same guard VoxelGpuWorldGen.cpp:29 keeps for columns).
static_assert(sizeof(VoxelFluidSim::FParticleCPU) == 32,
              "FParticleCPU must match FVoxelFluidParticle byte for byte");

// The basin sink's extent grid packs one ROW per uint32 (contract item 6,
// amended), so widening the grid past 32 cells silently drops every column
// past the 32nd -- and dropped columns read as "not the lake", i.e. water that
// should despawn keeps flowing. Caught here, not in a playtest.
static_assert(VoxelFluidSim::kBasinExtentMaskN > 0 && VoxelFluidSim::kBasinExtentMaskN <= 32,
              "kBasinExtentMaskN must fit one row in a uint32");

namespace
{
	// ---- solver-internal constants mirrored from VoxelFluidSim.usf ---------
	// Everything CONTRACT-shaped is mirrored in the header; these are the
	// solver's own knobs, needed CPU-side for sizing and dispatch.

	// VoxelFluidSim.usf VOXEL_FLUID_HASH_CELLS / _SCAN_BLOCK / _GROUP.
	constexpr uint32 kHashCells = 64u * 1024u;
	constexpr uint32 kScanBlock = 256u;
	constexpr uint32 kGroupSize = 64u;
	static_assert(kHashCells == kScanBlock * kScanBlock,
	              "single-workgroup ScanSums covers exactly ScanBlock^2 cells");

	// The shader's kernel-coefficient literals (VoxelFluidSim.usf:70,76),
	// restated ONCE here so the automation tests can pin them against the
	// double-precision derivations in VoxelFluidSim::Poly6CoeffUU et al.
	// 4.10696e-13, corrected from 4.10673e-13 (recycling pass): the original
	// literal was derived from a mis-multiplied 64*pi*h^9 (7.67034e14 for the
	// true 7.66990e14) and sat 5.6e-5 relative off -- found the first time the
	// KernelCoefficients test ran headless.
	constexpr double kShaderPoly6CoeffLiteral = 4.10696e-13;
	constexpr double kShaderSpikyGradCoeffLiteral = 5.86709e-8;

	// ---- shared compile policy ---------------------------------------------
	// SM5, NOT SM6, and that is deliberate honesty rather than a missed memo:
	// the brief's pattern file gates on SM6 because worldgen genuinely does
	// 64-bit integer math (VoxelGpuWorldGen.cpp:66-70). These kernels do
	// float3/uint math only, and the repo's own precedent for such kernels --
	// FVoxelQuadTotalCS at VoxelGpuWorldGen.cpp:232 -- says gating on SM6
	// anyway "would be harmless but dishonest about what it requires".
	class FVoxelFluidShader : public FGlobalShader
	{
	public:
		FVoxelFluidShader() = default;
		FVoxelFluidShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		// COLLISION IS LIVE (integration pass; contract "RATIFIED" item 5).
		// One unconditional define, no permutation dimension: the occupancy
		// volume gates on the same SM5 the solver does
		// (FVoxelFluidOccupancyVolume::IsSupportedOnCurrentRHI), so there is
		// no platform that could run the solver and not the volume -- a
		// permutation would double the compile count to cover a state the
		// host refuses to enter (TickRenderThread skips, counted, when the
		// volume is absent).
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("VOXEL_FLUID_HAS_COLLISION"), 1);
		}
	};

	// --- FluidFrameBeginMain: zero the alive slot ---------------------------
	class FVoxelFluidFrameBeginCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidFrameBeginCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidFrameBeginCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCounts)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidSpawnMain -----------------------------------------------------
	class FVoxelFluidSpawnCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidSpawnCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidSpawnCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SpawnCount)
			SHADER_PARAMETER(uint32, SpawnMode)
			SHADER_PARAMETER(uint32, SpawnSeed)
			SHADER_PARAMETER(uint32, SpawnEdge)
			SHADER_PARAMETER(float, SpawnTimeSec)
			SHADER_PARAMETER(FVector3f, SpawnCenterLocalUU)
			SHADER_PARAMETER(FVector3f, SpawnVelUU)
			SHADER_PARAMETER(FVector3f, SpawnJitterDirUU)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelFluidParticle>, ParticlesRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FreeList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SlotCounters)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidIntegrateMain -------------------------------------------------
	class FVoxelFluidIntegrateCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidIntegrateCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidIntegrateCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SimSlotCount)
			SHADER_PARAMETER(float, Dt)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelFluidParticle>, ParticlesRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPositions)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidHashCountMain -------------------------------------------------
	class FVoxelFluidHashCountCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidHashCountCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidHashCountCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SimSlotCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GridPositions)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellHash)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellRank)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- the 3-kernel scan --------------------------------------------------
	class FVoxelFluidScanBlocksCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidScanBlocksCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidScanBlocksCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellStarts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelFluidScanSumsCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidScanSumsCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidScanSumsCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
			// The sorted arrays' length (total hashed particles), published by
			// the last lane -- the bound every sorted-domain kernel reads.
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SortedCount)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelFluidScanAddCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidScanAddCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidScanAddCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellStarts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BlockSums)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidScatterMain ---------------------------------------------------
	// CellStarts/ParticleCellHash/ParticleCellRank are read-only here but the
	// shader declares them RWStructuredBuffer (one declaration serves every
	// kernel), so they bind as UAVs -- the same note FVoxelMeshCountCS carries
	// at VoxelGpuWorldGen.cpp:148-151.
	// Since the perf pass this kernel also builds the SORTED position array
	// (GridPositions in, OutPositions out): position + packed frozen cell key
	// per hashed particle, cell-grouped, which is what the constraint kernels
	// gather from.
	class FVoxelFluidScatterCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidScatterCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidScatterCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SimSlotCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GridPositions)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellStarts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellEntries)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellHash)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCellRank)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPositions)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidDensityLambdaMain ---------------------------------------------
	// SORTED DOMAIN since the perf pass: InPositions is the cell-grouped
	// (position + frozen key) array, the thread bound is the GPU-side
	// InSortedCount, and the kernel no longer references SimSlotCount,
	// GridPositions or InCellEntries -- removed here rather than left bound,
	// because a bound-but-unread parameter is how a stale binding hides.
	class FVoxelFluidDensityLambdaCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidDensityLambdaCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidDensityLambdaCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, RestDensity)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InPositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCellStarts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCellCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InSortedCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Lambdas)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidDeltaPosMain --------------------------------------------------
	class FVoxelFluidDeltaPosCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidDeltaPosCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidDeltaPosCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, RestDensity)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InPositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InLambdas)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCellStarts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCellCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InSortedCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutPositions)
			// The collision read (VoxelFluidResolveCollision inside the dp
			// clamp). GroundZLocalUU left the struct with the fallback plane:
			// the compiled shader no longer references it and a bound-but-
			// unread parameter is how a stale binding hides.
			VOXEL_FLUID_OCCUPANCY_PARAMETERS()
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- FluidFinalizeMain --------------------------------------------------
	class FVoxelFluidFinalizeCS : public FVoxelFluidShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidFinalizeCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidFinalizeCS, FVoxelFluidShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(float, Dt)
			SHADER_PARAMETER(float, BoundaryHalfExtentUU)
			SHADER_PARAMETER(FVector3f, BoundaryCenterLocalUU)
			SHADER_PARAMETER(uint32, BasinSinkEnabled)
			SHADER_PARAMETER(FVector3f, BasinBoxMinLocalUU)
			SHADER_PARAMETER(FVector3f, BasinBoxMaxLocalUU)
			SHADER_PARAMETER(float, BasinDatumZLocalUU)
			// The sink's TRUE extent (contract item 6, amended): 32 rows of 32
			// bits over the active window. A uniform array, not a buffer -- the
			// whole mask is 128 bytes and every particle reads exactly one row.
			// SCALAR_ARRAY, not ARRAY: a plain uint32[] fails the engine's
			// 16-byte element alignment assert, and the packed form is the same
			// 8 uint4 registers the shader's DECLARE_SCALAR_ARRAY declares.
			SHADER_PARAMETER(FVector2f, BasinMaskOriginLocalUU)
			SHADER_PARAMETER(float, BasinMaskInvCellUU)
			SHADER_PARAMETER_SCALAR_ARRAY(uint32, BasinExtentRows, [VoxelFluidSim::kBasinExtentMaskN])
			// Age sink (contract item 9): population-scaled, stagnant-only
			// recycling. StagnantSpeedUU is the moving/resting divide the
			// finalize kernel refreshes age stamps against.
			SHADER_PARAMETER(float, NowSeconds)
			SHADER_PARAMETER(float, MaxAgeSec)
			SHADER_PARAMETER(float, StagnantSpeedUU)
			SHADER_PARAMETER(uint32, AgePopStart)
			SHADER_PARAMETER(uint32, AgePopEnd)
			// Sorted domain (perf pass): InPositions is the final sorted
			// iterate, InCellEntries the sorted -> slot map corrections are
			// written back through, InSortedCount the thread bound.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, InPositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCellEntries)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InSortedCount)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelFluidParticle>, ParticlesRW)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ParticleCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FreeList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SlotCounters)
			// The full-step anti-tunnelling walk (contract item 7).
			VOXEL_FLUID_OCCUPANCY_PARAMETERS()
		END_SHADER_PARAMETER_STRUCT()
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelFluidFrameBeginCS,    VOXEL_FLUID_SIM_USF, "FluidFrameBeginMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidSpawnCS,         VOXEL_FLUID_SIM_USF, "FluidSpawnMain",         SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidIntegrateCS,     VOXEL_FLUID_SIM_USF, "FluidIntegrateMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidHashCountCS,     VOXEL_FLUID_SIM_USF, "FluidHashCountMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScanBlocksCS,    VOXEL_FLUID_SIM_USF, "FluidScanBlocksMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScanSumsCS,      VOXEL_FLUID_SIM_USF, "FluidScanSumsMain",      SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScanAddCS,       VOXEL_FLUID_SIM_USF, "FluidScanAddMain",       SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidScatterCS,       VOXEL_FLUID_SIM_USF, "FluidScatterMain",       SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidDensityLambdaCS, VOXEL_FLUID_SIM_USF, "FluidDensityLambdaMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidDeltaPosCS,      VOXEL_FLUID_SIM_USF, "FluidDeltaPosMain",      SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidFinalizeCS,      VOXEL_FLUID_SIM_USF, "FluidFinalizeMain",      SF_Compute);

// Here and not in the header: this TU has the complete FRDGPooledBuffer type.
// See the matching comment in VoxelFluidSim.h.
FVoxelFluidSimState::FVoxelFluidSimState() = default;
FVoxelFluidSimState::~FVoxelFluidSimState() = default;

// ---- pure math mirrors (unit-tested) ---------------------------------------

double VoxelFluidSim::Poly6CoeffUU()
{
	const double H = double(kKernelHUU);
	return 315.0 / (64.0 * UE_DOUBLE_PI * FMath::Pow(H, 9.0));
}

double VoxelFluidSim::SpikyGradCoeffUU()
{
	const double H = double(kKernelHUU);
	return 45.0 / (UE_DOUBLE_PI * FMath::Pow(H, 6.0));
}

double VoxelFluidSim::Poly6UU(double R2)
{
	const double H2 = double(kKernelHUU) * double(kKernelHUU);
	if (R2 >= H2)
	{
		return 0.0;
	}
	const double T = H2 - R2;
	return Poly6CoeffUU() * T * T * T;
}

int32 VoxelFluidSim::RestLatticeNeighbourCount()
{
	// All integer lattice offsets of kRestSpacingUU strictly inside radius
	// kKernelHUU, self included. h/spacing = 2.5, so offsets live in [-2, 2].
	int32 Count = 0;
	for (int32 X = -2; X <= 2; ++X)
	for (int32 Y = -2; Y <= 2; ++Y)
	for (int32 Z = -2; Z <= 2; ++Z)
	{
		const double R2 = double(X * X + Y * Y + Z * Z)
		                * double(kRestSpacingUU) * double(kRestSpacingUU);
		if (R2 < double(kKernelHUU) * double(kKernelHUU))
		{
			++Count;
		}
	}
	return Count;
}

float VoxelFluidSim::ComputeRestDensity()
{
	double Sum = 0.0;
	for (int32 X = -2; X <= 2; ++X)
	for (int32 Y = -2; Y <= 2; ++Y)
	for (int32 Z = -2; Z <= 2; ++Z)
	{
		const double R2 = double(X * X + Y * Y + Z * Z)
		                * double(kRestSpacingUU) * double(kRestSpacingUU);
		Sum += Poly6UU(R2);
	}
	return float(Sum);
}

uint32 VoxelFluidSim::CubeEdgeForCount(uint32 Count)
{
	if (Count == 0)
	{
		return 0;
	}
	uint32 Edge = uint32(FMath::CeilToInt(FMath::Pow(double(Count), 1.0 / 3.0)));
	// Float cube roots are not exact; walk to the smallest edge whose cube
	// holds Count rather than trusting the rounding either way.
	while (Edge > 1 && (Edge - 1) * (Edge - 1) * (Edge - 1) >= Count)
	{
		--Edge;
	}
	while (uint64(Edge) * Edge * Edge < Count)
	{
		++Edge;
	}
	return Edge;
}

// ---- the tick --------------------------------------------------------------

namespace
{
	FIntVector GroupsForSlots(uint32 Slots)
	{
		return FIntVector(FMath::DivideAndRoundUp(Slots, kGroupSize), 1, 1);
	}

	// Polls every outstanding counts/debug readback and timing query into the
	// snapshot. Non-blocking throughout: a result that is not ready stays in
	// flight for the next tick's poll.
	void PollCompletions(FVoxelFluidSimState& State)
	{
		// Timing first, so a counts snapshot updated below carries the newest
		// completed GPU time.
		float NewGpuMs = -1.0f;
		for (auto& Pair : State.TimingRing)
		{
			if (!Pair.bInFlight)
			{
				continue;
			}
			uint64 BeginMicros = 0;
			uint64 EndMicros = 0;
			// End was issued after Begin, so if End has a result, Begin does.
			if (RHIGetRenderQueryResult(Pair.End.GetReference(), EndMicros, false) &&
			    RHIGetRenderQueryResult(Pair.Begin.GetReference(), BeginMicros, false))
			{
				NewGpuMs = float(double(EndMicros - BeginMicros) / 1000.0);
				Pair.bInFlight = false;
			}
		}

		for (auto& Slot : State.CountsRing)
		{
			if (!Slot.bInFlight || !Slot.Readback->IsReady())
			{
				continue;
			}
			uint32 Counts[5] = { 0, 0, 0, 0, 0 };
			const void* Src = Slot.Readback->Lock(sizeof(Counts));
			if (Src != nullptr)
			{
				FMemory::Memcpy(Counts, Src, sizeof(Counts));
				Slot.Readback->Unlock();

				FScopeLock Lock(&State.SnapshotLock);
				// Late arrivals must never regress a newer snapshot.
				if (Slot.Generation >= State.LatestCounts.Generation)
				{
					State.LatestCounts.Alive = Counts[0];
					State.LatestCounts.DespawnedBasin = Counts[1];
					State.LatestCounts.DespawnedBoundary = Counts[2];
					State.LatestCounts.SpawnedTotal = Counts[3];
					State.LatestCounts.RecycledAge = Counts[4];
					State.LatestCounts.Generation = Slot.Generation;
					State.LatestCounts.bValid = true;
				}
			}
			Slot.bInFlight = false;
		}

		if (NewGpuMs >= 0.0f)
		{
			FScopeLock Lock(&State.SnapshotLock);
			State.LatestCounts.SimGpuMs = NewGpuMs;
		}

		if (State.bOccupancyReadbackInFlight && State.OccupancyReadback->IsReady())
		{
			const uint64 Bytes = uint64(vxc::kFluidVolumeWords) * sizeof(uint32);
			const void* Src = State.OccupancyReadback->Lock(uint32(Bytes));
			if (Src != nullptr)
			{
				TArray<uint32> Words;
				Words.SetNumUninitialized(int32(vxc::kFluidVolumeWords));
				FMemory::Memcpy(Words.GetData(), Src, Bytes);
				State.OccupancyReadback->Unlock();

				FScopeLock Lock(&State.SnapshotLock);
				State.OccupancyVerifyWords = MoveTemp(Words);
				State.OccupancyVerifySnapshotGeneration = State.OccupancyReadbackGeneration;
			}
			State.bOccupancyReadbackInFlight = false;
		}

		if (State.bDebugReadbackInFlight && State.DebugReadback->IsReady())
		{
			const uint32 Bytes = State.DebugReadbackSlots * sizeof(VoxelFluidSim::FParticleCPU);
			const void* Src = State.DebugReadback->Lock(Bytes);
			if (Src != nullptr)
			{
				const auto* P = static_cast<const VoxelFluidSim::FParticleCPU*>(Src);
				TArray<FVector3f> Positions;
				Positions.Reserve(State.DebugReadbackSlots);
				for (uint32 i = 0; i < State.DebugReadbackSlots; ++i)
				{
					if (P[i].Flags & VoxelFluidSim::kFlagAlive)
					{
						Positions.Add(P[i].Pos);
					}
				}
				State.DebugReadback->Unlock();

				FScopeLock Lock(&State.SnapshotLock);
				State.DebugAlivePositionsLocal = MoveTemp(Positions);
				State.DebugSnapshotGeneration = State.DebugReadbackGeneration;
			}
			State.bDebugReadbackInFlight = false;
		}
	}
}

void VoxelFluidSim::AddSimPasses(FRDGBuilder& GraphBuilder,
                                 FVoxelFluidSimState& State,
                                 const FVoxelFluidSimTickArgs& InArgs)
{
	check(IsInRenderingThread());

	FVoxelFluidSimTickArgs Args = InArgs;
	Args.Iterations = FMath::Clamp(Args.Iterations, 1, 8);
	Args.SimSlotBound = FMath::Min(Args.SimSlotBound, kMaxParticles);

	// COLLISION IS NOT OPTIONAL. The kernels are compiled with the occupancy
	// read baked in (ModifyCompilationEnvironment above), so a tick without a
	// volume is refused and COUNTED rather than run against unbound bits.
	// A skipped tick freezes the water for a frame, which is the designed
	// "blocked, never guessed" behaviour; the counter keeps it visible.
	if (Args.Occupancy == nullptr || !FVoxelFluidOccupancyVolume::IsSupportedOnCurrentRHI())
	{
		{
			FScopeLock Lock(&State.SnapshotLock);
			State.TicksSkippedNoOccupancy++;
			State.TicksSkippedNoOccupancySnapshot = State.TicksSkippedNoOccupancy;
		}
		PollCompletions(State);
		return;
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelFluidSim, "VoxelFluidSim");

	// ---- occupancy volume: clear + queued region fills, FIRST ---------------
	// Same graph, before any solver pass, or a substep collides against last
	// frame's terrain -- and RDG will NOT catch that (a stale-but-valid buffer
	// read is not an error). Enforced, not trusted: the AddPassesCount guard
	// below fails the build of any refactor that moves the fill into a
	// different graph or after the solver passes.
	//
	// AND BEFORE THE NOTHING-TO-SIMULATE EARLY-OUT BELOW, which is the half
	// that was missing (measured 2026-08-10, Saved/owner-playtest-round3.log --
	// see ShouldTickSim in VoxelFluidSim.h for the full account). Building the
	// volume is not part of simulating the water: it is the precondition for
	// there being any. Faucets refuse to emit into unbuilt occupancy, so a
	// solver that only maintains the volume once particles exist can never get
	// its first particle -- 8.5 minutes of that run's log with zero AddPasses
	// calls and every faucet deferring. An idle tick now costs one clear (once)
	// plus up to MaxRegionsPerFlush fills and nothing else.
	const uint64 OccupancySerialBefore = Args.Occupancy->GetStats().AddPassesCount;
	FRDGBufferRef OccupancyBits = Args.Occupancy->AddPasses(GraphBuilder);
	checkf(Args.Occupancy->GetStats().AddPassesCount == OccupancySerialBefore + 1,
	       TEXT("occupancy AddPasses must run exactly once, in this graph, before the solver passes"));
	if (OccupancyBits == nullptr)
	{
		// The RHI refused the volume (no compute). Counted, not silent.
		{
			FScopeLock Lock(&State.SnapshotLock);
			State.TicksSkippedNoOccupancy++;
			State.TicksSkippedNoOccupancySnapshot = State.TicksSkippedNoOccupancy;
		}
		// The clear/fill passes already added stay valid in the caller's graph.
		PollCompletions(State);
		return;
	}

	if (Args.Dt <= 0.0f || Args.SimSlotBound == 0)
	{
		// No SOLVER work can run (velocity derivation divides by Dt; zero slots
		// means nothing has ever spawned). The occupancy passes above stay in
		// the caller's graph -- that is the point of this ordering. Still poll,
		// so results already in flight keep landing.
		PollCompletions(State);
		return;
	}

	State.TickGeneration++;
	// Publish the splat width for the screen-space renderer (see the member's
	// comment in VoxelFluidSim.h). Set before the graph builds so the same
	// frame's render pass sees this tick's bound.
	State.RenderSlotBound = Args.SimSlotBound;

	// ---- persistent buffers -----------------------------------------------
	// Allocated once, cleared once, re-registered every frame. See the
	// lifetime doctrine at the top of VoxelFluidSim.h.
	const bool bFirstFrame = !State.bBuffersInitialized;
	if (bFirstFrame)
	{
		State.Particles = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FParticleCPU), kMaxParticles),
			TEXT("VoxelFluid.Particles"));
		State.Counts = AllocatePooledBuffer(
			// 5 slots: alive, despawnedBasin, despawnedBoundary, spawnedTotal,
			// despawnedAge (contract lines 60-70).
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 5),
			TEXT("VoxelFluid.ParticleCounts"));
		State.FreeList = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), kMaxParticles),
			TEXT("VoxelFluid.FreeList"));
		State.SlotCounters = AllocatePooledBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2),
			TEXT("VoxelFluid.SlotCounters"));
		State.bBuffersInitialized = true;
		UE_LOG(LogVoxelFluid, Display,
		       TEXT("Fluid sim buffers allocated: particles %u KB, freelist %u KB (cap %u particles)"),
		       kMaxParticles * 32 / 1024, kMaxParticles * 4 / 1024, kMaxParticles);
	}

	FRDGBufferRef ParticlesRDG = GraphBuilder.RegisterExternalBuffer(State.Particles, TEXT("VoxelFluid.Particles"));
	FRDGBufferRef CountsRDG = GraphBuilder.RegisterExternalBuffer(State.Counts, TEXT("VoxelFluid.ParticleCounts"));
	FRDGBufferRef FreeListRDG = GraphBuilder.RegisterExternalBuffer(State.FreeList, TEXT("VoxelFluid.FreeList"));
	FRDGBufferRef SlotCountersRDG = GraphBuilder.RegisterExternalBuffer(State.SlotCounters, TEXT("VoxelFluid.SlotCounters"));

	if (bFirstFrame)
	{
		// One-time zeroing: all flags dead, all counters zero. The free list
		// body needs no clear -- entries above the (zeroed) top are never read.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ParticlesRDG), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CountsRDG), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SlotCountersRDG), 0u);
	}

	// ---- transient buffers (die with the graph; sized to the slot bound) ---
	const uint32 Slots = Args.SimSlotBound;
	// PredictedG is SLOT-indexed (integrate output, the grid build's input);
	// SortedA/SortedB are the CELL-GROUPED constraint ping-pong the scatter
	// seeds (position + packed frozen cell key in w) -- the perf pass moved
	// the whole constraint loop into sorted index space so neighbour gathers
	// read contiguous float4 runs instead of hash-scatter-ordered slots.
	FRDGBufferRef PredictedG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(16, Slots), TEXT("VoxelFluid.Predicted0"));
	FRDGBufferRef SortedA = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(16, Slots), TEXT("VoxelFluid.SortedA"));
	FRDGBufferRef SortedB = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(16, Slots), TEXT("VoxelFluid.SortedB"));
	FRDGBufferRef LambdasRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, Slots), TEXT("VoxelFluid.Lambdas"));
	FRDGBufferRef CellHashRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, Slots), TEXT("VoxelFluid.CellHash"));
	FRDGBufferRef CellRankRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, Slots), TEXT("VoxelFluid.CellRank"));
	FRDGBufferRef CellEntriesRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, Slots), TEXT("VoxelFluid.CellEntries"));
	FRDGBufferRef CellCountsRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, kHashCells), TEXT("VoxelFluid.CellCounts"));
	FRDGBufferRef CellStartsRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, kHashCells), TEXT("VoxelFluid.CellStarts"));
	FRDGBufferRef BlockSumsRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, kScanBlock), TEXT("VoxelFluid.BlockSums"));
	// One uint: the sorted arrays' length this frame, written by ScanSums and
	// read by every sorted-domain kernel as its thread bound. GPU-owned so the
	// CPU never needs to know the alive count to dispatch correctly.
	FRDGBufferRef SortedCountRDG = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(4, 1), TEXT("VoxelFluid.SortedCount"));

	// ---- GPU timing bracket (begin) ---------------------------------------
	// RQT_AbsoluteTime pair as untracked NeverCull passes. Same-pipe compute
	// passes execute in declaration order here (no async compute flags
	// anywhere in this graph), so the bracket covers exactly this graph's
	// passes plus the 16-byte counts copy -- stated in the measurement spec.
	FVoxelFluidSimState::FTimingPair* Timing = nullptr;
	// DEFAULT OFF PENDING A BREADCRUMB-SAFE REWRITE. The raw
	// Begin/EndRenderQuery passes tripped RDG's breadcrumb sentinel assert on
	// the very first Execute (RenderGraphBuilder.cpp:1772, UE 5.8) and took
	// the whole editor down -- measured, not theorised: the 100k gate run
	// crashed 2 s in, and with this bracket skipped it survives. Until the
	// bracket is rebuilt on an RDG-sanctioned path, simGpuMs reads -1 and the
	// gate metric is the A/B frame-time delta the measurement plan already
	// defines. Opt back in with voxel.Fluid.GpuTiming 1 to reproduce.
	static const auto* CVarGpuTiming =
		IConsoleManager::Get().FindConsoleVariable(TEXT("voxel.Fluid.GpuTiming"));
	const bool bGpuTiming = CVarGpuTiming != nullptr && CVarGpuTiming->GetInt() != 0;
	if (bGpuTiming && GSupportsTimestampRenderQueries)
	{
		for (auto& Pair : State.TimingRing)
		{
			if (!Pair.bInFlight)
			{
				Timing = &Pair;
				break;
			}
		}
	}
	if (Timing != nullptr)
	{
		if (!Timing->Begin.IsValid())
		{
			Timing->Begin = RHICreateRenderQuery(RQT_AbsoluteTime);
			Timing->End = RHICreateRenderQuery(RQT_AbsoluteTime);
		}
		FRHIRenderQuery* Query = Timing->Begin.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelFluid.TimeBegin"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& InRHICmdList)
			{
				InRHICmdList.EndRenderQuery(Query);
			});
	}

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CellCountsRDG), 0u);

	// ---- pass 1: frame begin (zero the alive slot) ------------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidFrameBeginCS::FParameters>();
		Params->ParticleCounts = GraphBuilder.CreateUAV(CountsRDG);
		TShaderMapRef<FVoxelFluidFrameBeginCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.FrameBegin"),
		                             Shader, Params, FIntVector(1, 1, 1));
	}

	// ---- pass 2: spawn dispatches (0..N per tick) -------------------------
	// One dispatch per request; RDG serialises them on the shared UAVs, which
	// is required -- two spawn passes race on the freelist top otherwise. The
	// subsystem bounds the list (faucet count is bounded by the emit budget).
	for (const FVoxelFluidSpawnRequest& Spawn : Args.Spawns)
	{
		if (Spawn.Count == 0)
		{
			continue;
		}
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidSpawnCS::FParameters>();
		Params->SpawnCount = Spawn.Count;
		Params->SpawnMode = Spawn.Mode;
		Params->SpawnSeed = Spawn.Seed;
		Params->SpawnEdge = CubeEdgeForCount(Spawn.Count);
		Params->SpawnTimeSec = Spawn.SpawnTimeSec;
		Params->SpawnCenterLocalUU = Spawn.CenterLocalUU;
		Params->SpawnVelUU = Spawn.VelocityUU;
		Params->SpawnJitterDirUU = Spawn.JitterDirUU;
		Params->ParticlesRW = GraphBuilder.CreateUAV(ParticlesRDG);
		Params->ParticleCounts = GraphBuilder.CreateUAV(CountsRDG);
		Params->FreeList = GraphBuilder.CreateUAV(FreeListRDG);
		Params->SlotCounters = GraphBuilder.CreateUAV(SlotCountersRDG);
		TShaderMapRef<FVoxelFluidSpawnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder,
		                             RDG_EVENT_NAME("VoxelFluid.Spawn(%u)", Spawn.Count),
		                             Shader, Params, GroupsForSlots(Spawn.Count));
	}

	// ---- pass 3: integrate -------------------------------------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidIntegrateCS::FParameters>();
		Params->SimSlotCount = Slots;
		Params->Dt = Args.Dt;
		Params->ParticlesRW = GraphBuilder.CreateUAV(ParticlesRDG);
		Params->OutPositions = GraphBuilder.CreateUAV(PredictedG);
		TShaderMapRef<FVoxelFluidIntegrateCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.Integrate"),
		                             Shader, Params, GroupsForSlots(Slots));
	}

	// ---- passes 4-8: hash grid (count, 3-kernel scan, scatter) ------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidHashCountCS::FParameters>();
		Params->SimSlotCount = Slots;
		Params->GridPositions = GraphBuilder.CreateSRV(PredictedG);
		Params->CellCounts = GraphBuilder.CreateUAV(CellCountsRDG);
		Params->ParticleCellHash = GraphBuilder.CreateUAV(CellHashRDG);
		Params->ParticleCellRank = GraphBuilder.CreateUAV(CellRankRDG);
		TShaderMapRef<FVoxelFluidHashCountCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.HashCount"),
		                             Shader, Params, GroupsForSlots(Slots));
	}
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidScanBlocksCS::FParameters>();
		Params->CellCounts = GraphBuilder.CreateUAV(CellCountsRDG);
		Params->CellStarts = GraphBuilder.CreateUAV(CellStartsRDG);
		Params->BlockSums = GraphBuilder.CreateUAV(BlockSumsRDG);
		TShaderMapRef<FVoxelFluidScanBlocksCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.ScanBlocks"),
		                             Shader, Params, FIntVector(kHashCells / kScanBlock, 1, 1));
	}
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidScanSumsCS::FParameters>();
		Params->BlockSums = GraphBuilder.CreateUAV(BlockSumsRDG);
		Params->SortedCount = GraphBuilder.CreateUAV(SortedCountRDG);
		TShaderMapRef<FVoxelFluidScanSumsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.ScanSums"),
		                             Shader, Params, FIntVector(1, 1, 1)); // one group, by design
	}
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidScanAddCS::FParameters>();
		Params->CellStarts = GraphBuilder.CreateUAV(CellStartsRDG);
		Params->BlockSums = GraphBuilder.CreateUAV(BlockSumsRDG);
		TShaderMapRef<FVoxelFluidScanAddCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.ScanAdd"),
		                             Shader, Params, FIntVector(kHashCells / kScanBlock, 1, 1));
	}
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidScatterCS::FParameters>();
		Params->SimSlotCount = Slots;
		Params->GridPositions = GraphBuilder.CreateSRV(PredictedG);
		Params->CellStarts = GraphBuilder.CreateUAV(CellStartsRDG);
		Params->CellEntries = GraphBuilder.CreateUAV(CellEntriesRDG);
		Params->ParticleCellHash = GraphBuilder.CreateUAV(CellHashRDG);
		Params->ParticleCellRank = GraphBuilder.CreateUAV(CellRankRDG);
		Params->OutPositions = GraphBuilder.CreateUAV(SortedA); // seeds the sorted ping-pong
		TShaderMapRef<FVoxelFluidScatterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.Scatter"),
		                             Shader, Params, GroupsForSlots(Slots));
	}

	// ---- passes 9..: constraint iterations --------------------------------
	// SORTED-SPACE ping-pong (the perf pass): the scatter seeded SortedA with
	// cell-grouped (position + frozen key) entries; iteration 0 reads A and
	// writes B, then B<->A. Neighbour topology stays frozen throughout -- it
	// rides in each entry's w lane, which DeltaPos copies forward -- so no
	// kernel here binds the slot-space predictions at all. Thread bounds are
	// the GPU-side SortedCount; the dispatch width stays the slot bound
	// (sorted count <= slots always, extra lanes early-out on one cached read).
	const float RestDensity = ComputeRestDensity();
	FRDGBufferRef CurIn = SortedA;
	FRDGBufferRef CurOut = SortedB;
	for (int32 It = 0; It < Args.Iterations; ++It)
	{
		{
			auto* Params = GraphBuilder.AllocParameters<FVoxelFluidDensityLambdaCS::FParameters>();
			Params->RestDensity = RestDensity;
			Params->InPositions = GraphBuilder.CreateSRV(CurIn);
			Params->InCellStarts = GraphBuilder.CreateSRV(CellStartsRDG);
			Params->InCellCounts = GraphBuilder.CreateSRV(CellCountsRDG);
			Params->InSortedCount = GraphBuilder.CreateSRV(SortedCountRDG);
			Params->Lambdas = GraphBuilder.CreateUAV(LambdasRDG);
			TShaderMapRef<FVoxelFluidDensityLambdaCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder,
			                             RDG_EVENT_NAME("VoxelFluid.DensityLambda(it %d)", It),
			                             Shader, Params, GroupsForSlots(Slots));
		}
		{
			auto* Params = GraphBuilder.AllocParameters<FVoxelFluidDeltaPosCS::FParameters>();
			Params->RestDensity = RestDensity;
			Params->InPositions = GraphBuilder.CreateSRV(CurIn);
			Params->InLambdas = GraphBuilder.CreateSRV(LambdasRDG);
			Params->InCellStarts = GraphBuilder.CreateSRV(CellStartsRDG);
			Params->InCellCounts = GraphBuilder.CreateSRV(CellCountsRDG);
			Params->InSortedCount = GraphBuilder.CreateSRV(SortedCountRDG);
			Params->OutPositions = GraphBuilder.CreateUAV(CurOut);
			Args.Occupancy->BindShaderParameters(GraphBuilder, *Params);
			TShaderMapRef<FVoxelFluidDeltaPosCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder,
			                             RDG_EVENT_NAME("VoxelFluid.DeltaPos(it %d)", It),
			                             Shader, Params, GroupsForSlots(Slots));
		}
		const FRDGBufferRef Next = CurIn;
		CurIn = CurOut;
		CurOut = Next;
	}

	// ---- pass: finalize ----------------------------------------------------
	{
		auto* Params = GraphBuilder.AllocParameters<FVoxelFluidFinalizeCS::FParameters>();
		Params->Dt = Args.Dt;
		Params->BoundaryHalfExtentUU = Args.BoundaryHalfExtentUU;
		Params->BoundaryCenterLocalUU = Args.BoundaryCenterLocalUU;
		Params->BasinSinkEnabled = Args.bBasinSinkEnabled ? 1u : 0u;
		Params->BasinBoxMinLocalUU = Args.BasinBoxMinLocalUU;
		Params->BasinBoxMaxLocalUU = Args.BasinBoxMaxLocalUU;
		Params->BasinDatumZLocalUU = Args.BasinDatumZLocalUU;
		Params->BasinMaskOriginLocalUU = Args.BasinMaskOriginLocalUU;
		Params->BasinMaskInvCellUU = Args.BasinMaskInvCellUU;
		// Copied even when the sink is off: a shader parameter array left
		// uninitialised is last frame's bits, and BasinSinkEnabled is the only
		// thing standing between those and a despawn.
		for (int32 Row = 0; Row < VoxelFluidSim::kBasinExtentMaskN; ++Row)
		{
			GET_SCALAR_ARRAY_ELEMENT(Params->BasinExtentRows, Row) = Args.BasinExtentRows[Row];
		}
		Params->NowSeconds = Args.NowSeconds;
		Params->MaxAgeSec = Args.MaxAgeSec;
		Params->StagnantSpeedUU = Args.StagnantSpeedUU;
		Params->AgePopStart = Args.AgePopStart;
		Params->AgePopEnd = Args.AgePopEnd;
		Params->InPositions = GraphBuilder.CreateSRV(CurIn);
		Params->InCellEntries = GraphBuilder.CreateSRV(CellEntriesRDG);
		Params->InSortedCount = GraphBuilder.CreateSRV(SortedCountRDG);
		Params->ParticlesRW = GraphBuilder.CreateUAV(ParticlesRDG);
		Params->ParticleCounts = GraphBuilder.CreateUAV(CountsRDG);
		Params->FreeList = GraphBuilder.CreateUAV(FreeListRDG);
		Params->SlotCounters = GraphBuilder.CreateUAV(SlotCountersRDG);
		Args.Occupancy->BindShaderParameters(GraphBuilder, *Params);
		TShaderMapRef<FVoxelFluidFinalizeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("VoxelFluid.Finalize"),
		                             Shader, Params, GroupsForSlots(Slots));
	}

	// ---- readbacks ---------------------------------------------------------
	// The 20-byte counts copy, every frame -- the 4-byte QuadTotal pattern
	// (VoxelGpuMeshJobManager.cpp:205-217) at 5x the width. This is the whole
	// per-frame CPU cost of the conservation assert.
	{
		FVoxelFluidSimState::FCountsReadback* Free = nullptr;
		for (auto& Slot : State.CountsRing)
		{
			if (!Slot.bInFlight)
			{
				Free = &Slot;
				break;
			}
		}
		if (Free != nullptr)
		{
			if (!Free->Readback.IsValid())
			{
				Free->Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelFluid.CountsReadback"));
			}
			AddEnqueueCopyPass(GraphBuilder, Free->Readback.Get(), CountsRDG, 5 * sizeof(uint32));
			Free->Generation = State.TickGeneration;
			Free->bInFlight = true;
		}
		else
		{
			// Counted, not silent: if this climbs, the GPU is >2 frames behind
			// and the perf line's counts are correspondingly stale.
			State.CountsReadbackSkips++;
		}
	}

	if (Args.bReadbackDebugSlots && !State.bDebugReadbackInFlight)
	{
		const uint32 DebugSlots =
			FMath::Min(FMath::Min(Args.DebugSlotCount, FVoxelFluidSimState::kDebugMaxSlots), Slots);
		if (DebugSlots > 0)
		{
			if (!State.DebugReadback.IsValid())
			{
				State.DebugReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelFluid.DebugReadback"));
			}
			AddEnqueueCopyPass(GraphBuilder, State.DebugReadback.Get(), ParticlesRDG,
			                   DebugSlots * sizeof(FParticleCPU));
			State.DebugReadbackSlots = DebugSlots;
			State.DebugReadbackGeneration = State.TickGeneration;
			State.bDebugReadbackInFlight = true;
		}
	}

	// The occupancy VERIFY readback (voxel.Fluid.Occupancy.Verify): the whole
	// 16 MiB volume, copied AFTER this graph's fill passes so the snapshot is
	// exactly what this tick's solver collided against. Single-buffered and
	// debug-only; the game thread byte-compares one region per landed snapshot
	// against vxc::fluidFillRegion, the CPU reference.
	if (Args.bVerifyOccupancy && !State.bOccupancyReadbackInFlight)
	{
		if (!State.OccupancyReadback.IsValid())
		{
			State.OccupancyReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("VoxelFluid.OccupancyVerify"));
		}
		AddEnqueueCopyPass(GraphBuilder, State.OccupancyReadback.Get(), OccupancyBits,
		                   uint32(vxc::kFluidVolumeWords * sizeof(uint32)));
		State.OccupancyReadbackGeneration = State.TickGeneration;
		State.bOccupancyReadbackInFlight = true;
	}

	// ---- GPU timing bracket (end) ------------------------------------------
	if (Timing != nullptr)
	{
		FRHIRenderQuery* Query = Timing->End.GetReference();
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelFluid.TimeEnd"), ERDGPassFlags::NeverCull,
			[Query](FRHICommandListImmediate& InRHICmdList)
			{
				InRHICmdList.EndRenderQuery(Query);
			});
		Timing->bInFlight = true;
	}

	PollCompletions(State);
}

void VoxelFluidSim::ReleaseRenderThread(FVoxelFluidSimState& State)
{
	check(IsInRenderingThread());
	State.Particles.SafeRelease();
	State.Counts.SafeRelease();
	State.FreeList.SafeRelease();
	State.SlotCounters.SafeRelease();
	for (auto& Slot : State.CountsRing)
	{
		Slot.Readback.Reset();
		Slot.bInFlight = false;
	}
	State.DebugReadback.Reset();
	State.bDebugReadbackInFlight = false;
	State.OccupancyReadback.Reset();
	State.bOccupancyReadbackInFlight = false;
	// The renderer gates on Particles validity AND this; zero both so a
	// released state can never advertise stale splat work.
	State.RenderSlotBound = 0;
	for (auto& Pair : State.TimingRing)
	{
		Pair.Begin.SafeRelease();
		Pair.End.SafeRelease();
		Pair.bInFlight = false;
	}
	State.bBuffersInitialized = false;
}

// ---- automation tests -------------------------------------------------------
// CPU-side pins for every piece of solver logic that does not need a GPU:
// the shader-mirror coefficients, the rest-density lattice, the spawn edge,
// and the conservation predicate the subsystem asserts with. Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.Fluid; Quit"
// (same harness as VoxelGpuGeometryPoolTests.cpp).

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr EAutomationTestFlags kFluidTestFlags = EAutomationTestFlags::EditorContext
	                                               | EAutomationTestFlags::ClientContext
	                                               | EAutomationTestFlags::EngineFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidKernelCoeffTest,
	"VoxelEarth.Fluid.KernelCoefficients", kFluidTestFlags)

bool FVoxelFluidKernelCoeffTest::RunTest(const FString& Parameters)
{
	// The shader carries these as float literals (VoxelFluidSim.usf); if
	// either drifts from its derivation the density constraint is silently
	// wrong everywhere. 1e-5 relative -- literal precision, not float math.
	const double Poly6 = VoxelFluidSim::Poly6CoeffUU();
	const double Spiky = VoxelFluidSim::SpikyGradCoeffUU();
	TestTrue(TEXT("poly6 literal matches derivation"),
	         FMath::Abs(Poly6 - kShaderPoly6CoeffLiteral) / Poly6 < 1e-5);
	TestTrue(TEXT("spiky grad literal matches derivation"),
	         FMath::Abs(Spiky - kShaderSpikyGradCoeffLiteral) / Spiky < 1e-5);

	// Kernel shape sanity: positive inside support, exactly zero at/after h.
	TestTrue(TEXT("W(0) > 0"), VoxelFluidSim::Poly6UU(0.0) > 0.0);
	TestTrue(TEXT("W monotone"), VoxelFluidSim::Poly6UU(100.0) > VoxelFluidSim::Poly6UU(400.0));
	TestEqual(TEXT("W(h^2) == 0"), VoxelFluidSim::Poly6UU(625.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidRestDensityTest,
	"VoxelEarth.Fluid.RestDensity", kFluidTestFlags)

bool FVoxelFluidRestDensityTest::RunTest(const FString& Parameters)
{
	// h = 2.5x spacing puts exactly 81 lattice sites (offsets with
	// i^2+j^2+k^2 <= 6) inside the kernel support, self included.
	TestEqual(TEXT("rest lattice site count"), VoxelFluidSim::RestLatticeNeighbourCount(), 81);

	// The summed kernel must approximate the ideal number density
	// 1/spacing^3 = 1e-3 per UU^3 -- that agreement is what makes
	// "density / RestDensity - 1" a meaningful constraint. Within 5%.
	const double Rest = double(VoxelFluidSim::ComputeRestDensity());
	const double Ideal = 1.0 / FMath::Pow(double(VoxelFluidSim::kRestSpacingUU), 3.0);
	TestTrue(TEXT("rest density near ideal number density"),
	         FMath::Abs(Rest / Ideal - 1.0) < 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidCubeEdgeTest,
	"VoxelEarth.Fluid.CubeEdge", kFluidTestFlags)

bool FVoxelFluidCubeEdgeTest::RunTest(const FString& Parameters)
{
	const uint32 Cases[] = { 1, 2, 7, 8, 9, 26, 27, 28, 999, 1000, 5000, 100000, 300 * 1024 };
	for (uint32 Count : Cases)
	{
		const uint32 Edge = VoxelFluidSim::CubeEdgeForCount(Count);
		TestTrue(FString::Printf(TEXT("edge^3 holds %u"), Count),
		         uint64(Edge) * Edge * Edge >= Count);
		if (Edge > 1)
		{
			TestTrue(FString::Printf(TEXT("edge minimal for %u"), Count),
			         uint64(Edge - 1) * (Edge - 1) * (Edge - 1) < Count);
		}
	}
	TestEqual(TEXT("zero count -> zero edge"), VoxelFluidSim::CubeEdgeForCount(0), 0u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidTickGateTest,
	"VoxelEarth.Fluid.TickGate", kFluidTestFlags)

bool FVoxelFluidTickGateTest::RunTest(const FString& Parameters)
{
	using VoxelFluidSim::ShouldTickSim;
	const float Dt = 1.0f / 60.0f;

	// THE REGRESSION THIS PINS (Saved/owner-playtest-round3.log, 2026-08-10).
	// The gate used to read `bOriginLatched && CumulativeSpawnRequested > 0`,
	// and since the occupancy volume is only ever built inside a sim tick while
	// every faucet refuses to emit into an unbuilt cell, nothing could ever
	// start: 8.5 minutes, spawned=0, 174,840 deferred faucet-ticks, AddPasses
	// never called once. An idle frame MUST still tick.
	TestTrue(TEXT("latched with nothing ever spawned still ticks (builds occupancy)"),
	         ShouldTickSim(/*bOriginLatched*/ true, Dt));
	TestTrue(TEXT("latched with water alive ticks"), ShouldTickSim(true, Dt));

	// The two things that genuinely cannot run.
	TestFalse(TEXT("no origin, no tick"), ShouldTickSim(false, Dt));
	TestFalse(TEXT("zero dt, no tick (velocity derivation divides by it)"),
	          ShouldTickSim(true, 0.0f));
	TestFalse(TEXT("negative dt, no tick"), ShouldTickSim(true, -0.001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidConservationTest,
	"VoxelEarth.Fluid.Conservation", kFluidTestFlags)

bool FVoxelFluidConservationTest::RunTest(const FString& Parameters)
{
	using VoxelFluidSim::CheckConservation;
	// The exact predicate the subsystem asserts with every readback
	// (one definition, VoxelFluidSim.h) -- alive, basin, boundary, spawned.
	TestTrue(TEXT("all zero balances (the idle case)"), CheckConservation(0, 0, 0, 0));
	TestTrue(TEXT("spawned == alive"), CheckConservation(100, 0, 0, 100));
	TestTrue(TEXT("spawn minus despawns"), CheckConservation(70, 10, 20, 100));
	TestFalse(TEXT("leaked particle detected"), CheckConservation(71, 10, 20, 100));
	TestFalse(TEXT("lost particle detected"), CheckConservation(69, 10, 20, 100));
	TestFalse(TEXT("despawn without spawn detected"), CheckConservation(0, 0, 1, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidAgeRecyclingTest,
	"VoxelEarth.Fluid.AgeRecycling", kFluidTestFlags)

bool FVoxelFluidAgeRecyclingTest::RunTest(const FString& Parameters)
{
	// CPU mirror of the finalize kernel's effective-max-age math (contract
	// item 9). The band the subsystem uploads at the DEFAULTS (persistence
	// pass): soft cap voxel.Fluid.SoftCap = 200,000, so PopEnd == the emission
	// backpressure ramp start == SoftCap/2 = 100,000, PopStart == 60% of that
	// = 60,000, and MaxAgeSec defaults to 500 s (stagnant water only -- moving
	// water refreshes its stamp, pinned further down). Pinned here with those
	// real numbers so a drifted constant fails a test instead of shipping as
	// a re-jammed loop.
	using VoxelFluidSim::EffectiveMaxAgeSec;
	using VoxelFluidSim::kAgeScaleFloor;
	constexpr uint32 PopEnd = 100000;                      // emission ramp start
	constexpr uint32 PopStart = (PopEnd * 3u) / 5u;        // 60% of it = 60,000
	constexpr float MaxAge = 500.0f;

	// Full age at and below the start of the band: pockets refresh slowly.
	TestEqual(TEXT("empty sim -> full age"), EffectiveMaxAgeSec(0, PopStart, PopEnd, MaxAge), MaxAge);
	TestEqual(TEXT("at PopStart -> full age"),
	          EffectiveMaxAgeSec(PopStart, PopStart, PopEnd, MaxAge), MaxAge);

	// Linear inside the band; strictly monotone non-increasing.
	const float Mid = EffectiveMaxAgeSec((PopStart + PopEnd) / 2, PopStart, PopEnd, MaxAge);
	TestTrue(TEXT("mid-band shorter than full"), Mid < MaxAge);
	TestTrue(TEXT("mid-band above floor"), Mid > MaxAge * kAgeScaleFloor);
	TestTrue(TEXT("mid-band is the linear midpoint"),
	         FMath::IsNearlyEqual(Mid, MaxAge * 0.5f, MaxAge * 0.01f));

	// At and past PopEnd: floored, never zero and never negative -- a burst
	// over the band drains at a bounded pace instead of flushing instantly.
	TestTrue(TEXT("at PopEnd -> floor"),
	         FMath::IsNearlyEqual(EffectiveMaxAgeSec(PopEnd, PopStart, PopEnd, MaxAge),
	                              MaxAge * kAgeScaleFloor, 1.0e-4f));
	TestTrue(TEXT("far past PopEnd stays at the floor"),
	         FMath::IsNearlyEqual(EffectiveMaxAgeSec(VoxelFluidSim::kMaxParticles, PopStart, PopEnd, MaxAge),
	                              MaxAge * kAgeScaleFloor, 1.0e-4f));

	// Degenerate upload (start == end) must not divide by zero.
	const float Degenerate = EffectiveMaxAgeSec(1000, 500, 500, MaxAge);
	TestTrue(TEXT("degenerate band -> finite, floored"),
	         Degenerate >= MaxAge * kAgeScaleFloor && Degenerate <= MaxAge);

	// THE PROPERTY THE FIX RESTS ON (round-6 jam): at the round-6 inflow
	// (~1,340 particles/s unthrottled demand), sustainable population --
	// inflow x effective age -- must be BELOW the emission ramp start at the
	// ramp start itself, so population can never hold at the ramp and the
	// faucets never throttle. Then walk the actual dynamics
	// (dP/dt = inflow - P / effAge(P)) and confirm it settles under the ramp.
	{
		const float Demand = 1340.0f;
		TestTrue(TEXT("recycling outpaces round-6 demand at the ramp start"),
		         Demand * EffectiveMaxAgeSec(PopEnd, PopStart, PopEnd, MaxAge) < float(PopEnd));

		float Pop = 0.0f;
		const float Dt = 0.1f;
		for (int32 It = 0; It < 20000; ++It) // 2,000 sim-seconds
		{
			const float Age = EffectiveMaxAgeSec(uint32(Pop), PopStart, PopEnd, MaxAge);
			Pop = FMath::Max(0.0f, Pop + (Demand - Pop / Age) * Dt);
		}
		TestTrue(TEXT("round-6 demand settles below the emission ramp start"),
		         Pop < float(PopEnd));
		TestTrue(TEXT("...and above the full-age band (the valve is actually working)"),
		         Pop > float(PopStart));
	}

	// STAGNANT-ONLY AGING (persistence pass, owner directive): the refresh
	// rule the finalize kernel applies before its sinks -- moving water
	// rewrites its stamp to Now, resting water keeps it. Mirrored as
	// VoxelFluidSim::RefreshAgeStamp; the threshold's placement argument
	// lives at the StagnantSpeedUU uniform in VoxelFluidSim.usf.
	{
		using VoxelFluidSim::RefreshAgeStamp;
		const float Threshold = 15.0f; // voxel.Fluid.StagnantSpeedUU default
		const float Born = 100.0f;
		const float Now = 700.0f; // 600 s later -- past even the 500 s default

		// A stream in flight refreshes every frame: its age never exceeds one
		// frame no matter how long ago it was born, so it can never recycle
		// mid-journey (the bug this pass removes).
		const float MovingStamp = RefreshAgeStamp(Born, Now, 150.0f, Threshold);
		TestEqual(TEXT("moving water refreshes its stamp"), MovingStamp, Now);
		TestTrue(TEXT("moving water never exceeds the max age"),
		         Now - MovingStamp < MaxAge);

		// Resting water keeps its stamp and ages out normally.
		const float RestingStamp = RefreshAgeStamp(Born, Now, 3.0f, Threshold);
		TestEqual(TEXT("resting water keeps its stamp"), RestingStamp, Born);
		TestTrue(TEXT("resting water past the max age recycles"),
		         Now - RestingStamp > MaxAge);

		// Strict >: exactly-at-threshold counts as stagnant (matches the
		// shader's comparison, so the mirror cannot drift by an ulp of intent).
		TestEqual(TEXT("at-threshold speed does not refresh"),
		          RefreshAgeStamp(Born, Now, Threshold, Threshold), Born);

		// One frame of free fall at 60 Hz (g*dt = 980/60 = 16.3 UU/s) clears
		// the default threshold: anything with a downhill exit re-arms its
		// stamp on its very next step even from a standing start.
		TestEqual(TEXT("one frame of free fall clears the default threshold"),
		          RefreshAgeStamp(Born, Now, 980.0f / 60.0f, Threshold), Now);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
