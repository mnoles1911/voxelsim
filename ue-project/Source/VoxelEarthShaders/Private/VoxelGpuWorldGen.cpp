#include "VoxelGpuWorldGen.h"
#include "VoxelGpuWorldGenGraph.h"

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderingThread.h"

// For vxc::kWorldGenVersion, which ModifyCompilationEnvironment hands to
// worldgen.ush as the version half of the mirror contract.
#include "voxelcore/core.h"

DEFINE_LOG_CATEGORY_STATIC(LogVoxelGpu, Log, All);

// The virtual path every kernel is compiled from. VoxelWorldGen.usf is a
// two-line include shim; the kernels themselves live in /VoxelCore/worldgen.ush.
//
// This has to be a macro rather than a `const TCHAR*` because
// IMPLEMENT_GLOBAL_SHADER stringizes its path argument (it prepends L to make a
// wide literal), so a variable name would be pasted into `LkShaderPath`.
#define VOXEL_WORLDGEN_USF "/VoxelEarth/VoxelWorldGen.usf"

// The HLSL struct this mirrors is five 32-bit fields with no padding. If this
// ever stops being 20 bytes the readback silently misaligns, so assert it here
// rather than debug it as "the GPU produced garbage".
static_assert(sizeof(FVoxelGpuColumnSample) == 20, "GpuColumnSample must match worldgen.ush byte for byte");

namespace
{
	// The kernels work in 8x8x8 bricks, and the mesher needs a one-brick halo
	// on every side to read apron/AO neighbours — so a region needs at least
	// 3 bricks per axis before a single interior brick exists to mesh.
	constexpr uint32 kBrickEdge = 8;
	constexpr uint32 kCellsPerBrick = 512;   // 8^3
	constexpr uint32 kMasksPerBrick = 48;    // 3 axes * 2 dirs * 8 slices
	constexpr uint32 kMaxQuadsPerMask = 32;  // upper bound, docs/gpu-mesher-design.md
	constexpr uint32 kScanBlockSize = 256;

	// ScanSumsMain scans the per-block totals in a SINGLE workgroup of 256
	// threads, so the whole dispatch can carry at most 256 blocks of 256.
	constexpr uint32 kMaxMasksPerDispatch = kScanBlockSize * kScanBlockSize;

	// Every kernel reads the same loose parameters. worldgen.ush declares them
	// as plain globals under VXC_UE precisely so they land in $Globals, which
	// is where these bind by name.
	#define VOXEL_WORLDGEN_LOOSE_PARAMETERS() \
		SHADER_PARAMETER(FUintVector2, DispatchColumns) \
		SHADER_PARAMETER(FIntPoint,    RasterOriginPx) \
		SHADER_PARAMETER(FUintVector2, RasterSize) \
		SHADER_PARAMETER(int32,        PixelSizeMm) \
		SHADER_PARAMETER(uint32,       SeedLo) \
		SHADER_PARAMETER(uint32,       SeedHi) \
		SHADER_PARAMETER(int32,        OriginVx) \
		SHADER_PARAMETER(int32,        OriginVy) \
		SHADER_PARAMETER(int32,        BrickZMin) \
		SHADER_PARAMETER(uint32,       BricksZ) \
		SHADER_PARAMETER(uint32,       ScanCount) \
		SHADER_PARAMETER(uint32,       CoarseScale) \
		SHADER_PARAMETER(uint32,       RingSkirtMask)

	// Shared compile policy for all seven kernels.
	//
	// SM6 IS NOT A PREFERENCE. worldgen.ush does all of its coordinate and
	// hash math in 64-bit integers, and 64-bit integer shader ops exist only
	// on the DXC/SM6 path. On SM5 these kernels do not compile at all, which
	// is why ShouldCompilePermutation refuses rather than emitting something
	// that would silently differ from the CPU reference.
	class FVoxelWorldGenShader : public FGlobalShader
	{
	public:
		FVoxelWorldGenShader() = default;
		FVoxelWorldGenShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

			// Switches worldgen.ush's resource declarations to Unreal's
			// conventions. Nothing else about the shader changes — see the
			// long comment at the top of worldgen.ush.
			OutEnvironment.SetDefine(TEXT("VXC_UE"), 1);

