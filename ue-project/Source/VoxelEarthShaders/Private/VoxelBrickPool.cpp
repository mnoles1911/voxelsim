#include "VoxelBrickPool.h"
#include "VoxelGpuMeshJobManager.h" // VoxelGpuBrickPackEnabled -- the master brick gate
#include "VoxelGpuWorldGenGraph.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h" // the pack-span clock; see VoxelBrickGetPackSpanSeconds
#include "Misc/CommandLine.h"  // the -ExecCmds startup window; see the note above the accessors
#include "Misc/Parse.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogVoxelBrickPool, Log, All);

// A NAMED namespace, not an anonymous one, and no using-directive. Unreal's
// unity build concatenates translation units, at which point two anonymous
// namespaces are ONE namespace -- and VoxelGpuWorldGen.cpp in this same module
// already has a file-visible kBricksPerChunk. tools/lint-unity-collisions.py
// fails the build on exactly that. Every use below is qualified for the same
// reason a using-directive would not fix it: the other file's name would still
// be visible at file scope and the lookup would become ambiguous rather than
// wrong.
namespace VoxelBrickPoolDetail
{
	// docs/brick-volume-format.md section 1 and section 6. Restated rather than
	// included because this module does not link voxel-core.
	constexpr uint32 kBricksPerChunk = 64;
	// vxc::kMarchChunkEdgeVoxels. Restated for this file's no-voxel-core reason;
	// it is also VoxelCoords::ChunkEdgeVoxels, and a render chunk being 32 voxels
	// on a side is what makes a brick chunk key and a render chunk key the same
	// key (VoxelGpuMeshJobManager's BrickKey derivation).
	constexpr int64 kChunkEdgeVoxels = 32;
	// The class restates this publicly for BindShaderParameters. Tied here so a
	// change to either is a compile error rather than a bound that is right in
	// one translation unit and wrong in the shader.
	static_assert(kBricksPerChunk == FVoxelBrickPool::kBricksPerChunk,
	              "the header and this file disagree about bricks per chunk");
	constexpr uint32 kBrickDescBytes = 8;
	constexpr uint32 kChunkRecordDwords = 16;   // 64 B -- see the header layout table
	constexpr uint32 kChunkRecordBytes = kChunkRecordDwords * 4;
	// The descriptor's offset field is 28 bits, and BrickDescPoolWriteMain
	// MASKS. An arena above this would produce a wrong world rather than an
	// error, so Init refuses it.
	constexpr uint32 kMaxArenaWords = 1u << 28;

	// --- capacity, and where these numbers come from -----------------------
	//
	// vxc_volumeprobe, full cascade at 10 cm / 4 km, real tiles, no sampling:
	// cells 128.5 MiB, occupancy 56.0, descriptors 23.1, palette 14.0.
	//
	// RAISED 2026-08-19, IN THE SAME CHANGE THAT MADE THE CPU PATH PACK, because
	// the two cannot be separated: at the old 5.2% coverage nothing was ever
	// pressured (allocFail 0, evictions 0, 6.5% of chunk slots used), and at full
	// coverage the old 65,536 chunks / 4.19M descriptor slots are 1.35x OVER on
	// both axes against the real target of 88,151 chunks. See
	// FVoxelBrickPoolConfig's comment for why the target is 88,151 -- every chunk
	// that gets a mesh job -- and not the 50,560 that produce geometry.
	//
	// 131,072 chunks = 8.39M descriptor slots = 64.0 MiB of descriptors + 4.0 MiB
	// of records, i.e. 1.49x the 88,151 target.
	//
	// THE PAYLOAD ARENAS DO NOT SCALE WITH THE CHUNK COUNT and that is the whole
	// reason they move less than it does: occupancy and material dwords are paid
	// only by MIXED bricks, and the 37,539 zero-quad chunks the target grew by
	// store COLLAPSED -- 64 descriptors and a record, zero payload. So these stay
	// derived from the census (56.0 MiB occupancy, 128.5 + 14.0 = 142.5 MiB
	// material) at ~1.5x, not from chunks x a per-chunk mean.
	//
	// The per-chunk mean is recorded here because it will be the tempting number
	// and it is the wrong basis: 2,153 B/chunk (512 desc + 776 occ + 833 mat + 32
	// record) was measured over the 4,368 chunks the GPU fork happened to reach
	// first, which are the nearest and earliest -- exactly the selection a
	// coverage bug biases. Sizing on the census is sizing on a full walk.
	//
	// THESE ARE COMMITTED AT INIT AND DO NOT MOVE. A VRAM tool sees the
	// capacity; FVoxelBrickPool::GetResidentBytes is what tracks the world.
	// Committed total at these defaults: 64.0 + 4.0 + 96 + 224 = 388.0 MiB
	// (was 314.0).
	int32 GVoxelBrickPoolChunks = 131072;
	FAutoConsoleVariableRef CVarVoxelBrickPoolChunks(
		TEXT("voxel.Brick.PoolChunks"),
		GVoxelBrickPoolChunks,
		TEXT("Resident-chunk capacity of the brick pool: one 32 B record and one 64-slot descriptor ")
		TEXT("block each. Read once, at Init. The 10 cm / 4 km cascade offers 88,151 chunks to the ")
		TEXT("mesh path, INCLUDING the 37,539 that mesh to zero quads and store collapsed."),
		ECVF_ReadOnly);

	int32 GVoxelBrickPoolOccMiB = 96;
	FAutoConsoleVariableRef CVarVoxelBrickPoolOccMiB(
		TEXT("voxel.Brick.PoolOccMiB"),
		GVoxelBrickPoolOccMiB,
		TEXT("Occupancy arena size in MiB (16 dwords per MIXED brick). Census: 56.0 MiB at 10 cm / 4 km."),
		ECVF_ReadOnly);

	int32 GVoxelBrickPoolMatMiB = 224;
	FAutoConsoleVariableRef CVarVoxelBrickPoolMatMiB(
		TEXT("voxel.Brick.PoolMatMiB"),
		GVoxelBrickPoolMatMiB,
		TEXT("Material arena size in MiB (16 B palette plus payload, per MIXED brick). Census: ")
		TEXT("128.5 MiB of cells plus 14.0 MiB of palette at 10 cm / 4 km."),
		ECVF_ReadOnly);

	// --- the CPU arm ------------------------------------------------------
	//
	// See VoxelBrickPackOnCpuEnabled's declaration for what this is for. NOT
	// ECVF_ReadOnly: it is meant to be flipped between legs.
	int32 GVoxelBrickPackOnCpu = 1;
	FAutoConsoleVariableRef CVarVoxelBrickPackOnCpu(
		TEXT("voxel.Brick.PackOnCpu"),
		GVoxelBrickPackOnCpu,
		TEXT("1 (default) = the CPU worker and the game-thread edit re-mesh ALSO pack their chunk ")
		TEXT("into the brick volume, through vxc::packChunkBricksCanonical. 0 = they do not, which ")
		TEXT("is the pre-2026-08-19 behaviour and the control arm for what the CPU pack costs. ")
		TEXT("Subordinate to voxel.GPU.BrickPack (nothing packs at all when that is 0) and to ")
		TEXT("voxel.GPU.BrickPackResident (packed and discarded when that is 0)."),
		ECVF_Default);

	// --- PHASE 5: the terrain quad path, retired ---------------------------
	//
	// THE REAL SWITCH, and it is OFF BY DEFAULT and must stay that way until the
	// marcher can skip empty space, cross rings and reach past 51.2 m. With it on
	// today the world renders nearly empty beyond the near field, so this is a
	// measurement and readiness switch, not a product one.
	//
	// WHAT IT RETIRES: terrain quad PRODUCTION, on both arms.
	//   * the CPU worker stops meshing (it already could -- see SuppressQuadMesh)
	//   * the GPU fork dispatches a BRICK-ONLY job: bMeshChain false, no quad
	//     buffer, no 4-byte total readback, no pool write
	// It does NOT retire UVoxelGpuPoolComponent, FVoxelQuadVertexFactory,
	// VoxelQuadDecode.ush, FVoxelChunkQuad or the geometry suballocator: WATER
	// runs its own independent instances of all of them and must keep working.
	// See docs/phase5-quad-retirement-plan.md section 2.
	//
	// WHY IT IS A SEPARATE CVAR FROM voxel.Brick.SuppressQuadMesh RATHER THAN A
	// RENAME. That one is worker-only and has published measurements hanging off
	// it (0.608 ms/chunk scattered, and the Phase 5 terms 0.969 -> 0.389).
	// Renaming it would orphan those numbers. It stays exactly what it was; this
	// one subsumes it, and VoxelBrickSuppressQuadMeshEnabled ORs the two so the
	// worker honours either.
	int32 GVoxelTerrainRetireQuads = 1;   // PROTOTYPE DEFAULT: terrain is marched now
	FAutoConsoleVariableRef CVarVoxelTerrainRetireQuads(
		TEXT("voxel.Terrain.RetireQuads"),
		GVoxelTerrainRetireQuads,
		TEXT("MEASUREMENT AND READINESS ONLY, default 0 -- 1 retires terrain quad PRODUCTION on both ")
		TEXT("the CPU worker and the GPU fork, leaving the brick volume as the only terrain producer. ")
		TEXT("The world then renders nearly empty beyond the near field until the marcher can skip ")
		TEXT("empty space and cross rings, so this is not a product switch. Water is unaffected: it ")
		TEXT("runs its own quad pool. Requires BOTH voxel.GPU.BrickPack 1 and voxel.Brick.PackOnCpu 1 ")
		TEXT("-- with either off some producer would stop meshing without ever packing, so that ")
		TEXT("combination is refused rather than obeyed."),
		ECVF_Default);

	// --- the transitional-cost experiment ---------------------------------
	//
	// MEASUREMENT ONLY, AND IT MAKES THE TERRAIN DISAPPEAR. Never ship it on.
	//
	// The question it answers is the one the +36% cold fill raises and that no
	// argument can settle. Today the CPU worker runs BOTH producers for every
	// chunk: it meshes quads AND packs bricks. Phase 5 of the ray-marching plan
	// retires terrain quad meshing entirely, at which point the worker only
	// packs -- so the +36% is a transitional addition that becomes a REDUCTION.
	// That is the plan's own design and it is a PREDICTION, and this project's
	// standing rule is that no saving goes on the plan until an experiment
	// removes the work and measures the frame.
	//
	// At 1 the CPU worker SKIPS MeshChunkBricks and only packs. Every chunk then
	// meshes to zero quads, so the world renders empty -- that is expected, and
	// it is why this is a stopwatch arm and not a screenshot arm. What it
	// measures is cold fill against the same spawn and params: the Phase 5 shape
	// of the worker, today, on this binary.
	//
	// WORKER PATH ONLY. The game-thread edit re-mesh still meshes (suppressing
	// it would break editing for no timing gain) and the GPU fork still meshes
	// its ~8% (leaving it alone keeps the arm one variable). Both are stated so
	// the number is read as "the CPU worker stopped meshing", not "nothing
	// meshed".
	int32 GVoxelBrickSuppressQuadMesh = 0;
	FAutoConsoleVariableRef CVarVoxelBrickSuppressQuadMesh(
		TEXT("voxel.Brick.SuppressQuadMesh"),
		GVoxelBrickSuppressQuadMesh,
		TEXT("MEASUREMENT ONLY -- 1 makes the CPU mesh worker skip quad meshing and pack bricks only, ")
		TEXT("which is the Phase 5 shape of the worker and renders the world EMPTY. Exists to measure ")
		TEXT("whether the brick packer's cost is real or merely early, instead of arguing it. Requires ")
		TEXT("voxel.Brick.PackOnCpu 1 and voxel.GPU.BrickPack 1; ignored otherwise. NEVER SHIP AT 1."),
		ECVF_Default);

