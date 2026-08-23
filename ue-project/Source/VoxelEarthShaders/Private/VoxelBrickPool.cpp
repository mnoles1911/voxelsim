#include "VoxelBrickPool.h"
#include "VoxelGpuMeshJobManager.h" // VoxelGpuBrickPackEnabled -- the master brick gate
#include "VoxelGpuWorldGenGraph.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h" // the pack-span clock; see VoxelBrickGetPackSpanSeconds
#include "Misc/CommandLine.h"  // the -ExecCmds startup window; see the note above the accessors
#include "Misc/Parse.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h" // the batched-flush cross-check's async result path
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
	// RAISED 131,072 -> 262,144 ON 2026-08-23, and it is a fix, not a tuning.
	//
	// Raising voxel.Stream.JobsInFlightPerCore 8 -> 24 the same day (it was a
	// per-tick batch quota below the owner's 6,200 chunks/s floor by
	// arithmetic) put ~50% more chunks in flight, and the pool went from
	// evictions=0 to EVICTING AND DROPPING WRITES: measured on flag-free legs,
	// evictions 15,559 and writesDropped 769 on a line flight, 47,647 on a
	// surface flight, with indexEntries pinned at the old 131,072 capacity.
	// `evictions == 0` is a stated gate in this project and writesDropped is
	// lost chunk data -- it showed up as holes 0 -> 8 in the coverage verifier.
	//
	// The owner has approved growing to 1-2 GB VRAM. Committed total roughly
	// doubles with this, to ~776 MiB, which is inside that.
	//
	// THIS IS THE SHAPE TO WATCH, not the number: throughput changes that put
	// more chunks in flight cost pool capacity, and the pool does not complain
	// loudly -- it silently evicts and drops. Read `Voxel brick lifetime`'s
	// evictions/writesDropped after any such change.
	int32 GVoxelBrickPoolChunks = 262144;
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

	// --- voxel.GPU.BrickFlushBatch: fuse the pool flush's per-chunk passes ---
	//
	// WHY THIS EXISTS, IN NUMBERS. Tier B.1 (voxel.GPU.WorldGenBatch) fused
	// worldgen+pack across each Z-stack and CUT PASSES 3.4x (689 chunks in 215
	// stacks, ~3,010 passes vs ~10,335 per-chunk, crosscheck 0 FAIL) -- and
	// throughput did not move (brickPacks 488,408 vs the control's 562,898).
	// So per-chunk pass SETUP upstream of the pool was not the ceiling. What
	// stayed per-chunk was THIS pool's flush: 2 word copies + desc rebase +
	// record, ~4 small dispatches per chunk, on both arms of that A/B. This
	// switch fuses those into ~4 TABLE-DRIVEN dispatches per producing
	// dispatch (per B.1 stack), plus one fused clear pass per flush.
	//
	// WHY THE FUSION UNIT IS "CHUNKS SHARING ONE SCRATCH", NOT "the flush". A
	// compute pass binds fixed SRVs, so one dispatch can only read ONE set of
	// source buffers -- and the chunks of one B.1 stack are exactly the chunks
	// that share a set. Without -VoxelGpuWorldGenBatch=1 every payload has its
	// own buffers, every group is a singleton, and this switch falls back to
	// the classic passes chunk by chunk (counted, see the window line) -- so
	// it only pays off STACKED ON B.1, which is its whole point.
	//
	// DEFAULT 0 AND BYTE-IDENTICAL OFF: with the switch off, AddFlushPasses
	// takes the pre-existing branch verbatim -- same passes, same order, same
	// bytes -- so a control leg needs no rebuild. Command-line override for
	// VoxelBrickPackOnCpuEnabled's -ExecCmds reason (the harness delivers
	// cvars AFTER streaming begins; for this switch that would only blur the
	// counters, not the bytes, but a half-covered leg still misleads whoever
	// reads it). The CVAR is honoured mid-run because both paths emit
	// identical bytes; a flip's only residue is in the stats.
	int32 GVoxelBrickFlushBatch = 0;
	FAutoConsoleVariableRef CVarVoxelBrickFlushBatch(
		TEXT("voxel.GPU.BrickFlushBatch"),
		GVoxelBrickFlushBatch,
		TEXT("1 = fuse the brick pool flush's per-chunk passes (2 word copies + desc rebase + record) ")
		TEXT("into ~4 table-driven dispatches per producing dispatch, and all record clears into one. ")
		TEXT("Only fuses chunks that share one scratch (a voxel.GPU.WorldGenBatch stack); everything ")
		TEXT("else falls back per chunk, counted. 0 (default) = today's per-chunk passes, ")
		TEXT("byte-identical. Live cross-check: one fused group per flush is byte-compared against ")
		TEXT("the classic formula and reported on the [brick-flushbatch] window line."),
		ECVF_Default);

	bool VoxelBrickFlushBatchEnabled()
	{
		// -VoxelGpuBrickFlushBatch=<n> outranks the cvar; -1 means "not given"
		// and the cvar wins, so a run that passes nothing behaves as before.
		static const int32 CmdLine = []
		{
			int32 Value = -1;
			FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuBrickFlushBatch="), Value);
			return Value;
		}();
		return CmdLine >= 0 ? CmdLine != 0 : GVoxelBrickFlushBatch != 0;
	}

	// Dwords per flush-table entry. BOUND to the kernels as FlushTableStride,
	// never restated as a literal there -- the ChunkRecordDwords rule. 32 (two
	// cache lines exactly) rather than the 19 the fields need, for the record
	// format's own 16-not-10 reason: entry-aligned lines, and headroom that
	// does not cost a format change to use. The per-flush cost at the stack
	// cap is 64 entries x 128 B = 8 KiB, i.e. nothing.
	constexpr uint32 kFlushTableStride = 32;
	// Entries one table (and so one fused dispatch set) may carry. A B.1 stack
	// is capped at 64 chunks, so this never binds today; it exists so a future
	// producer that hands out bigger shared-scratch groups slices them instead
	// of building an unbounded table.
	constexpr int32 kFlushTableMaxEntries = 512;

	// --- batched-flush window counters ---------------------------------------
	//
	// Render thread writes (AddFlushPasses records passes there), game thread
	// reads-and-resets (the window line in Flush) -- so atomics, the
	// VoxelGpuBatchDetail pattern. int64 because a long soak overflows int32
	// pass counts.
	std::atomic<int64> GFlushBatchedFlushes{ 0 };  // batched flushes that carried GPU writes
	std::atomic<int64> GFlushFusedGroups{ 0 };     // fused dispatch sets recorded
	std::atomic<int64> GFlushFusedChunks{ 0 };     // chunks those sets covered
	std::atomic<int64> GFlushFusedPasses{ 0 };     // dispatches actually recorded for them
	std::atomic<int64> GFlushEquivPasses{ 0 };     // what the same chunks cost per-chunk
	std::atomic<int64> GFlushFallbackSingle{ 0 };  // singleton group -> classic passes
	std::atomic<int64> GFlushFallbackMixed{ 0 };   // shared Desc but differing Occ/Mat/Mask
	std::atomic<int64> GFlushFallbackInvalid{ 0 }; // payload refused (also logged as a drop)
	std::atomic<int64> GFlushClassicChunks{ 0 };   // chunks that took classic passes while armed
	std::atomic<int64> GFlushClearsFused{ 0 };     // record clears folded into fused passes
	std::atomic<int64> GFlushClearPasses{ 0 };     // fused clear dispatches recorded
	// The cross-check tallies. Ok counts CHUNKS byte-verified clean; a FAIL is
	// one sampled group with any mismatched dword (its own Error log carries
	// the dword count). Pending is samples whose readback has not landed yet,
	// published so the window line can say "no verdict yet" instead of
	// looking like a pass.
	std::atomic<int64> GFlushXchkChunksOk{ 0 };
	std::atomic<int64> GFlushXchkSampleFails{ 0 };
	std::atomic<int64> GFlushXchkSamples{ 0 };
	std::atomic<int32> GFlushXchkPending{ 0 };

	// RENDER THREAD ONLY. One entry per sampled fused group whose verify
	// readback is still in flight. Raw pointers, deleted at harvest: at
	// process exit whatever is still pending is deliberately leaked, because
	// destroying an FRHIGPUBufferReadback from a static destructor on the
	// main thread is the crash-on-exit shape FVoxelGpuBrickPayload's
	// destructor exists to avoid, and a leak of at most a few 8-byte staging
	// buffers at exit is the cheaper end of that trade.
	struct FPendingFlushVerify
	{
		FRHIGPUBufferReadback* Readback = nullptr;
		uint32 ExpectedChunks = 0;
	};
	TArray<FPendingFlushVerify> GPendingFlushVerify;
	// Round-robin over the flush's fused groups, so sampling does not
	// systematically verify whichever group a TMap happened to iterate first.
	uint64 GFlushSampleRotor = 0;

	// Harvests any landed verify results. RENDER THREAD ONLY -- called from
	// inside the flush render command, which is the same serial timeline the
	// readbacks were enqueued on.
	void PollFlushVerifyReadbacks_RenderThread()
	{
		for (int32 I = GPendingFlushVerify.Num() - 1; I >= 0; --I)
		{
			FPendingFlushVerify& Pending = GPendingFlushVerify[I];
			if (Pending.Readback == nullptr)
			{
				GPendingFlushVerify.RemoveAtSwap(I, EAllowShrinking::No);
				continue;
			}
			if (!Pending.Readback->IsReady())
			{
				continue;
			}
			uint32 Result[2] = { 0, 0 };
			if (const void* Src = Pending.Readback->Lock(sizeof(Result)))
			{
				FMemory::Memcpy(Result, Src, sizeof(Result));
				Pending.Readback->Unlock();
			}
			// TWO ways to fail, both counted as FAIL: mismatched bytes, and a
			// verify that checked fewer chunks than were sampled -- the second
			// is the "gate that tested nothing" failure this project has
			// shipped before (vxc_gpu), and it must not read as a pass.
			if (Result[0] == 0 && Result[1] == Pending.ExpectedChunks)
			{
				GFlushXchkChunksOk.fetch_add(int64(Result[1]), std::memory_order_relaxed);
			}
			else
			{
				GFlushXchkSampleFails.fetch_add(1, std::memory_order_relaxed);
				UE_LOG(LogVoxelBrickPool, Error,
				       TEXT("[brick-flushbatch] FLUSH CROSS-CHECK FAILED: %u mismatched dwords over %u ")
				       TEXT("verified chunks (%u sampled). The batched pool write did NOT reproduce the ")
				       TEXT("classic per-chunk bytes -- a wrong table, a wrong prefix sum, or a kernel ")
				       TEXT("bug -- and a wrong pool write is one chunk's bricks at another chunk's ")
				       TEXT("address: plausible terrain, wrong place. Turn voxel.GPU.BrickFlushBatch ")
				       TEXT("off and diff the batched kernels against their classic twins."),
				       Result[0], Result[1], Pending.ExpectedChunks);
			}
			delete Pending.Readback;
			GPendingFlushVerify.RemoveAtSwap(I, EAllowShrinking::No);
			GFlushXchkPending.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	// The [brick-flushbatch] window line. GAME THREAD (called from Flush), ~5 s
	// cadence, the MaybeLogBatchWindow shape: quiet while nothing happened,
	// and when something did, EVERY counter prints -- a fused count without
	// its fallbacks is how a batch that silently declined everything reads as
	// healthy. Read-and-reset with one reader (this function).
	double GFlushBatchWindowStart = 0.0;
	void MaybeLogFlushBatchWindow()
	{
		const double Now = FPlatformTime::Seconds();
		if (GFlushBatchWindowStart <= 0.0)
		{
			GFlushBatchWindowStart = Now;
			return;
		}
		if (Now - GFlushBatchWindowStart < 5.0)
		{
			return;
		}

		const int64 BatchedFlushes = GFlushBatchedFlushes.exchange(0, std::memory_order_relaxed);
		const int64 FusedGroups = GFlushFusedGroups.exchange(0, std::memory_order_relaxed);
		const int64 FusedChunks = GFlushFusedChunks.exchange(0, std::memory_order_relaxed);
		const int64 FusedPasses = GFlushFusedPasses.exchange(0, std::memory_order_relaxed);
		const int64 EquivPasses = GFlushEquivPasses.exchange(0, std::memory_order_relaxed);
		const int64 Single = GFlushFallbackSingle.exchange(0, std::memory_order_relaxed);
		const int64 Mixed = GFlushFallbackMixed.exchange(0, std::memory_order_relaxed);
		const int64 Invalid = GFlushFallbackInvalid.exchange(0, std::memory_order_relaxed);
		const int64 Classic = GFlushClassicChunks.exchange(0, std::memory_order_relaxed);
		const int64 ClearsFused = GFlushClearsFused.exchange(0, std::memory_order_relaxed);
		const int64 ClearPasses = GFlushClearPasses.exchange(0, std::memory_order_relaxed);
		const int64 XchkOk = GFlushXchkChunksOk.exchange(0, std::memory_order_relaxed);
		const int64 XchkFail = GFlushXchkSampleFails.exchange(0, std::memory_order_relaxed);
		const int64 XchkSamples = GFlushXchkSamples.exchange(0, std::memory_order_relaxed);
		const int32 XchkPending = GFlushXchkPending.load(std::memory_order_relaxed);

		if (BatchedFlushes == 0 && FusedGroups == 0 && Single == 0 && Mixed == 0 && Invalid == 0 &&
		    Classic == 0 && ClearsFused == 0 && XchkOk == 0 && XchkFail == 0 && XchkSamples == 0)
		{
			GFlushBatchWindowStart = Now;
			return;
		}

		// "vs ~N per-chunk": what the SAME fused chunks' passes would have
		// cost as classic per-chunk recordings -- the number this switch
		// exists to beat. The xcheck tail is the gate: FAIL > 0 is a wrong
		// world, and "0 ok / 0 FAIL" with fused chunks non-zero means the
		// verify never landed -- an answer of "no verdict", never "pass".
		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("[brick-flushbatch] %.1fs window: %lld flushes, %lld fused groups / %lld chunks ")
		       TEXT("(%.1f per flush) in %lld passes (vs ~%lld per-chunk); fallbacks single %lld ")
		       TEXT("mixed %lld invalid %lld (classic chunks %lld); clears %lld fused into %lld ")
		       TEXT("passes; xcheck %lld chunks ok / %lld sample FAIL (%lld samples, %d pending)"),
		       Now - GFlushBatchWindowStart, BatchedFlushes, FusedGroups, FusedChunks,
		       BatchedFlushes > 0 ? double(FusedChunks) / double(BatchedFlushes) : 0.0,
		       FusedPasses, EquivPasses,
		       Single, Mixed, Invalid, Classic, ClearsFused, ClearPasses,
		       XchkOk, XchkFail, XchkSamples, XchkPending);
		GFlushBatchWindowStart = Now;
	}

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

	// --- P1: voxel.GPU.PoolAlloc -------------------------------------------
	//
	// The switch itself. Default 0 = today's behaviour byte-for-byte: CPU
	// allocation from the per-chunk totals readback, so a control leg needs no
	// rebuild. See the accessor's declaration in VoxelBrickPool.h for why this
	// one is LATCHED at first call rather than staying live -- arming decides
	// who owns the arena words for the whole process.
	int32 GVoxelGpuPoolAlloc = 0;
	FAutoConsoleVariableRef CVarVoxelGpuPoolAlloc(
		TEXT("voxel.GPU.PoolAlloc"),
		GVoxelGpuPoolAlloc,
		TEXT("P1 of the GPU streaming architecture. 1 = brick-pool arena ranges are claimed by the ")
		TEXT("GENERATION GRAPH (GPU-side size-class allocator; no per-chunk totals readback, which is ")
		TEXT("the fence the GPU arm queues behind); the CPU producer claims through the same GPU ")
		TEXT("allocator in the pool flush. 0 (default) = today's CPU allocation from the readback, ")
		TEXT("byte-identical. LATCHED at pool Init -- use -VoxelGpuPoolAlloc=1 on the command line for ")
		TEXT("legs; a cvar flip after Init is IGNORED (and says so), because mid-run re-arming would ")
		TEXT("put two allocators over one range. Live cross-checks report on [brick-gpualloc]."),
		ECVF_Default);

	// --- [brick-gpualloc] window state ----------------------------------------
	//
	// Same shape as the flush-batch window above: render thread lands async
	// readbacks into atomics, game thread prints every ~5 s. The GPU counters
	// are CUMULATIVE (the state buffer is never reset), so the FAIL detectors
	// below compare against the last landed value -- a counter that moved is a
	// NEW failure, logged at Error; a counter that is merely non-zero was
	// already reported.
	std::atomic<int64> GAllocSnapClaims{ 0 };
	std::atomic<int64> GAllocSnapStackPops{ 0 };
	std::atomic<int64> GAllocSnapFrees{ 0 };
	std::atomic<int64> GAllocSnapClaimFailOcc{ 0 };
	std::atomic<int64> GAllocSnapClaimFailMat{ 0 };
	std::atomic<int64> GAllocSnapClaimFailWorst{ 0 };
	std::atomic<int64> GAllocSnapBitmapCollision{ 0 };
	std::atomic<int64> GAllocSnapFreeMissing{ 0 };
	std::atomic<int64> GAllocSnapPushOverflow{ 0 };
	std::atomic<int64> GAllocSnapOccBump{ 0 };
	std::atomic<int64> GAllocSnapMatBump{ 0 };
	std::atomic<int64> GAllocSnapOccInFlight{ 0 };
	std::atomic<int64> GAllocSnapMatInFlight{ 0 };
	std::atomic<int64> GAllocSnapOccPaddedCum{ 0 };
	std::atomic<int64> GAllocSnapOccActualCum{ 0 };
	std::atomic<int64> GAllocSnapMatPaddedCum{ 0 };
	std::atomic<int64> GAllocSnapMatActualCum{ 0 };
	std::atomic<int64> GAllocSnapStrandedOccDwords{ 0 };
	std::atomic<int64> GAllocSnapStrandedMatDwords{ 0 };
	std::atomic<int32> GAllocCountersLanded{ 0 };
	// The sampled verify tallies (cumulative across windows, reset never --
	// they are verdict counters, and 0 ok / 0 FAIL with samples pending must
	// read as "no verdict yet").
	std::atomic<int64> GAllocXchkOk{ 0 };
	std::atomic<int64> GAllocXchkFails{ 0 };
	std::atomic<int64> GAllocXchkUnwritten{ 0 };
	std::atomic<int64> GAllocXchkSamples{ 0 };
	std::atomic<int32> GAllocXchkPending{ 0 };

	// Counter indices in the state buffer -- MIRROR of the kCtr* list in
	// VoxelBrickPoolAlloc.usf. Indices, not meanings, are the contract.
	constexpr int32 kGpuAllocCtrOccBump = 0;
	constexpr int32 kGpuAllocCtrMatBump = 1;
	constexpr int32 kGpuAllocCtrClaims = 2;
	constexpr int32 kGpuAllocCtrStackPops = 3;
	constexpr int32 kGpuAllocCtrClaimFailOcc = 4;
	constexpr int32 kGpuAllocCtrClaimFailMat = 5;
	constexpr int32 kGpuAllocCtrClaimFailWorst = 6;
	constexpr int32 kGpuAllocCtrBitmapCollision = 7;
	constexpr int32 kGpuAllocCtrFrees = 8;
	constexpr int32 kGpuAllocCtrFreePushOverflow = 9;
	constexpr int32 kGpuAllocCtrFreeBitmapMissing = 10;
	constexpr int32 kGpuAllocCtrOccInFlight = 11;
	constexpr int32 kGpuAllocCtrMatInFlight = 12;
	constexpr int32 kGpuAllocCtrOccPaddedCum = 13;
	constexpr int32 kGpuAllocCtrOccActualCum = 14;
	constexpr int32 kGpuAllocCtrMatPaddedCum = 15;
	constexpr int32 kGpuAllocCtrMatActualCum = 16;
	// How many state dwords the counter readback copies: the counter block plus
	// both stack-top arrays, which is what the stranded-dwords figures need.
	// Kept in one number so the readback and the harvest cannot disagree.
	constexpr uint32 kGpuAllocCounterReadDwords = 96;

	// RENDER THREAD ONLY, the GPendingFlushVerify shape (and the same
	// deliberate leak-at-exit trade -- see that struct's comment).
	struct FPendingAllocVerify
	{
		FRHIGPUBufferReadback* Readback = nullptr;
		uint32 ExpectedChunks = 0;
	};
	TArray<FPendingAllocVerify> GPendingAllocVerify;
	struct FPendingAllocCounters
	{
		FRHIGPUBufferReadback* Readback = nullptr;
		// The layout the readback was taken under, for the stack-top walk.
		uint32 OccTopsFirst = 0, OccClasses = 0, OccClassStep = 0;
		uint32 MatTopsFirst = 0, MatClasses = 0, MatClassStep = 0;
	};
	TArray<FPendingAllocCounters> GPendingAllocCounters;

	void PollGpuAllocReadbacks_RenderThread()
	{
		for (int32 I = GPendingAllocVerify.Num() - 1; I >= 0; --I)
		{
			FPendingAllocVerify& Pending = GPendingAllocVerify[I];
			if (Pending.Readback == nullptr)
			{
				GPendingAllocVerify.RemoveAtSwap(I, EAllowShrinking::No);
				continue;
			}
			if (!Pending.Readback->IsReady())
			{
				continue;
			}
			uint32 Result[3] = { 0, 0, 0 };
			if (const void* Src = Pending.Readback->Lock(sizeof(Result)))
			{
				FMemory::Memcpy(Result, Src, sizeof(Result));
				Pending.Readback->Unlock();
			}
			GAllocXchkSamples.fetch_add(int64(Pending.ExpectedChunks), std::memory_order_relaxed);
			GAllocXchkUnwritten.fetch_add(int64(Result[2]), std::memory_order_relaxed);
			if (Result[0] == 0 && Result[1] == Pending.ExpectedChunks)
			{
				GAllocXchkOk.fetch_add(int64(Result[1]) - int64(Result[2]), std::memory_order_relaxed);
			}
			else
			{
				GAllocXchkFails.fetch_add(1, std::memory_order_relaxed);
				UE_LOG(LogVoxelBrickPool, Error,
				       TEXT("[brick-gpualloc] ALLOCATOR CROSS-CHECK FAILED: %u mismatched values over %u ")
				       TEXT("verified chunks (%u sampled, %u unwritten). A GPU-claimed chunk's record, ")
				       TEXT("side-table range or bitmap ownership disagrees with what the CPU allocated ")
				       TEXT("the slot for -- which is one chunk's bricks at another chunk's address: ")
				       TEXT("plausible terrain, wrong place. Turn voxel.GPU.PoolAlloc off (relaunch ")
				       TEXT("without -VoxelGpuPoolAlloc=1) and compare the claim kernel against ")
				       TEXT("AllocateForChunk."),
				       Result[0], Result[1], Pending.ExpectedChunks, Result[2]);
			}
			delete Pending.Readback;
			GPendingAllocVerify.RemoveAtSwap(I, EAllowShrinking::No);
			GAllocXchkPending.fetch_sub(1, std::memory_order_relaxed);
		}

		for (int32 I = GPendingAllocCounters.Num() - 1; I >= 0; --I)
		{
			FPendingAllocCounters& Pending = GPendingAllocCounters[I];
			if (Pending.Readback == nullptr)
			{
				GPendingAllocCounters.RemoveAtSwap(I, EAllowShrinking::No);
				continue;
			}
			if (!Pending.Readback->IsReady())
			{
				continue;
			}
			uint32 C[kGpuAllocCounterReadDwords] = {};
			if (const void* Src = Pending.Readback->Lock(sizeof(C)))
			{
				FMemory::Memcpy(C, Src, sizeof(C));
				Pending.Readback->Unlock();
			}
			// NEW failures, not merely non-zero ones: the counters are
			// cumulative, so the delta against the last landed value is what
			// gets announced. Announce BEFORE publishing the snapshot.
			const int64 PrevCollision = GAllocSnapBitmapCollision.load(std::memory_order_relaxed);
			const int64 PrevMissing = GAllocSnapFreeMissing.load(std::memory_order_relaxed);
			if (int64(C[kGpuAllocCtrBitmapCollision]) > PrevCollision)
			{
				UE_LOG(LogVoxelBrickPool, Error,
				       TEXT("[brick-gpualloc] DOUBLE GRANT: the page bitmap caught %lld new collision(s) ")
				       TEXT("(total %u). The GPU allocator handed out dwords somebody already holds; the ")
				       TEXT("colliding claims were FAILED and their ranges leaked rather than written. ")
				       TEXT("This is the allocator's own correctness gate firing -- treat the leg as ")
				       TEXT("invalid and diff the claim kernel."),
				       int64(C[kGpuAllocCtrBitmapCollision]) - PrevCollision, C[kGpuAllocCtrBitmapCollision]);
			}
			if (int64(C[kGpuAllocCtrFreeBitmapMissing]) > PrevMissing)
			{
				UE_LOG(LogVoxelBrickPool, Error,
				       TEXT("[brick-gpualloc] BAD FREE: %lld new page(s) freed that were not marked owned ")
				       TEXT("(total %u). A range was freed twice or never granted -- the free list may now ")
				       TEXT("hold a live chunk's dwords, which resurfaces as terrain corruption on reuse."),
				       int64(C[kGpuAllocCtrFreeBitmapMissing]) - PrevMissing, C[kGpuAllocCtrFreeBitmapMissing]);
			}
			GAllocSnapOccBump.store(int64(C[kGpuAllocCtrOccBump]), std::memory_order_relaxed);
			GAllocSnapMatBump.store(int64(C[kGpuAllocCtrMatBump]), std::memory_order_relaxed);
			GAllocSnapClaims.store(int64(C[kGpuAllocCtrClaims]), std::memory_order_relaxed);
			GAllocSnapStackPops.store(int64(C[kGpuAllocCtrStackPops]), std::memory_order_relaxed);
			GAllocSnapClaimFailOcc.store(int64(C[kGpuAllocCtrClaimFailOcc]), std::memory_order_relaxed);
			GAllocSnapClaimFailMat.store(int64(C[kGpuAllocCtrClaimFailMat]), std::memory_order_relaxed);
			GAllocSnapClaimFailWorst.store(int64(C[kGpuAllocCtrClaimFailWorst]), std::memory_order_relaxed);
			GAllocSnapBitmapCollision.store(int64(C[kGpuAllocCtrBitmapCollision]), std::memory_order_relaxed);
			GAllocSnapFrees.store(int64(C[kGpuAllocCtrFrees]), std::memory_order_relaxed);
			GAllocSnapPushOverflow.store(int64(C[kGpuAllocCtrFreePushOverflow]), std::memory_order_relaxed);
			GAllocSnapFreeMissing.store(int64(C[kGpuAllocCtrFreeBitmapMissing]), std::memory_order_relaxed);
			GAllocSnapOccInFlight.store(int64(C[kGpuAllocCtrOccInFlight]), std::memory_order_relaxed);
			GAllocSnapMatInFlight.store(int64(C[kGpuAllocCtrMatInFlight]), std::memory_order_relaxed);
			GAllocSnapOccPaddedCum.store(int64(C[kGpuAllocCtrOccPaddedCum]), std::memory_order_relaxed);
			GAllocSnapOccActualCum.store(int64(C[kGpuAllocCtrOccActualCum]), std::memory_order_relaxed);
			GAllocSnapMatPaddedCum.store(int64(C[kGpuAllocCtrMatPaddedCum]), std::memory_order_relaxed);
			GAllocSnapMatActualCum.store(int64(C[kGpuAllocCtrMatActualCum]), std::memory_order_relaxed);
			// Stranded memory: what sits parked in free stacks. Depth x class
			// size, per class, both arenas -- the honest cost of the no-coalesce
			// design, printed rather than argued about.
			int64 StrandedOcc = 0;
			for (uint32 K = 0; K < Pending.OccClasses && Pending.OccTopsFirst + K < kGpuAllocCounterReadDwords; ++K)
			{
				StrandedOcc += int64(C[Pending.OccTopsFirst + K]) * int64((K + 1) * Pending.OccClassStep);
			}
			int64 StrandedMat = 0;
			for (uint32 K = 0; K < Pending.MatClasses && Pending.MatTopsFirst + K < kGpuAllocCounterReadDwords; ++K)
			{
				StrandedMat += int64(C[Pending.MatTopsFirst + K]) * int64((K + 1) * Pending.MatClassStep);
			}
			GAllocSnapStrandedOccDwords.store(StrandedOcc, std::memory_order_relaxed);
			GAllocSnapStrandedMatDwords.store(StrandedMat, std::memory_order_relaxed);
			GAllocCountersLanded.store(1, std::memory_order_relaxed);
			delete Pending.Readback;
			GPendingAllocCounters.RemoveAtSwap(I, EAllowShrinking::No);
		}
	}

	// The window cadence. Game thread (Flush).
	double GAllocWindowStart = 0.0;
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

bool VoxelGpuPoolAllocEnabled()
{
	// LATCHED, cvar included -- see the declaration. The command line outranks
	// the cvar; -1 means "not given" and the cvar's value AT FIRST CALL wins.
	// First call happens at pool Init (or a producer's first gate read), both
	// before any claim could have been enqueued, so the latched value is the
	// value every claim and every arena decision ran under.
	static const bool bEnabled = []
	{
		int32 Value = -1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelGpuPoolAlloc="), Value);
		return Value >= 0 ? Value != 0 : VoxelBrickPoolDetail::GVoxelGpuPoolAlloc != 0;
	}();
	return bEnabled;
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

		// --- hierarchical coverage headroom (Phase 1 of the no-hole plan) ---
		//
		// -VoxelHierarchicalCoverage makes every coarse streaming level cover
		// the ground inside it (VoxelWorldSubsystem.cpp, VoxelStreamAdmission::
		// HierarchicalCoverageEnabled -- READ FROM THE COMMAND LINE HERE TOO,
		// deliberately, because this module must not depend on VoxelEarth; the
		// flag string is the shared contract). Measured per-level residency
		// (MODEFP, settled 4 km cascade: 96,814 chunks) projects to ~120,350
		// under full coverage -- 92% of the default 131,072 slots, and peak
		// residency under flight sits above settled. The pool evicts
		// farthest-from-focus on pressure and GetEvictions() == 0 is a stated
		// gate, so running the switch against the default pool would fail its
		// own gate by construction. The switch therefore brings its own
		// headroom: 262,144 slots. Committed cost at that size: descriptors
		// 128 MiB + records 8 MiB on top of the unchanged occupancy/material
		// arenas (those are paid per MIXED brick, not per slot) -- inside the
		// owner's approved 1-2 GB envelope.
		//
		// EXPLICIT AND REVERSIBLE, not a default change: without the flag this
		// block does not run and the pool is byte-identical to today. An
		// explicit -VoxelBrickPoolChunks=N outranks both the default and the
		// hierarchical raise (that is the right-sizing knob for step 4 of the
		// plan, and the pre-existing cvar path stays: voxel.Brick.PoolChunks
		// is ECVF_ReadOnly and read once, above). Command line and not a
		// console command because this is read ONCE, here, and a resize after
		// Init is refused -- see the guard at the top of this function.
		int32 ExplicitChunks = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelBrickPoolChunks="), ExplicitChunks) &&
		    ExplicitChunks > 0)
		{
			Config.ChunkCapacity = uint32(ExplicitChunks);
			UE_LOG(LogVoxelBrickPool, Log,
			       TEXT("FVoxelBrickPool: chunk capacity %u from -VoxelBrickPoolChunks="),
			       Config.ChunkCapacity);
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("VoxelHierarchicalCoverage")))
		{
			Config.ChunkCapacity = FMath::Max(Config.ChunkCapacity, 262144u);
			UE_LOG(LogVoxelBrickPool, Log,
			       TEXT("FVoxelBrickPool: chunk capacity raised to %u for -VoxelHierarchicalCoverage ")
			       TEXT("(projected residency ~120,350 of the default 131,072 = 92%%, and the ")
			       TEXT("evictions==0 gate must hold)."),
			       Config.ChunkCapacity);
		}
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

	// --- P1: arm the GPU allocator (voxel.GPU.PoolAlloc) --------------------
	//
	// ONE allocator over the WHOLE of each word arena -- the owner's decision.
	// The CPU Occ/MatArena objects above are still initialised but are NEVER
	// ALLOCATED FROM while armed (AddChunkFromGpu refuses outright; the CPU
	// producer routes through the GPU claim in Flush), so GetUsedOccWords /
	// GetUsedMatWords read 0 on an armed pool and the [brick-gpualloc] window
	// line carries the real usage. Descriptor slots and record slots stay on
	// DescArena for both producers -- fixed size, one dispenser, CPU-visible
	// identity for eviction and the index.
	// GLOBAL POOL ONLY. voxel.GPU.VerifyBrickPack stands up PRIVATE pools to
	// verify against, and those must keep the classic CPU allocator whatever
	// the switch says -- an armed private pool would refuse AddChunkFromGpu and
	// the gate would report a failure that is really a configuration collision.
	// (Calling GetGlobalVoxelBrickPool here is safe: on the global instance the
	// static is already constructed by the time any member function runs.)
	bGpuAllocArmed = VoxelGpuPoolAllocEnabled() && (this == &GetGlobalVoxelBrickPool());
	if (bGpuAllocArmed)
	{
		FVoxelBrickPoolAllocLayout& L = GpuAllocLayout;
		L.OccRegionFirst = 0;
		L.OccRegionWords = Config.OccWordCapacity;
		L.MatRegionFirst = 0;
		L.MatRegionWords = Config.MatWordCapacity;
		L.OccClassStep = kGpuAllocOccClassStep;
		L.MatClassStep = kGpuAllocMatClassStep;
		// Classes cover the per-chunk worst case (64 bricks x 16 occ / 132 mat
		// dwords), computed rather than restated so a brick-format change moves
		// the class count with it.
		L.OccClasses = (VoxelBrickPoolDetail::kBricksPerChunk * 16u + L.OccClassStep - 1u) / L.OccClassStep;
		L.MatClasses = (VoxelBrickPoolDetail::kBricksPerChunk * 132u + L.MatClassStep - 1u) / L.MatClassStep;
		L.FreeStackCap = kGpuAllocFreeStackCap;
		// State-buffer map: [0..63] counters, then the two top arrays, then the
		// two storage arrays. The counter block width is part of the readback
		// contract (kGpuAllocCounterReadDwords) -- it must cover the tops.
		L.OccTopsFirst = 64;
		L.MatTopsFirst = L.OccTopsFirst + L.OccClasses;
		L.OccStackFirst = L.MatTopsFirst + L.MatClasses;
		L.MatStackFirst = L.OccStackFirst + L.OccClasses * L.FreeStackCap;
		L.StateDwords = L.MatStackFirst + L.MatClasses * L.FreeStackCap;
		// One bit per class-step page, both arenas, 32 pages per dword.
		const uint32 OccPages = (L.OccRegionWords + L.OccClassStep - 1u) / L.OccClassStep;
		const uint32 MatPages = (L.MatRegionWords + L.MatClassStep - 1u) / L.MatClassStep;
		L.OccBitmapFirst = 0;
		L.MatBitmapFirst = (OccPages + 31u) / 32u;
		L.BitmapDwords = L.MatBitmapFirst + (MatPages + 31u) / 32u;
		L.SideDwords = Config.ChunkCapacity * 4u;

		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("FVoxelBrickPool: GPU allocator ARMED (voxel.GPU.PoolAlloc). One allocator over both ")
		       TEXT("word arenas: occ %u dwords in %u classes of %u, mat %u dwords in %u classes of %u; ")
		       TEXT("free stacks %u deep; state %u + bitmap %u + side %u dwords (%.1f MiB overhead). The ")
		       TEXT("per-chunk totals readback does not run for GPU-claimed chunks; correctness is watched ")
		       TEXT("live on [brick-gpualloc] and any FAIL there invalidates the run."),
		       L.OccRegionWords, L.OccClasses, L.OccClassStep,
		       L.MatRegionWords, L.MatClasses, L.MatClassStep,
		       L.FreeStackCap, L.StateDwords, L.BitmapDwords, L.SideDwords,
		       double(uint64(L.StateDwords + L.BitmapDwords + L.SideDwords) * 4) / (1024.0 * 1024.0));
	}

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
		// P1: zero when the GPU allocator is not armed, which is also what makes
		// EnsureCreated skip the three allocator buffers on an unarmed pool.
		Buffers->AllocStateDwords = bGpuAllocArmed ? GpuAllocLayout.StateDwords : 0;
		Buffers->AllocBitmapDwords = bGpuAllocArmed ? GpuAllocLayout.BitmapDwords : 0;
		Buffers->AllocSideDwords = bGpuAllocArmed ? GpuAllocLayout.SideDwords : 0;
	}
	return Buffers;
}

