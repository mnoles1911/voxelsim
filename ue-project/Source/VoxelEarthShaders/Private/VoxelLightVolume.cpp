// VoxelLightVolume.cpp -- RDG plumbing for the GPU-propagated sunlight volume.
//
// VoxelLightVolume.h owns the design argument (what the volume is, why 1 m
// cells, why 32 bits, the display-only doctrine and the budget arithmetic);
// VoxelLightVolume.usf owns the propagation rule. This file is the wiring:
// cvars, the two parameter structs, the allocation, the recentre, the dirty
// queue and the census line. Shapes are lifted from VoxelShadowMarch.cpp and
// VoxelGIMarchPass.cpp deliberately -- same decline-with-a-named-reason
// discipline, same "counters are counts of dispatched work, the printer
// divides" rule -- so that anyone who has read those has read this.

#include "VoxelLightVolume.h"

#include "VoxelBrickPool.h"       // the seed's traversal source (public seam)
#include "VoxelMarchChunkIndex.h" // and the GPU lookup that makes it probeable

#include "GlobalRenderResources.h" // GBlackVolumeTexture -- the never-null off binding
#include "RenderUtils.h"           // ...and the declaration the GI volume reaches it through
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED / RDG_EVENT_SCOPE_STAT
// DECLARED, NOT BORROWED -- the VoxelBrickPool.h rule applied to this file: it
// names FCriticalSection and FScopeLock, so it includes them rather than
// inheriting them from whatever the unity blob happened to pull in first. That
// trap costs the next person who edits an unrelated file in this module a
// non-unity compile they cannot reproduce.
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#include <atomic>

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FVoxelLightVolumeParameters, "VoxelLightVol");

DEFINE_LOG_CATEGORY_STATIC(LogVoxelLightVolume, Log, All);

DECLARE_GPU_STAT_NAMED(VoxelLightVolume, TEXT("VoxelLightVolume"));

// Macro, not a const TCHAR*: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (the same note VoxelShadowMarch.cpp:51-52 carries).
#define VOXEL_LIGHTVOL_USF "/VoxelEarth/VoxelLightVolume.usf"

namespace
{
	// =======================================================================
	// The arm
	// =======================================================================

