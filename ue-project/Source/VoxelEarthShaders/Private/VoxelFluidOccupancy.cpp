#include "VoxelFluidOccupancy.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "PixelFormat.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelFluidOccupancy, Log, All);

// A macro rather than a `const TCHAR*` for the same reason
// VOXEL_WORLDGEN_USF is one: IMPLEMENT_GLOBAL_SHADER stringizes its path
// argument (VoxelGpuWorldGen.cpp:18-24).
#define VOXEL_FLUID_OCCUPANCY_USF "/VoxelEarth/VoxelFluidOccupancy.usf"

namespace
{
	// Mirrors of the shader's own arithmetic. Named rather than inlined,
	// because every one of them is a place the host and the kernel could
	// silently disagree about how big something is.
	constexpr int32 kFluidOccBrickEdge = vxc::kFluidBrickEdge;              // 8
	constexpr int32 kVoxelsPerWord = vxc::kFluidBitsPerWord;        // 32
	constexpr int32 kBricksPerWordX = vxc::kFluidBricksPerWordX;    // 4
	constexpr int32 kBrickWords = vxc::kFluidBrickWords;            // 16

	// FluidOccupancyFillMain's group shape. Restated here because the dispatch
	// size is computed from it and a mismatch is a partially filled region,
	// which looks like terrain with holes in it rather than like a bug.
	constexpr int32 kFillGroupX = 8;
	constexpr int32 kFillGroupY = 8;

	// All bits set: unbuilt is SOLID. See the header -- this is the "blocked,
	// never guessed" half of the collision model, and clearing to zero instead
	// would leak water through terrain that has merely not arrived.
	constexpr uint32 kUnbuiltWord = vxc::kFluidVolumeUnbuiltWord;

	// One thread per output word. Nothing here is 64-bit integer maths, so
	// unlike the worldgen kernels this gates on SM5 and says so rather than
	// inheriting a requirement it does not have (the distinction
	// FVoxelQuadTotalCS draws, VoxelGpuWorldGen.cpp:225-242).
	class FVoxelFluidOccupancyFillCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidOccupancyFillCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidOccupancyFillCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FluidOccupancyBits)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FluidRegionBrickBits)
			SHADER_PARAMETER(uint32,      FluidVolumeDimVoxels)
			SHADER_PARAMETER(FUintVector, FluidRegionMinVoxel)
			SHADER_PARAMETER(FUintVector, FluidRegionSizeVoxels)
		END_SHADER_PARAMETER_STRUCT()
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelFluidOccupancyFillCS, VOXEL_FLUID_OCCUPANCY_USF,
                        "FluidOccupancyFillMain", SF_Compute);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// Both out of line for the reason FVoxelGpuQuadPayload's are
// (VoxelGpuQuadPayload.h:53-72): TRefCountPtr<FRDGPooledBuffer> needs the
// complete type to destroy, and the header has only the forward declaration.
FVoxelFluidOccupancyVolume::FVoxelFluidOccupancyVolume() = default;

FVoxelFluidOccupancyVolume::~FVoxelFluidOccupancyVolume()
{
	// The pooled buffer is the last reference to an RHI resource that render
	// commands may still be a frame or two behind on. Releasing it wherever
	// the volume happens to die fails as a crash on exit rather than as
	// anything a compiler catches -- the same hazard, and the same fix, as
	// UVoxelGpuPoolComponent::BeginDestroy.
	if (PooledBits.IsValid())
	{
		ENQUEUE_RENDER_COMMAND(VoxelFluidOccupancyRelease)(
			[Buffer = MoveTemp(PooledBits)](FRHICommandListImmediate&) mutable
			{
				Buffer.SafeRelease();
			});
	}
}

bool FVoxelFluidOccupancyVolume::IsSupportedOnCurrentRHI()
{
	return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5;
}

FIntVector FVoxelFluidOccupancyVolume::GetOriginVoxel() const
{
	FScopeLock Lock(&QueueLock);
	return OriginVoxel;
}

bool FVoxelFluidOccupancyVolume::IsFullyUnbuilt() const
{
	FScopeLock Lock(&QueueLock);
	return bFullyUnbuilt;
}

FVector FVoxelFluidOccupancyVolume::GetOriginUU() const
{
	// The hard requirement at the top of VoxelFluidCollision.ush: the origin
	// particle positions are relative to IS the volume's minimum corner, so a
	// position maps to a voxel with one floor and one integer add.
	const FIntVector Origin = GetOriginVoxel();
	const double VoxelUU = double(vxc::kFluidVoxelUU);
	return FVector(double(Origin.X) * VoxelUU, double(Origin.Y) * VoxelUU,
	               double(Origin.Z) * VoxelUU);
}

