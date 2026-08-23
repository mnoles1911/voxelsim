#include "VoxelGpuWorldGen.h"
#include "VoxelGpuWorldGenGraph.h"
#include "VoxelRasterAtlasGpu.h"   // A: the persistent raster atlas the VXC_RASTER_ATLAS permutation samples
#include "VoxelGpuWorklist.h"      // P3: the record ring the converted Column stage consumes

#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RHIGPUReadback.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RenderingThread.h"
#include "VoxelBrickPool.h"   // kChunkRecordDwords -- the record stride these
                              // two kernels write, bound rather than hardcoded

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

	// --- P1-C: the brick volume's geometry ---------------------------------
	//
	// Mirrors docs/brick-volume-format.md section 1 and brickpack.ush's own
	// constants. Restated here rather than included because this module
	// deliberately does not link voxel-core; the byte contract is the document,
	// and both sides quote it.
	constexpr uint32 kBricksPerChunkEdge = 4;
	constexpr uint32 kBricksPerChunk = kBricksPerChunkEdge * kBricksPerChunkEdge * kBricksPerChunkEdge;
	constexpr uint32 kBrickOccWords = 16;    // 512 bits of occupancy, 64 B
	// The widest a single brick's material allocation can be: the 8 bpp case,
	// 512 solid voxels x 8 bits and NO local palette. (The widest palette case
	// is 4 palette dwords + 64 payload dwords = 68.) Same number as
	// kMaxBrickMatWords in brickpack.ush, which sizes the groupshared staging
	// buffer the payload is assembled in.
	constexpr uint32 kMaxBrickMatWords = 132;

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
		SHADER_PARAMETER(uint32,       RingSkirtMask) \
		SHADER_PARAMETER(uint32,       SurfaceMip)

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

	// A (raster atlas). One permutation bit, ONLY on the three kernels whose
	// entry points can reach rasterElevationMm/rasterClimate (ColumnMain,
	// VoxelizeMain via the cavern mirror, BandReduceMain via cavernColumnFor).
	// The FALSE permutation preprocesses to the shipped text -- worldgen.ush's
	// #else arm is the original accessors character for character -- so the
	// pinned digest gate and the control arm compile the program that shipped.
	class FRasterAtlasDim : SHADER_PERMUTATION_BOOL("VXC_RASTER_ATLAS");

	// The atlas bindings those three kernels share under the TRUE permutation.
	// Loose uints deliberately NOT in VOXEL_WORLDGEN_LOOSE_PARAMETERS: that
	// block's 56-byte layout is static_asserted against the bench's cbuffer,
	// and the bench never compiles the atlas.
	#define VOXEL_RASTER_ATLAS_PARAMETERS() 		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, AtlasElevationMm) 		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AtlasClimatePacked) 		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AtlasPageTags) 		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AtlasMissStats) 		SHADER_PARAMETER(uint32, AtlasPagePx) 		SHADER_PARAMETER(uint32, AtlasPagesDim)

	// --- ColumnMain: one thread per column, full stratigraphy ---------------
	class FVoxelColumnCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelColumnCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelColumnCS, FVoxelWorldGenShader);
		using FPermutationDomain = TShaderPermutationDomain<FRasterAtlasDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			VOXEL_RASTER_ATLAS_PARAMETERS()
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
		using FPermutationDomain = TShaderPermutationDomain<FRasterAtlasDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			VOXEL_RASTER_ATLAS_PARAMETERS()
			// Bound again because the cavern pass evaluates terrain height at
			// a cave site's own xy, which is usually not a dispatch column.
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ElevationMm)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuColumnSample>, InColumns)
			// P3 Column stage: base element added to every InColumns read.
			// 0 on every classic dispatch (byte-identical output, digest
			// untouched); slice * 1024 when InColumns is the worklist column
			// arena. See ColumnReadBase in worldgen.ush.
			SHADER_PARAMETER(uint32, ColumnReadBase)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- ColumnWorklistMain: the CONVERTED Column stage (P3) ----------------
	//
	// One indirect dispatch per tick off the worklist args (16 groups per
	// consumed record), computing every consumed record's 1,024 columns into
	// the flush-level arena -- VoxelWorklistColumn.usf has the whole argument.
	// ATLAS ALWAYS ON: a record carries no raster window, so this class has no
	// permutation domain; it compiles the VXC_RASTER_ATLAS=1 form only.
	// Derives FVoxelWorldGenShader because it compiles worldgen.ush and must
	// carry the version lock like every kernel that does.
	class FVoxelWorklistColumnCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelWorklistColumnCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelWorklistColumnCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			VOXEL_RASTER_ATLAS_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuChunkWorkRecord>, WorklistRecords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, WorklistControl)
			SHADER_PARAMETER(uint32, RingCapacity)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GpuColumnSample>, OutColumns)
			RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FVoxelWorldGenShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("VXC_RASTER_ATLAS"), 1);
			// The torn-dispatch lock: the kernel #errors if its own derivation
			// of the stage shape disagrees with the host table entry these
			// defines are built from (FVoxelGpuWorklist::kColumnGroupsPerRecord
			// is also what kGroupsPerRecord[Column] feeds the args kernel).
			OutEnvironment.SetDefine(TEXT("VXC_WORKLIST_COLUMN_GROUPS"),
			                         FVoxelGpuWorklist::kColumnGroupsPerRecord);
			OutEnvironment.SetDefine(TEXT("VXC_WORKLIST_COLS_PER_RECORD"),
			                         FVoxelGpuWorklist::kColumnsPerRecord);
		}
	};

	// --- ColumnWorklistVerifyMain: converted bytes vs classic bytes ---------
	// (-VoxelGpuWorklistVerifyCols; the plan doc's stage-2 gate.) One 16-group
	// pass per VERIFIED chunk, verify arm only, accumulating into the worklist
	// stats buffer's [4..5] so the result rides the existing proof readback.
	class FVoxelWorklistColumnVerifyCS : public FVoxelWorldGenShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelWorklistColumnVerifyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelWorklistColumnVerifyCS, FVoxelWorldGenShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuColumnSample>, VerifyClassicColumns)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<GpuColumnSample>, VerifyArenaColumns)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, WorklistStats)
			SHADER_PARAMETER(uint32, VerifyArenaBase)
		END_SHADER_PARAMETER_STRUCT()

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			// Same defines as the main kernel: they share the file and its
			// shape #errors.
			FVoxelWorklistColumnCS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		}
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
	// --- AssetStampMain: asset compose into Cells ---------------------------
	//
	// NOT a worldgen kernel and deliberately not in worldgen.ush -- assets are
	// not terrain, the kernel has no bench leg, and the mirror's version lock
	// must not move for it (VoxelAssetStamp.usf's header carries the full
	// argument). Derives from FGlobalShader for the same reason QuadTotal
	// does; the only convention shared with the mirror is cell indexing.
	class FVoxelAssetStampCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelAssetStampCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelAssetStampCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return true;
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FUintVector2, DispatchColumns)
			SHADER_PARAMETER(uint32, BricksZ)
			SHADER_PARAMETER(int32, BrickZMin)
			SHADER_PARAMETER(FIntPoint, AnchorRel)
			SHADER_PARAMETER(int32, AnchorVz)
			SHADER_PARAMETER(int32, GridOriginZ)
			SHADER_PARAMETER(int32, RotOriginX)
			SHADER_PARAMETER(int32, RotOriginY)
			SHADER_PARAMETER(uint32, YawQuarter)
			SHADER_PARAMETER(uint32, SizeX)
			SHADER_PARAMETER(uint32, SizeY)
			SHADER_PARAMETER(uint32, SizeZ)
			SHADER_PARAMETER(uint32, ColStartsBase)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ColStarts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Spans)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- AssetStampCoarseMain: the CoarseLevel > 0 gather -------------------
	//
	// A separate class rather than a permutation of FVoxelAssetStampCS because
	// the two kernels are different SHAPES, not one kernel with a flag: the
	// scatter is threaded over baked columns, the gather over level-L cells of
	// a host-computed covering box, and their parameter sets differ by exactly
	// the fields that describe that box. Keeping level 0 on the untouched
	// scatter class is also what makes "level 0 byte-identical" a property of
	// the code layout instead of a test result. Same .usf, same span table,
	// same FGlobalShader reasoning as the scatter above.
	class FVoxelAssetStampCoarseCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelAssetStampCoarseCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelAssetStampCoarseCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return true;
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FUintVector2, DispatchColumns)
			SHADER_PARAMETER(uint32, BricksZ)
			SHADER_PARAMETER(int32, BrickZMin)
			SHADER_PARAMETER(FIntPoint, AnchorRel)
			SHADER_PARAMETER(int32, AnchorVz)
			SHADER_PARAMETER(int32, GridOriginZ)
			SHADER_PARAMETER(int32, RotOriginX)
			SHADER_PARAMETER(int32, RotOriginY)
			SHADER_PARAMETER(uint32, YawQuarter)
			SHADER_PARAMETER(uint32, SizeX)
			SHADER_PARAMETER(uint32, SizeY)
			SHADER_PARAMETER(uint32, SizeZ)
			SHADER_PARAMETER(uint32, ColStartsBase)
			// The gather's own three: the scale (host-computed 1 << level, same
			// VARIABLE_SHIFT reasoning as FillLooseParameters' CoarseScale) and
			// the covering cell box the dispatch is threaded over.
			SHADER_PARAMETER(uint32, CoarseScale)
			SHADER_PARAMETER(FUintVector2, CellBoxMin)
			SHADER_PARAMETER(FUintVector2, CellBoxSize)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ColStarts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Spans)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutCells)
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
	// --- QuadCompactMain / QuadPoolWriteMain: the D1 copies ----------------
	//
	// Same reasoning as FVoxelQuadTotalCS above, one step further: these are not
	// worldgen kernels, they do not #include worldgen.ush, and they have no
	// business inside the version lock or the determinism digest. They also do
	// not need SM6 -- copying a uint2 is not 64-bit integer maths -- so they gate
	// on SM5 and say so, rather than inheriting a requirement they do not have.
	//
	// They live in THIS translation unit, next to the other kernel declarations,
	// because IMPLEMENT_GLOBAL_SHADER must appear exactly once per class. Their
	// callers are elsewhere and reach them through the two AddQuad*Pass functions
	// declared in VoxelGpuWorldGenGraph.h.
	class FVoxelQuadCompactCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelQuadCompactCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelQuadCompactCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, NumQuads)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InQuads)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutQuads)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelQuadPoolWriteCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelQuadPoolWriteCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelQuadPoolWriteCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, DstFirst)
			SHADER_PARAMETER(uint32, NumQuads)
			SHADER_PARAMETER(uint32, ChunkId)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InQuads)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutQuads)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkIds)
		END_SHADER_PARAMETER_STRUCT()
	};

	// S2-1: id-only write over a freed range. Shares FVoxelQuadPoolWriteCS's
	// parameter struct shape minus the source buffer -- InQuads/OutQuads are not
	// bound because the hide never reads or writes quad payload.
	class FVoxelQuadPoolHideCS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelQuadPoolHideCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelQuadPoolHideCS, FGlobalShader);

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, DstFirst)
			SHADER_PARAMETER(uint32, NumQuads)
			SHADER_PARAMETER(uint32, ChunkId)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkIds)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- P1-C: the BrickPack chain -----------------------------------------
	//
	// SHARED COMPILE POLICY, AND IT IS DELIBERATELY NOT FVoxelWorldGenShader'S.
	// brickpack.ush carries NO worldgen version lock, because it contains no
	// worldgen math -- only the Cells indexing convention, mirrored in six
	// lines. So these must not be handed VXC_WORLDGEN_VERSION_CPP: doing so
	// would entangle the brick format's release cadence with the terrain
	// digest's, and a terrain version bump would then invalidate brick bytecode
	// that cannot possibly have changed. The argument is VoxelAssetStamp.usf's,
	// verbatim in shape, and VoxelBrickPack.usf's header states it too.
	//
	// SM6 anyway, and not because the kernels need 64-bit integers -- they do
	// not. They read the Cells buffer that VoxelizeMain wrote, and VoxelizeMain
	// needs SM6. A permutation compiled for a feature level on which its own
	// input cannot be produced is a permutation that can only ever be wrong.
	class FVoxelBrickPackShader : public FGlobalShader
	{
	public:
		FVoxelBrickPackShader() = default;
		FVoxelBrickPackShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM6);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
		                                         FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			// Switches brickpack.ush's resource DECLARATIONS (not its
			// arithmetic) to Unreal's conventions: no register() slots, and the
			// parameters as loose globals in $Globals.
			OutEnvironment.SetDefine(TEXT("VXC_UE"), 1);
		}
	};

	// The parameters brickpack.ush declares, by the names it declares them
	// under. DispatchColumns / BricksZ / ScanCount keep worldgen.ush's names and
	// meanings on purpose -- they describe the same region, and a second
	// vocabulary for one region is how two halves of a chain drift apart.
	//
	// THE FOUR WRITE BASES ARE ZERO ON EVERY PATH THIS FILE DISPATCHES, and that
	// is a decision, not an omission: a non-zero base lands IN the descriptor's
	// offset field, so keeping them at zero is what makes the scratch output the
	// chunk-relative form docs/brick-volume-format.md defines and
	// voxel.GPU.VerifyBrickPack compares against. The pool base is added later,
	// in exactly one kernel. They are still THREADED THROUGH rather than
	// deleted, because the kernel reads them either way and a parameter that
	// silently does nothing is this project's most expensive recurring bug.
	#define VOXEL_BRICKPACK_LOOSE_PARAMETERS() \
		SHADER_PARAMETER(FUintVector2, DispatchColumns) \
		SHADER_PARAMETER(uint32,       BricksZ) \
		SHADER_PARAMETER(uint32,       ScanCount) \
		SHADER_PARAMETER(uint32,       BrickDescBase) \
		SHADER_PARAMETER(uint32,       OccWriteBase) \
		SHADER_PARAMETER(uint32,       MatWriteBase) \
		SHADER_PARAMETER(uint32,       ChunkRecordBase)

	class FVoxelBrickClassifyCS : public FVoxelBrickPackShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickClassifyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickClassifyCS, FVoxelBrickPackShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICKPACK_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCells)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickOccCounts)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickMatCounts)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPackCS : public FVoxelBrickPackShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPackCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPackCS, FVoxelBrickPackShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICKPACK_LOOSE_PARAMETERS()
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InCells)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickOccOffsets)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickMatOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickOcc)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickMat)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickSkip)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkBrickMask)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- P1-C / P2: the pool-write kernels ---------------------------------
	//
	// Plain FGlobalShader, SM5, no VXC_UE: VoxelBrickPoolWrite.usf includes
	// nothing from voxel-core and interprets no format field except the 28-bit
	// offset and the 2-bit kind. Same standing as FVoxelQuadPoolWriteCS, and for
	// the same reasons -- it has no business inside any version lock.
	class FVoxelBrickPoolShader : public FGlobalShader
	{
	public:
		FVoxelBrickPoolShader() = default;
		FVoxelBrickPoolShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
			: FGlobalShader(Initializer) {}

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FVoxelBrickTotalCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickTotalCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickTotalCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, ScanCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InOccCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InOccOffsets)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InMatCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InMatOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickTotals)
		END_SHADER_PARAMETER_STRUCT()
	};

	// Tier B.1 (voxel.GPU.WorldGenBatch). Splits a batched stack region's two
	// scans into per-chunk totals -- see BrickStackTotalsMain's header comment
	// in VoxelBrickPoolWrite.usf for why the split is a subtraction over the
	// exclusive scans and why every batch cross-checks it. Same compile policy
	// as FVoxelBrickTotalCS, and for the same reason: it is arithmetic over the
	// scan arrays, not worldgen, so it has no business inside the version lock.
	class FVoxelBrickStackTotalsCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickStackTotalsCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickStackTotalsCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, ScanCount)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InOccCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InOccOffsets)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InMatCounts)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InMatOffsets)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutBrickTotals)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickWordCopyCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickWordCopyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickWordCopyCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, DstFirst)
			SHADER_PARAMETER(uint32, NumWords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InWords)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutWords)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickDescPoolWriteCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickDescPoolWriteCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickDescPoolWriteCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, DstFirst)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, OccBase)
			SHADER_PARAMETER(uint32, MatBase)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutBrickDesc)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickChunkRecordCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickChunkRecordCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickChunkRecordCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, SrcChunkIndex)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, ChunkSlot)
			SHADER_PARAMETER(uint32, BrickBase)
			SHADER_PARAMETER(uint32, RingLevel)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER(uint32, ChunkClimatePacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceGradPacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceZRelBits)
			SHADER_PARAMETER(FIntVector3, OriginVoxel)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickOcc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkBrickMask)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickChunkClearCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickChunkClearCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickChunkClearCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, ChunkSlot)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- the BATCHED pool flush (voxel.GPU.BrickFlushBatch) -----------------
	//
	// Table-driven twins of the four classic pool-write kernels above, plus the
	// live verify. Same compile policy as FVoxelBrickPoolShader's other
	// children: no worldgen include, no version lock -- these move dwords and
	// interpret nothing but the 28-bit offset and the 2-bit kind. See
	// VoxelBrickPoolWrite.usf for the table layout and why destinations need a
	// table at all (non-contiguous per-chunk pool allocations).

	class FVoxelBrickFlushBatchWordCopyCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickFlushBatchWordCopyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickFlushBatchWordCopyCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumTableEntries)
			SHADER_PARAMETER(uint32, TotalWords)
			SHADER_PARAMETER(uint32, TableFieldFirst)
			SHADER_PARAMETER(uint32, FlushTableStride)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InFlushTable)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InWords)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutWords)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickFlushBatchDescWriteCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickFlushBatchDescWriteCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickFlushBatchDescWriteCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumTableEntries)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, FlushTableStride)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InFlushTable)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutBrickDesc)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickFlushBatchRecordCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickFlushBatchRecordCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickFlushBatchRecordCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumTableEntries)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER(uint32, FlushTableStride)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InFlushTable)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickOcc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkBrickMask)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickFlushBatchClearCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickFlushBatchClearCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickFlushBatchClearCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumTableEntries)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InFlushTable)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickFlushVerifyCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickFlushVerifyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickFlushVerifyCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, SrcFirst)
			SHADER_PARAMETER(uint32, SrcChunkIndex)
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, ChunkSlot)
			SHADER_PARAMETER(uint32, BrickBase)
			SHADER_PARAMETER(uint32, RingLevel)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER(uint32, ChunkClimatePacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceGradPacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceZRelBits)
			SHADER_PARAMETER(FIntVector3, OriginVoxel)
			SHADER_PARAMETER(uint32, OccBase)
			SHADER_PARAMETER(uint32, MatBase)
			SHADER_PARAMETER(uint32, OccSrcFirst)
			SHADER_PARAMETER(uint32, MatSrcFirst)
			SHADER_PARAMETER(uint32, VerifyOccWords)
			SHADER_PARAMETER(uint32, VerifyMatWords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickOcc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InWords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkBrickMask)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InPoolDesc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPoolOcc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPoolMat)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPoolTable)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerify)
		END_SHADER_PARAMETER_STRUCT()
	};

	// --- P1: GPU-side pool allocation (voxel.GPU.PoolAlloc) -----------------
	//
	// Same standing as FVoxelBrickPoolShader's other children: these kernels
	// move dwords and interpret nothing but the 28-bit offset and the 2-bit
	// kind, so SM5 and no version lock. See VoxelBrickPoolAlloc.usf for the
	// allocator design; every layout number below is BOUND from
	// FVoxelBrickPool's one authoritative copy, never restated.

	// The layout parameters every alloc kernel shares, spelled once. A macro
	// rather than a nested struct because BEGIN_SHADER_PARAMETER_STRUCT
	// reflection matches loose global names, and the .usf declares these as
	// loose globals exactly as VoxelBrickPoolWrite.usf does.
	#define VOXEL_BRICK_ALLOC_LAYOUT_PARAMETERS() \
		SHADER_PARAMETER(uint32, OccRegionFirst) \
		SHADER_PARAMETER(uint32, OccRegionWords) \
		SHADER_PARAMETER(uint32, MatRegionFirst) \
		SHADER_PARAMETER(uint32, MatRegionWords) \
		SHADER_PARAMETER(uint32, OccClassStep) \
		SHADER_PARAMETER(uint32, OccClasses) \
		SHADER_PARAMETER(uint32, MatClassStep) \
		SHADER_PARAMETER(uint32, MatClasses) \
		SHADER_PARAMETER(uint32, FreeStackCap) \
		SHADER_PARAMETER(uint32, OccTopsFirst) \
		SHADER_PARAMETER(uint32, MatTopsFirst) \
		SHADER_PARAMETER(uint32, OccStackFirst) \
		SHADER_PARAMETER(uint32, MatStackFirst) \
		SHADER_PARAMETER(uint32, OccBitmapFirst) \
		SHADER_PARAMETER(uint32, MatBitmapFirst) \
		SHADER_PARAMETER(uint32, OccHistFirst) \
		SHADER_PARAMETER(uint32, OccHistBucketWords) \
		SHADER_PARAMETER(uint32, OccHistBuckets) \
		SHADER_PARAMETER(uint32, MatHistFirst) \
		SHADER_PARAMETER(uint32, MatHistBucketWords) \
		SHADER_PARAMETER(uint32, MatHistBuckets)

	class FVoxelBrickPoolClaimCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolClaimCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolClaimCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICK_ALLOC_LAYOUT_PARAMETERS()
			SHADER_PARAMETER(uint32, ChunkSlot)
			SHADER_PARAMETER(uint32, OccWorstWords)
			SHADER_PARAMETER(uint32, MatWorstWords)
			// Stack-claim (see the .usf): 0 = classic totals at [0..1]; n =
			// fused-stack member n-1, totals at [2+2c..], prefix summed in-kernel.
			SHADER_PARAMETER(uint32, TotalsChunkIndexPlusOne)
			// Stack size K; non-zero arms the in-kernel split gate.
			SHADER_PARAMETER(uint32, TotalsNumChunks)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickTotals)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocState)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocBitmap)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocSide)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutClaim)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPoolAllocWordCopyCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolAllocWordCopyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolAllocWordCopyCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, MaxWords)
			SHADER_PARAMETER(uint32, WordsClaimIndex)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InClaim)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InWords)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutWords)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPoolAllocDescCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolAllocDescCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolAllocDescCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, BrickBase)
			// Stack-claim: this member's slice of the shared scratch descs.
			SHADER_PARAMETER(uint32, SrcDescBase)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InClaim)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, OutBrickDesc)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPoolAllocRecordCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolAllocRecordCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolAllocRecordCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, BrickCount)
			SHADER_PARAMETER(uint32, ChunkSlot)
			SHADER_PARAMETER(uint32, BrickBase)
			SHADER_PARAMETER(uint32, RingLevel)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER(uint32, ChunkClimatePacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceGradPacked)
			SHADER_PARAMETER(uint32, ChunkSurfaceZRelBits)
			SHADER_PARAMETER(FIntVector3, OriginVoxel)
			// Stack-claim: this member's desc slice and L1-mask slot.
			SHADER_PARAMETER(uint32, SrcDescBase)
			SHADER_PARAMETER(uint32, ChunkMaskBase)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InClaim)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, InBrickDesc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InBrickOcc)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InChunkBrickMask)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPoolFreeCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolFreeCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolFreeCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICK_ALLOC_LAYOUT_PARAMETERS()
			SHADER_PARAMETER(uint32, NumSlots)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InSlotList)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocState)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocBitmap)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, AllocSide)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutChunkTable)
		END_SHADER_PARAMETER_STRUCT()
	};

	class FVoxelBrickPoolAllocVerifyCS : public FVoxelBrickPoolShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FVoxelBrickPoolAllocVerifyCS);
		SHADER_USE_PARAMETER_STRUCT(FVoxelBrickPoolAllocVerifyCS, FVoxelBrickPoolShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_BRICK_ALLOC_LAYOUT_PARAMETERS()
			SHADER_PARAMETER(uint32, NumEntries)
			SHADER_PARAMETER(uint32, ChunkRecordDwords)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InExpect)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InPoolTable)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InAllocSide)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InAllocBitmap)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutVerify)
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
		using FPermutationDomain = TShaderPermutationDomain<FRasterAtlasDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			VOXEL_WORLDGEN_LOOSE_PARAMETERS()
			VOXEL_RASTER_ATLAS_PARAMETERS()
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