	TAutoConsoleVariable<int32> CVarVoxelLightPropagated(
		// DEFAULT 0, AND IT SHIPS DARK ON PURPOSE. This changes the lighting of
		// every voxel surface in the world, and the owner has judged neither the
		// look nor the cost. The plan (docs/vs-lighting-implementation-plan-
		// 2026-09-06.md, phase L3) says so in as many words: "default 0 until the
		// owner judges". Nothing here flips it.
		TEXT("voxel.Light.Propagated"), 0,
		TEXT("Master switch for the GPU-propagated sunlight volume (Phase L3, "
		     "docs/vs-lighting-implementation-plan-2026-09-06.md). 0 (default) = nothing is "
		     "allocated, no pass is added, the marcher does not sample, and the frame is "
		     "byte-identical to a build without this file. 1 = an 8.0 MiB camera-following volume "
		     "is seeded from brick occupancy and relaxed a few iterations per frame, and the "
		     "marcher scales its ambient/wrap share by the sampled sky level -- so light wraps "
		     "into overhangs and dies in caves instead of lifting both by the same flat constant. "
		     "READ THE CENSUS LINE ('[voxel-light] census') before believing a frame: an armed "
		     "volume that relaxed zero cells looks exactly like a subtle lighting change."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelLightRelaxIterations(
		// 2 IS A STARTING POINT, NOT A MEASUREMENT. The propagation converges in
		// roughly (reach in cells / 1) iterations -- light crosses one cell per
		// iteration by construction -- so a 12 m wrap needs ~12 iterations to
		// settle from a cold seed. At 2 per frame that is six frames, which is
		// the "light settles in" behaviour the plan explicitly accepts (and which
		// VS's own chunk relighting shows too). Raise it to trade frame time for
		// settling speed; the leg measures which side of that trade this box is
		// on.
		TEXT("voxel.Light.RelaxIterations"), 2,
		TEXT("Relax iterations dispatched per frame while the volume is dirty (Phase L3). Each "
		     "iteration is one 6-neighbour max-minus-absorption sweep of the whole 128x128x64 "
		     "volume and moves light exactly one cell (1 m). 0 disarms the relax while leaving "
		     "the seed live -- a bisection state, and a useful one: it shows the raw sky mask "
		     "with no wrap at all. Clamped to [0, 16]. ZERO ITERATIONS ARE ALSO WHAT A SETTLED "
		     "WORLD RUNS, which is where the amortized budget actually comes from -- a still "
		     "camera in a streamed-in world dispatches nothing."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelLightAbsorptionCells(
		// 12 CELLS = 12 m, AND IT IS AN APPEARANCE KNOB, NOT A PHYSICAL ONE.
		// VS loses one of 32 levels per block, i.e. sky light reaches ~32 blocks
		// around a corner. Our cells are 1 m against their ~1 m block, so 12 is
		// deliberately SHORTER than VS's: their number is tuned for caves lit by
		// a shaft three blocks wide, ours has to look right against terrain
		// overhangs at 65% internal resolution. The owner's ladder moves it.
		TEXT("voxel.Light.AbsorptionCells"), 12.0f,
		TEXT("How far sky light reaches around a corner, in CELLS (1 m each): the propagation "
		     "loses 1/this of full brightness per cell of travel. Smaller = light dies faster and "
		     "overhangs go dark sooner; larger = the field flattens toward today's uniform "
		     "ambient, and at a very large value this feature stops doing anything at all "
		     "(which is a legitimate control arm). Clamped to [1, 64]."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelLightStrength(
		TEXT("voxel.Light.Strength"), 1.0f,
		TEXT("How much of the marcher's ambient/wrap share the sampled sky level is allowed to "
		     "scale (Phase L3). 0 = the volume is built, bound and sampled but changes NO pixel -- "
		     "a bisection state that separates 'the pass costs time' from 'the pass changes the "
		     "picture', which are the two questions the leg has to answer separately. 1 = full. "
		     "Clamped to [0, 1]."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<float> CVarVoxelLightFloor(
		// VS's MINBRIGHT, ported. Their fragment shader floors block light with
		// the comment "Light up all caves" (research doc section 1.4); without a
		// floor here, a cell the relax has not reached yet reads 0 and a
		// half-settled volume flickers black over the frame it settles in.
		TEXT("voxel.Light.Floor"), 0.25f,
		TEXT("The lowest fraction of today's flat ambient a cell may be scaled to, however dark "
		     "the propagated field says it is. This is Vintage Story's MINBRIGHT ('Light up all "
		     "caves') and it is also what keeps a volume that has not finished settling from "
		     "flashing black. 0 = no floor, i.e. a sealed cave goes fully unlit by this term. "
		     "Clamped to [0, 1]."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelLightRecentreCells(
		// The GI volume's dead-zone knob, same idea and the same failure mode if
		// set too wide (the camera leaves coverage without ever triggering a
		// recentre, which reads as "the lighting stopped working").
		TEXT("voxel.Light.RecentreCells"), 16,
		TEXT("Dead-zone half-width for camera-following recentring of the light volume, in 1 m "
		     "cells. The volume recentres when the camera leaves this box around the volume's "
		     "centre, and a recentre costs a FULL RESEED (16,384 columns) plus however many relax "
		     "iterations it takes to settle. Smaller = tighter tracking and more reseeds; larger "
		     "= fewer reseeds and the camera sits further off centre. Clamped so the camera "
		     "cannot leave the box: at most a quarter of the smallest axis."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelLightSeedColumnBudget(
		TEXT("voxel.Light.SeedColumnBudget"), 4096,
		TEXT("Maximum columns seeded in one frame (Phase L3). A full reseed is 16,384 columns and "
		     "is allowed to exceed this -- a recentre has to complete or the volume describes two "
		     "different boxes at once -- but incremental dirt from streaming is spread across "
		     "frames at this rate. Clamped to [64, 16384]."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarVoxelLightCensus(
		TEXT("voxel.Light.Census"), 1,
		TEXT("Print the 1 Hz '[voxel-light] census' line while the volume is armed. 1 (default) = "
		     "on, because an arm with no engagement proof is the failure this project has paid "
		     "for most often; 0 silences it for a capture whose log must stay quiet."),
		ECVF_RenderThreadSafe);

	// =======================================================================
	// Shaders
	// =======================================================================

	// TWO PARAMETER STRUCTS, NOT ONE SHARED. The seed writes the volume and the
	// relax reads one copy while writing the other, so a shared struct would have
	// to name both an SRV and a UAV for the seed pass -- and the only texture it
	// could point the SRV at is the one it is writing, which is a read/write
	// hazard RDG refuses (correctly). Two structs is the shape that cannot
	// express the hazard.
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelLightVolumeSeedParameters, )
		VOXEL_BRICK_POOL_PARAMETERS()
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MarchChunkIndex)
		SHADER_PARAMETER(FUintVector, MarchIndexDimChunks)
		SHADER_PARAMETER(uint32, MarchIndexCellsPerLevel)
		SHADER_PARAMETER(FIntVector, MarchBrickOriginVoxel)
		SHADER_PARAMETER(int32, MarchStepBudget)

		SHADER_PARAMETER(FIntVector, LightVolOriginVoxel)
		SHADER_PARAMETER(float, LightVolAbsorption)
		SHADER_PARAMETER(uint32, LightVolSeedColumnCount)
		// StructuredBuffer, NOT Buffer<uint>: a typed buffer SRV needs the
		// resource created with a format and this list is built fresh every frame
		// from a TArray, which is exactly what CreateStructuredBuffer is for.
		// Same choice VoxelGIMarchPass.cpp makes for its per-frame work list.
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, LightVolSeedColumns)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, LightVolDst)
	END_SHADER_PARAMETER_STRUCT()

	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelLightVolumeRelaxParameters, )
		SHADER_PARAMETER(float, LightVolAbsorption)
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, LightVolSrc)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float4>, LightVolDst)
	END_SHADER_PARAMETER_STRUCT()

	// A pass parameter struct with ONE MEMBER AND NO SHADER. See the
	// VoxelLightVolume.ToSRV pass at the end of the update for what it is for --
	// it exists to tell RDG that something OUTSIDE the graph is about to read
	// this texture, which a uniform-buffer binding cannot say for itself.
	BEGIN_SHADER_PARAMETER_STRUCT(FVoxelLightVolumeReadParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float4>, LightVolSrc)
	END_SHADER_PARAMETER_STRUCT()

	// The two structural numbers the kernels need at COMPILE time, pushed as
	// defines from the C++ constants rather than spelled in the .usf. The .usf's
	// own fallbacks exist only for a standalone dxc compile and say so.
	void VoxelLightVolumeSetDims(FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("LIGHTVOL_DIM_XY"), VoxelLightVolume::kDimXY);
		OutEnvironment.SetDefine(TEXT("LIGHTVOL_DIM_Z"), VoxelLightVolume::kDimZ);
		// A whole number by construction: the cell is 100 UU and a level-0 voxel
		// is 10. Computed rather than typed so a cell-size change cannot leave the
		// seed probing the wrong lattice.
		OutEnvironment.SetDefine(TEXT("LIGHTVOL_VOXELS_PER_CELL"),
		                         int32(VoxelLightVolume::kCellSizeUU / 10.0f));
	}

	constexpr int32 kSeedGroupSize = 64;
	constexpr int32 kRelaxGroupXY = 8;
	constexpr int32 kRelaxGroupZ = 4;
	static_assert(VoxelLightVolume::kDimXY % kRelaxGroupXY == 0 &&
	              VoxelLightVolume::kDimZ % kRelaxGroupZ == 0,
	              "the relax dispatch divides exactly at the shipped dims; the kernel's own bound "
	              "check stays as belt and braces, but a partial group would waste a whole group's "
	              "worth of threads on every dispatch and should be a deliberate decision.");

	class FVoxelLightVolumeSeedCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelLightVolumeSeedCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelLightVolumeSeedCS, FGlobalShader);
		using FParameters = FVoxelLightVolumeSeedParameters;

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			// The brick pool is the ONLY source -- see the #error in the .usf.
			OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
			OutEnvironment.SetDefine(TEXT("LIGHTVOL_SEED_GROUP"), kSeedGroupSize);
			VoxelLightVolumeSetDims(OutEnvironment);
		}
	};

	class FVoxelLightVolumeRelaxCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelLightVolumeRelaxCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelLightVolumeRelaxCS, FGlobalShader);
		using FParameters = FVoxelLightVolumeRelaxParameters;

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			// STILL SOURCE 1, even though the relax touches no brick: the .usf
			// includes VoxelBrickTraverse.ush once for both entry points and its
			// #error fires on the file, not on the kernel. Compiling the two
			// entry points against different source definitions would also mean
			// two different definitions of the shared header in one file.
			OutEnvironment.SetDefine(TEXT("VOXEL_MARCH_SOURCE"), 1);
			OutEnvironment.SetDefine(TEXT("LIGHTVOL_RELAX_GROUP_XY"), kRelaxGroupXY);
			OutEnvironment.SetDefine(TEXT("LIGHTVOL_RELAX_GROUP_Z"), kRelaxGroupZ);
			VoxelLightVolumeSetDims(OutEnvironment);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FVoxelLightVolumeSeedCS, VOXEL_LIGHTVOL_USF,
	                        "VoxelLightVolumeSeedMain", SF_Compute);
	IMPLEMENT_GLOBAL_SHADER(FVoxelLightVolumeRelaxCS, VOXEL_LIGHTVOL_USF,
	                        "VoxelLightVolumeRelaxMain", SF_Compute);

	// =======================================================================
	// State
	// =======================================================================

	// MONOTONIC COUNTERS, NEVER RESET, and that is what lets two consumers read
	// them without stealing each other's window: the 1 Hz printer keeps its own
	// baseline and prints deltas, and VoxelLightVolumeGetAndResetCensus keeps a
	// second baseline and returns deltas since ITS last call. A counter that is
	// drained by whoever reads it first is a counter two instruments quietly
	// disagree about, which is the failure the shadow census's window discipline
	// was written to avoid.
	struct FVoxelLightVolumeCounters
	{
		std::atomic<uint64> CellsRelaxed{0};
		std::atomic<uint64> SeedColumns{0};
		std::atomic<uint64> Recentres{0};
		std::atomic<uint64> FullReseeds{0};
		std::atomic<uint64> DirtyChunks{0};
		std::atomic<uint64> DirtyDropped{0};
		std::atomic<uint64> Overflows{0};
		std::atomic<uint64> Frames{0};
		std::atomic<uint64> DeclinedNoPool{0};
		std::atomic<uint64> DeclinedNoIndex{0};
		std::atomic<uint64> DeclinedNoVolume{0};
	};
	FVoxelLightVolumeCounters GCounters;

	// The queue's cap. 4,096 level-0 chunks is ~131,000 columns' worth of dirt
	// before dedupe -- far more than one frame can seed -- so hitting it means
	// the world is streaming faster than the volume can follow, which is exactly
	// when a full reseed is CHEAPER than tracking the difference. Overflow is
	// counted and printed, never silent.
	constexpr int32 kDirtyQueueCap = 4096;

	struct FVoxelLightVolumeState
	{
		// The ping-pong pair. Latest holds the field the marcher is sampling.
		FTextureRHIRef Volume[2];
		int32 Latest = 0;
		bool bAllocated = false;
		bool bAllocationRefused = false;
		// Set at allocation, consumed by the first update that registers the
		// pair. A MEMBER RATHER THAN A FUNCTION-STATIC, because a function-static
		// would not reset when the operator turns the feature off and on again --
		// and the second allocation's undefined bytes would then never be
		// cleared, which is light leaking through walls with no way to see why.
		bool bNeedClear = false;

		// The volume's min corner, in level-0 world voxels, always a multiple of
		// the voxels-per-cell so the cell lattice is stable under recentring (a
		// lattice that shifted by a fraction of a cell would resample the whole
		// field every recentre and shimmer).
		FIntVector OriginVoxel = FIntVector::ZeroValue;
		bool bHasOrigin = false;
		// ATOMIC BECAUSE THE GAME THREAD CAN RAISE IT. The dirty hook sets it on
		// queue overflow (under DirtyLock) while the render thread reads and
		// clears it in the pass build (without the lock, because it is the only
		// writer of the clearing direction). Relaxed is enough: the two
		// directions are a single independent bool, and the worst a lost
		// interleaving can do is delay one reseed by a frame or run one extra.
		std::atomic<bool> bNeedFullReseed{true};

		// Level-0 chunk coordinates queued by the game thread's index-delta hook
		// and drained by the render thread's pass build.
		FCriticalSection DirtyLock;
		TArray<FIntVector> DirtyChunks;

		TUniformBufferRef<FVoxelLightVolumeParameters> UniformBuffer;
		// What the uniform buffer currently describes, so it is rebuilt on change
		// rather than every frame -- the GI volume's PushVolumeParamsIfChanged
		// rule. EVERY FIELD THAT REACHES THE BUFFER MUST APPEAR HERE, or the
		// feature it gates latches its first value forever (that header's
		// failure-mode 5, quoted because it has already happened once).
		FVector3f LastOriginRelCameraUU = FVector3f(FLT_MAX);
		float LastStrength = -1.0f;
		float LastFloor = -1.0f;
		uint32 LastEnabled = 0xFFFFFFFFu;
		bool bUniformValid = false;

		// The 1 Hz printer's own baseline. See FVoxelLightVolumeCounters.
		double LastCensusSeconds = 0.0;
		FVoxelLightVolumeCensus PrinterBaseline;
	};
	FVoxelLightVolumeState GState;

	// The exported drain's baseline, kept beside the state rather than inside it
	// because it belongs to the CALLER's cadence, not to the volume's.
	FVoxelLightVolumeCensus GExportBaseline;

	int32 GetRelaxIterations()
	{
		return FMath::Clamp(CVarVoxelLightRelaxIterations.GetValueOnRenderThread(), 0, 16);
	}

	float GetAbsorption()
	{
		const float Cells =
			FMath::Clamp(CVarVoxelLightAbsorptionCells.GetValueOnRenderThread(), 1.0f, 64.0f);
		return 1.0f / Cells;
	}

	int32 GetRecentreCells()
	{
		// CLAMPED AGAINST THE VOLUME'S OWN HALF-EXTENT, and the smallest axis
		// decides. A dead zone wider than a quarter of the shortest axis (Z, 64
		// cells) lets the camera leave coverage before a recentre fires, and the
		// symptom is "the lighting stopped working" rather than "the setting is
		// wrong" -- which is the one failure this knob can produce.
		const int32 MaxDead = FMath::Min(VoxelLightVolume::kDimXY, VoxelLightVolume::kDimZ) / 4;
		return FMath::Clamp(CVarVoxelLightRecentreCells.GetValueOnRenderThread(), 1, MaxDead);
	}

	// Level-0 voxels per light cell. An integer by construction; asserted rather
	// than assumed, because a non-integer would put the seed's cell centres on a
	// drifting sub-voxel offset that no counter would ever show.
	constexpr int32 kVoxelsPerCell = int32(VoxelLightVolume::kCellSizeUU / 10.0f);
	static_assert(float(kVoxelsPerCell) * 10.0f == VoxelLightVolume::kCellSizeUU,
	              "a light cell must be a whole number of level-0 voxels: the seed addresses cell "
	              "centres in integer world voxels and there is no rounding rule that would be "
	              "correct for a fractional cell.");

	// Floor division that is correct for negative coordinates. The world is
	// centred on the planet origin and the camera is routinely on the negative
	// side of it, so C's truncating division would fold two adjacent cells onto
	// one at the origin -- a one-cell lighting seam through the middle of the
	// world, which is exactly the kind of defect that gets blamed on the shader.
	int32 FloorDiv(int64 Numerator, int64 Denominator)
	{
		const int64 Quotient = Numerator / Denominator;
		const int64 Remainder = Numerator % Denominator;
		return int32((Remainder != 0 && ((Remainder < 0) != (Denominator < 0))) ? Quotient - 1
		                                                                       : Quotient);
	}
}

namespace VoxelLightVolume
{
	bool IsEnabled()
	{
		return CVarVoxelLightPropagated.GetValueOnAnyThread() != 0;
	}
}

// ---------------------------------------------------------------------------
// The dirty channel (game thread)
// ---------------------------------------------------------------------------

void VoxelLightVolumeNoteBrickIndexDelta_GameThread(const FVoxelBrickIndexDelta& Delta)
{
	// THE CVAR, NOT THE ALLOCATION STATE, gates this. The volume is allocated on
	// the render thread on its first update, so a game-thread test against
	// GState.bAllocated would drop the deltas of the first frames -- the frames
	// during which the world around the camera first becomes resident, i.e. the
	// dirt that matters most.
	if (CVarVoxelLightPropagated.GetValueOnGameThread() == 0)
	{
		return;
	}

	FScopeLock Guard(&GState.DirtyLock);
	auto Note = [](const FVoxelBrickIndexEntry& Entry)
	{
		// LEVEL 0 ONLY, and the drop is COUNTED. This volume is 64 m of radius,
		// which is inside ring 0 (0-128 m) by construction, so a level-1 chunk
		// cannot intersect it and a cover chunk (kCoverLevel) is grass rather
		// than terrain. A silent drop here and a dead wire look identical in the
		// census, which is why they are two counters.
		if (Entry.Key.Level != 0)
		{
			GCounters.DirtyDropped.fetch_add(1);
			return;
		}
		if (GState.DirtyChunks.Num() >= kDirtyQueueCap)
		{
			// OVERFLOW FORCES A FULL RESEED -- the safe direction, and the cheap
			// one at this scale: a cold fill delivers tens of thousands of chunks
			// and enumerating them all to describe "everything changed" costs more
			// than reseeding all 16,384 columns once.
			GCounters.Overflows.fetch_add(1);
			GState.bNeedFullReseed = true;
			return;
		}
		GCounters.DirtyChunks.fetch_add(1);
		GState.DirtyChunks.Add(FIntVector(Entry.Key.X, Entry.Key.Y, Entry.Key.Z));
	};

	// BOTH HALVES DIRTY THE SAME CELLS. A retired chunk changes the field
	// exactly as much as an added one does -- it opens sky where there was rock.
	// Order does not matter here (unlike the index's own apply, where removed
	// must precede added because both can name one slot): this queue is a set of
	// PLACES, not of slots.
	for (const FVoxelBrickIndexEntry& Entry : Delta.Added)
	{
		Note(Entry);
	}
	for (const FVoxelBrickIndexEntry& Entry : Delta.Removed)
	{
		Note(Entry);
	}
}

// ---------------------------------------------------------------------------
// The census
// ---------------------------------------------------------------------------

namespace
{
	// A snapshot of the monotonic counters, in the exported struct's shape.
	FVoxelLightVolumeCensus SnapshotCounters()
	{
		FVoxelLightVolumeCensus Out;
		Out.CellsRelaxed = GCounters.CellsRelaxed.load();
		Out.SeedColumns = GCounters.SeedColumns.load();
		Out.Recentres = GCounters.Recentres.load();
		Out.FullReseeds = GCounters.FullReseeds.load();
		Out.DirtyChunks = GCounters.DirtyChunks.load();
		Out.DirtyDropped = GCounters.DirtyDropped.load();
		Out.Overflows = GCounters.Overflows.load();
		Out.Frames = GCounters.Frames.load();
		Out.DeclinedNoPool = GCounters.DeclinedNoPool.load();
		Out.DeclinedNoIndex = GCounters.DeclinedNoIndex.load();
		Out.DeclinedNoVolume = GCounters.DeclinedNoVolume.load();
		return Out;
	}