void FVoxelFluidOccupancyVolume::SetOriginVoxel(const FIntVector& NewOriginVoxel)
{
	FScopeLock Lock(&QueueLock);
	if (NewOriginVoxel == OriginVoxel && !bClearPending)
	{
		return;
	}

	OriginVoxel = NewOriginVoxel;
	bClearPending = true;
	bFullyUnbuilt = true;

	// Anything queued was packed against the OLD origin, so its volume-local
	// coordinates now name different world voxels. Dropping it is the only
	// correct answer; keeping it would write real terrain bits at the wrong
	// place, which is worse than a hole because it looks like terrain.
	PendingRegions.Reset();
}

// ---------------------------------------------------------------------------
// Region preparation
// ---------------------------------------------------------------------------

bool FVoxelFluidOccupancyVolume::SnapRegion(const FIntVector& OriginVoxel,
                                            const FIntVector& MinVoxelWorld,
                                            const FIntVector& MaxVoxelWorld,
                                            FIntVector& OutMinVoxelLocal,
                                            FIntVector& OutSizeVoxels)
{
	// World -> volume-local, then let the reference do the snapping. int64
	// through the subtraction and clamped into int32 afterwards: world voxel
	// coordinates span the whole planet and a chunk key far outside the volume
	// would otherwise overflow on the way to being rejected.
	const int64 MinL[3] = {int64(MinVoxelWorld.X) - OriginVoxel.X,
	                       int64(MinVoxelWorld.Y) - OriginVoxel.Y,
	                       int64(MinVoxelWorld.Z) - OriginVoxel.Z};
	const int64 MaxL[3] = {int64(MaxVoxelWorld.X) - OriginVoxel.X,
	                       int64(MaxVoxelWorld.Y) - OriginVoxel.Y,
	                       int64(MaxVoxelWorld.Z) - OriginVoxel.Z};

	// Anything wholly on one side of the volume is refused here rather than
	// squeezed into an int32 first.
	constexpr int64 Guard = 1 << 20;
	int32 MinIn[3], MaxIn[3];
	for (int32 A = 0; A < 3; ++A)
	{
		if (MaxL[A] < 0 || MinL[A] >= vxc::kFluidVolumeDimVoxels)
		{
			return false;
		}
		MinIn[A] = int32(FMath::Clamp<int64>(MinL[A], -Guard, Guard));
		MaxIn[A] = int32(FMath::Clamp<int64>(MaxL[A], -Guard, Guard));
	}

	vxc::FluidRegion Region;
	if (!vxc::fluidSnapRegion(MinIn, MaxIn, Region))
	{
		return false;
	}
	OutMinVoxelLocal = FIntVector(Region.minVoxel[0], Region.minVoxel[1], Region.minVoxel[2]);
	OutSizeVoxels = FIntVector(Region.sizeVoxels[0], Region.sizeVoxels[1], Region.sizeVoxels[2]);
	return true;
}

