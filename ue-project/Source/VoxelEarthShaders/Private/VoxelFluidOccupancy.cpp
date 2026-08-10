#include "VoxelFluidOccupancy.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "PixelFormat.h"
#include "RenderingThread.h"
#include "Misc/AutomationTest.h"

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
			SHADER_PARAMETER(FUintVector, FluidVolumeWrapOffsetVoxel)
			SHADER_PARAMETER(FUintVector, FluidRegionMinVoxel)
			SHADER_PARAMETER(FUintVector, FluidRegionSizeVoxels)
		END_SHADER_PARAMETER_STRUCT()
	};

	// The sub-box "I do not know yet" write, and the other half of a toroidal
	// recentre. Same thread mapping and same region uniforms as the fill by
	// design, so the dispatch calculation below is shared: one region shape, one
	// off-by-one to get wrong. No brick upload -- that is the point of it.
	class FVoxelFluidOccupancyMarkUnbuiltCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidOccupancyMarkUnbuiltCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidOccupancyMarkUnbuiltCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FluidOccupancyBits)
			SHADER_PARAMETER(uint32,      FluidVolumeDimVoxels)
			SHADER_PARAMETER(FUintVector, FluidVolumeWrapOffsetVoxel)
			SHADER_PARAMETER(FUintVector, FluidRegionMinVoxel)
			SHADER_PARAMETER(FUintVector, FluidRegionSizeVoxels)
		END_SHADER_PARAMETER_STRUCT()
	};

	// Contract item 8: the particle half of a recentre. Writes ParticlesRW
	// through the contract's own binding name; see the kernel's header for why
	// it lives in the occupancy shader and not the solver's.
	constexpr int32 kRebaseGroupX = 64;   // matches [numthreads(64,1,1)]

	class FVoxelFluidRebaseParticlesCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelFluidRebaseParticlesCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelFluidRebaseParticlesCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVoxelFluidParticle>, ParticlesRW)
			SHADER_PARAMETER(FVector3f, FluidRebaseDeltaUU)
			SHADER_PARAMETER(uint32,    FluidRebaseSlotCount)
		END_SHADER_PARAMETER_STRUCT()
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelFluidOccupancyFillCS, VOXEL_FLUID_OCCUPANCY_USF,
                        "FluidOccupancyFillMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidOccupancyMarkUnbuiltCS, VOXEL_FLUID_OCCUPANCY_USF,
                        "FluidOccupancyMarkUnbuiltMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelFluidRebaseParticlesCS, VOXEL_FLUID_OCCUPANCY_USF,
                        "FluidRebaseParticlesMain", SF_Compute);

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

// The tracking grid IS the host's initial-fill grid. If those two ever diverge
// an initial cell would land without setting exactly one bit, and IsRegionBuilt
// would answer false forever for terrain that is in fact built.
static_assert(FVoxelFluidOccupancyVolume::BuiltCellVoxels % vxc::kFluidRegionAlignX == 0,
              "a built cell must be a whole number of update-grid words in x");
static_assert(FVoxelFluidOccupancyVolume::BuiltCellVoxels % vxc::kFluidRegionAlignYZ == 0,
              "a built cell must be a whole number of bricks in y/z");
static_assert(FVoxelFluidOccupancyVolume::BuiltCellsPerAxis == 8, "8x8x8 = 512 bits");

void FVoxelFluidOccupancyVolume::MarkCellsBuilt_Locked(const FIntVector& MinVoxel,
                                                       const FIntVector& SizeVoxels)
{
	// WHOLE cells only: the ceil on the low corner and the floor on the high
	// one. A dirty-edit region of 32x8x8 voxels sitting inside an unbuilt cell
	// leaves it unbuilt, which is the conservative answer -- the rest of that
	// cell really is still all-ones.
	const int32 E = BuiltCellVoxels;
	int32 Lo[3], Hi[3];
	const int32 Min[3] = {MinVoxel.X, MinVoxel.Y, MinVoxel.Z};
	const int32 Size[3] = {SizeVoxels.X, SizeVoxels.Y, SizeVoxels.Z};
	for (int32 A = 0; A < 3; ++A)
	{
		Lo[A] = FMath::DivideAndRoundUp(Min[A], E);
		Hi[A] = (Min[A] + Size[A]) / E - 1;
		if (Hi[A] >= BuiltCellsPerAxis) { Hi[A] = BuiltCellsPerAxis - 1; }
		if (Lo[A] < 0) { Lo[A] = 0; }
	}
	for (int32 Z = Lo[2]; Z <= Hi[2]; ++Z)
	{
		for (int32 Y = Lo[1]; Y <= Hi[1]; ++Y)
		{
			for (int32 X = Lo[0]; X <= Hi[0]; ++X)
			{
				const int32 Bit = (Z * BuiltCellsPerAxis + Y) * BuiltCellsPerAxis + X;
				BuiltCellBits[Bit >> 6] |= (uint64(1) << (Bit & 63));
			}
		}
	}
}