	FVoxelLightVolumeCensus DiffCensus(const FVoxelLightVolumeCensus& Now,
	                                   const FVoxelLightVolumeCensus& Baseline)
	{
		FVoxelLightVolumeCensus Out;
		Out.CellsRelaxed = Now.CellsRelaxed - Baseline.CellsRelaxed;
		Out.SeedColumns = Now.SeedColumns - Baseline.SeedColumns;
		Out.Recentres = Now.Recentres - Baseline.Recentres;
		Out.FullReseeds = Now.FullReseeds - Baseline.FullReseeds;
		Out.DirtyChunks = Now.DirtyChunks - Baseline.DirtyChunks;
		Out.DirtyDropped = Now.DirtyDropped - Baseline.DirtyDropped;
		Out.Overflows = Now.Overflows - Baseline.Overflows;
		Out.Frames = Now.Frames - Baseline.Frames;
		Out.DeclinedNoPool = Now.DeclinedNoPool - Baseline.DeclinedNoPool;
		Out.DeclinedNoIndex = Now.DeclinedNoIndex - Baseline.DeclinedNoIndex;
		Out.DeclinedNoVolume = Now.DeclinedNoVolume - Baseline.DeclinedNoVolume;
		// STATE, NOT A WINDOW SUM: these three describe right now and are copied
		// through the diff unchanged rather than subtracted, which would produce
		// a meaningless "change in queue depth" number.
		Out.DirtyDepth = Now.DirtyDepth;
		Out.OriginVoxel = Now.OriginVoxel;
		Out.bArmed = Now.bArmed;
		return Out;
	}
}

FVoxelLightVolumeCensus VoxelLightVolumeGetAndResetCensus()
{
	FVoxelLightVolumeCensus Now = SnapshotCounters();
	{
		FScopeLock Guard(&GState.DirtyLock);
		Now.DirtyDepth = GState.DirtyChunks.Num();
	}
	Now.OriginVoxel = GState.OriginVoxel;
	// FROM THE CVAR, never from whether anything ran: "the arm is off" and "the
	// arm is on and did nothing" are different findings and a consumer must be
	// able to tell them apart without inspecting the numbers it is trying to
	// interpret (the hole-census rule, applied here).
	Now.bArmed = CVarVoxelLightPropagated.GetValueOnAnyThread() != 0;

	const FVoxelLightVolumeCensus Out = DiffCensus(Now, GExportBaseline);
	GExportBaseline = Now;
	return Out;
}

// ---------------------------------------------------------------------------
// The uniform buffer
// ---------------------------------------------------------------------------

namespace
{
	// Rebuilds the marcher's binding when anything it carries has moved.
	// RENDER THREAD.
	void UpdateUniformBuffer(const FVector3f& OriginRelCameraUU, bool bEnabled)
	{
		const float Strength =
			FMath::Clamp(CVarVoxelLightStrength.GetValueOnRenderThread(), 0.0f, 1.0f);
		const float Floor = FMath::Clamp(CVarVoxelLightFloor.GetValueOnRenderThread(), 0.0f, 1.0f);
		const uint32 Enabled = (bEnabled && GState.Volume[GState.Latest].IsValid()) ? 1u : 0u;

		if (GState.bUniformValid && GState.LastEnabled == Enabled &&
		    GState.LastStrength == Strength && GState.LastFloor == Floor &&
		    GState.LastOriginRelCameraUU == OriginRelCameraUU)
		{
			return;
		}

		FVoxelLightVolumeParameters Parameters;
		// NEVER NULL, even off -- an unbound member of a uniform buffer is a
		// validation failure, not a tolerated no-op. Black is also the right
		// VALUE to fall back to: level 0 with Enabled 0 means the marcher's test
		// fails before it ever reads a texel.
		Parameters.Volume = Enabled != 0u
			? GState.Volume[GState.Latest].GetReference()
			: GBlackVolumeTexture->TextureRHI.GetReference();
		// BILINEAR + CLAMP: bilinear on a Texture3D IS trilinear, which is the
		// whole reason this is a texture (research doc section 3, row 3 -- the
		// hardware filter is the per-vertex smoothing VS builds by hand). Clamp
		// rather than wrap because the marcher range-checks before sampling and a
		// wrapped read would fetch the far side of the volume, which is a real
		// place with a real value and therefore a plausible wrong answer.
		Parameters.VolumeSampler =
			TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		Parameters.OriginRelCameraUU = OriginRelCameraUU;
		Parameters.InvSizeUU = FVector3f(
			1.0f / (float(VoxelLightVolume::kDimXY) * VoxelLightVolume::kCellSizeUU),
			1.0f / (float(VoxelLightVolume::kDimXY) * VoxelLightVolume::kCellSizeUU),
			1.0f / (float(VoxelLightVolume::kDimZ) * VoxelLightVolume::kCellSizeUU));
		Parameters.CellSizeUU = VoxelLightVolume::kCellSizeUU;
		Parameters.Strength = Strength;
		Parameters.Floor = Floor;
		Parameters.Enabled = Enabled;

		// MultiFrame, not SingleFrame: this is rebuilt when the volume recentres
		// or a cvar moves, not every frame, so a single-frame lifetime would free
		// it out from under the next frame's draws (the GI volume's own note).
		GState.UniformBuffer =
			TUniformBufferRef<FVoxelLightVolumeParameters>::CreateUniformBufferImmediate(
				Parameters, UniformBuffer_MultiFrame);
		GState.LastOriginRelCameraUU = OriginRelCameraUU;
		GState.LastStrength = Strength;
		GState.LastFloor = Floor;
		GState.LastEnabled = Enabled;
		GState.bUniformValid = true;
	}
}

const TUniformBufferRef<FVoxelLightVolumeParameters>& VoxelLightVolumeGetUniformBuffer()
{
	if (!GState.bUniformValid)
	{
		// THE OFF BINDING, BUILT ON DEMAND. The marcher binds this on every emit
		// whether or not the feature has ever been armed, so it must exist before
		// the first update runs -- and it must describe the off state, not an
		// uninitialised one.
		UpdateUniformBuffer(FVector3f::ZeroVector, false);
	}
	return GState.UniformBuffer;
}

// ---------------------------------------------------------------------------
// The update
// ---------------------------------------------------------------------------

namespace
{
	// Allocates the ping-pong pair. RENDER THREAD. Returns false and says why,
	// once, rather than leaving the caller to discover a null texture inside RDG.
	bool EnsureAllocated(FRHICommandListBase& RHICmdList)
	{
		if (GState.bAllocated)
		{
			return true;
		}
		if (GState.bAllocationRefused)
		{
			return false;
		}

		// NOT ETextureCreateFlags::Dynamic and no lock/Map path: every write goes
		// through a UAV from a compute pass. Same reading of
		// gpu-pool-rendering-notes.md invariant 5 the GI volume's allocation
		// carries, reached from the other direction (it never uses a UAV; this
		// never uses an upload).
		const ETextureCreateFlags Flags =
			ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV;

		for (int32 I = 0; I < 2; ++I)
		{
			const FRHITextureCreateDesc Desc =
				FRHITextureCreateDesc::Create3D(
					I == 0 ? TEXT("VoxelLight.VolumeA") : TEXT("VoxelLight.VolumeB"),
					VoxelLightVolume::kDimXY, VoxelLightVolume::kDimXY, VoxelLightVolume::kDimZ,
					PF_R8G8B8A8)
					.SetFlags(Flags);
			GState.Volume[I] = RHICmdList.CreateTexture(Desc);
			if (!GState.Volume[I].IsValid())
			{
				GState.Volume[0].SafeRelease();
				GState.Volume[1].SafeRelease();
				GState.bAllocationRefused = true;
				UE_LOG(LogVoxelLightVolume, Error,
				       TEXT("[voxel-light] REFUSED: could not create the %lld MiB light volume pair "
				            "(%dx%dx%d, RGBA8, ping-pong). voxel.Light.Propagated stays armed but "
				            "nothing will be seeded, relaxed or sampled -- the census line will read "
				            "declinedNoVolume and the frame is the unlit control."),
				       (2 * VoxelLightVolume::kBytesPerVolume) / (1024 * 1024),
				       VoxelLightVolume::kDimXY, VoxelLightVolume::kDimXY, VoxelLightVolume::kDimZ);
				return false;
			}
		}

		GState.bAllocated = true;
		GState.Latest = 0;
		// A FRESH TEXTURE'S CONTENTS ARE UNDEFINED, and undefined bytes in the
		// AIR MASK read as "this cell may receive light" -- i.e. arbitrary light
		// leaking through arbitrary walls until the first seed covers the cell.
		// The clear is added to the graph by the caller (RDG owns the transition);
		// forcing a full reseed here is the other half of the same guarantee.
		GState.bNeedClear = true;
		GState.bNeedFullReseed = true;
		UE_LOG(LogVoxelLightVolume, Display,
		       TEXT("[voxel-light] allocated %dx%dx%d cells at %.0f UU (%.0f x %.0f x %.0f m), "
		            "RGBA8 ping-pong, %lld MiB resident. R=sky level, G=air mask, BA reserved for "
		            "L4 block light. DISPLAY ONLY -- no gameplay system may read this "
		            "(ADR-0006 invariants 3-5)."),
		       VoxelLightVolume::kDimXY, VoxelLightVolume::kDimXY, VoxelLightVolume::kDimZ,
		       VoxelLightVolume::kCellSizeUU,
		       VoxelLightVolume::kDimXY * VoxelLightVolume::kCellSizeUU / 100.0f,
		       VoxelLightVolume::kDimXY * VoxelLightVolume::kCellSizeUU / 100.0f,
		       VoxelLightVolume::kDimZ * VoxelLightVolume::kCellSizeUU / 100.0f,
		       (2 * VoxelLightVolume::kBytesPerVolume) / (1024 * 1024));
		return true;
	}