	// See VoxelBrickPackReuseMesherVoxelsEnabled's declaration. A CONTROL, not a
	// safety valve: it exists so the 4.56x this optimisation is worth stays
	// re-measurable on a later binary instead of becoming folklore.
	int32 GVoxelBrickPackReuseMesherVoxels = 1;
	FAutoConsoleVariableRef CVarVoxelBrickPackReuseMesherVoxels(
		TEXT("voxel.Brick.PackReuseMesherVoxels"),
		GVoxelBrickPackReuseMesherVoxels,
		TEXT("1 (default) = the CPU brick packer reads the voxels the mesher already materialised ")
		TEXT("(FDenseChunkSink) instead of sampling the world a second time. 0 = the pre-2026-08-19 ")
		TEXT("form, measured at 0.743 ms/chunk against 0.163 -- kept as the control arm for that ")
		TEXT("number. No effect under voxel.Brick.SuppressQuadMesh, where there is no mesher to reuse."),
		ECVF_Default);

	// Cost of the CPU pack, accumulated across worker threads. Microseconds
	// rather than a double, because a lock-free double add is not a thing and a
	// per-chunk mutex on the streaming path would be measuring the instrument.
	std::atomic<int64> GCpuPackCount{ 0 };
	std::atomic<int64> GCpuPackFromDense{ 0 };
	std::atomic<int64> GCpuPackMicros{ 0 };
	// The other two terms of the Phase 5 sum. See the header.
	std::atomic<int64> GCpuFillCount{ 0 };
	std::atomic<int64> GCpuFillMicros{ 0 };
	std::atomic<int64> GCpuMeshCount{ 0 };
	std::atomic<int64> GCpuMeshMicros{ 0 };
	// Wall clock of the first and most recent pack, for the packs-per-second
	// metric that survives the suppression arm. 0 means nothing has packed.
	std::atomic<int64> GFirstPackMicros{ 0 };
	std::atomic<int64> GLastPackMicros{ 0 };

	// How far the eviction focus may move, in level-0 voxels, before the
	// distance-sorted eviction order is considered stale. 256 level-0 voxels is
	// 8 chunks / 25.6 m: at a 20 m/s flight that is about one rebuild per second,
	// against a sort the pool only pays when it is evicting at all.
	constexpr int64 kEvictionFocusRebuildVoxels = 256;
	constexpr int64 kEvictionFocusRebuildVoxelsSq =
		kEvictionFocusRebuildVoxels * kEvictionFocusRebuildVoxels;

	FBufferRHIRef CreateArenaBuffer(FRHICommandListImmediate& RHICmdList, const TCHAR* Name,
	                                uint32 Stride, uint32 NumElements)
	{
		// ZERO-INITIALISED. A never-written descriptor slot then reads as kind 0
		// -- uniform AIR, no payload -- which is the one value a marcher can
		// consume safely, and a never-written record reads as anySolid clear.
		// Garbage here would be indistinguishable from a real chunk.
		const FRHIBufferCreateDesc Desc =
			FRHIBufferCreateDesc::CreateStructured(Name, uint64(NumElements) * Stride, Stride)
			.AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource
			          | EBufferUsageFlags::UnorderedAccess)
			.DetermineInitialState()
			.SetInitActionZeroData();
		return RHICmdList.CreateBuffer(Desc);
	}
}

// ---------------------------------------------------------------------------
// FVoxelGpuBrickPayload
// ---------------------------------------------------------------------------

FVoxelGpuBrickPayload::FVoxelGpuBrickPayload() = default;

FVoxelGpuBrickPayload::~FVoxelGpuBrickPayload()
{
	// Drops the references ON THE RENDER THREAD, for FVoxelGpuQuadPayload's
	// reason: these are the last references to RHI resources that render
	// commands may still be a frame or two behind on, and releasing them
	// wherever the payload happens to die fails as a crash on exit rather than
	// as anything a compiler catches.
	if (!Desc.IsValid() && !Occ.IsValid() && !Mat.IsValid() && !ChunkMask.IsValid())
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(VoxelGpuBrickPayloadRelease)(
		[D = MoveTemp(Desc), O = MoveTemp(Occ), M = MoveTemp(Mat),
		 C = MoveTemp(ChunkMask)](FRHICommandListImmediate&) mutable
	{
		D.SafeRelease();
		O.SafeRelease();
		M.SafeRelease();
		C.SafeRelease();
	});
}

// ---------------------------------------------------------------------------
// FVoxelBrickCpuPack, and the CPU arm's switches
// ---------------------------------------------------------------------------

uint64 FVoxelBrickCpuPack::ResidentBytes() const
{
	return uint64(VoxelBrickPoolDetail::kBricksPerChunk) * VoxelBrickPoolDetail::kBrickDescBytes
	     + uint64(Occ.Num()) * 4
	     + uint64(Mat.Num()) * 4
	     + VoxelBrickPoolDetail::kChunkRecordBytes;
}

// --- WHY THESE THREE READ THE COMMAND LINE AND NOT ONLY A CVAR --------------
//
// -ExecCmds LANDS AFTER STREAMING HAS BEGUN. This project documents that in at
// least three places (tools/voxel-capture.ps1:114, AdmissionBandSkipMode, and
// GpuMeshInFlight, all of which took command-line switches for exactly this
// reason), and the leg harness passes every cvar through -ExecCmds
// (voxel-run-flight-leg.ps1:163).
//
// For a switch that changes what a PRODUCER emits, that window is not cosmetic.
// Chunks meshed before the flip allocate quad-pool ranges normally and nothing
// frees them afterwards, so the pool keeps a fixed residue for the whole run --
// which is what `terrainPoolUsedQuads=74583`, byte-identical across two runs of
// a 330 s leg, looks like.
//
// ALL THREE, not just the retirement switch, because retirement is ANDed with
// the two pack gates: overriding it alone would leave it inert until the other
// two landed through -ExecCmds anyway, i.e. the same window with more moving
// parts. -1 means "not given"; the cvar then wins, so a run that passes nothing
// behaves exactly as before.
bool VoxelBrickPackOnCpuEnabled()
{
	static const int32 PackOnCpuCmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelBrickPackOnCpu="), Value);
		return Value;
	}();
	return PackOnCpuCmdLine >= 0
		? PackOnCpuCmdLine != 0
		: VoxelBrickPoolDetail::GVoxelBrickPackOnCpu != 0;
}

bool VoxelTerrainQuadsRetired()
{
	// REFUSED, not obeyed, unless BOTH producers pack. Retirement is only
	// coherent when everything that produces a chunk also packs it, and the two
	// arms are gated separately:
	//
	//   the GPU fork packs iff voxel.GPU.BrickPack
	//   the CPU worker packs iff voxel.GPU.BrickPack AND voxel.Brick.PackOnCpu
	//
	// Requiring only the master gate looked right and was not. With PackOnCpu 0
	// the worker -- which carries ~92% of streaming traffic -- would have stopped
	// meshing while never packing, so nine chunks in ten would have produced
	// NOTHING AT ALL. The world would be empty and every switch would read as
	// intended. Cheaper to refuse the combination than to diagnose it.
	static const int32 RetireQuadsCmdLine = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelRetireQuads="), Value);
		return Value;
	}();
	const bool bRetire = RetireQuadsCmdLine >= 0
		? RetireQuadsCmdLine != 0
		: VoxelBrickPoolDetail::GVoxelTerrainRetireQuads != 0;
	return bRetire
	    && VoxelBrickPackOnCpuEnabled()
	    && VoxelGpuBrickPackEnabled();
}

bool VoxelBrickSuppressQuadMeshEnabled()
{
	// The worker honours EITHER switch: the measurement arm, or the real
	// retirement. AND-ed with the pack gate here rather than at the call site, so
	// the arm cannot be left in the one state that is pure damage: quads
	// suppressed and bricks not packed, i.e. a worker that produces nothing.
	if (VoxelTerrainQuadsRetired())
	{
		return true;
	}
	// Accessor, not the raw cvar, for VoxelGpuBrickPackEnabled's reason at its
	// call site in the job manager: a command-line override is invisible to a
	// direct read, and half the reads honouring it is worse than none doing so.
	return VoxelBrickPoolDetail::GVoxelBrickSuppressQuadMesh != 0
	    && VoxelBrickPackOnCpuEnabled();
}

bool VoxelBrickPackReuseMesherVoxelsEnabled()
{
	return VoxelBrickPoolDetail::GVoxelBrickPackReuseMesherVoxels != 0;
}

void VoxelBrickNoteCpuFill(double FillMilliseconds)
{
	VoxelBrickPoolDetail::GCpuFillCount.fetch_add(1, std::memory_order_relaxed);
	VoxelBrickPoolDetail::GCpuFillMicros.fetch_add(
		int64(FillMilliseconds * 1000.0 + 0.5), std::memory_order_relaxed);
}

int64 VoxelBrickGetCpuFillCount()
{
	return VoxelBrickPoolDetail::GCpuFillCount.load(std::memory_order_relaxed);
}

double VoxelBrickGetCpuFillMs()
{
	return double(VoxelBrickPoolDetail::GCpuFillMicros.load(std::memory_order_relaxed)) / 1000.0;
}

void VoxelBrickNoteCpuMesh(double MeshMilliseconds)
{
	VoxelBrickPoolDetail::GCpuMeshCount.fetch_add(1, std::memory_order_relaxed);
	VoxelBrickPoolDetail::GCpuMeshMicros.fetch_add(
		int64(MeshMilliseconds * 1000.0 + 0.5), std::memory_order_relaxed);
}

int64 VoxelBrickGetCpuMeshCount()
{
	return VoxelBrickPoolDetail::GCpuMeshCount.load(std::memory_order_relaxed);
}

double VoxelBrickGetCpuMeshMs()
{
	return double(VoxelBrickPoolDetail::GCpuMeshMicros.load(std::memory_order_relaxed)) / 1000.0;
}

double VoxelBrickGetPackSpanSeconds()
{
	const int64 First = VoxelBrickPoolDetail::GFirstPackMicros.load(std::memory_order_relaxed);
	const int64 Last = VoxelBrickPoolDetail::GLastPackMicros.load(std::memory_order_relaxed);
	return (First > 0 && Last > First) ? double(Last - First) / 1000000.0 : 0.0;
}

void VoxelBrickNoteCpuPack(double PackMilliseconds, bool bFromDense)
{
	// Stamped at pack COMPLETION, on whatever worker ran it. The first stamp is
	// a compare-exchange from zero so the earliest of however many threads race
	// wins; the last is a plain store, and the raciness there is bounded by how
	// far apart two concurrent packs can finish, i.e. microseconds against a
	// span measured in tens of seconds.
	const int64 NowMicros = int64(FPlatformTime::Seconds() * 1000000.0);
	int64 Unset = 0;
	VoxelBrickPoolDetail::GFirstPackMicros.compare_exchange_strong(
		Unset, NowMicros, std::memory_order_relaxed, std::memory_order_relaxed);
	VoxelBrickPoolDetail::GLastPackMicros.store(NowMicros, std::memory_order_relaxed);

	VoxelBrickPoolDetail::GCpuPackCount.fetch_add(1, std::memory_order_relaxed);
	if (bFromDense)
	{
		VoxelBrickPoolDetail::GCpuPackFromDense.fetch_add(1, std::memory_order_relaxed);
	}
	VoxelBrickPoolDetail::GCpuPackMicros.fetch_add(
		int64(PackMilliseconds * 1000.0 + 0.5), std::memory_order_relaxed);
}