bool FVoxelFluidOccupancyVolume::IsRegionBuilt(const FIntVector& WorldVoxel) const
{
	FScopeLock Lock(&QueueLock);
	// THE ONE CONVERSION. The built-cell grid is VOLUME-LOCAL (the header's
	// LOCAL space) and so is every write to it: MarkCellsBuilt_Locked takes a
	// region's local MinVoxel, and RecentreTo renames the cells rather than
	// moving them. So the query is one subtraction and one divide, with NO WRAP
	// -- the wrap offset belongs to STORAGE addressing on the GPU and applying
	// it here would ask about a different cell.
	//
	// int64 through the subtraction: world voxel coordinates span the planet.
	const int64 Local[3] = {int64(WorldVoxel.X) - OriginVoxel.X,
	                        int64(WorldVoxel.Y) - OriginVoxel.Y,
	                        int64(WorldVoxel.Z) - OriginVoxel.Z};
	int32 Cell[3];
	for (int32 A = 0; A < 3; ++A)
	{
		if (Local[A] < 0 || Local[A] >= DimVoxels)
		{
			return false; // outside the volume: no collision data exists here
		}
		Cell[A] = int32(Local[A] / BuiltCellVoxels);
	}
	const int32 Bit = (Cell[2] * BuiltCellsPerAxis + Cell[1]) * BuiltCellsPerAxis + Cell[0];
	return (BuiltCellBits[Bit >> 6] & (uint64(1) << (Bit & 63))) != 0;
}

bool FVoxelFluidOccupancyVolume::ContainsWorldVoxel(const FIntVector& WorldVoxel) const
{
	FScopeLock Lock(&QueueLock);
	const int64 Local[3] = {int64(WorldVoxel.X) - OriginVoxel.X,
	                        int64(WorldVoxel.Y) - OriginVoxel.Y,
	                        int64(WorldVoxel.Z) - OriginVoxel.Z};
	for (int32 A = 0; A < 3; ++A)
	{
		if (Local[A] < 0 || Local[A] >= DimVoxels)
		{
			return false;
		}
	}
	return true;
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
	FMemory::Memzero(BuiltCellBits, sizeof(BuiltCellBits));

	// A hard reset makes every slot unbuilt, so there is nothing for a rolling
	// offset to preserve and zero is the offset that makes a raw dump of the
	// volume readable. Any queued unbuilt marks go with it -- the whole-volume
	// clear subsumes them.
	WrapOffsetVoxel = FIntVector::ZeroValue;
	PendingUnbuiltCells.Reset();

	// AND NO PARTICLE REBASE IS OWED. This call assumes no particles survive it:
	// it is the first latch (there are none) or a teleport (their water does not
	// follow the camera across the world -- it despawns at the boundary and the
	// ledger is credited, which is the designed outcome). Carrying a rebase over
	// a teleport would shift positions by a delta that is not even exactly
	// representable at planet scale, to move particles that are about to be
	// despawned anyway. RecentreTo is the call that preserves water.
	PendingRebaseDeltaVoxels = FIntVector::ZeroValue;

	// Anything queued was packed against the OLD origin, so its volume-local
	// coordinates now name different world voxels. Dropping it is the only
	// correct answer; keeping it would write real terrain bits at the wrong
	// place, which is worse than a hole because it looks like terrain.
	PendingRegions.Reset();
}

FIntVector FVoxelFluidOccupancyVolume::GetWrapOffsetVoxel() const
{
	FScopeLock Lock(&QueueLock);
	return WrapOffsetVoxel;
}

bool FVoxelFluidOccupancyVolume::GetCellBuilt_Locked(const FIntVector& Cell) const
{
	const int32 Bit = (Cell.Z * BuiltCellsPerAxis + Cell.Y) * BuiltCellsPerAxis + Cell.X;
	return (BuiltCellBits[Bit >> 6] & (uint64(1) << (Bit & 63))) != 0;
}