int64 FVoxelFluidOccupancyVolume::BrickWordCount(const FIntVector& SizeVoxels)
{
	vxc::FluidRegion Region;
	Region.sizeVoxels[0] = SizeVoxels.X;
	Region.sizeVoxels[1] = SizeVoxels.Y;
	Region.sizeVoxels[2] = SizeVoxels.Z;
	return vxc::fluidRegionBrickWordCount(Region);
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

bool FVoxelFluidOccupancyVolume::UpdateRegion(FVoxelFluidOccupancyRegion&& Region, FString& OutError)
{
	vxc::FluidRegion Ref;
	Ref.minVoxel[0] = Region.MinVoxel.X;
	Ref.minVoxel[1] = Region.MinVoxel.Y;
	Ref.minVoxel[2] = Region.MinVoxel.Z;
	Ref.sizeVoxels[0] = Region.SizeVoxels.X;
	Ref.sizeVoxels[1] = Region.SizeVoxels.Y;
	Ref.sizeVoxels[2] = Region.SizeVoxels.Z;

	// REFUSED, NOT CLAMPED. Every one of these means the caller computed the
	// box wrong, and a clamp leaves part of the dirty box holding pre-edit
	// bits -- invisible until water walks through the wall somebody just dug.
	if (!vxc::fluidRegionInBounds(Ref))
	{
		OutError = FString::Printf(TEXT("region min (%d,%d,%d) size (%d,%d,%d) is not inside the %d^3 volume"),
		                           Region.MinVoxel.X, Region.MinVoxel.Y, Region.MinVoxel.Z,
		                           Region.SizeVoxels.X, Region.SizeVoxels.Y, Region.SizeVoxels.Z,
		                           DimVoxels);
	}
	else if (!vxc::fluidRegionIsAligned(Ref))
	{
		OutError = FString::Printf(TEXT("region min (%d,%d,%d) size (%d,%d,%d) is not snapped to the update grid ")
		                           TEXT("(%d voxels in x, %d in y/z) -- use SnapRegion"),
		                           Region.MinVoxel.X, Region.MinVoxel.Y, Region.MinVoxel.Z,
		                           Region.SizeVoxels.X, Region.SizeVoxels.Y, Region.SizeVoxels.Z,
		                           kVoxelsPerWord, kFluidOccBrickEdge);
	}
	else if (int64(Region.BrickBits.Num()) != vxc::fluidRegionBrickWordCount(Ref))
	{
		OutError = FString::Printf(TEXT("region carries %d brick words, expected %lld"),
		                           Region.BrickBits.Num(),
		                           (long long)vxc::fluidRegionBrickWordCount(Ref));
	}
	else
	{
		FScopeLock Lock(&QueueLock);
		Stats.RegionsQueued++;
		Stats.BrickWordsUploaded += uint64(Region.BrickBits.Num());
		PendingRegions.Emplace(MoveTemp(Region));
		return true;
	}

	FScopeLock Lock(&QueueLock);
	Stats.RegionsRejected++;
	return false;
}

int32 FVoxelFluidOccupancyVolume::NumPendingRegions() const
{
	FScopeLock Lock(&QueueLock);
	return PendingRegions.Num();
}

void FVoxelFluidOccupancyVolume::SetMaxRegionsPerFlush(int32 InMax)
{
	MaxRegionsPerFlush = FMath::Max(1, InMax);
}

FVoxelFluidOccupancyVolume::FStats FVoxelFluidOccupancyVolume::GetStats() const
{
	FScopeLock Lock(&QueueLock);
	return Stats;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

FRDGBufferRef FVoxelFluidOccupancyVolume::Register(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!PooledBits.IsValid())
	{
		return nullptr;
	}
	return GraphBuilder.RegisterExternalBuffer(PooledBits);
}

FRDGBufferSRVRef FVoxelFluidOccupancyVolume::CreateBitsSRV(FRDGBuilder& GraphBuilder)
{
	FRDGBufferRef Buffer = Register(GraphBuilder);
	if (Buffer == nullptr)
	{
		return nullptr;
	}
	// Typed, not structured: VoxelFluidCollision.ush declares Buffer<uint> so
	// the collision read is a single typed load per voxel test, and the
	// contract names it that way (VoxelFluidContract.ush:66).
	return GraphBuilder.CreateSRV(Buffer, PF_R32_UINT);
}

FRDGBufferRef FVoxelFluidOccupancyVolume::AddPasses(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());

	if (!IsSupportedOnCurrentRHI())
	{
		return nullptr;
	}

	if (!PooledBits.IsValid())
	{
		// 4,194,304 words of 4 bytes = 16 MiB, allocated once and kept. A
		// vertex-buffer-usage desc rather than a structured one, because the
		// reader binds it as Buffer<uint>.
		const FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), uint32(WordCount));
		PooledBits = AllocatePooledBuffer(Desc, TEXT("VoxelFluid.OccupancyBits"));
		FScopeLock Lock(&QueueLock);
		bClearPending = true;
		UE_LOG(LogVoxelFluidOccupancy, Log,
		       TEXT("occupancy volume allocated: %d^3 voxels, %lld words, %.1f MiB"),
		       DimVoxels, (long long)WordCount, double(WordCount * 4) / (1024.0 * 1024.0));
	}

	FRDGBufferRef Bits = GraphBuilder.RegisterExternalBuffer(PooledBits);
	FRDGBufferUAVRef BitsUAV = GraphBuilder.CreateUAV(Bits, PF_R32_UINT);

	// ONE LOCKED SECTION for the whole state read: the clear flag and the
	// queue drain have to agree about which origin they belong to. Taking the
	// lock twice would let a SetOriginVoxel land between them and dispatch
	// regions packed against the new origin into a volume that was never
	// cleared for it.
	bool bDoClear = false;
	TArray<FVoxelFluidOccupancyRegion> ToApply;
	{
		FScopeLock Lock(&QueueLock);
		Stats.AddPassesCount++; // the solver's same-graph ordering guard reads this
		bDoClear = bClearPending;
		bClearPending = false;
		if (bDoClear)
		{
			Stats.ClearCount++;
		}

		const int32 Count = FMath::Min(PendingRegions.Num(), MaxRegionsPerFlush);
		if (Count > 0)
		{
			// Moved one at a time rather than by iterator range: BrickBits is
			// up to a few hundred KB per region and a copy here would put the
			// upload cost on the render thread twice.
			ToApply.Reserve(Count);
			for (int32 I = 0; I < Count; ++I)
			{
				ToApply.Emplace(MoveTemp(PendingRegions[I]));
			}
			PendingRegions.RemoveAt(0, Count, EAllowShrinking::No);
		}
	}

	if (bDoClear)
	{
		// RDG's own clear rather than a kernel that stores a constant: a
		// second mechanism for "fill a buffer with a value" is a second place
		// for the value to be wrong. All ones, so unbuilt is SOLID.
		AddClearUAVPass(GraphBuilder, BitsUAV, kUnbuiltWord);
	}

	// One UAV shared by every region pass, so RDG puts a barrier between them.
	// That is deliberate and not an oversight: the regions are USUALLY disjoint
	// (the alignment rule guarantees no two threads share a word WITHIN a
	// dispatch), but nothing stops a caller queueing two overlapping boxes in
	// one flush -- re-dirtying the same chunk twice in a frame does exactly
	// that -- and then the later pack is the correct one and must win. Skipping
	// the barrier would make which one wins undefined, for a handful of
	// microseconds on a pass that runs a few times a frame.
	uint64 WordsThisFlush = 0;
	for (const FVoxelFluidOccupancyRegion& Region : ToApply)
	{
		FRDGBufferRef BrickBits = CreateStructuredBuffer(
			GraphBuilder, TEXT("VoxelFluid.RegionBrickBits"), sizeof(uint32),
			Region.BrickBits.Num(), Region.BrickBits.GetData(),
			Region.BrickBits.Num() * sizeof(uint32));

		FVoxelFluidOccupancyFillCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelFluidOccupancyFillCS::FParameters>();
		Params->FluidOccupancyBits = BitsUAV;
		Params->FluidRegionBrickBits = GraphBuilder.CreateSRV(BrickBits);
		Params->FluidVolumeDimVoxels = uint32(DimVoxels);
		Params->FluidRegionMinVoxel = FUintVector(uint32(Region.MinVoxel.X), uint32(Region.MinVoxel.Y),
		                                          uint32(Region.MinVoxel.Z));
		Params->FluidRegionSizeVoxels = FUintVector(uint32(Region.SizeVoxels.X), uint32(Region.SizeVoxels.Y),
		                                            uint32(Region.SizeVoxels.Z));

		// One thread per output word: x counts 32-voxel columns, y and z count
		// voxels. The z dimension is NOT divided -- the group is flat in z, so
		// one group per z slice. A full-volume region is (2, 64, 512) groups,
		// every dimension well under the 65,535 limit, which is the reason for
		// the 8x8x1 shape in the first place.
		const int32 WordsX = Region.SizeVoxels.X / kVoxelsPerWord;
		const FIntVector GroupCount(FMath::DivideAndRoundUp(WordsX, kFillGroupX),
		                            FMath::DivideAndRoundUp(Region.SizeVoxels.Y, kFillGroupY),
		                            Region.SizeVoxels.Z);

		TShaderMapRef<FVoxelFluidOccupancyFillCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VoxelFluid.OccupancyFill(%dx%dx%d @ %d,%d,%d)",
			               Region.SizeVoxels.X, Region.SizeVoxels.Y, Region.SizeVoxels.Z,
			               Region.MinVoxel.X, Region.MinVoxel.Y, Region.MinVoxel.Z),
			Shader, Params, GroupCount);

		WordsThisFlush += uint64(WordsX) * uint64(Region.SizeVoxels.Y) * uint64(Region.SizeVoxels.Z);
	}

	if (ToApply.Num() > 0)
	{
		FScopeLock Lock(&QueueLock);
		Stats.RegionsApplied += uint64(ToApply.Num());
		Stats.WordsWritten += WordsThisFlush;
		bFullyUnbuilt = false;
	}

	return Bits;
}

// Compile-time agreement between this file, the kernel and the CPU reference.
// Every one of these is a number that appears in more than one place; a
// mismatch is a partially filled volume, and a partially filled volume is
// solid where it should be air, which reads as terrain rather than as a bug.
static_assert(kFluidOccBrickEdge == 8, "FluidOccupancyFillMain shifts by 3 for the brick index");
static_assert(kVoxelsPerWord == 32, "FluidOccupancyFillMain shifts by 5 for the word index");
static_assert(kBricksPerWordX == 4, "FluidOccupancyFillMain's unrolled gather is four bricks wide");
static_assert(kBrickWords == 16, "FluidOccupancyFillMain multiplies the brick index by 16");
static_assert(vxc::kFluidVolumeWords * 4 == 16 * 1024 * 1024, "the plan's 16 MB budget");