int64 VoxelBrickGetCpuPackCount()
{
	return VoxelBrickPoolDetail::GCpuPackCount.load(std::memory_order_relaxed);
}

int64 VoxelBrickGetCpuPackFromDenseCount()
{
	return VoxelBrickPoolDetail::GCpuPackFromDense.load(std::memory_order_relaxed);
}

double VoxelBrickGetCpuPackMs()
{
	return double(VoxelBrickPoolDetail::GCpuPackMicros.load(std::memory_order_relaxed)) / 1000.0;
}

// ---------------------------------------------------------------------------
// FVoxelBrickPoolBuffers
// ---------------------------------------------------------------------------

FVoxelBrickPoolBuffers::FVoxelBrickPoolBuffers() = default;
FVoxelBrickPoolBuffers::~FVoxelBrickPoolBuffers() = default;

uint64 FVoxelBrickPoolBuffers::GetCapacityBytes() const
{
	return uint64(DescSlots) * VoxelBrickPoolDetail::kBrickDescBytes
	     + uint64(OccWords) * 4
	     + uint64(MatWords) * 4
	     + uint64(ChunkSlots) * VoxelBrickPoolDetail::kChunkRecordBytes;
}

// ---------------------------------------------------------------------------
// FVoxelBrickPool
// ---------------------------------------------------------------------------

FVoxelBrickPool::FVoxelBrickPool() = default;
FVoxelBrickPool::~FVoxelBrickPool() = default;

void FVoxelBrickPool::Init(const FVoxelBrickPoolConfig& InConfig)
{
	if (bInitialised)
	{
		if (InConfig.ChunkCapacity != Config.ChunkCapacity ||
		    InConfig.OccWordCapacity != Config.OccWordCapacity ||
		    InConfig.MatWordCapacity != Config.MatWordCapacity)
		{
			// Resizing would destroy the buffers every resident chunk lives in
			// while the CPU-side allocations still claim them -- the quad pool's
			// D4-R1 failure, which comes back as terrain that reports as loaded
			// and is not there. Refused rather than papered over.
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("FVoxelBrickPool::Init called a second time with a DIFFERENT configuration ")
			       TEXT("(chunks %u->%u, occ %u->%u, mat %u->%u dwords). Ignored: every resident chunk ")
			       TEXT("would be stranded in a buffer nothing re-produces."),
			       Config.ChunkCapacity, InConfig.ChunkCapacity,
			       Config.OccWordCapacity, InConfig.OccWordCapacity,
			       Config.MatWordCapacity, InConfig.MatWordCapacity);
		}
		return;
	}

	Config = InConfig;
	if (Config.ChunkCapacity == 0)
	{
		Config.ChunkCapacity = uint32(FMath::Max(1, VoxelBrickPoolDetail::GVoxelBrickPoolChunks));
	}
	if (Config.OccWordCapacity == 0)
	{
		Config.OccWordCapacity = uint32(FMath::Max(1, VoxelBrickPoolDetail::GVoxelBrickPoolOccMiB)) * (1024u * 1024u / 4u);
	}
	if (Config.MatWordCapacity == 0)
	{
		Config.MatWordCapacity = uint32(FMath::Max(1, VoxelBrickPoolDetail::GVoxelBrickPoolMatMiB)) * (1024u * 1024u / 4u);
	}

	// The 28-bit offset field. BrickDescPoolWriteMain masks, so an arena past
	// this hands the marcher an address inside somebody else's payload and
	// nothing anywhere reports an error.
	if (Config.OccWordCapacity > VoxelBrickPoolDetail::kMaxArenaWords || Config.MatWordCapacity > VoxelBrickPoolDetail::kMaxArenaWords)
	{
		UE_LOG(LogVoxelBrickPool, Error,
		       TEXT("Brick arena capacity (occ %u, mat %u dwords) exceeds the %u a descriptor's 28-bit ")
		       TEXT("offset field can name. Clamped -- above it the offsets wrap and the world is wrong ")
		       TEXT("with no error anywhere."),
		       Config.OccWordCapacity, Config.MatWordCapacity, VoxelBrickPoolDetail::kMaxArenaWords);
		Config.OccWordCapacity = FMath::Min(Config.OccWordCapacity, VoxelBrickPoolDetail::kMaxArenaWords);
		Config.MatWordCapacity = FMath::Min(Config.MatWordCapacity, VoxelBrickPoolDetail::kMaxArenaWords);
	}

	const uint32 DescSlots = Config.ChunkCapacity * VoxelBrickPoolDetail::kBricksPerChunk;
	DescArena.Init(DescSlots);
	OccArena.Init(Config.OccWordCapacity);
	MatArena.Init(Config.MatWordCapacity);
	bInitialised = true;

	// THE HOLDER IS CREATED HERE, NOT LAZILY IN Flush, and the reason is a data
	// race rather than tidiness. Register runs on the RENDER thread and reads
	// this pointer; Flush runs on the GAME thread and used to be the only thing
	// that created it. A TSharedPtr assignment is not atomic, so a marcher
	// registering on the frame the pool first flushed could read a half-formed
	// pointer. Creating it once, before anything can look, removes the window
	// entirely -- the holder is pure CPU state and the RHI buffers inside it are
	// still created by the first flush, on the render thread, guarded by
	// Buffers->IsValid().
	GetOrCreateBuffers();

	UE_LOG(LogVoxelBrickPool, Log,
	       TEXT("FVoxelBrickPool: %u chunks (%u descriptor slots, %.1f MiB), occupancy %.1f MiB, ")
	       TEXT("materials %.1f MiB, records %.1f MiB — %.1f MiB committed. Census at 10 cm / 4 km: ")
	       TEXT("occupancy 56.0, cells 128.5, palette 14.0, descriptors 23.1 MiB."),
	       Config.ChunkCapacity, DescSlots, double(DescSlots) * VoxelBrickPoolDetail::kBrickDescBytes / (1024.0 * 1024.0),
	       double(Config.OccWordCapacity) * 4 / (1024.0 * 1024.0),
	       double(Config.MatWordCapacity) * 4 / (1024.0 * 1024.0),
	       double(Config.ChunkCapacity) * VoxelBrickPoolDetail::kChunkRecordBytes / (1024.0 * 1024.0),
	       double(uint64(DescSlots) * VoxelBrickPoolDetail::kBrickDescBytes
	              + uint64(Config.OccWordCapacity) * 4
	              + uint64(Config.MatWordCapacity) * 4
	              + uint64(Config.ChunkCapacity) * VoxelBrickPoolDetail::kChunkRecordBytes) / (1024.0 * 1024.0));
}


// ---------------------------------------------------------------------------
// P3: the marcher's seam
// ---------------------------------------------------------------------------

void FVoxelBrickPool::BuildChunkRecord(const FIntVector& OriginVoxel, uint32 RingLevel,
                                       bool bAnySolid, bool bAllSolid, uint32 BrickBase,
                                       uint64 BrickSolid,
                                       const FVoxelBrickChunkShading& Shading,
                                       uint32 OutRecord[kChunkRecordDwords])
{
	static_assert(kChunkRecordDwords * 4 == 64, "format section 6: the record is 64 bytes");
	OutRecord[0] = uint32(OriginVoxel.X);
	OutRecord[1] = uint32(OriginVoxel.Y);
	OutRecord[2] = uint32(OriginVoxel.Z);
	// THE RING LEVEL IS MASKED, NOT CLAMPED, exactly as the kernel masks it. A
	// level that does not fit four bits is a caller bug and the pool refuses to
	// invent a different one: masking reproduces the GPU byte for byte, and
	// GetLevelCensus reports any such key as out of range so it is visible.
	OutRecord[3] = (RingLevel & 0xfu)
	             | (bAnySolid ? (1u << 4) : 0u)
	             | (bAllSolid ? (1u << 5) : 0u);
	OutRecord[4] = BrickBase;
	OutRecord[5] = uint32(BrickSolid & 0xffffffffull);
	OutRecord[6] = uint32((BrickSolid >> 32) & 0xffffffffull);
	// STEP 2: the per-chunk shading terms. Packed through FVoxelBrickChunkShading
	// ::Pack, which is the ONE place the bit layout lives -- the GPU kernel is
	// handed these same three already-packed dwords, so there is nothing left in
	// HLSL that can diverge from this function.
	Shading.Pack(OutRecord[7], OutRecord[8], OutRecord[9]);

	// The reserved tail, written rather than left alone: a slot being reused
	// would otherwise keep the previous tenant's values in fields a later format
	// version may define. Written as a LOOP bounded by the constant so that
	// adding a field above cannot silently leave a later dword stale.
	for (int32 D = 10; D < kChunkRecordDwords; ++D)
	{
		OutRecord[D] = 0u;
	}
}

FVoxelBrickPool::FRDGRefs FVoxelBrickPool::Register(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());

	FRDGRefs Refs;
	// Buffers->IsValid() is the render thread's own flag: it is set by the first
	// Flush, in the render command, after CreateBuffer actually returned. Reading
	// the holder pointer is safe because Init creates it up front -- see
	// GetOrCreateBuffers' call in Init, which exists for exactly this reason.
	if (!Buffers.IsValid() || !Buffers->IsValid())
	{
		return Refs;
	}

	Refs.Desc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
	Refs.Occ = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
	Refs.Mat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
	Refs.ChunkTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled,
	                                                      TEXT("VoxelBrickPool.ChunkTable"));
	return Refs;
}

bool FVoxelBrickPool::CreateSRVs(FRDGBuilder& GraphBuilder, FRDGBufferSRVRef& OutDesc,
                                 FRDGBufferSRVRef& OutOcc, FRDGBufferSRVRef& OutMat,
                                 FRDGBufferSRVRef& OutTable)
{
	const FRDGRefs Refs = Register(GraphBuilder);
	if (!Refs.IsValid())
	{
		return false;
	}

	// STRUCTURED, not typed, and that is the difference from the fluid volume's
	// Buffer<uint>. These arenas are structured buffers with real strides -- 8 B
	// for a descriptor, 4 B for the two payload arenas and the record table --
	// and VoxelBrickPoolWrite.usf already declares them that way. Creating a
	// typed SRV over a structured buffer is the kind of mismatch that binds and
	// then reads plausible nonsense.
	OutDesc = GraphBuilder.CreateSRV(Refs.Desc);
	OutOcc = GraphBuilder.CreateSRV(Refs.Occ);
	OutMat = GraphBuilder.CreateSRV(Refs.Mat);
	OutTable = GraphBuilder.CreateSRV(Refs.ChunkTable);
	return true;
}

void FVoxelBrickPool::SnapshotResidentIndex(TArray<FVoxelBrickIndexEntry>& Out) const
{
	Out.Reset();
	Out.Reserve(Resident.Num());
	for (const TPair<FVoxelBrickChunkKey, FResidentChunk>& Pair : Resident)
	{
		FVoxelBrickIndexEntry Entry;
		Entry.Key = Pair.Key;
		Entry.ChunkSlot = Pair.Value.ChunkSlot;
		Out.Add(Entry);
	}
}