bool FVoxelFluidOccupancyVolume::RecentreTo(const FIntVector& NewOriginVoxel,
                                            TArray<FVoxelFluidOccupancyRefillCell>& OutRefillCells,
                                            FIntVector& OutDeltaVoxels, FString& OutError)
{
	OutRefillCells.Reset();
	OutDeltaVoxels = FIntVector::ZeroValue;

	FScopeLock Lock(&QueueLock);

	const int64 Delta[3] = {int64(NewOriginVoxel.X) - OriginVoxel.X,
	                        int64(NewOriginVoxel.Y) - OriginVoxel.Y,
	                        int64(NewOriginVoxel.Z) - OriginVoxel.Z};
	if (Delta[0] == 0 && Delta[1] == 0 && Delta[2] == 0)
	{
		return true; // no-op, and an empty refill list is the honest answer
	}

	// REFUSED, NOT ROUNDED. A rounded delta puts the window somewhere other than
	// where the caller thinks it is, and every world coordinate the caller then
	// packs is off by the remainder -- water colliding against terrain a few
	// metres away, which reads as worldgen.
	if (!vxc::fluidRecentreDeltaIsAligned(Delta))
	{
		OutError = FString::Printf(
			TEXT("recentre delta (%lld,%lld,%lld) is not a whole multiple of the %d-voxel ")
			TEXT("recentre step on every axis -- the wrap seam would land mid-word and the ")
			TEXT("entering slab would not be brick-aligned"),
			(long long)Delta[0], (long long)Delta[1], (long long)Delta[2],
			vxc::kFluidRecentreStepVoxels);
		Stats.RecentresRefused++;
		return false;
	}

	// A SLIDE HAS TO FIT IN A SLIDE. Origins are int32 world voxels, so their
	// difference can overflow int32 -- and the particle rebase cannot represent
	// a shift past vxc::kFluidRebaseExactMaxVoxels exactly anyway. Both cases
	// are the same thing physically: a teleport, where preserving the window
	// buys nothing because none of it survives. Refused with the caller told
	// which call to make instead, rather than silently truncating a delta and
	// putting the window somewhere neither side expects.
	for (int32 A = 0; A < 3; ++A)
	{
		if (!vxc::fluidRebaseIsExactlyRepresentable(Delta[A]))
		{
			OutError = FString::Printf(
				TEXT("recentre delta (%lld,%lld,%lld) is a teleport, not a slide (axis %d is past ")
				TEXT("%lld voxels) -- use SetOriginVoxel, which invalidates everything, because a ")
				TEXT("window this far away shares no terrain with the old one"),
				(long long)Delta[0], (long long)Delta[1], (long long)Delta[2], A,
				(long long)vxc::kFluidRebaseExactMaxVoxels);
			Stats.RecentresRefused++;
			return false;
		}
	}

	// --- 1. which cells of the NEW window are not carried over ---------------
	//
	// By containment, one cell at a time. Slab arithmetic would double-count the
	// overlap on a diagonal move (and mark, and refill, it two or three times),
	// and would need a special case for a move larger than the window. This
	// needs neither: a jump past the window simply fails the test everywhere.
	//
	// UNLESS A CLEAR IS ALREADY PENDING, in which case nothing is carried over
	// no matter where the window goes: the clear has not run yet but it will,
	// and it will take every slot with it. Sliding on top of that and reporting
	// only one slab as entering would leave the rest of the volume unbuilt with
	// nobody asked to refill it -- water frozen everywhere but the slab, with no
	// counter saying why.
	bool bEntering[BuiltCellsPerAxis * BuiltCellsPerAxis * BuiltCellsPerAxis] = {};
	int32 EnteringCount = 0;
	for (int32 Cz = 0; Cz < BuiltCellsPerAxis; ++Cz)
	{
		for (int32 Cy = 0; Cy < BuiltCellsPerAxis; ++Cy)
		{
			for (int32 Cx = 0; Cx < BuiltCellsPerAxis; ++Cx)
			{
				const int32 Cell[3] = {Cx, Cy, Cz};
				if (!bClearPending && vxc::fluidRecentreCellIsResident(Delta, Cell))
				{
					continue;
				}
				bEntering[(Cz * BuiltCellsPerAxis + Cy) * BuiltCellsPerAxis + Cx] = true;
				++EnteringCount;
			}
		}
	}

	// --- 2. queued regions, translated into the new frame --------------------
	//
	// A packed region's bits are correct for a WORLD box; only its local name
	// changed, by exactly -Delta. So it can be kept, which matters because it is
	// usually an edit somebody just made. One that no longer fits is dropped --
	// and its surviving cells are forced into the entering set, so a dropped
	// edit leaves unbuilt space (water freezes, then refills) rather than
	// pre-edit bits (water walks through the wall that was just dug).
	{
		TArray<FVoxelFluidOccupancyRegion> Kept;
		Kept.Reserve(PendingRegions.Num());
		for (FVoxelFluidOccupancyRegion& R : PendingRegions)
		{
			const FIntVector NewMin(R.MinVoxel.X - int32(Delta[0]), R.MinVoxel.Y - int32(Delta[1]),
			                        R.MinVoxel.Z - int32(Delta[2]));
			const int32 Min[3] = {NewMin.X, NewMin.Y, NewMin.Z};
			const int32 Size[3] = {R.SizeVoxels.X, R.SizeVoxels.Y, R.SizeVoxels.Z};
			bool bFits = true;
			for (int32 A = 0; A < 3; ++A)
			{
				if (Min[A] < 0 || Min[A] + Size[A] > DimVoxels) { bFits = false; }
			}
			if (bFits)
			{
				R.MinVoxel = NewMin;
				Kept.Emplace(MoveTemp(R));
				continue;
			}
			Stats.RegionsRetranslatedOut++;
			for (int32 Cz = 0; Cz < BuiltCellsPerAxis; ++Cz)
			{
				for (int32 Cy = 0; Cy < BuiltCellsPerAxis; ++Cy)
				{
					for (int32 Cx = 0; Cx < BuiltCellsPerAxis; ++Cx)
					{
						const int32 C[3] = {Cx, Cy, Cz};
						bool bOverlaps = true;
						for (int32 A = 0; A < 3; ++A)
						{
							const int32 CellLo = C[A] * BuiltCellVoxels;
							if (Min[A] >= CellLo + BuiltCellVoxels || Min[A] + Size[A] <= CellLo)
							{
								bOverlaps = false;
							}
						}
						const int32 Bit = (Cz * BuiltCellsPerAxis + Cy) * BuiltCellsPerAxis + Cx;
						if (bOverlaps && !bEntering[Bit])
						{
							bEntering[Bit] = true;
							++EnteringCount;
						}
					}
				}
			}
		}
		PendingRegions = MoveTemp(Kept);
	}

	// --- 3. slide the window -------------------------------------------------
	//
	// This is the whole of "the bits do not move": the origin advances and the
	// storage offset advances with it, so a world voxel that is still in view
	// keeps the exact slot it had. floorMod, not %, because the window slides
	// both ways.
	OriginVoxel = NewOriginVoxel;
	WrapOffsetVoxel = FIntVector(vxc::fluidWrapOffsetAfterMove(WrapOffsetVoxel.X, Delta[0]),
	                             vxc::fluidWrapOffsetAfterMove(WrapOffsetVoxel.Y, Delta[1]),
	                             vxc::fluidWrapOffsetAfterMove(WrapOffsetVoxel.Z, Delta[2]));
	OutDeltaVoxels = FIntVector(int32(Delta[0]), int32(Delta[1]), int32(Delta[2]));
	PendingRebaseDeltaVoxels += OutDeltaVoxels;

	// --- 4. the built-cell grid SHIFTS; it is not cleared --------------------
	//
	// The grid is local, so a slide renames its cells by the same step. Clearing
	// it wholesale would be safe and over-conservative: every faucet would defer
	// through a refill of terrain that is in fact already built, which is the
	// hovering-water symptom arriving by a second route.
	{
		uint64 Shifted[BuiltCellWords] = {};
		const int32 DeltaCells[3] = {int32(Delta[0] / BuiltCellVoxels),
		                             int32(Delta[1] / BuiltCellVoxels),
		                             int32(Delta[2] / BuiltCellVoxels)};
		for (int32 Cz = 0; Cz < BuiltCellsPerAxis; ++Cz)
		{
			for (int32 Cy = 0; Cy < BuiltCellsPerAxis; ++Cy)
			{
				for (int32 Cx = 0; Cx < BuiltCellsPerAxis; ++Cx)
				{
					const int32 Bit = (Cz * BuiltCellsPerAxis + Cy) * BuiltCellsPerAxis + Cx;
					if (bEntering[Bit])
					{
						continue; // entering cells are unbuilt by definition
					}
					// Resident: it was cell (C + DeltaCells) under the old origin.
					const FIntVector Old(Cx + DeltaCells[0], Cy + DeltaCells[1], Cz + DeltaCells[2]);
					if (GetCellBuilt_Locked(Old))
					{
						Shifted[Bit >> 6] |= (uint64(1) << (Bit & 63));
					}
				}
			}
		}
		FMemory::Memcpy(BuiltCellBits, Shifted, sizeof(BuiltCellBits));
	}

	// --- 5. hand the entering cells back, and queue their unbuilt marks ------
	OutRefillCells.Reserve(EnteringCount);
	for (int32 Cz = 0; Cz < BuiltCellsPerAxis; ++Cz)
	{
		for (int32 Cy = 0; Cy < BuiltCellsPerAxis; ++Cy)
		{
			for (int32 Cx = 0; Cx < BuiltCellsPerAxis; ++Cx)
			{
				if (!bEntering[(Cz * BuiltCellsPerAxis + Cy) * BuiltCellsPerAxis + Cx])
				{
					continue;
				}
				FVoxelFluidOccupancyRefillCell Cell;
				Cell.MinVoxel = FIntVector(Cx, Cy, Cz) * BuiltCellVoxels;
				Cell.SizeVoxels = FIntVector(BuiltCellVoxels, BuiltCellVoxels, BuiltCellVoxels);
				OutRefillCells.Add(Cell);
				// The pending whole-volume clear already says "all unbuilt", so
				// queuing 512 mark dispatches behind it would be 512 passes to
				// restate it. The refill list above is still the full 512 cells
				// -- somebody has to be asked to rebuild them.
				if (!bClearPending)
				{
					PendingUnbuiltCells.Add(Cell);
				}
			}
		}
	}

	bFullyUnbuilt = (EnteringCount == BuiltCellsPerAxis * BuiltCellsPerAxis * BuiltCellsPerAxis);
	Stats.Recentres++;
	Stats.CellsEntered += uint64(EnteringCount);

	UE_LOG(LogVoxelFluidOccupancy, Verbose,
	       TEXT("occupancy recentre: origin -> (%d,%d,%d), delta (%d,%d,%d), wrap offset "
	            "(%d,%d,%d), %d/%d cells entering (%.1f%% of the volume)"),
	       OriginVoxel.X, OriginVoxel.Y, OriginVoxel.Z, OutDeltaVoxels.X, OutDeltaVoxels.Y,
	       OutDeltaVoxels.Z, WrapOffsetVoxel.X, WrapOffsetVoxel.Y, WrapOffsetVoxel.Z,
	       EnteringCount, BuiltCellsPerAxis * BuiltCellsPerAxis * BuiltCellsPerAxis,
	       100.0 * double(EnteringCount) /
	           double(BuiltCellsPerAxis * BuiltCellsPerAxis * BuiltCellsPerAxis));
	return true;
}