#define VOXEL_QUAD_POOL_WRITE_USF "/VoxelEarth/VoxelQuadPoolWrite.usf"

IMPLEMENT_GLOBAL_SHADER(FVoxelQuadCompactCS,   VOXEL_QUAD_POOL_WRITE_USF, "QuadCompactMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelQuadPoolWriteCS, VOXEL_QUAD_POOL_WRITE_USF, "QuadPoolWriteMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelQuadPoolHideCS,  VOXEL_QUAD_POOL_WRITE_USF, "QuadPoolHideMain",  SF_Compute);

#define VOXEL_BRICK_PACK_USF "/VoxelEarth/VoxelBrickPack.usf"

IMPLEMENT_GLOBAL_SHADER(FVoxelBrickClassifyCS, VOXEL_BRICK_PACK_USF, "BrickClassifyMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPackCS,     VOXEL_BRICK_PACK_USF, "BrickPackMain",     SF_Compute);

#define VOXEL_BRICK_POOL_WRITE_USF "/VoxelEarth/VoxelBrickPoolWrite.usf"

IMPLEMENT_GLOBAL_SHADER(FVoxelBrickTotalCS,         VOXEL_BRICK_POOL_WRITE_USF, "BrickTotalMain",         SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickStackTotalsCS,   VOXEL_BRICK_POOL_WRITE_USF, "BrickStackTotalsMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickWordCopyCS,      VOXEL_BRICK_POOL_WRITE_USF, "BrickWordCopyMain",      SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickDescPoolWriteCS, VOXEL_BRICK_POOL_WRITE_USF, "BrickDescPoolWriteMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickChunkRecordCS,   VOXEL_BRICK_POOL_WRITE_USF, "BrickChunkRecordMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickChunkClearCS,    VOXEL_BRICK_POOL_WRITE_USF, "BrickChunkClearMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickFlushBatchWordCopyCS,  VOXEL_BRICK_POOL_WRITE_USF, "BrickFlushBatchWordCopyMain",  SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickFlushBatchDescWriteCS, VOXEL_BRICK_POOL_WRITE_USF, "BrickFlushBatchDescWriteMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickFlushBatchRecordCS,    VOXEL_BRICK_POOL_WRITE_USF, "BrickFlushBatchRecordMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickFlushBatchClearCS,     VOXEL_BRICK_POOL_WRITE_USF, "BrickFlushBatchClearMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickFlushVerifyCS,         VOXEL_BRICK_POOL_WRITE_USF, "BrickFlushVerifyMain",         SF_Compute);

#define VOXEL_BRICK_POOL_ALLOC_USF "/VoxelEarth/VoxelBrickPoolAlloc.usf"

IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolClaimCS,         VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolClaimMain",         SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolAllocWordCopyCS, VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolAllocWordCopyMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolAllocDescCS,     VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolAllocDescMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolAllocRecordCS,   VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolAllocRecordMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolFreeCS,          VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolFreeMain",          SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelBrickPoolAllocVerifyCS,   VOXEL_BRICK_POOL_ALLOC_USF, "BrickPoolAllocVerifyMain",   SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FVoxelAssetStampCS, "/VoxelEarth/VoxelAssetStamp.usf", "AssetStampMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelAssetStampCoarseCS, "/VoxelEarth/VoxelAssetStamp.usf", "AssetStampCoarseMain", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FVoxelColumnCS,     VOXEL_WORLDGEN_USF, "ColumnMain",     SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelVoxelizeCS,   VOXEL_WORLDGEN_USF, "VoxelizeMain",   SF_Compute);

// P3 Column stage: entry points live in VoxelWorklistColumn.usf (the record
// load and arena write), which includes worldgen.ush for columnSampleAt.
#define VOXEL_WORKLIST_COLUMN_USF "/VoxelEarth/VoxelWorklistColumn.usf"
IMPLEMENT_GLOBAL_SHADER(FVoxelWorklistColumnCS,       VOXEL_WORKLIST_COLUMN_USF, "ColumnWorklistMain",       SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelWorklistColumnVerifyCS, VOXEL_WORKLIST_COLUMN_USF, "ColumnWorklistVerifyMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMeshCountCS,  VOXEL_WORLDGEN_USF, "MeshCountMain",  SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanBlocksCS, VOXEL_WORLDGEN_USF, "ScanBlocksMain", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanSumsCS,   VOXEL_WORLDGEN_USF, "ScanSumsMain",   SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelScanAddCS,    VOXEL_WORLDGEN_USF, "ScanAddMain",    SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelMeshEmitCS,   VOXEL_WORLDGEN_USF, "MeshEmitMain",   SF_Compute);

namespace
{

	// P1-C. The brickpack.ush half of FillLooseParameters, and the ONE place the
	// four write bases are set. They are ZERO here and nowhere else decides:
	// every dispatch this file makes produces the CHUNK-RELATIVE form the byte
	// contract defines, and FVoxelBrickPool adds the pool base afterwards in a
	// single kernel. Threaded through rather than deleted -- see the macro's own
	// comment.
	template <typename TParams>
	void FillBrickPackParameters(TParams& Out, const FVoxelGpuRegionRequest& Req, uint32 NumBricks)
	{
		Out.DispatchColumns = Req.DispatchColumns;
		Out.BricksZ = Req.BricksZ;
		Out.ScanCount = NumBricks;
		Out.BrickDescBase = 0;
		Out.OccWriteBase = 0;
		Out.MatWriteBase = 0;
		Out.ChunkRecordBase = 0;
	}

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
		// 0..6 since the 8 km ring (2026-08-23): level 6 is just scale 64 to
		// this kernel -- coarseRep() is closed-form in the scale, nothing else
		// changes. The clamp ceiling mirrors FVoxelMarchChunkIndex::kLevels-1;
		// a literal here is how the fork silently declines a level the
		// cascade streams.
		Out.CoarseScale     = 1u << static_cast<uint32>(FMath::Clamp(Req.CoarseLevel, 0, 6));
		Out.RingSkirtMask   = Req.RingSkirtMask & 0xfu;
		// Backlog 0.0b: one process-wide value, from the single accessor the
		// CPU samplers and voxel.GPU.VerifyCoarse's reference also read -- so
		// the two arms cannot be configured apart. Dead in the kernel at
		// CoarseScale == 1, which keeps level 0 (and the pinned digest) a
		// statement about the pre-0.0b program in both flag states.
		Out.SurfaceMip      = VoxelGpuWorldGen::SurfaceMipEnabled() ? 1u : 0u;
	}

	// A. Fills the atlas half of the three sampling kernels' parameters. The
	// caller has already Register()ed the atlas into this graph.
	template <typename TParams>
	void FillRasterAtlasParameters(TParams& Out, const FVoxelRasterAtlasBindings& B)
	{
		Out.AtlasElevationMm = B.ElevationMm;
		Out.AtlasClimatePacked = B.ClimatePacked;
		Out.AtlasPageTags = B.PageTags;
		Out.AtlasMissStats = B.MissStats;
		Out.AtlasPagePx = B.PagePx;
		Out.AtlasPagesDim = B.PagesDim;
	}

	// P1-C. The three scan passes, over whichever count array is handed in.
	//
	// THIS IS worldgen.ush's SCAN, UNMODIFIED, AND THAT IS THE POINT. It is a
	// generic exclusive scan over a uint count array; the only things that
	// change between meshing and brick packing are ScanCount (3,072 masks ->
	// 64 bricks) and which buffers are bound to OutQuadCounts / OutQuadOffsets /
	// OutBlockSums. Writing a second scan for the brick arenas would be two
	// implementations of one prefix sum, and the mesher's is the one with a
	// determinism gate behind it.
	//
	// The names are the mesher's because the SHADER's names are: renaming them
	// would mean editing worldgen.ush, which is under a version lock shared with
	// the standalone determinism bench.
	void AddBrickScanPasses(FRDGBuilder& GraphBuilder, const FVoxelGpuRegionRequest& Request,
	                        const VoxelGpuWorldGen::FRegionGraphSizes& S,
	                        FRDGBufferRef Counts, FRDGBufferRef Offsets, FRDGBufferRef BlockSums,
	                        const TCHAR* Label)
	{
		{
			FVoxelScanBlocksCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelScanBlocksCS::FParameters>();
			FillLooseParameters(*Params, Request);
			Params->ScanCount = S.NumBricks;
			Params->OutQuadCounts = GraphBuilder.CreateUAV(Counts);
			Params->OutQuadOffsets = GraphBuilder.CreateUAV(Offsets);
			Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSums);

			TShaderMapRef<FVoxelScanBlocksCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickScanBlocks(%s)", Label), Shader, Params,
				FIntVector(S.BrickScanBlocks, 1, 1));
		}
		{
			FVoxelScanSumsCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelScanSumsCS::FParameters>();
			FillLooseParameters(*Params, Request);
			Params->ScanCount = S.NumBricks;
			Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSums);

			TShaderMapRef<FVoxelScanSumsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickScanSums(%s)", Label), Shader, Params,
				FIntVector(1, 1, 1));   // exactly one workgroup, by design
		}
		{
			FVoxelScanAddCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelScanAddCS::FParameters>();
			FillLooseParameters(*Params, Request);
			Params->ScanCount = S.NumBricks;
			Params->OutQuadOffsets = GraphBuilder.CreateUAV(Offsets);
			Params->OutBlockSums = GraphBuilder.CreateUAV(BlockSums);

			TShaderMapRef<FVoxelScanAddCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickScanAdd(%s)", Label), Shader, Params,
				FIntVector(S.BrickScanBlocks, 1, 1));
		}
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
	if (Req.bRasterAtlas)
	{
		// A: the atlas form carries NO window. Half-and-half is refused rather
		// than letting one source silently win -- a request that set the flag
		// but also filled a window has two authorities for the same pixels, and
		// whichever the kernel compiled against would look correct while the
		// other went stale.
		if (Req.RasterAtlas == nullptr || !Req.RasterAtlas->IsInitialized())
		{
			OutError = TEXT("bRasterAtlas without an initialized RasterAtlas");
			return false;
		}
		if (Req.RasterSize.X != 0 || Req.RasterSize.Y != 0 ||
		    Req.ElevationMm.Num() != 0 || Req.ClimatePacked.Num() != 0)
		{
			OutError = TEXT("bRasterAtlas with an inline window: two raster authorities on one request");
			return false;
		}
		if (Req.PixelSizeMm <= 0)
		{
			// The one raster fact the atlas form still carries -- the kernels'
			// mm->pixel divisions run on it exactly as before.
			OutError = TEXT("bRasterAtlas with PixelSizeMm <= 0");
			return false;
		}
	}
	else
	{
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

	if (Req.AssetInstances.Num() > 0)
	{
		// Coarse regions stamp through AssetStampCoarseMain now (the rep-coord
		// gather, byte-parity target FCoarseChunkGridSampler), so instances are
		// accepted at any level the terrain kernels themselves run at. What is
		// STILL refused is a level outside [0, 5]: FillLooseParameters clamps
		// the terrain's CoarseScale silently, and a stamp composed at a clamped
		// scale would sit at the wrong offset inside plausible terrain -- the
		// exact wrong-but-plausible output this function exists to catch.
		// 0..6 since the 8 km ring -- must track FillLooseParameters' clamp
		// above, for the reason stated there.
		if (Req.CoarseLevel < 0 || Req.CoarseLevel > 6)
		{
			OutError = FString::Printf(
				TEXT("AssetInstances (%d) on a CoarseLevel %d region — the stamp supports levels ")
				TEXT("0..6 (the range the terrain kernels' CoarseScale is derived over); a clamped ")
				TEXT("scale would stamp instances at the wrong offset inside plausible terrain."),
				Req.AssetInstances.Num(), Req.CoarseLevel);
			return false;
		}
		// Prefix-table shape: each instance owns SizeX*SizeY+1 entries and the
		// spans it indexes must exist. A packing overflow (SizeZ > 4095) is
		// refused HERE, where the species can still be named by the caller,
		// rather than truncated into a hole at the top of a tall asset.
		for (const FVoxelGpuRegionRequest::FAssetInstance& Inst : Req.AssetInstances)
		{
			if (Inst.SizeX == 0 || Inst.SizeY == 0 || Inst.SizeZ == 0 || Inst.SizeZ > 4095)
			{
				OutError = FString::Printf(
					TEXT("AssetInstance box %ux%ux%u is empty or too tall for span packing ")
					TEXT("(SizeZ max 4095)"), Inst.SizeX, Inst.SizeY, Inst.SizeZ);
				return false;
			}
			const uint64 Cols = uint64(Inst.SizeX) * uint64(Inst.SizeY);
			if (uint64(Inst.ColStartsBase) + Cols + 1 > uint64(Req.AssetColStarts.Num()))
			{
				OutError = FString::Printf(
					TEXT("AssetInstance ColStarts [%u, %llu) overruns AssetColStarts (%d)"),
					Inst.ColStartsBase, uint64(Inst.ColStartsBase) + Cols + 1,
					Req.AssetColStarts.Num());
				return false;
			}
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

	// --- P1-C: the brick chain's region shape -------------------------------
	//
	// brickpack.ush's decodeBrick has NO brick origin: it decomposes the region
	// into whole 4x4x4-brick chunks starting at the region's own corner, and
	// packs chunk c as bricks [4c, 4c+4) on every axis. So a region that is not
	// a whole number of chunks on every axis is SILENTLY PARTIALLY PACKED, and
	// -- much worse -- the mesher's 48x48x6 footprint would pack its HALO
	// corner: bricks 0..3 rather than the interior 1..4. That produces a
	// complete, self-consistent world displaced by one brick on every axis,
	// which passes every per-brick test and fails only as a screenshot.
	//
	// The kernel cannot check this without silently disabling itself, which is
	// the failure mode this project has already paid for. So it is refused here.
	if (Req.bBrickPack)
	{
		constexpr uint32 kChunkColumns = kBrickEdge * kBricksPerChunkEdge;   // 32
		if ((Cx % kChunkColumns) != 0 || (Cy % kChunkColumns) != 0 ||
		    (Req.BricksZ % kBricksPerChunkEdge) != 0)
		{
			OutError = FString::Printf(
				TEXT("bBrickPack on a %ux%u-column x %u-brick region — the brick chain packs WHOLE ")
				TEXT("render chunks counted from the region corner, so it needs columns that are ")
				TEXT("multiples of %u and BricksZ a multiple of %u. The mesher's 48x48x6 halo ")
				TEXT("footprint is exactly the shape that would pack the halo corner instead of the ")
				TEXT("interior — use VoxelGpuChunkRegion::MakeBrickRegion."),
				Cx, Cy, Req.BricksZ, kChunkColumns, kBricksPerChunkEdge);
			return false;
		}

		const uint64 NumBricks = uint64(Cx / kBrickEdge) * uint64(Cy / kBrickEdge) * uint64(Req.BricksZ);
		if (NumBricks == 0)
		{
			OutError = TEXT("bBrickPack region contains no bricks");
			return false;
		}
		// The brick counts are scanned by the SAME ScanSumsMain the mesher uses,
		// which scans the per-block totals in a single 256-thread workgroup.
		if (NumBricks > uint64(kMaxMasksPerDispatch))
		{
			OutError = FString::Printf(
				TEXT("bBrickPack brick count %llu exceeds the %u the single-workgroup ScanSumsMain ")
				TEXT("can scan — split the region into z-slabs"),
				NumBricks, kMaxMasksPerDispatch);
			return false;
		}
	}

	// Tier B.1: both batch-support flags are only meaningful on a brick-pack
	// region, and both are REFUSED elsewhere rather than ignored. A flag that
	// is set and silently does nothing is the exact failure shape that let the
	// GPU gate test nothing for a long stretch (docs: "vxc_gpu tested nothing")
	// -- the caller believes it asked for per-chunk totals or a readback, gets
	// neither, and every downstream read of the missing data looks like its own
	// unrelated bug.
	if (Req.bPerChunkBrickTotals && !Req.bBrickPack)
	{
		OutError = TEXT("bPerChunkBrickTotals without bBrickPack — there is no brick scan to ")
		           TEXT("split, so the flag would be silently inert");
		return false;
	}
	if (Req.bReadbackBricks && !Req.bBrickPack)
	{
		OutError = TEXT("bReadbackBricks without bBrickPack — there are no brick buffers to ")
		           TEXT("read back, so the flag would be silently inert");
		return false;
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

	S.bBrickPack = Request.bBrickPack;
	if (S.bBrickPack)
	{
		// Whole chunks only -- ValidateRegionRequest has already refused
		// anything else, so these divisions are exact.
		S.NumBrickChunks = (S.BricksX / kBricksPerChunkEdge)
		                 * (S.BricksY / kBricksPerChunkEdge)
		                 * (S.BricksZ / kBricksPerChunkEdge);
		S.NumBricks = S.NumBrickChunks * kBricksPerChunk;
		S.BrickScanBlocks = FMath::DivideAndRoundUp(S.NumBricks, kScanBlockSize);
		// The WORST CASE, which is what the scratch buffers are sized to: every
		// brick MIXED at 8 bpp. One chunk is 4 KB of occupancy and 33.8 KB of
		// material, so the scratch side needs no readback to size itself. Only
		// the POOL allocation does, and that is what BrickTotalMain's 8 bytes
		// are for.
		S.BrickOccWordsMax = S.NumBricks * kBrickOccWords;
		S.BrickMatWordsMax = S.NumBricks * kMaxBrickMatWords;
	}
	return S;
}

// The seven passes, and nothing else. No execute, no readback, no blocking --
// see VoxelGpuWorldGenGraph.h for why those are the caller's decisions.
VoxelGpuWorldGen::FRegionGraphResources
VoxelGpuWorldGen::AddRegionPasses(FRDGBuilder& GraphBuilder, const FVoxelGpuRegionRequest& Request,
                                  const FWorklistColumnFeed* ColumnFeed)
{
	check(IsInRenderingThread());

	FRegionGraphResources Out;
	Out.Sizes = ComputeRegionGraphSizes(Request);
	const FRegionGraphSizes& S = Out.Sizes;

	const uint32 Cx = Request.DispatchColumns.X;
	const uint32 Cy = Request.DispatchColumns.Y;

	// --- P3 Column stage: are this region's columns already in the arena? ---
	//
	// The feed's preconditions are the worklist record eligibility, restated
	// here as hard checks because a violation would not fail -- it would read
	// the wrong slice and build plausible terrain from it.
	const bool bWorklistColumns = ColumnFeed != nullptr && ColumnFeed->Arena != nullptr;
	if (bWorklistColumns)
	{
		checkf(Request.bRasterAtlas && Request.BandEdge == 0
		       && Cx == 32 && Cy == 32
		       && Cx * Cy == FVoxelGpuWorklist::kColumnsPerRecord,
		       TEXT("worklist column feed on a region that is not a lean atlas brick chunk"));
	}
	// Verify arm: run the CLASSIC ColumnMain as well and byte-compare -- the
	// converted path still feeds Voxelize, so what is verified is what runs.
	const bool bVerifyColumns = bWorklistColumns && ColumnFeed->bVerify
	                         && ColumnFeed->VerifyStats != nullptr;

	// --- inputs -----------------------------------------------------
	//
	// A: under bRasterAtlas there IS no per-request raster -- the sampling
	// kernels read the persistent page torus registered below, and the two
	// upload buffers are never created. Register is idempotent per graph
	// (RDG FindExternalBuffer returns the existing handle), so a batch of N
	// atlas jobs in one graph still registers one atlas.
	const bool bAtlas = Request.bRasterAtlas;
	FVoxelRasterAtlasBindings AtlasBindings;
	FRDGBufferRef ElevationBuffer = nullptr;
	FRDGBufferRef ClimateBuffer = nullptr;
	if (bAtlas)
	{
		AtlasBindings = Request.RasterAtlas->Register(GraphBuilder);
	}
	else
	{
		ElevationBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.ElevationMm"), sizeof(int32),
			Request.ElevationMm.Num(), Request.ElevationMm.GetData(),
			Request.ElevationMm.Num() * sizeof(int32));

		ClimateBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.ClimatePacked"), sizeof(uint32),
			Request.ClimatePacked.Num(), Request.ClimatePacked.GetData(),
			Request.ClimatePacked.Num() * sizeof(uint32));
	}

	// --- outputs ----------------------------------------------------
	//
	// P3 Column stage: with a feed and no verify, NO Voxel.Columns transient
	// exists for this region at all -- the columns live in the worklist arena
	// and nothing here produces or consumes a private copy. (Null rather than
	// an unwritten buffer: RDG refuses to extract what no pass wrote, and a
	// caller reaching for it should crash on the null, not on the assert.)
	Out.Columns = (!bWorklistColumns || bVerifyColumns)
		? GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVoxelGpuColumnSample), S.NumColumns),
			TEXT("Voxel.Columns"))
		: nullptr;

	Out.Cells = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumCells),
		TEXT("Voxel.Cells"));

	// --- pass 1: ColumnMain ----------------------------------------
	//
	// SKIPPED with a worklist column feed (the once-per-tick indirect
	// ColumnWorklistMain already computed this chunk's slice) -- that skip is
	// the Column stage's entire win: one pass per tick instead of one per
	// chunk. Under the verify arm the classic pass still runs, into the
	// transient, purely as the byte reference; Voxelize reads the ARENA
	// either way, so the verified path is the live path.
	if (!bWorklistColumns || bVerifyColumns)
	{
		FVoxelColumnCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelColumnCS::FParameters>();
		FillLooseParameters(*Params, Request);
		if (bAtlas)
		{
			FillRasterAtlasParameters(*Params, AtlasBindings);
		}
		else
		{
			Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
			Params->ClimatePacked = GraphBuilder.CreateSRV(ClimateBuffer);
		}
		Params->OutColumns = GraphBuilder.CreateUAV(Out.Columns);

		FVoxelColumnCS::FPermutationDomain Permutation;
		Permutation.Set<FRasterAtlasDim>(bAtlas);
		TShaderMapRef<FVoxelColumnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.ColumnMain"), Shader, Params,
			FIntVector(Cx / kBrickEdge, Cy / kBrickEdge, 1));
	}

	// --- pass 1v (verify arm only): converted columns vs classic ------------
	if (bVerifyColumns)
	{
		FVoxelWorklistColumnVerifyCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelWorklistColumnVerifyCS::FParameters>();
		Params->VerifyClassicColumns = GraphBuilder.CreateSRV(Out.Columns);
		Params->VerifyArenaColumns = GraphBuilder.CreateSRV(ColumnFeed->Arena);
		Params->WorklistStats = GraphBuilder.CreateUAV(ColumnFeed->VerifyStats);
		Params->VerifyArenaBase = ColumnFeed->SliceIndex * FVoxelGpuWorklist::kColumnsPerRecord;

		TShaderMapRef<FVoxelWorklistColumnVerifyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Voxel.WorklistColumnVerify(slice %u)", ColumnFeed->SliceIndex),
			Shader, Params,
			FIntVector(FVoxelGpuWorklist::kColumnGroupsPerRecord, 1, 1));
	}

	// --- pass 2: VoxelizeMain --------------------------------------
	{
		FVoxelVoxelizeCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelVoxelizeCS::FParameters>();
		FillLooseParameters(*Params, Request);
		if (bAtlas)
		{
			FillRasterAtlasParameters(*Params, AtlasBindings);
		}
		else
		{
			Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
		}
		// P3 Column stage: with a feed, this chunk's columns are its slice of
		// the worklist arena, addressed through ColumnReadBase. Classic:
		// the region's own transient at base 0 -- the compiled +0 changes no
		// output byte, which is what keeps the pinned digest green.
		Params->InColumns = GraphBuilder.CreateSRV(
			bWorklistColumns ? ColumnFeed->Arena : Out.Columns);
		Params->ColumnReadBase = bWorklistColumns
			? ColumnFeed->SliceIndex * FVoxelGpuWorklist::kColumnsPerRecord : 0u;
		Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);

		FVoxelVoxelizeCS::FPermutationDomain Permutation;
		Permutation.Set<FRasterAtlasDim>(bAtlas);
		TShaderMapRef<FVoxelVoxelizeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.VoxelizeMain"), Shader, Params,
			FIntVector(Cx / kBrickEdge, Cy / kBrickEdge, 1));
	}

	// --- pass 2a: AssetStampMain / AssetStampCoarseMain, one dispatch per ----
	// --- instance -------------------------------------------------------------
	//
	// Between VoxelizeMain (terrain into Cells) and everything that reads
	// Cells. ONE DISPATCH PER INSTANCE, IN ARRAY ORDER: the RDG UAV barrier
	// between consecutive passes is what serializes them, and that ordering is
	// the byte-parity contract with the CPU's first-non-air-wins compose --
	// instance i+1 must see instance i's writes to lose the same overlaps.
	// Within one dispatch no two threads own the same cell (level 0: the yaw
	// map is a bijection over baked columns; coarse: one thread per level-L
	// cell), so there is no intra-pass race on either kernel.
	//
	// An empty request skips all of this, including for the verify/digest
	// path, which never fills the arrays and therefore stays terrain-only.
	if (Request.AssetInstances.Num() > 0)
	{
		FRDGBufferRef ColStartsBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.AssetColStarts"), sizeof(uint32),
			Request.AssetColStarts.Num(), Request.AssetColStarts.GetData(),
			Request.AssetColStarts.Num() * sizeof(uint32));
		FRDGBufferRef SpansBuffer = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.AssetSpans"), sizeof(uint32),
			Request.AssetSpans.Num(), Request.AssetSpans.GetData(),
			Request.AssetSpans.Num() * sizeof(uint32));

		if (Request.CoarseLevel == 0)
		{
			// Level 0: the scatter kernel, untouched -- byte-identical to every
			// dispatch taken before the coarse gather existed.
			TShaderMapRef<FVoxelAssetStampCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			for (const FVoxelGpuRegionRequest::FAssetInstance& Inst : Request.AssetInstances)
			{
				FVoxelAssetStampCS::FParameters* Params =
					GraphBuilder.AllocParameters<FVoxelAssetStampCS::FParameters>();
				Params->DispatchColumns = Request.DispatchColumns;
				Params->BricksZ = Request.BricksZ;
				Params->BrickZMin = Request.BrickZMin;
				Params->AnchorRel = FIntPoint(Inst.AnchorRelVx, Inst.AnchorRelVy);
				Params->AnchorVz = Inst.AnchorVz;
				Params->GridOriginZ = Inst.GridOriginZ;
				Params->RotOriginX = Inst.RotOriginX;
				Params->RotOriginY = Inst.RotOriginY;
				Params->YawQuarter = Inst.YawQuarter;
				Params->SizeX = Inst.SizeX;
				Params->SizeY = Inst.SizeY;
				Params->SizeZ = Inst.SizeZ;
				Params->ColStartsBase = Inst.ColStartsBase;
				Params->ColStarts = GraphBuilder.CreateSRV(ColStartsBuffer);
				Params->Spans = GraphBuilder.CreateSRV(SpansBuffer);
				Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);

				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("Voxel.AssetStamp"), Shader, Params,
					FIntVector(FMath::DivideAndRoundUp(Inst.SizeX, 8u),
					           FMath::DivideAndRoundUp(Inst.SizeY, 8u), 1));
			}
		}
		else
		{
			// CoarseLevel > 0: the gather kernel, one thread per level-L cell
			// of the instance's covering cell box. The box is computed HERE,
			// mirroring FCoarseChunkGridSampler's shortlist bounds: cells whose
			// rep coordinate COULD fall inside the rotated XY box, i.e.
			// [floorDiv(min - s/2, s), floorDiv(max - s/2, s) + 1] inclusive,
			// clamped to the region -- conservative on purpose, because the
			// kernel's rep-in-box test is the exact filter (same division of
			// labour as the CPU: cover loosely, test exactly). A box clamped to
			// nothing is an instance whose rep coordinates never land in this
			// region: the CPU composes nothing for it, so skipping the dispatch
			// IS parity, not an optimisation over it.
			//
			// floorDiv is inlined here (this module deliberately does not link
			// voxel-core); (min - s/2) is routinely negative for an instance
			// leaning in from the negative side, so operator/ will not do.
			const int32 Scale = 1 << Request.CoarseLevel;   // validated 1..5
			const auto FloorDivI32 = [](int32 A, int32 B) -> int32
			{
				return (A >= 0) ? (A / B) : -((-A + B - 1) / B);
			};

			TShaderMapRef<FVoxelAssetStampCoarseCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			for (const FVoxelGpuRegionRequest::FAssetInstance& Inst : Request.AssetInstances)
			{
				const int32 RotSizeX = (Inst.YawQuarter & 1u) ? int32(Inst.SizeY) : int32(Inst.SizeX);
				const int32 RotSizeY = (Inst.YawQuarter & 1u) ? int32(Inst.SizeX) : int32(Inst.SizeY);
				const int32 MinRelVx = Inst.AnchorRelVx + Inst.RotOriginX;
				const int32 MinRelVy = Inst.AnchorRelVy + Inst.RotOriginY;
				const int32 Half = Scale / 2;

				const int32 C0x = FMath::Max(FloorDivI32(MinRelVx - Half, Scale), 0);
				const int32 C1x = FMath::Min(FloorDivI32(MinRelVx + RotSizeX - 1 - Half, Scale) + 1,
				                             int32(Cx) - 1);
				const int32 C0y = FMath::Max(FloorDivI32(MinRelVy - Half, Scale), 0);
				const int32 C1y = FMath::Min(FloorDivI32(MinRelVy + RotSizeY - 1 - Half, Scale) + 1,
				                             int32(Cy) - 1);
				if (C0x > C1x || C0y > C1y)
				{
					continue;
				}

				FVoxelAssetStampCoarseCS::FParameters* Params =
					GraphBuilder.AllocParameters<FVoxelAssetStampCoarseCS::FParameters>();
				Params->DispatchColumns = Request.DispatchColumns;
				Params->BricksZ = Request.BricksZ;
				Params->BrickZMin = Request.BrickZMin;
				Params->AnchorRel = FIntPoint(Inst.AnchorRelVx, Inst.AnchorRelVy);
				Params->AnchorVz = Inst.AnchorVz;
				Params->GridOriginZ = Inst.GridOriginZ;
				Params->RotOriginX = Inst.RotOriginX;
				Params->RotOriginY = Inst.RotOriginY;
				Params->YawQuarter = Inst.YawQuarter;
				Params->SizeX = Inst.SizeX;
				Params->SizeY = Inst.SizeY;
				Params->SizeZ = Inst.SizeZ;
				Params->ColStartsBase = Inst.ColStartsBase;
				Params->CoarseScale = uint32(Scale);
				Params->CellBoxMin = FUintVector2(uint32(C0x), uint32(C0y));
				Params->CellBoxSize = FUintVector2(uint32(C1x - C0x + 1), uint32(C1y - C0y + 1));
				Params->ColStarts = GraphBuilder.CreateSRV(ColStartsBuffer);
				Params->Spans = GraphBuilder.CreateSRV(SpansBuffer);
				Params->OutCells = GraphBuilder.CreateUAV(Out.Cells);

				FComputeShaderUtils::AddPass(
					GraphBuilder, RDG_EVENT_NAME("Voxel.AssetStampCoarse"), Shader, Params,
					FIntVector(FMath::DivideAndRoundUp(uint32(C1x - C0x + 1), 8u),
					           FMath::DivideAndRoundUp(uint32(C1y - C0y + 1), 8u), 1));
			}
		}
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
		if (bAtlas)
		{
			FillRasterAtlasParameters(*Params, AtlasBindings);
		}
		else
		{
			Params->ElevationMm = GraphBuilder.CreateSRV(ElevationBuffer);
		}
		Params->InColumns = GraphBuilder.CreateSRV(Out.Columns);
		Params->OutBand = GraphBuilder.CreateUAV(Out.Band);

		FVoxelBandReduceCS::FPermutationDomain Permutation;
		Permutation.Set<FRasterAtlasDim>(bAtlas);
		TShaderMapRef<FVoxelBandReduceCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BandReduceMain"), Shader, Params,
			FIntVector(1, 1, 1));   // exactly one workgroup, by design
	}

	// --- P1-C: the brick chain, after AssetStamp and off the mesh path ------
	//
	// BrickClassifyMain -> ScanBlocks/ScanSums/ScanAdd x2 -> BrickPackMain, over
	// the SAME Cells buffer the mesher reads, in the SAME graph. Zero new scan
	// code: the scan is worldgen.ush's own, unmodified, with the brick count
	// arrays bound where the quad ones normally go and ScanCount set to the
	// brick count. It is run TWICE because a brick allocates in two independent
	// arenas and one prefix sum cannot describe both -- six dispatches over 64
	// elements each for a single chunk.
	//
	// WITH bBrickPack FALSE NOTHING BELOW RUNS AND THE GRAPH IS BYTE-FOR-BYTE
	// THE ONE THAT SHIPPED. That is the whole shape of this phase: additive,
	// off-path, and switchable at runtime so an A/B is one cvar on one binary.
	//
	// ALL FOUR WRITE BASES ARE ZERO. See VOXEL_BRICKPACK_LOOSE_PARAMETERS.
	if (S.bBrickPack)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Voxel.BrickPack");

		FRDGBufferRef OccCounts = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBricks), TEXT("Voxel.BrickOccCounts"));
		FRDGBufferRef OccOffsets = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBricks), TEXT("Voxel.BrickOccOffsets"));
		FRDGBufferRef MatCounts = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBricks), TEXT("Voxel.BrickMatCounts"));
		FRDGBufferRef MatOffsets = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBricks), TEXT("Voxel.BrickMatOffsets"));
		// One block-sum buffer per scan rather than one reused twice: reuse
		// would be correct (RDG would order the two by the write-after-read on
		// it) but it would also make the second scan wait on the first for no
		// reason other than a buffer name.
		FRDGBufferRef OccBlockSums = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.BrickScanBlocks), TEXT("Voxel.BrickOccBlockSums"));
		FRDGBufferRef MatBlockSums = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.BrickScanBlocks), TEXT("Voxel.BrickMatBlockSums"));

		Out.BrickDesc = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2, S.NumBricks), TEXT("Voxel.BrickDesc"));
		Out.BrickOcc = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.BrickOccWordsMax), TEXT("Voxel.BrickOcc"));
		Out.BrickMat = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.BrickMatWordsMax), TEXT("Voxel.BrickMat"));
		// The 4^3 intra-brick skip mask. BrickPackMain writes it unconditionally
		// (2 dwords per MIXED brick, indexed by the brick's mixed rank), and
		// NOTHING READS IT. It is derivable for free from the 16 occupancy
		// dwords a marcher already holds in registers, and format section 5's
		// own recommendation is to spend the ~7 MiB it would cost across the
		// cascade on payload instead. So it is bound to a transient that dies
		// with the graph.
		Out.BrickSkip = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBricks * 2), TEXT("Voxel.BrickSkip"));
		Out.BrickChunkMask = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), S.NumBrickChunks * 2), TEXT("Voxel.BrickChunkMask"));
		Out.BrickTotals = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2), TEXT("Voxel.BrickTotals"));

		// THE L1 MASK IS ACCUMULATED WITH InterlockedOr AND MUST BE ZEROED
		// FIRST. An RDG transient buffer is not zero-initialised -- it is
		// whatever the pooled allocation last held -- so stale bits would claim
		// occupancy that is no longer there. The marcher would then enter empty
		// bricks rather than miss full ones, which fails slow and invisible: no
		// hole, no crash, just traffic. This is a host precondition brickpack.ush
		// states and cannot enforce.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Out.BrickChunkMask), 0u);

		// --- brick pass 1: BrickClassifyMain ------------------------------
		{
			FVoxelBrickClassifyCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelBrickClassifyCS::FParameters>();
			FillBrickPackParameters(*Params, Request, S.NumBricks);
			Params->InCells = GraphBuilder.CreateSRV(Out.Cells);
			Params->OutBrickOccCounts = GraphBuilder.CreateUAV(OccCounts);
			Params->OutBrickMatCounts = GraphBuilder.CreateUAV(MatCounts);

			TShaderMapRef<FVoxelBrickClassifyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			// ONE WORKGROUP PER BRICK, 64 threads, 8 cells each. The group id IS
			// the brick's pack index, which is what makes decodeBrick's chunk
			// decomposition the contract rather than an arrangement.
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickClassifyMain(%u bricks)", S.NumBricks),
				Shader, Params, FIntVector(S.NumBricks, 1, 1));
		}

		// --- brick passes 2-7: the two scans, worldgen.ush's own -----------
		AddBrickScanPasses(GraphBuilder, Request, S, OccCounts, OccOffsets, OccBlockSums, TEXT("Occ"));
		AddBrickScanPasses(GraphBuilder, Request, S, MatCounts, MatOffsets, MatBlockSums, TEXT("Mat"));

		// --- brick pass 8: BrickPackMain ----------------------------------
		{
			FVoxelBrickPackCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelBrickPackCS::FParameters>();
			FillBrickPackParameters(*Params, Request, S.NumBricks);
			Params->InCells = GraphBuilder.CreateSRV(Out.Cells);
			Params->InBrickOccOffsets = GraphBuilder.CreateSRV(OccOffsets);
			Params->InBrickMatOffsets = GraphBuilder.CreateSRV(MatOffsets);
			Params->OutBrickDesc = GraphBuilder.CreateUAV(Out.BrickDesc);
			Params->OutBrickOcc = GraphBuilder.CreateUAV(Out.BrickOcc);
			Params->OutBrickMat = GraphBuilder.CreateUAV(Out.BrickMat);
			Params->OutBrickSkip = GraphBuilder.CreateUAV(Out.BrickSkip);
			Params->OutChunkBrickMask = GraphBuilder.CreateUAV(Out.BrickChunkMask);

			TShaderMapRef<FVoxelBrickPackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPackMain(%u bricks)", S.NumBricks),
				Shader, Params, FIntVector(S.NumBricks, 1, 1));
		}

		// --- brick pass 9: BrickTotalMain ---------------------------------
		//
		// Unconditional, for FVoxelQuadTotalCS's reason: it costs one thread,
		// and being always present is what lets every path that runs the brick
		// chain -- including the blocking verification one -- cross-check the
		// GPU's totals against the CPU's derivation from the same scan arrays.
		// A kernel only the streaming path exercises is a kernel whose first bug
		// shows up in the streaming path.
		{
			FVoxelBrickTotalCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelBrickTotalCS::FParameters>();
			Params->ScanCount = S.NumBricks;
			Params->InOccCounts = GraphBuilder.CreateSRV(OccCounts);
			Params->InOccOffsets = GraphBuilder.CreateSRV(OccOffsets);
			Params->InMatCounts = GraphBuilder.CreateSRV(MatCounts);
			Params->InMatOffsets = GraphBuilder.CreateSRV(MatOffsets);
			Params->OutBrickTotals = GraphBuilder.CreateUAV(Out.BrickTotals);

			TShaderMapRef<FVoxelBrickTotalCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder, RDG_EVENT_NAME("Voxel.BrickTotalMain"), Shader, Params,
				FIntVector(1, 1, 1));
		}

		// --- brick pass 10, Tier B.1 only: BrickStackTotalsMain ------------
		//
		// Splits the two scans into PER-CHUNK totals so one batched region can
		// feed K per-chunk pool allocations from a single (2 + 2K)-dword
		// readback. Absent (and the graph byte-identical) unless the request
		// asked -- which only the voxel.GPU.WorldGenBatch stack path and its
		// gate do. The region pair is restated in [0, 1] by the same kernel so
		// the harvest can cross-check sum(per-chunk) == region without a second
		// copy; see the kernel's header for why that check exists.
		if (Request.bPerChunkBrickTotals)
		{
			Out.BrickStackTotals = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2 + 2 * S.NumBrickChunks),
				TEXT("Voxel.BrickStackTotals"));

			FVoxelBrickStackTotalsCS::FParameters* Params =
				GraphBuilder.AllocParameters<FVoxelBrickStackTotalsCS::FParameters>();
			Params->ScanCount = S.NumBricks;
			// 64 by contract; from the constant rather than a literal so this
			// host code and decodeBrick's chunk decomposition cannot disagree.
			Params->BrickCount = kBricksPerChunk;
			Params->InOccCounts = GraphBuilder.CreateSRV(OccCounts);
			Params->InOccOffsets = GraphBuilder.CreateSRV(OccOffsets);
			Params->InMatCounts = GraphBuilder.CreateSRV(MatCounts);
			Params->InMatOffsets = GraphBuilder.CreateSRV(MatOffsets);
			Params->OutBrickTotals = GraphBuilder.CreateUAV(Out.BrickStackTotals);

			TShaderMapRef<FVoxelBrickStackTotalsCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Voxel.BrickStackTotals(%u chunks)", S.NumBrickChunks),
				Shader, Params,
				FIntVector(FMath::DivideAndRoundUp(S.NumBrickChunks, 64u), 1, 1));
		}
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