			// Version lock. worldgen.ush #errors if this disagrees with its own
			// VXC_WORLDGEN_VERSION_USH, so a CPU worldgen change that never got
			// mirrored into HLSL fails the shader compile instead of surfacing
			// as per-cell material mismatches in voxel.GPU.VerifyRegion — which
			// is how this one produced two published wrong root causes.
			//
			// It is also load-bearing as a cache key: defines feed the shader
			// map hash, so bumping vxc::kWorldGenVersion guarantees a recompile
			// rather than a silent reuse of the previous version's bytecode.
			OutEnvironment.SetDefine(TEXT("VXC_WORLDGEN_VERSION_CPP"),
			                         uint32(vxc::kWorldGenVersion));
		}
	};

	// --- ColumnMain: one thread per column, full stratigraphy ---------------
	class FVoxelColumnCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelColumnCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelColumnCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ElevationMm)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ClimatePacked)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GpuColumnSample>, OutColumns)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- VoxelizeMain: reads those columns back, fills the cell grid --------
	class FVoxelVoxelizeCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelVoxelizeCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelVoxelizeCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			// Bound again because the cavern pass evaluates terrain height at
			// a cave site's own xy, which is usually not a dispatch column.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ElevationMm)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuColumnSample>, InColumns)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- MeshCountMain: greedy meshing, counting only ----------------------
	class FVoxelMeshCountCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMeshCountCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMeshCountCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			// Read-only here, but worldgen.ush declares OutCells as an
			// RWStructuredBuffer (one declaration serves every kernel), so it
			// has to be bound as a UAV.
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadCounts)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- The three scan kernels: exclusive prefix sum over the counts ------
	class FVoxelScanBlocksCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelScanBlocksCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelScanBlocksCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBlockSums)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelScanSumsCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelScanSumsCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelScanSumsCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBlockSums)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelScanAddCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelScanAddCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelScanAddCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBlockSums)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- MeshEmitMain: re-runs greedy meshing, writes at scanned offsets ----
	class FVoxelMeshEmitCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelMeshEmitCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelMeshEmitCS, FVoxelWorldGenShader);

		// Wave D / D2. The ONLY kernel with a permutation, and the reason it is
		// a permutation rather than an edit is in worldgen.ush next to the
		// define: the determinism digest hashes packed quad fields, and this
		// changes three of them. Both permutations are compiled, so one binary
		// can run the pinned gate and the streaming path.
		class FChunkLocalDim : SHADER_PERMUTATION_BOOL("VXC_MESH_CHUNK_LOCAL");
		using FPermutationDomain = TShaderPermutationDomain<FChunkLocalDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			// Read only by the chunk-local permutation. Set to 0 otherwise —
			// on the off permutation the name does not exist in the compiled
			// shader, and binding an unbound parameter is a silent no-op.
			SHADER_PARAMETER(uint32, QuadWriteBase)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
			// Same buffer the scan wrote, bound read-only. It is an SRV here
			// and a UAV in the scan passes; that is safe only because they are
			// different passes — RDG would flag both bindings in one pass.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InQuadOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutQuads)
		END_SHADER_PARAMETER_STRUCT()
	};
	// --- QuadTotalMain: the 4-byte scan total (Wave D / D3) ----------------
	//
	// NOT a worldgen kernel and deliberately not in worldgen.ush — see the
	// header comment in VoxelQuadScan.usf. It derives from FGlobalShader
	// directly rather than FVoxelWorldGenShader for the same reason: it has no
	// business inside the worldgen version lock, and it does not need SM6
	// (there is no 64-bit integer maths in it). Gating it on SM6 anyway would
	// be harmless but dishonest about what it requires.
	class FVoxelQuadTotalCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelQuadTotalCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelQuadTotalCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, MaskCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InQuadCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InQuadOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutQuadTotal)
		END_SHADER_PARAMETER_STRUCT()
	};
	// --- BandReduceMain: the footprint band (Wave D / D6) ------------------
	//
	// Derives from FVoxelWorldGenShader, unlike the quad-total kernel, because
	// it #includes worldgen.ush and therefore needs both VXC_UE and the
	// worldgen version lock's VXC_WORLDGEN_VERSION_CPP. It also genuinely needs
	// SM6: caveColumnFor and cavernColumnFor are 64-bit integer throughout.
	class FVoxelBandReduceCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBandReduceCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBandReduceCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			SHADER_PARAMETER(uint32, BandOriginI)
			SHADER_PARAMETER(uint32, BandOriginJ)
			SHADER_PARAMETER(uint32, BandEdge)
			// cavernColumnFor evaluates terrain height at a cave site's own xy,
			// so the raster is live here for the same reason it is in
			// VoxelizeMain.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ElevationMm)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuColumnSample>, InColumns)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, OutBand)
		END_SHADER_PARAMETER_STRUCT()
	};
}

#define VOXEL_BAND_REDUCE_USF "/VoxelEarth/VoxelBandReduce.usf"
IMPLEMENT_GLOBAL_SHADER(FVoxelBandReduceCS, VOXEL_BAND_REDUCE_USF, "BandReduceMain", SF_Compute);

#define VOXEL_QUAD_SCAN_USF "/VoxelEarth/VoxelQuadScan.usf"