FIntVector FVoxelFluidOccupancyVolume::TakePendingRebaseDeltaVoxels()
{
	FScopeLock Lock(&QueueLock);
	const FIntVector Delta = PendingRebaseDeltaVoxels;
	PendingRebaseDeltaVoxels = FIntVector::ZeroValue;
	if (Delta != FIntVector::ZeroValue)
	{
		Stats.RebaseDeltasTaken++;
	}
	return Delta;
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
	FIntVector Wrap = FIntVector::ZeroValue;
	TArray<FVoxelFluidOccupancyRefillCell> ToMarkUnbuilt;
	TArray<FVoxelFluidOccupancyRegion> ToApply;
	{
		FScopeLock Lock(&QueueLock);
		Stats.AddPassesCount++; // the solver's same-graph ordering guard reads this
		// Read inside the same section: a RecentreTo landing between this and the
		// queue drain would dispatch regions addressed with the wrong wrap.
		Wrap = WrapOffsetVoxel;
		bDoClear = bClearPending;
		bClearPending = false;
		if (bDoClear)
		{
			Stats.ClearCount++;
			// The clear puts every bit back to solid, so nothing is built any
			// more. Inside the same locked section as the queue drain, so a
			// region applied in this very flush still marks its cell below.
			FMemory::Memzero(BuiltCellBits, sizeof(BuiltCellBits));
		}

		// UNBUILT MARKS GO WHOLE, NOT BUDGETED, and they are drained before the
		// region list on purpose. A mark and a fill for the same entering cell
		// can arrive in the same flush (the caller refills as fast as it packs),
		// and the mark must be the earlier pass or it erases the fill it was
		// supposed to precede -- a stale-terrain bug that only shows up when the
		// queue happens to be short. They cost nothing to keep together: a
		// 64-voxel step is 64 cells, 0.5 M words, no upload.
		if (PendingUnbuiltCells.Num() > 0)
		{
			ToMarkUnbuilt = MoveTemp(PendingUnbuiltCells);
			PendingUnbuiltCells.Reset();
			Stats.CellsMarkedUnbuilt += uint64(ToMarkUnbuilt.Num());
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
			// Marked HERE, in the same locked section that decided the origin
			// these regions belong to. Doing it after the pass loop would let a
			// SetOriginVoxel land in between and leave bits set for cells that
			// now name different world voxels.
			for (const FVoxelFluidOccupancyRegion& Region : ToApply)
			{
				MarkCellsBuilt_Locked(Region.MinVoxel, Region.SizeVoxels);
			}
		}
	}

	if (bDoClear)
	{
		// RDG's own clear rather than a kernel that stores a constant: a
		// second mechanism for "fill a buffer with a value" is a second place
		// for the value to be wrong. All ones, so unbuilt is SOLID.
		AddClearUAVPass(GraphBuilder, BitsUAV, kUnbuiltWord);
	}

	// THE INVARIANT THE WHOLE-WORD WRITE RESTS ON, checked once per flush rather
	// than trusted. If the seam ever stopped landing on an output-word boundary,
	// every region dispatched below would tear one word somewhere in the volume:
	// a 32-voxel stripe of the wrong terrain, invisible except to the verify
	// gate and only by luck. It cannot happen through RecentreTo (which refuses
	// unaligned deltas) -- which is exactly why a check here is cheap.
	{
		const int32 WrapArr[3] = {Wrap.X, Wrap.Y, Wrap.Z};
		checkf(vxc::fluidWrapOffsetIsAligned(WrapArr),
		       TEXT("occupancy wrap offset (%d,%d,%d) is not a multiple of the %d-voxel "
		            "recentre step -- the wrap seam is mid-word and every region write below "
		            "would tear"),
		       Wrap.X, Wrap.Y, Wrap.Z, vxc::kFluidRecentreStepVoxels);
	}

	// The entering slab's "I do not know yet", BEFORE any fill in this flush
	// (see the drain above for why the order is load-bearing). Without it the
	// slab keeps the terrain of the slab that just left -- perfectly plausible
	// bits from 51.2 m away, which collide as terrain instead of freezing water.
	const FUintVector WrapU(uint32(Wrap.X), uint32(Wrap.Y), uint32(Wrap.Z));
	for (const FVoxelFluidOccupancyRefillCell& Cell : ToMarkUnbuilt)
	{
		FVoxelFluidOccupancyMarkUnbuiltCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelFluidOccupancyMarkUnbuiltCS::FParameters>();
		Params->FluidOccupancyBits = BitsUAV;
		Params->FluidVolumeDimVoxels = uint32(DimVoxels);
		Params->FluidVolumeWrapOffsetVoxel = WrapU;
		Params->FluidRegionMinVoxel = FUintVector(uint32(Cell.MinVoxel.X), uint32(Cell.MinVoxel.Y),
		                                          uint32(Cell.MinVoxel.Z));
		Params->FluidRegionSizeVoxels = FUintVector(uint32(Cell.SizeVoxels.X), uint32(Cell.SizeVoxels.Y),
		                                            uint32(Cell.SizeVoxels.Z));

		const int32 MarkWordsX = Cell.SizeVoxels.X / kVoxelsPerWord;
		const FIntVector GroupCount(FMath::DivideAndRoundUp(MarkWordsX, kFillGroupX),
		                            FMath::DivideAndRoundUp(Cell.SizeVoxels.Y, kFillGroupY),
		                            Cell.SizeVoxels.Z);

		TShaderMapRef<FVoxelFluidOccupancyMarkUnbuiltCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("VoxelFluid.OccupancyMarkUnbuilt(%dx%dx%d @ %d,%d,%d)",
			               Cell.SizeVoxels.X, Cell.SizeVoxels.Y, Cell.SizeVoxels.Z,
			               Cell.MinVoxel.X, Cell.MinVoxel.Y, Cell.MinVoxel.Z),
			Shader, Params, GroupCount);
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
		Params->FluidVolumeWrapOffsetVoxel = WrapU;
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

bool FVoxelFluidOccupancyVolume::AddRebaseParticlesPass(FRDGBuilder& GraphBuilder,
                                                        FRDGBufferUAVRef ParticlesRW,
                                                        uint32 SlotCount,
                                                        const FIntVector& DeltaVoxels)
{
	check(IsInRenderingThread());
	if (!IsSupportedOnCurrentRHI() || ParticlesRW == nullptr || SlotCount == 0 ||
	    DeltaVoxels == FIntVector::ZeroValue)
	{
		return false;
	}

	// The exactness bound from voxelcore, checked rather than assumed. A delta
	// past it is a teleport and cannot come from RecentreTo's aligned steps at
	// any plausible speed; if it somehow does, refusing is better than shifting
	// every particle by a rounded distance, because the rounding is silent and
	// the refusal is a counter that does not advance.
	const int64 Max = FMath::Max3<int64>(FMath::Abs(int64(DeltaVoxels.X)),
	                                     FMath::Abs(int64(DeltaVoxels.Y)),
	                                     FMath::Abs(int64(DeltaVoxels.Z)));
	if (!vxc::fluidRebaseIsExactlyRepresentable(Max))
	{
		UE_LOG(LogVoxelFluidOccupancy, Warning,
		       TEXT("particle rebase REFUSED: delta (%d,%d,%d) voxels exceeds the exactly ")
		       TEXT("representable %lld -- that is a teleport, not a recentre"),
		       DeltaVoxels.X, DeltaVoxels.Y, DeltaVoxels.Z,
		       (long long)vxc::kFluidRebaseExactMaxVoxels);
		return false;
	}

	// CLAMPED TO THE BUFFER, not trusted. The slot bound comes from the sim's
	// own published high water, which is a CPU-side estimate of a GPU counter
	// and is allowed to over-estimate; the kernel's only bound is this uniform,
	// so an over-estimate would be an out-of-range store -- dropped on some
	// vendors and landing on others, which is the whole reason the shader lint
	// has a rule for it.
	const uint32 Capacity = ParticlesRW->GetParent() != nullptr
	                            ? ParticlesRW->GetParent()->Desc.NumElements
	                            : 0u;
	const uint32 Slots = Capacity > 0 ? FMath::Min(SlotCount, Capacity) : SlotCount;

	FVoxelFluidRebaseParticlesCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelFluidRebaseParticlesCS::FParameters>();
	Params->ParticlesRW = ParticlesRW;
	Params->FluidRebaseDeltaUU = FVector3f(vxc::fluidRebaseDeltaUU(DeltaVoxels.X),
	                                       vxc::fluidRebaseDeltaUU(DeltaVoxels.Y),
	                                       vxc::fluidRebaseDeltaUU(DeltaVoxels.Z));
	Params->FluidRebaseSlotCount = Slots;

	TShaderMapRef<FVoxelFluidRebaseParticlesCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("VoxelFluid.RebaseParticles(%u slots, %d,%d,%d voxels)", Slots,
		               DeltaVoxels.X, DeltaVoxels.Y, DeltaVoxels.Z),
		Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(int32(Slots), kRebaseGroupX), 1, 1));
	return true;
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
// Toroidal addressing (contract item 4). The wrap is a mask, and the recentre
// quantum is what keeps the seam off the middle of an output word.
static_assert((vxc::kFluidVolumeDimVoxels & (vxc::kFluidVolumeDimVoxels - 1)) == 0,
              "VoxelFluidCollision.ush and VoxelFluidOccupancy.usf both wrap with "
              "& (FluidVolumeDimVoxels - 1)");