// --- Wave D / D1: the two GPU-side quad copies -----------------------------
//
// Both dispatch one thread per LIVE quad, which is the only bound either kernel
// has. The group size (64) is restated in the shader as an `i >= NumQuads`
// early-out because the dispatch rounds up, so the tail threads of the last
// group are real.

void VoxelGpuWorldGen::AddWorklistColumnPass(FRDGBuilder& GraphBuilder,
                                             const FWorklistColumnDispatch& Dispatch)
{
	check(IsInRenderingThread());
	check(Dispatch.Records && Dispatch.Control && Dispatch.IndirectArgs && Dispatch.ColumnArena);
	check(Dispatch.Atlas != nullptr && Dispatch.PixelSizeMm != 0 && Dispatch.RingCapacity > 0);

	// Register is idempotent per graph -- if the flush graph ever gains a
	// second atlas consumer this stays one registration.
	const FVoxelRasterAtlasBindings AtlasBindings = Dispatch.Atlas->Register(GraphBuilder);

	FVoxelWorklistColumnCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelWorklistColumnCS::FParameters>();
	// The loose block carries the PROCESS-WIDE half only; the per-record half
	// (origin, coarse scale) comes off the ring inside the kernel -- that
	// substitution IS the conversion. The per-region fields are zeroed dead
	// weight the reflection layout requires; the kernel never reads them.
	Params->DispatchColumns = FUintVector2(32, 32);
	Params->RasterOriginPx = FIntPoint::ZeroValue;
	Params->RasterSize = FUintVector2(0, 0);
	Params->PixelSizeMm = Dispatch.PixelSizeMm;
	Params->SeedLo = Dispatch.SeedLo;
	Params->SeedHi = Dispatch.SeedHi;
	Params->OriginVx = 0;
	Params->OriginVy = 0;
	Params->BrickZMin = 0;
	Params->BricksZ = 4;
	Params->ScanCount = 0;
	Params->CoarseScale = 1;
	Params->RingSkirtMask = 0;
	// Same single accessor FillLooseParameters reads, so the converted and
	// classic dispatches cannot be configured apart.
	Params->SurfaceMip = SurfaceMipEnabled() ? 1u : 0u;
	FillRasterAtlasParameters(*Params, AtlasBindings);
	Params->WorklistRecords = GraphBuilder.CreateSRV(Dispatch.Records);
	Params->WorklistControl = GraphBuilder.CreateSRV(Dispatch.Control);
	Params->RingCapacity = Dispatch.RingCapacity;
	Params->OutColumns = GraphBuilder.CreateUAV(Dispatch.ColumnArena);
	Params->IndirectArgs = Dispatch.IndirectArgs;

	TShaderMapRef<FVoxelWorklistColumnCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	// THE dispatch shape this whole arm exists for: group count = Take * 16
	// off the triple the args pass wrote, never seen by the CPU. Recorded
	// even when Take is 0 -- a constant pass per tick is the property.
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.WorklistColumn(indirect)"), Shader, Params,
		Dispatch.IndirectArgs, Dispatch.IndirectArgsOffset);
}