void FVoxelBrickPool::SetIndexSink(FVoxelBrickIndexSink InSink, TArray<FVoxelBrickIndexEntry>& OutSnapshot)
{
	// Snapshot and registration in one call, under the game thread, with no
	// Flush able to run between them -- see the declaration for why the two-call
	// form is a window and not a style preference.
	IndexSink = MoveTemp(InSink);
	SnapshotResidentIndex(OutSnapshot);
}

void FVoxelBrickPool::GetLevelCensus(FLevelCensus& Out) const
{
	Out = FLevelCensus{};
	for (const TPair<FVoxelBrickChunkKey, FResidentChunk>& Pair : Resident)
	{
		const int32 Level = Pair.Key.Level;
		if (Level < 0 || Level >= kLevelBuckets)
		{
			// Counted apart rather than clamped. A key outside what
			// LevelAndFlags [0:3] can name is a defect -- the record would carry
			// a level that is not this chunk's -- and clamping it into bucket 0
			// would hide the one thing this walk could have caught.
			++Out.OutOfRangeChunks;
			continue;
		}
		++Out.Chunks[Level];
		// The same accounting GetResidentBytes uses, per chunk: 64 descriptor
		// slots and one record always, plus whatever the mixed bricks needed.
		Out.ResidentBytes[Level] +=
			uint64(VoxelBrickPoolDetail::kBricksPerChunk) * VoxelBrickPoolDetail::kBrickDescBytes
			+ uint64(Pair.Value.OccWords) * 4
			+ uint64(Pair.Value.MatWords) * 4
			+ VoxelBrickPoolDetail::kChunkRecordBytes;
	}
}

FVoxelBrickPoolBuffersRef FVoxelBrickPool::GetOrCreateBuffers()
{
	if (!Buffers.IsValid())
	{
		Buffers = MakeShared<FVoxelBrickPoolBuffers, ESPMode::ThreadSafe>();
		Buffers->DescSlots = Config.ChunkCapacity * VoxelBrickPoolDetail::kBricksPerChunk;
		Buffers->OccWords = Config.OccWordCapacity;
		Buffers->MatWords = Config.MatWordCapacity;
		Buffers->ChunkSlots = Config.ChunkCapacity;
	}
	return Buffers;
}

int32 FVoxelBrickPool::FindChunkSlot(const FVoxelBrickChunkKey& Key) const
{
	if (const FResidentChunk* Found = Resident.Find(Key))
	{
		return int32(Found->ChunkSlot);
	}
	return INDEX_NONE;
}

bool FVoxelBrickPool::DebugGetResidentChunk(const FVoxelBrickChunkKey& Key, FResidentChunk& Out) const
{
	if (const FResidentChunk* Found = Resident.Find(Key))
	{
		Out = *Found;
		return true;
	}
	return false;
}

void FVoxelBrickPool::FreeResident(const FResidentChunk& Chunk)
{
	FVoxelGpuPoolAllocation Alloc;
	Alloc.Offset = Chunk.BrickBase;
	Alloc.NumQuads = VoxelBrickPoolDetail::kBricksPerChunk;
	DescArena.Free(Alloc);

	if (Chunk.OccWords > 0)
	{
		Alloc.Offset = Chunk.OccBase;
		Alloc.NumQuads = Chunk.OccWords;
		OccArena.Free(Alloc);
	}
	if (Chunk.MatWords > 0)
	{
		Alloc.Offset = Chunk.MatBase;
		Alloc.NumQuads = Chunk.MatWords;
		MatArena.Free(Alloc);
	}
}

void FVoxelBrickPool::SetEvictionFocusVoxel0(int64 X, int64 Y, int64 Z)
{
	EvictionFocus[0] = X;
	EvictionFocus[1] = Y;
	EvictionFocus[2] = Z;
	bHasEvictionFocus = true;
	// The order is NOT rebuilt here. This is called every streaming tick and the
	// sort is only worth paying for when something is actually about to be
	// evicted -- EvictOne checks the staleness before it takes the front.
}

int64 FVoxelBrickPool::FocusDistSqOf(const FVoxelBrickChunkKey& Key) const
{
	// A level-L chunk's centre in LEVEL-0 voxels. The origin is Key * 32 in its
	// own level's units by construction -- the same arithmetic
	// FVoxelMarchChunk::OriginVoxel carries -- and a level-L voxel is 1<<L
	// level-0 voxels, so one shift puts every ring in one comparable frame. int64
	// throughout: at 10 cm the 4 km cascade is ~40,000 level-0 voxels across and
	// the squares overflow int32 several times over.
	// EVERYTHING IS RANKED IN COVER CELLS -- HALF level-0 voxels -- and not in
	// level-0 voxels, because one member of the cascade is now FINER than level 0
	// and integer arithmetic cannot express half a voxel.
	//
	// THE COVER LEVEL IS NOT 1 << 7. That is the trap this function had waiting:
	// the shift is the ring cascade's rule (a level-L voxel is 2^L level-0
	// voxels) and cover is the one key that does not obey it. Left alone, a cover
	// chunk would rank as 128x further from the camera than it is, every cover
	// chunk in the world would sort to the front of the eviction order, and cover
	// would be evicted the instant the pool came under any pressure at all --
	// while every counter read healthy and the only symptom was cover missing
	// from the near field. Ranking in cover cells makes the two rules one rule.
	//
	// Range: a 4 km cascade is ~40,000 level-0 voxels, i.e. ~80,000 cover cells;
	// the sum of three squares is ~1.9e10, comfortably inside int64 and nowhere
	// near it. The comment on this function's int64 choice therefore still holds
	// with a 4x margin eaten.
	const int64 Cells = VoxelBrickPoolDetail::kChunkEdgeVoxels;
	const bool bCover = (Key.Level == kCoverLevel);
	// Cover cells spanned by one chunk of this key's lattice.
	const int64 Span = bCover
		? Cells
		: (Cells * kCoverCellsPerVoxel0 * (int64(1) << FMath::Clamp(Key.Level, 0, 15)));
	const int64 Half = Span / 2;
	const int64 Cx = int64(Key.X) * Span + Half;
	const int64 Cy = int64(Key.Y) * Span + Half;
	const int64 Cz = int64(Key.Z) * Span + Half;
	// The focus arrives in LEVEL-0 voxels (this class's published contract, and
	// changing that would touch every caller); convert it here rather than asking
	// callers to know about cover.
	const int64 Fx = EvictionFocus[0] * kCoverCellsPerVoxel0;
	const int64 Fy = EvictionFocus[1] * kCoverCellsPerVoxel0;
	const int64 Fz = EvictionFocus[2] * kCoverCellsPerVoxel0;
	const int64 Dx = Cx - Fx;
	const int64 Dy = Cy - Fy;
	const int64 Dz = Cz - Fz;
	return Dx * Dx + Dy * Dy + Dz * Dz;
}

void FVoxelBrickPool::RebuildEvictionOrder()
{
	// Rebuilt from the RESIDENT SET, not by sorting the existing array: the array
	// carries ghosts (keys removed, or removed and re-added) and the resident map
	// is the only thing that knows which entries are real. This is also what
	// makes the ghost accumulation self-limiting -- every rebuild drops them.
	EvictionOrder.Reset();
	EvictionOrder.Reserve(Resident.Num());
	for (const TPair<FVoxelBrickChunkKey, FResidentChunk>& Pair : Resident)
	{
		EvictionOrder.Add(Pair.Key);
	}
	EvictionCursor = 0;

	if (!bHasEvictionFocus)
	{
		// No focus: fall back to insertion order, which is what this always did.
		// Sorted by the add sequence so the fallback is the SAME order as before
		// rather than TMap iteration order, which is not an order at all.
		const TMap<FVoxelBrickChunkKey, FResidentChunk>& R = Resident;
		EvictionOrder.Sort([&R](const FVoxelBrickChunkKey& A, const FVoxelBrickChunkKey& B)
		{
			const FResidentChunk* CA = R.Find(A);
			const FResidentChunk* CB = R.Find(B);
			return (CA ? CA->AddSequence : 0) < (CB ? CB->AddSequence : 0);
		});
		bEvictionOrderSorted = false;
		return;
	}

	// FARTHEST FIRST. Distances are cached into an array first rather than
	// recomputed inside the comparator: a sort calls the predicate O(n log n)
	// times and FocusDistSqOf is six multiplies, which at 131,072 chunks is
	// tens of millions of them for no reason.
	TMap<FVoxelBrickChunkKey, int64> DistSq;
	DistSq.Reserve(EvictionOrder.Num());
	for (const FVoxelBrickChunkKey& Key : EvictionOrder)
	{
		DistSq.Add(Key, FocusDistSqOf(Key));
	}
	EvictionOrder.Sort([&DistSq](const FVoxelBrickChunkKey& A, const FVoxelBrickChunkKey& B)
	{
		const int64* DA = DistSq.Find(A);
		const int64* DB = DistSq.Find(B);
		return (DA ? *DA : 0) > (DB ? *DB : 0);
	});

	EvictionOrderFocus[0] = EvictionFocus[0];
	EvictionOrderFocus[1] = EvictionFocus[1];
	EvictionOrderFocus[2] = EvictionFocus[2];
	bEvictionOrderSorted = true;
}