IMPLEMENT_GLOBAL_SHADER(FVoxelQuadTotalCS,  VOXEL_QUAD_SCAN_USF, "QuadTotalMain",  SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FVoxelColumnCS,     VOXEL_WORLDGEN_USF, "ColumnMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelVoxelizeCS,   VOXEL_WORLDGEN_USF, "VoxelizeMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMeshCountCS,  VOXEL_WORLDGEN_USF, "MeshCountMain",  SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanBlocksCS, VOXEL_WORLDGEN_USF, "ScanBlocksMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanSumsCS,   VOXEL_WORLDGEN_USF, "ScanSumsMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanAddCS,    VOXEL_WORLDGEN_USF, "ScanAddMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMeshEmitCS,   VOXEL_WORLDGEN_USF, "MeshEmitMain",   SF_Compute);

namespace
{
	// Fills the loose parameters that every kernel shares. ScanCount varies by
	// pass, so it is set by the caller afterwards.
	template <typename TParams>
	void FillLooseParameters(TParams& Out, const FVoxelGpuRegionRequest& Req)
	{
		Out.DispatchColumns = Req.DispatchColumns;
		Out.RasterOriginPx  = Req.RasterOriginPx;
		Out.RasterSize      = Req.RasterSize;
		Out.PixelSizeMm     = Req.PixelSizeMm;
		Out.SeedLo          = static_cast<uint32>(Req.Seed & 0xffffffffull);
		Out.SeedHi          = static_cast<uint32>(Req.Seed >> 32);
		Out.OriginVx        = Req.OriginVx;
		Out.OriginVy        = Req.OriginVy;
		Out.BrickZMin       = Req.BrickZMin;
		Out.BricksZ         = Req.BricksZ;
		Out.ScanCount       = 0;
		// D5. The SCALE, not the level: worldgen.ush must contain no variable
		// shift distance (lint-shader-ub VARIABLE_SHIFT), so 1 << level is
		// computed here where the level is already range-checked. Scale 1 is
		// the identity in coarseRep(), so a request that never sets CoarseLevel
		// is byte-for-byte the pre-D5 dispatch.
		Out.CoarseScale     = 1u << static_cast<uint32>(FMath::Clamp(Req.CoarseLevel, 0, 5));
		Out.RingSkirtMask   = Req.RingSkirtMask & 0xfu;
	}

}

uint32 VoxelGpuWorldGen::FRegionGraphSizes::ColumnsBytes() const
{
	return NumColumns * uint32(sizeof(FVoxelGpuColumnSample));
}

// Rejects anything the kernels' own guards would otherwise have to absorb.
// Every one of these is a caller bug, and catching it here produces a
// readable message instead of a wrong-but-plausible quad stream.
bool VoxelGpuWorldGen::ValidateRegionRequest(const FVoxelGpuRegionRequest& Req, FString& OutError)
{
	const uint32 Cx = Req.DispatchColumns.X;
	const uint32 Cy = Req.DispatchColumns.Y;

	if (Cx == 0 || Cy == 0 || (Cx % kBrickEdge) != 0 || (Cy % kBrickEdge) != 0)
	{
		OutError = FString::Printf(TEXT("DispatchColumns (%u, %u) must be non-zero multiples of %u"),
		                           Cx, Cy, kBrickEdge);
		return false;
	}
	if (Req.BricksZ == 0)
	{
		OutError = TEXT("BricksZ must be non-zero");
		return false;
	}
	if (Req.RasterSize.X == 0 || Req.RasterSize.Y == 0)
	{
		OutError = TEXT("RasterSize must be non-zero on both axes");
		return false;
	}

	const uint64 Expected = uint64(Req.RasterSize.X) * uint64(Req.RasterSize.Y);
	if (uint64(Req.ElevationMm.Num()) != Expected || uint64(Req.ClimatePacked.Num()) != Expected)
	{
		OutError = FString::Printf(
			TEXT("Raster arrays do not match RasterSize %ux%u (=%llu): elevation=%d climate=%d"),
			Req.RasterSize.X, Req.RasterSize.Y, Expected,
			Req.ElevationMm.Num(), Req.ClimatePacked.Num());
		return false;
	}

	// --- Wave D / D6: the band window ---------------------------------------
	//
	// BandReduceMain's inner loop skips any cell that falls outside
	// DispatchColumns rather than reading out of bounds — which would silently
	// reduce over a PARTIAL window and hand back a band that is not an outer
	// bound of the columns the caller asked about. A band that is wrong in that
	// direction skips chunks that should have been meshed, i.e. holes in the
	// world. So the mis-sizing is rejected here instead of being absorbed
	// there, and the kernel's guard stays as unreachable defence.
	// D5.3. A NON-ZERO SKIRT MASK IS ONLY MEANINGFUL FOR A SINGLE-CHUNK REGION,
	// and this refuses rather than trusting the caller.
	//
	// regionCellMat applies the mask against the fixed chunk interior [8, 40) on
	// both lateral axes, which is where a 6-brick chunk dispatch puts its 4
	// interior bricks. Batch two chunks into one region and that is no longer
	// the interior of anything: a shared interior cell is one chunk's interior
	// AND its neighbour's apron, so a single mask would rewrite real geometry to
	// AIR instead of merely failing to add a retaining wall. Holes in the world,
	// from a feature whose whole purpose is to close them.
	//
	// The correct batched form is N masks in a buffer, keyed on the reading
	// chunk. Until that exists this rejection is what keeps the scalar SOUND
	// rather than sound-for-now -- the difference between a constraint and an
	// assumption is whether anything checks it.
	if (Req.RingSkirtMask != 0)
	{
		constexpr uint32 kChunkDispatchColumns = 48;   // 32 interior + 2 * 8 halo
		if (Cx != kChunkDispatchColumns || Cy != kChunkDispatchColumns)
		{
			OutError = FString::Printf(
				TEXT("RingSkirtMask %u on a %ux%u-column region — the skirt is applied against a ")
				TEXT("single chunk's interior [8, 40) and is only meaningful for a %ux%u chunk ")
				TEXT("dispatch. A batched region needs per-chunk masks in a buffer; a scalar here ")
				TEXT("would rewrite a neighbour's interior geometry to AIR."),
				Req.RingSkirtMask, Cx, Cy, kChunkDispatchColumns, kChunkDispatchColumns);
			return false;
		}
		if ((Req.RingSkirtMask & ~0xfu) != 0)
		{
			OutError = FString::Printf(
				TEXT("RingSkirtMask %u has bits outside the four lateral faces (1=-X 2=+X 4=-Y ")
				TEXT("8=+Y)"), Req.RingSkirtMask);
			return false;
		}
	}

	if (Req.BandEdge > 0)
	{
		// PER AXIS, not one origin against both. With a single origin the old
		// form was equivalent; with BandOriginJ it is not, and the failure it
		// would let through is the exact one this check exists to stop -- a
		// window that overhangs in Y only still reduces over a partial grid and
		// returns a band that is not an outer bound, in the unsafe direction.
		const uint64 EndI = uint64(Req.BandOriginI) + uint64(Req.BandEdge);
		const uint64 EndJ = uint64(Req.BandOriginJ) + uint64(Req.BandEdge);
		if (EndI > uint64(Cx) || EndJ > uint64(Cy))
		{
			OutError = FString::Printf(
				TEXT("Band window x[%u, %llu) y[%u, %llu) does not fit inside DispatchColumns ")
				TEXT("(%u, %u) — the reduction would silently run over a partial window and ")
				TEXT("return a band that is not an outer bound of the columns asked about"),
				Req.BandOriginI, EndI, Req.BandOriginJ, EndJ, Cx, Cy);
			return false;
		}
	}

	if (Req.bMeshChain)
	{
		const uint32 BricksX = Cx / kBrickEdge;
		const uint32 BricksY = Cy / kBrickEdge;
		if (BricksX < 3 || BricksY < 3 || Req.BricksZ < 3)
		{
			OutError = FString::Printf(
				TEXT("Mesh chain needs >= 3 bricks per axis (have %u, %u, %u) — a thinner region ")
				TEXT("has no interior brick once the 1-brick halo is removed"),
				BricksX, BricksY, Req.BricksZ);
			return false;
		}

		const uint64 MaskCount = uint64(BricksX - 2) * uint64(BricksY - 2)
		                       * uint64(Req.BricksZ - 2) * uint64(kMasksPerBrick);
		if (MaskCount > kMaxMasksPerDispatch)
		{
			OutError = FString::Printf(
				TEXT("Mask count %llu exceeds the %u the single-workgroup ScanSumsMain can scan — ")
				TEXT("split the region into z-slabs like the bench does"),
				MaskCount, kMaxMasksPerDispatch);
			return false;
		}

		// A parameter that silently does nothing is the failure mode this
		// project has already been bitten by twice (the unbound
		// FShaderParameter, the dead GPUCullMergeGap cvar). QuadWriteBase is
		// only read by the chunk-local permutation, so asking for one without
		// the other is a caller bug and says so.
		if (Req.QuadWriteBase != 0 && !Req.bChunkLocalQuads)
		{
			OutError = FString::Printf(
				TEXT("QuadWriteBase %u requires bChunkLocalQuads — the default emit permutation ")
				TEXT("does not read it, so the base would be silently ignored and every quad ")
				TEXT("would land at offset 0"),
				Req.QuadWriteBase);
			return false;
		}
		const uint64 MaxQuads = MaskCount * uint64(kMaxQuadsPerMask);
		if (uint64(Req.QuadWriteBase) + MaxQuads > uint64(TNumericLimits<uint32>::Max()))
		{
			OutError = FString::Printf(
				TEXT("QuadWriteBase %u + %llu quads overflows a uint32 quad index"),
				Req.QuadWriteBase, MaxQuads);
			return false;
		}
	}
	return true;
}

VoxelGpuWorldGen::FRegionGraphSizes
VoxelGpuWorldGen::ComputeRegionGraphSizes(const FVoxelGpuRegionRequest& Request)
{
	FRegionGraphSizes S;
	S.BricksX = Request.DispatchColumns.X / kBrickEdge;
	S.BricksY = Request.DispatchColumns.Y / kBrickEdge;
	S.BricksZ = Request.BricksZ;

	S.NumColumns = Request.DispatchColumns.X * Request.DispatchColumns.Y;
	S.NumCells = S.BricksX * S.BricksY * S.BricksZ * kCellsPerBrick;

	S.bMesh = Request.bMeshChain;
	if (S.bMesh)
	{
		S.MaskCount = (S.BricksX - 2) * (S.BricksY - 2) * (S.BricksZ - 2) * kMasksPerBrick;
		S.NumBlocks = FMath::DivideAndRoundUp(S.MaskCount, kScanBlockSize);
		S.MaxQuads = S.MaskCount * kMaxQuadsPerMask;
		S.QuadWriteBase = Request.bChunkLocalQuads ? Request.QuadWriteBase : 0;
		S.QuadBufferElements = S.QuadWriteBase + S.MaxQuads;
	}
	return S;
}

// The seven passes, and nothing else. No execute, no readback, no blocking --
// see VoxelGpuWorldGenGraph.h for why those are the caller's decisions.
VoxelGpuWorldGen::FRegionGraphResources
VoxelGpuWorldGen::AddRegionPasses(FRDGBuilder& GraphBuilder, const FVoxelGpuRegionRequest& Request)
{
	check(IsInRenderingThread());

	FRegionGraphResources Out;
	Out.Sizes = ComputeRegionGraphSizes(Request);
	const FRegionGraphSizes& S = Out.Sizes;

	const uint32 Cx = Request.DispatchColumns.X;
	const uint32 Cy = Request.DispatchColumns.Y;

	// --- inputs -----------------------------------------------------
	FRDGBufferRef ElevationBuffer = CreateStructuredBuffer(
		GraphBuilder, TEXT("Voxel.ElevationMm"), sizeof(int32),
		Request.ElevationMm.Num(), Request.ElevationMm.GetData(),
		Request.ElevationMm.Num() * sizeof(int32));

	FRDGBufferRef ClimateBuffer = CreateStructuredBuffer(
		GraphBuilder, TEXT("Voxel.ClimatePacked"), sizeof(uint32),
		Request.ClimatePacked.Num(), Request.ClimatePacked.GetData(),
		Request.ClimatePacked.Num() * sizeof(uint32));

	// --- outputs ----------------------------------------------------
	Out.Columns = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelGpuColumnSample), S.NumColumns),
		TEXT("Voxel.Columns"));

	Out.Cells = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumCells),
		TEXT("Voxel.Cells"));

	// --- pass 1: ColumnMain ----------------------------------------
	{
		FVoxelColumnCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelColumnCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
		Params->ClimatePacked = GraphBuilder.CreateSRV(ClimateBuffer);
		Params->OutColumns = GraphBuilder.CreateUAV(Out.Columns);

		TShaderMapRef<FVoxelColumnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.ColumnMain"), Shader, Params,
			FIntVector(Cx / kBrickEdge, Cy / kBrickEdge, 1));
	}

	// --- pass 2: VoxelizeMain --------------------------------------
	{
		FVoxelVoxelizeCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelVoxelizeCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
		Params->InColumns = GraphBuilder.CreateSRV(Out.Columns);
		Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);

		TShaderMapRef<FVoxelVoxelizeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.VoxelizeMain"), Shader, Params,
			FIntVector(Cx / kBrickEdge, Cy / kBrickEdge, 1));
	}

	// --- pass 2b: BandReduceMain (Wave D / D6) -----------------------
	//
	// After ColumnMain, before anything else needs it, and independent of the
	// mesh chain: the band is a pure function of the columns and does not know
	// about chunk-z. Requested per job because only one chunk per (X,Y)
	// footprint needs to produce one.
	if (Request.BandEdge > 0)
	{
		Out.Band = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 2), TEXT("Voxel.Band"));

		FVoxelBandReduceCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelBandReduceCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->BandOriginI = Request.BandOriginI;
		Params->BandOriginJ = Request.BandOriginJ;
		Params->BandEdge = Request.BandEdge;
		Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
		Params->InColumns = GraphBuilder.CreateSRV(Out.Columns);
		Params->OutBand = GraphBuilder.CreateUAV(Out.Band);

		TShaderMapRef<FVoxelBandReduceCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BandReduceMain"), Shader, Params,
			FIntVector(1, 1, 1));   // exactly one workgroup, by design
	}

	if (!S.bMesh)
	{
		return Out;
	}

	Out.Counts = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.MaskCount), TEXT("Voxel.QuadCounts"));
	Out.Offsets = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.MaskCount), TEXT("Voxel.QuadOffsets"));
	FRDGBufferRef BlockSumsBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBlocks), TEXT("Voxel.BlockSums"));
	Out.Quads = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2, S.QuadBufferElements),
		TEXT("Voxel.Quads"));

	// --- pass 3: MeshCountMain ---------------------------------
	{
		FVoxelMeshCountCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelMeshCountCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ScanCount = S.MaskCount;
		Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);
		Params->OutQuadCounts = GraphBuilder.CreateUAV(Out.Counts);

		TShaderMapRef<FVoxelMeshCountCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.MeshCountMain"), Shader, Params,
			FIntVector(FMath::DivideAndRoundUp(S.MaskCount, 64u), 1, 1));
	}

	// --- pass 4: ScanBlocksMain --------------------------------
	{
		FVoxelScanBlocksCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelScanBlocksCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ScanCount = S.MaskCount;
		Params->OutQuadCounts = GraphBuilder.CreateUAV(Out.Counts);
		Params->OutQuadOffsets = GraphBuilder.CreateUAV(Out.Offsets);
		Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);

		TShaderMapRef<FVoxelScanBlocksCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.ScanBlocksMain"), Shader, Params,
			FIntVector(S.NumBlocks, 1, 1));
	}

	// --- pass 5: ScanSumsMain (exactly one group, by design) ----
	{
		FVoxelScanSumsCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelScanSumsCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ScanCount = S.MaskCount;
		Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);

		TShaderMapRef<FVoxelScanSumsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.ScanSumsMain"), Shader, Params,
			FIntVector(1, 1, 1));
	}

	// --- pass 6: ScanAddMain -----------------------------------
	{
		FVoxelScanAddCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelScanAddCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ScanCount = S.MaskCount;
		Params->OutQuadOffsets = GraphBuilder.CreateUAV(Out.Offsets);
		Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSumsBuffer);

		TShaderMapRef<FVoxelScanAddCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.ScanAddMain"), Shader, Params,
			FIntVector(S.NumBlocks, 1, 1));
	}

	// --- pass 7: MeshEmitMain ----------------------------------
	{
		FVoxelMeshEmitCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelMeshEmitCS::FParameters>();
		FillLooseParameters(*Params, Request);
		Params->ScanCount = S.MaskCount;
		Params->QuadWriteBase = S.QuadWriteBase;
		Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);
		Params->InQuadOffsets = GraphBuilder.CreateSRV(Out.Offsets);
		Params->OutQuads = GraphBuilder.CreateUAV(Out.Quads);

		FVoxelMeshEmitCS::FPermutationDomain Permutation;
		Permutation.Set<FVoxelMeshEmitCS::FChunkLocalDim>(Request.bChunkLocalQuads);

		TShaderMapRef<FVoxelMeshEmitCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.MeshEmitMain(%s)",
			                             Request.bChunkLocalQuads ? TEXT("chunk-local") : TEXT("brick-local")),
			Shader, Params,
			FIntVector(FMath::DivideAndRoundUp(S.MaskCount, 64u), 1, 1));
	}

	// --- pass 8: QuadTotalMain (Wave D / D3) -------------------------
	//
	// Unconditional, because it costs one thread and because being always
	// present is what lets RunRegionBlocking cross-check it against the CPU
	// derivation on every voxel.GPU.VerifyRegion run. A kernel that only the
	// streaming path exercises is a kernel whose first bug shows up in the
	// streaming path.
	{
		Out.Total = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("Voxel.QuadTotal"));

		FVoxelQuadTotalCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelQuadTotalCS::FParameters>();
		Params->MaskCount = S.MaskCount;
		Params->InQuadCounts = GraphBuilder.CreateSRV(Out.Counts);
		Params->InQuadOffsets = GraphBuilder.CreateSRV(Out.Offsets);
		Params->OutQuadTotal = GraphBuilder.CreateUAV(Out.Total);

		TShaderMapRef<FVoxelQuadTotalCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.QuadTotalMain"), Shader, Params,
			FIntVector(1, 1, 1));
	}

	return Out;
}