void VoxelGpuWorldGen::AddQuadCompactPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst, FRDGBufferRef Src,
                                          uint32 SrcFirst, uint32 NumQuads)
{
	if (NumQuads == 0 || Dst == nullptr || Src == nullptr)
	{
		// A zero-quad chunk never reaches here -- the manager short-circuits it
		// a phase earlier -- but a pass that dispatches nothing is worse than no
		// pass, because it looks like work in a capture.
		return;
	}

	FVoxelQuadCompactCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelQuadCompactCS::FParameters>();
	Params->SrcFirst = SrcFirst;
	Params->NumQuads = NumQuads;
	Params->InQuads = GraphBuilder.CreateSRV(Src);
	Params->OutQuads = GraphBuilder.CreateUAV(Dst);

	TShaderMapRef<FVoxelQuadCompactCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.QuadCompact(%u quads)", NumQuads), Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(NumQuads, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddQuadPoolWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstQuads, FRDGBufferRef DstIds,
                                            FRDGBufferRef Src, uint32 SrcFirst, uint32 DstFirst,
                                            uint32 NumQuads, uint32 ChunkId)
{
	if (NumQuads == 0 || DstQuads == nullptr || DstIds == nullptr || Src == nullptr)
	{
		return;
	}

	FVoxelQuadPoolWriteCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelQuadPoolWriteCS::FParameters>();
	Params->SrcFirst = SrcFirst;
	Params->DstFirst = DstFirst;
	Params->NumQuads = NumQuads;
	Params->ChunkId = ChunkId;
	Params->InQuads = GraphBuilder.CreateSRV(Src);
	Params->OutQuads = GraphBuilder.CreateUAV(DstQuads);
	Params->OutChunkIds = GraphBuilder.CreateUAV(DstIds);

	TShaderMapRef<FVoxelQuadPoolWriteCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.QuadPoolWrite(chunk %u, %u quads @ %u)", ChunkId, NumQuads, DstFirst),
		Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(NumQuads, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddQuadPoolHidePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstIds,
                                           uint32 DstFirst, uint32 NumQuads, uint32 HiddenChunkId)
{
	if (NumQuads == 0 || DstIds == nullptr)
	{
		return;
	}

	FVoxelQuadPoolHideCS::FParameters* Params = GraphBuilder.AllocParameters<FVoxelQuadPoolHideCS::FParameters>();
	Params->DstFirst = DstFirst;
	Params->NumQuads = NumQuads;
	Params->ChunkId = HiddenChunkId;
	Params->OutChunkIds = GraphBuilder.CreateUAV(DstIds);

	TShaderMapRef<FVoxelQuadPoolHideCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.QuadPoolHide(%u quads @ %u)", NumQuads, DstFirst),
		Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(NumQuads, 64u), 1, 1));
}