// RENDER THREAD ONLY. Creates the arenas (and, when armed, the allocator's
// three buffers) if they do not exist yet. Factored out of the flush command
// because the GENERATION graph can now touch the pool before the first flush
// ever runs -- see the declaration.
void FVoxelBrickPool::EnsureCreated_RenderThread(FRHICommandListImmediate& RHICmdList,
                                                 const FVoxelBrickPoolBuffersRef& Buffers)
{
	if (!Buffers.IsValid())
	{
		return;
	}
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

	// P1: the allocator's own buffers, only on an armed pool. Zero-initialised
	// IS the allocator's initial state -- see the holder's comment.
	if (Buffers->AllocStateDwords > 0 && !Buffers->HasGpuAlloc())
	{
		Buffers->AllocStateBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.AllocState"),
		                                          sizeof(uint32), Buffers->AllocStateDwords);
		Buffers->AllocBitmapBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.AllocBitmap"),
		                                           sizeof(uint32), Buffers->AllocBitmapDwords);
		Buffers->AllocSideBuffer = VoxelBrickPoolDetail::CreateArenaBuffer(RHICmdList, TEXT("VoxelBrickPool.AllocSide"),
		                                         sizeof(uint32), Buffers->AllocSideDwords);
		Buffers->AllocStatePooled = new FRDGPooledBuffer(
			RHICmdList, Buffers->AllocStateBuffer,
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Buffers->AllocStateDwords),
			Buffers->AllocStateDwords, TEXT("VoxelBrickPool.AllocState"));
		Buffers->AllocBitmapPooled = new FRDGPooledBuffer(
			RHICmdList, Buffers->AllocBitmapBuffer,
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Buffers->AllocBitmapDwords),
			Buffers->AllocBitmapDwords, TEXT("VoxelBrickPool.AllocBitmap"));
		Buffers->AllocSidePooled = new FRDGPooledBuffer(
			RHICmdList, Buffers->AllocSideBuffer,
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Buffers->AllocSideDwords),
			Buffers->AllocSideDwords, TEXT("VoxelBrickPool.AllocSide"));
	}
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

	// P1: a GPU-allocated chunk's word ranges are returned BY A GPU PASS, fed
	// from the side table -- the CPU never learned them and its word fields
	// here are zeros, not sizes. The descriptor block above was CPU-allocated
	// and is freed normally. Anything still queued for this slot is dropped
	// with it: a CPU-producer claim (PendingGpuCpuWrites) that ran after the
	// free would repopulate a record the resident map no longer owns, and a
	// pending index add would announce a chunk that will never land.
	if (Chunk.bGpuArenas)
	{
		for (int32 I = PendingGpuCpuWrites.Num() - 1; I >= 0; --I)
		{
			if (PendingGpuCpuWrites[I].ChunkSlot == Chunk.ChunkSlot)
			{
				PendingGpuCpuWrites.RemoveAt(I, EAllowShrinking::No);
				++WritesDropped;
			}
		}
		for (int32 I = PendingGpuIndexAdds.Num() - 1; I >= 0; --I)
		{
			if (PendingGpuIndexAdds[I].ChunkSlot == Chunk.ChunkSlot)
			{
				PendingGpuIndexAdds.RemoveAt(I, EAllowShrinking::No);
			}
		}
		PendingGpuFreeSlots.Add(Chunk.ChunkSlot);
		++GpuFreesQueued;
		return;
	}

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
			// P1: a GPU-allocated chunk's record is zeroed by the FREE PASS
			// (BrickPoolFreeMain) rather than by the classic clear -- one
			// mechanism per slot, so the two can never race over who zeroes.
			if (!Found->bGpuArenas)
			{
				PendingClears.Add(Found->ChunkSlot);
			}
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
	// P1: REFUSED on an armed pool, and loudly. This path allocates occ/mat
	// words from the CPU arenas, and while voxel.GPU.PoolAlloc is armed those
	// words belong to the GPU allocator -- a CPU grant here could overlap a GPU
	// claim, which is precisely the corruption the one-allocator rule exists to
	// make impossible. Nothing is supposed to reach this on an armed pool
	// (GPU-claimed jobs never publish a payload; the pack-and-discard arm never
	// publishes at all), so every hit is a wiring bug worth a line each.
	if (bGpuAllocArmed)
	{
		++GpuFallbackShellRefused;
		++AllocFailures;
		UE_LOG(LogVoxelBrickPool, Error,
		       TEXT("AddChunkFromGpu called for chunk (%d,%d,%d) L%d while voxel.GPU.PoolAlloc is armed. ")
		       TEXT("Refused: the CPU word arenas are not this pool's allocator any more. The chunk has ")
		       TEXT("no resident volume; find the producer that still takes the readback path."),
		       Key.X, Key.Y, Key.Z, Key.Level);
		return INDEX_NONE;
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

	// P1: on an armed pool the CPU producer claims through the GPU allocator --
	// the one-allocator rule. The CPU half here is only the SHELL (descriptor
	// block + record slot); the words are claimed by the same claim kernel the
	// GPU producer uses, in Flush's render command, over an UPLOADED copy of
	// this pack. Slower than the unarmed Lock/Memcpy path and deliberately so:
	// edits and cold fallback are off the steady-state hot loop, and what this
	// buys is that no dword can ever have two owners.
	if (bGpuAllocArmed)
	{
		FResidentChunk Shell;
		if (!AllocateGpuChunkShell(Key, Pack->OriginVoxel, Shell))
		{
			return INDEX_NONE;
		}
		++ChunksAddedFromCpu;
		if (Shading.IsNeutral())
		{
			++ChunksAddedNeutralShading;
		}
		FPendingGpuCpuWrite Write;
		Write.Pack = Pack;
		Write.Key = Key;
		Write.ChunkSlot = Shell.ChunkSlot;
		Write.BrickBase = Shell.BrickBase;
		Write.RingLevel = uint32(FMath::Clamp(Key.Level, 0, 15));
		Write.Shading = Shading;
		Write.OriginVoxel = Pack->OriginVoxel;
		PendingGpuCpuWrites.Add(MoveTemp(Write));
		return int32(Shell.ChunkSlot);
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
	// P1: the free pass zeroes a GPU-allocated chunk's record; see EvictOne.
	if (!Found->bGpuArenas)
	{
		PendingClears.Add(Slot);
	}
	// P3, in lockstep with the clear, exactly as eviction does it. NOTHING IN
	// STREAMING CALLS THIS YET -- which is precisely why the index consumer must
	// implement removal before it is ever exercised, rather than after.
	PendingIndexRemovals.Add(FVoxelBrickIndexEntry{ Key, Slot });
	NoteResidentDelta(Key, -1);
	Resident.Remove(Key);
	return true;
}

// --- P1: GPU-side pool allocation (voxel.GPU.PoolAlloc) ----------------------

bool FVoxelBrickPool::AllocateGpuChunkShell(const FVoxelBrickChunkKey& Key,
                                            const FIntVector& OriginVoxel,
                                            FResidentChunk& OutChunk)
{
	if (!bInitialised)
	{
		Init(FVoxelBrickPoolConfig{});
	}
	if (!bGpuAllocArmed)
	{
		// A caller that reached for the GPU path on an unarmed pool has read a
		// different gate than Init did -- the half-covered state every accessor
		// comment in this file warns about. Refuse rather than invent ranges.
		UE_LOG(LogVoxelBrickPool, Error,
		       TEXT("AllocateGpuChunkShell for chunk (%d,%d,%d) L%d on an UNARMED pool -- the caller's ")
		       TEXT("gate and Init's disagree. Refused."),
		       Key.X, Key.Y, Key.Z, Key.Level);
		return false;
	}

	// OccWords/MatWords 0: the shell is the descriptor block and the record
	// slot only. Everything else AllocateForChunk does -- re-add-frees-first,
	// evict-until-it-fits, the resident record, the eviction order -- is wanted
	// here unchanged, which is why this is a thin wrapper and not a fork.
	if (!AllocateForChunk(Key, 0, 0, OutChunk))
	{
		++GpuFallbackShellRefused;
		return false;
	}
	FResidentChunk* Entry = Resident.Find(Key);
	Entry->bGpuArenas = true;
	OutChunk.bGpuArenas = true;

	// The index learns about the chunk on the next Flush, which runs after the
	// caller has enqueued the graph that writes the record -- the seam's
	// ordering guarantee, kept on this path by construction.
	PendingGpuIndexAdds.Add(FVoxelBrickIndexEntry{ Key, OutChunk.ChunkSlot });
	++GpuShellsAllocated;

	// The verify ring. Fixed capacity, overwrite-oldest.
	constexpr int32 kRingCap = 64;
	FRecentGpuShell Recent;
	Recent.Key = Key;
	Recent.ChunkSlot = OutChunk.ChunkSlot;
	Recent.OriginVoxel = OriginVoxel;
	if (RecentGpuShells.Num() < kRingCap)
	{
		RecentGpuShells.Add(Recent);
	}
	else
	{
		RecentGpuShells[RecentGpuShellCursor % kRingCap] = Recent;
	}
	RecentGpuShellCursor = (RecentGpuShellCursor + 1) % kRingCap;
	return true;
}

void FVoxelBrickPool::ReleaseGpuChunkShell(const FVoxelBrickChunkKey& Key, uint32 ExpectSlot)
{
	if (const FResidentChunk* Found = Resident.Find(Key))
	{
		// The slot guard: a re-add may have replaced this shell already, and the
		// replacement freed the old one -- releasing the NEW tenant on behalf of
		// a dead job would evict a healthy chunk.
		if (Found->bGpuArenas && Found->ChunkSlot == ExpectSlot)
		{
			RemoveChunk(Key);
		}
	}
}

void FVoxelBrickPool::NoteGpuAllocFallback(EGpuAllocFallback Reason)
{
	switch (Reason)
	{
	case EGpuAllocFallback::Stacked:      ++GpuFallbackStacked; break;
	case EGpuAllocFallback::Discard:      ++GpuFallbackDiscard; break;
	case EGpuAllocFallback::ShellRefused: ++GpuFallbackShellRefused; break;
	}
}

void FVoxelBrickPool::FlushPendingGpuFrees()
{
	if (PendingGpuFreeSlots.Num() == 0)
	{
		return;
	}

	TArray<uint32> Slots = MoveTemp(PendingGpuFreeSlots);
	PendingGpuFreeSlots.Reset();
	const FVoxelBrickPoolAllocLayout Layout = GpuAllocLayout;

	ENQUEUE_RENDER_COMMAND(VoxelBrickPoolGpuFree)(
		[Buffers = GetOrCreateBuffers(), Slots = MoveTemp(Slots), Layout](FRHICommandListImmediate& RHICmdList)
	{
		EnsureCreated_RenderThread(RHICmdList, Buffers);
		if (!Buffers.IsValid() || !Buffers->IsValid() || !Buffers->HasGpuAlloc())
		{
			// Nothing was ever claimed if the buffers do not exist, so there is
			// nothing to free -- but creation FAILING after claims ran would be
			// a leak, and it is worth a line.
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("[brick-gpualloc] free of %d slot(s) dropped: allocator buffers unavailable."),
			       Slots.Num());
			return;
		}

		FRDGBuilder GraphBuilder(RHICmdList);
		{
			RDG_EVENT_SCOPE(GraphBuilder, "VoxelBrickPool.GpuFree(%d slots)", Slots.Num());
			VoxelGpuWorldGen::FBrickPoolAllocBuffers AB;
			AB.PoolDesc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
			AB.PoolOcc = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
			AB.PoolMat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
			AB.PoolTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled, TEXT("VoxelBrickPool.ChunkTable"));
			AB.AllocState = GraphBuilder.RegisterExternalBuffer(Buffers->AllocStatePooled, TEXT("VoxelBrickPool.AllocState"));
			AB.AllocBitmap = GraphBuilder.RegisterExternalBuffer(Buffers->AllocBitmapPooled, TEXT("VoxelBrickPool.AllocBitmap"));
			AB.AllocSide = GraphBuilder.RegisterExternalBuffer(Buffers->AllocSidePooled, TEXT("VoxelBrickPool.AllocSide"));

			FRDGBufferRef SlotList = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.BrickPoolFreeSlots"), sizeof(uint32),
				Slots.Num(), Slots.GetData(), Slots.Num() * sizeof(uint32));
			VoxelGpuWorldGen::AddBrickPoolFreePass(GraphBuilder, AB, Layout, SlotList, uint32(Slots.Num()));
		}
		GraphBuilder.Execute();
	});
}