bool VoxelGpuWorldGen::IsSupportedOnCurrentRHI()
{
	return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM6;
}

FVoxelGpuRegionResult VoxelGpuWorldGen::RunRegionBlocking(const FVoxelGpuRegionRequest& Request)
{
	FVoxelGpuRegionResult Result;

	if (!ValidateRegionRequest(Request, Result.Error))
	{
		return Result;
	}
	if (!IsSupportedOnCurrentRHI())
	{
		Result.Error = TEXT("Requires SM6 (64-bit integer shader ops). Add ")
		               TEXT("+D3D12TargetedShaderFormats=PCD3D_SM6 and relaunch with -sm6.");
		return Result;
	}

	const FRegionGraphSizes Sizes = ComputeRegionGraphSizes(Request);
	const bool bMesh = Sizes.bMesh;
	const uint32 NumColumns = Sizes.NumColumns;
	const uint32 NumCells = Sizes.NumCells;
	const uint32 MaskCount = Sizes.MaskCount;
	const uint32 MaxQuads = Sizes.MaxQuads;
	// The quad buffer is QuadWriteBase slots of other people's geometry
	// followed by this dispatch's MaxQuads. Equal to MaxQuads on every path
	// that does not set a base, which is every caller of this blocking one.
	const uint32 QuadElements = Sizes.QuadBufferElements;
	const uint32 QuadBase = Sizes.QuadWriteBase;

	// Filled on the render thread, read back on this one after the flush.
	TArray<FVoxelGpuColumnSample> ColumnsOut;
	TArray<uint32> CellsOut;
	TArray<uint32> CountsOut;
	TArray<uint32> OffsetsOut;
	TArray<uint64> QuadsOut;
	// Wave D / D3: what QuadTotalMain computed, so it can be checked against
	// the CPU derivation below. 0xffffffff means "the readback never ran".
	uint32 GpuTotalOut = 0xffffffffu;
	// Wave D / D6: BandReduceMain's two raw voxel-z extrema, only when asked
	// for. bBandOut stays false unless the copy actually landed, so a missing
	// readback reads as "no band" rather than as a band of zeroes -- and a band
	// of zeroes claims the whole world is empty.
	int32 BandOut[2] = { 0, 0 };
	bool bBandOut = false;
	const bool bWantBand = Request.BandEdge > 0;
	FString RenderError;

	ENQUEUE_RENDER_COMMAND(VoxelGpuRunRegion)(
		[&Request, &ColumnsOut, &CellsOut, &CountsOut, &OffsetsOut, &QuadsOut, &GpuTotalOut,
		 &BandOut, &bBandOut, &RenderError, NumColumns, NumCells, bMesh, MaskCount, QuadElements,
		 bWantBand]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		// The seven passes, shared verbatim with FVoxelGpuMeshJobManager.
		const FRegionGraphResources Graph = AddRegionPasses(GraphBuilder, Request);
		FRDGBufferRef ColumnsBuffer = Graph.Columns;
		FRDGBufferRef CellsBuffer = Graph.Cells;
		FRDGBufferRef CountsBuffer = Graph.Counts;
		FRDGBufferRef OffsetsBuffer = Graph.Offsets;
		FRDGBufferRef QuadsBuffer = Graph.Quads;

		// --- readback ---------------------------------------------------
		// A verification path, so everything comes back. The streaming path
		// (G3) will read none of this — the geometry stays on the GPU.
		FRHIGPUBufferReadback ColumnsReadback(TEXT("Voxel.ColumnsReadback"));
		FRHIGPUBufferReadback CellsReadback(TEXT("Voxel.CellsReadback"));
		FRHIGPUBufferReadback CountsReadback(TEXT("Voxel.CountsReadback"));
		FRHIGPUBufferReadback OffsetsReadback(TEXT("Voxel.OffsetsReadback"));
		FRHIGPUBufferReadback QuadsReadback(TEXT("Voxel.QuadsReadback"));
		FRHIGPUBufferReadback TotalReadback(TEXT("Voxel.QuadTotalReadback"));
		FRHIGPUBufferReadback BandReadback(TEXT("Voxel.BandReadback"));

		const uint32 ColumnsBytes = NumColumns * sizeof(FVoxelGpuColumnSample);
		const uint32 CellsBytes = NumCells * sizeof(uint32);
		const uint32 CountsBytes = MaskCount * sizeof(uint32);
		const uint32 QuadsBytes = QuadElements * sizeof(uint64);

		AddEnqueueCopyPass(GraphBuilder, &ColumnsReadback, ColumnsBuffer, ColumnsBytes);
		AddEnqueueCopyPass(GraphBuilder, &CellsReadback, CellsBuffer, CellsBytes);
		// The band is independent of the mesh chain -- it is a pure function of
		// the columns -- so it is copied outside the bMesh block, which is what
		// lets the gate run band-only (bMeshChain false) probes cheaply.
		if (bWantBand && Graph.Band != nullptr)
		{
			AddEnqueueCopyPass(GraphBuilder, &BandReadback, Graph.Band, 2 * sizeof(int32));
		}
		if (bMesh)
		{
			AddEnqueueCopyPass(GraphBuilder, &CountsReadback, CountsBuffer, CountsBytes);
			AddEnqueueCopyPass(GraphBuilder, &OffsetsReadback, OffsetsBuffer, CountsBytes);
			AddEnqueueCopyPass(GraphBuilder, &QuadsReadback, QuadsBuffer, QuadsBytes);
			AddEnqueueCopyPass(GraphBuilder, &TotalReadback, Graph.Total, sizeof(uint32));
		}

		GraphBuilder.Execute();

		// Blocking is normally the wrong thing to do on the render thread. It
		// is correct here and only here: this is a console-driven verification
		// that has to hand a finished answer back to the caller, and the
		// readback objects above live on this stack frame.
		RHICmdList.SubmitAndBlockUntilGPUIdle();

		const auto CopyOut = [&RenderError](FRHIGPUBufferReadback& Readback, void* Dest, uint32 Bytes,
		                                    const TCHAR* Name)
		{
			if (Bytes == 0)
			{
				return;
			}
			if (!Readback.IsReady())
			{
				RenderError = FString::Printf(TEXT("%s readback not ready after GPU idle"), Name);
				return;
			}
			const void* Src = Readback.Lock(Bytes);
			if (Src == nullptr)
			{
				RenderError = FString::Printf(TEXT("%s readback lock returned null"), Name);
				return;
			}
			FMemory::Memcpy(Dest, Src, Bytes);
			Readback.Unlock();
		};

		ColumnsOut.SetNumUninitialized(NumColumns);
		CopyOut(ColumnsReadback, ColumnsOut.GetData(), ColumnsBytes, TEXT("Columns"));

		CellsOut.SetNumUninitialized(NumCells);
		CopyOut(CellsReadback, CellsOut.GetData(), CellsBytes, TEXT("Cells"));

		if (bWantBand && Graph.Band != nullptr)
		{
			CopyOut(BandReadback, BandOut, 2 * sizeof(int32), TEXT("Band"));
			bBandOut = RenderError.IsEmpty();
		}

		if (bMesh)
		{
			CountsOut.SetNumUninitialized(MaskCount);
			CopyOut(CountsReadback, CountsOut.GetData(), CountsBytes, TEXT("Counts"));

			OffsetsOut.SetNumUninitialized(MaskCount);
			CopyOut(OffsetsReadback, OffsetsOut.GetData(), CountsBytes, TEXT("Offsets"));

			QuadsOut.SetNumUninitialized(QuadElements);
			CopyOut(QuadsReadback, QuadsOut.GetData(), QuadsBytes, TEXT("Quads"));

			CopyOut(TotalReadback, &GpuTotalOut, sizeof(uint32), TEXT("QuadTotal"));
		}
	});

	FlushRenderingCommands();

	if (!RenderError.IsEmpty())
	{
		Result.Error = RenderError;
		return Result;
	}

	Result.Columns = MoveTemp(ColumnsOut);
	Result.Cells = MoveTemp(CellsOut);
	Result.bBandValid = bBandOut;
	Result.BandMaxSurfaceTopVoxel = BandOut[0];
	Result.BandMinDeepestAirVoxel = BandOut[1];

	if (bMesh)
	{
		// The scan is exclusive, so the live quad count is the last mask's
		// offset plus its own count — the same derivation the bench uses.
		Result.NumQuads = (MaskCount > 0)
			? OffsetsOut[MaskCount - 1] + CountsOut[MaskCount - 1]
			: 0;

		// Wave D / D3. QuadTotalMain computes the SAME number on the GPU, and
		// the streaming path is about to trust it as the only thing it reads
		// back. Checking it here means the kernel is gated by every
		// voxel.GPU.VerifyRegion run rather than first being exercised, and
		// first being wrong, inside DispatchJobs.
		//
		// THE RESULT IS LOGGED EITHER WAY, deliberately. A guard that has never
		// been observed firing is a guard nobody has tested, and a guard that
		// prints nothing on success is indistinguishable from one that was
		// never reached.
		if (GpuTotalOut != Result.NumQuads)
		{
			Result.Error = FString::Printf(
				TEXT("QuadTotalMain disagrees with the CPU derivation: gpu %u, cpu %u ")
				TEXT("(Offsets[%u] + Counts[%u]). The 4-byte total is what D3's streaming ")
				TEXT("path allocates from, so this must be exact."),
				GpuTotalOut, Result.NumQuads, MaskCount - 1, MaskCount - 1);
			UE_LOG(LogVoxelGpu, Error, TEXT("[D3 quad-total cross-check] FAIL — %s"), *Result.Error);
			return Result;
		}
		UE_LOG(LogVoxelGpu, Log,
		       TEXT("[D3 quad-total cross-check] PASS — QuadTotalMain %u == CPU-derived %u ")
		       TEXT("(Offsets[%u] + Counts[%u], %u masks)"),
		       GpuTotalOut, Result.NumQuads, MaskCount - 1, MaskCount - 1, MaskCount);

		if (Result.NumQuads > MaxQuads)
		{
			Result.Error = FString::Printf(
				TEXT("Scan reports %u quads but the buffer holds at most %u — the ")
				TEXT("%u-quads-per-mask bound is wrong or the scan is corrupt"),
				Result.NumQuads, MaxQuads, kMaxQuadsPerMask);
			return Result;
		}

		// Trim to [QuadBase, QuadBase + NumQuads): the live quads this dispatch
		// wrote, with anyone else's slots dropped. QuadBase is 0 on every
		// current caller, so this is a no-op removal today.
		if (QuadBase > 0)
		{
			QuadsOut.RemoveAt(0, int32(QuadBase), EAllowShrinking::No);
		}
		QuadsOut.SetNum(Result.NumQuads, EAllowShrinking::No);
		Result.Quads = MoveTemp(QuadsOut);

		// Handed back so callers can map a quad to its brick and re-base the
		// brick-local coordinates -- see the comment on FVoxelGpuRegionResult::Quads.
		Result.QuadCounts = MoveTemp(CountsOut);
		Result.QuadOffsets = MoveTemp(OffsetsOut);
	}

	Result.bOk = true;
	return Result;
}