// --- P1-C / P2: the four moves that make a packed chunk resident -------------
//
// Each dispatches one thread per element it writes and restates its own bound in
// the shader, because a dispatch size rounds up and the tail threads of the last
// group are real. Same shape and same reasoning as the quad passes above.

void VoxelGpuWorldGen::AddBrickWordCopyPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst, FRDGBufferRef Src,
                                            uint32 SrcFirst, uint32 DstFirst, uint32 NumWords)
{
	if (NumWords == 0 || Dst == nullptr || Src == nullptr)
	{
		// A chunk whose every brick collapsed allocates nothing in an arena, and
		// that is a NORMAL outcome -- an all-air chunk is 64 descriptors and no
		// payload at all, which is the property that makes the census affordable.
		// So this is a legitimate early-out, not the "cannot happen" kind.
		return;
	}

	FVoxelBrickWordCopyCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickWordCopyCS::FParameters>();
	Params->SrcFirst = SrcFirst;
	Params->DstFirst = DstFirst;
	Params->NumWords = NumWords;
	Params->InWords = GraphBuilder.CreateSRV(Src);
	Params->OutWords = GraphBuilder.CreateUAV(Dst);

	TShaderMapRef<FVoxelBrickWordCopyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickWordCopy(%u dwords @ %u)", NumWords, DstFirst),
		Shader, Params, FIntVector(FMath::DivideAndRoundUp(NumWords, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickDescPoolWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstDesc,
                                                 FRDGBufferRef SrcDesc, uint32 SrcFirst, uint32 DstFirst,
                                                 uint32 BrickCount, uint32 OccBase, uint32 MatBase)
{
	if (BrickCount == 0 || DstDesc == nullptr || SrcDesc == nullptr)
	{
		return;
	}

	FVoxelBrickDescPoolWriteCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickDescPoolWriteCS::FParameters>();
	Params->SrcFirst = SrcFirst;
	Params->DstFirst = DstFirst;
	Params->BrickCount = BrickCount;
	Params->OccBase = OccBase;
	Params->MatBase = MatBase;
	Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
	Params->OutBrickDesc = GraphBuilder.CreateUAV(DstDesc);

	TShaderMapRef<FVoxelBrickDescPoolWriteCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickDescPoolWrite(%u descs @ %u, occ+%u mat+%u)",
		               BrickCount, DstFirst, OccBase, MatBase),
		Shader, Params, FIntVector(FMath::DivideAndRoundUp(BrickCount, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickChunkRecordPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
                                               FRDGBufferRef SrcDesc, FRDGBufferRef SrcOcc,
                                               FRDGBufferRef SrcChunkMask,
                                               uint32 SrcFirst, uint32 SrcChunkIndex, uint32 BrickCount,
                                               uint32 ChunkSlot, uint32 BrickBase, uint32 RingLevel,
                                               const FIntVector& OriginVoxel,
                                               const FVoxelBrickChunkShading& Shading)
{
	if (DstTable == nullptr || SrcDesc == nullptr || SrcOcc == nullptr || SrcChunkMask == nullptr)
	{
		return;
	}

	FVoxelBrickChunkRecordCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickChunkRecordCS::FParameters>();
	Params->SrcFirst = SrcFirst;
	Params->SrcChunkIndex = SrcChunkIndex;
	Params->BrickCount = BrickCount;
	Params->ChunkSlot = ChunkSlot;
	Params->BrickBase = BrickBase;
	Params->RingLevel = RingLevel;
	// FROM THE C++ CONSTANT, so this kernel and BuildChunkRecord cannot disagree
	// about the record length. See VoxelBrickPoolWrite.usf's ChunkRecordDwords.
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	// Packed on the CPU, through the same FVoxelBrickChunkShading::Pack that
	// BuildChunkRecord uses, so the kernel receives finished dwords and the two
	// writers cannot lay the bits out differently.
	Shading.Pack(Params->ChunkClimatePacked, Params->ChunkSurfaceGradPacked,
	             Params->ChunkSurfaceZRelBits);
	Params->OriginVoxel = FIntVector3(OriginVoxel.X, OriginVoxel.Y, OriginVoxel.Z);
	Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
	Params->InBrickOcc = GraphBuilder.CreateSRV(SrcOcc);
	Params->InChunkBrickMask = GraphBuilder.CreateSRV(SrcChunkMask);
	Params->OutChunkTable = GraphBuilder.CreateUAV(DstTable);

	TShaderMapRef<FVoxelBrickChunkRecordCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	// ONE workgroup: one thread per brick, and the allSolid reduce is over the
	// 64 of them.
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickChunkRecord(slot %u, L%u)", ChunkSlot, RingLevel),
		Shader, Params, FIntVector(1, 1, 1));
}

void VoxelGpuWorldGen::AddBrickChunkClearPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
                                              uint32 ChunkSlot)
{
	if (DstTable == nullptr)
	{
		return;
	}

	FVoxelBrickChunkClearCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickChunkClearCS::FParameters>();
	Params->ChunkSlot = ChunkSlot;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	Params->OutChunkTable = GraphBuilder.CreateUAV(DstTable);

	TShaderMapRef<FVoxelBrickChunkClearCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickChunkClear(slot %u)", ChunkSlot), Shader, Params,
		FIntVector(1, 1, 1));
}