void FVoxelBrickPool::MaybePumpGpuAllocWindow()
{
	using namespace VoxelBrickPoolDetail;

	const double Now = FPlatformTime::Seconds();
	if (GAllocWindowStart <= 0.0)
	{
		GAllocWindowStart = Now;
		return;
	}
	if (Now - GAllocWindowStart < 5.0)
	{
		return;
	}
	GAllocWindowStart = Now;

	// The latch promise: a cvar flip after Init does nothing, and SAYS so once
	// rather than leaving someone to discover their control leg was not one.
	static bool bWarnedFlip = false;
	if (!bWarnedFlip && ((GVoxelGpuPoolAlloc != 0) != bGpuAllocArmed))
	{
		bWarnedFlip = true;
		UE_LOG(LogVoxelBrickPool, Warning,
		       TEXT("[brick-gpualloc] voxel.GPU.PoolAlloc was flipped after Init and is IGNORED -- the ")
		       TEXT("allocator armed state is latched per process (armed=%d). Relaunch with ")
		       TEXT("-VoxelGpuPoolAlloc=%d to actually change it."),
		       bGpuAllocArmed ? 1 : 0, bGpuAllocArmed ? 0 : 1);
	}

	// --- sample the verify ring ---------------------------------------------
	TArray<uint32> Expect;
	int32 Samples = 0;
	for (int32 I = 0; I < RecentGpuShells.Num() && Samples < 8; ++I)
	{
		const FRecentGpuShell& Shell = RecentGpuShells[I];
		const FResidentChunk* Entry = Resident.Find(Shell.Key);
		if (Entry == nullptr || !Entry->bGpuArenas || Entry->ChunkSlot != Shell.ChunkSlot)
		{
			continue; // evicted or replaced since -- stale, skip
		}
		Expect.Add(Entry->ChunkSlot);
		Expect.Add(uint32(Shell.OriginVoxel.X));
		Expect.Add(uint32(Shell.OriginVoxel.Y));
		Expect.Add(uint32(Shell.OriginVoxel.Z));
		Expect.Add(uint32(FMath::Clamp(Shell.Key.Level, 0, 15)));
		Expect.Add(Entry->BrickBase);
		Expect.Add(0u);
		Expect.Add(0u);
		++Samples;
	}

	const FVoxelBrickPoolAllocLayout Layout = GpuAllocLayout;
	ENQUEUE_RENDER_COMMAND(VoxelBrickPoolAllocWindow)(
		[Buffers = GetOrCreateBuffers(), Layout, Expect = MoveTemp(Expect)](FRHICommandListImmediate& RHICmdList)
	{
		// Harvest anything a previous window enqueued -- same serial timeline.
		PollGpuAllocReadbacks_RenderThread();

		EnsureCreated_RenderThread(RHICmdList, Buffers);
		if (!Buffers.IsValid() || !Buffers->IsValid() || !Buffers->HasGpuAlloc())
		{
			return;
		}

		FRDGBuilder GraphBuilder(RHICmdList);
		{
			RDG_EVENT_SCOPE(GraphBuilder, "VoxelBrickPool.AllocWindow");
			VoxelGpuWorldGen::FBrickPoolAllocBuffers AB;
			AB.PoolDesc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
			AB.PoolOcc = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
			AB.PoolMat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
			AB.PoolTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled, TEXT("VoxelBrickPool.ChunkTable"));
			AB.AllocState = GraphBuilder.RegisterExternalBuffer(Buffers->AllocStatePooled, TEXT("VoxelBrickPool.AllocState"));
			AB.AllocBitmap = GraphBuilder.RegisterExternalBuffer(Buffers->AllocBitmapPooled, TEXT("VoxelBrickPool.AllocBitmap"));
			AB.AllocSide = GraphBuilder.RegisterExternalBuffer(Buffers->AllocSidePooled, TEXT("VoxelBrickPool.AllocSide"));

			const uint32 NumEntries = uint32(Expect.Num() / 8);
			if (NumEntries > 0)
			{
				FRDGBufferRef ExpectBuf = CreateStructuredBuffer(
					GraphBuilder, TEXT("Voxel.BrickPoolAllocExpect"), sizeof(uint32),
					Expect.Num(), Expect.GetData(), Expect.Num() * sizeof(uint32));
				FRDGBufferRef VerifyBuf = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 3),
					TEXT("Voxel.BrickPoolAllocVerify"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VerifyBuf), 0u);
				VoxelGpuWorldGen::AddBrickPoolAllocVerifyPass(GraphBuilder, AB, Layout,
				                                              ExpectBuf, NumEntries, VerifyBuf);
				FRHIGPUBufferReadback* Readback =
					new FRHIGPUBufferReadback(TEXT("Voxel.BrickPoolAllocVerify"));
				AddEnqueueCopyPass(GraphBuilder, Readback, VerifyBuf, 3 * sizeof(uint32));
				FPendingAllocVerify Pending;
				Pending.Readback = Readback;
				Pending.ExpectedChunks = NumEntries;
				GPendingAllocVerify.Add(Pending);
				GAllocXchkPending.fetch_add(1, std::memory_order_relaxed);
			}

			// The counter block, always -- it is what carries the two always-on
			// FAIL counters (bitmap collisions, bad frees) to the log.
			FRHIGPUBufferReadback* Counters =
				new FRHIGPUBufferReadback(TEXT("Voxel.BrickPoolAllocCounters"));
			AddEnqueueCopyPass(GraphBuilder, Counters, AB.AllocState,
			                   kGpuAllocCounterReadDwords * sizeof(uint32));
			FPendingAllocCounters PendingCounters;
			PendingCounters.Readback = Counters;
			PendingCounters.OccTopsFirst = Layout.OccTopsFirst;
			PendingCounters.OccClasses = Layout.OccClasses;
			PendingCounters.OccClassStep = Layout.OccClassStep;
			PendingCounters.MatTopsFirst = Layout.MatTopsFirst;
			PendingCounters.MatClasses = Layout.MatClasses;
			PendingCounters.MatClassStep = Layout.MatClassStep;
			GPendingAllocCounters.Add(PendingCounters);
		}
		GraphBuilder.Execute();
	});

	// --- the window line, from the LAST LANDED snapshot ----------------------
	//
	// The GPU numbers are one window stale by construction (the readback above
	// lands next window). That is the price of never waiting, and it is stated
	// on the line itself so nobody reads a fresh CPU tally against a stale GPU
	// one without knowing.
	if (GpuShellsAllocated > 0 || GpuFreesQueued > 0)
	{
		const bool bLanded = GAllocCountersLanded.load(std::memory_order_relaxed) != 0;
		const double MiB = 1024.0 * 1024.0;
		const int64 PaddedCum = GAllocSnapOccPaddedCum.load(std::memory_order_relaxed)
		                      + GAllocSnapMatPaddedCum.load(std::memory_order_relaxed);
		const int64 ActualCum = GAllocSnapOccActualCum.load(std::memory_order_relaxed)
		                      + GAllocSnapMatActualCum.load(std::memory_order_relaxed);
		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("[brick-gpualloc] shells %lld, freesQueued %lld; GPU (prev window%s): claims %lld ")
		       TEXT("(%lld stack reuse), frees %lld, inFlight %.1f MiB occ + %.1f MiB mat, bump ")
		       TEXT("high-water %.1f/%.1f MiB occ %.1f/%.1f MiB mat, padding %.1f%%, stranded ")
		       TEXT("%.1f MiB, leakedRuns %lld; claimFail occ %lld mat %lld worst %lld; ")
		       TEXT("FAIL: doubleGrant %lld badFree %lld; xcheck %lld ok / %lld FAIL ")
		       TEXT("(%lld sampled, %lld unwritten, %d pending)"),
		       GpuShellsAllocated, GpuFreesQueued,
		       bLanded ? TEXT("") : TEXT(", NOT LANDED YET -- no verdict"),
		       GAllocSnapClaims.load(std::memory_order_relaxed),
		       GAllocSnapStackPops.load(std::memory_order_relaxed),
		       GAllocSnapFrees.load(std::memory_order_relaxed),
		       double(GAllocSnapOccInFlight.load(std::memory_order_relaxed)) * 4 / MiB,
		       double(GAllocSnapMatInFlight.load(std::memory_order_relaxed)) * 4 / MiB,
		       double(GAllocSnapOccBump.load(std::memory_order_relaxed)) * 4 / MiB,
		       double(GpuAllocLayout.OccRegionWords) * 4 / MiB,
		       double(GAllocSnapMatBump.load(std::memory_order_relaxed)) * 4 / MiB,
		       double(GpuAllocLayout.MatRegionWords) * 4 / MiB,
		       ActualCum > 0 ? 100.0 * double(PaddedCum - ActualCum) / double(PaddedCum) : 0.0,
		       double(GAllocSnapStrandedOccDwords.load(std::memory_order_relaxed)
		              + GAllocSnapStrandedMatDwords.load(std::memory_order_relaxed)) * 4 / MiB,
		       GAllocSnapPushOverflow.load(std::memory_order_relaxed),
		       GAllocSnapClaimFailOcc.load(std::memory_order_relaxed),
		       GAllocSnapClaimFailMat.load(std::memory_order_relaxed),
		       GAllocSnapClaimFailWorst.load(std::memory_order_relaxed),
		       GAllocSnapBitmapCollision.load(std::memory_order_relaxed),
		       GAllocSnapFreeMissing.load(std::memory_order_relaxed),
		       GAllocXchkOk.load(std::memory_order_relaxed),
		       GAllocXchkFails.load(std::memory_order_relaxed),
		       GAllocXchkSamples.load(std::memory_order_relaxed),
		       GAllocXchkUnwritten.load(std::memory_order_relaxed),
		       GAllocXchkPending.load(std::memory_order_relaxed));
	}

	// The GT fallback tallies get their own line only when non-zero -- a
	// fallback means the armed path is DECLINING work, which must not hide.
	if (GpuFallbackStacked + GpuFallbackDiscard + GpuFallbackShellRefused > 0)
	{
		UE_LOG(LogVoxelBrickPool, Warning,
		       TEXT("[brick-gpualloc] fallbacks: stacked %lld, discardArm %lld, shellRefused %lld ")
		       TEXT("(cumulative). Stacked chunks lose their volume while armed (B.1 stacking is ")
		       TEXT("skipped when PoolAlloc is on); shellRefused means the descriptor pool is full ")
		       TEXT("even after eviction."),
		       GpuFallbackStacked, GpuFallbackDiscard, GpuFallbackShellRefused);
	}
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
	// P1. The GPU-side allocator state is NOT reset here -- Reset with no re-add
	// is teardown-only (see the declaration), and the buffers die with the
	// holder. What must not survive is the CPU-side pending work naming slots
	// this pool no longer owns.
	PendingGpuFreeSlots.Reset();
	PendingGpuIndexAdds.Reset();
	PendingGpuCpuWrites.Reset();
	RecentGpuShells.Reset();
	RecentGpuShellCursor = 0;
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
                                                  const TArray<uint32>& Clears,
                                                  bool bBatchedFlush)
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

	// The classic per-chunk emission, in ONE place so the batched arm's
	// fallbacks record byte-for-byte the passes the control arm records --
	// two copies of this body is two places for the rebase-wrap rule below to
	// be half-remembered.
	const auto EmitClassicWrite = [&GraphBuilder, DstDesc, DstOcc, DstMat, DstTable](
		const FPendingWrite& Write)
	{
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
			return;
		}

		FRDGBufferRef SrcDesc = GraphBuilder.RegisterExternalBuffer(P->Desc, TEXT("BrickSrc.Desc"));
		FRDGBufferRef SrcMask = GraphBuilder.RegisterExternalBuffer(P->ChunkMask, TEXT("BrickSrc.Mask"));
		FRDGBufferRef SrcOcc = P->Occ.IsValid()
			? GraphBuilder.RegisterExternalBuffer(P->Occ, TEXT("BrickSrc.Occ")) : nullptr;
		FRDGBufferRef SrcMat = P->Mat.IsValid()
			? GraphBuilder.RegisterExternalBuffer(P->Mat, TEXT("BrickSrc.Mat")) : nullptr;

		// The chunk's arena words are ONE CONTIGUOUS RUN in the scratch,
		// starting at SrcOccFirst/SrcMatFirst -- zero for a single-chunk
		// payload (offsets chunk-relative from 0, the original shape), the
		// predecessors' summed totals for a chunk handed out of a batched
		// stack region (Tier B.1). Either way the whole run moves as one copy.
		if (P->OccWords > 0 && SrcOcc != nullptr)
		{
			VoxelGpuWorldGen::AddBrickWordCopyPass(GraphBuilder, DstOcc, SrcOcc,
			                                       P->SrcOccFirst, Write.OccBase, P->OccWords);
		}
		if (P->MatWords > 0 && SrcMat != nullptr)
		{
			VoxelGpuWorldGen::AddBrickWordCopyPass(GraphBuilder, DstMat, SrcMat,
			                                       P->SrcMatFirst, Write.MatBase, P->MatWords);
		}

		// THE DESC BASES ARE A SUBTRACTION DRESSED AS AN ADD, and the wrap is
		// deliberate. A batched chunk's descriptor offsets are BATCH-relative
		// (they include SrcOccFirst), so the value that turns them into pool
		// addresses is `Write.OccBase - P->SrcOccFirst` -- which can be
		// "negative" as a uint. That is fine BY THE KERNEL'S OWN ARITHMETIC:
		// BrickDescPoolWriteMain computes ((offset + base) & 0x0fffffff), and
		// two's-complement wrap followed by the 28-bit mask yields exactly
		// (offset - SrcOccFirst + OccBase) whenever the true result fits in 28
		// bits -- which FVoxelBrickPool::Init already guarantees by refusing
		// arenas past 2^28 dwords. For the single-chunk shape SrcOccFirst is 0
		// and this is byte-for-byte the old pass.
		VoxelGpuWorldGen::AddBrickDescPoolWritePass(GraphBuilder, DstDesc, SrcDesc,
		                                            P->SrcBrickFirst, Write.BrickBase,
		                                            P->BrickCount,
		                                            Write.OccBase - P->SrcOccFirst,
		                                            Write.MatBase - P->SrcMatFirst);

		// The record LAST, and reading the SCRATCH buffers. Last because a
		// record is what makes a chunk visible to a marcher, and it must not
		// name arena ranges that have not been written yet. Scratch because
		// the descriptor offsets there and the scratch occupancy words they
		// point at are relative to the SAME buffer -- chunk-relative for a
		// single-chunk payload, batch-relative for a Tier B.1 stack member --
		// so the allSolid walk needs no base either way and cannot be wrong
		// about one. That self-consistency is why the batched path did not
		// have to touch this pass at all.
		VoxelGpuWorldGen::AddBrickChunkRecordPass(
			GraphBuilder, DstTable, SrcDesc, SrcOcc, SrcMask,
			P->SrcBrickFirst, P->SrcChunkIndex, P->BrickCount,
			Write.ChunkSlot, Write.BrickBase, Write.RingLevel, P->OriginVoxel,
			Write.Shading);
	};

	if (!bBatchedFlush)
	{
		// --- THE CONTROL ARM, UNTOUCHED -----------------------------------
		//
		// CLEARS FIRST. A slot retired and re-allocated inside one batch would
		// otherwise end up cleared by a command recorded for the chunk that
		// used to own it. The other order is only safe because AddChunkFromGpu
		// and RemoveChunk drop any write that a clear invalidates -- both
		// halves of that rule are needed, and this is the half that is visible
		// in a capture.
		for (uint32 Slot : Clears)
		{
			VoxelGpuWorldGen::AddBrickChunkClearPass(GraphBuilder, DstTable, Slot);
		}

		for (const FPendingWrite& Write : Writes)
		{
			if (Write.CpuPack.IsValid())
			{
				// The CPU arm's bytes are in system memory, not in a buffer
				// this graph could copy from. UploadCpuWrites_RenderThread
				// moves them after this graph executes -- see its declaration
				// for why after and not before.
				continue;
			}
			EmitClassicWrite(Write);
		}
		return;
	}

	// =======================================================================
	// THE BATCHED ARM (voxel.GPU.BrickFlushBatch).
	//
	// THE ORDERING ARGUMENT, STATED ONCE AND RELIED ON THROUGHOUT. Three
	// facts make this sound, and none of them is new to the batched shape:
	//
	//   1. CLEAR-BEFORE-WRITE SURVIVES because the fused clear pass and every
	//      record pass (fused or classic) declare UAV access to the SAME
	//      buffer (DstTable), and RDG serialises same-resource writers in
	//      RECORDING order. The clear is recorded first below, so it executes
	//      first -- the invariant is carried by the resource declaration, not
	//      by hope.
	//   2. INTRA-GRAPH ORDER BETWEEN THE ARENA COPIES AND THE RECORD PASS
	//      DOES NOT MATTER, batched or not, because NOTHING READS THE POOL
	//      INSIDE THIS GRAPH: the record pass reads scratch, not the arenas
	//      it names. Visibility to the marcher is at GRAPH granularity -- the
	//      marcher's passes are in a later graph on the same immediate
	//      command list, behind this graph's Execute() -- so "record last"
	//      is a graph-level guarantee here exactly as it was per-chunk.
	//   3. WITHIN ONE FUSED PASS, chunks write DISJOINT destination ranges:
	//      each pending write holds a LIVE arena allocation (frees drop the
	//      write), the allocator never hands out overlapping live ranges,
	//      and at most one write per slot survives to a flush (the drop
	//      rules in AllocateForChunk / RemoveChunk / EvictOne). So threads
	//      of one dispatch never race on an address.
	//
	// Two fused groups' passes DO both write the same arena UAVs and get an
	// RDG barrier between them -- ordering we do not need but that costs a
	// barrier, not correctness. That is the price of grouping by source
	// buffers, and it is still ~4 passes per STACK against ~4 per CHUNK.
	// =======================================================================

	// Harvest any earlier flush's verify results first; this is the same
	// serial render-thread timeline they were enqueued on.
	VoxelBrickPoolDetail::PollFlushVerifyReadbacks_RenderThread();

	if (Writes.Num() > 0)
	{
		// Denominator for "chunks per batched flush" on the window line.
		VoxelBrickPoolDetail::GFlushBatchedFlushes.fetch_add(1, std::memory_order_relaxed);
	}

	// --- clears, fused into one dispatch -----------------------------------
	if (Clears.Num() > 1)
	{
		FRDGBufferRef SlotList = CreateStructuredBuffer(
			GraphBuilder, TEXT("Voxel.BrickFlushClearSlots"), sizeof(uint32),
			Clears.Num(), Clears.GetData(), Clears.Num() * sizeof(uint32));
		VoxelGpuWorldGen::AddBrickFlushBatchClearPass(GraphBuilder, DstTable, SlotList,
		                                              uint32(Clears.Num()));
		VoxelBrickPoolDetail::GFlushClearsFused.fetch_add(Clears.Num(), std::memory_order_relaxed);
		VoxelBrickPoolDetail::GFlushClearPasses.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		for (uint32 Slot : Clears)
		{
			VoxelGpuWorldGen::AddBrickChunkClearPass(GraphBuilder, DstTable, Slot);
		}
	}

	// --- group the GPU writes by producing dispatch -------------------------
	//
	// The scratch Desc buffer's pointer IS the producing dispatch's identity:
	// a Tier B.1 stack hands every member a payload sharing one buffer set,
	// and a classic per-chunk job's payload shares with nobody. Grouping on
	// the pointer therefore recovers exactly the stacks, with no protocol
	// between the job manager and the pool -- the buffers themselves are the
	// contract. Occ/Mat/Mask are checked against the group's first member
	// rather than trusted, because a payload that shared Desc but not the
	// rest would make the fused copies read one stack's words at another
	// stack's offsets; that cannot happen today, and "cannot happen" is
	// counted (fallbackMixed) rather than assumed.
	struct FFlushGroup
	{
		const FVoxelGpuBrickPayload* Rep = nullptr;
		TArray<int32> Members;
	};
	TMap<const void*, int32> GroupIndexByDesc;
	TArray<FFlushGroup> Groups;
	TArray<int32> ClassicWrites;

	for (int32 I = 0; I < Writes.Num(); ++I)
	{
		const FPendingWrite& Write = Writes[I];
		if (Write.CpuPack.IsValid())
		{
			continue; // UploadCpuWrites_RenderThread's, after Execute, both arms
		}
		const FVoxelGpuBrickPayloadRef& P = Write.Payload;
		if (!P.IsValid() || !P->Desc.IsValid() || !P->ChunkMask.IsValid() || !P->Occ.IsValid()
		    || P->BrickCount != VoxelBrickPoolDetail::kBricksPerChunk)
		{
			// EmitClassicWrite logs the drop with the same message the control
			// arm would; the BrickCount case is unreachable past
			// AddChunkFromGpu's own refusal and guarded anyway because a fused
			// dispatch sized on the CONTRACT would half-write a chunk that
			// somehow carried a different count.
			VoxelBrickPoolDetail::GFlushFallbackInvalid.fetch_add(1, std::memory_order_relaxed);
			EmitClassicWrite(Write);
			continue;
		}
		const void* Key = P->Desc.GetReference();
		if (const int32* Existing = GroupIndexByDesc.Find(Key))
		{
			FFlushGroup& Group = Groups[*Existing];
			const FVoxelGpuBrickPayload* Rep = Group.Rep;
			if (P->Occ.GetReference() != Rep->Occ.GetReference() ||
			    P->Mat.GetReference() != Rep->Mat.GetReference() ||
			    P->ChunkMask.GetReference() != Rep->ChunkMask.GetReference())
			{
				VoxelBrickPoolDetail::GFlushFallbackMixed.fetch_add(1, std::memory_order_relaxed);
				ClassicWrites.Add(I);
				continue;
			}
			Group.Members.Add(I);
		}
		else
		{
			const int32 NewIndex = Groups.Num();
			FFlushGroup& Group = Groups.Emplace_GetRef();
			Group.Rep = P.Get();
			Group.Members.Add(I);
			GroupIndexByDesc.Add(Key, NewIndex);
		}
	}

	// One fused slice's recorded state, kept so the verify sampler below can
	// re-reach the slice's writes and registered buffers after all groups
	// have been recorded.
	struct FFusedSlice
	{
		TArray<int32> Members;
		FRDGBufferRef SrcDesc = nullptr;
		FRDGBufferRef SrcOcc = nullptr;
		FRDGBufferRef SrcMat = nullptr;
		FRDGBufferRef SrcMask = nullptr;
	};
	TArray<FFusedSlice> FusedSlices;

	for (const FFlushGroup& Group : Groups)
	{
		if (Group.Members.Num() == 1)
		{
			// A singleton fused set would be the same ~4 passes plus a table
			// upload -- strictly worse. This is the EXPECTED shape whenever
			// voxel.GPU.WorldGenBatch is off (every payload its own buffers),
			// which is why the window line reports it as its own reason
			// rather than folding it into a generic "fallback".
			VoxelBrickPoolDetail::GFlushFallbackSingle.fetch_add(1, std::memory_order_relaxed);
			ClassicWrites.Add(Group.Members[0]);
			continue;
		}

		// Register the shared scratch ONCE per group (idempotent per graph,
		// but stating the intent: one group, one buffer set).
		const FVoxelGpuBrickPayload* Rep = Group.Rep;
		FRDGBufferRef SrcDesc = GraphBuilder.RegisterExternalBuffer(Rep->Desc, TEXT("BrickSrc.Desc"));
		FRDGBufferRef SrcMask = GraphBuilder.RegisterExternalBuffer(Rep->ChunkMask, TEXT("BrickSrc.Mask"));
		FRDGBufferRef SrcOcc = GraphBuilder.RegisterExternalBuffer(Rep->Occ, TEXT("BrickSrc.Occ"));
		FRDGBufferRef SrcMat = Rep->Mat.IsValid()
			? GraphBuilder.RegisterExternalBuffer(Rep->Mat, TEXT("BrickSrc.Mat")) : nullptr;

		// Slices only guard a producer bigger than any that exists (stacks
		// cap at 64 chunks; the table caps at 512 entries).
		for (int32 SliceStart = 0; SliceStart < Group.Members.Num();
		     SliceStart += VoxelBrickPoolDetail::kFlushTableMaxEntries)
		{
			const int32 SliceCount = FMath::Min(VoxelBrickPoolDetail::kFlushTableMaxEntries,
			                                    Group.Members.Num() - SliceStart);

			// --- build the destination table --------------------------------
			//
			// BUILT HERE, ON THE RENDER THREAD, PER FLUSH, and OWNED BY THE
			// GRAPH: CreateStructuredBuffer copies the array into RDG's own
			// allocator, so the table's lifetime is exactly the graph's and
			// no CPU-side state survives the flush. Layout is the field map
			// in VoxelBrickPoolWrite.usf; the CUM columns are inclusive
			// prefix sums for the word-copy kernel's binary search.
			const uint32 Stride = VoxelBrickPoolDetail::kFlushTableStride;
			TArray<uint32> Table;
			Table.SetNumZeroed(SliceCount * int32(Stride));
			uint32 OccCum = 0;
			uint32 MatCum = 0;
			uint32 EquivPasses = 0;
			for (int32 S = 0; S < SliceCount; ++S)
			{
				const FPendingWrite& Write = Writes[Group.Members[SliceStart + S]];
				const FVoxelGpuBrickPayload* P = Write.Payload.Get();
				// Mirror the CLASSIC copy conditions exactly: a chunk whose
				// scratch mat buffer is missing gets no mat copy there, so it
				// must get none here either -- word counts of zero suppress
				// the copy without touching the desc rebase fields.
				const uint32 OccWords = (P->OccWords > 0) ? P->OccWords : 0u;
				const uint32 MatWords = (P->MatWords > 0 && SrcMat != nullptr) ? P->MatWords : 0u;
				OccCum += OccWords;
				MatCum += MatWords;

				uint32* E = Table.GetData() + S * int32(Stride);
				E[0] = P->SrcBrickFirst;
				E[1] = Write.BrickBase;
				E[2] = P->SrcOccFirst;
				E[3] = Write.OccBase;
				E[4] = OccWords;
				E[5] = OccCum;
				E[6] = P->SrcMatFirst;
				E[7] = Write.MatBase;
				E[8] = MatWords;
				E[9] = MatCum;
				E[10] = Write.ChunkSlot;
				E[11] = P->SrcChunkIndex;
				E[12] = Write.RingLevel;
				E[13] = uint32(P->OriginVoxel.X);
				E[14] = uint32(P->OriginVoxel.Y);
				E[15] = uint32(P->OriginVoxel.Z);
				Write.Shading.Pack(E[16], E[17], E[18]);
				// [19..31] stay zero -- SetNumZeroed wrote them, and they are
				// written rather than left alone for the record tail's reason.

				EquivPasses += 2u + (OccWords > 0 ? 1u : 0u) + (MatWords > 0 ? 1u : 0u);
			}

			FRDGBufferRef TableBuffer = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.BrickFlushTable"), sizeof(uint32),
				Table.Num(), Table.GetData(), Table.Num() * sizeof(uint32));

			// --- the ~4 fused passes ---------------------------------------
			uint32 FusedPasses = 0;
			if (OccCum > 0)
			{
				VoxelGpuWorldGen::AddBrickFlushBatchWordCopyPass(
					GraphBuilder, DstOcc, SrcOcc, TableBuffer, Stride, /*FieldFirst*/ 2u,
					uint32(SliceCount), OccCum);
				++FusedPasses;
			}
			if (MatCum > 0 && SrcMat != nullptr)
			{
				VoxelGpuWorldGen::AddBrickFlushBatchWordCopyPass(
					GraphBuilder, DstMat, SrcMat, TableBuffer, Stride, /*FieldFirst*/ 6u,
					uint32(SliceCount), MatCum);
				++FusedPasses;
			}
			VoxelGpuWorldGen::AddBrickFlushBatchDescWritePass(
				GraphBuilder, DstDesc, SrcDesc, TableBuffer, Stride, uint32(SliceCount),
				VoxelBrickPoolDetail::kBricksPerChunk);
			++FusedPasses;
			VoxelGpuWorldGen::AddBrickFlushBatchRecordPass(
				GraphBuilder, DstTable, SrcDesc, SrcOcc, SrcMask, TableBuffer, Stride,
				uint32(SliceCount), VoxelBrickPoolDetail::kBricksPerChunk);
			++FusedPasses;

			VoxelBrickPoolDetail::GFlushFusedGroups.fetch_add(1, std::memory_order_relaxed);
			VoxelBrickPoolDetail::GFlushFusedChunks.fetch_add(SliceCount, std::memory_order_relaxed);
			VoxelBrickPoolDetail::GFlushFusedPasses.fetch_add(FusedPasses, std::memory_order_relaxed);
			VoxelBrickPoolDetail::GFlushEquivPasses.fetch_add(EquivPasses, std::memory_order_relaxed);

			FFusedSlice& Slice = FusedSlices.Emplace_GetRef();
			Slice.Members.Append(Group.Members.GetData() + SliceStart, SliceCount);
			Slice.SrcDesc = SrcDesc;
			Slice.SrcOcc = SrcOcc;
			Slice.SrcMat = SrcMat;
			Slice.SrcMask = SrcMask;
		}
	}

	// The classic leftovers, AFTER the fused groups only for tidy capture
	// grouping -- ordering between them is carried by the resource
	// declarations (point 1 above), not by this loop's position.
	for (int32 I : ClassicWrites)
	{
		EmitClassicWrite(Writes[I]);
		VoxelBrickPoolDetail::GFlushClassicChunks.fetch_add(1, std::memory_order_relaxed);
	}

	// --- the live cross-check: sample ONE fused slice this flush -------------
	//
	// One workgroup per chunk of the sampled slice, comparing the pool bytes
	// the fused passes landed against the classic formula computed from the
	// SAME FPendingWrite fields the classic passes read -- never from the
	// table, so a table bug cannot verify itself (see BrickFlushVerifyMain).
	// One slice per flush keeps the reintroduced per-chunk cost to a few
	// dispatches per flush while every 5 s window still accumulates hundreds
	// of verified chunks at streaming rates. RDG orders the verify behind the
	// fused writes because it declares SRV reads of the four pool buffers the
	// writes declared UAVs on.
	if (FusedSlices.Num() > 0)
	{
		const FFusedSlice& Slice =
			FusedSlices[int32(VoxelBrickPoolDetail::GFlushSampleRotor++ % uint64(FusedSlices.Num()))];

		FRDGBufferRef VerifyResult = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2), TEXT("Voxel.BrickFlushVerify"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VerifyResult), 0u);

		VoxelGpuWorldGen::FBrickFlushVerifyBuffers VerifyBuffers;
		VerifyBuffers.PoolDesc = DstDesc;
		VerifyBuffers.PoolOcc = DstOcc;
		VerifyBuffers.PoolMat = DstMat;
		VerifyBuffers.PoolTable = DstTable;
		VerifyBuffers.SrcDesc = Slice.SrcDesc;
		VerifyBuffers.SrcOcc = Slice.SrcOcc;
		// A slice with no mat buffer has every MatWords at zero, so the
		// kernel never reads InWords -- the occ buffer stands in only so the
		// binding is non-null, exactly the "bound but unread" shape, stated.
		VerifyBuffers.SrcMat = (Slice.SrcMat != nullptr) ? Slice.SrcMat : Slice.SrcOcc;
		VerifyBuffers.SrcChunkMask = Slice.SrcMask;
		VerifyBuffers.OutVerify = VerifyResult;

		for (int32 I : Slice.Members)
		{
			const FPendingWrite& Write = Writes[I];
			const FVoxelGpuBrickPayload* P = Write.Payload.Get();
			VoxelGpuWorldGen::FBrickFlushVerifyArgs Args;
			Args.SrcBrickFirst = P->SrcBrickFirst;
			Args.SrcChunkIndex = P->SrcChunkIndex;
			Args.BrickCount = P->BrickCount;
			Args.ChunkSlot = Write.ChunkSlot;
			Args.BrickBase = Write.BrickBase;
			Args.RingLevel = Write.RingLevel;
			Args.OccBase = Write.OccBase;
			Args.MatBase = Write.MatBase;
			Args.OccSrcFirst = P->SrcOccFirst;
			Args.MatSrcFirst = P->SrcMatFirst;
			Args.OccWords = (P->OccWords > 0) ? P->OccWords : 0u;
			Args.MatWords = (P->MatWords > 0 && Slice.SrcMat != nullptr) ? P->MatWords : 0u;
			Args.OriginVoxel = P->OriginVoxel;
			Args.Shading = Write.Shading;
			VoxelGpuWorldGen::AddBrickFlushVerifyPass(GraphBuilder, VerifyBuffers, Args);
		}

		FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("Voxel.BrickFlushVerify"));
		AddEnqueueCopyPass(GraphBuilder, Readback, VerifyResult, 2 * sizeof(uint32));
		VoxelBrickPoolDetail::GPendingFlushVerify.Add(
			VoxelBrickPoolDetail::FPendingFlushVerify{ Readback, uint32(Slice.Members.Num()) });
		VoxelBrickPoolDetail::GFlushXchkSamples.fetch_add(1, std::memory_order_relaxed);
		VoxelBrickPoolDetail::GFlushXchkPending.fetch_add(1, std::memory_order_relaxed);
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
	// P1: pending GPU-side frees go out FIRST, before anything below could
	// enqueue a claim that reuses a freed slot -- the ordering rule on
	// FlushPendingGpuFrees's declaration. No-op unarmed and when empty.
	FlushPendingGpuFrees();

	if (PendingWrites.Num() == 0 && PendingClears.Num() == 0 &&
	    PendingGpuCpuWrites.Num() == 0 && PendingGpuIndexAdds.Num() == 0)
	{
		// The window still has to tick while the pipeline idles -- the counter
		// readback is how a FAIL that happened at the END of activity reaches
		// the log.
		if (bGpuAllocArmed)
		{
			MaybePumpGpuAllocWindow();
		}
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
	// P1: the CPU producer's armed-path writes, moved out here for the same
	// hollow-array reason as Writes below. Their claims ride the flush command's
	// graph, so the index Added entries queued for them (and for the GPU
	// producer's shells, whose graphs were enqueued in DispatchBatch earlier
	// this tick) deliver at the bottom of this function -- behind every record
	// write they name, exactly as the classic path's do.
	TArray<FPendingGpuCpuWrite> GpuCpuWrites = MoveTemp(PendingGpuCpuWrites);
	PendingGpuCpuWrites.Reset();
	if (IndexSink)
	{
		IndexDelta.Added.Reserve(Writes.Num() + PendingGpuIndexAdds.Num());
		for (const FPendingWrite& W : Writes)
		{
			IndexDelta.Added.Add(FVoxelBrickIndexEntry{ W.Key, W.ChunkSlot });
		}
		IndexDelta.Added.Append(PendingGpuIndexAdds);
	}
	PendingGpuIndexAdds.Reset();
	for (const FPendingGpuCpuWrite& W : GpuCpuWrites)
	{
		// This is PCIe traffic too -- the pack is uploaded to scratch before the
		// claim copies it into the arenas -- so it stays on the same counter the
		// unarmed Lock/Memcpy path uses.
		if (W.Pack.IsValid())
		{
			CpuUploadBytes += int64(W.Pack->ResidentBytes());
		}
	}

	const double FlushEnqueueStart = FPlatformTime::Seconds();
	FlushStageMs.PrepMs += (FlushEnqueueStart - FlushPrepStart) * 1000.0;
	// READ HERE, ON THE GAME THREAD, AND CAPTURED BY VALUE. The render command
	// may run a frame later; the passes must be the ones this batch was queued
	// under, not whatever the cvar says by then -- half a flush on each arm is
	// the one state neither arm's counters describe.
	const bool bBatchedFlush = VoxelBrickPoolDetail::VoxelBrickFlushBatchEnabled();
	ENQUEUE_RENDER_COMMAND(VoxelBrickPoolFlush)(
		[Buffers = GetOrCreateBuffers(), Writes = MoveTemp(Writes),
		 Clears = MoveTemp(Clears), bBatchedFlush,
		 GpuCpuWrites = MoveTemp(GpuCpuWrites),
		 Layout = GpuAllocLayout](FRHICommandListImmediate& RHICmdList) mutable
	{
		if (!Buffers.IsValid())
		{
			return;
		}

		// First use creates the arenas. No scene proxy is involved -- nothing
		// draws from this pool -- so there is no bootstrap to wait for and no
		// window in which a write has nowhere to land. (P1 moved the creation
		// into EnsureCreated_RenderThread because the generation graph can now
		// touch the pool before the first flush -- same code, one home.)
		EnsureCreated_RenderThread(RHICmdList, Buffers);

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
		AddFlushPasses_RenderThread(GraphBuilder, Buffers, Writes, Clears, bBatchedFlush);

		// --- P1: the CPU producer's claims through the GPU allocator --------
		//
		// One-allocator rule: the pack was built on a worker, its SIZES are
		// known here, but the RANGES are decided by the same claim kernel the
		// GPU producer uses -- the sizes go up as a 2-dword upload standing in
		// for the scan totals, the pack's bytes go up as transient scratch, and
		// the identical write kernels land them. Recorded AFTER the classic
		// passes but ordering against them is moot: an armed pool has no
		// classic writes (AddChunkFromGpu refuses), and the frees these claims
		// might reuse slots from were enqueued in their own command before this
		// one (FlushPendingGpuFrees at the top of Flush).
		if (GpuCpuWrites.Num() > 0 && Buffers->HasGpuAlloc())
		{
			RDG_EVENT_SCOPE(GraphBuilder, "VoxelBrickPool.CpuClaims(%d chunks)", GpuCpuWrites.Num());
			VoxelGpuWorldGen::FBrickPoolAllocBuffers AB;
			AB.PoolDesc = GraphBuilder.RegisterExternalBuffer(Buffers->DescPooled, TEXT("VoxelBrickPool.Desc"));
			AB.PoolOcc = GraphBuilder.RegisterExternalBuffer(Buffers->OccPooled, TEXT("VoxelBrickPool.Occ"));
			AB.PoolMat = GraphBuilder.RegisterExternalBuffer(Buffers->MatPooled, TEXT("VoxelBrickPool.Mat"));
			AB.PoolTable = GraphBuilder.RegisterExternalBuffer(Buffers->ChunkTablePooled, TEXT("VoxelBrickPool.ChunkTable"));
			AB.AllocState = GraphBuilder.RegisterExternalBuffer(Buffers->AllocStatePooled, TEXT("VoxelBrickPool.AllocState"));
			AB.AllocBitmap = GraphBuilder.RegisterExternalBuffer(Buffers->AllocBitmapPooled, TEXT("VoxelBrickPool.AllocBitmap"));
			AB.AllocSide = GraphBuilder.RegisterExternalBuffer(Buffers->AllocSidePooled, TEXT("VoxelBrickPool.AllocSide"));

			// A 1-dword stand-in for an arena a chunk owns nothing in. Never
			// indexed (no MIXED brick exists when an arena is empty); it exists
			// because an SRV must bind SOMETHING valid.
			static const uint32 ZeroDword = 0;
			FRDGBufferRef Dummy = CreateStructuredBuffer(
				GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimDummy"), sizeof(uint32), 1,
				&ZeroDword, sizeof(uint32));

			for (const FPendingGpuCpuWrite& W : GpuCpuWrites)
			{
				if (!W.Pack.IsValid())
				{
					continue;
				}
				const FVoxelBrickCpuPack& Pack = *W.Pack;
				const uint32 OccWords = Pack.OccWords();
				const uint32 MatWords = Pack.MatWords();
				const uint32 Totals[2] = { OccWords, MatWords };
				const uint32 Mask[2] = { uint32(Pack.BrickSolid & 0xffffffffull),
				                         uint32((Pack.BrickSolid >> 32) & 0xffffffffull) };

				FRDGBufferRef TotalsBuf = CreateStructuredBuffer(
					GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimTotals"), sizeof(uint32), 2,
					Totals, sizeof(Totals));
				FRDGBufferRef MaskBuf = CreateStructuredBuffer(
					GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimMask"), sizeof(uint32), 2,
					Mask, sizeof(Mask));
				FRDGBufferRef DescBuf = CreateStructuredBuffer(
					GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimDesc"), sizeof(uint32) * 2,
					int32(VoxelBrickPoolDetail::kBricksPerChunk), Pack.Desc.GetData(),
					Pack.Desc.Num() * sizeof(uint32));
				FRDGBufferRef OccBuf = OccWords > 0
					? CreateStructuredBuffer(GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimOcc"),
					                         sizeof(uint32), int32(OccWords), Pack.Occ.GetData(),
					                         OccWords * sizeof(uint32))
					: Dummy;
				FRDGBufferRef MatBuf = MatWords > 0
					? CreateStructuredBuffer(GraphBuilder, TEXT("Voxel.BrickPoolCpuClaimMat"),
					                         sizeof(uint32), int32(MatWords), Pack.Mat.GetData(),
					                         MatWords * sizeof(uint32))
					: Dummy;

				FRDGBufferRef Claim = VoxelGpuWorldGen::AddBrickPoolClaimPass(
					GraphBuilder, AB, Layout, TotalsBuf, W.ChunkSlot,
					VoxelBrickPoolDetail::kBricksPerChunk * 16u,
					VoxelBrickPoolDetail::kBricksPerChunk * 132u);
				VoxelGpuWorldGen::AddBrickPoolAllocWritePasses(
					GraphBuilder, AB, Claim, OccBuf, MatBuf, DescBuf, MaskBuf,
					VoxelBrickPoolDetail::kBricksPerChunk, W.ChunkSlot, W.BrickBase,
					W.RingLevel, W.OriginVoxel, W.Shading, OccWords, MatWords);
			}
		}
		else if (GpuCpuWrites.Num() > 0)
		{
			UE_LOG(LogVoxelBrickPool, Error,
			       TEXT("[brick-gpualloc] %d CPU-producer write(s) dropped: allocator buffers ")
			       TEXT("unavailable. Those chunks hold a shell and no volume."),
			       GpuCpuWrites.Num());
		}

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

	// The batched-flush window line. Only while armed: an unarmed control leg
	// must not gain a log line, because leg diffs treat new lines as signal.
	if (bBatchedFlush)
	{
		VoxelBrickPoolDetail::MaybeLogFlushBatchWindow();
	}
	// P1: the allocator's window -- samples, counter readback, and the
	// [brick-gpualloc] line. Same only-while-armed rule as above.
	if (bGpuAllocArmed)
	{
		MaybePumpGpuAllocWindow();
	}
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


	// P1: on an armed pool the occ/mat rows above are CPU-arena numbers and the
	// CPU arenas are retired -- they legitimately read 0 while the world is
	// full. Said here so nobody reads "0 dwords used" as "the volume is empty".
	if (Pool.IsGpuAllocArmed())
	{
		UE_LOG(LogVoxelBrickPool, Log,
		       TEXT("  voxel.GPU.PoolAlloc is ARMED: occ/mat usage above reads 0 by design (the words ")
		       TEXT("are GPU-allocated). Read bytes in flight, bump high-water, padding and the FAIL ")
		       TEXT("counters off the [brick-gpualloc] window line instead."));
	}

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