// ---------------------------------------------------------------------------
// Packed-quad decode test hook (G2)
//
// The vertex factory will call DecodeVoxelQuadVertex during rendering, where a
// mistake in it shows up only as terrain that looks a bit wrong. This runs the
// same function over a quad buffer and hands the results back so they can be
// compared against a CPU reference numerically instead.
// ---------------------------------------------------------------------------

#define VOXEL_QUAD_DECODE_TEST_USF "/VoxelEarth/VoxelQuadDecodeTest.usf"

namespace
{
	class FVoxelQuadDecodeTestCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelQuadDecodeTestCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelQuadDecodeTestCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumQuads)
			SHADER_PARAMETER(float, LevelScale)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InQuads)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, OutVertices)
		END_SHADER_PARAMETER_STRUCT()
	};
}

IMPLEMENT_GLOBAL_SHADER(FVoxelQuadDecodeTestCS, VOXEL_QUAD_DECODE_TEST_USF,
                        "QuadDecodeTestMain", SF_Compute);

bool VoxelGpuWorldGen::DecodeQuadsBlocking(const TArray<uint64>& Quads,
                                           float LevelScale,
                                           TArray<FDecodedVertex>& OutVertices,
                                           FString& OutError)
{
	OutVertices.Reset();

	if (!IsSupportedOnCurrentRHI())
	{
		OutError = TEXT("Requires SM6");
		return false;
	}
	if (Quads.IsEmpty())
	{
		return true;
	}

	const uint32 NumQuads = uint32(Quads.Num());
	const uint32 NumVertices = NumQuads * 6;

	TArray<FVector4f> Raw;
	FString RenderError;

	ENQUEUE_RENDER_COMMAND(VoxelGpuDecodeQuads)(
		[&Quads, &Raw, &RenderError, NumQuads, NumVertices, LevelScale]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGBufferRef QuadsBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.DecodeIn"), sizeof(uint64), NumQuads,
			Quads.GetData(), NumQuads * sizeof(uint64));

		FRDGBufferRef VertsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumVertices),
			TEXT("Voxel.DecodeOut"));

		FVoxelQuadDecodeTestCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelQuadDecodeTestCS::FParameters>();
		Params->NumQuads = NumQuads;
		Params->LevelScale = LevelScale;
		Params->InQuads = GraphBuilder.CreateSRV(QuadsBuffer);
		Params->OutVertices = GraphBuilder.CreateUAV(VertsBuffer);

		TShaderMapRef<FVoxelQuadDecodeTestCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.QuadDecodeTest"), Shader, Params,
			FIntVector(FMath::DivideAndRoundUp(NumVertices, 64u), 1, 1));

		FRHIGPUBufferReadback Readback(TEXT("Voxel.DecodeReadback"));
		const uint32 Bytes = NumVertices * sizeof(FVector4f);
		AddEnqueueCopyPass(GraphBuilder, &Readback, VertsBuffer, Bytes);

		GraphBuilder.Execute();
		RHICmdList.SubmitAndBlockUntilGPUIdle();

		if (!Readback.IsReady())
		{
			RenderError = TEXT("decode readback not ready after GPU idle");
			return;
		}
		const void* Src = Readback.Lock(Bytes);
		if (Src == nullptr)
		{
			RenderError = TEXT("decode readback lock returned null");
			return;
		}
		Raw.SetNumUninitialized(NumVertices);
		FMemory::Memcpy(Raw.GetData(), Src, Bytes);
		Readback.Unlock();
	});

	FlushRenderingCommands();

	if (!RenderError.IsEmpty())
	{
		OutError = RenderError;
		return false;
	}

	OutVertices.SetNumUninitialized(NumVertices);
	for (uint32 I = 0; I < NumVertices; ++I)
	{
		const FVector4f& R = Raw[int32(I)];
		FDecodedVertex& V = OutVertices[int32(I)];
		V.PositionUU[0] = R.X;
		V.PositionUU[1] = R.Y;
		V.PositionUU[2] = R.Z;

		// The shader packed ao | mat<<8 through asfloat, so reinterpret rather
		// than convert -- a float cast here would mangle the bits.
		uint32 Packed = 0;
		FMemory::Memcpy(&Packed, &R.W, sizeof(uint32));
		V.AmbientOcclusion = Packed & 0xffu;
		V.MaterialId = (Packed >> 8) & 0xffu;
	}

	return true;
}