static_assert(vxc::kFluidRecentreStepVoxels % kVoxelsPerWord == 0,
              "a recentre step must keep the wrap seam on an output-word boundary");
static_assert(vxc::kFluidRecentreStepVoxels % kFluidOccBrickEdge == 0,
              "a recentre step must keep entering slabs brick-aligned for the packer");
static_assert(kRebaseGroupX == 64, "FluidRebaseParticlesMain declares [numthreads(64,1,1)]");

// ---- automation tests -------------------------------------------------------
// No GPU and no render graph: the built-cell grid is plain state, and the
// coordinate convention it lives in is exactly what a faucet's emit gate
// depends on. Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.Fluid; Quit"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFluidOccupancyBuiltCellsTest,
	"VoxelEarth.Fluid.OccupancyBuiltCells",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FVoxelFluidOccupancyBuiltCellsTest::RunTest(const FString& Parameters)
{
	using FVolume = FVoxelFluidOccupancyVolume;
	constexpr int32 E = FVolume::BuiltCellVoxels;   // 64
	constexpr int32 Dim = FVolume::DimVoxels;       // 512

	// A planet-scale origin, because the world->local subtraction is the step
	// the query gets wrong if anybody "simplifies" it: these are the real
	// numbers from Saved/owner-playtest-round3.log's origin latch line.
	const FIntVector Origin(-612406, -507706, 19209);

	FVolume Volume;
	Volume.SetOriginVoxel(Origin);
	{
		// AddPasses is what normally consumes the clear, and it needs a graph.
		// Clearing the flag by hand is the whole reason this test is a friend:
		// everything below is about the grid, not about the dispatch.
		FScopeLock Lock(&Volume.QueueLock);
		Volume.bClearPending = false;
	}

	TestFalse(TEXT("nothing is built before any region lands"),
	          Volume.IsRegionBuilt(Origin + FIntVector(10, 10, 10)));

	// --- mark whole cells, ask in WORLD coordinates -------------------------
	// Centre, an edge cell, and the far corner cell: the three places an
	// off-by-a-cell or a missing origin subtraction shows up differently.
	const FIntVector Cells[] = {FIntVector(4, 4, 4), FIntVector(0, 3, 7), FIntVector(7, 7, 7),
	                            FIntVector(0, 0, 0)};
	for (const FIntVector& Cell : Cells)
	{
		{
			FScopeLock Lock(&Volume.QueueLock);
			Volume.MarkCellsBuilt_Locked(Cell * E, FIntVector(E, E, E));
		}
		// Every corner and the middle of the cell, in world voxels.
		const int32 Offsets[] = {0, 1, E / 2, E - 1};
		for (int32 Ox : Offsets)
		{
			for (int32 Oy : Offsets)
			{
				for (int32 Oz : Offsets)
				{
					const FIntVector World = Origin + Cell * E + FIntVector(Ox, Oy, Oz);
					TestTrue(FString::Printf(TEXT("cell (%d,%d,%d)+(%d,%d,%d) reads built"),
					                         Cell.X, Cell.Y, Cell.Z, Ox, Oy, Oz),
					         Volume.IsRegionBuilt(World));
					TestTrue(TEXT("...and is inside the volume"), Volume.ContainsWorldVoxel(World));
				}
			}
		}
		// The cell one over on each axis must NOT have been promoted (that is
		// the failure a >= vs > in the mark's floor would produce).
		for (int32 A = 0; A < 3; ++A)
		{
			FIntVector Neighbour = Cell;
			Neighbour[A] += 1;
			if (Neighbour[A] >= FVolume::BuiltCellsPerAxis)
			{
				continue;
			}
			bool bIsAlreadyMarked = false;
			for (const FIntVector& Other : Cells)
			{
				bIsAlreadyMarked |= (Other == Neighbour);
			}
			if (bIsAlreadyMarked)
			{
				continue; // marked by another case in this list
			}
			TestFalse(TEXT("the next cell over is not promoted"),
			          Volume.IsRegionBuilt(Origin + Neighbour * E));
		}
	}

	// --- outside the window is never built, and says so separately ----------
	const FIntVector Outside[] = {Origin - FIntVector(1, 0, 0), Origin - FIntVector(0, 0, 1),
	                              Origin + FIntVector(Dim, 0, 0),
	                              Origin + FIntVector(0, 0, Dim),
	                              // The playtest's own case: the faucets sat
	                              // ~55 m under the window's floor.
	                              Origin - FIntVector(0, 0, 551)};
	for (const FIntVector& World : Outside)
	{
		TestFalse(TEXT("outside the window is not built"), Volume.IsRegionBuilt(World));
		TestFalse(TEXT("outside the window is not contained"), Volume.ContainsWorldVoxel(World));
	}

	// --- a partial region does NOT promote its cell -------------------------
	{
		FScopeLock Lock(&Volume.QueueLock);
		Volume.MarkCellsBuilt_Locked(FIntVector(2, 2, 2) * E, FIntVector(32, 8, 8));
	}
	TestFalse(TEXT("a sub-cell edit region leaves its cell unbuilt"),
	          Volume.IsRegionBuilt(Origin + FIntVector(2, 2, 2) * E));

	// --- the toroidal recentre RENAMES cells; a world voxel keeps its answer -
	{
		// One step on x, two on y: the built cell (4,4,4) becomes (3,2,4) and
		// the same WORLD voxel must still read built.
		const FIntVector Step(E, 2 * E, 0);
		TArray<FVoxelFluidOccupancyRefillCell> Refill;
		FIntVector Delta = FIntVector::ZeroValue;
		FString Error;
		const FIntVector WorldInCentreCell = Origin + FIntVector(4, 4, 4) * E + FIntVector(3, 5, 7);
		TestTrue(TEXT("recentre by whole steps is accepted"),
		         Volume.RecentreTo(Origin + Step, Refill, Delta, Error));
		TestTrue(TEXT("the reported delta is the step"), Delta == Step);
		TestTrue(TEXT("a built world voxel survives the slide"),
		         Volume.IsRegionBuilt(WorldInCentreCell));
		// The slab that entered on +x is unbuilt: its far-x column of cells.
		TestFalse(TEXT("the entering slab is unbuilt"),
		          Volume.IsRegionBuilt(Origin + Step + FIntVector(7, 4, 4) * E));
		TestTrue(TEXT("the entering cells were handed back"), Refill.Num() > 0);
		// And a world voxel that left the window is outside, not built.
		TestFalse(TEXT("what slid out is no longer contained"),
		          Volume.ContainsWorldVoxel(Origin + FIntVector(0, 0, 0)));
	}

	// --- a hard reset throws the grid away ----------------------------------
	Volume.SetOriginVoxel(Origin + FIntVector(0, 0, 4096));
	TestFalse(TEXT("SetOriginVoxel clears every built bit"),
	          Volume.IsRegionBuilt(Origin + FIntVector(0, 0, 4096) + FIntVector(4, 4, 4) * E));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