// --- the BATCHED pool flush (voxel.GPU.BrickFlushBatch) ----------------------
//
// Each pass replaces K of the classic passes above with ONE dispatch over a
// destination table. The table itself is built and owned by
// FVoxelBrickPool::AddFlushPasses_RenderThread (see there for layout and
// lifetime); these functions only bind and dispatch. Every early-out mirrors
// the classic passes': a null buffer or a zero count records nothing, because
// a pass that dispatches nothing looks like work in a capture.

void VoxelGpuWorldGen::AddBrickFlushBatchWordCopyPass(FRDGBuilder& GraphBuilder, FRDGBufferRef Dst,
                                                      FRDGBufferRef Src, FRDGBufferRef Table,
                                                      uint32 TableStride, uint32 TableFieldFirst,
                                                      uint32 NumEntries, uint32 TotalWords)
{
	if (TotalWords == 0 || NumEntries == 0 || Dst == nullptr || Src == nullptr || Table == nullptr)
	{
		// TotalWords 0 is NORMAL: a group of all-collapsed chunks owns no arena
		// words at all, exactly as the classic copy's zero-word early-out says.
		return;
	}

	FVoxelBrickFlushBatchWordCopyCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickFlushBatchWordCopyCS::FParameters>();
	Params->NumTableEntries = NumEntries;
	Params->TotalWords = TotalWords;
	Params->TableFieldFirst = TableFieldFirst;
	Params->FlushTableStride = TableStride;
	Params->InFlushTable = GraphBuilder.CreateSRV(Table);
	Params->InWords = GraphBuilder.CreateSRV(Src);
	Params->OutWords = GraphBuilder.CreateUAV(Dst);

	TShaderMapRef<FVoxelBrickFlushBatchWordCopyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickFlushBatchWordCopy(%u chunks, %u dwords)", NumEntries, TotalWords),
		Shader, Params, FIntVector(FMath::DivideAndRoundUp(TotalWords, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickFlushBatchDescWritePass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstDesc,
                                                       FRDGBufferRef SrcDesc, FRDGBufferRef Table,
                                                       uint32 TableStride, uint32 NumEntries,
                                                       uint32 BrickCount)
{
	if (NumEntries == 0 || BrickCount == 0 || DstDesc == nullptr || SrcDesc == nullptr || Table == nullptr)
	{
		return;
	}

	FVoxelBrickFlushBatchDescWriteCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickFlushBatchDescWriteCS::FParameters>();
	Params->NumTableEntries = NumEntries;
	Params->BrickCount = BrickCount;
	Params->FlushTableStride = TableStride;
	Params->InFlushTable = GraphBuilder.CreateSRV(Table);
	Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
	Params->OutBrickDesc = GraphBuilder.CreateUAV(DstDesc);

	TShaderMapRef<FVoxelBrickFlushBatchDescWriteCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickFlushBatchDescWrite(%u chunks)", NumEntries),
		Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(NumEntries * BrickCount, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickFlushBatchRecordPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
                                                    FRDGBufferRef SrcDesc, FRDGBufferRef SrcOcc,
                                                    FRDGBufferRef SrcChunkMask, FRDGBufferRef Table,
                                                    uint32 TableStride, uint32 NumEntries,
                                                    uint32 BrickCount)
{
	if (NumEntries == 0 || DstTable == nullptr || SrcDesc == nullptr || SrcOcc == nullptr ||
	    SrcChunkMask == nullptr || Table == nullptr)
	{
		return;
	}

	FVoxelBrickFlushBatchRecordCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickFlushBatchRecordCS::FParameters>();
	Params->NumTableEntries = NumEntries;
	Params->BrickCount = BrickCount;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	Params->FlushTableStride = TableStride;
	Params->InFlushTable = GraphBuilder.CreateSRV(Table);
	Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
	Params->InBrickOcc = GraphBuilder.CreateSRV(SrcOcc);
	Params->InChunkBrickMask = GraphBuilder.CreateSRV(SrcChunkMask);
	Params->OutChunkTable = GraphBuilder.CreateUAV(DstTable);

	TShaderMapRef<FVoxelBrickFlushBatchRecordCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	// One WORKGROUP per chunk (the allSolid reduce is per chunk), all in one
	// dispatch -- that is the entire difference from K record passes.
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickFlushBatchRecord(%u chunks)", NumEntries),
		Shader, Params, FIntVector(int32(NumEntries), 1, 1));
}

void VoxelGpuWorldGen::AddBrickFlushBatchClearPass(FRDGBuilder& GraphBuilder, FRDGBufferRef DstTable,
                                                   FRDGBufferRef SlotList, uint32 NumSlots)
{
	if (NumSlots == 0 || DstTable == nullptr || SlotList == nullptr)
	{
		return;
	}

	FVoxelBrickFlushBatchClearCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickFlushBatchClearCS::FParameters>();
	Params->NumTableEntries = NumSlots;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	Params->InFlushTable = GraphBuilder.CreateSRV(SlotList);
	Params->OutChunkTable = GraphBuilder.CreateUAV(DstTable);

	TShaderMapRef<FVoxelBrickFlushBatchClearCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickFlushBatchClear(%u slots)", NumSlots),
		Shader, Params,
		FIntVector(FMath::DivideAndRoundUp(NumSlots * uint32(FVoxelBrickPool::kChunkRecordDwords), 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickFlushVerifyPass(FRDGBuilder& GraphBuilder,
                                               const FBrickFlushVerifyBuffers& Buffers,
                                               const FBrickFlushVerifyArgs& Args)
{
	if (Buffers.PoolDesc == nullptr || Buffers.PoolOcc == nullptr || Buffers.PoolMat == nullptr ||
	    Buffers.PoolTable == nullptr || Buffers.SrcDesc == nullptr || Buffers.SrcOcc == nullptr ||
	    Buffers.SrcMat == nullptr || Buffers.SrcChunkMask == nullptr || Buffers.OutVerify == nullptr)
	{
		return;
	}

	FVoxelBrickFlushVerifyCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickFlushVerifyCS::FParameters>();
	Params->SrcFirst = Args.SrcBrickFirst;
	Params->SrcChunkIndex = Args.SrcChunkIndex;
	Params->BrickCount = Args.BrickCount;
	Params->ChunkSlot = Args.ChunkSlot;
	Params->BrickBase = Args.BrickBase;
	Params->RingLevel = Args.RingLevel;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	// Packed on the CPU through the one Pack the record writers share, so the
	// expected record here cannot lay bits out differently from the writers.
	Args.Shading.Pack(Params->ChunkClimatePacked, Params->ChunkSurfaceGradPacked,
	                  Params->ChunkSurfaceZRelBits);
	Params->OriginVoxel = FIntVector3(Args.OriginVoxel.X, Args.OriginVoxel.Y, Args.OriginVoxel.Z);
	Params->OccBase = Args.OccBase;
	Params->MatBase = Args.MatBase;
	Params->OccSrcFirst = Args.OccSrcFirst;
	Params->MatSrcFirst = Args.MatSrcFirst;
	Params->VerifyOccWords = Args.OccWords;
	Params->VerifyMatWords = Args.MatWords;
	Params->InBrickDesc = GraphBuilder.CreateSRV(Buffers.SrcDesc);
	Params->InBrickOcc = GraphBuilder.CreateSRV(Buffers.SrcOcc);
	Params->InWords = GraphBuilder.CreateSRV(Buffers.SrcMat);
	Params->InChunkBrickMask = GraphBuilder.CreateSRV(Buffers.SrcChunkMask);
	Params->InPoolDesc = GraphBuilder.CreateSRV(Buffers.PoolDesc);
	Params->InPoolOcc = GraphBuilder.CreateSRV(Buffers.PoolOcc);
	Params->InPoolMat = GraphBuilder.CreateSRV(Buffers.PoolMat);
	Params->InPoolTable = GraphBuilder.CreateSRV(Buffers.PoolTable);
	Params->OutVerify = GraphBuilder.CreateUAV(Buffers.OutVerify);

	TShaderMapRef<FVoxelBrickFlushVerifyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	// One workgroup, one chunk -- this is deliberately the CLASSIC per-chunk
	// shape, because its whole purpose is to be the independent arm of the
	// comparison. It runs on a SAMPLED group only, so the per-chunk cost the
	// batch removed does not quietly come back through its own gate.
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Voxel.BrickFlushVerify(slot %u)", Args.ChunkSlot),
		Shader, Params, FIntVector(1, 1, 1));
}

// --- P1: GPU-side pool allocation (voxel.GPU.PoolAlloc) ----------------------
//
// See VoxelBrickPoolAlloc.usf for the design. These functions only bind and
// dispatch; the layout is FVoxelBrickPool's and arrives complete in Layout.

namespace
{
	// One filler for the shared layout block, because three parameter structs
	// carry it and a field forgotten in one of them is a kernel addressing the
	// wrong dwords with no error anywhere.
	template <typename TParams>
	void FillBrickAllocLayout(TParams* Params, const VoxelGpuWorldGen::FBrickPoolAllocLayout& L)
	{
		Params->OccRegionFirst = L.OccRegionFirst;
		Params->OccRegionWords = L.OccRegionWords;
		Params->MatRegionFirst = L.MatRegionFirst;
		Params->MatRegionWords = L.MatRegionWords;
		Params->OccClassStep = L.OccClassStep;
		Params->OccClasses = L.OccClasses;
		Params->MatClassStep = L.MatClassStep;
		Params->MatClasses = L.MatClasses;
		Params->FreeStackCap = L.FreeStackCap;
		Params->OccTopsFirst = L.OccTopsFirst;
		Params->MatTopsFirst = L.MatTopsFirst;
		Params->OccStackFirst = L.OccStackFirst;
		Params->MatStackFirst = L.MatStackFirst;
		Params->OccBitmapFirst = L.OccBitmapFirst;
		Params->MatBitmapFirst = L.MatBitmapFirst;
		Params->OccHistFirst = L.OccHistFirst;
		Params->OccHistBucketWords = L.OccHistBucketWords;
		Params->OccHistBuckets = L.OccHistBuckets;
		Params->MatHistFirst = L.MatHistFirst;
		Params->MatHistBucketWords = L.MatHistBucketWords;
		Params->MatHistBuckets = L.MatHistBuckets;
	}
}

FRDGBufferRef VoxelGpuWorldGen::AddBrickPoolClaimPass(FRDGBuilder& GraphBuilder,
                                                      const FBrickPoolAllocBuffers& Buffers,
                                                      const FBrickPoolAllocLayout& Layout,
                                                      FRDGBufferRef BrickTotals, uint32 ChunkSlot,
                                                      uint32 OccWorstWords, uint32 MatWorstWords,
                                                      uint32 TotalsChunkIndexPlusOne,
                                                      uint32 TotalsNumChunks)
{
	if (!Buffers.IsValid() || BrickTotals == nullptr)
	{
		return nullptr;
	}

	FRDGBufferRef Claim = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 8), TEXT("Voxel.BrickPoolClaim"));

	FVoxelBrickPoolClaimCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickPoolClaimCS::FParameters>();
	FillBrickAllocLayout(Params, Layout);
	Params->ChunkSlot = ChunkSlot;
	Params->OccWorstWords = OccWorstWords;
	Params->MatWorstWords = MatWorstWords;
	Params->TotalsChunkIndexPlusOne = TotalsChunkIndexPlusOne;
	Params->TotalsNumChunks = TotalsNumChunks;
	Params->InBrickTotals = GraphBuilder.CreateSRV(BrickTotals);
	Params->AllocState = GraphBuilder.CreateUAV(Buffers.AllocState);
	Params->AllocBitmap = GraphBuilder.CreateUAV(Buffers.AllocBitmap);
	Params->AllocSide = GraphBuilder.CreateUAV(Buffers.AllocSide);
	Params->OutClaim = GraphBuilder.CreateUAV(Claim);

	TShaderMapRef<FVoxelBrickPoolClaimCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolClaim(slot %u)", ChunkSlot),
		Shader, Params, FIntVector(1, 1, 1));
	return Claim;
}

void VoxelGpuWorldGen::AddBrickPoolAllocWritePasses(FRDGBuilder& GraphBuilder,
                                                    const FBrickPoolAllocBuffers& Buffers,
                                                    FRDGBufferRef Claim,
                                                    FRDGBufferRef SrcOcc, FRDGBufferRef SrcMat,
                                                    FRDGBufferRef SrcDesc, FRDGBufferRef SrcChunkMask,
                                                    uint32 BrickCount, uint32 ChunkSlot, uint32 BrickBase,
                                                    uint32 RingLevel, const FIntVector& OriginVoxel,
                                                    const FVoxelBrickChunkShading& Shading,
                                                    uint32 OccWorstWords, uint32 MatWorstWords,
                                                    uint32 SrcDescBase, uint32 ChunkMaskBase)
{
	if (!Buffers.IsValid() || Claim == nullptr || SrcOcc == nullptr || SrcMat == nullptr ||
	    SrcDesc == nullptr || SrcChunkMask == nullptr || BrickCount == 0)
	{
		return;
	}

	// The two word copies, worst-case dispatched: the actual counts live on the
	// GPU (in the claim), so the excess threads read two dwords and exit. An
	// indirect dispatch here would be P3 built early against measurement #1
	// (pass and dispatch setup is NOT the current ceiling). A zero bound skips
	// the pass outright -- the CPU producer passes its EXACT counts here, and an
	// all-collapsed chunk owns no words in an arena (a zero-group dispatch is
	// not merely wasteful, it is invalid).
	if (OccWorstWords > 0)
	{
		FVoxelBrickPoolAllocWordCopyCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelBrickPoolAllocWordCopyCS::FParameters>();
		Params->MaxWords = OccWorstWords;
		Params->WordsClaimIndex = 0;
		Params->InClaim = GraphBuilder.CreateSRV(Claim);
		Params->InWords = GraphBuilder.CreateSRV(SrcOcc);
		Params->OutWords = GraphBuilder.CreateUAV(Buffers.PoolOcc);
		TShaderMapRef<FVoxelBrickPoolAllocWordCopyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolAllocOccCopy(slot %u)", ChunkSlot),
			Shader, Params, FIntVector(FMath::DivideAndRoundUp(OccWorstWords, 64u), 1, 1));
	}
	if (MatWorstWords > 0)
	{
		FVoxelBrickPoolAllocWordCopyCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelBrickPoolAllocWordCopyCS::FParameters>();
		Params->MaxWords = MatWorstWords;
		Params->WordsClaimIndex = 1;
		Params->InClaim = GraphBuilder.CreateSRV(Claim);
		Params->InWords = GraphBuilder.CreateSRV(SrcMat);
		Params->OutWords = GraphBuilder.CreateUAV(Buffers.PoolMat);
		TShaderMapRef<FVoxelBrickPoolAllocWordCopyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolAllocMatCopy(slot %u)", ChunkSlot),
			Shader, Params, FIntVector(FMath::DivideAndRoundUp(MatWorstWords, 64u), 1, 1));
	}
	{
		FVoxelBrickPoolAllocDescCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelBrickPoolAllocDescCS::FParameters>();
		Params->BrickCount = BrickCount;
		Params->BrickBase = BrickBase;
		Params->SrcDescBase = SrcDescBase;
		Params->InClaim = GraphBuilder.CreateSRV(Claim);
		Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
		Params->OutBrickDesc = GraphBuilder.CreateUAV(Buffers.PoolDesc);
		TShaderMapRef<FVoxelBrickPoolAllocDescCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolAllocDesc(slot %u)", ChunkSlot),
			Shader, Params, FIntVector(FMath::DivideAndRoundUp(BrickCount, 64u), 1, 1));
	}
	{
		FVoxelBrickPoolAllocRecordCS::FParameters* Params =
			GraphBuilder.AllocParameters<FVoxelBrickPoolAllocRecordCS::FParameters>();
		Params->BrickCount = BrickCount;
		Params->ChunkSlot = ChunkSlot;
		Params->BrickBase = BrickBase;
		Params->RingLevel = RingLevel;
		Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
		Shading.Pack(Params->ChunkClimatePacked, Params->ChunkSurfaceGradPacked,
		             Params->ChunkSurfaceZRelBits);
		Params->OriginVoxel = FIntVector3(OriginVoxel.X, OriginVoxel.Y, OriginVoxel.Z);
		Params->SrcDescBase = SrcDescBase;
		Params->ChunkMaskBase = ChunkMaskBase;
		Params->InClaim = GraphBuilder.CreateSRV(Claim);
		Params->InBrickDesc = GraphBuilder.CreateSRV(SrcDesc);
		Params->InBrickOcc = GraphBuilder.CreateSRV(SrcOcc);
		Params->InChunkBrickMask = GraphBuilder.CreateSRV(SrcChunkMask);
		Params->OutChunkTable = GraphBuilder.CreateUAV(Buffers.PoolTable);
		TShaderMapRef<FVoxelBrickPoolAllocRecordCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolAllocRecord(slot %u, L%u)", ChunkSlot, RingLevel),
			Shader, Params, FIntVector(1, 1, 1));
	}
}