bool FVoxelBrickPool::EvictOne()
{
	if (Resident.Num() == 0)
	{
		return false;
	}

	// Rebuild when the sorted order does not exist yet, when the cursor has run
	// off the end, or when the focus has moved far enough that "farthest" no
	// longer means what it meant when the array was built. NOT per eviction: a
	// sort inside AddChunkFrom*'s evict-until-it-fits loop would turn a pressured
	// add into an O(n log n) per iteration.
	bool bNeedRebuild = EvictionCursor >= EvictionOrder.Num();
	if (bHasEvictionFocus && !bNeedRebuild)
	{
		if (!bEvictionOrderSorted)
		{
			bNeedRebuild = true;
		}
		else
		{
			const int64 Dx = EvictionFocus[0] - EvictionOrderFocus[0];
			const int64 Dy = EvictionFocus[1] - EvictionOrderFocus[1];
			const int64 Dz = EvictionFocus[2] - EvictionOrderFocus[2];
			bNeedRebuild = (Dx * Dx + Dy * Dy + Dz * Dz)
			             > VoxelBrickPoolDetail::kEvictionFocusRebuildVoxelsSq;
		}
	}
	if (bNeedRebuild)
	{
		RebuildEvictionOrder();
	}

	const bool bByDistance = bHasEvictionFocus && bEvictionOrderSorted;

	// Front first, skipping stale entries. A key can appear here more than once
	// (re-added after a removal); the sequence number on the resident record is
	// what tells the current entry from a ghost, which is what keeps this O(1)
	// amortised instead of an O(n) removal on every add.
	while (EvictionCursor < EvictionOrder.Num())
	{
		const FVoxelBrickChunkKey Key = EvictionOrder[EvictionCursor];
		++EvictionCursor;
		if (FResidentChunk* Found = Resident.Find(Key))
		{
			FreeResident(*Found);
			PendingClears.Add(Found->ChunkSlot);
			// P3: the same retirement, for the GPU-side chunk index. Queued in
			// lockstep with the clear above so the index and the record table
			// can never disagree about which slots are live -- see
			// FVoxelBrickIndexDelta, and the eviction hazard note on
			// PendingIndexRemovals, which is about THIS line.
			PendingIndexRemovals.Add(FVoxelBrickIndexEntry{ Key, Found->ChunkSlot });
			// A pending write for the evicted slot would otherwise land after
			// its clear and resurrect a chunk whose ranges have been handed
			// back. Dropped here, which is the same rule RemoveChunk follows.
			for (int32 I = PendingWrites.Num() - 1; I >= 0; --I)
			{
				if (PendingWrites[I].ChunkSlot == Found->ChunkSlot)
				{
					PendingWrites.RemoveAt(I, EAllowShrinking::No);
					++WritesDropped;
				}
			}
			NoteResidentDelta(Key, -1);
			Resident.Remove(Key);
			++Evictions;
			EvictionsByDistance += bByDistance ? 1 : 0;
			if (!bAnnouncedFirstEviction)
			{
				bAnnouncedFirstEviction = true;
				UE_LOG(LogVoxelBrickPool, Error,
				       TEXT("BRICK POOL EVICTED FOR THE FIRST TIME -- chunk (%d,%d,%d) L%d, %d resident ")
				       TEXT("of %u, %s. THREE THINGS THAT WERE TRUE UNTIL NOW ARE NOT ANY MORE, and all ")
				       TEXT("three fail silently: (1) an ADOPTED parked chunk whose bricks were evicted ")
				       TEXT("has no job to re-produce it and is simply absent from the volume; (2) the ")
				       TEXT("index delta's Removed path is live for the first time, so a consumer that ")
				       TEXT("never implemented removal now reads freed slots; (3) resident bytes are a ")
				       TEXT("capacity artefact rather than a census of the world. Check ")
				       TEXT("voxel.Brick.Stats for allocFail and the largest free run before reading ")
				       TEXT("any residency figure from this run. Logged ONCE; the count keeps rising ")
				       TEXT("silently after this line."),
				       Key.X, Key.Y, Key.Z, Key.Level, Resident.Num(), Config.ChunkCapacity,
				       bByDistance ? TEXT("ranked by distance from the focus")
				                   : TEXT("in INSERTION ORDER -- no eviction focus was ever set, so this ")
				                     TEXT("evicted the chunk that loaded EARLIEST, which is the one ")
				                     TEXT("nearest the player"));
			}
			return true;
		}
	}

	// The array held nothing live but the map is not empty -- every entry was a
	// ghost. One rebuild, then take the front. Bounded: the rebuild is built from
	// the resident map, so it contains no ghosts and this cannot recurse twice.
	if (Resident.Num() > 0 && EvictionCursor >= EvictionOrder.Num() && !bNeedRebuild)
	{
		RebuildEvictionOrder();
		return EvictOne();
	}
	return false;
}

bool FVoxelBrickPool::AllocateForChunk(const FVoxelBrickChunkKey& Key, uint32 OccWords,
                                      uint32 MatWords, FResidentChunk& OutChunk)
{
	// A re-mesh of a resident chunk is a REPLACEMENT. Freeing first means the
	// arenas see the old ranges back before the new ones are asked for, which is
	// also what keeps a churning chunk from ratcheting the high-water mark.
	//
	// THIS IS ALSO WHAT MAKES THE TWO PRODUCERS SAFE TOGETHER. A chunk that the
	// GPU fork packed and that an edit then re-meshes on the game thread arrives
	// here with the same key; the old ranges go back and the new ones are taken,
	// so the pool holds exactly one version of a chunk no matter which arm
	// produced either version. Without this a two-producer pool would leak an
	// allocation per re-mesh.
	if (FResidentChunk* Existing = Resident.Find(Key))
	{
		FreeResident(*Existing);
		for (int32 I = PendingWrites.Num() - 1; I >= 0; --I)
		{
			if (PendingWrites[I].ChunkSlot == Existing->ChunkSlot)
			{
				PendingWrites.RemoveAt(I, EAllowShrinking::No);
				++WritesDropped;
			}
		}
		NoteResidentDelta(Key, -1);
		Resident.Remove(Key);
	}

	FVoxelGpuPoolAllocation DescAlloc;
	FVoxelGpuPoolAllocation OccAlloc;
	FVoxelGpuPoolAllocation MatAlloc;

	// Evict until it fits. Bounded by the resident set, and every iteration
	// removes one chunk, so this terminates.
	for (;;)
	{
		DescAlloc = DescArena.Alloc(VoxelBrickPoolDetail::kBricksPerChunk);
		OccAlloc = (OccWords > 0) ? OccArena.Alloc(OccWords) : FVoxelGpuPoolAllocation{ 0, 0 };
		MatAlloc = (MatWords > 0) ? MatArena.Alloc(MatWords) : FVoxelGpuPoolAllocation{ 0, 0 };

		const bool bDescOk = DescAlloc.IsValid();
		const bool bOccOk = (OccWords == 0) || OccAlloc.IsValid();
		const bool bMatOk = (MatWords == 0) || MatAlloc.IsValid();
		if (bDescOk && bOccOk && bMatOk)
		{
			break;
		}

		// Give back whatever DID succeed before evicting, or the retry leaks.
		if (bDescOk) { DescArena.Free(DescAlloc); }
		if (OccWords > 0 && OccAlloc.IsValid()) { OccArena.Free(OccAlloc); }
		if (MatWords > 0 && MatAlloc.IsValid()) { MatArena.Free(MatAlloc); }

		if (!EvictOne())
		{
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("Brick pool REFUSED chunk (%d,%d,%d) L%d: %u occ + %u mat dwords, 64 descriptors. ")
			       TEXT("Free: desc %u (largest run %u), occ %u (%u), mat %u (%u), resident %d chunks. ")
			       TEXT("Nothing left to evict, so this is capacity or fragmentation, not churn."),
			       Key.X, Key.Y, Key.Z, Key.Level, OccWords, MatWords,
			       DescArena.GetFreeQuads(), DescArena.GetLargestFreeRun(),
			       OccArena.GetFreeQuads(), OccArena.GetLargestFreeRun(),
			       MatArena.GetFreeQuads(), MatArena.GetLargestFreeRun(),
			       Resident.Num());
			++AllocFailures;
			return false;
		}
	}

	// EVERY descriptor allocation is exactly 64 slots and the capacity is a
	// multiple of 64, so first-fit over an always-coalesced free list hands back
	// 64-aligned offsets by induction. ChunkSlot = BrickBase / 64 depends on
	// that, so it is checked rather than believed.
	checkf((DescAlloc.Offset % VoxelBrickPoolDetail::kBricksPerChunk) == 0,
	       TEXT("descriptor allocation %u is not 64-aligned; ChunkSlot would collide"), DescAlloc.Offset);

	OutChunk = FResidentChunk{};
	OutChunk.Key = Key;
	OutChunk.ChunkSlot = DescAlloc.Offset / VoxelBrickPoolDetail::kBricksPerChunk;
	OutChunk.BrickBase = DescAlloc.Offset;
	OutChunk.OccBase = (OccWords > 0) ? OccAlloc.Offset : 0;
	OutChunk.MatBase = (MatWords > 0) ? MatAlloc.Offset : 0;
	OutChunk.OccWords = OccWords;
	OutChunk.MatWords = MatWords;
	OutChunk.AddSequence = NextAddSequence++;
	NoteResidentDelta(Key, +1);
	Resident.Add(Key, OutChunk);
	// APPENDED, i.e. evicted last, and correct without a re-sort under both
	// orders: under the focus order a chunk that just streamed in is near, and
	// under the insertion-order fallback appending IS the order.
	EvictionOrder.Add(Key);
	++ChunksAdded;
	return true;
}

int32 FVoxelBrickPool::AddChunkFromGpu(const FVoxelGpuBrickPayloadRef& Payload,
                                       const FVoxelBrickChunkKey& Key,
                                       const FVoxelBrickChunkShading& Shading)
{
	if (!bInitialised)
	{
		Init(FVoxelBrickPoolConfig{});
	}
	if (!Payload.IsValid() || Payload->BrickCount != VoxelBrickPoolDetail::kBricksPerChunk)
	{
		UE_LOG(LogVoxelBrickPool, Error,
		       TEXT("Brick add for chunk (%d,%d,%d) L%d refused: payload %s, brickCount %u (expected %u). ")
		       TEXT("A chunk is 64 descriptor slots ALWAYS, collapsed bricks included \u2014 that is what makes ")
		       TEXT("BrickBase + brickIndex addressable without an indirection."),
		       Key.X, Key.Y, Key.Z, Key.Level,
		       Payload.IsValid() ? TEXT("present") : TEXT("NULL"),
		       Payload.IsValid() ? Payload->BrickCount : 0u, VoxelBrickPoolDetail::kBricksPerChunk);
		++AllocFailures;
		return INDEX_NONE;
	}

	FResidentChunk Chunk;
	if (!AllocateForChunk(Key, Payload->OccWords, Payload->MatWords, Chunk))
	{
		return INDEX_NONE;
	}

	FPendingWrite Write;
	Write.Payload = Payload;
	Write.Key = Key;
	Write.ChunkSlot = Chunk.ChunkSlot;
	Write.BrickBase = Chunk.BrickBase;
	Write.OccBase = Chunk.OccBase;
	Write.MatBase = Chunk.MatBase;
	Write.RingLevel = uint32(FMath::Clamp(Key.Level, 0, 15));
	Write.Shading = Shading;
	if (Shading.IsNeutral())
	{
		++ChunksAddedNeutralShading;
	}
	Write.OriginVoxel = Payload->OriginVoxel;
	PendingWrites.Add(MoveTemp(Write));

	return int32(Chunk.ChunkSlot);
}

// The CPU arm. Same arenas, same allocator, same eviction -- see the header.
int32 FVoxelBrickPool::AddChunkFromCpu(const FVoxelBrickCpuPackRef& Pack,
                                       const FVoxelBrickChunkKey& Key,
                                       const FVoxelBrickChunkShading& Shading)
{
	if (!bInitialised)
	{
		Init(FVoxelBrickPoolConfig{});
	}
	// 128 dwords, ALWAYS: 64 descriptors of 2 dwords each, collapsed bricks
	// included. Checked rather than assumed for AddChunkFromGpu's reason -- a
	// short descriptor array would leave the tail of a 64-slot allocation holding
	// whatever the previous tenant left, which reads as real terrain.
	const int32 WantDescDwords = int32(VoxelBrickPoolDetail::kBricksPerChunk) * 2;
	if (!Pack.IsValid() || Pack->Desc.Num() != WantDescDwords)
	{
		UE_LOG(LogVoxelBrickPool, Error,
		       TEXT("CPU brick add for chunk (%d,%d,%d) L%d refused: pack %s, %d descriptor dwords ")
		       TEXT("(expected %d). A chunk is 64 descriptor slots ALWAYS."),
		       Key.X, Key.Y, Key.Z, Key.Level,
		       Pack.IsValid() ? TEXT("present") : TEXT("NULL"),
		       Pack.IsValid() ? Pack->Desc.Num() : 0, WantDescDwords);
		++AllocFailures;
		return INDEX_NONE;
	}

	FResidentChunk Chunk;
	if (!AllocateForChunk(Key, Pack->OccWords(), Pack->MatWords(), Chunk))
	{
		return INDEX_NONE;
	}
	++ChunksAddedFromCpu;

	FPendingWrite Write;
	Write.CpuPack = Pack;
	Write.Key = Key;
	Write.ChunkSlot = Chunk.ChunkSlot;
	Write.BrickBase = Chunk.BrickBase;
	Write.OccBase = Chunk.OccBase;
	Write.MatBase = Chunk.MatBase;
	Write.RingLevel = uint32(FMath::Clamp(Key.Level, 0, 15));
	Write.Shading = Shading;
	if (Shading.IsNeutral())
	{
		++ChunksAddedNeutralShading;
	}
	Write.OriginVoxel = Pack->OriginVoxel;
	PendingWrites.Add(MoveTemp(Write));

	return int32(Chunk.ChunkSlot);
}