	// Where the volume wants to be this frame, in level-0 world voxels, snapped
	// to the cell lattice. Pure function of the camera.
	FIntVector DesiredOriginVoxel(const FVector& CameraWorldUU)
	{
		// In CELLS first, in double, then multiplied back up: computing the origin
		// in voxels and snapping there would round twice.
		const int64 CamCellX =
			int64(FMath::FloorToDouble(CameraWorldUU.X / double(VoxelLightVolume::kCellSizeUU)));
		const int64 CamCellY =
			int64(FMath::FloorToDouble(CameraWorldUU.Y / double(VoxelLightVolume::kCellSizeUU)));
		const int64 CamCellZ =
			int64(FMath::FloorToDouble(CameraWorldUU.Z / double(VoxelLightVolume::kCellSizeUU)));
		return FIntVector(int32((CamCellX - VoxelLightVolume::kDimXY / 2) * kVoxelsPerCell),
		                  int32((CamCellY - VoxelLightVolume::kDimXY / 2) * kVoxelsPerCell),
		                  int32((CamCellZ - VoxelLightVolume::kDimZ / 2) * kVoxelsPerCell));
	}

	// Turns queued level-0 chunk coordinates into a deduplicated column list for
	// the current origin. RENDER THREAD; drains as much of the queue as the
	// budget allows and leaves the rest for the next frame.
	void BuildSeedColumns(int32 Budget, TArray<uint32>& OutColumns)
	{
		TSet<uint32> Seen;
		Seen.Reserve(Budget);

		FScopeLock Guard(&GState.DirtyLock);
		int32 Consumed = 0;
		for (const FIntVector& Chunk : GState.DirtyChunks)
		{
			if (OutColumns.Num() >= Budget)
			{
				break;
			}
			++Consumed;
			// A level-0 chunk is 32 voxels on a side. Its cell span, plus a
			// ONE-CELL HALO on every side: the seed decides a column from cell
			// CENTRES, so a surface that moved inside the chunk can change the
			// answer for the cell just outside it, and re-seeding one cell too
			// many costs a column while missing one leaves a stale lit cell
			// against a new wall.
			const int32 MinVoxX = Chunk.X * 32;
			const int32 MinVoxY = Chunk.Y * 32;
			const int32 MinCellX = FloorDiv(int64(MinVoxX - GState.OriginVoxel.X), kVoxelsPerCell) - 1;
			const int32 MinCellY = FloorDiv(int64(MinVoxY - GState.OriginVoxel.Y), kVoxelsPerCell) - 1;
			const int32 MaxCellX = FloorDiv(int64(MinVoxX + 31 - GState.OriginVoxel.X), kVoxelsPerCell) + 1;
			const int32 MaxCellY = FloorDiv(int64(MinVoxY + 31 - GState.OriginVoxel.Y), kVoxelsPerCell) + 1;
			for (int32 Cy = FMath::Max(MinCellY, 0);
			     Cy <= FMath::Min(MaxCellY, VoxelLightVolume::kDimXY - 1); ++Cy)
			{
				for (int32 Cx = FMath::Max(MinCellX, 0);
				     Cx <= FMath::Min(MaxCellX, VoxelLightVolume::kDimXY - 1); ++Cx)
				{
					const uint32 Packed = uint32(Cx) | (uint32(Cy) << 16);
					bool bAlready = false;
					Seen.Add(Packed, &bAlready);
					if (!bAlready)
					{
						OutColumns.Add(Packed);
					}
				}
			}
		}
		if (Consumed > 0)
		{
			GState.DirtyChunks.RemoveAt(0, Consumed, EAllowShrinking::No);
		}
	}
}

void VoxelLightVolumeUpdate_RenderThread(FRDGBuilder& GraphBuilder, const FGlobalShaderMap* ShaderMap,
                                         const FVoxelLightVolumeFrame& Frame)
{
	const bool bArmed = CVarVoxelLightPropagated.GetValueOnRenderThread() != 0;
	if (!bArmed)
	{
		// THE OFF ARM, AND IT IS SILENT BY DESIGN. Nothing is allocated, no pass
		// is added, and the uniform buffer keeps (or takes) its Enabled = 0 form
		// so the marcher's emissive is byte-identical to the pre-L3 expression.
		// The one thing that must still happen is releasing a volume the operator
		// just turned off, or an 8 MiB allocation would outlive its arm for the
		// rest of the session.
		if (GState.bAllocated)
		{
			GState.Volume[0].SafeRelease();
			GState.Volume[1].SafeRelease();
			GState.bAllocated = false;
			GState.bHasOrigin = false;
			GState.bNeedFullReseed = true;
			GState.bUniformValid = false; // forces the off binding to be rebuilt
			FScopeLock Guard(&GState.DirtyLock);
			GState.DirtyChunks.Reset();
		}
		UpdateUniformBuffer(FVector3f::ZeroVector, false);
		return;
	}

	GCounters.Frames.fetch_add(1);

	if (!EnsureAllocated(GraphBuilder.RHICmdList))
	{
		GCounters.DeclinedNoVolume.fetch_add(1);
		UpdateUniformBuffer(FVector3f::ZeroVector, false);
		return;
	}

	// ---- recentre ---------------------------------------------------------
	//
	// THE DEAD ZONE IS WHAT MAKES THIS AFFORDABLE. Following the camera exactly
	// would mean re-addressing the whole field every frame; following it in
	// jumps means a full reseed on the jump and nothing in between. Same trade,
	// same shape and the same knob as the GI volume's VolumeRecentreCells.
	const FIntVector Desired = DesiredOriginVoxel(Frame.CameraWorldUU);
	if (!GState.bHasOrigin)
	{
		GState.OriginVoxel = Desired;
		GState.bHasOrigin = true;
		GState.bNeedFullReseed = true;
	}
	else
	{
		const int32 DeadCells = GetRecentreCells();
		const FIntVector DriftVoxels = Desired - GState.OriginVoxel;
		const int32 DriftCells = FMath::Max3(FMath::Abs(DriftVoxels.X), FMath::Abs(DriftVoxels.Y),
		                                     FMath::Abs(DriftVoxels.Z)) / kVoxelsPerCell;
		if (DriftCells > DeadCells)
		{
			GState.OriginVoxel = Desired;
			GState.bNeedFullReseed = true;
			GCounters.Recentres.fetch_add(1);
			// THE DIRTY QUEUE IS DROPPED WITH THE ORIGIN, and that is correct
			// rather than lossy: its entries were converted against the OLD
			// origin's lattice and the full reseed that follows covers every
			// column anyway. Keeping them would seed a few columns twice.
			FScopeLock Guard(&GState.DirtyLock);
			GState.DirtyChunks.Reset();
		}
	}

	// ---- the sources ------------------------------------------------------
	auto* SeedParams = GraphBuilder.AllocParameters<FVoxelLightVolumeSeedParameters>();
	// FALSE MEANS THERE IS NOTHING TO PROBE, and it is ordinary on the first
	// frames of every run (the arenas are created lazily by the first flush).
	// Skipping is correct; dispatching against a half-filled struct would probe
	// null SRVs, and a null SRV reads as zeros -- which is a legal descriptor
	// meaning uniform AIR, so every column would seed as open sky and the whole
	// volume would read fully lit with no error anywhere.
	//
	// BindShaderParameters IS SAFE TO CALL AGAIN THIS FRAME -- it registers the
	// pool's persistent buffers into the graph and consumes nothing -- which is
	// why the pool is bound here and the INDEX is not (see FVoxelLightVolumeFrame::
	// ChunkIndexSRV for the difference and why it matters).
	const bool bPoolBound = GetGlobalVoxelBrickPool().BindShaderParameters(GraphBuilder, *SeedParams);
	FRDGBufferSRVRef IndexSRV = Frame.ChunkIndexSRV;

	// ---- register the pair ------------------------------------------------
	FRDGTextureRef VolumeRDG[2] = {
		RegisterExternalTexture(GraphBuilder, GState.Volume[0].GetReference(),
		                        TEXT("VoxelLight.VolumeA")),
		RegisterExternalTexture(GraphBuilder, GState.Volume[1].GetReference(),
		                        TEXT("VoxelLight.VolumeB")),
	};

	// The undefined-contents clear, on the first frame the pair exists. BOTH
	// copies, because the relax reads whichever the seed did not write, and
	// undefined bytes in the AIR MASK are light leaking through walls (see
	// EnsureAllocated).
	//
	// Cleared to level 0 / air mask 0 -- "no light and no permission to receive
	// any", which is INERT rather than merely empty: a relax over a cleared
	// volume writes nothing but zeros, so a seed that has not run yet cannot
	// produce a lit frame. The alternative clear (air mask 1) would light the
	// entire world for one frame.
	if (GState.bNeedClear)
	{
		GState.bNeedClear = false;
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeRDG[0]),
		                FVector4f(0.0f, 0.0f, 0.0f, 1.0f));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeRDG[1]),
		                FVector4f(0.0f, 0.0f, 0.0f, 1.0f));
	}

	if (!bPoolBound)
	{
		GCounters.DeclinedNoPool.fetch_add(1);
	}
	else if (IndexSRV == nullptr)
	{
		GCounters.DeclinedNoIndex.fetch_add(1);
	}

	RDG_EVENT_SCOPE_STAT(GraphBuilder, VoxelLightVolume, "VoxelLightVolume");

	const float Absorption = GetAbsorption();
	int32 SeededColumns = 0;

	// ---- SEED -------------------------------------------------------------
	if (bPoolBound && IndexSRV != nullptr)
	{
		TArray<uint32> Columns;
		if (GState.bNeedFullReseed)
		{
			// A FULL RESEED IS ALLOWED TO EXCEED THE PER-FRAME BUDGET, and that
			// is not an oversight: a recentre moves the box, so a half-seeded
			// volume would describe two different boxes at once and the marcher
			// would sample a field that is partly somewhere else. 16,384 columns
			// is 64 KB of upload and 1,048,576 probes -- the one expensive frame
			// this feature has, and it happens once per dead-zone crossing.
			Columns.Reserve(VoxelLightVolume::kDimXY * VoxelLightVolume::kDimXY);
			for (int32 Cy = 0; Cy < VoxelLightVolume::kDimXY; ++Cy)
			{
				for (int32 Cx = 0; Cx < VoxelLightVolume::kDimXY; ++Cx)
				{
					Columns.Add(uint32(Cx) | (uint32(Cy) << 16));
				}
			}
			GState.bNeedFullReseed = false;
			GCounters.FullReseeds.fetch_add(1);
		}
		else
		{
			const int32 Budget =
				FMath::Clamp(CVarVoxelLightSeedColumnBudget.GetValueOnRenderThread(), 64, 16384);
			BuildSeedColumns(Budget, Columns);
		}

		if (Columns.Num() > 0)
		{
			SeededColumns = Columns.Num();
			GCounters.SeedColumns.fetch_add(uint64(SeededColumns));

			FRDGBufferRef ColumnBuffer = CreateStructuredBuffer(
				GraphBuilder, TEXT("VoxelLight.SeedColumns"), sizeof(uint32),
				Columns.Num(), Columns.GetData(), Columns.Num() * sizeof(uint32));

			SeedParams->MarchChunkIndex = IndexSRV;
			SeedParams->MarchIndexDimChunks = FUintVector(FVoxelMarchChunkIndex::kDimXY,
			                                              FVoxelMarchChunkIndex::kDimXY,
			                                              FVoxelMarchChunkIndex::kDimZ);
			SeedParams->MarchIndexCellsPerLevel = FVoxelMarchChunkIndex::kCellsPerLevel;
			SeedParams->MarchBrickOriginVoxel = Frame.FrameOriginVoxel;
			SeedParams->MarchStepBudget = FMath::Max(Frame.StepBudget, 1);
			SeedParams->LightVolOriginVoxel = GState.OriginVoxel;
			SeedParams->LightVolAbsorption = Absorption;
			SeedParams->LightVolSeedColumnCount = uint32(Columns.Num());
			SeedParams->LightVolSeedColumns = GraphBuilder.CreateSRV(ColumnBuffer);
			// THE SEED WRITES THE LATEST COPY, not the spare. The marcher may
			// sample this frame before any relax runs, and a seed written into the
			// copy nothing is bound to would be a frame of stale light.
			SeedParams->LightVolDst = GraphBuilder.CreateUAV(VolumeRDG[GState.Latest]);

			TShaderMapRef<FVoxelLightVolumeSeedCS> SeedShader(ShaderMap);
			const int32 SeedGroups = FMath::DivideAndRoundUp(Columns.Num(), kSeedGroupSize);
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("VoxelLightVolume.Seed(%d columns)", Columns.Num()),
				ERDGPassFlags::Compute, SeedShader, SeedParams, FIntVector(SeedGroups, 1, 1));
		}
	}

	// ---- RELAX ------------------------------------------------------------
	//
	// RUN ONLY WHILE THERE IS SOMETHING TO PROPAGATE, which is what makes the
	// budget amortized rather than constant: a settled camera in a streamed-in
	// world seeds nothing, relaxes nothing and costs its memory only. The
	// condition is deliberately generous -- any seeding this frame, or anything
	// still queued -- because under-relaxing shows as light that never arrives
	// and over-relaxing shows as a number in a profile.
	int32 RelaxIterations = 0;
	{
		int32 QueueDepth = 0;
		{
			FScopeLock Guard(&GState.DirtyLock);
			QueueDepth = GState.DirtyChunks.Num();
		}
		const bool bWantRelax = (SeededColumns > 0) || (QueueDepth > 0);
		RelaxIterations = bWantRelax ? GetRelaxIterations() : 0;
	}

	for (int32 Iteration = 0; Iteration < RelaxIterations; ++Iteration)
	{
		const int32 Src = GState.Latest;
		const int32 Dst = 1 - Src;
		auto* RelaxParams = GraphBuilder.AllocParameters<FVoxelLightVolumeRelaxParameters>();
		RelaxParams->LightVolAbsorption = Absorption;
		RelaxParams->LightVolSrc = VolumeRDG[Src];
		RelaxParams->LightVolDst = GraphBuilder.CreateUAV(VolumeRDG[Dst]);

		TShaderMapRef<FVoxelLightVolumeRelaxCS> RelaxShader(ShaderMap);
		const FIntVector Groups(VoxelLightVolume::kDimXY / kRelaxGroupXY,
		                        VoxelLightVolume::kDimXY / kRelaxGroupXY,
		                        VoxelLightVolume::kDimZ / kRelaxGroupZ);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("VoxelLightVolume.Relax(%d/%d)", Iteration + 1,
			                             RelaxIterations),
			ERDGPassFlags::Compute, RelaxShader, RelaxParams, Groups);

		GState.Latest = Dst;
		GCounters.CellsRelaxed.fetch_add(
			uint64(VoxelLightVolume::kDimXY) * VoxelLightVolume::kDimXY * VoxelLightVolume::kDimZ);
	}

	// ---- THE TRANSITION THE EMIT'S BINDING CANNOT ASK FOR ------------------
	//
	// The volume is an EXTERNAL texture, written here through a UAV and read by
	// the marcher's emit through a UNIFORM BUFFER -- and a uniform buffer is
	// invisible to RDG. So RDG has no reason to move the texture out of UAV
	// state before the emit samples it, and that read would be a state mismatch:
	// a validation error in a development build and undefined data in a shipping
	// one, i.e. a lighting defect that reproduces on one machine and not
	// another. Nothing in the seed, the relax or the emit would name the cause.
	//
	// A PASS THAT DECLARES THE TEXTURE AS AN SRV AND DOES NOTHING is how you
	// tell RDG that somebody outside the graph is about to read it. It costs a
	// barrier and no work, and it runs every frame the volume is armed because
	// on a frame with no seed and no relax the barrier is already satisfied and
	// RDG elides it.
	//
	// THE ALTERNATIVE WAS REJECTED: binding the volume to the emit as an RDG
	// texture would mean the marcher's shader reaching it as a loose
	// `Texture3D Volume;` rather than through VoxelLightVol -- and a loose
	// declaration shadowing a uniform-buffer member is the silent-zeros trap
	// three headers in this module already carry warnings about.
	{
		auto* ReadParams = GraphBuilder.AllocParameters<FVoxelLightVolumeReadParameters>();
		ReadParams->LightVolSrc = VolumeRDG[GState.Latest];
		GraphBuilder.AddPass(RDG_EVENT_NAME("VoxelLightVolume.ToSRV"), ReadParams,
		                     ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
		                     [](FRHIComputeCommandList&) {});
	}

	// ---- the marcher's binding --------------------------------------------
	//
	// The origin RELATIVE TO THE CAMERA, differenced in DOUBLE and narrowed
	// once. Differencing in float at world scale is the catastrophic
	// cancellation FVoxelLightVolumeParameters::OriginRelCameraUU documents; the
	// subtraction here is the only place the two magnitudes ever meet.
	const FVector OriginWorldUU(double(GState.OriginVoxel.X) * 10.0,
	                            double(GState.OriginVoxel.Y) * 10.0,
	                            double(GState.OriginVoxel.Z) * 10.0);
	UpdateUniformBuffer(FVector3f(OriginWorldUU - Frame.CameraWorldUU), true);

	// ---- the 1 Hz census line ---------------------------------------------
	//
	// THE HOUSE PATTERN: counts of dispatched work, a stated window, and a
	// verdict word a leg can grep and FAIL on. cellsRelaxed == 0 across a window
	// in which the camera moved means the amortization never engaged -- either
	// nothing is being marked dirty (a dead wire) or the relax budget is 0 --
	// and those are different owners, which is why seedColumns, dirtyDepth and
	// the three decline counters are on the same line.
	//
	// GREP: "[voxel-light] census"
	if (CVarVoxelLightCensus.GetValueOnRenderThread() != 0)
	{
		const double Now = FPlatformTime::Seconds();
		if (GState.LastCensusSeconds <= 0.0)
		{
			GState.LastCensusSeconds = Now;
			GState.PrinterBaseline = SnapshotCounters();
		}
		else if (Now - GState.LastCensusSeconds >= 1.0)
		{
			FVoxelLightVolumeCensus Snapshot = SnapshotCounters();
			const FVoxelLightVolumeCensus W = DiffCensus(Snapshot, GState.PrinterBaseline);
			GState.PrinterBaseline = Snapshot;
			const double Window = Now - GState.LastCensusSeconds;
			GState.LastCensusSeconds = Now;

			int32 QueueDepth = 0;
			{
				FScopeLock Guard(&GState.DirtyLock);
				QueueDepth = GState.DirtyChunks.Num();
			}
			const double Frames = FMath::Max(double(W.Frames), 1.0);
			UE_LOG(LogVoxelLightVolume, Display,
			       TEXT("[voxel-light] census %.2fs frames=%llu | cellsRelaxed=%.0f/frame "
			            "seedColumns=%.0f/frame | dirtyDepth=%d dirtyChunks=%llu dropped=%llu "
			            "overflow=%llu | recentres=%llu fullReseeds=%llu iters=%d "
			            "absorb=1/%.1f cells | origin=(%d,%d,%d) voxels dims=%dx%dx%d @ %.0f UU "
			            "| declined pool=%llu index=%llu volume=%llu | %s"),
			       Window, W.Frames,
			       double(W.CellsRelaxed) / Frames, double(W.SeedColumns) / Frames,
			       QueueDepth, W.DirtyChunks, W.DirtyDropped, W.Overflows,
			       W.Recentres, W.FullReseeds, GetRelaxIterations(), 1.0f / Absorption,
			       GState.OriginVoxel.X, GState.OriginVoxel.Y, GState.OriginVoxel.Z,
			       VoxelLightVolume::kDimXY, VoxelLightVolume::kDimXY, VoxelLightVolume::kDimZ,
			       VoxelLightVolume::kCellSizeUU,
			       W.DeclinedNoPool, W.DeclinedNoIndex, W.DeclinedNoVolume,
			       // THE VERDICT WORD, so a leg greps a string rather than
			       // reasoning about eleven numbers. SETTLED is a legitimate zero
			       // and INERT is not, and no reader should have to derive which
			       // one they are looking at.
			       (W.CellsRelaxed == 0 && W.SeedColumns == 0)
			           ? ((W.DeclinedNoPool + W.DeclinedNoIndex + W.DeclinedNoVolume) > 0
			                  ? TEXT("INERT-DECLINED (armed, and every frame refused -- read the "
			                         "decline counters, NOT the zeros)")
			                  : TEXT("SETTLED (nothing dirty; a MOVING camera reading this is a "
					                 "dead dirty wire and the leg should FAIL)"))
			           : TEXT("PROPAGATING"));
		}
	}
}