void VoxelGpuWorldGen::AddBrickPoolFreePass(FRDGBuilder& GraphBuilder,
                                            const FBrickPoolAllocBuffers& Buffers,
                                            const FBrickPoolAllocLayout& Layout,
                                            FRDGBufferRef SlotList, uint32 NumSlots)
{
	if (!Buffers.IsValid() || SlotList == nullptr || NumSlots == 0)
	{
		return;
	}

	FVoxelBrickPoolFreeCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickPoolFreeCS::FParameters>();
	FillBrickAllocLayout(Params, Layout);
	Params->NumSlots = NumSlots;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	Params->InSlotList = GraphBuilder.CreateSRV(SlotList);
	Params->AllocState = GraphBuilder.CreateUAV(Buffers.AllocState);
	Params->AllocBitmap = GraphBuilder.CreateUAV(Buffers.AllocBitmap);
	Params->AllocSide = GraphBuilder.CreateUAV(Buffers.AllocSide);
	Params->OutChunkTable = GraphBuilder.CreateUAV(Buffers.PoolTable);

	TShaderMapRef<FVoxelBrickPoolFreeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolFree(%u slots)", NumSlots),
		Shader, Params, FIntVector(FMath::DivideAndRoundUp(NumSlots, 64u), 1, 1));
}