bool FVoxelBrickPool::RemoveChunk(const FVoxelBrickChunkKey& Key)
{
	FResidentChunk* Found = Resident.Find(Key);
	if (Found == nullptr)
	{
		return false;
	}

	FreeResident(*Found);
	const uint32 Slot = Found->ChunkSlot;
	// A write still queued for this slot describes a chunk that has just been
	// retired. Dropping it is what makes clear-before-write sound for a slot
	// that is retired and re-allocated inside one batch: the only write left for
	// the slot is the one that came after the clear.
	for (int32 I = PendingWrites.Num() - 1; I >= 0; --I)
	{
		if (PendingWrites[I].ChunkSlot == Slot)
		{
			PendingWrites.RemoveAt(I, EAllowShrinking::No);
			++WritesDropped;
		}
	}
	PendingClears.Add(Slot);
	// P3, in lockstep with the clear, exactly as eviction does it. NOTHING IN
	// STREAMING CALLS THIS YET -- which is precisely why the index consumer must
	// implement removal before it is ever exercised, rather than after.
	PendingIndexRemovals.Add(FVoxelBrickIndexEntry{ Key, Slot });
	NoteResidentDelta(Key, -1);
	Resident.Remove(Key);
	return true;
}

void FVoxelBrickPool::Reset()
{
	DescArena.Reset();
	OccArena.Reset();
	MatArena.Reset();
	for (std::atomic<int32>& N : LevelChunkCounts) { N.store(0, std::memory_order_relaxed); }
	Resident.Reset();
	EvictionOrder.Reset();
	EvictionCursor = 0;
	bEvictionOrderSorted = false;
	PendingWrites.Reset();
	PendingClears.Reset();
	PendingIndexRemovals.Reset();
}

// Records every pending write and clear into Graph -- AND DOES NOT EXECUTE IT.
//
// THE SPLIT IS THE FIX FOR A REAL CRASH, not tidiness. FRDGBuilder::Execute()
// opens with check(LocalCurrentBreadcrumb == FRHIBreadcrumbNode::Sentinel)
// (RenderGraphBuilder.cpp:1772): NO RDG event scope may still be open when a
// graph executes. The first in-engine Flush declared its RDG_EVENT_SCOPE in the
// same block as Execute(), so the scope was still alive at the call and the
// assert fired on the very first dispatch.
//
// Bracing the scope into a nested block would also have worked and would have
// been one edit away from breaking again. This is the shape AddRegionPasses
// already uses and the reason it never had the bug: a function that only ADDS
// passes cannot leave a scope open past an Execute it does not own. Anything
// added here inherits that property for free.
//
// RENDER THREAD ONLY.
void FVoxelBrickPool::AddFlushPasses_RenderThread(FRDGBuilder& GraphBuilder,
                                                  const FVoxelBrickPoolBuffersRef& Buffers,
                                                  const TArray<FPendingWrite>& Writes,
                                                  const TArray<uint32>& Clears)
{
	RDG_EVENT_SCOPE(GraphBuilder, "VoxelBrickPool.Flush(%d writes, %d clears)",
	                Writes.Num(), Clears.Num());

	FRDGBufferRef DstDesc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled,
	                                                           TEXT("VoxelBrickPool.Desc"));
	FRDGBufferRef DstOcc = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled,
	                                                          TEXT("VoxelBrickPool.Occ"));
	FRDGBufferRef DstMat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled,
	                                                          TEXT("VoxelBrickPool.Mat"));
	FRDGBufferRef DstTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled,
	                                                            TEXT("VoxelBrickPool.ChunkTable"));

	// CLEARS FIRST. A slot retired and re-allocated inside one batch would
	// otherwise end up cleared by a command recorded for the chunk that used
	// to own it. The other order is only safe because AddChunkFromGpu and
	// RemoveChunk drop any write that a clear invalidates -- both halves of
	// that rule are needed, and this is the half that is visible in a
	// capture.
	for (uint32 Slot : Clears)
	{
		VoxelGpuWorldGen::AddBrickChunkClearPass(GraphBuilder, DstTable, Slot);
	}

	for (const FPendingWrite& Write : Writes)
	{
		if (Write.CpuPack.IsValid())
		{
			// The CPU arm's bytes are in system memory, not in a buffer this
			// graph could copy from. UploadCpuWrites_RenderThread moves them
			// after this graph executes -- see its declaration for why after
			// and not before.
			continue;
		}
		const FVoxelGpuBrickPayloadRef& P = Write.Payload;
		// Occ is required even by a chunk that allocated no occupancy
		// dwords: the record pass reads it to decide allSolid, and a write
		// that skipped the record would leave descriptors in the pool that
		// no record names -- allocated, populated and invisible.
		if (!P.IsValid() || !P->Desc.IsValid() || !P->ChunkMask.IsValid() || !P->Occ.IsValid())
		{
			// The chunk holds an allocation and a record slot with nothing
			// in them. Loud, because on the marching path that is terrain
			// that reports as loaded and is not there.
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("Brick write for slot %u DROPPED — payload has no buffers. 64 descriptors ")
			       TEXT("at %u are allocated and empty."),
			       Write.ChunkSlot, Write.BrickBase);
			continue;
		}

		FRDGBufferRef SrcDesc = GraphBuilder.RegisterExternalBuffer(P->Desc, TEXT("BrickSrc.Desc"));
		FRDGBufferRef SrcMask = GraphBuilder.RegisterExternalBuffer(P->ChunkMask, TEXT("BrickSrc.Mask"));
		FRDGBufferRef SrcOcc = P->Occ.IsValid()
			? GraphBuilder.RegisterExternalBuffer(P->Occ, TEXT("BrickSrc.Occ")) : nullptr;
		FRDGBufferRef SrcMat = P->Mat.IsValid()
			? GraphBuilder.RegisterExternalBuffer(P->Mat, TEXT("BrickSrc.Mat")) : nullptr;

		// The payload's occupancy offsets are chunk-relative and start at 0,
		// so the whole run moves as one copy.
		if (P->OccWords > 0 && SrcOcc != nullptr)
		{
			VoxelGpuWorldGen::AddBrickWordCopyPass(GraphBuilder, DstOcc, SrcOcc,
			                                       /*SrcFirst*/ 0, Write.OccBase, P->OccWords);
		}
		if (P->MatWords > 0 && SrcMat != nullptr)
		{
			VoxelGpuWorldGen::AddBrickWordCopyPass(GraphBuilder, DstMat, SrcMat,
			                                       /*SrcFirst*/ 0, Write.MatBase, P->MatWords);
		}

		VoxelGpuWorldGen::AddBrickDescPoolWritePass(GraphBuilder, DstDesc, SrcDesc,
		                                            P->SrcBrickFirst, Write.BrickBase,
		                                            P->BrickCount, Write.OccBase, Write.MatBase);

		// The record LAST, and reading the SCRATCH buffers. Last because a
		// record is what makes a chunk visible to a marcher, and it must not
		// name arena ranges that have not been written yet. Scratch because
		// those offsets are chunk-relative, so the allSolid walk needs no
		// base and cannot be wrong about one.
		VoxelGpuWorldGen::AddBrickChunkRecordPass(
			GraphBuilder, DstTable, SrcDesc, SrcOcc, SrcMask,
			P->SrcBrickFirst, P->SrcChunkIndex, P->BrickCount,
			Write.ChunkSlot, Write.BrickBase, Write.RingLevel, P->OriginVoxel,
			Write.Shading);
	}
}

// The CPU arm's upload. RENDER THREAD ONLY, AFTER the graph -- see the header.
//
// EVERY FORMAT DECISION HERE IS A COPY OF A KERNEL, NOT A SECOND DERIVATION, and
// the two are named so a future reader can diff them:
//
//   the descriptor rebase   == BrickDescPoolWriteMain (VoxelBrickPoolWrite.usf)
//   the 32 B chunk record   == BrickChunkRecordMain   (same file)
//
// The one thing that is NOT recomputed is allSolid: it comes off
// vxc::ChunkBrickPack, which derived it from the CELL data exactly as the kernel
// does. That is the field this project would have got wrong by deriving -- a
// brick can be fully solid and still MIXED, so "all 64 descriptors are uniform
// SOLID" is strictly stronger than allSolid and under-reports it -- and it is
// the reason this function copies flags rather than inspecting descriptors.
void FVoxelBrickPool::UploadCpuWrites_RenderThread(FRHICommandListImmediate& RHICmdList,
                                                   const FVoxelBrickPoolBuffersRef& Buffers,
                                                   const TArray<FPendingWrite>& Writes)
{
	// docs/brick-volume-format.md section 2. Restated here for the same
	// no-voxel-core reason the rest of this file restates its constants, and
	// identical to VoxelBrickPoolWrite.usf's kBrickOffsetMask / kBrickFieldMask /
	// kBrickKindMixed.
	constexpr uint32 kBrickOffsetMask = 0x0fffffffu;
	constexpr uint32 kBrickFieldMask  = 0xf0000000u;
	constexpr uint32 kBrickKindMixed  = 2u;

	// Scratch reused across the batch: a chunk's rebased descriptors are 128
	// dwords and building them per chunk into a fresh TArray would allocate once
	// per chunk on the render thread, which is the cost this pool exists to avoid
	// paying per chunk anywhere.
	TArray<uint32> RebasedDesc;
	RebasedDesc.SetNumUninitialized(int32(VoxelBrickPoolDetail::kBricksPerChunk) * 2);

	for (const FPendingWrite& Write : Writes)
	{
		const FVoxelBrickCpuPackRef& P = Write.CpuPack;
		if (!P.IsValid())
		{
			continue; // a GPU write; the graph already handled it
		}

		// --- descriptors, with the pool bases folded into the offset fields ---
		//
		// MIXED ONLY. A uniform brick's offset fields are zero by contract and
		// must stay zero: rebasing them would point a collapsed brick at a real
		// arena range, which is worse than useless because it reads as valid.
		for (uint32 I = 0; I < VoxelBrickPoolDetail::kBricksPerChunk; ++I)
		{
			uint32 Dx = P->Desc[int32(I) * 2 + 0];
			uint32 Dy = P->Desc[int32(I) * 2 + 1];
			if (((Dx >> 28) & 3u) == kBrickKindMixed)
			{
				const uint32 OccOffset = ((Dx & kBrickOffsetMask) + Write.OccBase) & kBrickOffsetMask;
				const uint32 MatOffset = ((Dy & kBrickOffsetMask) + Write.MatBase) & kBrickOffsetMask;
				Dx = OccOffset | (Dx & kBrickFieldMask);
				Dy = MatOffset | (Dy & kBrickFieldMask);
			}
			RebasedDesc[int32(I) * 2 + 0] = Dx;
			RebasedDesc[int32(I) * 2 + 1] = Dy;
		}

		// --- the three arena writes ---------------------------------------
		//
		// ORDER WITHIN A CHUNK MATTERS AND IS THE KERNELS' ORDER: payload first,
		// descriptors second, RECORD LAST. A record is what makes a chunk visible
		// to a marcher, so it must not name arena ranges that have not been
		// written yet. These land on one command list in the order they are
		// issued, so the ordering is the issue order and nothing else.
		if (P->OccWords() > 0)
		{
			const uint32 Bytes = P->OccWords() * 4;
			if (void* Dst = RHICmdList.LockBuffer(Buffers->OccBuffer, Write.OccBase * 4u,
			                                      Bytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, P->Occ.GetData(), Bytes);
				RHICmdList.UnlockBuffer(Buffers->OccBuffer);
			}
		}
		if (P->MatWords() > 0)
		{
			const uint32 Bytes = P->MatWords() * 4;
			if (void* Dst = RHICmdList.LockBuffer(Buffers->MatBuffer, Write.MatBase * 4u,
			                                      Bytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, P->Mat.GetData(), Bytes);
				RHICmdList.UnlockBuffer(Buffers->MatBuffer);
			}
		}
		{
			const uint32 Bytes = VoxelBrickPoolDetail::kBricksPerChunk * VoxelBrickPoolDetail::kBrickDescBytes;
			if (void* Dst = RHICmdList.LockBuffer(
				    Buffers->DescBuffer,
				    Write.BrickBase * VoxelBrickPoolDetail::kBrickDescBytes, Bytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, RebasedDesc.GetData(), Bytes);
				RHICmdList.UnlockBuffer(Buffers->DescBuffer);
			}
		}

		// --- the 32 B record ------------------------------------------------
		//
		// Eight dwords, field for field with BrickChunkRecordMain's tail:
		// origin xyz, LevelAndFlags ([0:3] ring level, [4] anySolid, [5]
		// allSolid), BrickBase, the 64-bit L1 mask as two dwords, and a zero.
		{
			uint32 Record[kChunkRecordDwords];
			BuildChunkRecord(Write.OriginVoxel, Write.RingLevel, P->bAnySolid, P->bAllSolid,
			                 Write.BrickBase, P->BrickSolid, Write.Shading, Record);

			// uint32 throughout: the table is ChunkCapacity * 32 B, which at the
			// 131,072-chunk default is 4 MiB, and LockBuffer takes a uint32 offset.
			const uint32 Offset = Write.ChunkSlot * VoxelBrickPoolDetail::kChunkRecordBytes;
			if (void* Dst = RHICmdList.LockBuffer(Buffers->ChunkTableBuffer, Offset,
			                                      VoxelBrickPoolDetail::kChunkRecordBytes, RLM_WriteOnly))
			{
				FMemory::Memcpy(Dst, Record, VoxelBrickPoolDetail::kChunkRecordBytes);
				RHICmdList.UnlockBuffer(Buffers->ChunkTableBuffer);
			}
		}
	}
}