void VoxelGpuWorldGen::AddBrickPoolAllocVerifyPass(FRDGBuilder& GraphBuilder,
                                                   const FBrickPoolAllocBuffers& Buffers,
                                                   const FBrickPoolAllocLayout& Layout,
                                                   FRDGBufferRef Expect, uint32 NumEntries,
                                                   FRDGBufferRef OutVerify)
{
	if (!Buffers.IsValid() || Expect == nullptr || OutVerify == nullptr || NumEntries == 0)
	{
		return;
	}

	FVoxelBrickPoolAllocVerifyCS::FParameters* Params =
		GraphBuilder.AllocParameters<FVoxelBrickPoolAllocVerifyCS::FParameters>();
	FillBrickAllocLayout(Params, Layout);
	Params->NumEntries = NumEntries;
	Params->ChunkRecordDwords = uint32(FVoxelBrickPool::kChunkRecordDwords);
	Params->InExpect = GraphBuilder.CreateSRV(Expect);
	Params->InPoolTable = GraphBuilder.CreateSRV(Buffers.PoolTable);
	Params->InAllocSide = GraphBuilder.CreateSRV(Buffers.AllocSide);
	Params->InAllocBitmap = GraphBuilder.CreateSRV(Buffers.AllocBitmap);
	Params->OutVerify = GraphBuilder.CreateUAV(OutVerify);

	TShaderMapRef<FVoxelBrickPoolAllocVerifyCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("Voxel.BrickPoolAllocVerify(%u chunks)", NumEntries),
		Shader, Params, FIntVector(FMath::DivideAndRoundUp(NumEntries, 64u), 1, 1));
}

bool VoxelGpuWorldGen::IsSupportedOnCurrentRHI()
{
	return GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM6;
}

bool VoxelGpuWorldGen::SurfaceMipEnabled()
{
	// Parsed once and cached: the value must be identical for every brick the
	// process ever generates (CPU worker, GPU dispatch, verify reference),
	// because coarse bricks generated under two rules would sit side by side
	// in the same caches and rings. See the header comment for the full
	// contract and for why this is a command-line switch, not a cvar.
	static const bool bEnabled = []
	{
		int32 Value = 0;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelSurfaceMip="), Value);
		return Value != 0;
	}();
	return bEnabled;
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

	// Tier B.1 gate-only brick readbacks (bReadbackBricks). Sized from the
	// SAME FRegionGraphSizes the graph is built from, so the copies and the
	// buffers cannot disagree about a length. The occ/mat arrays are the
	// worst-case scratch sizes; only the totals-named prefix is live.
	const bool bWantBricks = Request.bBrickPack && Request.bReadbackBricks;
	const bool bWantStackTotals = bWantBricks && Request.bPerChunkBrickTotals;
	const uint32 BrickDescDwords = bWantBricks ? Sizes.NumBricks * 2u : 0u;
	const uint32 BrickOccDwords = bWantBricks ? Sizes.BrickOccWordsMax : 0u;
	const uint32 BrickMatDwords = bWantBricks ? Sizes.BrickMatWordsMax : 0u;
	const uint32 StackTotalDwords = bWantStackTotals ? (2u + 2u * Sizes.NumBrickChunks) : 0u;
	TArray<uint32> BrickDescOut;
	TArray<uint32> BrickOccOut;
	TArray<uint32> BrickMatOut;
	TArray<uint32> BrickTotalsOut;
	TArray<uint32> StackTotalsOut;

	ENQUEUE_RENDER_COMMAND(VoxelGpuRunRegion)(
		[&Request, &ColumnsOut, &CellsOut, &CountsOut, &OffsetsOut, &QuadsOut, &GpuTotalOut,
		 &BandOut, &bBandOut, &RenderError, NumColumns, NumCells, bMesh, MaskCount, QuadElements,
		 bWantBand, bWantBricks, bWantStackTotals, BrickDescDwords, BrickOccDwords, BrickMatDwords,
		 StackTotalDwords, &BrickDescOut, &BrickOccOut, &BrickMatOut, &BrickTotalsOut, &StackTotalsOut]
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
		// Tier B.1 gate-only. Declared unconditionally (cheap, they hold nothing
		// until enqueued) so the copy-out below can reference them either way.
		FRHIGPUBufferReadback BrickDescReadback(TEXT("Voxel.BrickDescReadback"));
		FRHIGPUBufferReadback BrickOccReadback(TEXT("Voxel.BrickOccReadback"));
		FRHIGPUBufferReadback BrickMatReadback(TEXT("Voxel.BrickMatReadback"));
		FRHIGPUBufferReadback BrickTotalsReadback(TEXT("Voxel.BrickTotalsReadback"));
		FRHIGPUBufferReadback StackTotalsReadback(TEXT("Voxel.BrickStackTotalsReadback"));

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
		// Tier B.1 gate-only: the brick chain's outputs. THE STREAMING PATH
		// NEVER TAKES THESE COPIES -- bReadbackBricks is a verification flag,
		// and the whole point of the batched path is that nothing but sizes
		// crosses PCIe. Guarded on the buffers existing so a refused chain
		// cannot enqueue a copy from null.
		if (bWantBricks && Graph.BrickDesc != nullptr && Graph.BrickOcc != nullptr &&
		    Graph.BrickMat != nullptr && Graph.BrickTotals != nullptr)
		{
			AddEnqueueCopyPass(GraphBuilder, &BrickDescReadback, Graph.BrickDesc,
			                   BrickDescDwords * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, &BrickOccReadback, Graph.BrickOcc,
			                   BrickOccDwords * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, &BrickMatReadback, Graph.BrickMat,
			                   BrickMatDwords * sizeof(uint32));
			AddEnqueueCopyPass(GraphBuilder, &BrickTotalsReadback, Graph.BrickTotals,
			                   2 * sizeof(uint32));
			if (bWantStackTotals && Graph.BrickStackTotals != nullptr)
			{
				AddEnqueueCopyPass(GraphBuilder, &StackTotalsReadback, Graph.BrickStackTotals,
				                   StackTotalDwords * sizeof(uint32));
			}
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

		if (bWantBricks)
		{
			BrickDescOut.SetNumUninitialized(int32(BrickDescDwords));
			CopyOut(BrickDescReadback, BrickDescOut.GetData(),
			        BrickDescDwords * sizeof(uint32), TEXT("BrickDesc"));
			BrickOccOut.SetNumUninitialized(int32(BrickOccDwords));
			CopyOut(BrickOccReadback, BrickOccOut.GetData(),
			        BrickOccDwords * sizeof(uint32), TEXT("BrickOcc"));
			BrickMatOut.SetNumUninitialized(int32(BrickMatDwords));
			CopyOut(BrickMatReadback, BrickMatOut.GetData(),
			        BrickMatDwords * sizeof(uint32), TEXT("BrickMat"));
			BrickTotalsOut.SetNumUninitialized(2);
			CopyOut(BrickTotalsReadback, BrickTotalsOut.GetData(),
			        2 * sizeof(uint32), TEXT("BrickTotals"));
			if (bWantStackTotals)
			{
				StackTotalsOut.SetNumUninitialized(int32(StackTotalDwords));
				CopyOut(StackTotalsReadback, StackTotalsOut.GetData(),
				        StackTotalDwords * sizeof(uint32), TEXT("BrickStackTotals"));
			}
		}

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

	// Tier B.1 gate-only brick readbacks; empty on every other path.
	Result.BrickDescRaw = MoveTemp(BrickDescOut);
	Result.BrickOccRaw = MoveTemp(BrickOccOut);
	Result.BrickMatRaw = MoveTemp(BrickMatOut);
	Result.BrickTotalsRaw = MoveTemp(BrickTotalsOut);
	Result.BrickStackTotalsRaw = MoveTemp(StackTotalsOut);

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