void FVoxelBrickPool::Flush()
{
	if (PendingWrites.Num() == 0 && PendingClears.Num() == 0)
	{
		return;
	}

	TArray<FPendingWrite> Writes = MoveTemp(PendingWrites);
	TArray<uint32> Clears = MoveTemp(PendingClears);
	PendingWrites.Reset();
	PendingClears.Reset();

	// Counted on the GAME THREAD, before the writes are moved into the render
	// command, because after the move this object no longer knows what is in
	// them. This is the CPU arm's PCIe traffic and it is the number to read when
	// asking what the CPU arm costs on the render thread -- the GPU arm moves its
	// bytes arena-to-arena and appears here as zero, correctly.
	for (const FPendingWrite& W : Writes)
	{
		if (W.CpuPack.IsValid())
		{
			CpuUploadBytes += int64(W.CpuPack->ResidentBytes());
		}
	}

	// --- P3: the index delta, BUILT here and DELIVERED below ----------------
	//
	// BUILT BEFORE THE ENQUEUE because Writes is MOVED INTO the render command a
	// few lines down and is a hollow array afterwards. Reading it after the move
	// compiles, runs, and yields an empty delta -- so the index would simply stop
	// being told about new chunks, with every pool counter still healthy and the
	// marched world missing everything that streamed in. Building here and
	// delivering after the enqueue keeps both properties: the delta describes the
	// real batch, and the consumer still lands behind the pool write.
	//
	// Removed is taken unconditionally, sink or no sink. Leaving a retirement
	// queued for a consumer that does not exist would replay it on the next flush
	// against an index that has already moved on.
	const double FlushPrepStart = FPlatformTime::Seconds();
	FVoxelBrickIndexDelta IndexDelta;
	IndexDelta.Removed = MoveTemp(PendingIndexRemovals);
	PendingIndexRemovals.Reset();
	if (IndexSink)
	{
		IndexDelta.Added.Reserve(Writes.Num());
		for (const FPendingWrite& W : Writes)
		{
			IndexDelta.Added.Add(FVoxelBrickIndexEntry{ W.Key, W.ChunkSlot });
		}
	}

	const double FlushEnqueueStart = FPlatformTime::Seconds();
	FlushStageMs.PrepMs += (FlushEnqueueStart - FlushPrepStart) * 1000.0;
	ENQUEUE_RENDER_COMMAND(VoxelBrickPoolFlush)(
		[Buffers = GetOrCreateBuffers(), Writes = MoveTemp(Writes),
		 Clears = MoveTemp(Clears)](FRHICommandListImmediate& RHICmdList) mutable
	{
		if (!Buffers.IsValid())
		{
			return;
		}

		// First use creates the arenas. No scene proxy is involved -- nothing
		// draws from this pool -- so there is no bootstrap to wait for and no
		// window in which a write has nowhere to land.
		if (!Buffers->IsValid())
		{
			Buffers->DescBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.Desc"),
			                                        VoxelBrickPoolDetail::kBrickDescBytes, Buffers->DescSlots);
			Buffers->OccBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.Occ"),
			                                       sizeof(uint32), Buffers->OccWords);
			Buffers->MatBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.Mat"),
			                                       sizeof(uint32), Buffers->MatWords);
			Buffers->ChunkTableBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.ChunkTable"),
			                                              sizeof(uint32),
			                                              Buffers->ChunkSlots * VoxelBrickPoolDetail::kChunkRecordDwords);

			Buffers->DescPooled = new FRDGPooledBuffer(
				RHICmdList, Buffers->DescBuffer,
				FRDGBufferDesc::CreateStructuredDesc(VoxelBrickPoolDetail::kBrickDescBytes, Buffers->DescSlots),
				Buffers->DescSlots, TEXT("VoxelBrickPool.Desc"));
			Buffers->OccPooled = new FRDGPooledBuffer(
				RHICmdList, Buffers->OccBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Buffers->OccWords),
				Buffers->OccWords, TEXT("VoxelBrickPool.Occ"));
			Buffers->MatPooled = new FRDGPooledBuffer(
				RHICmdList, Buffers->MatBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Buffers->MatWords),
				Buffers->MatWords, TEXT("VoxelBrickPool.Mat"));
			Buffers->ChunkTablePooled = new FRDGPooledBuffer(
				RHICmdList, Buffers->ChunkTableBuffer,
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32),
				                                     Buffers->ChunkSlots * VoxelBrickPoolDetail::kChunkRecordDwords),
				Buffers->ChunkSlots * VoxelBrickPoolDetail::kChunkRecordDwords, TEXT("VoxelBrickPool.ChunkTable"));
		}

		// Creation can fail (a lost device, an out-of-memory arena), and the
		// next line would then register a null pooled buffer into a graph. This
		// path has already crashed once in an editor on its first ever run, so
		// it says what happened rather than taking the second failure as well.
		if (!Buffers->IsValid())
		{
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("Brick pool buffers could not be created (%u desc slots, %u occ, %u mat dwords). ")
			       TEXT("%d write(s) and %d clear(s) DROPPED -- those chunks hold an allocation and a ")
			       TEXT("record slot with nothing in them."),
			       Buffers->DescSlots, Buffers->OccWords, Buffers->MatWords, Writes.Num(), Clears.Num());
			return;
		}

		FRDGBuilder GraphBuilder(RHICmdList);
		// Every pass, inside a function that closes its own event scope before
		// it returns. See AddFlushPasses_RenderThread for why that is
		// load-bearing.
		AddFlushPasses_RenderThread(GraphBuilder, Buffers, Writes, Clears);
		GraphBuilder.Execute();

		// AFTER Execute(), and that is the clear-before-write rule rather than a
		// preference -- see UploadCpuWrites_RenderThread's declaration. Returns
		// immediately when the batch holds no CPU writes, which is every batch
		// under voxel.Brick.PackOnCpu 0.
		UploadCpuWrites_RenderThread(RHICmdList, Buffers, Writes);
	});
	const double FlushSinkStart = FPlatformTime::Seconds();
	FlushStageMs.EnqueueMs += (FlushSinkStart - FlushEnqueueStart) * 1000.0;

	// --- P3: the index delta, DELIVERED LAST --------------------------------
	//
	// AFTER the render command is enqueued, and that ordering is the seam's whole
	// guarantee: a consumer that enqueues its index upload from inside this call
	// lands behind the pool write on the same command list, so the GPU can never
	// see an index entry for a slot the pool has not written yet. See
	// FVoxelBrickIndexSink.
	if (IndexSink && !IndexDelta.IsEmpty())
	{
		IndexSink(IndexDelta);
	}
	FlushStageMs.SinkMs += (FPlatformTime::Seconds() - FlushSinkStart) * 1000.0;
}

uint64 FVoxelBrickPool::GetResidentBytes() const
{
	return uint64(DescArena.GetUsedQuads()) * VoxelBrickPoolDetail::kBrickDescBytes
	     + uint64(OccArena.GetUsedQuads()) * 4
	     + uint64(MatArena.GetUsedQuads()) * 4
	     + uint64(Resident.Num()) * VoxelBrickPoolDetail::kChunkRecordBytes;
}

FVoxelBrickPool& GetGlobalVoxelBrickPool()
{
	static FVoxelBrickPool Pool;
	return Pool;
}

// ---------------------------------------------------------------------------
// voxel.Brick.Stats -- the P2 gate, printable
// ---------------------------------------------------------------------------
//
// Every number the phase is measured on, in one place, with the census beside it
// so the comparison is not left to whoever reads the log. THE SUBTRACTION IS
// STATED: the census's ~415 MiB headline includes a 193.3 MiB dense flat brick
// index that this design does not allocate, so the figure to compare a sparse
// chunk-table pool against is ~222 MiB, not 415.
static FAutoConsoleCommand GVoxelBrickStatsCmd(
	TEXT("voxel.Brick.Stats"),
	TEXT("Brick pool residency: resident chunks, used and free per arena, largest free run "
	     "(the gap IS the fragmentation), allocation failures, evictions, and resident bytes "
	     "against the measured census."),
	FConsoleCommandDelegate::CreateStatic([]()
{
	const FVoxelBrickPool& Pool = GetGlobalVoxelBrickPool();
	if (!Pool.IsInitialised())
	{
		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("voxel.Brick.Stats: the brick pool has never been initialised — nothing has been ")
		       TEXT("packed. Check voxel.GPU.BrickPack."));
		return;
	}

	const FVoxelBrickPoolConfig& C = Pool.GetConfig();
	const double MiB = 1024.0 * 1024.0;
	const FVoxelBrickPoolBuffersRef Buffers = Pool.DebugGetBuffers();

	// Each term divided by ITS OWN count, not by a shared one. mesh and pack
	// counts are equal on a normal arm and wildly unequal on the suppression arm
	// (mesh 0), so a shared denominator would silently rescale whichever term the
	// arm turned off.
	const auto PerChunk = [](double TotalMs, int64 Count)
	{
		return Count > 0 ? TotalMs / double(Count) : 0.0;
	};
	const double MeshPerChunk = PerChunk(VoxelBrickGetCpuMeshMs(), VoxelBrickGetCpuMeshCount());
	const double FillPerChunk = PerChunk(VoxelBrickGetCpuFillMs(), VoxelBrickGetCpuFillCount());
	const double PackPerChunk = PerChunk(VoxelBrickGetCpuPackMs(), VoxelBrickGetCpuPackCount());
	// THE TOTAL IS NOT THE SUM OF THE THREE MEANS, AND GETTING THAT WRONG ONCE
	// ALREADY REVERSED A VERDICT. Summing MeshPerChunk + FillPerChunk +
	// PackPerChunk assumes the three denominators match. On a normal arm they
	// nearly do (mesh 82,749 / pack 82,653) so the sum happens to be right. On
	// the suppression arm they do not: mesh is a 0.740 ms/chunk mean over NINETY-
	// SIX chunks -- the game-thread edit path, which suppression deliberately
	// leaves meshing -- and adding it at full weight to a per-chunk figure
	// dominated by 83,671 chunks inflated the total from 0.389 to 1.127 and made
	// Phase 5 read as a LOSS against today's 0.969 when it is a 2.49x win.
	//
	// The note three lines above is correct that each TERM needs its own
	// denominator; it simply did not survive being summed. So: total worker ms
	// over chunks the arm actually processed. Every chunk packs on both arms,
	// which is what makes the pack count the right denominator for all of them.
	const double TotalArmMs = VoxelBrickGetCpuMeshMs() + VoxelBrickGetCpuFillMs()
	                        + VoxelBrickGetCpuPackMs();
	const double TotalPerChunk = PerChunk(TotalArmMs, VoxelBrickGetCpuPackCount());
	const double PackSpanSeconds = VoxelBrickGetPackSpanSeconds();

	UE_LOG(LogVoxelBrickPool, Log,
	       TEXT("voxel.Brick.Stats: %d chunks resident of %u; resident %.1f MiB, committed %.1f MiB.\n")
	       TEXT("  desc  %u / %u slots (%.1f MiB), largest free run %u\n")
	       TEXT("  occ   %u / %u dwords (%.1f MiB), largest free run %u, %d free runs\n")
	       TEXT("  mat   %u / %u dwords (%.1f MiB), largest free run %u, %d free runs\n")
	       TEXT("  added %lld (gpu %lld, cpu %lld), evictions %lld (%lld by distance), allocFail %lld, ")
	       TEXT("writesDropped %lld\n")
	       TEXT("  cpu arm: %lld chunks packed, %.1f MiB uploaded. PackOnCpu %d, ReuseMesherVoxels %d, ")
	       TEXT("SuppressQuadMesh %d, BrickPack %d, RetireQuads %d -- all EFFECTIVE values, so a ")
	       TEXT("command-line override shows here rather than the cvar it overrode.\n")
	       TEXT("  --- the Phase 5 question, in its three measured terms -------------------\n")
	       TEXT("  Every ms below is WORKER-THREAD time summed across threads. It is not wall time and ")
	       TEXT("not game-thread time: it is charged against streaming THROUGHPUT, which is where a ")
	       TEXT("regression here shows. The game-thread half is brickFlush= on the tick-budget line.\n")
	       TEXT("    mesh  %8lld chunks %10.1f ms %8.3f ms/chunk  (0 = this arm did not mesh)\n")
	       TEXT("    fill  %8lld chunks %10.1f ms %8.3f ms/chunk  (0 = the mesher supplied the voxels)\n")
	       TEXT("    pack  %8lld chunks %10.1f ms %8.3f ms/chunk  (%lld read the MESHER'S OWN voxels)\n")
	       TEXT("    THIS ARM'S TOTAL: %.3f ms per chunk.\n")
	       TEXT("  today = mesh + pack, because FDenseChunkSink makes the pack's fill free. ")
	       TEXT("phase 5 = fill + pack, because with nothing meshing the packer must materialise the ")
	       TEXT("voxels itself. COMPARE THE TWO 'TOTAL' LINES ACROSS ARMS AND DO NOT MIX TERMS -- one ")
	       TEXT("leg only ever has one of mesh/fill, and pairing this leg's mesh with another leg's ")
	       TEXT("fill is the arithmetic this instrument exists to replace.\n")
	       TEXT("  If pack > 0 and 'read the MESHER'S OWN voxels' is 0 while SuppressQuadMesh is 0, ")
	       TEXT("the reuse is OFF and every pack figure here is the expensive form.\n")
	       TEXT("  fill span: %lld packs over %.1f s = %.0f packs/s. THIS IS THE THROUGHPUT NUMBER ")
	       TEXT("TO READ UNDER voxel.Brick.SuppressQuadMesh, because that arm publishes no geometry ")
	       TEXT("and so collapses 'loaded=' (3,243 against 50,504) while the pool fills normally. ")
	       TEXT("Only meaningful once jobsInFlight is 0 and holding -- a churning world keeps ")
	       TEXT("extending the span.\n")
	       TEXT("  census at 10 cm / 4 km: occupancy 56.0 + cells 128.5 + palette 14.0 + descriptors 23.1 ")
	       TEXT("= 221.6 MiB for THIS structure. The published ~415 MiB includes a 193.3 MiB dense flat ")
	       TEXT("brick index that a chunk-table pool does not allocate — subtract it before comparing."),
	       Pool.GetNumResidentChunks(), C.ChunkCapacity,
	       double(Pool.GetResidentBytes()) / MiB,
	       Buffers.IsValid() ? double(Buffers->GetCapacityBytes()) / MiB : 0.0,
	       Pool.GetUsedDescSlots(), C.ChunkCapacity * VoxelBrickPoolDetail::kBricksPerChunk,
	       double(Pool.GetUsedDescSlots()) * VoxelBrickPoolDetail::kBrickDescBytes / MiB, Pool.GetLargestFreeDescRun(),
	       Pool.GetUsedOccWords(), C.OccWordCapacity,
	       double(Pool.GetUsedOccWords()) * 4 / MiB, Pool.GetLargestFreeOccRun(), Pool.GetOccFreeRunCount(),
	       Pool.GetUsedMatWords(), C.MatWordCapacity,
	       double(Pool.GetUsedMatWords()) * 4 / MiB, Pool.GetLargestFreeMatRun(), Pool.GetMatFreeRunCount(),
	       Pool.GetChunksAdded(), Pool.GetChunksAddedFromGpu(), Pool.GetChunksAddedFromCpu(),
	       Pool.GetEvictions(), Pool.GetEvictionsByDistance(),
	       Pool.GetAllocFailures(), Pool.GetWritesDropped(),
	       VoxelBrickGetCpuPackCount(),
	       double(Pool.GetCpuUploadBytes()) / MiB,
	       VoxelBrickPackOnCpuEnabled() ? 1 : 0,
	       VoxelBrickPoolDetail::GVoxelBrickPackReuseMesherVoxels,
	       VoxelBrickSuppressQuadMeshEnabled() ? 1 : 0,
	       VoxelGpuBrickPackEnabled() ? 1 : 0,
	       VoxelTerrainQuadsRetired() ? 1 : 0,
	       VoxelBrickGetCpuMeshCount(), VoxelBrickGetCpuMeshMs(), MeshPerChunk,
	       VoxelBrickGetCpuFillCount(), VoxelBrickGetCpuFillMs(), FillPerChunk,
	       VoxelBrickGetCpuPackCount(), VoxelBrickGetCpuPackMs(), PackPerChunk,
	       VoxelBrickGetCpuPackFromDenseCount(),
	       TotalPerChunk,
	       VoxelBrickGetCpuPackCount(), PackSpanSeconds,
	       PackSpanSeconds > 0.0 ? double(VoxelBrickGetCpuPackCount()) / PackSpanSeconds : 0.0);


	// --- residency by ring level -------------------------------------------
	//
	// THE INPUT TO THE MARCHER SCOPE DECISION. Its first step is level 0 only, so
	// what fraction of residency L0 is decides whether that step covers enough
	// screen to measure anything. docs/gpu-g0-sizing.md says ring chunk counts
	// are flat by construction and R0 is ~80% of resident chunks; the same claim
	// was MEASURED WRONG for quads on 2026-08-19 (R0 18.4%, near-uniform). This
	// prints it for bricks so neither number has to be assumed.
	{
		FVoxelBrickPool::FLevelCensus Census;
		Pool.GetLevelCensus(Census);
		const int32 Total = Pool.GetNumResidentChunks();
		FString Line;
		for (int32 L = 0; L < FVoxelBrickPool::kLevelBuckets; ++L)
		{
			if (Census.Chunks[L] == 0)
			{
				continue;
			}
			Line += FString::Printf(TEXT("L%d %d (%.1f%%, %.1f MiB)  "), L, Census.Chunks[L],
			                        Total > 0 ? 100.0 * double(Census.Chunks[L]) / double(Total) : 0.0,
			                        double(Census.ResidentBytes[L]) / MiB);
		}
		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("  residency by ring level: %s\n")
		       TEXT("    Percentages are of RESIDENT CHUNKS, not of screen. A ring that is a small ")
		       TEXT("share of the count can still be most of what a camera sees, and vice versa -- ")
		       TEXT("this bounds the marcher scope question, it does not answer it."),
		       Line.IsEmpty() ? TEXT("(nothing resident)") : *Line);

		if (Census.OutOfRangeChunks > 0)
		{
			// A key whose level cannot be named by LevelAndFlags [0:3] means the
			// record carries a level that is not this chunk's, which a marcher
			// stepping across rings would read as a different ring entirely.
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("  %d resident chunks have a ring level outside 0..%d. Their records carry a ")
			       TEXT("TRUNCATED level, so a cone march across rings reads them at the wrong scale."),
			       Census.OutOfRangeChunks, FVoxelBrickPool::kLevelBuckets - 1);
		}
	}

	// THE SILENT NO-OP CHECK, and it is here because this project has paid for
	// that failure mode repeatedly: a cvar that is on, a code path that runs, and
	// nothing resident at the end of it. These two counters cannot disagree for
	// any benign reason -- every successful CPU pack is offered to the pool, and
	// the pool either takes it or logs a refusal -- so a gap between them is
	// either a refusal storm (allocFail moved) or publication being off.
	if (VoxelBrickGetCpuPackCount() > 0 && Pool.GetChunksAddedFromCpu() == 0)
	{
		UE_LOG(LogVoxelBrickPool, Warning,
		       TEXT("  %lld chunks were PACKED on the CPU and NONE became resident. Either ")
		       TEXT("voxel.GPU.BrickPackResident is 0 (packed and discarded, which is a deliberate ")
		       TEXT("control arm) or every add was refused — check allocFail above."),
		       VoxelBrickGetCpuPackCount());
	}

	if (Pool.GetEvictions() > 0)
	{
		UE_LOG(LogVoxelBrickPool, Warning,
		       TEXT("  %lld chunks were EVICTED. Resident bytes are then a capacity artefact, not a ")
		       TEXT("census of the world: nothing in the streaming path frees from this pool yet, so ")
		       TEXT("eviction is what bounds it. Do not quote the figure above against the census ")
		       TEXT("without this line."),
		       Pool.GetEvictions());
	}
}));
